/**
 * @file src/clipboard/files/transfer.h
 * @brief Portable range-transfer scheduler: turns file reads into pipelined chunk requests to the client.
 *
 * No internal threads; time only advances through tick(). Callbacks are invoked without the internal
 * mutex held and, across threads, in the order the events were produced.
 */
#pragma once

#include "manifest.h"
#include "types.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace clipboard::files {
  struct chunk_request {
    offer_id_t offer_id;
    std::uint32_t request_id;
    std::uint32_t file_index;
    std::uint64_t offset;
    std::uint32_t length;
  };

  struct transfer_options {
    std::uint32_t chunk_bytes = 4u << 20;
    std::size_t max_outstanding = 4;
    std::chrono::milliseconds timeout {15000};
    int retries = 1;
  };

  class transfer {
  public:
    struct callbacks {
      std::function<void(const chunk_request &)> send_request;  ///< to the client (control 0x3005)
      std::function<void(std::uint32_t read_id, std::string data, bool last)> deliver;  ///< to the agent, in offset order
      std::function<void(std::uint32_t read_id, read_error)> fail;
    };

    transfer(callbacks cb, transfer_options opt = {});

    /// Replaces the offer; pending reads of the old offer fail with gone.
    void set_offer(const manifest &m);
    /// Drops the offer; pending reads fail with gone.
    void clear_offer();
    /// False if no offer / bad index / duplicate read id / range beyond size.
    bool start_read(std::uint32_t read_id, std::uint32_t file_index, std::uint64_t offset, std::uint64_t length);
    void cancel_read(std::uint32_t read_id);
    /// False means the chunk is unknown or late (HTTP 410).
    bool on_chunk(const offer_id_t &offer, std::uint32_t request_id, std::uint32_t file_index, std::uint64_t offset, std::string data);
    bool on_chunk_error(const offer_id_t &offer, std::uint32_t request_id, read_error err);
    /// Timeouts and retries. Requests issued before the first tick are timed from the first tick that sees them.
    void tick(std::chrono::steady_clock::time_point now);
    std::optional<offer_id_t> current_offer() const;

  private:
    using clock = std::chrono::steady_clock;

    struct read_state {
      std::uint32_t file_index = 0;
      std::uint64_t end = 0;  ///< absolute end offset of the range
      std::uint64_t next_request = 0;  ///< next offset to request
      std::uint64_t next_deliver = 0;  ///< next offset to hand to the agent
      std::map<std::uint64_t, std::string> buffered;  ///< out-of-order arrivals by offset
      std::size_t outstanding = 0;
    };

    struct request_state {
      std::uint32_t read_id;
      std::uint64_t offset;
      std::uint32_t length;
      std::optional<clock::time_point> issued;
      int attempts;
    };

    void issue_locked(std::uint32_t read_id, read_state &rs);
    void pump_locked();
    void drop_read_locked(std::uint32_t read_id);
    void fail_read_locked(std::uint32_t read_id, read_error err);
    void fail_all_locked(read_error err);
    void drain(std::unique_lock<std::mutex> &lock);

    callbacks cb_;
    transfer_options opt_;
    mutable std::mutex mutex_;
    std::optional<manifest> offer_;
    std::unordered_map<std::uint32_t, read_state> reads_;
    std::deque<std::uint32_t> round_robin_;  ///< reads that still have chunks to request
    std::unordered_map<std::uint32_t, request_state> requests_;
    std::uint32_t next_request_id_ = 1;
    std::size_t outstanding_ = 0;
    std::optional<clock::time_point> last_tick_;
    std::deque<std::function<void()>> events_;
    bool dispatching_ = false;
  };
}  // namespace clipboard::files
