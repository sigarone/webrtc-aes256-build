// Table-driven validator for the IPC messages. The table in schema.cpp is the machine-readable
// twin of schema.cddl (a test keeps the two in sync). Validation is fail-closed: unknown kinds,
// unknown fields, wrong types, out-of-range values and wrong directions are all rejected.
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "qmedia/ipc/cbor.h"
#include "qmedia/ipc/status.h"

namespace qmedia::ipc {

enum class Dir : uint8_t { ClientToEngine, EngineToClient };

// id rules: Request and Response carry an id >= 1 (the response echoes the request id);
// Event carries id 0; Any accepts both (used by "err", which may be connection level).
enum class IdRule : uint8_t { Request, Response, Event, Any };

// Text: UTF-8 without control characters (no NUL, no C0, no DEL). Sdp: the same, but CR, LF and HT
// are allowed. Both are length-checked in bytes.
enum class FType : uint8_t { Uint, Bool, Text, Sdp, Bytes, Enum, Object, Array, ScalarMap };

struct FieldSpec {
  std::string_view name;
  FType type;
  bool required;
  bool nonzero;     // Bytes only: reject an all-zero value (key material)
  uint64_t lo, hi;  // Uint: value range. Text/Bytes: length in bytes. Array/ScalarMap: entry count.
  std::span<const std::string_view> enums;  // Enum: allowed values
  std::span<const FieldSpec> object;        // Object, or the element object of an Array
  FType elem;                               // Array element type: Text or Object
  uint32_t elem_lo, elem_hi;                // Array of Text: element length range
};

struct MessageSpec {
  std::string_view kind;
  Dir dir;
  IdRule id_rule;
  std::span<const FieldSpec> fields;
};

std::span<const MessageSpec> Messages();
const MessageSpec* FindMessage(std::string_view kind);

struct ValidatedMessage {
  const MessageSpec* spec = nullptr;
  uint32_t id = 0;
  const cbor::Value* root = nullptr;  // the whole message map (valid while the buffer lives)
};

// Validates an already decoded message against the schema for the given direction.
Err ValidateMessage(const cbor::Value& root, Dir expected_dir, ValidatedMessage* out);

// Accessors for validated messages. A missing optional field yields nullptr / the fallback.
const cbor::Value* Field(const ValidatedMessage& m, std::string_view name);
uint64_t FieldUint(const ValidatedMessage& m, std::string_view name, uint64_t fallback = 0);
std::string_view FieldText(const ValidatedMessage& m, std::string_view name);

}  // namespace qmedia::ipc
