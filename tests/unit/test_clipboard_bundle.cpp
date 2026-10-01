/**
 * @file tests/unit/test_clipboard_bundle.cpp
 * @brief Test src/clipboard/bundle.*
 */
#include "../tests_common.h"
#include "src/clipboard/bundle.h"

#include <fstream>
#include <sstream>

namespace {
  std::string from_hex(std::string_view hex) {
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
      out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return out;
  }

  struct vector_case {
    std::string name;
    std::string expect;
    std::string data;
  };

  std::vector<vector_case> load_vectors() {
    std::ifstream in(CLIPBOARD_VECTORS_PATH);
    std::vector<vector_case> cases;
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }
      std::istringstream fields(line);
      vector_case c;
      std::string hex;
      fields >> c.name >> c.expect >> hex;
      c.data = from_hex(hex);
      cases.push_back(c);
    }
    return cases;
  }
}  // namespace

TEST(ClipboardBundle, SharedVectors) {
  const auto cases = load_vectors();
  ASSERT_EQ(cases.size(), 12u);
  for (const auto &c : cases) {
    const auto result = clipboard::decode(c.data, 1024);
    if (c.expect == "ok" || c.expect == "ok_lossy") {
      EXPECT_EQ(result.error, clipboard::decode_error::none) << c.name;
      const auto reencoded = clipboard::encode(result.items);
      if (c.expect == "ok") {
        EXPECT_EQ(reencoded, c.data) << c.name;
      } else {
        EXPECT_NE(reencoded, c.data) << c.name;
      }
    } else {
      EXPECT_STREQ(clipboard::error_name(result.error), c.expect.c_str()) << c.name;
      EXPECT_TRUE(result.items.empty()) << c.name;
    }
  }
}

TEST(ClipboardBundle, RejectsInputOverLimit) {
  const auto data = clipboard::encode({{clipboard::item_type::text, std::string(100, 'a')}});
  EXPECT_EQ(clipboard::decode(data, 50).error, clipboard::decode_error::too_large);
  EXPECT_EQ(clipboard::decode(data, data.size()).error, clipboard::decode_error::none);
}

TEST(ClipboardBundle, KeepsOrderAndComputesMask) {
  const std::vector<clipboard::item> items {
    {clipboard::item_type::text, "t"},
    {clipboard::item_type::png, "p"},
  };
  const auto encoded = clipboard::encode(items);
  EXPECT_EQ(encoded.size(), clipboard::encoded_size(items));
  const auto decoded = clipboard::decode(encoded, 1024);
  ASSERT_EQ(decoded.items.size(), 2u);
  EXPECT_EQ(decoded.items[0].type, clipboard::item_type::text);
  EXPECT_EQ(decoded.items[1].type, clipboard::item_type::png);
  EXPECT_EQ(clipboard::mask_of(decoded.items), clipboard::format_text | clipboard::format_png);
  EXPECT_EQ(clipboard::mask_of(clipboard::item_type::rtf), clipboard::format_rtf);
}
