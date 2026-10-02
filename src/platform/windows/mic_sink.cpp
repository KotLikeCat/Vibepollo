/**
 * @file src/platform/windows/mic_sink.cpp
 * @brief WASAPI shared-mode render sink that plays client microphone audio into a virtual audio device.
 * @note Endpoint matching is implemented here (substring, case-insensitive) rather than shared with audio.cpp,
 *       whose helpers are exact-match, file-local and bound to its global enumerator; audio.cpp stays untouched.
 */
// standard includes
#include <algorithm>
#include <array>
#include <cwctype>
#include <format>
#include <memory>
#include <vector>

// platform includes
#include <WinSock2.h>
#include <Audioclient.h>
#include <functiondiscoverykeys_devpkey.h>
#include <mmdeviceapi.h>
#include <propidl.h>

// local includes
#include "mic_sink_win.h"
#include "PolicyConfig.h"
#include "src/logging.h"
#include "src/platform/mic_sink.h"
#include "src/utility.h"
#include "utf_utils.h"

using namespace std::literals;

namespace {
  template<class T>
  struct com_release {
    void operator()(T *p) const {
      if (p) {
        p->Release();
      }
    }
  };

  template<class T>
  using com_ptr = std::unique_ptr<T, com_release<T>>;

  // Same property keys as audio.cpp (defined locally to avoid duplicate INITGUID symbols).
  const PROPERTYKEY key_friendly_name = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
  const PROPERTYKEY key_description = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 2};
  const PROPERTYKEY key_adapter_name = {{0x026e516e, 0xb814, 0x414b, {0x83, 0xcd, 0x85, 0x6d, 0x6f, 0xef, 0x48, 0x22}}, 2};

  // KSDATAFORMAT_SUBTYPE_IEEE_FLOAT, defined locally to avoid needing INITGUID/ksguid linkage.
  const GUID subtype_ieee_float = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

  constexpr int sample_rate = 48000;
  // Room for the largest adaptive target (120 ms) + drop margin (40 ms) + one packet.
  constexpr REFERENCE_TIME buffer_100ns = 300 * 10000;  // 300 ms

  // PKEY_AudioEngine_DeviceFormat (defined locally, like the keys above).
  const PROPERTYKEY key_device_format = {{0xf19f064d, 0x082c, 0x4e27, {0xbc, 0x73, 0x68, 0x82, 0xa1, 0xbb, 0x8e, 0x4c}}, 0};

  /// Initialises COM for the calling thread for the lifetime of the object.
  class com_scope {
  public:
    com_scope() {
      const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED | COINIT_SPEED_OVER_MEMORY);
      ok_ = SUCCEEDED(hr);  // S_FALSE (already initialised) also needs a matching CoUninitialize
    }

    ~com_scope() {
      if (ok_) {
        CoUninitialize();
      }
    }

    com_scope(const com_scope &) = delete;
    com_scope &operator=(const com_scope &) = delete;

  private:
    bool ok_ = false;
  };

  using platf::mic_win::endpoint_info;
  using platf::mic_win::latency_controller;

  std::wstring lower(std::wstring s) {
    std::ranges::transform(s, s.begin(), [](wchar_t c) {
      return static_cast<wchar_t>(std::towlower(c));
    });
    return s;
  }

  bool contains_ci(const std::wstring &hay, const std::wstring &needle_lower) {
    return !needle_lower.empty() && lower(hay).find(needle_lower) != std::wstring::npos;
  }

  std::wstring prop_string(IPropertyStore *store, const PROPERTYKEY &key) {
    PROPVARIANT v;
    PropVariantInit(&v);
    std::wstring out;
    if (store && SUCCEEDED(store->GetValue(key, &v)) && v.vt == VT_LPWSTR && v.pwszVal) {
      out = v.pwszVal;
    }
    PropVariantClear(&v);
    return out;
  }

  com_ptr<IMMDeviceEnumerator> make_enumerator() {
    IMMDeviceEnumerator *e = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), reinterpret_cast<void **>(&e)))) {
      return nullptr;
    }
    return com_ptr<IMMDeviceEnumerator>(e);
  }

  /// Active render endpoints, with their devices (devices[i] corresponds to infos[i]). Requires COM.
  bool enumerate(IMMDeviceEnumerator *en, std::vector<endpoint_info> &infos, std::vector<com_ptr<IMMDevice>> *devices, EDataFlow flow = eRender) {
    IMMDeviceCollection *raw = nullptr;
    if (FAILED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &raw)) || !raw) {
      return false;
    }
    com_ptr<IMMDeviceCollection> coll(raw);
    UINT count = 0;
    coll->GetCount(&count);
    for (UINT i = 0; i < count; ++i) {
      IMMDevice *d = nullptr;
      if (FAILED(coll->Item(i, &d)) || !d) {
        continue;
      }
      com_ptr<IMMDevice> dev(d);
      endpoint_info info;
      LPWSTR id = nullptr;
      if (SUCCEEDED(dev->GetId(&id)) && id) {
        info.id = id;
        CoTaskMemFree(id);
      }
      IPropertyStore *ps = nullptr;
      if (SUCCEEDED(dev->OpenPropertyStore(STGM_READ, &ps)) && ps) {
        com_ptr<IPropertyStore> store(ps);
        info.friendly = prop_string(store.get(), key_friendly_name);
        info.desc = prop_string(store.get(), key_description);
        info.adapter = prop_string(store.get(), key_adapter_name);
      }
      infos.push_back(std::move(info));
      if (devices) {
        devices->push_back(std::move(dev));
      }
    }
    return true;
  }

  /// Copy of the endpoint's PKEY_AudioEngine_DeviceFormat (WAVEFORMATEX with its extension bytes), empty on failure.
  std::vector<BYTE> device_format(IMMDevice *dev) {
    std::vector<BYTE> out;
    IPropertyStore *ps = nullptr;
    if (!dev || FAILED(dev->OpenPropertyStore(STGM_READ, &ps)) || !ps) {
      return out;
    }
    com_ptr<IPropertyStore> store(ps);
    PROPVARIANT v;
    PropVariantInit(&v);
    if (SUCCEEDED(store->GetValue(key_device_format, &v)) && v.vt == VT_BLOB && v.blob.pBlobData && v.blob.cbSize >= sizeof(WAVEFORMATEX)) {
      const auto *wf = reinterpret_cast<const WAVEFORMATEX *>(v.blob.pBlobData);
      const ULONG need = sizeof(WAVEFORMATEX) + wf->cbSize;
      if (v.blob.cbSize >= need) {
        out.assign(v.blob.pBlobData, v.blob.pBlobData + v.blob.cbSize);
      }
    }
    PropVariantClear(&v);
    return out;
  }

  /**
   * The "Steam Streaming Microphone" driver copies raw bytes from its render endpoint to its capture endpoint without
   * conversion, so a render format different from the capture format changes pitch (and clicks). Align the render
   * endpoint's device format with the paired capture endpoint's before the stream is initialised.
   */
  void align_with_paired_capture(IMMDeviceEnumerator *en, const endpoint_info &render, IMMDevice *render_dev) {
    std::vector<endpoint_info> caps;
    std::vector<com_ptr<IMMDevice>> cap_devs;
    if (!enumerate(en, caps, &cap_devs, eCapture)) {
      return;
    }
    const int ci = platf::mic_win::find_paired_capture(caps, render);
    if (ci < 0) {
      return;
    }
    auto cap_fmt = device_format(cap_devs[ci].get());
    auto render_fmt = device_format(render_dev);
    if (cap_fmt.empty()) {
      return;
    }
    const auto &cf = *reinterpret_cast<const WAVEFORMATEX *>(cap_fmt.data());
    if (!render_fmt.empty() && !platf::mic_win::formats_differ(cf, *reinterpret_cast<const WAVEFORMATEX *>(render_fmt.data()))) {
      return;
    }
    IPolicyConfig *raw = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_CPolicyConfigClient, nullptr, CLSCTX_ALL, IID_IPolicyConfig, reinterpret_cast<void **>(&raw));
    if (FAILED(hr) || !raw) {
      BOOST_LOG(warning) << "mic sink: cannot align '"sv << utf_utils::to_utf8(render.friendly) << "' with '"sv << utf_utils::to_utf8(caps[ci].friendly)
                         << "' (policy config unavailable [0x"sv << util::hex(hr).to_string_view() << "]); microphone pitch will be wrong"sv;
      return;
    }
    com_ptr<IPolicyConfig> policy(raw);
    // Undocumented API: pass copies, never the property-store buffers.
    std::wstring id_copy = render.id;
    std::vector<BYTE> endpoint_fmt = cap_fmt;
    std::vector<BYTE> mix_fmt = cap_fmt;
    hr = policy->SetDeviceFormat(id_copy.c_str(), reinterpret_cast<WAVEFORMATEX *>(endpoint_fmt.data()), reinterpret_cast<WAVEFORMATEX *>(mix_fmt.data()));
    if (FAILED(hr)) {
      BOOST_LOG(warning) << "mic sink: cannot align '"sv << utf_utils::to_utf8(render.friendly) << "' with '"sv << utf_utils::to_utf8(caps[ci].friendly)
                         << "' [0x"sv << util::hex(hr).to_string_view() << "]; microphone pitch will be wrong"sv;
      return;
    }
    BOOST_LOG(info) << "mic sink: aligned '"sv << utf_utils::to_utf8(render.friendly) << "' format to '"sv << utf_utils::to_utf8(caps[ci].friendly) << "' ("sv
                    << platf::mic_win::format_to_string(cf) << ')';
  }

  int select(const std::vector<endpoint_info> &infos, const std::string &name) {
    return platf::mic_win::select_endpoint(infos, name);
  }

  /// Thread affinity: create, open, close and destroy on the same thread (COM is initialised per open).
  class wasapi_mic_sink final: public platf::mic_sink {
  public:
    explicit wasapi_mic_sink(std::string name):
        name_(std::move(name)) {}

    ~wasapi_mic_sink() override {
      close();
    }

    bool open() override {
      close();
      com_.reset(new com_scope());
      // Locals (enumerator, devices) are released when open_impl returns, i.e. before any CoUninitialize in close().
      if (!open_impl()) {
        close();
        return false;
      }
      return true;
    }

    bool open_impl() {

      auto en = make_enumerator();
      if (!en) {
        BOOST_LOG(warning) << "mic sink: cannot create device enumerator"sv;
        return false;
      }
      std::vector<endpoint_info> infos;
      std::vector<com_ptr<IMMDevice>> devices;
      if (!enumerate(en.get(), infos, &devices)) {
        return false;
      }
      const int idx = select(infos, name_);
      if (idx < 0) {
        BOOST_LOG(warning) << "mic sink: no matching virtual microphone endpoint"sv;
        return false;
      }

      warn_if_default_render(en.get(), infos[idx]);
      align_with_paired_capture(en.get(), infos[idx], devices[idx].get());

      IAudioClient *ac = nullptr;
      if (FAILED(devices[idx]->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void **>(&ac))) || !ac) {
        return false;
      }
      client_.reset(ac);

      WAVEFORMATEXTENSIBLE fmt {};
      fmt.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
      fmt.Format.nChannels = 1;
      fmt.Format.nSamplesPerSec = sample_rate;
      fmt.Format.wBitsPerSample = 32;
      fmt.Format.nBlockAlign = 4;
      fmt.Format.nAvgBytesPerSec = sample_rate * 4;
      fmt.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
      fmt.Samples.wValidBitsPerSample = 32;
      fmt.dwChannelMask = SPEAKER_FRONT_CENTER;
      fmt.SubFormat = subtype_ieee_float;

      const HRESULT hr = client_->Initialize(
        AUDCLNT_SHAREMODE_SHARED,
        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY,
        buffer_100ns,
        0,
        &fmt.Format,
        nullptr
      );
      if (FAILED(hr)) {
        BOOST_LOG(warning) << "mic sink: IAudioClient::Initialize failed [0x"sv << util::hex(hr).to_string_view() << ']';
        return false;
      }
      if (FAILED(client_->GetBufferSize(&buffer_frames_))) {
        return false;
      }
      IAudioRenderClient *rc = nullptr;
      if (FAILED(client_->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void **>(&rc))) || !rc) {
        return false;
      }
      render_.reset(rc);

      started_ = false;
      latency_.restart(latency_controller::clock::now());
      // Prebuffer silence so the first real frames do not underrun, then start.
      if (!push_silence(latency_.target_frames())) {
        return false;
      }
      if (FAILED(client_->Start())) {
        return false;
      }
      started_ = true;
      BOOST_LOG(info) << "mic sink: opened endpoint '"sv << utf_utils::to_utf8(infos[idx].friendly) << '\'';
      opened_ = true;
      return true;
    }

    bool write(const float *mono48k, std::size_t frames) override {
      if (!opened_ || !client_ || !render_) {
        return false;
      }
      if (frames == 0) {
        return true;
      }
      UINT32 padding = 0;
      HRESULT hr = client_->GetCurrentPadding(&padding);
      if (FAILED(hr)) {
        handle_failure(hr);
        return false;
      }
      // Adaptive latency: drop above target + 40 ms, re-prebuffer `target` after an underrun (padding == 0).
      const auto old_target_ms = latency_.target_ms();
      const auto act = latency_.on_write(padding, latency_controller::clock::now());
      if (latency_.target_ms() != old_target_ms) {
        const auto now = latency_controller::clock::now();
        if (now - last_target_log_ >= 2s) {
          last_target_log_ = now;
          BOOST_LOG(info) << "mic sink: latency target "sv << old_target_ms << " -> "sv << latency_.target_ms() << " ms"sv;
        }
      }
      if (act.what == latency_controller::kind::drop) {
        return true;
      }
      if (act.what == latency_controller::kind::prebuffer_then_write) {
        const UINT32 pre = act.frames;
        if (pre + frames <= buffer_frames_) {
          if (!push_silence(pre)) {
            handle_failure(E_FAIL);
            return false;
          }
          padding = pre;
        }
      }
      if (padding + frames > buffer_frames_) {
        return true;
      }
      BYTE *data = nullptr;
      hr = render_->GetBuffer(static_cast<UINT32>(frames), &data);
      if (FAILED(hr) || !data) {
        handle_failure(hr);
        return false;
      }
      std::copy_n(mono48k, frames, reinterpret_cast<float *>(data));
      hr = render_->ReleaseBuffer(static_cast<UINT32>(frames), 0);
      if (FAILED(hr)) {
        handle_failure(hr);
        return false;
      }
      return true;
    }

    void close() override {
      if (client_ && started_) {
        client_->Stop();
      }
      const bool was_open = opened_;
      started_ = false;
      opened_ = false;
      render_.reset();
      client_.reset();
      com_.reset();  // after all COM pointers are released
      if (was_open) {
        BOOST_LOG(info) << "mic sink: closed"sv;
      }
    }

  private:
    /// Host audio would be sent into the microphone if the sink endpoint is a default playback device. Warn once.
    void warn_if_default_render(IMMDeviceEnumerator *en, const endpoint_info &sink) {
      if (warned_default_) {
        return;
      }
      for (const auto role : {eConsole, eMultimedia, eCommunications}) {
        IMMDevice *d = nullptr;
        if (FAILED(en->GetDefaultAudioEndpoint(eRender, role, &d)) || !d) {
          continue;
        }
        com_ptr<IMMDevice> dev(d);
        LPWSTR id = nullptr;
        if (SUCCEEDED(dev->GetId(&id)) && id) {
          const bool same = sink.id == id;
          CoTaskMemFree(id);
          if (same) {
            warned_default_ = true;
            BOOST_LOG(warning) << "mic sink: '"sv << utf_utils::to_utf8(sink.friendly)
                               << "' is the default playback device; host audio will be sent into the microphone"sv;
            return;
          }
        }
      }
    }

    bool push_silence(UINT32 frames) {
      BYTE *data = nullptr;
      if (FAILED(render_->GetBuffer(frames, &data)) || !data) {
        return false;
      }
      return SUCCEEDED(render_->ReleaseBuffer(frames, AUDCLNT_BUFFERFLAGS_SILENT));
    }

    /// Device removed or otherwise unusable: stop accepting data so the receiver reopens later.
    void handle_failure(HRESULT hr) {
      if (hr == AUDCLNT_E_DEVICE_INVALIDATED) {
        BOOST_LOG(warning) << "mic sink: device invalidated"sv;
      } else {
        BOOST_LOG(warning) << "mic sink: render failure [0x"sv << util::hex(hr).to_string_view() << ']';
      }
      close();
    }

    std::string name_;
    std::unique_ptr<com_scope> com_;
    com_ptr<IAudioClient> client_;
    com_ptr<IAudioRenderClient> render_;
    UINT32 buffer_frames_ = 0;
    bool started_ = false;
    bool opened_ = false;
    bool warned_default_ = false;
    latency_controller latency_ {sample_rate};
    latency_controller::clock::time_point last_target_log_ {};
  };
}  // namespace

namespace platf::mic_win {
  int select_endpoint(const std::vector<endpoint_info> &infos, const std::string &name) {
    if (!name.empty()) {
      const auto needle = lower(utf_utils::from_utf8(name));
      for (size_t i = 0; i < infos.size(); ++i) {
        const auto &e = infos[i];
        if (contains_ci(e.id, needle) || contains_ci(e.friendly, needle) || contains_ci(e.desc, needle) || contains_ci(e.adapter, needle)) {
          return static_cast<int>(i);
        }
      }
      return -1;
    }
    for (const auto *virt : {L"steam streaming microphone", L"cable input"}) {
      const std::wstring needle = virt;
      for (size_t i = 0; i < infos.size(); ++i) {
        const auto &e = infos[i];
        if (contains_ci(e.friendly, needle) || contains_ci(e.desc, needle) || contains_ci(e.adapter, needle)) {
          return static_cast<int>(i);
        }
      }
    }
    return -1;
  }

  int find_paired_capture(const std::vector<endpoint_info> &captures, const endpoint_info &render) {
    if (render.adapter.empty()) {
      return -1;
    }
    const auto needle = lower(render.adapter);
    for (size_t i = 0; i < captures.size(); ++i) {
      if (lower(captures[i].adapter) == needle) {
        return static_cast<int>(i);
      }
    }
    return -1;
  }

  namespace {
    /// Sample type: the EXTENSIBLE sub-format's leading word, otherwise the format tag.
    WORD effective_tag(const WAVEFORMATEX &f) {
      if (f.wFormatTag == WAVE_FORMAT_EXTENSIBLE && f.cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX)) {
        return static_cast<WORD>(reinterpret_cast<const WAVEFORMATEXTENSIBLE &>(f).SubFormat.Data1);
      }
      return f.wFormatTag;
    }
  }  // namespace

  bool formats_differ(const WAVEFORMATEX &a, const WAVEFORMATEX &b) {
    return a.nChannels != b.nChannels || a.nSamplesPerSec != b.nSamplesPerSec || a.wBitsPerSample != b.wBitsPerSample || effective_tag(a) != effective_tag(b);
  }

  std::string format_to_string(const WAVEFORMATEX &f) {
    const auto tag = effective_tag(f);
    return std::format("{}ch {} Hz {}-bit {}", f.nChannels, f.nSamplesPerSec, f.wBitsPerSample, tag == WAVE_FORMAT_IEEE_FLOAT ? "float" : tag == WAVE_FORMAT_PCM ? "PCM" : "other");
  }

  latency_controller::latency_controller(std::uint32_t sample_rate):
      rate_(sample_rate),
      target_(static_cast<std::uint32_t>(static_cast<std::uint64_t>(sample_rate) * min_ms / 1000)) {}

  void latency_controller::restart(clock::time_point now) {
    dropping_ = false;
    last_event_ = now;
    has_event_ = true;
  }

  latency_controller::action latency_controller::on_write(std::uint32_t padding, clock::time_point now) {
    if (padding == 0) {
      // Underrun: be more conservative from now on and rebuild the buffer.
      target_ = std::min(target_ + frames_of(raise_step_ms), frames_of(max_ms));
      last_event_ = now;
      has_event_ = true;
      dropping_ = false;
      return {kind::prebuffer_then_write, target_};
    }
    if (!has_event_) {
      has_event_ = true;
      last_event_ = now;
    }
    if (now - last_event_ >= calm_period) {
      const auto floor = frames_of(min_ms);
      const auto step = frames_of(lower_step_ms);
      target_ = target_ > floor + step ? target_ - step : floor;
      last_event_ = now;
    }
    if (padding > drop_frames()) {
      dropping_ = true;
    } else if (padding < target_) {
      dropping_ = false;
    }
    return {dropping_ ? kind::drop : kind::write, 0};
  }

  bool is_mic_endpoint(const std::vector<endpoint_info> &infos, const std::string &name, const std::wstring &id) {
    if (id.empty()) {
      return false;
    }
    const int idx = select_endpoint(infos, name);
    return idx >= 0 && infos[idx].id == id;
  }

  bool is_mic_endpoint_id(const std::string &name, const std::wstring &id) {
    auto en = make_enumerator();
    if (!en) {
      return false;
    }
    std::vector<endpoint_info> infos;
    if (!enumerate(en.get(), infos, nullptr)) {
      return false;
    }
    return is_mic_endpoint(infos, name, id);
  }

  std::optional<std::string> resolve_endpoint_name(const std::string &name_or_empty, bool *audio_present) {
    if (audio_present) {
      *audio_present = false;
    }
    com_scope com;
    auto en = make_enumerator();
    if (!en) {
      return std::nullopt;
    }
    std::vector<endpoint_info> infos;
    if (!enumerate(en.get(), infos, nullptr)) {
      return std::nullopt;
    }
    if (audio_present) {
      *audio_present = !infos.empty();
    }
    const int idx = select(infos, name_or_empty);
    if (idx < 0) {
      return std::nullopt;
    }
    return utf_utils::to_utf8(infos[idx].friendly);
  }
}  // namespace platf::mic_win

namespace platf {
  std::unique_ptr<mic_sink> create_mic_sink(const std::string &name_or_empty) {
    return std::make_unique<wasapi_mic_sink>(name_or_empty);
  }

  bool mic_sink_available(const std::string &name_or_empty) {
    return mic_win::resolve_endpoint_name(name_or_empty).has_value();
  }
}  // namespace platf
