/**
 * @file tests/unit/platform/windows/test_clipboard_agent.cpp
 * @brief Tests of tools/clipboard_agent (OLE data object, streams, prefetch) with a fake range source.
 *
 * ShellCopiesFolderTree emulates what Explorer does on paste (descriptor + per-index FILECONTENTS streams, or
 * CF_HDROP) through OleSetClipboard/OleGetClipboard. Driving the real Explorer paste (IFileOperation with a data
 * object source) is not practical headless; that path is covered by the end-to-end checklist.
 */
#include "../../../tests_common.h"
#include "src/clipboard/files/agent_protocol.h"
#include "src/clipboard/files/manifest.h"
#include "src/platform/windows/utf_utils.h"
#include "tools/clipboard_agent/data_object.h"
#include "tools/clipboard_agent/file_stream.h"
#include "tools/clipboard_agent/pipe_range_source.h"
#include "tools/clipboard_agent/prefetch.h"

#include <objbase.h>
#include <ole2.h>
#include <shlobj.h>
#include <windows.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <thread>

namespace {
  namespace fs = std::filesystem;
  namespace cf = clipboard::files;
  using namespace clipboard_agent;

  constexpr const char *k_cyrillic_tree =
    "4d4c434601000102030405060708090a0b0c0d0e0f030000000200000000000000000068e5cf8b0100000a00d09fd0b0d0bfd0bad0b00100001000000000007b68e5cf8b0100001a00d09fd0b0d0bfd0bad0b02fd184d0b0d0b9d0bb20d0b92e74787401000000000000000000000000000000000500622e62696e";

  std::string from_hex(std::string_view hex) {
    std::string out;
    for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
      out.push_back(static_cast<char>(std::stoi(std::string(hex.substr(i, 2)), nullptr, 16)));
    }
    return out;
  }

  char expected_byte(std::uint32_t file_index, std::uint64_t i) {
    return static_cast<char>((file_index * 31 + i) & 0xff);
  }

  std::string expected_bytes(std::uint32_t file_index, std::uint64_t off, std::uint64_t len) {
    std::string s(len, '\0');
    for (std::uint64_t i = 0; i < len; ++i) {
      s[i] = expected_byte(file_index, off + i);
    }
    return s;
  }

  class fake_source: public range_source {
  public:
    read_result read(std::uint32_t idx, std::uint64_t off, std::uint32_t len, std::string &out) override {
      ++calls;
      if (delay_ms > 0) {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms));
      }
      if (fail) {
        return {false, *fail};
      }
      out = expected_bytes(idx, off, len);
      return {true, cf::read_error::io};
    }

    std::atomic<int> calls {0};
    int delay_ms {0};
    std::optional<cf::read_error> fail;
  };

  offer make_offer(const cf::manifest &m, bool prefetch) {
    offer o;
    o.id = m.offer_id;
    o.entries = m.entries;
    o.windows_paths = cf::sanitize_for_windows(m.entries);
    o.prefetch = prefetch;
    return o;
  }

  offer cyrillic_offer(bool prefetch) {
    auto d = cf::decode_manifest(from_hex(k_cyrillic_tree));
    EXPECT_EQ(d.error, cf::manifest_error::none);
    return make_offer(d.value, prefetch);
  }

  fs::path unique_temp_root(const char *tag) {
    static std::atomic<int> n {0};
    return fs::temp_directory_path() / (std::string("vibepollo-agent-test-") + tag + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++n));
  }

  struct release_medium {
    STGMEDIUM m {};
    ~release_medium() {
      ReleaseStgMedium(&m);
    }
  };

  FORMATETC fmt_of(const char *name, DWORD tymed, LONG lindex = -1) {
    FORMATETC f {};
    f.cfFormat = static_cast<CLIPFORMAT>(RegisterClipboardFormatA(name));
    f.dwAspect = DVASPECT_CONTENT;
    f.lindex = lindex;
    f.tymed = tymed;
    return f;
  }

  std::string read_all(IStream *s, ULONG chunk) {
    std::string out;
    std::string buf(chunk, '\0');
    for (;;) {
      ULONG got = 0;
      const HRESULT hr = s->Read(buf.data(), chunk, &got);
      EXPECT_TRUE(SUCCEEDED(hr));
      if (FAILED(hr) || got == 0) {
        break;
      }
      out.append(buf.data(), got);
    }
    return out;
  }

  std::vector<std::wstring> descriptor_names(IDataObject *obj, std::vector<FILEDESCRIPTORW> *out = nullptr) {
    auto f = fmt_of(CFSTR_FILEDESCRIPTORW, TYMED_HGLOBAL);
    release_medium med;
    EXPECT_EQ(obj->GetData(&f, &med.m), S_OK);
    std::vector<std::wstring> names;
    auto *g = static_cast<FILEGROUPDESCRIPTORW *>(GlobalLock(med.m.hGlobal));
    if (g == nullptr) {
      return names;
    }
    for (UINT i = 0; i < g->cItems; ++i) {
      names.emplace_back(g->fgd[i].cFileName);
      if (out) {
        out->push_back(g->fgd[i]);
      }
    }
    GlobalUnlock(med.m.hGlobal);
    return names;
  }

  struct obj_ptr {
    IDataObject *p;

    explicit obj_ptr(IDataObject *x):
        p(x) {}

    ~obj_ptr() {
      if (p) {
        p->Release();
      }
    }

    IDataObject *operator->() const {
      return p;
    }
  };

  class ClipboardAgent: public ::testing::Test {
  protected:
    void SetUp() override {
      ole_ = SUCCEEDED(OleInitialize(nullptr));
    }

    void TearDown() override {
      if (ole_) {
        OleUninitialize();
      }
    }

    bool ole_ {false};
  };
}  // namespace

TEST_F(ClipboardAgent, DescriptorListsEntries) {
  auto src = std::make_shared<fake_source>();
  obj_ptr obj(create_data_object(cyrillic_offer(false), src, unique_temp_root("desc")));
  std::vector<FILEDESCRIPTORW> d;
  const auto names = descriptor_names(obj.p, &d);
  ASSERT_EQ(names.size(), 3u);
  EXPECT_EQ(names[0], L"Папка");
  EXPECT_EQ(names[1], L"Папка\\файл й.txt");
  EXPECT_EQ(names[2], L"b.bin");
  EXPECT_TRUE(d[0].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
  EXPECT_FALSE(d[1].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY);
  EXPECT_EQ(d[1].nFileSizeLow, 1048576u);
  EXPECT_EQ(d[1].nFileSizeHigh, 0u);
  EXPECT_EQ(d[2].nFileSizeLow, 0u);
  const DWORD want = FD_ATTRIBUTES | FD_FILESIZE | FD_WRITESTIME | FD_PROGRESSUI;
  EXPECT_EQ(d[1].dwFlags & want, want);
  const auto ft = filetime_from_unix_ms(1700000000123);
  EXPECT_EQ(d[1].ftLastWriteTime.dwLowDateTime, ft.dwLowDateTime);
  EXPECT_EQ(d[1].ftLastWriteTime.dwHighDateTime, ft.dwHighDateTime);

  auto drop = fmt_of(CFSTR_PREFERREDDROPEFFECT, TYMED_HGLOBAL);
  release_medium med;
  ASSERT_EQ(obj->GetData(&drop, &med.m), S_OK);
  EXPECT_EQ(*static_cast<DWORD *>(GlobalLock(med.m.hGlobal)), static_cast<DWORD>(DROPEFFECT_COPY));
  GlobalUnlock(med.m.hGlobal);

  FORMATETC hdrop {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  EXPECT_EQ(obj->QueryGetData(&hdrop), DV_E_FORMATETC);
  auto unknown = fmt_of("Vibepollo.Test.Unknown", TYMED_HGLOBAL);
  EXPECT_EQ(obj->QueryGetData(&unknown), DV_E_FORMATETC);

  IEnumFORMATETC *en = nullptr;
  ASSERT_EQ(obj->EnumFormatEtc(DATADIR_GET, &en), S_OK);
  FORMATETC f;
  ULONG fetched = 0;
  int count = 0;
  while (en->Next(1, &f, &fetched) == S_OK) {
    ++count;
  }
  en->Release();
  EXPECT_EQ(count, 3);

  // SetData: only the performed-drop-effect / paste-succeeded feedback is accepted.
  auto performed = fmt_of(CFSTR_PERFORMEDDROPEFFECT, TYMED_HGLOBAL);
  STGMEDIUM in {};
  const DWORD effect = DROPEFFECT_COPY;
  in.tymed = TYMED_HGLOBAL;
  in.hGlobal = GlobalAlloc(GMEM_MOVEABLE, sizeof(DWORD));
  std::memcpy(GlobalLock(in.hGlobal), &effect, sizeof(effect));
  GlobalUnlock(in.hGlobal);
  EXPECT_EQ(obj->SetData(&performed, &in, TRUE), S_OK);
  STGMEDIUM other = in;
  EXPECT_EQ(obj->SetData(&drop, &other, FALSE), E_NOTIMPL);
}

TEST_F(ClipboardAgent, FileContentsStreamsBytes) {
  auto src = std::make_shared<fake_source>();
  obj_ptr obj(create_data_object(cyrillic_offer(false), src, unique_temp_root("contents")));
  const auto want = expected_bytes(1, 0, 1048576);
  for (ULONG chunk : {1u << 20, 4095u, 1u}) {
    auto f = fmt_of(CFSTR_FILECONTENTS, TYMED_ISTREAM, 1);
    release_medium med;
    ASSERT_EQ(obj->GetData(&f, &med.m), S_OK);
    ASSERT_EQ(med.m.tymed, static_cast<DWORD>(TYMED_ISTREAM));
    IStream *s = med.m.pstm;
    STATSTG st {};
    ASSERT_EQ(s->Stat(&st, STATFLAG_DEFAULT), S_OK);
    EXPECT_EQ(st.cbSize.QuadPart, 1048576u);
    EXPECT_STREQ(st.pwcsName, L"файл й.txt");
    CoTaskMemFree(st.pwcsName);
    if (chunk == 1) {
      // read just a prefix with 1-byte reads; the full compare uses the larger chunks
      std::string head;
      char c;
      for (int i = 0; i < 2000; ++i) {
        ULONG got = 0;
        ASSERT_EQ(s->Read(&c, 1, &got), S_OK);
        ASSERT_EQ(got, 1u);
        head.push_back(c);
      }
      EXPECT_EQ(head, want.substr(0, 2000));
    } else {
      EXPECT_EQ(read_all(s, chunk) == want, true);
    }
    // seek to the middle and read
    LARGE_INTEGER mv;
    mv.QuadPart = 600000;
    ULARGE_INTEGER np {};
    ASSERT_EQ(s->Seek(mv, STREAM_SEEK_SET, &np), S_OK);
    EXPECT_EQ(np.QuadPart, 600000u);
    std::string buf(1000, '\0');
    ULONG got = 0;
    ASSERT_EQ(s->Read(buf.data(), 1000, &got), S_OK);
    EXPECT_EQ(got, 1000u);
    EXPECT_EQ(buf, want.substr(600000, 1000));
    mv.QuadPart = -10;
    ASSERT_EQ(s->Seek(mv, STREAM_SEEK_END, &np), S_OK);
    EXPECT_EQ(read_all(s, 4096), want.substr(want.size() - 10));
  }
  // empty file
  auto f = fmt_of(CFSTR_FILECONTENTS, TYMED_ISTREAM, 2);
  release_medium med;
  ASSERT_EQ(obj->GetData(&f, &med.m), S_OK);
  EXPECT_EQ(read_all(med.m.pstm, 100), "");
}

TEST_F(ClipboardAgent, DirectoryContentsRejected) {
  auto src = std::make_shared<fake_source>();
  obj_ptr obj(create_data_object(cyrillic_offer(false), src, unique_temp_root("dir")));
  auto dir = fmt_of(CFSTR_FILECONTENTS, TYMED_ISTREAM, 0);
  STGMEDIUM med {};
  EXPECT_EQ(obj->GetData(&dir, &med), DV_E_LINDEX);
  EXPECT_EQ(obj->QueryGetData(&dir), DV_E_LINDEX);
  auto oob = fmt_of(CFSTR_FILECONTENTS, TYMED_ISTREAM, 99);
  EXPECT_EQ(obj->GetData(&oob, &med), DV_E_LINDEX);
  auto any = fmt_of(CFSTR_FILECONTENTS, TYMED_ISTREAM, -1);
  EXPECT_EQ(obj->QueryGetData(&any), S_OK);
}

TEST_F(ClipboardAgent, ErrorBecomesReadFault) {
  auto src = std::make_shared<fake_source>();
  src->fail = cf::read_error::changed;
  obj_ptr obj(create_data_object(cyrillic_offer(false), src, unique_temp_root("err")));
  auto f = fmt_of(CFSTR_FILECONTENTS, TYMED_ISTREAM, 1);
  release_medium med;
  ASSERT_EQ(obj->GetData(&f, &med.m), S_OK);
  char c[16];
  ULONG got = 99;
  EXPECT_EQ(med.m.pstm->Read(c, sizeof(c), &got), STG_E_READFAULT);
  EXPECT_EQ(got, 0u);
}

TEST_F(ClipboardAgent, StreamReleasedMidReadDoesNotHang) {
  auto src = std::make_shared<fake_source>();
  src->delay_ms = 300;
  obj_ptr obj(create_data_object(cyrillic_offer(false), src, unique_temp_root("rel")));
  auto f = fmt_of(CFSTR_FILECONTENTS, TYMED_ISTREAM, 1);
  STGMEDIUM med {};
  ASSERT_EQ(obj->GetData(&f, &med), S_OK);
  const auto t0 = std::chrono::steady_clock::now();
  med.pstm->Release();  // worker is mid-read; the destructor must cancel and join
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(5));
}

TEST_F(ClipboardAgent, HdropAfterPrefetch) {
  const auto root = unique_temp_root("hdrop");
  const auto folder = root / cf::offer_id_hex(cyrillic_offer(true).id);
  auto src = std::make_shared<fake_source>();
  {
    obj_ptr obj(create_data_object(cyrillic_offer(true), src, root));
    FORMATETC hdrop {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
    ASSERT_EQ(obj->QueryGetData(&hdrop), S_OK);
    release_medium med;
    ASSERT_EQ(obj->GetData(&hdrop, &med.m), S_OK);
    auto *df = static_cast<DROPFILES *>(GlobalLock(med.m.hGlobal));
    ASSERT_NE(df, nullptr);
    EXPECT_TRUE(df->fWide);
    std::vector<std::wstring> paths;
    for (const wchar_t *p = reinterpret_cast<const wchar_t *>(reinterpret_cast<const char *>(df) + df->pFiles); *p; p += wcslen(p) + 1) {
      paths.emplace_back(p);
    }
    GlobalUnlock(med.m.hGlobal);
    ASSERT_EQ(paths.size(), 2u);
    EXPECT_EQ(fs::path(paths[0]), (folder / L"Папка"));
    EXPECT_EQ(fs::path(paths[1]), (folder / L"b.bin"));
    EXPECT_TRUE(fs::is_directory(paths[0]));
    EXPECT_EQ(fs::file_size(paths[1]), 0u);
    std::ifstream in(fs::path(paths[0]) / L"файл й.txt", std::ios::binary);
    std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    EXPECT_EQ(got == expected_bytes(1, 0, 1048576), true);

    // once prefetched, contents are served from disk (no further source reads)
    const int calls = src->calls.load();
    auto f = fmt_of(CFSTR_FILECONTENTS, TYMED_ISTREAM, 1);
    release_medium sm;
    ASSERT_EQ(obj->GetData(&f, &sm.m), S_OK);
    EXPECT_EQ(read_all(sm.m.pstm, 65536) == expected_bytes(1, 0, 1048576), true);
    EXPECT_EQ(src->calls.load(), calls);
  }
  EXPECT_FALSE(fs::exists(folder));  // prefetch folder deleted with the data object

  // large offer: no CF_HDROP
  obj_ptr big(create_data_object(cyrillic_offer(false), src, root));
  FORMATETC hdrop {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  EXPECT_EQ(big->QueryGetData(&hdrop), DV_E_FORMATETC);
  STGMEDIUM med {};
  EXPECT_EQ(big->GetData(&hdrop, &med), DV_E_FORMATETC);
}

TEST_F(ClipboardAgent, PrefetchFailureFailsHdrop) {
  auto src = std::make_shared<fake_source>();
  src->fail = cf::read_error::gone;
  const auto root = unique_temp_root("pfail");
  obj_ptr obj(create_data_object(cyrillic_offer(true), src, root));
  FORMATETC hdrop {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  STGMEDIUM med {};
  EXPECT_EQ(obj->GetData(&hdrop, &med), E_FAIL);
}

TEST_F(ClipboardAgent, OverlongPathsAreLeftOutOfDescriptor) {
  cf::manifest m;
  m.offer_id = {1};
  m.entries.push_back({cf::entry_kind::file, 3, 0, "short.txt"});
  std::string long_name;
  for (int i = 0; i < 4; ++i) {  // 4 x 100 chars + separators > 259
    long_name += std::string(100, 'x') + (i < 3 ? "/" : "");
  }
  m.entries.push_back({cf::entry_kind::file, 3, 0, long_name});
  m.entries.push_back({cf::entry_kind::file, 3, 0, "tail.txt"});
  const auto o = make_offer(m, false);
  EXPECT_EQ(descriptor_omitted_count(o), 1u);
  auto src = std::make_shared<fake_source>();
  obj_ptr obj(create_data_object(o, src, unique_temp_root("long")));
  const auto names = descriptor_names(obj.p);
  ASSERT_EQ(names.size(), 2u);
  EXPECT_EQ(names[1], L"tail.txt");
  // descriptor index 1 maps to entry 2: bytes must match file_index 2
  auto f = fmt_of(CFSTR_FILECONTENTS, TYMED_ISTREAM, 1);
  release_medium med;
  ASSERT_EQ(obj->GetData(&f, &med.m), S_OK);
  EXPECT_EQ(read_all(med.m.pstm, 100), expected_bytes(2, 0, 3));
}

TEST_F(ClipboardAgent, PipeRangeSourceRoundTrip) {
  namespace ag = cf::agent;
  std::shared_ptr<pipe_range_source> ps;
  std::vector<std::thread> workers;
  std::mutex wm;
  std::atomic<int> cancels {0};
  ps = std::make_shared<pipe_range_source>([&](const std::string &frame) {
    auto m = ag::decode(frame);
    if (!m) {
      return false;
    }
    if (m->type == ag::msg::cancel_read) {
      ++cancels;
      return true;
    }
    auto rr = ag::decode_read_range(m->payload);
    if (!rr) {
      return false;
    }
    std::lock_guard lock(wm);
    workers.emplace_back([&ps, r = *rr] {
      if (r.file_index == 9) {
        ps->on_range_error({r.read_id, cf::read_error::gone});
        return;
      }
      if (r.file_index == 8) {
        return;  // never answered
      }
      const auto all = expected_bytes(r.file_index, r.offset, r.length);
      for (std::size_t off = 0; off < all.size(); off += 1000) {
        const auto n = std::min<std::size_t>(1000, all.size() - off);
        ps->on_range_data({r.read_id, off + n == all.size(), all.substr(off, n)});
      }
    });
    return true;
  });
  std::string out;
  auto rr = ps->read(3, 17, 2500, out);
  EXPECT_TRUE(rr.ok);
  EXPECT_EQ(out, expected_bytes(3, 17, 2500));
  rr = ps->read(9, 0, 10, out);
  EXPECT_FALSE(rr.ok);
  EXPECT_EQ(rr.error, cf::read_error::gone);
  auto tok = std::make_shared<cancel_token>();
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    tok->cancel();
  });
  rr = ps->read_cancellable(8, 0, 10, out, tok);
  canceller.join();
  EXPECT_FALSE(rr.ok);
  EXPECT_EQ(cancels.load(), 1);
  {
    std::lock_guard lock(wm);
    for (auto &w : workers) {
      w.join();
    }
  }
}

namespace {
  /// Emulates the shell side of a paste from the OLE clipboard into `dest`.
  void paste_virtual_files(IDataObject *src, const fs::path &dest) {
    std::vector<FILEDESCRIPTORW> d;
    const auto names = descriptor_names(src, &d);
    for (std::size_t i = 0; i < names.size(); ++i) {
      const fs::path target = dest / fs::path(names[i]);
      if (d[i].dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
        fs::create_directories(target);
        continue;
      }
      fs::create_directories(target.parent_path());
      auto f = fmt_of(CFSTR_FILECONTENTS, TYMED_ISTREAM, static_cast<LONG>(i));
      release_medium med;
      ASSERT_EQ(src->GetData(&f, &med.m), S_OK);
      std::ofstream out(target, std::ios::binary);
      out << read_all(med.m.pstm, 65536);
    }
  }

  cf::manifest tree_of_50() {
    cf::manifest m;
    m.offer_id = {7};
    for (int d = 0; d < 5; ++d) {
      const std::string dir = "dir" + std::to_string(d);
      m.entries.push_back({cf::entry_kind::directory, 0, 1700000000000, dir});
      m.entries.push_back({cf::entry_kind::directory, 0, 1700000000000, dir + "/sub"});
      for (int f = 0; f < 10; ++f) {
        m.entries.push_back({cf::entry_kind::file, static_cast<std::uint64_t>(100 + d * 37 + f * 11), 1700000000000, dir + (f % 2 ? "/sub/" : "/") + "f" + std::to_string(f) + ".bin"});
      }
    }
    return m;
  }
}  // namespace

TEST_F(ClipboardAgent, ShellCopiesFolderTree) {
  if (!ole_) {
    GTEST_SKIP() << "OLE is not available";
  }
  for (const bool prefetch : {false, true}) {
    const auto m = tree_of_50();
    auto src = std::make_shared<fake_source>();
    const auto root = unique_temp_root("shell");
    obj_ptr obj(create_data_object(make_offer(m, prefetch), src, root));
    if (FAILED(OleSetClipboard(obj.p))) {
      GTEST_SKIP() << "OLE clipboard is not available in this session (no interactive desktop)";
    }
    IDataObject *pasted = nullptr;
    ASSERT_EQ(OleGetClipboard(&pasted), S_OK);
    obj_ptr from_clip(pasted);
    EXPECT_EQ(OleIsCurrentClipboard(obj.p), S_OK);

    const fs::path dest = unique_temp_root("dest");
    fs::create_directories(dest);
    paste_virtual_files(from_clip.p, dest);

    std::size_t files = 0;
    for (std::size_t i = 0; i < m.entries.size(); ++i) {
      const auto &e = m.entries[i];
      const fs::path p = dest / fs::path(e.path);
      if (e.kind == cf::entry_kind::directory) {
        EXPECT_TRUE(fs::is_directory(p)) << e.path;
        continue;
      }
      ++files;
      std::ifstream in(p, std::ios::binary);
      std::string got((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
      EXPECT_EQ(got == expected_bytes(static_cast<std::uint32_t>(i), 0, e.size), true) << e.path;
    }
    EXPECT_EQ(files, 50u);

    if (prefetch) {
      FORMATETC hdrop {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
      release_medium med;
      ASSERT_EQ(from_clip->GetData(&hdrop, &med.m), S_OK);
      auto *df = static_cast<DROPFILES *>(GlobalLock(med.m.hGlobal));
      ASSERT_NE(df, nullptr);
      std::size_t top = 0;
      for (const wchar_t *p = reinterpret_cast<const wchar_t *>(reinterpret_cast<const char *>(df) + df->pFiles); *p; p += wcslen(p) + 1) {
        ++top;
        EXPECT_TRUE(fs::exists(p));
      }
      GlobalUnlock(med.m.hGlobal);
      EXPECT_EQ(top, 5u);
    }
    OleSetClipboard(nullptr);
    std::error_code ec;
    fs::remove_all(dest, ec);
  }
}
