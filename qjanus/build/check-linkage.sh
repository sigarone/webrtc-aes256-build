#!/usr/bin/env bash
# check-linkage.sh <elf>...: every dynamic dependency of every given ELF file,
# resolved transitively, must exist, and none may be a system libssl/libcrypto
# (the only TLS/crypto code in the process is the static BoringSSL inside janus).
# LD_LIBRARY_PATH is honoured (the shipped lib dir is not on the default path).
set -uo pipefail
rc=0
for f in "$@"; do
  out=$(ldd "$f" 2>&1) || { echo "ERROR: ldd failed on $f"; echo "$out"; rc=1; continue; }
  if echo "$out" | grep -q 'not found'; then
    echo "ERROR: unresolved dependency of $f:"; echo "$out" | grep 'not found'; rc=1
  fi
  if echo "$out" | grep -Eq 'libssl|libcrypto|libgnutls-openssl|libmbedtls|libwolfssl'; then
    echo "ERROR: $f links a TLS/crypto library besides the static BoringSSL:"
    echo "$out" | grep -E 'libssl|libcrypto|libgnutls-openssl|libmbedtls|libwolfssl'; rc=1
  fi
  if readelf -d "$f" 2>/dev/null | grep -Eq 'NEEDED.*(libssl|libcrypto)'; then
    echo "ERROR: $f has libssl/libcrypto in its NEEDED list"; rc=1
  fi
done
[ "$rc" = 0 ] && echo "linkage ok: no system libssl/libcrypto, no unresolved libraries ($# files)"
exit "$rc"
