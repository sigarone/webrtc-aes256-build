// Small security primitives. No logging, no allocation.
#pragma once

#include <cstddef>

namespace qmedia::ipc {

// Zeroes memory in a way the optimizer may not remove.
void SecureZero(void* p, size_t n) noexcept;

// Constant-time equality for secrets (nonces, MACs). Returns true iff all n bytes match.
bool ConstantTimeEqual(const void* a, const void* b, size_t n) noexcept;

// Fills the buffer from the operating system CSPRNG. Returns false on failure.
bool FillRandom(void* p, size_t n) noexcept;

}  // namespace qmedia::ipc
