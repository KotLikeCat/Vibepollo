/**
 * @file src/clipboard/sync_policy.h
 * @brief Decides which clients receive clipboard change notifications.
 */
#pragma once

#include "bundle.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace clipboard {
  struct client_info {
    std::string uuid;
    bool can_read;  ///< Client has PERM::clipboard_read
  };

  /// Thread-safe: used by the nvhttp thread (writes) and the watcher thread (reads).
  class sync_policy {
  public:
    /// Records that the host clipboard was written on behalf of a client.
    void note_host_write(std::uint32_t seq_after_write, std::string origin_uuid);
    /// Clients that must be notified about clipboard version `seq` (echo to the writer suppressed).
    std::vector<std::string> recipients(std::uint32_t seq, const std::vector<client_info> &clients) const;
    /// Serializes host clipboard writes with the watcher's sequence reads.
    std::unique_lock<std::mutex> lock_host_write();

  private:
    mutable std::mutex mutex_;
    std::mutex write_mutex_;
    std::optional<std::uint32_t> last_write_seq_;
    std::string last_write_origin_;
  };

  sync_policy &shared_policy();

  /// Remembers which sessions already received the current clipboard state.
  class greeting_tracker {
  public:
    /// Returns uuids present now but not on the previous call (in input order) and forgets the rest.
    std::vector<std::string> new_sessions(const std::vector<std::string> &current_uuids);

  private:
    std::set<std::string> known_;
  };

  /// Drops the PNG item when the encoded bundle exceeds max_bytes. False if still too large.
  bool fit_to_limit(std::vector<item> &items, std::size_t max_bytes);
}  // namespace clipboard
