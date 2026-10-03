# Clipboard files, Mac → Windows host — design

Status: design approved by the user in chat on 2026-10-02 ("да, норм"). Phase 2 of clipboard sync (phase 1: docs/superpowers/specs/2026-10-01-clipboard-sync-design.md). Host → Mac files is a separate, later spec.
Branches: Vibepollo / moonlight-qt / moonlight-common-c `feat/clipboard-files` (from master).

## Goal
Copy files and folders in macOS Finder, enter the stream window, paste them on the Windows host — in Explorer (any size, incl. gigabytes, with Explorer's own progress dialog) and into applications (Telegram, Discord, browser upload) for reasonably small sets. Behaviour mirrors RDP: bytes move only when they are needed.

## User decisions
- Any sizes incl. gigabytes → lazy transfer on paste.
- Paste targets include apps → small sets are prefetched and also exposed as real files.
- Mac → host first; same focus rule as phase 1 (Mac clipboard goes to the host when the stream window gains focus).

## Overview
1. On focus gain, if the Mac pasteboard holds file URLs, moonlight-qt builds a manifest (files + folders, sizes, mtimes, relative paths) and POSTs it to the host as a *file offer*.
2. Vibepollo hands the offer to a resident user-session agent, `sunshine_clipboard_agent.exe`, which places an OLE data object on the Windows clipboard exposing virtual files (`FileGroupDescriptorW` + `FileContents`) and, for small offers, real files (`CF_HDROP`) backed by a prefetch into a temp folder.
3. When Explorer/an app reads file contents, the agent asks sunshine.exe for byte ranges; sunshine.exe asks the client with a control packet; the client reads the range from disk and POSTs it back over HTTPS; sunshine.exe forwards the bytes to the agent.

## Capability
- Host config: `clipboard_files` (bool, default enabled), `clipboard_files_prefetch_bytes` (default 268435456 = 256 MiB; 0 disables prefetch; max 4 GiB). Both restart-required.
- `/serverinfo`: `<ClipboardFiles>1</ClipboardFiles>` when `clipboard_sync && clipboard_files` and the agent is running. The client only sends offers to hosts that advertise it; otherwise phase-1 behaviour (file-URL pasteboards are skipped).

## Wire protocol (Vibepollo protocol extension)
### Offer manifest — `POST /actions/clipboard?type=files` (client → host)
Binary "MLCF" v1, little-endian:
- `char magic[4] = "MLCF"`, `u8 version = 1`, `u8 offer_id[16]` (random per offer), `u32 entry_count`.
- Then `entry_count` entries: `u8 kind` (1 = file, 2 = directory), `u64 size` (0 for directories), `i64 mtime_ms` (Unix ms UTC), `u16 path_len`, `path` (UTF-8, NFC, `/`-separated, relative; top-level items have no `/`; a directory entry precedes its children).
- Entry index (0-based) is the file id used in range requests.
- Limits: entries ≤ 100000; path ≤ 1024 bytes; each component ≤ 255 UTF-16 units; body ≤ 32 MiB.
- Host validation (reject with 400): bad magic/version, truncated, limits exceeded, empty/`.`/`..` components, leading `/`, backslashes, NUL, a child whose parent directory entry is missing, duplicate paths (case-insensitive). Name sanitising for Windows (done once, host side, deterministic): characters `<>:"|?*` and control chars → `_`; trailing dots/spaces trimmed; reserved device names (CON, PRN, AUX, NUL, COM1-9, LPT1-9, with any extension) get a `_` suffix; collisions after sanitising get ` (2)`, ` (3)`… before the extension.
- Permissions: the client must hold `clipboard_set` and `file_upload` and have an active session (same checks as the bundle POST, plus `file_upload`); otherwise 403.
- 200 = offer installed. A new offer replaces the previous one (one active offer per host).

### Range request — control packet `0x3005` (host → client, encrypted control stream only)
Payload: `u8 version = 1`, `u8 offer_id[16]`, `u32 request_id`, `u32 file_index`, `u64 offset`, `u32 length` (1 … 4 MiB). Not coalesced: every request is delivered.

### Range response — `POST /actions/clipboard?type=file-chunk&offer=<32 hex>&req=<id>&file=<idx>&offset=<n>` (client → host)
- Body = the bytes (exactly `length`, or fewer only when the range reaches EOF). `Content-Type: application/octet-stream`.
- Failure: empty body + header `X-Clipboard-Error: gone | changed | io` (gone = not the client's current offer / session; changed = file size or mtime differs from the manifest or it disappeared; io = read error).
- Host replies 200 if a matching outstanding request exists, 410 otherwise (late/cancelled) — the client just drops it.
- HTTPS request body limit: set `max_request_streambuf_size` to `max(clipboard_max_bytes, 32 MiB) + 64 KiB` so both a 32 MiB manifest and a 4 MiB chunk fit.

### Scheduling (host)
- Chunk size 4 MiB; at most 4 outstanding requests per offer; per-request timeout 15 s, one retry, then the read fails with an I/O error.
- Requests are issued only on demand (an agent read) or by the prefetcher; nothing is requested for large offers until a paste reads them.
- Cancellation: when the agent abandons a read (stream released / Explorer cancel), sunshine.exe stops issuing new requests for it; late chunks get 410.

## Performance requirements
- Target: ≥ 80 MB/s for a single large file on gigabit Ethernet (the link, not TLS, is the limit: AES-GCM runs at ~8 GB/s on the Mac); measured in E2E by timing a 4 GB copy.
- Pipelining: the host keeps up to 4 requests of 4 MiB in flight per offer (≥ 16 MiB outstanding); the agent's `IStream` reads ahead by the same amount.
- Connection reuse: chunk POSTs must reuse kept-alive TLS connections (no handshake per chunk). The client uses one `NvHTTP`/`QNetworkAccessManager` per fileserver worker and verifies keep-alive is honoured by the host's HTTPS server (add a test or a log counter of new TLS sessions during a transfer).
- No blocking on the HTTPS io thread: the chunk handler validates, moves the body to the transfer layer and replies immediately; pipe writes happen on another thread.
- Copies are acceptable (GB/s memcpy) but avoid per-byte processing.
- Minimum supported Mac uplink: about 3 Mbps. Timeouts are progress-aware (host: 15 s per request counted from the later of its issue and the last completed chunk; agent: 60 s without any `range_data` frame; client: inactivity-based POST timeout), so a slower link only slows a paste; below ~3 Mbps (4 × 4 MiB = 16 MiB = ~134 Mbit within 60 s without progress, plus overhead) a single chunk can still stall past a timeout and fail the paste.
- Mac → host traffic flows opposite to the video stream on full-duplex links, so no rate cap is needed here (host → Mac will need one).

## Host components (Vibepollo)
- `src/clipboard/files/manifest.{h,cpp}` — portable: MLCF decode/encode, validation, Windows name sanitiser, helpers (total size, top-level items).
- `src/clipboard/files/transfer.{h,cpp}` — portable: active offer registry, request ids, outstanding-limit queue, timeouts/retry, routing of received chunks to waiting readers, cancellation. Injected callbacks: `send_request(session, request)` and agent delivery. No Windows APIs (unit-testable).
- `src/nvhttp.cpp` — `type=files` and `type=file-chunk` handlers (permission checks, hand off quickly; no long work on the HTTPS io thread), body-size limit.
- `src/stream.cpp` — packet `IDX_CLIPBOARD_FILE_REQUEST` → `0x3005`, a per-session **queue** (not a single slot like 0x3003) drained in the control broadcast loop, `stream::session::post_clipboard_file_request(...)`.
- Agent lifecycle (`src/platform/windows/clipboard_agent_host.{h,cpp}`): launch `tools\sunshine_clipboard_agent.exe` as the logged-on user with `ProcessHandler` (mirror the display helper: resident, ping, restart with cooldown, relaunch on user logon change) and connect with `FramedPipe` / `SelfHealingPipe`. sunshine.exe never touches OLE.
- Pipe messages (framed, ≤ 2 MiB per frame → chunks are split into ≤ 1 MiB frames): `SetOffer{offer_id, manifest(sanitised), prefetch}`, `ClearOffer`, agent→core `ReadRange{read_id, file_index, offset, length}`, core→agent `RangeData{read_id, bytes…, last}` / `RangeError{read_id, code}`, agent→core `CancelRead{read_id}`, agent→core `ClipboardSet{sequence_number}` (for echo suppression), `Ping/Pong`.
- Echo suppression: when the agent reports `ClipboardSet{seq}`, sunshine.exe records `note_host_write(seq, origin client)` exactly like phase-1 writes, so the watcher does not bounce the offer back.
- `tools/sunshine_clipboard_agent.cpp` (+ small sources): resident user-context process.
  - STA thread with a message loop, `OleInitialize`; the data object is placed with `OleSetClipboard(dataObject)`. `OleFlushClipboard` is never called, so the data stays lazy (rendered on demand).
  - `IDataObject` offering `CFSTR_FILEDESCRIPTORW` (all entries, `FD_ATTRIBUTES|FD_FILESIZE|FD_WRITESTIME|FD_PROGRESSUI|FD_UNICODE`, directories with `FILE_ATTRIBUTE_DIRECTORY`, 64-bit sizes), `CFSTR_FILECONTENTS` (`lindex` → a new `IStream` per call), `CFSTR_PREFERREDDROPEFFECT = DROPEFFECT_COPY`, and — only when total size ≤ `clipboard_files_prefetch_bytes` — `CF_HDROP`.
  - `IStream` objects aggregate the free-threaded marshaler so cross-process reads run on RPC threads, not the STA. `Read` blocks until data arrives (read-ahead of up to 4 chunks), supports `Seek`/`Stat`; returns `STG_E_READFAULT` on errors.
  - Prefetch (small offers): starts as soon as the offer is set, writes into `%LOCALAPPDATA%\Temp\Vibepollo\clipboard\<offer>\` preserving the tree; `GetData(CF_HDROP)` waits (≤ 120 s) for completion and returns the top-level paths; on failure it returns `E_FAIL`. Virtual-file reads of a prefetched file are served from disk.
  - Cleanup: delete prefetch folders of replaced offers, on `ClearOffer`, and stale ones on agent start.
  - If the clipboard is replaced by someone else, the agent drops its data object (`IDataObject` released) and tells sunshine.exe to cancel outstanding reads.

## Client components (moonlight-qt + moonlight-common-c)
- moonlight-common-c: `ConnListenerClipboardFileRequest(const uint8_t offerId[16], uint32_t requestId, uint32_t fileIndex, uint64_t offset, uint32_t length)` (appended to `CONNECTION_LISTENER_CALLBACKS`, fake stub, NULL fill), `IDX_CLIPBOARD_FILE_REQUEST` → `0x3005` only in `packetTypesGen7Enc`, parse with runt/version checks, delivered on the async callback queue **without** collapsing.
- `MacPasteboard::fileURLs()` — file URLs of the current pasteboard (`NSPasteboardTypeFileURL` items); the sensitive-data check still applies.
- `app/streaming/clipboard/files/manifestbuilder.{h,cpp}` — walks the selection (QDirIterator, no symlink following; symlinks and `.DS_Store` skipped with a log), NFC-normalises names, enforces the limits above, records absolute source paths + size + mtime; runs on the clipboard worker thread.
- `app/streaming/clipboard/files/filecodec.{h,cpp}` — MLCF encoder (mirror of the host decoder; shared test vectors).
- `app/streaming/clipboard/files/fileserver.{h,cpp}` — keeps the current offer (id → manifest with absolute paths); serves range requests on a small pool of worker threads (4, each with its own `NvHTTP`): validates offer/index, checks size+mtime against the manifest, reads the range, `NvHTTP::postClipboardFileChunk(...)` (`QByteArray`, timeout 30 s); errors reported with `X-Clipboard-Error`.
- `ClipboardSync::push()` — when the pasteboard holds file URLs and `NvComputer::clipboardFilesSupported`: build the manifest, `POST type=files`, mark the change count as sent (same state machine as phase 1); otherwise the phase-1 path. A new offer replaces the old one in `fileserver`; the old offer keeps no data on disk (nothing to clean on the Mac).
- `NvComputer::clipboardFilesSupported` from `<ClipboardFiles>`.

## Error handling summary
- Host unsupported / permission denied → phase-1 behaviour; one log line.
- Mac file changed or deleted after copy → `changed` → Explorer shows a copy error for that file.
- Client disconnected mid-paste → requests time out → `STG_E_READFAULT` → Explorer error; prefetch → `CF_HDROP` unavailable.
- Agent crash → sunshine.exe restarts it; the offer is lost (the Windows clipboard no longer holds it).

## Testing
- Host unit (portable, gtest): manifest decode/validation (all rejection cases), sanitiser (reserved names, illegal chars, collisions, Cyrillic preserved), transfer scheduler (outstanding limit, ordering, timeout + retry, cancellation, late chunk → gone), shared MLCF vectors with the client.
- Host Windows gtest (build-win): agent data object in-process — set an offer with a fake range provider, read `FileGroupDescriptorW`/`FileContents` through `OleGetClipboard`, compare bytes; `CF_HDROP` after prefetch; Shell copy (`IFileOperation` paste into a temp dir) of the virtual files.
- Client QtTest: manifest builder (tree, NFC, symlink/.DS_Store skip, limits), MLCF vectors, fileserver range checks (changed file, EOF short read).
- E2E (win11-gaming): Finder copy → Explorer paste of (a) a 4 GB file, (b) a folder with nested Cyrillic names, (c) a small image into Telegram/Discord; cancel mid-copy; delete the source after copy → error; old client/host → no regression.

## Out of scope
Host → Mac files (next spec), drag and drop, moving (cut) semantics, resuming interrupted copies.
