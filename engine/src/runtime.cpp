#include "runtime.h"

#include <winsock2.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <utility>

#include "api/audio/audio_processing.h"
#include "api/audio/builtin_audio_processing_builder.h"
#include "api/audio/create_audio_device_module.h"
#include "api/audio_codecs/builtin_audio_decoder_factory.h"
#include "api/audio_codecs/builtin_audio_encoder_factory.h"
#include "api/create_peerconnection_factory.h"
#include "api/environment/environment_factory.h"
#include "api/make_ref_counted.h"
#include "rtc_base/qaudion_tuning.h"
#include "rtc_base/ssl_adapter.h"

#if defined(QMEDIA_CI_BUILD)
#include "modules/audio_device/audio_device_impl.h"
#include "modules/audio_device/dummy/file_audio_device.h"
#endif

namespace qmedia::engine {

namespace {

// Number of bytes of the UTF-8 sequence that starts at s[i], or 0 if it is not valid there.
size_t Utf8SeqLen(const std::string& s, size_t i) {
  const unsigned char c = static_cast<unsigned char>(s[i]);
  size_t need = 0;
  uint32_t min = 0;
  if (c < 0x80) return 1;
  if (c >= 0xC2 && c <= 0xDF) {
    need = 2;
    min = 0x80;
  } else if (c >= 0xE0 && c <= 0xEF) {
    need = 3;
    min = 0x800;
  } else if (c >= 0xF0 && c <= 0xF4) {
    need = 4;
    min = 0x10000;
  } else {
    return 0;
  }
  if (i + need > s.size()) return 0;
  uint32_t cp = c & (0xFFu >> (need + 1));
  for (size_t k = 1; k < need; ++k) {
    const unsigned char cc = static_cast<unsigned char>(s[i + k]);
    if ((cc & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (cc & 0x3F);
  }
  if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
  return need;
}

std::string Lower(std::string s) {
  for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

}  // namespace

std::string SanitizeText(const std::string& in, size_t max_bytes) {
  std::string out;
  out.reserve(std::min(in.size(), max_bytes));
  for (size_t i = 0; i < in.size();) {
    const size_t n = Utf8SeqLen(in, i);
    if (n == 0) {  // not valid UTF-8 here: one replacement per byte
      if (out.size() + 1 > max_bytes) break;
      out.push_back('?');
      ++i;
      continue;
    }
    if (out.size() + n > max_bytes) break;
    if (n == 1) {
      const unsigned char c = static_cast<unsigned char>(in[i]);
      out.push_back((c < 0x20 || c == 0x7F) ? ' ' : static_cast<char>(c));
    } else {
      out.append(in, i, n);
    }
    i += n;
  }
  return out;
}

std::unique_ptr<Runtime> Runtime::Create(const Options& opts) {
  std::unique_ptr<Runtime> r(new Runtime());
  if (!r->Init(opts)) return nullptr;
  return r;
}

bool Runtime::Init(const Options& opts) {
  // The library must be the strict build. These are not tunable: a library that is not strict
  // (or that allows the frame-cryptor magic-bytes bypass) is refused outright.
  if (webrtc::qaudion::TransportLevel() != 3 || webrtc::qaudion::MagicBytesBypassAllowed() ||
      webrtc::qaudion::BuildInfo().find("transport=strict") == std::string::npos) {
    return false;
  }
  webrtc::qaudion::SetRequireDtlsPqc();  // tighten-only; the strict build already pins it

  WSADATA wsa;
  wsa_started_ = WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
  if (!wsa_started_) return false;
  webrtc::InitializeSSL();

  const webrtc::Environment env = webrtc::CreateEnvironment();

  if (opts.fake_audio) {
#if defined(QMEDIA_CI_BUILD)
    auto device = std::make_unique<webrtc::FileAudioDevice>(env, opts.fake_audio_in, opts.fake_audio_out);
    auto impl = webrtc::make_ref_counted<webrtc::AudioDeviceModuleImpl>(
        env, webrtc::AudioDeviceModule::kDummyAudio, std::move(device), /*create_detached=*/true);
    if (impl->CreatePlatformSpecificObjects(env) != 0 || impl->AttachAudioBuffer() != 0) return false;
    adm_ = impl;
#else
    return false;
#endif
  } else {
    // The Windows Core Audio device module of this library. The published library carries the
    // original Core Audio implementation (AudioDeviceWindowsCore); the newer one that has its own
    // factory function is not part of it.
    adm_ = webrtc::CreateAudioDeviceModule(env, webrtc::AudioDeviceModule::kPlatformDefaultAudio);
  }
  if (!adm_) return false;

  // AEC3, noise suppression and automatic gain control run in the audio processing module; the
  // audio source below asks for all three.
  apm_ = webrtc::BuiltinAudioProcessingBuilder().Build(env);
  if (!apm_) return false;

  network_ = webrtc::Thread::CreateWithSocketServer();
  worker_ = webrtc::Thread::Create();
  signaling_ = webrtc::Thread::Create();
  network_->SetName("qm-network", nullptr);
  worker_->SetName("qm-worker", nullptr);
  signaling_->SetName("qm-signaling", nullptr);
  if (!network_->Start() || !worker_->Start() || !signaling_->Start()) return false;

  factory_ = webrtc::CreatePeerConnectionFactory(
      network_.get(), worker_.get(), signaling_.get(), adm_,
      webrtc::CreateBuiltinAudioEncoderFactory(), webrtc::CreateBuiltinAudioDecoderFactory(),
      /*video_encoder_factory=*/nullptr, /*video_decoder_factory=*/nullptr,
      /*audio_mixer=*/nullptr, apm_);
  if (!factory_) return false;

  if (opts.allow_loopback) {
    webrtc::PeerConnectionFactoryInterface::Options o;
    o.network_ignore_mask = 0;
    factory_->SetOptions(o);
  }
  return true;
}

Runtime::~Runtime() {
  factory_ = nullptr;
  if (signaling_) signaling_->Stop();
  if (worker_) worker_->Stop();
  if (network_) network_->Stop();
  adm_ = nullptr;
  apm_ = nullptr;
  if (wsa_started_) {
    webrtc::CleanupSSL();
    WSACleanup();
  }
}

std::vector<AudioDeviceEntry> Runtime::ListAudioDevices(bool input) {
  std::vector<AudioDeviceEntry> out;
  worker_->BlockingCall([&] {
    const int16_t n = input ? adm_->RecordingDevices() : adm_->PlayoutDevices();
    // The Windows implementation lists the default device first and the default communications
    // device second, then every endpoint.
    webrtc::AudioDeviceModule::AudioLayer layer = webrtc::AudioDeviceModule::kDummyAudio;
    adm_->ActiveAudioLayer(&layer);
    const bool windows_core = layer == webrtc::AudioDeviceModule::kWindowsCoreAudio ||
                              layer == webrtc::AudioDeviceModule::kWindowsCoreAudio2 ||
                              layer == webrtc::AudioDeviceModule::kPlatformDefaultAudio;
    for (int16_t i = 0; i < n && out.size() < 64; ++i) {
      char name[webrtc::kAdmMaxDeviceNameSize] = {0};
      char guid[webrtc::kAdmMaxGuidSize] = {0};
      const int32_t rc = input ? adm_->RecordingDeviceName(static_cast<uint16_t>(i), name, guid)
                               : adm_->PlayoutDeviceName(static_cast<uint16_t>(i), name, guid);
      if (rc != 0) continue;
      name[sizeof name - 1] = '\0';
      guid[sizeof guid - 1] = '\0';
      AudioDeviceEntry e;
      e.name = SanitizeText(name, 128);
      if (windows_core && i == 0) {
        e.id = "default";
        e.is_default = true;
      } else if (windows_core && i == 1) {
        e.id = "communications";
        e.is_communications = true;
      } else {
        e.id = SanitizeText(guid, 256);
        if (e.id.empty()) e.id = "index-" + std::to_string(i);
      }
      out.push_back(std::move(e));
    }
  });
  return out;
}

Status Runtime::SelectAudioDevice(bool input, const std::string& id) {
  Status result = Status::Ok();
  worker_->BlockingCall([&] {
    int32_t index = -1;
    bool by_type = false;
    webrtc::AudioDeviceModule::WindowsDeviceType type = webrtc::AudioDeviceModule::kDefaultCommunicationDevice;
    if (id.empty() || id == "communications") {
      by_type = true;
    } else if (id == "default") {
      by_type = true;
      type = webrtc::AudioDeviceModule::kDefaultDevice;
    } else {
      const int16_t n = input ? adm_->RecordingDevices() : adm_->PlayoutDevices();
      for (int16_t i = 0; i < n; ++i) {
        char name[webrtc::kAdmMaxDeviceNameSize] = {0};
        char guid[webrtc::kAdmMaxGuidSize] = {0};
        const int32_t rc = input ? adm_->RecordingDeviceName(static_cast<uint16_t>(i), name, guid)
                                 : adm_->PlayoutDeviceName(static_cast<uint16_t>(i), name, guid);
        if (rc != 0) continue;
        guid[sizeof guid - 1] = '\0';
        std::string g = SanitizeText(guid, 256);
        if (g.empty()) g = "index-" + std::to_string(i);
        if (i >= 2 && g == id) {
          index = i;
          break;
        }
        if (i < 2 && Lower(g) == Lower(id)) {  // a non-Windows module without the two aliases
          index = i;
          break;
        }
      }
      if (index < 0) {
        result = Status::Fail("not_found", "device");
        return;
      }
    }

    // Stop, switch, start again: a stream that is running must move to the new device.
    const bool active = input ? adm_->Recording() : adm_->Playing();
    if (active) {
      if (input) {
        adm_->StopRecording();
      } else {
        adm_->StopPlayout();
      }
    }
    int32_t rc = 0;
    if (input) {
      rc = by_type ? adm_->SetRecordingDevice(type) : adm_->SetRecordingDevice(static_cast<uint16_t>(index));
    } else {
      rc = by_type ? adm_->SetPlayoutDevice(type) : adm_->SetPlayoutDevice(static_cast<uint16_t>(index));
    }
    if (rc != 0) {
      result = Status::Fail("invalid_state", "device_select_failed");
    }
    if (active) {
      int32_t init = input ? adm_->InitRecording() : adm_->InitPlayout();
      int32_t start = 0;
      if (init == 0) start = input ? adm_->StartRecording() : adm_->StartPlayout();
      if ((init != 0 || start != 0) && result.ok()) {
        result = Status::Fail("invalid_state", "device_restart_failed");
      }
    }
  });
  return result;
}

}  // namespace qmedia::engine
