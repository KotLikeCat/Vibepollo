/**
 * @file src/clipboard/files/manifest.cpp
 * @brief MLCF v1 manifest codec, validation and Windows name sanitizer.
 */
#include "manifest.h"

#include <algorithm>
#include <set>
#include <unordered_map>

namespace clipboard::files {
  namespace {
    constexpr std::string_view magic = "MLCF";
    constexpr std::size_t header_size = 4 + 1 + 16 + 4;
    constexpr std::size_t entry_fixed_size = 1 + 8 + 8 + 2;

    /// UTF-8 -> UTF-16; returns false on malformed input.
    bool to_utf16(std::string_view s, std::u16string &out) {
      out.clear();
      std::size_t i = 0;
      while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        std::uint32_t cp;
        std::size_t len;
        if (b < 0x80) {
          cp = b;
          len = 1;
        } else if (b >= 0xC2 && b <= 0xDF) {
          cp = b & 0x1F;
          len = 2;
        } else if (b >= 0xE0 && b <= 0xEF) {
          cp = b & 0x0F;
          len = 3;
        } else if (b >= 0xF0 && b <= 0xF4) {
          cp = b & 0x07;
          len = 4;
        } else {
          return false;
        }
        if (i + len > s.size()) {
          return false;
        }
        for (std::size_t k = 1; k < len; ++k) {
          const auto c = static_cast<unsigned char>(s[i + k]);
          if ((c & 0xC0) != 0x80) {
            return false;
          }
          cp = (cp << 6) | (c & 0x3F);
        }
        if ((len == 3 && cp < 0x800) || (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) || (cp >= 0xD800 && cp <= 0xDFFF)) {
          return false;
        }
        if (cp >= 0x10000) {
          cp -= 0x10000;
          out.push_back(static_cast<char16_t>(0xD800 + (cp >> 10)));
          out.push_back(static_cast<char16_t>(0xDC00 + (cp & 0x3FF)));
        } else {
          out.push_back(static_cast<char16_t>(cp));
        }
        i += len;
      }
      return true;
    }

    /// Locale-independent simple case fold for one UTF-16 code unit (NTFS-like for common scripts).
    char16_t fold_unit(char16_t c) {
      if (c >= u'A' && c <= u'Z') {
        return static_cast<char16_t>(c + 0x20);
      }
      if (c < 0xC0) {
        return c;
      }
      if (c <= 0xDE) {
        return c == 0xD7 ? c : static_cast<char16_t>(c + 0x20);
      }
      if (c >= 0x100 && c <= 0x17F) {
        if (c == 0x130 || c == 0x131 || c == 0x138 || c == 0x149 || c == 0x17F) {
          return c;
        }
        if (c == 0x178) {
          return 0xFF;
        }
        if ((c >= 0x139 && c <= 0x148) || (c >= 0x179 && c <= 0x17E)) {
          return (c & 1) ? static_cast<char16_t>(c + 1) : c;  // odd = upper in these ranges
        }
        return (c & 1) ? c : static_cast<char16_t>(c + 1);
      }
      if (c >= 0x391 && c <= 0x3A9 && c != 0x3A2) {
        return static_cast<char16_t>(c + 0x20);
      }
      if (c >= 0x410 && c <= 0x42F) {
        return static_cast<char16_t>(c + 0x20);
      }
      if (c >= 0x400 && c <= 0x40F) {
        return static_cast<char16_t>(c + 0x50);
      }
      if ((c >= 0x460 && c <= 0x481) || (c >= 0x48A && c <= 0x4BF) || (c >= 0x4D0 && c <= 0x4FF)) {
        return (c & 1) ? c : static_cast<char16_t>(c + 1);
      }
      if (c == 0x4C0) {
        return 0x4CF;
      }
      if (c >= 0x4C1 && c <= 0x4CE) {
        return (c & 1) ? static_cast<char16_t>(c + 1) : c;
      }
      return c;
    }

    /// Case-insensitive comparison key.
    std::u16string fold_key(std::string_view s) {
      std::u16string u;
      if (!to_utf16(s, u)) {
        u.assign(s.begin(), s.end());
      }
      for (auto &c : u) {
        c = fold_unit(c);
      }
      return u;
    }

    std::size_t utf16_len(std::string_view s) {
      std::size_t n = 0;
      for (const char ch : s) {
        const auto b = static_cast<unsigned char>(ch);
        if ((b & 0xC0) != 0x80) {
          n += b >= 0xF0 ? 2 : 1;
        }
      }
      return n;
    }

    /// Truncates on a code point boundary to at most max_units UTF-16 units.
    std::string truncate_utf16(std::string_view s, std::size_t max_units) {
      std::size_t n = 0;
      std::size_t i = 0;
      while (i < s.size()) {
        const auto b = static_cast<unsigned char>(s[i]);
        const std::size_t len = b < 0x80 ? 1 : b >= 0xF0 ? 4 : b >= 0xE0 ? 3 : 2;
        const std::size_t units = len == 4 ? 2 : 1;
        if (n + units > max_units) {
          break;
        }
        n += units;
        i += len;
      }
      return std::string(s.substr(0, i));
    }

    /// Shrinks the part before the last extension so the whole name fits max_component_utf16 units.
    std::string fit_component(const std::string &name, std::size_t limit = max_component_utf16) {
      const auto total = utf16_len(name);
      if (total <= limit) {
        return name;
      }
      const auto dot = name.rfind('.');
      const bool has_ext = dot != std::string::npos && dot > 0 && utf16_len(name.substr(dot)) < limit;
      const auto ext = has_ext ? name.substr(dot) : std::string();
      const auto stem = has_ext ? name.substr(0, dot) : name;
      return truncate_utf16(stem, limit - utf16_len(ext)) + ext;
    }

    struct reader {
      std::string_view data;
      std::size_t pos = 0;

      bool take(std::size_t n, const char *&p) {
        if (data.size() - pos < n) {
          return false;
        }
        p = data.data() + pos;
        pos += n;
        return true;
      }

      template<typename T>
      bool read_le(T &v) {
        const char *p;
        if (!take(sizeof(T), p)) {
          return false;
        }
        std::uint64_t u = 0;
        for (std::size_t i = 0; i < sizeof(T); ++i) {
          u |= static_cast<std::uint64_t>(static_cast<unsigned char>(p[i])) << (8 * i);
        }
        v = static_cast<T>(u);
        return true;
      }
    };

    template<typename T>
    void write_le(std::string &out, T v) {
      const auto u = static_cast<std::uint64_t>(v);
      for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<char>((u >> (8 * i)) & 0xFF));
      }
    }

    manifest_error validate_path(const std::string &path) {
      if (path.size() > max_path_bytes) {
        return manifest_error::path_too_long;
      }
      if (path.find('\0') != std::string::npos) {
        return manifest_error::nul;
      }
      if (path.find('\\') != std::string::npos) {
        return manifest_error::backslash;
      }
      if (!path.empty() && path[0] == '/') {
        return manifest_error::absolute_path;
      }
      std::size_t start = 0;
      std::u16string u16;
      while (true) {
        const auto slash = path.find('/', start);
        const auto comp = std::string_view(path).substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        if (comp.empty() || comp == "." || comp == "..") {
          return manifest_error::bad_component;
        }
        if (!to_utf16(comp, u16)) {
          return manifest_error::bad_component;
        }
        if (u16.size() > max_component_utf16) {
          return manifest_error::component_too_long;
        }
        if (slash == std::string::npos) {
          break;
        }
        start = slash + 1;
      }
      return manifest_error::none;
    }

    std::string parent_of(const std::string &path) {
      const auto slash = path.rfind('/');
      return slash == std::string::npos ? std::string() : path.substr(0, slash);
    }

    bool is_reserved_base(std::string_view base) {
      while (!base.empty() && base.back() == ' ') {
        base.remove_suffix(1);
      }
      std::string up(base);
      for (auto &c : up) {
        if (c >= 'a' && c <= 'z') {
          c = static_cast<char>(c - 'a' + 'A');
        }
      }
      if (up == "CON" || up == "PRN" || up == "AUX" || up == "NUL") {
        return true;
      }
      return up.size() == 4 && (up.compare(0, 3, "COM") == 0 || up.compare(0, 3, "LPT") == 0) && up[3] >= '1' && up[3] <= '9';
    }

    std::string sanitize_component(std::string_view comp) {
      std::string s;
      for (const char ch : comp) {
        const auto b = static_cast<unsigned char>(ch);
        if (b < 0x20 || std::string_view("<>:\"|?*").find(ch) != std::string_view::npos) {
          s.push_back('_');
        } else {
          s.push_back(ch);
        }
      }
      const auto trim = [&s]() {
        while (!s.empty() && (s.back() == '.' || s.back() == ' ')) {
          s.pop_back();
        }
      };
      trim();
      // Truncate first, then apply the reserved rule (truncation may itself create a reserved base).
      s = fit_component(s);
      trim();
      if (s.empty()) {
        return "_";
      }
      const auto dot = s.find('.');
      const auto base = std::string_view(s).substr(0, dot);
      if (is_reserved_base(base)) {
        s.insert(base.size(), "_");
        if (utf16_len(s) > max_component_utf16) {
          // Cut the tail so the "_" after the reserved base survives.
          s = truncate_utf16(s, max_component_utf16);
          trim();
        }
      }
      return s;
    }
  }  // namespace

  decode_result decode_manifest(std::string_view data) {
    decode_result res;
    const auto fail = [&](manifest_error e) {
      res.value = {};
      res.error = e;
      return res;
    };
    if (data.size() > max_manifest_bytes) {
      return fail(manifest_error::too_many_entries);
    }
    reader r {data};
    const char *p;
    if (!r.take(4, p)) {
      return fail(manifest_error::truncated);
    }
    if (std::string_view(p, 4) != magic) {
      return fail(manifest_error::bad_magic);
    }
    std::uint8_t version;
    if (!r.read_le(version)) {
      return fail(manifest_error::truncated);
    }
    if (version != 1) {
      return fail(manifest_error::bad_version);
    }
    manifest m;
    if (!r.take(16, p)) {
      return fail(manifest_error::truncated);
    }
    std::copy_n(reinterpret_cast<const std::uint8_t *>(p), 16, m.offer_id.begin());
    std::uint32_t count;
    if (!r.read_le(count)) {
      return fail(manifest_error::truncated);
    }
    if (count == 0) {
      return fail(manifest_error::empty);
    }
    if (count > max_entries) {
      return fail(manifest_error::too_many_entries);
    }
    // Every entry needs at least entry_fixed_size + 1 bytes; reject absurd counts early.
    if ((data.size() - header_size) / (entry_fixed_size + 1) < count) {
      return fail(manifest_error::truncated);
    }
    m.entries.reserve(count);
    std::set<std::u16string> dirs;
    std::set<std::u16string> seen;
    for (std::uint32_t i = 0; i < count; ++i) {
      std::uint8_t kind;
      entry e {};
      std::uint16_t path_len;
      if (!r.read_le(kind) || !r.read_le(e.size) || !r.read_le(e.mtime_ms) || !r.read_le(path_len)) {
        return fail(manifest_error::truncated);
      }
      if (kind != 1 && kind != 2) {
        return fail(manifest_error::bad_component);
      }
      e.kind = static_cast<entry_kind>(kind);
      if (path_len > max_path_bytes) {
        return fail(manifest_error::path_too_long);
      }
      if (!r.take(path_len, p)) {
        return fail(manifest_error::truncated);
      }
      e.path.assign(p, path_len);
      if (const auto err = validate_path(e.path); err != manifest_error::none) {
        return fail(err);
      }
      // The format requires a directory entry to precede its children (the Mac client emits pre-order).
      const auto parent = parent_of(e.path);
      if (!parent.empty() && !dirs.contains(fold_key(parent))) {
        return fail(manifest_error::missing_parent);
      }
      const auto key = fold_key(e.path);
      if (!seen.insert(key).second) {
        return fail(manifest_error::duplicate_path);
      }
      if (e.kind == entry_kind::directory) {
        dirs.insert(key);
      }
      m.entries.push_back(std::move(e));
    }
    if (r.pos != data.size()) {
      return fail(manifest_error::trailing_bytes);
    }
    res.value = std::move(m);
    return res;
  }

  std::string encode_manifest(const manifest &m) {
    std::string out;
    out.append(magic);
    out.push_back(1);
    out.append(reinterpret_cast<const char *>(m.offer_id.data()), m.offer_id.size());
    write_le(out, static_cast<std::uint32_t>(m.entries.size()));
    for (const auto &e : m.entries) {
      out.push_back(static_cast<char>(e.kind));
      write_le(out, e.size);
      write_le(out, e.mtime_ms);
      write_le(out, static_cast<std::uint16_t>(e.path.size()));
      out.append(e.path);
    }
    return out;
  }

  std::uint64_t total_size(const manifest &m) {
    std::uint64_t total = 0;
    for (const auto &e : m.entries) {
      if (e.kind == entry_kind::file) {
        total += e.size;
      }
    }
    return total;
  }

  std::vector<std::string> sanitize_for_windows(const std::vector<entry> &entries) {
    std::vector<std::string> result;
    result.reserve(entries.size());
    // original directory path (folded) -> sanitized '/'-joined path
    std::unordered_map<std::u16string, std::string> dir_map;
    // sanitized parent (folded) -> folded names already used in it
    std::unordered_map<std::u16string, std::set<std::u16string>> used;

    for (const auto &e : entries) {
      const auto slash = e.path.rfind('/');
      const std::string leaf = slash == std::string::npos ? e.path : e.path.substr(slash + 1);
      std::string parent_sanitized;
      if (slash != std::string::npos) {
        const auto it = dir_map.find(fold_key(e.path.substr(0, slash)));
        if (it != dir_map.end()) {
          parent_sanitized = it->second;
        } else {
          // Parent not declared (unvalidated input): sanitize its components directly.
          std::size_t start = 0;
          const auto parent = e.path.substr(0, slash);
          while (start <= parent.size()) {
            const auto s = parent.find('/', start);
            const auto comp = parent.substr(start, s == std::string::npos ? std::string::npos : s - start);
            if (!parent_sanitized.empty()) {
              parent_sanitized.push_back('/');
            }
            parent_sanitized += sanitize_component(comp);
            if (s == std::string::npos) {
              break;
            }
            start = s + 1;
          }
        }
      }

      auto name = sanitize_component(leaf);
      auto &names = used[fold_key(parent_sanitized)];
      if (names.contains(fold_key(name))) {
        const auto dot = name.rfind('.');
        const bool has_ext = dot != std::string::npos && dot > 0;
        const auto stem = has_ext ? name.substr(0, dot) : name;
        const auto ext = has_ext ? name.substr(dot) : std::string();
        for (int n = 2;; ++n) {
          const auto tail = " (" + std::to_string(n) + ")" + ext;
          auto candidate = truncate_utf16(stem, max_component_utf16 - std::min(utf16_len(tail), max_component_utf16)) + tail;
          if (!names.contains(fold_key(candidate))) {
            name = std::move(candidate);
            break;
          }
        }
      }
      names.insert(fold_key(name));

      const auto full = parent_sanitized.empty() ? name : parent_sanitized + "/" + name;
      if (e.kind == entry_kind::directory) {
        dir_map[fold_key(e.path)] = full;
      }
      auto win = full;
      std::replace(win.begin(), win.end(), '/', '\\');
      result.push_back(std::move(win));
    }
    return result;
  }

  std::size_t utf16_length(std::string_view s) {
    return utf16_len(s);
  }

  std::string offer_id_hex(const offer_id_t &id) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (const auto b : id) {
      out.push_back(digits[b >> 4]);
      out.push_back(digits[b & 0xF]);
    }
    return out;
  }

  std::optional<offer_id_t> parse_offer_id_hex(std::string_view hex) {
    if (hex.size() != 32) {
      return std::nullopt;
    }
    const auto nibble = [](char c) -> int {
      if (c >= '0' && c <= '9') {
        return c - '0';
      }
      if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
      }
      if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
      }
      return -1;
    };
    offer_id_t id {};
    for (std::size_t i = 0; i < 16; ++i) {
      const int hi = nibble(hex[2 * i]);
      const int lo = nibble(hex[2 * i + 1]);
      if (hi < 0 || lo < 0) {
        return std::nullopt;
      }
      id[i] = static_cast<std::uint8_t>((hi << 4) | lo);
    }
    return id;
  }

  const char *error_name(manifest_error e) {
    switch (e) {
      case manifest_error::none:
        return "none";
      case manifest_error::bad_magic:
        return "bad_magic";
      case manifest_error::bad_version:
        return "bad_version";
      case manifest_error::truncated:
        return "truncated";
      case manifest_error::empty:
        return "empty";
      case manifest_error::too_many_entries:
        return "too_many_entries";
      case manifest_error::path_too_long:
        return "path_too_long";
      case manifest_error::component_too_long:
        return "component_too_long";
      case manifest_error::bad_component:
        return "bad_component";
      case manifest_error::absolute_path:
        return "absolute_path";
      case manifest_error::backslash:
        return "backslash";
      case manifest_error::nul:
        return "nul";
      case manifest_error::missing_parent:
        return "missing_parent";
      case manifest_error::duplicate_path:
        return "duplicate_path";
      case manifest_error::trailing_bytes:
        return "trailing_bytes";
    }
    return "unknown";
  }
}  // namespace clipboard::files
