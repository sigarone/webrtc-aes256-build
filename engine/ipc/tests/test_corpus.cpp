#include <filesystem>
#include <fstream>
#include <iterator>
#include <set>
#include <string>

#include "corpus_data.h"
#include "qmedia/ipc/session.h"
#include "testing.h"

using namespace qmedia::ipc;
using namespace qmedia::testgen;
using cbor::Buf;
using cbor::Value;

namespace fs = std::filesystem;

namespace {

bool ReadFile(const fs::path& p, Buf* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

bool StartsWith(const std::string& s, const char* prefix) { return s.rfind(prefix, 0) == 0; }

}  // namespace

// The committed corpus is exactly what corpus_data.h produces. If this fails, run
//   qmedia_corpus_gen engine/ipc/fuzz/corpus
// and commit the result.
QTEST(corpus_is_in_sync_with_the_schema) {
  const fs::path root = QMEDIA_CORPUS_DIR;
  std::set<std::string> expected;
  for (const CorpusFile& f : BuildCorpus()) {
    const fs::path p = root / f.subdir / f.name;
    expected.insert(p.generic_string());
    Buf on_disk;
    const bool read = ReadFile(p, &on_disk);
    if (!read) std::fprintf(stderr, "  missing corpus file: %s\n", p.generic_string().c_str());
    CHECK(read);
    if (read && on_disk != f.bytes) {
      std::fprintf(stderr, "  stale corpus file: %s\n", p.generic_string().c_str());
      CHECK(false);
    }
  }
  // No stray files.
  for (const auto& e : fs::recursive_directory_iterator(root)) {
    if (!e.is_regular_file()) continue;
    if (expected.count(e.path().generic_string()) == 0) {
      std::fprintf(stderr, "  unexpected corpus file: %s\n", e.path().generic_string().c_str());
      CHECK(false);
    }
  }
}

QTEST(corpus_seeds_behave_as_named) {
  const fs::path dir = fs::path(QMEDIA_CORPUS_DIR) / "message";
  int checked = 0;
  for (const auto& e : fs::directory_iterator(dir)) {
    const std::string name = e.path().filename().string();
    Buf b;
    CHECK(ReadFile(e.path(), &b));
    Value v;
    const Err dec = cbor::Decode(b, &v);
    ValidatedMessage m;
    if (StartsWith(name, "ok_c2e_")) {
      CHECK_EQ(dec, Err::Ok);
      CHECK_EQ(ValidateMessage(v, Dir::ClientToEngine, &m), Err::Ok);
    } else if (StartsWith(name, "ok_e2c_")) {
      CHECK_EQ(dec, Err::Ok);
      CHECK_EQ(ValidateMessage(v, Dir::EngineToClient, &m), Err::Ok);
    } else if (StartsWith(name, "bad_cbor_")) {
      CHECK(dec != Err::Ok);
    } else if (StartsWith(name, "bad_c2e_")) {
      CHECK_EQ(dec, Err::Ok);
      CHECK(ValidateMessage(v, Dir::ClientToEngine, &m) != Err::Ok);
    } else if (StartsWith(name, "bad_e2c_")) {
      CHECK_EQ(dec, Err::Ok);
      CHECK(ValidateMessage(v, Dir::EngineToClient, &m) != Err::Ok);
    } else {
      std::fprintf(stderr, "  unclassified corpus file: %s\n", name.c_str());
      CHECK(false);
    }
    ++checked;
  }
  CHECK(checked > 100);
}

QTEST(corpus_session_streams_never_crash_and_happy_path_shuts_down) {
  const fs::path dir = fs::path(QMEDIA_CORPUS_DIR) / "session";
  const Buf nonce = FixedNonce();
  int checked = 0;
  for (const auto& e : fs::directory_iterator(dir)) {
    Buf b;
    CHECK(ReadFile(e.path(), &b));
    MemoryStream s(b);
    FrameBuffer buf;
    StubHandler stub;
    const ServeResult r = Serve(s, buf, nonce, 1000, stub);
    const std::string name = e.path().filename().string();
    if (name == "happy.bin") CHECK(r == ServeResult::Shutdown);
    if (name == "wrong_nonce.bin") CHECK(r == ServeResult::HandshakeFailed && s.written().empty());
    if (name == "hello_only.bin") CHECK(r == ServeResult::PeerClosed);
    if (name == "oversized_frame.bin" || name == "second_hello.bin") {
      CHECK(r == ServeResult::ProtocolViolation);
    }
    if (name == "truncated.bin") CHECK(r == ServeResult::IoError);
    if (name == "every_kind.bin") CHECK(r == ServeResult::PeerClosed);
    ++checked;
  }
  CHECK(checked >= 7);
}
