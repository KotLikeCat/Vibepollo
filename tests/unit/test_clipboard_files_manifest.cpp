/**
 * @file tests/unit/test_clipboard_files_manifest.cpp
 * @brief Test src/clipboard/files/manifest.*
 */
#include "../tests_common.h"
#include "src/clipboard/files/manifest.h"

#include <fstream>
#include <map>
#include <sstream>

using namespace clipboard::files;

namespace {
  std::string from_hex(std::string_view hex) {
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
      out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return out;
  }

  std::map<std::string, std::string> load_vectors() {
    std::ifstream in(CLIPBOARD_FILES_VECTORS_PATH);
    std::map<std::string, std::string> cases;
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty() || line[0] == '#') {
        continue;
      }
      std::istringstream fields(line);
      std::string name;
      std::string hex;
      fields >> name >> hex;
      cases[name] = from_hex(hex);
    }
    return cases;
  }

  offer_id_t sample_id() {
    offer_id_t id {};
    for (std::size_t i = 0; i < id.size(); ++i) {
      id[i] = static_cast<std::uint8_t>(i);
    }
    return id;
  }

  manifest make(std::vector<entry> entries) {
    manifest m;
    m.offer_id = sample_id();
    m.entries = std::move(entries);
    return m;
  }

  entry file(std::string path, std::uint64_t size = 1) {
    return {entry_kind::file, size, 0, std::move(path)};
  }

  entry dir(std::string path) {
    return {entry_kind::directory, 0, 0, std::move(path)};
  }

  manifest_error decode_err(const manifest &m) {
    return decode_manifest(encode_manifest(m)).error;
  }

  std::vector<std::string> sanitize_paths(std::vector<std::string> paths) {
    std::vector<entry> entries;
    for (auto &p : paths) {
      entries.push_back(file(std::move(p)));
    }
    return sanitize_for_windows(entries);
  }
}  // namespace

TEST(ClipboardFilesManifest, DecodesSharedVectors) {
  const auto cases = load_vectors();
  ASSERT_EQ(cases.size(), 3u);
  for (const auto &[name, data] : cases) {
    const auto r = decode_manifest(data);
    ASSERT_EQ(r.error, manifest_error::none) << name;
    EXPECT_EQ(r.value.offer_id, sample_id()) << name;
    EXPECT_EQ(encode_manifest(r.value), data) << name;
  }
  const auto single = decode_manifest(cases.at("single_file")).value;
  ASSERT_EQ(single.entries.size(), 1u);
  EXPECT_EQ(single.entries[0].kind, entry_kind::file);
  EXPECT_EQ(single.entries[0].size, 5u);
  EXPECT_EQ(single.entries[0].mtime_ms, 1700000000000);
  EXPECT_EQ(single.entries[0].path, "a.txt");

  const auto tree = decode_manifest(cases.at("cyrillic_tree")).value;
  ASSERT_EQ(tree.entries.size(), 3u);
  EXPECT_EQ(tree.entries[0].kind, entry_kind::directory);
  EXPECT_EQ(tree.entries[0].path, "Папка");
  EXPECT_EQ(tree.entries[1].path, "Папка/файл й.txt");
  EXPECT_EQ(tree.entries[1].size, 1048576u);
  EXPECT_EQ(tree.entries[1].mtime_ms, 1700000000123);
  EXPECT_EQ(tree.entries[2].path, "b.bin");
  EXPECT_EQ(tree.entries[2].size, 0u);
  EXPECT_EQ(tree.entries[2].mtime_ms, 0);

  const auto big = decode_manifest(cases.at("over_4gib")).value;
  ASSERT_EQ(big.entries.size(), 1u);
  EXPECT_EQ(big.entries[0].size, 5000000000u);
  EXPECT_EQ(big.entries[0].mtime_ms, 1);
  EXPECT_EQ(big.entries[0].path, "big.iso");
}

TEST(ClipboardFilesManifest, RejectsEmptyOffer) {
  EXPECT_EQ(decode_err(make({})), manifest_error::empty);
}

TEST(ClipboardFilesManifest, RejectsBadMagic) {
  auto data = load_vectors().at("single_file");
  data[0] = 'X';
  EXPECT_EQ(decode_manifest(data).error, manifest_error::bad_magic);
}

TEST(ClipboardFilesManifest, RejectsBadVersion) {
  auto data = load_vectors().at("single_file");
  data[4] = 2;
  EXPECT_EQ(decode_manifest(data).error, manifest_error::bad_version);
}

TEST(ClipboardFilesManifest, RejectsTruncatedEntry) {
  auto data = load_vectors().at("single_file");
  data.pop_back();
  EXPECT_EQ(decode_manifest(data).error, manifest_error::truncated);
}

TEST(ClipboardFilesManifest, RejectsTrailingBytes) {
  auto data = load_vectors().at("single_file");
  data.push_back('x');
  EXPECT_EQ(decode_manifest(data).error, manifest_error::trailing_bytes);
}

TEST(ClipboardFilesManifest, RejectsPathRules) {
  EXPECT_EQ(decode_err(make({file("/a")})), manifest_error::absolute_path);
  EXPECT_EQ(decode_err(make({file("a\\b")})), manifest_error::backslash);
  EXPECT_EQ(decode_err(make({dir("a"), file("a/../b")})), manifest_error::bad_component);
  EXPECT_EQ(decode_err(make({dir("a"), file("a//b")})), manifest_error::bad_component);
  EXPECT_EQ(decode_err(make({file(".")})), manifest_error::bad_component);
  EXPECT_EQ(decode_err(make({file(std::string("a\0b", 3))})), manifest_error::nul);
  EXPECT_EQ(decode_err(make({file("x/y")})), manifest_error::missing_parent);
  EXPECT_EQ(decode_err(make({file("A.txt"), file("a.txt")})), manifest_error::duplicate_path);
  EXPECT_EQ(decode_err(make({file(std::string(1025, 'a'))})), manifest_error::path_too_long);
  EXPECT_EQ(decode_err(make({file(std::string(256, 'a'))})), manifest_error::component_too_long);
  EXPECT_EQ(decode_err(make({file(std::string(255, 'a'))})), manifest_error::none);
}

TEST(ClipboardFilesManifest, RejectsTooManyEntries) {
  auto data = encode_manifest(make({file("a")}));
  const std::uint32_t n = 100001;
  for (int i = 0; i < 4; ++i) {
    data[21 + i] = static_cast<char>((n >> (8 * i)) & 0xFF);
  }
  EXPECT_EQ(decode_manifest(data).error, manifest_error::too_many_entries);
}

TEST(ClipboardFilesManifest, SanitizerCases) {
  const auto out = sanitize_paths({"CON.txt", "nul", "com1.log", "a:b?.txt", "name. ", "Папка/файл й.txt"});
  // "Папка/файл й.txt" has no declared parent here; its components are sanitized directly.
  ASSERT_EQ(out.size(), 6u);
  EXPECT_EQ(out[0], "CON_.txt");
  EXPECT_EQ(out[1], "nul_");
  EXPECT_EQ(out[2], "com1_.log");
  EXPECT_EQ(out[3], "a_b_.txt");
  EXPECT_EQ(out[4], "name");
  EXPECT_EQ(out[5], "Папка\\файл й.txt");

  // Collisions.
  const auto col = sanitize_paths({"a?.txt", "a_.txt"});
  EXPECT_EQ(col[0], "a_.txt");
  EXPECT_EQ(col[1], "a_ (2).txt");

  // Renamed directory propagates to children.
  const auto tree = sanitize_for_windows({dir("d:x"), file("d:x/f?.txt"), file("d:x/f_.txt")});
  EXPECT_EQ(tree[0], "d_x");
  EXPECT_EQ(tree[1], "d_x\\f_.txt");
  EXPECT_EQ(tree[2], "d_x\\f_ (2).txt");
}

TEST(ClipboardFilesManifest, TotalSizeAndHex) {
  const auto cases = load_vectors();
  const auto tree = decode_manifest(cases.at("cyrillic_tree")).value;
  EXPECT_EQ(total_size(tree), 1048576u);

  const auto id = sample_id();
  EXPECT_EQ(offer_id_hex(id), "000102030405060708090a0b0c0d0e0f");
  const auto parsed = parse_offer_id_hex("000102030405060708090a0b0c0d0e0f");
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(*parsed, id);
  EXPECT_FALSE(parse_offer_id_hex("0001").has_value());
  EXPECT_FALSE(parse_offer_id_hex("zz0102030405060708090a0b0c0d0e0f").has_value());
}

TEST(ClipboardFilesManifest, UnicodeCaseInsensitive) {
  EXPECT_EQ(decode_err(make({dir("Папка"), file("папка")})), manifest_error::duplicate_path);
  EXPECT_EQ(decode_err(make({dir("папка"), file("Папка/x")})), manifest_error::none);
  const auto out = sanitize_paths({"Ж.txt", "ж.txt"});
  EXPECT_EQ(out[0], "Ж.txt");
  EXPECT_EQ(out[1], "ж (2).txt");
}

TEST(ClipboardFilesManifest, ReservedNamesAndLength) {
  const auto out = sanitize_paths({"CONSOLE.txt", "Con.tar.gz"});
  EXPECT_EQ(out[0], "CONSOLE.txt");
  EXPECT_EQ(out[1], "Con_.tar.gz");
  const auto longname = std::string(251, 'a') + ".txt";  // 255 units
  const auto col = sanitize_paths({longname, longname + ""});
  EXPECT_EQ(col[0], longname);
  EXPECT_LE(col[1].size(), 255u);
  EXPECT_NE(col[1], col[0]);
  EXPECT_EQ(col[1].substr(col[1].size() - 4), ".txt");
  const auto con = sanitize_paths({"con" + std::string(248, 'b') + ".c"});  // no reserved; sanity
  EXPECT_LE(con[0].size(), 255u);
  const auto res = sanitize_paths({"con." + std::string(251, 'x')});
  EXPECT_LE(res[0].size(), 255u);
}

TEST(ClipboardFilesManifest, RejectsMalformedEntries) {
  auto data = encode_manifest(make({file("a.txt")}));
  auto bad_kind = data;
  bad_kind[25] = 9;
  EXPECT_EQ(decode_manifest(bad_kind).error, manifest_error::bad_component);
  EXPECT_EQ(decode_err(make({file("a\xff.txt")})), manifest_error::bad_component);
  EXPECT_EQ(decode_err(make({file("a\xc3.txt")})), manifest_error::bad_component);
  auto past_end = data;
  past_end[25 + 1 + 8 + 8] = 100;  // path_len low byte
  EXPECT_EQ(decode_manifest(past_end).error, manifest_error::truncated);
  EXPECT_NE(decode_manifest(std::string(max_manifest_bytes + 1, 'x')).error, manifest_error::none);
}
