/**
 * @file src/clipboard/files/types.h
 * @brief Shared types for clipboard file transfer (Mac to host).
 */
#pragma once

#include <array>
#include <cstdint>

namespace clipboard::files {
  using offer_id_t = std::array<std::uint8_t, 16>;

  /// Error reported for a failed range read (wire values shared with X-Clipboard-Error / agent protocol).
  enum class read_error : std::uint8_t {
    gone = 1,
    changed = 2,
    io = 3,
    timeout = 4,
  };
}  // namespace clipboard::files
