/**
 * @file src/clipboard/bundle.h
 * @brief Wire container ("MLCB" v1) for clipboard sync between host and client.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace clipboard {
  enum class item_type : std::uint8_t {
    text = 1,  ///< UTF-8, "\n" line endings
    html = 2,  ///< UTF-8 HTML fragment (no CF_HTML header)
    rtf = 3,  ///< RTF bytes as-is
    png = 4,  ///< PNG image
  };

  struct item {
    item_type type;
    std::string data;
  };

  inline constexpr std::uint32_t format_text = 0x1;
  inline constexpr std::uint32_t format_html = 0x2;
  inline constexpr std::uint32_t format_rtf = 0x4;
  inline constexpr std::uint32_t format_png = 0x8;
  inline constexpr std::uint32_t format_all = 0xF;
  inline constexpr std::size_t max_items = 8;

  enum class decode_error {
    none,
    bad_magic,
    bad_version,
    too_many_items,
    truncated,
    trailing_bytes,
    duplicate_type,
    too_large,
  };

  struct decode_result {
    decode_error error = decode_error::none;
    std::vector<item> items;  ///< Known types only, in wire order. Empty on error.
  };

  std::uint32_t mask_of(item_type type);
  std::uint32_t mask_of(const std::vector<item> &items);
  std::size_t encoded_size(const std::vector<item> &items);
  /// Encodes items in the given order. Caller guarantees at most max_items with unique types.
  std::string encode(const std::vector<item> &items);
  decode_result decode(std::string_view data, std::size_t max_bytes);
  const char *error_name(decode_error error);
}  // namespace clipboard
