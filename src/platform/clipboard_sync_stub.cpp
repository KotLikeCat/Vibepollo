/**
 * @file src/platform/clipboard_sync_stub.cpp
 * @brief Clipboard sync stubs for platforms without an implementation.
 */
#include "src/platform/clipboard_sync.h"

namespace platf::clipboard_sync {
  bool supported() {
    return false;
  }

  std::uint32_t sequence() {
    return 0;
  }

  std::uint32_t available_formats() {
    return 0;
  }

  std::optional<std::vector<::clipboard::item>> read(std::uint32_t) {
    return std::nullopt;
  }

  std::optional<std::uint32_t> write(const std::vector<::clipboard::item> &) {
    return std::nullopt;
  }
}  // namespace platf::clipboard_sync
