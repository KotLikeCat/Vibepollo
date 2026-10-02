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
#include <functional>
#include <mutex>
#include <atomic>
#include <string>
#include <thread>
#include <vector>

using namespace std::literals;

namespace mic::receiver {
  namespace {
    using clock = std::chrono::steady_clock;

    constexpr auto owner_silence_timeout = 1s;
    constexpr auto sink_warning_interval = 60s;
    std::atomic<std::chrono::milliseconds::rep> g_sink_retry_ms {5000};
    constexpr auto slow_open_threshold = 50ms;  ///< an open() slower than this leaves a stale backlog behind
    constexpr std::size_t keep_after_open = 2;  ///< newest packets kept after a slow open
    constexpr std::size_t max_queue = 50;  ///< 1 s of audio; beyond that the worker is stuck, drop

    struct packet_t {
      std::uint16_t seq;
      std::uint64_t generation;
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
    bool g_started = false;  // the persistent worker was started (at most once per process / test reset)
    bool g_shutdown = false;
    std::chrono::milliseconds g_idle_timeout = 3s;
    bool g_busy = false;  // worker is processing a packet
    bool g_has_owner = false;
    std::uint64_t g_owner = 0;
    clock::time_point g_owner_last_packet;
    std::function<void()> g_worker_init;
    clock::time_point g_rate_window_start;
    int g_rate_count = 0;
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
      bool decoder_error_logged = false;
      bool write_failed_logged = false;
      clock::time_point last_write_failure;
      clock::time_point last_decoder_error;
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
      if (st.tried_open && now - st.last_open_attempt < std::chrono::milliseconds(g_sink_retry_ms.load())) {
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
      if (st.sink_open && !st.sink->write(samples, frame_samples)) {
        const auto now = clock::now();
        if (!st.write_failed_logged || now - st.last_write_failure >= sink_warning_interval) {
          st.write_failed_logged = true;
          st.last_write_failure = now;
          BOOST_LOG(warning) << "Microphone sink failed; closing it and retrying later"sv;
        }
        st.sink->close();
        st.sink.reset();
        st.sink_open = false;
        st.tried_open = true;
        st.last_open_attempt = now;  // reopen after the retry interval
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
          const auto now = clock::now();
          if (!st.decoder_error_logged || now - st.last_decoder_error >= sink_warning_interval) {
            st.decoder_error_logged = true;
            st.last_decoder_error = now;
            BOOST_LOG(error) << "Couldn't create microphone Opus decoder: "sv << opus_strerror(err);
          }
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

    void close_stream(worker_state_t &st) {
      if (st.sink_open) {
        st.sink->close();
        BOOST_LOG(info) << "Microphone stream closed"sv;
      }
      st.sink.reset();
      st.sink_open = false;
      st.tried_open = false;
      st.decoder.reset();
      st.have_last = false;
    }

    // Persistent worker: started once per process, joined only by shutdown().
    void worker_main(sink_factory_t factory) {
      worker_state_t st;
      std::function<void()> init;
      {
        std::lock_guard guard {g_mutex};
        init = g_worker_init;
      }
      if (init) {
        init();
      }
      std::unique_lock lock {g_mutex};
      auto last_activity = clock::now();
      while (!g_shutdown) {
        if (g_queue.empty()) {
          g_busy = false;
          g_idle_cv.notify_all();
          const bool open = st.sink_open || st.decoder;
          if (!open) {
            g_cv.wait(lock);
          } else if (g_cv.wait_until(lock, last_activity + g_idle_timeout) == std::cv_status::timeout && g_queue.empty() && !g_shutdown) {
            lock.unlock();
            close_stream(st);
            lock.lock();
          }
          continue;
        }
        auto pkt = std::move(g_queue.front());
        g_queue.pop_front();
        g_busy = true;
        lock.unlock();
        if (!st.sink_open) {
          const auto open_started = clock::now();
          if (ensure_sink(st, factory) && clock::now() - open_started >= slow_open_threshold) {
            // The sink was slow to open: everything queued meanwhile is stale. Keep only the newest packets.
            lock.lock();
            while (g_queue.size() > keep_after_open) {
              g_queue.pop_front();
            }
            if (g_queue.size() == keep_after_open) {
              pkt = std::move(g_queue.front());
              g_queue.pop_front();
            }
            lock.unlock();
            st.have_last = false;
            if (st.decoder) {
              opus_decoder_ctl(st.decoder.get(), OPUS_RESET_STATE);
            }
          }
        }
        if (st.generation != pkt.generation) {
          st.generation = pkt.generation;
          reset_stream(st);
        }
        process(st, factory, pkt);
        lock.lock();
        last_activity = clock::now();
      }
      g_busy = false;
      g_idle_cv.notify_all();
      lock.unlock();
      close_stream(st);
    }
  }  // namespace

  void set_sink_factory(sink_factory_t factory) {
    std::lock_guard lock {g_mutex};
    g_factory = std::move(factory);
  }

  void set_worker_init(std::function<void()> init) {
    std::lock_guard lock {g_mutex};
    g_worker_init = std::move(init);
  }

  void submit(std::uint64_t session_id, std::uint16_t seq, std::string_view opus) {
    if (opus.empty() || opus.size() > max_opus_bytes) {
      return;
    }
    {
      std::lock_guard lock {g_mutex};
      if (g_shutdown) {
        return;
      }
      const auto now = clock::now();
      if (g_has_owner && g_owner != session_id && now - g_owner_last_packet < owner_silence_timeout) {
        return;
      }
      if (!g_has_owner || g_owner != session_id) {
        g_has_owner = true;
        g_owner = session_id;
        ++g_owner_generation;
        g_rate_count = 0;
      }
      g_owner_last_packet = now;

      // Per-owner rate cap: at most max_packets_per_second packets in any 1 s window.
      if (g_rate_count == 0 || now - g_rate_window_start >= 1s) {
        g_rate_window_start = now;
        g_rate_count = 0;
      }
      if (++g_rate_count > max_packets_per_second) {
        return;
      }

      if (g_queue.size() >= max_queue) {
        return;
      }
      g_queue.push_back({seq, g_owner_generation, std::vector<std::uint8_t>(opus.begin(), opus.end())});

      if (!g_started) {
        g_started = true;
        g_worker = std::thread {worker_main, g_factory};
      }
    }
    g_cv.notify_one();
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
      g_shutdown = true;
      worker = std::move(g_worker);
    }
    g_cv.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
  }

  void reset_for_tests() {
    shutdown();
    std::lock_guard lock {g_mutex};
    g_queue.clear();
    g_started = false;
    g_shutdown = false;
    g_busy = false;
    g_has_owner = false;
    g_rate_count = 0;
    g_worker_init = nullptr;
    g_idle_timeout = 3s;
    g_sink_retry_ms = 5000;
    ++g_owner_generation;
  }

  void set_sink_retry_for_testing(std::chrono::milliseconds interval) {
    g_sink_retry_ms = interval.count();
  }

  void set_idle_timeout_for_testing(std::chrono::milliseconds timeout) {
    std::lock_guard lock {g_mutex};
    g_idle_timeout = timeout;
  }

  bool wait_idle_for_testing(std::chrono::milliseconds timeout) {
    std::unique_lock lock {g_mutex};
    return g_idle_cv.wait_for(lock, timeout, [] {
      return g_queue.empty() && !g_busy;
    });
  }
}  // namespace mic::receiver
