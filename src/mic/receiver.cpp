/**
 * @file src/mic/receiver.cpp
 * @brief Receives client microphone Opus frames, conceals loss and feeds a platf::mic_sink.
 */
#include "receiver.h"

#include "src/logging.h"

#include <opus/opus.h>

#include <array>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::literals;

namespace mic::receiver {
  namespace {
    using clock = std::chrono::steady_clock;

    constexpr auto owner_silence_timeout = 1s;
    constexpr auto worker_idle_timeout = 3s;
    constexpr auto sink_retry_interval = 5s;
    constexpr auto sink_warning_interval = 60s;
    constexpr std::size_t max_queue = 50;  ///< 1 s of audio; beyond that the worker is stuck, drop

    struct packet_t {
      std::uint16_t seq;
      std::vector<std::uint8_t> data;
    };

    struct decoder_deleter {
      void operator()(OpusDecoder *d) const {
        opus_decoder_destroy(d);
      }
    };

    std::mutex g_mutex;
    std::condition_variable g_cv;
    std::condition_variable g_idle_cv;
    sink_factory_t g_factory;
    std::deque<packet_t> g_queue;
    std::thread g_worker;
    bool g_worker_running = false;  // worker thread is (about to be) active; guarded by g_mutex
    bool g_worker_done = false;  // worker finished its loop and can be joined; guarded by g_mutex
    bool g_stop = false;
    bool g_busy = false;  // worker is processing a packet
    bool g_has_owner = false;
    std::uint64_t g_owner = 0;
    clock::time_point g_owner_last_packet;
    std::uint64_t g_owner_generation = 0;  // bumped whenever ownership changes; the worker resets its seq state

    // Worker-only state.
    struct worker_state_t {
      std::unique_ptr<OpusDecoder, decoder_deleter> decoder;
      std::unique_ptr<platf::mic_sink> sink;
      bool sink_open = false;
      bool have_last = false;
      std::uint16_t last_seq = 0;
      std::uint64_t generation = 0;
      clock::time_point last_open_attempt;
      clock::time_point last_warning;
      bool warned = false;
      bool tried_open = false;
    };

    void reset_stream(worker_state_t &st) {
      st.have_last = false;
      if (st.decoder) {
        opus_decoder_ctl(st.decoder.get(), OPUS_RESET_STATE);
      }
    }

    bool ensure_sink(worker_state_t &st, sink_factory_t &factory) {
      if (st.sink_open) {
        return true;
      }
      const auto now = clock::now();
      if (st.tried_open && now - st.last_open_attempt < sink_retry_interval) {
        return false;
      }
      st.tried_open = true;
      st.last_open_attempt = now;
      st.sink = factory ? factory() : nullptr;
      if (st.sink && st.sink->open()) {
        st.sink_open = true;
        BOOST_LOG(info) << "Microphone stream opened"sv;
        return true;
      }
      st.sink.reset();
      if (!st.warned || now - st.last_warning >= sink_warning_interval) {
        st.warned = true;
        st.last_warning = now;
        BOOST_LOG(warning) << "Microphone packets are arriving but no microphone sink device is available"sv;
      }
      return false;
    }

    void render(worker_state_t &st, const float *samples) {
      if (st.sink_open) {
        st.sink->write(samples, frame_samples);
      }
    }

    void process(worker_state_t &st, sink_factory_t &factory, const packet_t &pkt) {
      if (!ensure_sink(st, factory)) {
        return;
      }
      if (!st.decoder) {
        int err = 0;
        st.decoder.reset(opus_decoder_create(sample_rate, 1, &err));
        if (!st.decoder) {
          BOOST_LOG(error) << "Couldn't create microphone Opus decoder: "sv << opus_strerror(err);
          return;
        }
      }

      int missing = 0;
      if (st.have_last) {
        const auto diff = static_cast<std::uint16_t>(pkt.seq - st.last_seq);
        if (diff == 0 || diff >= 0x8000) {
          return;  // duplicate or older than what was already played
        }
        if (diff <= 3) {
          missing = diff - 1;  // 1-2 lost frames: conceal
        } else {
          opus_decoder_ctl(st.decoder.get(), OPUS_RESET_STATE);  // large gap: resync without a PLC burst
        }
      }

      std::array<float, frame_samples> out;
      for (int i = 0; i < missing; ++i) {
        const int n = opus_decode_float(st.decoder.get(), nullptr, 0, out.data(), frame_samples, 0);
        if (n == frame_samples) {
          render(st, out.data());
        }
      }

      const int n = opus_decode_float(st.decoder.get(), pkt.data.data(), static_cast<opus_int32>(pkt.data.size()), out.data(), frame_samples, 0);
      st.have_last = true;
      st.last_seq = pkt.seq;
      if (n != frame_samples) {
        BOOST_LOG(debug) << "Microphone Opus decode failed or returned "sv << n << " samples"sv;
        return;
      }
      render(st, out.data());
    }

    void worker_main(sink_factory_t factory) {
      worker_state_t st;
      std::unique_lock lock {g_mutex};
      st.generation = g_owner_generation;
      auto last_activity = clock::now();
      while (!g_stop) {
        if (g_queue.empty()) {
          g_busy = false;
          g_idle_cv.notify_all();
          if (g_cv.wait_until(lock, last_activity + worker_idle_timeout) == std::cv_status::timeout && g_queue.empty()) {
            break;
          }
          continue;
        }
        auto pkt = std::move(g_queue.front());
        g_queue.pop_front();
        g_busy = true;
        if (st.generation != g_owner_generation) {
          st.generation = g_owner_generation;
          reset_stream(st);
        }
        lock.unlock();
        process(st, factory, pkt);
        lock.lock();
        last_activity = clock::now();
      }
      g_busy = false;
      g_worker_done = true;
      g_idle_cv.notify_all();
      lock.unlock();
      if (st.sink_open) {
        st.sink->close();
        BOOST_LOG(info) << "Microphone stream closed"sv;
      }
    }
  }  // namespace

  void set_sink_factory(sink_factory_t factory) {
    std::lock_guard lock {g_mutex};
    g_factory = std::move(factory);
  }

  void submit(std::uint64_t session_id, std::uint16_t seq, std::string_view opus) {
    if (opus.empty() || opus.size() > max_opus_bytes) {
      return;
    }
    std::thread finished;
    {
      std::lock_guard lock {g_mutex};
      const auto now = clock::now();
      if (g_has_owner && g_owner != session_id && now - g_owner_last_packet < owner_silence_timeout) {
        return;
      }
      if (!g_has_owner || g_owner != session_id) {
        g_has_owner = true;
        g_owner = session_id;
        ++g_owner_generation;
      }
      g_owner_last_packet = now;

      if (g_queue.size() >= max_queue) {
        return;
      }
      g_queue.push_back({seq, std::vector<std::uint8_t>(opus.begin(), opus.end())});

      if (g_worker_running && g_worker_done) {
        finished = std::move(g_worker);
        g_worker_running = false;
        g_worker_done = false;
      }
      if (!g_worker_running) {
        g_stop = false;
        g_worker_running = true;
        g_worker_done = false;
        g_worker = std::thread {worker_main, g_factory};
      }
    }
    g_cv.notify_one();
    if (finished.joinable()) {
      finished.join();
    }
  }

  void session_ended(std::uint64_t session_id) {
    std::lock_guard lock {g_mutex};
    if (g_has_owner && g_owner == session_id) {
      g_has_owner = false;
      ++g_owner_generation;
    }
  }

  void shutdown() {
    std::thread worker;
    {
      std::lock_guard lock {g_mutex};
      g_stop = true;
      worker = std::move(g_worker);
    }
    g_cv.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
    std::lock_guard lock {g_mutex};
    g_queue.clear();
    g_worker_running = false;
    g_worker_done = false;
    g_stop = false;
    g_busy = false;
    g_has_owner = false;
    ++g_owner_generation;
  }

  bool wait_idle_for_testing(std::chrono::milliseconds timeout) {
    std::unique_lock lock {g_mutex};
    return g_idle_cv.wait_for(lock, timeout, [] {
      return g_queue.empty() && !g_busy;
    });
  }
}  // namespace mic::receiver
