// libFuzzer target: CBOR decoder + schema validator.
// Properties checked on every input:
//  - decoding never crashes, over-reads or allocates unboundedly (ASan/UBSan, hard node limit);
//  - an accepted input is canonical: re-encoding the decoded value gives the same bytes;
//  - validation of a decoded value never crashes, in either direction;
//  - a validated message re-validates identically (the validator has no hidden state);
//  - an independent oracle agrees with every message the validator accepted: no unknown top-level
//    field, every required one present, scalar fields inside their declared type and range, and
//    for install_key the key is exactly 32 bytes, not all zero, with a slot of 0 to 15. A validator
//    that fails open would otherwise only be caught by a crash, which it never causes.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>
#include <string_view>

#include "qmedia/ipc/cbor.h"
#include "qmedia/ipc/limits.h"
#include "qmedia/ipc/schema.h"

namespace {

using namespace qmedia::ipc;

bool HasControl(std::span<const uint8_t> raw, bool allow_breaks) {
  for (uint8_t b : raw) {
    if (b >= 0x20 && b != 0x7F) continue;
    if (allow_breaks && (b == 0x09 || b == 0x0A || b == 0x0D)) continue;
    return true;
  }
  return false;
}

// Re-checks a validated message against its spec without using the validator's code.
void AbortUnlessConsistent(const cbor::Value& root, const ValidatedMessage& m) {
  if (root.type != cbor::Value::Type::Map || m.spec == nullptr) std::abort();
  for (size_t i = 0; i + 1 < root.items.size(); i += 2) {
    const std::string_view key = root.items[i].text();
    if (key == "v" || key == "t" || key == "id") continue;
    bool known = false;
    for (const FieldSpec& f : m.spec->fields) known = known || f.name == key;
    if (!known) std::abort();  // an unknown field was accepted
  }
  for (const FieldSpec& f : m.spec->fields) {
    const cbor::Value* v = cbor::MapGet(root, f.name);
    if (v == nullptr) {
      if (f.required) std::abort();  // a required field is missing
      continue;
    }
    switch (f.type) {
      case FType::Uint:
        if (v->type != cbor::Value::Type::Uint || v->u < f.lo || v->u > f.hi) std::abort();
        break;
      case FType::Bool:
        if (v->type != cbor::Value::Type::Bool) std::abort();
        break;
      case FType::Text:
      case FType::Sdp:
        if (v->type != cbor::Value::Type::Text || v->raw.size() < f.lo || v->raw.size() > f.hi ||
            HasControl(v->raw, f.type == FType::Sdp)) {
          std::abort();
        }
        break;
      case FType::Bytes: {
        if (v->type != cbor::Value::Type::Bytes || v->raw.size() < f.lo || v->raw.size() > f.hi) {
          std::abort();
        }
        if (f.nonzero) {
          bool any = false;
          for (uint8_t b : v->raw) any = any || b != 0;
          if (!any) std::abort();
        }
        break;
      }
      case FType::Enum: {
        bool ok = false;
        if (v->type == cbor::Value::Type::Text) {
          for (std::string_view e : f.enums) ok = ok || e == v->text();
        }
        if (!ok) std::abort();
        break;
      }
      case FType::Object:
      case FType::ScalarMap:
        if (v->type != cbor::Value::Type::Map) std::abort();
        break;
      case FType::Array:
        if (v->type != cbor::Value::Type::Array || v->items.size() < f.lo || v->items.size() > f.hi) {
          std::abort();
        }
        break;
    }
  }
  if (m.spec->kind == "install_key" && m.spec->dir == Dir::ClientToEngine) {
    const cbor::Value* key = cbor::MapGet(root, "key");
    const cbor::Value* slot = cbor::MapGet(root, "slot");
    if (key == nullptr || key->raw.size() != kKeyBytes) std::abort();
    if (slot == nullptr || slot->u > 15) std::abort();
  }
}

}  // namespace

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
      AbortUnlessConsistent(v, m1);
    }
  }
  return 0;
}
