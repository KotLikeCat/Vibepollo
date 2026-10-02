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
  constexpr std::size_t max_opus_bytes = 200;

  /// Sets how the worker creates its sink (called once at startup; defaults to no sink).
  void set_sink_factory(sink_factory_t factory);

  /// Queues one Opus packet from `session_id`. Never blocks on decoding or device I/O.
  /// Packets from another session are dropped while the owner is active.
  void submit(std::uint64_t session_id, std::uint16_t seq, std::string_view opus);

  /// Releases ownership when the owning session is torn down.
  void session_ended(std::uint64_t session_id);

  /// Stops and joins the worker thread (closing the sink); later submits are ignored. Call at process exit.
  void shutdown();

  /// Test-only: shuts down, joins and clears all state so the receiver can start again.
  void reset_for_tests();
  /// Test-only: shortens the 3 s idle period after which the sink and decoder are closed.
  void set_idle_timeout_for_testing(std::chrono::milliseconds timeout);

  /// Test-only: shortens the 5 s delay before a failed or closed sink is reopened.
  void set_sink_retry_for_testing(std::chrono::milliseconds interval);

  /// Test hook: waits until the queue is drained and the worker is idle.
  bool wait_idle_for_testing(std::chrono::milliseconds timeout);
}  // namespace mic::receiver
