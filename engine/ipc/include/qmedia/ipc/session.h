// Server side of one IPC connection: nonce-authenticated hello, then a validated message loop.
//
// Fail-closed rules:
//  - The first frame must be a valid "hello" carrying the session nonce (constant-time compare).
//    Any failure before that point closes the connection WITHOUT sending a single byte.
//  - After the hello, any framing, CBOR or schema violation ends the session (best-effort "err"
//    frame with a static detail, then ProtocolViolation). Semantic errors (unknown handle, wrong
//    state) are the handler's business: it answers "err" and the session continues.
//  - The receive buffer is wiped after every message and when the session ends, so key material
//    from "install_key" does not linger.
#pragma once

#include <cstdint>
#include <span>
#include <utility>
#include <vector>

#include "qmedia/ipc/cbor.h"
#include "qmedia/ipc/frame.h"
#include "qmedia/ipc/schema.h"

namespace qmedia::ipc {

enum class ServeResult : uint8_t {
  Shutdown,           // the client sent "shutdown" and got its "ok"
  PeerClosed,         // the client closed the connection between messages
  HandshakeFailed,    // wrong or missing hello / nonce; nothing was sent
  ProtocolViolation,  // malformed or out-of-contract message after the hello
  IoError,
};

// Replies produced while handling one message. Written in order after Handle returns.
class Outbox {
 public:
  void Push(cbor::Buf payload) { items_.push_back(std::move(payload)); }
  const std::vector<cbor::Buf>& items() const { return items_; }

 private:
  std::vector<cbor::Buf> items_;
};

enum class HandlerAction : uint8_t { Continue, Shutdown };

class MessageHandler {
 public:
  virtual ~MessageHandler() = default;
  // msg and everything it points to is valid only for the duration of the call.
  virtual HandlerAction Handle(const ValidatedMessage& msg, Outbox& out) = 0;
};

// Handler used until the engine core is linked: ping -> pong, shutdown -> ok, anything else
// -> err "unsupported".
class StubHandler final : public MessageHandler {
 public:
  HandlerAction Handle(const ValidatedMessage& msg, Outbox& out) override;
};

// Runs one connection. `buf` is supplied by the caller (it is wiped on every exit path).
ServeResult Serve(ByteStream& stream, FrameBuffer& buf, std::span<const uint8_t> nonce,
                  uint32_t hello_timeout_ms, MessageHandler& handler);

}  // namespace qmedia::ipc
