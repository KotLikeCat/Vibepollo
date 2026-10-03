/**
 * @file tools/clipboard_agent/prefetch.cpp
 * @brief Background download of a whole offer into a temp folder (backs CF_HDROP for small offers).
 */
#include "prefetch.h"

#include "file_stream.h"
#include "src/platform/windows/utf_utils.h"

#include <algorithm>
#include <fstream>
#include <windows.h>

namespace clipboard_agent {
  namespace fs = std::filesystem;
  using clipboard::files::entry_kind;

  namespace {
    std::atomic<std::uint64_t> g_instance {0};  ///< folder name suffix: an older object of the same offer never deletes a newer one's folder
  }

  fs::path extended_path(const fs::path &p) {
    std::wstring s = fs::absolute(p).make_preferred().wstring();
    if (s.rfind(L"\\\\?\\", 0) == 0) {
      return fs::path(s);
    }
    if (s.rfind(L"\\\\", 0) == 0) {
      return fs::path(L"\\\\?\\UNC\\" + s.substr(2));
    }
    return fs::path(L"\\\\?\\" + s);
  }

  void remove_stale_prefetch(const fs::path &root) {
    std::error_code ec;
    if (!fs::is_directory(root, ec)) {
      return;
    }
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
      std::error_code rec;
      fs::remove_all(extended_path(it->path()), rec);
    }
  }

  prefetcher::prefetcher(const offer &o, std::shared_ptr<range_source> src, fs::path root):
      entries_(o.entries),
      windows_paths_(o.windows_paths),
      src_(std::move(src)),
      dir_(root / (clipboard::files::offer_id_hex(o.id) + "-" + std::to_string(++g_instance))),
      token_(std::make_shared<cancel_token>()) {
    thread_ = std::thread([this] {
      run();
    });
  }

  prefetcher::~prefetcher() {
    token_->cancel();
    if (thread_.joinable()) {
      thread_.join();
    }
    std::error_code ec;
    fs::remove_all(extended_path(dir_), ec);
  }

  prefetcher::state prefetcher::wait(std::chrono::milliseconds timeout) {
    std::unique_lock lock(m_);
    cv_.wait_for(lock, timeout, [this] {
      return state_ != state::running;
    });
    return state_;
  }

  prefetcher::state prefetcher::current() const {
    std::lock_guard lock(m_);
    return state_;
  }

  fs::path prefetcher::path_for(std::size_t index) const {
    return extended_path(dir_ / fs::path(utf_utils::from_utf8(windows_paths_.at(index))));
  }

  std::vector<fs::path> prefetcher::top_level_paths() const {
    std::vector<fs::path> out;
    for (std::size_t i = 0; i < windows_paths_.size(); ++i) {
      if (windows_paths_[i].find('\\') == std::string::npos) {
        out.push_back(fs::absolute(dir_ / fs::path(utf_utils::from_utf8(windows_paths_[i]))).make_preferred());
      }
    }
    return out;
  }

  namespace {
    bool write_at(const fs::path &file, std::uint64_t offset, const std::string &data) {
      HANDLE h = CreateFileW(file.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
      if (h == INVALID_HANDLE_VALUE) {
        return false;
      }
      LARGE_INTEGER pos;
      pos.QuadPart = static_cast<LONGLONG>(offset);
      bool ok = SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) != 0;
      std::size_t done = 0;
      while (ok && done < data.size()) {
        DWORD wrote = 0;
        ok = WriteFile(h, data.data() + done, static_cast<DWORD>(std::min<std::size_t>(data.size() - done, 1u << 24)), &wrote, nullptr) && wrote > 0;
        done += wrote;
      }
      CloseHandle(h);
      return ok;
    }
  }  // namespace

  /// Hands out (file, offset, len) pieces in order and creates directories / empty files as it passes them.
  bool prefetcher::claim(std::size_t &index, std::uint64_t &offset, std::uint32_t &len) {
    std::lock_guard lock(cursor_m_);
    std::error_code ec;
    while (cursor_file_ < entries_.size() && !failed_.load()) {
      const auto &e = entries_[cursor_file_];
      if (!cursor_started_) {
        const fs::path target = path_for(cursor_file_);
        if (e.kind == entry_kind::directory) {
          fs::create_directories(target, ec);
          if (ec) {
            failed_ = true;
            break;
          }
          ++cursor_file_;
          continue;
        }
        fs::create_directories(target.parent_path(), ec);
        HANDLE h = ec ? INVALID_HANDLE_VALUE : CreateFileW(target.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) {
          failed_ = true;
          break;
        }
        CloseHandle(h);
        cursor_started_ = true;
        cursor_off_ = 0;
      }
      if (cursor_off_ >= e.size) {
        ++cursor_file_;
        cursor_started_ = false;
        continue;
      }
      index = cursor_file_;
      offset = cursor_off_;
      len = static_cast<std::uint32_t>(std::min<std::uint64_t>(k_piece_bytes, e.size - cursor_off_));
      cursor_off_ += len;
      return true;
    }
    return false;
  }

  void prefetcher::worker() {
    try {
      std::size_t index = 0;
      std::uint64_t offset = 0;
      std::uint32_t len = 0;
      std::string piece;
      while (!failed_.load() && claim(index, offset, len)) {
        if (token_->cancelled()) {
          failed_ = true;
          break;
        }
        piece.clear();
        const auto rr = src_->read_cancellable(static_cast<std::uint32_t>(index), offset, len, piece, token_);
        if (!rr.ok || piece.size() != len || !write_at(path_for(index), offset, piece)) {
          failed_ = true;
          token_->cancel();  // wake the other in-flight reads
          break;
        }
      }
    } catch (...) {
      failed_ = true;
      token_->cancel();
    }
  }

  /// Gives every prefetched file / folder its Mac modification time (Explorer copies CF_HDROP files with their mtime).
  void prefetcher::apply_mtimes() {
    for (std::size_t i = 0; i < entries_.size(); ++i) {
      const FILETIME ft = filetime_from_unix_ms(entries_[i].mtime_ms);
      HANDLE h = CreateFileW(path_for(i).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
      if (h == INVALID_HANDLE_VALUE) {
        continue;  // cosmetic only
      }
      SetFileTime(h, nullptr, nullptr, &ft);
      CloseHandle(h);
    }
  }

  void prefetcher::run() {
    std::error_code ec;
    fs::create_directories(extended_path(dir_), ec);
    if (ec) {
      failed_ = true;
    } else {
      // Up to k_prefetch_parallel reads (4 x 4 MiB) in flight across files and pieces.
      std::vector<std::thread> pool;
      for (std::size_t i = 0; i < k_prefetch_parallel; ++i) {
        pool.emplace_back([this] {
          worker();
        });
      }
      for (auto &t : pool) {
        t.join();
      }
      if (!failed_.load()) {
        apply_mtimes();  // after the last piece of every file is written
      }
    }
    {
      std::lock_guard lock(m_);
      state_ = failed_.load() ? state::failed : state::done;
    }
    cv_.notify_all();
  }
}  // namespace clipboard_agent
