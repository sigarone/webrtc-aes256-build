#include <cstring>
#include <vector>

#include "qmedia/ipc/frame.h"
#include "qmedia/ipc/limits.h"
#include "qmedia/ipc/secure.h"
#include "testing.h"

using namespace qmedia::ipc;
using cbor::Buf;

namespace {

Buf Header(uint32_t len) {
  return Buf{static_cast<uint8_t>(len >> 24), static_cast<uint8_t>(len >> 16),
             static_cast<uint8_t>(len >> 8), static_cast<uint8_t>(len)};
}

bool AllZero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; ++i) {
    if (p[i] != 0) return false;
  }
  return true;
}

}  // namespace

QTEST(frame_round_trip) {
  const Buf payload{1, 2, 3, 4, 5};
  MemoryStream out({});
  CHECK_EQ(WriteFrame(out, payload, 1000), Err::Ok);
  const Buf wire = out.written();
  CHECK_EQ(wire.size(), 4u + 5u);
  CHECK(wire[0] == 0 && wire[1] == 0 && wire[2] == 0 && wire[3] == 5);  // big endian

  MemoryStream in(wire);
  FrameBuffer buf;
  CHECK_EQ(ReadFrame(in, buf, 1000), Err::Ok);
  CHECK(buf.view().size() == 5 && std::memcmp(buf.view().data(), payload.data(), 5) == 0);
  CHECK(EncodeFrame(payload) == wire);
}

QTEST(frame_header_is_big_endian) {
  Buf wire = Header(0x00000102);
  wire.resize(4 + 0x102, 0xAB);
  MemoryStream in(wire);
  FrameBuffer buf;
  CHECK_EQ(ReadFrame(in, buf, 1000), Err::Ok);
  CHECK_EQ(buf.view().size(), 0x102u);
}

QTEST(frame_rejects_empty_and_oversized_from_the_header_alone) {
  {
    const Buf wire = Header(0);
    MemoryStream in(wire);
    FrameBuffer buf;
    CHECK_EQ(ReadFrame(in, buf, 1000), Err::FrameEmpty);
  }
  for (uint32_t len : {static_cast<uint32_t>(kMaxFramePayload + 1), 0x7FFFFFFFu, 0xFFFFFFFFu}) {
    Buf wire = Header(len);
    wire.resize(4 + 64, 0x11);  // a little payload that must never be consumed
    MemoryStream in(wire);
    FrameBuffer buf;
    CHECK_EQ(ReadFrame(in, buf, 1000), Err::FrameTooLarge);
    CHECK_EQ(in.consumed(), 4u);  // the payload was not read
    CHECK_EQ(buf.capacity(), 0u);  // and nothing was allocated for it
  }
}

QTEST(frame_honours_a_smaller_caller_limit) {
  // The limit is checked from the header alone and never exceeds the protocol maximum.
  Buf wire = Header(kMaxHelloFramePayload + 1);
  wire.resize(4 + kMaxHelloFramePayload + 1, 0x33);
  {
    MemoryStream in(wire);
    FrameBuffer buf;
    CHECK_EQ(ReadFrame(in, buf, 1000, kMaxHelloFramePayload), Err::FrameTooLarge);
    CHECK_EQ(in.consumed(), 4u);
    CHECK_EQ(buf.capacity(), 0u);
  }
  {
    Buf ok = Header(kMaxHelloFramePayload);
    ok.resize(4 + kMaxHelloFramePayload, 0x33);
    MemoryStream in(ok);
    FrameBuffer buf;
    CHECK_EQ(ReadFrame(in, buf, 1000, kMaxHelloFramePayload), Err::Ok);
    CHECK_EQ(buf.view().size(), kMaxHelloFramePayload);
  }
  {
    // A caller cannot raise the limit above the protocol maximum.
    Buf big = Header(static_cast<uint32_t>(kMaxFramePayload + 1));
    big.resize(4 + 16, 0x44);
    MemoryStream in(big);
    FrameBuffer buf;
    CHECK_EQ(ReadFrame(in, buf, 1000, kMaxFramePayload * 8), Err::FrameTooLarge);
  }
}

QTEST(frame_accepts_exactly_the_maximum) {
  Buf wire = Header(static_cast<uint32_t>(kMaxFramePayload));
  wire.resize(4 + kMaxFramePayload, 0x22);
  MemoryStream in(wire);
  FrameBuffer buf;
  CHECK_EQ(ReadFrame(in, buf, 1000), Err::Ok);
  CHECK_EQ(buf.view().size(), kMaxFramePayload);
}

QTEST(frame_eof_and_truncation) {
  FrameBuffer buf;
  {
    MemoryStream in({});
    CHECK_EQ(ReadFrame(in, buf, 1000), Err::IoEof);  // clean end between frames
  }
  {
    const Buf wire{0, 0};
    MemoryStream in(wire);
    CHECK_EQ(ReadFrame(in, buf, 1000), Err::IoError);  // ended inside the header
  }
  {
    Buf wire = Header(10);
    wire.push_back(1);
    wire.push_back(2);
    MemoryStream in(wire);
    CHECK_EQ(ReadFrame(in, buf, 1000), Err::IoError);  // ended inside the payload
    CHECK_EQ(buf.view().size(), 0u);
  }
  {
    const Buf wire = Header(10);
    MemoryStream in(wire);
    CHECK_EQ(ReadFrame(in, buf, 1000), Err::IoTruncated);  // header, then nothing
  }
}

// Several threads write frames to one stream (events from libwebrtc threads, replies from the
// session loop). A stream with an atomic WriteAll can only keep frames whole if a frame is ONE call.
class CountingStream final : public ByteStream {
 public:
  IoResult ReadExact(uint8_t*, size_t, uint32_t) override { return IoResult::Eof; }
  IoResult WriteAll(const uint8_t* src, size_t n, uint32_t) override {
    ++calls;
    last.assign(src, src + n);
    return IoResult::Ok;
  }
  int calls = 0;
  Buf last;
};

QTEST(frame_write_is_one_write_call) {
  CountingStream out;
  const Buf payload{9, 8, 7};
  CHECK_EQ(WriteFrame(out, payload, 1000), Err::Ok);
  CHECK_EQ(out.calls, 1);
  CHECK(out.last == EncodeFrame(payload));
}

QTEST(frame_write_validates_size) {
  MemoryStream out({});
  CHECK_EQ(WriteFrame(out, std::span<const uint8_t>(), 1000), Err::FrameEmpty);
  const std::vector<uint8_t> big(kMaxFramePayload + 1, 0);
  CHECK_EQ(WriteFrame(out, big, 1000), Err::FrameTooLarge);
  CHECK(out.written().empty());
  const std::vector<uint8_t> max(kMaxFramePayload, 0);
  CHECK_EQ(WriteFrame(out, max, 1000), Err::Ok);
}

QTEST(frame_buffer_wipe_zeroes_everything_that_was_received) {
  FrameBuffer buf;
  Buf wire = Header(64);
  for (int i = 0; i < 64; ++i) wire.push_back(static_cast<uint8_t>(0xC0 + (i % 16)));
  MemoryStream in(wire);
  CHECK_EQ(ReadFrame(in, buf, 1000), Err::Ok);
  CHECK(!AllZero(buf.raw(), 64));
  buf.Wipe();
  CHECK(AllZero(buf.raw(), buf.capacity()));
  CHECK_EQ(buf.view().size(), 0u);

  // A smaller second frame must not leave the tail of the first one behind after a wipe.
  Buf wire2 = Header(8);
  wire2.resize(4 + 8, 0xEE);
  MemoryStream in2(wire2);
  CHECK_EQ(ReadFrame(in2, buf, 1000), Err::Ok);
  buf.Wipe();
  CHECK(AllZero(buf.raw(), buf.capacity()));
}

QTEST(memory_stream_delivers_the_bytes_that_arrived_before_failing) {
  // A pipe read that ends early has already written what it received into the destination. The
  // in-memory stream must do the same, or the wipe-on-failure paths are never exercised by the
  // unit tests and the session fuzzer.
  const Buf wire{0x11, 0x22, 0x33};
  MemoryStream in(wire);
  uint8_t dst[8] = {0};
  CHECK(in.ReadExact(dst, sizeof dst, 1000) == IoResult::Error);
  CHECK(dst[0] == 0x11 && dst[1] == 0x22 && dst[2] == 0x33);
  CHECK(AllZero(dst + 3, sizeof dst - 3));
  CHECK_EQ(in.consumed(), wire.size());
  CHECK(in.ReadExact(dst, 1, 1000) == IoResult::Eof);
}

QTEST(frame_failed_read_wipes_the_partial_payload) {
  FrameBuffer buf;
  Buf wire = Header(32);
  wire.resize(4 + 16, 0xD7);  // half of the payload, then the stream ends
  MemoryStream in(wire);
  CHECK_EQ(ReadFrame(in, buf, 1000), Err::IoError);
  CHECK_EQ(buf.capacity(), 32u);  // the payload buffer existed and received 16 bytes
  CHECK(AllZero(buf.raw(), buf.capacity()));
  CHECK_EQ(buf.view().size(), 0u);
}

QTEST(secure_primitives) {
  uint8_t a[32], b[32];
  std::memset(a, 0x42, sizeof a);
  std::memset(b, 0x42, sizeof b);
  CHECK(ConstantTimeEqual(a, b, sizeof a));
  b[31] ^= 1;
  CHECK(!ConstantTimeEqual(a, b, sizeof a));
  b[31] ^= 1;
  b[0] ^= 0x80;
  CHECK(!ConstantTimeEqual(a, b, sizeof a));
  SecureZero(a, sizeof a);
  CHECK(AllZero(a, sizeof a));
  uint8_t r1[32] = {0}, r2[32] = {0};
  CHECK(FillRandom(r1, sizeof r1));
  CHECK(FillRandom(r2, sizeof r2));
  CHECK(std::memcmp(r1, r2, sizeof r1) != 0);  // 2^-256 false failure
  CHECK(!AllZero(r1, sizeof r1));
}

namespace {

// Records the timeout of every read, so the idle/body split can be checked.
class RecordingStream final : public ByteStream {
 public:
  explicit RecordingStream(Buf data, IoResult fail_after_first = IoResult::Ok)
      : data_(std::move(data)), fail_(fail_after_first) {}
  IoResult ReadExact(uint8_t* dst, size_t n, uint32_t timeout_ms) override {
    timeouts.push_back(timeout_ms);
    if (timeouts.size() > 1 && fail_ != IoResult::Ok) return fail_;
    if (pos_ + n > data_.size()) return IoResult::Error;
    std::memcpy(dst, data_.data() + pos_, n);
    pos_ += n;
    return IoResult::Ok;
  }
  IoResult WriteAll(const uint8_t*, size_t, uint32_t) override { return IoResult::Ok; }
  std::vector<uint32_t> timeouts;

 private:
  Buf data_;
  size_t pos_ = 0;
  IoResult fail_;
};

}  // namespace

QTEST(frame_idle_timeout_applies_to_the_first_byte_only) {
  Buf wire = Header(3);
  wire.resize(7, 0x41);
  RecordingStream s(wire);
  FrameBuffer buf;
  CHECK_EQ(ReadFrame(s, buf, 1234), Err::Ok);
  CHECK_EQ(s.timeouts.size(), 3u);  // first byte, rest of the header, payload
  CHECK_EQ(s.timeouts[0], 1234u);   // the caller's idle timeout
  CHECK(s.timeouts[1] <= kFrameBodyTimeoutMs && s.timeouts[1] > 0);
  CHECK(s.timeouts[2] <= kFrameBodyTimeoutMs && s.timeouts[2] > 0);

  RecordingStream forever(wire);
  CHECK_EQ(ReadFrame(forever, buf, kNoTimeout), Err::Ok);
  CHECK_EQ(forever.timeouts[0], kNoTimeout);        // idle wait is unbounded when asked
  CHECK(forever.timeouts[1] <= kFrameBodyTimeoutMs);  // but a started frame never is
}

QTEST(frame_stall_inside_a_frame_times_out_and_wipes) {
  FrameBuffer buf;
  {
    RecordingStream s(Header(8), IoResult::Timeout);  // stalls after the first header byte
    CHECK_EQ(ReadFrame(s, buf, kNoTimeout), Err::IoTimeout);
  }
  {
    Buf wire = Header(8);
    wire.resize(4 + 8, 0x66);
    // First read (1 byte) works, the second (rest of header) times out.
    RecordingStream s(wire, IoResult::Timeout);
    CHECK_EQ(ReadFrame(s, buf, kNoTimeout), Err::IoTimeout);
    CHECK(buf.capacity() == 0 || AllZero(buf.raw(), buf.capacity()));
  }
}
