/**
 * @file src/clipboard/files/transfer.cpp
 * @brief Portable range-transfer scheduler.
 */
#include "transfer.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace clipboard::files {
  transfer::transfer(callbacks cb, transfer_options opt):
      cb_(std::move(cb)),
      opt_(opt) {
    if (opt_.chunk_bytes == 0) {
      opt_.chunk_bytes = 4u << 20;
    }
    if (opt_.max_outstanding == 0) {
      opt_.max_outstanding = 1;
    }
  }

  void transfer::drain(std::unique_lock<std::mutex> &lock) {
    if (dispatching_) {
      return;  // the active dispatcher (possibly us, re-entered from a callback) will run it
    }
    dispatching_ = true;
    while (!events_.empty()) {
      auto ev = std::move(events_.front());
      events_.pop_front();
      lock.unlock();
      try {
        ev();
      } catch (...) {
        lock.lock();
        dispatching_ = false;
        throw;
      }
      lock.lock();
    }
    dispatching_ = false;
  }

  void transfer::issue_locked(std::uint32_t read_id, read_state &rs) {
    const auto length = static_cast<std::uint32_t>(std::min<std::uint64_t>(opt_.chunk_bytes, rs.end - rs.next_request));
    const auto id = next_request_id_++;
    requests_[id] = {read_id, rs.next_request, length, last_tick_, 0};
    chunk_request req {offer_->offer_id, id, rs.file_index, rs.next_request, length};
    rs.next_request += length;
    ++rs.outstanding;
    ++outstanding_;
    events_.push_back([this, req] {
      if (cb_.send_request) {
        cb_.send_request(req);
      }
    });
  }

  void transfer::pump_locked() {
    while (offer_ && outstanding_ < opt_.max_outstanding && !round_robin_.empty()) {
      const auto read_id = round_robin_.front();
      round_robin_.pop_front();
      auto it = reads_.find(read_id);
      if (it == reads_.end()) {
        continue;
      }
      issue_locked(read_id, it->second);
      if (it->second.next_request < it->second.end) {
        round_robin_.push_back(read_id);
      }
    }
  }

  void transfer::drop_read_locked(std::uint32_t read_id) {
    auto it = reads_.find(read_id);
    if (it == reads_.end()) {
      return;
    }
    for (auto r = requests_.begin(); r != requests_.end();) {
      if (r->second.read_id == read_id) {
        r = requests_.erase(r);
      } else {
        ++r;
      }
    }
    outstanding_ -= it->second.outstanding;  // in-flight + buffered slots
    round_robin_.erase(std::remove(round_robin_.begin(), round_robin_.end(), read_id), round_robin_.end());
    reads_.erase(it);
  }

  void transfer::fail_read_locked(std::uint32_t read_id, read_error err) {
    if (!reads_.count(read_id)) {
      return;
    }
    drop_read_locked(read_id);
    events_.push_back([this, read_id, err] {
      if (cb_.fail) {
        cb_.fail(read_id, err);
      }
    });
  }

  void transfer::fail_all_locked(read_error err) {
    std::vector<std::uint32_t> ids;
    ids.reserve(reads_.size());
    for (auto &[id, _] : reads_) {
      ids.push_back(id);
    }
    std::sort(ids.begin(), ids.end());
    for (auto id : ids) {
      fail_read_locked(id, err);
    }
  }

  void transfer::set_offer(const manifest &m) {
    std::unique_lock lock(mutex_);
    fail_all_locked(read_error::gone);
    offer_ = m;
    drain(lock);
  }

  void transfer::clear_offer() {
    std::unique_lock lock(mutex_);
    fail_all_locked(read_error::gone);
    offer_.reset();
    drain(lock);
  }

  bool transfer::start_read(std::uint32_t read_id, std::uint32_t file_index, std::uint64_t offset, std::uint64_t length) {
    std::unique_lock lock(mutex_);
    if (!offer_ || file_index >= offer_->entries.size() || reads_.count(read_id)) {
      return false;
    }
    const auto &e = offer_->entries[file_index];
    if (e.kind != entry_kind::file || offset > e.size || length > e.size - offset) {
      return false;
    }
    if (length == 0) {
      events_.push_back([this, read_id] {
        if (cb_.deliver) {
          cb_.deliver(read_id, std::string {}, true);
        }
      });
    } else {
      read_state rs;
      rs.file_index = file_index;
      rs.end = offset + length;
      rs.next_request = offset;
      rs.next_deliver = offset;
      reads_.emplace(read_id, std::move(rs));
      round_robin_.push_back(read_id);
      pump_locked();
    }
    drain(lock);
    return true;
  }

  void transfer::cancel_read(std::uint32_t read_id) {
    std::unique_lock lock(mutex_);
    drop_read_locked(read_id);
    pump_locked();
    drain(lock);
  }

  bool transfer::on_chunk(const offer_id_t &offer, std::uint32_t request_id, std::uint32_t file_index, std::uint64_t offset, std::string data) {
    std::unique_lock lock(mutex_);
    if (!offer_ || offer_->offer_id != offer) {
      return false;
    }
    auto rit = requests_.find(request_id);
    if (rit == requests_.end()) {
      return false;
    }
    const auto req = rit->second;
    auto it = reads_.find(req.read_id);
    if (it == reads_.end() || it->second.file_index != file_index || req.offset != offset) {
      return false;
    }
    auto &rs = it->second;
    // Requests never extend past the file size, so any length mismatch (short or long) is an io error.
    if (data.size() != req.length) {
      fail_read_locked(req.read_id, read_error::io);
      pump_locked();
      drain(lock);
      return true;
    }
    // The slot stays held (in-flight -> buffered) until the chunk is handed to deliver.
    requests_.erase(rit);
    rs.buffered.emplace(req.offset, std::move(data));
    while (true) {
      auto b = rs.buffered.find(rs.next_deliver);
      if (b == rs.buffered.end()) {
        break;
      }
      std::string piece = std::move(b->second);
      rs.buffered.erase(b);
      rs.next_deliver += piece.size();
      --rs.outstanding;
      --outstanding_;
      const bool last = rs.next_deliver >= rs.end;
      const auto read_id = req.read_id;
      events_.push_back([this, read_id, piece = std::move(piece), last]() mutable {
        if (cb_.deliver) {
          cb_.deliver(read_id, std::move(piece), last);
        }
      });
      if (last) {
        break;
      }
    }
    if (rs.next_deliver >= rs.end) {
      drop_read_locked(req.read_id);
    }
    pump_locked();
    drain(lock);
    return true;
  }

  bool transfer::on_chunk_error(const offer_id_t &offer, std::uint32_t request_id, read_error err) {
    std::unique_lock lock(mutex_);
    if (!offer_ || offer_->offer_id != offer) {
      return false;
    }
    auto rit = requests_.find(request_id);
    if (rit == requests_.end()) {
      return false;
    }
    fail_read_locked(rit->second.read_id, err);
    pump_locked();
    drain(lock);
    return true;
  }

  void transfer::tick(clock::time_point now) {
    std::unique_lock lock(mutex_);
    last_tick_ = now;
    std::vector<std::uint32_t> expired;
    for (auto &[id, r] : requests_) {
      if (!r.issued) {
        r.issued = now;
      } else if (now - *r.issued >= opt_.timeout) {
        expired.push_back(id);
      }
    }
    std::sort(expired.begin(), expired.end());
    for (auto id : expired) {
      auto it = requests_.find(id);
      if (it == requests_.end()) {
        continue;  // read already failed by an earlier expiry in this tick
      }
      auto r = it->second;
      if (r.attempts >= opt_.retries) {
        fail_read_locked(r.read_id, read_error::timeout);
        continue;
      }
      requests_.erase(it);
      const auto nid = next_request_id_++;
      requests_[nid] = {r.read_id, r.offset, r.length, now, r.attempts + 1};
      chunk_request req {offer_->offer_id, nid, reads_.at(r.read_id).file_index, r.offset, r.length};
      events_.push_back([this, req] {
        if (cb_.send_request) {
          cb_.send_request(req);
        }
      });
    }
    pump_locked();
    drain(lock);
  }

  std::optional<offer_id_t> transfer::current_offer() const {
    std::lock_guard lock(mutex_);
    if (!offer_) {
      return std::nullopt;
    }
    return offer_->offer_id;
  }
}  // namespace clipboard::files
