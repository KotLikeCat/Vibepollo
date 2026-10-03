/**
 * @file src/clipboard/files/types.h
 * @brief Shared types for clipboard file transfer (Mac to host).
 */
#pragma once

#include <array>
#include <cstdint>

namespace clipboard::files {
  using offer_id_t = std::array<std::uint8_t, 16>;
}  // namespace clipboard::files
