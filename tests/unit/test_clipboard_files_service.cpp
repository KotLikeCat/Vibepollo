/**
 * @file tests/unit/test_clipboard_files_service.cpp
 * @brief Test src/clipboard/files/service.* (portable core, fakes injected through the test seam)
 */
#include "../tests_common.h"
#include "src/clipboard/files/manifest.h"
#include "src/clipboard/files/service.h"

#include <chrono>
#include <string>
#include <vector>

using namespace clipboard::files;
namespace ag = clipboard::files::agent;
namespace svc = clipboard::files::service;

namespace {
  constexpr std::uint64_t MiB = 1u << 20;
  constexpr std::uintptr_t owner_session = 0x1000;

  offer_id_t make_id(std::uint8_t base = 0) {
    offer_id_t id {};
    for (std::size_t i = 0; i < id.size(); ++i) {
      id[i] = static_cast<std::uint8_t>(base + i);
    }
    return id;
  }

  std::string make_mlcf(std::uint64_t size, std::uint8_t base = 0) {
    manifest m;
    m.offer_id = make_id(base);
    m.entries.push_back({entry_kind::file, size, 1700000000000, "a.bin"});
    return encode_manifest(m);
  }

  struct fixture: ::testing::Test {
    std::vector<std::string> frames;
    std::vector<std::pair<std::uintptr_t, chunk_request>> posted;
    std::vector<std::string> logs;
    std::vector<std::pair<std::uint32_t, std::string>> notes;
    bool connected = true;
    bool post_ok = true;
    int call_count = 0;
    int fail_call = -1;  ///< the send_frame call with this ordinal fails
    int frames_before_failure = -1;  ///< send_frame fails after this many successes (-1 = never)

    void SetUp() override {
      svc::hooks h;
      h.send_frame = [this](const std::string &f) {
        if (++call_count == fail_call) {
          return false;
        }
        if (frames_before_failure == 0) {
          return false;
        }
        if (frames_before_failure > 0) {
          --frames_before_failure;
        }
        frames.push_back(f);
        return true;
      };
      h.post_request = [this](std::uintptr_t s, const chunk_request &r) {
        if (!post_ok) {
          return false;
        }
        posted.emplace_back(s, r);
        return true;
      };
      h.agent_connected = [this] {
        return connected;
      };
      h.note_clipboard_set = [this](std::uint32_t seq, const std::string &origin) {
        notes.emplace_back(seq, origin);
      };
      h.log = [this](const std::string &l) {
        logs.push_back(l);
      };
      svc::set_test_hooks(std::move(h));
    }

    void TearDown() override {
      svc::set_test_hooks({});
    }

    void read_range(std::uint32_t read_id, const offer_id_t &offer, std::uint32_t file, std::uint64_t offset, std::uint64_t length) {
      svc::handle_agent_message({ag::msg::read_range, ag::encode_read_range({read_id, offer, file, offset, length}).substr(1)});
    }

    std::vector<ag::range_error_t> errors() const {
      std::vector<ag::range_error_t> out;
      for (const auto &f : frames) {
        const auto m = ag::decode(f);
        if (m && m->type == ag::msg::range_error) {
          out.push_back(*ag::decode_range_error(m->payload));
        }
      }
      return out;
    }

    std::vector<ag::range_data_t> data() const {
      std::vector<ag::range_data_t> out;
      for (const auto &f : frames) {
        const auto m = ag::decode(f);
        if (m && m->type == ag::msg::range_data) {
          out.push_back(*ag::decode_range_data(m->payload));
        }
      }
      return out;
    }
  };
}  // namespace

TEST_F(fixture, InstallOfferForwardsFramesAndSetsTransfer) {
  const auto mlcf = make_mlcf(3 * MiB);
  ASSERT_EQ(svc::install_offer(owner_session, mlcf, "uuid-1"), svc::offer_result::ok);
  ASSERT_FALSE(frames.empty());
  std::string reassembled;
  ag::offer_assembler asmb;
  std::optional<std::tuple<offer_id_t, bool, std::string>> done;
  for (const auto &f : frames) {
    const auto m = ag::decode(f);
    ASSERT_TRUE(m && m->type == ag::msg::set_offer_part);
    done = asmb.add(m->payload);
  }
  ASSERT_TRUE(done.has_value());
  EXPECT_EQ(std::get<0>(*done), make_id());
  EXPECT_EQ(std::get<2>(*done), mlcf);
  // The scheduler accepts a read for the offer.
  read_range(1, make_id(), 0, 0, 1024);
  ASSERT_EQ(posted.size(), 1u);
  EXPECT_EQ(posted[0].first, owner_session);
  EXPECT_EQ(posted[0].second.offer_id, make_id());
}

TEST_F(fixture, BadManifestRejected) {
  EXPECT_EQ(svc::install_offer(owner_session, "garbage"), svc::offer_result::bad_manifest);
  manifest empty;
  empty.offer_id = make_id();
  EXPECT_EQ(svc::install_offer(owner_session, encode_manifest(empty)), svc::offer_result::bad_manifest);
  EXPECT_TRUE(frames.empty());
}

TEST_F(fixture, UnsupportedWhenAgentNotConnectedOrPipeDrops) {
  connected = false;
  EXPECT_EQ(svc::install_offer(owner_session, make_mlcf(10)), svc::offer_result::unsupported);
  connected = true;
  frames_before_failure = 0;
  EXPECT_EQ(svc::install_offer(owner_session, make_mlcf(10)), svc::offer_result::unsupported);
  // The failed install must not leave a readable offer behind.
  frames_before_failure = -1;
  read_range(1, make_id(), 0, 0, 5);
  ASSERT_EQ(errors().size(), 1u);
  EXPECT_EQ(errors()[0].error, read_error::gone);
}

TEST_F(fixture, ChunkRoundTripDeliversRangeData) {
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(3 * MiB), "u"), svc::offer_result::ok);
  frames.clear();
  read_range(7, make_id(), 0, 0, 3 * MiB);
  ASSERT_EQ(posted.size(), 1u);
  const auto req = posted[0].second;
  EXPECT_EQ(req.length, 3 * MiB);
  EXPECT_TRUE(svc::on_chunk(offer_id_hex(make_id()), req.request_id, req.file_index, req.offset, std::string(3 * MiB, 'x'), "1.2.3.4:5"));
  const auto d = data();
  ASSERT_EQ(d.size(), 3u);
  std::size_t total = 0;
  for (std::size_t i = 0; i < d.size(); ++i) {
    EXPECT_EQ(d[i].read_id, 7u);
    EXPECT_LE(d[i].data.size(), ag::max_frame_payload);
    EXPECT_EQ(d[i].last, i + 1 == d.size());
    total += d[i].data.size();
  }
  EXPECT_EQ(total, 3 * MiB);
  EXPECT_TRUE(errors().empty());
}

TEST_F(fixture, SessionEndFailsOfferReads) {
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(3 * MiB)), svc::offer_result::ok);
  read_range(3, make_id(), 0, 0, MiB);
  ASSERT_EQ(posted.size(), 1u);
  const auto req = posted[0].second;
  svc::session_ended(owner_session);
  const auto e = errors();
  ASSERT_EQ(e.size(), 1u);
  EXPECT_EQ(e[0].read_id, 3u);
  EXPECT_EQ(e[0].error, read_error::gone);
  EXPECT_FALSE(svc::on_chunk(offer_id_hex(make_id()), req.request_id, 0, 0, std::string(MiB, 'x')));
}

TEST_F(fixture, OtherSessionEndDoesNothing) {
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(MiB)), svc::offer_result::ok);
  read_range(3, make_id(), 0, 0, 100);
  svc::session_ended(owner_session + 1);
  EXPECT_TRUE(errors().empty());
  const auto req = posted.at(0).second;
  EXPECT_TRUE(svc::on_chunk(offer_id_hex(make_id()), req.request_id, 0, 0, std::string(100, 'x')));
}

TEST_F(fixture, StaleOfferReadIsGone) {
  read_range(1, make_id(), 0, 0, 10);  // no offer at all
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(MiB, 0)), svc::offer_result::ok);
  frames.clear();
  read_range(2, make_id(9), 0, 0, 10);  // other offer
  EXPECT_TRUE(posted.empty());
  const auto e = errors();
  ASSERT_EQ(e.size(), 1u);
  EXPECT_EQ(e[0].read_id, 2u);
  EXPECT_EQ(e[0].error, read_error::gone);
}

TEST_F(fixture, AgentDisconnectClearsOfferAndOwner) {
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(MiB)), svc::offer_result::ok);
  read_range(1, make_id(), 0, 0, 100);
  const auto req = posted.at(0).second;
  svc::handle_agent_connection(false);
  // Read ids restart at 1 in the next agent process: the stale offer must not be readable.
  read_range(1, make_id(), 0, 0, 100);
  EXPECT_EQ(posted.size(), 1u);
  EXPECT_FALSE(svc::on_chunk(offer_id_hex(make_id()), req.request_id, 0, 0, std::string(100, 'x')));
  const auto e = errors();
  ASSERT_FALSE(e.empty());
  EXPECT_EQ(e.back().error, read_error::gone);
  // The owner is forgotten: ending its session later is a no-op and a new offer works.
  svc::session_ended(owner_session);
  EXPECT_EQ(svc::install_offer(owner_session + 5, make_mlcf(MiB)), svc::offer_result::ok);
}

TEST_F(fixture, SendFailureMidDeliveryAbortsRead) {
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(3 * MiB)), svc::offer_result::ok);
  read_range(4, make_id(), 0, 0, 3 * MiB);
  const auto req = posted.at(0).second;
  frames.clear();
  frames_before_failure = 1;  // first data frame goes through, second is dropped
  EXPECT_TRUE(svc::on_chunk(offer_id_hex(make_id()), req.request_id, 0, 0, std::string(3 * MiB, 'x')));
  EXPECT_EQ(data().size(), 1u);
  // The error frame itself is dropped too (queue full), but a later request for the read is never delivered.
  frames_before_failure = -1;
  frames.clear();
  EXPECT_FALSE(svc::on_chunk(offer_id_hex(make_id()), req.request_id, 0, 0, std::string(10, 'x')));
  EXPECT_TRUE(data().empty());
}

TEST_F(fixture, SendFailureReportsIoWhenPossible) {
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(2 * MiB)), svc::offer_result::ok);
  read_range(4, make_id(), 0, 0, 2 * MiB);
  const auto req = posted.at(0).second;
  frames.clear();
  fail_call = call_count + 2;  // first data frame ok, second dropped, the range_error frame goes through
  EXPECT_TRUE(svc::on_chunk(offer_id_hex(make_id()), req.request_id, 0, 0, std::string(2 * MiB, 'x')));
  EXPECT_EQ(data().size(), 1u);
  const auto e = errors();
  ASSERT_EQ(e.size(), 1u);
  EXPECT_EQ(e[0].read_id, 4u);
  EXPECT_EQ(e[0].error, read_error::io);
}

TEST_F(fixture, PostRequestFailureFailsReadWithGone) {
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(MiB)), svc::offer_result::ok);
  post_ok = false;
  read_range(5, make_id(), 0, 0, 100);
  const auto e = errors();
  ASSERT_EQ(e.size(), 1u);
  EXPECT_EQ(e[0].read_id, 5u);
  EXPECT_EQ(e[0].error, read_error::gone);
}

TEST_F(fixture, ClipboardSetNotesOfferingClient) {
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(10), "client-uuid"), svc::offer_result::ok);
  svc::handle_agent_message({ag::msg::clipboard_set, ag::encode_clipboard_set(42).substr(1)});
  ASSERT_EQ(notes.size(), 1u);
  EXPECT_EQ(notes[0].first, 42u);
  EXPECT_EQ(notes[0].second, "client-uuid");
}

TEST_F(fixture, OfferDroppedClearsOffer) {
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(MiB)), svc::offer_result::ok);
  svc::handle_agent_message({ag::msg::offer_dropped, ag::encode_offer_dropped(make_id()).substr(1)});
  frames.clear();
  read_range(1, make_id(), 0, 0, 10);
  EXPECT_TRUE(posted.empty());
  EXPECT_EQ(errors().size(), 1u);
}

TEST_F(fixture, ThroughputLogCountsDistinctConnections) {
  svc::set_prefetch_bytes(0);
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(8 * MiB)), svc::offer_result::ok);
  read_range(1, make_id(), 0, 0, 8 * MiB);
  ASSERT_EQ(posted.size(), 2u);
  for (std::size_t i = 0; i < posted.size(); ++i) {
    const auto &r = posted[i].second;
    EXPECT_TRUE(svc::on_chunk(offer_id_hex(make_id()), r.request_id, 0, r.offset, std::string(r.length, 'x'), i == 0 ? "9.9.9.9:1" : "9.9.9.9:2"));
  }
  svc::session_ended(owner_session);
  ASSERT_EQ(logs.size(), 1u);
  EXPECT_NE(logs[0].find("2 chunks, 8388608 bytes over 2 connections"), std::string::npos) << logs[0];
}

TEST_F(fixture, SameConnectionKeyCountsOnce) {
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(8 * MiB)), svc::offer_result::ok);
  read_range(1, make_id(), 0, 0, 8 * MiB);
  for (const auto &p : posted) {
    EXPECT_TRUE(svc::on_chunk(offer_id_hex(make_id()), p.second.request_id, 0, p.second.offset, std::string(p.second.length, 'x'), "9.9.9.9:1"));
  }
  svc::session_ended(owner_session);
  ASSERT_EQ(logs.size(), 1u);
  EXPECT_NE(logs[0].find("over 1 connections"), std::string::npos) << logs[0];
}

TEST_F(fixture, ActiveTracksReads) {
  EXPECT_FALSE(svc::active());
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(3 * MiB)), svc::offer_result::ok);
  read_range(1, make_id(), 0, 0, 100);
  EXPECT_TRUE(svc::active());
  svc::on_chunk(offer_id_hex(make_id()), posted.at(0).second.request_id, 0, 0, std::string(100, 'x'));
  const auto later = [] {
    return std::chrono::steady_clock::now() + std::chrono::milliseconds(2100);  // past the 2 s linger
  };
  EXPECT_FALSE(svc::active(later()));  // completed
  read_range(2, make_id(), 0, 0, 100);
  EXPECT_TRUE(svc::active(later()));  // in flight: no linger needed
  svc::on_chunk_error(offer_id_hex(make_id()), posted.at(1).second.request_id, read_error::io);
  EXPECT_FALSE(svc::active(later()));  // failed
  read_range(3, make_id(), 0, 0, 100);
  EXPECT_TRUE(svc::active(later()));
  svc::handle_agent_message({ag::msg::cancel_read, ag::encode_cancel_read(3).substr(1)});
  EXPECT_FALSE(svc::active(later()));  // cancelled
  read_range(4, make_id(), 0, 0, 100);
  EXPECT_TRUE(svc::active(later()));
  svc::session_ended(owner_session);
  EXPECT_FALSE(svc::active(later()));  // offer cleared
}

TEST_F(fixture, ActiveLingersTwoSecondsAfterLastRead) {
  EXPECT_FALSE(svc::active());
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(3 * MiB)), svc::offer_result::ok);
  read_range(1, make_id(), 0, 0, 100);
  svc::on_chunk(offer_id_hex(make_id()), posted.at(0).second.request_id, 0, 0, std::string(100, 'x'));
  const auto done = std::chrono::steady_clock::now();
  EXPECT_TRUE(svc::active(done));  // right after the read finished: still fast
  EXPECT_TRUE(svc::active(done + std::chrono::milliseconds(1500)));
  EXPECT_FALSE(svc::active(done + std::chrono::milliseconds(2100)));
  // The next file's read re-arms it immediately.
  read_range(2, make_id(), 0, 0, 100);
  EXPECT_TRUE(svc::active(done + std::chrono::seconds(10)));
}

TEST(clipboard_files_query, StrictParsing) {
  const std::string id = offer_id_hex(make_id());
  EXPECT_TRUE(svc::parse_chunk_query(id, "1", "2", "3").has_value());
  EXPECT_TRUE(svc::parse_chunk_query(id, "4294967295", "0", "18446744073709551615").has_value());
  EXPECT_FALSE(svc::parse_chunk_query(id, "-1", "0", "0"));
  EXPECT_FALSE(svc::parse_chunk_query(id, " 1", "0", "0"));
  EXPECT_FALSE(svc::parse_chunk_query(id, "1x", "0", "0"));
  EXPECT_FALSE(svc::parse_chunk_query(id, "4294967296", "0", "0"));
  EXPECT_FALSE(svc::parse_chunk_query(id, "", "0", "0"));
  EXPECT_FALSE(svc::parse_chunk_query(id, "1", "+1", "0"));
  EXPECT_FALSE(svc::parse_chunk_query(id, "1", "0", "18446744073709551616"));
  EXPECT_FALSE(svc::parse_chunk_query(id.substr(1), "1", "0", "0"));
  EXPECT_FALSE(svc::parse_chunk_query(id + "0", "1", "0", "0"));
  auto bad = id;
  bad[5] = 'g';
  EXPECT_FALSE(svc::parse_chunk_query(bad, "1", "0", "0"));
}

TEST_F(fixture, InstallRechecksOwnerSessionAlive) {
  svc::hooks h;
  h.send_frame = [](const std::string &) { return true; };
  h.agent_connected = [] { return true; };
  h.post_request = [](std::uintptr_t, const chunk_request &) { return true; };
  h.session_alive = [](std::uintptr_t) { return false; };
  svc::set_test_hooks(std::move(h));
  EXPECT_EQ(svc::install_offer(owner_session, make_mlcf(MiB)), svc::offer_result::unsupported);
  read_range(1, make_id(), 0, 0, 10);
  EXPECT_FALSE(svc::active());
}

TEST_F(fixture, LateClipboardSetKeepsOriginAfterClear) {
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(10), "client-uuid"), svc::offer_result::ok);
  svc::session_ended(owner_session);
  svc::handle_agent_message({ag::msg::clipboard_set, ag::encode_clipboard_set(7).substr(1)});
  ASSERT_EQ(notes.size(), 1u);
  EXPECT_EQ(notes[0].second, "client-uuid");
}

TEST_F(fixture, ClipboardSetWithoutOriginIsSkipped) {
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(10)), svc::offer_result::ok);
  svc::handle_agent_message({ag::msg::clipboard_set, ag::encode_clipboard_set(7).substr(1)});
  EXPECT_TRUE(notes.empty());
}

namespace {
  std::string deep_mlcf(std::size_t levels, std::size_t component) {
    manifest m;
    m.offer_id = make_id();
    std::string path;
    for (std::size_t i = 0; i < levels; ++i) {
      if (i) {
        path += '/';
      }
      path += std::string(component, static_cast<char>('a' + i));
      m.entries.push_back({i + 1 == levels ? entry_kind::file : entry_kind::directory, i + 1 == levels ? 3u : 0u, 0, path});
    }
    return encode_manifest(m);
  }
}  // namespace

TEST_F(fixture, LongWindowsPathIsRejectedWithToken) {
  std::string why;
  // 4 x 100 + 3 separators = 403 UTF-16 units > 259
  EXPECT_EQ(svc::install_offer(owner_session, deep_mlcf(4, 100), "u", &why), svc::offer_result::bad_manifest);
  EXPECT_EQ(why, "windows_path_too_long");
  EXPECT_TRUE(frames.empty());
  read_range(1, make_id(), 0, 0, 3);  // nothing was installed
  ASSERT_EQ(errors().size(), 1u);
  EXPECT_EQ(errors()[0].error, read_error::gone);
  // exactly at the limit is fine: 259 = 2 x 129 + 1 separator
  why.clear();
  EXPECT_EQ(svc::install_offer(owner_session, deep_mlcf(2, 129), "u", &why), svc::offer_result::ok);
  EXPECT_TRUE(why.empty());
  // one over: 2 x 130 + 1 = 261 > 259 (also covers an install replacing a good offer: it must not clear it)
  EXPECT_EQ(svc::install_offer(owner_session, deep_mlcf(2, 130), "u", &why), svc::offer_result::bad_manifest);
  EXPECT_EQ(why, "windows_path_too_long");
}

TEST_F(fixture, OtherBadManifestsReportTheirErrorName) {
  std::string why;
  EXPECT_EQ(svc::install_offer(owner_session, "garbage", "u", &why), svc::offer_result::bad_manifest);
  EXPECT_EQ(why, error_name(decode_manifest("garbage").error));
  EXPECT_FALSE(why.empty());
  manifest empty;
  empty.offer_id = make_id();
  EXPECT_EQ(svc::install_offer(owner_session, encode_manifest(empty), "u", &why), svc::offer_result::bad_manifest);
  EXPECT_EQ(why, "empty");
}

TEST_F(fixture, UnencryptedControlStreamIsForbidden) {
  bool encrypted = false;
  svc::hooks h;
  h.send_frame = [](const std::string &) { return true; };
  h.agent_connected = [] { return true; };
  h.post_request = [](std::uintptr_t, const chunk_request &) { return true; };
  h.session_encrypted = [&](std::uintptr_t) { return encrypted; };
  svc::set_test_hooks(std::move(h));
  EXPECT_EQ(svc::install_offer(owner_session, make_mlcf(MiB), "u"), svc::offer_result::forbidden);
  EXPECT_FALSE(svc::chunk_forbidden("someone"));  // nothing installed
  encrypted = true;
  EXPECT_EQ(svc::install_offer(owner_session, make_mlcf(MiB), "u"), svc::offer_result::ok);
}

TEST_F(fixture, ChunkPostsAreBoundToTheOfferingClient) {
  EXPECT_FALSE(svc::chunk_forbidden("anyone"));  // no offer: the chunk is simply 410
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(MiB), "owner-uuid"), svc::offer_result::ok);
  EXPECT_FALSE(svc::chunk_forbidden("owner-uuid"));
  EXPECT_TRUE(svc::chunk_forbidden("other-uuid"));
  svc::session_ended(owner_session);
  EXPECT_FALSE(svc::chunk_forbidden("other-uuid"));
}

TEST_F(fixture, ReadFromSupersededOfferIsGoneAtomically) {
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(MiB, 0), "u"), svc::offer_result::ok);
  ASSERT_EQ(svc::install_offer(owner_session, make_mlcf(MiB, 9), "u"), svc::offer_result::ok);  // B replaces A
  frames.clear();
  read_range(1, make_id(0), 0, 0, 10);  // the old data object still asks with A's id
  EXPECT_TRUE(posted.empty());
  ASSERT_EQ(errors().size(), 1u);
  EXPECT_EQ(errors()[0].error, read_error::gone);
  EXPECT_FALSE(svc::active(std::chrono::steady_clock::now() + std::chrono::seconds(3)));
  // invalid range on the current offer maps to io
  read_range(2, make_id(9), 0, MiB, 10);
  ASSERT_EQ(errors().size(), 2u);
  EXPECT_EQ(errors()[1].error, read_error::io);
}
