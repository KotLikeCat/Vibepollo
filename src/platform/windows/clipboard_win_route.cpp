/**
 * @file src/platform/windows/clipboard_win_route.cpp
 * @brief Public clipboard read API for the host. As SYSTEM, Windows hides non-standard clipboard
 *        formats (HTML/RTF/PNG) from the process, so reads are delegated to sunshine_clipboard_helper.exe
 *        running with the interactive user's token.
 */
#include "src/platform/clipboard_sync.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/windows/clipboard_win.h"
#include "src/platform/windows/misc.h"

#include <windows.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

namespace {
  using namespace std::chrono_literals;

  constexpr std::size_t k_max_bundle_bytes = 256ull * 1024 * 1024 + 64 * 1024;
  constexpr int k_exit_busy = 3;
  constexpr int k_exit_empty = 4;

  void warn_throttled(const std::string &message) {
    static std::mutex mutex;
    static std::chrono::steady_clock::time_point last {};
    static bool warned = false;
    const auto now = std::chrono::steady_clock::now();
    {
      std::lock_guard lock(mutex);
      if (warned && now - last < 60s) {
        return;
      }
      warned = true;
      last = now;
    }
    BOOST_LOG(warning) << "Clipboard helper failed, falling back to in-process read: " << message;
  }

  struct tmp_file {
    FILE *file = std::tmpfile();

    tmp_file() = default;
    tmp_file(const tmp_file &) = delete;
    tmp_file &operator=(const tmp_file &) = delete;

    ~tmp_file() {
      if (file) {
        std::fclose(file);
      }
    }
  };

  struct helper_result {
    bool ok = false;  ///< Child ran to completion; exit_code and output are valid.
    int exit_code = -1;
    std::string output;
    std::string error;
  };

  std::filesystem::path helper_dir() {
    std::wstring buffer(MAX_PATH, L'\0');
    const DWORD len = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
    buffer.resize(len);
    return std::filesystem::path(buffer).parent_path() / L"tools";
  }

  helper_result run_helper(const std::string &argument, std::chrono::milliseconds timeout) {
    helper_result result;
    const auto dir = helper_dir();
    const auto exe = dir / L"sunshine_clipboard_helper.exe";
    std::error_code fs_ec;
    if (!std::filesystem::exists(exe, fs_ec)) {
      result.error = "helper not found";
      return result;
    }
    tmp_file out;
    if (!out.file) {
      result.error = "tmpfile failed";
      return result;
    }
    const std::string cmd = "\"" + exe.string() + "\" " + argument;
    boost::filesystem::path working_dir {dir.string()};
    std::error_code ec;
    platf::bp::group group;
    auto child = platf::run_command(false, false, cmd, working_dir, platf::bp::this_process::env(), out.file, ec, &group);
    if (ec || !child.valid()) {
      result.error = "spawn failed: " + (ec ? ec.message() : std::string("invalid child"));
      return result;
    }
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::error_code run_ec;
    while (child.running(run_ec)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        std::error_code term_ec;
        group.terminate(term_ec);
        child.wait(run_ec);
        result.error = "timeout";
        return result;
      }
      std::this_thread::sleep_for(10ms);
    }
    child.wait(run_ec);
    result.exit_code = child.exit_code();
    std::fflush(out.file);
    std::fseek(out.file, 0, SEEK_END);
    const long size = std::ftell(out.file);
    std::fseek(out.file, 0, SEEK_SET);
    if (size < 0 || static_cast<unsigned long long>(size) > k_max_bundle_bytes) {
      result.error = "bad output size";
      return result;
    }
    result.output.resize(static_cast<std::size_t>(size));
    if (size > 0 && std::fread(result.output.data(), 1, result.output.size(), out.file) != result.output.size()) {
      result.error = "short read of helper output";
      result.output.clear();
      return result;
    }
    result.ok = true;
    return result;
  }
}  // namespace

namespace platf::clipboard_sync {
  std::uint32_t available_formats() {
    if (!platf::is_running_as_system()) {
      return local::available_formats();
    }
    const auto result = run_helper("formats", 2000ms);
    if (!result.ok) {
      warn_throttled(result.error);
      return local::available_formats();
    }
    if (result.exit_code != 0) {
      if (result.exit_code != k_exit_busy) {
        warn_throttled("formats exit code " + std::to_string(result.exit_code));
      }
      return local::available_formats();
    }
    char *end = nullptr;
    const auto value = std::strtoul(result.output.c_str(), &end, 10);
    if (end == result.output.c_str()) {
      warn_throttled("unparsable formats output");
      return local::available_formats();
    }
    return static_cast<std::uint32_t>(value) & ::clipboard::format_all;
  }

  std::optional<std::vector<::clipboard::item>> read(std::uint32_t formats_mask) {
    if (!platf::is_running_as_system()) {
      return local::read(formats_mask);
    }
    const auto result = run_helper("read " + std::to_string(formats_mask), 10000ms);
    if (!result.ok) {
      warn_throttled(result.error);
      return local::read(formats_mask);
    }
    switch (result.exit_code) {
      case 0:
        {
          auto decoded = ::clipboard::decode(result.output, k_max_bundle_bytes);
          if (decoded.error != ::clipboard::decode_error::none) {
            warn_throttled(std::string("decode error: ") + ::clipboard::error_name(decoded.error));
            return local::read(formats_mask);
          }
          return std::move(decoded.items);
        }
      case k_exit_empty:
        return std::vector<::clipboard::item> {};
      case k_exit_busy:
        return std::nullopt;
      default:
        warn_throttled("read exit code " + std::to_string(result.exit_code));
        return local::read(formats_mask);
    }
  }
}  // namespace platf::clipboard_sync
