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
#          YAML twice. Needs `gradle` (shadowJar via shadow-relocate/),
#          `unzip`/`zip`, and GNU coreutils `timeout` on PATH; the workflow
#          sets up Java+Gradle before calling this. The lk shadowJar
#          invocation runs under a hard `timeout` with bounded GRADLE_OPTS -
#          see the comment above that call for why.
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

# Gradle's default GRADLE_USER_HOME (~/.gradle) lives on the runner's root
# filesystem. Run 36621762942's build(plain) leg - same sync/patch/build
# steps as lk, minus packaging - printed `df -h /` right before this script
# runs and showed root at 100% full (253M free out of 145G), while the big
# build volume (TMPDIR now lives there, see build-m150-android.yml) had 71G
# free. gradle/actions/setup-gradle does not relocate GRADLE_USER_HOME, so
# every lk run to date has pointed the Gradle 7.6 distribution + Shadow
# plugin + dependency caches at that nearly-full root disk. A write that
# lands on a filesystem with a few hundred MB left doesn't reliably surface
# as a clean ENOSPC - it can degrade the whole host (journald, the runner
# agent's own log upload) before any process gets a clean I/O error, which
# matches "runner lost, no retrievable log" far better than a plain resource
# or network stall would. $WORK already inherits TMPDIR, so anchoring
# GRADLE_USER_HOME to it moves every Gradle write onto the same large volume
# without needing to know the absolute workdir path here.
#
# Must be an unconditional assignment, not "${GRADLE_USER_HOME:-...}": run
# 36633174033 (first run with this block in place) still showed root at
# only ~1.1-1.2G free around the gradle invocation, and its "Gate G1-G10"
# step env dump confirmed why - gradle/actions/setup-gradle unconditionally
# exports GRADLE_USER_HOME=/home/runner/.gradle into $GITHUB_ENV during the
# "Set up Gradle 7.6" step, so it is ALREADY non-empty by the time this
# script runs and the :- default never fired. This build still passed on
# that ~1G margin, but that's luck, not the fix working - force it here.
export GRADLE_USER_HOME="$WORK/gradle-home"
mkdir -p "$GRADLE_USER_HOME"

echo "::group::package-android: relocate classes.jar (org.webrtc -> livekit.org.webrtc)"
mkdir -p "$SHADOW_DIR/libs"
cp "$RAWD/classes.jar" "$SHADOW_DIR/libs/classes.jar"

# Runs 36547153188 and 36598248775 hung here ~30 min with no retrievable log
# (runner lost) after run 36534186215 hit ENOSPC in /tmp. Two independent
# failure modes for one step means it gets a hard budget instead of trusting
# the job's 350-minute ceiling: an unbounded --no-daemon JVM (heap sized off
# whatever RAM maximize-build-space left) can either OOM the runner outright
# or, short of that, start swapping hard enough to look identical to a
# network stall; a plugin/dependency resolution call with no explicit HTTP
# timeout can also sit retrying against a half-open TCP connection for a
# very long time. `timeout` below turns either failure into a loud, fast
# exit instead of a silent one; GRADLE_OPTS bounds the JVM so a real hang is
# more likely to be the network path (visible in --info) than raw memory
# pressure.
report_resources() {
  label=$1
  echo "package-android: $label disk (/ = root, likely near-full; \$SHADOW_DIR project dir; \$GRADLE_USER_HOME = big volume):"
  df -h / "$SHADOW_DIR" "$GRADLE_USER_HOME" 2>&1 | sed 's/^/  /'
  echo "package-android: $label memory:"
  (free -h 2>/dev/null || vm_stat 2>/dev/null || echo "  (no free/vm_stat on this host)") | sed 's/^/  /'
}

report_resources "pre-gradle"

export GRADLE_OPTS="-Xmx2g -Xms256m -Dorg.gradle.internal.http.connectionTimeout=60000 -Dorg.gradle.internal.http.socketTimeout=60000 -Dorg.gradle.internal.repository.max.retries=1"
GRADLE_STEP_TIMEOUT=20m

set +e
(
  cd "$SHADOW_DIR"
  timeout -k 30s "$GRADLE_STEP_TIMEOUT" gradle --no-daemon --console=plain --stacktrace --info shadowJar
)
gradle_rc=$?
set -e
report_resources "post-gradle"
if [ "$gradle_rc" -eq 124 ] || [ "$gradle_rc" -eq 137 ]; then
  echo "::error::package-android: gradle shadowJar timed out after $GRADLE_STEP_TIMEOUT (rc=$gradle_rc) - likely stuck plugin/dependency resolution or an OOM-adjacent stall, see --info output and the resource report above" >&2
  exit 1
elif [ "$gradle_rc" -ne 0 ]; then
  echo "::error::package-android: gradle shadowJar failed (rc=$gradle_rc)" >&2
  exit 1
fi

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
