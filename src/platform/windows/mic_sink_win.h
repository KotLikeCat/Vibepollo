/**
 * @file src/platform/windows/mic_sink_win.h
 * @brief Windows-only helpers of the microphone sink (exposed for tests).
 */
#pragma once

#include <optional>
#include <string>
#include <vector>

namespace platf::mic_win {
  /// Names of an active render endpoint (UTF-16).
  struct endpoint_info {
    std::wstring id;
    std::wstring friendly;
    std::wstring desc;
    std::wstring adapter;
  };

  /// Pure selection rule over a list of endpoints; returns the index or -1. Never matches a real speaker in auto mode.
  int select_endpoint(const std::vector<endpoint_info> &infos, const std::string &name);

  /**
   * @brief True when the endpoint with this id is the one the sink would feed for the configured name (or the auto rule).
   *        audio.cpp uses it so a default-device reset never picks the virtual microphone as the host's playback device.
   */
  bool is_mic_endpoint(const std::vector<endpoint_info> &infos, const std::string &name, const std::wstring &id);

  /**
   * @brief Same as above, enumerating the active render endpoints (needs COM initialised on the calling thread).
   * @return false when the endpoint cannot be determined to be the mic sink (including enumeration failure).
   */
  bool is_mic_endpoint_id(const std::string &name, const std::wstring &id);

  /**
   * @brief Resolve the render endpoint that the sink would use.
   * @param name_or_empty Non-empty: case-insensitive "contains" match on id/friendly name/description/adapter name.
   *                      Empty: first of "Steam Streaming Microphone", "CABLE Input". Never a real speaker.
   * @param[out] audio_present Set to true when at least one active render endpoint exists (optional).
   * @return Friendly name of the selected endpoint, or nullopt.
   */
  std::optional<std::string> resolve_endpoint_name(const std::string &name_or_empty, bool *audio_present = nullptr);
}  // namespace platf::mic_win
