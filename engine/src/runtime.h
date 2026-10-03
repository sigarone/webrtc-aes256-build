// The libwebrtc runtime of the engine: threads, audio device, audio processing and the one
// peer-connection factory. Everything here is generic media plumbing; the transport policy is
// compiled into libwebrtc (rtc_qaudion_transport_strict) and nothing in this file can relax it.
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "api/audio/audio_device.h"
#include "api/audio/audio_processing.h"
#include "api/environment/environment.h"
#include "api/peer_connection_interface.h"
#include "api/scoped_refptr.h"
#include "rtc_base/ssl_certificate.h"
#include "rtc_base/thread.h"

namespace qmedia::engine {

// How the engine is started. The production executable only ever uses the defaults; the CI
// executable (QMEDIA_CI_BUILD) can replace the sound card with a file reader/writer so a runner
// without audio hardware can run a real call.
struct Options {
  bool fake_audio = false;
  std::string fake_audio_in;   // raw 48 kHz stereo 16-bit file that plays the microphone
  std::string fake_audio_out;  // raw file that receives what would have been played out
  bool allow_loopback = false;  // lets ICE use the loopback interface (CI only)
};

// A verdict with static strings only: never a copy of input, never a library message.
struct Status {
  const char* code = nullptr;    // an ipc errcode value, or nullptr for success
  const char* detail = nullptr;  // static identifier, may be nullptr
  bool ok() const { return code == nullptr; }
  static Status Ok() { return Status{}; }
  static Status Fail(const char* code, const char* detail = nullptr) { return Status{code, detail}; }
};

struct AudioDeviceEntry {
  std::string id;
  std::string name;
  bool is_default = false;
  bool is_communications = false;
};

class Runtime {
 public:
  // Starts the threads and builds the factory. Returns nullptr on failure.
  static std::unique_ptr<Runtime> Create(const Options& opts);
  ~Runtime();
  Runtime(const Runtime&) = delete;
  Runtime& operator=(const Runtime&) = delete;

  webrtc::PeerConnectionFactoryInterface* factory() { return factory_.get(); }
  webrtc::Thread* signaling() { return signaling_.get(); }
  webrtc::Thread* worker() { return worker_.get(); }

  // Audio devices. Ids are opaque to the host. kind: true = input (microphone), false = output.
  std::vector<AudioDeviceEntry> ListAudioDevices(bool input);
  // An empty id selects the system default. Re-initialises a running stream on the new device.
  Status SelectAudioDevice(bool input, const std::string& id);

 private:
  Runtime() = default;
  bool Init(const Options& opts);

  webrtc::scoped_refptr<webrtc::AudioDeviceModule> adm_;
  webrtc::scoped_refptr<webrtc::AudioProcessing> apm_;
  std::unique_ptr<webrtc::Thread> network_;
  std::unique_ptr<webrtc::Thread> worker_;
  std::unique_ptr<webrtc::Thread> signaling_;
  webrtc::scoped_refptr<webrtc::PeerConnectionFactoryInterface> factory_;
  bool wsa_started_ = false;
};

// SHA-256 of the DER encoding of a certificate: the DTLS fingerprint the signalling binds. Computed
// here from the DER bytes with the operating system's hash, so it does not depend on how the
// library names or sizes its own digest.
bool CertSha256(const webrtc::SSLCertificate& cert, std::array<uint8_t, 32>* out);

// The same digest for a certificate given as the standard base64 of its DER encoding, which is how
// the library's certificate statistics carry it. The identical hash path as CertSha256, so a
// fingerprint read from the statistics equals the one cert_create returns for the same certificate.
bool CertBase64Sha256(const std::string& base64_der, std::array<uint8_t, 32>* out);

// Replaces every control character by a space and cuts the text to at most max_bytes at a UTF-8
// boundary, so it satisfies the schema's text rules. Used for device names and ids.
std::string SanitizeText(const std::string& in, size_t max_bytes);

}  // namespace qmedia::engine
