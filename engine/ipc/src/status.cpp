#include "qmedia/ipc/status.h"

namespace qmedia::ipc {

const char* ErrName(Err e) noexcept {
  switch (e) {
    case Err::Ok: return "ok";
    case Err::IoEof: return "io_eof";
    case Err::IoTimeout: return "io_timeout";
    case Err::IoError: return "io_error";
    case Err::IoTruncated: return "io_truncated";
    case Err::FrameEmpty: return "frame_empty";
    case Err::FrameTooLarge: return "frame_too_large";
    case Err::CborTruncated: return "cbor_truncated";
    case Err::CborTrailing: return "cbor_trailing";
    case Err::CborNonShortest: return "cbor_non_shortest";
    case Err::CborUnsupported: return "cbor_unsupported";
    case Err::CborBadUtf8: return "cbor_bad_utf8";
    case Err::CborTooDeep: return "cbor_too_deep";
    case Err::CborTooLarge: return "cbor_too_large";
    case Err::CborKeyOrder: return "cbor_key_order";
    case Err::CborKeyType: return "cbor_key_type";
    case Err::SchemaNotMap: return "schema_not_map";
    case Err::SchemaHeader: return "schema_header";
    case Err::SchemaVersion: return "schema_version";
    case Err::SchemaUnknownKind: return "schema_unknown_kind";
    case Err::SchemaDirection: return "schema_direction";
    case Err::SchemaId: return "schema_id";
    case Err::SchemaUnknownField: return "schema_unknown_field";
    case Err::SchemaMissingField: return "schema_missing_field";
    case Err::SchemaType: return "schema_type";
    case Err::SchemaRange: return "schema_range";
    case Err::SchemaLength: return "schema_length";
    case Err::SchemaEnum: return "schema_enum";
    case Err::SchemaZeroKey: return "schema_zero_key";
    case Err::SchemaText: return "schema_text";
    case Err::SessionNotHello: return "session_not_hello";
    case Err::SessionNonce: return "session_nonce";
    case Err::SessionHelloRepeat: return "session_hello_repeat";
    case Err::SessionBadNonceSize: return "session_bad_nonce_size";
    case Err::PipeBadName: return "pipe_bad_name";
    case Err::PipeSecurity: return "pipe_security";
    case Err::PipeCreate: return "pipe_create";
    case Err::PipeBusy: return "pipe_busy";
    case Err::PipeNotFound: return "pipe_not_found";
    case Err::PipeAccessDenied: return "pipe_access_denied";
    case Err::PipeClientPid: return "pipe_client_pid";
    case Err::PipeServerPid: return "pipe_server_pid";
    case Err::PipeNonceRead: return "pipe_nonce_read";
  }
  return "unknown";
}

}  // namespace qmedia::ipc
