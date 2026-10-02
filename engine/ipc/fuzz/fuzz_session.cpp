// libFuzzer target: framing + authenticated hello + message loop, over an in-memory stream.
// The input is the raw byte stream a client would send. The session nonce is fixed (bytes 0x00 to
// 0x1F) so mutated seeds that start with a valid hello reach the message loop.
// Properties checked on every input:
//  - Serve never crashes, hangs or reads beyond the input;
//  - after Serve returns, the receive buffer holds no non-zero byte (key wipe);
//  - a failed handshake sends nothing;
//  - every reply the engine wrote is a well-formed, schema-valid engine message in a valid frame.
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "qmedia/ipc/cbor.h"
#include "qmedia/ipc/frame.h"
#include "qmedia/ipc/limits.h"
#include "qmedia/ipc/schema.h"
#include "qmedia/ipc/session.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  using namespace qmedia::ipc;
  uint8_t nonce[kNonceBytes];
  for (size_t i = 0; i < sizeof nonce; ++i) nonce[i] = static_cast<uint8_t>(i);

  MemoryStream stream(std::span<const uint8_t>(data, size));
  FrameBuffer buf;
  StubHandler handler;
  const ServeResult r = Serve(stream, buf, std::span<const uint8_t>(nonce, sizeof nonce), 1000, handler);

  for (size_t i = 0; i < buf.capacity(); ++i) {
    if (buf.raw()[i] != 0) std::abort();  // key material left behind
  }
  const std::vector<uint8_t>& out = stream.written();
  if (r == ServeResult::HandshakeFailed && !out.empty()) std::abort();

  size_t pos = 0;
  while (pos < out.size()) {
    if (out.size() - pos < kFrameHeaderBytes) std::abort();
    const size_t len = (static_cast<size_t>(out[pos]) << 24) | (static_cast<size_t>(out[pos + 1]) << 16) |
                       (static_cast<size_t>(out[pos + 2]) << 8) | static_cast<size_t>(out[pos + 3]);
    pos += kFrameHeaderBytes;
    if (len == 0 || len > kMaxFramePayload || out.size() - pos < len) std::abort();
    cbor::Value v;
    if (cbor::Decode(std::span<const uint8_t>(out.data() + pos, len), &v) != Err::Ok) std::abort();
    ValidatedMessage m;
    if (ValidateMessage(v, Dir::EngineToClient, &m) != Err::Ok) std::abort();
    pos += len;
  }
  return 0;
}
