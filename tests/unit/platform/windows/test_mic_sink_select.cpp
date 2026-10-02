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
