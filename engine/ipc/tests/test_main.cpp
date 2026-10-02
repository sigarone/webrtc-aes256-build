#include <cstring>

#include "testing.h"

int main(int argc, char** argv) {
  const char* filter = argc > 1 ? argv[1] : nullptr;
  int ran = 0;
  for (const qtest::Case& c : qtest::Registry()) {
    if (filter != nullptr && std::strstr(c.name, filter) == nullptr) continue;
    const int before = qtest::Failures();
    std::fprintf(stderr, "[ RUN  ] %s\n", c.name);
    c.fn();
    std::fprintf(stderr, "[ %s ] %s\n", qtest::Failures() == before ? " OK " : "FAIL", c.name);
    ++ran;
  }
  std::fprintf(stderr, "%d tests, %d failed checks\n", ran, qtest::Failures());
  return (qtest::Failures() == 0 && ran > 0) ? 0 : 1;
}
