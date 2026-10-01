#include <cstring>
#include <initializer_list>
#include <string>

#include "qmedia/ipc/cbor.h"
#include "qmedia/ipc/limits.h"
#include "testing.h"

using namespace qmedia::ipc;
using cbor::Buf;
using cbor::Value;

namespace {

Buf Bytes(std::initializer_list<int> v) {
  Buf b;
  for (int x : v) b.push_back(static_cast<uint8_t>(x));
  return b;
}

Err Dec(const Buf& b, Value* v = nullptr) {
  Value tmp;
  return cbor::Decode(b, v != nullptr ? v : &tmp);
}

// A valid input must decode and re-encode to the same bytes.
void ExpectCanonical(const Buf& b) {
  Value v;
  CHECK_EQ(Dec(b, &v), Err::Ok);
  Buf again;
  cbor::Encode(v, &again);
  CHECK(again == b);
}

}  // namespace

QTEST(cbor_unsigned_integers_shortest_form) {
  ExpectCanonical(Bytes({0x00}));
  ExpectCanonical(Bytes({0x17}));
  ExpectCanonical(Bytes({0x18, 0x18}));
  ExpectCanonical(Bytes({0x18, 0xFF}));
  ExpectCanonical(Bytes({0x19, 0x01, 0x00}));
  ExpectCanonical(Bytes({0x19, 0xFF, 0xFF}));
  ExpectCanonical(Bytes({0x1A, 0x00, 0x01, 0x00, 0x00}));
  ExpectCanonical(Bytes({0x1B, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00}));
  ExpectCanonical(Bytes({0x1B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF}));
  Value v;
  const Buf max32 = Bytes({0x1A, 0xFF, 0xFF, 0xFF, 0xFF});
  CHECK_EQ(Dec(max32, &v), Err::Ok);
  CHECK_EQ(v.u, 0xFFFFFFFFull);
}

QTEST(cbor_rejects_non_shortest_integers_and_lengths) {
  CHECK_EQ(Dec(Bytes({0x18, 0x00})), Err::CborNonShortest);
  CHECK_EQ(Dec(Bytes({0x18, 0x17})), Err::CborNonShortest);
  CHECK_EQ(Dec(Bytes({0x19, 0x00, 0xFF})), Err::CborNonShortest);
  CHECK_EQ(Dec(Bytes({0x1A, 0x00, 0x00, 0xFF, 0xFF})), Err::CborNonShortest);
  CHECK_EQ(Dec(Bytes({0x1B, 0x00, 0x00, 0x00, 0x00, 0xFF, 0xFF, 0xFF, 0xFF})), Err::CborNonShortest);
  // text string of length 1 written with a one-byte length argument
  CHECK_EQ(Dec(Bytes({0x78, 0x01, 'a'})), Err::CborNonShortest);
  // array of 0 elements written with a one-byte length argument
  CHECK_EQ(Dec(Bytes({0x98, 0x00})), Err::CborNonShortest);
}

QTEST(cbor_rejects_unsupported_major_types_and_simple_values) {
  CHECK_EQ(Dec(Bytes({0x20})), Err::CborUnsupported);              // -1
  CHECK_EQ(Dec(Bytes({0x38, 0x63})), Err::CborUnsupported);        // -100
  CHECK_EQ(Dec(Bytes({0xC0, 0x00})), Err::CborUnsupported);        // tag 0
  CHECK_EQ(Dec(Bytes({0xD8, 0x18, 0x00})), Err::CborUnsupported);  // tag 24
  CHECK_EQ(Dec(Bytes({0xF9, 0x3C, 0x00})), Err::CborUnsupported);  // float16 1.0
  CHECK_EQ(Dec(Bytes({0xFA, 0x3F, 0x80, 0x00, 0x00})), Err::CborUnsupported);  // float32
  CHECK_EQ(Dec(Bytes({0xFB, 0x3F, 0xF0, 0, 0, 0, 0, 0, 0})), Err::CborUnsupported);  // float64
  CHECK_EQ(Dec(Bytes({0xF7})), Err::CborUnsupported);              // undefined
  CHECK_EQ(Dec(Bytes({0xE0})), Err::CborUnsupported);              // simple(0)
  CHECK_EQ(Dec(Bytes({0xF8, 0x20})), Err::CborUnsupported);        // simple(32)
  CHECK_EQ(Dec(Bytes({0xFF})), Err::CborUnsupported);              // stray break
  CHECK_EQ(Dec(Bytes({0x1C})), Err::CborUnsupported);              // reserved additional info
}

QTEST(cbor_rejects_indefinite_lengths) {
  CHECK_EQ(Dec(Bytes({0x9F, 0x01, 0xFF})), Err::CborUnsupported);              // array
  CHECK_EQ(Dec(Bytes({0xBF, 0x61, 'a', 0x01, 0xFF})), Err::CborUnsupported);  // map
  CHECK_EQ(Dec(Bytes({0x5F, 0x41, 0x01, 0xFF})), Err::CborUnsupported);       // byte string
  CHECK_EQ(Dec(Bytes({0x7F, 0x61, 'a', 0xFF})), Err::CborUnsupported);        // text string
}

QTEST(cbor_strings_and_simple_values) {
  // The decoded strings are views, so every input must stay alive while its Value is inspected.
  Value v;
  const Buf empty_bin = Bytes({0x40});
  CHECK_EQ(Dec(empty_bin, &v), Err::Ok);
  CHECK(v.type == Value::Type::Bytes && v.raw.empty());
  const Buf bin3 = Bytes({0x43, 1, 2, 3});
  CHECK_EQ(Dec(bin3, &v), Err::Ok);
  CHECK(v.type == Value::Type::Bytes && v.raw.size() == 3);
  const Buf txt = Bytes({0x63, 'a', 'b', 'c'});
  CHECK_EQ(Dec(txt, &v), Err::Ok);
  CHECK(v.type == Value::Type::Text && v.text() == "abc");
  ExpectCanonical(Bytes({0xF4}));
  ExpectCanonical(Bytes({0xF5}));
  ExpectCanonical(Bytes({0xF6}));
  Buf big = Bytes({0x58, 0x18});  // 24 bytes: first length that needs the extra byte
  big.resize(2 + 24, 0x41);
  CHECK_EQ(Dec(big, &v), Err::Ok);
  ExpectCanonical(big);
}

QTEST(cbor_utf8_validation) {
  CHECK_EQ(Dec(Bytes({0x62, 0xC3, 0xA9})), Err::Ok);              // U+00E9
  CHECK_EQ(Dec(Bytes({0x63, 0xE2, 0x82, 0xAC})), Err::Ok);        // U+20AC
  CHECK_EQ(Dec(Bytes({0x64, 0xF0, 0x9F, 0x98, 0x80})), Err::Ok);  // U+1F600
  CHECK_EQ(Dec(Bytes({0x61, 0x80})), Err::CborBadUtf8);           // lone continuation byte
  CHECK_EQ(Dec(Bytes({0x62, 0xC0, 0x80})), Err::CborBadUtf8);     // overlong NUL
  CHECK_EQ(Dec(Bytes({0x62, 0xC1, 0xBF})), Err::CborBadUtf8);     // overlong
  CHECK_EQ(Dec(Bytes({0x63, 0xE0, 0x80, 0x80})), Err::CborBadUtf8);        // overlong 3-byte
  CHECK_EQ(Dec(Bytes({0x63, 0xED, 0xA0, 0x80})), Err::CborBadUtf8);        // surrogate U+D800
  CHECK_EQ(Dec(Bytes({0x64, 0xF4, 0x90, 0x80, 0x80})), Err::CborBadUtf8);  // above U+10FFFF
  CHECK_EQ(Dec(Bytes({0x64, 0xF0, 0x80, 0x80, 0x80})), Err::CborBadUtf8);  // overlong 4-byte
  CHECK_EQ(Dec(Bytes({0x62, 0xC3})), Err::CborTruncated);  // string cut short by the input
  CHECK_EQ(Dec(Bytes({0x62, 0xC3, 0x28})), Err::CborBadUtf8);  // bad continuation byte
  CHECK_EQ(Dec(Bytes({0x61, 0xFF})), Err::CborBadUtf8);
  // Raw bytes are not checked in a byte string.
  CHECK_EQ(Dec(Bytes({0x41, 0xFF})), Err::Ok);
}

QTEST(cbor_truncated_and_trailing) {
  CHECK_EQ(Dec(Buf{}), Err::CborTruncated);
  CHECK_EQ(Dec(Bytes({0x18})), Err::CborTruncated);
  CHECK_EQ(Dec(Bytes({0x19, 0x01})), Err::CborTruncated);
  CHECK_EQ(Dec(Bytes({0x43, 1, 2})), Err::CborTruncated);
  CHECK_EQ(Dec(Bytes({0x82, 0x01})), Err::CborTruncated);
  CHECK_EQ(Dec(Bytes({0xA1, 0x61, 'a'})), Err::CborTruncated);
  CHECK_EQ(Dec(Bytes({0x00, 0x00})), Err::CborTrailing);
  CHECK_EQ(Dec(Bytes({0xF6, 0x00})), Err::CborTrailing);
}

QTEST(cbor_huge_length_claims_do_not_allocate) {
  // Array, map and string lengths far beyond the input: rejected from the header alone.
  CHECK_EQ(Dec(Bytes({0x9B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})), Err::CborTruncated);
  CHECK_EQ(Dec(Bytes({0xBB, 0x7F, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})), Err::CborTruncated);
  CHECK_EQ(Dec(Bytes({0x9A, 0x00, 0xFF, 0xFF, 0xFF})), Err::CborTruncated);
  CHECK_EQ(Dec(Bytes({0x5B, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF})), Err::CborTruncated);
  CHECK_EQ(Dec(Bytes({0x7A, 0xFF, 0xFF, 0xFF, 0xFF})), Err::CborTruncated);
}

QTEST(cbor_map_key_rules) {
  // sorted, text keys
  ExpectCanonical(Bytes({0xA2, 0x61, 'a', 0x01, 0x61, 'b', 0x02}));
  // shorter key sorts before longer key (encoded-byte order)
  ExpectCanonical(Bytes({0xA2, 0x61, 'z', 0x01, 0x62, 'a', 'a', 0x02}));
  CHECK_EQ(Dec(Bytes({0xA2, 0x62, 'a', 'a', 0x01, 0x61, 'z', 0x02})), Err::CborKeyOrder);
  // unsorted
  CHECK_EQ(Dec(Bytes({0xA2, 0x61, 'b', 0x01, 0x61, 'a', 0x02})), Err::CborKeyOrder);
  // duplicate
  CHECK_EQ(Dec(Bytes({0xA2, 0x61, 'a', 0x01, 0x61, 'a', 0x02})), Err::CborKeyOrder);
  // integer key, byte-string key, array key
  CHECK_EQ(Dec(Bytes({0xA1, 0x01, 0x01})), Err::CborKeyType);
  CHECK_EQ(Dec(Bytes({0xA1, 0x41, 'a', 0x01})), Err::CborKeyType);
  CHECK_EQ(Dec(Bytes({0xA1, 0x80, 0x01})), Err::CborKeyType);
  // key longer than kMaxKeyBytes
  Buf longkey = Bytes({0xA1, 0x78, static_cast<int>(kMaxKeyBytes + 1)});
  longkey.resize(longkey.size() + kMaxKeyBytes + 1, 'k');
  longkey.push_back(0x01);
  CHECK_EQ(Dec(longkey), Err::CborKeyType);
  // key of exactly kMaxKeyBytes is fine
  Buf maxkey = Bytes({0xA1, 0x78, static_cast<int>(kMaxKeyBytes)});
  maxkey.resize(maxkey.size() + kMaxKeyBytes, 'k');
  maxkey.push_back(0x01);
  CHECK_EQ(Dec(maxkey), Err::Ok);
  // a map key must not hide behind a tag or a non-text type
  CHECK_EQ(Dec(Bytes({0xA1, 0xC0, 0x61, 'a', 0x01})), Err::CborKeyType);
}

// The bounds are part of the contract (schema.cddl, README). Changing one is a conscious edit.
static_assert(kMaxDepth == 6);
static_assert(kMaxNodes == 4096);
static_assert(kMaxKeyBytes == 64);
static_assert(kMaxFramePayload == 1048576);
static_assert(kNonceBytes == 32 && kKeyBytes == 32 && kFingerprintBytes == 32);

QTEST(cbor_depth_and_node_limits) {
  // kMaxDepth levels of nesting below the root are accepted, one more is not.
  Buf ok;
  for (int i = 0; i < kMaxDepth; ++i) ok.push_back(0x81);
  ok.push_back(0x00);
  CHECK_EQ(Dec(ok), Err::Ok);
  Buf deep;
  for (int i = 0; i <= kMaxDepth + 1; ++i) deep.push_back(0x81);
  deep.push_back(0x00);
  CHECK_EQ(Dec(deep), Err::CborTooDeep);
  // A very deep input must not overflow the stack: the limit applies long before.
  Buf evil(100000, 0x81);
  evil.push_back(0x00);
  CHECK_EQ(Dec(evil), Err::CborTooDeep);

  // kMaxNodes data items in total.
  auto array_of = [](size_t n) {
    Buf b;
    cbor::PutHead(b, 4, n);
    b.resize(b.size() + n, 0x00);
    return b;
  };
  CHECK_EQ(Dec(array_of(kMaxNodes - 1)), Err::Ok);  // root + (kMaxNodes - 1) elements
  CHECK_EQ(Dec(array_of(kMaxNodes)), Err::CborTooLarge);
}

QTEST(cbor_builders_are_canonical) {
  const Buf m = cbor::MapBuilder()
                    .Uint("zz", 1)
                    .Str("a", "x")
                    .Flag("mm", false)
                    .Bin("b", Bytes({1, 2}))
                    .Finish();
  Value v;
  CHECK_EQ(Dec(m, &v), Err::Ok);  // sorted by the builder, so the decoder accepts it
  ExpectCanonical(m);
  CHECK(cbor::MapGet(v, "zz") != nullptr && cbor::MapGet(v, "zz")->u == 1);
  CHECK(cbor::MapGet(v, "nope") == nullptr);
  const Buf a = cbor::ArrayBuilder().Add(Bytes({0x01})).Add(Bytes({0xF5})).Finish();
  ExpectCanonical(a);
}

QTEST(cbor_decoded_strings_are_views_into_the_input) {
  const Buf in = Bytes({0xA1, 0x61, 'k', 0x43, 7, 8, 9});
  Value v;
  CHECK_EQ(Dec(in, &v), Err::Ok);
  const Value* val = cbor::MapGet(v, "k");
  CHECK(val != nullptr);
  CHECK(val != nullptr && val->raw.data() >= in.data() &&
        val->raw.data() + val->raw.size() <= in.data() + in.size());
}
