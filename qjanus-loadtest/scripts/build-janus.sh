#!/usr/bin/env bash
# Builds the Janus gateway for the load-test smoke run: pinned v1.4.2, only the
# VideoRoom plugin and the HTTP + WebSockets transports, linked against the
# BoringSSL / libsrtp / libnice / libwebsockets that build-deps.sh installed.
#
# Patches (smoke/patches, applied in this order, each must apply to the pristine pin):
#   janus-dtls13.patch           DTLS 1.3 + X25519MLKEM768 + AES-256-GCM-only policy
#                                (the feasibility-probe patch, byte for byte) and the
#                                "DTLS-POLICY ... ok=" log line the smoke test asserts
#   janus-websockets-notls.patch libwebsockets without a TLS library has no ssl_* fields
#
# Installs into $PREFIX/janus. Environment: BSSL (default /opt/boringssl),
# PREFIX (default /opt/qjanus-loadtest), SRC (scratch dir, default $HOME/qjl-src).
# Ubuntu 24.04 x86_64. Idempotent (always rebuilds Janus from the pin).
set -euxo pipefail

JANUS_TAG=v1.4.2
JANUS_SHA=0a24110ae55a172c4293749b763dbb66a138f9ec

HERE=$(cd "$(dirname "$0")" && pwd)
PATCHES=$(cd "$HERE/../smoke/patches" && pwd)
BSSL=${BSSL:-/opt/boringssl}
PREFIX=${PREFIX:-/opt/qjanus-loadtest}
SRC=${SRC:-$HOME/qjl-src}
NPROC=$(nproc)
JPREFIX=$PREFIX/janus
VERS=$PREFIX/versions.txt
LOG=$PREFIX/janus-configure.log
mkdir -p "$SRC" "$PREFIX"
touch "$VERS"

# -------------------------------------------------------------- fetch + patch
cd "$SRC"
rm -rf janus && mkdir janus && cd janus
git init -q .
git remote add origin https://github.com/meetecho/janus-gateway.git
git fetch -q --depth 1 origin "$JANUS_SHA"
git checkout -q FETCH_HEAD
test "$(git rev-parse HEAD)" = "$JANUS_SHA"
grep -v '^janus-gateway ' "$VERS" > "$VERS.tmp" || true
echo "janus-gateway $JANUS_TAG $JANUS_SHA" >> "$VERS.tmp"
mv "$VERS.tmp" "$VERS"

for p in janus-dtls13.patch janus-websockets-notls.patch; do
  git apply --check "$PATCHES/$p"
  git apply --stat "$PATCHES/$p"
  git apply "$PATCHES/$p"
done
sh autogen.sh

# --------------------------------------------------------------- configure
# never link a system libcurl (it would drag a system OpenSSL into the process)
export LIBCURL_CFLAGS=" " LIBCURL_LIBS=" "
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$BSSL/lib/pkgconfig"
export CPPFLAGS="-I$PREFIX/include"
export LDFLAGS="-L$PREFIX/lib -L$BSSL/lib -Wl,-rpath,$PREFIX/lib"
export LD_LIBRARY_PATH="$PREFIX/lib"
# static BoringSSL + static libsrtp: libcrypto has to come after libsrtp2 on the link line
export LIBS="-L$BSSL/lib -lssl -lcrypto -lstdc++ -lpthread -ldl -lm"
./configure --prefix="$JPREFIX" \
  --enable-boringssl="$BSSL" --enable-dtls-settimeout \
  --enable-rest --disable-all-transports --enable-websockets \
  --disable-all-plugins --enable-plugin-videoroom \
  --disable-all-handlers --disable-all-loggers \
  --disable-data-channels --disable-turn-rest-api --disable-post-processing \
  --disable-sample-event-handler --disable-docs 2>&1 | tee "$LOG" | tail -80

# The configure summary must say exactly what we intend to build.
bad=0
want() {                                   # want LABEL VALUE: summary line "LABEL: VALUE"
  local got
  got=$(awk -v k="$1:" '{ l = $0; sub(/^[ \t]+/, "", l)
                          if (index(l, k) == 1) { v = substr(l, length(k) + 1); gsub(/^[ \t]+|[ \t]+$/, "", v); print v; exit } }' "$LOG")
  if [ "$got" != "$2" ]; then echo "configure summary: '$1' is '$got', want '$2'"; bad=1; fi
}
want 'SSL/crypto library' BoringSSL
want 'DTLS set-timeout' yes
want 'DataChannels support' no
want 'Recordings post-processor' no
want 'TURN REST API client' no
want 'REST (HTTP/HTTPS)' yes
want 'WebSockets' yes
want 'RabbitMQ' no
want 'MQTT' no
want 'Unix Sockets' no
want 'Nanomsg' no
want 'Echo Test' no
want 'Streaming' no
want 'Video Call' no
want 'SIP Gateway' no
want 'NoSIP (RTP Bridge)' no
want 'Audio Bridge' no
want 'Video Room' yes
want 'Record&Play' no
want 'Text Room' no
want 'Lua Interpreter' no
want 'Duktape Interpreter' no
want 'Sample event handler' no
want 'WebSocket ev. handler' no
want 'RabbitMQ event handler' no
want 'MQTT event handler' no
want 'Nanomsg event handler' no
want 'GELF event handler' no
want 'JSON file logger' no
grep -Eq 'libsrtp version:[[:space:]]+2\.x' "$LOG" || { echo "configure summary: libsrtp 2.x not detected"; bad=1; }
[ "$bad" = 0 ] || exit 1

# ------------------------------------------------------------ build + install
make -j"$NPROC"
make install

# what got installed must be exactly the intended set of binaries and modules
# (libtool installs libjanus_x.so.N plus the symlinks libjanus_x.so that Janus loads)
find "$JPREFIX/lib/janus" -maxdepth 2 \( -type f -o -type l \) -name '*.so' | sort | tee "$PREFIX/janus-modules.txt"
sed "s#^$JPREFIX/##" "$PREFIX/janus-modules.txt" > "$PREFIX/janus-modules.rel"
cat > "$PREFIX/janus-modules.want" <<'LIST'
lib/janus/plugins/libjanus_videoroom.so
lib/janus/transports/libjanus_http.so
lib/janus/transports/libjanus_websockets.so
LIST
diff "$PREFIX/janus-modules.rel" "$PREFIX/janus-modules.want"

# -------------------------------------------------------------- linkage check
# Exactly one TLS implementation may live in the process: the static BoringSSL
# inside janus. Every dynamic dependency, resolved transitively, must exist and none
# may be a system libssl/libcrypto (or another TLS stack). That covers the
# websockets transport and, through it, libwebsockets.
check_linkage() {
  local f out rc=0
  for f in "$@"; do
    out=$(ldd "$f" 2>&1) || { echo "ERROR: ldd failed on $f"; echo "$out"; rc=1; continue; }
    if echo "$out" | grep 'not found'; then echo "ERROR: unresolved dependency of $f"; rc=1; fi
    if echo "$out" | grep -E 'libssl|libcrypto|libgnutls-openssl|libmbedtls|libwolfssl'; then
      echo "ERROR: $f links a TLS/crypto library besides the static BoringSSL"; rc=1
    fi
    if readelf -d "$f" | grep -E 'NEEDED.*(libssl|libcrypto)'; then
      echo "ERROR: $f has libssl/libcrypto in its NEEDED list"; rc=1
    fi
  done
  return "$rc"
}
LWS_SO=$(find "$PREFIX/lib" -maxdepth 1 -name 'libwebsockets.so*' -type f | sed -n 1p)
check_linkage "$JPREFIX/bin/janus" "$JPREFIX"/lib/janus/plugins/*.so "$JPREFIX"/lib/janus/transports/*.so "$LWS_SO"
echo "== ldd janus"; ldd "$JPREFIX/bin/janus" | tee "$PREFIX/janus-ldd.txt"
echo "== ldd websockets transport"; ldd "$JPREFIX/lib/janus/transports/libjanus_websockets.so" | tee "$PREFIX/janus-ws-ldd.txt"
grep -q libwebsockets "$PREFIX/janus-ws-ldd.txt"    # the transport really uses the libwebsockets we built

"$JPREFIX/bin/janus" --version | tee "$PREFIX/janus-version.txt"
cat "$VERS"
