/**
 * @file tests/unit/platform/windows/test_clipboard_win.cpp
 * @brief Integration test of src/platform/windows/clipboard_win.cpp against the real clipboard.
 */
#include "../../../tests_common.h"
#include "src/platform/clipboard_sync.h"
#include "src/platform/windows/clipboard_win.h"

#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace {
  namespace cs = platf::clipboard_sync;
  namespace lc = platf::clipboard_sync::local;
  using clipboard::item;
  using clipboard::item_type;
  using namespace std::chrono_literals;

  std::string from_hex(std::string_view hex) {
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
      out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return out;
  }

  const std::string k_png = from_hex(
    "89504e470d0a1a0a0000000d49484452000000010000000108060000001f15c489"
    "0000000d4944415478da636460f85f0f0002870180eb47ba920000000049454e44ae426082");

  std::optional<std::string> find(const std::vector<item> &items, item_type type) {
    for (const auto &entry : items) {
      if (entry.type == type) {
        return entry.data;
      }
    }
    return std::nullopt;
  }
}  // namespace

TEST(ClipboardWin, RoundTripsAllFormats) {
  const std::vector<item> items {
    {item_type::text, "Привет\nмир"},
    {item_type::html, "<b>жирный</b> 🎉"},
    {item_type::rtf, "{\\rtf1\\ansi hello}"},
    {item_type::png, k_png},
  };
  const auto seq = cs::write(items);
  if (!seq) {
    GTEST_SKIP() << "Clipboard is not available in this session";
  }
  EXPECT_TRUE(cs::supported());
  EXPECT_EQ(cs::sequence(), *seq);
  EXPECT_EQ(lc::available_formats(), clipboard::format_all);
  const auto back = lc::read(clipboard::format_all);
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(find(*back, item_type::text), "Привет\nмир");
  EXPECT_EQ(find(*back, item_type::html), "<b>жирный</b> 🎉");
  EXPECT_EQ(find(*back, item_type::rtf), "{\\rtf1\\ansi hello}");
  EXPECT_EQ(find(*back, item_type::png), k_png);
}

TEST(ClipboardWin, ReadHonorsMask) {
  if (!cs::write({{item_type::text, "only"}, {item_type::png, k_png}})) {
    GTEST_SKIP() << "Clipboard is not available in this session";
  }
  const auto back = lc::read(clipboard::format_text);
  ASSERT_TRUE(back.has_value());
  ASSERT_EQ(back->size(), 1u);
  EXPECT_EQ((*back)[0].type, item_type::text);
}

TEST(ClipboardWin, ConvertsDibOnlyImageToPng) {
  BITMAPINFOHEADER header {};
  header.biSize = sizeof(header);
  header.biWidth = 1;
  header.biHeight = 1;
  header.biPlanes = 1;
  header.biBitCount = 32;
  header.biCompression = BI_RGB;
  std::string dib(reinterpret_cast<const char *>(&header), sizeof(header));
  dib.append(std::string("\x00\x00\xff\x00", 4));  // one red pixel, BGRA
  if (!OpenClipboard(nullptr)) {
    GTEST_SKIP() << "Clipboard is not available in this session";
  }
  EmptyClipboard();
  HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, dib.size());
  std::memcpy(GlobalLock(handle), dib.data(), dib.size());
  GlobalUnlock(handle);
  SetClipboardData(CF_DIB, handle);
  CloseClipboard();

  EXPECT_EQ(lc::available_formats() & clipboard::format_png, clipboard::format_png);
  const auto back = lc::read(clipboard::format_png);
  ASSERT_TRUE(back.has_value());
  ASSERT_EQ(back->size(), 1u);
  EXPECT_EQ((*back)[0].data.compare(0, 8, "\x89PNG\r\n\x1a\n", 8), 0);
}

TEST(ClipboardWin, RejectsImageAbovePixelCap) {
  BITMAPINFOHEADER header {};
  header.biSize = sizeof(header);
  header.biWidth = 20000;
  header.biHeight = 20000;  // 400 MP claimed, tiny body
  header.biPlanes = 1;
  header.biBitCount = 32;
  header.biCompression = BI_RGB;
  std::string dib(reinterpret_cast<const char *>(&header), sizeof(header));
  dib.append(std::string("\x00\x00\xff\x00", 4));
  if (!OpenClipboard(nullptr)) {
    GTEST_SKIP() << "Clipboard is not available in this session";
  }
  EmptyClipboard();
  HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, dib.size());
  std::memcpy(GlobalLock(handle), dib.data(), dib.size());
  GlobalUnlock(handle);
  SetClipboardData(CF_DIB, handle);
  CloseClipboard();

  const auto back = lc::read(clipboard::format_png);
  if (back) {
    EXPECT_FALSE(find(*back, item_type::png).has_value());
  }
}

TEST(ClipboardWin, ReportsUnavailableWhenClipboardIsLocked) {
  std::atomic<int> state {0};  // 0 = starting, 1 = holding, 2 = could not open
  std::atomic<bool> release {false};
  std::thread holder([&] {
    // OpenClipboard(nullptr) calls from the same process do not exclude each other,
    // so the holder needs a real window handle to actually lock the clipboard.
    HWND owner = CreateWindowExW(0, L"STATIC", L"clipboard-holder", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, nullptr, nullptr);
    if (!owner || !OpenClipboard(owner)) {
      if (owner) {
        DestroyWindow(owner);
      }
      state = 2;
      return;
    }
    state = 1;
    while (!release) {
      std::this_thread::sleep_for(5ms);
    }
    CloseClipboard();
    DestroyWindow(owner);
  });
  while (state == 0) {
    std::this_thread::sleep_for(1ms);
  }
  if (state == 2) {
    release = true;
    holder.join();
    GTEST_SKIP() << "Clipboard is not available in this session";
  }
  const auto start = std::chrono::steady_clock::now();
  const auto result = lc::read(clipboard::format_all);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  release = true;
  holder.join();
  EXPECT_FALSE(result.has_value());
  EXPECT_LT(elapsed, 500ms);
}

TEST(ClipboardWin, WritesValidDibForPngItems) {
  if (!cs::write({{item_type::png, k_png}})) {
    GTEST_SKIP() << "Clipboard is not available in this session";
  }
  if (!OpenClipboard(nullptr)) {
    GTEST_SKIP() << "Clipboard is not available in this session";
  }
  UINT format = CF_DIBV5;
  HANDLE handle = GetClipboardData(CF_DIBV5);
  if (!handle) {
    format = CF_DIB;
    handle = GetClipboardData(CF_DIB);
  }
  ASSERT_NE(handle, nullptr) << "Neither CF_DIBV5 nor CF_DIB present";
  const auto *header = static_cast<const BITMAPINFOHEADER *>(GlobalLock(handle));
  ASSERT_NE(header, nullptr);
  const DWORD size = header->biSize;
  const LONG width = header->biWidth;
  const LONG height = header->biHeight;
  const WORD bits = header->biBitCount;
  GlobalUnlock(handle);
  CloseClipboard();
  if (format == CF_DIBV5) {
    EXPECT_EQ(size, sizeof(BITMAPV5HEADER));
  } else {
    EXPECT_GE(size, sizeof(BITMAPINFOHEADER));
  }
  EXPECT_EQ(width, 1);
  EXPECT_EQ(std::abs(height), 1);
  EXPECT_EQ(bits, 32);
}
