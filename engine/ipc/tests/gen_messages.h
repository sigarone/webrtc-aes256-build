// Test support: builds a valid message of every kind straight from the validator table, so the
// tests and the seed corpus follow the schema automatically.
#pragma once

#include <cstddef>
#include <cstdint>

#include "qmedia/ipc/cbor.h"
#include "qmedia/ipc/schema.h"

namespace qmedia::testgen {

struct GenOptions {
  bool full = false;    // false: required fields at their lower bound. true: every field, upper bound.
  size_t len_cap = 0;   // 0 = no cap; otherwise strings, byte strings and arrays are capped
  uint32_t id = 7;      // used for requests, replies and "err"; events always carry 0
};

// One valid message of the given kind.
ipc::cbor::Buf GenMessage(const ipc::MessageSpec& spec, const GenOptions& opt);

// The same message with exactly one of the envelope fields replaced or removed. Used by tests.
ipc::cbor::Buf GenMessageWith(const ipc::MessageSpec& spec, const GenOptions& opt,
                         const char* override_key, const ipc::cbor::Buf* override_value);

}  // namespace qmedia::testgen
