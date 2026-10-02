/**
 * @file tests/unit/test_mic_receiver.cpp
 * @brief Tests mic::receiver sequencing, loss concealment and session ownership with a fake sink.
 */
#include "src/mic/receiver.h"

#include <gtest/gtest.h>
#include <opus/opus.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {
  struct sink_state_t {
    std::mutex mutex;
    int opened = 0;
    int closed = 0;
    std::vector<std::size_t> writes;  // frames per write call
  };

  class fake_sink: public platf::mic_sink {
  public:
    explicit fake_sink(std::shared_ptr<sink_state_t> state):
        state_(std::move(state)) {
    }

    bool open() override {
      std::lock_guard lock {state_->mutex};
      ++state_->opened;
      return true;
    }

    void write(const float *samples, std::size_t frames) override {
      EXPECT_NE(samples, nullptr);
      std::lock_guard lock {state_->mutex};
      state_->writes.push_back(frames);
    }

    void close() override {
      std::lock_guard lock {state_->mutex};
      ++state_->closed;
    }

  private:
    std::shared_ptr<sink_state_t> state_;
  };

  class MicReceiverTest: public ::testing::Test {
  protected:
    void SetUp() override {
      mic::receiver::shutdown();
      state = std::make_shared<sink_state_t>();
      mic::receiver::set_sink_factory([s = state]() {
        return std::make_unique<fake_sink>(s);
      });
      int err = 0;
      encoder = opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP, &err);
      ASSERT_EQ(err, OPUS_OK);
    }

    void TearDown() override {
      mic::receiver::shutdown();
      if (encoder) {
        opus_encoder_destroy(encoder);
      }
    }

    // Encodes the next 20 ms of a 440 Hz tone.
    std::string frame() {
      float pcm[mic::receiver::frame_samples];
      for (int i = 0; i < mic::receiver::frame_samples; ++i) {
        pcm[i] = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f * static_cast<float>(phase++) / 48000.0f);
      }
      unsigned char out[200];
      const int n = opus_encode_float(encoder, pcm, mic::receiver::frame_samples, out, sizeof(out));
      EXPECT_GT(n, 0);
      return std::string(reinterpret_cast<char *>(out), static_cast<std::size_t>(n));
    }

    void send(std::uint64_t session, std::uint16_t seq) {
      mic::receiver::submit(session, seq, frame());
    }

    std::size_t settle() {
      EXPECT_TRUE(mic::receiver::wait_idle_for_testing(5s));
      std::lock_guard lock {state->mutex};
      for (const auto n : state->writes) {
        EXPECT_EQ(n, static_cast<std::size_t>(mic::receiver::frame_samples));
      }
      return state->writes.size();
    }

    std::shared_ptr<sink_state_t> state;
    OpusEncoder *encoder = nullptr;
    long phase = 0;
  };
}  // namespace

TEST_F(MicReceiverTest, InOrderFramesAreDecoded) {
  for (std::uint16_t i = 0; i < 5; ++i) {
    send(1, i);
  }
  EXPECT_EQ(settle(), 5u);
  EXPECT_EQ(state->opened, 1);
}

TEST_F(MicReceiverTest, DuplicateAndStaleAreDropped) {
  send(1, 10);
  send(1, 11);
  send(1, 11);  // duplicate
  send(1, 10);  // stale
  send(1, 12);
  EXPECT_EQ(settle(), 3u);
}

TEST_F(MicReceiverTest, SmallGapsAreConcealed) {
  send(1, 0);
  send(1, 2);  // one lost: 1 PLC + frame
  send(1, 5);  // two lost: 2 PLC + frame
  EXPECT_EQ(settle(), 1u + 2u + 3u);
}

TEST_F(MicReceiverTest, LargeGapResetsWithoutPlc) {
  send(1, 0);
  send(1, 4);  // three lost: reset, no PLC
  send(1, 100);
  EXPECT_EQ(settle(), 3u);
}

TEST_F(MicReceiverTest, SequenceWrapIsInOrder) {
  send(1, 65534);
  send(1, 65535);
  send(1, 0);
  send(1, 1);
  EXPECT_EQ(settle(), 4u);
}

TEST_F(MicReceiverTest, SecondSessionIgnoredWhileOwnerActive) {
  send(1, 0);
  send(2, 0);
  send(2, 1);
  send(1, 1);
  EXPECT_EQ(settle(), 2u);
}

TEST_F(MicReceiverTest, SecondSessionAcceptedAfterOwnerEnds) {
  send(1, 0);
  EXPECT_EQ(settle(), 1u);
  mic::receiver::session_ended(1);
  send(2, 500);
  send(2, 501);
  EXPECT_EQ(settle(), 3u);
}

TEST_F(MicReceiverTest, SecondSessionAcceptedAfterOwnerSilent) {
  send(1, 0);
  EXPECT_EQ(settle(), 1u);
  std::this_thread::sleep_for(1100ms);
  send(2, 0);
  EXPECT_EQ(settle(), 2u);
}

TEST_F(MicReceiverTest, MalformedPacketsAreIgnored) {
  mic::receiver::submit(1, 0, std::string_view {});
  mic::receiver::submit(1, 1, std::string(2000, 'x'));
  EXPECT_EQ(settle(), 0u);
}

TEST_F(MicReceiverTest, ShutdownClosesSink) {
  send(1, 0);
  EXPECT_EQ(settle(), 1u);
  mic::receiver::shutdown();
  std::lock_guard lock {state->mutex};
  EXPECT_EQ(state->closed, 1);
}
