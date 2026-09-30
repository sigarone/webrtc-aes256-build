#!/usr/bin/env bash
# Builds janus-gateway (latest release tag) against the BoringSSL / libsrtp /
# libnice from build-deps.sh, after applying janus-dtls13.patch.
set -euxo pipefail

JANUS_TAG=${JANUS_TAG:-v1.4.2}
BSSL=${BSSL:-/opt/boringssl}
PREFIX=${PREFIX:-/opt/janus-probe}
SRC=${SRC:-$HOME/jp-src}
PROBE=$(cd "$(dirname "$0")/.." && pwd)
NPROC=$(nproc)
mkdir -p "$SRC"

cd "$SRC"
rm -rf janus && git clone -q --depth 1 --branch "$JANUS_TAG" https://github.com/meetecho/janus-gateway.git janus
cd janus
sed -i "/^janus-gateway /d;/^Janus /d" "$PREFIX/versions.txt" || true
echo "janus-gateway $JANUS_TAG $(git rev-parse HEAD)" >> "$PREFIX/versions.txt"
git apply --check "$PROBE/janus-dtls13.patch"
git apply --stat "$PROBE/janus-dtls13.patch"
git apply "$PROBE/janus-dtls13.patch"
sh autogen.sh

# never link a system libcurl (it would drag a system OpenSSL into the process)
export LIBCURL_CFLAGS=" " LIBCURL_LIBS=" "
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$BSSL/lib/pkgconfig"
export LDFLAGS="-L$PREFIX/lib -L$BSSL/lib -Wl,-rpath,$PREFIX/lib"
# static BoringSSL + static libsrtp: make sure libcrypto comes after libsrtp2 on the link line
export LIBS="-L$BSSL/lib -lssl -lcrypto -lstdc++ -lpthread -ldl -lm"
./configure --prefix="$PREFIX/janus" \
  --enable-boringssl="$BSSL" --enable-dtls-settimeout \
  --enable-rest --disable-all-transports --disable-websockets \
  --disable-all-plugins --enable-plugin-echotest --enable-plugin-videoroom \
  --disable-all-handlers --disable-all-loggers \
  --disable-data-channels --disable-turn-rest-api --disable-sample-event-handler \
  --disable-docs 2>&1 | tee "$PREFIX/janus-configure.log" | tail -60
make -j"$NPROC"
make install

# exactly one TLS implementation may live in the process: BoringSSL, static
echo "== ldd janus"; ldd "$PREFIX/janus/bin/janus" | tee "$PREFIX/janus-ldd.txt"
if grep -E 'libssl|libcrypto' "$PREFIX/janus-ldd.txt"; then
  echo "ERROR: janus links a system OpenSSL dynamically"; exit 1
fi
for f in "$PREFIX"/janus/lib/janus/plugins/*.so "$PREFIX"/janus/lib/janus/transports/*.so; do
  if ldd "$f" | grep -E 'libssl|libcrypto'; then echo "ERROR: $f links a system OpenSSL"; exit 1; fi
done
"$PREFIX/janus/bin/janus" --version | tee -a "$PREFIX/versions.txt"
cat "$PREFIX/versions.txt"
