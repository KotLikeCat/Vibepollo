/**
 * @file src/clipboard/files/service.cpp
 * @brief Portable core of the host clipboard-files service (see service.h).
 */
#include "service.h"

#include "manifest.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <iomanip>

namespace clipboard::files::service {
  namespace {
    struct offer_stats {
      std::uint64_t chunks = 0;
      std::uint64_t bytes = 0;
      std::set<std::string> connections;
      std::optional<std::chrono::steady_clock::time_point> first;
      std::chrono::steady_clock::time_point last;
    };

    struct state_t {
      std::mutex mutex;  ///< guards everything below; never held while calling into `tr` or hooks
      std::mutex install_mutex;  ///< serializes install_offer / clearing
      hooks h;
      std::unique_ptr<transfer> tr;
      std::uint64_t prefetch_bytes = 0;
      bool has_owner = false;
      std::uintptr_t owner = 0;
      std::string owner_uuid;
      std::optional<offer_id_t> offer;
      std::set<std::uint32_t> active_reads;
      std::string last_origin;  ///< origin of the most recent offer; survives clear so a late clipboard_set still resolves
      std::set<std::uint32_t> aborted;  ///< reads already failed toward the agent; late events are ignored
      offer_stats stats;
    };

    state_t &st() {
      static state_t s;
      return s;
    }

    std::atomic<bool> g_active {false};
    /// steady_clock ticks of the last moment a read was in flight (0 = never). Keeps active() true for a short linger
    /// after the last read ends so the control loop does not fall back to its slow iterate between files / refills.
    std::atomic<std::chrono::steady_clock::rep> g_last_active {0};
    constexpr auto k_active_linger = std::chrono::seconds(2);

    void stamp_active() {
      g_last_active.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_release);
    }

    void read_started(std::uint32_t id) {
      std::lock_guard lock {st().mutex};
      st().active_reads.insert(id);
      g_active.store(true, std::memory_order_release);
    }

    /// `served` = the read really ran (not rejected up front): only those arm the linger.
    void read_finished(std::uint32_t id, bool served = true) {
      std::lock_guard lock {st().mutex};
      st().active_reads.erase(id);
      if (served) {
        stamp_active();  // before g_active drops, so a reader that sees "idle" also sees the stamp
      }
      g_active.store(!st().active_reads.empty(), std::memory_order_release);
    }

    transfer::callbacks make_callbacks();

    /// The transfer is created once (configure() / set_test_hooks()) and never replaced while running.
    transfer *tr() {
      std::lock_guard lock {st().mutex};
      return st().tr.get();
    }

    bool send_frame(const std::string &frame) {
      std::function<bool(const std::string &)> f;
      {
        std::lock_guard lock {st().mutex};
        f = st().h.send_frame;
      }
      return f && f(frame);
    }

    void send_error(std::uint32_t read_id, read_error e) {
      send_frame(agent::encode_range_error({read_id, e}));
    }

    bool is_aborted(std::uint32_t read_id) {
      std::lock_guard lock {st().mutex};
      return st().aborted.contains(read_id);
    }

    /// Fails a read toward the agent and stops the scheduler from working on it.
    void abort_read(std::uint32_t read_id, read_error e) {
      {
        std::lock_guard lock {st().mutex};
        if (!st().aborted.insert(read_id).second) {
          return;
        }
      }
      read_finished(read_id);
      send_error(read_id, e);
      if (auto *t = tr()) {
        t->cancel_read(read_id);
      }
    }

    void do_deliver(std::uint32_t read_id, std::string data, bool last) {
      if (is_aborted(read_id)) {
        return;
      }
      std::size_t pos = 0;
      do {
        const auto n = std::min(agent::max_frame_payload, data.size() - pos);
        const bool final_part = pos + n >= data.size();
        if (!send_frame(agent::encode_range_data({read_id, last && final_part, data.substr(pos, n)}))) {
          abort_read(read_id, read_error::io);
          return;
        }
        pos += n;
      } while (pos < data.size());
      if (last) {
        read_finished(read_id);
      }
    }

    void do_fail(std::uint32_t read_id, read_error e) {
      read_finished(read_id);
      {
        std::lock_guard lock {st().mutex};
        if (!st().aborted.insert(read_id).second) {
          return;
        }
      }
      send_error(read_id, e);
    }

    void do_send_request(const chunk_request &req) {
      std::function<bool(std::uintptr_t, const chunk_request &)> post;
      std::uintptr_t owner = 0;
      bool has_owner;
      {
        std::lock_guard lock {st().mutex};
        post = st().h.post_request;
        owner = st().owner;
        has_owner = st().has_owner;
      }
      if (!has_owner || !post || !post(owner, req)) {
        // No control stream to ask: fail the read through the scheduler (reports `gone` to the agent).
        if (auto *t = tr()) {
          t->on_chunk_error(req.offer_id, req.request_id, read_error::gone);
        }
      }
    }

    transfer::callbacks make_callbacks() {
      return {do_send_request, do_deliver, do_fail};
    }

    void log_stats(const std::optional<offer_id_t> &offer, const offer_stats &s) {
      std::function<void(const std::string &)> log;
      {
        std::lock_guard lock {st().mutex};
        log = st().h.log;
      }
      if (!log || !offer || s.chunks == 0) {
        return;
      }
      const double secs = s.first ? std::chrono::duration<double>(s.last - *s.first).count() : 0.0;
      const double mbps = secs > 0 ? (static_cast<double>(s.bytes) / 1048576.0) / secs : 0.0;
      std::ostringstream o;
      o << "Clipboard files: offer " << offer_id_hex(*offer) << ": " << s.chunks << " chunks, " << s.bytes << " bytes over " << s.connections.size()
        << " connections, " << std::fixed << std::setprecision(1) << mbps << " MB/s";
      log(o.str());
    }

    /// Forgets the current offer (state first, then the scheduler so pending reads fail).
    void clear_current(bool tell_agent) {
      std::optional<offer_id_t> offer;
      offer_stats stats;
      {
        std::lock_guard lock {st().mutex};
        offer = st().offer;
        stats = std::move(st().stats);
        st().stats = {};
        st().offer.reset();
        st().has_owner = false;
        st().owner_uuid.clear();
      }
      log_stats(offer, stats);
      if (auto *t = tr()) {
        t->clear_offer();
      }
      {
        std::lock_guard lock {st().mutex};
        st().aborted.clear();
        st().active_reads.clear();
        g_active.store(false, std::memory_order_release);
      }
      if (tell_agent && offer) {
        send_frame(agent::encode_clear_offer(*offer));
      }
    }
  }  // namespace

  void set_test_hooks(hooks h, transfer_options opt) {
    auto &s = st();
    std::lock_guard install {s.install_mutex};
    {
      std::lock_guard lock {s.mutex};
      s.h = std::move(h);
      s.has_owner = false;
      s.owner_uuid.clear();
      s.offer.reset();
      s.aborted.clear();
      s.active_reads.clear();
      g_active.store(false);
      g_last_active.store(0);
      s.last_origin.clear();
      s.stats = {};
      s.prefetch_bytes = 0;
      s.tr = std::make_unique<transfer>(make_callbacks(), opt);
    }
  }

  void configure(hooks h) {
    auto &s = st();
    std::lock_guard install {s.install_mutex};
    std::lock_guard lock {s.mutex};
    s.h = std::move(h);
    if (!s.tr) {
      s.tr = std::make_unique<transfer>(make_callbacks());
    }
  }

  bool active(std::chrono::steady_clock::time_point now) {
    if (g_active.load(std::memory_order_acquire)) {
      return true;
    }
    const auto last = g_last_active.load(std::memory_order_acquire);
    if (last == 0) {
      return false;
    }
    const std::chrono::steady_clock::time_point at {std::chrono::steady_clock::duration {last}};
    return now - at < k_active_linger;
  }

  bool chunk_forbidden(std::string_view uuid) {
    std::lock_guard lock {st().mutex};
    return st().has_owner && st().owner_uuid != uuid;
  }

  namespace {
    template<class T>
    std::optional<T> parse_dec(std::string_view v) {
      T out {};
      const auto *end = v.data() + v.size();
      const auto r = std::from_chars(v.data(), end, out, 10);
      if (v.empty() || r.ec != std::errc {} || r.ptr != end) {
        return std::nullopt;
      }
      return out;
    }
  }  // namespace

  std::optional<chunk_query> parse_chunk_query(std::string_view offer, std::string_view req, std::string_view file, std::string_view offset) {
    if (offer.size() != 32) {
      return std::nullopt;
    }
    const auto id = parse_offer_id_hex(offer);
    const auto r = parse_dec<std::uint32_t>(req);
    const auto f = parse_dec<std::uint32_t>(file);
    const auto o = parse_dec<std::uint64_t>(offset);
    if (!id || !r || !f || !o) {
      return std::nullopt;
    }
    return chunk_query {*id, *r, *f, *o};
  }

  void set_prefetch_bytes(std::uint64_t bytes) {
    std::lock_guard lock {st().mutex};
    st().prefetch_bytes = bytes;
  }

  void tick(std::chrono::steady_clock::time_point now) {
    if (auto *t = tr()) {
      t->tick(now);
    }
  }

  offer_result install_offer(std::uintptr_t session_id, std::string_view mlcf, std::string origin_uuid, std::string *error) {
    auto decoded = decode_manifest(mlcf);
    if (decoded.error != manifest_error::none) {
      if (error) {
        *error = error_name(decoded.error);
      }
      return offer_result::bad_manifest;
    }
    // Explorer's virtual files and CF_HDROP cannot express longer paths: refuse loudly instead of pasting an incomplete tree.
    for (const auto &p : sanitize_for_windows(decoded.value.entries)) {
      if (utf16_length(p) > max_windows_path_utf16) {
        if (error) {
          *error = "windows_path_too_long";
        }
        return offer_result::bad_manifest;
      }
    }
    std::function<bool()> connected;
    std::function<bool(std::uintptr_t)> encrypted;
    {
      std::lock_guard lock {st().mutex};
      connected = st().h.agent_connected;
      encrypted = st().h.session_encrypted;
    }
    if (!connected || !connected()) {
      return offer_result::unsupported;
    }
    // 0x3005 carries the offer id (the capability for chunk POSTs): it may only travel over an encrypted control stream.
    if (encrypted && !encrypted(session_id)) {
      return offer_result::forbidden;
    }

    std::lock_guard install {st().install_mutex};
    auto *t = tr();
    if (!t) {
      return offer_result::unsupported;
    }
    clear_current(false);  // logs the previous offer's throughput

    bool prefetch;
    {
      std::lock_guard lock {st().mutex};
      prefetch = st().prefetch_bytes > 0 && total_size(decoded.value) <= st().prefetch_bytes;
      st().has_owner = true;
      st().owner = session_id;
      st().last_origin = origin_uuid;
      st().owner_uuid = std::move(origin_uuid);
      st().offer = decoded.value.offer_id;
    }
    // Scheduler first so a read_range racing with the agent's offer install is never rejected as stale.
    t->set_offer(decoded.value);
    for (const auto &frame : agent::split_offer(decoded.value.offer_id, prefetch, mlcf)) {
      if (!send_frame(frame)) {
        clear_current(true);
        return offer_result::unsupported;
      }
    }
    // The owning session may have ended while we were installing (session_ended is serialized on install_mutex,
    // but it could have run before the owner was set).
    std::function<bool(std::uintptr_t)> alive;
    {
      std::lock_guard lock {st().mutex};
      alive = st().h.session_alive;
    }
    if (alive && !alive(session_id)) {
      clear_current(true);
      return offer_result::unsupported;
    }
    return offer_result::ok;
  }

  bool on_chunk(std::string_view offer_hex, std::uint32_t req, std::uint32_t file, std::uint64_t offset, std::string body, std::string_view connection_key) {
    const auto id = parse_offer_id_hex(offer_hex);
    if (!id) {
      return false;
    }
    auto *t = tr();
    if (!t) {
      return false;
    }
    const auto size = body.size();
    if (!t->on_chunk(*id, req, file, offset, std::move(body))) {
      return false;
    }
    const auto now = std::chrono::steady_clock::now();
    std::lock_guard lock {st().mutex};
    if (st().offer && *st().offer == *id) {
      auto &s = st().stats;
      ++s.chunks;
      s.bytes += size;
      if (!s.first) {
        s.first = now;
      }
      s.last = now;
      if (!connection_key.empty()) {
        s.connections.emplace(connection_key);
      }
    }
    return true;
  }

  bool on_chunk_error(std::string_view offer_hex, std::uint32_t req, read_error err) {
    const auto id = parse_offer_id_hex(offer_hex);
    if (!id) {
      return false;
    }
    auto *t = tr();
    return t && t->on_chunk_error(*id, req, err);
  }

  void session_ended(std::uintptr_t session_id) {
    std::lock_guard install {st().install_mutex};
    {
      std::lock_guard lock {st().mutex};
      if (!st().has_owner || st().owner != session_id) {
        return;
      }
    }
    clear_current(true);
  }

  void handle_agent_connection(bool connected) {
    if (connected) {
      return;
    }
    // The dead agent's data object is gone and its read ids restart in the next process.
    std::lock_guard install {st().install_mutex};
    clear_current(false);
  }

  void handle_agent_message(const agent::message &m) {
    auto *t = tr();
    if (!t) {
      return;
    }
    switch (m.type) {
      case agent::msg::read_range:
        {
          const auto r = agent::decode_read_range(m.payload);
          if (!r) {
            return;
          }
          // The offer check and the start are one atomic step inside the scheduler (a new offer can arrive any time).
          read_started(r->read_id);
          const auto res = t->start_read(r->offer, r->read_id, r->file_index, r->offset, r->length);
          if (res != start_result::ok) {
            read_finished(r->read_id, false);
            send_error(r->read_id, res == start_result::gone ? read_error::gone : read_error::io);
          }
          return;
        }
      case agent::msg::cancel_read:
        if (const auto id = agent::decode_cancel_read(m.payload)) {
          read_finished(*id);
          t->cancel_read(*id);
        }
        return;
      case agent::msg::offer_dropped:
        if (const auto id = agent::decode_offer_dropped(m.payload)) {
          std::lock_guard install {st().install_mutex};
          bool current;
          {
            std::lock_guard lock {st().mutex};
            current = st().offer && *st().offer == *id;
          }
          if (current) {
            clear_current(false);
          }
        }
        return;
      case agent::msg::clipboard_set:
        if (const auto seq = agent::decode_clipboard_set(m.payload)) {
          std::function<void(std::uint32_t, const std::string &)> note;
          std::string uuid;
          {
            std::lock_guard lock {st().mutex};
            note = st().h.note_clipboard_set;
            uuid = st().last_origin;
          }
          if (note && !uuid.empty()) {
            note(*seq, uuid);
          }
        }
        return;
      default:
        return;
    }
  }
}  // namespace clipboard::files::service
