#!/usr/bin/env bash
# run-aruba.sh - controller of the qjanus load test against the PRODUCTION node (run it from Git Bash on
# the PC, from a checkout of this repository). One command, unattended, safe by construction:
#
#   * the node's token secret and admin key never leave the node: tokens are minted ON the node
#     (aruba/aruba_tool.py mint, expiry <= 3 h, one per bot) and piped straight into a GitHub repository
#     secret, rooms are created / destroyed ON the node, the runners never see the token secret;
#   * the run only happens inside the window 23:30 - 06:00 Europe/Rome and only while the gates hold
#     (no active call, no group call, no call in the last 15 minutes, load, health). Outside the window,
#     or while a gate is closed, it polls every 10 minutes;
#   * a guard on the node (aruba_tool.py guard) checks every 5 s and, on a real call, a degraded
#     bcrypto-server (CPU, /api/v1/health latency), load average > 3.5 or the end of the window,
#     destroys the load-test rooms at once and writes an ABORT file; this controller then cancels the
#     workflow run. Nothing here ever restarts or reloads a service;
#   * at the end (also on Ctrl-C or any error) it stops the guard and the samplers, destroys the rooms,
#     deletes every repository secret it created and VERIFIES that nothing is left.
#
# Usage:   NODE=<ssh host> ARUBA_WS_URL=wss://host/janus REF=<branch> bash aruba/run-aruba.sh
# Environment (defaults in brackets)
#   ARUBA_WS_URL     public wss:// URL of the node's Janus endpoint (required; stored as a secret)
#   REF              git ref the workflow is dispatched on, it must contain the pre-minted mode [main]
#   SCENARIOS        space separated, in this order [audio8 video4 video8lite]
#   RAMP_MAX_<name>  last ramp step (rooms) of a scenario, e.g. RAMP_MAX_audio8=16
#   NODE             ssh host (alias) of the node, root access (required)
#   OUT              where results go [qjanus-loadtest/out/aruba]
#   MAX_WAIT_MIN     give up waiting for the window / gates after this long [1500]
#   DRY_GATE=1       only evaluate the gates once (ignoring the window) and exit
#   REPO, SSH_BIN, SSH_CFG, RDIR   repository, the ssh binary and optional config file, the remote work dir
#
# Needs: the gh CLI (GH_TOKEN is taken from `gh auth token`), curl, ssh, openssl, python on the PC.
set -uo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
REPO=${REPO:-sigarone/webrtc-aes256-build}
REF=${REF:-main}
NODE=${NODE:-}
SSH_BIN=${SSH_BIN:-ssh}
SSH_CFG=${SSH_CFG:-}
RDIR=${RDIR:-/root/aruba-loadtest}
OUT=${OUT:-$ROOT/out/aruba}
SCENARIOS=${SCENARIOS:-audio8 video4 video8lite}
MAX_WAIT_MIN=${MAX_WAIT_MIN:-1500}
WORKFLOW=qjanus-loadtest.yml
S_WS=ARUBA_WS_URL
S_TOK=ARUBA_SESSION_TOKENS
S_SEED=ARUBA_LOADTEST_SEED
TOKEN_TTL=7200
POLL_GATE_SEC=600
POLL_RUN_SEC=20
RUN_TIMEOUT_MIN=110
export GH_PROMPT_DISABLED=1 GH_NO_UPDATE_NOTIFIER=1

log() { printf '%s %s\n' "$(date +%H:%M:%S)" "$*"; }
die() { log "ERROR: $*"; exit 1; }

# scenario table: SIZE WORKFLOW_SCENARIO PROFILE START STEP MAX SHARDS
params() {
  case $1 in
    audio8)     echo "8 audio8 spec 2 2 24 12" ;;
    video4)     echo "4 video4 spec 2 2 12 12" ;;
    video8lite) echo "8 video8 lite 1 1 6 6" ;;
    *) return 1 ;;
  esac
}

rsh() {
  local -a cfg=()
  [ -z "$SSH_CFG" ] || cfg=(-F "$SSH_CFG")
  "$SSH_BIN" "${cfg[@]}" -o BatchMode=yes -o ConnectTimeout=20 "$NODE" "$@"
}
tool() { rsh "cd $RDIR && QJANUS_LOADTEST_SEED_FILE=$RDIR/seed python3 $RDIR/aruba_tool.py $*"; }

gh_api() {                                          # gh_api METHOD PATH [json body]
  local method=$1 path=$2 body=${3:-}
  if [ -n "$body" ]; then
    curl -sS -m 60 -X "$method" -H "Authorization: Bearer $GH_TOKEN" -H "Accept: application/vnd.github+json" \
      "https://api.github.com/repos/$REPO$path" -d "$body"
  else
    curl -sS -m 60 -X "$method" -H "Authorization: Bearer $GH_TOKEN" -H "Accept: application/vnd.github+json" \
      "https://api.github.com/repos/$REPO$path"
  fi
}

PY=""
for c in python python3 py; do
  if "$c" -c 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)' > /dev/null 2>&1; then PY=$c; break; fi
done
[ -n "$PY" ] || die "no python 3 on this machine"

# (the python of Windows prints CRLF: strip the CR, it would end up in URLs)
jget() { "$PY" -c 'import json,sys; d=json.load(sys.stdin); print(eval(sys.argv[1], {}, {"d": d}))' "$1" | tr -d '\r'; }

SEED=""
INSTALLED=0
RUN_ID=""
ABORTED=0

# ----------------------------------------------------------------------------- gate
wait_gate() {                                       # wait_gate: poll the node's gate until it opens
  local deadline=$(( $(date +%s) + MAX_WAIT_MIN * 60 )) out rc
  while :; do
    out=$(tool gate) && rc=0 || rc=$?
    if [ "$rc" = 0 ]; then log "gate open: $out"; return 0; fi
    log "gate closed (rc $rc): $out"
    [ "$(date +%s)" -lt "$deadline" ] || return 1
    sleep "$POLL_GATE_SEC"
  done
}

# ----------------------------------------------------------------------------- node side
install_node() {
  rsh "umask 077; mkdir -p $RDIR/guard" || return 1
  rsh "cat > $RDIR/aruba_tool.py && chmod 700 $RDIR/aruba_tool.py" < "$HERE/aruba_tool.py" || return 1
  rsh "cat > $RDIR/node-sampler.sh && chmod 700 $RDIR/node-sampler.sh" < "$ROOT/scripts/node-sampler.sh" || return 1
  printf '%s\n' "$SEED" | rsh "umask 077; cat > $RDIR/seed" || return 1
  INSTALLED=1
}

start_node_jobs() {
  # setsid -f forks and returns at once; nothing is backgrounded with '&' in the remote shell, so no
  # subshell keeps the ssh channel open (with '&' on a whole && list the subshell waited for the
  # guard and held the session until the connection was reset - seen on 2026-10-02 at 00:19).
  rsh "cd $RDIR && rm -f guard/ABORT guard/STOP \
    && QJANUS_LOADTEST_SEED_FILE=$RDIR/seed setsid -f nohup python3 aruba_tool.py guard --out $RDIR/guard > guard.log 2>&1 < /dev/null \
    && setsid -f nohup bash node-sampler.sh --out $RDIR/janus.csv --process qjanus,janus --interval 2 --duration 28800 --quiet > sampler-janus.log 2>&1 < /dev/null \
    && setsid -f nohup bash node-sampler.sh --out $RDIR/bcrypto.csv --process bcrypto-lite --interval 2 --duration 28800 --quiet > sampler-bc.log 2>&1 < /dev/null \
    ; sleep 12; cat $RDIR/guard.log" < /dev/null || return 1
  node_alive || return 1
}

node_alive() {                                      # 0 = guard running, no ABORT file; 1 = abort / guard gone; 2 = node unreachable
  local st
  st=$(rsh "if [ -f $RDIR/guard/ABORT ]; then echo ABORT; cat $RDIR/guard/ABORT; elif [ -f $RDIR/guard/guard.pid ] && kill -0 \$(cat $RDIR/guard/guard.pid) 2> /dev/null; then echo UP; else echo DOWN; fi") || { log "node unreachable"; return 2; }
  case $st in
    UP) return 0 ;;
    *) log "guard state: $st"; return 1 ;;
  esac
}

# ----------------------------------------------------------------------------- one scenario
run_scenario() {                                    # run_scenario NAME
  local name=$1 size sc profile start step max shards maxvar
  read -r size sc profile start step max shards < <(params "$name") || die "unknown scenario $name"
  maxvar=RAMP_MAX_$name
  max=${!maxvar:-$max}
  local tokens=$(( max * size )) dir="$OUT/$name" t0 t1 body disp
  mkdir -p "$dir"
  log "== scenario $name: $sc size $size, ramp $start+$step..$max rooms, $shards shards, $tokens tokens"

  tool rooms create --rooms "$max" --size "$size" || return 1
  # tokens: minted on the node, piped straight into the secret (never held in a variable or a file here)
  tool mint --count "$tokens" --ttl-sec "$TOKEN_TTL" | gh secret set "$S_TOK" -R "$REPO" > /dev/null || return 1

  t0=$(date +%s)
  disp=$(date -u -d "@$(( t0 - 5 ))" +%Y-%m-%dT%H:%M:%SZ)
  body=$("$PY" -c 'import json,sys; a=sys.argv; print(json.dumps({"ref": a[1], "inputs": {
    "mode": "remote", "ws_url_secret_name": a[2], "token_secret_name": "", "session_tokens_secret_name": a[3],
    "seed_secret_name": a[4], "scenario": a[5], "run_mode": "ramp", "ramp_start": a[6], "ramp_step": a[7],
    "ramp_max": a[8], "hold_sec": "60", "join_rate": "2", "video_profile": a[9], "shards": a[10],
    "bots_per_browser": "16", "start_delay_sec": "240"}}))' "$REF" "$S_WS" "$S_TOK" "$S_SEED" "$sc" "$start" "$step" "$max" "$profile" "$shards")
  gh_api POST "/actions/workflows/$WORKFLOW/dispatches" "$body" > "$dir/dispatch.out" || return 1
  [ ! -s "$dir/dispatch.out" ] || { log "dispatch refused: $(head -c 300 "$dir/dispatch.out")"; return 1; }

  RUN_ID=""
  for _ in 1 2 3 4 5 6 7 8 9 10 11 12; do
    sleep 10
    RUN_ID=$(gh_api GET "/actions/workflows/$WORKFLOW/runs?event=workflow_dispatch&per_page=5&created=%3E%3D$disp" | jget 'next(iter([r["id"] for r in d["workflow_runs"]]), "")') || RUN_ID=""
    [ -n "$RUN_ID" ] && break
  done
  [ -n "$RUN_ID" ] || { log "no workflow run appeared"; return 1; }
  log "workflow run $RUN_ID started"

  local status="" waited=0 unreach=0 alive
  while :; do
    sleep "$POLL_RUN_SEC"
    waited=$(( waited + POLL_RUN_SEC ))
    node_alive && alive=0 || alive=$?
    if [ "$alive" = 2 ]; then unreach=$(( unreach + 1 )); else unreach=0; fi
    if [ "$alive" = 1 ] || [ "$unreach" -ge 3 ]; then
      ABORTED=1
      log "ABORT: cancelling run $RUN_ID"
      gh_api POST "/actions/runs/$RUN_ID/cancel" > /dev/null || true
      break
    fi
    status=$(gh_api GET "/actions/runs/$RUN_ID" | jget 'd["status"] + " " + str(d.get("conclusion"))') || status=""
    case $status in completed*) log "run finished: $status"; break ;; esac
    if [ "$waited" -gt $(( RUN_TIMEOUT_MIN * 60 )) ]; then
      ABORTED=1
      log "run exceeded $RUN_TIMEOUT_MIN min: cancelling"
      gh_api POST "/actions/runs/$RUN_ID/cancel" > /dev/null || true
      break
    fi
  done
  t1=$(date +%s)
  echo "$t0 $t1" > "$dir/window.txt"

  # per-scenario cleanup first (the secret with the tokens must not outlive the run), then the artifacts
  gh secret delete "$S_TOK" -R "$REPO" > /dev/null 2>&1 || true
  tool rooms destroy || true
  if [ "$ABORTED" = 1 ]; then
    sleep 20                                         # let a cancelled run upload what it has
  fi
  fetch_artifacts "$dir"
  return 0
}

fetch_artifacts() {                                 # fetch_artifacts DIR: artifacts of $RUN_ID
  local dir=$1 line name url n
  gh_api GET "/actions/runs/$RUN_ID/artifacts?per_page=100" \
    | "$PY" -c 'import json,sys; [print(a["name"], a["archive_download_url"]) for a in json.load(sys.stdin)["artifacts"] if not a["expired"]]' | tr -d '\r' > "$dir/artifacts.txt" || return 0
  while read -r name url; do
    [ -n "$name" ] || continue
    n=$dir/$name
    mkdir -p "$n"
    curl -sSL --retry 3 --retry-all-errors -m 300 -H "Authorization: Bearer $GH_TOKEN" -o "$n.zip" "$url" && "$PY" -m zipfile -e "$n.zip" "$n" && rm -f "$n.zip"
  done < "$dir/artifacts.txt"
  line=$(ls "$dir" | tr '\n' ' ')
  log "artifacts in $dir: $line"
}

# ----------------------------------------------------------------------------- cleanup + verification
cleanup() {
  local orig=$? rc=0 left out s
  trap - EXIT INT TERM
  log "== cleanup"
  if [ "$INSTALLED" = 1 ]; then
    rsh "touch $RDIR/guard/STOP" > /dev/null 2>&1 || true
    sleep 8
    tool rooms destroy || rc=1
    # the [x] in the patterns keeps pkill / pgrep from matching the remote shell that runs them
    rsh "pkill -TERM -f '[n]ode-sampler.sh --out $RDIR'; pkill -TERM -f '[a]ruba_tool.py guard'; sleep 3; true" > /dev/null 2>&1 || true
    mkdir -p "$OUT"
    rsh "cat $RDIR/janus.csv" > "$OUT/janus-sampler.csv" 2> /dev/null || true
    rsh "cat $RDIR/bcrypto.csv" > "$OUT/bcrypto-sampler.csv" 2> /dev/null || true
    rsh "cat $RDIR/guard/guard.csv" > "$OUT/guard.csv" 2> /dev/null || true
    rsh "cat $RDIR/guard/ABORT" > "$OUT/ABORT.txt" 2> /dev/null || rm -f "$OUT/ABORT.txt"
  fi
  for s in "$S_TOK" "$S_SEED" "$S_WS"; do gh secret delete "$s" -R "$REPO" > /dev/null 2>&1 || true; done

  # ---- verification
  left=$(gh secret list -R "$REPO" 2> /dev/null | grep -c '^ARUBA_' || true)
  if [ "$left" = 0 ]; then log "verify: no ARUBA_* repository secret left"; else log "VERIFY FAILED: $left ARUBA_* secret(s) still exist"; rc=1; fi
  if [ "$INSTALLED" = 1 ]; then
    out=$(tool rooms list) && log "verify: rooms $out" || { log "VERIFY FAILED: cannot list rooms"; rc=1; }
    case $out in *'"harness_rooms": 0'*) ;; *) log "VERIFY FAILED: harness rooms left"; rc=1 ;; esac
    out=$(rsh "pgrep -fc '[n]ode-sampler.sh --out $RDIR|[a]ruba_tool.py guard' || true")
    case $out in 0) log "verify: no sampler / guard process left" ;; *) log "VERIFY FAILED: sampler or guard still running ($out)"; rc=1 ;; esac
    rsh "rm -rf $RDIR" > /dev/null 2>&1 && log "verify: node work dir removed" || log "node work dir NOT removed"
  fi
  if [ "$rc" = 0 ]; then log "CLEANUP VERIFIED"; else log "CLEANUP INCOMPLETE: finish by hand (rooms, secrets, processes)"; fi
  if [ "$orig" != 0 ]; then exit "$orig"; fi
  exit "$rc"
}

# ----------------------------------------------------------------------------- main
# sourced for a check of the functions (ARUBA_SOURCE_ONLY=1): define them and stop here
if [ "${ARUBA_SOURCE_ONLY:-0}" = 1 ]; then return 0 2> /dev/null || exit 0; fi
command -v gh > /dev/null || die "gh not found"
GH_TOKEN=$(gh auth token) || die "gh is not logged in"
export GH_TOKEN
SEED=$(openssl rand -hex 24) || die "no random seed"
for s in $SCENARIOS; do params "$s" > /dev/null || die "unknown scenario $s"; done
[ -n "$NODE" ] || die "NODE (the ssh host of the node) is required"
[ "${DRY_GATE:-0}" = 1 ] || [ -n "${ARUBA_WS_URL:-}" ] || die "ARUBA_WS_URL is required"

if [ "${DRY_GATE:-0}" = 1 ]; then
  rsh "umask 077; mkdir -p $RDIR" && rsh "cat > $RDIR/aruba_tool.py" < "$HERE/aruba_tool.py" || die "cannot copy the helper"
  printf '%s\n' "$SEED" | rsh "umask 077; cat > $RDIR/seed"
  tool gate --ignore-window; rc=$?
  rsh "rm -rf $RDIR"
  exit "$rc"
fi

trap cleanup EXIT INT TERM
log "waiting for the window and the gates (poll every $((POLL_GATE_SEC / 60)) min)"
install_node || die "cannot install the helper on the node"
wait_gate || die "the gates did not open within $MAX_WAIT_MIN min"
printf '%s' "$ARUBA_WS_URL" | gh secret set "$S_WS" -R "$REPO" > /dev/null || die "cannot set $S_WS"
printf '%s' "$SEED" | gh secret set "$S_SEED" -R "$REPO" > /dev/null || die "cannot set $S_SEED"
start_node_jobs || die "guard or samplers did not start"

first=1
for name in $SCENARIOS; do
  if [ "$first" = 0 ]; then
    log "re-checking the gates before the next scenario"
    wait_gate || { log "gates did not reopen"; break; }
  fi
  first=0
  run_scenario "$name" || { log "scenario $name failed to run"; ABORTED=1; }
  [ "$ABORTED" = 0 ] || { log "stopping after an abort"; break; }
done
exit 0
