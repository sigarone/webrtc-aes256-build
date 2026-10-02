#include "qmedia/ipc/message.h"

#include "qmedia/ipc/limits.h"

namespace qmedia::ipc {

cbor::MapBuilder BeginMessage(std::string_view kind, uint32_t id) {
  cbor::MapBuilder b;
  b.Uint("v", kProtocolVersion).Str("t", kind).Uint("id", id);
  return b;
}

cbor::Buf BuildHello(uint32_t id, std::span<const uint8_t> nonce) {
  return BeginMessage("hello", id).Bin("nonce", nonce).Finish();
}

cbor::Buf BuildHelloOk(uint32_t id) {
  return BeginMessage("hello_ok", id)
      .Str("engine", kEngineIdent)
      .Uint("max_frame", kMaxFramePayload)
      .Finish();
}

cbor::Buf BuildPong(uint32_t id) { return BeginMessage("pong", id).Finish(); }

cbor::Buf BuildOk(uint32_t id) { return BeginMessage("ok", id).Finish(); }

cbor::Buf BuildErr(uint32_t id, std::string_view code, std::string_view detail) {
  cbor::MapBuilder b = BeginMessage("err", id);
  b.Str("code", code);
  if (!detail.empty()) b.Str("detail", detail);
  return b.Finish();
}

}  // namespace qmedia::ipc
