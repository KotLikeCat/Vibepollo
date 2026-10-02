/**
 * @file tests/unit/platform/windows/test_clipboard_helper.cpp
 * @brief Runs sunshine_clipboard_helper.exe against the real clipboard.
 */
#include "../../../tests_common.h"
#include "src/clipboard/bundle.h"
#include "src/platform/clipboard_sync.h"

#include <windows.h>

#include <string>

namespace {
  using clipboard::item;
  using clipboard::item_type;

  struct run_output {
    bool started = false;
    DWORD exit_code = 0;
    std::string out;
  };

  run_output run_helper(const std::wstring &args) {
    run_output result;
    SECURITY_ATTRIBUTES sa {sizeof(sa), nullptr, TRUE};
    HANDLE read_end = nullptr;
    HANDLE write_end = nullptr;
    if (!CreatePipe(&read_end, &write_end, &sa, 0)) {
      return result;
    }
    SetHandleInformation(read_end, HANDLE_FLAG_INHERIT, 0);
    const std::wstring exe = L"" SUNSHINE_CLIPBOARD_HELPER_PATH;
    std::wstring cmd = L"\"" + exe + L"\"" + (args.empty() ? L"" : L" " + args);
    STARTUPINFOW si {};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = write_end;
    si.hStdError = write_end;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi {};
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(write_end);
    if (!ok) {
      CloseHandle(read_end);
      return result;
    }
    result.started = true;
    char buffer[4096];
    DWORD got = 0;
    while (ReadFile(read_end, buffer, sizeof(buffer), &got, nullptr) && got > 0) {
      result.out.append(buffer, got);
    }
    WaitForSingleObject(pi.hProcess, 15000);
    GetExitCodeProcess(pi.hProcess, &result.exit_code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(read_end);
    return result;
  }

  bool write_sample() {
    return platf::clipboard_sync::write({
      {item_type::text, "line1\nline2"},
      {item_type::html, "<b>жирный</b>"},
      {item_type::rtf, "{\\rtf1\\ansi {\\b bold}}"},
    }).has_value();
  }
}  // namespace

TEST(ClipboardHelper, ReadReturnsAllWrittenFormats) {
  if (!write_sample()) {
    GTEST_SKIP() << "Clipboard is not available in this session";
  }
  const auto run = run_helper(L"read 15");
  ASSERT_TRUE(run.started);
  if (run.exit_code == 3) {
    GTEST_SKIP() << "Clipboard is not available in this session";
  }
  ASSERT_EQ(run.exit_code, 0u);
  const auto decoded = clipboard::decode(run.out, 1 << 20);
  ASSERT_EQ(decoded.error, clipboard::decode_error::none);
  ASSERT_EQ(decoded.items.size(), 3u);
  std::string text;
  std::string html;
  std::string rtf;
  for (const auto &entry : decoded.items) {
    if (entry.type == item_type::text) {
      text = entry.data;
    } else if (entry.type == item_type::html) {
      html = entry.data;
    } else if (entry.type == item_type::rtf) {
      rtf = entry.data;
    }
  }
  EXPECT_EQ(text, "line1\nline2");
  EXPECT_EQ(html, "<b>жирный</b>");
  EXPECT_EQ(rtf, "{\\rtf1\\ansi {\\b bold}}");
}

TEST(ClipboardHelper, FormatsPrintsMask) {
  if (!write_sample()) {
    GTEST_SKIP() << "Clipboard is not available in this session";
  }
  const auto run = run_helper(L"formats");
  ASSERT_TRUE(run.started);
  ASSERT_EQ(run.exit_code, 0u);
  const auto mask = static_cast<std::uint32_t>(std::stoul(run.out));
  const std::uint32_t wanted = clipboard::format_text | clipboard::format_html | clipboard::format_rtf;
  EXPECT_EQ(mask & wanted, wanted);
}

TEST(ClipboardHelper, BadArgumentsExit2) {
  const auto run = run_helper(L"");
  ASSERT_TRUE(run.started);
  EXPECT_EQ(run.exit_code, 2u);
  EXPECT_TRUE(run.out.empty());
}
