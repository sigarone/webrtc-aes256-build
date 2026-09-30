#!/usr/bin/env bash
# node-sampler.sh - host + process sampler for the qjanus load test.
#
# Runs on the SFU host during a test and appends one CSV row every --interval
# seconds (default 2) to --out FILE. Pure bash + awk, only reads /proc and /sys,
# needs no root. Header (written once, also when appending to an existing file):
#
#   ts,iso,cpu_pct,rss_mb,threads,total_cpu_pct,load1,rx_mbps,tx_mbps,rx_drop_d,
#   tx_drop_d,udp_in_pps,udp_out_pps,udp_in_errors_d,udp_rcvbuf_errors_d,
#   udp_sndbuf_errors_d,cg_throttled_ms_d,pid            (one line, no wrap)
#
#   ts                    epoch seconds with millisecond decimals
#   iso                   the same instant, UTC, ISO 8601
#   cpu_pct               process CPU in percent of ONE core (200 = a 200 % quota)
#   rss_mb, threads       process resident set / thread count
#   total_cpu_pct         whole host CPU, 0-100 (guest time is not counted twice)
#   load1                 1 minute load average
#   rx_mbps, tx_mbps      NIC throughput of --iface, megabits/s
#   rx_drop_d, tx_drop_d  NIC drop deltas over the interval
#   udp_in_pps/out_pps    UDP datagrams/s (IPv4 + IPv6 summed)
#   udp_*_errors_d        deltas of Udp InErrors / RcvbufErrors / SndbufErrors
#   cg_throttled_ms_d     cgroup CPU throttling delta in ms (empty if unavailable)
#   pid                   the sampled process (empty while it is absent)
#
# Missing values are empty fields. Counter wraps / resets (negative deltas) are
# clamped to 0. The first row is computed from the first two snapshots, so the
# first row appears one interval after the start (no bogus first row).
#
# Usage:
#   node-sampler.sh --out FILE [options]
#     --out FILE          CSV to append to (required)
#     --interval SEC      seconds between rows (default 2; decimals allowed)
#     --pid N             sample this pid (no name lookup)
#     --process A[,B]     process names (comm, exact match); default qjanus,janus.
#                         Re-resolved when the process restarts; while it is
#                         absent the process columns stay empty.
#     --iface NAME        NIC to measure (default: interface of the default route
#                         from /proc/net/route, else the first non-lo interface)
#     --latest FILE       also write the newest row as a one-line JSON object
#                         (keys = header names), replaced atomically
#     --duration SEC      stop after SEC seconds
#     --count N           stop after N rows
#     --quiet             no progress messages on stderr
#     --help
#
#   Stop it with SIGTERM (or SIGINT when started in the foreground): the current
#   row is complete, the file is closed after every row, the exit status is 0.
#   Note: a background job started by a NON-interactive shell ignores SIGINT,
#   so scripts must stop it with `kill -TERM`.
#
# Environment: PROC_ROOT (default /proc), SYS_ROOT (default /sys), both only to
# run against fixtures; SAMPLER_CLK_TCK overrides `getconf CLK_TCK`.
#
# TEST ONLY: `--proc-root-seq DIR,DIR,...` (with `--interval 0`) replaces the live
# /proc by a fixed sequence of fake /proc trees, one per snapshot; the run ends
# when the sequence is exhausted (N trees -> N-1 rows). A tree may contain a file
# `_ts` holding the snapshot time. `--sys-root-seq DIR,DIR,...` does the same for
# /sys (a shorter list repeats its last entry).
#
# The output never contains hostnames or IP addresses.

set -u
export LC_ALL=C                                   # decimal point and number formatting must not depend on the locale

HEADER='ts,iso,cpu_pct,rss_mb,threads,total_cpu_pct,load1,rx_mbps,tx_mbps,rx_drop_d,tx_drop_d,udp_in_pps,udp_out_pps,udp_in_errors_d,udp_rcvbuf_errors_d,udp_sndbuf_errors_d,cg_throttled_ms_d,pid'

PROC_ROOT=${PROC_ROOT:-/proc}
SYS_ROOT=${SYS_ROOT:-/sys}
INTERVAL=2
OUT=""
PID_ARG=""
PROCS="qjanus,janus"
IFACE=""
LATEST=""
DURATION=""
COUNT=""
QUIET=0
SEQ_PROC=""
SEQ_SYS=""

usage() { sed -n '2,/^$/{/^#/s/^# \{0,1\}//p}' "$0"; }
die() { echo "node-sampler: $*" >&2; exit 2; }

while [ $# -gt 0 ]; do
  case $1 in
    --*=*) set -- "${1%%=*}" "${1#*=}" "${@:2}" ;;   # --opt=value -> --opt value
  esac
  case $1 in
    --out)            [ $# -ge 2 ] || die "--out needs a value"; OUT=$2; shift 2 ;;
    --interval)       [ $# -ge 2 ] || die "--interval needs a value"; INTERVAL=$2; shift 2 ;;
    --pid)            [ $# -ge 2 ] || die "--pid needs a value"; PID_ARG=$2; shift 2 ;;
    --process)        [ $# -ge 2 ] || die "--process needs a value"; PROCS=$2; shift 2 ;;
    --iface)          [ $# -ge 2 ] || die "--iface needs a value"; IFACE=$2; shift 2 ;;
    --latest)         [ $# -ge 2 ] || die "--latest needs a value"; LATEST=$2; shift 2 ;;
    --duration)       [ $# -ge 2 ] || die "--duration needs a value"; DURATION=$2; shift 2 ;;
    --count)          [ $# -ge 2 ] || die "--count needs a value"; COUNT=$2; shift 2 ;;
    --proc-root-seq)  [ $# -ge 2 ] || die "--proc-root-seq needs a value"; SEQ_PROC=$2; shift 2 ;;
    --sys-root-seq)   [ $# -ge 2 ] || die "--sys-root-seq needs a value"; SEQ_SYS=$2; shift 2 ;;
    --quiet)          QUIET=1; shift ;;
    -h|--help)        usage; exit 0 ;;
    *)                die "unknown option: $1 (see --help)" ;;
  esac
done

[ -n "$OUT" ] || die "--out FILE is required (see --help)"
case $INTERVAL in ''|*[!0-9.]*|*.*.*|.) die "--interval must be a number of seconds" ;; esac
case $PID_ARG in *[!0-9]*) die "--pid must be a number" ;; esac
case $DURATION in *[!0-9]*) die "--duration must be a whole number of seconds" ;; esac
case $COUNT in *[!0-9]*) die "--count must be a whole number" ;; esac
if [ -z "$SEQ_PROC" ] && ! awk -v v="$INTERVAL" 'BEGIN { exit !(v + 0 > 0) }'; then
  die "--interval 0 is only allowed together with --proc-root-seq (test mode)"
fi

# ---------------------------------------------------------------- clock ticks
HZ=${SAMPLER_CLK_TCK:-$(getconf CLK_TCK 2>/dev/null || true)}
case $HZ in ''|*[!0-9]*|0) HZ=100 ;; esac

# ------------------------------------------------------------------- helpers
# now_ts: epoch seconds with milliseconds (bash >= 5 EPOCHREALTIME, else GNU date)
now_ts() {
  local t frac
  if [ -n "${EPOCHREALTIME-}" ]; then
    t=${EPOCHREALTIME/,/.}                    # some locales use a decimal comma
  else
    t=$(date +%s.%N 2>/dev/null || true)
    case $t in ''|*N*) t="$(date +%s).000" ;; esac
  fi
  frac="${t#*.}000"
  printf '%s.%s' "${t%%.*}" "${frac:0:3}"
}

# resolve_pid ROOT: lowest pid whose comm equals one of the --process names
resolve_pid() {
  local root=$1 d comm n p best="" names
  IFS=, read -r -a names <<< "$PROCS"
  for d in "$root"/[0-9]*; do
    [ -r "$d/comm" ] || continue
    comm=""
    { read -r comm < "$d/comm"; } 2>/dev/null || true       # the process may exit meanwhile
    for n in "${names[@]}"; do
      if [ "$comm" = "$n" ]; then
        p=${d##*/}
        if [ -z "$best" ] || [ "$p" -lt "$best" ]; then best=$p; fi
      fi
    done
  done
  printf '%s' "$best"
}

# pid_alive ROOT PID: PID still exists in ROOT and is still one of our processes
pid_alive() {
  local root=$1 pid=$2 comm="" n names
  [ -r "$root/$pid/stat" ] || return 1
  { read -r comm < "$root/$pid/comm"; } 2>/dev/null || true
  IFS=, read -r -a names <<< "$PROCS"
  for n in "${names[@]}"; do [ "$comm" = "$n" ] && return 0; done
  return 1
}

# detect_iface ROOT: interface of the default route, else the first non-lo interface
detect_iface() {
  local root=$1 n=""
  if [ -r "$root/net/route" ]; then
    n=$(awk 'NR > 1 && $2 == "00000000" && $8 == "00000000" {
               m = $7 + 0
               if (best == "" || m < bm) { best = $1; bm = m }
             }
             END { if (best != "") print best }' "$root/net/route")
  fi
  if [ -z "$n" ] && [ -r "$root/net/dev" ]; then
    n=$(awk -F: 'NR > 2 { gsub(/[ \t]/, "", $1); if ($1 != "" && $1 != "lo") { print $1; exit } }' "$root/net/dev")
  fi
  printf '%s' "$n"
}

# ---------------------------------------------------------------- snapshots
# One awk run reads every counter we need from a /proc (+ /sys) tree and prints
# ONE line of 19 space separated fields, "-" = unavailable:
#  1 ts  2 cpu ticks (utime+stime)  3 process start time  4 rss kB  5 threads
#  6 pid  7 host cpu total  8 host cpu idle  9 load1  10 rx bytes  11 rx drop
#  12 tx bytes  13 tx drop  14 udp in  15 udp out  16 udp in errors
#  17 udp rcvbuf errors  18 udp sndbuf errors  19 cgroup throttled (usec)
IFS= read -r -d '' SNAP_AWK <<'AWK'
function I(x) { return sprintf("%.0f", x) }
function rd(f,   l) {                      # first line of a file, "" if unreadable
  l = ""
  if ((getline l < f) < 0) l = ""
  close(f)
  sub(/\r$/, "", l)
  return l
}
BEGIN {
  for (i = 1; i <= 19; i++) out[i] = "-"
  out[1] = ts ""

  # ---- the sampled process
  if (pid != "") {
    l = rd(proot "/" pid "/stat")
    p = 0
    for (i = length(l); i > 0; i--) if (substr(l, i, 1) == ")") { p = i; break }   # comm may hold spaces and ')'
    if (p > 0) {
      n = split(substr(l, p + 2), a, " ")   # a[k] = field k+2 of proc(5)
      if (n >= 20) { out[2] = I(a[12] + a[13]); out[3] = a[20] ""; out[6] = pid "" }
    }
    f = proot "/" pid "/status"
    while ((getline l < f) > 0) {
      n = split(l, a, /[ \t]+/)
      if (a[1] == "VmRSS:") out[4] = a[2] ""
      else if (a[1] == "Threads:") out[5] = a[2] ""
    }
    close(f)

    # cgroup CPU throttling: v2 cpu.stat throttled_usec, v1 cpu.stat throttled_time (ns)
    f = proot "/" pid "/cgroup"
    v2 = ""; v1 = ""; c1 = ""
    while ((getline l < f) > 0) {
      sub(/\r$/, "", l)
      i1 = index(l, ":"); if (!i1) continue
      hier = substr(l, 1, i1 - 1); r = substr(l, i1 + 1)
      i2 = index(r, ":"); if (!i2) continue
      ctrl = substr(r, 1, i2 - 1); path = substr(r, i2 + 1)
      if (hier == "0" && ctrl == "") v2 = path
      else if (("," ctrl ",") ~ /,cpu,/) { v1 = path; c1 = ctrl }
    }
    close(f)
    thr = "-"
    if (v2 != "") {
      f = sroot "/fs/cgroup" (v2 == "/" ? "" : v2) "/cpu.stat"
      while ((getline l < f) > 0) { split(l, a, " "); if (a[1] == "throttled_usec") thr = I(a[2]) }
      close(f)
    }
    if (thr == "-" && v1 != "") {
      m = split("cpu,cpuacct cpu " c1, cand, " ")
      for (k = 1; k <= m && thr == "-"; k++) {
        f = sroot "/fs/cgroup/" cand[k] (v1 == "/" ? "" : v1) "/cpu.stat"
        while ((getline l < f) > 0) { split(l, a, " "); if (a[1] == "throttled_time") thr = I(a[2] / 1000) }
        close(f)
      }
    }
    out[19] = thr
  }

  # ---- host CPU (guest / guest_nice are already inside user / nice)
  l = rd(proot "/stat")
  if (substr(l, 1, 4) == "cpu ") {
    n = split(l, a, " ")
    t = 0
    for (i = 2; i <= 9 && i <= n; i++) t += a[i]
    out[7] = I(t); out[8] = I(a[5] + a[6])
  }
  l = rd(proot "/loadavg")
  split(l, a, " ")
  if (a[1] ~ /^[0-9.]+$/) out[9] = a[1] ""

  # ---- NIC counters ("eth0:123 ..." may have no space after the colon)
  if (iface != "") {
    f = proot "/net/dev"
    while ((getline l < f) > 0) {
      i1 = index(l, ":"); if (!i1) continue
      nm = substr(l, 1, i1 - 1); gsub(/[ \t]/, "", nm)
      if (nm != iface) continue
      n = split(substr(l, i1 + 1), a, " ")
      if (n >= 12) { out[10] = I(a[1]); out[11] = I(a[4]); out[12] = I(a[9]); out[13] = I(a[12]) }
    }
    close(f)
  }

  # ---- UDP: /proc/net/snmp ("Udp:" header line, then value line) + snmp6 Udp6*
  have = 0
  f = proot "/net/snmp"
  while ((getline l < f) > 0) {
    n = split(l, a, " ")
    if (a[1] != "Udp:") continue
    if (a[2] ~ /^[0-9]+$/) { for (i = 2; i <= n; i++) u4[hn[i]] = a[i]; have = 1 }
    else for (i = 2; i <= n; i++) hn[i] = a[i]
  }
  close(f)
  f = proot "/net/snmp6"
  while ((getline l < f) > 0) {
    n = split(l, a, " ")
    if (n == 2 && substr(a[1], 1, 4) == "Udp6") { u6[a[1]] = a[2]; have = 1 }
  }
  close(f)
  if (have) {
    out[14] = I(u4["InDatagrams"] + u6["Udp6InDatagrams"])
    out[15] = I(u4["OutDatagrams"] + u6["Udp6OutDatagrams"])
    out[16] = I(u4["InErrors"] + u6["Udp6InErrors"])
    out[17] = I(u4["RcvbufErrors"] + u6["Udp6RcvbufErrors"])
    out[18] = I(u4["SndbufErrors"] + u6["Udp6SndbufErrors"])
  }

  s = out[1]
  for (i = 2; i <= 19; i++) s = s " " out[i]
  print s
}
AWK

# snapshot PROC_ROOT SYS_ROOT TS PID -> one snapshot line
snapshot() {
  awk -v proot="$1" -v sroot="$2" -v ts="$3" -v pid="$4" -v iface="$IFACE" "$SNAP_AWK"
}

# ---------------------------------------------------------------- CSV rows
# make_row PREV CUR -> one CSV row computed from two snapshot lines
IFS= read -r -d '' ROW_AWK <<'AWK'
function I(x) { return sprintf("%.0f", x) }
function fm(v, spec) { return (v == "-") ? "" : sprintf(spec, v) }
function pos(x) { return (x < 0) ? 0 : x }
function delta(i) { return (P[i] == "-" || C[i] == "-") ? "-" : pos(C[i] - P[i]) }
function iso(t,   s, days, rem, z, era, doe, yoe, y, doy, mp, m, dd) {   # civil-from-days, UTC
  s = int(t); days = int(s / 86400); rem = s - days * 86400
  z = days + 719468; era = int(z / 146097); doe = z - era * 146097
  yoe = int((doe - int(doe / 1460) + int(doe / 36524) - int(doe / 146096)) / 365)
  y = yoe + era * 400
  doy = doe - (365 * yoe + int(yoe / 4) - int(yoe / 100))
  mp = int((5 * doy + 2) / 153)
  dd = doy - int((153 * mp + 2) / 5) + 1
  m = (mp < 10) ? mp + 3 : mp - 9
  if (m <= 2) y++
  return sprintf("%04d-%02d-%02dT%02d:%02d:%02dZ", y, m, dd, int(rem / 3600), int((rem % 3600) / 60), rem % 60)
}
BEGIN {
  split(prev, P, " "); split(cur, C, " ")
  dt = C[1] - P[1]
  ok = (dt > 0)

  cpu = "-"; rss = "-"; thr = "-"; pid = "-"; cg = "-"
  if (C[6] != "-") {                              # process present in the current snapshot
    pid = C[6]
    if (C[4] != "-") rss = C[4] / 1024
    thr = C[5]
    same = (P[6] == C[6] && P[3] == C[3] && P[3] != "-")   # same pid AND same start time
    if (ok && same && P[2] != "-" && C[2] != "-") cpu = pos(C[2] - P[2]) / hz / dt * 100
    if (same && P[19] != "-" && C[19] != "-") cg = pos(C[19] - P[19]) / 1000
  }

  tot = "-"
  if (P[7] != "-" && C[7] != "-") {
    dtot = C[7] - P[7]; didle = pos(C[8] - P[8])
    if (dtot > 0) { tot = 100 * (dtot - didle) / dtot; if (tot < 0) tot = 0; if (tot > 100) tot = 100 }
  }

  rx = "-"; tx = "-"; ui = "-"; uo = "-"
  if (ok && delta(10) != "-") rx = delta(10) * 8 / dt / 1000000
  if (ok && delta(12) != "-") tx = delta(12) * 8 / dt / 1000000
  if (ok && delta(14) != "-") ui = delta(14) / dt
  if (ok && delta(15) != "-") uo = delta(15) / dt

  d11 = delta(11); d13 = delta(13); d16 = delta(16); d17 = delta(17); d18 = delta(18)
  printf "%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s,%s\n",
    sprintf("%.3f", C[1]), iso(C[1]), fm(cpu, "%.2f"), fm(rss, "%.1f"), fm(thr, "%.0f"),
    fm(tot, "%.2f"), (C[9] == "-" ? "" : C[9]), fm(rx, "%.3f"), fm(tx, "%.3f"),
    fm(d11, "%.0f"), fm(d13, "%.0f"), fm(ui, "%.1f"), fm(uo, "%.1f"),
    fm(d16, "%.0f"), fm(d17, "%.0f"), fm(d18, "%.0f"), fm(cg, "%.1f"), (pid == "-" ? "" : pid)
}
AWK

make_row() { awk -v prev="$1" -v cur="$2" -v hz="$HZ" "$ROW_AWK"; }

# row_to_json ROW -> one-line JSON object (empty field -> null)
row_to_json() {
  printf '%s\n' "$1" | awk -F, -v hdr="$HEADER" '
    BEGIN { n = split(hdr, h, ",") }
    { printf "{"
      for (i = 1; i <= n; i++) {
        if (i > 1) printf ","
        if ($i == "") printf "\"%s\":null", h[i]
        else if (h[i] == "iso") printf "\"%s\":\"%s\"", h[i], $i
        else printf "\"%s\":%s", h[i], $i
      }
      print "}" }'
}

# ------------------------------------------------------------------- main
STOP=0
SLEEP_PID=""
trap 'STOP=1' INT TERM

nap() {          # interruptible sleep (a plain `sleep` would delay the trap)
  [ -n "$SEQ_PROC" ] && return 0
  sleep "$INTERVAL" &
  SLEEP_PID=$!
  wait "$SLEEP_PID" 2>/dev/null
  [ "$STOP" = 0 ] || kill "$SLEEP_PID" 2>/dev/null
  SLEEP_PID=""
}

if [ -n "$SEQ_PROC" ]; then
  IFS=, read -r -a PSEQ <<< "$SEQ_PROC"
  if [ -n "$SEQ_SYS" ]; then IFS=, read -r -a SSEQ <<< "$SEQ_SYS"; else SSEQ=("$SYS_ROOT"); fi
else
  PSEQ=("$PROC_ROOT"); SSEQ=("$SYS_ROOT")
fi

# take_snapshot INDEX -> sets SNAP; picks the pid (and re-resolves it after a restart)
CUR_PID=$PID_ARG
take_snapshot() {
  local idx=$1 proot sroot ts si
  if [ -n "$SEQ_PROC" ]; then proot=${PSEQ[$idx]}; else proot=${PSEQ[0]}; fi
  si=$idx; [ "$si" -ge "${#SSEQ[@]}" ] && si=$(( ${#SSEQ[@]} - 1 ))
  if [ -n "$SEQ_PROC" ]; then sroot=${SSEQ[$si]}; else sroot=${SSEQ[0]}; fi
  if [ -z "$PID_ARG" ]; then
    if [ -z "$CUR_PID" ] || ! pid_alive "$proot" "$CUR_PID"; then CUR_PID=$(resolve_pid "$proot"); fi
  fi
  ts=""
  if [ -n "$SEQ_PROC" ] && [ -r "$proot/_ts" ]; then read -r ts < "$proot/_ts" || true; fi
  [ -n "$ts" ] || ts=$(now_ts)
  SNAP=$(snapshot "$proot" "$sroot" "$ts" "$CUR_PID")
}

case $OUT in */*) mkdir -p "${OUT%/*}" 2>/dev/null || true ;; esac
if [ ! -s "$OUT" ]; then printf '%s\n' "$HEADER" > "$OUT" || die "cannot write $OUT"; fi

first_root=${PSEQ[0]}
[ -n "$IFACE" ] || IFACE=$(detect_iface "$first_root")
[ "$QUIET" = 1 ] || echo "node-sampler: writing $OUT every ${INTERVAL}s (iface=${IFACE:-none}, clk_tck=$HZ)" >&2

ROWS=0
IDX=0
take_snapshot "$IDX"
PREV=$SNAP
SECONDS=0
while [ "$STOP" = 0 ]; do
  [ -n "$COUNT" ] && [ "$ROWS" -ge "$COUNT" ] && break
  [ -n "$DURATION" ] && [ "$SECONDS" -ge "$DURATION" ] && break
  nap
  [ "$STOP" = 0 ] || break
  IDX=$(( IDX + 1 ))
  if [ -n "$SEQ_PROC" ] && [ "$IDX" -ge "${#PSEQ[@]}" ]; then break; fi
  take_snapshot "$IDX"
  ROW=$(make_row "$PREV" "$SNAP")
  PREV=$SNAP
  printf '%s\n' "$ROW" >> "$OUT" || echo "node-sampler: cannot append to $OUT" >&2
  ROWS=$(( ROWS + 1 ))
  if [ -n "$LATEST" ]; then
    row_to_json "$ROW" > "$LATEST.tmp.$$" && mv -f "$LATEST.tmp.$$" "$LATEST"
  fi
done

[ "$QUIET" = 1 ] || echo "node-sampler: stopped after $ROWS rows" >&2
exit 0
