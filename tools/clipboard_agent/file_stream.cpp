/**
 * @file tools/clipboard_agent/file_stream.cpp
 * @brief Read-only IStream over a remote file (read-ahead) or a prefetched file on disk.
 */
#include "file_stream.h"

#include <objbase.h>

#include <algorithm>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <mutex>
#include <vector>
#include <cstring>
#include <fstream>
#include <thread>

namespace clipboard_agent {
  FILETIME filetime_from_unix_ms(std::int64_t ms) {
    constexpr std::int64_t epoch_diff_ms = 11644473600000LL;
    std::int64_t v = ms;
    if (v > (INT64_MAX / 10000) - epoch_diff_ms) {
      v = (INT64_MAX / 10000) - epoch_diff_ms;
    }
    v += epoch_diff_ms;
    if (v < 0) {
      v = 0;
    }
    const auto ticks = static_cast<std::uint64_t>(v) * 10000ULL;
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(ticks & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(ticks >> 32);
    return ft;
  }

  namespace {
    constexpr std::size_t k_max_pieces = k_readahead_bytes / k_piece_bytes;  ///< concurrent reads == window / piece

    class stream_impl final: public IStream {
    public:
      stream_impl(std::uint64_t size, std::wstring name, std::int64_t mtime_ms):
          size_(size),
          name_(std::move(name)),
          mtime_(filetime_from_unix_ms(mtime_ms)) {}

      void init_remote(std::shared_ptr<range_source> src, std::uint32_t index) {
        src_ = std::move(src);
        index_ = index;
        token_ = std::make_shared<cancel_token>();
        for (std::size_t i = 0; i < k_max_pieces; ++i) {
          workers_.emplace_back([this] {
            worker_loop();
          });
        }
      }

      void init_disk(const std::filesystem::path &path) {
        file_.open(path, std::ios::binary);
      }

      ~stream_impl() {
        if (!workers_.empty()) {
          std::shared_ptr<cancel_token> tok;
          {
            std::lock_guard lock(m_);
            stop_ = true;
            tok = token_;
          }
          tok->cancel();
          cv_.notify_all();
          for (auto &w : workers_) {
            w.join();
          }
        }
      }

      // IUnknown
      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (ppv == nullptr) {
          return E_POINTER;
        }
        *ppv = nullptr;
        if (riid == IID_IUnknown || riid == IID_ISequentialStream || riid == IID_IStream) {
          *ppv = static_cast<IStream *>(this);
          AddRef();
          return S_OK;
        }
        return E_NOINTERFACE;
      }

      ULONG STDMETHODCALLTYPE AddRef() override {
        return static_cast<ULONG>(++refs_);
      }

      ULONG STDMETHODCALLTYPE Release() override {
        const auto n = --refs_;
        if (n == 0) {
          delete this;
        }
        return static_cast<ULONG>(n);
      }

      // ISequentialStream
      HRESULT STDMETHODCALLTYPE Read(void *pv, ULONG cb, ULONG *pcbRead) override {
        if (pcbRead != nullptr) {
          *pcbRead = 0;
        }
        if (pv == nullptr && cb != 0) {
          return STG_E_INVALIDPOINTER;
        }
        ULONG got = 0;
        const HRESULT hr = src_ ? read_remote(static_cast<char *>(pv), cb, got) : read_disk(static_cast<char *>(pv), cb, got);
        if (pcbRead != nullptr) {
          *pcbRead = got;
        }
        return hr;
      }

      HRESULT STDMETHODCALLTYPE Write(const void *, ULONG, ULONG *) override {
        return STG_E_ACCESSDENIED;
      }

      // IStream
      HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER *newpos) override {
        std::lock_guard lock(m_);
        std::int64_t base = 0;
        switch (origin) {
          case STREAM_SEEK_SET:
            base = 0;
            break;
          case STREAM_SEEK_CUR:
            base = static_cast<std::int64_t>(pos_);
            break;
          case STREAM_SEEK_END:
            base = static_cast<std::int64_t>(size_);
            break;
          default:
            return STG_E_INVALIDFUNCTION;
        }
        const std::int64_t target = base + move.QuadPart;
        if (target < 0) {
          return STG_E_INVALIDFUNCTION;
        }
        const auto np = static_cast<std::uint64_t>(target);
        if (src_ && np != pos_) {
          const bool in_window = (!pieces_.empty() && np >= pieces_.begin()->first && np <= next_fetch_) || (pieces_.empty() && np == next_fetch_);
          if (!in_window) {
            ++gen_;  // outstanding pieces belong to the old position: cancel and forget them
            token_->cancel();
            token_ = std::make_shared<cancel_token>();
            pieces_.clear();
            next_fetch_ = np;
          }
          cv_.notify_all();
        }
        pos_ = np;
        if (newpos != nullptr) {
          newpos->QuadPart = np;
        }
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER) override {
        return STG_E_ACCESSDENIED;
      }

      HRESULT STDMETHODCALLTYPE CopyTo(IStream *dest, ULARGE_INTEGER cb, ULARGE_INTEGER *pcbRead, ULARGE_INTEGER *pcbWritten) override {
        if (dest == nullptr) {
          return STG_E_INVALIDPOINTER;
        }
        std::uint64_t remaining = cb.QuadPart;
        std::uint64_t total_read = 0;
        std::uint64_t total_written = 0;
        std::string tmp(64 * 1024, '\0');
        HRESULT hr = S_OK;
        while (remaining > 0) {
          const auto want = static_cast<ULONG>(std::min<std::uint64_t>(remaining, tmp.size()));
          ULONG got = 0;
          hr = Read(tmp.data(), want, &got);
          if (FAILED(hr) || got == 0) {
            break;
          }
          total_read += got;
          ULONG wrote = 0;
          hr = dest->Write(tmp.data(), got, &wrote);
          total_written += wrote;
          if (FAILED(hr) || wrote != got) {
            if (SUCCEEDED(hr)) {
              hr = STG_E_MEDIUMFULL;
            }
            break;
          }
          remaining -= got;
        }
        if (pcbRead != nullptr) {
          pcbRead->QuadPart = total_read;
        }
        if (pcbWritten != nullptr) {
          pcbWritten->QuadPart = total_written;
        }
        return FAILED(hr) ? hr : S_OK;
      }

      HRESULT STDMETHODCALLTYPE Commit(DWORD) override {
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE Revert() override {
        return E_NOTIMPL;
      }

      HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
      }

      HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override {
        return STG_E_INVALIDFUNCTION;
      }

      HRESULT STDMETHODCALLTYPE Stat(STATSTG *st, DWORD flags) override {
        if (st == nullptr) {
          return STG_E_INVALIDPOINTER;
        }
        std::memset(st, 0, sizeof(*st));
        if (!(flags & STATFLAG_NONAME)) {
          const auto bytes = (name_.size() + 1) * sizeof(wchar_t);
          st->pwcsName = static_cast<LPOLESTR>(CoTaskMemAlloc(bytes));
          if (st->pwcsName == nullptr) {
            return E_OUTOFMEMORY;
          }
          std::memcpy(st->pwcsName, name_.c_str(), bytes);
        }
        st->type = STGTY_STREAM;
        st->cbSize.QuadPart = size_;
        st->mtime = mtime_;
        st->grfMode = STGM_READ;
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE Clone(IStream **ppstm) override {
        if (ppstm != nullptr) {
          *ppstm = nullptr;
        }
        return E_NOTIMPL;
      }

    private:
      HRESULT read_disk(char *dst, ULONG cb, ULONG &got) {
        std::lock_guard lock(m_);
        if (pos_ >= size_ || cb == 0) {
          return S_OK;
        }
        if (!file_) {
          return STG_E_READFAULT;
        }
        file_.clear();
        file_.seekg(static_cast<std::streamoff>(pos_));
        const auto want = static_cast<std::streamsize>(std::min<std::uint64_t>(cb, size_ - pos_));
        file_.read(dst, want);
        const auto n = file_.gcount();
        if (n <= 0) {
          return STG_E_READFAULT;
        }
        got = static_cast<ULONG>(n);
        pos_ += got;
        return S_OK;
      }

      /// Drops fully consumed pieces from the front (caller holds m_).
      void drop_consumed() {
        while (!pieces_.empty()) {
          const auto &[start, p] = *pieces_.begin();
          if (start + p.len > pos_) {
            break;
          }
          pieces_.erase(pieces_.begin());
        }
      }

      HRESULT read_remote(char *dst, ULONG cb, ULONG &got) {
        std::unique_lock lock(m_);
        while (got < cb && pos_ < size_) {
          drop_consumed();
          auto it = pieces_.upper_bound(pos_);
          piece *p = nullptr;
          std::uint64_t start = 0;
          if (it != pieces_.begin()) {
            --it;
            if (pos_ < it->first + it->second.len) {
              p = &it->second;
              start = it->first;
            }
          }
          if (p != nullptr && p->st == piece::failed) {
            return STG_E_READFAULT;
          }
          if (p == nullptr || p->st == piece::pending) {
            if (stop_) {
              return STG_E_READFAULT;
            }
            cv_.notify_all();  // workers may be idle waiting for a free slot
            cv_.wait(lock);
            continue;
          }
          const auto at = static_cast<std::size_t>(pos_ - start);
          const std::size_t n = std::min<std::size_t>(cb - got, p->data.size() - at);
          std::memcpy(dst + got, p->data.data() + at, n);
          got += static_cast<ULONG>(n);
          pos_ += n;
          if (pos_ >= start + p->len) {
            drop_consumed();
            cv_.notify_all();  // a slot is free
          }
        }
        return S_OK;
      }

      /// Up to k_max_pieces of these run concurrently, each issuing one <= 4 MiB read at consecutive offsets.
      void worker_loop() {
        std::unique_lock lock(m_);
        while (!stop_) {
          drop_consumed();
          if (next_fetch_ >= size_ || pieces_.size() >= k_max_pieces) {
            cv_.wait(lock);
            continue;
          }
          const std::uint64_t start = next_fetch_;
          const auto len = static_cast<std::uint32_t>(std::min<std::uint64_t>(k_piece_bytes, size_ - start));
          next_fetch_ += len;
          pieces_[start] = piece {len, piece::pending, {}};
          const auto gen = gen_;
          const auto tok = token_;
          lock.unlock();
          std::string data;
          read_result rr {false, clipboard::files::read_error::io};
          try {
            rr = src_->read_cancellable(index_, start, len, data, tok);
          } catch (...) {
            rr = {false, clipboard::files::read_error::io};
          }
          lock.lock();
          auto it = pieces_.find(start);
          if (gen == gen_ && it != pieces_.end()) {
            if (rr.ok && data.size() == len) {
              it->second.data = std::move(data);
              it->second.st = piece::ready;
            } else {
              it->second.st = piece::failed;
            }
          }  // else: seeked away / consumed while in flight; discard
          cv_.notify_all();
        }
      }

      struct piece {
        std::uint32_t len;
        enum state_t {
          pending,
          ready,
          failed
        } st;
        std::string data;
      };

      std::atomic<long> refs_ {1};
      const std::uint64_t size_;
      const std::wstring name_;
      const FILETIME mtime_;

      std::mutex m_;
      std::condition_variable cv_;
      std::uint64_t pos_ {0};

      // remote mode
      std::shared_ptr<range_source> src_;
      std::uint32_t index_ {0};
      std::shared_ptr<cancel_token> token_;
      std::vector<std::thread> workers_;
      std::map<std::uint64_t, piece> pieces_;  ///< claimed pieces, contiguous from the first key up to next_fetch_
      std::uint64_t next_fetch_ {0};
      std::uint64_t gen_ {0};
      bool stop_ {false};

      // disk mode
      std::ifstream file_;
    };

    /// Process-lifetime MTA thread that hosts the streams, so IStream calls from other processes arrive on RPC
    /// threads and never queue behind the (message-pumping) STA that owns the clipboard.
    class mta_host {
    public:
      mta_host():
          thread_([this] {
            run();
          }) {}

      /// Runs `fn` on the MTA thread and waits for it (used for object creation only, never for I/O).
      bool run_sync(std::function<void()> fn) {
        auto t = std::make_shared<task>();
        t->fn = std::move(fn);
        {
          std::lock_guard lock(m_);
          if (stop_) {
            return false;
          }
          q_.push_back(t);
        }
        cv_.notify_all();
        std::unique_lock lock(t->m);
        t->cv.wait(lock, [&] {
          return t->done;
        });
        return true;
      }

      void stop() {
        {
          std::lock_guard lock(m_);
          stop_ = true;
        }
        cv_.notify_all();
        if (thread_.joinable()) {
          thread_.join();
        }
      }

    private:
      struct task {
        std::function<void()> fn;
        std::mutex m;
        std::condition_variable cv;
        bool done {false};
      };

      void run() {
        const bool com = SUCCEEDED(CoInitializeEx(nullptr, COINIT_MULTITHREADED));
        for (;;) {
          std::shared_ptr<task> t;
          {
            std::unique_lock lock(m_);
            cv_.wait(lock, [&] {
              return stop_ || !q_.empty();
            });
            if (q_.empty()) {
              break;  // stop requested and drained
            }
            t = q_.front();
            q_.pop_front();
          }
          t->fn();
          {
            std::lock_guard lock(t->m);
            t->done = true;
          }
          t->cv.notify_all();
        }
        if (com) {
          CoUninitialize();
        }
      }

      std::mutex m_;
      std::condition_variable cv_;
      std::deque<std::shared_ptr<task>> q_;
      bool stop_ {false};
      std::thread thread_;
    };

    std::mutex g_host_mutex;
    mta_host *g_host = nullptr;  // intentionally never deleted: a joinable thread must not be destroyed at exit
    bool g_host_stopped = false;

    mta_host *host() {
      std::lock_guard lock(g_host_mutex);
      if (g_host == nullptr && !g_host_stopped) {
        g_host = new mta_host();
      }
      return g_host;
    }
  }  // namespace

  IStream *create_mta_hosted_stream(std::function<IStream *()> create) {
    IStream *marshaled = nullptr;  // CoMarshalInterThreadInterfaceInStream result (an IStream holding the marshal data)
    IStream *direct = nullptr;
    mta_host *h = host();
    const bool ran = h != nullptr && h->run_sync([&] {
      IStream *raw = create();
      if (raw == nullptr) {
        return;
      }
      if (SUCCEEDED(CoMarshalInterThreadInterfaceInStream(IID_IStream, raw, &marshaled))) {
        raw->Release();  // the marshal stub now owns the object
      } else {
        direct = raw;
      }
    });
    if (!ran) {
      return create();  // host already shut down: plain object on the caller's thread
    }
    if (direct != nullptr) {
      return direct;
    }
    if (marshaled == nullptr) {
      return nullptr;
    }
    IStream *proxy = nullptr;
    if (FAILED(CoGetInterfaceAndReleaseStream(marshaled, IID_IStream, reinterpret_cast<void **>(&proxy)))) {
      return nullptr;
    }
    return proxy;
  }

  void shutdown_mta_host() {
    mta_host *h = nullptr;
    {
      std::lock_guard lock(g_host_mutex);
      h = g_host;
      g_host = nullptr;
      g_host_stopped = true;
    }
    if (h != nullptr) {
      h->stop();
      delete h;
    }
  }

  IStream *create_remote_stream(std::shared_ptr<range_source> src, std::uint32_t index, std::uint64_t size, std::wstring name, std::int64_t mtime_ms) {
    auto *s = new stream_impl(size, std::move(name), mtime_ms);
    s->init_remote(std::move(src), index);
    return s;
  }

  IStream *create_disk_stream(std::filesystem::path path, std::uint64_t size, std::wstring name, std::int64_t mtime_ms) {
    auto *s = new stream_impl(size, std::move(name), mtime_ms);
    s->init_disk(path);
    return s;
  }
}  // namespace clipboard_agent
