/**
 * @file src/clipboard/sync_policy.cpp
 * @brief Decides which clients receive clipboard change notifications.
 */
#include "sync_policy.h"

#include <utility>

namespace clipboard {
  void sync_policy::note_host_write(std::uint32_t seq_after_write, std::string origin_uuid) {
    std::lock_guard lock {mutex_};
    last_write_seq_ = seq_after_write;
    last_write_origin_ = std::move(origin_uuid);
  }

  std::vector<std::string> sync_policy::recipients(std::uint32_t seq, const std::vector<client_info> &clients) const {
    std::lock_guard lock {mutex_};
    std::vector<std::string> out;
    for (const auto &client : clients) {
      if (!client.can_read) {
        continue;
      }
      if (last_write_seq_ && *last_write_seq_ == seq && client.uuid == last_write_origin_) {
        continue;
      }
      out.push_back(client.uuid);
    }
    return out;
  }

  std::unique_lock<std::mutex> sync_policy::lock_host_write() {
    return std::unique_lock<std::mutex> {write_mutex_};
  }

  sync_policy &shared_policy() {
    static sync_policy instance;
    return instance;
  }

  std::vector<std::string> greeting_tracker::new_sessions(const std::vector<std::string> &current_uuids) {
    std::vector<std::string> fresh;
    for (const auto &uuid : current_uuids) {
      if (!known_.contains(uuid)) {
        fresh.push_back(uuid);
      }
    }
    known_ = std::set<std::string>(current_uuids.begin(), current_uuids.end());
    return fresh;
  }

  bool fit_to_limit(std::vector<item> &items, std::size_t max_bytes) {
    if (encoded_size(items) <= max_bytes) {
      return true;
    }
    std::erase_if(items, [](const item &entry) {
      return entry.type == item_type::png;
    });
    return encoded_size(items) <= max_bytes;
  }
}  // namespace clipboard
