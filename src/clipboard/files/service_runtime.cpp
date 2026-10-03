/**
 * @file src/clipboard/files/service_runtime.cpp
 * @brief Production wiring of the clipboard-files service: agent pipe, control stream, 200 ms ticker.
 */
#include "service.h"

#include "src/clipboard/sync_policy.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/clipboard_agent.h"
#include "src/rtsp.h"
#include "src/stream.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

using namespace std::literals;

namespace clipboard::files::service {
  namespace {
    std::mutex g_run_mutex;
    std::condition_variable g_run_cv;
    bool g_running = false;
    bool g_agent_started = false;
    std::thread g_ticker;

    bool post_request(std::uintptr_t session_id, const chunk_request &req) {
      for (const auto &session : rtsp_stream::get_sessions_snapshot()) {
        if (reinterpret_cast<std::uintptr_t>(session.get()) == session_id) {
          stream::session::post_clipboard_file_request(*session, req);
          return true;
        }
      }
      return false;
    }
  }  // namespace

  void start() {
    std::lock_guard lock {g_run_mutex};
    if (g_running || !config::sunshine.clipboard_files || !config::sunshine.clipboard_sync) {
      return;
    }
    hooks h;
    h.send_frame = [](const std::string &frame) {
      return platf::clipboard_agent::send(frame);
    };
    h.post_request = post_request;
    h.session_alive = [](std::uintptr_t id) {
      for (const auto &session : rtsp_stream::get_sessions_snapshot()) {
        if (reinterpret_cast<std::uintptr_t>(session.get()) == id) {
          return true;
        }
      }
      return false;
    };
    h.session_encrypted = [](std::uintptr_t id) {
      for (const auto &session : rtsp_stream::get_sessions_snapshot()) {
        if (reinterpret_cast<std::uintptr_t>(session.get()) == id) {
          return stream::session::control_encrypted(*session);
        }
      }
      return false;
    };
    h.agent_connected = [] {
      return platf::clipboard_agent::connected();
    };
    h.note_clipboard_set = [](std::uint32_t seq, const std::string &origin) {
      shared_policy().note_host_write(seq, origin);
    };
    h.log = [](const std::string &line) {
      BOOST_LOG(info) << line;
    };
    configure(std::move(h));
    set_prefetch_bytes(config::sunshine.clipboard_files_prefetch_bytes);

    g_agent_started = platf::clipboard_agent::start(
      [](const agent::message &m) {
        handle_agent_message(m);
      },
      [](bool connected) {
        handle_agent_connection(connected);
      }
    );
    if (!g_agent_started) {
      return;
    }
    g_running = true;
    g_ticker = std::thread {[] {
      std::unique_lock lk {g_run_mutex};
      while (g_running) {
        g_run_cv.wait_for(lk, 200ms, [] {
          return !g_running;
        });
        if (!g_running) {
          break;
        }
        lk.unlock();
        tick(std::chrono::steady_clock::now());
        lk.lock();
      }
    }};
  }

  void stop() {
    std::thread ticker;
    bool agent;
    {
      std::lock_guard lock {g_run_mutex};
      g_running = false;
      ticker = std::move(g_ticker);
      agent = g_agent_started;
      g_agent_started = false;
    }
    g_run_cv.notify_all();
    if (ticker.joinable()) {
      ticker.join();
    }
    if (agent) {
      platf::clipboard_agent::stop();
    }
  }
}  // namespace clipboard::files::service
