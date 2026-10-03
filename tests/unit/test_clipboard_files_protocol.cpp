/**
 * @file tests/unit/test_clipboard_files_protocol.cpp
 * @brief Test src/clipboard/files/agent_protocol.*
 */
#include "../tests_common.h"
#include "src/clipboard/files/agent_protocol.h"

#include <string>

using namespace clipboard::files;
namespace ag = clipboard::files::agent;

namespace {
  offer_id_t make_id(std::uint8_t base = 0) {
    offer_id_t id {};
    for (std::size_t i = 0; i < id.size(); ++i) {
      id[i] = static_cast<std::uint8_t>(base + i);
    }
    return id;
  }

  std::string payload_of(const std::string &frame, ag::msg expected) {
    auto m = ag::decode(frame);
    EXPECT_TRUE(m.has_value());
    if (!m) {
      return {};
    }
    EXPECT_EQ(m->type, expected);
    return m->payload;
  }
}  // namespace

TEST(ClipboardFilesProtocol, GenericRoundTrip) {
  const auto frame = ag::encode(ag::msg::ping, "abcd");
  ASSERT_EQ(frame.size(), 5u);
  EXPECT_EQ(static_cast<unsigned char>(frame[0]), 10);
  auto m = ag::decode(frame);
  ASSERT_TRUE(m);
  EXPECT_EQ(m->type, ag::msg::ping);
  EXPECT_EQ(m->payload, "abcd");
}

TEST(ClipboardFilesProtocol, DecodeRejectsBadFrames) {
  EXPECT_FALSE(ag::decode(""));
  EXPECT_FALSE(ag::decode(std::string(1, '\0')));
  EXPECT_FALSE(ag::decode(std::string(1, '\x0c')));
  EXPECT_FALSE(ag::decode(std::string(1, '\xff')));
  EXPECT_TRUE(ag::decode(std::string(1, '\x01')));
}

TEST(ClipboardFilesProtocol, Hello) {
  auto p = payload_of(ag::encode_hello({1, 0xdeadbeef}), ag::msg::hello);
  auto v = ag::decode_hello(p);
  ASSERT_TRUE(v);
  EXPECT_EQ(v->version, 1u);
  EXPECT_EQ(v->pid, 0xdeadbeefu);
  EXPECT_FALSE(ag::decode_hello(p.substr(0, 7)));
  EXPECT_FALSE(ag::decode_hello(p + "x"));
}

TEST(ClipboardFilesProtocol, SetOfferPart) {
  auto p = payload_of(ag::encode_set_offer_part({make_id(), true, false, "hello"}), ag::msg::set_offer_part);
  auto v = ag::decode_set_offer_part(p);
  ASSERT_TRUE(v);
  EXPECT_EQ(v->id, make_id());
  EXPECT_TRUE(v->prefetch);
  EXPECT_FALSE(v->last);
  EXPECT_EQ(v->data, "hello");
  EXPECT_FALSE(ag::decode_set_offer_part(p.substr(0, 17)));
}

TEST(ClipboardFilesProtocol, ClearAndDropped) {
  auto p = payload_of(ag::encode_clear_offer(make_id(3)), ag::msg::clear_offer);
  auto v = ag::decode_clear_offer(p);
  ASSERT_TRUE(v);
  EXPECT_EQ(*v, make_id(3));
  EXPECT_FALSE(ag::decode_clear_offer(p.substr(0, 15)));

  auto p2 = payload_of(ag::encode_offer_dropped(make_id(9)), ag::msg::offer_dropped);
  auto v2 = ag::decode_offer_dropped(p2);
  ASSERT_TRUE(v2);
  EXPECT_EQ(*v2, make_id(9));
}

TEST(ClipboardFilesProtocol, ReadRange) {
  auto p = payload_of(ag::encode_read_range({7, 2, 5000000000ull, 4u << 20}), ag::msg::read_range);
  EXPECT_EQ(p.size(), 24u);
  auto v = ag::decode_read_range(p);
  ASSERT_TRUE(v);
  EXPECT_EQ(v->read_id, 7u);
  EXPECT_EQ(v->file_index, 2u);
  EXPECT_EQ(v->offset, 5000000000ull);
  EXPECT_EQ(v->length, 4u << 20);
  EXPECT_FALSE(ag::decode_read_range(p.substr(1)));
}

TEST(ClipboardFilesProtocol, RangeDataAndError) {
  auto p = payload_of(ag::encode_range_data({42, true, std::string("\0\1\2", 3)}), ag::msg::range_data);
  auto v = ag::decode_range_data(p);
  ASSERT_TRUE(v);
  EXPECT_EQ(v->read_id, 42u);
  EXPECT_TRUE(v->last);
  EXPECT_EQ(v->data, std::string("\0\1\2", 3));

  auto pe = payload_of(ag::encode_range_error({5, read_error::changed}), ag::msg::range_error);
  auto ve = ag::decode_range_error(pe);
  ASSERT_TRUE(ve);
  EXPECT_EQ(ve->read_id, 5u);
  EXPECT_EQ(ve->error, read_error::changed);
  pe.back() = '\x63';
  EXPECT_FALSE(ag::decode_range_error(pe));
}

TEST(ClipboardFilesProtocol, U32Messages) {
  auto c = ag::decode_cancel_read(payload_of(ag::encode_cancel_read(11), ag::msg::cancel_read));
  ASSERT_TRUE(c);
  EXPECT_EQ(*c, 11u);
  auto s = ag::decode_clipboard_set(payload_of(ag::encode_clipboard_set(0x01020304), ag::msg::clipboard_set));
  ASSERT_TRUE(s);
  EXPECT_EQ(*s, 0x01020304u);
  auto pi = ag::decode_ping(payload_of(ag::encode_ping(99), ag::msg::ping));
  ASSERT_TRUE(pi);
  EXPECT_EQ(*pi, 99u);
  auto po = ag::decode_pong(payload_of(ag::encode_pong(100), ag::msg::pong));
  ASSERT_TRUE(po);
  EXPECT_EQ(*po, 100u);
  EXPECT_FALSE(ag::decode_ping("abc"));
}

TEST(ClipboardFilesProtocol, LittleEndian) {
  auto p = payload_of(ag::encode_clipboard_set(0x01020304), ag::msg::clipboard_set);
  EXPECT_EQ(p, std::string("\x04\x03\x02\x01", 4));
}

TEST(ClipboardFilesProtocol, SplitAndAssemble3MiB) {
  std::string mlcf(3u << 20, 'x');
  for (std::size_t i = 0; i < mlcf.size(); i += 4099) {
    mlcf[i] = static_cast<char>(i);
  }
  auto frames = ag::split_offer(make_id(), true, mlcf);
  ASSERT_EQ(frames.size(), 3u);
  ag::offer_assembler asmb;
  for (std::size_t i = 0; i < frames.size(); ++i) {
    auto m = ag::decode(frames[i]);
    ASSERT_TRUE(m);
    EXPECT_EQ(m->type, ag::msg::set_offer_part);
    auto part = ag::decode_set_offer_part(m->payload);
    ASSERT_TRUE(part);
    EXPECT_LE(part->data.size(), ag::max_frame_payload);
    EXPECT_EQ(part->last, i == frames.size() - 1);
    auto r = asmb.add(m->payload);
    if (i + 1 < frames.size()) {
      EXPECT_FALSE(r);
    } else {
      ASSERT_TRUE(r);
      EXPECT_EQ(std::get<0>(*r), make_id());
      EXPECT_TRUE(std::get<1>(*r));
      EXPECT_TRUE(std::get<2>(*r) == mlcf);
    }
  }
}

TEST(ClipboardFilesProtocol, SplitSmallAndEmpty) {
  auto one = ag::split_offer(make_id(), false, "abc");
  ASSERT_EQ(one.size(), 1u);
  ag::offer_assembler a;
  auto r = a.add(ag::decode(one[0])->payload);
  ASSERT_TRUE(r);
  EXPECT_FALSE(std::get<1>(*r));
  EXPECT_EQ(std::get<2>(*r), "abc");

  auto empty = ag::split_offer(make_id(), false, "");
  ASSERT_EQ(empty.size(), 1u);
  EXPECT_TRUE(ag::decode_set_offer_part(ag::decode(empty[0])->payload)->last);
}

TEST(ClipboardFilesProtocol, AssemblerRestartsOnNewOffer) {
  std::string big(2u << 20, 'a');
  auto first = ag::split_offer(make_id(1), false, big);
  ASSERT_EQ(first.size(), 2u);
  ag::offer_assembler a;
  EXPECT_FALSE(a.add(ag::decode(first[0])->payload));
  auto second = ag::split_offer(make_id(2), true, "new");
  auto r = a.add(ag::decode(second[0])->payload);
  ASSERT_TRUE(r);
  EXPECT_EQ(std::get<0>(*r), make_id(2));
  EXPECT_EQ(std::get<2>(*r), "new");
}

TEST(ClipboardFilesProtocol, AssemblerRejectsMalformed) {
  ag::offer_assembler a;
  EXPECT_FALSE(a.add("short"));
}
