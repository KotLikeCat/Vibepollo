/**
 * @file src/platform/mic_sink_stub.cpp
 * @brief Microphone sink stubs for platforms without an implementation.
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
