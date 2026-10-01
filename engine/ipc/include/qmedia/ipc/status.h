// One error enum for the whole IPC library (I/O, framing, CBOR, schema, session, pipe).
// Names are static strings; they never contain message content.
#pragma once

#include <cstdint>

namespace qmedia::ipc {

enum class Err : uint8_t {
  Ok = 0,
  // stream I/O
  IoEof,        // clean end of stream before any byte of a frame
  IoTimeout,
  IoError,
  IoTruncated,  // stream ended inside a frame
  // framing
  FrameEmpty,
  FrameTooLarge,
  // CBOR (strict deterministic subset)
  CborTruncated,
  CborTrailing,
  CborNonShortest,
  CborUnsupported,  // tags, floats, negative ints, indefinite lengths, simple values
  CborBadUtf8,
  CborTooDeep,
  CborTooLarge,
  CborKeyOrder,  // unsorted or duplicate map key
  CborKeyType,   // map key is not a short text string
  // schema
  SchemaNotMap,
  SchemaHeader,
  SchemaVersion,
  SchemaUnknownKind,
  SchemaDirection,
  SchemaId,
  SchemaUnknownField,
  SchemaMissingField,
  SchemaType,
  SchemaRange,
  SchemaLength,
  SchemaEnum,
  SchemaZeroKey,
  SchemaText,  // control character in a text field
  // session
  SessionNotHello,
  SessionNonce,
  SessionHelloRepeat,
  SessionBadNonceSize,
  // Windows named pipe transport
  PipeBadName,
  PipeSecurity,
  PipeCreate,
  PipeBusy,
  PipeNotFound,
  PipeAccessDenied,
  PipeClientPid,
  PipeServerPid,
  PipeNonceRead,
};

// Stable snake_case identifier, e.g. "cbor_non_shortest". Safe to log and to send as "detail".
const char* ErrName(Err e) noexcept;

}  // namespace qmedia::ipc
