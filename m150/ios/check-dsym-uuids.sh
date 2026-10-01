#!/bin/sh
# check-dsym-uuids.sh - verify every Mach-O slice in a .xcframework has a
# matching dSYM with the SAME UUID (a mismatched/missing dSYM means crash
# symbolication silently breaks for that slice - fail closed instead).
#
# usage: check-dsym-uuids.sh <xcframework_dir> <dsym_dir>
# exit: 0 all slices matched | 1 mismatch/missing | 2 usage/tooling problem
set -eu

usage() { echo "usage: $0 <xcframework_dir> <dsym_dir>" >&2; exit 2; }
[ $# -eq 2 ] || usage
XC=$1
DSYMS=$2
[ -d "$XC" ] || { echo "::error::check-dsym-uuids: $XC not found" >&2; exit 2; }
[ -d "$DSYMS" ] || { echo "::error::check-dsym-uuids: $DSYMS not found" >&2; exit 2; }
command -v dwarfdump >/dev/null 2>&1 || { echo "::error::check-dsym-uuids: dwarfdump not available" >&2; exit 2; }

SLICES=$(find "$XC" -type f -name 'WebRTC')
[ -n "$SLICES" ] || { echo "::error::check-dsym-uuids: no Mach-O slice found under $XC" >&2; exit 1; }

fail=0
for f in $SLICES; do
  echo "-- slice: $f"
  BIN_UUIDS=$(dwarfdump --uuid "$f" 2>/dev/null | awk '{print $2}' | sort -u)
  [ -n "$BIN_UUIDS" ] || { echo "::error::check-dsym-uuids: could not read UUID(s) from $f" >&2; fail=1; continue; }
  for u in $BIN_UUIDS; do
    hit=$(find "$DSYMS" -type f -name '*.dSYM' -prune -o -type f -print 2>/dev/null | xargs -I{} sh -c "dwarfdump --uuid '{}' 2>/dev/null | grep -q '$u' && echo {}" 2>/dev/null | head -1)
    if [ -z "$hit" ]; then
      # fall back to scanning inside .dSYM bundles directly
      hit=$(find "$DSYMS" -type d -name '*.dSYM' | while read -r d; do
        dwarfdump --uuid "$d" 2>/dev/null | grep -q "$u" && printf '%s\n' "$d" && break
      done)
    fi
    if [ -z "$hit" ]; then
      echo "::error::check-dsym-uuids: no dSYM found for UUID $u ($f)" >&2
      fail=1
    else
      echo "   UUID $u -> $hit"
    fi
  done
done

[ "$fail" -eq 0 ] || exit 1
echo "check-dsym-uuids: all slices matched a dSYM"
