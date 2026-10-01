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

QTEST(frame_failed_read_wipes_the_partial_payload) {
  FrameBuffer buf;
  Buf wire = Header(32);
  wire.resize(4 + 16, 0xD7);  // half of the payload, then the stream ends
  MemoryStream in(wire);
  CHECK_EQ(ReadFrame(in, buf, 1000), Err::IoError);
  CHECK(buf.capacity() == 0 || AllZero(buf.raw(), buf.capacity()));
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
