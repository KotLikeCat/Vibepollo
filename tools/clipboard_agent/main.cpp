/**
 * @file tools/clipboard_agent/main.cpp
 * @brief Resident user-session agent: owns the Windows clipboard for Mac -> host file offers.
 *        Launched by sunshine.exe as the logged-on user (`sunshine_clipboard_agent.exe <pipe guid>`).
 * @note Never writes to stdout/stderr; logging is disabled.
 */
// winsock2.h must precede windows.h (pulled in by the project headers below)
#include <winsock2.h>

#include "data_object.h"
#include "pipe_range_source.h"
#include "prefetch.h"
#include "src/clipboard/files/agent_protocol.h"
#include "src/clipboard/files/manifest.h"
#include "src/platform/windows/ipc/pipes.h"

#include <boost/log/core.hpp>

#include <ole2.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace {
  namespace agent = clipboard::files::agent;
  using clipboard::files::offer_id_t;
  using platf::dxgi::INamedPipe;
  using platf::dxgi::PipeResult;

  constexpr UINT WM_APP_SET_OFFER = WM_APP + 1;  // lparam: heap clipboard_agent::offer*
  constexpr UINT WM_APP_CLEAR_OFFER = WM_APP + 2;  // lparam: heap offer_id_t*
  constexpr UINT WM_APP_PIPE_DEAD = WM_APP + 3;
  constexpr std::size_t k_rx_buffer = 2u * 1024 * 1024 + 4096;  // FramedPipe only returns frames that fit

  std::unique_ptr<INamedPipe> g_pipe;
  std::mutex g_send_mutex;
  std::shared_ptr<clipboard_agent::pipe_range_source> g_source;
  HWND g_hwnd = nullptr;
  std::filesystem::path g_prefetch_root;

  // STA-thread state
  IDataObject *g_object = nullptr;
  offer_id_t g_object_id {};

  bool send_frame(const std::string &frame) {
    std::lock_guard lock(g_send_mutex);
    if (!g_pipe) {
      return false;
    }
    return g_pipe->send(std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t *>(frame.data()), frame.size()), 5000);
  }

  std::filesystem::path default_prefetch_root() {
    std::wstring base(MAX_PATH, L'\0');
    DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), static_cast<DWORD>(base.size()));
    if (n == 0 || n >= base.size()) {
      n = GetTempPathW(static_cast<DWORD>(base.size()), base.data());
      base.resize(n);
      return std::filesystem::path(base) / L"Vibepollo" / L"clipboard";
    }
    base.resize(n);
    return std::filesystem::path(base) / L"Temp" / L"Vibepollo" / L"clipboard";
  }

  std::unique_ptr<INamedPipe> connect(const std::string &guid) {
    using namespace platf::dxgi;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
    do {
      // The host tries an anonymous pipe first and a plain named pipe after a failed connect.
      if (auto p = FramedPipeFactory(std::make_unique<AnonymousPipeFactory>()).create_client(guid); p && p->is_connected()) {
        return p;
      }
      if (auto p = FramedPipeFactory(std::make_unique<NamedPipeFactory>()).create_client(guid); p && p->is_connected()) {
        return p;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(100));
    } while (std::chrono::steady_clock::now() < deadline);
    return nullptr;
  }

  void release_object() {
    if (g_object != nullptr) {
      g_object->Release();
      g_object = nullptr;
    }
  }

  void handle_set_offer(clipboard_agent::offer *raw) {
    std::unique_ptr<clipboard_agent::offer> o(raw);
    const auto id = o->id;
    release_object();
    IDataObject *obj = clipboard_agent::create_data_object(std::move(*o), g_source, g_prefetch_root);
    HRESULT hr = E_FAIL;
    for (int attempt = 0; attempt < 10; ++attempt) {
      hr = OleSetClipboard(obj);
      if (SUCCEEDED(hr)) {
        break;
      }
      Sleep(50);  // clipboard briefly locked by another app
    }
    if (FAILED(hr)) {
      obj->Release();
      send_frame(agent::encode_offer_dropped(id));
      return;
    }
    g_object = obj;
    g_object_id = id;
    send_frame(agent::encode_clipboard_set(GetClipboardSequenceNumber()));
  }

  void handle_clear_offer(const offer_id_t &id) {
    if (g_object == nullptr || g_object_id != id) {
      return;
    }
    if (OleIsCurrentClipboard(g_object) == S_OK) {
      OleSetClipboard(nullptr);
    }
    release_object();
  }

  void handle_clipboard_update() {
    if (g_object != nullptr && OleIsCurrentClipboard(g_object) == S_FALSE) {
      const auto id = g_object_id;
      release_object();
      send_frame(agent::encode_offer_dropped(id));
    }
  }

  LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
      case WM_APP_SET_OFFER:
        handle_set_offer(reinterpret_cast<clipboard_agent::offer *>(lp));
        return 0;
      case WM_APP_CLEAR_OFFER:
        {
          std::unique_ptr<offer_id_t> id(reinterpret_cast<offer_id_t *>(lp));
          handle_clear_offer(*id);
          return 0;
        }
      case WM_CLIPBOARDUPDATE:
        handle_clipboard_update();
        return 0;
      case WM_APP_PIPE_DEAD:
        PostQuitMessage(0);
        return 0;
      default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
  }

  void handle_set_offer_part(agent::offer_assembler &assembler, const std::string &payload) {
    auto done = assembler.add(payload);
    if (!done) {
      return;
    }
    auto &[id, prefetch, mlcf] = *done;
    auto decoded = clipboard::files::decode_manifest(mlcf);
    if (decoded.error != clipboard::files::manifest_error::none) {
      send_frame(agent::encode_offer_dropped(id));
      return;
    }
    auto *o = new clipboard_agent::offer {id, std::move(decoded.value.entries), {}, prefetch};
    o->windows_paths = clipboard::files::sanitize_for_windows(o->entries);
    if (!PostMessageW(g_hwnd, WM_APP_SET_OFFER, 0, reinterpret_cast<LPARAM>(o))) {
      delete o;
    }
  }

  void receive_loop() {
    std::vector<std::uint8_t> buf(k_rx_buffer);
    agent::offer_assembler assembler;
    for (;;) {
      std::size_t n = 0;
      const PipeResult r = g_pipe->receive(buf, n, 500);
      if (r == PipeResult::Timeout) {
        continue;
      }
      if (r != PipeResult::Success) {
        break;
      }
      auto m = agent::decode(std::string_view(reinterpret_cast<const char *>(buf.data()), n));
      if (!m) {
        continue;
      }
      switch (m->type) {
        case agent::msg::ping:
          if (auto nonce = agent::decode_ping(m->payload)) {
            send_frame(agent::encode_pong(*nonce));
          }
          break;
        case agent::msg::range_data:
          if (auto v = agent::decode_range_data(m->payload)) {
            g_source->on_range_data(*v);
          }
          break;
        case agent::msg::range_error:
          if (auto v = agent::decode_range_error(m->payload)) {
            g_source->on_range_error(*v);
          }
          break;
        case agent::msg::set_offer_part:
          handle_set_offer_part(assembler, m->payload);
          break;
        case agent::msg::clear_offer:
          if (auto id = agent::decode_clear_offer(m->payload)) {
            auto *p = new offer_id_t(*id);
            if (!PostMessageW(g_hwnd, WM_APP_CLEAR_OFFER, 0, reinterpret_cast<LPARAM>(p))) {
              delete p;
            }
          }
          break;
        default:
          break;
      }
    }
    g_source->shutdown();
    PostMessageW(g_hwnd, WM_APP_PIPE_DEAD, 0, 0);
  }
}  // namespace

int main(int argc, char **argv) {
  boost::log::core::get()->set_logging_enabled(false);
  if (argc < 2) {
    return 2;
  }
  const std::string guid = argv[1];

  if (FAILED(OleInitialize(nullptr))) {
    return 3;
  }

  g_prefetch_root = default_prefetch_root();
  clipboard_agent::remove_stale_prefetch(g_prefetch_root);

  WNDCLASSW wc {};
  wc.lpfnWndProc = wnd_proc;
  wc.hInstance = GetModuleHandleW(nullptr);
  wc.lpszClassName = L"VibepolloClipboardAgent";
  if (RegisterClassW(&wc) == 0) {
    return 4;
  }
  g_hwnd = CreateWindowExW(0, wc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, nullptr);
  if (g_hwnd == nullptr || !AddClipboardFormatListener(g_hwnd)) {
    return 4;
  }

  g_pipe = connect(guid);
  if (!g_pipe) {
    return 5;
  }
  g_source = std::make_shared<clipboard_agent::pipe_range_source>(send_frame);
  if (!send_frame(agent::encode_hello({agent::protocol_version, GetCurrentProcessId()}))) {
    return 5;
  }

  std::thread rx(receive_loop);

  MSG msg;
  while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  // Shutting down: drop our offer from the clipboard if it is still ours.
  g_pipe->disconnect();
  g_source->shutdown();
  if (rx.joinable()) {
    rx.join();
  }
  if (g_object != nullptr && OleIsCurrentClipboard(g_object) == S_OK) {
    OleSetClipboard(nullptr);
  }
  release_object();
  RemoveClipboardFormatListener(g_hwnd);
  OleUninitialize();
  return 0;
}
