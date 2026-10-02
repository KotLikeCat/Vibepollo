/**
 * @file src/platform/mic_sink.h
 * @brief Platform interface for the virtual microphone render endpoint fed by mic::receiver.
 */
#pragma once

#include <cstddef>
#include <memory>
#include <string>

namespace platf {
  /// Renders mono 48 kHz float samples into a virtual microphone device. Used from a single thread.
  class mic_sink {
  public:
    virtual ~mic_sink() = default;
    /// Opens the endpoint. Returns false when no suitable device exists or it cannot be started.
    virtual bool open() = 0;
    /// Queues `frames` mono samples at 48 kHz. May drop samples to bound latency.
    virtual void write(const float *mono48k, std::size_t frames) = 0;
    virtual void close() = 0;
  };

  /// Creates a sink for the named device (empty = automatic selection). Returns nullptr when unsupported.
  std::unique_ptr<mic_sink> create_mic_sink(const std::string &name_or_empty);
  /// True when a suitable render endpoint exists for the given name (empty = automatic selection).
  bool mic_sink_available(const std::string &name_or_empty);
}  // namespace platf
