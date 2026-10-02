// libFuzzer target: CBOR decoder + schema validator.
// Properties checked on every input:
//  - decoding never crashes, over-reads or allocates unboundedly (ASan/UBSan, hard node limit);
//  - an accepted input is canonical: re-encoding the decoded value gives the same bytes;
//  - validation of a decoded value never crashes, in either direction;
//  - a validated message re-validates identically (the validator has no hidden state);
//  - an independent oracle (oracle.h) agrees with the validator on every decoded input, in both
//    directions: same verdict and same kind. The oracle re-implements every rule, including the
//    envelope (version, direction, id rule) and every nested object, array element and scalar
//    map, so a validator that fails open (or closed) anywhere is caught without needing a crash;
//  - every accepted message also satisfies the hand-pinned invariants (32-byte non-zero key with a
//    slot of 0 to 15 in install_key, 32-byte nonce in hello), which catches a wrong table edit.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

#include "oracle.h"
#include "qmedia/ipc/cbor.h"
#include "qmedia/ipc/schema.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace qmedia::ipc;
  cbor::Value v;
  const Err e = cbor::Decode(std::span<const uint8_t>(data, size), &v);
  if (e != Err::Ok) return 0;

  cbor::Buf again;
  cbor::Encode(v, &again);
  if (again.size() != size || (size != 0 && std::memcmp(again.data(), data, size) != 0)) {
    std::abort();  // canonical form violated
  }

  for (Dir d : {Dir::ClientToEngine, Dir::EngineToClient}) {
    ValidatedMessage m1, m2;
    const Err r1 = ValidateMessage(v, d, &m1);
    const Err r2 = ValidateMessage(v, d, &m2);
    if (r1 != r2) std::abort();
    const MessageSpec* expected = oracle::Accepts(v, d);
    if ((r1 == Err::Ok) != (expected != nullptr)) std::abort();  // validator and oracle disagree
    if (r1 == Err::Ok) {
      if (m1.spec != m2.spec || m1.id != m2.id) std::abort();
      if (m1.spec != expected || m1.spec->dir != d || m1.root != &v) std::abort();
      if (static_cast<uint64_t>(m1.id) != cbor::MapGet(v, "id")->u) std::abort();
      if (!oracle::PinnedInvariantsHold(v, *m1.spec)) std::abort();
    }
  }
  return 0;
}
