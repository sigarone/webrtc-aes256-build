#!/bin/sh
# package-android.sh - turn the raw build_aar.py output into the shippable
# artifact: the AAR passes through unchanged (plain org.webrtc; the
# LiveKit-prefixed shadow-relocate variant is gone) after the G1 spot-check, so
# a broken raw AAR fails here, not three steps later in gates.sh.
#
# usage: package-android.sh <plain> <raw_aar> <out_dir>
# exit: 0 ok | 1 packaging failed | 2 usage problem
set -eu

REPO_ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)
ASSERT_NO_KEY="$REPO_ROOT/ci/assert-no-key-strings.sh"

usage() { echo "usage: $0 <plain> <raw_aar> <out_dir>" >&2; exit 2; }
[ $# -eq 3 ] || usage
VARIANT=$1
RAW_AAR=$2
OUT_DIR=$3

case "$VARIANT" in plain) ;; *) echo "::error::package-android: variant must be plain (the -lk variant is gone)" >&2; exit 2 ;; esac
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

OUT="$OUT_DIR/webrtc-android-m150-plain.aar"
cp "$RAW_AAR" "$OUT"
spot_check "$OUT"
echo "package-android: plain -> $OUT"
