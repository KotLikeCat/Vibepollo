/**
 * @file tools/clipboard_agent/file_stream.h
 * @brief Read-only IStream over a remote file (read-ahead) or a prefetched file on disk.
 */
#pragma once

#include "range_source.h"

#include <objidl.h>
#include <windows.h>

#include <filesystem>
#include <memory>
#include <string>

namespace clipboard_agent {
  inline constexpr std::uint32_t k_piece_bytes = 4u << 20;  ///< size of one range_source read
  inline constexpr std::uint64_t k_readahead_bytes = 16u << 20;  ///< read-ahead window

  /// IStream (refcount 1) over file `index` of the offer, served via `src` with read-ahead.
  IStream *create_remote_stream(std::shared_ptr<range_source> src, std::uint32_t index, std::uint64_t size, std::wstring name, std::int64_t mtime_ms);
  /// IStream (refcount 1) over a prefetched file on disk.
  IStream *create_disk_stream(std::filesystem::path path, std::uint64_t size, std::wstring name, std::int64_t mtime_ms);
  /// Unix milliseconds to FILETIME (clamped at the FILETIME epoch).
  FILETIME filetime_from_unix_ms(std::int64_t ms);
}  // namespace clipboard_agent
