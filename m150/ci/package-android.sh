#!/bin/sh
# package-android.sh - turn the raw build_aar.py output into the shippable
# artifact for the requested variant.
#   plain: pass the AAR through unchanged (still runs the G1 spot-check so a
#          broken raw AAR fails here, not three steps later in gates.sh).
#   lk:    reproduce webrtc-sdk/android's own shadow-relocation (org.webrtc ->
#          livekit.org.webrtc in classes.jar) + native lib rename
#          (libjingle_peerconnection_so.so -> liblkjingle_peerconnection_so.so)
#          and repackage, EXACTLY the steps build-livekit-android.yml (M144)
#          already does inline - lifted out to a script so build-m150-android.yml
#          can call one line per variant instead of duplicating ~60 lines of
#          YAML twice. Needs `gradle` (shadowJar via shadow-relocate/) and
#          `unzip`/`zip` on PATH; the workflow sets up Java+Gradle before
#          calling this.
#
# usage: package-android.sh <plain|lk> <raw_aar> <out_dir>
# exit: 0 ok | 1 packaging failed | 2 usage problem
set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
ASSERT_NO_KEY="$REPO_ROOT/ci/assert-no-key-strings.sh"
SHADOW_DIR="$REPO_ROOT/shadow-relocate"

usage() { echo "usage: $0 <plain|lk> <raw_aar> <out_dir>" >&2; exit 2; }
[ $# -eq 3 ] || usage
VARIANT=$1
RAW_AAR=$2
OUT_DIR=$3

case "$VARIANT" in plain|lk) ;; *) echo "::error::package-android: variant must be plain|lk" >&2; exit 2 ;; esac
[ -f "$RAW_AAR" ] || { echo "::error::package-android: raw AAR not found: $RAW_AAR" >&2; exit 2; }
mkdir -p "$OUT_DIR"
OUT_DIR=$(CDPATH= cd -- "$OUT_DIR" && pwd)

spot_check() {
  aar=$1
  scan=$(mktemp -d)
  unzip -q -o "$aar" -d "$scan"
  sos=$(find "$scan/jni" -type f -name '*.so')
  [ -n "$sos" ] || { echo "::error::package-android: no .so under jni/ in $aar" >&2; rm -rf "$scan"; exit 1; }
  # shellcheck disable=SC2086
  sh "$ASSERT_NO_KEY" -m 5000000 -P 'Failed to derive HkdfSha256 key from secret' -F 'password_=' $sos
  rm -rf "$scan"
}

if [ "$VARIANT" = plain ]; then
  OUT="$OUT_DIR/webrtc-android-m150-plain.aar"
  cp "$RAW_AAR" "$OUT"
  spot_check "$OUT"
  echo "package-android: plain -> $OUT"
  exit 0
fi

# --- lk: shadow-relocate + rename ------------------------------------------
[ -d "$SHADOW_DIR" ] || { echo "::error::package-android: $SHADOW_DIR missing" >&2; exit 2; }
command -v gradle >/dev/null 2>&1 || { echo "::error::package-android: gradle not on PATH (workflow must set up Java+Gradle first)" >&2; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
RAWD="$WORK/raw"
mkdir -p "$RAWD"
unzip -q -o "$RAW_AAR" -d "$RAWD"
find "$RAWD" -maxdepth 2

echo "::group::package-android: relocate classes.jar (org.webrtc -> livekit.org.webrtc)"
mkdir -p "$SHADOW_DIR/libs"
cp "$RAWD/classes.jar" "$SHADOW_DIR/libs/classes.jar"
(
  cd "$SHADOW_DIR"
  gradle --no-daemon shadowJar
)
RELOCATED=$(find "$SHADOW_DIR/build/libs" -name '*-all.jar' | head -1)
[ -n "$RELOCATED" ] || { echo "::error::package-android: shadowJar produced no *-all.jar" >&2; exit 1; }
unzip -l "$RELOCATED" | grep -q 'livekit/org/webrtc/' || { echo "::error::package-android: relocated jar has no livekit/org/webrtc/ entries" >&2; exit 1; }
if unzip -l "$RELOCATED" | grep -qE '  org/webrtc/'; then
  echo "::error::package-android: relocation incomplete - bare org/webrtc/ still present" >&2
  exit 1
fi
cp "$RELOCATED" "$RAWD/classes.jar"
echo "::endgroup::"

echo "::group::package-android: rename native lib"
renamed=0
for f in "$RAWD"/jni/*/libjingle_peerconnection_so.so; do
  [ -e "$f" ] || continue
  mv "$f" "$(dirname "$f")/liblkjingle_peerconnection_so.so"
  renamed=$((renamed + 1))
done
[ "$renamed" -gt 0 ] || { echo "::error::package-android: no jni/*/libjingle_peerconnection_so.so to rename" >&2; find "$RAWD" -name '*.so'; exit 1; }
echo "::endgroup::"

echo "::group::package-android: repackage final AAR"
OUT="$OUT_DIR/webrtc-android-lk-m150-prefixed.aar"
rm -f "$OUT"
(
  cd "$RAWD"
  extra=""
  for opt in R.txt proguard.txt lint.jar; do [ -e "$opt" ] && extra="$extra $opt"; done
  # shellcheck disable=SC2086
  zip -qr "$OUT" AndroidManifest.xml classes.jar jni $extra
)
ls -lh "$OUT"
spot_check "$OUT"
echo "::endgroup::"

echo "package-android: lk -> $OUT"
