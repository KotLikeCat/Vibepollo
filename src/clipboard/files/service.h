/**
 * @file src/clipboard/files/service.h
 * @brief Host-side glue between the HTTPS endpoints, the 0x3005 control request and the user-session clipboard agent.
 *
 * The logic in service.cpp is portable; everything platform specific (agent pipe, control stream, ticker thread)
 * is reached through `hooks`, installed by start() (service_runtime.cpp) or by tests via set_test_hooks().
 */
#pragma once

#include "agent_protocol.h"
#include "transfer.h"
#include "types.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace clipboard::files::service {
  struct hooks {
    /// Non-blocking enqueue of a complete agent frame; false = dropped.
    std::function<bool(const std::string &frame)> send_frame;
    /// Queue a 0x3005 request on the control stream of `session_id`; false = the session is gone.
    std::function<bool(std::uintptr_t session_id, const chunk_request &)> post_request;
    /// Agent connected (hello received).
    std::function<bool()> agent_connected;
    /// The agent wrote the host clipboard (sequence number) on behalf of the offering client.
    std::function<void(std::uint32_t seq, const std::string &origin_uuid)> note_clipboard_set;
    /// Optional: is the control session still alive? (null = always)
    std::function<bool(std::uintptr_t session_id)> session_alive;
    /// Info-level log sink (throughput summary per offer).
    std::function<void(const std::string &line)> log;
  };

  enum class offer_result { ok, bad_manifest, unsupported };

  /// Creates the transfer, starts the ticker and the platform agent. Safe to call once; stop() undoes it.
  void start();
  void stop();

  /// Decode + validate `mlcf`, replace the current offer (agent first, then the scheduler).
  /// `origin_uuid` is the offering client (used for clipboard echo suppression).
  offer_result install_offer(std::uintptr_t session_id, std::string_view mlcf, std::string origin_uuid = {});
  /// false -> HTTP 410. `connection_key` identifies the TCP connection (remote address:port) for keep-alive accounting.
  bool on_chunk(std::string_view offer_hex, std::uint32_t req, std::uint32_t file, std::uint64_t offset, std::string body, std::string_view connection_key = {});
  bool on_chunk_error(std::string_view offer_hex, std::uint32_t req, read_error err);
  /// Lock-free: true while any agent read is in flight (the control loop then iterates fast so 0x3005 requests are not delayed).
  bool active();

  struct chunk_query {
    offer_id_t offer;
    std::uint32_t req;
    std::uint32_t file;
    std::uint64_t offset;
  };
  /// Strict parse of the file-chunk query values: full-string decimal, no sign/space/junk/overflow, offer = exactly 32 hex chars.
  std::optional<chunk_query> parse_chunk_query(std::string_view offer, std::string_view req, std::string_view file, std::string_view offset);

  /// Clears the offer when `session_id` owns it.
  void session_ended(std::uintptr_t session_id);

  // Used by the runtime glue and tests.
  void handle_agent_message(const agent::message &m);
  void handle_agent_connection(bool connected);
  void tick(std::chrono::steady_clock::time_point now);
  void set_prefetch_bytes(std::uint64_t bytes);

  /// Test seam: replaces hooks and resets all state (fresh transfer). Pass `{}` to reset.
  void set_test_hooks(hooks h, transfer_options opt = {});
}  // namespace clipboard::files::service
