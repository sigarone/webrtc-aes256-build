// Strict deterministic CBOR subset (RFC 8949 section 4.2.1), hand written on purpose.
//
// Accepted data items:
//   unsigned integer, byte string, text string (valid UTF-8), array, map with text-string keys,
//   false, true, null. All lengths are definite.
// Rejected (the decoder never produces them and never skips over them):
//   negative integers, tags, floats, undefined, other simple values, indefinite lengths,
//   non-shortest integer or length encodings, unsorted or duplicate map keys, trailing bytes.
// Because the decoder only accepts the canonical form, Encode(Decode(x)) == x for every accepted x.
//
// The decoder is zero-copy: byte and text strings in the decoded Value are views into the input
// buffer. The caller owns that buffer and is responsible for wiping it after use.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "qmedia/ipc/status.h"

namespace qmedia::ipc::cbor {

using Buf = std::vector<uint8_t>;

struct Value {
  enum class Type : uint8_t { Uint, Bytes, Text, Array, Map, Bool, Null };

  Type type = Type::Null;
  uint64_t u = 0;                // Uint value; Bool: 0 or 1
  std::span<const uint8_t> raw;  // Bytes / Text payload (view into the decoded buffer)
  std::vector<Value> items;      // Array: elements. Map: key0, value0, key1, value1, ...

  std::string_view text() const {
    return std::string_view(reinterpret_cast<const char*>(raw.data()), raw.size());
  }
};

// Decodes exactly one data item that must span the whole input.
Err Decode(std::span<const uint8_t> in, Value* out);

// Map lookup by key. Returns nullptr when v is not a map or the key is absent.
const Value* MapGet(const Value& map, std::string_view key);

// Canonical encoding of a Value (maps are sorted by encoded key).
void Encode(const Value& v, Buf* out);

// ---- Encoding primitives and builders -----------------------------------------------------

void PutHead(Buf& out, uint8_t major, uint64_t arg);
void PutUint(Buf& out, uint64_t v);
void PutBin(Buf& out, std::span<const uint8_t> v);
void PutStr(Buf& out, std::string_view v);
void PutBool(Buf& out, bool v);
void PutNull(Buf& out);

// Collects (key, encoded value) pairs and emits a deterministic map. Duplicate keys are a bug of
// the caller; the decoder rejects them.
class MapBuilder {
 public:
  MapBuilder& Uint(std::string_view key, uint64_t v);
  MapBuilder& Flag(std::string_view key, bool v);
  MapBuilder& Str(std::string_view key, std::string_view v);
  MapBuilder& Bin(std::string_view key, std::span<const uint8_t> v);
  MapBuilder& Raw(std::string_view key, Buf encoded_value);
  Buf Finish() const;

 private:
  struct Entry {
    Buf key;
    Buf value;
  };
  std::vector<Entry> entries_;
};

class ArrayBuilder {
 public:
  ArrayBuilder& Add(Buf encoded_item);
  Buf Finish() const;

 private:
  std::vector<Buf> items_;
};

}  // namespace qmedia::ipc::cbor
