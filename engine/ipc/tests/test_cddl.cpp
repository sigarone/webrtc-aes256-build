// Keeps schema.cddl (the contract) and the validator table in schema.cpp (the implementation)
// in agreement: same message kinds, directions, id rules, field names, optionality, types, ranges,
// enums and sizes. The CDDL file is parsed with a small reader for the subset it documents at the
// top of the file.
#include <algorithm>
#include <fstream>
#include <map>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "qmedia/ipc/limits.h"
#include "qmedia/ipc/schema.h"
#include "testing.h"

using namespace qmedia::ipc;

namespace {

std::string Trim(std::string s) {
  const char* ws = " \t\r\n";
  const size_t a = s.find_first_not_of(ws);
  if (a == std::string::npos) return {};
  const size_t b = s.find_last_not_of(ws);
  return s.substr(a, b - a + 1);
}

std::string Range(uint64_t lo, uint64_t hi) { return std::to_string(lo) + ".." + std::to_string(hi); }

// ---- Table side ---------------------------------------------------------------------------

std::string DescribeFields(std::span<const FieldSpec> fields);

std::string DescribeType(const FieldSpec& f) {
  switch (f.type) {
    case FType::Uint: return "uint:" + Range(f.lo, f.hi);
    case FType::Bool: return "bool";
    case FType::Text: return "tstr:" + Range(f.lo, f.hi);
    case FType::Sdp: return "sdp:" + Range(f.lo, f.hi);
    case FType::Bytes: return "bstr:" + Range(f.lo, f.hi) + (f.nonzero ? "!nonzero" : "");
    case FType::Enum: {
      std::string s = "enum:";
      for (size_t i = 0; i < f.enums.size(); ++i) {
        if (i != 0) s += "|";
        s += std::string(f.enums[i]);
      }
      return s;
    }
    case FType::Array:
      if (f.elem == FType::Text) {
        return "arr:" + Range(f.lo, f.hi) + ":tstr:" + Range(f.elem_lo, f.elem_hi);
      }
      return "arr:" + Range(f.lo, f.hi) + ":obj{" + DescribeFields(f.object) + "}";
    case FType::ScalarMap:
      return "smap:" + Range(f.lo, f.hi) + ":key" + Range(1, kMaxScalarKeyBytes) + ":text0.." +
             std::to_string(kMaxScalarTextBytes);
    case FType::Object: return "obj{" + DescribeFields(f.object) + "}";
  }
  return "?";
}

std::string DescribeFields(std::span<const FieldSpec> fields) {
  std::vector<std::string> v;
  for (const FieldSpec& f : fields) {
    v.push_back(std::string(f.name) + (f.required ? "" : "?") + "=" + DescribeType(f));
  }
  std::sort(v.begin(), v.end());
  std::string out;
  for (const std::string& s : v) out += s + ";";
  return out;
}

const char* IdRuleName(IdRule r) {
  switch (r) {
    case IdRule::Request: return "req-id";
    case IdRule::Response: return "resp-id";
    case IdRule::Event: return "event-id";
    case IdRule::Any: return "any-id";
  }
  return "?";
}

// ---- CDDL side ----------------------------------------------------------------------------

struct Rule {
  std::vector<std::string> lines;  // field lines, comments stripped
  std::string kind;                // from the "t:" line, empty for nested objects
  std::string id_rule;             // from the "id:" line of a message
  bool has_version_1 = false;
};

struct Cddl {
  std::map<std::string, Rule> rules;
  std::vector<std::string> client_union, engine_union;
  bool ok = true;
  std::string error;
};

void SplitUnion(const std::string& body, std::vector<std::string>* out) {
  std::stringstream ss(body);
  std::string part;
  while (std::getline(ss, part, '/')) {
    part = Trim(part);
    if (!part.empty()) out->push_back(part);
  }
}

Cddl ParseCddl(const std::string& path) {
  Cddl c;
  std::ifstream in(path);
  if (!in) {
    c.ok = false;
    c.error = "cannot open " + path;
    return c;
  }
  std::string line;
  std::string current;      // rule being read, or empty
  std::string union_name;   // union being read, or empty
  std::string union_body;
  auto flush_union = [&]() {
    if (union_name == "client-message") SplitUnion(union_body, &c.client_union);
    if (union_name == "engine-message") SplitUnion(union_body, &c.engine_union);
    union_name.clear();
    union_body.clear();
  };
  static const std::regex rule_open(R"(^([a-z0-9-]+) = \{$)");
  static const std::regex union_open(R"(^(client-message|engine-message) =\s*$)");
  while (std::getline(in, line)) {
    std::string code = line;
    std::string comment;
    const size_t semi = code.find(';');
    if (semi != std::string::npos) {
      comment = code.substr(semi + 1);
      code = code.substr(0, semi);
    }
    code = Trim(code);
    std::smatch m;
    if (!union_name.empty()) {
      if (code.empty()) {
        flush_union();
      } else {
        union_body += " " + code;
      }
      continue;
    }
    if (current.empty()) {
      if (std::regex_match(code, m, rule_open)) {
        current = m[1];
        if (c.rules.count(current) != 0) {
          c.ok = false;
          c.error = "duplicate rule " + current;
        }
        c.rules[current] = Rule{};
      } else if (std::regex_match(code, m, union_open)) {
        union_name = m[1];
      }
      continue;
    }
    if (code == "}") {
      current.clear();
      continue;
    }
    if (code.empty()) continue;
    if (code.back() != ',') {
      c.ok = false;
      c.error = "field line without trailing comma in " + current + ": " + code;
      continue;
    }
    code.pop_back();
    Rule& r = c.rules[current];
    if (code.rfind("v: ", 0) == 0) {
      r.has_version_1 = Trim(code.substr(3)) == "1";
    } else if (code.rfind("t: ", 0) == 0) {
      std::string t = Trim(code.substr(3));
      if (t.size() < 2 || t.front() != '"' || t.back() != '"') {
        c.ok = false;
        c.error = "bad t line in " + current;
      } else {
        r.kind = t.substr(1, t.size() - 2);
      }
    } else if (code.rfind("id: ", 0) == 0 && !r.kind.empty()) {
      r.id_rule = Trim(code.substr(4));
    } else {
      if (comment.find("nonzero") != std::string::npos) code += " !nonzero";
      if (comment.find("sdp") != std::string::npos) code += " !sdp";
      r.lines.push_back(code);
    }
  }
  if (!union_name.empty()) flush_union();
  return c;
}

std::string CanonRange(const std::string& a, const std::string& b) { return a + ".." + b; }

std::string DescribeCddlFields(const Cddl& c, const std::vector<std::string>& lines);

// Converts a CDDL type expression to the same canonical text DescribeType produces.
std::string CanonType(const Cddl& c, std::string t, bool* ok) {
  t = Trim(t);
  bool nonzero = false;
  bool sdp = false;
  const size_t nz = t.find(" !nonzero");
  if (nz != std::string::npos) {
    nonzero = true;
    t = Trim(t.substr(0, nz));
  }
  const size_t sd = t.find(" !sdp");
  if (sd != std::string::npos) {
    sdp = true;
    t = Trim(t.substr(0, sd));
  }
  std::smatch m;
  static const std::regex re_uint(R"(^(\d+)\.\.(\d+)$)");
  static const std::regex re_size_range(R"(^(tstr|bstr) \.size \((\d+)\.\.(\d+)\)$)");
  static const std::regex re_size_exact(R"(^(tstr|bstr) \.size (\d+)$)");
  static const std::regex re_arr_text(R"(^\[(\d+)\*(\d+) tstr \.size \((\d+)\.\.(\d+)\)\]$)");
  static const std::regex re_arr_obj(R"(^\[(\d+)\*(\d+) ([a-z0-9-]+)\]$)");
  static const std::regex re_smap(
      R"(^\{ (\d+)\*(\d+) tstr \.size \((\d+)\.\.(\d+)\) => scalar \}$)");

  if (t == "handle") return "uint:1..4294967295";
  if (t == "bool") return "bool";
  if (!t.empty() && t[0] == '"') {
    std::string out = "enum:";
    std::stringstream ss(t);
    std::string part;
    bool first = true;
    while (std::getline(ss, part, '/')) {
      part = Trim(part);
      if (part.size() < 2 || part.front() != '"' || part.back() != '"') {
        *ok = false;
        return "bad-enum";
      }
      if (!first) out += "|";
      out += part.substr(1, part.size() - 2);
      first = false;
    }
    return out;
  }
  if (std::regex_match(t, m, re_uint)) return "uint:" + CanonRange(m[1], m[2]);
  if (std::regex_match(t, m, re_size_range)) {
    const std::string base = (sdp && m[1] == "tstr") ? "sdp" : std::string(m[1]);
    return base + ":" + CanonRange(m[2], m[3]) + (nonzero ? "!nonzero" : "");
  }
  if (std::regex_match(t, m, re_size_exact)) {
    return std::string(m[1]) + ":" + CanonRange(m[2], m[2]) + (nonzero ? "!nonzero" : "");
  }
  if (std::regex_match(t, m, re_arr_text)) {
    return "arr:" + CanonRange(m[1], m[2]) + ":tstr:" + CanonRange(m[3], m[4]);
  }
  if (std::regex_match(t, m, re_arr_obj)) {
    const auto it = c.rules.find(m[3]);
    if (it == c.rules.end()) {
      *ok = false;
      return "missing-rule";
    }
    return "arr:" + CanonRange(m[1], m[2]) + ":obj{" + DescribeCddlFields(c, it->second.lines) + "}";
  }
  if (std::regex_match(t, m, re_smap)) {
    // The text value limit is the rule "scalar"; its size is checked separately.
    return "smap:" + CanonRange(m[1], m[2]) + ":key" + CanonRange(m[3], m[4]) + ":text0.." +
           std::to_string(kMaxScalarTextBytes);
  }
  *ok = false;
  return "unparsed(" + t + ")";
}

std::string DescribeCddlFields(const Cddl& c, const std::vector<std::string>& lines) {
  std::vector<std::string> v;
  for (std::string line : lines) {
    bool optional = false;
    if (line.rfind("? ", 0) == 0) {
      optional = true;
      line = line.substr(2);
    }
    const size_t colon = line.find(": ");
    if (colon == std::string::npos) {
      v.push_back("unparsed-line(" + line + ")");
      continue;
    }
    const std::string name = line.substr(0, colon);
    bool ok = true;
    const std::string type = CanonType(c, line.substr(colon + 2), &ok);
    v.push_back(name + (optional ? "?" : "") + "=" + type);
  }
  std::sort(v.begin(), v.end());
  std::string out;
  for (const std::string& s : v) out += s + ";";
  return out;
}

}  // namespace

QTEST(cddl_matches_the_validator_table) {
  const Cddl c = ParseCddl(QMEDIA_CDDL_FILE);
  if (!c.ok) std::fprintf(stderr, "  cddl parse error: %s\n", c.error.c_str());
  CHECK(c.ok);

  // The scalar rule carries the text limit that the table keeps in limits.h.
  {
    std::ifstream in(QMEDIA_CDDL_FILE);
    std::stringstream ss;
    ss << in.rdbuf();
    const std::string want =
        "scalar = uint / bool / tstr .size (0.." + std::to_string(kMaxScalarTextBytes) + ")";
    CHECK(ss.str().find(want) != std::string::npos);
  }

  std::map<std::string, const Rule*> by_kind;
  std::map<std::string, std::string> rule_of_kind;
  for (const auto& [name, rule] : c.rules) {
    if (rule.kind.empty()) continue;
    CHECK(rule.has_version_1);
    CHECK(by_kind.emplace(rule.kind, &rule).second);
    rule_of_kind[rule.kind] = name;
  }

  // Same set of kinds on both sides.
  std::set<std::string> table_kinds;
  for (const MessageSpec& spec : Messages()) table_kinds.insert(std::string(spec.kind));
  std::set<std::string> cddl_kinds;
  for (const auto& [kind, rule] : by_kind) cddl_kinds.insert(kind);
  CHECK(table_kinds == cddl_kinds);

  // The unions list each message exactly once and agree with the table's direction.
  std::set<std::string> in_client(c.client_union.begin(), c.client_union.end());
  std::set<std::string> in_engine(c.engine_union.begin(), c.engine_union.end());
  CHECK_EQ(in_client.size(), c.client_union.size());
  CHECK_EQ(in_engine.size(), c.engine_union.size());
  for (const std::string& n : in_client) CHECK(in_engine.count(n) == 0);

  for (const MessageSpec& spec : Messages()) {
    const std::string kind(spec.kind);
    const auto it = by_kind.find(kind);
    if (it == by_kind.end()) {
      std::fprintf(stderr, "  kind missing from schema.cddl: %s\n", kind.c_str());
      CHECK(false);
      continue;
    }
    const Rule& rule = *it->second;
    const std::string& rname = rule_of_kind[kind];
    const bool c2e = spec.dir == Dir::ClientToEngine;
    CHECK(c2e ? in_client.count(rname) == 1 : in_engine.count(rname) == 1);
    if (rule.id_rule != IdRuleName(spec.id_rule)) {
      std::fprintf(stderr, "  id rule differs for %s: cddl=%s table=%s\n", kind.c_str(),
                   rule.id_rule.c_str(), IdRuleName(spec.id_rule));
      CHECK(false);
    }
    const std::string want = DescribeFields(spec.fields);
    const std::string got = DescribeCddlFields(c, rule.lines);
    if (want != got) {
      std::fprintf(stderr, "  fields differ for %s\n    table: %s\n    cddl : %s\n", kind.c_str(),
                   want.c_str(), got.c_str());
      CHECK(false);
    }
  }
  // Every name in a union is a rule with a kind.
  for (const std::string& n : c.client_union) CHECK(c.rules.count(n) == 1 && !c.rules.at(n).kind.empty());
  for (const std::string& n : c.engine_union) CHECK(c.rules.count(n) == 1 && !c.rules.at(n).kind.empty());
}

QTEST(cddl_parser_self_check) {
  // The comparison above would pass vacuously if the parser returned nothing.
  const Cddl c = ParseCddl(QMEDIA_CDDL_FILE);
  CHECK(c.ok);
  CHECK(c.rules.size() > 50);
  CHECK(c.client_union.size() >= 28);
  CHECK(c.engine_union.size() >= 20);
  const auto it = c.rules.find("install-key");
  CHECK(it != c.rules.end());
  if (it != c.rules.end()) {
    const std::string d = DescribeCddlFields(c, it->second.lines);
    CHECK(d.find("key=bstr:32..32!nonzero") != std::string::npos);
    CHECK(d.find("slot=uint:0..15") != std::string::npos);
    CHECK(d.find("direction=enum:send|recv") != std::string::npos);
  }
}
