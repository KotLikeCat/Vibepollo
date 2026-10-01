/**
 * @file src/clipboard/formats.h
 * @brief Portable conversions between wire clipboard formats and Windows clipboard formats.
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace clipboard {
  std::string crlf_to_lf(std::string_view text);
  std::string lf_to_crlf(std::string_view text);
  /// Wraps an HTML fragment into a Windows "HTML Format" (CF_HTML) document with valid offsets.
  std::string cf_html_wrap(std::string_view fragment);
  /// Extracts the fragment from CF_HTML (offsets first, then comment markers). nullopt if neither works.
  std::optional<std::string> cf_html_extract_fragment(std::string_view cf_html);
  /// Drops any bytes after the PNG IEND chunk. Non-PNG or truncated input is returned unchanged.
  std::string trim_png(std::string png);
}  // namespace clipboard
