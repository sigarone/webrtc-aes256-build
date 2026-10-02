#include "qmedia/ipc/cbor.h"

#include <algorithm>
#include <cstring>

#include "qmedia/ipc/limits.h"

namespace qmedia::ipc::cbor {

namespace {

struct Cursor {
  const uint8_t* p;
  const uint8_t* end;
  size_t remaining() const { return static_cast<size_t>(end - p); }
};

struct Budget {
  size_t nodes = kMaxNodes;
};

bool ValidUtf8(const uint8_t* p, size_t n) {
  size_t i = 0;
  while (i < n) {
    const uint8_t c = p[i];
    if (c < 0x80) {
      ++i;
      continue;
    }
    size_t len;
    uint32_t cp;
    if (c >= 0xC2 && c <= 0xDF) {
      len = 2;
      cp = c & 0x1Fu;
    } else if (c >= 0xE0 && c <= 0xEF) {
      len = 3;
      cp = c & 0x0Fu;
    } else if (c >= 0xF0 && c <= 0xF4) {
      len = 4;
      cp = c & 0x07u;
    } else {
      return false;
    }
    if (n - i < len) return false;
    for (size_t k = 1; k < len; ++k) {
      if ((p[i + k] & 0xC0) != 0x80) return false;
      cp = (cp << 6) | (p[i + k] & 0x3Fu);
    }
    if (len == 3 && (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF))) return false;
    if (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) return false;
    i += len;
  }
  return true;
}

// Reads the initial byte and its argument. Caller has already excluded major type 7.
Err ReadHead(Cursor& c, uint64_t* arg) {
  if (c.p == c.end) return Err::CborTruncated;
  const uint8_t ai = *c.p++ & 0x1F;
  if (ai < 24) {
    *arg = ai;
    return Err::Ok;
  }
  size_t n;
  uint64_t min_value;
  switch (ai) {
    case 24: n = 1; min_value = 24; break;
    case 25: n = 2; min_value = 0x100; break;
    case 26: n = 4; min_value = 0x10000; break;
    case 27: n = 8; min_value = 0x100000000ull; break;
    default: return Err::CborUnsupported;  // 28..30 reserved, 31 indefinite length
  }
  if (c.remaining() < n) return Err::CborTruncated;
  uint64_t v = 0;
  for (size_t i = 0; i < n; ++i) v = (v << 8) | *c.p++;
  if (v < min_value) return Err::CborNonShortest;
  *arg = v;
  return Err::Ok;
}

// Lexicographic order of the encoded key bytes (RFC 8949 4.2.1).
int CompareEncoded(const uint8_t* a, size_t an, const uint8_t* b, size_t bn) {
  const size_t m = std::min(an, bn);
  const int r = m == 0 ? 0 : std::memcmp(a, b, m);
  if (r != 0) return r;
  if (an == bn) return 0;
  return an < bn ? -1 : 1;
}

Err DecodeItem(Cursor& c, Budget& budget, int depth, Value* out) {
  if (depth > kMaxDepth) return Err::CborTooDeep;
  if (budget.nodes == 0) return Err::CborTooLarge;
  --budget.nodes;
  if (c.p == c.end) return Err::CborTruncated;

  const uint8_t initial = *c.p;
  const uint8_t major = initial >> 5;

  if (major == 7) {
    ++c.p;
    switch (initial & 0x1F) {
      case 20: out->type = Value::Type::Bool; out->u = 0; return Err::Ok;
      case 21: out->type = Value::Type::Bool; out->u = 1; return Err::Ok;
      case 22: out->type = Value::Type::Null; return Err::Ok;
      default: return Err::CborUnsupported;  // undefined, other simple values, floats, break
    }
  }

  uint64_t arg = 0;
  Err e = ReadHead(c, &arg);
  if (e != Err::Ok) return e;

  switch (major) {
    case 0:
      out->type = Value::Type::Uint;
      out->u = arg;
      return Err::Ok;
    case 1:
      return Err::CborUnsupported;  // negative integers are not part of the subset
    case 2:
    case 3: {
      if (arg > c.remaining()) return Err::CborTruncated;
      const size_t n = static_cast<size_t>(arg);
      if (major == 3 && !ValidUtf8(c.p, n)) return Err::CborBadUtf8;
      out->type = major == 2 ? Value::Type::Bytes : Value::Type::Text;
      out->raw = std::span<const uint8_t>(c.p, n);
      c.p += n;
      return Err::Ok;
    }
    case 4: {
      // Every element takes at least one byte: bounds the count by the bytes really present.
      if (arg > c.remaining()) return Err::CborTruncated;
      if (arg > budget.nodes) return Err::CborTooLarge;
      out->type = Value::Type::Array;
      out->items.reserve(static_cast<size_t>(arg));
      for (uint64_t i = 0; i < arg; ++i) {
        out->items.emplace_back();
        e = DecodeItem(c, budget, depth + 1, &out->items.back());
        if (e != Err::Ok) return e;
      }
      return Err::Ok;
    }
    case 5: {
      if (arg > c.remaining() / 2) return Err::CborTruncated;
      if (arg * 2 > budget.nodes) return Err::CborTooLarge;
      out->type = Value::Type::Map;
      out->items.reserve(static_cast<size_t>(arg) * 2);
      const uint8_t* prev_start = nullptr;
      size_t prev_len = 0;
      for (uint64_t i = 0; i < arg; ++i) {
        if (c.p == c.end) return Err::CborTruncated;
        if ((*c.p >> 5) != 3) return Err::CborKeyType;  // keys are text strings
        const uint8_t* key_start = c.p;
        out->items.emplace_back();
        e = DecodeItem(c, budget, depth + 1, &out->items.back());
        if (e != Err::Ok) return e;
        if (out->items.back().raw.size() > kMaxKeyBytes) return Err::CborKeyType;
        const size_t key_len = static_cast<size_t>(c.p - key_start);
        if (prev_start != nullptr &&
            CompareEncoded(prev_start, prev_len, key_start, key_len) >= 0) {
          return Err::CborKeyOrder;  // unsorted or duplicate
        }
        prev_start = key_start;
        prev_len = key_len;
        out->items.emplace_back();
        e = DecodeItem(c, budget, depth + 1, &out->items.back());
        if (e != Err::Ok) return e;
      }
      return Err::Ok;
    }
    default:
      return Err::CborUnsupported;  // major 6: tags
  }
}

void EncodeInto(const Value& v, Buf& out) {
  switch (v.type) {
    case Value::Type::Uint: PutUint(out, v.u); break;
    case Value::Type::Bytes: PutBin(out, v.raw); break;
    case Value::Type::Text: PutStr(out, v.text()); break;
    case Value::Type::Bool: PutBool(out, v.u != 0); break;
    case Value::Type::Null: PutNull(out); break;
    case Value::Type::Array:
      PutHead(out, 4, v.items.size());
      for (const Value& it : v.items) EncodeInto(it, out);
      break;
    case Value::Type::Map: {
      MapBuilder b;
      for (size_t i = 0; i + 1 < v.items.size(); i += 2) {
        Buf enc;
        EncodeInto(v.items[i + 1], enc);
        b.Raw(v.items[i].text(), std::move(enc));
      }
      const Buf m = b.Finish();
      out.insert(out.end(), m.begin(), m.end());
      break;
    }
  }
}

}  // namespace

Err Decode(std::span<const uint8_t> in, Value* out) {
  Cursor c{in.data(), in.data() + in.size()};
  Budget budget;
  *out = Value{};
  Err e = DecodeItem(c, budget, 0, out);
  if (e != Err::Ok) return e;
  if (c.p != c.end) return Err::CborTrailing;
  return Err::Ok;
}

const Value* MapGet(const Value& map, std::string_view key) {
  if (map.type != Value::Type::Map) return nullptr;
  for (size_t i = 0; i + 1 < map.items.size(); i += 2) {
    if (map.items[i].text() == key) return &map.items[i + 1];
  }
  return nullptr;
}

void Encode(const Value& v, Buf* out) {
  out->clear();
  EncodeInto(v, *out);
}

void PutHead(Buf& out, uint8_t major, uint64_t arg) {
  const uint8_t m = static_cast<uint8_t>(major << 5);
  if (arg < 24) {
    out.push_back(static_cast<uint8_t>(m | arg));
    return;
  }
  size_t n;
  uint8_t ai;
  if (arg <= 0xFF) {
    n = 1;
    ai = 24;
  } else if (arg <= 0xFFFF) {
    n = 2;
    ai = 25;
  } else if (arg <= 0xFFFFFFFFull) {
    n = 4;
    ai = 26;
  } else {
    n = 8;
    ai = 27;
  }
  out.push_back(static_cast<uint8_t>(m | ai));
  for (size_t i = n; i > 0; --i) out.push_back(static_cast<uint8_t>(arg >> (8 * (i - 1))));
}

void PutUint(Buf& out, uint64_t v) { PutHead(out, 0, v); }

void PutBin(Buf& out, std::span<const uint8_t> v) {
  PutHead(out, 2, v.size());
  out.insert(out.end(), v.begin(), v.end());
}

void PutStr(Buf& out, std::string_view v) {
  PutHead(out, 3, v.size());
  out.insert(out.end(), v.begin(), v.end());
}

void PutBool(Buf& out, bool v) { out.push_back(v ? 0xF5 : 0xF4); }

void PutNull(Buf& out) { out.push_back(0xF6); }

MapBuilder& MapBuilder::Uint(std::string_view key, uint64_t v) {
  Buf val;
  PutUint(val, v);
  return Raw(key, std::move(val));
}

MapBuilder& MapBuilder::Flag(std::string_view key, bool v) {
  Buf val;
  PutBool(val, v);
  return Raw(key, std::move(val));
}

MapBuilder& MapBuilder::Str(std::string_view key, std::string_view v) {
  Buf val;
  PutStr(val, v);
  return Raw(key, std::move(val));
}

MapBuilder& MapBuilder::Bin(std::string_view key, std::span<const uint8_t> v) {
  Buf val;
  PutBin(val, v);
  return Raw(key, std::move(val));
}

MapBuilder& MapBuilder::Raw(std::string_view key, Buf encoded_value) {
  Entry e;
  PutStr(e.key, key);
  e.value = std::move(encoded_value);
  entries_.push_back(std::move(e));
  return *this;
}

Buf MapBuilder::Finish() const {
  std::vector<const Entry*> sorted;
  sorted.reserve(entries_.size());
  for (const Entry& e : entries_) sorted.push_back(&e);
  std::sort(sorted.begin(), sorted.end(), [](const Entry* a, const Entry* b) {
    return CompareEncoded(a->key.data(), a->key.size(), b->key.data(), b->key.size()) < 0;
  });
  Buf out;
  PutHead(out, 5, sorted.size());
  for (const Entry* e : sorted) {
    out.insert(out.end(), e->key.begin(), e->key.end());
    out.insert(out.end(), e->value.begin(), e->value.end());
  }
  return out;
}

ArrayBuilder& ArrayBuilder::Add(Buf encoded_item) {
  items_.push_back(std::move(encoded_item));
  return *this;
}

Buf ArrayBuilder::Finish() const {
  Buf out;
  PutHead(out, 4, items_.size());
  for (const Buf& it : items_) out.insert(out.end(), it.begin(), it.end());
  return out;
}

}  // namespace qmedia::ipc::cbor
