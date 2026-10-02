// Independent second implementation of the message rules, used as an oracle for the validator in
// src/schema.cpp by fuzz_message and by a unit test.
//
// It shares the spec table (Messages()) with the validator and nothing else: every check is
// written again here, and it descends into every nested object, array element and scalar map,
// so a validator that fails open (or closed) anywhere in a message, not only at the top level,
// disagrees with it. On top of the table it pins a few invariants by hand (the key and the nonce
// sizes), so a wrong edit of the table itself is caught too.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

#include "qmedia/ipc/cbor.h"
#include "qmedia/ipc/limits.h"
#include "qmedia/ipc/schema.h"

namespace qmedia::ipc::oracle {

using Type = cbor::Value::Type;

inline bool TextOk(std::span<const uint8_t> raw, uint64_t lo, uint64_t hi, bool sdp) {
  if (raw.size() < lo || raw.size() > hi) return false;
  for (uint8_t b : raw) {
    const bool control = b < 0x20 || b == 0x7F;
    const bool line = b == 0x09 || b == 0x0A || b == 0x0D;
    if (control && !(sdp && line)) return false;
  }
  return true;
}

inline bool ObjectOk(std::span<const FieldSpec> fields, const cbor::Value& obj, bool envelope);

inline bool FieldOk(const FieldSpec& f, const cbor::Value& v) {
  switch (f.type) {
    case FType::Uint:
      return v.type == Type::Uint && v.u >= f.lo && v.u <= f.hi;
    case FType::Bool:
      return v.type == Type::Bool;
    case FType::Text:
      return v.type == Type::Text && TextOk(v.raw, f.lo, f.hi, false);
    case FType::Sdp:
      return v.type == Type::Text && TextOk(v.raw, f.lo, f.hi, true);
    case FType::Bytes: {
      if (v.type != Type::Bytes || v.raw.size() < f.lo || v.raw.size() > f.hi) return false;
      if (!f.nonzero) return true;
      for (uint8_t b : v.raw) {
        if (b != 0) return true;
      }
      return false;
    }
    case FType::Enum:
      if (v.type != Type::Text) return false;
      for (std::string_view e : f.enums) {
        if (e == v.text()) return true;
      }
      return false;
    case FType::Object:
      return ObjectOk(f.object, v, false);
    case FType::Array:
      if (v.type != Type::Array || v.items.size() < f.lo || v.items.size() > f.hi) return false;
      for (const cbor::Value& el : v.items) {
        if (f.elem == FType::Text) {
          if (el.type != Type::Text || !TextOk(el.raw, f.elem_lo, f.elem_hi, false)) return false;
        } else if (f.elem == FType::Object) {
          if (!ObjectOk(f.object, el, false)) return false;
        } else {
          return false;
        }
      }
      return true;
    case FType::ScalarMap:
      if (v.type != Type::Map) return false;
      if (v.items.size() / 2 < f.lo || v.items.size() / 2 > f.hi) return false;
      for (size_t i = 0; i + 1 < v.items.size(); i += 2) {
        const cbor::Value& k = v.items[i];
        const cbor::Value& x = v.items[i + 1];
        if (k.type != Type::Text || !TextOk(k.raw, 1, kMaxScalarKeyBytes, false)) return false;
        if (x.type == Type::Uint || x.type == Type::Bool) continue;
        if (x.type == Type::Text && TextOk(x.raw, 0, kMaxScalarTextBytes, false)) continue;
        return false;
      }
      return true;
  }
  return false;
}

inline bool ObjectOk(std::span<const FieldSpec> fields, const cbor::Value& obj, bool envelope) {
  if (obj.type != Type::Map) return false;
  for (size_t i = 0; i + 1 < obj.items.size(); i += 2) {
    const std::string_view key = obj.items[i].text();
    if (envelope && (key == "v" || key == "t" || key == "id")) continue;
    const FieldSpec* f = nullptr;
    for (const FieldSpec& c : fields) {
      if (c.name == key) {
        f = &c;
        break;
      }
    }
    if (f == nullptr || !FieldOk(*f, obj.items[i + 1])) return false;
  }
  for (const FieldSpec& c : fields) {
    if (!c.required) continue;
    bool present = false;
    for (size_t i = 0; i + 1 < obj.items.size(); i += 2) present = present || obj.items[i].text() == c.name;
    if (!present) return false;
  }
  return true;
}

// Hand-written invariants that must hold for an accepted message whatever the table says.
inline bool PinnedInvariantsHold(const cbor::Value& root, const MessageSpec& spec) {
  if (spec.kind == "install_key") {
    if (spec.dir != Dir::ClientToEngine) return false;
    const cbor::Value* key = cbor::MapGet(root, "key");
    const cbor::Value* slot = cbor::MapGet(root, "slot");
    const cbor::Value* participant = cbor::MapGet(root, "participant");
    if (key == nullptr || key->type != Type::Bytes || key->raw.size() != 32) return false;
    bool any = false;
    for (uint8_t b : key->raw) any = any || b != 0;
    if (!any) return false;
    if (slot == nullptr || slot->type != Type::Uint || slot->u > 15) return false;
    if (participant == nullptr || participant->type != Type::Text || participant->raw.empty()) return false;
  }
  if (spec.kind == "hello") {
    if (spec.dir != Dir::ClientToEngine) return false;
    const cbor::Value* nonce = cbor::MapGet(root, "nonce");
    if (nonce == nullptr || nonce->type != Type::Bytes || nonce->raw.size() != 32) return false;
  }
  return true;
}

// The spec of the message if the rules accept `root` travelling in direction `dir`, else nullptr.
inline const MessageSpec* Accepts(const cbor::Value& root, Dir dir) {
  if (root.type != Type::Map) return nullptr;
  const cbor::Value* v = cbor::MapGet(root, "v");
  const cbor::Value* t = cbor::MapGet(root, "t");
  const cbor::Value* id = cbor::MapGet(root, "id");
  if (v == nullptr || t == nullptr || id == nullptr) return nullptr;
  if (v->type != Type::Uint || v->u != 1) return nullptr;
  if (t->type != Type::Text) return nullptr;
  if (id->type != Type::Uint || id->u > 0xFFFFFFFFull) return nullptr;
  const MessageSpec* spec = nullptr;
  for (const MessageSpec& m : Messages()) {
    if (m.kind == t->text()) {
      spec = &m;
      break;
    }
  }
  if (spec == nullptr || spec->dir != dir) return nullptr;
  switch (spec->id_rule) {
    case IdRule::Request:
    case IdRule::Response:
      if (id->u == 0) return nullptr;
      break;
    case IdRule::Event:
      if (id->u != 0) return nullptr;
      break;
    case IdRule::Any:
      break;
  }
  if (!ObjectOk(spec->fields, root, true)) return nullptr;
  return spec;
}

}  // namespace qmedia::ipc::oracle
