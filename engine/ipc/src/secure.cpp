#include "qmedia/ipc/secure.h"

#include <cstdint>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>
#else
#include <sys/random.h>

#include <cerrno>
#endif

namespace qmedia::ipc {

void SecureZero(void* p, size_t n) noexcept {
  if (p == nullptr || n == 0) return;
#if defined(_WIN32)
  SecureZeroMemory(p, n);
#else
  volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
  while (n--) *v++ = 0;
#endif
}

bool ConstantTimeEqual(const void* a, const void* b, size_t n) noexcept {
  const volatile uint8_t* x = static_cast<const volatile uint8_t*>(a);
  const volatile uint8_t* y = static_cast<const volatile uint8_t*>(b);
  uint8_t diff = 0;
  for (size_t i = 0; i < n; ++i) diff = static_cast<uint8_t>(diff | (x[i] ^ y[i]));
  return diff == 0;
}

bool FillRandom(void* p, size_t n) noexcept {
  uint8_t* dst = static_cast<uint8_t*>(p);
#if defined(_WIN32)
  while (n > 0) {
    const ULONG chunk = n > 0x10000 ? 0x10000 : static_cast<ULONG>(n);
    if (BCryptGenRandom(nullptr, dst, chunk, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) return false;
    dst += chunk;
    n -= chunk;
  }
  return true;
#else
  while (n > 0) {
    const ssize_t got = getrandom(dst, n, 0);
    if (got < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    dst += got;
    n -= static_cast<size_t>(got);
  }
  return true;
#endif
}

}  // namespace qmedia::ipc
