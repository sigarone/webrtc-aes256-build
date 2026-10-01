#!/bin/sh
# test-gates.sh - self-test for m150/gates.sh against synthetic fixtures (no
# real compiled webrtc binary needed/available). Builds a fake AAR and a fake
# xcframework with the exact literal strings gates.sh looks for, runs the
# happy path (all gates pass) for both platforms, then a handful of negative
# cases proving each gate actually fails closed when its signal is missing.
#
#   sh m150/ci/test-gates.sh
set -u
HERE=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)   # m150/
GATES="$HERE/gates.sh"
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT INT TERM
fail=0

# Prefer a real `zip` if present (real CI runners have one); otherwise fall
# back to Python's zipfile module (this dev sandbox has no `zip` binary) so
# this self-test still runs everywhere gates.sh itself needs to run.
if command -v zip >/dev/null 2>&1; then
  ZIPDIR() { ( cd "$1" && zip -qr "$2" "${@:3}" ); }  # zipdir <srcdir> <out.zip> <members...>
else
  # NOTE: `command -v python3` can resolve to the Windows Store's stub
  # launcher, which "succeeds" as a PATH lookup but errors out (prints an
  # Italian "install from the Store" message) the moment it actually runs -
  # so a real invocation is required to pick a working interpreter, not just
  # a PATH check.
  PY=""
  for cand in python3 python; do
    if command -v "$cand" >/dev/null 2>&1 && "$cand" -c 'pass' >/dev/null 2>&1; then PY=$(command -v "$cand"); break; fi
  done
  [ -n "$PY" ] || { echo "no working python3/python found and no zip binary either - cannot build test fixtures" >&2; exit 2; }
  ZIPDIR() {
    srcdir=$1; out=$2; shift 2
    "$PY" - "$srcdir" "$out" "$@" <<'PYEOF'
import os, sys, zipfile
srcdir, out = sys.argv[1], sys.argv[2]
members = sys.argv[3:]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED) as zf:
    for m in members:
        p = os.path.join(srcdir, m)
        if os.path.isdir(p):
            for root, _, files in os.walk(p):
                for fn in files:
                    full = os.path.join(root, fn)
                    zf.write(full, os.path.relpath(full, srcdir))
        elif os.path.isfile(p):
            zf.write(p, m)
PYEOF
  }
fi

expect() { # name expected_rc cmd...
  name=$1; want=$2; shift 2
  out=$("$@" 2>&1); rc=$?
  if [ "$rc" -eq "$want" ]; then echo "ok   - $name (rc=$rc)"; else echo "FAIL - $name: rc=$rc want=$want"; echo "$out" | sed 's/^/       /' | tail -20; fail=1; fi
}
# A "happy path" run should exit 0 everywhere G5's toolchain (nm/llvm-nm) is
# available. This self-test's own sandbox may have neither (no Android NDK,
# no Xcode) - in that case gates.sh correctly fails closed AT G5 (by design,
# see gates.sh's own comment), after G1-G4 have already passed. Treat that
# specific, identified outcome as a pass for THIS self-test (it is exercising
# a real environment gap, not a gates.sh defect), and only that one.
expect_happy() { # name cmd...
  name=$1; shift
  out=$("$@" 2>&1); rc=$?
  if [ "$rc" -eq 0 ]; then
    echo "ok   - $name (rc=0, all gates incl. G5)"
  elif [ "$rc" -eq 1 ] && printf '%s' "$out" | grep -q 'G5 (no llvm-nm/nm resolved'; then
    echo "ok   - $name (rc=1, but only because this sandbox has no nm/llvm-nm - G1-G4 passed first)"
  else
    echo "FAIL - $name: rc=$rc"; echo "$out" | sed 's/^/       /' | tail -20; fail=1
  fi
}

# ---- synthetic dnn_dir (G4 extracts array names live from these) ----------
DNN="$T/dnn"; mkdir -p "$DNN"
cat > "$DNN/plc_data.c" <<'EOF'
#include "plc_data.h"
const WeightArray plcmodel_arrays[] = { {0} };
static const float plc_dense_in_weights_float[4] = {0,0,0,0};
EOF
cat > "$DNN/fargan_data.c" <<'EOF'
#include "fargan_data.h"
const WeightArray fargan_arrays[] = { {0} };
EOF

# ---- synthetic ssl.h (G9, informational) -----------------------------------
SSLH="$T/ssl.h"
printf '#define SSL_GROUP_X25519_MLKEM1024 0x0203\n' > "$SSLH"

# gates.sh's G1 (via assert-no-key-strings.sh -m 5000000) refuses any file
# under 5 MB as "too small to trust" - every synthetic fixture is padded past
# that floor with `truncate` (near-instant, zero-fill) so the self-test
# exercises the real gate logic, not the size check, without the multi-second
# cost `head -c N /dev/zero` has on some sandboxes.
pad_file() { truncate -s 5200000 "$1"; }

# Every literal every gate looks for, in one blob - the "everything passes"
# fixture for a PLAIN build.
make_so_plain() {
  { printf 'Failed to derive HkdfSha256 key from secret.\000'
    printf 'Q-AUDION dtls\000Q-AUDION opus-dec\000Q-AUDION opus-enc\000Q-AUDION frame-replay drop dup=\000'
    printf 'transport=strict\000'
    printf 'setQaudionOpusEncoderComplexity\000Java_org_webrtc_PeerConnectionFactory_setQaudionOpusEncoderComplexity\000'
    printf 'plcmodel_arrays\000fargan_arrays\000plc_dense_in_weights_float\000'
  } > "$1"
  pad_file "$1"
}
make_aar() { # $1=out.aar $2=so_maker $3=abi_dir
  d="$T/aarbuild"; rm -rf "$d"; mkdir -p "$d/jni/$3"
  "$2" "$d/jni/$3/libjingle_peerconnection_so.so"
  printf '<manifest/>' > "$d/AndroidManifest.xml"
  # classes.jar: a real (tiny) zip with the expected class path present
  mkdir -p "$d/classes_src/org/webrtc"
  : > "$d/classes_src/org/webrtc/PeerConnectionFactory.class"
  ZIPDIR "$d/classes_src" "$d/classes.jar" org
  ZIPDIR "$d" "$1" AndroidManifest.xml classes.jar jni
}

make_xcframework() { # $1=out_dir $2=so_maker
  d="$1"; rm -rf "$d"; mkdir -p "$d/ios-arm64/WebRTC.framework" "$d/ios-arm64-simulator/WebRTC.framework" "$d/Headers"
  "$2" "$d/ios-arm64/WebRTC.framework/WebRTC"
  "$2" "$d/ios-arm64-simulator/WebRTC.framework/WebRTC"
  printf 'setQaudionOpusEncoderComplexity' > "$d/Headers/RTCPeerConnectionFactory.h"
  cat > "$d/Info.plist" <<'EOF'
<?xml version="1.0"?>
<plist><dict>
<key>AvailableLibraries</key>
<array>
<dict><key>LibraryIdentifier</key><string>ios-arm64</string></dict>
<dict><key>LibraryIdentifier</key><string>ios-arm64-simulator</string></dict>
</array>
</dict></plist>
EOF
}

echo "=== happy path: android plain ==="
AAR="$T/plain.aar"
make_aar "$AAR" make_so_plain arm64-v8a
expect_happy "android plain: all gates pass" sh "$GATES" android plain "$AAR" "$DNN" "$SSLH"

echo "=== happy path: ios plain ==="
XC="$T/WebRTC.xcframework"
make_xcframework "$XC" make_so_plain
expect_happy "ios plain: all gates pass" sh "$GATES" ios plain "$XC" "$DNN" "$SSLH"

echo "=== negative: G1 fails when key material present ==="
AARBAD="$T/g1bad.aar"
# a leaky .so variant instead of the clean one
make_so_leaky() { { printf 'secret \000 len \000 slat << \000\n derived_key \000'; printf 'Q-AUDION dtls\000Q-AUDION opus-dec\000Q-AUDION opus-enc\000Q-AUDION frame-replay drop dup=\000transport=strict\000'; printf 'setQaudionOpusEncoderComplexity\000Java_org_webrtc_PeerConnectionFactory_setQaudionOpusEncoderComplexity\000'; printf 'plcmodel_arrays\000fargan_arrays\000plc_dense_in_weights_float\000'; } > "$1"; pad_file "$1"; }
make_aar "$AARBAD" make_so_leaky arm64-v8a
# rc=2, not 1: this fixture also lacks the G1 positive-control string, so
# assert-no-key-strings.sh fails closed on "can't even see this code" before
# it gets to report the leak itself - see its own test suite's identical
# "leak with a missing control still fails" case (ci/test-assert-no-key-strings.sh).
expect "G1 catches leaked key material" 2 sh "$GATES" android plain "$AARBAD" "$DNN" "$SSLH"

echo "=== negative: G3 fails when the P12 frame-replay marker is missing ==="
make_so_noreplay() { { printf 'Failed to derive HkdfSha256 key from secret.\000'; printf 'Q-AUDION dtls\000Q-AUDION opus-dec\000Q-AUDION opus-enc\000transport=strict\000'; printf 'setQaudionOpusEncoderComplexity\000Java_org_webrtc_PeerConnectionFactory_setQaudionOpusEncoderComplexity\000'; printf 'plcmodel_arrays\000fargan_arrays\000plc_dense_in_weights_float\000'; } > "$1"; pad_file "$1"; }
AARG3="$T/g3bad.aar"
make_aar "$AARG3" make_so_noreplay arm64-v8a
expect "G3 catches a build without the Q-AUDION frame-replay marker" 1 sh "$GATES" android plain "$AARG3" "$DNN" "$SSLH"

echo "=== negative: the removed -lk variant is rejected ==="
expect "gates.sh refuses the removed lk variant" 2 sh "$GATES" android lk "$AAR" "$DNN" "$SSLH"

echo "=== negative: G2 fails when plain build carries the relaxed-cipher marker ==="
make_so_relaxed() { { printf 'Failed to derive HkdfSha256 key from secret.\000'; printf 'Q-AUDION dtls\000Q-AUDION opus-dec\000Q-AUDION opus-enc\000Q-AUDION frame-replay drop dup=\000'; printf '!AESGCM+AES256\000transport=strict\000'; printf 'setQaudionOpusEncoderComplexity\000Java_org_webrtc_PeerConnectionFactory_setQaudionOpusEncoderComplexity\000'; printf 'plcmodel_arrays\000fargan_arrays\000plc_dense_in_weights_float\000'; } > "$1"; pad_file "$1"; }
AARG2="$T/g2bad.aar"
make_aar "$AARG2" make_so_relaxed arm64-v8a
expect "G2 catches the relaxed-cipher marker in a plain build" 1 sh "$GATES" android plain "$AARG2" "$DNN" "$SSLH"

echo "=== negative: G6 fails on an extra ABI ==="
d="$T/aarbuild-g6"; rm -rf "$d"; mkdir -p "$d/jni/arm64-v8a" "$d/jni/armeabi-v7a"
make_so_plain "$d/jni/arm64-v8a/libjingle_peerconnection_so.so"
make_so_plain "$d/jni/armeabi-v7a/libjingle_peerconnection_so.so"
printf '<manifest/>' > "$d/AndroidManifest.xml"
mkdir -p "$d/classes_src/org/webrtc"
: > "$d/classes_src/org/webrtc/PeerConnectionFactory.class"
ZIPDIR "$d/classes_src" "$d/classes.jar" org
AARG6="$T/g6bad.aar"
ZIPDIR "$d" "$AARG6" AndroidManifest.xml classes.jar jni
expect "G6 catches an extra ABI (armeabi-v7a alongside arm64-v8a)" 1 sh "$GATES" android plain "$AARG6" "$DNN" "$SSLH"

[ "$fail" -eq 0 ] && echo "ALL OK" || echo "SOME TESTS FAILED"
exit "$fail"
