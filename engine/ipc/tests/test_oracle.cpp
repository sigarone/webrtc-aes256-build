// Tests of the fuzzing oracle (fuzz/oracle.h): it must agree with the validator on every valid
// and invalid message we can name, and it must look inside nested objects, array elements and
// scalar maps. An oracle that only checks the top level of a message cannot catch a validator that
// fails open in a nested element (an ICE server entry, a simulcast layer, a stats entry).
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../fuzz/oracle.h"
#include "gen_messages.h"
#include "qmedia/ipc/cbor.h"
#include "qmedia/ipc/schema.h"
#include "testing.h"

using namespace qmedia::ipc;
using namespace qmedia::testgen;
using cbor::Buf;
using cbor::Value;

namespace fs = std::filesystem;

namespace {

Buf Str(std::string_view s) {
  Buf b;
  cbor::PutStr(b, s);
  return b;
}

const MessageSpec& Spec(std::string_view kind) {
  const MessageSpec* s = FindMessage(kind);
  if (s == nullptr) std::abort();
  return *s;
}

// Decodes b and checks that validator and oracle give the same verdict in both directions.
// Returns false (and reports) on a disagreement.
bool Agree(const Buf& b, const char* what) {
  Value v;
  if (cbor::Decode(b, &v) != Err::Ok) return true;  // the oracle only judges decoded values
  bool ok = true;
  for (Dir d : {Dir::ClientToEngine, Dir::EngineToClient}) {
    ValidatedMessage m;
    const bool valid = ValidateMessage(v, d, &m) == Err::Ok;
    const MessageSpec* o = oracle::Accepts(v, d);
    if (valid != (o != nullptr) || (valid && m.spec != o)) {
      std::fprintf(stderr, "  oracle and validator disagree on %s\n", what);
      ok = false;
    }
    if (valid && !oracle::PinnedInvariantsHold(v, *m.spec)) {
      std::fprintf(stderr, "  pinned invariant broken by %s\n", what);
      ok = false;
    }
  }
  return ok;
}

bool ReadAll(const fs::path& p, Buf* out) {
  std::ifstream in(p, std::ios::binary);
  if (!in) return false;
  out->assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  return true;
}

// Decodes, then requires that the validator AND the oracle both refuse the message.
void ExpectBothReject(const Buf& b, Dir d, const char* what) {
  Value v;
  CHECK_EQ(cbor::Decode(b, &v), Err::Ok);
  ValidatedMessage m;
  const bool valid = ValidateMessage(v, d, &m) == Err::Ok;
  const bool oracle_ok = oracle::Accepts(v, d) != nullptr;
  if (valid || oracle_ok) {
    std::fprintf(stderr, "  %s: validator %s, oracle %s\n", what, valid ? "accepts" : "rejects",
                 oracle_ok ? "accepts" : "rejects");
  }
  CHECK(!valid);
  CHECK(!oracle_ok);
}

Buf IceServers(Buf server) { return cbor::ArrayBuilder().Add(std::move(server)).Finish(); }

}  // namespace

QTEST(oracle_agrees_with_the_validator_on_every_generated_and_corpus_message) {
  int n = 0;
  for (const MessageSpec& spec : Messages()) {
    for (bool full : {false, true}) {
      GenOptions o;
      o.full = full;
      const Buf b = GenMessage(spec, o);
      CHECK(Agree(b, std::string(spec.kind).c_str()));
      Value v;
      CHECK_EQ(cbor::Decode(b, &v), Err::Ok);
      CHECK(oracle::Accepts(v, spec.dir) == &spec);  // a valid message is accepted, as its kind
      ++n;
    }
  }
  for (const auto& e : fs::directory_iterator(fs::path(QMEDIA_CORPUS_DIR) / "message")) {
    Buf b;
    CHECK(ReadAll(e.path(), &b));
    CHECK(Agree(b, e.path().filename().string().c_str()));
    ++n;
  }
  CHECK(n > 200);
}

QTEST(oracle_checks_the_envelope) {
  const MessageSpec& ping = Spec("ping");
  GenOptions o;
  const Buf v2 = [] {
    Buf b;
    cbor::PutUint(b, 2);
    return b;
  }();
  ExpectBothReject(GenMessageWith(ping, o, "v", &v2), Dir::ClientToEngine, "version 2");
  ExpectBothReject(GenMessage(ping, o), Dir::EngineToClient, "client message in the engine direction");
  ExpectBothReject(GenMessage(Spec("pong"), o), Dir::ClientToEngine, "engine message in the client direction");
  const Buf id0 = [] {
    Buf b;
    cbor::PutUint(b, 0);
    return b;
  }();
  ExpectBothReject(GenMessageWith(ping, o, "id", &id0), Dir::ClientToEngine, "request with id 0");
  const Buf id5 = [] {
    Buf b;
    cbor::PutUint(b, 5);
    return b;
  }();
  ExpectBothReject(GenMessageWith(Spec("pc_state"), o, "id", &id5), Dir::EngineToClient, "event with an id");
  ExpectBothReject(GenMessageWith(ping, o, "t", nullptr), Dir::ClientToEngine, "no kind");
}

QTEST(oracle_looks_inside_nested_objects_arrays_and_scalar_maps) {
  // Every message below is well formed at the top level (known field, right container type, entry
  // count in range) and wrong only inside an element.
  GenOptions full;
  full.full = true;
  const MessageSpec& pc = Spec("pc_create");

  {
    cbor::MapBuilder s;
    s.Raw("urls", cbor::ArrayBuilder().Add(Str("stun:a.invalid")).Finish()).Uint("extra", 1);
    const Buf servers = IceServers(s.Finish());
    ExpectBothReject(GenMessageWith(pc, full, "ice_servers", &servers), Dir::ClientToEngine,
                     "unknown field in an ICE server");
  }
  {
    cbor::MapBuilder s;
    s.Str("username", "u");
    const Buf servers = IceServers(s.Finish());
    ExpectBothReject(GenMessageWith(pc, full, "ice_servers", &servers), Dir::ClientToEngine,
                     "ICE server without urls");
  }
  {
    cbor::MapBuilder s;
    s.Raw("urls", cbor::ArrayBuilder().Add(Str("turn:a.invalid\n:1")).Finish());
    const Buf servers = IceServers(s.Finish());
    ExpectBothReject(GenMessageWith(pc, full, "ice_servers", &servers), Dir::ClientToEngine,
                     "line break in an ICE server url");
  }
  {
    cbor::MapBuilder s;
    s.Raw("urls", cbor::ArrayBuilder().Add(Str("")).Finish());
    const Buf servers = IceServers(s.Finish());
    ExpectBothReject(GenMessageWith(pc, full, "ice_servers", &servers), Dir::ClientToEngine,
                     "empty ICE server url");
  }
  {
    cbor::MapBuilder s;
    s.Raw("urls", cbor::ArrayBuilder().Add(Str("stun:a.invalid")).Finish()).Uint("username", 7);
    const Buf servers = IceServers(s.Finish());
    ExpectBothReject(GenMessageWith(pc, full, "ice_servers", &servers), Dir::ClientToEngine,
                     "ICE server username of the wrong type");
  }
  {
    cbor::MapBuilder layer;
    layer.Str("rid", "a").Flag("active", true).Uint("scale_down", 17);
    const Buf layers = cbor::ArrayBuilder().Add(layer.Finish()).Finish();
    ExpectBothReject(GenMessageWith(Spec("set_simulcast"), full, "layers", &layers), Dir::ClientToEngine,
                     "simulcast layer scale out of range");
  }
  {
    cbor::MapBuilder entry;
    entry.Str("type", "t").Str("id", "x").Raw("values", cbor::MapBuilder().Bin("a", Buf{1}).Finish());
    const Buf entries = cbor::ArrayBuilder().Add(entry.Finish()).Finish();
    ExpectBothReject(GenMessageWith(Spec("stats"), full, "entries", &entries), Dir::EngineToClient,
                     "byte string in a stats scalar map");
  }
  {
    cbor::MapBuilder entry;
    entry.Str("type", "t").Str("id", "x").Raw("values", cbor::MapBuilder().Uint("a\nb", 1).Finish());
    const Buf entries = cbor::ArrayBuilder().Add(entry.Finish()).Finish();
    ExpectBothReject(GenMessageWith(Spec("stats"), full, "entries", &entries), Dir::EngineToClient,
                     "line break in a stats key");
  }
  {
    cbor::MapBuilder dev;
    dev.Str("id", "").Str("name", "n").Str("kind", "bogus").Flag("is_default", true);
    const Buf devices = cbor::ArrayBuilder().Add(dev.Finish()).Finish();
    ExpectBothReject(GenMessageWith(Spec("devices"), full, "devices", &devices), Dir::EngineToClient,
                     "unknown device kind");
  }
}

QTEST(oracle_pins_the_key_and_nonce_sizes_independently_of_the_table) {
  Buf key16;
  cbor::PutBin(key16, Buf(16, 0x5A));
  GenOptions o;
  Value v;
  CHECK_EQ(cbor::Decode(GenMessageWith(Spec("install_key"), o, "key", &key16), &v), Err::Ok);
  CHECK(!oracle::PinnedInvariantsHold(v, Spec("install_key")));
  Buf zero;
  cbor::PutBin(zero, Buf(32, 0));
  Value z;
  CHECK_EQ(cbor::Decode(GenMessageWith(Spec("install_key"), o, "key", &zero), &z), Err::Ok);
  CHECK(!oracle::PinnedInvariantsHold(z, Spec("install_key")));
  Buf nonce31;
  cbor::PutBin(nonce31, Buf(31, 1));
  Value h;
  CHECK_EQ(cbor::Decode(GenMessageWith(Spec("hello"), o, "nonce", &nonce31), &h), Err::Ok);
  CHECK(!oracle::PinnedInvariantsHold(h, Spec("hello")));
  Value good;
  CHECK_EQ(cbor::Decode(GenMessage(Spec("install_key"), o), &good), Err::Ok);
  CHECK(oracle::PinnedInvariantsHold(good, Spec("install_key")));
}
