// Hard limits of the IPC protocol. Every bound the decoder and validator use is here,
// so a reviewer can audit the whole attack surface in one place.
#pragma once

#include <cstddef>
#include <cstdint>

namespace qmedia::ipc {

inline constexpr uint32_t kProtocolVersion = 1;

// Framing: 4-byte big-endian length, then that many payload bytes.
inline constexpr size_t kFrameHeaderBytes = 4;
inline constexpr size_t kMaxFramePayload = 1u << 20;  // 1 MiB. Video never goes through IPC.
// Before the nonce has been checked the peer is unauthenticated: its first frame may be this big at
// most (a hello is about 70 bytes), so it cannot make the engine allocate or decode more.
inline constexpr size_t kMaxHelloFramePayload = 256;

// CBOR decoder bounds.
inline constexpr int kMaxDepth = 6;          // nesting levels below the root map
inline constexpr size_t kMaxNodes = 4096;    // data items in one message
inline constexpr size_t kMaxKeyBytes = 64;   // map keys are short text strings

// Scalar maps in "stats" values: short text keys, scalar values.
inline constexpr size_t kMaxScalarKeyBytes = 48;
inline constexpr size_t kMaxScalarTextBytes = 128;

// Fixed-size secrets and digests.
inline constexpr size_t kNonceBytes = 32;
inline constexpr size_t kKeyBytes = 32;
inline constexpr size_t kFingerprintBytes = 32;

// Timeouts (milliseconds).
inline constexpr uint32_t kNoTimeout = 0xFFFFFFFFu;
inline constexpr uint32_t kHelloTimeoutMs = 5000;
inline constexpr uint32_t kConnectTimeoutMs = 15000;
inline constexpr uint32_t kNonceReadTimeoutMs = 5000;
// Once the first byte of a frame has arrived the rest of it must follow within this time.
inline constexpr uint32_t kFrameBodyTimeoutMs = 10000;

}  // namespace qmedia::ipc
