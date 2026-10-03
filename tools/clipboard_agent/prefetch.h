/**
 * @file tools/clipboard_agent/prefetch.h
 * @brief Background download of a whole offer into a temp folder (backs CF_HDROP for small offers).
 */
#pragma once

#include "range_source.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace clipboard_agent {
  /// Adds the `\\?\` prefix so paths longer than MAX_PATH work.
  std::filesystem::path extended_path(const std::filesystem::path &p);
  /// Removes every child of `root` (stale prefetch folders of a previous agent run).
  void remove_stale_prefetch(const std::filesystem::path &root);

  class prefetcher {
  public:
    enum class state {
      running,
      done,
      failed
    };

    /// Starts the background thread immediately. The tree is created under `root / <offer_id_hex>-<instance>` (unique
    /// per prefetcher).
    prefetcher(const offer &o, std::shared_ptr<range_source> src, std::filesystem::path root);
    /// Cancels, joins and deletes the folder.
    ~prefetcher();
    prefetcher(const prefetcher &) = delete;
    prefetcher &operator=(const prefetcher &) = delete;

    /// Waits up to `timeout` for completion; returns the current state.
    state wait(std::chrono::milliseconds timeout);
    state current() const;
    /// Disk path (extended, `\\?\`) of entry `index`.
    std::filesystem::path path_for(std::size_t index) const;
    /// Plain (non-extended) paths of the top-level entries, in entry order.
    std::vector<std::filesystem::path> top_level_paths() const;

  private:
    void run();
    void worker();
    bool claim(std::size_t &index, std::uint64_t &offset, std::uint32_t &len);

    static constexpr std::size_t k_prefetch_parallel = 4;
    std::mutex cursor_m_;
    std::size_t cursor_file_ {0};
    std::uint64_t cursor_off_ {0};
    bool cursor_started_ {false};
    std::atomic<bool> failed_ {false};

    std::vector<clipboard::files::entry> entries_;
    std::vector<std::string> windows_paths_;
    std::shared_ptr<range_source> src_;
    std::filesystem::path dir_;
    std::shared_ptr<cancel_token> token_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    state state_ {state::running};
    std::thread thread_;
  };
}  // namespace clipboard_agent
