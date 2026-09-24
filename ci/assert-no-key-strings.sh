#!/bin/sh
# assert-no-key-strings.sh - fail if a built WebRTC binary still carries the
# upstream key-dump log strings. Prints only file names, sizes and counts,
# never binary content.
#
# Background: upstream api/crypto/frame_crypto_transformer.cc logs the
# frame-cryptor secret / salt / DERIVED AES KEY as decimal byte lists at
# RTC_LOG(LS_INFO):
#     "secret " ... " len " ... " slat << " ... "\n derived_key " ...   (HKDF)
#     "raw_key " ... " len " ... " slat << " ... "\n derived_key " ...   (PBKDF2)
# no-key-log.patch deletes both statements. The format-string literals cannot
# survive in a binary built from a patched tree, so their absence is a cheap,
# toolchain-independent proof that the patch really reached the shipped code
# ("slat" is upstream's own typo of "salt").
#
# usage: assert-no-key-strings.sh [-P STR] [-p STR] [-m MIN_BYTES] FILE...
#   FILE...   binaries to scan: every Mach-O slice of an xcframework, every
#             jni/<abi>/*.so of an AAR (extract it first).
#   -P STR    positive control that MUST be present in every file (a string the
#             patch keeps, e.g. 'Failed to derive HkdfSha256 key from secret'):
#             proves the scan can see this code at all. Missing => exit 2.
#   -p STR    same, but only a warning when missing.
#   -m N      refuse (exit 2) files smaller than N bytes (default 1000000): an
#             empty or truncated file must never pass as "clean".
# exit: 0 clean | 1 forbidden string found | 2 could not scan (fail closed)
#
# Raw byte scan (grep -a -F, LC_ALL=C), not `strings`: no dependence on the
# Mach-O section heuristics of Apple's strings. Whole-binary substring match is
# safe for a WebRTC binary: the shipped Android .so had exactly one hit for each
# literal below and all of them came from these two statements.
set -eu
LC_ALL=C
export LC_ALL

POS_STRICT=""
POS_SOFT=""
MIN_BYTES=1000000
while getopts "P:p:m:" opt; do
  case "$opt" in
    P) POS_STRICT="$OPTARG" ;;
    p) POS_SOFT="$OPTARG" ;;
    m) MIN_BYTES="$OPTARG" ;;
    *) echo "usage: $0 [-P STR] [-p STR] [-m MIN_BYTES] FILE..." >&2; exit 2 ;;
  esac
done
shift $((OPTIND - 1))
if [ "$#" -eq 0 ]; then
  echo "::error::assert-no-key-strings: no files given - nothing was scanned"
  exit 2
fi

# grep -c prints the count and exits 1 when it is 0; only rc >= 2 is an error.
count() {
  _n=$(grep -a -c -F -e "$1" "$2" 2>/dev/null) && _rc=0 || _rc=$?
  case "$_rc" in
    0|1) printf '%s' "${_n:-0}" ;;
    *) printf 'ERR' ;;
  esac
}

status=0
for f in "$@"; do
  if [ ! -f "$f" ] || [ ! -r "$f" ]; then
    echo "::error::assert-no-key-strings: cannot read $f"
    exit 2
  fi
  size=$(wc -c < "$f" | tr -d ' ')
  if [ "$size" -lt "$MIN_BYTES" ]; then
    echo "::error::assert-no-key-strings: $f is only $size bytes (< $MIN_BYTES) - refusing to call it clean"
    exit 2
  fi
  hits=0
  line="$f size=$size"
  for pair in "derived_key:derived_key" "slat:slat << " "raw_key:raw_key"; do
    name=${pair%%:*}
    lit=${pair#*:}
    n=$(count "$lit" "$f")
    if [ "$n" = "ERR" ]; then
      echo "::error::assert-no-key-strings: grep failed on $f"
      exit 2
    fi
    line="$line $name=$n"
    hits=$((hits + n))
  done
  if [ -n "$POS_STRICT" ] || [ -n "$POS_SOFT" ]; then
    ctl="${POS_STRICT:-$POS_SOFT}"
    c=$(count "$ctl" "$f")
    line="$line control=$c"
    if [ "$c" = "ERR" ] || [ "$c" = "0" ]; then
      if [ -n "$POS_STRICT" ]; then
        echo "$line"
        echo "::error::assert-no-key-strings: positive control missing in $f - the scan cannot see this code, refusing to pass"
        exit 2
      fi
      echo "::warning::assert-no-key-strings: positive control not found in $f"
    fi
  fi
  echo "$line"
  if [ "$hits" -ne 0 ]; then
    echo "::error::assert-no-key-strings: upstream key-dump strings still present in $f ($hits line(s))"
    status=1
  fi
done

if [ "$status" -eq 0 ]; then
  echo "assert-no-key-strings: $# file(s) clean"
fi
exit "$status"
