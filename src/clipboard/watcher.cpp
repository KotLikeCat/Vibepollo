/**
 * @file src/clipboard/watcher.cpp
 * @brief Polls the host clipboard and queues change notifications for streaming clients.
 */
#include "watcher.h"

#include "sync_policy.h"
#include "src/config.h"
#include "src/crypto.h"
#include "src/logging.h"
#include "src/platform/clipboard_sync.h"
#include "src/rtsp.h"
#include "src/stream.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>

namespace clipboard::watcher {
  namespace {
    std::atomic<bool> g_running {false};
    std::thread g_thread;

    void post_to(const std::vector<std::shared_ptr<stream::session_t>> &sessions, const std::vector<std::string> &uuids, std::uint32_t seq, std::uint32_t formats) {
      for (const auto &session : sessions) {
        const auto uuid = stream::session::uuid(*session);
        if (std::find(uuids.begin(), uuids.end(), uuid) != uuids.end()) {
          stream::session::post_clipboard_changed(*session, seq, formats);
        }
      }
    }

    void run() {
      greeting_tracker greeter;
      std::uint32_t last_seq = platf::clipboard_sync::sequence();
      while (g_running.load(std::memory_order_acquire)) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));

        const auto sessions = rtsp_stream::get_sessions_snapshot();
        std::vector<client_info> clients;
        std::vector<std::string> uuids;
        for (const auto &session : sessions) {
          auto uuid = stream::session::uuid(*session);
          const bool can_read = !!(stream::session::permission(*session) & crypto::PERM::clipboard_read);
          clients.push_back({uuid, can_read});
          uuids.push_back(std::move(uuid));
        }
        const auto fresh = greeter.new_sessions(uuids);
        if (sessions.empty()) {
          continue;
        }

        std::uint32_t seq;
        {
          auto write_guard = shared_policy().lock_host_write();
          seq = platf::clipboard_sync::sequence();
        }

        if (seq != last_seq) {
          last_seq = seq;
          if (const auto formats = platf::clipboard_sync::available_formats()) {
            post_to(sessions, shared_policy().recipients(seq, clients), seq, formats);
            BOOST_LOG(debug) << "Clipboard changed: seq "sv << seq << ", formats 0x"sv << std::hex << formats << std::dec;
          }
        } else if (!fresh.empty()) {
          if (const auto formats = platf::clipboard_sync::available_formats()) {
            std::vector<std::string> greet;
            for (const auto &client : clients) {
              if (client.can_read && std::find(fresh.begin(), fresh.end(), client.uuid) != fresh.end()) {
                greet.push_back(client.uuid);
              }
            }
            post_to(sessions, greet, seq, formats);
          }
        }
      }
    }
  }  // namespace

  void start() {
    if (!config::sunshine.clipboard_sync || !platf::clipboard_sync::supported() || g_running.exchange(true)) {
      return;
    }
    g_thread = std::thread(run);
    BOOST_LOG(info) << "Clipboard sync watcher started"sv;
  }

  void stop() {
    if (!g_running.exchange(false)) {
      return;
    }
    if (g_thread.joinable()) {
      g_thread.join();
    }
  }
}  // namespace clipboard::watcher
