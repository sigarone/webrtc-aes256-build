#!/bin/sh
# Self-test for ci/assert-no-key-strings.sh (POSIX sh; runs on Linux, macOS and
# Git-Bash). Builds throw-away fake binaries and checks every exit code.
#   sh ci/test-assert-no-key-strings.sh
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
GATE="$HERE/assert-no-key-strings.sh"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT INT TERM
fail=0

pad() { head -c 4096 /dev/zero; }
# same byte layout as the real literals: NUL-separated, "\n derived_key " has a
# leading newline + space
leaky_hkdf()  { { pad; printf 'secret \000 len \000 slat << \000\n derived_key \000 len \000'; pad; } > "$1"; }
leaky_pbkdf() { { pad; printf 'raw_key \000 len \000'; pad; } > "$1"; }
clean_bin()   { { pad; printf 'Failed to derive HkdfSha256 key from secret.\000 other strings \000'; pad; } > "$1"; }
clean_no_ctl(){ { pad; printf 'nothing interesting here\000'; pad; } > "$1"; }
leaky_ctl()   { { pad; printf 'Failed to derive HkdfSha256 key from secret.\000'; printf 'secret \000 len \000 slat << \000\n derived_key \000'; pad; } > "$1"; }

expect() { # name expected_rc cmd...
  name=$1; want=$2; shift 2
  out=$("$@" 2>&1); rc=$?
  if [ "$rc" -eq "$want" ]; then echo "ok   - $name (rc=$rc)"; else echo "FAIL - $name: rc=$rc want=$want"; echo "$out" | sed 's/^/       /'; fail=1; fi
}

leaky_hkdf "$T/hkdf.bin"; leaky_pbkdf "$T/pbkdf.bin"; clean_bin "$T/clean.bin"; clean_no_ctl "$T/noctl.bin"; leaky_ctl "$T/leakyctl.bin"
head -c 100 /dev/zero > "$T/tiny.bin"

expect "clean binary passes"                       0 sh "$GATE" -m 100 "$T/clean.bin"
expect "HKDF log strings are caught"               1 sh "$GATE" -m 100 "$T/hkdf.bin"
expect "PBKDF2 log strings are caught"             1 sh "$GATE" -m 100 "$T/pbkdf.bin"
expect "one leaky file among clean ones fails"     1 sh "$GATE" -m 100 "$T/clean.bin" "$T/hkdf.bin" "$T/clean.bin"
expect "strict control present passes"             0 sh "$GATE" -m 100 -P 'Failed to derive HkdfSha256 key from secret' "$T/clean.bin"
expect "strict control missing fails closed"       2 sh "$GATE" -m 100 -P 'Failed to derive HkdfSha256 key from secret' "$T/noctl.bin"
expect "soft control missing only warns"           0 sh "$GATE" -m 100 -p 'Failed to derive HkdfSha256 key from secret' "$T/noctl.bin"
expect "truncated/tiny file fails closed"          2 sh "$GATE" "$T/tiny.bin"
expect "missing file fails closed"                 2 sh "$GATE" -m 100 "$T/does-not-exist.bin"
expect "no arguments fails closed"                 2 sh "$GATE"
expect "leak with a present control fails (1)"     1 sh "$GATE" -m 100 -P 'Failed to derive HkdfSha256 key from secret' "$T/leakyctl.bin"
expect "leak with a missing control still fails"   2 sh "$GATE" -m 100 -P 'Failed to derive HkdfSha256 key from secret' "$T/hkdf.bin"
{ pad; printf 'Failed to derive HkdfSha256 key from secret.\000 with bad M-I from \000, password_=\000'; pad; } > "$T/icepwd.bin"
expect "extra -F literal is caught"                1 sh "$GATE" -m 100 -F 'password_=' "$T/icepwd.bin"
expect "extra -F literal absent passes"            0 sh "$GATE" -m 100 -F 'password_=' "$T/clean.bin"
expect "two -F literals, second one hits"          1 sh "$GATE" -m 100 -F 'nope-not-there' -F 'password_=' "$T/icepwd.bin"
expect "-F with a glob char is literal, no hit"    0 sh "$GATE" -m 100 -F '*' "$T/clean.bin"
expect "empty -F fails closed"                     2 sh "$GATE" -m 100 -F '' "$T/clean.bin"
expect "non-numeric -m fails closed"               2 sh "$GATE" -m 5MB "$T/clean.bin"

# never echo binary content: the output may contain names and counts only
out=$(sh "$GATE" -m 100 "$T/hkdf.bin" 2>&1 || true)
case "$out" in *"secret "*|*"len "*) echo "FAIL - output leaked literal content"; fail=1 ;; *) echo "ok   - output has counts only" ;; esac

[ "$fail" -eq 0 ] && echo "ALL OK" || echo "SOME TESTS FAILED"
exit "$fail"
