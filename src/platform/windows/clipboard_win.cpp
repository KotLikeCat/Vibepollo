/**
 * @file src/platform/windows/clipboard_win.cpp
 * @brief Windows clipboard access for clipboard sync: text, HTML, RTF and PNG (via WIC).
 * @note No logging here on purpose: the file is also compiled into an isolated test.
 */
#include "src/platform/clipboard_sync.h"

#include "src/clipboard/formats.h"
#include "src/platform/windows/utf_utils.h"

#include <windows.h>
#include <objidl.h>
#include <shlwapi.h>
#include <wincodec.h>

#include <chrono>
#include <cstring>
#include <cwchar>
#include <string>
#include <thread>
#include <utility>

namespace {
  using ::clipboard::item;
  using ::clipboard::item_type;

  template<class T>
  class com_ref {
  public:
    com_ref() = default;
    com_ref(const com_ref &) = delete;
    com_ref &operator=(const com_ref &) = delete;

    ~com_ref() {
      if (ptr_) {
        ptr_->Release();
      }
    }

    T **put() {
      return &ptr_;
    }

    T *get() const {
      return ptr_;
    }

    T *operator->() const {
      return ptr_;
    }

    explicit operator bool() const {
      return ptr_ != nullptr;
    }

    void attach(T *ptr) {
      if (ptr_) {
        ptr_->Release();
      }
      ptr_ = ptr;
    }

  private:
    T *ptr_ = nullptr;
  };

  /// COM must be initialized on the calling (nvhttp) thread for WIC.
  class com_scope {
  public:
    com_scope():
        result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED)) {
    }

    ~com_scope() {
      if (SUCCEEDED(result_)) {
        CoUninitialize();
      }
    }

  private:
    HRESULT result_;
  };

  /// Another program may hold the clipboard briefly; retry 5 times, 20 ms apart.
  class clipboard_scope {
  public:
    clipboard_scope() {
      for (int attempt = 0; attempt < 5; ++attempt) {
        if (OpenClipboard(nullptr)) {
          open_ = true;
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
      }
    }

    ~clipboard_scope() {
      if (open_) {
        CloseClipboard();
      }
    }

    bool open() const {
      return open_;
    }

  private:
    bool open_ = false;
  };

  UINT cf_html() {
    static const UINT format = RegisterClipboardFormatW(L"HTML Format");
    return format;
  }

  UINT cf_rtf() {
    static const UINT format = RegisterClipboardFormatW(L"Rich Text Format");
    return format;
  }

  UINT cf_png() {
    static const UINT format = RegisterClipboardFormatW(L"PNG");
    return format;
  }

  /// GlobalSize() may round the block up, so callers must trim the result.
  std::optional<std::string> read_global(UINT format) {
    HANDLE handle = GetClipboardData(format);
    if (!handle) {
      return std::nullopt;
    }
    const SIZE_T size = GlobalSize(handle);
    const void *data = GlobalLock(handle);
    if (!data) {
      return std::nullopt;
    }
    std::string out(static_cast<const char *>(data), size);
    GlobalUnlock(handle);
    return out;
  }

  bool set_global(UINT format, const std::string &bytes) {
    HGLOBAL handle = GlobalAlloc(GMEM_MOVEABLE, bytes.size());
    if (!handle) {
      return false;
    }
    void *data = GlobalLock(handle);
    if (!data) {
      GlobalFree(handle);
      return false;
    }
    std::memcpy(data, bytes.data(), bytes.size());
    GlobalUnlock(handle);
    if (!SetClipboardData(format, handle)) {
      GlobalFree(handle);
      return false;
    }
    return true;
  }

  std::string until_nul(std::string bytes) {
    if (const auto pos = bytes.find('\0'); pos != std::string::npos) {
      bytes.resize(pos);
    }
    return bytes;
  }

  std::optional<std::string> read_text() {
    HANDLE handle = GetClipboardData(CF_UNICODETEXT);
    if (!handle) {
      return std::nullopt;
    }
    const auto *text = static_cast<const wchar_t *>(GlobalLock(handle));
    if (!text) {
      return std::nullopt;
    }
    const std::size_t max_chars = GlobalSize(handle) / sizeof(wchar_t);
    const std::wstring wide(text, wcsnlen(text, max_chars));
    GlobalUnlock(handle);
    return ::clipboard::crlf_to_lf(utf_utils::to_utf8(wide));
  }

  std::string dib_to_bmp_file(const std::string &dib) {
    if (dib.size() < sizeof(BITMAPINFOHEADER)) {
      return {};
    }
    BITMAPINFOHEADER header;
    std::memcpy(&header, dib.data(), sizeof(header));
    DWORD colors = header.biClrUsed;
    if (colors == 0 && header.biBitCount <= 8) {
      colors = 1u << header.biBitCount;
    }
    const DWORD masks = (header.biCompression == BI_BITFIELDS && header.biSize == sizeof(BITMAPINFOHEADER)) ? 3 * sizeof(DWORD) : 0;
    BITMAPFILEHEADER file {};
    file.bfType = 0x4D42;  // "BM"
    file.bfOffBits = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + header.biSize + masks + colors * sizeof(RGBQUAD));
    file.bfSize = static_cast<DWORD>(sizeof(BITMAPFILEHEADER) + dib.size());
    std::string out(reinterpret_cast<const char *>(&file), sizeof(file));
    out += dib;
    return out;
  }

  /// Decodes any WIC-readable image and re-encodes it as PNG, or as a 32bpp BGRA BMP with a V5 header.
  std::optional<std::string> transcode(const std::string &input, const GUID &container) {
    if (input.empty() || input.size() > 0xFFFFFFFFull) {
      return std::nullopt;
    }
    com_scope com;
    com_ref<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(factory.put())))) {
      return std::nullopt;
    }
    com_ref<IStream> source;
    source.attach(SHCreateMemStream(reinterpret_cast<const BYTE *>(input.data()), static_cast<UINT>(input.size())));
    if (!source) {
      return std::nullopt;
    }
    com_ref<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromStream(source.get(), nullptr, WICDecodeMetadataCacheOnDemand, decoder.put()))) {
      return std::nullopt;
    }
    com_ref<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, frame.put()))) {
      return std::nullopt;
    }
    com_ref<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(converter.put())) ||
        FAILED(converter->Initialize(frame.get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
      return std::nullopt;
    }
    com_ref<IStream> target;
    target.attach(SHCreateMemStream(nullptr, 0));
    if (!target) {
      return std::nullopt;
    }
    com_ref<IWICBitmapEncoder> encoder;
    if (FAILED(factory->CreateEncoder(container, nullptr, encoder.put())) ||
        FAILED(encoder->Initialize(target.get(), WICBitmapEncoderNoCache))) {
      return std::nullopt;
    }
    com_ref<IWICBitmapFrameEncode> output;
    com_ref<IPropertyBag2> options;
    if (FAILED(encoder->CreateNewFrame(output.put(), options.put()))) {
      return std::nullopt;
    }
    if (container == GUID_ContainerFormatBmp && options) {
      PROPBAG2 option {};
      option.pstrName = const_cast<LPOLESTR>(L"EnableV5Header32bppBGRA");
      VARIANT value;
      VariantInit(&value);
      value.vt = VT_BOOL;
      value.boolVal = VARIANT_TRUE;
      options->Write(1, &option, &value);
    }
    UINT width = 0;
    UINT height = 0;
    WICPixelFormatGUID pixel_format = GUID_WICPixelFormat32bppBGRA;
    if (FAILED(output->Initialize(options.get())) ||
        FAILED(converter->GetSize(&width, &height)) ||
        FAILED(output->SetSize(width, height)) ||
        FAILED(output->SetPixelFormat(&pixel_format)) ||
        FAILED(output->WriteSource(converter.get(), nullptr)) ||
        FAILED(output->Commit()) ||
        FAILED(encoder->Commit())) {
      return std::nullopt;
    }
    STATSTG stat {};
    if (FAILED(target->Stat(&stat, STATFLAG_NONAME))) {
      return std::nullopt;
    }
    std::string out(static_cast<std::size_t>(stat.cbSize.QuadPart), '\0');
    LARGE_INTEGER zero {};
    ULONG read = 0;
    if (FAILED(target->Seek(zero, STREAM_SEEK_SET, nullptr)) ||
        FAILED(target->Read(out.data(), static_cast<ULONG>(out.size()), &read))) {
      return std::nullopt;
    }
    out.resize(read);
    return out;
  }

  std::optional<std::string> read_image_as_png() {
    if (auto png = read_global(cf_png())) {
      return ::clipboard::trim_png(std::move(*png));
    }
    for (const UINT format : {static_cast<UINT>(CF_DIBV5), static_cast<UINT>(CF_DIB)}) {
      if (auto dib = read_global(format)) {
        const auto bmp = dib_to_bmp_file(*dib);
        if (!bmp.empty()) {
          return transcode(bmp, GUID_ContainerFormatPng);
        }
      }
    }
    return std::nullopt;
  }

  /// Converts wire items to native clipboard payloads before the clipboard is opened.
  std::vector<std::pair<UINT, std::string>> to_native(const std::vector<item> &items) {
    std::vector<std::pair<UINT, std::string>> native;
    for (const auto &entry : items) {
      switch (entry.type) {
        case item_type::text:
          {
            const std::wstring wide = utf_utils::from_utf8(::clipboard::lf_to_crlf(entry.data));
            native.emplace_back(CF_UNICODETEXT, std::string(reinterpret_cast<const char *>(wide.c_str()), (wide.size() + 1) * sizeof(wchar_t)));
            break;
          }
        case item_type::html:
          {
            std::string html = ::clipboard::cf_html_wrap(entry.data);
            html.push_back('\0');
            native.emplace_back(cf_html(), std::move(html));
            break;
          }
        case item_type::rtf:
          {
            std::string rtf = entry.data;
            rtf.push_back('\0');
            native.emplace_back(cf_rtf(), std::move(rtf));
            break;
          }
        case item_type::png:
          {
            native.emplace_back(cf_png(), entry.data);
            if (auto bmp = transcode(entry.data, GUID_ContainerFormatBmp); bmp && bmp->size() > sizeof(BITMAPFILEHEADER)) {
              native.emplace_back(CF_DIBV5, bmp->substr(sizeof(BITMAPFILEHEADER)));
            }
            break;
          }
      }
    }
    return native;
  }
}  // namespace

namespace platf::clipboard_sync {
  bool supported() {
    return true;
  }

  std::uint32_t sequence() {
    return GetClipboardSequenceNumber();
  }

  std::uint32_t available_formats() {
    std::uint32_t mask = 0;
    if (IsClipboardFormatAvailable(CF_UNICODETEXT)) {
      mask |= ::clipboard::format_text;
    }
    if (IsClipboardFormatAvailable(cf_html())) {
      mask |= ::clipboard::format_html;
    }
    if (IsClipboardFormatAvailable(cf_rtf())) {
      mask |= ::clipboard::format_rtf;
    }
    if (IsClipboardFormatAvailable(cf_png()) || IsClipboardFormatAvailable(CF_DIBV5) || IsClipboardFormatAvailable(CF_DIB)) {
      mask |= ::clipboard::format_png;
    }
    return mask;
  }

  std::optional<std::vector<item>> read(std::uint32_t formats_mask) {
    clipboard_scope scope;
    if (!scope.open()) {
      return std::nullopt;
    }
    std::vector<item> items;
    if (formats_mask & ::clipboard::format_text) {
      if (auto text = read_text()) {
        items.push_back({item_type::text, std::move(*text)});
      }
    }
    if (formats_mask & ::clipboard::format_html) {
      if (const auto raw = read_global(cf_html())) {
        if (auto fragment = ::clipboard::cf_html_extract_fragment(*raw)) {
          items.push_back({item_type::html, std::move(*fragment)});
        }
      }
    }
    if (formats_mask & ::clipboard::format_rtf) {
      if (auto rtf = read_global(cf_rtf())) {
        auto trimmed = until_nul(std::move(*rtf));
        if (!trimmed.empty()) {
          items.push_back({item_type::rtf, std::move(trimmed)});
        }
      }
    }
    if (formats_mask & ::clipboard::format_png) {
      if (auto png = read_image_as_png()) {
        items.push_back({item_type::png, std::move(*png)});
      }
    }
    return items;
  }

  std::optional<std::uint32_t> write(const std::vector<item> &items) {
    const auto native = to_native(items);
    {
      clipboard_scope scope;
      if (!scope.open() || !EmptyClipboard()) {
        return std::nullopt;
      }
      for (const auto &[format, bytes] : native) {
        set_global(format, bytes);
      }
    }
    return GetClipboardSequenceNumber();
  }
}  // namespace platf::clipboard_sync
