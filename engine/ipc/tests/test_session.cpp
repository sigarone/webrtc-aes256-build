#include <cstring>
#include <string>
#include <vector>

#include "gen_messages.h"
#include "qmedia/ipc/frame.h"
#include "qmedia/ipc/limits.h"
#include "qmedia/ipc/message.h"
#include "qmedia/ipc/session.h"
#include "testing.h"

using namespace qmedia::ipc;
using namespace qmedia::testgen;
using cbor::Buf;
using cbor::Value;

namespace {

Buf Nonce(uint8_t seed) {
  Buf n(kNonceBytes);
  for (size_t i = 0; i < n.size(); ++i) n[i] = static_cast<uint8_t>(seed + i);
  return n;
}

void Append(Buf& stream, const Buf& payload) {
  const Buf f = EncodeFrame(payload);
  stream.insert(stream.end(), f.begin(), f.end());
}

Buf SimpleRequest(std::string_view kind, uint32_t id) {
  return BeginMessage(kind, id).Finish();
}

// Splits the engine output into frames and returns the "t" of each message.
std::vector<std::string> Kinds(const Buf& out) {
  std::vector<std::string> kinds;
  size_t pos = 0;
  while (pos + 4 <= out.size()) {
    const size_t len = (static_cast<size_t>(out[pos]) << 24) | (static_cast<size_t>(out[pos + 1]) << 16) |
                       (static_cast<size_t>(out[pos + 2]) << 8) | out[pos + 3];
    pos += 4;
    if (pos + len > out.size()) break;
    Value v;
    if (cbor::Decode(std::span<const uint8_t>(out.data() + pos, len), &v) != Err::Ok) break;
    const Value* t = cbor::MapGet(v, "t");
    kinds.push_back(t != nullptr ? std::string(t->text()) : std::string("?"));
    pos += len;
  }
  return kinds;
}

struct Run {
  ServeResult result;
  Buf out;
  FrameBuffer buf;
};

// Runs Serve over an in-memory stream.
void RunServe(const Buf& stream_bytes, const Buf& nonce, Run* r, MessageHandler* handler = nullptr) {
  MemoryStream s(stream_bytes);
  StubHandler stub;
  r->result = qmedia::ipc::Serve(s, r->buf, nonce, 1000, handler != nullptr ? *handler : stub);
  r->out = s.written();
}

bool BufferIsClean(const FrameBuffer& b) {
  for (size_t i = 0; i < b.capacity(); ++i) {
    if (b.raw()[i] != 0) return false;
  }
  return true;
}

}  // namespace

QTEST(session_happy_path) {
  const Buf nonce = Nonce(1);
  Buf in;
  Append(in, BuildHello(1, nonce));
  Append(in, SimpleRequest("ping", 2));
  Append(in, SimpleRequest("ping", 3));
  Append(in, SimpleRequest("shutdown", 4));
  Run r;
  RunServe(in, nonce, &r);
  CHECK(r.result == ServeResult::Shutdown);
  const auto kinds = Kinds(r.out);
  CHECK((kinds == std::vector<std::string>{"hello_ok", "pong", "pong", "ok"}));
  CHECK(BufferIsClean(r.buf));
}

QTEST(session_peer_closes_between_messages) {
  const Buf nonce = Nonce(2);
  Buf in;
  Append(in, BuildHello(1, nonce));
  Append(in, SimpleRequest("ping", 2));
  Run r;
  RunServe(in, nonce, &r);
  CHECK(r.result == ServeResult::PeerClosed);
  CHECK((Kinds(r.out) == std::vector<std::string>{"hello_ok", "pong"}));
}

QTEST(session_wrong_nonce_is_silent) {
  const Buf nonce = Nonce(3);
  Buf wrong = nonce;
  wrong[kNonceBytes - 1] ^= 1;  // differs in the very last byte
  Buf in;
  Append(in, BuildHello(1, wrong));
  Append(in, SimpleRequest("ping", 2));
  Run r;
  RunServe(in, nonce, &r);
  CHECK(r.result == ServeResult::HandshakeFailed);
  CHECK(r.out.empty());  // not a single byte is sent to a peer without the nonce
  CHECK(BufferIsClean(r.buf));

  Buf wrong_first = nonce;
  wrong_first[0] ^= 0x80;
  Buf in2;
  Append(in2, BuildHello(1, wrong_first));
  Run r2;
  RunServe(in2, nonce, &r2);
  CHECK(r2.result == ServeResult::HandshakeFailed);
  CHECK(r2.out.empty());
}

QTEST(session_first_message_must_be_hello) {
  const Buf nonce = Nonce(4);
  Buf in;
  Append(in, SimpleRequest("ping", 1));
  Run r;
  RunServe(in, nonce, &r);
  CHECK(r.result == ServeResult::HandshakeFailed);
  CHECK(r.out.empty());

  // garbage, an empty frame, an oversized header, an engine-direction message, a hello with a
  // short nonce, and an empty stream: all silent failures
  const Buf bad_cbor{0xFF, 0xFF};
  Buf g;
  Append(g, bad_cbor);
  Run rg;
  RunServe(g, nonce, &rg);
  CHECK(rg.result == ServeResult::HandshakeFailed && rg.out.empty());

  const Buf empty_frame{0, 0, 0, 0};
  Run re;
  RunServe(empty_frame, nonce, &re);
  CHECK(re.result == ServeResult::HandshakeFailed && re.out.empty());

  const Buf huge{0xFF, 0xFF, 0xFF, 0xFF};
  Run rh;
  RunServe(huge, nonce, &rh);
  CHECK(rh.result == ServeResult::HandshakeFailed && rh.out.empty());

  Buf eng;
  Append(eng, BuildPong(1));
  Run rp;
  RunServe(eng, nonce, &rp);
  CHECK(rp.result == ServeResult::HandshakeFailed && rp.out.empty());

  Buf shortn;
  Append(shortn, BuildHello(1, Buf(16, 7)));
  Run rs;
  RunServe(shortn, nonce, &rs);
  CHECK(rs.result == ServeResult::HandshakeFailed && rs.out.empty());

  Run rn;
  RunServe(Buf{}, nonce, &rn);
  CHECK(rn.result == ServeResult::HandshakeFailed && rn.out.empty());
}

QTEST(session_rejects_a_bad_server_nonce_size) {
  Buf in;
  Append(in, BuildHello(1, Nonce(5)));
  Run r;
  RunServe(in, Buf(16, 5), &r);
  CHECK(r.result == ServeResult::HandshakeFailed);
  CHECK(r.out.empty());
}

QTEST(session_second_hello_is_a_violation) {
  const Buf nonce = Nonce(6);
  Buf in;
  Append(in, BuildHello(1, nonce));
  Append(in, BuildHello(2, nonce));
  Run r;
  RunServe(in, nonce, &r);
  CHECK(r.result == ServeResult::ProtocolViolation);
  CHECK((Kinds(r.out) == std::vector<std::string>{"hello_ok", "err"}));
}

QTEST(session_violations_end_the_session) {
  const Buf nonce = Nonce(7);
  auto after_hello = [&](const Buf& payload) {
    Buf in;
    Append(in, BuildHello(1, nonce));
    Append(in, payload);
    Append(in, SimpleRequest("ping", 99));  // must never be answered
    Run r;
    RunServe(in, nonce, &r);
    CHECK(r.result == ServeResult::ProtocolViolation);
    const auto k = Kinds(r.out);
    CHECK((k == std::vector<std::string>{"hello_ok", "err"}));
    CHECK(BufferIsClean(r.buf));
  };
  after_hello(Buf{0xFF});                                   // not CBOR
  after_hello(Buf{0x00});                                   // not a map
  after_hello(BeginMessage("no_such_kind", 5).Finish());    // unknown kind
  after_hello(BuildPong(5));                                // engine message from the client
  after_hello(BeginMessage("ping", 0).Finish());            // request id 0
  after_hello(cbor::MapBuilder().Uint("v", 2).Str("t", "ping").Uint("id", 5).Finish());  // version
  after_hello(BeginMessage("ping", 5).Uint("extra", 1).Finish());                        // field
  after_hello(BeginMessage("session_close", 5).Finish());   // missing field
  after_hello(BeginMessage("session_close", 5).Uint("session", 0).Finish());  // range
}

QTEST(session_oversized_and_empty_frames_end_the_session) {
  const Buf nonce = Nonce(8);
  for (const Buf& header : {Buf{0, 0, 0, 0}, Buf{0x00, 0x10, 0x00, 0x01}, Buf{0xFF, 0xFF, 0xFF, 0xFF}}) {
    Buf in;
    Append(in, BuildHello(1, nonce));
    in.insert(in.end(), header.begin(), header.end());
    in.resize(in.size() + 32, 0x55);  // payload bytes the engine must not consume
    Run r;
    RunServe(in, nonce, &r);
    CHECK(r.result == ServeResult::ProtocolViolation);
    CHECK((Kinds(r.out) == std::vector<std::string>{"hello_ok", "err"}));
  }
}

QTEST(session_truncated_frame_is_an_io_error) {
  const Buf nonce = Nonce(9);
  Buf in;
  Append(in, BuildHello(1, nonce));
  const Buf ping = SimpleRequest("ping", 2);
  Append(in, ping);
  in.resize(in.size() - 3);  // cut the last frame short
  Run r;
  RunServe(in, nonce, &r);
  CHECK(r.result == ServeResult::IoError);
}

QTEST(session_unsupported_requests_get_an_error_reply_and_the_session_continues) {
  const Buf nonce = Nonce(10);
  Buf in;
  Append(in, BuildHello(1, nonce));
  GenOptions o;
  o.id = 2;
  Append(in, GenMessage(*FindMessage("session_create"), o));
  Append(in, SimpleRequest("ping", 3));
  Run r;
  RunServe(in, nonce, &r);
  CHECK(r.result == ServeResult::PeerClosed);
  CHECK((Kinds(r.out) == std::vector<std::string>{"hello_ok", "err", "pong"}));
}

namespace {

// Remembers whether the key bytes were still readable while the handler ran, and keeps nothing.
class KeyProbe final : public MessageHandler {
 public:
  HandlerAction Handle(const ValidatedMessage& msg, Outbox& out) override {
    if (msg.spec->kind == "install_key") {
      const Value* k = Field(msg, "key");
      seen_len = k != nullptr ? k->raw.size() : 0;
      if (k != nullptr) key_copy.assign(k->raw.begin(), k->raw.end());
    }
    out.Push(BuildOk(msg.id));
    return HandlerAction::Continue;
  }
  size_t seen_len = 0;
  Buf key_copy;
};

}  // namespace

QTEST(session_key_material_is_wiped_from_the_receive_buffer) {
  const Buf nonce = Nonce(11);
  Buf key(32);
  for (size_t i = 0; i < key.size(); ++i) key[i] = static_cast<uint8_t>(0xA0 + i);

  GenOptions o;
  o.id = 2;
  const MessageSpec& spec = *FindMessage("install_key");
  const Buf key_value = [&] {
    Buf b;
    cbor::PutBin(b, key);
    return b;
  }();
  Buf in;
  Append(in, BuildHello(1, nonce));
  Append(in, GenMessageWith(spec, o, "key", &key_value));
  Append(in, SimpleRequest("ping", 3));

  KeyProbe probe;
  Run r;
  RunServe(in, nonce, &r, &probe);
  CHECK_EQ(probe.seen_len, 32u);
  CHECK(probe.key_copy == key);  // the handler could read it
  CHECK(BufferIsClean(r.buf));   // and afterwards no byte of it is left in the buffer
  // The key does not appear in anything the engine sent.
  const std::string out(r.out.begin(), r.out.end());
  const std::string needle(key.begin(), key.end());
  CHECK(out.find(needle) == std::string::npos);
}

QTEST(session_reply_is_valid_engine_message) {
  const Buf nonce = Nonce(12);
  Buf in;
  Append(in, BuildHello(41, nonce));
  Append(in, SimpleRequest("shutdown", 42));
  Run r;
  RunServe(in, nonce, &r);
  CHECK(r.result == ServeResult::Shutdown);
  // Both replies echo the request id and validate against the engine-to-client schema.
  size_t pos = 0;
  uint32_t expect_id = 41;
  while (pos + 4 <= r.out.size()) {
    const size_t len = (static_cast<size_t>(r.out[pos]) << 24) | (static_cast<size_t>(r.out[pos + 1]) << 16) |
                       (static_cast<size_t>(r.out[pos + 2]) << 8) | r.out[pos + 3];
    pos += 4;
    Value v;
    CHECK_EQ(cbor::Decode(std::span<const uint8_t>(r.out.data() + pos, len), &v), Err::Ok);
    ValidatedMessage m;
    CHECK_EQ(ValidateMessage(v, Dir::EngineToClient, &m), Err::Ok);
    CHECK_EQ(m.id, expect_id);
    ++expect_id;
    pos += len;
  }
}
