// Out-of-tree consumer smoke test for the M150 Windows library (S0.2).
//
// This file is compiled and linked by m150/ci/link-smoke.py using ONLY what a
// consumer of the release gets: webrtc-headers.zip, build-flags.json,
// webrtc.lib and libcxx.lib. It deliberately lives outside the GN build.
//
// What it proves, in one process and with no network beyond the loopback:
//   1. the headers + flags + libraries really compile and link together
//      (ABI: STL, CRT, RTTI, defines);
//   2. two PeerConnections created from ONE strict factory connect through
//      loopback ICE with an audio track and a data channel;
//   3. the negotiated transport is exactly the hardened one: DTLS 1.3,
//      TLS_AES_256_GCM_SHA384, SRTP AEAD_AES_256_GCM, key exchange group
//      X25519MLKEM768 (on both ends);
//   4. application data crosses the encrypted association.
//
// Output carries verdicts only: no SDP, no fingerprints, no addresses, no ids.

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/audio_options.h"
#include "api/create_peerconnection_factory.h"
#include "api/data_channel_interface.h"
#include "api/dtls_transport_interface.h"
#include "api/environment/environment_factory.h"
#include "api/jsep.h"
#include "api/make_ref_counted.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/rtc_error.h"
#include "api/rtp_sender_interface.h"
#include "api/scoped_refptr.h"
#include "api/set_local_description_observer_interface.h"
#include "api/set_remote_description_observer_interface.h"
#include "api/stats/rtc_stats_collector_callback.h"
#include "api/stats/rtc_stats_report.h"
#include "api/stats/rtcstats_objects.h"
#include "rtc_base/qaudion_tuning.h"
#include "rtc_base/ssl_adapter.h"

namespace {

using std::chrono::seconds;

constexpr uint16_t kDtls13 = 0xfefc;
constexpr int kTlsAes256GcmSha384 = 0x1302;
constexpr int kSrtpAeadAes256Gcm = 0x0008;
constexpr int kX25519Mlkem768 = 0x11ec;

int g_failures = 0;

void Check(bool ok, const char* what) {
  std::printf("[smoke] %-58s %s\n", what, ok ? "ok" : "FAILED");
  std::fflush(stdout);
  if (!ok) {
    ++g_failures;
  }
}

class Event {
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

class CreateSdpObserver : public webrtc::CreateSessionDescriptionObserver {
 public:
  void OnSuccess(webrtc::SessionDescriptionInterface* desc) override {
    desc_.reset(desc);  // ownership is transferred to the observer
    done_.Set();
  }
  void OnFailure(webrtc::RTCError) override {
    failed_ = true;
    done_.Set();
  }
  bool Wait() { return done_.WaitFor(seconds(20)) && !failed_ && desc_; }
  std::unique_ptr<webrtc::SessionDescriptionInterface> Take() {
    return std::move(desc_);
  }

 private:
  Event done_;
  bool failed_ = false;
  std::unique_ptr<webrtc::SessionDescriptionInterface> desc_;
};

class SetLocalObserver : public webrtc::SetLocalDescriptionObserverInterface {
 public:
  void OnSetLocalDescriptionComplete(webrtc::RTCError error) override {
    ok_ = error.ok();
    done_.Set();
  }
  bool Wait() { return done_.WaitFor(seconds(20)) && ok_; }

 private:
  Event done_;
  bool ok_ = false;
};

class SetRemoteObserver : public webrtc::SetRemoteDescriptionObserverInterface {
 public:
  void OnSetRemoteDescriptionComplete(webrtc::RTCError error) override {
    ok_ = error.ok();
    done_.Set();
  }
  bool Wait() { return done_.WaitFor(seconds(20)) && ok_; }

 private:
  Event done_;
  bool ok_ = false;
};

class StatsObserver : public webrtc::RTCStatsCollectorCallback {
 public:
  void OnStatsDelivered(
      const webrtc::scoped_refptr<const webrtc::RTCStatsReport>& report)
      override {
    for (const webrtc::RTCTransportStats* t :
         report->GetStatsOfType<webrtc::RTCTransportStats>()) {
      if (t->dtls_cipher.has_value()) {
        dtls_cipher_ = *t->dtls_cipher;
      }
      if (t->srtp_cipher.has_value()) {
        srtp_cipher_ = *t->srtp_cipher;
      }
      if (t->dtls_state.has_value()) {
        dtls_state_ = *t->dtls_state;
      }
    }
    done_.Set();
  }
  bool Wait() { return done_.WaitFor(seconds(20)); }
  const std::string& dtls_cipher() const { return dtls_cipher_; }
  const std::string& srtp_cipher() const { return srtp_cipher_; }
  const std::string& dtls_state() const { return dtls_state_; }

 private:
  Event done_;
  std::string dtls_cipher_;
  std::string srtp_cipher_;
  std::string dtls_state_;
};

class Peer : public webrtc::PeerConnectionObserver,
             public webrtc::DataChannelObserver {
 public:
  void OnSignalingChange(
      webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnIceCandidate(const webrtc::IceCandidate*) override {}
  void OnIceGatheringChange(
      webrtc::PeerConnectionInterface::IceGatheringState state) override {
    if (state == webrtc::PeerConnectionInterface::kIceGatheringComplete) {
      gathered_.Set();
    }
  }
  void OnConnectionChange(
      webrtc::PeerConnectionInterface::PeerConnectionState state) override {
    if (state == webrtc::PeerConnectionInterface::PeerConnectionState::
                     kConnected) {
      connected_.Set();
    }
    if (state ==
            webrtc::PeerConnectionInterface::PeerConnectionState::kFailed ||
        state ==
            webrtc::PeerConnectionInterface::PeerConnectionState::kClosed) {
      failed_.Set();
    }
  }
  void OnDataChannel(
      webrtc::scoped_refptr<webrtc::DataChannelInterface> channel) override {
    remote_channel_ = channel;
    remote_channel_->RegisterObserver(this);
  }
  // DataChannelObserver
  void OnStateChange() override {
    if (local_channel_ &&
        local_channel_->state() == webrtc::DataChannelInterface::kOpen) {
      channel_open_.Set();
    }
  }
  void OnMessage(const webrtc::DataBuffer& buffer) override {
    std::lock_guard<std::mutex> l(m_);
    message_.assign(reinterpret_cast<const char*>(buffer.data.data()),
                    buffer.data.size());
    got_message_.Set();
  }
  std::string message() {
    std::lock_guard<std::mutex> l(m_);
    return message_;
  }

  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> local_channel_;
  webrtc::scoped_refptr<webrtc::DataChannelInterface> remote_channel_;
  Event gathered_;
  Event connected_;
  Event failed_;
  Event channel_open_;
  Event got_message_;

 private:
  std::mutex m_;
  std::string message_;
};

std::optional<std::string> ToSdp(const webrtc::SessionDescriptionInterface* d) {
  std::string out;
  if (!d || !d->ToString(&out)) {
    return std::nullopt;
  }
  return out;
}

// Reads the negotiated transport of the first sender through the public API.
struct TransportInfo {
  bool valid = false;
  int tls_version = -1;
  int cipher = -1;
  int srtp = -1;
  int group = -1;
};

TransportInfo ReadTransport(Peer& p) {
  TransportInfo out;
  auto senders = p.pc->GetSenders();
  if (senders.empty()) {
    return out;
  }
  auto dtls = senders[0]->dtls_transport();
  if (!dtls) {
    return out;
  }
  webrtc::DtlsTransportInformation info = dtls->Information();
  if (info.state() != webrtc::DtlsTransportState::kConnected) {
    return out;
  }
  out.valid = true;
  out.tls_version = info.tls_version().value_or(-1);
  out.cipher = info.ssl_cipher_suite().value_or(-1);
  out.srtp = info.srtp_cipher_suite().value_or(-1);
  out.group = info.ssl_group_id().value_or(-1);
  return out;
}

void CheckTransport(const char* side, const TransportInfo& t) {
  std::string label = std::string(side) + ": dtls transport connected";
  Check(t.valid, label.c_str());
  label = std::string(side) + ": DTLS 1.3 (0xFEFC)";
  Check(t.tls_version == kDtls13, label.c_str());
  label = std::string(side) + ": TLS_AES_256_GCM_SHA384";
  Check(t.cipher == kTlsAes256GcmSha384, label.c_str());
  label = std::string(side) + ": SRTP AEAD_AES_256_GCM";
  Check(t.srtp == kSrtpAeadAes256Gcm, label.c_str());
  label = std::string(side) + ": group X25519MLKEM768";
  Check(t.group == kX25519Mlkem768, label.c_str());
}

void CheckStats(const char* side, Peer& p) {
  auto cb = webrtc::make_ref_counted<StatsObserver>();
  p.pc->GetStats(cb.get());
  std::string label = std::string(side) + ": getStats delivered";
  Check(cb->Wait(), label.c_str());
  label = std::string(side) + ": stats dtlsCipher TLS_AES_256_GCM_SHA384";
  Check(cb->dtls_cipher() == "TLS_AES_256_GCM_SHA384", label.c_str());
  label = std::string(side) + ": stats srtpCipher AEAD_AES_256_GCM";
  Check(cb->srtp_cipher() == "AEAD_AES_256_GCM", label.c_str());
}

}  // namespace

int main() {
  std::printf("[smoke] build info: %s\n",
              webrtc::qaudion::BuildInfo().c_str());
  Check(webrtc::qaudion::BuildInfo().find("transport=strict") !=
            std::string::npos,
        "qaudion marker transport=strict");
  Check(webrtc::qaudion::TransportLevel() == 3, "qaudion transport level 3");
  Check(!webrtc::qaudion::MagicBytesBypassAllowed(),
        "frame-cryptor magic-bytes bypass not allowed");

  webrtc::InitializeSSL();

  webrtc::Environment env = webrtc::CreateEnvironment();
  auto adm = webrtc::CreateAudioDeviceModule(
      env, webrtc::AudioDeviceModule::kDummyAudio);
  Check(adm != nullptr, "dummy audio device module");
  auto apm = webrtc::BuiltinAudioProcessingBuilder().Build(env);

  auto factory = webrtc::CreatePeerConnectionFactory(
      /*network_thread=*/nullptr, /*worker_thread=*/nullptr,
      /*signaling_thread=*/nullptr, adm,
      webrtc::CreateBuiltinAudioEncoderFactory(),
      webrtc::CreateBuiltinAudioDecoderFactory(),
      /*video_encoder_factory=*/nullptr, /*video_decoder_factory=*/nullptr,
      /*audio_mixer=*/nullptr, apm);
  Check(factory != nullptr, "strict peer connection factory");
  if (!factory) {
    return 2;
  }

  // Allow the loopback interface so the test also works on a runner without a
  // usable physical adapter.
  webrtc::PeerConnectionFactoryInterface::Options options;
  options.network_ignore_mask = 0;
  factory->SetOptions(options);

  webrtc::PeerConnectionInterface::RTCConfiguration config;
  config.sdp_semantics = webrtc::SdpSemantics::kUnifiedPlan;

  Peer a;
  Peer b;
  {
    auto ra = factory->CreatePeerConnectionOrError(
        config, webrtc::PeerConnectionDependencies(&a));
    auto rb = factory->CreatePeerConnectionOrError(
        config, webrtc::PeerConnectionDependencies(&b));
    Check(ra.ok() && rb.ok(), "two peer connections in one process");
    if (!ra.ok() || !rb.ok()) {
      return 2;
    }
    a.pc = ra.MoveValue();
    b.pc = rb.MoveValue();
  }

  webrtc::AudioOptions audio_options;
  auto source = factory->CreateAudioSource(audio_options);
  auto track = factory->CreateAudioTrack("smoke-audio", source.get());
  auto added = a.pc->AddTrack(track, {"smoke-stream"});
  Check(added.ok(), "audio track added");

  auto channel = a.pc->CreateDataChannelOrError("smoke-data", nullptr);
  Check(channel.ok(), "data channel created");
  if (channel.ok()) {
    a.local_channel_ = channel.MoveValue();
    a.local_channel_->RegisterObserver(&a);
  }

  // Offer / answer, no trickle: wait for gathering to finish on each side and
  // hand over the complete local description.
  auto offer_obs = webrtc::make_ref_counted<CreateSdpObserver>();
  a.pc->CreateOffer(offer_obs.get(),
                    webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  Check(offer_obs->Wait(), "offer created");
  auto offer = offer_obs->Take();
  auto set_local_a = webrtc::make_ref_counted<SetLocalObserver>();
  a.pc->SetLocalDescription(std::move(offer), set_local_a);
  Check(set_local_a->Wait(), "offerer local description set");
  Check(a.gathered_.WaitFor(seconds(20)), "offerer ICE gathering complete");
  std::optional<std::string> offer_sdp = ToSdp(a.pc->local_description());
  Check(offer_sdp.has_value(), "offerer local description readable");
  if (!offer_sdp) {
    return 2;
  }

  auto set_remote_b = webrtc::make_ref_counted<SetRemoteObserver>();
  b.pc->SetRemoteDescription(
      webrtc::CreateSessionDescription(webrtc::SdpType::kOffer, *offer_sdp),
      set_remote_b);
  Check(set_remote_b->Wait(), "answerer remote description set");

  auto answer_obs = webrtc::make_ref_counted<CreateSdpObserver>();
  b.pc->CreateAnswer(answer_obs.get(),
                     webrtc::PeerConnectionInterface::RTCOfferAnswerOptions());
  Check(answer_obs->Wait(), "answer created");
  auto answer = answer_obs->Take();
  auto set_local_b = webrtc::make_ref_counted<SetLocalObserver>();
  b.pc->SetLocalDescription(std::move(answer), set_local_b);
  Check(set_local_b->Wait(), "answerer local description set");
  Check(b.gathered_.WaitFor(seconds(20)), "answerer ICE gathering complete");
  std::optional<std::string> answer_sdp = ToSdp(b.pc->local_description());
  Check(answer_sdp.has_value(), "answerer local description readable");
  if (!answer_sdp) {
    return 2;
  }

  auto set_remote_a = webrtc::make_ref_counted<SetRemoteObserver>();
  a.pc->SetRemoteDescription(
      webrtc::CreateSessionDescription(webrtc::SdpType::kAnswer, *answer_sdp),
      set_remote_a);
  Check(set_remote_a->Wait(), "offerer remote description set");

  const bool connected = a.connected_.WaitFor(seconds(40)) &&
                         b.connected_.WaitFor(seconds(40));
  Check(connected, "both peers connected over loopback");

  if (connected) {
    CheckTransport("offerer ", ReadTransport(a));
    CheckTransport("answerer", ReadTransport(b));
    CheckStats("offerer ", a);
    CheckStats("answerer", b);

    if (a.local_channel_) {
      Check(a.channel_open_.WaitFor(seconds(20)), "data channel open");
      a.local_channel_->Send(webrtc::DataBuffer(std::string("q-audion-smoke")));
      Check(b.got_message_.WaitFor(seconds(20)) &&
                b.message() == "q-audion-smoke",
            "data crosses the encrypted association");
    }
  }

  if (a.local_channel_) {
    a.local_channel_->UnregisterObserver();
  }
  if (b.remote_channel_) {
    b.remote_channel_->UnregisterObserver();
  }
  a.pc->Close();
  b.pc->Close();
  a.pc = nullptr;
  b.pc = nullptr;
  factory = nullptr;
  webrtc::CleanupSSL();

  if (g_failures != 0) {
    std::printf("[smoke] FAILED (%d check(s))\n", g_failures);
    return 1;
  }
  std::printf("[smoke] SMOKE-OK\n");
  return 0;
}
