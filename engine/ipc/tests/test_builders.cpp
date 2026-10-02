// The replies and events the engine core builds must be accepted by the schema validator in the
// engine-to-client direction, and must carry the values they were built from.
#include <string>
#include <vector>

#include "qmedia/ipc/cbor.h"
#include "qmedia/ipc/message.h"
#include "qmedia/ipc/schema.h"
#include "testing.h"

using namespace qmedia::ipc;
using cbor::Buf;
using cbor::Value;

namespace {

// Decodes and validates `wire` as an engine message. The decoded value points into `wire`.
bool Accepts(const Buf& wire, Value* root, ValidatedMessage* msg, std::string_view kind) {
  if (cbor::Decode(wire, root) != Err::Ok) return false;
  if (ValidateMessage(*root, Dir::EngineToClient, msg) != Err::Ok) return false;
  return msg->spec != nullptr && msg->spec->kind == kind;
}

Buf Fp(uint8_t seed) {
  Buf b(kFingerprintBytes);
  for (size_t i = 0; i < b.size(); ++i) b[i] = static_cast<uint8_t>(seed + i);
  return b;
}

}  // namespace

QTEST(builders_replies_validate) {
  {
    const Buf w = BuildSessionCreated(5, 11);
    Value v;
    ValidatedMessage m;
    CHECK(Accepts(w, &v, &m, "session_created"));
    CHECK_EQ(m.id, 5u);
    CHECK_EQ(FieldUint(m, "session"), 11u);
  }
  {
    const Buf fp = Fp(3);
    const Buf w = BuildCertCreated(6, 12, fp);
    Value v;
    ValidatedMessage m;
    CHECK(Accepts(w, &v, &m, "cert_created"));
    CHECK_EQ(FieldUint(m, "cert"), 12u);
    const Value* f = Field(m, "fingerprint");
    CHECK(f != nullptr && f->raw.size() == kFingerprintBytes);
    if (f != nullptr) CHECK(Buf(f->raw.begin(), f->raw.end()) == fp);
  }
  {
    const Buf w = BuildPcCreated(7, 13);
    Value v;
    ValidatedMessage m;
    CHECK(Accepts(w, &v, &m, "pc_created"));
    CHECK_EQ(FieldUint(m, "pc"), 13u);
  }
  {
    const Buf w = BuildSdpReady(8, 14, "offer", "v=0\r\ns=-\r\n");
    Value v;
    ValidatedMessage m;
    CHECK(Accepts(w, &v, &m, "sdp_ready"));
    CHECK(FieldText(m, "type") == "offer");
    CHECK(FieldText(m, "sdp") == "v=0\r\ns=-\r\n");
  }
}

QTEST(builders_devices_validate) {
  std::vector<DeviceInfo> devs;
  DeviceInfo a;
  a.id = "dev-a";
  a.name = "Microphone";
  a.kind = "audio_in";
  a.is_default = true;
  a.has_is_communications = true;
  a.is_communications = false;
  devs.push_back(a);
  DeviceInfo b;
  b.id = "";
  b.name = "Speakers";
  b.kind = "audio_out";
  devs.push_back(b);
  const Buf w = BuildDevices(9, devs);
  Value v;
  ValidatedMessage m;
  CHECK(Accepts(w, &v, &m, "devices"));
  const Value* arr = Field(m, "devices");
  CHECK(arr != nullptr && arr->items.size() == 2);

  const Buf empty = BuildDevices(10, std::span<const DeviceInfo>());
  Value v2;
  ValidatedMessage m2;
  CHECK(Accepts(empty, &v2, &m2, "devices"));
}

QTEST(builders_stats_validate_with_every_scalar_kind) {
  std::vector<StatEntry> entries;
  StatEntry e;
  e.type = "inbound-rtp";
  e.id = "r1";
  e.values.push_back({"packetsReceived", StatValue::OfUint(1234)});
  e.values.push_back({"active", StatValue::OfBool(true)});
  e.values.push_back({"jitter", StatValue::OfText("0.004000")});
  entries.push_back(e);
  StatEntry none;
  none.type = "transport";
  none.id = "";
  entries.push_back(none);
  const Buf w = BuildStats(15, 16, entries);
  Value v;
  ValidatedMessage m;
  CHECK(Accepts(w, &v, &m, "stats"));
  CHECK_EQ(FieldUint(m, "pc"), 16u);
  const Value* arr = Field(m, "entries");
  CHECK(arr != nullptr && arr->items.size() == 2);
}

QTEST(builders_events_validate) {
  {
    const Buf w = BuildIceCandidate(1, "candidate:1 1 udp 1 192.0.2.1 9 typ host", "0", 0);
    Value v;
    ValidatedMessage m;
    CHECK(Accepts(w, &v, &m, "ice_candidate"));
    CHECK_EQ(m.id, 0u);
    CHECK(FieldText(m, "mid") == "0");
    CHECK(Field(m, "mline_index") != nullptr);
  }
  {
    // Neither mid nor mline index.
    const Buf w = BuildIceCandidate(1, "", "", -1);
    Value v;
    ValidatedMessage m;
    CHECK(Accepts(w, &v, &m, "ice_candidate"));
    CHECK(Field(m, "mid") == nullptr);
    CHECK(Field(m, "mline_index") == nullptr);
  }
  for (const char* st : {"new", "gathering", "complete"}) {
    const Buf w = BuildIceGatheringState(2, st);
    Value v;
    ValidatedMessage m;
    CHECK(Accepts(w, &v, &m, "ice_gathering_state"));
  }
  for (const char* st : {"new", "checking", "connected", "completed", "failed", "disconnected", "closed"}) {
    const Buf w = BuildIceConnectionState(2, st);
    Value v;
    ValidatedMessage m;
    CHECK(Accepts(w, &v, &m, "ice_connection_state"));
  }
  for (const char* st : {"new", "connecting", "connected", "disconnected", "failed", "closed"}) {
    const Buf w = BuildPcState(2, st);
    Value v;
    ValidatedMessage m;
    CHECK(Accepts(w, &v, &m, "pc_state"));
  }
  {
    const Buf w = BuildNegotiationNeeded(3);
    Value v;
    ValidatedMessage m;
    CHECK(Accepts(w, &v, &m, "negotiation_needed"));
  }
  {
    const Buf fp = Fp(9);
    const Buf w = BuildTransportInfo(4, "DTLS1.3", "TLS_AES_256_GCM_SHA384", "X25519MLKEM768",
                                     "AEAD_AES_256_GCM", fp);
    Value v;
    ValidatedMessage m;
    CHECK(Accepts(w, &v, &m, "transport_info"));
    CHECK(FieldText(m, "group") == "X25519MLKEM768");
    const Value* f = Field(m, "remote_cert_fingerprint");
    CHECK(f != nullptr && f->raw.size() == kFingerprintBytes);
  }
  for (const char* r : {"no_dtls", "tls_version", "dtls_cipher", "group", "srtp_cipher"}) {
    const Buf w = BuildTransportViolation(5, r);
    Value v;
    ValidatedMessage m;
    CHECK(Accepts(w, &v, &m, "transport_violation"));
  }
  for (const char* st : {"ok", "missing_key", "decryption_failed", "encryption_failed", "internal_error"}) {
    const Buf w = BuildCryptorState(6, "0", "remote", "audio", st);
    Value v;
    ValidatedMessage m;
    CHECK(Accepts(w, &v, &m, "cryptor_state"));
  }
  {
    const Buf w = BuildDevicesChanged();
    Value v;
    ValidatedMessage m;
    CHECK(Accepts(w, &v, &m, "devices_changed"));
  }
}

QTEST(builders_are_rejected_in_the_wrong_direction) {
  // An engine reply is not a client request.
  const Buf w = BuildPcCreated(1, 2);
  Value v;
  CHECK_EQ(cbor::Decode(w, &v), Err::Ok);
  ValidatedMessage m;
  CHECK(ValidateMessage(v, Dir::ClientToEngine, &m) != Err::Ok);
}
