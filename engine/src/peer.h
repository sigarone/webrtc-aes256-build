// One peer connection of a session: SDP, ICE, transport verdicts, frame cryptors, mute, tuning,
// statistics. All verdicts are static strings (see Status); nothing from the network or from
// libwebrtc messages is copied into a reply or an event except the values the schema asks for.
#pragma once

#include <cstdint>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "api/crypto/frame_crypto_transformer.h"
#include "api/jsep.h"
#include "api/media_stream_interface.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "keys.h"
#include "qmedia/ipc/cbor.h"
#include "qmedia/ipc/message.h"
#include "runtime.h"
#include "stats.h"

namespace qmedia::engine {

// Where events go. Thread safe; may be called from any libwebrtc thread.
class Emitter {
 public:
  virtual ~Emitter() = default;
  virtual void Emit(ipc::cbor::Buf payload) = 0;
};

struct IceServerSpec {
  std::vector<std::string> urls;
  std::string username;
  std::string credential;
};

struct PeerParams {
  bool relay_only = false;
  std::vector<IceServerSpec> ice_servers;
};

class Peer final : public webrtc::PeerConnectionObserver {
 public:
  Peer(uint32_t id, uint32_t session, Runtime& rt, Emitter& out);
  ~Peer() override;

  // Creates the peer connection with the call's certificate. The configuration is compiled in.
  Status Init(const PeerParams& params, webrtc::scoped_refptr<webrtc::RTCCertificate> cert);
  // Idempotent. After it returns no event of this peer is emitted any more.
  void Close();

  uint32_t id() const { return id_; }
  uint32_t session() const { return session_; }
  bool closed() const;

  Status CreateOffer(bool ice_restart, std::string* sdp);
  Status CreateAnswer(std::string* sdp);
  Status SetDescription(bool local, const std::string& type, const std::string& sdp);
  Status AddIceCandidate(const std::string& candidate, const std::string& mid, int mline_index);
  Status RestartIce();
  Status UpdateIceServers(const std::vector<IceServerSpec>& servers);

  // Ties the frame cryptor of the media section `mid` to a participant and a direction.
  Status BindMedia(SessionKeys& keys, const std::string& mid, const std::string& participant, bool send);
  Status SetMuted(const std::string& track, bool muted);
  Status GetStats(StatsScope scope, std::vector<ipc::StatEntry>* out);
  // bitrate_bps: 0 = unchanged.
  Status SetBitrate(int bitrate_bps);

  // PeerConnectionObserver
  void OnSignalingChange(webrtc::PeerConnectionInterface::SignalingState) override {}
  void OnDataChannel(webrtc::scoped_refptr<webrtc::DataChannelInterface>) override {}
  void OnNegotiationNeededEvent(uint32_t event_id) override;
  void OnStandardizedIceConnectionChange(
      webrtc::PeerConnectionInterface::IceConnectionState state) override;
  void OnConnectionChange(webrtc::PeerConnectionInterface::PeerConnectionState state) override;
  void OnIceGatheringChange(webrtc::PeerConnectionInterface::IceGatheringState state) override;
  void OnIceCandidate(const webrtc::IceCandidate* candidate) override;
  void OnTrack(webrtc::scoped_refptr<webrtc::RtpTransceiverInterface> transceiver) override;

  // Called by the cryptor observers.
  void OnCryptorState(const std::string& mid, const std::string& participant, bool audio,
                      webrtc::FrameCryptionState state);

 private:
  struct Bound {
    std::string mid;
    bool send = false;
    std::string participant;
    webrtc::scoped_refptr<webrtc::FrameCryptorTransformer> transformer;
    webrtc::scoped_refptr<webrtc::FrameCryptorTransformerObserver> observer;
  };

  Status EnsureLocalAudio();
  void CheckTransport();
  void Emit(ipc::cbor::Buf payload);
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc() const;

  const uint32_t id_;
  const uint32_t session_;
  Runtime& rt_;
  Emitter& out_;

  mutable std::mutex mu_;
  webrtc::scoped_refptr<webrtc::PeerConnectionInterface> pc_;
  bool closed_ = false;
  bool local_added_ = false;
  bool mic_muted_ = false;
  bool remote_audio_muted_ = false;
  webrtc::scoped_refptr<webrtc::AudioTrackInterface> track_;
  std::set<const void*> reported_transports_;
  std::vector<Bound> bound_;
  SessionKeys* keys_ = nullptr;  // set by BindMedia; only used to unregister sender cryptors
};

}  // namespace qmedia::engine
