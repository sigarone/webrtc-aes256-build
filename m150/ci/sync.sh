#!/bin/sh
# sync.sh - install a PINNED depot_tools, then gclient sync the M150
# webrtc-sdk/webrtc source at an EXACT commit, and hard-verify afterwards
# that src, third_party/boringssl/src and third_party/opus/src all landed on
# the pinned commits (fail closed on ANY mismatch: a moved fork tag, a drifted
# DEPS entry, a partial/failed sync must never silently proceed to a build).
#
# Why a fork tag and not `gclient sync --revision src@<sha>` on the upstream
# webrtc-sdk/webrtc.git URL directly: GitHub's shallow-clone protocol refuses
# `git fetch <sha> --depth=1` for a commit that is not a ref tip (confirmed
# for this exact repo by this project's own M144 CI history - see build.yml's
# webrtc_ref comment). A lightweight tag on a fork IS a ref tip and IS
# shallow-fetchable. This script therefore syncs from a fork URL (default
# https://github.com/sigarone/webrtc.git, override with WEBRTC_FORK_URL) at a
# tag/ref name, and then hard-checks the resulting commit against the full
# 40-hex pin below - so a moved/wrong tag fails the build instead of silently
# shipping the wrong source. See Appendix B of webrtc-plan.md for the exact
# public commands (repo sync + tag creation) the fork owner runs once before
# a webrtc_ref depending on them can be dispatched - this script does not and
# cannot perform those, by design (no push access from CI).
#
# usage: sync.sh <workdir> <webrtc_ref> <target_os_csv>
#   <workdir>        e.g. /home/runner/webrtc - gets .gclient + src/ under it
#   <webrtc_ref>     tag/branch on the fork (see above). Charset-restricted.
#   <target_os_csv>  gclient target_os, comma separated, e.g. "android,unix"
#                     or "ios,mac"
# env:
#   WEBRTC_FORK_URL     default https://github.com/sigarone/webrtc.git
#   DEPOT_TOOLS_SHA     REQUIRED, full 40-hex commit of chromium/tools/depot_tools
#                        to pin (finding #18). No default: an unpinned
#                        depot_tools drifts silently (exactly what finding #18
#                        flagged in the M144 workflows) - resolve it once with
#                        `git ls-remote https://chromium.googlesource.com/chromium/tools/depot_tools.git HEAD`
#                        (a commit contemporary with the m150 cut, ~2026-05)
#                        and pass it in via the workflow env. This script
#                        refuses to guess and fails closed if it is unset.
# exit: 0 ok | 1 sync/verification failed | 2 usage/env problem
set -eu

usage() { echo "usage: $0 <workdir> <webrtc_ref> <target_os_csv>" >&2; exit 2; }
[ $# -eq 3 ] || usage
WORKDIR=$1
WEBRTC_REF=$2
TARGET_OS_CSV=$3

case "$WEBRTC_REF" in ''|*[!A-Za-z0-9._/-]*) echo "::error::sync: webrtc_ref charset [A-Za-z0-9._/-] only" >&2; exit 2 ;; esac
case "$TARGET_OS_CSV" in ''|*[!A-Za-z0-9,_-]*) echo "::error::sync: target_os charset [A-Za-z0-9,_-] only" >&2; exit 2 ;; esac

: "${WEBRTC_FORK_URL:=https://github.com/sigarone/webrtc.git}"
if [ -z "${DEPOT_TOOLS_SHA:-}" ]; then
  echo "::error::sync: DEPOT_TOOLS_SHA is not set - refusing to install an unpinned depot_tools (finding #18)." >&2
  echo "  Resolve once with: git ls-remote https://chromium.googlesource.com/chromium/tools/depot_tools.git HEAD" >&2
  echo "  then pass it as env DEPOT_TOOLS_SHA in the workflow (a commit contemporary with the m150 cut, ~2026-05)." >&2
  exit 2
fi
case "$DEPOT_TOOLS_SHA" in [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;; *) echo "::error::sync: DEPOT_TOOLS_SHA must be a full 40-hex commit" >&2; exit 2 ;; esac

# Pins resolved 2026-09-28 from webrtc-sdk/webrtc@ba469aa2093ba950066258ca0a59a6fbd1295582's
# own DEPS file (raw.githubusercontent.com), i.e. these are NOT independently
# chosen - they are exactly what that commit's DEPS says src/third_party/{
# boringssl,opus}/src must be at. See m150/README.md for how to re-derive
# them if the source author ever re-pins webrtc_ref.
WEBRTC_PIN=ba469aa2093ba950066258ca0a59a6fbd1295582
BORINGSSL_PIN=f91f1447397c6719f9774dfb8e67329378e1f3d3
OPUS_PIN=55513e81d8f606bd75d0ff773d2144e5f2a732f5

mkdir -p "$WORKDIR"
WORKDIR=$(CDPATH= cd -- "$WORKDIR" && pwd)

echo "::group::sync: install depot_tools @ $DEPOT_TOOLS_SHA"
if [ ! -d "$WORKDIR/depot_tools/.git" ]; then
  git clone https://chromium.googlesource.com/chromium/tools/depot_tools.git "$WORKDIR/depot_tools"
fi
git -C "$WORKDIR/depot_tools" fetch --depth 1 origin "$DEPOT_TOOLS_SHA"
git -C "$WORKDIR/depot_tools" checkout --detach "$DEPOT_TOOLS_SHA"
ACTUAL_DT=$(git -C "$WORKDIR/depot_tools" rev-parse HEAD)
[ "$ACTUAL_DT" = "$DEPOT_TOOLS_SHA" ] || { echo "::error::sync: depot_tools checkout landed on $ACTUAL_DT, not $DEPOT_TOOLS_SHA" >&2; exit 1; }
echo "$WORKDIR/depot_tools" >> "${GITHUB_PATH:-/dev/null}" 2>/dev/null || true
PATH="$WORKDIR/depot_tools:$PATH"
export PATH DEPOT_TOOLS_UPDATE=0
echo "::endgroup::"

echo "::group::sync: gclient config + sync ($WEBRTC_REF, target_os=$TARGET_OS_CSV)"
TARGET_OS_PY=$(printf '%s' "$TARGET_OS_CSV" | awk -F',' '{for(i=1;i<=NF;i++){printf "\"%s\"%s", $i, (i<NF?", ":"")}}')
cat > "$WORKDIR/.gclient" <<EOF
solutions = [{
  "name": "src",
  "url": "${WEBRTC_FORK_URL}@${WEBRTC_REF}",
  "deps_file": "DEPS",
  "managed": False,
  "custom_deps": {},
}]
target_os = [${TARGET_OS_PY}]
EOF
(
  cd "$WORKDIR"
  gclient sync --no-history --shallow --nohooks
  gclient runhooks
)
echo "::endgroup::"

echo "::group::sync: hard pin verification (fail closed)"
fail=0
check_pin() {
  label=$1; dir=$2; want=$3
  if [ ! -d "$dir/.git" ]; then
    echo "::error::sync: $label checkout missing at $dir" >&2
    fail=1
    return
  fi
  got=$(git -C "$dir" rev-parse HEAD)
  echo "$label: got=$got want=$want"
  if [ "$got" != "$want" ]; then
    echo "::error::sync: $label is at $got, expected exactly $want (full 40-hex, not a prefix match)" >&2
    fail=1
  fi
}
check_pin "webrtc src"           "$WORKDIR/src"                             "$WEBRTC_PIN"
check_pin "third_party/boringssl" "$WORKDIR/src/third_party/boringssl/src"  "$BORINGSSL_PIN"
check_pin "third_party/opus"      "$WORKDIR/src/third_party/opus/src"       "$OPUS_PIN"
[ "$fail" -eq 0 ] || { echo "::error::sync: one or more source pins do not match - refusing to build from an unverified tree" >&2; exit 1; }
echo "::endgroup::"

du -sh "$WORKDIR/src" 2>/dev/null || true
echo "sync: ok - src=$WEBRTC_PIN boringssl=$BORINGSSL_PIN opus=$OPUS_PIN depot_tools=$DEPOT_TOOLS_SHA"
