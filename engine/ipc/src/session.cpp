#include "qmedia/ipc/session.h"

#include "qmedia/ipc/message.h"
#include "qmedia/ipc/secure.h"

namespace qmedia::ipc {

HandlerAction StubHandler::Handle(const ValidatedMessage& msg, Outbox& out) {
  if (msg.spec->kind == "ping") {
    out.Push(BuildPong(msg.id));
    return HandlerAction::Continue;
  }
  if (msg.spec->kind == "shutdown") {
    out.Push(BuildOk(msg.id));
    return HandlerAction::Shutdown;
  }
  out.Push(BuildErr(msg.id, errcode::kUnsupported, "engine_core_not_linked"));
  return HandlerAction::Continue;
}

namespace {

struct WipeGuard {
  FrameBuffer& buf;
  ~WipeGuard() { buf.Wipe(); }
};

// Decodes and validates the frame currently in buf as a client message.
Err ParseClientMessage(const FrameBuffer& buf, cbor::Value* root, ValidatedMessage* msg) {
  Err e = cbor::Decode(buf.view(), root);
  if (e != Err::Ok) return e;
  return ValidateMessage(*root, Dir::ClientToEngine, msg);
}

ServeResult Violation(ByteStream& stream, Err why) {
  // Best effort; the connection is about to be closed anyway.
  const cbor::Buf reply = BuildErr(0, errcode::kBadRequest, ErrName(why));
  (void)WriteFrame(stream, reply, 1000);
  return ServeResult::ProtocolViolation;
}

}  // namespace

ServeResult Serve(ByteStream& stream, FrameBuffer& buf, std::span<const uint8_t> nonce,
                  uint32_t hello_timeout_ms, MessageHandler& handler) {
  WipeGuard guard{buf};

  // ---- Hello: any failure is silent. ----------------------------------------------------
  if (nonce.size() != kNonceBytes) return ServeResult::HandshakeFailed;
  {
    Err e = ReadFrame(stream, buf, hello_timeout_ms, kMaxHelloFramePayload);
    if (e != Err::Ok) return ServeResult::HandshakeFailed;
    cbor::Value root;
    ValidatedMessage msg;
    e = ParseClientMessage(buf, &root, &msg);
    if (e != Err::Ok || msg.spec->kind != "hello") return ServeResult::HandshakeFailed;
    const cbor::Value* n = Field(msg, "nonce");  // validated: bytes, exactly kNonceBytes
    const bool match = n != nullptr && n->raw.size() == kNonceBytes &&
                       ConstantTimeEqual(n->raw.data(), nonce.data(), kNonceBytes);
    const uint32_t hello_id = msg.id;
    buf.Wipe();
    if (!match) return ServeResult::HandshakeFailed;
    const cbor::Buf ok = BuildHelloOk(hello_id);
    if (WriteFrame(stream, ok, 5000) != Err::Ok) return ServeResult::IoError;
  }

  // ---- Message loop. ----------------------------------------------------------------------
  for (;;) {
    Err e = ReadFrame(stream, buf, kNoTimeout);
    if (e == Err::IoEof) return ServeResult::PeerClosed;
    if (e == Err::FrameEmpty || e == Err::FrameTooLarge) return Violation(stream, e);
    if (e != Err::Ok) return ServeResult::IoError;

    Outbox out;
    HandlerAction action = HandlerAction::Continue;
    {
      cbor::Value root;
      ValidatedMessage msg;
      e = ParseClientMessage(buf, &root, &msg);
      if (e != Err::Ok) {
        buf.Wipe();
        return Violation(stream, e);
      }
      if (msg.spec->kind == "hello") {
        buf.Wipe();
        return Violation(stream, Err::SessionHelloRepeat);
      }
      action = handler.Handle(msg, out);
    }
    buf.Wipe();

    for (const cbor::Buf& reply : out.items()) {
      if (WriteFrame(stream, reply, 5000) != Err::Ok) return ServeResult::IoError;
    }
    if (action == HandlerAction::Shutdown) return ServeResult::Shutdown;
  }
}

}  // namespace qmedia::ipc
