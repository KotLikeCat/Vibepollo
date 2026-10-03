/**
 * @file src/clipboard/files/agent_protocol.h
 * @brief Portable pipe-message codec between sunshine.exe and the user-session clipboard agent.
 */
#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

namespace clipboard::files::agent {
  enum class msg : std::uint8_t {
    hello = 1,
    set_offer_part = 2,
    clear_offer = 3,
    read_range = 4,
    range_data = 5,
    range_error = 6,
    cancel_read = 7,
    clipboard_set = 8,
    offer_dropped = 9,
    ping = 10,
    pong = 11,
  };

  constexpr std::uint32_t protocol_version = 1;
  constexpr std::size_t max_frame_payload = 1u << 20;  ///< data frames carry at most 1 MiB

  struct message {
    msg type;
    std::string payload;
  };

  /// u8 type + payload.
  std::string encode(msg type, std::string_view payload);
  /// Rejects empty frames and unknown types.
  std::optional<message> decode(std::string_view frame);

  struct hello_t {
    std::uint32_t version;
    std::uint32_t pid;
  };
  struct read_range_t {
    std::uint32_t read_id;
    std::uint32_t file_index;
    std::uint64_t offset;
    std::uint64_t length;
  };
  struct range_data_t {
    std::uint32_t read_id;
    bool last;
    std::string data;
  };
  struct range_error_t {
    std::uint32_t read_id;
    read_error error;
  };
  struct set_offer_part_t {
    offer_id_t id;
    bool prefetch;
    bool last;
    std::string data;
  };

  // Each encode_* returns a complete frame (type byte included); decode_* take the message payload.
  std::string encode_hello(const hello_t &v);
  std::optional<hello_t> decode_hello(std::string_view payload);
  std::string encode_set_offer_part(const set_offer_part_t &v);
  std::optional<set_offer_part_t> decode_set_offer_part(std::string_view payload);
  std::string encode_clear_offer(const offer_id_t &id);
  std::optional<offer_id_t> decode_clear_offer(std::string_view payload);
  std::string encode_read_range(const read_range_t &v);
  std::optional<read_range_t> decode_read_range(std::string_view payload);
  std::string encode_range_data(const range_data_t &v);
  std::optional<range_data_t> decode_range_data(std::string_view payload);
  std::string encode_range_error(const range_error_t &v);
  std::optional<range_error_t> decode_range_error(std::string_view payload);
  std::string encode_cancel_read(std::uint32_t read_id);
  std::optional<std::uint32_t> decode_cancel_read(std::string_view payload);
  std::string encode_clipboard_set(std::uint32_t sequence_number);
  std::optional<std::uint32_t> decode_clipboard_set(std::string_view payload);
  std::string encode_offer_dropped(const offer_id_t &id);
  std::optional<offer_id_t> decode_offer_dropped(std::string_view payload);
  std::string encode_ping(std::uint32_t nonce);
  std::optional<std::uint32_t> decode_ping(std::string_view payload);
  std::string encode_pong(std::uint32_t nonce);
  std::optional<std::uint32_t> decode_pong(std::string_view payload);

  /// Splits an MLCF encoding into set_offer_part frames (each slice <= max_frame_payload), `last` only on the final one.
  std::vector<std::string> split_offer(const offer_id_t &id, bool prefetch, std::string_view mlcf);

  /// Reassembles set_offer_part payloads; a part with a different offer id restarts assembly.
  class offer_assembler {
  public:
    /// Returns (offer id, prefetch, mlcf) once the `last` part arrives; nullopt otherwise or on malformed input.
    std::optional<std::tuple<offer_id_t, bool, std::string>> add(std::string_view set_offer_part_payload);

  private:
    bool active_ {false};
    bool overflow_ {false};
    offer_id_t id_ {};
    bool prefetch_ {false};
    std::string buf_;
  };
}  // namespace clipboard::files::agent
