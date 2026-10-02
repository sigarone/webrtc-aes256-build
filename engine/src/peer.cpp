#include "peer.h"

#include <chrono>
#include <condition_variable>
#include <cstring>
#include <optional>
#include <utility>

#include "api/dtls_transport_interface.h"
#include "api/make_ref_counted.h"
#include "api/priority.h"
#include "api/rtp_parameters.h"
#include "api/rtp_receiver_interface.h"
#include "api/rtp_sender_interface.h"
#include "api/rtp_transceiver_interface.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "rtc_base/buffer.h"
#include "rtc_base/qaudion_tuning.h"
#include "rtc_base/ssl_certificate.h"

namespace qmedia::engine {

namespace {

using std::chrono::seconds;

// The values the strict libwebrtc build negotiates and nothing else.
constexpr int kDtls13 = 0xfefc;
constexpr int kTlsAes256GcmSha384 = 0x1302;
constexpr int kSrtpAeadAes256Gcm = 0x0008;
constexpr int kX25519Mlkem768 = 0x11ec;

class Waiter {
 public:
  void Set() {
    std::lock_guard<std::mutex> l(m_);
    set_ = true;
    cv_.notify_all();
  }
  bool WaitFor(seconds s) {
    std::unique_lock<std::mutex> l(m_);
    return cv_.wait_for(l, s, [this] { return set_; });
  }

 private:
  std::mutex m_;
  std::condition_variable cv_;
  bool set_ = false;
};

class CreateObs : public webrtc::CreateSessionDescriptionObserver {
 public:
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    desc_.reset(desc);
    done_.Set();
  }
  void OnFailure(webrtc::RTCError) override {
    failed_ = true;
    done_.Set();
  }
  bool Wait() { return done_.WaitFor(seconds(20)) && !failed_ && desc_; }
  std::unique_ptr<webrtc::SessionDescriptionInterface> Take() { return std::move(desc_); }

 private:
  Waiter done_;
  bool failed_ = false;
  std::unique_ptr<webrtc::SessionDescriptionInterface> desc_;
};

class SetLocalObs : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    ok_ = error.ok();
    done_.Set();
  }
  bool Wait() { return done_.WaitFor(seconds(20)) && ok_; }

 private:
  Waiter done_;
  bool ok_ = false;
};

class SetRemoteObs : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    ok_ = error.ok();
    done_.Set();
  }
  bool Wait() { return done_.WaitFor(seconds(20)) && ok_; }

 private:
  Waiter done_;
  bool ok_ = false;
};

class StatsObs : public webrtc::RTCStatsCollectorCallback {
 public:
  void OnStatsDelivered(const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report) override {
    report_ = report;
    done_.Set();
  }
  bool Wait() { return done_.WaitFor(seconds(10)) && report_; }
  const webrtc::RTCStatsReport& report() const { return *report_; }

 private:
  Waiter done_;
  webrtc::scoped_refptr<const webrtc::RTCStatsReport> report_;
};

// Forwards the state of one cryptor to its peer. The peer detaches it before it goes away.
class CryptorObs : public webrtc::FrameCryptorTransformerObserver {
 public:
  CryptorObs(Peer* peer, std::string mid, bool audio)
      : peer_(peer), mid_(std::move(mid)), audio_(audio) {}
  void Detach() {
    std::lock_guard<std::mutex> l(m_);
    peer_ = nullptr;
  }
  void OnFrameCryptionStateChanged(const std::string participant_id,
                                   webrtc::FrameCryptionState state) override {
    Peer* p;
    {
      std::lock_guard<std::mutex> l(m_);
      p = peer_;
      if (p == nullptr) return;
      // Calls into the peer happen under the lock so Detach() cannot return while one is running.
      // The provider id carries a two character direction prefix ("s:" / "r:"); strip it.
      const std::string participant =
          participant_id.size() > 2 ? participant_id.substr(2) : participant_id;
      p->OnCryptorState(mid_, participant, audio_, state);
    }
  }

 private:
  std::mutex m_;
  Peer* peer_;
  const std::string mid_;
  const bool audio_;
};

const char* PcStateName(webrtc::PeerConnectionInterface::PeerConnectionState s) {
  using S = webrtc::PeerConnectionInterface::PeerConnectionState;
  switch (s) {
    case S::kNew: return "new";
    case S::kConnecting: return "connecting";
    case S::kConnected: return "connected";
    case S::kDisconnected: return "disconnected";
    case S::kFailed: return "failed";
    case S::kClosed: return "closed";
  }
  return "failed";
}

const char* IceStateName(webrtc::PeerConnectionInterface::IceConnectionState s) {
  using S = webrtc::PeerConnectionInterface::IceConnectionState;
  switch (s) {
    case S::kIceConnectionNew: return "new";
    case S::kIceConnectionChecking: return "checking";
    case S::kIceConnectionConnected: return "connected";
    case S::kIceConnectionCompleted: return "completed";
    case S::kIceConnectionFailed: return "failed";
    case S::kIceConnectionDisconnected: return "disconnected";
    case S::kIceConnectionClosed: return "closed";
    case S::kIceConnectionMax: return "failed";
  }
  return "failed";
}

webrtc::PeerConnectionInterface::IceServers ToIceServers(const std::vector<IceServerSpec>& in) {
  webrtc::PeerConnectionInterface::IceServers out;
  for (const IceServerSpec& s : in) {
    webrtc::PeerConnectionInterface::IceServer is;
    is.urls = s.urls;
    is.username = s.username;
    is.password = s.credential;
    out.push_back(std::move(is));
  }
  return out;
}

}  // namespace

Peer::Peer(uint32_t id, uint32_t session, Runtime& rt, Emitter& out)
    : id_(id), session_(session), rt_(rt), out_(out) {}

Peer::~Peer() { Close(); }

bool Peer::closed() const {
  std::lock_guard<std::mutex> l(mu_);
  return closed_;
}

webrtc::scoped_refptr<webrtc::PeerConnectionInterface> Peer::pc() const {
  std::lock_guard<std::mutex> l(mu_);
  return pc_;
}

void Peer::Emit(ipc::cbor::Buf payload) {
  {
    std::lock_guard<std::mutex> l(mu_);
    if (closed_) return;
  }
  out_.Emit(std::move(payload));
}

Status Peer::Init(const PeerParams& params, webrtc::scoped_refptr<webrtc::RTCCertificate> cert) {
  using PCI = webrtc::PeerConnectionInterface;
  PCI::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;
  config.servers = ToIceServers(params.ice_servers);
  config.type = params.relay_only ? PCI::kRelay : PCI::kAll;
  config.bundle_policy = PCI::kBundlePolicyMaxBundle;
  config.rtcp_mux_policy = PCI::kRtcpMuxPolicyRequire;
  config.certificates.push_back(std::move(cert));
  // The same media settings the mobile apps use for a native audio call
  // (PeerConnectionHolder.kt, RTCConfiguration block, main 4f6b3983 lines 4218-4332).
  config.crypto_options.srtp.enable_gcm_crypto_suites = true;
  config.crypto_options.srtp.enable_aes128_sha1_32_crypto_cipher = false;
  config.crypto_options.srtp.enable_encrypted_rtp_header_extensions = true;
  config.crypto_options.sframe.require_frame_encryption = false;
  config.continual_gathering_policy = PCI::GATHER_CONTINUALLY;
  config.tcp_candidate_policy = PCI::kTcpCandidatePolicyDisabled;
  config.audio_jitter_buffer_max_packets = 17;
  config.audio_jitter_buffer_fast_accelerate = false;
  config.turn_port_prune_policy = webrtc::KEEP_FIRST_READY;

  auto result = rt_.factory()->CreatePeerConnectionOrError(
      config, webrtc::PeerConnectionDependencies(this));
  if (!result.ok()) return Status::Fail("internal", "pc_create_failed");
  std::lock_guard<std::mutex> l(mu_);
  pc_ = result.MoveValue();
  return Status::Ok();
}

void Peer::Close() {
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
  std::vector<Bound> bound;
  {
    std::lock_guard<std::mutex> l(mu_);
    if (closed_) return;
    closed_ = true;
    pc = std::move(pc_);
    bound = std::move(bound_);
  }
  for (Bound& b : bound) {
    auto* obs = static_cast<CryptorObs*>(b.observer.get());
    if (obs != nullptr) obs->Detach();
    if (b.transformer) b.transformer->UnRegisterFrameCryptorTransformerObserver();
    if (keys_ != nullptr && b.send) keys_->UnregisterSender(b.transformer.get());
  }
  if (pc) pc->Close();
}

Status Peer::EnsureLocalAudio() {
  auto p = pc();
  if (!p) return Status::Fail("invalid_state", "pc_closed");
  {
    std::lock_guard<std::mutex> l(mu_);
    if (local_added_) return Status::Ok();
  }
  webrtc::AudioOptions options;
  options.echo_cancellation = true;
  options.noise_suppression = true;
  options.auto_gain_control = true;
  options.highpass_filter = true;
  auto source = rt_.factory()->CreateAudioSource(options);
  auto track = rt_.factory()->CreateAudioTrack("qm-mic", source.get());
  if (!track) return Status::Fail("internal", "audio_track_failed");
  bool muted;
  {
    std::lock_guard<std::mutex> l(mu_);
    muted = mic_muted_;
  }
  track->set_enabled(!muted);
  // AddTrack reuses a receive-only audio transceiver that a remote offer created, and creates a
  // send-receive one otherwise, so the same call serves offerer and answerer.
  auto added = p->AddTrack(track, {});
  if (!added.ok()) return Status::Fail("internal", "add_track_failed");
  std::lock_guard<std::mutex> l(mu_);
  track_ = track;
  local_added_ = true;
  return Status::Ok();
}

Status Peer::CreateOffer(bool ice_restart, std::string* sdp) {
  auto p = pc();
  if (!p) return Status::Fail("invalid_state", "pc_closed");
  Status s = EnsureLocalAudio();
  if (!s.ok()) return s;
  webrtc::PeerConnectionInterface::RTCOfferAnswerOptions opts;
  opts.ice_restart = ice_restart;
  auto obs = webrtc::make_ref_counted<CreateObs>();
  p->CreateOffer(obs.get(), opts);
  if (!obs->Wait()) return Status::Fail("internal", "create_offer_failed");
  auto desc = obs->Take();
  if (!desc->ToString(sdp)) return Status::Fail("internal", "sdp_serialize_failed");
  return Status::Ok();
}

Status Peer::CreateAnswer(std::string* sdp) {
  auto p = pc();
  if (!p) return Status::Fail("invalid_state", "pc_closed");
  auto obs = webrtc::make_ref_counted<CreateObs>();
  p->CreateAnswer(obs.get(), webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  if (!obs->Wait()) return Status::Fail("invalid_state", "create_answer_failed");
  auto desc = obs->Take();
  if (!desc->ToString(sdp)) return Status::Fail("internal", "sdp_serialize_failed");
  return Status::Ok();
}

Status Peer::SetDescription(bool local, const std::string& type, const std::string& sdp) {
  auto p = pc();
  if (!p) return Status::Fail("invalid_state", "pc_closed");
  std::unique_ptr<webrtc::SessionDescriptionInterface> desc;
  webrtc::SdpType t = webrtc::SdpType::kOffer;
  if (type == "rollback") {
    desc = webrtc::CreateRollbackSessionDescription();
  } else {
    if (type == "offer") t = webrtc::SdpType::kOffer;
    else if (type == "answer") t = webrtc::SdpType::kAnswer;
    else t = webrtc::SdpType::kPrAnswer;
    desc = webrtc::CreateSessionDescription(t, sdp);
    if (!desc) return Status::Fail("bad_request", "sdp_parse");
  }
  if (local) {
    auto obs = webrtc::make_ref_counted<SetLocalObs>();
    p->SetLocalDescription(std::move(desc), obs);
    if (!obs->Wait()) return Status::Fail("invalid_state", "set_local_failed");
    return Status::Ok();
  }
  auto obs = webrtc::make_ref_counted<SetRemoteObs>();
  p->SetRemoteDescription(std::move(desc), obs);
  if (!obs->Wait()) return Status::Fail("invalid_state", "set_remote_failed");
  if (t == webrtc::SdpType::kOffer && type != "rollback") {
    // The answerer sends audio too: attach the local track before the answer is made.
    return EnsureLocalAudio();
  }
  return Status::Ok();
}

Status Peer::AddIceCandidate(const std::string& candidate, const std::string& mid, int mline_index) {
  auto p = pc();
  if (!p) return Status::Fail("invalid_state", "pc_closed");
  if (candidate.empty()) return Status::Ok();  // end of candidates
  std::unique_ptr<webrtc::IceCandidate> c =
      webrtc::IceCandidate::Create(mid, mline_index, candidate, nullptr);
  if (!c) return Status::Fail("bad_request", "candidate_parse");
  if (!p->AddIceCandidate(c.get())) return Status::Fail("invalid_state", "add_candidate_failed");
  return Status::Ok();
}

Status Peer::RestartIce() {
  auto p = pc();
  if (!p) return Status::Fail("invalid_state", "pc_closed");
  p->RestartIce();
  return Status::Ok();
}

Status Peer::UpdateIceServers(const std::vector<IceServerSpec>& servers) {
  auto p = pc();
  if (!p) return Status::Fail("invalid_state", "pc_closed");
  webrtc::PeerConnectionInterface::RTCConfiguration config = p->GetConfiguration();
  config.servers = ToIceServers(servers);
  const webrtc::RTCError e = p->SetConfiguration(config);
  if (!e.ok()) return Status::Fail("bad_request", "ice_servers_rejected");
  return Status::Ok();
}

Status Peer::BindMedia(SessionKeys& keys, const std::string& mid, const std::string& participant,
                       bool send) {
  auto p = pc();
  if (!p) return Status::Fail("invalid_state", "pc_closed");
  {
    std::lock_guard<std::mutex> l(mu_);
    for (const Bound& b : bound_) {
      if (b.mid == mid && b.send == send) return Status::Fail("invalid_state", "already_bound");
    }
  }
  Status result = Status::Ok();
  Bound bound;
  bound.mid = mid;
  bound.send = send;
  bound.participant = participant;
  // Creating and attaching a cryptor must happen on the signaling thread.
  rt_.signaling()->BlockingCall([&] {
    webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> found;
    for (auto& t : p->GetTransceivers()) {
      const std::optional<std::string> m = t->mid();
      if (m && *m == mid) {
        found = t;
        break;
      }
    }
    if (!found) {
      result = Status::Fail("not_found", "mid");
      return;
    }
    const bool audio = found->media_type() == webrtc::MediaType::AUDIO;
    // The class is already a ref-counted object; it is created with new and held by a scoped_refptr.
    webrtc::scoped_refptr<webrtc::FrameCryptorTransformer> transformer(
        new webrtc::FrameCryptorTransformer(
            rt_.signaling(), SessionKeys::ProviderId(participant, send),
            audio ? webrtc::FrameCryptorTransformer::MediaType::kAudioFrame
                  : webrtc::FrameCryptorTransformer::MediaType::kVideoFrame,
            webrtc::FrameCryptorTransformer::Algorithm::kAesGcm, keys.provider()));
    auto obs = webrtc::make_ref_counted<CryptorObs>(this, mid, audio);
    transformer->RegisterFrameCryptorTransformerObserver(obs);
    transformer->SetKeyIndex(send ? keys.SendSlot(participant) : 0);
    transformer->SetEnabled(true);
    if (send) {
      found->sender()->SetFrameTransformer(transformer);
    } else {
      found->receiver()->SetFrameTransformer(transformer);
    }
    bound.transformer = transformer;
    bound.observer = obs;
  });
  if (!result.ok()) return result;
  if (send) keys.RegisterSender(participant, bound.transformer);
  std::lock_guard<std::mutex> l(mu_);
  keys_ = &keys;
  bound_.push_back(std::move(bound));
  return Status::Ok();
}

void Peer::OnCryptorState(const std::string& mid, const std::string& participant, bool audio,
                          webrtc::FrameCryptionState state) {
  const char* name = nullptr;
  switch (state) {
    case webrtc::FrameCryptionState::kOk: name = "ok"; break;
    case webrtc::FrameCryptionState::kMissingKey: name = "missing_key"; break;
    case webrtc::FrameCryptionState::kDecryptionFailed: name = "decryption_failed"; break;
    case webrtc::FrameCryptionState::kEncryptionFailed: name = "encryption_failed"; break;
    case webrtc::FrameCryptionState::kInternalError: name = "internal_error"; break;
    default: return;  // new, ratcheted: nothing to tell the host
  }
  Emit(ipc::BuildCryptorState(id_, mid, SanitizeText(participant, 64), audio ? "audio" : "video", name));
}

Status Peer::SetMuted(const std::string& track, bool muted) {
  auto p = pc();
  if (!p) return Status::Fail("invalid_state", "pc_closed");
  if (track == "mic") {
    webrtc::scoped_refptr<webrtc::AudioTrackInterface> t;
    {
      std::lock_guard<std::mutex> l(mu_);
      mic_muted_ = muted;
      t = track_;
    }
    if (t) t->set_enabled(!muted);
    return Status::Ok();
  }
  if (track == "remote_audio") {
    {
      std::lock_guard<std::mutex> l(mu_);
      remote_audio_muted_ = muted;
    }
    for (auto& r : p->GetReceivers()) {
      if (r->media_type() == webrtc::MediaType::AUDIO && r->track()) r->track()->set_enabled(!muted);
    }
    return Status::Ok();
  }
  return Status::Fail("unsupported", "video_not_in_this_build");
}

Status Peer::GetStats(StatsScope scope, std::vector<ipc::StatEntry>* out) {
  auto p = pc();
  if (!p) return Status::Fail("invalid_state", "pc_closed");
  auto obs = webrtc::make_ref_counted<StatsObs>();
  p->GetStats(obs.get());
  if (!obs->Wait()) return Status::Fail("internal", "stats_timeout");
  *out = ReduceStats(obs->report(), scope);
  return Status::Ok();
}

Status Peer::SetBitrate(int bitrate_bps) {
  auto p = pc();
  if (!p) return Status::Fail("invalid_state", "pc_closed");
  if (bitrate_bps <= 0) return Status::Ok();
  bool any = false;
  for (auto& s : p->GetSenders()) {
    if (s->media_type() != webrtc::MediaType::AUDIO) continue;
    webrtc::RtpParameters params = s->GetParameters();
    if (params.encodings.empty()) continue;
    for (auto& e : params.encodings) {
      // Same clamp as the mobile apps (applyNativeAudioEncoderClamp): the encoder is held at the
      // negotiated rate on both bounds, with adaptive packetisation off.
      e.min_bitrate_bps = bitrate_bps;
      e.max_bitrate_bps = bitrate_bps;
      e.adaptive_ptime = false;
      e.network_priority = webrtc::Priority::kHigh;
    }
    if (!s->SetParameters(params).ok()) return Status::Fail("invalid_state", "set_parameters_failed");
    any = true;
  }
  return any ? Status::Ok() : Status::Fail("invalid_state", "no_audio_sender");
}

void Peer::OnNegotiationNeededEvent(uint32_t) { Emit(ipc::BuildNegotiationNeeded(id_)); }

void Peer::OnStandardizedIceConnectionChange(webrtc::PeerConnectionInterface::IceConnectionState state) {
  Emit(ipc::BuildIceConnectionState(id_, IceStateName(state)));
}

void Peer::OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) {
  using S = webrtc::PeerConnectionInterface::IceGatheringState;
  const char* n = state == S::kIceGatheringNew ? "new"
                  : state == S::kIceGatheringGathering ? "gathering"
                                                       : "complete";
  Emit(ipc::BuildIceGatheringState(id_, n));
}

void Peer::OnIceCandidate(const webrtc::IceCandidate* candidate) {
  const std::string text = candidate->ToString();
  if (text.empty() || text.size() > 2048) return;
  const int idx = candidate->sdp_mline_index();
  const std::string mid = candidate->sdp_mid();
  Emit(ipc::BuildIceCandidate(id_, text, mid.size() <= 16 ? mid : std::string(), idx >= 0 && idx <= 255 ? idx : -1));
}

void Peer::OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) {
  bool muted;
  {
    std::lock_guard<std::mutex> l(mu_);
    muted = remote_audio_muted_;
  }
  if (muted && transceiver->media_type() == webrtc::MediaType::AUDIO && transceiver->receiver() &&
      transceiver->receiver()->track()) {
    transceiver->receiver()->track()->set_enabled(false);
  }
}

void Peer::OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) {
  Emit(ipc::BuildPcState(id_, PcStateName(state)));
  if (state == webrtc::PeerConnectionInterface::PeerConnectionState::kConnected) CheckTransport();
}

// Runs on the signaling thread after the connection reached "connected". Every DTLS transport must
// carry exactly the compiled-in policy; the library enforces it, this is the independent check
// whose verdict the host compares with what the signed handshake promised.
void Peer::CheckTransport() {
  auto p = pc();
  if (!p) return;
  for (auto& t : p->GetTransceivers()) {
    auto sender = t->sender();
    if (!sender) continue;
    auto dtls = sender->dtls_transport();
    if (!dtls) continue;
    {
      std::lock_guard<std::mutex> l(mu_);
      if (!reported_transports_.insert(dtls.get()).second) continue;
    }
    const webrtc::DtlsTransportInformation info = dtls->Information();
    if (info.state() != webrtc::DtlsTransportState::kConnected) {
      std::lock_guard<std::mutex> l(mu_);
      reported_transports_.erase(dtls.get());
      continue;
    }
    const char* violation = nullptr;
    if (!info.tls_version()) violation = "no_dtls";
    else if (*info.tls_version() != kDtls13) violation = "tls_version";
    else if (info.ssl_cipher_suite().value_or(-1) != kTlsAes256GcmSha384) violation = "dtls_cipher";
    else if (info.ssl_group_id().value_or(-1) != kX25519Mlkem768) violation = "group";
    else if (info.srtp_cipher_suite().value_or(-1) != kSrtpAeadAes256Gcm) violation = "srtp_cipher";

    webrtc::Buffer digest;
    bool have_fp = false;
    const webrtc::SSLCertChain* chain = info.remote_ssl_certificates();
    if (chain != nullptr && chain->GetSize() > 0 &&
        chain->Get(0).ComputeDigest("sha-256", digest) && digest.size() == ipc::kFingerprintBytes) {
      have_fp = true;
    }
    if (violation == nullptr && !have_fp) violation = "no_dtls";

    if (violation != nullptr) {
      Emit(ipc::BuildTransportViolation(id_, violation));
      // The policy is broken: close the connection (not from inside this callback).
      rt_.signaling()->PostTask([p] { p->Close(); });
      return;
    }
    Emit(ipc::BuildTransportInfo(id_, "DTLS1.3", "TLS_AES_256_GCM_SHA384", "X25519MLKEM768",
                                 "AEAD_AES_256_GCM",
                                 std::span<const uint8_t>(digest.data(), digest.size())));
  }
}

}  // namespace qmedia::engine
