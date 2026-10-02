/**
 * @file src/platform/windows/mic_sink_win.h
 * @brief Windows-only helpers of the microphone sink (exposed for tests).
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// platform includes
#include <WinSock2.h>
#include <Windows.h>
#include <mmreg.h>

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

  /**
   * @brief Index into `captures` of the capture endpoint that belongs to the same virtual cable as `render`
   *        (equal, case-insensitive, non-empty adapter/device-interface name), or -1.
   */
  int find_paired_capture(const std::vector<endpoint_info> &captures, const endpoint_info &render);

  /**
   * @brief True when two formats differ in channel count, sample rate, bit depth or sample type.
   *        EXTENSIBLE formats are compared by their sub-format; only the fields in `cbSize` are read.
   */
  bool formats_differ(const WAVEFORMATEX &a, const WAVEFORMATEX &b);

  /// True for the known virtual audio cables (Steam Streaming Microphone, VB-Audio); only those get their format rewritten.
  bool is_known_virtual_cable(const endpoint_info &e);

  /// Human readable format, e.g. "1ch 44100 Hz 32-bit float".
  std::string format_to_string(const WAVEFORMATEX &f);

  /**
   * @brief Adaptive latency state machine of the sink (all units are frames).
   *        Starts at 40 ms; every underrun raises the target by 20 ms (max 120 ms); 30 s without underruns lower it by
   *        10 ms (min 40 ms). Incoming frames are dropped above target + 40 ms until the backlog is below the target.
   */
  class latency_controller {
  public:
    using clock = std::chrono::steady_clock;

    enum class kind {
      write,  ///< write the incoming frames
      drop,  ///< discard the incoming frames
      prebuffer_then_write,  ///< push `frames` of silence, then write
    };

    struct action {
      kind what = kind::write;
      std::uint32_t frames = 0;  ///< silence frames for prebuffer_then_write
    };

    explicit latency_controller(std::uint32_t sample_rate);

    /// Forget timers and the drop state (the learned target is kept) when the sink is (re)opened.
    void restart(clock::time_point now);

    /// @param padding_frames Frames queued in the endpoint buffer right now.
    action on_write(std::uint32_t padding_frames, clock::time_point now);

    std::uint32_t target_frames() const {
      return target_;
    }

    std::uint32_t target_ms() const {
      return static_cast<std::uint32_t>(static_cast<std::uint64_t>(target_) * 1000 / rate_);
    }

    std::uint32_t drop_frames() const {
      return target_ + frames_of(drop_margin_ms);
    }

    std::uint32_t max_target_frames() const {
      return frames_of(max_ms);
    }

    static constexpr std::uint32_t min_ms = 40;
    static constexpr std::uint32_t max_ms = 120;
    static constexpr std::uint32_t raise_step_ms = 20;
    static constexpr std::uint32_t lower_step_ms = 10;
    static constexpr std::uint32_t drop_margin_ms = 40;
    static constexpr std::chrono::seconds calm_period {30};
    /// A write after a longer pause than this with an empty buffer is a restart, not an underrun.
    static constexpr std::chrono::milliseconds idle_gap_limit {100};

  private:
    std::uint32_t frames_of(std::uint32_t ms) const {
      return static_cast<std::uint32_t>(static_cast<std::uint64_t>(rate_) * ms / 1000);
    }

    std::uint32_t rate_;
    std::uint32_t target_;
    bool dropping_ = false;
    bool has_write_ = false;
    clock::time_point last_write_ {};
    bool has_event_ = false;
    clock::time_point last_event_ {};  ///< last underrun, decay step or restart
  };
}  // namespace platf::mic_win
