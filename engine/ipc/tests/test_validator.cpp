#include <set>
#include <string>

#include "gen_messages.h"
#include "qmedia/ipc/cbor.h"
#include "qmedia/ipc/limits.h"
#include "qmedia/ipc/message.h"
#include "qmedia/ipc/schema.h"
#include "testing.h"

using namespace qmedia::ipc;
using namespace qmedia::testgen;
using cbor::Buf;
using cbor::Value;

namespace {

// Decodes and validates. The Value must outlive the ValidatedMessage, so it is passed in.
Err Check(const Buf& b, Dir dir, Value* root, ValidatedMessage* msg = nullptr) {
  Err e = cbor::Decode(b, root);
  if (e != Err::Ok) return e;
  ValidatedMessage tmp;
  return ValidateMessage(*root, dir, msg != nullptr ? msg : &tmp);
}

Err CheckC2E(const Buf& b) {
  Value root;
  return Check(b, Dir::ClientToEngine, &root);
}

Buf Uint(uint64_t v) {
  Buf b;
  cbor::PutUint(b, v);
  return b;
}
Buf Str(std::string_view s) {
  Buf b;
  cbor::PutStr(b, s);
  return b;
}
Buf Bin(size_t n, uint8_t fill) {
  Buf b;
  cbor::PutBin(b, Buf(n, fill));
  return b;
}
const MessageSpec& Spec(std::string_view kind) {
  const MessageSpec* s = FindMessage(kind);
  if (s == nullptr) {
    std::fprintf(stderr, "unknown kind in test: %.*s\n", static_cast<int>(kind.size()), kind.data());
    std::abort();
  }
  return *s;
}

}  // namespace

QTEST(schema_every_kind_has_a_valid_minimal_and_full_message) {
  std::set<std::string_view> seen;
  for (const MessageSpec& spec : Messages()) {
    CHECK(seen.insert(spec.kind).second);  // kinds are unique
    for (bool full : {false, true}) {
      GenOptions o;
      o.full = full;
      const Buf b = GenMessage(spec, o);
      Value root;
      ValidatedMessage msg;
      const Err e = Check(b, spec.dir, &root, &msg);
      if (e != Err::Ok) {
        std::fprintf(stderr, "  kind %.*s full=%d -> %s\n", static_cast<int>(spec.kind.size()),
                     spec.kind.data(), full, ErrName(e));
      }
      CHECK_EQ(e, Err::Ok);
      if (e == Err::Ok) {
        CHECK(msg.spec == &spec);
        // Canonical: re-encoding gives the very same bytes.
        Buf again;
        cbor::Encode(root, &again);
        CHECK(again == b);
        // The same message in the other direction is refused.
        Value root2;
        CHECK_EQ(Check(b, spec.dir == Dir::ClientToEngine ? Dir::EngineToClient : Dir::ClientToEngine,
                       &root2),
                 Err::SchemaDirection);
      }
    }
  }
  CHECK(seen.size() > 40);
}

QTEST(schema_rejects_unknown_fields_everywhere) {
  for (const MessageSpec& spec : Messages()) {
    GenOptions o;
    // Rebuild the message with one extra field (the builder keeps the map sorted).
    Value root;
    const Buf base = GenMessage(spec, o);
    CHECK_EQ(cbor::Decode(base, &root), Err::Ok);
    cbor::MapBuilder mb;
    for (size_t i = 0; i + 1 < root.items.size(); i += 2) {
      Buf enc;
      cbor::Encode(root.items[i + 1], &enc);
      mb.Raw(root.items[i].text(), std::move(enc));
    }
    mb.Uint("zz_extra", 1);
    const Buf with_extra = mb.Finish();
    Value r2;
    CHECK_EQ(Check(with_extra, spec.dir, &r2), Err::SchemaUnknownField);
  }
}

QTEST(schema_envelope_checks) {
  const MessageSpec& ping = Spec("ping");
  GenOptions o;
  CHECK_EQ(CheckC2E(GenMessage(ping, o)), Err::Ok);

  // version
  CHECK_EQ(CheckC2E(GenMessageWith(ping, o, "v", nullptr)), Err::SchemaHeader);
  for (uint64_t bad : {0ull, 2ull, 255ull, 0xFFFFFFFFull}) {
    const Buf v = Uint(bad);
    CHECK_EQ(CheckC2E(GenMessageWith(ping, o, "v", &v)), Err::SchemaVersion);
  }
  const Buf vs = Str("1");
  CHECK_EQ(CheckC2E(GenMessageWith(ping, o, "v", &vs)), Err::SchemaVersion);

  // kind
  CHECK_EQ(CheckC2E(GenMessageWith(ping, o, "t", nullptr)), Err::SchemaHeader);
  const Buf tn = Uint(5);
  CHECK_EQ(CheckC2E(GenMessageWith(ping, o, "t", &tn)), Err::SchemaHeader);
  const Buf tu = Str("no_such_kind");
  CHECK_EQ(CheckC2E(GenMessageWith(ping, o, "t", &tu)), Err::SchemaUnknownKind);
  const Buf te = Str("");
  CHECK_EQ(CheckC2E(GenMessageWith(ping, o, "t", &te)), Err::SchemaUnknownKind);
  const Buf tc = Str("PING");
  CHECK_EQ(CheckC2E(GenMessageWith(ping, o, "t", &tc)), Err::SchemaUnknownKind);

  // request id
  CHECK_EQ(CheckC2E(GenMessageWith(ping, o, "id", nullptr)), Err::SchemaHeader);
  const Buf id0 = Uint(0);
  CHECK_EQ(CheckC2E(GenMessageWith(ping, o, "id", &id0)), Err::SchemaId);
  const Buf idbig = Uint(0x100000000ull);
  CHECK_EQ(CheckC2E(GenMessageWith(ping, o, "id", &idbig)), Err::SchemaId);
  const Buf idmax = Uint(0xFFFFFFFFull);
  CHECK_EQ(CheckC2E(GenMessageWith(ping, o, "id", &idmax)), Err::Ok);
  const Buf idstr = Str("1");
  CHECK_EQ(CheckC2E(GenMessageWith(ping, o, "id", &idstr)), Err::SchemaId);

  // events carry id 0, replies carry id >= 1
  const MessageSpec& ev = Spec("devices_changed");
  const Buf id1 = Uint(1);
  Value root;
  CHECK_EQ(Check(GenMessageWith(ev, o, "id", &id1), Dir::EngineToClient, &root), Err::SchemaId);
  const MessageSpec& pong = Spec("pong");
  Value root2;
  CHECK_EQ(Check(GenMessageWith(pong, o, "id", &id0), Dir::EngineToClient, &root2), Err::SchemaId);
  // "err" may carry any id, including 0
  const MessageSpec& err = Spec("err");
  Value root3, root4;
  CHECK_EQ(Check(GenMessageWith(err, o, "id", &id0), Dir::EngineToClient, &root3), Err::Ok);
  CHECK_EQ(Check(GenMessageWith(err, o, "id", &id1), Dir::EngineToClient, &root4), Err::Ok);
}

QTEST(schema_root_must_be_a_map) {
  Value root;
  ValidatedMessage msg;
  for (const Buf& b : {Buf{0x00}, Buf{0x80}, Buf{0xF6}, Buf{0x60}, Buf{0x40}}) {
    CHECK_EQ(Check(b, Dir::ClientToEngine, &root, &msg), Err::SchemaNotMap);
  }
  CHECK_EQ(Check(Buf{0xA0}, Dir::ClientToEngine, &root, &msg), Err::SchemaHeader);  // empty map
}

QTEST(schema_install_key_bounds) {
  const MessageSpec& spec = Spec("install_key");
  GenOptions o;
  CHECK_EQ(CheckC2E(GenMessage(spec, o)), Err::Ok);

  // key length
  for (size_t n : {0u, 1u, 31u, 33u, 64u}) {
    const Buf k = Bin(n, 0x77);
    CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "key", &k)), Err::SchemaLength);
  }
  const Buf good = Bin(32, 0x77);
  CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "key", &good)), Err::Ok);
  // an all-zero key is refused (retired slots are random, a zero key is always a bug)
  const Buf zero = Bin(32, 0x00);
  CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "key", &zero)), Err::SchemaZeroKey);
  // a key as text, or as an integer
  const Buf txt = Str("0123456789abcdef0123456789abcdef");
  CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "key", &txt)), Err::SchemaType);
  const Buf num = Uint(7);
  CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "key", &num)), Err::SchemaType);
  CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "key", nullptr)), Err::SchemaMissingField);

  // slot 0..15
  for (uint64_t s : {0ull, 7ull, 15ull}) {
    const Buf v = Uint(s);
    CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "slot", &v)), Err::Ok);
  }
  for (uint64_t s : {16ull, 17ull, 255ull, 0xFFFFFFFFull}) {
    const Buf v = Uint(s);
    CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "slot", &v)), Err::SchemaRange);
  }
  // direction
  const Buf dir_bad = Str("both");
  CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "direction", &dir_bad)), Err::SchemaEnum);
  const Buf dir_ok = Str("recv");
  CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "direction", &dir_ok)), Err::Ok);
  // participant length 1..64
  const Buf p0 = Str("");
  CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "participant", &p0)), Err::SchemaLength);
  const Buf p64 = Str(std::string(64, 'p'));
  CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "participant", &p64)), Err::Ok);
  const Buf p65 = Str(std::string(65, 'p'));
  CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "participant", &p65)), Err::SchemaLength);
  // session handle: 1..2^32-1
  const Buf h0 = Uint(0);
  CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "session", &h0)), Err::SchemaRange);
  const Buf hbig = Uint(0x100000000ull);
  CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "session", &hbig)), Err::SchemaRange);
  CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "session", nullptr)), Err::SchemaMissingField);
}

QTEST(schema_hello_nonce_bounds) {
  const MessageSpec& spec = Spec("hello");
  GenOptions o;
  for (size_t n : {0u, 31u, 33u}) {
    const Buf v = Bin(n, 0x11);
    CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "nonce", &v)), Err::SchemaLength);
  }
  const Buf ok = Bin(32, 0x11);
  CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "nonce", &ok)), Err::Ok);
  CHECK_EQ(CheckC2E(GenMessageWith(spec, o, "nonce", nullptr)), Err::SchemaMissingField);
}

QTEST(schema_required_enum_and_type_checks) {
  const MessageSpec& sd = Spec("set_remote_description");
  GenOptions o;
  const Buf bad_type = Str("answer2");
  CHECK_EQ(CheckC2E(GenMessageWith(sd, o, "type", &bad_type)), Err::SchemaEnum);
  const Buf int_type = Uint(1);
  CHECK_EQ(CheckC2E(GenMessageWith(sd, o, "type", &int_type)), Err::SchemaType);
  CHECK_EQ(CheckC2E(GenMessageWith(sd, o, "type", nullptr)), Err::SchemaMissingField);
  const Buf sdp_big = Str(std::string(262145, 's'));
  CHECK_EQ(CheckC2E(GenMessageWith(sd, o, "sdp", &sdp_big)), Err::SchemaLength);
  const Buf sdp_max = Str(std::string(262144, 's'));
  CHECK_EQ(CheckC2E(GenMessageWith(sd, o, "sdp", &sdp_max)), Err::Ok);
  const Buf sdp_bytes = Bin(4, 1);
  CHECK_EQ(CheckC2E(GenMessageWith(sd, o, "sdp", &sdp_bytes)), Err::SchemaType);

  const MessageSpec& mute = Spec("set_muted");
  const Buf m_int = Uint(1);
  CHECK_EQ(CheckC2E(GenMessageWith(mute, o, "muted", &m_int)), Err::SchemaType);
  const Buf track_bad = Str("speaker");
  CHECK_EQ(CheckC2E(GenMessageWith(mute, o, "track", &track_bad)), Err::SchemaEnum);

  const MessageSpec& sv = Spec("start_video");
  GenOptions full;
  full.full = true;
  CHECK_EQ(CheckC2E(GenMessage(sv, full)), Err::Ok);
  const Buf fps0 = Uint(0);
  CHECK_EQ(CheckC2E(GenMessageWith(sv, full, "fps", &fps0)), Err::SchemaRange);
  const Buf fps121 = Uint(121);
  CHECK_EQ(CheckC2E(GenMessageWith(sv, full, "fps", &fps121)), Err::SchemaRange);
}

QTEST(schema_nested_objects_and_arrays) {
  GenOptions full;
  full.full = true;

  // pc_create with ice servers
  const MessageSpec& pc = Spec("pc_create");
  cbor::MapBuilder srv;
  srv.Raw("urls", cbor::ArrayBuilder().Add(Str("turn:example.invalid:3478")).Finish());
  srv.Str("username", "u").Str("credential", "c");
  const Buf one = cbor::ArrayBuilder().Add(srv.Finish()).Finish();
  CHECK_EQ(CheckC2E(GenMessageWith(pc, full, "ice_servers", &one)), Err::Ok);

  cbor::MapBuilder bad_srv;
  bad_srv.Raw("urls", cbor::ArrayBuilder().Add(Str("stun:example.invalid")).Finish());
  bad_srv.Uint("extra", 1);
  const Buf unknown_nested = cbor::ArrayBuilder().Add(bad_srv.Finish()).Finish();
  CHECK_EQ(CheckC2E(GenMessageWith(pc, full, "ice_servers", &unknown_nested)), Err::SchemaUnknownField);

  cbor::MapBuilder no_urls;
  no_urls.Str("username", "u");
  const Buf missing_nested = cbor::ArrayBuilder().Add(no_urls.Finish()).Finish();
  CHECK_EQ(CheckC2E(GenMessageWith(pc, full, "ice_servers", &missing_nested)), Err::SchemaMissingField);

  cbor::MapBuilder empty_urls;
  empty_urls.Raw("urls", cbor::ArrayBuilder().Finish());
  const Buf zero_urls = cbor::ArrayBuilder().Add(empty_urls.Finish()).Finish();
  CHECK_EQ(CheckC2E(GenMessageWith(pc, full, "ice_servers", &zero_urls)), Err::SchemaLength);

  cbor::ArrayBuilder nine;
  for (int i = 0; i < 9; ++i) nine.Add(srv.Finish());
  const Buf too_many = nine.Finish();
  CHECK_EQ(CheckC2E(GenMessageWith(pc, full, "ice_servers", &too_many)), Err::SchemaLength);

  const Buf not_array = Str("x");
  CHECK_EQ(CheckC2E(GenMessageWith(pc, full, "ice_servers", &not_array)), Err::SchemaType);

  cbor::ArrayBuilder wrong_elem;
  wrong_elem.Add(Uint(1));
  const Buf wrong = wrong_elem.Finish();
  CHECK_EQ(CheckC2E(GenMessageWith(pc, full, "ice_servers", &wrong)), Err::SchemaType);

  // simulcast layers: 1..4
  const MessageSpec& sim = Spec("set_simulcast");
  const Buf no_layers = cbor::ArrayBuilder().Finish();
  CHECK_EQ(CheckC2E(GenMessageWith(sim, full, "layers", &no_layers)), Err::SchemaLength);
}

QTEST(schema_scalar_map_in_stats) {
  const MessageSpec& st = Spec("stats");
  GenOptions o;
  o.full = true;
  const Buf ok = GenMessage(st, o);
  Value r;
  CHECK_EQ(Check(ok, Dir::EngineToClient, &r), Err::Ok);

  auto entry_with_values = [&](Buf values) {
    cbor::MapBuilder e;
    e.Str("type", "inbound-rtp").Str("id", "x").Raw("values", std::move(values));
    return cbor::ArrayBuilder().Add(e.Finish()).Finish();
  };
  Value r1, r2, r3;
  {
    const Buf nested = cbor::MapBuilder().Raw("a", cbor::MapBuilder().Finish()).Finish();
    const Buf entries = entry_with_values(nested);
    CHECK_EQ(Check(GenMessageWith(st, o, "entries", &entries), Dir::EngineToClient, &r1),
             Err::SchemaType);
  }
  {
    const Buf bin = cbor::MapBuilder().Bin("a", Buf{1}).Finish();
    const Buf entries = entry_with_values(bin);
    CHECK_EQ(Check(GenMessageWith(st, o, "entries", &entries), Dir::EngineToClient, &r2),
             Err::SchemaType);
  }
  {
    const Buf longtext = cbor::MapBuilder().Str("a", std::string(129, 'x')).Finish();
    const Buf entries = entry_with_values(longtext);
    CHECK_EQ(Check(GenMessageWith(st, o, "entries", &entries), Dir::EngineToClient, &r3),
             Err::SchemaType);
  }
}

QTEST(schema_transport_info_requires_a_32_byte_fingerprint) {
  const MessageSpec& ti = Spec("transport_info");
  GenOptions o;
  Value r;
  CHECK_EQ(Check(GenMessage(ti, o), Dir::EngineToClient, &r), Err::Ok);
  for (size_t n : {0u, 20u, 31u, 33u, 48u}) {
    const Buf f = Bin(n, 0x33);
    Value rr;
    CHECK_EQ(Check(GenMessageWith(ti, o, "remote_cert_fingerprint", &f), Dir::EngineToClient, &rr),
             Err::SchemaLength);
  }
}

QTEST(schema_accessors) {
  const MessageSpec& ik = Spec("install_key");
  GenOptions o;
  const Buf b = GenMessage(ik, o);
  Value root;
  ValidatedMessage msg;
  CHECK_EQ(Check(b, Dir::ClientToEngine, &root, &msg), Err::Ok);
  CHECK_EQ(msg.id, 7u);
  CHECK_EQ(FieldUint(msg, "slot", 99), 0u);
  CHECK_EQ(FieldUint(msg, "missing", 99), 99u);
  CHECK(Field(msg, "key") != nullptr && Field(msg, "key")->raw.size() == kKeyBytes);
  CHECK(FieldText(msg, "direction") == "send");
  CHECK(FieldText(msg, "missing").empty());
}

QTEST(message_builders_produce_valid_engine_messages) {
  const struct {
    Buf msg;
    const char* kind;
  } cases[] = {
      {BuildHelloOk(3), "hello_ok"},
      {BuildPong(4), "pong"},
      {BuildOk(5), "ok"},
      {BuildErr(6, errcode::kBadRequest, "schema_type"), "err"},
      {BuildErr(0, errcode::kInternal), "err"},
  };
  for (const auto& c : cases) {
    Value root;
    ValidatedMessage m;
    CHECK_EQ(Check(c.msg, Dir::EngineToClient, &root, &m), Err::Ok);
    CHECK(m.spec != nullptr && m.spec->kind == c.kind);
  }
  const Buf nonce(kNonceBytes, 9);
  Value root;
  ValidatedMessage m;
  CHECK_EQ(Check(BuildHello(1, nonce), Dir::ClientToEngine, &root, &m), Err::Ok);
}

QTEST(schema_text_fields_reject_control_characters) {
  const MessageSpec& ik = Spec("install_key");
  GenOptions o;
  for (const char* bad : {"a\nb", "a\rb", "a\tb", "a\x01" "b", "a\x1f" "b", "a\x7f" "b"}) {
    const Buf p = Str(bad);
    CHECK_EQ(CheckC2E(GenMessageWith(ik, o, "participant", &p)), Err::SchemaText);
  }
  const Buf nul = Str(std::string("a\0b", 3));
  CHECK_EQ(CheckC2E(GenMessageWith(ik, o, "participant", &nul)), Err::SchemaText);
  const Buf utf8 = Str("caf\xC3\xA9 \xE2\x82\xAC");
  CHECK_EQ(CheckC2E(GenMessageWith(ik, o, "participant", &utf8)), Err::Ok);

  // ICE server URLs (array of text) and the candidate line
  const MessageSpec& pc = Spec("pc_create");
  cbor::MapBuilder srv;
  srv.Raw("urls", cbor::ArrayBuilder().Add(Str("turn:h\n:1")).Finish());
  const Buf urls = cbor::ArrayBuilder().Add(srv.Finish()).Finish();
  GenOptions full;
  full.full = true;
  CHECK_EQ(CheckC2E(GenMessageWith(pc, full, "ice_servers", &urls)), Err::SchemaText);
  const MessageSpec& cand = Spec("add_ice_candidate");
  const Buf line = Str("candidate:1 1 udp 1 1.2.3.4 5 typ host\r\n");
  CHECK_EQ(CheckC2E(GenMessageWith(cand, o, "candidate", &line)), Err::SchemaText);

  // SDP may contain line breaks and tabs, but not NUL or other control characters.
  const MessageSpec& sd = Spec("set_remote_description");
  const Buf crlf = Str("v=0\r\na=b\tc\r\n");
  CHECK_EQ(CheckC2E(GenMessageWith(sd, o, "sdp", &crlf)), Err::Ok);
  const Buf sdp_nul = Str(std::string("v=0\r\n\0", 6));
  CHECK_EQ(CheckC2E(GenMessageWith(sd, o, "sdp", &sdp_nul)), Err::SchemaText);
  const Buf sdp_ctl = Str("v=0\r\n\x07");
  CHECK_EQ(CheckC2E(GenMessageWith(sd, o, "sdp", &sdp_ctl)), Err::SchemaText);
  const Buf sdp_del = Str("v=0\x7f");
  CHECK_EQ(CheckC2E(GenMessageWith(sd, o, "sdp", &sdp_del)), Err::SchemaText);

  // keys and text values of the stats scalar maps
  const MessageSpec& st = Spec("stats");
  auto entries_with = [&](Buf values) {
    cbor::MapBuilder e;
    e.Str("type", "t").Str("id", "x").Raw("values", std::move(values));
    return cbor::ArrayBuilder().Add(e.Finish()).Finish();
  };
  GenOptions sfull;
  sfull.full = true;
  Value r1, r2, r3;
  const Buf bad_key = entries_with(cbor::MapBuilder().Uint("a\nb", 1).Finish());
  CHECK_EQ(Check(GenMessageWith(st, sfull, "entries", &bad_key), Dir::EngineToClient, &r1), Err::SchemaText);
  const Buf bad_val = entries_with(cbor::MapBuilder().Str("a", "x\ny").Finish());
  CHECK_EQ(Check(GenMessageWith(st, sfull, "entries", &bad_val), Dir::EngineToClient, &r2), Err::SchemaText);
  const Buf good = entries_with(cbor::MapBuilder().Str("a", "x y").Finish());
  CHECK_EQ(Check(GenMessageWith(st, sfull, "entries", &good), Dir::EngineToClient, &r3), Err::Ok);
}
