// Seed corpus definition, shared by the generator (corpus_gen) and the freshness test.
// The corpus is committed under engine/ipc/fuzz/corpus; test_corpus fails when a file is missing
// or differs from what this header produces, so a schema change cannot leave stale seeds behind.
#pragma once

#include <string>
#include <vector>

#include "gen_messages.h"
#include "qmedia/ipc/frame.h"
#include "qmedia/ipc/limits.h"
#include "qmedia/ipc/message.h"
#include "qmedia/ipc/schema.h"

namespace qmedia::testgen {

struct CorpusFile {
  std::string subdir;  // "message" or "session"
  std::string name;
  ipc::cbor::Buf bytes;
};

// The nonce the session fuzzer and the session seeds agree on: bytes 0x00 .. 0x1F.
inline ipc::cbor::Buf FixedNonce() {
  ipc::cbor::Buf n(ipc::kNonceBytes);
  for (size_t i = 0; i < n.size(); ++i) n[i] = static_cast<uint8_t>(i);
  return n;
}

inline std::string Dirname(ipc::Dir d) { return d == ipc::Dir::ClientToEngine ? "c2e" : "e2c"; }

inline ipc::cbor::Buf Raw(std::initializer_list<int> v) {
  ipc::cbor::Buf b;
  for (int x : v) b.push_back(static_cast<uint8_t>(x));
  return b;
}

inline void AppendFrame(ipc::cbor::Buf& stream, const ipc::cbor::Buf& payload) {
  const ipc::cbor::Buf f = ipc::EncodeFrame(payload);
  stream.insert(stream.end(), f.begin(), f.end());
}

inline std::vector<CorpusFile> BuildCorpus() {
  using ipc::cbor::Buf;
  std::vector<CorpusFile> out;
  auto msg = [&](std::string name, Buf b) { out.push_back({"message", std::move(name), std::move(b)}); };
  auto ses = [&](std::string name, Buf b) { out.push_back({"session", std::move(name), std::move(b)}); };

  // One minimal and one full message per kind.
  for (const ipc::MessageSpec& spec : ipc::Messages()) {
    GenOptions mn;
    GenOptions fl;
    fl.full = true;
    fl.len_cap = 40;
    msg("ok_" + Dirname(spec.dir) + "_" + std::string(spec.kind) + "_min.cbor", GenMessage(spec, mn));
    msg("ok_" + Dirname(spec.dir) + "_" + std::string(spec.kind) + "_full.cbor", GenMessage(spec, fl));
  }

  // Not CBOR in the accepted subset.
  msg("bad_cbor_nonshortest_int.cbor", Raw({0x18, 0x05}));
  msg("bad_cbor_negative_int.cbor", Raw({0x20}));
  msg("bad_cbor_tag.cbor", Raw({0xC0, 0x00}));
  msg("bad_cbor_float.cbor", Raw({0xF9, 0x3C, 0x00}));
  msg("bad_cbor_indefinite_array.cbor", Raw({0x9F, 0x01, 0xFF}));
  msg("bad_cbor_duplicate_keys.cbor", Raw({0xA2, 0x61, 'a', 0x01, 0x61, 'a', 0x02}));
  msg("bad_cbor_unsorted_keys.cbor", Raw({0xA2, 0x61, 'b', 0x01, 0x61, 'a', 0x02}));
  msg("bad_cbor_integer_key.cbor", Raw({0xA1, 0x01, 0x01}));
  msg("bad_cbor_bad_utf8.cbor", Raw({0x61, 0xFF}));
  msg("bad_cbor_trailing.cbor", Raw({0x00, 0x00}));
  msg("bad_cbor_truncated_map.cbor", Raw({0xA1, 0x61, 'a'}));
  msg("bad_cbor_too_deep.cbor", Raw({0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0x00}));
  msg("bad_cbor_huge_array.cbor", Raw({0x9B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  msg("bad_cbor_huge_string.cbor", Raw({0x7A, 0xFF, 0xFF, 0xFF, 0xFF}));

  // Valid CBOR that the schema must refuse.
  {
    const ipc::MessageSpec& ik = *ipc::FindMessage("install_key");
    const ipc::MessageSpec& ping = *ipc::FindMessage("ping");
    GenOptions o;
    Buf zero_key;
    ipc::cbor::PutBin(zero_key, Buf(32, 0));
    msg("bad_c2e_zero_key.cbor", GenMessageWith(ik, o, "key", &zero_key));
    Buf short_key;
    ipc::cbor::PutBin(short_key, Buf(31, 0x55));
    msg("bad_c2e_short_key.cbor", GenMessageWith(ik, o, "key", &short_key));
    Buf slot16;
    ipc::cbor::PutUint(slot16, 16);
    msg("bad_c2e_slot_16.cbor", GenMessageWith(ik, o, "slot", &slot16));
    msg("bad_c2e_missing_key.cbor", GenMessageWith(ik, o, "key", nullptr));
    Buf v2;
    ipc::cbor::PutUint(v2, 2);
    msg("bad_c2e_version_2.cbor", GenMessageWith(ping, o, "v", &v2));
    Buf unknown;
    ipc::cbor::PutStr(unknown, "no_such_kind");
    msg("bad_c2e_unknown_kind.cbor", GenMessageWith(ping, o, "t", &unknown));
    Buf id0;
    ipc::cbor::PutUint(id0, 0);
    msg("bad_c2e_request_id_0.cbor", GenMessageWith(ping, o, "id", &id0));
    msg("bad_c2e_extra_field.cbor", ipc::BeginMessage("ping", 5).Uint("zz", 1).Finish());
    msg("bad_c2e_wrong_direction.cbor", ipc::BuildPong(5));
    msg("bad_e2c_wrong_direction.cbor", ipc::BuildHello(5, FixedNonce()));
  }

  // Frame streams for the session fuzzer (hello with the fixed nonce first).
  {
    const Buf nonce = FixedNonce();
    Buf happy;
    AppendFrame(happy, ipc::BuildHello(1, nonce));
    AppendFrame(happy, ipc::BeginMessage("ping", 2).Finish());
    AppendFrame(happy, ipc::BeginMessage("shutdown", 3).Finish());
    ses("happy.bin", happy);

    Buf hello_only;
    AppendFrame(hello_only, ipc::BuildHello(1, nonce));
    ses("hello_only.bin", hello_only);

    Buf wrong = nonce;
    wrong[31] ^= 1;
    Buf wrong_stream;
    AppendFrame(wrong_stream, ipc::BuildHello(1, wrong));
    ses("wrong_nonce.bin", wrong_stream);

    Buf every_kind;
    AppendFrame(every_kind, ipc::BuildHello(1, nonce));
    GenOptions mn;
    for (const ipc::MessageSpec& spec : ipc::Messages()) {
      if (spec.dir == ipc::Dir::ClientToEngine && spec.kind != "hello" && spec.kind != "shutdown") {
        AppendFrame(every_kind, GenMessage(spec, mn));
      }
    }
    ses("every_kind.bin", every_kind);

    Buf oversized;
    AppendFrame(oversized, ipc::BuildHello(1, nonce));
    const Buf huge = Raw({0x00, 0x10, 0x00, 0x01});
    oversized.insert(oversized.end(), huge.begin(), huge.end());
    ses("oversized_frame.bin", oversized);

    Buf second_hello;
    AppendFrame(second_hello, ipc::BuildHello(1, nonce));
    AppendFrame(second_hello, ipc::BuildHello(2, nonce));
    ses("second_hello.bin", second_hello);

    Buf truncated = happy;
    truncated.resize(truncated.size() - 4);
    ses("truncated.bin", truncated);
  }
  return out;
}

}  // namespace qmedia::testgen
