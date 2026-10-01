// libFuzzer target: CBOR decoder + schema validator.
// Properties checked on every input:
//  - decoding never crashes, over-reads or allocates unboundedly (ASan/UBSan, hard node limit);
//  - an accepted input is canonical: re-encoding the decoded value gives the same bytes;
//  - validation of a decoded value never crashes, in either direction;
//  - a validated message re-validates identically (the validator has no hidden state).
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>

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
    if (r1 == Err::Ok) {
      if (m1.spec != m2.spec || m1.id != m2.id) std::abort();
      if (m1.spec == nullptr || m1.spec->dir != d) std::abort();
    }
  }
  return 0;
}
