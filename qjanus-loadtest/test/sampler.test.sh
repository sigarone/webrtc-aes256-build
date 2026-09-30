#!/usr/bin/env bash
# Tests for scripts/node-sampler.sh and scripts/sampler-summary.sh.
#
#   bash test/sampler.test.sh
#
# Deterministic: the sampler runs against the fake /proc and /sys trees in
# test/fixtures (three consecutive snapshots t0, t1, t2, two seconds apart) through
# its test-only `--proc-root-seq`/`--sys-root-seq` mechanism, so nothing sleeps and
# every value is asserted exactly. A few checks (loop, SIGTERM, live /proc) run in
# real time, about 8 seconds in total. Prints PASS/FAIL per check and exits nonzero
# if anything failed. Needs bash, awk, sed, cp, mktemp only (Git Bash and Ubuntu).

set -u

HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/.." && pwd)
SAMPLER=$ROOT/scripts/node-sampler.sh
SUMMARY=$ROOT/scripts/sampler-summary.sh
FIX=$HERE/fixtures                        # contains proc/t0..t2 and sys/t0..t2
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

PASS=0
FAIL=0
check() {                                  # check NAME EXPECTED ACTUAL
  if [ "$2" = "$3" ]; then
    PASS=$((PASS + 1)); echo "PASS $1"
  else
    FAIL=$((FAIL + 1)); echo "FAIL $1"; echo "     expected: $2"; echo "     actual:   $3"
  fi
}
check_has() {                              # check_has NAME NEEDLE HAYSTACK
  case $3 in
    *"$2"*) PASS=$((PASS + 1)); echo "PASS $1" ;;
    *) FAIL=$((FAIL + 1)); echo "FAIL $1"; echo "     missing:  $2"; echo "     in:       $3" ;;
  esac
}
col() { printf '%s\n' "$1" | awk -F, -v n="$2" '{ print $n }'; }   # field N of a CSV row

HEADER='ts,iso,cpu_pct,rss_mb,threads,total_cpu_pct,load1,rx_mbps,tx_mbps,rx_drop_d,tx_drop_d,udp_in_pps,udp_out_pps,udp_in_errors_d,udp_rcvbuf_errors_d,udp_sndbuf_errors_d,cg_throttled_ms_d,pid'

# Expected rows for the pristine fixtures (interval 2 s, CLK_TCK 100), worked out by hand:
#  t0 -> t1: process 300 ticks / 2 s = 150 %; host busy 400 of 1000 ticks = 40 %; eth0 rx +25e6 B = 100 Mbps,
#            tx +12.5e6 B = 50 Mbps; rx drop 10->13; UDP in (20000 v4 + 2000 v6) / 2 s = 11000 pps, out (30000 + 1000) / 2 = 15500;
#            InErrors 4 + 1, RcvbufErrors 3 + 2, SndbufErrors 1 + 0; cgroup throttled 1000000 -> 1250000 us = 250 ms
#  t1 -> t2: process 200 ticks / 2 s = 100 %; host busy 360 of 800 ticks = 45 % (guest 50 not counted twice);
#            rx +5e6 B = 20 Mbps, tx +1e6 B = 4 Mbps; rx drop 13 -> 4 (counter reset, clamped to 0), tx drop 5 -> 7;
#            UDP in 400 / 2 = 200 pps, out 600 / 2 = 300 pps; throttled +50500 us = 50.5 ms
ROW1='1700000002.000,2023-11-14T22:13:22Z,150.00,256.0,31,40.00,0.52,100.000,50.000,3,0,11000.0,15500.0,5,5,1,250.0,4242'
ROW2='1700000004.000,2023-11-14T22:13:24Z,100.00,300.0,32,45.00,1.25,20.000,4.000,0,2,200.0,300.0,0,0,0,50.5,4242'

# run_seq ROOT OUT [args...]: sample the t0,t1,t2 trees under ROOT/proc and ROOT/sys
run_seq() {
  local root=$1 out=$2
  shift 2
  SAMPLER_CLK_TCK=100 bash "$SAMPLER" \
    --proc-root-seq "$root/proc/t0,$root/proc/t1,$root/proc/t2" \
    --sys-root-seq "$root/sys/t0,$root/sys/t1,$root/sys/t2" \
    --interval 0 --out "$out" --quiet "$@"
}
mkvariant() {                              # mkvariant NAME: writable copy of the fixtures in $TMP/NAME
  mkdir -p "$TMP/$1"
  cp -R "$FIX/proc" "$TMP/$1/proc"
  cp -R "$FIX/sys" "$TMP/$1/sys"
}
rows() { sed '1d' "$1"; }                  # data rows of a CSV file

# ------------------------------------------------------------ sampler: values
run_seq "$FIX" "$TMP/base.csv"
check "sampler exits 0 on the fixture sequence" 0 $?
check "header is the contract header" "$HEADER" "$(sed -n 1p "$TMP/base.csv")"
check "two snapshots pairs give exactly two rows (no bogus first row)" 3 "$(wc -l < "$TMP/base.csv" | tr -d ' ')"
check "row 1: cpu, rss, host cpu, NIC, UDP, throttling, pid" "$ROW1" "$(sed -n 2p "$TMP/base.csv")"
check "row 2: counter reset clamped to 0, drops, guest not double counted" "$ROW2" "$(sed -n 3p "$TMP/base.csv")"
check "process comm with spaces and ')' is parsed (t1 fixture)" 150.00 "$(col "$(sed -n 2p "$TMP/base.csv")" 3)"

run_seq "$FIX" "$TMP/pid.csv" --pid 4242
check "--pid 4242 gives the same rows" "$ROW1
$ROW2" "$(rows "$TMP/pid.csv")"

run_seq "$FIX" "$TMP/proc.csv" --process nosuch,janus
check "--process list: the matching name is found (exact comm match, decoy janus-helper ignored)" "$ROW1
$ROW2" "$(rows "$TMP/proc.csv")"

run_seq "$FIX" "$TMP/count.csv" --count 1
check "--count 1 stops after one row" "$ROW1" "$(rows "$TMP/count.csv")"

run_seq "$FIX" "$TMP/latest.csv" --latest "$TMP/latest.json"
check "--latest holds the newest row as one JSON line" \
  '{"ts":1700000004.000,"iso":"2023-11-14T22:13:24Z","cpu_pct":100.00,"rss_mb":300.0,"threads":32,"total_cpu_pct":45.00,"load1":1.25,"rx_mbps":20.000,"tx_mbps":4.000,"rx_drop_d":0,"tx_drop_d":2,"udp_in_pps":200.0,"udp_out_pps":300.0,"udp_in_errors_d":0,"udp_rcvbuf_errors_d":0,"udp_sndbuf_errors_d":0,"cg_throttled_ms_d":50.5,"pid":4242}' \
  "$(cat "$TMP/latest.json")"
check "--latest leaves no temp file behind" 1 "$(find "$TMP" -maxdepth 1 -name 'latest.json*' | wc -l | tr -d ' ')"

# appending: the header must be written only once
cp "$TMP/base.csv" "$TMP/append.csv"
run_seq "$FIX" "$TMP/append.csv"
check "appending to an existing file keeps a single header" 1 "$(grep -c '^ts,' "$TMP/append.csv")"
check "appending adds the new rows" 5 "$(wc -l < "$TMP/append.csv" | tr -d ' ')"

# --------------------------------------------------------- sampler: interfaces
check "default route interface (eth0) is used: NIC rate of eth0" 100.000 "$(col "$(sed -n 2p "$TMP/base.csv")" 8)"
run_seq "$FIX" "$TMP/docker.csv" --iface docker0
check "--iface overrides the default route (docker0: +250000 B, +50000 B)" "1700000002.000,2023-11-14T22:13:22Z,150.00,256.0,31,40.00,0.52,1.000,0.200,0,0,11000.0,15500.0,5,5,1,250.0,4242" "$(sed -n 2p "$TMP/docker.csv")"
mkvariant noroute
rm -f "$TMP"/noroute/proc/t*/net/route
run_seq "$TMP/noroute" "$TMP/noroute.csv"
check "no default route: first non-lo interface (docker0)" 1.000 "$(col "$(sed -n 2p "$TMP/noroute.csv")" 8)"

# ------------------------------------------------------- sampler: process states
run_seq "$FIX" "$TMP/absent.csv" --process nosuchproc
check "absent process: empty process fields, host fields still filled (row 1)" \
  "1700000002.000,2023-11-14T22:13:22Z,,,,40.00,0.52,100.000,50.000,3,0,11000.0,15500.0,5,5,1,," "$(sed -n 2p "$TMP/absent.csv")"
check "absent process: row 2 too" \
  "1700000004.000,2023-11-14T22:13:24Z,,,,45.00,1.25,20.000,4.000,0,2,200.0,300.0,0,0,0,," "$(sed -n 3p "$TMP/absent.csv")"

mkvariant restart                          # the process restarts between t0 and t1 as pid 4243 (new start time)
for i in 1 2; do
  mv "$TMP/restart/proc/t$i/4242" "$TMP/restart/proc/t$i/4243"
  sed -i.bak 's/^4242 (/4243 (/; s/ 987654 / 987700 /' "$TMP/restart/proc/t$i/4243/stat"
  rm -f "$TMP/restart/proc/t$i/4243/stat.bak"
done
run_seq "$TMP/restart" "$TMP/restart.csv"
check "restarted process: re-resolved by name, no cpu/throttle delta across the restart" \
  "1700000002.000,2023-11-14T22:13:22Z,,256.0,31,40.00,0.52,100.000,50.000,3,0,11000.0,15500.0,5,5,1,,4243" "$(sed -n 2p "$TMP/restart.csv")"
check "restarted process: deltas resume on the next row" \
  "1700000004.000,2023-11-14T22:13:24Z,100.00,300.0,32,45.00,1.25,20.000,4.000,0,2,200.0,300.0,0,0,0,50.5,4243" "$(sed -n 3p "$TMP/restart.csv")"

mkvariant wrap                             # cpu tick counter goes backwards in t2
sed -i.bak 's/ 1280 720 / 100 20 /' "$TMP/wrap/proc/t2/4242/stat"
run_seq "$TMP/wrap" "$TMP/wrap.csv"
check "process cpu ticks going backwards clamp to 0 %" 0.00 "$(col "$(sed -n 3p "$TMP/wrap.csv")" 3)"

# ------------------------------------------------------------- sampler: cgroups
mkvariant cgv1                             # cgroup v1: cpu,cpuacct hierarchy, throttled_time in ns
rm -rf "$TMP/cgv1/sys"
n=0
for v in 5000000000 5250000000 5250000000; do
  mkdir -p "$TMP/cgv1/sys/t$n/fs/cgroup/cpu,cpuacct/qjanus.slice"
  printf 'nr_periods 10\nnr_throttled 1\nthrottled_time %s\n' "$v" > "$TMP/cgv1/sys/t$n/fs/cgroup/cpu,cpuacct/qjanus.slice/cpu.stat"
  n=$((n + 1))
done
for i in 0 1 2; do printf '12:pids:/x\n4:cpu,cpuacct:/qjanus.slice\n1:name=systemd:/x\n' > "$TMP/cgv1/proc/t$i/4242/cgroup"; done
run_seq "$TMP/cgv1" "$TMP/cgv1.csv"
check "cgroup v1 throttled_time (ns) -> 250 ms" 250.0 "$(col "$(sed -n 2p "$TMP/cgv1.csv")" 17)"
check "cgroup v1: no new throttling -> 0.0" 0.0 "$(col "$(sed -n 3p "$TMP/cgv1.csv")" 17)"

mkvariant nocg
rm -f "$TMP"/nocg/proc/t*/4242/cgroup
run_seq "$TMP/nocg" "$TMP/nocg.csv"
check "no /proc/PID/cgroup: throttling field empty, the rest intact" "1700000002.000,2023-11-14T22:13:22Z,150.00,256.0,31,40.00,0.52,100.000,50.000,3,0,11000.0,15500.0,5,5,1,,4242" "$(sed -n 2p "$TMP/nocg.csv")"

mkvariant rootcg                           # root cgroup: cpu.stat has no throttled_* keys
for i in 0 1 2; do
  echo '0::/' > "$TMP/rootcg/proc/t$i/4242/cgroup"
  rm -rf "$TMP/rootcg/sys/t$i/fs"; mkdir -p "$TMP/rootcg/sys/t$i/fs/cgroup"
  printf 'usage_usec 1\nuser_usec 1\nsystem_usec 0\n' > "$TMP/rootcg/sys/t$i/fs/cgroup/cpu.stat"
done
run_seq "$TMP/rootcg" "$TMP/rootcg.csv"
check "root cgroup without throttled_usec: field empty" "" "$(col "$(sed -n 2p "$TMP/rootcg.csv")" 17)"

mkvariant nosnmp
rm -f "$TMP"/nosnmp/proc/t*/net/snmp "$TMP"/nosnmp/proc/t*/net/snmp6
run_seq "$TMP/nosnmp" "$TMP/nosnmp.csv"
check "no snmp files: the UDP fields are empty" "1700000002.000,2023-11-14T22:13:22Z,150.00,256.0,31,40.00,0.52,100.000,50.000,3,0,,,,,,250.0,4242" "$(sed -n 2p "$TMP/nosnmp.csv")"

# ------------------------------------------------------------- sampler: CLI
bash "$SAMPLER" --help > "$TMP/help.txt" 2>&1
check "--help exits 0" 0 $?
check_has "--help documents the test-only sequence option" "--proc-root-seq" "$(cat "$TMP/help.txt")"
bash "$SAMPLER" --bogus > /dev/null 2>&1
check "unknown option exits 2" 2 $?
bash "$SAMPLER" > /dev/null 2>&1
check "missing --out exits 2" 2 $?
bash "$SAMPLER" --out "$TMP/x.csv" --interval 0 > /dev/null 2>&1
check "--interval 0 outside test mode exits 2" 2 $?
bash "$SAMPLER" --out "$TMP/x.csv" --interval abc > /dev/null 2>&1
check "non numeric --interval exits 2" 2 $?
check "the sampler never writes hostnames or IPs" 0 "$(grep -Ec '[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+' "$TMP/base.csv")"

# ------------------------------------------------- sampler: real time behaviour
# static fixture tree, real clock: the loop sleeps, timestamps advance, values are steady
PROC_ROOT="$FIX/proc/t1" SYS_ROOT="$FIX/sys/t1" SAMPLER_CLK_TCK=100 \
  bash "$SAMPLER" --interval 1 --count 2 --process janus --out "$TMP/rt.csv" --quiet
check "real-time loop exits 0" 0 $?
check "real-time loop writes two rows" 3 "$(wc -l < "$TMP/rt.csv" | tr -d ' ')"
t1=$(col "$(sed -n 2p "$TMP/rt.csv")" 1); t2=$(col "$(sed -n 3p "$TMP/rt.csv")" 1)
check "real-time rows are at least one second apart" 1 "$(awk -v a="$t1" -v b="$t2" 'BEGIN { d = b - a; print (d > 0.8) ? 1 : 0 }')"
check "real-time rows on a static tree: 0 % process cpu, pid 4242" "0.00,4242" "$(col "$(sed -n 2p "$TMP/rt.csv")" 3),$(col "$(sed -n 2p "$TMP/rt.csv")" 18)"

# SIGTERM: clean exit, file intact, no partial row
PROC_ROOT="$FIX/proc/t1" SYS_ROOT="$FIX/sys/t1" \
  bash "$SAMPLER" --interval 1 --duration 60 --out "$TMP/term.csv" --quiet &
spid=$!
for _ in $(seq 1 120); do                       # wait for the first row (a loaded machine starts slowly)
  [ -s "$TMP/term.csv" ] && [ "$(wc -l < "$TMP/term.csv" | tr -d ' ')" -ge 2 ] && break
  sleep 0.5
done
kill -TERM "$spid" 2> /dev/null
wait "$spid"
check "SIGTERM: exit status 0" 0 $?
nrows=$(rows "$TMP/term.csv" | wc -l | tr -d ' ')
check "SIGTERM: at least one complete row was written" 1 "$([ "$nrows" -ge 1 ] && echo 1 || echo 0)"
check "SIGTERM: every row has 18 fields" 0 "$(rows "$TMP/term.csv" | awk -F, 'NF != 18' | wc -l | tr -d ' ')"

# live /proc (Linux only): the real files parse into a sane row
if [ -r /proc/net/dev ] && [ -r /proc/net/snmp ] && [ -r /proc/stat ] && [ -r "/proc/$$/stat" ] && [ -r /proc/loadavg ]; then
  bash "$SAMPLER" --pid $$ --interval 1 --count 2 --out "$TMP/live.csv" --quiet
  live=$(sed -n 3p "$TMP/live.csv")
  check "live /proc: 2 rows of 18 fields" "2,18" "$(rows "$TMP/live.csv" | wc -l | tr -d ' '),$(printf '%s\n' "$live" | awk -F, '{ print NF }')"
  check "live /proc: pid column is the sampled pid" "$$" "$(col "$live" 18)"
  check "live /proc: cpu_pct, rss_mb, threads and total_cpu_pct are numbers" 4 \
    "$(printf '%s\n' "$live" | awk -F, '{ n = 0; for (i = 3; i <= 6; i++) if ($i ~ /^[0-9]+([.][0-9]+)?$/) n++; print n }')"
  now=$(date +%s)
  check "live /proc: ts is the current time" 1 "$(awk -v a="$(col "$live" 1)" -v b="$now" 'BEGIN { d = a - b; print (d > -5 && d < 5) ? 1 : 0 }')"
else
  echo "SKIP live /proc checks (no Linux /proc here)"
fi

# ----------------------------------------------------------------- summarizer
H=$HEADER
csv() {                                    # csv TS CPU RSS: one full row (other columns fixed), empty args stay empty
  printf '%s,2026-01-01T00:00:00Z,%s,%s,30,20.00,0.5,5.000,2.000,1,0,100.0,100.0,0,0,0,2.0,42\n' "$1" "$2" "$3"
}
{ echo "$H"; for i in 0 1 2 3 4; do csv $((1000 + i * 2)) $((10 + i * 10)).00 100.0; done; } > "$TMP/a.csv"
{ echo "$H"; for i in 0 1 2 3 4; do csv $((1010 + i * 2)) $((60 + i * 10)).00 ""; done; printf '1020.000,2026-01-01T00:00:20Z,55'; } > "$TMP/b.csv"  # empty rss, partial last line

out=$(bash "$SUMMARY" "$TMP/a.csv" | tr -s " ")                    # squeeze the column padding
check_has "summary: rows and time span" "rows: 5 span: 8.0 s" "$out"
check_has "summary: cpu_pct n/min/avg/p50/p95/max of 10..50" "cpu_pct 5 10.00 30.00 30.00 50.00 50.00" "$out"
check_has "summary: totals of the *_d columns" "totals: rx_drop_d=5.0 tx_drop_d=0.0 udp_in_errors_d=0.0 udp_rcvbuf_errors_d=0.0 udp_sndbuf_errors_d=0.0 cg_throttled_ms_d=10.0" "$out"

out=$(bash "$SUMMARY" "$TMP/a.csv" "$TMP/b.csv" | tr -s " ")
check_has "two files: 10 rows (the partial last line is ignored)" "rows: 10 span:" "$out"
check_has "two files: cpu_pct avg 55, p50 = rank 5 = 50, p95 = rank 10 = 100" "cpu_pct 10 10.00 55.00 50.00 100.00 100.00" "$out"
check_has "empty fields are not counted (rss_mb n=5)" "rss_mb 5 100.00 100.00 100.00 100.00 100.00" "$out"

cat "$TMP/a.csv" "$TMP/b.csv" > "$TMP/cat.csv"                      # header line in the middle
out=$(bash "$SUMMARY" "$TMP/cat.csv" | tr -s " ")
check_has "concatenated CSV with a header in the middle: same statistics" "cpu_pct 10 10.00 55.00 50.00 100.00 100.00" "$out"

out=$(bash "$SUMMARY" --from 1006 --to 1012 "$TMP/a.csv" "$TMP/b.csv" | tr -s " ")
check_has "--from/--to keep the rows 1006..1012 (cpu 40,50,60,70)" "cpu_pct 4 40.00 55.00 50.00 70.00 70.00" "$out"

out=$(bash "$SUMMARY" --json --from 1006 --to 1012 "$TMP/a.csv" "$TMP/b.csv")
check_has "--json: header fields" '{"files":2,"rows":4,"from":1006.000,"to":1012.000,"first_iso":"2026-01-01T00:00:00Z","last_iso":"2026-01-01T00:00:00Z","span_sec":6.000,' "$out"
check_has "--json: cpu_pct object" '"cpu_pct":{"n":4,"min":40.00,"avg":55.00,"p50":50.00,"p95":70.00,"max":70.00}' "$out"
check_has "--json: totals object" '"totals":{"rx_drop_d":4.0,"tx_drop_d":0.0,"udp_in_errors_d":0.0,"udp_rcvbuf_errors_d":0.0,"udp_sndbuf_errors_d":0.0,"cg_throttled_ms_d":8.0}}' "$out"
check "--json is a single line" 1 "$(printf '%s\n' "$out" | wc -l | tr -d ' ')"

out=$(bash "$SUMMARY" --json --from 5000 "$TMP/a.csv")
check_has "no rows in range: rows 0 and null statistics, exit 0" '"rows":0,"from":null,"to":null' "$out"
bash "$SUMMARY" "$TMP/nosuchfile.csv" > /dev/null 2>&1
check "missing input file exits 2" 2 $?
bash "$SUMMARY" > /dev/null 2>&1
check "no input file exits 2" 2 $?

# the summarizer reads real sampler output too
out=$(bash "$SUMMARY" --json "$TMP/base.csv")
check_has "sampler output -> summary: cpu_pct avg of 150 and 100" '"cpu_pct":{"n":2,"min":100.00,"avg":125.00,"p50":100.00,"p95":150.00,"max":150.00}' "$out"

echo
echo "sampler tests: $PASS passed, $FAIL failed"
[ "$FAIL" = 0 ]
