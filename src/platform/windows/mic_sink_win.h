/**
 * @file src/platform/windows/mic_sink_win.h
 * @brief Windows-only helpers of the microphone sink (exposed for tests).
 */
#pragma once

#include <optional>
#include <string>

namespace platf::mic_win {
  /**
   * @brief Resolve the render endpoint that the sink would use.
   * @param name_or_empty Non-empty: case-insensitive "contains" match on id/friendly name/description/adapter name.
   *                      Empty: first of "Steam Streaming Microphone", "CABLE Input". Never a real speaker.
   * @param[out] audio_present Set to true when at least one active render endpoint exists (optional).
   * @return Friendly name of the selected endpoint, or nullopt.
   */
  std::optional<std::string> resolve_endpoint_name(const std::string &name_or_empty, bool *audio_present = nullptr);
}  // namespace platf::mic_win
