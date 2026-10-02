// Minimal test harness (no third-party dependency).
#pragma once

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace qtest {

struct Case {
  const char* name;
  void (*fn)();
};

inline std::vector<Case>& Registry() {
  static std::vector<Case> r;
  return r;
}

inline int& Failures() {
  static int f = 0;
  return f;
}

struct Registrar {
  Registrar(const char* name, void (*fn)()) { Registry().push_back({name, fn}); }
};

}  // namespace qtest

#define QTEST(name)                                         \
  static void name();                                       \
  static ::qtest::Registrar registrar_##name(#name, &name); \
  static void name()

#define CHECK(cond)                                                          \
  do {                                                                       \
    if (!(cond)) {                                                           \
      std::fprintf(stderr, "  CHECK failed: %s (%s:%d)\n", #cond, __FILE__, __LINE__); \
      ++::qtest::Failures();                                                 \
    }                                                                        \
  } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))
