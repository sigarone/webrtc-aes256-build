#!/bin/sh
# gates.sh - binary gates G1-G10 for the M150 builds (Android, iOS, Windows; plan
# webrtc-plan.md v2 §2.5). Stops at the FIRST failing gate (fail closed,
# same convention as ci/assert-no-key-strings.sh: an inconclusive check is a
# failure, never a pass-by-default).
#
# usage: gates.sh <android|ios|windows> <plain> <artifact> <dnn_dir> <ssl_h>
#   <platform>  android | ios | windows
#   <variant>   plain (the only variant: the LiveKit-prefixed -lk variant is gone)
#   <artifact>  android: path to the built .aar
#               ios:     path to the built .xcframework directory
#               windows: path to the staged directory holding webrtc.lib (a
#                        complete COFF static library, x64) and include/
#                        (the public headers). variant must be plain. G5 and
#                        G6 are re-expressed for a static library (no dynamic
#                        export table to scan): see the windows branches.
#   <dnn_dir>   third_party/opus/src/dnn from the SAME checkout the artifact
#               was built from (G4 extracts its expected array names live
#               from here, and reads it for G9's companion info line).
#   <ssl_h>     third_party/boringssl/src/include/openssl/ssl.h from the same
#               checkout (G9, informational only).
# exit: 0 all gates pass | 1 a gate failed | 2 usage/tooling problem
set -eu
LC_ALL=C
export LC_ALL

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SELF_DIR/.." && pwd)
ASSERT_NO_KEY="$REPO_ROOT/ci/assert-no-key-strings.sh"

usage() { echo "usage: $0 <android|ios|windows> <plain> <artifact> <dnn_dir> <ssl_h>" >&2; exit 2; }
[ $# -eq 5 ] || usage
PLATFORM=$1
VARIANT=$2
ARTIFACT=$3
DNN_DIR=$4
SSL_H=$5

case "$PLATFORM" in android|ios|windows) ;; *) echo "::error::gates: platform must be android|ios|windows" >&2; exit 2 ;; esac
case "$VARIANT" in plain) ;; *) echo "::error::gates: variant must be plain (the -lk variant is gone)" >&2; exit 2 ;; esac
[ -e "$ARTIFACT" ] || { echo "::error::gates: artifact not found: $ARTIFACT" >&2; exit 2; }
[ -f "$ASSERT_NO_KEY" ] || { echo "::error::gates: missing $ASSERT_NO_KEY" >&2; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

gate_fail() { echo "::error::gates: $1 FAILED"; exit 1; }
gate_ok()   { echo "gates: $1 ok"; }

# ---------------------------------------------------------------------------
# nm/llvm-nm resolution. On a real CI runner these are NOT guaranteed to be
# on PATH by default - the correct one is the Clang toolchain gclient's
# hooks fetch into the checkout itself. dnn_dir is <src>/third_party/opus/src/dnn,
# so <src> = dnn_dir/../../../.. ; search webrtc's own vendored toolchain
# there first, then fall back to whatever the runner's PATH provides.
# ---------------------------------------------------------------------------
SRC_GUESS=$(CDPATH= cd -- "$DNN_DIR/../../../.." 2>/dev/null && pwd || true)
# dnn_dir is supposed to be <src>/third_party/opus/src/dnn (4 real levels
# below <src>). If it isn't - wrong argument, or a shallow/synthetic dnn_dir
# like a self-test fixture - going up 4 levels collapses onto "/" (or close
# to it) instead of a real checkout root. Concatenating that with a fixed
# suffix then produces a bogus path such as "//third_party/..." - and on a
# Windows/MSYS shell, executing a path starting with "//" is resolved as a
# UNC network path, which can hang for a long time waiting on SMB name
# resolution instead of failing fast. Refuse to build a candidate on top of
# a SRC_GUESS that isn't a plausible checkout root (a real one always has a
# DEPS file) - PATH is still tried below.
case "$SRC_GUESS" in
  */*) [ -f "$SRC_GUESS/DEPS" ] || SRC_GUESS="" ;;
  *) SRC_GUESS="" ;;
esac
# A candidate must actually RUN, not just have -x set (a stale/foreign binary
# at that path, or a broken symlink, would otherwise be trusted blindly for a
# gate that matters).
works() { [ -n "$1" ] && "$1" --version >/dev/null 2>&1; }
resolve_nm() {
  # $1 = android|ios|windows
  if [ "$1" = windows ]; then
    if [ -n "$SRC_GUESS" ]; then
      c="$SRC_GUESS/third_party/llvm-build/Release+Asserts/bin/llvm-nm.exe"
      works "$c" && { printf '%s\n' "$c"; return 0; }
    fi
    c=$(command -v llvm-nm 2>/dev/null || true)
    works "$c" && { printf '%s\n' "$c"; return 0; }
    return 1
  elif [ "$1" = android ]; then
    if [ -n "$SRC_GUESS" ]; then
      for c in \
        "$SRC_GUESS/third_party/llvm-build/Release+Asserts/bin/llvm-nm" \
        "$SRC_GUESS/third_party/android_ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/llvm-nm"; do
        works "$c" && { printf '%s\n' "$c"; return 0; }
      done
    fi
    c=$(command -v llvm-nm 2>/dev/null || true)
    works "$c" && { printf '%s\n' "$c"; return 0; }
    return 1
  else
    c=$(command -v nm 2>/dev/null || true)
    works "$c" && { printf '%s\n' "$c"; return 0; }
    return 1
  fi
}

# ---------------------------------------------------------------------------
# Collect the list of code binaries to scan, per platform, and (Android only)
# unpack the AAR once for every gate that needs its contents.
# ---------------------------------------------------------------------------
BINARIES=""
if [ "$PLATFORM" = android ]; then
  AAR_SCAN="$WORK/aar"
  mkdir -p "$AAR_SCAN"
  unzip -q -o "$ARTIFACT" -d "$AAR_SCAN"
  [ -d "$AAR_SCAN/jni" ] || gate_fail "G6 (no jni/ dir in AAR - is this a valid AAR?)"
  BINARIES=$(find "$AAR_SCAN/jni" -type f -name '*.so')
  [ -n "$BINARIES" ] || gate_fail "G1-G5 setup (no .so found under jni/)"
elif [ "$PLATFORM" = windows ]; then
  [ -d "$ARTIFACT" ] || { echo "::error::gates: windows artifact must be the staged directory" >&2; exit 2; }
  BINARIES=$(find "$ARTIFACT" -maxdepth 1 -type f -name 'webrtc.lib')
  [ -n "$BINARIES" ] || gate_fail "G1-G5 setup (no webrtc.lib in $ARTIFACT)"
  [ "$(printf '%s\n' $BINARIES | wc -l | tr -d ' ')" -eq 1 ] || gate_fail "G1-G5 setup (more than one webrtc.lib)"
else
  [ -d "$ARTIFACT" ] || { echo "::error::gates: ios artifact must be the .xcframework directory" >&2; exit 2; }
  # Mach-O slices only: the dSYM DWARF companions share the file name but
  # carry no __cstring data, so the mandatory positive control never matches.
  BINARIES=$(find "$ARTIFACT" -type f -name 'WebRTC' -not -path '*.dSYM/*')
  [ -n "$BINARIES" ] || gate_fail "G1-G5 setup (no Mach-O slice found under $ARTIFACT)"
fi
echo "gates: scanning $(printf '%s\n' $BINARIES | wc -l | tr -d ' ') binary file(s):"
printf '%s\n' $BINARIES

# ---------------------------------------------------------------------------
# G1 - no key material in any binary. -P is MANDATORY on every platform
# (finding #17: the M144 iOS workflow used the soft '-p', which let a scan
# that can't even see the code pass silently - never do that again). -F
# 'password_=' catches the m150 P3 addition (ICE password no-log, on top of
# the original derived_key/slat/raw_key literals).
# ---------------------------------------------------------------------------
echo "::group::G1: no key material (raw byte scan, positive control mandatory)"
# shellcheck disable=SC2086
sh "$ASSERT_NO_KEY" -m 5000000 -P 'Failed to derive HkdfSha256 key from secret' -F 'password_=' $BINARIES
echo "::endgroup::"
gate_ok G1

# ---------------------------------------------------------------------------
# G2 - transport strictness marker (P6/P7 self-report via qaudion_tuning).
#   plain: '!AESGCM+AES256' absent AND 'transport=strict' present somewhere.
#   (the LiveKit-prefixed -lk variant, which was 'transport=switchable', is gone)
# ---------------------------------------------------------------------------
echo "::group::G2: transport marker ($VARIANT)"
any_hit() { lit=$1; for f in $BINARIES; do grep -a -q -F -- "$lit" "$f" && return 0; done; return 1; }
if any_hit '!AESGCM+AES256'; then gate_fail "G2 (plain build must not contain the relaxed-cipher marker '!AESGCM+AES256')"; fi
any_hit 'transport=strict' || gate_fail "G2 (plain build missing 'transport=strict')"
echo "::endgroup::"
gate_ok G2

# ---------------------------------------------------------------------------
# G3 - Q-AUDION self-verification log lines present (dtls, opus-dec, opus-enc,
# frame-replay). 'Q-AUDION frame-replay' is the P12 receiver anti-replay drop
# line (api/crypto/frame_crypto_transformer.cc): its presence proves the
# replay window is compiled into every shipped binary.
# ---------------------------------------------------------------------------
echo "::group::G3: Q-AUDION self-check lines"
for lit in 'Q-AUDION dtls' 'Q-AUDION opus-dec' 'Q-AUDION opus-enc' 'Q-AUDION frame-replay'; do
  any_hit "$lit" || gate_fail "G3 (missing self-check line: $lit)"
done
echo "::endgroup::"
gate_ok G3

# ---------------------------------------------------------------------------
# G4 - deep-PLC / FARGAN model weights actually compiled in. The 3 expected
# array names are extracted LIVE from the checked-out dnn_dir (never
# hardcoded here), so this gate self-adapts if the source author repins
# Opus/the model. Detection tries, in order: (a) full (non-dynamic) symbol
# table via nm/llvm-nm - static const arrays only show up here if some
# debug info survived symbol_level=1; (b) a raw byte scan, which catches the
# same names if they only survive as DWARF .debug_str text. If NEITHER
# finds any of the 3 names, we fail closed rather than assume the weights
# were optimized away silently.
#   NOTE (flagged, not hidden): this gate's detection method is UNVERIFIED
#   against a real compiled M150 artifact - no such artifact exists yet.
#   First real CI run must confirm nm/byte-scan actually finds these names;
#   if symbol_level=1 strips them entirely, G4 needs a different signal
#   (e.g. a dedicated P8 self-check line, like G3) and this comment should
#   be updated when that's known.
# ---------------------------------------------------------------------------
echo "::group::G4: deep-PLC / FARGAN weights present"
[ -f "$DNN_DIR/plc_data.c" ] || gate_fail "G4 (dnn_dir has no plc_data.c: $DNN_DIR)"
[ -f "$DNN_DIR/fargan_data.c" ] || gate_fail "G4 (dnn_dir has no fargan_data.c: $DNN_DIR)"
NAME1=$(grep -m1 -oE '^const WeightArray [A-Za-z_0-9]+\[\]' "$DNN_DIR/plc_data.c" | awk '{print $3}' | tr -d '[]')
NAME2=$(grep -m1 -oE '^const WeightArray [A-Za-z_0-9]+\[\]' "$DNN_DIR/fargan_data.c" | awk '{print $3}' | tr -d '[]')
NAME3=$(grep -m1 -oE '^static const (float|opus_int8) [A-Za-z_0-9]+\[' "$DNN_DIR/plc_data.c" | awk '{print $4}' | tr -d '[')
[ -n "$NAME1" ] && [ -n "$NAME2" ] && [ -n "$NAME3" ] || gate_fail "G4 (could not extract expected array names from $DNN_DIR - source layout changed?)"
echo "expected array names: $NAME1 / $NAME2 / $NAME3"
NM4=$(resolve_nm "$PLATFORM" || true)
[ -n "$NM4" ] && echo "G4: using nm binary: $NM4" || echo "G4: no nm/llvm-nm resolved - relying on raw byte scan only"
found=0
for f in $BINARIES; do
  hits=""
  if [ -n "$NM4" ]; then
    hits=$("$NM4" -a "$f" 2>/dev/null | grep -cE "($NAME1|$NAME2|$NAME3)" || true)
  fi
  if [ -n "$hits" ] && [ "$hits" -gt 0 ] 2>/dev/null; then found=1; fi
  # fallback: raw byte / DWARF-string scan, same technique as G1/G2/G3
  for n in "$NAME1" "$NAME2" "$NAME3"; do
    grep -a -q -F -- "$n" "$f" 2>/dev/null && found=1
  done
done
[ "$found" -eq 1 ] || gate_fail "G4 (none of $NAME1/$NAME2/$NAME3 found in any binary via nm or raw scan)"
echo "::endgroup::"
gate_ok G4

# ---------------------------------------------------------------------------
# G5 - no codec internals exported. Android: llvm-nm -D --defined-only must
# have zero opus_/lpcnet_/fargan_/silk_/celt_ hits. iOS: nm -gU likewise.
# ---------------------------------------------------------------------------
echo "::group::G5: no exported codec symbols"
CODEC_RE='^(opus_|lpcnet_|fargan_|silk_|celt_)'
if [ "$PLATFORM" = windows ]; then
  # A static library has no dynamic symbol table: every opus_*/silk_*/celt_*
  # symbol is (necessarily) defined inside webrtc.lib, so "no exported codec
  # symbols" is checked where it can leak into a consumer's DLL export table:
  # no /EXPORT: linker directive (.drectve) may name a codec symbol.
  for f in $BINARIES; do
    n_exp=$(grep -a -o -E '/EXPORT:[A-Za-z0-9_?@$]+' "$f" | wc -l | tr -d ' ')
    bad=$(grep -a -o -E '/EXPORT:[A-Za-z0-9_?@$]+' "$f" | grep -c -E '/EXPORT:_?(opus_|lpcnet_|fargan_|silk_|celt_)' || true)
    echo "G5: $f carries $n_exp /EXPORT: directive(s), $bad naming codec internals"
    [ "${bad:-0}" -eq 0 ] 2>/dev/null || gate_fail "G5 ($f has $bad /EXPORT: directive(s) for codec-internal symbols)"
  done
  echo "::endgroup::"
  gate_ok G5
else
NM5=$(resolve_nm "$PLATFORM" || true)
[ -n "$NM5" ] || gate_fail "G5 (no llvm-nm/nm resolved - cannot verify absence of exported codec symbols, refusing to pass by default)"
echo "G5: using nm binary: $NM5"
for f in $BINARIES; do
  if [ "$PLATFORM" = android ]; then
    bad=$("$NM5" -D --defined-only "$f" 2>/dev/null | awk '{print $3}' | grep -cE "$CODEC_RE" || true)
  else
    bad=$("$NM5" -gU "$f" 2>/dev/null | awk '{print $3}' | sed 's/^_//' | grep -cE "$CODEC_RE" || true)
  fi
  if [ -n "$bad" ] && [ "$bad" -gt 0 ] 2>/dev/null; then
    gate_fail "G5 ($f exports $bad codec-internal symbol(s) matching $CODEC_RE)"
  fi
done
echo "::endgroup::"
gate_ok G5
fi

# ---------------------------------------------------------------------------
# G6 - ABI/slice allow-list.
#   android: jni/ must contain ONLY arm64-v8a.
#   ios: the xcframework's Info.plist LibraryIdentifier set must be exactly
#        {ios-arm64, ios-arm64-simulator}.
# ---------------------------------------------------------------------------
echo "::group::G6: ABI allow-list"
if [ "$PLATFORM" = windows ]; then
  # Every COFF object in the archive must be x64 (IMAGE_FILE_MACHINE_AMD64,
  # 0x8664); any i386/ARM/ARM64 member fails, and an archive with fewer than
  # 100 x64 members is an empty/wrong build.
  PYG6=""
  for cand in python3 python py; do
    if command -v "$cand" >/dev/null 2>&1 && "$cand" -c 'pass' >/dev/null 2>&1; then PYG6=$cand; break; fi
  done
  [ -n "$PYG6" ] || gate_fail "G6 (no working python to parse the COFF archive)"
  for f in $BINARIES; do
    "$PYG6" - "$f" <<'PYEOF' || gate_fail "G6 (webrtc.lib is not a pure x64 COFF archive)"
import struct, sys
data = open(sys.argv[1], 'rb').read()
if data[:8] != b'!<arch>\n':
    print("not an ar archive (thin archive?)"); sys.exit(1)
pos, seen, order = 8, {}, 0
while pos + 60 <= len(data):
    name = data[pos:pos+16].decode('latin1').strip()
    size = int(data[pos+48:pos+58].decode('latin1').strip())
    body = pos + 60
    if name not in ('/', '//') and size >= 20:
        m = struct.unpack_from('<H', data, body)[0]
        if m == 0 and struct.unpack_from('<H', data, body + 2)[0] == 0xFFFF:
            m = struct.unpack_from('<H', data, body + 6)[0]
        seen[m] = seen.get(m, 0) + 1
    pos = body + size + (size & 1)
print("machine types in webrtc.lib: " + ", ".join("0x%04x x%d" % kv for kv in sorted(seen.items())))
bad = [m for m in seen if m in (0x14c, 0x1c0, 0x1c4, 0xaa64)]
if bad or seen.get(0x8664, 0) < 100:
    sys.exit(1)
PYEOF
  done
elif [ "$PLATFORM" = android ]; then
  ABIS=$(find "$AAR_SCAN/jni" -mindepth 1 -maxdepth 1 -type d -exec basename {} \; | sort)
  [ "$ABIS" = "arm64-v8a" ] || gate_fail "G6 (jni/ ABI set is [$ABIS], want exactly arm64-v8a)"
else
  PLIST="$ARTIFACT/Info.plist"
  [ -f "$PLIST" ] || gate_fail "G6 (no Info.plist in $ARTIFACT)"
  IDS=$(grep -A1 'LibraryIdentifier' "$PLIST" | grep -oE '<string>[^<]+</string>' | sed -e 's/<string>//' -e 's/<\/string>//' | sort -u)
  WANT=$(printf 'ios-arm64\nios-arm64-simulator\n' | sort -u)
  [ "$IDS" = "$WANT" ] || gate_fail "G6 (xcframework LibraryIdentifiers are [$IDS], want exactly ios-arm64/ios-arm64-simulator)"
fi
echo "::endgroup::"
gate_ok G6

# ---------------------------------------------------------------------------
# G7 - the code binary must grow by 1-6 MB vs the M144 build of the SAME
# variant (a WAY-off size is a sign of a broken/empty/wrong-arch build).
# Baselines below are exact bytes of what this repo has actually shipped for
# M144; where no M144 build of that variant has shipped yet there is no
# baseline to diff against, so G7 is reported informational-only (does not
# fail the run) and prints a reminder to pin a real baseline once an M144
# (or first M150) build of that variant exists.
# ---------------------------------------------------------------------------
echo "::group::G7: size delta vs M144 baseline (same variant)"
case "${PLATFORM}-${VARIANT}" in
  android-plain) BASELINE=13648040 ;;
  *) BASELINE=0 ;;
esac
TOTAL=0
for f in $BINARIES; do TOTAL=$((TOTAL + $(wc -c < "$f"))); done
echo "current total .so/Mach-O bytes: $TOTAL"
if [ "$BASELINE" -gt 0 ]; then
  DELTA=$((TOTAL - BASELINE))
  echo "M144 baseline: $BASELINE, delta: $DELTA bytes"
  if [ "$DELTA" -lt 1000000 ] || [ "$DELTA" -gt 6000000 ]; then
    gate_fail "G7 (delta ${DELTA}B is outside the expected 1-6 MB growth window vs M144 baseline $BASELINE)"
  fi
else
  echo "::warning::G7: no pinned M144 baseline for ${PLATFORM}-${VARIANT} yet - skipped (informational), size=$TOTAL"
fi
echo "::endgroup::"
gate_ok G7

# ---------------------------------------------------------------------------
# G8 - P8 runtime API present: Android classes.jar has
# setQaudionOpusEncoderComplexity under org/webrtc/,
# AND the .so has the matching JNI symbol/string. iOS: the umbrella header
# under Headers/ declares it, and the selector is in the binary.
# ---------------------------------------------------------------------------
echo "::group::G8: P8 tuning API present"
if [ "$PLATFORM" = windows ]; then
  # C++ API of P8 (rtc_base/qaudion_tuning.h): declared in the shipped headers
  # AND defined in webrtc.lib (MSVC-ABI mangled name, e.g.
  # ?SetOpusEncoderComplexity@qaudion@webrtc@@YAXH@Z - matched loosely).
  QH="$ARTIFACT/include/rtc_base/qaudion_tuning.h"
  [ -f "$QH" ] || gate_fail "G8 (rtc_base/qaudion_tuning.h missing from the staged headers)"
  for fn in SetOpusEncoderComplexity SetOpusDecoderComplexity SetOpusMinPacketLossPercent SetRequireDtlsPqc RaiseTransportLevel TransportLevel; do
    grep -q -F "$fn" "$QH" || gate_fail "G8 (qaudion_tuning.h does not declare $fn)"
    found=0
    for f in $BINARIES; do
      grep -a -q -F "${fn}@qaudion" "$f" 2>/dev/null && found=1
    done
    [ "$found" -eq 1 ] || gate_fail "G8 (no ${fn}@qaudion symbol in webrtc.lib)"
  done
elif [ "$PLATFORM" = android ]; then
  CJ="$AAR_SCAN/classes.jar"
  [ -f "$CJ" ] || gate_fail "G8 (no classes.jar in AAR)"
  unzip -l "$CJ" | grep -qE 'org/webrtc/PeerConnectionFactory\.class' || gate_fail "G8 (PeerConnectionFactory.class not found in classes.jar)"
  # The Java method lives in classes.jar; the .so only carries the jni_zero
  # native export, e.g. Java_org_webrtc_PeerConnectionFactory_nativeSetQaudionOpusEncoderComplexity
  # (capital S after the 'native' prefix), so match the .so case-insensitively.
  PCF_CLASS=$(unzip -l "$CJ" | awk '{print $4}' | grep -E '^org/webrtc/PeerConnectionFactory\.class$' | head -1)
  unzip -p "$CJ" "$PCF_CLASS" | grep -a -q -F 'setQaudionOpusEncoderComplexity' || gate_fail "G8 (PeerConnectionFactory.class lacks setQaudionOpusEncoderComplexity)"
  found=0
  for f in $BINARIES; do
    grep -a -q -i -F 'QaudionOpusEncoderComplexity' "$f" 2>/dev/null && found=1
  done
  [ "$found" -eq 1 ] || gate_fail "G8 (no QaudionOpusEncoderComplexity JNI export in any .so)"
else
  # Several headers match *PeerConnectionFactory* (e.g. ...FactoryOptions.h),
  # so look for the declaration itself in the framework's public headers.
  HDR=$(find "$ARTIFACT" -path '*/Headers/*' -name '*.h' -exec grep -l 'setQaudionOpusEncoderComplexity' {} + 2>/dev/null | head -1)
  [ -n "$HDR" ] || gate_fail "G8 (no public header under Headers/ declares setQaudionOpusEncoderComplexity in $ARTIFACT)"
  case "$HDR" in *PeerConnectionFactory.h) ;; *) gate_fail "G8 (setQaudionOpusEncoderComplexity declared in unexpected header $HDR)";; esac
  found=0
  for f in $BINARIES; do
    grep -a -q -F 'setQaudionOpusEncoderComplexity' "$f" 2>/dev/null && found=1
  done
  [ "$found" -eq 1 ] || gate_fail "G8 (selector setQaudionOpusEncoderComplexity: not found in any Mach-O slice)"
fi
echo "::endgroup::"
gate_ok G8

# ---------------------------------------------------------------------------
# G10 - Android only: every .class file in classes.jar must be Java 17 or
# older (class-file major version <= 61 - JVMS 4.1, bytes 6-7, big-endian
# u16). The app compiles Java/Kotlin with target 17 and its JVM unit tests
# run on JDK 17 only (VPS and the self-hosted CI have no other JDK
# installed); the M150 build/ repo used to hard-code javac's --release to
# 21 (major version 65) until P10, which this gate is the backstop for -
# a future upstream/build-repo change that reintroduces --release 21 (or
# higher) must fail the build here, not surface later as 122 JVM unit test
# UnsupportedClassVersionError failures. iOS ships no classes.jar, so this
# gate only runs for platform=android. Fails CLOSED: zero .class files
# found is itself a failure, never a silent pass.
# ---------------------------------------------------------------------------
if [ "$PLATFORM" = android ]; then
  echo "::group::G10: classes.jar bytecode <= Java 17 (major version 61)"
  CJ10="$AAR_SCAN/classes.jar"
  [ -f "$CJ10" ] || gate_fail "G10 (no classes.jar in AAR)"
  CJ10_SCAN="$WORK/classes-g10"
  mkdir -p "$CJ10_SCAN"
  unzip -q -o "$CJ10" -d "$CJ10_SCAN"
  CLASS_FILES=$(find "$CJ10_SCAN" -type f -name '*.class')
  [ -n "$CLASS_FILES" ] || gate_fail "G10 (no .class files found in classes.jar)"
  N_CLASSES=0
  BAD=0
  for cf in $CLASS_FILES; do
    N_CLASSES=$((N_CLASSES + 1))
    B6=$(od -An -tu1 -j 6 -N 1 "$cf" | tr -d '[:space:]')
    B7=$(od -An -tu1 -j 7 -N 1 "$cf" | tr -d '[:space:]')
    MAJOR=$((B6 * 256 + B7))
    if [ "$MAJOR" -gt 61 ]; then
      BAD=$((BAD + 1))
      echo "::error::G10: $cf has class-file major version $MAJOR (> 61 / Java 17)"
    fi
  done
  echo "G10: checked $N_CLASSES .class file(s), major version <= 61"
  [ "$BAD" -eq 0 ] || gate_fail "G10 ($BAD .class file(s) exceed major version 61 / Java 17)"
  echo "::endgroup::"
  gate_ok G10
fi

# ---------------------------------------------------------------------------
# G9 - informational only: print which SSL_GROUP_*MLKEM1024* defines the
# pinned BoringSSL exposes, so a human can eyeball PQC group support. Never
# fails the run.
# ---------------------------------------------------------------------------
echo "::group::G9 (informational): BoringSSL MLKEM1024 defines"
if [ -f "$SSL_H" ]; then
  grep -E '#define SSL_GROUP_.*MLKEM1024' "$SSL_H" || echo "(none found - check BoringSSL pin)"
else
  echo "::warning::G9: ssl_h not found at $SSL_H - skipped"
fi
echo "::endgroup::"

echo "gates: ALL GATES PASSED for ${PLATFORM}/${VARIANT}"
