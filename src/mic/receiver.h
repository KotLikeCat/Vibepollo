/**
 * @file src/mic/receiver.h
 * @brief Receives client microphone Opus frames, conceals loss and feeds a platf::mic_sink.
 */
#pragma once

#include "src/platform/mic_sink.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string_view>

namespace mic::receiver {
  using sink_factory_t = std::function<std::unique_ptr<platf::mic_sink>()>;

  constexpr int sample_rate = 48000;
  constexpr int frame_samples = 960;  ///< 20 ms at 48 kHz
  constexpr std::size_t max_opus_bytes = 1275;

  /// Sets how the worker creates its sink (called once at startup; defaults to no sink).
  void set_sink_factory(sink_factory_t factory);

  /// Queues one Opus packet from `session_id`. Never blocks on decoding or device I/O.
  /// Packets from another session are dropped while the owner is active.
  void submit(std::uint64_t session_id, std::uint16_t seq, std::string_view opus);

  /// Releases ownership when the owning session is torn down.
  void session_ended(std::uint64_t session_id);

  /// Stops the worker thread, closes the sink and clears all state. Restartable.
  void shutdown();

  /// Test hook: waits until the queue is drained and the worker is idle.
  bool wait_idle_for_testing(std::chrono::milliseconds timeout);
}  // namespace mic::receiver
