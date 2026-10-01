/**
 * @file src/clipboard/bundle.cpp
 * @brief Wire container ("MLCB" v1) for clipboard sync between host and client.
 */
#include "bundle.h"

#include <bitset>

namespace clipboard {
  namespace {
    constexpr std::string_view k_magic = "MLCB";
    constexpr std::uint8_t k_version = 1;
    constexpr std::size_t k_header_size = 6;
    constexpr std::size_t k_item_header_size = 5;

    std::uint32_t read_le32(const char *p) {
      const auto *u = reinterpret_cast<const unsigned char *>(p);
      return std::uint32_t(u[0]) | (std::uint32_t(u[1]) << 8) | (std::uint32_t(u[2]) << 16) | (std::uint32_t(u[3]) << 24);
    }
  }  // namespace

  std::uint32_t mask_of(item_type type) {
    return 1u << (static_cast<std::uint8_t>(type) - 1u);
  }

  std::uint32_t mask_of(const std::vector<item> &items) {
    std::uint32_t mask = 0;
    for (const auto &entry : items) {
      mask |= mask_of(entry.type);
    }
    return mask;
  }

  std::size_t encoded_size(const std::vector<item> &items) {
    std::size_t size = k_header_size;
    for (const auto &entry : items) {
      size += k_item_header_size + entry.data.size();
    }
    return size;
  }

  std::string encode(const std::vector<item> &items) {
    std::string out;
    out.reserve(encoded_size(items));
    out.append(k_magic);
    out.push_back(static_cast<char>(k_version));
    out.push_back(static_cast<char>(items.size()));
    for (const auto &entry : items) {
      out.push_back(static_cast<char>(static_cast<std::uint8_t>(entry.type)));
      const auto length = static_cast<std::uint32_t>(entry.data.size());
      for (int shift = 0; shift < 32; shift += 8) {
        out.push_back(static_cast<char>((length >> shift) & 0xFF));
      }
      out.append(entry.data);
    }
    return out;
  }

  decode_result decode(std::string_view data, std::size_t max_bytes) {
    decode_result result;
    auto fail = [&result](decode_error error) {
      result.error = error;
      result.items.clear();
      return result;
    };
    if (data.size() > max_bytes) {
      return fail(decode_error::too_large);
    }
    if (data.size() < k_header_size) {
      return fail(decode_error::truncated);
    }
    if (data.substr(0, k_magic.size()) != k_magic) {
      return fail(decode_error::bad_magic);
    }
    if (static_cast<std::uint8_t>(data[4]) != k_version) {
      return fail(decode_error::bad_version);
    }
    const std::size_t count = static_cast<std::uint8_t>(data[5]);
    if (count > max_items) {
      return fail(decode_error::too_many_items);
    }
    std::size_t pos = k_header_size;
    std::bitset<256> seen;
    for (std::size_t i = 0; i < count; ++i) {
      if (data.size() - pos < k_item_header_size) {
        return fail(decode_error::truncated);
      }
      const auto type = static_cast<std::uint8_t>(data[pos]);
      const std::uint32_t length = read_le32(data.data() + pos + 1);
      pos += k_item_header_size;
      if (length > data.size() - pos) {
        return fail(decode_error::truncated);
      }
      if (seen.test(type)) {
        return fail(decode_error::duplicate_type);
      }
      seen.set(type);
      if (type >= 1 && type <= 4) {
        result.items.push_back({static_cast<item_type>(type), std::string(data.substr(pos, length))});
      }
      pos += length;
    }
    if (pos != data.size()) {
      return fail(decode_error::trailing_bytes);
    }
    return result;
  }

  const char *error_name(decode_error error) {
    switch (error) {
      case decode_error::none:
        return "ok";
      case decode_error::bad_magic:
        return "bad_magic";
      case decode_error::bad_version:
        return "bad_version";
      case decode_error::too_many_items:
        return "too_many_items";
      case decode_error::truncated:
        return "truncated";
      case decode_error::trailing_bytes:
        return "trailing_bytes";
      case decode_error::duplicate_type:
        return "duplicate_type";
      case decode_error::too_large:
        return "too_large";
    }
    return "unknown";
  }
}  // namespace clipboard
