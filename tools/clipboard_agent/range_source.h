/**
 * @file tools/clipboard_agent/range_source.h
 * @brief Blocking source of file bytes (backed by the pipe to sunshine.exe in production, fake in tests).
 */
#pragma once

#include "src/clipboard/files/manifest.h"
#include "src/clipboard/files/types.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace clipboard_agent {
  struct offer {
    clipboard::files::offer_id_t id;
    std::vector<clipboard::files::entry> entries;
    std::vector<std::string> windows_paths;  ///< index-aligned sanitised relative paths, UTF-8, backslash separated
    bool prefetch;
  };

  struct read_result {
    bool ok;
    clipboard::files::read_error error;
  };

  /// Lets the owner of a blocked read abort it (stream released mid-read, prefetcher destroyed).
  class cancel_token {
  public:
    bool cancelled() const {
      return flag_.load();
    }

    void cancel() {
      flag_.store(true);
      std::function<void()> hook;
      {
        std::lock_guard lock(m_);
        hook = hook_;
      }
      if (hook) {
        hook();
      }
    }

    /// Source-side: called by cancel() to wake a waiting read. Cleared with an empty function.
    void set_hook(std::function<void()> hook) {
      std::lock_guard lock(m_);
      hook_ = std::move(hook);
    }

  private:
    std::atomic<bool> flag_ {false};
    std::mutex m_;
    std::function<void()> hook_;
  };

  class range_source {  ///< blocking; called from worker / RPC threads
  public:
    virtual ~range_source() = default;
    virtual read_result read(std::uint32_t file_index, std::uint64_t offset, std::uint32_t length, std::string &out) = 0;

    /// Like read(), but a source that can may return early (ok=false, io) once `token` is cancelled.
    virtual read_result read_cancellable(std::uint32_t file_index, std::uint64_t offset, std::uint32_t length, std::string &out, const std::shared_ptr<cancel_token> &token) {
      (void) token;
      return read(file_index, offset, length, out);
    }
  };
}  // namespace clipboard_agent
