/**
 * @file src/clipboard/files/manifest.h
 * @brief MLCF v1 manifest codec, validation and Windows name sanitizer for clipboard file transfer.
 */
#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace clipboard::files {
  enum class entry_kind : std::uint8_t {
    file = 1,
    directory = 2,
  };

  struct entry {
    entry_kind kind;
    std::uint64_t size;
    std::int64_t mtime_ms;
    std::string path;  ///< UTF-8, '/' separators
  };

  struct manifest {
    offer_id_t offer_id {};
    std::vector<entry> entries;
  };

  enum class manifest_error {
    none,
    bad_magic,
    bad_version,
    truncated,
    empty,
    too_many_entries,
    path_too_long,
    component_too_long,
    bad_component,
    absolute_path,
    backslash,
    nul,
    missing_parent,
    duplicate_path,
    trailing_bytes,
  };

  inline constexpr std::size_t max_entries = 100000;
  inline constexpr std::size_t max_path_bytes = 1024;
  inline constexpr std::size_t max_component_utf16 = 255;
  inline constexpr std::size_t max_manifest_bytes = 32u << 20;
  /// Longest sanitized relative path the host accepts (FILEDESCRIPTORW::cFileName holds MAX_PATH incl. the NUL).
  inline constexpr std::size_t max_windows_path_utf16 = 259;

  struct decode_result {
    manifest value;
    manifest_error error = manifest_error::none;
  };

  decode_result decode_manifest(std::string_view data);
  /// Serializes without validating (callers/tests may build invalid manifests on purpose).
  std::string encode_manifest(const manifest &m);
  std::uint64_t total_size(const manifest &m);
  /// Index-aligned Windows-safe relative paths ('\\' separators, UTF-8).
  std::vector<std::string> sanitize_for_windows(const std::vector<entry> &entries);
  /// Length of valid UTF-8 `s` in UTF-16 code units.
  std::size_t utf16_length(std::string_view s);
  std::string offer_id_hex(const offer_id_t &id);
  std::optional<offer_id_t> parse_offer_id_hex(std::string_view hex);
  const char *error_name(manifest_error e);
}  // namespace clipboard::files
