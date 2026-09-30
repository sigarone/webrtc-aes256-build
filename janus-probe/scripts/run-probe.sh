#!/usr/bin/env bash
# Starts the patched Janus, runs the Playwright client scenarios against it,
# then stops Janus and writes sanitised logs (no IPs) to $OUT.
set -uo pipefail

PREFIX=${PREFIX:-/opt/janus-probe}
JROOT=$PREFIX/janus
PROBE=$(cd "$(dirname "$0")/.." && pwd)
OUT=${OUT:-$PROBE/out}
mkdir -p "$OUT" "$JROOT/etc/janus"

for f in "$PROBE"/conf/*.jcfg; do
  sed "s#@JROOT@#$JROOT#g" "$f" > "$JROOT/etc/janus/$(basename "$f")"
done

export LD_LIBRARY_PATH="$PREFIX/lib"
RAW="$OUT/janus.raw.log"
: > "$RAW"
"$JROOT/bin/janus" -F "$JROOT/etc/janus" -L "$RAW" > "$OUT/janus.stdout.raw" 2>&1 &
JPID=$!

up=0
for i in $(seq 1 40); do
  if curl -sf http://127.0.0.1:8088/janus/info > "$OUT/janus-info.json"; then up=1; break; fi
  if ! kill -0 "$JPID" 2>/dev/null; then break; fi
  sleep 1
done
if [ "$up" != 1 ]; then
  echo "Janus did not come up"; tail -80 "$OUT/janus.stdout.raw" || true
  RC=3
else
  python3 - "$OUT/janus-info.json" <<'PY' || true
import json,sys
d=json.load(open(sys.argv[1]))
keep={k:d.get(k) for k in ('name','version_string','git-commit','ssl-version','dtls-mtu','ice-lite','ipv6','api_secret')}
print("janus info:", json.dumps({k:v for k,v in keep.items() if k!='api_secret'}))
PY
  (cd "$PROBE/client" && JANUS_LOG="$RAW" OUT_DIR="$OUT" node probe.mjs)
  RC=$?
fi

kill -INT "$JPID" 2>/dev/null || true
for i in $(seq 1 10); do kill -0 "$JPID" 2>/dev/null || break; sleep 1; done
kill -9 "$JPID" 2>/dev/null || true

# ---- sanitised copies (public repo/artifacts: no IPs, no candidate lines)
sanitize() {
  grep -v -i 'candidate' "$1" \
    | sed -E 's/[0-9]{1,3}(\.[0-9]{1,3}){3}/<ip>/g; s/[0-9a-fA-F]{8}-[0-9a-fA-F-]{27}\.local/<mdns>/g; s/\[[0-9]{6,}\]/[h]/g' \
    | sed -E 's/(fingerprint|Fingerprint)[^:]*:? *([0-9A-F]{2}:){8,}[0-9A-F]{2}/\1 <fp>/g'
}
sanitize "$RAW" > "$OUT/janus.sanitized.log" || true
grep -E 'Crypto:|BoringSSL|DTLS|dtls|Handshake|SRTP|srtp|retransmi|alert' "$OUT/janus.sanitized.log" > "$OUT/janus-dtls-lines.log" || true
grep 'DTLS-POLICY' "$OUT/janus.sanitized.log" | sed -E 's/^\[[^]]*\] *//' | sort | uniq -c | sort -rn > "$OUT/dtls-policy-summary.txt" || true
rm -f "$OUT/janus.raw.log" "$OUT/janus.stdout.raw"
cp "$PREFIX/versions.txt" "$OUT/versions.txt" 2>/dev/null || true
cp "$PREFIX/janus-ldd.txt" "$OUT/janus-ldd.txt" 2>/dev/null || true

echo "=================== DTLS-POLICY lines (Janus side) ==================="
cat "$OUT/dtls-policy-summary.txt" || true
echo "=================== probe summary ==================="
cat "$OUT/summary.txt" 2>/dev/null || echo "(no summary)"
exit "$RC"
