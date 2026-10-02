# Microphone passthrough (Mac Moonlight → Windows Vibepollo) — design + tasks

Status: decisions delegated to Claude by the user on 2026-10-01 ("делай сам и решай сам"; "парсек использует стим микрофон, так шо смотри сам").
Branches: Vibepollo `feat/microphone` (from `feat/clipboard-sync`), moonlight-qt `feat/audio`, moonlight-common-c `feat/microphone` (from `4c1d0d6`, fork remote `fork`).

## Goal
While streaming, the Mac's microphone reaches the host as a Windows microphone, so games, Discord, etc. on the host hear the user. Latency target: under ~100 ms added on a LAN.

## Prior art (research 2026-10-01)
No mic support in Sunshine, Apollo releases or Vibepollo. Forks (Apollo PR #1428, JimothySnicket/moonlight-mic) send Opus 20 ms frames inside the encrypted control stream and render them on the host into a virtual audio device ("Steam Streaming Microphone" or VB-CABLE). We follow the same shape with our own packet type; Artemis wire compatibility is not a goal.

## Wire protocol (Vibepollo protocol extension)
- Client → host control message type **0x3004 "microphone audio"**, present only in moonlight-common-c `packetTypesGen7Enc` (all other generation tables: -1 → not available). This means it is only sent over the encrypted control stream.
- Payload: `u8 version = 1`, `u16 LE seq` (increments per frame, wraps), then one Opus packet (48 kHz, mono, 20 ms = 960 samples). Opus packet ≤ 200 bytes (the encoder is capped), so the whole control message stays under moonlight-common-c's 256-byte `tempBuffer`.
- Sent with ENet flags 0 (unreliable, sequenced) on a new channel `CTRL_CHANNEL_MIC 0x07`: late packets are dropped by ENet, no head-of-line blocking behind reliable traffic. (If the peer negotiated fewer channels, `sendMessageEnet` already falls back to channel 0.)
- Host advertises support in `/serverinfo` as `<Microphone>1</Microphone>` when `mic_passthrough` is enabled and a sink device is available (see host). Clients send only when the flag is present.

## moonlight-common-c
- `Limelight.h`: `int LiSendMicrophoneOpusFrame(const unsigned char* opusData, int length);`
  Returns 0 on success, -2 if the control stream is not connected, -3 if the host/protocol does not support it (packet type -1 or control stream not encrypted), -1 on invalid length (0 or > 200) or allocation failure. Thread-safe (sends serialize on `enetMutex`). Maintains the u16 seq internally (reset when the control stream starts).
- `Limelight-internal.h`: `#define CTRL_CHANNEL_MIC 0x07`.
- ControlStream.c: `IDX_MIC_AUDIO` index + `0x3004` in `packetTypesGen7Enc`, `-1` in every other table (mirror how `IDX_CLIPBOARD_CHANGED` was added).

## Host (Vibepollo)
- Config (audio section, model on `clipboard_sync`/`virtual_sink`): `mic_passthrough` (bool, default **enabled**), `mic_sink` (string, default empty = auto). Docs in docs/configuration.md, locale keys in both `en.json` files (`mic_passthrough`, `mic_passthrough_desc`, `mic_sink`, `mic_sink_desc`). Not restart-required (read when the mic stream opens).
- `src/stream.cpp`: `IDX_MIC_AUDIO` → `0x3004` in `packetTypes`; `server->map(packetTypes[IDX_MIC_AUDIO], ...)`:
  - ignore unless `config::audio.mic_passthrough`;
  - permission: the client must hold at least one input permission (`session->permission & crypto::PERM::_all_inputs`), else debug-log and drop (voice is treated as input; no new PERM bit / web UI change);
  - parse header (version 1, seq), hand `(session id, seq, opus bytes)` to `mic::receiver` — never block the control thread.
- `src/mic/receiver.{h,cpp}` (portable): one active mic stream at a time — the session that sent the most recent packet owns it; packets from another session are dropped until the owner has been silent for 1 s or its session ended (`mic::receiver::session_ended(id)` called from session teardown).
  - Queue packets (mutex + condition variable), worker thread started lazily on the first packet and stopped after 3 s without packets or on shutdown.
  - Jitter handling by seq: drop duplicates and packets older than the last played seq; a gap of 1–2 frames → Opus PLC (`opus_decode_float(dec, NULL, 0, ...)`) per missing frame; a gap > 2 frames → reset decoder state and continue (no PLC burst).
  - Decode with `opus_decoder_create(48000, 1)`; output 960 float samples per frame to a `platf::mic_sink` interface.
- `src/platform/common.h` (or a new `src/platform/mic_sink.h`): `class mic_sink { virtual bool open() = 0; virtual void write(const float* mono48k, size_t frames) = 0; virtual void close() = 0; }`, `std::unique_ptr<mic_sink> platf::create_mic_sink(const std::string& name_or_empty)`, `bool platf::mic_sink_available(const std::string& name_or_empty)`. Non-Windows: stub returning nullptr / false.
- `src/platform/windows/mic_sink.cpp`: WASAPI shared-mode **render** client:
  - Endpoint selection among active `eRender` endpoints using the existing friendly-name helpers in audio.cpp (expose what is needed rather than duplicating): `mic_sink` non-empty → match any field containing it (case-insensitive); empty → first match of (1) "Steam Streaming Microphone", (2) "CABLE Input" (VB-Audio). Never fall back to a real speaker.
  - `IAudioClient::Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 100 ms buffer, 0, WAVEFORMATEXTENSIBLE float32 48 kHz mono, nullptr)`; `IAudioRenderClient` writes; COM via the existing `co_init_t` pattern on the worker thread.
  - Latency control: if `GetCurrentPadding` exceeds 120 ms, drop the incoming frame (catch up); start rendering only after 40 ms are queued (prebuffer) after open/reset.
  - `mic_sink_available` enumerates endpoints; serverinfo caches the answer for 10 s.
- `src/nvhttp.cpp` serverinfo: `<Microphone>1</Microphone>` next to `ClipboardSync` when `config::audio.mic_passthrough && platf::mic_sink_available(config::audio.mic_sink)`.
- Logging: info when the mic stream opens (device name) / closes; warning (rate-limited, once per 60 s) when packets arrive but no sink is available.

## Client (moonlight-qt, macOS first; SDL-based so other desktops work too)
- Preferences: `micPassthrough` (key `micpassthrough`, default **false**), `micDevice` (key `micdevice`, default "" = system default input). Settings → Audio: checkbox "Send microphone to host" + combo "Microphone" (System default + `SystemProperties::getAudioInputDevices()` = SDL capture devices, same pattern/helpers as the output device combo incl. "(not connected)").
- `NvComputer`: `bool microphoneSupported` from `<Microphone>` (parse + `ASSIGN_IF_CHANGED`, like `clipboardSyncVersion`).
- `app/streaming/audio/mic/micframer.{h,cpp}` (pure): accumulates float mono samples of arbitrary chunk sizes and emits exact 960-sample frames.
- `app/streaming/audio/mic/micstreamer.{h,cpp}`: created by `Session` after the connection is established when `micPassthrough && computer->microphoneSupported`; destroyed before `LiStopConnection`.
  - Opens the SDL capture device (`AudioDevice::resolve` over capture names, fallback default) as 48 kHz, `AUDIO_F32SYS`, mono, 960-sample buffer; the SDL capture callback feeds the framer; each full frame is Opus-encoded (`opus_encoder_create(48000, 1, OPUS_APPLICATION_VOIP)`, bitrate 48 kbps, complexity 5, `max_data_bytes` 200) and sent with `LiSendMicrophoneOpusFrame`. -3 → stop the streamer for this session with one log line; -2 → drop silently.
  - Mute toggle hotkey **Ctrl+Alt+Shift+U**: muted = capture device closed (macOS mic indicator off), unmuted = reopened. Starts unmuted. Log the state.
  - Streams regardless of window focus.
- `app/Info.plist`: `NSMicrophoneUsageDescription` = "Moonlight sends your microphone to the host PC when microphone passthrough is enabled in Settings."
- Launch warning when the chosen mic device is not connected (same pattern as the output device).

## Testing
- common-c: compiled via moonlight-qt build; behaviour covered by host/client tests and E2E.
- Host gtest (`tests/unit/test_mic_receiver.cpp`, portable): real Opus encode → packets → receiver with a fake sink: in-order frames decoded (960 samples each); duplicate and stale seq dropped; 1–2 frame gaps produce PLC frames; >2 gap resets; seq wrap 65535→0 is in order; second session's packets ignored while owner active, accepted after owner's `session_ended`. Windows-only gtest for the WASAPI sink: `create_mic_sink("no-such-device-xyz")` returns nullptr/open fails gracefully; `mic_sink_available("no-such-device-xyz") == false`.
- Client QtTest (`tests/audio`): MicFramer (chunks 1, 333, 960, 2000 samples → exact frames, remainder kept); Opus round trip of a 1 kHz sine frame stays ≤ 200 bytes.
- E2E (Claude, on win11-gaming): a debug env var `ML_MIC_TEST_TONE=1` on the client replaces capture with a 440 Hz sine; the host renders into the sink; a user-context recorder on the host (loopback of the sink's render endpoint) confirms a 440 Hz signal. Then the user tests with Discord/Sound Recorder.

## Tasks (execution order; two tracks run in parallel in different repos)
- **M1 common-c** (sonnet): API + packet type + channel. Push `feat/microphone` to `fork`; bump the moonlight-qt submodule on `feat/audio`.
- **M2 host protocol + config** (sonnet): config keys/docs/locales, packet type, handler (permission + parse), `mic::receiver` with fake-sink tests, session teardown hook, serverinfo flag (using `mic_sink_available`, stubbed false until M3).
- **M3 host WASAPI sink** (sonnet): endpoint selection, render client, latency control, availability check + Windows gtests; wire into receiver.
- **M4 client** (sonnet, after M1): prefs + settings UI + NvComputer flag + MicFramer/MicStreamer + hotkey + Info.plist + tone test mode + tests.
- **M5 E2E** (Claude): deploy, tone test, report.
