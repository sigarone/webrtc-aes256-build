#!/usr/bin/env bash
# Builds janus-gateway (latest release tag) against the BoringSSL / libsrtp /
# libnice from build-deps.sh, after applying janus-dtls13.patch.
# VARIANT=legacy-timer builds the A/B baseline used by the loss soak: the same
# patch minus the post-handshake DTLS timer servicing (stock janus_dtls_retry).
set -euxo pipefail

JANUS_TAG=${JANUS_TAG:-v1.4.2}
VARIANT=${VARIANT:-}
BSSL=${BSSL:-/opt/boringssl}
PREFIX=${PREFIX:-/opt/janus-probe}
SRC=${SRC:-$HOME/jp-src}
PROBE=$(cd "$(dirname "$0")/.." && pwd)
NPROC=$(nproc)
SFX=${VARIANT:+-$VARIANT}
JPREFIX=$PREFIX/janus$SFX
mkdir -p "$SRC"

cd "$SRC"
rm -rf "janus$SFX"
git clone -q --depth 1 --branch "$JANUS_TAG" https://github.com/meetecho/janus-gateway.git "janus$SFX"
cd "janus$SFX"
if [ -z "$VARIANT" ]; then
  sed -i "/^janus-gateway /d;/^Janus /d" "$PREFIX/versions.txt" || true
  echo "janus-gateway $JANUS_TAG $(git rev-parse HEAD)" >> "$PREFIX/versions.txt"
fi
git apply --check "$PROBE/janus-dtls13.patch"
git apply --stat "$PROBE/janus-dtls13.patch"
git apply "$PROBE/janus-dtls13.patch"
if [ "$VARIANT" = legacy-timer ]; then git apply "$PROBE/ab/legacy-timer.patch"; fi
sh autogen.sh

# never link a system libcurl (it would drag a system OpenSSL into the process)
export LIBCURL_CFLAGS=" " LIBCURL_LIBS=" "
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$BSSL/lib/pkgconfig"
export LDFLAGS="-L$PREFIX/lib -L$BSSL/lib -Wl,-rpath,$PREFIX/lib"
# static BoringSSL + static libsrtp: make sure libcrypto comes after libsrtp2 on the link line
export LIBS="-L$BSSL/lib -lssl -lcrypto -lstdc++ -lpthread -ldl -lm"
./configure --prefix="$JPREFIX" \
  --enable-boringssl="$BSSL" --enable-dtls-settimeout \
  --enable-rest --disable-all-transports --disable-websockets \
  --disable-all-plugins --enable-plugin-echotest --enable-plugin-videoroom \
  --disable-all-handlers --disable-all-loggers \
  --disable-data-channels --disable-turn-rest-api --disable-sample-event-handler \
  --disable-docs 2>&1 | tee "$PREFIX/janus-configure$SFX.log" | tail -60
make -j"$NPROC"
make install

# exactly one TLS implementation may live in the process: BoringSSL, static
echo "== ldd janus"; ldd "$JPREFIX/bin/janus" | tee "$PREFIX/janus-ldd$SFX.txt"
if grep -E 'libssl|libcrypto' "$PREFIX/janus-ldd$SFX.txt"; then
  echo "ERROR: janus links a system OpenSSL dynamically"; exit 1
fi
for f in "$JPREFIX"/lib/janus/plugins/*.so "$JPREFIX"/lib/janus/transports/*.so; do
  if ldd "$f" | grep -E 'libssl|libcrypto'; then echo "ERROR: $f links a system OpenSSL"; exit 1; fi
done
if [ -z "$VARIANT" ]; then "$JPREFIX/bin/janus" --version | tee -a "$PREFIX/versions.txt"; fi
cat "$PREFIX/versions.txt"
