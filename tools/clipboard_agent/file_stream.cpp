/**
 * @file tools/clipboard_agent/file_stream.cpp
 * @brief Read-only IStream over a remote file (read-ahead) or a prefetched file on disk.
 */
#include "file_stream.h"

#include <objbase.h>

#include <algorithm>
#include <condition_variable>
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
    class stream_impl final: public IStream {
    public:
      stream_impl(std::uint64_t size, std::wstring name, std::int64_t mtime_ms):
          size_(size),
          name_(std::move(name)),
          mtime_(filetime_from_unix_ms(mtime_ms)) {
        // Cross-process reads must not be funnelled through the (message-pumping) STA.
        IUnknown *unk = nullptr;
        if (SUCCEEDED(CoCreateFreeThreadedMarshaler(static_cast<IStream *>(this), &unk))) {
          ftm_ = unk;
        }
      }

      void init_remote(std::shared_ptr<range_source> src, std::uint32_t index) {
        src_ = std::move(src);
        index_ = index;
        token_ = std::make_shared<cancel_token>();
        worker_ = std::thread([this] {
          worker_loop();
        });
      }

      void init_disk(std::filesystem::path path) {
        path_ = std::move(path);
      }

      ~stream_impl() {
        if (worker_.joinable()) {
          {
            std::lock_guard lock(m_);
            stop_ = true;
          }
          token_->cancel();
          cv_.notify_all();
          worker_.join();
        }
        if (ftm_ != nullptr) {
          ftm_->Release();
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
        if (riid == IID_IMarshal && ftm_ != nullptr) {
          return ftm_->QueryInterface(riid, ppv);
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
          if (np >= buf_start_ && np <= buf_start_ + buf_.size()) {
            buf_off_ = static_cast<std::size_t>(np - buf_start_);
          } else {
            ++gen_;
            buf_.clear();
            buf_off_ = 0;
            buf_start_ = np;
            err_ = false;
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
        std::ifstream in(path_, std::ios::binary);
        if (!in) {
          return STG_E_READFAULT;
        }
        in.seekg(static_cast<std::streamoff>(pos_));
        const auto want = static_cast<std::streamsize>(std::min<std::uint64_t>(cb, size_ - pos_));
        in.read(dst, want);
        const auto n = in.gcount();
        if (n <= 0) {
          return STG_E_READFAULT;
        }
        got = static_cast<ULONG>(n);
        pos_ += got;
        return S_OK;
      }

      HRESULT read_remote(char *dst, ULONG cb, ULONG &got) {
        std::unique_lock lock(m_);
        while (got < cb && pos_ < size_) {
          const bool in_window = pos_ >= buf_start_ + buf_off_ && pos_ < buf_start_ + buf_.size();
          if (!in_window) {
            if (err_) {
              return STG_E_READFAULT;
            }
            if (stop_) {
              return STG_E_READFAULT;
            }
            cv_.notify_all();  // worker may be idle waiting for a window slot
            cv_.wait(lock);
            continue;
          }
          const std::size_t at = static_cast<std::size_t>(pos_ - buf_start_);
          const std::size_t n = std::min<std::size_t>({static_cast<std::size_t>(cb - got), buf_.size() - at, static_cast<std::size_t>(size_ - pos_)});
          std::memcpy(dst + got, buf_.data() + at, n);
          got += static_cast<ULONG>(n);
          pos_ += n;
          buf_off_ = static_cast<std::size_t>(pos_ - buf_start_);
          if (buf_off_ >= k_piece_bytes) {  // compact consumed bytes
            buf_.erase(0, buf_off_);
            buf_start_ += buf_off_;
            buf_off_ = 0;
          }
          cv_.notify_all();
        }
        return S_OK;
      }

      void worker_loop() {
        std::unique_lock lock(m_);
        while (!stop_) {
          const std::uint64_t next = buf_start_ + buf_.size();
          const std::size_t ahead = buf_.size() - std::min(buf_off_, buf_.size());
          if (inflight_ || err_ || next >= size_ || ahead + k_piece_bytes > k_readahead_bytes) {
            cv_.wait(lock);
            continue;
          }
          const auto gen = gen_;
          const auto len = static_cast<std::uint32_t>(std::min<std::uint64_t>(k_piece_bytes, size_ - next));
          inflight_ = true;
          lock.unlock();
          std::string piece;
          read_result rr {false, clipboard::files::read_error::io};
          try {
            rr = src_->read_cancellable(index_, next, len, piece, token_);
          } catch (...) {
            rr = {false, clipboard::files::read_error::io};
          }
          lock.lock();
          inflight_ = false;
          if (gen == gen_) {
            if (rr.ok && piece.size() == len) {
              buf_ += piece;
            } else {
              err_ = true;
            }
          }
          cv_.notify_all();
        }
      }

      std::atomic<long> refs_ {1};
      IUnknown *ftm_ {nullptr};
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
      std::thread worker_;
      std::string buf_;
      std::uint64_t buf_start_ {0};  ///< file offset of buf_[0]
      std::size_t buf_off_ {0};  ///< consumed prefix of buf_
      std::uint64_t gen_ {0};
      bool inflight_ {false};
      bool err_ {false};
      bool stop_ {false};

      // disk mode
      std::filesystem::path path_;
    };
  }  // namespace

  IStream *create_remote_stream(std::shared_ptr<range_source> src, std::uint32_t index, std::uint64_t size, std::wstring name, std::int64_t mtime_ms) {
    auto *s = new stream_impl(size, std::move(name), mtime_ms);
    s->init_remote(std::move(src), index);
    return s;
  }

  IStream *create_disk_stream(std::filesystem::path path, std::uint64_t size, std::wstring name, std::int64_t mtime_ms) {
    auto *s = new stream_impl(size, std::move(name), mtime_ms);
    s->init_disk(std::move(path));
    return s;
  }
}  // namespace clipboard_agent
