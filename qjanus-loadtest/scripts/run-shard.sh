#!/usr/bin/env bash
# run-shard.sh - run ONE shard of a remote load test.
#
# The only place that turns the environment of the workflow's `shard` step (or of
# an operator's shell) into a `qjanus-load` command line; the smoke run (step i)
# calls it the same way. Every value is validated (number / enum / charset) and put
# into a quoted array: nothing is eval'ed, nothing is expanded by the shell twice.
# Secrets are read by qjanus-load from the environment and are never echoed.
#
# Usage: [VAR=value ...] scripts/run-shard.sh        (run from any directory)
#
# Required environment
#   QJANUS_WS_URL          Janus WebSocket URL          } read by qjanus-load, never printed
#   QJANUS_TOKEN_SECRET    core token secret            } ONE of these two: the secret (tokens are minted here)
#   QJANUS_SESSION_TOKENS  JSON array, one pre-minted    } or pre-minted session tokens, one per bot (the secret
#                          session token per bot         } then stays on the node; when both are set the tokens win)
#   QJANUS_LOADTEST_SEED   id seed                      }
#   SHARD, SHARDS          this shard (0-based) and the shard count, SHARD < SHARDS
#   START_AT               epoch seconds at which all shards start their first step
#   RUN_ID                 run id shared by all shards: letters, digits, . _ - (max 80)
#   SCENARIO               audio8 | video4 | video8
#   RUN_MODE               run | ramp
#   HOLD_SEC, JOIN_RATE    measurement window (seconds), bots started per second
#   VIDEO_PROFILE          spec | lite | tiny
#   BOTS_PER_BROWSER       bots per Chromium process
#   ROOMS                  (RUN_MODE=run) rooms, all shards together
#   RAMP_START, RAMP_STEP, RAMP_MAX   (RUN_MODE=ramp) rooms per step
# Optional environment
#   OUT_DIR                output directory, default out/shard-$SHARD (relative to qjanus-loadtest/)
#   SETTLE_SEC             seconds between the last join and the measurement window
#   EXTRA_ARGS             extra qjanus-load flags as one space separated string, one word per
#                          flag or flag value (for the smoke run, e.g. "--cpu-file /path/x.csv");
#                          words may only contain letters, digits and . _ / : = , + @ % -
#
# Fixed flags: --expect-transport strict, --field-trials WebRTC-EnableDtlsPqc/Enabled/.
# No --manage-rooms: the Janus HTTP API is node-local, so the rooms of the seed must
# already exist on the node. Ends in `exec node bin/qjanus-load.mjs`, so its exit code is
# qjanus-load's (0 ok, 1 not ok, 2 usage / configuration error).
set -euo pipefail

case ${1:-} in
  '') ;;
  -h|--help) sed -n '2,/^set -euo/{/^#/s/^# \{0,1\}//p}' "$0"; exit 0 ;;
  *) echo "run-shard: no arguments are accepted, configuration is by environment (see --help)" >&2; exit 2 ;;
esac

ROOT=$(cd "$(dirname "$0")/.." && pwd)                       # qjanus-loadtest/

bad() { echo "run-shard: $*" >&2; exit 2; }
# The variable NAME is reported, never its value (some are secrets or URLs).
need() { [ -n "${!1:-}" ] || bad "environment variable $1 is not set or empty"; }
is_int() { need "$1"; [[ ${!1} =~ ^[0-9]+$ ]] || bad "$1 must be a whole number"; }
is_num() { need "$1"; [[ ${!1} =~ ^[0-9]+([.][0-9]+)?$ ]] || bad "$1 must be a number"; }
is_enum() {                                                   # is_enum NAME allowed...
  local name=$1 v w ok=0
  need "$name"
  v=${!name}
  shift
  for w in "$@"; do [ "$v" = "$w" ] && ok=1; done
  [ "$ok" = 1 ] || bad "$name must be one of: $*"
}

need QJANUS_WS_URL
if [ -z "${QJANUS_SESSION_TOKENS:-}" ]; then
  need QJANUS_TOKEN_SECRET
  # a pre-minted token list wins: the core token secret must not be used by a runner that was given tokens
else
  unset QJANUS_TOKEN_SECRET
fi
need QJANUS_LOADTEST_SEED
is_int SHARDS
is_int SHARD
[ "$SHARDS" -ge 1 ] || bad "SHARDS must be at least 1"
[ "$SHARD" -lt "$SHARDS" ] || bad "SHARD must be smaller than SHARDS"
is_int START_AT
need RUN_ID
[[ $RUN_ID =~ ^[A-Za-z0-9._-]{1,80}$ ]] || bad "RUN_ID may only contain letters, digits, . _ - (max 80)"
is_enum SCENARIO audio8 video4 video8
is_enum RUN_MODE run ramp
is_num HOLD_SEC
is_num JOIN_RATE
is_enum VIDEO_PROFILE spec lite tiny
is_int BOTS_PER_BROWSER

OUT_DIR=${OUT_DIR:-out/shard-$SHARD}
case $OUT_DIR in -*|'') bad "OUT_DIR must not be empty or start with a dash" ;; esac

args=("$RUN_MODE" --scenario "$SCENARIO" --shard "$SHARD/$SHARDS" --start-at "$START_AT"
      --hold-sec "$HOLD_SEC" --join-rate "$JOIN_RATE" --video-profile "$VIDEO_PROFILE"
      --bots-per-browser "$BOTS_PER_BROWSER" --run-id "$RUN_ID" --out "$OUT_DIR"
      --expect-transport strict --field-trials 'WebRTC-EnableDtlsPqc/Enabled/')

if [ -n "${SETTLE_SEC:-}" ]; then
  is_num SETTLE_SEC
  args+=(--settle-sec "$SETTLE_SEC")
fi

if [ "$RUN_MODE" = run ]; then
  is_int ROOMS
  args+=(--rooms "$ROOMS")
else
  is_int RAMP_START
  is_int RAMP_STEP
  is_int RAMP_MAX
  args+=(--ramp-start "$RAMP_START" --ramp-step "$RAMP_STEP" --ramp-max "$RAMP_MAX")
fi

if [ -n "${EXTRA_ARGS:-}" ]; then
  read -r -a extra <<< "$EXTRA_ARGS"                          # word splitting only, no globbing, no eval
  for w in "${extra[@]}"; do
    [[ $w =~ ^[A-Za-z0-9_./:=,+@%-]+$ ]] || bad "EXTRA_ARGS contains a word with disallowed characters"
  done
  args+=("${extra[@]}")
fi

# No --manage-rooms (see above). The Janus URL, token secret (or pre-minted tokens) and seed reach
# qjanus-load through the environment (QJANUS_WS_URL, QJANUS_TOKEN_SECRET | QJANUS_SESSION_TOKENS,
# QJANUS_LOADTEST_SEED).
cd "$ROOT"
exec node bin/qjanus-load.mjs "${args[@]}"
