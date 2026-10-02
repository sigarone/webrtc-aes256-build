#include "stats.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "api/stats/attribute.h"
#include "api/stats/rtc_stats.h"
#include "runtime.h"

namespace qmedia::engine {

namespace {

constexpr const char* kTypes[] = {"inbound-rtp", "outbound-rtp",    "remote-inbound-rtp",
                                  "remote-outbound-rtp", "media-source", "transport",
                                  "candidate-pair",  "codec"};

// Attribute names that may leave the engine. Identifiers, addresses, certificates, fmtp lines and
// every reference to another entry are deliberately absent.
constexpr const char* kAttributes[] = {
    "kind", "mimeType", "clockRate", "channels", "payloadType",
    "packetsReceived", "packetsSent", "bytesReceived", "bytesSent", "headerBytesReceived",
    "packetsLost", "packetsDiscarded", "packetsRetransmitted", "retransmittedBytesSent",
    "fecPacketsReceived", "fecPacketsDiscarded", "nackCount", "pliCount", "firCount",
    "jitter", "jitterBufferDelay", "jitterBufferTargetDelay", "jitterBufferEmittedCount",
    "roundTripTime", "totalRoundTripTime", "currentRoundTripTime", "fractionLost",
    "audioLevel", "totalAudioEnergy", "totalSamplesDuration", "totalSamplesReceived",
    "concealedSamples", "silentConcealedSamples", "concealmentEvents",
    "insertedSamplesForDeceleration", "removedSamplesForAcceleration",
    "echoReturnLoss", "echoReturnLossEnhancement",
    "framesDecoded", "framesEncoded", "framesPerSecond", "frameWidth", "frameHeight",
    "keyFramesDecoded", "freezeCount",
    "dtlsState", "iceState", "dtlsCipher", "srtpCipher", "tlsVersion", "selectedCandidatePairChanges",
    "state", "nominated", "writable", "availableOutgoingBitrate", "availableIncomingBitrate",
};

bool InList(const char* name, const char* const* list, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (std::strcmp(name, list[i]) == 0) return true;
  }
  return false;
}

std::string DecimalText(double v) {
  char buf[40];
  std::snprintf(buf, sizeof buf, "%.9g", v);
  return std::string(buf);
}

bool Convert(const webrtc::Attribute& a, ipc::StatValue* out) {
  if (!a.has_value()) return false;
  if (a.holds_alternative<bool>()) {
    *out = ipc::StatValue::OfBool(a.get<bool>());
  } else if (a.holds_alternative<uint32_t>()) {
    *out = ipc::StatValue::OfUint(a.get<uint32_t>());
  } else if (a.holds_alternative<uint64_t>()) {
    *out = ipc::StatValue::OfUint(a.get<uint64_t>());
  } else if (a.holds_alternative<int32_t>()) {
    const int32_t v = a.get<int32_t>();
    *out = v >= 0 ? ipc::StatValue::OfUint(static_cast<uint64_t>(v))
                  : ipc::StatValue::OfText(std::to_string(v));
  } else if (a.holds_alternative<int64_t>()) {
    const int64_t v = a.get<int64_t>();
    *out = v >= 0 ? ipc::StatValue::OfUint(static_cast<uint64_t>(v))
                  : ipc::StatValue::OfText(std::to_string(v));
  } else if (a.holds_alternative<double>()) {
    *out = ipc::StatValue::OfText(DecimalText(a.get<double>()));
  } else if (a.holds_alternative<std::string>()) {
    *out = ipc::StatValue::OfText(SanitizeText(a.get<std::string>(), 128));
    if (out->s.empty()) return false;  // the schema wants a non-empty key, not necessarily a value; skip empties
  } else {
    return false;  // sequences and maps are not part of the reduced report
  }
  return true;
}

const std::string* Find(const ipc::StatEntry& e, const char* name) {
  for (const auto& kv : e.values) {
    if (kv.first == name && kv.second.kind == ipc::StatValue::Kind::Text) return &kv.second.s;
  }
  return nullptr;
}

bool InScope(const ipc::StatEntry& e, StatsScope scope) {
  if (scope == StatsScope::All) return true;
  if (scope == StatsScope::Transport) return e.type == "transport" || e.type == "candidate-pair";
  const char* want_kind = scope == StatsScope::Audio ? "audio" : "video";
  const std::string* kind = Find(e, "kind");
  if (kind != nullptr) return *kind == want_kind;
  if (e.type == "codec") {
    const std::string* mime = Find(e, "mimeType");
    return mime != nullptr && mime->rfind(std::string(want_kind) + "/", 0) == 0;
  }
  return false;
}

}  // namespace

StatsScope ParseStatsScope(std::string_view s) {
  if (s == "transport") return StatsScope::Transport;
  if (s == "audio") return StatsScope::Audio;
  if (s == "video") return StatsScope::Video;
  return StatsScope::All;
}

std::vector<ipc::StatEntry> ReduceStats(const webrtc::RTCStatsReport& report, StatsScope scope) {
  std::vector<ipc::StatEntry> out;
  for (const webrtc::RTCStats& s : report) {
    if (out.size() >= 64) break;
    if (!InList(s.type(), kTypes, sizeof kTypes / sizeof kTypes[0])) continue;
    ipc::StatEntry e;
    e.type = s.type();
    for (const webrtc::Attribute& a : s.Attributes()) {
      if (e.values.size() >= 64) break;
      if (!InList(a.name(), kAttributes, sizeof kAttributes / sizeof kAttributes[0])) continue;
      ipc::StatValue v;
      if (!Convert(a, &v)) continue;
      e.values.emplace_back(a.name(), std::move(v));
    }
    if (!InScope(e, scope)) continue;
    e.id = "n" + std::to_string(out.size());
    out.push_back(std::move(e));
  }
  return out;
}

}  // namespace qmedia::engine
