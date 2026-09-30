#!/usr/bin/env bash
# caddy-check.sh: proves the Caddy block of the README (conf/Caddyfile.example) in front of the INSTALLED
# node: TLS on 443-style port 8443 with Caddy's own certificate, path /janus -> 127.0.0.1:8188 with the
# WebSocket upgrade, everything else 404, and the server-facing HTTP API (8088) not reachable through it.
# Then the client-facing API conformance suite runs through wss://.../janus.
# Needs: sudo, the installed qjanus service, node, env QJANUS_FINGERPRINT, OUT_DIR, RUNNER_TEMP.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
EX=$HERE/../conf/Caddyfile.example
TMP=${RUNNER_TEMP:-/tmp}
OUT=${OUT_DIR:-$TMP}

sudo apt-get install -y --no-install-recommends caddy > /dev/null
sudo systemctl disable --now caddy > /dev/null 2>&1 || true

# global options for a throw-away instance + the example block with the test host name
{
  echo '{'
  echo '	admin off'
  echo '	auto_https disable_redirects'
  echo '}'
  sed -e 's|^sfu\.example\.invalid {|https://localhost:8443 {|' -e '/^https:\/\/localhost:8443 {/a\
	tls internal' "$EX"
} > "$TMP/Caddyfile"
cat "$TMP/Caddyfile"
grep -q '^https://localhost:8443 {' "$TMP/Caddyfile"
grep -q 'reverse_proxy 127.0.0.1:8188' "$TMP/Caddyfile"
sudo caddy validate --config "$TMP/Caddyfile" --adapter caddyfile

sudo nohup caddy run --config "$TMP/Caddyfile" --adapter caddyfile > "$TMP/caddy.log" 2>&1 &
up=0
for _ in $(seq 1 60); do
  if (exec 3<> /dev/tcp/127.0.0.1/8443) 2> /dev/null; then up=1; break; fi
  sleep 0.5
done
[ "$up" = 1 ] || { cat "$TMP/caddy.log"; echo "::error::caddy did not come up"; exit 1; }

# only /janus reaches the WebSocket API, the HTTP API is not reachable through Caddy at all
[ "$(curl -sk -o /dev/null -w '%{http_code}' https://localhost:8443/)" = 404 ] || { echo "::error::/ must be 404"; exit 1; }
if curl -sk https://localhost:8443/janus/info | grep -q server_info; then echo "::error::the HTTP API is reachable through Caddy"; exit 1; fi
if curl -sk https://localhost:8443/admin/info | grep -q server_info; then echo "::error::the admin path is reachable through Caddy"; exit 1; fi

cd "$HERE/conformance"
set -a; . <(sudo cat /etc/qjanus/qjanus.env); set +a
echo "::add-mask::$QJANUS_TOKEN_SECRET"; echo "::add-mask::$QJANUS_ADMIN_KEY"
rc=0
NODE_TLS_REJECT_UNAUTHORIZED=0 QJANUS_WS=wss://localhost:8443/janus QJANUS_SLOW=0 \
  OUT_DIR="$OUT/conformance-caddy" SECRETS_FILE="$TMP/secrets-caddy.json" \
  node conformance.mjs 2>&1 | tee "$OUT/conformance-caddy.log" | grep -E '^(PASS|FAIL|conformance:)|^      ' || true
grep -Eq '^conformance: ([0-9]+)/\1 passed' "$OUT/conformance-caddy.log" || rc=1
sudo pkill -x caddy || true
[ "$rc" = 0 ] || { echo "::error::the conformance suite failed through Caddy"; exit 1; }
echo "caddy-check: ok"
