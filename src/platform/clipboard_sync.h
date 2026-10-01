/**
 * @file src/platform/clipboard_sync.h
 * @brief Host clipboard access for clipboard sync (implemented on Windows only).
 */
#pragma once

#include "src/clipboard/bundle.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace platf::clipboard_sync {
  /// True when this platform implements host clipboard sync.
  bool supported();
  /// Current clipboard version (Windows: GetClipboardSequenceNumber()).
  std::uint32_t sequence();
  /// Bitmask of clipboard::format_* currently available, without reading any data.
  std::uint32_t available_formats();
  /// Reads the requested formats in wire format. nullopt when the clipboard is unavailable.
  std::optional<std::vector<::clipboard::item>> read(std::uint32_t formats_mask);
  /// Replaces the clipboard with the items. Returns the clipboard version after the write, nullopt on failure.
  std::optional<std::uint32_t> write(const std::vector<::clipboard::item> &items);
}  // namespace platf::clipboard_sync
