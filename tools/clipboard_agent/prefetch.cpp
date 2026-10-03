/**
 * @file tools/clipboard_agent/prefetch.cpp
 * @brief Background download of a whole offer into a temp folder (backs CF_HDROP for small offers).
 */
#include "prefetch.h"

#include "file_stream.h"
#include "src/platform/windows/utf_utils.h"

#include <algorithm>
#include <fstream>

namespace clipboard_agent {
  namespace fs = std::filesystem;
  using clipboard::files::entry_kind;

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
      dir_(root / clipboard::files::offer_id_hex(o.id)),
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

  void prefetcher::run() {
    bool ok = true;
    try {
      std::error_code ec;
      fs::create_directories(extended_path(dir_), ec);
      ok = !ec;
      for (std::size_t i = 0; ok && i < entries_.size(); ++i) {
        const auto &e = entries_[i];
        const fs::path target = path_for(i);
        if (e.kind == entry_kind::directory) {
          fs::create_directories(target, ec);
          ok = !ec;
          continue;
        }
        fs::create_directories(target.parent_path(), ec);
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        ok = static_cast<bool>(out);
        std::uint64_t off = 0;
        std::string piece;
        while (ok && off < e.size) {
          if (token_->cancelled()) {
            ok = false;
            break;
          }
          const auto len = static_cast<std::uint32_t>(std::min<std::uint64_t>(k_piece_bytes, e.size - off));
          piece.clear();
          const auto rr = src_->read_cancellable(static_cast<std::uint32_t>(i), off, len, piece, token_);
          if (!rr.ok || piece.size() != len) {
            ok = false;
            break;
          }
          out.write(piece.data(), static_cast<std::streamsize>(piece.size()));
          ok = static_cast<bool>(out);
          off += len;
        }
        out.close();
        ok = ok && !out.fail();
      }
    } catch (...) {
      ok = false;
    }
    {
      std::lock_guard lock(m_);
      state_ = ok ? state::done : state::failed;
    }
    cv_.notify_all();
  }
}  // namespace clipboard_agent
