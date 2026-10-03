/**
 * @file src/platform/windows/clipboard_agent_host.cpp
 * @brief Launches and supervises the user-session clipboard agent and talks to it over a framed pipe.
 */
// standard includes
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>

// local includes
#include "src/logging.h"
#include "src/platform/clipboard_agent.h"
#include "src/platform/windows/ipc/misc_utils.h"
#include "src/platform/windows/ipc/pipes.h"
#include "src/platform/windows/ipc/process_handler.h"
#include "src/platform/windows/misc.h"

namespace platf::clipboard_agent {
  using namespace std::literals;
  namespace agent = clipboard::files::agent;
  using platf::dxgi::AnonymousPipeFactory;
  using platf::dxgi::AsyncNamedPipe;
  using platf::dxgi::FramedPipeFactory;
  using platf::dxgi::INamedPipe;
  using platf::dxgi::NamedPipeFactory;

  namespace {
    constexpr auto kPingInterval = 5s;
    constexpr auto kLivenessTimeout = 20s;
    constexpr auto kMinCooldown = 2s;
    constexpr auto kMaxCooldown = 30s;
    constexpr auto kMissingExeCooldown = 30s;
    constexpr auto kStableRun = 30s;
    constexpr std::size_t kMaxQueuedBytes = 64u << 20;

    std::mutex g_mutex;  // guards g_thread, g_on_message, g_queue and the wake condition
    std::condition_variable g_cv;
    std::jthread g_thread;
    std::function<void(const agent::message &)> g_on_message;
    std::function<void(bool)> g_on_connection;
    std::deque<std::string> g_queue;
    std::size_t g_queued_bytes = 0;
    std::atomic<bool> g_running {false};
    std::atomic<bool> g_connected {false};

    std::filesystem::path agent_exe_path() {
      std::wstring buf(MAX_PATH, L'\0');
      const auto n = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
      buf.resize(n);
      return std::filesystem::path(buf).parent_path() / L"tools" / L"sunshine_clipboard_agent.exe";
    }

    /// Sleeps up to `d`, returns false as soon as stop is requested.
    bool interruptible_wait(std::chrono::milliseconds d) {
      std::unique_lock lock(g_mutex);
      g_cv.wait_for(lock, d, [] {
        return !g_running.load();
      });
      return g_running.load();
    }

    enum class run_result {
      no_session,
      launch_failed,
      connect_failed,
      ended,  ///< connected at some point; ended for any reason
    };

    // Note: agent->core frames must stay small (AsyncNamedPipe has a 64 KiB receive buffer); range_data (up to 1 MiB)
    // flows core->agent only.

    void notify_connection(bool state) {
      std::function<void(bool)> cb;
      {
        std::lock_guard lock(g_mutex);
        cb = g_on_connection;
      }
      if (cb) {
        cb(state);
      }
    }

    /// True when an agent launched now would land in the active console user's session.
    bool console_user_available(DWORD session) {
      if (platf::is_running_as_system()) {
        HANDLE token = nullptr;
        if (!WTSQueryUserToken(session, &token)) {
          return false;
        }
        CloseHandle(token);
        return true;
      }
      // Not SYSTEM: the agent would inherit sunshine's own user/session, which must be the console session.
      DWORD own = 0xFFFFFFFF;
      return ProcessIdToSessionId(GetCurrentProcessId(), &own) && own == session;
    }

    run_result run_once(const std::filesystem::path &exe, DWORD session, bool use_named, std::chrono::steady_clock::duration &connected_for) {
      connected_for = {};
      const std::string guid = platf::dxgi::generate_guid();

      std::unique_ptr<INamedPipe> server;
      if (use_named) {
        server = FramedPipeFactory(std::make_unique<NamedPipeFactory>()).create_server(guid);
      } else {
        server = FramedPipeFactory(std::make_unique<AnonymousPipeFactory>()).create_server(guid);
      }
      if (!server) {
        BOOST_LOG(warning) << "Clipboard agent: failed to create the control pipe ("sv << (use_named ? "named"sv : "anonymous"sv) << ')';
        return run_result::launch_failed;
      }

      // The agent owns the user's clipboard, so it must run in the user's session, never as SYSTEM
      // (allow_system_fallback stays false).
      ProcessHandler proc(/*use_job=*/false);
      if (!proc.start(exe.wstring(), platf::from_utf8(guid), /*allow_system_fallback=*/false)) {
        return run_result::launch_failed;
      }

      server->wait_for_client_connection(5000);
      if (!server->is_connected()) {
        BOOST_LOG(warning) << "Clipboard agent did not connect to the control pipe in time"sv;
        proc.terminate();
        return run_result::connect_failed;
      }

      {
        std::lock_guard lock(g_mutex);
        g_queue.clear();  // nothing queued for a previous agent may reach this one
        g_queued_bytes = 0;
      }
      std::atomic<bool> broken {false};
      std::atomic<bool> hello_ok {false};
      std::atomic<std::int64_t> last_rx {std::chrono::steady_clock::now().time_since_epoch().count()};
      auto touch = [&] {
        last_rx.store(std::chrono::steady_clock::now().time_since_epoch().count());
      };

      AsyncNamedPipe pipe(std::move(server));
      const bool started = pipe.start(
        [&](std::span<const std::uint8_t> bytes) {
          touch();
          auto m = agent::decode(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
          if (!m) {
            BOOST_LOG(warning) << "Clipboard agent: dropped an undecodable frame"sv;
            return;
          }
          if (m->type == agent::msg::pong) {
            return;
          }
          if (m->type == agent::msg::hello) {
            if (auto h = agent::decode_hello(m->payload)) {
              BOOST_LOG(info) << "Clipboard agent connected (pid "sv << h->pid << ", protocol "sv << h->version << ')';
              if (h->version != agent::protocol_version) {
                BOOST_LOG(warning) << "Clipboard agent protocol mismatch; expected "sv << agent::protocol_version;
                broken = true;
              } else {
                {
                  std::lock_guard lock(g_mutex);
                  g_connected = true;
                }
                hello_ok = true;
                notify_connection(true);
              }
            }
            return;
          }
          std::function<void(const agent::message &)> cb;
          {
            std::lock_guard lock(g_mutex);
            cb = g_on_message;
          }
          if (cb && hello_ok.load()) {
            cb(*m);
          }
        },
        [&](const std::string &err) {
          BOOST_LOG(warning) << "Clipboard agent pipe error: "sv << err;
          broken = true;
        },
        [&]() {
          broken = true;
        }
      );
      if (!started) {
        proc.terminate();
        return run_result::connect_failed;
      }

      const auto connected_at = std::chrono::steady_clock::now();
      auto next_ping = connected_at;
      std::uint32_t nonce = 0;

      while (g_running.load() && !broken.load()) {
        {
          std::unique_lock lock(g_mutex);
          g_cv.wait_for(lock, 500ms, [] {
            return !g_running.load() || !g_queue.empty();
          });
        }
        // Pop one frame at a time so the byte counter covers everything not yet written (at most one frame is in flight).
        while (g_running.load() && !broken.load()) {
          std::string frame;
          {
            std::lock_guard lock(g_mutex);
            if (g_queue.empty()) {
              break;
            }
            frame = std::move(g_queue.front());
            g_queue.pop_front();
            g_queued_bytes -= frame.size();
          }
          pipe.send(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(frame.data()), frame.size()));
          if (!pipe.is_connected()) {
            broken = true;
          }
        }

        const auto now = std::chrono::steady_clock::now();
        if (now >= next_ping) {
          const auto ping = agent::encode_ping(++nonce);
          pipe.send(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(ping.data()), ping.size()));
          next_ping = now + kPingInterval;
        }

        const HANDLE ph = proc.get_process_handle();
        if (ph == nullptr || WaitForSingleObject(ph, 0) != WAIT_TIMEOUT) {
          DWORD exit_code = 0;
          if (ph != nullptr) {
            GetExitCodeProcess(ph, &exit_code);
          }
          BOOST_LOG(warning) << "Clipboard agent exited (code "sv << exit_code << ')';
          break;
        }
        const std::chrono::steady_clock::time_point rx {std::chrono::steady_clock::duration {last_rx.load()}};
        if (!hello_ok.load() && now - connected_at > 10s) {
          BOOST_LOG(warning) << "Clipboard agent sent no hello; restarting"sv;
          break;
        }
        if (now - rx > kLivenessTimeout) {
          BOOST_LOG(warning) << "Clipboard agent stopped responding; restarting"sv;
          break;
        }
        DWORD current = WTSGetActiveConsoleSessionId();
        if (current != session) {
          BOOST_LOG(info) << "Active console session changed; relaunching clipboard agent"sv;
          break;
        }
      }

      bool was_connected = false;
      {
        std::lock_guard lock(g_mutex);
        was_connected = g_connected;
        g_connected = false;
        g_queue.clear();
        g_queued_bytes = 0;
      }
      connected_for = std::chrono::steady_clock::now() - connected_at;
      pipe.stop();
      proc.terminate();
      if (was_connected) {
        notify_connection(false);
      }
      return run_result::ended;
    }

    void supervise() {
      const auto exe = agent_exe_path();
      bool logged_missing = false;
      bool logged_no_session = false;
      std::chrono::steady_clock::duration cooldown = kMinCooldown;
      unsigned connect_failures = 0;

      while (g_running.load()) {
        std::error_code ec;
        if (!std::filesystem::exists(exe, ec)) {
          if (!logged_missing) {
            BOOST_LOG(warning) << "Clipboard agent not found at "sv << exe.string() << "; clipboard file transfer is unavailable, will keep retrying"sv;
            logged_missing = true;
          }
          if (!interruptible_wait(kMissingExeCooldown)) {
            return;
          }
          continue;
        }
        logged_missing = false;

        const DWORD session = WTSGetActiveConsoleSessionId();
        if (session == 0xFFFFFFFF) {
          if (!logged_no_session) {
            BOOST_LOG(info) << "Clipboard agent: no active console session; waiting"sv;
            logged_no_session = true;
          }
          if (!interruptible_wait(2s)) {
            return;
          }
          continue;
        }
        if (!console_user_available(session)) {
          if (!logged_no_session) {
            BOOST_LOG(info) << "Clipboard agent: no logged-on console user yet; waiting"sv;
            logged_no_session = true;
          }
          if (!interruptible_wait(2s)) {
            return;
          }
          continue;
        }
        logged_no_session = false;

        std::chrono::steady_clock::duration connected_for {};
        const auto result = run_once(exe, session, connect_failures % 2 == 1, connected_for);
        if (!g_running.load()) {
          return;
        }
        if (result == run_result::ended) {
          connect_failures = 0;
          cooldown = connected_for >= kStableRun ? std::chrono::steady_clock::duration {kMinCooldown} : std::min<std::chrono::steady_clock::duration>(cooldown * 2, kMaxCooldown);
        } else {
          ++connect_failures;
          cooldown = std::min<std::chrono::steady_clock::duration>(cooldown * 2, kMaxCooldown);
        }
        if (!interruptible_wait(std::chrono::duration_cast<std::chrono::milliseconds>(cooldown))) {
          return;
        }
      }
    }
  }  // namespace

  bool start(std::function<void(const agent::message &)> on_message, std::function<void(bool)> on_connection_changed) {
    std::lock_guard lock(g_mutex);
    if (g_running.load()) {
      return true;
    }
    g_on_message = std::move(on_message);
    g_on_connection = std::move(on_connection_changed);
    g_running = true;
    try {
      g_thread = std::jthread([] {
        try {
          supervise();
        } catch (const std::exception &e) {
          BOOST_LOG(error) << "Clipboard agent supervisor failed: "sv << e.what();
        }
      });
    } catch (...) {
      g_running = false;
      return false;
    }
    return true;
  }

  void stop() {
    std::jthread thread;
    {
      std::lock_guard lock(g_mutex);
      if (!g_running.exchange(false)) {
        return;
      }
      thread = std::move(g_thread);
    }
    g_cv.notify_all();
    if (thread.joinable()) {
      thread.join();
    }
    std::lock_guard lock(g_mutex);
    g_on_message = nullptr;
    g_on_connection = nullptr;
    g_queue.clear();
    g_queued_bytes = 0;
    g_connected = false;
  }

  bool connected() {
    return g_connected.load();
  }

  bool send(const std::string &frame) {
    {
      std::lock_guard lock(g_mutex);
      if (!g_connected || g_queued_bytes + frame.size() > kMaxQueuedBytes) {
        return false;
      }
      g_queue.push_back(frame);
      g_queued_bytes += frame.size();
    }
    g_cv.notify_all();
    return true;
  }
}  // namespace platf::clipboard_agent
