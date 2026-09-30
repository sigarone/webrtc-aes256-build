#!/usr/bin/env bash
# Builds janus-gateway (pinned) against the BoringSSL / libsrtp / libnice /
# libwebsockets from build-deps.sh, after applying qjanus/patches/*.patch, with
# ONLY the VideoRoom plugin and the HTTP + WebSockets transports.
set -euxo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
QJ=$(cd "$HERE/.." && pwd)
# shellcheck source=pins.env
. "$HERE/pins.env"

BSSL=${BSSL:-/opt/boringssl}
PREFIX=${PREFIX:-/opt/qjanus}
SRC=${SRC:-$HOME/qjanus-src}
NPROC=$(nproc)
mkdir -p "$SRC"

cd "$SRC"
rm -rf janus && mkdir janus && cd janus
git init -q . && git remote add origin https://github.com/meetecho/janus-gateway.git
git fetch -q --depth 1 origin "$JANUS_SHA" && git checkout -q FETCH_HEAD
test "$(git rev-parse HEAD)" = "$JANUS_SHA"
echo "janus-gateway $JANUS_TAG $JANUS_SHA" >> "$PREFIX/versions.txt"

# patches, in order; every one must apply cleanly to the pristine pin
for p in "$QJ"/patches/*.patch; do
  git apply --check "$p"
  git apply --stat "$p"
  git apply "$p"
done
sh autogen.sh

# never link a system libcurl (it would drag a system OpenSSL into the process)
export LIBCURL_CFLAGS=" " LIBCURL_LIBS=" "
export PKG_CONFIG_PATH="$PREFIX/lib/pkgconfig:$BSSL/lib/pkgconfig"
export CPPFLAGS="-I$PREFIX/include"
export LDFLAGS="-L$PREFIX/lib -L$BSSL/lib -Wl,-rpath,$PREFIX/lib"
export LD_LIBRARY_PATH="$PREFIX/lib"
# static BoringSSL + static libsrtp: make sure libcrypto comes after libsrtp2 on the link line
export LIBS="-L$BSSL/lib -lssl -lcrypto -lstdc++ -lpthread -ldl -lm"
./configure --prefix="$PREFIX" \
  --enable-boringssl="$BSSL" --enable-dtls-settimeout \
  --disable-all-plugins --enable-plugin-videoroom \
  --disable-all-transports --enable-rest --enable-websockets \
  --disable-all-handlers --disable-all-loggers \
  --disable-data-channels --disable-turn-rest-api --disable-post-processing \
  --disable-docs 2>&1 | tee "$PREFIX/janus-configure.log" | tail -80

# the configure summary must say exactly what we intend to ship
python3 - "$PREFIX/janus-configure.log" <<'PY'
import re, sys
log = open(sys.argv[1]).read()
def val(label):
    m = re.search(r'^\s*' + re.escape(label) + r':\s*(\S.*?)\s*$', log, re.M)
    return m.group(1) if m else None
want = {
    'SSL/crypto library': 'BoringSSL', 'DTLS set-timeout': 'yes', 'DataChannels support': 'no',
    'Recordings post-processor': 'no', 'TURN REST API client': 'no',
    'REST (HTTP/HTTPS)': 'yes', 'WebSockets': 'yes', 'RabbitMQ': 'no', 'MQTT': 'no',
    'Unix Sockets': 'no', 'Nanomsg': 'no', 'Echo Test': 'no', 'Streaming': 'no', 'Video Call': 'no',
    'SIP Gateway': 'no', 'NoSIP (RTP Bridge)': 'no', 'Audio Bridge': 'no', 'Video Room': 'yes',
    'Record&Play': 'no', 'Text Room': 'no', 'Lua Interpreter': 'no', 'Duktape Interpreter': 'no',
    'Sample event handler': 'no', 'WebSocket ev. handler': 'no', 'RabbitMQ event handler': 'no',
    'MQTT event handler': 'no', 'Nanomsg event handler': 'no', 'GELF event handler': 'no',
    'JSON file logger': 'no',
}
bad = [f'{k}: got {val(k)!r}, want {v!r}' for k, v in want.items() if val(k) != v]
if 'libsrtp version:           2.x' not in log:
    bad.append('libsrtp 2.x not detected')
if bad:
    print('configure summary mismatch:\n  ' + '\n  '.join(bad)); sys.exit(1)
print('configure summary as intended')
PY

make -j"$NPROC"
make install

# what got installed must be exactly the intended set of binaries and modules
# libtool installs libjanus_x.so.2.0.11 plus the symlinks libjanus_x.so.2 and libjanus_x.so (what Janus loads)
find "$PREFIX/lib/janus" -maxdepth 2 \( -type f -o -type l \) -name "*.so" | sort | tee "$PREFIX/janus-modules.txt"
diff <(sed "s#^$PREFIX/##" "$PREFIX/janus-modules.txt") - <<'LIST'
lib/janus/plugins/libjanus_videoroom.so
lib/janus/transports/libjanus_http.so
lib/janus/transports/libjanus_websockets.so
LIST

# exactly one TLS implementation may live in the process: BoringSSL, static
"$HERE/check-linkage.sh" "$PREFIX/bin/janus" "$PREFIX"/lib/janus/plugins/*.so "$PREFIX"/lib/janus/transports/*.so
"$PREFIX/bin/janus" --version | tee -a "$PREFIX/versions.txt"
cat "$PREFIX/versions.txt"
