#include "qmedia/ipc/schema.h"

#include <cstddef>

#include "qmedia/ipc/limits.h"

namespace qmedia::ipc {

namespace {

using SV = std::string_view;
using cbor::Value;

constexpr std::span<const SV> kNoEnums{};
constexpr std::span<const FieldSpec> kNoFields{};

// ---- Field constructors -------------------------------------------------------------------
constexpr FieldSpec Num(SV n, uint64_t lo, uint64_t hi, bool req = true) {
  return FieldSpec{n, FType::Uint, req, false, lo, hi, kNoEnums, kNoFields, FType::Uint, 0, 0};
}
constexpr FieldSpec Hnd(SV n) { return Num(n, 1, 0xFFFFFFFFull, true); }
constexpr FieldSpec Flag(SV n, bool req = true) {
  return FieldSpec{n, FType::Bool, req, false, 0, 0, kNoEnums, kNoFields, FType::Uint, 0, 0};
}
constexpr FieldSpec Str(SV n, uint64_t lo, uint64_t hi, bool req = true) {
  return FieldSpec{n, FType::Text, req, false, lo, hi, kNoEnums, kNoFields, FType::Uint, 0, 0};
}
constexpr FieldSpec Sdp(SV n, uint64_t lo, uint64_t hi, bool req = true) {
  return FieldSpec{n, FType::Sdp, req, false, lo, hi, kNoEnums, kNoFields, FType::Uint, 0, 0};
}
constexpr FieldSpec Bin(SV n, uint64_t lo, uint64_t hi, bool req = true, bool nonzero = false) {
  return FieldSpec{n, FType::Bytes, req, nonzero, lo, hi, kNoEnums, kNoFields, FType::Uint, 0, 0};
}
constexpr FieldSpec En(SV n, std::span<const SV> e, bool req = true) {
  return FieldSpec{n, FType::Enum, req, false, 0, 0, e, kNoFields, FType::Uint, 0, 0};
}
constexpr FieldSpec ArrText(SV n, uint64_t lo, uint64_t hi, uint32_t tlo, uint32_t thi,
                            bool req = true) {
  return FieldSpec{n, FType::Array, req, false, lo, hi, kNoEnums, kNoFields, FType::Text, tlo, thi};
}
constexpr FieldSpec ArrObj(SV n, uint64_t lo, uint64_t hi, std::span<const FieldSpec> obj,
                           bool req = true) {
  return FieldSpec{n, FType::Array, req, false, lo, hi, kNoEnums, obj, FType::Object, 0, 0};
}
constexpr FieldSpec Smap(SV n, uint64_t lo, uint64_t hi, bool req = true) {
  return FieldSpec{n, FType::ScalarMap, req, false, lo, hi, kNoEnums, kNoFields, FType::Uint, 0, 0};
}

// ---- Enumerations -------------------------------------------------------------------------
constexpr SV kDirection[] = {"send", "recv"};
constexpr SV kIcePolicy[] = {"all", "relay"};
constexpr SV kSdpType[] = {"offer", "answer", "pranswer", "rollback"};
constexpr SV kSdpReadyType[] = {"offer", "answer"};
constexpr SV kTrack[] = {"mic", "camera", "screen", "remote_audio", "remote_video"};
constexpr SV kDeviceKind[] = {"audio_in", "audio_out", "video_in"};
constexpr SV kVideoSource[] = {"camera", "screen"};
constexpr SV kPcmPoint[] = {"capture", "playout"};
constexpr SV kStatsScope[] = {"all", "transport", "audio", "video"};
constexpr SV kMediaKind[] = {"audio", "video"};
constexpr SV kErrCode[] = {"bad_request", "unsupported", "not_found", "invalid_state",
                           "invalid_key", "limit",       "internal"};
constexpr SV kIceGathering[] = {"new", "gathering", "complete"};
constexpr SV kIceConn[] = {"new",    "checking",     "connected", "completed",
                           "failed", "disconnected", "closed"};
constexpr SV kPcState[] = {"new", "connecting", "connected", "disconnected", "failed", "closed"};
constexpr SV kViolation[] = {"no_dtls", "tls_version", "dtls_cipher", "group", "srtp_cipher"};
constexpr SV kCryptorState[] = {"ok", "missing_key", "decryption_failed", "encryption_failed",
                                "internal_error"};

// ---- Field lists --------------------------------------------------------------------------
constexpr FieldSpec kFHello[] = {Bin("nonce", kNonceBytes, kNonceBytes)};
constexpr FieldSpec kFHelloOk[] = {Str("engine", 1, 64), Num("max_frame", 1, kMaxFramePayload)};
constexpr FieldSpec kFSession[] = {Hnd("session")};
constexpr FieldSpec kFIceServer[] = {ArrText("urls", 1, 8, 1, 256), Str("username", 0, 256, false),
                                     Str("credential", 0, 256, false)};
constexpr FieldSpec kFPcCreate[] = {Hnd("session"), Hnd("cert"), En("ice_policy", kIcePolicy, false),
                                    ArrObj("ice_servers", 0, 8, kFIceServer, false)};
constexpr FieldSpec kFPc[] = {Hnd("pc")};
constexpr FieldSpec kFCreateOffer[] = {Hnd("pc"), Flag("ice_restart", false)};
constexpr FieldSpec kFSetDesc[] = {Hnd("pc"), En("type", kSdpType), Sdp("sdp", 0, 262144)};
constexpr FieldSpec kFCandidate[] = {Hnd("pc"), Str("candidate", 0, 2048), Str("mid", 0, 16, false),
                                     Num("mline_index", 0, 255, false)};
constexpr FieldSpec kFUpdateIce[] = {Hnd("pc"), ArrObj("ice_servers", 0, 8, kFIceServer)};
constexpr FieldSpec kFInstallKey[] = {Hnd("session"), Str("participant", 1, 64), Num("slot", 0, 15),
                                      Bin("key", kKeyBytes, kKeyBytes, true, true),
                                      En("direction", kDirection)};
constexpr FieldSpec kFRetire[] = {Hnd("session"), Str("participant", 1, 64),
                                  En("direction", kDirection), Num("slot", 0, 15)};
constexpr FieldSpec kFSelectSlot[] = {Hnd("session"), Str("participant", 1, 64), Num("slot", 0, 15)};
constexpr FieldSpec kFBindMedia[] = {Hnd("pc"), Str("mid", 1, 16), Str("participant", 1, 64),
                                     En("direction", kDirection)};
constexpr FieldSpec kFSetMuted[] = {Hnd("pc"), En("track", kTrack), Flag("muted")};
constexpr FieldSpec kFSelectDevice[] = {En("kind", kDeviceKind), Str("device", 0, 256)};
constexpr FieldSpec kFStartVideo[] = {
    Hnd("pc"),
    En("source", kVideoSource),
    Str("device", 0, 256, false),
    Str("mid", 1, 16, false),
    Num("width", 16, 8192, false),
    Num("height", 16, 8192, false),
    Num("fps", 1, 120, false),
    Num("max_bitrate_bps", 10000, 100000000, false)};
constexpr FieldSpec kFStopVideo[] = {Hnd("pc"), En("source", kVideoSource)};
constexpr FieldSpec kFKeyframe[] = {Hnd("pc"), Str("mid", 1, 16)};
constexpr FieldSpec kFLayer[] = {Str("rid", 1, 16), Flag("active"),
                                 Num("max_bitrate_bps", 10000, 100000000, false),
                                 Num("scale_down", 1, 16, false)};
constexpr FieldSpec kFSimulcast[] = {Hnd("pc"), Str("mid", 1, 16), ArrObj("layers", 1, 4, kFLayer)};
constexpr FieldSpec kFPcmTap[] = {Hnd("session"), En("point", kPcmPoint), Flag("enabled"),
                                  Num("sample_rate_hz", 8000, 48000, false),
                                  Num("frame_ms", 10, 100, false)};
constexpr FieldSpec kFGetStats[] = {Hnd("pc"), En("scope", kStatsScope, false)};
constexpr FieldSpec kFTuning[] = {Hnd("pc"), Num("ptime_ms", 10, 120, false),
                                  Num("bitrate_bps", 6000, 510000, false), Flag("cbr", false),
                                  Num("fec_floor_pct", 0, 100, false)};

constexpr FieldSpec kFErr[] = {En("code", kErrCode), Str("detail", 0, 128, false)};
constexpr FieldSpec kFCertCreated[] = {Hnd("cert"), Bin("fingerprint", kFingerprintBytes,
                                                        kFingerprintBytes)};
constexpr FieldSpec kFSdpReady[] = {Hnd("pc"), En("type", kSdpReadyType), Sdp("sdp", 1, 262144)};
constexpr FieldSpec kFDevice[] = {Str("id", 0, 256), Str("name", 0, 128), En("kind", kDeviceKind),
                                  Flag("is_default"), Flag("is_communications", false)};
constexpr FieldSpec kFDevices[] = {ArrObj("devices", 0, 64, kFDevice)};
constexpr FieldSpec kFStatEntry[] = {Str("type", 1, 32), Str("id", 0, 64), Smap("values", 0, 64)};
constexpr FieldSpec kFStats[] = {Hnd("pc"), ArrObj("entries", 0, 64, kFStatEntry)};
constexpr FieldSpec kFIceGathering[] = {Hnd("pc"), En("state", kIceGathering)};
constexpr FieldSpec kFIceConn[] = {Hnd("pc"), En("state", kIceConn)};
constexpr FieldSpec kFPcState[] = {Hnd("pc"), En("state", kPcState)};
constexpr FieldSpec kFTransportInfo[] = {
    Hnd("pc"),
    Str("tls_version", 1, 16),
    Str("dtls_cipher", 1, 64),
    Str("group", 0, 64),
    Str("srtp_cipher", 1, 64),
    Bin("remote_cert_fingerprint", kFingerprintBytes, kFingerprintBytes)};
constexpr FieldSpec kFViolation[] = {Hnd("pc"), En("reason", kViolation)};
constexpr FieldSpec kFCryptor[] = {Hnd("pc"), Str("mid", 1, 16), Str("participant", 1, 64),
                                   En("kind", kMediaKind), En("state", kCryptorState)};
constexpr FieldSpec kFPcmFrame[] = {Hnd("session"), En("point", kPcmPoint),
                                    Num("seq", 0, 0xFFFFFFFFull), Num("sample_rate_hz", 8000, 48000),
                                    Num("channels", 1, 2), Bin("samples", 0, 32768)};

constexpr Dir C2E = Dir::ClientToEngine;
constexpr Dir E2C = Dir::EngineToClient;

constexpr std::span<const FieldSpec> kEmpty{};

// Keep this list in the same order as schema.cddl.
constexpr MessageSpec kMessages[] = {
    // client -> engine
    {"hello", C2E, IdRule::Request, kFHello},
    {"ping", C2E, IdRule::Request, kEmpty},
    {"shutdown", C2E, IdRule::Request, kEmpty},
    {"session_create", C2E, IdRule::Request, kEmpty},
    {"session_close", C2E, IdRule::Request, kFSession},
    {"cert_create", C2E, IdRule::Request, kFSession},
    {"pc_create", C2E, IdRule::Request, kFPcCreate},
    {"pc_close", C2E, IdRule::Request, kFPc},
    {"create_offer", C2E, IdRule::Request, kFCreateOffer},
    {"create_answer", C2E, IdRule::Request, kFPc},
    {"set_local_description", C2E, IdRule::Request, kFSetDesc},
    {"set_remote_description", C2E, IdRule::Request, kFSetDesc},
    {"add_ice_candidate", C2E, IdRule::Request, kFCandidate},
    {"restart_ice", C2E, IdRule::Request, kFPc},
    {"update_ice_servers", C2E, IdRule::Request, kFUpdateIce},
    {"install_key", C2E, IdRule::Request, kFInstallKey},
    {"retire_slot", C2E, IdRule::Request, kFRetire},
    {"select_send_slot", C2E, IdRule::Request, kFSelectSlot},
    {"bind_media", C2E, IdRule::Request, kFBindMedia},
    {"set_muted", C2E, IdRule::Request, kFSetMuted},
    {"list_devices", C2E, IdRule::Request, kEmpty},
    {"select_device", C2E, IdRule::Request, kFSelectDevice},
    {"start_video", C2E, IdRule::Request, kFStartVideo},
    {"stop_video", C2E, IdRule::Request, kFStopVideo},
    {"request_keyframe", C2E, IdRule::Request, kFKeyframe},
    {"set_simulcast", C2E, IdRule::Request, kFSimulcast},
    {"pcm_tap", C2E, IdRule::Request, kFPcmTap},
    {"get_stats", C2E, IdRule::Request, kFGetStats},
    {"set_audio_tuning", C2E, IdRule::Request, kFTuning},
    // engine -> client: responses
    {"hello_ok", E2C, IdRule::Response, kFHelloOk},
    {"pong", E2C, IdRule::Response, kEmpty},
    {"ok", E2C, IdRule::Response, kEmpty},
    {"err", E2C, IdRule::Any, kFErr},
    {"session_created", E2C, IdRule::Response, kFSession},
    {"cert_created", E2C, IdRule::Response, kFCertCreated},
    {"pc_created", E2C, IdRule::Response, kFPc},
    {"sdp_ready", E2C, IdRule::Response, kFSdpReady},
    {"devices", E2C, IdRule::Response, kFDevices},
    {"stats", E2C, IdRule::Response, kFStats},
    // engine -> client: events
    {"ice_candidate", E2C, IdRule::Event, kFCandidate},
    {"ice_gathering_state", E2C, IdRule::Event, kFIceGathering},
    {"ice_connection_state", E2C, IdRule::Event, kFIceConn},
    {"pc_state", E2C, IdRule::Event, kFPcState},
    {"negotiation_needed", E2C, IdRule::Event, kFPc},
    {"transport_info", E2C, IdRule::Event, kFTransportInfo},
    {"transport_violation", E2C, IdRule::Event, kFViolation},
    {"cryptor_state", E2C, IdRule::Event, kFCryptor},
    {"devices_changed", E2C, IdRule::Event, kEmpty},
    {"pcm_frame", E2C, IdRule::Event, kFPcmFrame},
};

// UTF-8 is already validated by the decoder. Here: no NUL, no C0 control characters, no DEL, so a
// text field can neither truncate a C string nor smuggle a line break into a log or a header.
bool CleanText(std::span<const uint8_t> raw, bool allow_line_breaks) {
  for (uint8_t b : raw) {
    if (b >= 0x20 && b != 0x7F) continue;
    if (allow_line_breaks && (b == '\t' || b == '\n' || b == '\r')) continue;
    return false;
  }
  return true;
}

bool Contains(std::span<const SV> set, SV v) {
  for (SV s : set) {
    if (s == v) return true;
  }
  return false;
}

Err CheckObject(std::span<const FieldSpec> fields, const Value& obj, bool skip_header);

Err CheckValue(const FieldSpec& f, const Value& v) {
  switch (f.type) {
    case FType::Uint:
      if (v.type != Value::Type::Uint) return Err::SchemaType;
      if (v.u < f.lo || v.u > f.hi) return Err::SchemaRange;
      return Err::Ok;
    case FType::Bool:
      return v.type == Value::Type::Bool ? Err::Ok : Err::SchemaType;
    case FType::Text:
    case FType::Sdp:
      if (v.type != Value::Type::Text) return Err::SchemaType;
      if (v.raw.size() < f.lo || v.raw.size() > f.hi) return Err::SchemaLength;
      if (!CleanText(v.raw, f.type == FType::Sdp)) return Err::SchemaText;
      return Err::Ok;
    case FType::Bytes: {
      if (v.type != Value::Type::Bytes) return Err::SchemaType;
      if (v.raw.size() < f.lo || v.raw.size() > f.hi) return Err::SchemaLength;
      if (f.nonzero) {
        uint8_t acc = 0;
        for (uint8_t b : v.raw) acc = static_cast<uint8_t>(acc | b);
        if (acc == 0) return Err::SchemaZeroKey;
      }
      return Err::Ok;
    }
    case FType::Enum:
      if (v.type != Value::Type::Text) return Err::SchemaType;
      return Contains(f.enums, v.text()) ? Err::Ok : Err::SchemaEnum;
    case FType::Object:
      return CheckObject(f.object, v, false);
    case FType::Array: {
      if (v.type != Value::Type::Array) return Err::SchemaType;
      if (v.items.size() < f.lo || v.items.size() > f.hi) return Err::SchemaLength;
      for (const Value& el : v.items) {
        if (f.elem == FType::Text) {
          if (el.type != Value::Type::Text) return Err::SchemaType;
          if (el.raw.size() < f.elem_lo || el.raw.size() > f.elem_hi) return Err::SchemaLength;
          if (!CleanText(el.raw, false)) return Err::SchemaText;
        } else {
          const Err e = CheckObject(f.object, el, false);
          if (e != Err::Ok) return e;
        }
      }
      return Err::Ok;
    }
    case FType::ScalarMap: {
      if (v.type != Value::Type::Map) return Err::SchemaType;
      const size_t pairs = v.items.size() / 2;
      if (pairs < f.lo || pairs > f.hi) return Err::SchemaLength;
      for (size_t i = 0; i + 1 < v.items.size(); i += 2) {
        const Value& k = v.items[i];
        const Value& val = v.items[i + 1];
        if (k.raw.empty() || k.raw.size() > kMaxScalarKeyBytes) return Err::SchemaLength;
        if (!CleanText(k.raw, false)) return Err::SchemaText;
        if (val.type == Value::Type::Uint || val.type == Value::Type::Bool) continue;
        if (val.type == Value::Type::Text && val.raw.size() <= kMaxScalarTextBytes) {
          if (!CleanText(val.raw, false)) return Err::SchemaText;
          continue;
        }
        return Err::SchemaType;
      }
      return Err::Ok;
    }
  }
  return Err::SchemaType;
}

Err CheckObject(std::span<const FieldSpec> fields, const Value& obj, bool skip_header) {
  if (obj.type != Value::Type::Map) return Err::SchemaType;
  if (fields.size() > 32) return Err::SchemaType;  // presence mask is 32 bits
  uint32_t seen = 0;
  for (size_t i = 0; i + 1 < obj.items.size(); i += 2) {
    const SV key = obj.items[i].text();
    if (skip_header && (key == "v" || key == "t" || key == "id")) continue;
    size_t idx = fields.size();
    for (size_t j = 0; j < fields.size(); ++j) {
      if (fields[j].name == key) {
        idx = j;
        break;
      }
    }
    if (idx == fields.size()) return Err::SchemaUnknownField;
    const Err e = CheckValue(fields[idx], obj.items[i + 1]);
    if (e != Err::Ok) return e;
    seen |= 1u << idx;
  }
  for (size_t j = 0; j < fields.size(); ++j) {
    if (fields[j].required && (seen & (1u << j)) == 0) return Err::SchemaMissingField;
  }
  return Err::Ok;
}

}  // namespace

std::span<const MessageSpec> Messages() { return std::span<const MessageSpec>(kMessages); }

const MessageSpec* FindMessage(std::string_view kind) {
  for (const MessageSpec& m : kMessages) {
    if (m.kind == kind) return &m;
  }
  return nullptr;
}

Err ValidateMessage(const Value& root, Dir expected_dir, ValidatedMessage* out) {
  if (root.type != Value::Type::Map) return Err::SchemaNotMap;
  const Value* v = cbor::MapGet(root, "v");
  const Value* t = cbor::MapGet(root, "t");
  const Value* id = cbor::MapGet(root, "id");
  if (v == nullptr || t == nullptr || id == nullptr) return Err::SchemaHeader;
  if (v->type != Value::Type::Uint || v->u != kProtocolVersion) return Err::SchemaVersion;
  if (t->type != Value::Type::Text) return Err::SchemaHeader;
  if (id->type != Value::Type::Uint || id->u > 0xFFFFFFFFull) return Err::SchemaId;
  const MessageSpec* spec = FindMessage(t->text());
  if (spec == nullptr) return Err::SchemaUnknownKind;
  if (spec->dir != expected_dir) return Err::SchemaDirection;
  switch (spec->id_rule) {
    case IdRule::Request:
    case IdRule::Response:
      if (id->u == 0) return Err::SchemaId;
      break;
    case IdRule::Event:
      if (id->u != 0) return Err::SchemaId;
      break;
    case IdRule::Any:
      break;
  }
  const Err e = CheckObject(spec->fields, root, true);
  if (e != Err::Ok) return e;
  out->spec = spec;
  out->id = static_cast<uint32_t>(id->u);
  out->root = &root;
  return Err::Ok;
}

const Value* Field(const ValidatedMessage& m, std::string_view name) {
  return m.root == nullptr ? nullptr : cbor::MapGet(*m.root, name);
}

uint64_t FieldUint(const ValidatedMessage& m, std::string_view name, uint64_t fallback) {
  const Value* v = Field(m, name);
  return (v != nullptr && v->type == Value::Type::Uint) ? v->u : fallback;
}

std::string_view FieldText(const ValidatedMessage& m, std::string_view name) {
  const Value* v = Field(m, name);
  return (v != nullptr && v->type == Value::Type::Text) ? v->text() : std::string_view();
}

}  // namespace qmedia::ipc
