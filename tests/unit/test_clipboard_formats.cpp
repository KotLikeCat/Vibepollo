/**
 * @file tests/unit/test_clipboard_formats.cpp
 * @brief Test src/clipboard/formats.*
 */
#include "../tests_common.h"
#include "src/clipboard/formats.h"

namespace {
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
}  // namespace

TEST(ClipboardFormats, ConvertsLineEndings) {
  EXPECT_EQ(clipboard::crlf_to_lf("a\r\nb\nc\r"), "a\nb\nc\r");
  EXPECT_EQ(clipboard::lf_to_crlf("a\nb\r\nc\n"), "a\r\nb\r\nc\r\n");
  EXPECT_EQ(clipboard::lf_to_crlf(clipboard::crlf_to_lf("x\r\ny")), "x\r\ny");
}

TEST(ClipboardFormats, CfHtmlRoundTripAscii) {
  EXPECT_EQ(clipboard::cf_html_extract_fragment(clipboard::cf_html_wrap("<b>bold</b>")), "<b>bold</b>");
}

TEST(ClipboardFormats, CfHtmlRoundTripUtf8) {
  const std::string fragment = "Привет <i>мир</i> 🎉";
  EXPECT_EQ(clipboard::cf_html_extract_fragment(clipboard::cf_html_wrap(fragment)), fragment);
}

TEST(ClipboardFormats, CfHtmlOffsetsPointAtDocument) {
  const auto wrapped = clipboard::cf_html_wrap("<p>x</p>");
  const auto start_html = std::stoul(wrapped.substr(wrapped.find("StartHTML:") + 10, 10));
  const auto end_html = std::stoul(wrapped.substr(wrapped.find("EndHTML:") + 8, 10));
  EXPECT_EQ(wrapped.compare(start_html, 6, "<html>"), 0);
  EXPECT_EQ(end_html, wrapped.size());
}

TEST(ClipboardFormats, CfHtmlFallsBackToMarkers) {
  const std::string html = "Version:0.9\r\nStartHTML:-1\r\n<html><body><!--StartFragment-->hi<!--EndFragment--></body></html>";
  EXPECT_EQ(clipboard::cf_html_extract_fragment(html), "hi");
}

TEST(ClipboardFormats, CfHtmlIgnoresTrailingGarbage) {
  auto wrapped = clipboard::cf_html_wrap("<u>u</u>");
  wrapped += std::string("\0\0garbage", 9);
  EXPECT_EQ(clipboard::cf_html_extract_fragment(wrapped), "<u>u</u>");
}

TEST(ClipboardFormats, CfHtmlRejectsGarbageAndBadOffsets) {
  EXPECT_FALSE(clipboard::cf_html_extract_fragment("not html").has_value());
  EXPECT_FALSE(clipboard::cf_html_extract_fragment("Version:0.9\r\nStartFragment:0000000010\r\nEndFragment:0000099999\r\n").has_value());
}

TEST(ClipboardFormats, TrimPngCutsAfterIend) {
  EXPECT_EQ(clipboard::trim_png(k_png + std::string("\0\0padding", 9)), k_png);
  EXPECT_EQ(clipboard::trim_png(k_png), k_png);
}

TEST(ClipboardFormats, TrimPngLeavesOtherDataAlone) {
  EXPECT_EQ(clipboard::trim_png("abc"), "abc");
  const auto truncated = k_png.substr(0, 20);
  EXPECT_EQ(clipboard::trim_png(truncated), truncated);
}
