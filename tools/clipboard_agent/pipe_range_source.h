/**
 * @file tools/clipboard_agent/pipe_range_source.h
 * @brief range_source that fetches bytes from sunshine.exe through read_range / range_data frames.
 */
#pragma once

#include "range_source.h"
#include "src/clipboard/files/agent_protocol.h"

#include <chrono>
#include <condition_variable>
#include <functional>
#include <map>
#include <mutex>

namespace clipboard_agent {
  /// Request router shared by all offers; hand each data object a source bound to *its* offer id with bind().
  class pipe_range_source final: public std::enable_shared_from_this<pipe_range_source> {
  public:
    /// `send` writes one complete frame to the pipe (thread-safe); returns false when the pipe is gone.
    using sender = std::function<bool(const std::string &frame)>;

    /// `read_timeout` is the longest a read waits without ANY range_data frame arriving (it restarts on every frame).
    explicit pipe_range_source(sender send, std::chrono::milliseconds read_timeout = std::chrono::seconds(60));

    /// A range_source whose every read_range frame carries `offer` (never a global "current offer").
    std::shared_ptr<range_source> bind(const clipboard::files::offer_id_t &offer);
    read_result read(const clipboard::files::offer_id_t &offer, std::uint32_t file_index, std::uint64_t offset, std::uint32_t length, std::string &out, const std::shared_ptr<cancel_token> &token);

    /// Called by the pipe receive thread.
    void on_range_data(const clipboard::files::agent::range_data_t &v);
    void on_range_error(const clipboard::files::agent::range_error_t &v);
    /// Fails every pending and future read (pipe broken).
    void shutdown();

  private:
    struct pending {
      std::string data;
      std::uint64_t expected {0};
      bool done {false};
      bool failed {false};
      clipboard::files::read_error error {clipboard::files::read_error::io};
    };

    sender send_;
    std::chrono::milliseconds timeout_;
    std::mutex m_;
    std::condition_variable cv_;
    std::map<std::uint32_t, pending> pending_;
    std::uint32_t next_id_ {0};
    bool dead_ {false};
    std::chrono::steady_clock::time_point last_progress_ {};  ///< last range_data frame received (any read)
  };
}  // namespace clipboard_agent
