/**
 * @file tools/clipboard_agent/pipe_range_source.cpp
 * @brief range_source that fetches bytes from sunshine.exe through read_range / range_data frames.
 */
#include "pipe_range_source.h"

namespace clipboard_agent {
  namespace agent = clipboard::files::agent;
  using clipboard::files::read_error;

  pipe_range_source::pipe_range_source(sender send, std::chrono::milliseconds read_timeout):
      send_(std::move(send)),
      timeout_(read_timeout) {}

  namespace {
    class bound_source final: public range_source {
    public:
      bound_source(std::shared_ptr<pipe_range_source> owner, clipboard::files::offer_id_t offer):
          owner_(std::move(owner)),
          offer_(offer) {}

      read_result read(std::uint32_t file_index, std::uint64_t offset, std::uint32_t length, std::string &out) override {
        return owner_->read(offer_, file_index, offset, length, out, nullptr);
      }

      read_result read_cancellable(std::uint32_t file_index, std::uint64_t offset, std::uint32_t length, std::string &out, const std::shared_ptr<cancel_token> &token) override {
        return owner_->read(offer_, file_index, offset, length, out, token);
      }

    private:
      std::shared_ptr<pipe_range_source> owner_;
      clipboard::files::offer_id_t offer_;
    };
  }  // namespace

  std::shared_ptr<range_source> pipe_range_source::bind(const clipboard::files::offer_id_t &offer) {
    return std::make_shared<bound_source>(shared_from_this(), offer);
  }

  read_result pipe_range_source::read(const clipboard::files::offer_id_t &offer, std::uint32_t file_index, std::uint64_t offset, std::uint32_t length, std::string &out, const std::shared_ptr<cancel_token> &token) {
    out.clear();
    std::uint32_t id = 0;
    {
      std::lock_guard lock(m_);
      if (dead_) {
        return {false, read_error::io};
      }
      id = ++next_id_;
      pending p;
      p.expected = length;
      p.data.reserve(length);
      pending_.emplace(id, std::move(p));
    }
    std::uint64_t hook_id = 0;
    if (token) {
      hook_id = token->add_hook([this] {
        std::lock_guard lock(m_);
        cv_.notify_all();
      });
    }
    auto finish = [&](read_result rr) {
      if (token) {
        token->remove_hook(hook_id);
      }
      return rr;
    };

    if (!send_(agent::encode_read_range({id, offer, file_index, offset, length}))) {
      std::lock_guard lock(m_);
      pending_.erase(id);
      return finish({false, read_error::io});
    }

    std::unique_lock lock(m_);
    const auto deadline = std::chrono::steady_clock::now() + timeout_;
    for (;;) {
      auto it = pending_.find(id);
      if (it == pending_.end()) {  // shutdown() cleared it
        return finish({false, read_error::io});
      }
      if (it->second.done || it->second.failed) {
        const auto p = std::move(it->second);
        pending_.erase(it);
        if (p.failed) {
          return finish({false, p.error});
        }
        out = std::move(p.data);
        return finish({true, read_error::io});
      }
      const bool cancelled = token && token->cancelled();
      const bool timed_out = std::chrono::steady_clock::now() >= deadline;
      if (cancelled || timed_out || dead_) {
        const bool was_dead = dead_;
        pending_.erase(it);
        lock.unlock();
        if (!was_dead) {  // the pipe is gone: nothing to cancel, and the pipe object may be shutting down
          send_(agent::encode_cancel_read(id));
        }
        return finish({false, timed_out && !cancelled ? read_error::timeout : read_error::io});
      }
      cv_.wait_until(lock, deadline);
    }
  }

  void pipe_range_source::on_range_data(const agent::range_data_t &v) {
    std::lock_guard lock(m_);
    auto it = pending_.find(v.read_id);
    if (it == pending_.end()) {
      return;  // cancelled or unknown
    }
    auto &p = it->second;
    if (p.data.size() + v.data.size() > p.expected) {
      p.failed = true;
      p.error = read_error::io;
    } else {
      p.data += v.data;
      if (v.last) {
        if (p.data.size() == p.expected) {
          p.done = true;
        } else {
          p.failed = true;
          p.error = read_error::io;
        }
      }
    }
    cv_.notify_all();
  }

  void pipe_range_source::on_range_error(const agent::range_error_t &v) {
    std::lock_guard lock(m_);
    auto it = pending_.find(v.read_id);
    if (it == pending_.end()) {
      return;
    }
    it->second.failed = true;
    it->second.error = v.error;
    cv_.notify_all();
  }

  void pipe_range_source::shutdown() {
    std::lock_guard lock(m_);
    dead_ = true;
    cv_.notify_all();
  }
}  // namespace clipboard_agent
