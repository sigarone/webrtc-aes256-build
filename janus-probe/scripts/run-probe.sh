#!/usr/bin/env bash
# Starts a patched Janus, runs the Playwright client suites against it, stops
# Janus and writes sanitised logs (no IPs, no candidates) to $OUT.
#   main         -> patched Janus (janus-dtls13.patch)
#   soak-fixed   -> the same binary, loss soak
#   soak-legacy  -> A/B baseline: same patch minus the post-handshake timer servicing
set -uo pipefail

PREFIX=${PREFIX:-/opt/janus-probe}
PROBE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$PROBE/out}
mkdir -p "$OUT"
export LD_LIBRARY_PATH="$PREFIX/lib"

JPID=""
start_janus() { # $1 = install root, $2 = tag
  local jroot=$1 tag=$2
  mkdir -p "$jroot/etc/janus"
  for f in "$PROBE"/conf/*.jcfg; do
    sed "s#@JROOT@#$jroot#g" "$f" > "$jroot/etc/janus/$(basename "$f")"
  done
  if [ -n "${MTU:-}" ]; then sed -i "s/dtls_mtu = .*/dtls_mtu = $MTU/" "$jroot/etc/janus/janus.jcfg"; fi
  RAW="$OUT/janus-$tag.raw.log"; : > "$RAW"
  "$jroot/bin/janus" -F "$jroot/etc/janus" -L "$RAW" > "$OUT/janus-$tag.stdout.raw" 2>&1 &
  JPID=$!
  local up=0
  for i in $(seq 1 40); do
    if curl -sf http://127.0.0.1:8088/janus/info > "$OUT/janus-info-$tag.json"; then up=1; break; fi
    kill -0 "$JPID" 2>/dev/null || break
    sleep 1
  done
  if [ "$up" != 1 ]; then echo "Janus ($tag) did not come up"; tail -80 "$OUT/janus-$tag.stdout.raw" || true; return 1; fi
  python3 - "$OUT/janus-info-$tag.json" <<'PY' || true
import json,sys
d=json.load(open(sys.argv[1]))
print("janus info:", json.dumps({k:d.get(k) for k in ('name','version_string','dtls-mtu','ice-lite','ipv6')}))
PY
  return 0
}
stop_janus() {
  [ -n "$JPID" ] || return 0
  kill -INT "$JPID" 2>/dev/null || true
  for i in $(seq 1 10); do kill -0 "$JPID" 2>/dev/null || break; sleep 1; done
  kill -9 "$JPID" 2>/dev/null || true
  wait "$JPID" 2>/dev/null || true
  JPID=""
}
sanitize() {
  grep -v -i 'candidate' "$1" \
    | sed -E 's/[0-9]{1,3}(\.[0-9]{1,3}){3}/<ip>/g; s/[0-9a-fA-F]{8}-[0-9a-fA-F-]{27}\.local/<mdns>/g; s/\[[0-9]{6,}\]/[h]/g' \
    | sed -E 's/([0-9A-F]{2}:){8,}[0-9A-F]{2}/<fp>/g'
}

run_suite() { # $1 = suite, $2 = install root, $3 = tag
  local suite=$1 jroot=$2 tag=$3 rc=0
  echo "################ suite=$suite janus=$tag"
  if start_janus "$jroot" "$tag"; then
    mkdir -p "$OUT/$tag"
    (cd "$PROBE/client" && SUITE="$suite" JANUS_PID="$JPID" JANUS_LOG="$RAW" OUT_DIR="$OUT/$tag" node probe.mjs)
    rc=$?
  else
    rc=3
  fi
  stop_janus
  sanitize "$RAW" > "$OUT/janus-$tag.sanitized.log" || true
  rm -f "$RAW" "$OUT/janus-$tag.stdout.raw"
  return $rc
}

RC=0
run_suite main   "$PREFIX/janus"              main         || RC=1
MTU=600 ONLY=A1,B1 run_suite main "$PREFIX/janus" mtu600 || RC=1
run_suite soak   "$PREFIX/janus"              soak-fixed   || true
if [ -x "$PREFIX/janus-legacy-timer/bin/janus" ]; then
  run_suite soak "$PREFIX/janus-legacy-timer" soak-legacy  || true
fi

cat "$OUT"/janus-*.sanitized.log | grep -E 'Crypto:|BoringSSL|DTLS|dtls|Handshake|SRTP|srtp|retransmi|alert' > "$OUT/janus-dtls-lines.log" || true
grep -h 'DTLS-POLICY' "$OUT/janus-main.sanitized.log" | sed -E 's/^\[[^]]*\] *//' | sort | uniq -c | sort -rn > "$OUT/dtls-policy-summary.txt" || true
cp "$PREFIX/versions.txt" "$OUT/versions.txt" 2>/dev/null || true
cp "$PREFIX"/janus-ldd*.txt "$OUT/" 2>/dev/null || true

echo "=================== DTLS-POLICY lines (Janus side, main suite) ==================="
cat "$OUT/dtls-policy-summary.txt" || true
echo "=================== main suite ==================="
cat "$OUT/main/summary.txt" 2>/dev/null || echo "(no summary)"
echo "=================== small DTLS MTU (600) ==================="
cat "$OUT/mtu600/summary.txt" 2>/dev/null || true
echo "=================== loss soak: patched (timer serviced after connect) ==================="
grep SOAK "$OUT/soak-fixed/summary.txt" 2>/dev/null || true
echo "=================== loss soak: legacy timer (stock janus_dtls_retry) ==================="
grep SOAK "$OUT/soak-legacy/summary.txt" 2>/dev/null || true
{
  echo "main suite"; cat "$OUT/main/summary.txt" 2>/dev/null
  echo; echo "dtls_mtu=600"; cat "$OUT/mtu600/summary.txt" 2>/dev/null
  echo; echo "loss soak, patched"; grep SOAK "$OUT/soak-fixed/summary.txt" 2>/dev/null
  echo; echo "loss soak, legacy timer"; grep SOAK "$OUT/soak-legacy/summary.txt" 2>/dev/null
} > "$OUT/summary.txt"
exit "$RC"
