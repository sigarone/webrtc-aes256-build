#!/usr/bin/env bash
# smoke.sh - end-to-end smoke test of the load-test harness against a Janus built
# by scripts/build-deps.sh + scripts/build-janus.sh (DTLS 1.3 / ML-KEM / AES-256-GCM
# only, VideoRoom, token auth), all on one machine (CI: ubuntu-latest).
#
# What it does
#   1. renders the Janus configuration from smoke/conf (fresh random token secret,
#      admin key and id seed for this run; they live in the environment and in a
#      private 0700 temp dir only, are never printed, never put on a command line and
#      never written to $OUT) and starts Janus;
#   2. starts scripts/node-sampler.sh against the Janus pid (out/smoke/sampler.csv);
#   3. runs the steps below, one failing step never stops the others;
#   4. stops everything, writes sanitised logs (no IPs, candidates, fingerprints,
#      tokens, long hex ids), runs scripts/smoke-assert.mjs over the outputs and
#      writes $OUT/SUMMARY.md with a table of all steps.
# Exit status 0 only if every step and every assertion passed.
#
# Steps (SMOKE_ONLY=a,b re-runs just those; default all)
#   negative  scripts/smoke-negative.mjs: access control (tokens, admin key, join tokens, kick)
#   a         audio8  run,  1 room, hold 20 s
#   b         video4  run,  1 room, --video-profile lite, hold 20 s
#   c         video8  run,  1 room, --video-profile tiny, hold 15 s
#   d         audio8  ramp 1..2 rooms, hold 15 s: expects stopReason max_rooms, 2 rooms sustainable
#   e         audio8  ramp that must stop on the CPU limit at the first step (see step_e)
#   f         audio8  run of 2 rooms as two concurrent shards (0/2, 1/2, --start-at)
#   report    merge f0 + f1 + the sampler CSV with `qjanus-load report` (runs with f)
#   g         audio8  run with --manage-rooms ON (the harness creates and destroys its room)
#   i         remote mode as the workflow's shard step runs it: two concurrent scripts/run-shard.sh
#             processes (shards 0/2 and 1/2, audio8 ramp 1..2 rooms), then `qjanus-load report`
#   h         audio8  ramp under injected UDP loss: expects stopReason loss (see step_h). Runs LAST;
#             needs passwordless `sudo -n iptables`, otherwise it is reported as SKIP.
# Rooms for a..f are pre-created by `qjanus-admin create-rooms` (--manage-rooms OFF).
#
# Environment
#   PREFIX             build-deps.sh prefix, default /opt/qjanus-loadtest (Janus in $PREFIX/janus)
#   OUT                output dir, default <qjanus-loadtest>/out/smoke
#   SMOKE_ONLY         comma list of steps to run (negative,a,b,c,d,e,f,report,g,i,h)
#   SMOKE_ICE_LITE     0 switches Janus ICE lite off (default on)
#   SMOKE_DEBUG_LEVEL  Janus debug level (default 5; the DTLS-POLICY lines need >= 4)
#   SMOKE_STEP_TIMEOUT time limit in seconds of every load / negative run (default 600)
#
# Not run with `set -x` and not with `set -e` on purpose: secrets are in the
# environment, and failures are collected instead of aborting.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)                       # qjanus-loadtest/
PREFIX=${PREFIX:-/opt/qjanus-loadtest}
JROOT=$PREFIX/janus
OUT=${OUT:-$ROOT/out/smoke}
ICE_LITE=true; [ "${SMOKE_ICE_LITE:-1}" = 0 ] && ICE_LITE=false
DEBUG_LEVEL=${SMOKE_DEBUG_LEVEL:-5}
STEP_TIMEOUT=${SMOKE_STEP_TIMEOUT:-600}
ALL_STEPS="negative a b c d e f report g i h"
ONLY=${SMOKE_ONLY:-}

for s in ${ONLY//,/ }; do
  case " $ALL_STEPS " in *" $s "*) ;; *) echo "smoke: unknown step '$s' in SMOKE_ONLY (valid: ${ALL_STEPS// /,})" >&2; exit 2 ;; esac
done
wanted() {                                         # wanted STEP: is STEP selected?
  [ -z "$ONLY" ] && return 0
  case ",$ONLY," in *",$1,"*) return 0 ;; esac
  [ "$1" = report ] && case ",$ONLY," in *,f,*) return 0 ;; esac
  return 1
}

mkdir -p "$OUT/logs"
RUN=$(mktemp -d "${RUNNER_TEMP:-${TMPDIR:-/tmp}}/qjanus-smoke.XXXXXX")   # private: configs with secrets, raw logs
chmod 700 "$RUN"

# ------------------------------------------------------------------- secrets
rand_hex() { head -c "$1" /dev/urandom | od -An -tx1 | tr -d ' \n'; }
QJANUS_TOKEN_SECRET=$(rand_hex 32)
QJANUS_ADMIN_KEY=$(rand_hex 24)
QJANUS_LOADTEST_SEED=$(rand_hex 32)
if [ -n "${GITHUB_ACTIONS:-}" ]; then              # keep them out of the public log even if a tool echoed one
  echo "::add-mask::$QJANUS_TOKEN_SECRET"
  echo "::add-mask::$QJANUS_ADMIN_KEY"
  echo "::add-mask::$QJANUS_LOADTEST_SEED"
fi
# The client-facing WebSocket URL: Janus ignores the path, "/janus" mirrors the production proxy path.
export QJANUS_TOKEN_SECRET QJANUS_ADMIN_KEY QJANUS_LOADTEST_SEED
export QJANUS_WS_URL=ws://127.0.0.1:8188/janus
export QJANUS_ADMIN_URL=http://127.0.0.1:8088/janus
export LD_LIBRARY_PATH="$PREFIX/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# ------------------------------------------------------------------ sanitising
# Nothing that identifies the runner or a session may reach $OUT: candidates, IPv4
# addresses, mDNS names, handle ids, DTLS fingerprints, session tokens and every
# long hex string (room ids, seeds, secrets, join tokens).
sanitize() {
  grep --line-buffered -v -i 'candidate' \
    | sed -u -E 's/[0-9]{1,3}(\.[0-9]{1,3}){3}/<ip>/g;
                 s/[0-9a-fA-F]{8}-[0-9a-fA-F-]{27}\.local/<mdns>/g;
                 s/\[[0-9]{6,}\]/[h]/g;
                 s/([0-9A-Fa-f]{2}:){8,}[0-9A-Fa-f]{2}/<fp>/g;
                 s/[0-9]{9,},janus,[A-Za-z0-9_.,:+\/=-]+/<token>/g;
                 s/[0-9a-fA-F]{32,}/<hex>/g'
}

# ------------------------------------------------------------------ Janus setup
CONF=$RUN/etc
JPID=""
SAMPLER_PID=""
SAMPLERX_PID=""

render() {                                         # render TEMPLATE DEST: fill @KEYS@ (builtins only: no secret on any command line)
  local src=$1 dst=$2 line
  : > "$dst"
  while IFS= read -r line || [ -n "$line" ]; do
    line=${line//@JROOT@/$JROOT}
    line=${line//@CONFDIR@/$CONF}
    line=${line//@TOKEN_SECRET@/$QJANUS_TOKEN_SECRET}
    line=${line//@ADMIN_KEY@/$QJANUS_ADMIN_KEY}
    line=${line//@DEBUG_LEVEL@/$DEBUG_LEVEL}
    line=${line//@ICE_LITE@/$ICE_LITE}
    printf '%s\n' "$line" >> "$dst"
  done < "$src"
  chmod 600 "$dst"
}

start_janus() {
  mkdir -p "$CONF"
  local t
  for t in "$ROOT"/smoke/conf/*.jcfg.tmpl; do
    render "$t" "$CONF/$(basename "${t%.tmpl}")"
  done
  "$JROOT/bin/janus" -F "$CONF" -L "$RUN/janus.raw.log" > "$RUN/janus.stdout.raw" 2>&1 &
  JPID=$!
  local i up=0
  for i in $(seq 1 60); do
    # WebSocket port first, then the info: the HTTP transport answers before the WebSockets
    # transport is registered, and `info` lists only the transports registered so far
    if (exec 3<>/dev/tcp/127.0.0.1/8188) 2>/dev/null; then
      sleep 1
      if curl -sf --max-time 3 "$QJANUS_ADMIN_URL/info" > "$RUN/info.raw.json" 2>/dev/null; then up=1; break; fi
    fi
    kill -0 "$JPID" 2>/dev/null || break
    sleep 1
  done
  if [ "$up" != 1 ]; then
    echo "smoke: Janus did not come up (HTTP 8088 + WebSocket 8188)"
    tail -60 "$RUN/janus.stdout.raw" | sanitize || true
    return 1
  fi
  # keep only non-identifying fields of the info reply and check what got loaded
  node - "$RUN/info.raw.json" "$OUT/janus-info.json" <<'JS'
const fs = require('node:fs');
const [src, dst] = process.argv.slice(2);
const d = JSON.parse(fs.readFileSync(src, 'utf8'));
const keep = {
  name: d.name, version_string: d.version_string, 'dtls-mtu': d['dtls-mtu'], 'ice-lite': d['ice-lite'],
  'ice-tcp': d['ice-tcp'], auth_token: d.auth_token, api_secret: d.api_secret,
  transports: Object.keys(d.transports || {}), plugins: Object.keys(d.plugins || {}),
};
fs.writeFileSync(dst, `${JSON.stringify(keep, null, 1)}\n`);
console.log(`janus ${keep.version_string} (${keep.name}) transports=${keep.transports.join(',')} plugins=${keep.plugins.join(',')}`);
const problems = [];
if (!keep.transports.includes('janus.transport.websockets')) problems.push('WebSockets transport not loaded');
if (!keep.transports.includes('janus.transport.http')) problems.push('HTTP transport not loaded');
if (!keep.plugins.includes('janus.plugin.videoroom')) problems.push('VideoRoom plugin not loaded');
if (keep.plugins.length !== 1 || keep.transports.length !== 2) problems.push('unexpected modules loaded');
if (keep.auth_token !== true) problems.push('token authentication is not enabled');
if (keep.api_secret === true) problems.push('api_secret is enabled');
if (problems.length) { console.error(`smoke: ${problems.join('; ')}`); process.exit(1); }
JS
}

stop_janus() {
  [ -n "$JPID" ] || return 0
  kill -TERM "$JPID" 2>/dev/null || true
  local i
  for i in $(seq 1 15); do kill -0 "$JPID" 2>/dev/null || break; sleep 1; done
  kill -9 "$JPID" 2>/dev/null || true
  wait "$JPID" 2>/dev/null || true
  JPID=""
}

# a background job started here ignores SIGINT: stop the samplers with SIGTERM
stop_samplers() {
  local p
  for p in "$SAMPLER_PID" "$SAMPLERX_PID"; do
    [ -n "$p" ] || continue
    kill -TERM "$p" 2>/dev/null || true
    wait "$p" 2>/dev/null || true
  done
  SAMPLER_PID=""; SAMPLERX_PID=""
}

LOGS_DONE=0
finish_logs() {                                    # sanitised copies of the raw Janus logs
  [ "$LOGS_DONE" = 0 ] || return 0
  LOGS_DONE=1
  [ -f "$RUN/janus.raw.log" ] && sanitize < "$RUN/janus.raw.log" > "$OUT/janus-main.sanitized.log"
  [ -f "$RUN/janus.stdout.raw" ] && sanitize < "$RUN/janus.stdout.raw" > "$OUT/janus-stdout.sanitized.log"
  grep -h 'DTLS-POLICY' "$OUT/janus-main.sanitized.log" 2>/dev/null \
    | sed -E 's/^(\[[^]]*\] *)+//; s/ dtls_in=.*//' | sort | uniq -c | sort -rn > "$OUT/dtls-policy-summary.txt"
  cp "$PREFIX/versions.txt" "$OUT/versions.txt" 2>/dev/null || true
  cp "$PREFIX/janus-version.txt" "$OUT/janus-version.txt" 2>/dev/null || true
  cp "$PREFIX"/janus-ldd.txt "$PREFIX"/janus-ws-ldd.txt "$OUT/" 2>/dev/null || true
  return 0
}

CLEANED=0
cleanup() {
  [ "$CLEANED" = 0 ] || return 0
  CLEANED=1
  if [ -e "${LOSS_MARK:-}" ]; then loss_remove || echo "smoke: WARNING the loopback loss rule could not be removed"; fi
  stop_samplers
  stop_janus
  finish_logs
  rm -rf "$RUN"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

# ------------------------------------------------------------- step bookkeeping
SKIP_RC=77                                         # a step function returns this to be reported as SKIP (not FAIL)
S_IDS=(); S_TITLES=(); S_RCS=(); S_SECS=()
result_word() {                                    # result_word RC
  case $1 in 0) echo PASS ;; "$SKIP_RC") echo SKIP ;; *) echo "FAIL (exit $1)" ;; esac
}
run_step() {                                       # run_step ID "title" command...
  local id=$1 title=$2 rc t0=$SECONDS
  shift 2
  echo "::group::[$id] $title"
  { "$@" 2>&1; } | sanitize | tee "$OUT/logs/$id.log"
  rc=${PIPESTATUS[0]}
  echo "::endgroup::"
  echo "[$id] $title: $(result_word "$rc") after $((SECONDS - t0)) s"
  S_IDS+=("$id"); S_TITLES+=("$title"); S_RCS+=("$rc"); S_SECS+=("$((SECONDS - t0))")
  return 0
}

# Every node process is time limited (SMOKE_STEP_TIMEOUT); the harness stops gracefully on SIGTERM.
admin() { timeout -k 10 120 node "$ROOT/bin/qjanus-admin.mjs" "$@"; }
load()  { timeout -k 20 "$STEP_TIMEOUT" node "$ROOT/bin/qjanus-load.mjs" "$@"; }
admin_logged() {                                   # admin CLI with its output appended, sanitised, to logs/admin.log
  admin "$@" 2>&1 | sanitize >> "$OUT/logs/admin.log"
  return "${PIPESTATUS[0]}"
}
# rooms 0 and 1 with 8 seats each, always fresh: a participant a previous step left behind must not
# make the next step fail with "id exists" (this also exercises destroy-rooms and create-rooms every time)
ensure_rooms() {
  admin_logged destroy-rooms --rooms 2 && admin_logged create-rooms --rooms 2 --size 8
}

# Flags shared by every load run: strict transport (DTLS 1.3, AES-256-GCM), a fast join
# rate and a short settle. The Chromium field trials are the harness default (ML-KEM DTLS,
# all simulcast layers even for the small lite/tiny captures). A shared CI runner can stall a
# renderer for a few hundred ms (Chromium then counts a video freeze), so the smoke tolerates
# freezes: it tests the harness, not the runner.
COMMON=(--expect-transport strict --join-rate 4 --settle-sec 6 --freeze-tolerance 30)
CPU_REAL=(--cpu-file "$OUT/sampler.csv")

step_negative() { timeout -k 20 "$STEP_TIMEOUT" node "$HERE/smoke-negative.mjs" --out "$OUT/negative.json"; }
step_a() { ensure_rooms && load run --scenario audio8 --rooms 1 --hold-sec 20 --out "$OUT/a" --run-id smoke-a "${COMMON[@]}" "${CPU_REAL[@]}"; }
step_b() { ensure_rooms && load run --scenario video4 --video-profile lite --rooms 1 --hold-sec 20 --out "$OUT/b" --run-id smoke-b "${COMMON[@]}" "${CPU_REAL[@]}"; }
step_c() { ensure_rooms && load run --scenario video8 --video-profile tiny --rooms 1 --hold-sec 15 --out "$OUT/c" --run-id smoke-c "${COMMON[@]}" "${CPU_REAL[@]}"; }
step_d() { ensure_rooms && load ramp --scenario audio8 --ramp-start 1 --ramp-step 1 --ramp-max 2 --hold-sec 15 --out "$OUT/d" --run-id smoke-d "${COMMON[@]}" "${CPU_REAL[@]}"; }
# e: the smallest CPU limit the harness accepts is 1 (percent of one core). A second
# sampler runs with SAMPLER_CLK_TCK=1, which inflates cpu_pct by a factor of 100, so
# any real Janus CPU load breaches that limit and the ramp must stop at step 1.
step_e() { ensure_rooms && load ramp --scenario audio8 --ramp-start 1 --ramp-step 1 --ramp-max 2 --hold-sec 12 --out "$OUT/e" --run-id smoke-e "${COMMON[@]}" --cpu-file "$OUT/sampler-x100.csv" --cpu-limit 1; }
step_f() {                                         # two shards of a 2-room run, concurrently and in lock step
  ensure_rooms || return 1
  local start i rc=0
  local -a pids=()
  start=$(( $(date +%s) + 15 ))
  for i in 0 1; do
    load run --scenario audio8 --rooms 2 --shard "$i/2" --start-at "$start" --hold-sec 20 --out "$OUT/f$i" --run-id smoke-f \
      "${COMMON[@]}" "${CPU_REAL[@]}" > "$RUN/f$i.out" 2>&1 &
    pids+=($!)
  done
  for i in 0 1; do
    wait "${pids[$i]}" || rc=1
    sed "s/^/[shard $i] /" "$RUN/f$i.out"
  done
  return "$rc"
}
step_report() { load report --in "$OUT/f0" --in "$OUT/f1" --sampler "$OUT/sampler.csv" --out "$OUT/report"; }
step_g() {                                         # --manage-rooms ON: the harness must create and destroy its rooms
  admin_logged destroy-rooms --rooms 2 || return 1
  local rc=0
  load run --scenario audio8 --rooms 1 --hold-sec 12 --manage-rooms --admin-url "$QJANUS_ADMIN_URL" --admin-key-env QJANUS_ADMIN_KEY \
    --out "$OUT/g" --run-id smoke-g "${COMMON[@]}" "${CPU_REAL[@]}" || rc=1
  admin list-rooms --json > "$RUN/rooms-after-g.json" 2> "$RUN/rooms-after-g.err" || rc=1
  sanitize < "$RUN/rooms-after-g.err" >> "$OUT/logs/admin.log" || true
  mv -f "$RUN/rooms-after-g.json" "$OUT/rooms-after-g.json" || rc=1
  return "$rc"
}

# i: remote mode end to end. scripts/run-shard.sh is what the workflow's shard step runs; it gets the
# same environment variables here. With 2 shards and a ramp 1..2, step 1 has room 0 (shard 0 only) and
# step 2 adds room 1 (shard 1), so shard 1 owns no room in step 1. The smoke's own QJANUS_* variables
# (URL, token secret, seed) are already exported.
step_i() {
  ensure_rooms || return 1
  local start i rc=0
  local -a pids=()
  start=$(( $(date +%s) + 15 ))
  for i in 0 1; do
    SHARD=$i SHARDS=2 START_AT=$start RUN_ID=smoke-i SCENARIO=audio8 RUN_MODE=ramp       RAMP_START=1 RAMP_STEP=1 RAMP_MAX=2 HOLD_SEC=12 JOIN_RATE=4 SETTLE_SEC=6       VIDEO_PROFILE=spec BOTS_PER_BROWSER=16 OUT_DIR="$OUT/i$i" EXTRA_ARGS="--cpu-file $OUT/sampler.csv"       timeout -k 20 "$STEP_TIMEOUT" bash "$HERE/run-shard.sh" > "$RUN/i$i.out" 2>&1 &
    pids+=($!)
  done
  for i in 0 1; do
    wait "${pids[$i]}" || rc=1
    sed "s/^/[shard $i] /" "$RUN/i$i.out"
  done
  load report --in "$OUT/i0" --in "$OUT/i1" --sampler "$OUT/sampler.csv" --out "$OUT/report-i" || rc=1
  return "$rc"
}

# ---- step h: injected loss on the loopback interface, like the feasibility probe (withLoss)
# 6 % of all UDP datagrams delivered over `lo` are dropped (browser <-> Janus media and DTLS
# alike). The rule is ALWAYS removed again: at the end of the step, and from the EXIT/INT/TERM
# cleanup through the marker file (the step runs in a subshell, so a variable would be lost).
LOSS_RULE=(INPUT -i lo -p udp -m statistic --mode random --probability 0.06 -j DROP)
LOSS_MARK=$RUN/loss-rule-armed
loss_present() { sudo -n iptables -C "${LOSS_RULE[@]}" 2>/dev/null; }
loss_remove() {                                    # idempotent: delete every copy, up to 20
  local n=0
  while [ "$n" -lt 20 ] && loss_present; do
    sudo -n iptables -D "${LOSS_RULE[@]}" 2>/dev/null || break
    n=$((n + 1))
  done
  loss_present && return 1
  rm -f "$LOSS_MARK"
  return 0
}
step_h() {
  if ! sudo -n iptables -S INPUT > /dev/null 2>&1; then
    echo "SKIP: 'sudo -n iptables' is not usable here (needs passwordless sudo and iptables); step h only runs on CI runners"
    return "$SKIP_RC"
  fi
  ensure_rooms || return 1
  local rc=0
  : > "$LOSS_MARK"                                 # armed BEFORE the rule exists, so no window without a cleanup
  if sudo -n iptables -I "${LOSS_RULE[@]}"; then
    echo "loopback UDP loss rule installed (6 %)"
    # default cpu limit (180), so CPU cannot be the reason; DTLS handshakes must survive the loss
    load ramp --scenario audio8 --ramp-start 1 --ramp-step 1 --ramp-max 2 --hold-sec 25 --breach-windows 2 --loss-limit-pct 1       --join-timeout-sec 90 --out "$OUT/h" --run-id smoke-h "${COMMON[@]}" "${CPU_REAL[@]}" || rc=1
  else
    echo "ERROR: could not install the loopback loss rule"; rc=1
  fi
  if loss_remove; then echo "loopback UDP loss rule removed"; else echo "ERROR: the loopback UDP loss rule is still installed"; rc=1; fi
  return "$rc"
}

# ------------------------------------------------------------------------ run
echo "== smoke: starting Janus"
rc_start=0
start_janus || rc_start=1
if [ "$rc_start" != 0 ]; then
  finish_logs
  exit 1
fi
echo "== janus build"; cat "$PREFIX/versions.txt" 2>/dev/null || true

echo "== smoke: starting the node sampler"
bash "$HERE/node-sampler.sh" --pid "$JPID" --out "$OUT/sampler.csv" --interval 2 --latest "$OUT/sampler-latest.json" --quiet &
SAMPLER_PID=$!
if wanted e; then
  SAMPLER_CLK_TCK=1 bash "$HERE/node-sampler.sh" --pid "$JPID" --out "$OUT/sampler-x100.csv" --interval 2 --quiet &
  SAMPLERX_PID=$!
fi
sleep 3                                            # a few baseline rows before the first load

for id in negative a b c d e f report g i h; do
  wanted "$id" || continue
  case $id in
    negative) run_step negative "access control: tokens, admin key, join tokens, kick" step_negative ;;
    a) run_step a "audio8 run, 1 room, 20 s" step_a ;;
    b) run_step b "video4 run, 1 room, lite, 20 s" step_b ;;
    c) run_step c "video8 run, 1 room, tiny, 15 s" step_c ;;
    d) run_step d "audio8 ramp 1..2 rooms (expects max_rooms)" step_d ;;
    e) run_step e "audio8 ramp stopped by the CPU limit" step_e ;;
    f) run_step f "audio8 run, 2 rooms as 2 concurrent shards" step_f ;;
    report) run_step report "merge shards + sampler into a report" step_report ;;
    g) run_step g "audio8 run with --manage-rooms" step_g ;;
    i) run_step i "remote-mode shards via run-shard.sh (ramp 1..2 rooms, 2 shards)" step_i ;;
    h) run_step h "audio8 ramp under injected UDP loss (expects stopReason loss)" step_h ;;
  esac
done

echo "== smoke: cleaning up"
admin_logged destroy-rooms --rooms 2 || true
sleep 2                                            # a couple of idle sampler rows
stop_samplers
stop_janus
finish_logs

echo "== sampler summary"
bash "$HERE/sampler-summary.sh" "$OUT/sampler.csv" | tee "$OUT/sampler-summary.txt" || true
bash "$HERE/sampler-summary.sh" --json "$OUT/sampler.csv" > "$OUT/sampler-summary.json" || true

# ------------------------------------------------------------------ assertions
ASSERT_STEPS=""
for id in negative a b c d e f g i h; do
  wanted "$id" || continue
  skipped=0
  for n in "${!S_IDS[@]}"; do [ "${S_IDS[$n]}" = "$id" ] && [ "${S_RCS[$n]}" = "$SKIP_RC" ] && skipped=1; done
  [ "$skipped" = 1 ] || ASSERT_STEPS="$ASSERT_STEPS,$id"
done
ASSERT_STEPS=${ASSERT_STEPS#,}
run_step assert "assert the outputs (smoke-assert.mjs)" \
  node "$HERE/smoke-assert.mjs" --out "$OUT" --steps "$ASSERT_STEPS" --janus-log "$OUT/janus-main.sanitized.log" --json "$OUT/assert.json"

# --------------------------------------------------------------------- summary
FAILED=0
{
  echo "# qjanus load-test smoke run"
  echo
  echo "| step | what | result | seconds |"
  echo "| --- | --- | --- | --- |"
  for n in "${!S_IDS[@]}"; do
    res=$(result_word "${S_RCS[$n]}")
    echo "| ${S_IDS[$n]} | ${S_TITLES[$n]} | $res | ${S_SECS[$n]} |"
  done
  echo
  echo "## Runs"
  node - "$OUT" <<'JS'
const fs = require('node:fs');
const path = require('node:path');
const out = process.argv[2];
const rows = [];
for (const d of ['a', 'b', 'c', 'd', 'e', 'f0', 'f1', 'g', 'i0', 'i1', 'h']) {
  const f = path.join(out, d, 'summary.json');
  if (!fs.existsSync(f)) continue;
  const s = JSON.parse(fs.readFileSync(f, 'utf8'));
  const m = s.maxSustainable ? `${s.maxSustainable.rooms} rooms` : '-';
  rows.push(`| ${d} | ${s.scenario} ${s.mode} | ${s.ok} | ${s.stopReason ?? '-'} | ${m} | ${s.bots.steady}/${s.bots.attempted} | ${s.media.lossPct} | ${s.media.audioOutPps.p50} | ${s.join.p95Ms ?? '-'} |`);
}
if (rows.length) {
  console.log('| dir | scenario | ok | stopReason | max sustainable | steady/bots | loss % | audio pps p50 | join p95 ms |');
  console.log('| --- | --- | --- | --- | --- | --- | --- | --- | --- |');
  console.log(rows.join('\n'));
}
JS
  echo
  echo "## Janus"
  echo '```'
  head -5 "$OUT/janus-version.txt" 2>/dev/null
  cat "$OUT/versions.txt" 2>/dev/null
  echo '```'
  echo
  echo "DTLS-POLICY lines seen by Janus (count, negotiated parameters):"
  echo '```'
  cat "$OUT/dtls-policy-summary.txt" 2>/dev/null
  echo '```'
  echo
  echo "## Sampler (Janus process and host)"
  echo '```'
  cat "$OUT/sampler-summary.txt" 2>/dev/null
  echo '```'
} > "$OUT/SUMMARY.md"

for rc in "${S_RCS[@]}"; do [ "$rc" = 0 ] || [ "$rc" = "$SKIP_RC" ] || FAILED=1; done
echo
if [ "$FAILED" = 0 ]; then echo "== smoke: ALL STEPS PASSED"; else echo "== smoke: FAILED (see $OUT/SUMMARY.md)"; fi
exit "$FAILED"
