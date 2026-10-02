#include "engine.h"

#include <array>
#include <cstring>
#include <optional>
#include <string>
#include <utility>

#include "qmedia/ipc/message.h"
#include "qmedia/ipc/secure.h"
#include "rtc_base/buffer.h"
#include "rtc_base/qaudion_tuning.h"
#include "rtc_base/rtc_certificate_generator.h"
#include "rtc_base/ssl_identity.h"

namespace qmedia::engine {

namespace {

using ipc::cbor::Buf;
using ipc::cbor::Value;
namespace ec = ipc::errcode;

void Reply(ipc::Outbox& out, uint32_t id, const Status& s) {
  if (s.ok()) {
    out.Push(ipc::BuildOk(id));
  } else {
    out.Push(ipc::BuildErr(id, s.code, s.detail != nullptr ? s.detail : ""));
  }
}

void Fail(ipc::Outbox& out, uint32_t id, const char* code, const char* detail) {
  out.Push(ipc::BuildErr(id, code, detail));
}

std::string Text(const ipc::ValidatedMessage& m, const char* name) {
  return std::string(ipc::FieldText(m, name));
}

bool Fingerprint(const webrtc::scoped_refptr<webrtc::RTCCertificate>& cert, std::array<uint8_t, 32>* out) {
  webrtc::Buffer digest;
  if (!cert->GetSSLCertificate().ComputeDigest("sha-256", digest)) return false;
  if (digest.size() != out->size()) return false;
  std::memcpy(out->data(), digest.data(), out->size());
  return true;
}

std::vector<IceServerSpec> ParseIceServers(const Value* arr) {
  std::vector<IceServerSpec> out;
  if (arr == nullptr) return out;
  for (const Value& item : arr->items) {
    IceServerSpec s;
    if (const Value* urls = ipc::cbor::MapGet(item, "urls")) {
      for (const Value& u : urls->items) s.urls.emplace_back(u.text());
    }
    if (const Value* u = ipc::cbor::MapGet(item, "username")) s.username = std::string(u->text());
    if (const Value* c = ipc::cbor::MapGet(item, "credential")) s.credential = std::string(c->text());
    out.push_back(std::move(s));
  }
  return out;
}

}  // namespace

Engine::Engine(ipc::ByteStream& stream, Options opts) : stream_(stream), opts_(std::move(opts)) {}

Engine::~Engine() { Shutdown(); }

void Engine::Emit(Buf payload) {
  std::lock_guard<std::mutex> l(emit_mu_);
  if (closing_ || broken_) return;
  if (ipc::WriteFrame(stream_, payload, 5000) != ipc::Err::Ok) broken_ = true;
}

bool Engine::EnsureRuntime() {
  if (rt_) return true;
  rt_ = Runtime::Create(opts_);
  if (!rt_) return false;
  if (!opts_.fake_audio) {
    watcher_ = DeviceWatcher::Start([this] { Emit(ipc::BuildDevicesChanged()); });
  }
  return true;
}

Engine::Session* Engine::FindSession(uint32_t id) {
  const auto it = sessions_.find(id);
  return it == sessions_.end() ? nullptr : &it->second;
}

Peer* Engine::FindPeer(uint32_t id) {
  const auto it = peers_.find(id);
  if (it == peers_.end() || it->second->closed()) return nullptr;
  return it->second.get();
}

void Engine::CloseSession(uint32_t id) {
  Session* s = FindSession(id);
  if (s == nullptr) return;
  for (uint32_t pc : s->pcs) {
    const auto it = peers_.find(pc);
    if (it != peers_.end()) it->second->Close();
  }
  if (s->keys) s->keys->WipeAll();
  sessions_.erase(id);
}

void Engine::Shutdown() {
  {
    std::lock_guard<std::mutex> l(emit_mu_);
    if (closing_) return;
    closing_ = true;
  }
  watcher_.reset();
  std::vector<uint32_t> ids;
  for (const auto& kv : sessions_) ids.push_back(kv.first);
  for (uint32_t id : ids) CloseSession(id);
  peers_.clear();
}

ipc::HandlerAction Engine::Handle(const ipc::ValidatedMessage& m, ipc::Outbox& out) {
  const std::string_view k = m.spec->kind;
  const uint32_t id = m.id;

  if (k == "ping") {
    out.Push(ipc::BuildPong(id));
    return ipc::HandlerAction::Continue;
  }
  if (k == "shutdown") {
    Shutdown();
    out.Push(ipc::BuildOk(id));
    return ipc::HandlerAction::Shutdown;
  }

  // ---- Sessions and certificates -------------------------------------------------------------
  if (k == "session_create") {
    if (sessions_.size() >= kMaxSessions) {
      Fail(out, id, ec::kLimit.data(), "sessions");
    } else if (!EnsureRuntime()) {
      Fail(out, id, ec::kInternal.data(), "runtime_init_failed");
    } else {
      Session s;
      s.id = next_handle_++;
      s.keys = std::make_unique<SessionKeys>();
      const uint32_t sid = s.id;
      sessions_.emplace(sid, std::move(s));
      out.Push(ipc::BuildSessionCreated(id, sid));
    }
    return ipc::HandlerAction::Continue;
  }
  if (k == "session_close") {
    const uint32_t sid = static_cast<uint32_t>(ipc::FieldUint(m, "session"));
    if (FindSession(sid) == nullptr) {
      Fail(out, id, ec::kNotFound.data(), "session");
    } else {
      CloseSession(sid);
      out.Push(ipc::BuildOk(id));
    }
    return ipc::HandlerAction::Continue;
  }
  if (k == "cert_create") {
    Session* s = FindSession(static_cast<uint32_t>(ipc::FieldUint(m, "session")));
    if (s == nullptr) {
      Fail(out, id, ec::kNotFound.data(), "session");
    } else if (s->certs.size() >= kMaxCertsPerSession) {
      Fail(out, id, ec::kLimit.data(), "certs");
    } else {
      // A fresh ECDSA P-256 certificate. It lives as long as the session: it is never regenerated
      // and never evicted while a peer connection uses it (a rebuilt peer connection of the same
      // call presents the same certificate).
      auto cert = webrtc::RTCCertificateGenerator::GenerateCertificate(webrtc::KeyParams::ECDSA(),
                                                                       std::nullopt);
      std::array<uint8_t, 32> fp{};
      if (!cert || !Fingerprint(cert, &fp)) {
        Fail(out, id, ec::kInternal.data(), "cert_generation_failed");
      } else {
        CertEntry e;
        e.id = next_handle_++;
        e.cert = cert;
        s->certs.push_back(e);
        out.Push(ipc::BuildCertCreated(id, e.id, fp));
      }
    }
    return ipc::HandlerAction::Continue;
  }

  // ---- Peer connections ----------------------------------------------------------------------
  if (k == "pc_create") {
    Session* s = FindSession(static_cast<uint32_t>(ipc::FieldUint(m, "session")));
    if (s == nullptr) {
      Fail(out, id, ec::kNotFound.data(), "session");
      return ipc::HandlerAction::Continue;
    }
    const uint32_t cert_id = static_cast<uint32_t>(ipc::FieldUint(m, "cert"));
    const CertEntry* cert = nullptr;
    for (const CertEntry& c : s->certs) {
      if (c.id == cert_id) cert = &c;
    }
    if (cert == nullptr) {
      Fail(out, id, ec::kNotFound.data(), "cert");
      return ipc::HandlerAction::Continue;
    }
    if (s->pcs.size() >= kMaxPcsPerSession) {
      Fail(out, id, ec::kLimit.data(), "pcs");
      return ipc::HandlerAction::Continue;
    }
    PeerParams params;
    params.relay_only = ipc::FieldText(m, "ice_policy") == "relay";
    params.ice_servers = ParseIceServers(ipc::Field(m, "ice_servers"));
    const uint32_t pid = next_handle_++;
    auto peer = std::make_unique<Peer>(pid, s->id, *rt_, *this);
    const Status st = peer->Init(params, cert->cert);
    if (!st.ok()) {
      Reply(out, id, st);
      return ipc::HandlerAction::Continue;
    }
    peers_.emplace(pid, std::move(peer));
    s->pcs.push_back(pid);
    out.Push(ipc::BuildPcCreated(id, pid));
    return ipc::HandlerAction::Continue;
  }

  // Every remaining command that names a peer connection.
  const bool pc_command = k == "pc_close" || k == "create_offer" || k == "create_answer" ||
                          k == "set_local_description" || k == "set_remote_description" ||
                          k == "add_ice_candidate" || k == "restart_ice" ||
                          k == "update_ice_servers" || k == "bind_media" || k == "set_muted" ||
                          k == "start_video" || k == "stop_video" || k == "request_keyframe" ||
                          k == "set_simulcast" || k == "get_stats" || k == "set_audio_tuning";
  if (pc_command) {
    Peer* p = FindPeer(static_cast<uint32_t>(ipc::FieldUint(m, "pc")));
    if (p == nullptr) {
      Fail(out, id, ec::kNotFound.data(), "pc");
      return ipc::HandlerAction::Continue;
    }
    if (k == "pc_close") {
      p->Close();
      Session* s = FindSession(p->session());
      if (s != nullptr) {
        for (size_t i = 0; i < s->pcs.size(); ++i) {
          if (s->pcs[i] == p->id()) {
            s->pcs.erase(s->pcs.begin() + static_cast<std::ptrdiff_t>(i));
            break;
          }
        }
      }
      out.Push(ipc::BuildOk(id));
    } else if (k == "create_offer") {
      std::string sdp;
      const Value* restart = ipc::Field(m, "ice_restart");
      const Status st = p->CreateOffer(restart != nullptr && restart->u != 0, &sdp);
      if (!st.ok() || sdp.empty() || sdp.size() > 262144) {
        Reply(out, id, st.ok() ? Status::Fail("internal", "sdp_size") : st);
      } else {
        out.Push(ipc::BuildSdpReady(id, p->id(), "offer", sdp));
      }
    } else if (k == "create_answer") {
      std::string sdp;
      const Status st = p->CreateAnswer(&sdp);
      if (!st.ok() || sdp.empty() || sdp.size() > 262144) {
        Reply(out, id, st.ok() ? Status::Fail("internal", "sdp_size") : st);
      } else {
        out.Push(ipc::BuildSdpReady(id, p->id(), "answer", sdp));
      }
    } else if (k == "set_local_description" || k == "set_remote_description") {
      Reply(out, id, p->SetDescription(k == "set_local_description", Text(m, "type"), Text(m, "sdp")));
    } else if (k == "add_ice_candidate") {
      const Value* idx = ipc::Field(m, "mline_index");
      Reply(out, id,
            p->AddIceCandidate(Text(m, "candidate"), Text(m, "mid"),
                               idx != nullptr ? static_cast<int>(idx->u) : -1));
    } else if (k == "restart_ice") {
      Reply(out, id, p->RestartIce());
    } else if (k == "update_ice_servers") {
      Reply(out, id, p->UpdateIceServers(ParseIceServers(ipc::Field(m, "ice_servers"))));
    } else if (k == "bind_media") {
      Session* s = FindSession(p->session());
      if (s == nullptr) {
        Fail(out, id, ec::kNotFound.data(), "session");
      } else {
        Reply(out, id,
              p->BindMedia(*s->keys, Text(m, "mid"), Text(m, "participant"),
                           ipc::FieldText(m, "direction") == "send"));
      }
    } else if (k == "set_muted") {
      const Value* mv = ipc::Field(m, "muted");
      Reply(out, id, p->SetMuted(Text(m, "track"), mv != nullptr && mv->u != 0));
    } else if (k == "get_stats") {
      std::vector<ipc::StatEntry> entries;
      const Status st = p->GetStats(ParseStatsScope(ipc::FieldText(m, "scope")), &entries);
      if (!st.ok()) {
        Reply(out, id, st);
      } else {
        out.Push(ipc::BuildStats(id, p->id(), entries));
      }
    } else if (k == "set_audio_tuning") {
      // ptime and CBR are negotiated in the SDP (a=ptime, minptime, cbr in the fmtp line) and the
      // host owns that SDP; they cannot be changed on a live encoder. Nothing is applied when one
      // of them is present, so a request is never half done.
      if (ipc::Field(m, "ptime_ms") != nullptr || ipc::Field(m, "cbr") != nullptr) {
        Fail(out, id, ec::kUnsupported.data(), "sdp_level_parameter");
      } else {
        Status st = Status::Ok();
        if (ipc::Field(m, "bitrate_bps") != nullptr) {
          st = p->SetBitrate(static_cast<int>(ipc::FieldUint(m, "bitrate_bps")));
        }
        if (st.ok() && ipc::Field(m, "fec_floor_pct") != nullptr) {
          webrtc::qaudion::SetOpusMinPacketLossPercent(static_cast<int>(ipc::FieldUint(m, "fec_floor_pct")));
        }
        Reply(out, id, st);
      }
    } else {
      // start_video, stop_video, request_keyframe, set_simulcast
      Fail(out, id, ec::kUnsupported.data(), "video_not_in_this_build");
    }
    return ipc::HandlerAction::Continue;
  }

  // ---- Frame keys (session level) ------------------------------------------------------------
  if (k == "install_key" || k == "retire_slot" || k == "select_send_slot") {
    Session* s = FindSession(static_cast<uint32_t>(ipc::FieldUint(m, "session")));
    if (s == nullptr) {
      Fail(out, id, ec::kNotFound.data(), "session");
      return ipc::HandlerAction::Continue;
    }
    const std::string participant = Text(m, "participant");
    const int slot = static_cast<int>(ipc::FieldUint(m, "slot"));
    if (k == "install_key") {
      const Value* key = ipc::Field(m, "key");
      const bool send = ipc::FieldText(m, "direction") == "send";
      if (key == nullptr || !s->keys->Install(participant, slot, key->raw, send)) {
        Fail(out, id, ec::kInvalidKey.data(), "install_failed");
      } else {
        out.Push(ipc::BuildOk(id));
      }
    } else if (k == "retire_slot") {
      if (!s->keys->Retire(participant, slot, ipc::FieldText(m, "direction") == "send")) {
        Fail(out, id, ec::kInternal.data(), "retire_failed");
      } else {
        out.Push(ipc::BuildOk(id));
      }
    } else {
      s->keys->SelectSendSlot(participant, slot);
      out.Push(ipc::BuildOk(id));
    }
    return ipc::HandlerAction::Continue;
  }

  // ---- Devices -------------------------------------------------------------------------------
  if (k == "list_devices") {
    if (!EnsureRuntime()) {
      Fail(out, id, ec::kInternal.data(), "runtime_init_failed");
      return ipc::HandlerAction::Continue;
    }
    std::vector<ipc::DeviceInfo> devices;
    for (const bool input : {true, false}) {
      for (const AudioDeviceEntry& e : rt_->ListAudioDevices(input)) {
        if (devices.size() >= 64) break;
        ipc::DeviceInfo d;
        d.id = e.id;
        d.name = e.name;
        d.kind = input ? "audio_in" : "audio_out";
        d.is_default = e.is_default;
        d.has_is_communications = true;
        d.is_communications = e.is_communications;
        devices.push_back(std::move(d));
      }
    }
    out.Push(ipc::BuildDevices(id, devices));
    return ipc::HandlerAction::Continue;
  }
  if (k == "select_device") {
    const std::string_view kind = ipc::FieldText(m, "kind");
    if (kind == "video_in") {
      Fail(out, id, ec::kUnsupported.data(), "video_not_in_this_build");
    } else if (!EnsureRuntime()) {
      Fail(out, id, ec::kInternal.data(), "runtime_init_failed");
    } else {
      Reply(out, id, rt_->SelectAudioDevice(kind == "audio_in", Text(m, "device")));
    }
    return ipc::HandlerAction::Continue;
  }

  // pcm_tap and anything else
  Fail(out, id, ec::kUnsupported.data(), "not_in_this_build");
  return ipc::HandlerAction::Continue;
}

}  // namespace qmedia::engine
