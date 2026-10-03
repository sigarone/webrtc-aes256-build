#include "qmedia/ipc/message.h"

#include "qmedia/ipc/limits.h"

namespace qmedia::ipc {

cbor::MapBuilder BeginMessage(std::string_view kind, uint32_t id) {
  cbor::MapBuilder b;
  b.Uint("v", kProtocolVersion).Str("t", kind).Uint("id", id);
  return b;
}

cbor::Buf BuildHello(uint32_t id, std::span<const uint8_t> nonce) {
  return BeginMessage("hello", id).Bin("nonce", nonce).Finish();
}

cbor::Buf BuildHelloOk(uint32_t id) {
  return BeginMessage("hello_ok", id)
      .Str("engine", kEngineIdent)
      .Uint("max_frame", kMaxFramePayload)
      .Finish();
}

cbor::Buf BuildPong(uint32_t id) { return BeginMessage("pong", id).Finish(); }

cbor::Buf BuildOk(uint32_t id) { return BeginMessage("ok", id).Finish(); }

cbor::Buf BuildErr(uint32_t id, std::string_view code, std::string_view detail) {
  cbor::MapBuilder b = BeginMessage("err", id);
  b.Str("code", code);
  if (!detail.empty()) b.Str("detail", detail);
  return b.Finish();
}

cbor::Buf BuildSessionCreated(uint32_t id, uint32_t session) {
  return BeginMessage("session_created", id).Uint("session", session).Finish();
}

cbor::Buf BuildCertCreated(uint32_t id, uint32_t cert, std::span<const uint8_t> fingerprint) {
  return BeginMessage("cert_created", id).Uint("cert", cert).Bin("fingerprint", fingerprint).Finish();
}

cbor::Buf BuildPcCreated(uint32_t id, uint32_t pc) {
  return BeginMessage("pc_created", id).Uint("pc", pc).Finish();
}

cbor::Buf BuildSdpReady(uint32_t id, uint32_t pc, std::string_view type, std::string_view sdp) {
  return BeginMessage("sdp_ready", id).Uint("pc", pc).Str("type", type).Str("sdp", sdp).Finish();
}

cbor::Buf BuildDevices(uint32_t id, std::span<const DeviceInfo> devices) {
  cbor::ArrayBuilder arr;
  for (const DeviceInfo& d : devices) {
    cbor::MapBuilder m;
    m.Str("id", d.id).Str("name", d.name).Str("kind", d.kind).Flag("is_default", d.is_default);
    if (d.has_is_communications) m.Flag("is_communications", d.is_communications);
    arr.Add(m.Finish());
  }
  return BeginMessage("devices", id).Raw("devices", arr.Finish()).Finish();
}

cbor::Buf BuildStats(uint32_t id, uint32_t pc, std::span<const StatEntry> entries) {
  cbor::ArrayBuilder arr;
  for (const StatEntry& e : entries) {
    cbor::MapBuilder values;
    for (const auto& kv : e.values) {
      switch (kv.second.kind) {
        case StatValue::Kind::Uint: values.Uint(kv.first, kv.second.u); break;
        case StatValue::Kind::Bool: values.Flag(kv.first, kv.second.b); break;
        case StatValue::Kind::Text: values.Str(kv.first, kv.second.s); break;
      }
    }
    cbor::MapBuilder m;
    m.Str("type", e.type).Str("id", e.id).Raw("values", values.Finish());
    arr.Add(m.Finish());
  }
  return BeginMessage("stats", id).Uint("pc", pc).Raw("entries", arr.Finish()).Finish();
}

cbor::Buf BuildIceCandidate(uint32_t pc, std::string_view candidate, std::string_view mid,
                            int mline_index) {
  cbor::MapBuilder b = BeginMessage("ice_candidate", 0);
  b.Uint("pc", pc).Str("candidate", candidate);
  if (!mid.empty()) b.Str("mid", mid);
  if (mline_index >= 0) b.Uint("mline_index", static_cast<uint64_t>(mline_index));
  return b.Finish();
}

cbor::Buf BuildIceGatheringState(uint32_t pc, std::string_view state) {
  return BeginMessage("ice_gathering_state", 0).Uint("pc", pc).Str("state", state).Finish();
}

cbor::Buf BuildIceConnectionState(uint32_t pc, std::string_view state) {
  return BeginMessage("ice_connection_state", 0).Uint("pc", pc).Str("state", state).Finish();
}

cbor::Buf BuildPcState(uint32_t pc, std::string_view state) {
  return BeginMessage("pc_state", 0).Uint("pc", pc).Str("state", state).Finish();
}

cbor::Buf BuildNegotiationNeeded(uint32_t pc) {
  return BeginMessage("negotiation_needed", 0).Uint("pc", pc).Finish();
}

cbor::Buf BuildTransportInfo(uint32_t pc, std::string_view tls_version, std::string_view dtls_cipher,
                             std::string_view group, std::string_view srtp_cipher,
                             std::span<const uint8_t> remote_cert_fingerprint,
                             std::span<const uint8_t> local_cert_fingerprint) {
  return BeginMessage("transport_info", 0)
      .Uint("pc", pc)
      .Str("tls_version", tls_version)
      .Str("dtls_cipher", dtls_cipher)
      .Str("group", group)
      .Str("srtp_cipher", srtp_cipher)
      .Bin("remote_cert_fingerprint", remote_cert_fingerprint)
      .Bin("local_cert_fingerprint", local_cert_fingerprint)
      .Finish();
}

cbor::Buf BuildTransportViolation(uint32_t pc, std::string_view reason) {
  return BeginMessage("transport_violation", 0).Uint("pc", pc).Str("reason", reason).Finish();
}

cbor::Buf BuildCryptorState(uint32_t pc, std::string_view mid, std::string_view participant,
                            std::string_view kind, std::string_view state) {
  return BeginMessage("cryptor_state", 0)
      .Uint("pc", pc)
      .Str("mid", mid)
      .Str("participant", participant)
      .Str("kind", kind)
      .Str("state", state)
      .Finish();
}

cbor::Buf BuildDevicesChanged() { return BeginMessage("devices_changed", 0).Finish(); }

}  // namespace qmedia::ipc
