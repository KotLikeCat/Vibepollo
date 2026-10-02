/**
 * @file src/platform/windows/mic_sink.cpp
 * @brief Microphone sink stubs for platforms until the WASAPI implementation lands (task M3).
 */
#include "src/platform/mic_sink.h"

namespace platf {
  std::unique_ptr<mic_sink> create_mic_sink(const std::string &) {
    return nullptr;
  }

  bool mic_sink_available(const std::string &) {
    return false;
  }
}  // namespace platf
