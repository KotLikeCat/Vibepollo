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
      return do_read(idx, off, len, out, nullptr);
    }

    read_result read_cancellable(std::uint32_t idx, std::uint64_t off, std::uint32_t len, std::string &out, const std::shared_ptr<cancel_token> &token) override {
      return do_read(idx, off, len, out, token);
    }

    std::atomic<int> calls {0};
    std::atomic<int> current {0};
    std::atomic<int> max_current {0};
    int delay_ms {0};
    std::optional<cf::read_error> fail;

  private:
    read_result do_read(std::uint32_t idx, std::uint64_t off, std::uint32_t len, std::string &out, const std::shared_ptr<cancel_token> &token) {
      ++calls;
      const int now = ++current;
      int prev = max_current.load();
      while (now > prev && !max_current.compare_exchange_weak(prev, now)) {}
      struct leave {
        std::atomic<int> &c;

        ~leave() {
          --c;
        }
      } guard {current};
      for (int waited = 0; waited < delay_ms; waited += 5) {
        if (token && token->cancelled()) {
          return {false, cf::read_error::io};
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      if (fail) {
        return {false, *fail};
      }
      out = expected_bytes(idx, off, len);
      return {true, cf::read_error::io};
    }
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

  std::vector<fs::path> &temp_roots() {
    static std::vector<fs::path> roots;
    return roots;
  }

  fs::path unique_temp_root(const char *tag) {
    static std::atomic<int> n {0};
    auto p = fs::temp_directory_path() / (std::string("vibepollo-agent-test-") + tag + "-" + std::to_string(GetCurrentProcessId()) + "-" + std::to_string(++n));
    temp_roots().push_back(p);
    return p;
  }

  std::chrono::steady_clock::time_point g_posted_seen;

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
      for (const auto &p : temp_roots()) {
        std::error_code ec;
        fs::remove_all(p, ec);
      }
      temp_roots().clear();
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
  src->delay_ms = 5000;  // the fake honours the cancel token: releasing must abort the read, not wait it out
  obj_ptr obj(create_data_object(cyrillic_offer(false), src, unique_temp_root("rel")));
  auto f = fmt_of(CFSTR_FILECONTENTS, TYMED_ISTREAM, 1);
  STGMEDIUM med {};
  ASSERT_EQ(obj->GetData(&f, &med), S_OK);
  const auto wait_start = std::chrono::steady_clock::now();
  while (src->current.load() == 0 && std::chrono::steady_clock::now() - wait_start < std::chrono::seconds(3)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_GT(src->current.load(), 0);  // a worker is blocked inside read()
  const auto t0 = std::chrono::steady_clock::now();
  med.pstm->Release();
  EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(2));
  const auto t1 = std::chrono::steady_clock::now();
  while (src->current.load() != 0 && std::chrono::steady_clock::now() - t1 < std::chrono::seconds(2)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(src->current.load(), 0);
}

TEST_F(ClipboardAgent, HdropAfterPrefetch) {
  const auto root = unique_temp_root("hdrop");
  fs::path folder;
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
    folder = fs::path(paths[0]).parent_path();
    EXPECT_EQ(folder.parent_path(), root);
    EXPECT_EQ(folder.filename().string().rfind(cf::offer_id_hex(cyrillic_offer(true).id) + "-", 0), 0u);  // <hex>-<instance>
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

namespace {
  namespace ag = cf::agent;

  /// A pipe_range_source whose "pipe" answers read_range frames from a fake file system (expected_bytes).
  struct loop_router {
    std::shared_ptr<pipe_range_source> ps;
    std::mutex m;
    std::vector<std::thread> workers;
    std::vector<ag::read_range_t> log;
    std::atomic<int> cancels {0};
    std::function<void(const ag::read_range_t &)> responder;

    explicit loop_router(std::chrono::milliseconds timeout = std::chrono::seconds(60)) {
      responder = [this](const ag::read_range_t &r) {
        const auto all = expected_bytes(r.file_index, r.offset, r.length);
        for (std::size_t off = 0; off < all.size(); off += 1000) {
          const auto n = std::min<std::size_t>(1000, all.size() - off);
          ps->on_range_data({r.read_id, off + n == all.size(), all.substr(off, n)});
        }
      };
      ps = std::make_shared<pipe_range_source>(
        [this](const std::string &frame) {
          auto msg = ag::decode(frame);
          if (!msg) {
            return false;
          }
          if (msg->type == ag::msg::cancel_read) {
            ++cancels;
            return true;
          }
          auto rr = ag::decode_read_range(msg->payload);
          if (!rr) {
            return false;
          }
          std::lock_guard lock(m);
          log.push_back(*rr);
          workers.emplace_back([this, r = *rr] {
            responder(r);
          });
          return true;
        },
        timeout
      );
    }

    ~loop_router() {
      std::vector<std::thread> w;
      {
        std::lock_guard lock(m);
        w.swap(workers);
      }
      for (auto &t : w) {
        t.join();
      }
    }
  };

  cf::offer_id_t id_of(std::uint8_t b) {
    cf::offer_id_t id {};
    id[0] = b;
    return id;
  }
}  // namespace

TEST_F(ClipboardAgent, PipeRangeSourceRoundTrip) {
  loop_router lr;
  lr.responder = [&lr](const ag::read_range_t &r) {
    if (r.file_index == 9) {
      lr.ps->on_range_error({r.read_id, cf::read_error::gone});
    } else if (r.file_index == 8) {
      return;  // never answered
    } else {
      const auto all = expected_bytes(r.file_index, r.offset, r.length);
      for (std::size_t off = 0; off < all.size(); off += 1000) {
        const auto n = std::min<std::size_t>(1000, all.size() - off);
        lr.ps->on_range_data({r.read_id, off + n == all.size(), all.substr(off, n)});
      }
    }
  };
  auto src = lr.ps->bind(id_of(1));
  std::string out;
  auto rr = src->read(3, 17, 2500, out);
  EXPECT_TRUE(rr.ok);
  EXPECT_EQ(out, expected_bytes(3, 17, 2500));
  rr = src->read(9, 0, 10, out);
  EXPECT_FALSE(rr.ok);
  EXPECT_EQ(rr.error, cf::read_error::gone);
  auto tok = std::make_shared<cancel_token>();
  std::thread canceller([&] {
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    tok->cancel();
  });
  rr = src->read_cancellable(8, 0, 10, out, tok);
  canceller.join();
  EXPECT_FALSE(rr.ok);
  EXPECT_EQ(lr.cancels.load(), 1);
  {
    std::lock_guard lock(lr.m);
    ASSERT_EQ(lr.log.size(), 3u);
    for (const auto &r : lr.log) {
      EXPECT_EQ(r.offer, id_of(1));
    }
  }
}

TEST_F(ClipboardAgent, PipeRangeSourceTimeoutShortLastAndOverflow) {
  {
    loop_router lr(std::chrono::milliseconds(250));
    lr.responder = [](const ag::read_range_t &) {};
    std::string out;
    const auto t0 = std::chrono::steady_clock::now();
    auto rr = lr.ps->bind(id_of(1))->read(0, 0, 10, out);
    EXPECT_FALSE(rr.ok);
    EXPECT_EQ(rr.error, cf::read_error::timeout);
    EXPECT_LT(std::chrono::steady_clock::now() - t0, std::chrono::seconds(5));
    EXPECT_EQ(lr.cancels.load(), 1);  // late data would be ignored; the core is told to stop
  }
  {
    loop_router lr;
    lr.responder = [&lr](const ag::read_range_t &r) {
      lr.ps->on_range_data({r.read_id, true, "abc"});  // `last` after 3 of 10 bytes
    };
    std::string out;
    auto rr = lr.ps->bind(id_of(1))->read(0, 0, 10, out);
    EXPECT_FALSE(rr.ok);
    EXPECT_EQ(rr.error, cf::read_error::io);
  }
  {
    loop_router lr;
    lr.responder = [&lr](const ag::read_range_t &r) {
      lr.ps->on_range_data({r.read_id, false, std::string(11, 'x')});  // more than requested
    };
    std::string out;
    auto rr = lr.ps->bind(id_of(1))->read(0, 0, 10, out);
    EXPECT_FALSE(rr.ok);
    EXPECT_EQ(rr.error, cf::read_error::io);
  }
  {
    loop_router lr;
    lr.ps->shutdown();  // dead source: reads fail fast and never send
    std::string out;
    EXPECT_FALSE(lr.ps->bind(id_of(1))->read(0, 0, 10, out).ok);
    EXPECT_EQ(lr.cancels.load(), 0);
  }
}

TEST_F(ClipboardAgent, ReadsKeepTheirOwnOfferId) {
  loop_router lr;
  const std::uint64_t big = 20u << 20;  // > 16 MiB window: later pieces are requested only after consumption
  cf::manifest ma;
  ma.offer_id = id_of(0xA1);
  ma.entries.push_back({cf::entry_kind::file, big, 0, "a.bin"});
  cf::manifest mb;
  mb.offer_id = id_of(0xB2);
  mb.entries.push_back({cf::entry_kind::file, 100, 0, "b.bin"});

  obj_ptr obj_a(create_data_object(make_offer(ma, false), lr.ps->bind(ma.offer_id), unique_temp_root("ida")));
  auto f = fmt_of(CFSTR_FILECONTENTS, TYMED_ISTREAM, 0);
  release_medium stream_a;
  ASSERT_EQ(obj_a->GetData(&f, &stream_a.m), S_OK);

  // A newer offer replaces A as "current" and its own stream reads too.
  obj_ptr obj_b(create_data_object(make_offer(mb, false), lr.ps->bind(mb.offer_id), unique_temp_root("idb")));
  release_medium stream_b;
  ASSERT_EQ(obj_b->GetData(&f, &stream_b.m), S_OK);
  EXPECT_EQ(read_all(stream_b.m.pstm, 4096), expected_bytes(0, 0, 100));

  // A's stream reads on after B exists: the late pieces (offset >= 16 MiB) must still be A's.
  EXPECT_EQ(read_all(stream_a.m.pstm, 1u << 20) == expected_bytes(0, 0, big), true);
  std::lock_guard lock(lr.m);
  int late = 0;
  for (const auto &r : lr.log) {
    if (r.offset >= (16u << 20)) {
      ++late;
      EXPECT_EQ(r.offer, ma.offer_id);
    }
    if (r.offer == mb.offer_id) {
      EXPECT_EQ(r.length, 100u);
    }
  }
  EXPECT_GE(late, 1);
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

TEST_F(ClipboardAgent, PipelinedReadsOfLargeFile) {
  cf::manifest m;
  m.offer_id = id_of(5);
  const std::uint64_t size = 41u * 1024 * 1024 + 12345;  // > 10 pieces, odd tail
  m.entries.push_back({cf::entry_kind::file, size, 0, "big.bin"});
  auto src = std::make_shared<fake_source>();
  src->delay_ms = 30;
  obj_ptr obj(create_data_object(make_offer(m, false), src, unique_temp_root("pipe")));
  auto f = fmt_of(CFSTR_FILECONTENTS, TYMED_ISTREAM, 0);
  release_medium med;
  ASSERT_EQ(obj->GetData(&f, &med.m), S_OK);
  IStream *s = med.m.pstm;

  // sequential read with an odd chunk size, compared byte for byte
  std::string got;
  got.reserve(size);
  std::string buf(1000003, '\0');
  for (;;) {
    ULONG n = 0;
    ASSERT_EQ(s->Read(buf.data(), static_cast<ULONG>(buf.size()), &n), S_OK);
    if (n == 0) {
      break;
    }
    got.append(buf.data(), n);
  }
  ASSERT_EQ(got.size(), size);
  EXPECT_EQ(got == expected_bytes(0, 0, size), true);
  EXPECT_GE(src->max_current.load(), 3);  // 4 x 4 MiB pipeline
  EXPECT_LE(src->max_current.load(), 4);

  // seek inside the window, backwards (outside it) and far forwards; always byte-exact
  auto check_at = [&](std::uint64_t at) {
    LARGE_INTEGER mv;
    mv.QuadPart = static_cast<LONGLONG>(at);
    ASSERT_EQ(s->Seek(mv, STREAM_SEEK_SET, nullptr), S_OK);
    std::string b(70000, '\0');
    ULONG n = 0;
    ASSERT_EQ(s->Read(b.data(), static_cast<ULONG>(b.size()), &n), S_OK);
    ASSERT_EQ(n, std::min<std::uint64_t>(b.size(), size - at));
    EXPECT_EQ(b.substr(0, n) == expected_bytes(0, at, n), true) << at;
  };
  check_at(5u << 20);  // backwards, outside the (consumed) window
  check_at((5u << 20) + 3000000);  // forwards within the freshly buffered window
  check_at(30u << 20);  // far forwards: discards outstanding pieces
  check_at(100);  // and back again
  check_at(size - 10);  // tail
}

TEST_F(ClipboardAgent, PrefetchRunsReadsInParallel) {
  cf::manifest m;
  m.offer_id = id_of(6);
  for (int i = 0; i < 12; ++i) {
    m.entries.push_back({cf::entry_kind::file, 100, 0, "f" + std::to_string(i) + ".bin"});
  }
  auto src = std::make_shared<fake_source>();
  src->delay_ms = 50;
  obj_ptr obj(create_data_object(make_offer(m, true), src, unique_temp_root("ppar")));
  FORMATETC hdrop {CF_HDROP, nullptr, DVASPECT_CONTENT, -1, TYMED_HGLOBAL};
  release_medium med;
  ASSERT_EQ(obj->GetData(&hdrop, &med.m), S_OK);
  EXPECT_GE(src->max_current.load(), 3);
  EXPECT_LE(src->max_current.load(), 4);
}

TEST_F(ClipboardAgent, StreamReadRunsOffTheSta) {
  auto src = std::make_shared<fake_source>();
  src->delay_ms = 600;
  obj_ptr obj(create_data_object(cyrillic_offer(false), src, unique_temp_root("sta")));
  auto f = fmt_of(CFSTR_FILECONTENTS, TYMED_ISTREAM, 1);
  STGMEDIUM med {};
  ASSERT_EQ(obj->GetData(&f, &med), S_OK);
  IStream *s = med.pstm;
  IClientSecurity *cs = nullptr;
  EXPECT_EQ(s->QueryInterface(IID_IClientSecurity, reinterpret_cast<void **>(&cs)), S_OK) << "the stream must be a proxy to an MTA object";
  if (cs) {
    cs->Release();
  }

  IStream *marshaled = nullptr;
  ASSERT_EQ(CoMarshalInterThreadInterfaceInStream(IID_IStream, s, &marshaled), S_OK);
  std::atomic<bool> started {false};
  std::atomic<bool> finished {false};
  std::thread consumer([&] {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IStream *p = nullptr;
    if (SUCCEEDED(CoGetInterfaceAndReleaseStream(marshaled, IID_IStream, reinterpret_cast<void **>(&p)))) {
      started = true;
      char c[10];
      ULONG n = 0;
      p->Read(c, sizeof(c), &n);  // blocks ~600 ms in the fake source
      p->Release();
    }
    started = true;
    finished = true;
    CoUninitialize();
  });

  auto pump_until = [](auto &&cond, std::chrono::milliseconds limit) {
    const auto t0 = std::chrono::steady_clock::now();
    MSG msg;
    while (!cond() && std::chrono::steady_clock::now() - t0 < limit) {
      MsgWaitForMultipleObjects(0, nullptr, FALSE, 5, QS_ALLINPUT);
      while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_APP + 9) {
          g_posted_seen = std::chrono::steady_clock::now();
        }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
      }
    }
  };
  pump_until(
    [&] {
      return started.load();
    },
    std::chrono::seconds(5)
  );
  pump_until(
    [] {
      return false;
    },
    std::chrono::milliseconds(100)
  );
  g_posted_seen = {};
  const auto posted = std::chrono::steady_clock::now();
  PostThreadMessageW(GetCurrentThreadId(), WM_APP + 9, 0, 0);
  pump_until(
    [] {
      return g_posted_seen != std::chrono::steady_clock::time_point {};
    },
    std::chrono::seconds(3)
  );
  ASSERT_NE(g_posted_seen, std::chrono::steady_clock::time_point {});
  EXPECT_LT(g_posted_seen - posted, std::chrono::milliseconds(300)) << "the STA was blocked by the stream read";
  EXPECT_FALSE(finished.load());  // the read is still in flight, so the check above really overlapped it
  pump_until(
    [&] {
      return finished.load();
    },
    std::chrono::seconds(5)
  );
  consumer.join();
  s->Release();
}
