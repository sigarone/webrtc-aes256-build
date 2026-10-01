#include "qmedia/ipc/frame.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "qmedia/ipc/secure.h"

namespace qmedia::ipc {

namespace {

class Deadline {
 public:
  explicit Deadline(uint32_t timeout_ms)
      : infinite_(timeout_ms == kNoTimeout),
        end_(std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms)) {}

  uint32_t Remaining() const {
    if (infinite_) return kNoTimeout;
    const auto now = std::chrono::steady_clock::now();
    if (now >= end_) return 0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(end_ - now).count();
    return static_cast<uint32_t>(std::min<int64_t>(ms, 0xFFFFFFFEll));
  }

 private:
  bool infinite_;
  std::chrono::steady_clock::time_point end_;
};

Err MapIo(IoResult r) {
  switch (r) {
    case IoResult::Ok: return Err::Ok;
    case IoResult::Eof: return Err::IoEof;
    case IoResult::Timeout: return Err::IoTimeout;
    case IoResult::Error: return Err::IoError;
  }
  return Err::IoError;
}

}  // namespace

void FrameBuffer::Wipe() {
  if (data_ && high_water_ > 0) SecureZero(data_.get(), high_water_);
  high_water_ = 0;
  size_ = 0;
}

uint8_t* FrameBuffer::Prepare(size_t n) {
  if (n > capacity_) {
    data_.reset(new uint8_t[n]);  // the previous storage was wiped by the caller
    capacity_ = n;
  }
  high_water_ = n;
  return data_.get();
}

Err ReadFrame(ByteStream& s, FrameBuffer& buf, uint32_t idle_timeout_ms, size_t max_payload) {
  if (max_payload > kMaxFramePayload) max_payload = kMaxFramePayload;
  buf.Wipe();

  uint8_t hdr[kFrameHeaderBytes];
  IoResult r = s.ReadExact(hdr, 1, idle_timeout_ms);
  if (r != IoResult::Ok) return MapIo(r);
  const Deadline body(kFrameBodyTimeoutMs);
  r = s.ReadExact(hdr + 1, sizeof hdr - 1, body.Remaining());
  if (r != IoResult::Ok) return r == IoResult::Eof ? Err::IoError : MapIo(r);  // ended inside the header

  const uint32_t len = (static_cast<uint32_t>(hdr[0]) << 24) | (static_cast<uint32_t>(hdr[1]) << 16) |
                       (static_cast<uint32_t>(hdr[2]) << 8) | static_cast<uint32_t>(hdr[3]);
  if (len == 0) return Err::FrameEmpty;
  if (len > max_payload) return Err::FrameTooLarge;  // decided before reading the payload

  uint8_t* dst = buf.Prepare(len);
  r = s.ReadExact(dst, len, body.Remaining());
  if (r != IoResult::Ok) {
    buf.Wipe();
    return r == IoResult::Eof ? Err::IoTruncated : MapIo(r);
  }
  buf.size_ = len;
  return Err::Ok;
}

Err WriteFrame(ByteStream& s, std::span<const uint8_t> payload, uint32_t timeout_ms) {
  if (payload.empty()) return Err::FrameEmpty;
  if (payload.size() > kMaxFramePayload) return Err::FrameTooLarge;
  const Deadline deadline(timeout_ms);
  const uint32_t len = static_cast<uint32_t>(payload.size());
  const uint8_t hdr[kFrameHeaderBytes] = {static_cast<uint8_t>(len >> 24),
                                          static_cast<uint8_t>(len >> 16),
                                          static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)};
  IoResult r = s.WriteAll(hdr, sizeof hdr, deadline.Remaining());
  if (r != IoResult::Ok) return MapIo(r);
  r = s.WriteAll(payload.data(), payload.size(), deadline.Remaining());
  return MapIo(r);
}

cbor::Buf EncodeFrame(std::span<const uint8_t> payload) {
  const uint32_t len = static_cast<uint32_t>(payload.size());
  cbor::Buf out(kFrameHeaderBytes + payload.size());
  out[0] = static_cast<uint8_t>(len >> 24);
  out[1] = static_cast<uint8_t>(len >> 16);
  out[2] = static_cast<uint8_t>(len >> 8);
  out[3] = static_cast<uint8_t>(len);
  if (!payload.empty()) std::memcpy(out.data() + kFrameHeaderBytes, payload.data(), payload.size());
  return out;
}

IoResult MemoryStream::ReadExact(uint8_t* dst, size_t n, uint32_t) {
  if (n == 0) return IoResult::Ok;
  const size_t left = in_.size() - pos_;
  if (left == 0) return IoResult::Eof;
  if (left < n) {
    pos_ = in_.size();
    return IoResult::Error;
  }
  std::memcpy(dst, in_.data() + pos_, n);
  pos_ += n;
  return IoResult::Ok;
}

IoResult MemoryStream::WriteAll(const uint8_t* src, size_t n, uint32_t) {
  out_.insert(out_.end(), src, src + n);
  return IoResult::Ok;
}

}  // namespace qmedia::ipc
