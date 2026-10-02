/**
 * @file src/platform/windows/clipboard_win.h
 * @brief Direct Win32 clipboard reads in the calling process context (Windows only).
 * @note When running as SYSTEM these only see standard formats; use platf::clipboard_sync::read() instead.
 */
#pragma once

#include "src/clipboard/bundle.h"

#include <cstdint>
#include <optional>
#include <vector>

namespace platf::clipboard_sync::local {
  /// Bitmask of clipboard::format_* available to the calling process.
  std::uint32_t available_formats();
  /// Reads the requested formats; nullopt when the clipboard cannot be opened.
  std::optional<std::vector<::clipboard::item>> read(std::uint32_t formats_mask);
}  // namespace platf::clipboard_sync::local
