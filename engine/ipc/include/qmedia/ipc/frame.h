// Length-prefixed framing: 4-byte big-endian payload length, then the payload.
// 1 <= length <= kMaxFramePayload. An oversized or empty length is rejected from the header alone;
// the payload is neither read nor allocated.
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

#include "qmedia/ipc/cbor.h"
#include "qmedia/ipc/limits.h"
#include "qmedia/ipc/status.h"

namespace qmedia::ipc {

enum class IoResult : uint8_t { Ok, Eof, Timeout, Error };

// Blocking byte stream. ReadExact fills exactly n bytes, or reports:
//   Eof     the stream ended before the first byte of this call,
//   Error   the stream ended or failed after some bytes were consumed,
//   Timeout the deadline passed (the stream must then be considered unusable).
class ByteStream {
 public:
  virtual ~ByteStream() = default;
  virtual IoResult ReadExact(uint8_t* dst, size_t n, uint32_t timeout_ms) = 0;
  virtual IoResult WriteAll(const uint8_t* src, size_t n, uint32_t timeout_ms) = 0;
};

// Receive buffer for one frame. Everything that was ever written into it is zeroed by Wipe(),
// which also runs in the destructor. Decoded Values point into this buffer.
class FrameBuffer {
 public:
  FrameBuffer() = default;
  ~FrameBuffer() { Wipe(); }
  FrameBuffer(const FrameBuffer&) = delete;
  FrameBuffer& operator=(const FrameBuffer&) = delete;

  std::span<const uint8_t> view() const { return std::span<const uint8_t>(data_.get(), size_); }
  size_t capacity() const { return capacity_; }
  // Raw access for tests that check the wipe.
  const uint8_t* raw() const { return data_.get(); }

  // Zeroes every byte that may hold message content. Safe to call at any time.
  void Wipe();

 private:
  friend Err ReadFrame(ByteStream&, FrameBuffer&, uint32_t, size_t);
  uint8_t* Prepare(size_t n);  // only valid right after Wipe()

  std::unique_ptr<uint8_t[]> data_;
  size_t capacity_ = 0;
  size_t high_water_ = 0;
  size_t size_ = 0;
};

// Reads one frame into buf (wiping it first). idle_timeout_ms bounds the wait for the first byte
// of the frame (kNoTimeout = wait forever). After that byte the rest of the frame must arrive
// within kFrameBodyTimeoutMs, so a peer that stalls in the middle of a frame cannot hold the
// reader forever. A length above max_payload (never above kMaxFramePayload) is FrameTooLarge,
// decided from the header alone.
Err ReadFrame(ByteStream& s, FrameBuffer& buf, uint32_t idle_timeout_ms,
              size_t max_payload = kMaxFramePayload);

// Writes header and payload. Fails with FrameEmpty / FrameTooLarge for an invalid size.
Err WriteFrame(ByteStream& s, std::span<const uint8_t> payload, uint32_t timeout_ms);

// Header plus payload as one buffer (tests, fixtures).
cbor::Buf EncodeFrame(std::span<const uint8_t> payload);

// In-memory stream for tests and fuzzing: reads from `in`, appends writes to `out`.
class MemoryStream final : public ByteStream {
 public:
  explicit MemoryStream(std::span<const uint8_t> in) : in_(in) {}
  IoResult ReadExact(uint8_t* dst, size_t n, uint32_t) override;
  IoResult WriteAll(const uint8_t* src, size_t n, uint32_t) override;

  size_t consumed() const { return pos_; }
  const std::vector<uint8_t>& written() const { return out_; }

 private:
  std::span<const uint8_t> in_;
  size_t pos_ = 0;
  std::vector<uint8_t> out_;
};

}  // namespace qmedia::ipc
