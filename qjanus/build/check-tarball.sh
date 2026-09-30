#!/usr/bin/env bash
# check-tarball.sh <qjanus tarball> [<repo qjanus/build dir>]
# Runs inside a CLEAN ubuntu:24.04 container (no compiler, no -dev packages): proves that the tarball
#  - needs exactly the packages in its apt-deps.txt (nothing else is installed but curl/jq for the test),
#  - is relocatable (unpacked into an arbitrary directory, no LD_LIBRARY_PATH),
#  - links no system libssl/libcrypto, resolves every library,
#  - starts with the rendered production templates and loads exactly VideoRoom + HTTP + WebSockets,
#  - reports the pinned dependency versions.
# docker run --rm -v "$PWD:/repo:ro" -v "$OUT:/out:ro" ubuntu:24.04 /repo/qjanus/build/check-tarball.sh /out/qjanus-....tar.gz
set -euo pipefail
TARBALL=${1:?tarball}
BUILD_DIR=${2:-$(cd "$(dirname "$0")" && pwd)}
export DEBIAN_FRONTEND=noninteractive
ok() { echo "OK   $*"; }
fail() { echo "FAIL $*"; exit 1; }

apt-get update -qq
apt-get install -y -qq --no-install-recommends ca-certificates curl jq > /dev/null

mkdir -p /tmp/a/b/c && tar -xzf "$TARBALL" -C /tmp/a/b/c
ROOT=/tmp/a/b/c/qjanus
test -x "$ROOT/bin/janus" || fail "bin/janus missing"
test -s "$ROOT/apt-deps.txt" || fail "apt-deps.txt missing"
echo "runtime packages:"; cat "$ROOT/apt-deps.txt"
# shellcheck disable=SC2046
apt-get install -y -qq --no-install-recommends $(cat "$ROOT/apt-deps.txt") > /dev/null
ok "the packages of apt-deps.txt install on a clean ubuntu:24.04"

env -u LD_LIBRARY_PATH "$BUILD_DIR/check-linkage.sh" "$ROOT/bin/janus" "$ROOT"/lib/janus/*/*.so "$ROOT"/lib/*.so.*
ok "relocated tree: every library resolves, no system libssl/libcrypto"
env -u LD_LIBRARY_PATH "$ROOT/bin/janus" --version | sed -n '1,12p'

# a node-like config: the production templates rendered exactly as the unit does
export QJANUS_TOKEN_SECRET=$(openssl rand -hex 32) QJANUS_ADMIN_KEY=$(openssl rand -hex 32) QJANUS_HTTP_BIND=127.0.0.1
mkdir -p /tmp/node && cd /tmp/node
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 30 -subj "/CN=qjanus" \
  -keyout dtls.key -out dtls.crt 2> /dev/null
export QJANUS_DTLS_CERT=/tmp/node/dtls.crt QJANUS_DTLS_KEY=/tmp/node/dtls.key
"$ROOT/libexec/qjanus-render-config" /tmp/node/etc
ls -l /tmp/node/etc
grep -q "$QJANUS_TOKEN_SECRET" /tmp/node/etc/janus.jcfg || fail "token secret not rendered"
[ "$(stat -c %a /tmp/node/etc/janus.jcfg)" = 600 ] || fail "rendered config is not 0600"
ok "templates render"
grep -q 'nat_1_1_mapping = ' /tmp/node/etc/janus.jcfg && fail "nat_1_1_mapping must be off by default"
QJANUS_NAT_1_1=192.0.2.7 "$ROOT/libexec/qjanus-render-config" /tmp/node/etc-nat 2> /dev/null
grep -q '^	nat_1_1_mapping = "192.0.2.7"$' /tmp/node/etc-nat/janus.jcfg || fail "QJANUS_NAT_1_1 is not rendered"
if QJANUS_NAT_1_1='1.2.3.4"; x' "$ROOT/libexec/qjanus-render-config" /tmp/node/etc-bad 2> /dev/null; then fail "an invalid QJANUS_NAT_1_1 must be refused"; fi
if QJANUS_HTTP_BIND='1.2.3.4|x' "$ROOT/libexec/qjanus-render-config" /tmp/node/etc-bad 2> /dev/null; then fail "an invalid QJANUS_HTTP_BIND must be refused"; fi
ok "optional 1:1 NAT mapping renders, invalid settings are refused"

run_janus() { # $1 = config dir, $2 = log
  env -u LD_LIBRARY_PATH "$ROOT/bin/janus" -F "$1" > "$2" 2>&1 &
  JPID=$!
  for _ in $(seq 1 40); do
    curl -sf http://127.0.0.1:8088/janus/info > /tmp/node/info.json && return 0
    kill -0 "$JPID" 2> /dev/null || break
    sleep 0.5
  done
  cat "$2"; fail "janus did not come up"
}
stop_janus() { kill -INT "$JPID" 2> /dev/null || true; wait "$JPID" 2> /dev/null || true; }

run_janus /tmp/node/etc /tmp/node/janus.log
jq -c '{name,version_string,plugins:(.plugins|keys),transports:(.transports|keys),data_channels,"ice-lite","ice-tcp",ipv6,"dtls-mtu","session-timeout","reclaim-session-timeout","min-nack-queue","twcc-period","auth_token","api_secret",dependencies}' /tmp/node/info.json
jq -e '.version_string == "1.4.2"' /tmp/node/info.json > /dev/null || fail "version"
jq -e '(.plugins | keys) == ["janus.plugin.videoroom"]' /tmp/node/info.json > /dev/null || fail "plugin set"
jq -e '(.transports | keys) == ["janus.transport.http","janus.transport.websockets"]' /tmp/node/info.json > /dev/null || fail "transport set"
jq -e '.data_channels == false and .["ice-lite"] == true and .["ice-tcp"] == false and .ipv6 == true' /tmp/node/info.json > /dev/null || fail "ice/data-channel flags"
jq -e '.["dtls-mtu"] == 1200 and .["session-timeout"] == 60 and .["reclaim-session-timeout"] == 20 and .["min-nack-queue"] == 500 and .["twcc-period"] == 200' /tmp/node/info.json > /dev/null || fail "media settings"
jq -e '.auth_token == true and .api_secret == false' /tmp/node/info.json > /dev/null || fail "auth flags"
jq -e 'has("dependencies") | not' /tmp/node/info.json > /dev/null || fail "dependencies are not hidden"
ok "info: VideoRoom + HTTP + WebSockets only, hardened core settings, dependencies hidden"
# no libssl/libcrypto mapped into the running process
if grep -E 'libssl|libcrypto' /proc/$JPID/maps; then fail "a system libssl/libcrypto is mapped into the process"; fi
ok "no libssl/libcrypto mapped into the running process"
if grep -E 'FATAL' /tmp/node/janus.log; then fail "fatal errors at startup"; fi
grep -E 'WARN|ERR' /tmp/node/janus.log || true
stop_janus

# the pinned dependency versions, visible only with hide_dependencies = false
mkdir -p /tmp/node/etc2 && cp /tmp/node/etc/* /tmp/node/etc2/
sed -i 's/hide_dependencies = true/hide_dependencies = false/; s#/tmp/node/etc#/tmp/node/etc2#' /tmp/node/etc2/janus.jcfg
run_janus /tmp/node/etc2 /tmp/node/janus2.log
jq -c .dependencies /tmp/node/info.json
jq -e '.dependencies.crypto | test("BoringSSL")' /tmp/node/info.json > /dev/null || fail "crypto library is not BoringSSL"
jq -e '.dependencies.libsrtp | test("2\\.8\\.1")' /tmp/node/info.json > /dev/null || fail "libsrtp is not 2.8.1"
jq -e '.dependencies.libnice | test("0\\.1\\.24")' /tmp/node/info.json > /dev/null || fail "libnice is not 0.1.24"
ok "dependencies: BoringSSL, libsrtp 2.8.1, libnice 0.1.24"
stop_janus
echo "check-tarball: all checks passed"
