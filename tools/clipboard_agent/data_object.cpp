/**
 * @file tools/clipboard_agent/data_object.cpp
 * @brief OLE data object exposing an offer as virtual files (and CF_HDROP when prefetched).
 */
#include "data_object.h"

#include "file_stream.h"
#include "prefetch.h"
#include "src/platform/windows/utf_utils.h"

#include <shlobj.h>

#include <chrono>
#include <cstring>
#include <optional>

namespace clipboard_agent {
  using clipboard::files::entry_kind;

  namespace {
    constexpr DWORD k_fd_unicode = 0x80000000u;
    constexpr auto k_prefetch_wait = std::chrono::seconds(120);

    struct format_ids {
      CLIPFORMAT descriptor;
      CLIPFORMAT contents;
      CLIPFORMAT preferred_effect;
      CLIPFORMAT performed_effect;
      CLIPFORMAT paste_succeeded;
    };

    const format_ids &ids() {
      static const format_ids f {
        static_cast<CLIPFORMAT>(RegisterClipboardFormatA(CFSTR_FILEDESCRIPTORW)),
        static_cast<CLIPFORMAT>(RegisterClipboardFormatA(CFSTR_FILECONTENTS)),
        static_cast<CLIPFORMAT>(RegisterClipboardFormatA(CFSTR_PREFERREDDROPEFFECT)),
        static_cast<CLIPFORMAT>(RegisterClipboardFormatA(CFSTR_PERFORMEDDROPEFFECT)),
        static_cast<CLIPFORMAT>(RegisterClipboardFormatA(CFSTR_PASTESUCCEEDED)),
      };
      return f;
    }

    bool fits_descriptor(const std::string &utf8_path) {
      return utf_utils::from_utf8(utf8_path).size() <= k_max_descriptor_name;
    }

    FORMATETC make_fmt(CLIPFORMAT cf, DWORD tymed) {
      FORMATETC f {};
      f.cfFormat = cf;
      f.dwAspect = DVASPECT_CONTENT;
      f.lindex = -1;
      f.tymed = tymed;
      return f;
    }

    HRESULT hglobal_medium(const void *data, SIZE_T bytes, STGMEDIUM *out) {
      HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, bytes);
      if (h == nullptr) {
        return STG_E_MEDIUMFULL;
      }
      void *p = GlobalLock(h);
      if (p == nullptr) {
        GlobalFree(h);
        return E_OUTOFMEMORY;
      }
      std::memcpy(p, data, bytes);
      GlobalUnlock(h);
      out->tymed = TYMED_HGLOBAL;
      out->hGlobal = h;
      out->pUnkForRelease = nullptr;
      return S_OK;
    }

    class data_object final: public IDataObject {
    public:
      data_object(offer o, std::shared_ptr<range_source> src, std::filesystem::path root):
          o_(std::move(o)),
          src_(std::move(src)) {
        for (std::size_t i = 0; i < o_.entries.size(); ++i) {
          if (fits_descriptor(o_.windows_paths[i])) {
            desc_to_entry_.push_back(i);
          }
        }
        if (o_.prefetch) {
          pf_ = std::make_unique<prefetcher>(o_, src_, std::move(root));
        }
      }

      // IUnknown
      HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void **ppv) override {
        if (ppv == nullptr) {
          return E_POINTER;
        }
        if (riid == IID_IUnknown || riid == IID_IDataObject) {
          *ppv = static_cast<IDataObject *>(this);
          AddRef();
          return S_OK;
        }
        *ppv = nullptr;
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

      // IDataObject
      HRESULT STDMETHODCALLTYPE GetData(FORMATETC *fmt, STGMEDIUM *med) override {
        if (fmt == nullptr || med == nullptr) {
          return E_POINTER;
        }
        std::memset(med, 0, sizeof(*med));
        const HRESULT q = check(*fmt);
        if (FAILED(q)) {
          return q;
        }
        const auto &f = ids();
        if (fmt->cfFormat == f.descriptor) {
          return get_descriptor(med);
        }
        if (fmt->cfFormat == f.contents) {
          return get_contents(fmt->lindex, med);
        }
        if (fmt->cfFormat == f.preferred_effect) {
          const DWORD effect = DROPEFFECT_COPY;
          return hglobal_medium(&effect, sizeof(effect), med);
        }
        return get_hdrop(med);
      }

      HRESULT STDMETHODCALLTYPE GetDataHere(FORMATETC *, STGMEDIUM *) override {
        return E_NOTIMPL;
      }

      HRESULT STDMETHODCALLTYPE QueryGetData(FORMATETC *fmt) override {
        if (fmt == nullptr) {
          return E_POINTER;
        }
        return check(*fmt);
      }

      HRESULT STDMETHODCALLTYPE GetCanonicalFormatEtc(FORMATETC *, FORMATETC *out) override {
        if (out != nullptr) {
          out->ptd = nullptr;
        }
        return DATA_S_SAMEFORMATETC;
      }

      HRESULT STDMETHODCALLTYPE SetData(FORMATETC *fmt, STGMEDIUM *med, WINBOOL release) override {
        if (fmt == nullptr || med == nullptr) {
          return E_POINTER;
        }
        const auto &f = ids();
        if ((fmt->cfFormat != f.performed_effect && fmt->cfFormat != f.paste_succeeded) || med->tymed != TYMED_HGLOBAL || med->hGlobal == nullptr) {
          return E_NOTIMPL;
        }
        if (const void *p = GlobalLock(med->hGlobal)) {
          if (GlobalSize(med->hGlobal) >= sizeof(DWORD)) {
            DWORD v = 0;
            std::memcpy(&v, p, sizeof(v));
            (fmt->cfFormat == f.performed_effect ? performed_effect_ : paste_succeeded_) = v;
          }
          GlobalUnlock(med->hGlobal);
        }
        if (release) {
          ReleaseStgMedium(med);
        }
        return S_OK;
      }

      HRESULT STDMETHODCALLTYPE EnumFormatEtc(DWORD direction, IEnumFORMATETC **out) override {
        if (out == nullptr) {
          return E_POINTER;
        }
        *out = nullptr;
        if (direction != DATADIR_GET) {
          return E_NOTIMPL;
        }
        const auto &f = ids();
        FORMATETC list[4];
        UINT n = 0;
        list[n++] = make_fmt(f.descriptor, TYMED_HGLOBAL);
        list[n++] = make_fmt(f.contents, TYMED_ISTREAM);
        list[n++] = make_fmt(f.preferred_effect, TYMED_HGLOBAL);
        if (pf_) {
          list[n++] = make_fmt(CF_HDROP, TYMED_HGLOBAL);
        }
        return SHCreateStdEnumFmtEtc(n, list, out);
      }

      HRESULT STDMETHODCALLTYPE DAdvise(FORMATETC *, DWORD, IAdviseSink *, DWORD *) override {
        return OLE_E_ADVISENOTSUPPORTED;
      }

      HRESULT STDMETHODCALLTYPE DUnadvise(DWORD) override {
        return OLE_E_ADVISENOTSUPPORTED;
      }

      HRESULT STDMETHODCALLTYPE EnumDAdvise(IEnumSTATDATA **) override {
        return OLE_E_ADVISENOTSUPPORTED;
      }

    private:
      HRESULT check(const FORMATETC &fmt) const {
        const auto &f = ids();
        if (fmt.dwAspect != DVASPECT_CONTENT) {
          return DV_E_DVASPECT;
        }
        if (fmt.cfFormat == f.descriptor || fmt.cfFormat == f.preferred_effect) {
          return (fmt.tymed & TYMED_HGLOBAL) ? S_OK : DV_E_TYMED;
        }
        if (fmt.cfFormat == f.contents) {
          if (!(fmt.tymed & TYMED_ISTREAM)) {
            return DV_E_TYMED;
          }
          if (fmt.lindex == -1) {
            return S_OK;
          }
          return entry_for_lindex(fmt.lindex) ? S_OK : DV_E_LINDEX;
        }
        if (fmt.cfFormat == CF_HDROP && pf_) {
          return (fmt.tymed & TYMED_HGLOBAL) ? S_OK : DV_E_TYMED;
        }
        return DV_E_FORMATETC;
      }

      /// Entry index for a descriptor index, nullopt for out of range or directories.
      std::optional<std::size_t> entry_for_lindex(LONG lindex) const {
        if (lindex < 0 || static_cast<std::size_t>(lindex) >= desc_to_entry_.size()) {
          return std::nullopt;
        }
        const std::size_t e = desc_to_entry_[static_cast<std::size_t>(lindex)];
        if (o_.entries[e].kind != entry_kind::file) {
          return std::nullopt;
        }
        return e;
      }

      HRESULT get_descriptor(STGMEDIUM *med) const {
        const std::size_t n = desc_to_entry_.size();
        const std::size_t bytes = offsetof(FILEGROUPDESCRIPTORW, fgd) + n * sizeof(FILEDESCRIPTORW);
        std::string buf(bytes, '\0');
        auto *g = reinterpret_cast<FILEGROUPDESCRIPTORW *>(buf.data());
        g->cItems = static_cast<UINT>(n);
        for (std::size_t i = 0; i < n; ++i) {
          const auto &e = o_.entries[desc_to_entry_[i]];
          FILEDESCRIPTORW d {};
          d.dwFlags = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI | k_fd_unicode;
          d.dwFileAttributes = e.kind == entry_kind::directory ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
          d.ftLastWriteTime = filetime_from_unix_ms(e.mtime_ms);
          const std::uint64_t size = e.kind == entry_kind::directory ? 0 : e.size;
          d.nFileSizeHigh = static_cast<DWORD>(size >> 32);
          d.nFileSizeLow = static_cast<DWORD>(size & 0xFFFFFFFFu);
          const auto name = utf_utils::from_utf8(o_.windows_paths[desc_to_entry_[i]]);
          std::memcpy(d.cFileName, name.c_str(), (name.size() + 1) * sizeof(wchar_t));
          std::memcpy(buf.data() + offsetof(FILEGROUPDESCRIPTORW, fgd) + i * sizeof(FILEDESCRIPTORW), &d, sizeof(d));
        }
        return hglobal_medium(buf.data(), buf.size(), med);
      }

      HRESULT get_contents(LONG lindex, STGMEDIUM *med) {
        const auto e = entry_for_lindex(lindex);
        if (!e) {
          return DV_E_LINDEX;
        }
        const auto &en = o_.entries[*e];
        const auto &path = o_.windows_paths[*e];
        const auto slash = path.rfind('\\');
        const auto leaf = utf_utils::from_utf8(slash == std::string::npos ? path : path.substr(slash + 1));
        const bool from_disk = pf_ && pf_->current() == prefetcher::state::done;
        const auto size = en.size;
        const auto mtime = en.mtime_ms;
        const auto index = static_cast<std::uint32_t>(*e);
        IStream *s = create_mta_hosted_stream([&]() -> IStream * {
          if (from_disk) {
            return create_disk_stream(pf_->path_for(*e), size, leaf, mtime);
          }
          return create_remote_stream(src_, index, size, leaf, mtime);
        });
        if (s == nullptr) {
          return E_FAIL;
        }
        med->tymed = TYMED_ISTREAM;
        med->pstm = s;
        med->pUnkForRelease = nullptr;
        return S_OK;
      }

      HRESULT get_hdrop(STGMEDIUM *med) {
        if (!pf_) {
          return DV_E_FORMATETC;
        }
        if (pf_->wait(std::chrono::duration_cast<std::chrono::milliseconds>(k_prefetch_wait)) != prefetcher::state::done) {
          return E_FAIL;
        }
        std::wstring list;
        for (const auto &p : pf_->top_level_paths()) {
          list += p.wstring();
          list.push_back(L'\0');
        }
        list.push_back(L'\0');
        std::string buf(sizeof(DROPFILES) + list.size() * sizeof(wchar_t), '\0');
        DROPFILES df {};
        df.pFiles = sizeof(DROPFILES);
        df.fWide = TRUE;
        std::memcpy(buf.data(), &df, sizeof(df));
        std::memcpy(buf.data() + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
        return hglobal_medium(buf.data(), buf.size(), med);
      }

      std::atomic<long> refs_ {1};
      offer o_;
      std::shared_ptr<range_source> src_;
      std::vector<std::size_t> desc_to_entry_;
      std::unique_ptr<prefetcher> pf_;
      DWORD performed_effect_ {0};
      DWORD paste_succeeded_ {0};
    };
  }  // namespace

  IDataObject *create_data_object(offer o, std::shared_ptr<range_source> src, std::filesystem::path prefetch_root) {
    return new data_object(std::move(o), std::move(src), std::move(prefetch_root));
  }

  bool offer_fits_descriptors(const offer &o) {
    return descriptor_omitted_count(o) == 0;
  }

  std::size_t descriptor_omitted_count(const offer &o) {
    std::size_t n = 0;
    for (const auto &p : o.windows_paths) {
      if (!fits_descriptor(p)) {
        ++n;
      }
    }
    return n;
  }
}  // namespace clipboard_agent
