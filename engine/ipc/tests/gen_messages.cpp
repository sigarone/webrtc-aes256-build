#include "gen_messages.h"

#include <algorithm>
#include <string>

#include "qmedia/ipc/limits.h"

namespace qmedia::testgen {

namespace {

using ipc::FieldSpec;
using ipc::FType;
using ipc::cbor::Buf;

uint64_t Pick(uint64_t lo, uint64_t hi, const GenOptions& o) {
  uint64_t v = o.full ? hi : lo;
  if (o.len_cap != 0) v = std::min<uint64_t>(v, std::max<uint64_t>(lo, o.len_cap));
  return v;
}

Buf GenObject(std::span<const FieldSpec> fields, const GenOptions& o);

Buf GenValue(const FieldSpec& f, const GenOptions& o) {
  Buf out;
  switch (f.type) {
    case FType::Uint:
      ipc::cbor::PutUint(out, o.full ? f.hi : f.lo);
      break;
    case FType::Bool:
      ipc::cbor::PutBool(out, true);
      break;
    case FType::Text:
    case FType::Sdp: {
      const std::string s(static_cast<size_t>(Pick(f.lo, f.hi, o)), 'a');
      ipc::cbor::PutStr(out, s);
      break;
    }
    case FType::Bytes: {
      const size_t n = static_cast<size_t>(Pick(f.lo, f.hi, o));
      Buf b(n);
      for (size_t i = 0; i < n; ++i) b[i] = static_cast<uint8_t>(0x5A + (i % 7));  // never all zero
      ipc::cbor::PutBin(out, b);
      break;
    }
    case FType::Enum: {
      const std::string_view e = o.full ? f.enums.back() : f.enums.front();
      ipc::cbor::PutStr(out, e);
      break;
    }
    case FType::Object:
      return GenObject(f.object, o);
    case FType::Array: {
      uint64_t n = o.full ? std::min<uint64_t>(f.hi, 3) : f.lo;
      n = std::max<uint64_t>(n, f.lo);
      ipc::cbor::ArrayBuilder ab;
      for (uint64_t i = 0; i < n; ++i) {
        if (f.elem == FType::Text) {
          Buf el;
          ipc::cbor::PutStr(el, std::string(Pick(f.elem_lo, f.elem_hi, o), 'u'));
          ab.Add(std::move(el));
        } else {
          ab.Add(GenObject(f.object, o));
        }
      }
      return ab.Finish();
    }
    case FType::ScalarMap: {
      const uint64_t n = o.full ? std::min<uint64_t>(f.hi, 3) : f.lo;
      ipc::cbor::MapBuilder mb;
      for (uint64_t i = 0; i < n; ++i) {
        const std::string key = "k" + std::to_string(i);
        if (i % 3 == 0) mb.Uint(key, 1000 + i);
        else if (i % 3 == 1) mb.Flag(key, true);
        else mb.Str(key, "v");
      }
      return mb.Finish();
    }
  }
  return out;
}

Buf GenObject(std::span<const FieldSpec> fields, const GenOptions& o) {
  ipc::cbor::MapBuilder mb;
  for (const FieldSpec& f : fields) {
    if (f.required || o.full) mb.Raw(f.name, GenValue(f, o));
  }
  return mb.Finish();
}

}  // namespace

Buf GenMessageWith(const ipc::MessageSpec& spec, const GenOptions& o, const char* override_key,
                   const Buf* override_value) {
  ipc::cbor::MapBuilder mb;
  auto put = [&](std::string_view key, Buf value) {
    if (override_key != nullptr && key == override_key) {
      if (override_value != nullptr) mb.Raw(key, *override_value);
      return;  // nullptr value = field removed
    }
    mb.Raw(key, std::move(value));
  };
  Buf v, t, id;
  ipc::cbor::PutUint(v, ipc::kProtocolVersion);
  ipc::cbor::PutStr(t, spec.kind);
  ipc::cbor::PutUint(id, spec.id_rule == ipc::IdRule::Event ? 0 : o.id);
  put("v", std::move(v));
  put("t", std::move(t));
  put("id", std::move(id));
  for (const FieldSpec& f : spec.fields) {
    if (f.required || o.full) put(f.name, GenValue(f, o));
  }
  return mb.Finish();
}

Buf GenMessage(const ipc::MessageSpec& spec, const GenOptions& opt) {
  return GenMessageWith(spec, opt, nullptr, nullptr);
}

}  // namespace qmedia::testgen
