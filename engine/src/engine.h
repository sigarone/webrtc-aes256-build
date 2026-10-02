// The engine core: the IPC message handler that owns sessions, certificates, peer connections,
// frame keys and the libwebrtc runtime. It is generic: it knows no messaging protocol, no
// identity and no server.
//
// Threading: Handle() runs on the IPC reader thread only, so the tables below need no lock.
// Events come from libwebrtc threads through Emit(), which serialises writes with one mutex and
// stops for good once Shutdown() has run.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "rtc_base/rtc_certificate.h"
#include "devices.h"
#include "keys.h"
#include "peer.h"
#include "qmedia/ipc/frame.h"
#include "qmedia/ipc/session.h"
#include "runtime.h"

namespace qmedia::engine {

inline constexpr size_t kMaxSessions = 4;
inline constexpr size_t kMaxCertsPerSession = 8;
inline constexpr size_t kMaxPcsPerSession = 8;

class Engine final : public ipc::MessageHandler, public Emitter {
 public:
  Engine(ipc::ByteStream& stream, Options opts);
  ~Engine() override;

  ipc::HandlerAction Handle(const ipc::ValidatedMessage& msg, ipc::Outbox& out) override;

  // Closes every peer connection, overwrites every frame key and stops all events. Idempotent.
  void Shutdown();

  // Emitter
  void Emit(ipc::cbor::Buf payload) override;

 private:
  struct CertEntry {
    uint32_t id = 0;
    webrtc::scoped_refptr<webrtc::RTCCertificate> cert;
  };
  struct Session {
    uint32_t id = 0;
    std::unique_ptr<SessionKeys> keys;
    std::vector<CertEntry> certs;
    std::vector<uint32_t> pcs;
  };

  Session* FindSession(uint32_t id);
  Peer* FindPeer(uint32_t id);
  void CloseSession(uint32_t id);
  bool EnsureRuntime();

  ipc::ByteStream& stream_;
  const Options opts_;
  std::unique_ptr<Runtime> rt_;
  std::unique_ptr<DeviceWatcher> watcher_;
  uint32_t next_handle_ = 1;
  std::map<uint32_t, Session> sessions_;
  std::map<uint32_t, std::unique_ptr<Peer>> peers_;  // kept (closed) until Shutdown: observers must outlive the pc

  std::mutex emit_mu_;
  bool closing_ = false;
  bool broken_ = false;
};

}  // namespace qmedia::engine
