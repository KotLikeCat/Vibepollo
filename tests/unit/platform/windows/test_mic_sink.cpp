/**
 * @file tests/unit/platform/windows/test_mic_sink.cpp
 * @brief Windows microphone sink: endpoint selection and graceful failure.
 */
#include <gtest/gtest.h>

#include <string>
#include <vector>

#include "src/platform/mic_sink.h"
#include "src/platform/windows/mic_sink_win.h"

namespace {
  const std::string kMissing = "no-such-device-xyz";
}

TEST(MicSinkWin, UnknownDeviceIsNotAvailable) {
  EXPECT_FALSE(platf::mic_sink_available(kMissing));
}

TEST(MicSinkWin, UnknownDeviceOpenFailsGracefully) {
  auto sink = platf::create_mic_sink(kMissing);
  if (sink) {
    EXPECT_FALSE(sink->open());
    sink->close();
  }
}

TEST(MicSinkWin, AutoRuleNeverSelectsRealDevice) {
  bool present = false;
  auto name = platf::mic_win::resolve_endpoint_name("", &present);
  if (!present) {
    GTEST_SKIP() << "no active audio render endpoints on this machine";
  }
  if (!name) {
    SUCCEED() << "auto rule selected nothing (no virtual microphone installed)";
    return;
  }
  const bool virt = name->find("Steam Streaming Microphone") != std::string::npos || name->find("CABLE Input") != std::string::npos;
  EXPECT_TRUE(virt) << "auto rule picked non-virtual endpoint: " << *name;
}
