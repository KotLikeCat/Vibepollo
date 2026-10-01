/**
 * @file src/clipboard/formats.cpp
 * @brief Portable conversions between wire clipboard formats and Windows clipboard formats.
 */
#include "formats.h"

#include <cstdio>
#include <cstring>

namespace clipboard {
  namespace {
    constexpr std::string_view k_prefix = "<html><body>\r\n<!--StartFragment-->";
    constexpr std::string_view k_suffix = "<!--EndFragment-->\r\n</body></html>";
    constexpr std::string_view k_start_marker = "<!--StartFragment-->";
    constexpr std::string_view k_end_marker = "<!--EndFragment-->";

    std::string cf_html_header(std::size_t start_html, std::size_t end_html, std::size_t start_fragment, std::size_t end_fragment) {
      char buffer[160];
      const int length = std::snprintf(
        buffer,
        sizeof(buffer),
        "Version:0.9\r\nStartHTML:%010zu\r\nEndHTML:%010zu\r\nStartFragment:%010zu\r\nEndFragment:%010zu\r\n",
        start_html,
        end_html,
        start_fragment,
        end_fragment
      );
      return std::string(buffer, static_cast<std::size_t>(length));
    }

    std::optional<std::size_t> read_offset(std::string_view html, std::string_view key) {
      auto pos = html.find(key);
      if (pos == std::string_view::npos) {
        return std::nullopt;
      }
      pos += key.size();
      std::size_t value = 0;
      std::size_t digits = 0;
      while (pos < html.size() && html[pos] >= '0' && html[pos] <= '9' && digits < 12) {
        value = value * 10 + static_cast<std::size_t>(html[pos] - '0');
        ++pos;
        ++digits;
      }
      if (digits == 0) {
        return std::nullopt;
      }
      return value;
    }
  }  // namespace

  std::string crlf_to_lf(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\r' && i + 1 < text.size() && text[i + 1] == '\n') {
        continue;
      }
      out.push_back(text[i]);
    }
    return out;
  }

  std::string lf_to_crlf(std::string_view text) {
    std::string out;
    out.reserve(text.size() + text.size() / 8);
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (text[i] == '\n' && (i == 0 || text[i - 1] != '\r')) {
        out.push_back('\r');
      }
      out.push_back(text[i]);
    }
    return out;
  }

  std::string cf_html_wrap(std::string_view fragment) {
    const std::size_t header_length = cf_html_header(0, 0, 0, 0).size();
    const std::size_t start_html = header_length;
    const std::size_t start_fragment = start_html + k_prefix.size();
    const std::size_t end_fragment = start_fragment + fragment.size();
    const std::size_t end_html = end_fragment + k_suffix.size();
    std::string out = cf_html_header(start_html, end_html, start_fragment, end_fragment);
    out.append(k_prefix);
    out.append(fragment);
    out.append(k_suffix);
    return out;
  }

  std::optional<std::string> cf_html_extract_fragment(std::string_view cf_html) {
    const auto start = read_offset(cf_html, "StartFragment:");
    const auto end = read_offset(cf_html, "EndFragment:");
    if (start && end && *start <= *end && *end <= cf_html.size()) {
      return std::string(cf_html.substr(*start, *end - *start));
    }
    const auto marker_start = cf_html.find(k_start_marker);
    const auto marker_end = cf_html.find(k_end_marker);
    if (marker_start != std::string_view::npos && marker_end != std::string_view::npos &&
        marker_start + k_start_marker.size() <= marker_end) {
      const auto begin = marker_start + k_start_marker.size();
      return std::string(cf_html.substr(begin, marker_end - begin));
    }
    return std::nullopt;
  }

  std::string trim_png(std::string png) {
    static constexpr char k_signature[] = "\x89PNG\r\n\x1a\n";
    if (png.size() < 8 || std::memcmp(png.data(), k_signature, 8) != 0) {
      return png;
    }
    std::size_t pos = 8;
    while (pos + 12 <= png.size()) {
      const auto *u = reinterpret_cast<const unsigned char *>(png.data() + pos);
      const std::size_t length = (std::size_t(u[0]) << 24) | (std::size_t(u[1]) << 16) | (std::size_t(u[2]) << 8) | std::size_t(u[3]);
      if (length > png.size() - pos - 12) {
        break;
      }
      const bool is_end = std::memcmp(png.data() + pos + 4, "IEND", 4) == 0;
      pos += 12 + length;
      if (is_end) {
        png.resize(pos);
        break;
      }
    }
    return png;
  }
}  // namespace clipboard
