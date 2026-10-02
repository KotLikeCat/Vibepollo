/**
 * @file tests/unit/platform/windows/test_mic_sink_select.cpp
 * @brief Pure microphone endpoint selection rule (no audio devices needed; never skips).
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "src/platform/windows/mic_sink_win.h"

namespace {
  using platf::mic_win::endpoint_info;

  endpoint_info ep(const wchar_t *friendly) {
    return {L"{id}", friendly, L"", L""};
  }
}  // namespace

TEST(MicSinkSelect, AutoRulePrefersSteamThenCable) {
  std::vector<endpoint_info> all = {
    ep(L"Speakers (Steam Streaming Speakers)"),
    ep(L"Speakers (Steam Streaming Microphone)"),
    ep(L"Speakers (Realtek)"),
    ep(L"CABLE Input (VB-Audio Virtual Cable)"),
  };
  EXPECT_EQ(platf::mic_win::select_endpoint(all, ""), 1);
  all.erase(all.begin() + 1);
  EXPECT_EQ(platf::mic_win::select_endpoint(all, ""), 2);
  all.pop_back();
  EXPECT_EQ(platf::mic_win::select_endpoint(all, ""), -1);
}

TEST(MicSinkSelect, CaseInsensitiveAndExplicitName) {
  const std::vector<endpoint_info> all = {
    ep(L"Speakers (Realtek)"),
    ep(L"cable INPUT (VB-Audio Virtual Cable)"),
  };
  EXPECT_EQ(platf::mic_win::select_endpoint(all, ""), 1);
  EXPECT_EQ(platf::mic_win::select_endpoint(all, "REALTEK"), 0);
  EXPECT_EQ(platf::mic_win::select_endpoint(all, "vb-audio"), 1);
  EXPECT_EQ(platf::mic_win::select_endpoint(all, "nope"), -1);
}

TEST(MicSinkSelect, MicEndpointIsNotAnEligibleDefaultWhenResettingFromSteamSpeakers) {
  const std::vector<endpoint_info> all = {
    {L"{steam-speakers}", L"Speakers (Steam Streaming Speakers)", L"", L""},
    {L"{steam-mic}", L"Speakers (Steam Streaming Microphone)", L"", L""},
  };
  // The only replacement for Steam Speakers is the mic's render side: it must be classified as the mic sink.
  EXPECT_TRUE(platf::mic_win::is_mic_endpoint(all, "", L"{steam-mic}"));
  EXPECT_FALSE(platf::mic_win::is_mic_endpoint(all, "", L"{steam-speakers}"));
  EXPECT_FALSE(platf::mic_win::is_mic_endpoint(all, "", L""));
  EXPECT_FALSE(platf::mic_win::is_mic_endpoint(all, "", L"{unknown}"));

  // Explicit mic_sink name selects the same endpoint the sink would use.
  const std::vector<endpoint_info> with_cable = {
    {L"{real}", L"Speakers (Realtek)", L"", L""},
    {L"{cable}", L"CABLE Input (VB-Audio Virtual Cable)", L"", L""},
  };
  EXPECT_TRUE(platf::mic_win::is_mic_endpoint(with_cable, "vb-audio", L"{cable}"));
  EXPECT_FALSE(platf::mic_win::is_mic_endpoint(with_cable, "vb-audio", L"{real}"));
}

namespace {
  using platf::mic_win::latency_controller;
  using kind = latency_controller::kind;
  using clk = latency_controller::clock;

  constexpr std::uint32_t rate = 48000;
  constexpr std::uint32_t ms(std::uint32_t v) {
    return rate * v / 1000;
  }

  WAVEFORMATEXTENSIBLE make_fmt(WORD ch, DWORD hz, WORD bits, DWORD subtype_tag) {
    WAVEFORMATEXTENSIBLE f {};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = ch;
    f.Format.nSamplesPerSec = hz;
    f.Format.wBitsPerSample = bits;
    f.Format.nBlockAlign = static_cast<WORD>(ch * bits / 8);
    f.Format.nAvgBytesPerSec = f.Format.nBlockAlign * hz;
    f.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    f.Samples.wValidBitsPerSample = bits;
    f.SubFormat.Data1 = subtype_tag;
    return f;
  }
}  // namespace

TEST(MicSinkPairing, PairsByAdapterName) {
  const endpoint_info render {L"{r}", L"Speakers (Steam Streaming Microphone)", L"", L"Steam Streaming Microphone"};
  const std::vector<endpoint_info> caps = {
    {L"{c0}", L"Microphone (Realtek)", L"", L"Realtek Audio"},
    {L"{c1}", L"Microphone (Steam Streaming Microphone)", L"", L"steam streaming microphone"},
  };
  EXPECT_EQ(platf::mic_win::find_paired_capture(caps, render), 1);

  const endpoint_info cable {L"{r2}", L"CABLE Input (VB-Audio Virtual Cable)", L"", L"VB-Audio Virtual Cable"};
  const std::vector<endpoint_info> cable_caps = {{L"{c2}", L"CABLE Output (VB-Audio Virtual Cable)", L"", L"VB-Audio Virtual Cable"}};
  EXPECT_EQ(platf::mic_win::find_paired_capture(cable_caps, cable), 0);
}

TEST(MicSinkPairing, NoPairWhenNoMatchOrEmptyAdapter) {
  const std::vector<endpoint_info> caps = {{L"{c0}", L"Microphone (Realtek)", L"", L"Realtek Audio"}};
  EXPECT_EQ(platf::mic_win::find_paired_capture(caps, {L"{r}", L"Speakers (Steam)", L"", L"Steam Streaming Microphone"}), -1);
  EXPECT_EQ(platf::mic_win::find_paired_capture({{L"{c}", L"x", L"", L""}}, {L"{r}", L"y", L"", L""}), -1);
  EXPECT_EQ(platf::mic_win::find_paired_capture({}, {L"{r}", L"y", L"", L"A"}), -1);
}

TEST(MicSinkFormat, DifferenceDetection) {
  const auto base = make_fmt(1, 44100, 32, WAVE_FORMAT_IEEE_FLOAT);
  EXPECT_FALSE(platf::mic_win::formats_differ(base.Format, make_fmt(1, 44100, 32, WAVE_FORMAT_IEEE_FLOAT).Format));
  EXPECT_TRUE(platf::mic_win::formats_differ(base.Format, make_fmt(2, 44100, 32, WAVE_FORMAT_IEEE_FLOAT).Format));
  EXPECT_TRUE(platf::mic_win::formats_differ(base.Format, make_fmt(1, 48000, 32, WAVE_FORMAT_IEEE_FLOAT).Format));
  EXPECT_TRUE(platf::mic_win::formats_differ(base.Format, make_fmt(1, 44100, 16, WAVE_FORMAT_IEEE_FLOAT).Format));
  EXPECT_TRUE(platf::mic_win::formats_differ(base.Format, make_fmt(1, 44100, 32, WAVE_FORMAT_PCM).Format));

  // Plain WAVEFORMATEX float equals the EXTENSIBLE float of the same shape.
  WAVEFORMATEX plain = base.Format;
  plain.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
  plain.cbSize = 0;
  EXPECT_FALSE(platf::mic_win::formats_differ(base.Format, plain));
}

TEST(MicSinkLatency, StartsAt40msAndWritesNormally) {
  latency_controller c(rate);
  const auto t0 = clk::now();
  EXPECT_EQ(c.target_frames(), ms(40));
  EXPECT_EQ(c.on_write(ms(40), t0).what, kind::write);
}

TEST(MicSinkLatency, UnderrunRaisesTargetAndPrebuffersIt) {
  latency_controller c(rate);
  const auto t0 = clk::now();
  auto a = c.on_write(0, t0);
  EXPECT_EQ(a.what, kind::prebuffer_then_write);
  EXPECT_EQ(a.frames, ms(60));
  EXPECT_EQ(c.target_frames(), ms(60));
  a = c.on_write(0, t0 + std::chrono::seconds(1));
  EXPECT_EQ(a.frames, ms(80));
}

TEST(MicSinkLatency, TargetCapsAt120ms) {
  latency_controller c(rate);
  auto t = clk::now();
  for (int i = 0; i < 20; ++i) {
    c.on_write(0, t += std::chrono::seconds(1));
  }
  EXPECT_EQ(c.target_frames(), ms(120));
  EXPECT_EQ(c.on_write(0, t + std::chrono::seconds(1)).frames, ms(120));
}

TEST(MicSinkLatency, DecaysAfter30sWithoutUnderrunsNotBelow40ms) {
  latency_controller c(rate);
  const auto t0 = clk::now();
  c.on_write(0, t0);
  c.on_write(0, t0);  // 80 ms
  ASSERT_EQ(c.target_frames(), ms(80));
  c.on_write(ms(80), t0 + std::chrono::seconds(29));
  EXPECT_EQ(c.target_frames(), ms(80));
  c.on_write(ms(80), t0 + std::chrono::seconds(30));
  EXPECT_EQ(c.target_frames(), ms(70));
  c.on_write(ms(70), t0 + std::chrono::seconds(60));
  EXPECT_EQ(c.target_frames(), ms(60));
  for (int i = 3; i < 20; ++i) {
    c.on_write(ms(50), t0 + std::chrono::seconds(30 * i));
  }
  EXPECT_EQ(c.target_frames(), ms(40));
}

TEST(MicSinkLatency, UnderrunResetsCalmTimer) {
  latency_controller c(rate);
  const auto t0 = clk::now();
  c.on_write(0, t0);  // 60 ms
  c.on_write(0, t0 + std::chrono::seconds(20));  // 80 ms
  c.on_write(ms(80), t0 + std::chrono::seconds(45));  // only 25 s since the last underrun
  EXPECT_EQ(c.target_frames(), ms(80));
}

TEST(MicSinkLatency, DropHysteresis) {
  latency_controller c(rate);
  const auto t0 = clk::now();
  // target 40 ms -> drop above 80 ms, resume below 40 ms
  EXPECT_EQ(c.on_write(ms(80), t0).what, kind::write);
  EXPECT_EQ(c.on_write(ms(81), t0).what, kind::drop);
  EXPECT_EQ(c.on_write(ms(60), t0).what, kind::drop);
  EXPECT_EQ(c.on_write(ms(40), t0).what, kind::drop);
  EXPECT_EQ(c.on_write(ms(39), t0).what, kind::write);
  EXPECT_EQ(c.on_write(ms(60), t0).what, kind::write);
}
