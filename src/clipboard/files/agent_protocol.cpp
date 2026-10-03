/**
 * @file src/clipboard/files/agent_protocol.cpp
 * @brief Portable pipe-message codec between sunshine.exe and the user-session clipboard agent.
 */
#include "agent_protocol.h"

#include <algorithm>
#include <cstring>

namespace clipboard::files::agent {
  namespace {
    void put_u32(std::string &out, std::uint32_t v) {
      for (int i = 0; i < 4; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
      }
    }

    void put_u64(std::string &out, std::uint64_t v) {
      for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
      }
    }

    void put_id(std::string &out, const offer_id_t &id) {
      out.append(reinterpret_cast<const char *>(id.data()), id.size());
    }

    struct reader {
      std::string_view s;
      std::size_t pos {0};
      bool ok {true};

      std::uint64_t num(int bytes) {
        if (!ok || s.size() - pos < static_cast<std::size_t>(bytes)) {
          ok = false;
          return 0;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < bytes; ++i) {
          v |= static_cast<std::uint64_t>(static_cast<unsigned char>(s[pos + i])) << (8 * i);
        }
        pos += bytes;
        return v;
      }

      offer_id_t id() {
        offer_id_t r {};
        if (!ok || s.size() - pos < r.size()) {
          ok = false;
          return r;
        }
        std::memcpy(r.data(), s.data() + pos, r.size());
        pos += r.size();
        return r;
      }

      std::string_view rest() {
        auto r = s.substr(pos);
        pos = s.size();
        return r;
      }

      bool done() const {
        return ok && pos == s.size();
      }
    };

    std::string wrap(msg t, const std::string &payload) {
      return encode(t, payload);
    }

    std::string u32_msg(msg t, std::uint32_t v) {
      std::string p;
      put_u32(p, v);
      return wrap(t, p);
    }

    std::optional<std::uint32_t> u32_decode(std::string_view payload) {
      reader r {payload};
      auto v = static_cast<std::uint32_t>(r.num(4));
      if (!r.done()) {
        return std::nullopt;
      }
      return v;
    }

    std::string id_msg(msg t, const offer_id_t &id) {
      std::string p;
      put_id(p, id);
      return wrap(t, p);
    }

    std::optional<offer_id_t> id_decode(std::string_view payload) {
      reader r {payload};
      auto id = r.id();
      if (!r.done()) {
        return std::nullopt;
      }
      return id;
    }
  }  // namespace

  std::string encode(msg type, std::string_view payload) {
    std::string out;
    out.reserve(payload.size() + 1);
    out.push_back(static_cast<char>(type));
    out.append(payload);
    return out;
  }

  std::optional<message> decode(std::string_view frame) {
    if (frame.empty()) {
      return std::nullopt;
    }
    const auto t = static_cast<unsigned char>(frame[0]);
    if (t < static_cast<unsigned char>(msg::hello) || t > static_cast<unsigned char>(msg::pong)) {
      return std::nullopt;
    }
    return message {static_cast<msg>(t), std::string(frame.substr(1))};
  }

  std::string encode_hello(const hello_t &v) {
    std::string p;
    put_u32(p, v.version);
    put_u32(p, v.pid);
    return wrap(msg::hello, p);
  }

  std::optional<hello_t> decode_hello(std::string_view payload) {
    reader r {payload};
    hello_t v {};
    v.version = static_cast<std::uint32_t>(r.num(4));
    v.pid = static_cast<std::uint32_t>(r.num(4));
    if (!r.done()) {
      return std::nullopt;
    }
    return v;
  }

  std::string encode_set_offer_part(const set_offer_part_t &v) {
    std::string p;
    p.reserve(18 + v.data.size());
    put_id(p, v.id);
    p.push_back(v.prefetch ? 1 : 0);
    p.push_back(v.last ? 1 : 0);
    p.append(v.data);
    return wrap(msg::set_offer_part, p);
  }

  std::optional<set_offer_part_t> decode_set_offer_part(std::string_view payload) {
    reader r {payload};
    set_offer_part_t v {};
    v.id = r.id();
    const auto pf = r.num(1);
    const auto last = r.num(1);
    if (!r.ok || pf > 1 || last > 1) {
      return std::nullopt;
    }
    v.prefetch = pf != 0;
    v.last = last != 0;
    v.data = std::string(r.rest());
    return v;
  }

  std::string encode_clear_offer(const offer_id_t &id) {
    return id_msg(msg::clear_offer, id);
  }

  std::optional<offer_id_t> decode_clear_offer(std::string_view payload) {
    return id_decode(payload);
  }

  std::string encode_read_range(const read_range_t &v) {
    std::string p;
    put_u32(p, v.read_id);
    put_id(p, v.offer);
    put_u32(p, v.file_index);
    put_u64(p, v.offset);
    put_u64(p, v.length);
    return wrap(msg::read_range, p);
  }

  std::optional<read_range_t> decode_read_range(std::string_view payload) {
    reader r {payload};
    read_range_t v {};
    v.read_id = static_cast<std::uint32_t>(r.num(4));
    v.offer = r.id();
    v.file_index = static_cast<std::uint32_t>(r.num(4));
    v.offset = r.num(8);
    v.length = r.num(8);
    if (!r.done()) {
      return std::nullopt;
    }
    return v;
  }

  std::string encode_range_data(const range_data_t &v) {
    std::string p;
    p.reserve(5 + v.data.size());
    put_u32(p, v.read_id);
    p.push_back(v.last ? 1 : 0);
    p.append(v.data);
    return wrap(msg::range_data, p);
  }

  std::optional<range_data_t> decode_range_data(std::string_view payload) {
    reader r {payload};
    range_data_t v {};
    v.read_id = static_cast<std::uint32_t>(r.num(4));
    const auto last = r.num(1);
    if (!r.ok || last > 1) {
      return std::nullopt;
    }
    v.last = last != 0;
    v.data = std::string(r.rest());
    return v;
  }

  std::string encode_range_error(const range_error_t &v) {
    std::string p;
    put_u32(p, v.read_id);
    p.push_back(static_cast<char>(v.error));
    return wrap(msg::range_error, p);
  }

  std::optional<range_error_t> decode_range_error(std::string_view payload) {
    reader r {payload};
    range_error_t v {};
    v.read_id = static_cast<std::uint32_t>(r.num(4));
    const auto e = r.num(1);
    if (!r.done() || e < static_cast<std::uint64_t>(read_error::gone) || e > static_cast<std::uint64_t>(read_error::timeout)) {
      return std::nullopt;
    }
    v.error = static_cast<read_error>(e);
    return v;
  }

  std::string encode_cancel_read(std::uint32_t read_id) {
    return u32_msg(msg::cancel_read, read_id);
  }

  std::optional<std::uint32_t> decode_cancel_read(std::string_view payload) {
    return u32_decode(payload);
  }

  std::string encode_clipboard_set(std::uint32_t sequence_number) {
    return u32_msg(msg::clipboard_set, sequence_number);
  }

  std::optional<std::uint32_t> decode_clipboard_set(std::string_view payload) {
    return u32_decode(payload);
  }

  std::string encode_offer_dropped(const offer_id_t &id) {
    return id_msg(msg::offer_dropped, id);
  }

  std::optional<offer_id_t> decode_offer_dropped(std::string_view payload) {
    return id_decode(payload);
  }

  std::string encode_ping(std::uint32_t nonce) {
    return u32_msg(msg::ping, nonce);
  }

  std::optional<std::uint32_t> decode_ping(std::string_view payload) {
    return u32_decode(payload);
  }

  std::string encode_pong(std::uint32_t nonce) {
    return u32_msg(msg::pong, nonce);
  }

  std::optional<std::uint32_t> decode_pong(std::string_view payload) {
    return u32_decode(payload);
  }

  std::vector<std::string> split_offer(const offer_id_t &id, bool prefetch, std::string_view mlcf) {
    std::vector<std::string> frames;
    std::size_t pos = 0;
    do {
      const auto n = std::min(max_frame_payload, mlcf.size() - pos);
      const bool last = pos + n >= mlcf.size();
      frames.push_back(encode_set_offer_part({id, prefetch, last, std::string(mlcf.substr(pos, n))}));
      pos += n;
    } while (pos < mlcf.size());
    return frames;
  }

  std::optional<std::tuple<offer_id_t, bool, std::string>> offer_assembler::add(std::string_view set_offer_part_payload) {
    auto part = decode_set_offer_part(set_offer_part_payload);
    if (!part) {
      if (active_) {
        failure_ = id_;
        active_ = false;
        overflow_ = false;
        buf_.clear();
      }
      return std::nullopt;
    }
    if (!active_ || part->id != id_) {
      id_ = part->id;
      buf_.clear();
      active_ = true;
      overflow_ = false;
    }
    prefetch_ = part->prefetch;
    constexpr std::size_t max_assembled = (32u << 20) + (64u << 10);
    if (!overflow_ && buf_.size() + part->data.size() > max_assembled) {
      failure_ = id_;
      overflow_ = true;  // discard the rest of this oversized offer, never deliver a truncated one
      buf_.clear();
      buf_.shrink_to_fit();
    }
    if (overflow_) {
      if (part->last) {
        active_ = false;
        overflow_ = false;
      }
      return std::nullopt;
    }
    buf_.append(part->data);
    if (!part->last) {
      return std::nullopt;
    }
    active_ = false;
    std::tuple<offer_id_t, bool, std::string> out {id_, prefetch_, std::move(buf_)};
    buf_.clear();
    return out;
  }
}  // namespace clipboard::files::agent
