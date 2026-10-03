// Builders for the messages the engine itself sends, plus the client-side hello (tests, helpers).
#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "qmedia/ipc/cbor.h"

namespace qmedia::ipc {

namespace errcode {
inline constexpr std::string_view kBadRequest = "bad_request";
inline constexpr std::string_view kUnsupported = "unsupported";
inline constexpr std::string_view kNotFound = "not_found";
inline constexpr std::string_view kInvalidState = "invalid_state";
inline constexpr std::string_view kInvalidKey = "invalid_key";
inline constexpr std::string_view kLimit = "limit";
inline constexpr std::string_view kInternal = "internal";
}  // namespace errcode

inline constexpr std::string_view kEngineIdent = "qaudion-media/0.1";

// A map builder that already carries the envelope fields v, t and id.
cbor::MapBuilder BeginMessage(std::string_view kind, uint32_t id);

cbor::Buf BuildHello(uint32_t id, std::span<const uint8_t> nonce);
cbor::Buf BuildHelloOk(uint32_t id);
cbor::Buf BuildPong(uint32_t id);
cbor::Buf BuildOk(uint32_t id);
// detail must be a static, content-free string (for example ErrName(...)).
cbor::Buf BuildErr(uint32_t id, std::string_view code, std::string_view detail = {});

// ---- Replies and events of the engine core -----------------------------------------------------
// Every builder produces a message that validates against schema.cddl in the engine-to-client
// direction (a unit test checks all of them). Text arguments must already satisfy the schema (no
// control characters, within the length bounds): the builders do not truncate.

cbor::Buf BuildSessionCreated(uint32_t id, uint32_t session);
// fingerprint: the raw SHA-256 of the DER certificate, exactly 32 bytes.
cbor::Buf BuildCertCreated(uint32_t id, uint32_t cert, std::span<const uint8_t> fingerprint);
cbor::Buf BuildPcCreated(uint32_t id, uint32_t pc);
// type is "offer" or "answer".
cbor::Buf BuildSdpReady(uint32_t id, uint32_t pc, std::string_view type, std::string_view sdp);

struct DeviceInfo {
  std::string id;
  std::string name;
  std::string_view kind;  // "audio_in", "audio_out" or "video_in"
  bool is_default = false;
  bool has_is_communications = false;
  bool is_communications = false;
};
cbor::Buf BuildDevices(uint32_t id, std::span<const DeviceInfo> devices);

// One statistic value: the schema's scalar (unsigned integer, boolean or short text).
struct StatValue {
  enum class Kind : uint8_t { Uint, Bool, Text };
  Kind kind = Kind::Uint;
  uint64_t u = 0;
  bool b = false;
  std::string s;
  static StatValue OfUint(uint64_t v) {
    StatValue x;
    x.kind = Kind::Uint;
    x.u = v;
    return x;
  }
  static StatValue OfBool(bool v) {
    StatValue x;
    x.kind = Kind::Bool;
    x.b = v;
    return x;
  }
  static StatValue OfText(std::string v) {
    StatValue x;
    x.kind = Kind::Text;
    x.s = std::move(v);
    return x;
  }
};
struct StatEntry {
  std::string type;
  std::string id;
  std::vector<std::pair<std::string, StatValue>> values;
};
cbor::Buf BuildStats(uint32_t id, uint32_t pc, std::span<const StatEntry> entries);

// Events (id 0). mline_index < 0 means "not present".
cbor::Buf BuildIceCandidate(uint32_t pc, std::string_view candidate, std::string_view mid,
                            int mline_index);
cbor::Buf BuildIceGatheringState(uint32_t pc, std::string_view state);
cbor::Buf BuildIceConnectionState(uint32_t pc, std::string_view state);
cbor::Buf BuildPcState(uint32_t pc, std::string_view state);
cbor::Buf BuildNegotiationNeeded(uint32_t pc);
cbor::Buf BuildTransportInfo(uint32_t pc, std::string_view tls_version, std::string_view dtls_cipher,
                             std::string_view group, std::string_view srtp_cipher,
                             std::span<const uint8_t> remote_cert_fingerprint,
                             std::span<const uint8_t> local_cert_fingerprint);
cbor::Buf BuildTransportViolation(uint32_t pc, std::string_view reason);
cbor::Buf BuildCryptorState(uint32_t pc, std::string_view mid, std::string_view participant,
                            std::string_view kind, std::string_view state);
cbor::Buf BuildDevicesChanged();

}  // namespace qmedia::ipc
