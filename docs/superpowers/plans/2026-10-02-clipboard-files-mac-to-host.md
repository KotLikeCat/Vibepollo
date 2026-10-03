# Clipboard files (Mac → Windows host) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Copy files/folders in macOS Finder and paste them on the Windows host — lazily (RDP-like) in Explorer for any size, and as real files in apps for small sets.

**Architecture:** moonlight-qt posts a file manifest ("offer") when the stream window gains focus. Vibepollo hands it to a resident user-session agent (`sunshine_clipboard_agent.exe`) that puts an OLE data object (virtual files + `CF_HDROP` for prefetched small offers) on the Windows clipboard. Reads become 4 MiB range requests: agent → sunshine.exe (pipe) → client (control packet 0x3005) → client reads disk → HTTPS POST chunk → sunshine.exe → agent.

**Tech Stack:** C++23 (Vibepollo, MinGW UCRT64, gtest via `scripts/dev/wintest.sh` on build-win), Win32 OLE/COM, Qt 6 + SDL2 (moonlight-qt, QtTest), C (moonlight-common-c, ENet control stream).

**Spec:** `docs/superpowers/specs/2026-10-02-clipboard-files-mac-to-host-design.md` (read it first; it is the authority).

## Global Constraints
- Branch `feat/clipboard-files` in all three repos (Vibepollo from master 6316861d; moonlight-qt from master ea5a7bae; moonlight-common-c from master a8eb510).
- Wire: MLCF v1 manifest (`"MLCF"`, `u8 1`, `u8 offer_id[16]`, `u32 count`, entries `u8 kind(1 file,2 dir) u64 size i64 mtime_ms u16 path_len path(UTF-8 NFC, '/')`), little-endian.
- Limits: entries 1…100000; path ≤ 1024 bytes; component ≤ 255 UTF-16 units; manifest body ≤ 32 MiB.
- Control packet `0x3005` host→client, encrypted control stream only (`packetTypesGen7Enc`), payload `u8 1, u8 offer_id[16], u32 request_id, u32 file_index, u64 offset, u32 length` (1…4 MiB), never coalesced.
- HTTPS: `POST /actions/clipboard?type=files`; `POST /actions/clipboard?type=file-chunk&offer=<32 hex>&req=<id>&file=<idx>&offset=<n>`; error header `X-Clipboard-Error: gone|changed|io`; host replies 410 for unknown/late chunks; body limit `max(clipboard_max_bytes, 32 MiB) + 64 KiB`.
- Permissions: `clipboard_set` + `file_upload` + active session for both POSTs.
- Scheduling: chunk 4 MiB, ≤ 4 outstanding per offer, timeout 15 s, one retry; agent read-ahead 16 MiB.
- Config: `clipboard_files` (bool, default true), `clipboard_files_prefetch_bytes` (default 268435456, 0 disables, max 4294967296); both restart-required; `<ClipboardFiles>1</ClipboardFiles>` in serverinfo when `clipboard_sync && clipboard_files` and the agent is connected.
- Performance: ≥ 80 MB/s on gigabit; kept-alive TLS connections for chunk POSTs; no blocking work on the HTTPS io thread.
- Never `git add -A` (Vibepollo `.superpowers/` is untracked scratch); never stage moonlight-qt `build*/` dirs; never kill processes by name.
- Commit trailer (every commit):
  ```
  Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>
  Claude-Session: https://claude.ai/code/session_011K1ZBptPZQaLN6y3u9cCM5
  ```
- Host build/test: commit, then `scripts/dev/wintest.sh '<regex with \|>' <targets…>` (Bash timeout 1200000). moonlight-qt: `cd build && PATH=/opt/homebrew/opt/qt/bin:$PATH make -j10`; after editing any header run `make clean` in `build/app` first (stale objects caused phantom bugs). After moonlight-common-c changes delete `build/app/Moonlight.app/Contents/MacOS/Moonlight` to force a relink.

## Review Focus
1. Folder with thousands of small files pasted in Explorer — expect correct tree, no per-file stall (read-ahead must not wait 15 s per tiny file) → H2 test `ManySmallFilesDoNotSerialize`, H4 test `ShellCopiesFolderTree`.
2. Source file modified/deleted on the Mac after copy — expect a clean Explorer error, not hang or corrupted file → C3 test `ChangedFileReportsChanged`, H2 test `ChunkErrorFailsRead`.
3. Names illegal on Windows (`CON.txt`, `a:b?`, trailing dot) and NFD Cyrillic from APFS — expect sanitized, NFC, no collisions → H1 tests `SanitizerCases`, C2 test `NormalizesToNfc`.
4. Client disconnects mid-paste / stream ends — expect reads fail within ~30 s, agent stays alive, temp files cleaned → H2 test `TimeoutRetriesOnceThenFails`, H5 test `SessionEndFailsOfferReads`.
5. A new Finder copy while an Explorer paste of the previous offer is running — expect the running paste fails with "gone" (offer replaced) rather than mixing files → H2 test `NewOfferFailsPendingReads`, C3 test `StaleOfferReportsGone`.

## File map
Host (Vibepollo):
- `src/clipboard/files/manifest.{h,cpp}` — MLCF codec, validation, Windows sanitizer (H1)
- `src/clipboard/files/transfer.{h,cpp}` — portable range scheduler (H2)
- `src/clipboard/files/agent_protocol.{h,cpp}` — portable pipe message codec (H3)
- `src/platform/windows/clipboard_agent_host.{h,cpp}` — launch/supervise agent, pipe I/O (H3)
- `tools/clipboard_agent/{main.cpp, data_object.{h,cpp}, file_stream.{h,cpp}, prefetch.{h,cpp}, range_source.h, pipe_range_source.{h,cpp}}` — the agent exe (H4)
- `src/clipboard/files/service.{h,cpp}` — glues nvhttp ↔ transfer ↔ agent ↔ stream (H5)
- `src/nvhttp.cpp`, `src/stream.{h,cpp}`, `src/config.{h,cpp}`, `src/confighttp.cpp`, `docs/configuration.md`, both `en.json` locales, `cmake/*`, `tests/CMakeLists.txt`, `.github/workflows/build-windows-selfhosted.yml` (H3/H5)
- Tests: `tests/unit/test_clipboard_files_manifest.cpp`, `test_clipboard_files_transfer.cpp`, `test_clipboard_files_protocol.cpp`, `tests/unit/platform/windows/test_clipboard_agent.cpp`, fixture `tests/fixtures/clipboard_files/vectors.txt`
Client:
- moonlight-common-c `src/{Limelight.h, ControlStream.c, FakeCallbacks.c}` (C1)
- moonlight-qt `app/streaming/clipboard/files/{manifestbuilder,filecodec,fileserver}.{h,cpp}` (C2, C3), `macpasteboard.{h,mm}`, `clipboardsync.{h,cpp}`, `app/backend/{nvhttp,nvcomputer}.{h,cpp}`, `app/streaming/session.{h,cpp}`, `app/app.pro`, `tests/clipboard/*` (C2, C3)

## Shared MLCF vectors
`tests/fixtures/clipboard_files/vectors.txt` (host) and `tests/clipboard/files_vectors.txt` (client), identical content, format `name hex`; offer_id = bytes 00..0f:
```
single_file 4d4c434601000102030405060708090a0b0c0d0e0f010000000105000000000000000068e5cf8b0100000500612e747874
cyrillic_tree 4d4c434601000102030405060708090a0b0c0d0e0f030000000200000000000000000068e5cf8b0100000a00d09fd0b0d0bfd0bad0b00100001000000000007b68e5cf8b0100001a00d09fd0b0d0bfd0bad0b02fd184d0b0d0b9d0bb20d0b92e74787401000000000000000000000000000000000500622e62696e
over_4gib 4d4c434601000102030405060708090a0b0c0d0e0f010000000100f2052a01000000010000000000000007006269672e69736f
```
Semantics: `single_file` = [file "a.txt" 5 B mtime 1700000000000]; `cyrillic_tree` = [dir "Папка" mtime 1700000000000, file "Папка/файл й.txt" 1048576 B mtime 1700000000123, file "b.bin" 0 B mtime 0]; `over_4gib` = [file "big.iso" 5000000000 B mtime 1]. An offer with 0 entries (`…0f00000000`) must be rejected (`empty`).

---

### Task H1 (sonnet): Host manifest codec, validation, Windows sanitizer

**Files:** Create `src/clipboard/files/manifest.{h,cpp}`, `tests/unit/test_clipboard_files_manifest.cpp`, `tests/fixtures/clipboard_files/vectors.txt`; Modify `cmake/compile_definitions/common.cmake` (or wherever `src/clipboard/bundle.cpp` is listed — mirror it), `tests/CMakeLists.txt` (register `test_component_clipboard_files_manifest` like `test_component_clipboard_bundle`).

**Interfaces — Produces:**
```cpp
namespace clipboard::files {
  using offer_id_t = std::array<std::uint8_t, 16>;
  enum class entry_kind : std::uint8_t { file = 1, directory = 2 };
  struct entry { entry_kind kind; std::uint64_t size; std::int64_t mtime_ms; std::string path; };  // UTF-8, '/'
  struct manifest { offer_id_t offer_id {}; std::vector<entry> entries; };
  enum class manifest_error { none, bad_magic, bad_version, truncated, empty, too_many_entries, path_too_long,
                              component_too_long, bad_component, absolute_path, backslash, nul, missing_parent,
                              duplicate_path, trailing_bytes };
  constexpr std::size_t max_entries = 100000, max_path_bytes = 1024, max_component_utf16 = 255, max_manifest_bytes = 32u << 20;
  struct decode_result { manifest value; manifest_error error = manifest_error::none; };
  decode_result decode_manifest(std::string_view data);
  std::string encode_manifest(const manifest &m);
  std::uint64_t total_size(const manifest &m);
  std::vector<std::string> sanitize_for_windows(const std::vector<entry> &entries);  // index-aligned, '\\' separators, UTF-8
  std::string offer_id_hex(const offer_id_t &id);                // 32 lowercase hex chars
  std::optional<offer_id_t> parse_offer_id_hex(std::string_view hex);
  const char *error_name(manifest_error e);
}
```

- [ ] **Step 1: Write the failing tests** in `tests/unit/test_clipboard_files_manifest.cpp` (gtest, portable):
  - `DecodesSharedVectors`: read `vectors.txt` (path via the same mechanism the bundle vectors test uses), decode each, check entries per the semantics above, re-encode equals input.
  - `RejectsEmptyOffer` (count 0 → `empty`), `RejectsBadMagic`, `RejectsBadVersion`, `RejectsTruncatedEntry` (cut the `single_file` vector by 1 byte → `truncated`), `RejectsTrailingBytes` (append 1 byte).
  - `RejectsPathRules`: build manifests with paths `"/a"`→`absolute_path`, `"a\\b"`→`backslash`, `"a/../b"`→`bad_component`, `"a//b"`→`bad_component`, `"."`→`bad_component`, path containing `\0`→`nul`, file `"x/y"` without dir `"x"`→`missing_parent`, `"A.txt"`+`"a.txt"`→`duplicate_path`, 1025-byte path→`path_too_long`, a 256-char component→`component_too_long`.
  - `RejectsTooManyEntries`: header claiming 100001 entries with a short body → `too_many_entries` (checked before reading entries).
  - `SanitizerCases`: `{"CON.txt"→"CON_.txt", "nul"→"nul_", "com1.log"→"com1_.log", "a:b?.txt"→"a_b_.txt", "name. "→"name", "Папка/файл й.txt"→"Папка\\файл й.txt"}`; collision: entries `"a?.txt"` and `"a_.txt"` → `"a_.txt"`, `"a_ (2).txt"`; a sanitized directory name propagates to its children's paths.
  - `TotalSizeAndHex`: total of `cyrillic_tree` = 1048576; `offer_id_hex` of 00..0f = `"000102030405060708090a0b0c0d0e0f"`, round-trips via `parse_offer_id_hex`; bad hex → nullopt.
- [ ] **Step 2:** Commit the tests; run `scripts/dev/wintest.sh 'clipboard_files_manifest' test_component_clipboard_files_manifest` → expect build failure (missing header).
- [ ] **Step 3: Implement** `manifest.cpp`: bounds-checked little-endian reader; validate `entry_count` (0 → empty, > max → too_many_entries) before the loop; per entry validate kind ∈ {1,2}, path rules, UTF-16 length per component (count code units of the UTF-8 → UTF-16 conversion), parent dirs (set of seen directory paths, case-insensitive via ASCII+Unicode simple lowercase — use `std::towlower` on the UTF-16 string), duplicates (case-insensitive set). Sanitizer: per component replace `<>:"|?*` and chars < 0x20 with `_`, strip trailing `.`/space (if that empties the name use `_`), reserved base names (case-insensitive `CON PRN AUX NUL COM1-9 LPT1-9`, base = text before the first dot) get `_` appended to the base; resolve collisions per parent directory case-insensitively by inserting ` (n)` before the last extension, n = 2,3,…; children of a renamed directory use the renamed directory.
- [ ] **Step 4:** Run wintest again → all tests pass; `sunshine` target still builds (`scripts/dev/wintest.sh 'clipboard_files_manifest\|clipboard_bundle' sunshine test_component_clipboard_files_manifest test_component_clipboard_bundle`).
- [ ] **Step 5:** Commit `feat(clipboard-files): MLCF manifest codec, validation and Windows name sanitizer`.

### Task H2 (sonnet): Host transfer scheduler (portable)

**Files:** Create `src/clipboard/files/transfer.{h,cpp}`, `tests/unit/test_clipboard_files_transfer.cpp`; Modify cmake source list, `tests/CMakeLists.txt` (`test_component_clipboard_files_transfer`).

**Interfaces — Consumes:** `clipboard::files::{offer_id_t, manifest, entry}` (H1). **Produces:**
```cpp
namespace clipboard::files {
  enum class read_error : std::uint8_t { gone = 1, changed = 2, io = 3, timeout = 4 };
  struct chunk_request { offer_id_t offer_id; std::uint32_t request_id; std::uint32_t file_index; std::uint64_t offset; std::uint32_t length; };
  struct transfer_options { std::uint32_t chunk_bytes = 4u << 20; std::size_t max_outstanding = 4; std::chrono::milliseconds timeout {15000}; int retries = 1; };
  class transfer {
  public:
    struct callbacks {
      std::function<void(const chunk_request &)> send_request;                       // to the client (control 0x3005)
      std::function<void(std::uint32_t read_id, std::string data, bool last)> deliver; // to the agent, in offset order
      std::function<void(std::uint32_t read_id, read_error)> fail;
    };
    transfer(callbacks cb, transfer_options opt = {});
    void set_offer(const manifest &m);              // replaces; pending reads of the old offer fail with gone
    void clear_offer();                             // pending reads fail with gone
    bool start_read(std::uint32_t read_id, std::uint32_t file_index, std::uint64_t offset, std::uint64_t length); // false if no offer / bad index / range beyond size
    void cancel_read(std::uint32_t read_id);
    bool on_chunk(const offer_id_t &offer, std::uint32_t request_id, std::uint32_t file_index, std::uint64_t offset, std::string data); // false → HTTP 410
    bool on_chunk_error(const offer_id_t &offer, std::uint32_t request_id, read_error err);
    void tick(std::chrono::steady_clock::time_point now);  // timeouts + retry
    std::optional<offer_id_t> current_offer() const;
  };
}
```
Rules: one outstanding-request budget per offer shared by all reads (round-robin between reads so a big file does not starve small files); each read is split into chunks of `chunk_bytes`; chunks are delivered to `deliver` strictly in offset order per read (buffer out-of-order arrivals); `last=true` on the final piece; a short chunk (fewer bytes than requested) is accepted only if it ends exactly at the file size, otherwise it is an `io` error; zero-length reads complete immediately with `deliver(read_id, "", true)`; callbacks are invoked without the internal mutex held; thread-safe.

- [ ] **Step 1: Write failing tests** (fake callbacks recording calls, a manual clock passed to `tick`): `SplitsIntoChunksWithinOutstandingLimit` (12 MiB read → 3 requests issued immediately with lengths 4 MiB; a 20 MiB read → 4 issued, the 5th after one completes), `DeliversInOffsetOrder` (answer chunk 2 before chunk 1 → deliver order 1,2), `ManySmallFilesDoNotSerialize` (100 reads of 1 KiB files → first 4 requests issued at once, each completion issues the next; total 100 requests, no tick needed), `ChunkErrorFailsRead` (`changed` → `fail(read, changed)`, remaining requests of that read cancelled), `TimeoutRetriesOnceThenFails` (no answer → after 15 s tick a retry with a new request_id; after another 15 s → `fail(timeout)`), `LateChunkIsGone` (`on_chunk` for an unknown request → false), `NewOfferFailsPendingReads` (`set_offer` → pending `fail(gone)`; chunks of the old offer → false), `CancelReadStopsRequests`, `ShortChunkOnlyAtEof`, `ZeroLengthRead`, `RangeBeyondSizeRejected`.
- [ ] **Step 2:** Commit tests, wintest → build fails.
- [ ] **Step 3: Implement** (single mutex; per-read state: file, next_offset_to_request, next_offset_to_deliver, map<offset, data> buffered, set of outstanding request ids; global `std::deque<read_id>` for round-robin issuing; request map request_id → {read_id, offset, length, deadline, attempts}).
- [ ] **Step 4:** wintest `'clipboard_files'` targets `test_component_clipboard_files_manifest test_component_clipboard_files_transfer` → pass.
- [ ] **Step 5:** Commit `feat(clipboard-files): range transfer scheduler`.

### Task H3 (sonnet): Agent protocol codec, agent supervision, config, serverinfo flag

**Files:** Create `src/clipboard/files/agent_protocol.{h,cpp}`, `src/platform/windows/clipboard_agent_host.{h,cpp}`, `tests/unit/test_clipboard_files_protocol.cpp`; Modify `src/config.{h,cpp}` (keys next to `clipboard_sync`), `src/confighttp.cpp` (both `restart_required_keys` lists), `docs/configuration.md`, both `en.json` locales + web UI schema (mirror `clipboard_sync` exactly), `src/nvhttp.cpp` (serverinfo `<ClipboardFiles>`), `src/main.cpp` (start/stop the agent host next to `clipboard::watcher`), cmake, `tests/CMakeLists.txt`.

**Interfaces — Produces:**
```cpp
namespace clipboard::files::agent {
  enum class msg : std::uint8_t { hello = 1, set_offer_part = 2, clear_offer = 3, read_range = 4, range_data = 5,
                                  range_error = 6, cancel_read = 7, clipboard_set = 8, offer_dropped = 9, ping = 10, pong = 11 };
  constexpr std::uint32_t protocol_version = 1;
  constexpr std::size_t max_frame_payload = 1u << 20;   // data frames carry ≤ 1 MiB
  // set_offer_part: offer_id[16], u8 prefetch, u8 last, bytes (a slice of the MLCF encoding of the SANITISED manifest)
  // clear_offer: offer_id[16]
  // read_range (agent→core): u32 read_id, u32 file_index, u64 offset, u64 length
  // range_data (core→agent): u32 read_id, u8 last, bytes
  // range_error (core→agent): u32 read_id, u8 read_error
  // cancel_read (agent→core): u32 read_id
  // clipboard_set (agent→core): u32 sequence_number
  // offer_dropped (agent→core): offer_id[16]
  // hello (agent→core): u32 protocol_version, u32 pid; ping/pong: u32 nonce
  struct message { msg type; std::string payload; };
  std::string encode(msg type, std::string_view payload);           // u8 type + payload
  std::optional<message> decode(std::string_view frame);
  // typed helpers (encode_*/decode_*) for each message above, little-endian
  std::vector<std::string> split_offer(const offer_id_t &id, bool prefetch, std::string_view mlcf);  // set_offer_part frames
  class offer_assembler { public: std::optional<std::tuple<offer_id_t, bool, std::string>> add(std::string_view set_offer_part_payload); };
}
namespace platf::clipboard_agent {
  // sunshine.exe side; Windows implementation in clipboard_agent_host.cpp, stub elsewhere
  bool start(std::function<void(const clipboard::files::agent::message &)> on_message);  // launches tools\sunshine_clipboard_agent.exe as the user, connects
  void stop();
  bool connected();
  bool send(const std::string &frame);   // thread-safe, non-blocking enqueue
}
```
Supervision mirrors `src/platform/windows/display_helper_integration.cpp` (ProcessHandler with use_job=false, `SelfHealingPipe` over `FramedPipeFactory(AnonymousPipeFactory)` then Named fallback, ping every 5 s, restart with cooldown, relaunch when the active console user changes). The agent receives the pipe name/GUID on its command line like `sunshine_wgc_capture`.

- [ ] **Step 1:** Tests for the codec: round-trip of every message type; `decode` rejects empty frames and unknown types; `split_offer` of a 3 MiB payload yields 3 frames with `last` only on the final one and `offer_assembler` reassembles exactly; interleaved offers (new offer id mid-stream) restart assembly.
- [ ] **Step 2:** Commit, wintest → fail. **Step 3:** Implement codec + config keys/docs/locales/schema + restart keys + serverinfo flag (`platf::clipboard_agent::connected()`) + `clipboard_agent_host` (launch/supervise; on_message dispatch is a no-op stub until H5) + main.cpp start/stop. For this task the agent exe does not exist yet: `start` must log once and keep retrying with cooldown without crashing.
- [ ] **Step 4:** wintest `'clipboard_files\|clipboard_bundle'` targets `sunshine test_component_clipboard_files_protocol test_component_clipboard_files_manifest` → pass.
- [ ] **Step 5:** Commit `feat(clipboard-files): agent protocol, agent supervision and config`.

### Task H4 (sonnet): The agent exe (OLE data object, streams, prefetch)

**Files:** Create `tools/clipboard_agent/{main.cpp,range_source.h,data_object.h,data_object.cpp,file_stream.h,file_stream.cpp,prefetch.h,prefetch.cpp,pipe_range_source.h,pipe_range_source.cpp}`, `tests/unit/platform/windows/test_clipboard_agent.cpp`; Modify `tools/CMakeLists.txt` (target `sunshine_clipboard_agent`, link `ole32 oleaut32 uuid shell32 shlwapi` + the H1/H3 sources + `TOOL_SOURCES`; disable Boost.Log output like the clipboard helper), `cmake/packaging/windows.cmake` + `cmake/targets/windows.cmake` (ship it in `tools\` exactly like `sunshine_clipboard_helper`), `tests/CMakeLists.txt` (`test_component_clipboard_agent`, Windows-only, SKIP_REGULAR_EXPRESSION), CI workflow target list.

**Interfaces — Consumes:** H1 manifest + sanitizer, H3 protocol. **Produces:**
```cpp
namespace clipboard_agent {
  struct read_result { bool ok; clipboard::files::read_error error; };
  class range_source {   // blocking; called from RPC (MTA) threads
  public:
    virtual ~range_source() = default;
    virtual read_result read(std::uint32_t file_index, std::uint64_t offset, std::uint32_t length, std::string &out) = 0;
  };
  struct offer { clipboard::files::offer_id_t id; std::vector<clipboard::files::entry> entries; std::vector<std::string> windows_paths; bool prefetch; };
  // Creates the IDataObject (refcount 1). It owns a prefetcher when offer.prefetch.
  IDataObject *create_data_object(offer o, std::shared_ptr<range_source> src, std::filesystem::path prefetch_root);
}
```
Data object rules (spec "Host components" → agent): formats `CFSTR_FILEDESCRIPTORW` (HGLOBAL `FILEGROUPDESCRIPTORW` with one `FILEDESCRIPTORW` per entry, `cFileName` = windows path relative to the offer root, flags `FD_ATTRIBUTES|FD_FILESIZE|FD_WRITESTIME|FD_PROGRESSUI|FD_UNICODE`, dirs `FILE_ATTRIBUTE_DIRECTORY`, mtime ms → FILETIME), `CFSTR_FILECONTENTS` (TYMED_ISTREAM, `lindex` = entry index; directories → `DV_E_LINDEX`), `CFSTR_PREFERREDDROPEFFECT` (DWORD `DROPEFFECT_COPY`), and `CF_HDROP` only when `offer.prefetch` (waits ≤ 120 s for the prefetcher, then returns `DROPFILES` of the top-level prefetched paths; `E_FAIL` on prefetch failure). `QueryGetData`/`EnumFormatEtc` consistent with that list; `SetData` → `E_NOTIMPL` except accept `CFSTR_PERFORMEDDROPEFFECT`/`CFSTR_PASTESUCCEEDED` (store, `S_OK`).
`IStream` (`file_stream`): aggregates the free-threaded marshaler (`CoCreateFreeThreadedMarshaler`), `Read` serves from an internal read-ahead window (≤ 16 MiB, issuing reads of 4 MiB to `range_source` from a worker thread), `Seek` (resets read-ahead), `Stat` (size, name, mtime), `Clone` → `E_NOTIMPL`, errors → `STG_E_READFAULT`. Prefetched files are served from disk.
Prefetcher: background thread; creates the tree under `prefetch_root / offer_id_hex`; reads every file through `range_source` in 4 MiB pieces; signals completion/failure; deletes its folder on destruction.
`main.cpp`: parse pipe GUID; connect (`AnonymousPipeFactory` client like `sunshine_wgc_capture`); send `hello`; on `set_offer_part` assemble → build `offer` (paths from the sanitised manifest) → `OleSetClipboard(create_data_object(...))` on the STA thread → send `clipboard_set{GetClipboardSequenceNumber()}`; on `clear_offer` → `OleSetClipboard(nullptr)` only if our object is still the clipboard owner (`OleIsCurrentClipboard`); when another app replaces the clipboard (`AddClipboardFormatListener` + `OleIsCurrentClipboard == S_FALSE`) → `offer_dropped`; `pipe_range_source` turns `read` into `read_range` frames and waits for `range_data`/`range_error` (per read_id event; `cancel_read` when the stream is released mid-read); `ping` → `pong`; on startup delete stale folders under `%LOCALAPPDATA%\Temp\Vibepollo\clipboard`; exit when the pipe breaks permanently. Never write to stdout/stderr.

- [ ] **Step 1: Write failing Windows tests** (in-process, fake `range_source` that serves deterministic bytes `byte(i) = (file_index*31 + i) & 0xff` and can inject errors/delays):
  - `DescriptorListsEntries`: `cyrillic_tree` offer → `GetData(CFSTR_FILEDESCRIPTORW)` → 3 descriptors, names `Папка`, `Папка\файл й.txt`, `b.bin`, directory attribute on the first, sizes right.
  - `FileContentsStreamsBytes`: read entry 1 fully through `IStream::Read` with odd buffer sizes (1, 4095, 1 MiB) and compare; `Stat` size matches; `Seek` to middle then read.
  - `DirectoryContentsRejected` (`lindex` 0 → `DV_E_LINDEX`).
  - `ErrorBecomesReadFault`: fake returns `changed` → `Read` returns `STG_E_READFAULT`.
  - `HdropAfterPrefetch`: small offer with prefetch → `GetData(CF_HDROP)` returns paths under the temp root with correct contents; large offer (prefetch=false) → `CF_HDROP` not offered (`QueryGetData` = `DV_E_FORMATETC`).
  - `ShellCopiesFolderTree`: `OleSetClipboard` the object, then paste into a temp dir with `IFileOperation`/`SHFileOperation` from `CF_HDROP`… (for virtual files use the shell: `SHCreateDataObject`-free path — call `IFileOperation::CopyItems` with the data object as source if supported; if the shell API path is impractical in a headless test, document it and cover with E2E) — 50 small files in nested folders arrive intact. Mark `GTEST_SKIP` if OLE clipboard is unavailable (no interactive session).
- [ ] **Step 2:** Commit, wintest → fail. **Step 3:** Implement. **Step 4:** wintest `'clipboard'` targets `sunshine sunshine_clipboard_agent test_component_clipboard_agent test_component_clipboard_files_manifest test_component_clipboard_files_protocol test_component_clipboard_files_transfer` → pass/skip (no failures). **Step 5:** Commit `feat(clipboard-files): user-session clipboard agent with virtual files`.

### Task H5 (sonnet): Host wiring — endpoints, control packet, service

**Files:** Create `src/clipboard/files/service.{h,cpp}`; Modify `src/nvhttp.cpp` (handlers + body limit), `src/stream.{h,cpp}` (packet `IDX_CLIPBOARD_FILE_REQUEST` → `0x3005` appended after `IDX_MIC_AUDIO`, per-session `std::deque` queue with mutex drained in the control broadcast loop next to the clipboard notice, `stream::session::post_clipboard_file_request(session, chunk_request)`, `session_ended` hook), `src/clipboard/sync_policy.*` or the watcher (echo suppression via `note_host_write` on `clipboard_set`), `tests/unit/test_clipboard_files_service.cpp` (portable parts).

**Interfaces — Consumes:** H1, H2, H3, H4 protocols. **Produces:**
```cpp
namespace clipboard::files::service {
  void start();   // creates transfer, starts platf::clipboard_agent with on_message
  void stop();
  enum class offer_result { ok, bad_manifest, unsupported };
  offer_result install_offer(std::uintptr_t session_id, std::string_view mlcf);   // decode+validate, sanitize, split_offer → agent, transfer.set_offer
  bool on_chunk(std::string_view offer_hex, std::uint32_t req, std::uint32_t file, std::uint64_t offset, std::string body);  // false → 410
  bool on_chunk_error(std::string_view offer_hex, std::uint32_t req, read_error err);
  void session_ended(std::uintptr_t session_id);   // fails/clears the offer owned by that session
}
```
Flow: agent `read_range` → `transfer.start_read`; `transfer.send_request` → `stream::session::post_clipboard_file_request` for the session that installed the offer (if gone → fail `gone`); `transfer.deliver` → split into ≤ 1 MiB `range_data` frames → agent; `fail` → `range_error`; agent `cancel_read` → `transfer.cancel_read`; agent `offer_dropped` → `transfer.clear_offer`; agent `clipboard_set{seq}` → `note_host_write(seq, origin = offering client)`; a 200 ms ticker thread calls `transfer.tick`.
Keep-alive check (spec "Performance requirements"): count distinct HTTPS connections that delivered `file-chunk` requests per offer (key on the remote endpoint/socket of `request`) and log at info when an offer is replaced/cleared: "Clipboard files: offer <hex>: <N> chunks, <B> bytes over <C> connections, <MB/s>". C must stay ≤ the worker count (4) for a single paste.
nvhttp: `type=files` → perms (`clipboard_set` + `file_upload` + active session) → 403; `install_offer` → 200 / 400 / 503 (agent not connected); `type=file-chunk` → parse query (bad → 400), perms, `X-Clipboard-Error` → `on_chunk_error`, else `on_chunk(std::move(body))` → 200/410. Handlers must not block. Body limit as in Global Constraints.

- [ ] **Step 1:** Portable tests for `service` logic that does not need Windows (inject fakes via a test seam: `service::set_test_hooks({send_frame, post_request})`): `InstallOfferForwardsFramesAndSetsTransfer`, `ChunkRoundTripDeliversRangeData` (read_range → request → on_chunk → range_data frames ≤ 1 MiB with `last` set), `SessionEndFailsOfferReads` (pending read → range_error gone; later chunk → 410), `BadManifestRejected`.
- [ ] **Step 2:** Commit, wintest → fail. **Step 3:** Implement. **Step 4:** wintest all clipboard tests + `sunshine` → pass. Add the new test targets to the CI workflow. **Step 5:** Commit `feat(clipboard-files): host endpoints, 0x3005 requests and agent wiring`; push.

### Task C1 (haiku): moonlight-common-c — 0x3005 file request callback

**Files:** `src/Limelight.h`, `src/ControlStream.c`, `src/FakeCallbacks.c` (repo: moonlight-qt/moonlight-common-c/moonlight-common-c, branch `feat/clipboard-files` from a8eb510); moonlight-qt: submodule bump + `app/streaming/session.{h,cpp}` add a no-op `clClipboardFileRequest` (filled in C3) appended last in `k_ConnCallbacks` order.

**Interfaces — Produces:** `typedef void(*ConnListenerClipboardFileRequest)(const uint8_t offerId[16], uint32_t requestId, uint32_t fileIndex, uint64_t offset, uint32_t length);` appended as the last field of `CONNECTION_LISTENER_CALLBACKS`.
- [ ] **Step 1:** Mirror exactly how `IDX_CLIPBOARD_CHANGED`/`clipboardChanged` were added (IDX define, `0x3005` only in `packetTypesGen7Enc`, `-1` elsewhere, async-callback union member with the 5 fields + offer bytes, parse branch with runt check `payload ≥ 37` and `version == 1`, length 1…4 MiB else drop, dispatch case) but DO NOT apply the duplicate-collapsing used for clipboardChanged.
- [ ] **Step 2:** Build moonlight-qt (`build/` incremental; delete the app binary to force relink) → succeeds. **Step 3:** Commit in common-c, push `fork feat/clipboard-files`; commit submodule bump + session stub in moonlight-qt on `feat/clipboard-files`, push origin.

### Task C2 (sonnet): Client manifest builder + MLCF encoder

**Files:** Create `app/streaming/clipboard/files/{manifestbuilder,filecodec}.{h,cpp}`, `tests/clipboard/files_vectors.txt` (content above); Modify `app/app.pro` (common SOURCES/HEADERS), `tests/clipboard/clipboard_tests.pro`, `tests/clipboard/tst_clipboard.mm`.

**Interfaces — Produces:**
```cpp
namespace ClipboardFiles {
  struct Entry { bool isDir; quint64 size; qint64 mtimeMs; QString relativePath; QString absolutePath; };   // relativePath NFC, '/'
  struct Limits { int maxEntries = 100000; int maxPathBytes = 1024; int maxComponentUtf16 = 255; };
  struct BuildResult { QVector<Entry> entries; QStringList skipped; QString error; };  // error non-empty → do not offer
  BuildResult buildManifest(const QStringList &topLevelPaths, const Limits &limits = {});
  QByteArray encodeManifest(const QByteArray &offerId16, const QVector<Entry> &entries);   // MLCF v1
  QByteArray newOfferId();   // 16 random bytes (QRandomGenerator::system)
}
```
Rules: top-level items keep their own name; directories are listed before their children (pre-order, children sorted by name for determinism); symlinks are not followed (skipped, added to `skipped`); `.DS_Store` skipped; names NFC via `QString::normalized(QString::NormalizationForm_C)`; unreadable entries skipped; limits exceeded → `error`.
- [ ] **Step 1: Failing QtTests:** `encodesSharedVectors` (build the 3 vectors' entries in code, encode with offer 00..0f, compare with `files_vectors.txt` hex), `buildsTreePreOrder` (temp tree `top/` with `a.txt`, `sub/b.txt` → entries `top`, `top/a.txt`, `top/sub`, `top/sub/b.txt`), `normalizesToNfc` (create a file named with NFD `"й.txt"` → relativePath equals NFC `"й.txt"`), `skipsSymlinksAndDsStore`, `enforcesLimits` (maxEntries=2 with 3 files → error).
- [ ] **Step 2:** Run tests → fail. **Step 3:** Implement. **Step 4:** Tests pass; app builds (`make clean` in `build/app` first — headers added). **Step 5:** Commit `clipboard-files: manifest builder and MLCF encoder`.

### Task C3 (sonnet): Client offer push + range serving

**Files:** Create `app/streaming/clipboard/files/fileserver.{h,cpp}`; Modify `macpasteboard.{h,mm}` (`QStringList fileURLs() const`), `clipboardsync.{h,cpp}` (offer path in `push()`), `app/backend/nvhttp.{h,cpp}` (`int postClipboardFiles(const QByteArray& mlcf, int timeoutMs)`, `int postClipboardFileChunk(const QByteArray& offerHex, quint32 req, quint32 file, quint64 offset, const QByteArray& body, const QByteArray& errorCode, int timeoutMs)`), `app/backend/nvcomputer.{h,cpp}` (`bool clipboardFilesSupported` from `<ClipboardFiles>`, ASSIGN_IF_CHANGED), `app/streaming/session.cpp` (`clClipboardFileRequest` → `m_ClipboardSync->notifyFileRequest(...)`), tests in `tests/clipboard/tst_clipboard.mm`.

**Interfaces — Consumes:** C1 callback, C2 builder/encoder. **Produces:**
```cpp
class FileServer : public QObject {
public:
  explicit FileServer(NvComputer *computer, int workers = 4);
  ~FileServer();                                   // joins workers
  void setOffer(const QByteArray &offerId16, const QVector<ClipboardFiles::Entry> &entries);
  void clearOffer();
  void request(const QByteArray &offerId16, quint32 requestId, quint32 fileIndex, quint64 offset, quint32 length); // thread-safe
  // for tests:
  struct Reply { QByteArray body; QByteArray error; };
  static Reply readRange(const QVector<ClipboardFiles::Entry> &entries, quint32 fileIndex, quint64 offset, quint32 length);
};
```
Rules: `readRange` validates the index (dir or out of range → `io`), stats the file (missing or size/mtime differ from the manifest → `changed`), reads `length` bytes (fewer only at EOF); unknown offer → `gone`. Workers: 4 threads, each with its own `NvHTTP` (keep-alive reuse), pulling from a FIFO of requests; POST timeout 30 s. `ClipboardSync::push()`: if `fileURLs()` non-empty and not sensitive → if `computer->clipboardFilesSupported` build manifest (on the worker thread) → `postClipboardFiles` → on 200 `fileServer.setOffer`, mark change count sent; on 403 disable file offers for the session (log once); if unsupported → keep phase-1 behaviour (skip).
- [ ] **Step 1: Failing QtTests** on `FileServer::readRange` with a temp file: `readsFullAndPartialRanges`, `shortReadAtEof`, `changedFileReportsChanged` (rewrite with a different size after building the entry), `directoryIndexIsIo`; and `StaleOfferReportsGone` via a pure helper `FileServer::classify(offerKnown, …)` or by calling `request()` with a different offer and capturing the reply through an injectable sender (add a test seam `setSenderForTests(std::function<void(Reply, quint32 req)>)`).
- [ ] **Step 2:** Run → fail. **Step 3:** Implement (incl. NvHTTP methods using `buildRequest` + `X-Clipboard-Error` header). **Step 4:** Tests pass; `make clean` + app build. **Step 5:** Commit `clipboard-files: offer push and range serving`; push.

### Task E1 (controller, with the user): deploy and E2E
- [ ] Download the CI installer for the final Vibepollo commit, check the host log for active sessions first (memory: anonymous serverinfo lies), install on win11-gaming, verify `<ClipboardFiles>1`.
- [ ] Clean-build the Mac app; the user relaunches it.
- [ ] Matrix: 4 GB file → Explorer (time it; target ≥ 80 MB/s), folder with nested Cyrillic names and 1000 small files → Explorer, small image → Telegram/Discord paste (CF_HDROP), cancel mid-copy, delete source after copy → error, new Finder copy during a paste → old paste fails "gone", old client against new host and new client against old host → no regression.
