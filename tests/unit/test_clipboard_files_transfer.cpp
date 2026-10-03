/**
 * @file tests/unit/test_clipboard_files_transfer.cpp
 * @brief Test src/clipboard/files/transfer.*
 */
#include "../tests_common.h"
#include "src/clipboard/files/manifest.h"
#include "src/clipboard/files/transfer.h"

#include <chrono>
#include <string>
#include <utility>
#include <vector>

using namespace clipboard::files;
using namespace std::chrono_literals;

namespace {
  constexpr std::uint64_t MiB = 1u << 20;

  struct delivered {
    std::uint32_t read_id;
    std::string data;
    bool last;
  };

  struct failure {
    std::uint32_t read_id;
    read_error err;
  };

  struct harness {
    std::vector<chunk_request> requests;
    std::vector<delivered> out;
    std::vector<failure> fails;
    std::chrono::steady_clock::time_point now {std::chrono::steady_clock::time_point {} + 1000s};
    transfer t;

    static offer_id_t make_id() {
      offer_id_t id {};
      for (std::size_t i = 0; i < id.size(); ++i) {
        id[i] = static_cast<std::uint8_t>(i);
      }
      return id;
    }

    explicit harness(transfer_options opt = {}):
        t(
          transfer::callbacks {
            [this](const chunk_request &r) { requests.push_back(r); },
            [this](std::uint32_t id, std::string d, bool last) { out.push_back({id, std::move(d), last}); },
            [this](std::uint32_t id, read_error e) { fails.push_back({id, e}); },
          },
          opt
        ) {}

    void offer(const std::vector<std::uint64_t> &sizes) {
      manifest m;
      m.offer_id = make_id();
      for (std::size_t i = 0; i < sizes.size(); ++i) {
        m.entries.push_back({entry_kind::file, sizes[i], 0, "f" + std::to_string(i)});
      }
      t.set_offer(m);
      t.tick(now);
    }

    bool answer(const chunk_request &r, std::size_t len) {
      return t.on_chunk(r.offer_id, r.request_id, r.file_index, r.offset, std::string(len, 'x'));
    }
  };
}  // namespace

TEST(ClipboardFilesTransfer, SplitsIntoChunksWithinOutstandingLimit) {
  harness h;
  h.offer({20 * MiB, 12 * MiB});
  ASSERT_TRUE(h.t.start_read(1, 0, 0, 20 * MiB));
  ASSERT_EQ(h.requests.size(), 4u);
  for (std::size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(h.requests[i].length, 4 * MiB);
    EXPECT_EQ(h.requests[i].offset, i * 4 * MiB);
    EXPECT_EQ(h.requests[i].offer_id, harness::make_id());
  }
  ASSERT_TRUE(h.answer(h.requests[0], 4 * MiB));
  ASSERT_EQ(h.requests.size(), 5u);
  EXPECT_EQ(h.requests[4].offset, 16 * MiB);
  EXPECT_EQ(h.out.size(), 1u);
}

TEST(ClipboardFilesTransfer, ThreeChunksForTwelveMiB) {
  harness h;
  h.offer({12 * MiB});
  ASSERT_TRUE(h.t.start_read(7, 0, 0, 12 * MiB));
  EXPECT_EQ(h.requests.size(), 3u);
  for (auto &r : h.requests) {
    EXPECT_EQ(r.length, 4 * MiB);
  }
}

TEST(ClipboardFilesTransfer, DeliversInOffsetOrder) {
  harness h;
  h.offer({8 * MiB});
  ASSERT_TRUE(h.t.start_read(1, 0, 0, 8 * MiB));
  ASSERT_EQ(h.requests.size(), 2u);
  ASSERT_TRUE(h.t.on_chunk(h.requests[1].offer_id, h.requests[1].request_id, 0, 4 * MiB, std::string(4 * MiB, 'b')));
  EXPECT_TRUE(h.out.empty());
  ASSERT_TRUE(h.t.on_chunk(h.requests[0].offer_id, h.requests[0].request_id, 0, 0, std::string(4 * MiB, 'a')));
  ASSERT_EQ(h.out.size(), 2u);
  EXPECT_EQ(h.out[0].data[0], 'a');
  EXPECT_FALSE(h.out[0].last);
  EXPECT_EQ(h.out[1].data[0], 'b');
  EXPECT_TRUE(h.out[1].last);
}

TEST(ClipboardFilesTransfer, ManySmallFilesDoNotSerialize) {
  harness h;
  h.offer(std::vector<std::uint64_t>(100, 1024));
  for (std::uint32_t i = 0; i < 100; ++i) {
    ASSERT_TRUE(h.t.start_read(i + 1, i, 0, 1024));
  }
  EXPECT_EQ(h.requests.size(), 4u);
  for (std::size_t i = 0; i < 100; ++i) {
    ASSERT_LT(i, h.requests.size());
    ASSERT_TRUE(h.answer(h.requests[i], 1024));
  }
  EXPECT_EQ(h.requests.size(), 100u);
  EXPECT_EQ(h.out.size(), 100u);
  for (auto &d : h.out) {
    EXPECT_TRUE(d.last);
  }
}

TEST(ClipboardFilesTransfer, RoundRobinBetweenReads) {
  harness h;
  h.offer({64 * MiB, 1024});
  ASSERT_TRUE(h.t.start_read(1, 0, 0, 64 * MiB));
  ASSERT_TRUE(h.t.start_read(2, 1, 0, 1024));
  ASSERT_TRUE(h.answer(h.requests[0], 4 * MiB));
  ASSERT_TRUE(h.answer(h.requests[1], 4 * MiB));
  bool small_seen = false;
  for (auto &r : h.requests) {
    small_seen |= r.file_index == 1;
  }
  EXPECT_TRUE(small_seen);
}

TEST(ClipboardFilesTransfer, ChunkErrorFailsRead) {
  harness h;
  h.offer({20 * MiB, 1024});
  ASSERT_TRUE(h.t.start_read(1, 0, 0, 20 * MiB));
  ASSERT_EQ(h.requests.size(), 4u);
  ASSERT_TRUE(h.t.on_chunk_error(h.requests[1].offer_id, h.requests[1].request_id, read_error::changed));
  ASSERT_EQ(h.fails.size(), 1u);
  EXPECT_EQ(h.fails[0].read_id, 1u);
  EXPECT_EQ(h.fails[0].err, read_error::changed);
  // remaining requests of that read are cancelled: late answers are rejected
  EXPECT_FALSE(h.answer(h.requests[0], 4 * MiB));
  EXPECT_FALSE(h.answer(h.requests[2], 4 * MiB));
  EXPECT_EQ(h.requests.size(), 4u);
}

TEST(ClipboardFilesTransfer, TimeoutRetriesOnceThenFails) {
  harness h;
  h.offer({1024});
  ASSERT_TRUE(h.t.start_read(1, 0, 0, 1024));
  ASSERT_EQ(h.requests.size(), 1u);
  h.t.tick(h.now + 14s);
  EXPECT_EQ(h.requests.size(), 1u);
  h.t.tick(h.now + 15s);
  ASSERT_EQ(h.requests.size(), 2u);
  EXPECT_NE(h.requests[1].request_id, h.requests[0].request_id);
  EXPECT_EQ(h.requests[1].offset, h.requests[0].offset);
  EXPECT_TRUE(h.fails.empty());
  EXPECT_FALSE(h.answer(h.requests[0], 1024));  // old id is dead
  h.t.tick(h.now + 29s);
  EXPECT_TRUE(h.fails.empty());
  h.t.tick(h.now + 30s);
  ASSERT_EQ(h.fails.size(), 1u);
  EXPECT_EQ(h.fails[0].err, read_error::timeout);
  EXPECT_EQ(h.requests.size(), 2u);
}

TEST(ClipboardFilesTransfer, RetryAnswerSucceeds) {
  harness h;
  h.offer({1024});
  ASSERT_TRUE(h.t.start_read(1, 0, 0, 1024));
  h.t.tick(h.now + 15s);
  ASSERT_EQ(h.requests.size(), 2u);
  ASSERT_TRUE(h.answer(h.requests[1], 1024));
  ASSERT_EQ(h.out.size(), 1u);
  EXPECT_TRUE(h.out[0].last);
}

TEST(ClipboardFilesTransfer, LateChunkIsGone) {
  harness h;
  h.offer({1024});
  EXPECT_FALSE(h.t.on_chunk(harness::make_id(), 999, 0, 0, std::string(1024, 'x')));
  EXPECT_FALSE(h.t.on_chunk_error(harness::make_id(), 999, read_error::io));
}

TEST(ClipboardFilesTransfer, NewOfferFailsPendingReads) {
  harness h;
  h.offer({20 * MiB});
  ASSERT_TRUE(h.t.start_read(1, 0, 0, 20 * MiB));
  chunk_request old = h.requests[0];
  manifest m;
  m.offer_id = harness::make_id();
  m.offer_id[0] = 0x99;
  m.entries.push_back({entry_kind::file, 10, 0, "n"});
  h.t.set_offer(m);
  ASSERT_EQ(h.fails.size(), 1u);
  EXPECT_EQ(h.fails[0].err, read_error::gone);
  EXPECT_FALSE(h.answer(old, 4 * MiB));
  ASSERT_TRUE(h.t.current_offer().has_value());
  EXPECT_EQ((*h.t.current_offer())[0], 0x99);
}

TEST(ClipboardFilesTransfer, ClearOfferFailsPendingReads) {
  harness h;
  h.offer({1024});
  ASSERT_TRUE(h.t.start_read(1, 0, 0, 1024));
  h.t.clear_offer();
  ASSERT_EQ(h.fails.size(), 1u);
  EXPECT_EQ(h.fails[0].err, read_error::gone);
  EXPECT_FALSE(h.t.current_offer().has_value());
  EXPECT_FALSE(h.t.start_read(2, 0, 0, 10));
}

TEST(ClipboardFilesTransfer, CancelReadStopsRequests) {
  harness h;
  h.offer({40 * MiB, 1024});
  ASSERT_TRUE(h.t.start_read(1, 0, 0, 40 * MiB));
  ASSERT_EQ(h.requests.size(), 4u);
  h.t.cancel_read(1);
  EXPECT_FALSE(h.answer(h.requests[0], 4 * MiB));
  EXPECT_EQ(h.requests.size(), 4u);
  EXPECT_TRUE(h.fails.empty());
  ASSERT_TRUE(h.t.start_read(2, 1, 0, 1024));
  EXPECT_EQ(h.requests.size(), 5u);  // budget freed
}

TEST(ClipboardFilesTransfer, ShortOrLongChunkIsIoError) {
  harness h;
  h.offer({8 * MiB});
  ASSERT_TRUE(h.t.start_read(1, 0, 0, 8 * MiB));
  EXPECT_TRUE(h.answer(h.requests[0], 4 * MiB - 1));
  ASSERT_EQ(h.fails.size(), 1u);
  EXPECT_EQ(h.fails[0].err, read_error::io);
}

TEST(ClipboardFilesTransfer, ZeroLengthRead) {
  harness h;
  h.offer({0, 10});
  ASSERT_TRUE(h.t.start_read(1, 0, 0, 0));
  EXPECT_TRUE(h.requests.empty());
  ASSERT_EQ(h.out.size(), 1u);
  EXPECT_EQ(h.out[0].read_id, 1u);
  EXPECT_TRUE(h.out[0].data.empty());
  EXPECT_TRUE(h.out[0].last);
}

TEST(ClipboardFilesTransfer, RangeBeyondSizeRejected) {
  harness h;
  h.offer({100});
  EXPECT_FALSE(h.t.start_read(1, 0, 50, 51));
  EXPECT_FALSE(h.t.start_read(1, 0, 101, 0));
  EXPECT_FALSE(h.t.start_read(1, 5, 0, 1));
  EXPECT_TRUE(h.t.start_read(1, 0, 50, 50));
  EXPECT_FALSE(h.t.start_read(1, 0, 0, 1));  // duplicate read id
}

TEST(ClipboardFilesTransfer, NoOfferRejectsRead) {
  harness h;
  EXPECT_FALSE(h.t.start_read(1, 0, 0, 1));
}

TEST(ClipboardFilesTransfer, MismatchedChunkMetadataRejected) {
  harness h;
  h.offer({8 * MiB});
  ASSERT_TRUE(h.t.start_read(1, 0, 0, 8 * MiB));
  auto r = h.requests[0];
  EXPECT_FALSE(h.t.on_chunk(r.offer_id, r.request_id, 0, 123, std::string(r.length, 'x')));
  offer_id_t other = r.offer_id;
  other[0] ^= 0xff;
  EXPECT_FALSE(h.t.on_chunk(other, r.request_id, 0, r.offset, std::string(r.length, 'x')));
}

TEST(ClipboardFilesTransfer, ReorderBufferIsBounded) {
  harness h;
  h.offer({40 * MiB});
  ASSERT_TRUE(h.t.start_read(1, 0, 0, 40 * MiB));
  ASSERT_EQ(h.requests.size(), 4u);
  for (std::size_t i = 1; i < 4; ++i) {
    ASSERT_TRUE(h.answer(h.requests[i], 4 * MiB));
  }
  EXPECT_EQ(h.requests.size(), 4u);  // head missing: no slot released
  EXPECT_TRUE(h.out.empty());
  ASSERT_TRUE(h.answer(h.requests[0], 4 * MiB));
  ASSERT_EQ(h.out.size(), 4u);
  EXPECT_EQ(h.requests.size(), 8u);
}

TEST(ClipboardFilesTransfer, BudgetFreedAfterTimeoutFailure) {
  harness h;
  h.offer({8 * MiB, 1024});
  ASSERT_TRUE(h.t.start_read(1, 0, 0, 8 * MiB));
  ASSERT_TRUE(h.t.start_read(2, 1, 0, 1024));
  h.t.tick(h.now + 15s);
  h.t.tick(h.now + 30s);
  EXPECT_EQ(h.fails.size(), 2u);
  const auto before = h.requests.size();
  ASSERT_TRUE(h.t.start_read(3, 1, 0, 1024));
  EXPECT_EQ(h.requests.size(), before + 1);
}

TEST(ClipboardFilesTransfer, BudgetFreedAfterSetOfferAndCancel) {
  harness h;
  h.offer({40 * MiB});
  ASSERT_TRUE(h.t.start_read(1, 0, 0, 40 * MiB));
  manifest m;
  m.offer_id = harness::make_id();
  m.offer_id[0] = 0x42;
  m.entries.push_back({entry_kind::file, 1024, 0, "n"});
  h.t.set_offer(m);
  auto n = h.requests.size();
  ASSERT_TRUE(h.t.start_read(2, 0, 0, 1024));
  EXPECT_EQ(h.requests.size(), n + 1);
  h.t.cancel_read(2);
  ASSERT_TRUE(h.t.start_read(3, 0, 0, 1024));
  EXPECT_EQ(h.requests.size(), n + 2);
}

TEST(ClipboardFilesTransfer, DeliverCallbackMayReenter) {
  std::vector<chunk_request> requests;
  std::vector<std::uint32_t> delivered_ids;
  transfer *tp = nullptr;
  transfer t(transfer::callbacks {
    [&](const chunk_request &r) { requests.push_back(r); },
    [&](std::uint32_t id, std::string, bool) {
      delivered_ids.push_back(id);
      if (id == 1) {
        EXPECT_TRUE(tp->start_read(2, 0, 0, 10));
      }
    },
    [](std::uint32_t, read_error) {},
  });
  tp = &t;
  manifest m;
  m.offer_id = harness::make_id();
  m.entries.push_back({entry_kind::file, 10, 0, "a"});
  t.set_offer(m);
  ASSERT_TRUE(t.start_read(1, 0, 0, 10));
  ASSERT_TRUE(t.on_chunk(m.offer_id, requests[0].request_id, 0, 0, std::string(10, 'x')));
  ASSERT_EQ(requests.size(), 2u);
  ASSERT_TRUE(t.on_chunk(m.offer_id, requests[1].request_id, 0, 0, std::string(10, 'x')));
  EXPECT_EQ(delivered_ids, (std::vector<std::uint32_t> {1, 2}));
}

TEST(ClipboardFilesTransfer, OffsetsAboveFourGiB) {
  harness h;
  const std::uint64_t big = 5000000000ull;
  h.offer({big});
  const std::uint64_t start = 4294967296ull + 123;
  ASSERT_TRUE(h.t.start_read(1, 0, start, 9 * MiB));
  ASSERT_EQ(h.requests.size(), 3u);
  EXPECT_EQ(h.requests[0].offset, start);
  EXPECT_EQ(h.requests[1].offset, start + 4 * MiB);
  EXPECT_EQ(h.requests[2].offset, start + 8 * MiB);
  EXPECT_EQ(h.requests[2].length, 1 * MiB);
  for (auto &r : h.requests) {
    ASSERT_TRUE(h.answer(r, r.length));
  }
  ASSERT_EQ(h.out.size(), 3u);
  EXPECT_TRUE(h.out[2].last);
}
