#!/usr/bin/env bash
# sampler-summary.sh - summarise node-sampler.sh CSV files.
#
# Usage: sampler-summary.sh [--from EPOCH] [--to EPOCH] [--json] FILE...
#
#   --from EPOCH   only rows with ts >= EPOCH   (epoch seconds, decimals allowed)
#   --to EPOCH     only rows with ts <= EPOCH
#   --json         machine readable output (one line of JSON) instead of a table
#
# Per column min / avg / p50 / p95 / max (percentiles by nearest rank) for
# cpu_pct, rss_mb, total_cpu_pct, rx_mbps, tx_mbps, udp_in_pps, udp_out_pps, plus the
# number of rows, the time span and the SUM of the *_d columns (drops, UDP
# errors, cgroup throttling). Robust to empty fields, a partial last line,
# several files and header lines in the middle (concatenated files): header lines
# and rows with too few fields are skipped, empty fields are not counted.
#
# Pure bash + POSIX awk.

set -u
export LC_ALL=C                                   # decimal point and number formatting must not depend on the locale

die() { echo "sampler-summary: $*" >&2; exit 2; }

FROM=""; TO=""; JSON=0; FILES=()
while [ $# -gt 0 ]; do
  case $1 in
    --from) [ $# -ge 2 ] || die "--from needs a value"; FROM=$2; shift 2 ;;
    --to)   [ $# -ge 2 ] || die "--to needs a value"; TO=$2; shift 2 ;;
    --json) JSON=1; shift ;;
    -h|--help) sed -n '2,/^$/{/^#/s/^# \{0,1\}//p}' "$0"; exit 0 ;;
    --) shift; FILES+=("$@"); break ;;
    -*) die "unknown option: $1" ;;
    *)  FILES+=("$1"); shift ;;
  esac
done
[ "${#FILES[@]}" -gt 0 ] || die "no input file (see --help)"
for f in "${FILES[@]}"; do [ -r "$f" ] || die "cannot read $f"; done
for v in "$FROM" "$TO"; do
  case $v in *[!0-9.]*|*.*.*|.) die "--from/--to need an epoch number" ;; esac
done

awk -F, -v from="$FROM" -v to="$TO" -v json="$JSON" -v nfiles="${#FILES[@]}" '
BEGIN {
  # default column order; replaced whenever a header line is met
  hdr = "ts,iso,cpu_pct,rss_mb,threads,total_cpu_pct,load1,rx_mbps,tx_mbps,rx_drop_d,tx_drop_d,udp_in_pps,udp_out_pps,udp_in_errors_d,udp_rcvbuf_errors_d,udp_sndbuf_errors_d,cg_throttled_ms_d,pid"
  set_header(hdr)
  ns = split("cpu_pct rss_mb total_cpu_pct rx_mbps tx_mbps udp_in_pps udp_out_pps", stat, " ")
  nt = split("rx_drop_d tx_drop_d udp_in_errors_d udp_rcvbuf_errors_d udp_sndbuf_errors_d cg_throttled_ms_d", tot, " ")
  rows = 0; tmin = ""; tmax = ""
}
function set_header(h,   i, n, a) {
  delete idx
  n = split(h, a, ",")
  for (i = 1; i <= n; i++) idx[a[i]] = i
  ncols = n
}
# Shell sort of v[1..n], numeric ascending
function sort(v, n,   gap, i, j, t) {
  for (gap = int(n / 2); gap > 0; gap = int(gap / 2))
    for (i = gap + 1; i <= n; i++) {
      t = v[i]
      for (j = i; j > gap && v[j - gap] > t; j -= gap) v[j] = v[j - gap]
      v[j] = t
    }
}
function rank(n, pct,   r) {              # nearest rank: ceil(pct / 100 * n), in integers
  r = int((pct * n + 99) / 100)
  if (r < 1) r = 1
  if (r > n) r = n
  return r
}
function num(s) { return (s ~ /^[-+]?[0-9]*[.]?[0-9]+([eE][-+]?[0-9]+)?$/) }
{
  sub(/\r$/, "")
  if ($0 == "") next
  if ($1 == "ts") { set_header($0); next }               # header line (also mid-file)
  if (NF < ncols || !num($1)) next                       # partial / garbage line
  t = $1 + 0
  if (from != "" && t < from + 0) next
  if (to != "" && t > to + 0) next
  rows++
  if (tmin == "" || t < tmin) { tmin = t; iso_first = $(idx["iso"]) }
  if (tmax == "" || t > tmax) { tmax = t; iso_last = $(idx["iso"]) }
  for (k = 1; k <= ns; k++) {
    c = stat[k]; v = $(idx[c])
    if (v != "" && num(v)) { cnt[c]++; val[c, cnt[c]] = v + 0 }
  }
  for (k = 1; k <= nt; k++) {
    c = tot[k]; v = $(idx[c])
    if (v != "" && num(v)) { sum[c] += v + 0; tcnt[c]++ }
  }
}
END {
  span = (rows > 0) ? tmax - tmin : 0
  if (json) {
    printf "{\"files\":%d,\"rows\":%d,", nfiles, rows
    if (rows > 0) printf "\"from\":%.3f,\"to\":%.3f,\"first_iso\":\"%s\",\"last_iso\":\"%s\",", tmin, tmax, iso_first, iso_last
    else printf "\"from\":null,\"to\":null,\"first_iso\":null,\"last_iso\":null,"
    printf "\"span_sec\":%.3f,\"columns\":{", span
  } else {
    printf "files: %d   rows: %d   span: %.1f s", nfiles, rows, span
    if (rows > 0) printf "   (%s .. %s)", iso_first, iso_last
    printf "\n"
    printf "%-16s %6s %10s %10s %10s %10s %10s\n", "column", "n", "min", "avg", "p50", "p95", "max"
  }
  for (k = 1; k <= ns; k++) {
    c = stat[k]; n = cnt[c] + 0
    if (n > 0) {
      delete w
      s = 0
      for (i = 1; i <= n; i++) { w[i] = val[c, i]; s += w[i] }
      sort(w, n)
      mn = w[1]; mx = w[n]; av = s / n; p50 = w[rank(n, 50)]; p95 = w[rank(n, 95)]
    }
    if (json) {
      if (k > 1) printf ","
      if (n > 0) printf "\"%s\":{\"n\":%d,\"min\":%.2f,\"avg\":%.2f,\"p50\":%.2f,\"p95\":%.2f,\"max\":%.2f}", c, n, mn, av, p50, p95, mx
      else printf "\"%s\":{\"n\":0,\"min\":null,\"avg\":null,\"p50\":null,\"p95\":null,\"max\":null}", c
    } else {
      if (n > 0) printf "%-16s %6d %10.2f %10.2f %10.2f %10.2f %10.2f\n", c, n, mn, av, p50, p95, mx
      else printf "%-16s %6d %10s %10s %10s %10s %10s\n", c, 0, "-", "-", "-", "-", "-"
    }
  }
  if (json) {
    printf "},\"totals\":{"
    for (k = 1; k <= nt; k++) { c = tot[k]; printf "%s\"%s\":%.1f", (k > 1 ? "," : ""), c, sum[c] + 0 }
    printf "}}\n"
  } else {
    printf "totals:"
    for (k = 1; k <= nt; k++) { c = tot[k]; printf " %s=%.1f", c, sum[c] + 0 }
    printf "\n"
  }
}
' "${FILES[@]}"
