#!/bin/sh
# apply-series.sh - apply the M150 patch series to a webrtc-sdk/webrtc
# checkout.
#
# Ownership split (do not violate, see the plan's "SHARED CONTRACT"):
#   - m150/series and m150/patches/* belong to the SOURCE-PATCH author.
#     This script only READS them.
#   - this script itself belongs to the PIPELINE author.
#
# m150/series format, one line per patch:
#   <dir relative to <src>> <patch file name>   [# comment]
#   '.'            -> apply inside <src> itself (the webrtc src repo)
#   'third_party'  -> apply inside <src>/third_party (the split DEPS repo)
#   'third_party/opus/src' or 'third_party/boringssl/src' -> those repos,
#     if a patch ever needs to touch vendored Opus/BoringSSL directly.
#   'build'        -> apply inside <src>/build (the separate depot_tools
#     'build' repo, DEPS 'src/build' - e.g. P10, which pins javac's
#     --release level for the Android AAR's classes.jar).
# Blank lines and lines starting with '#' are ignored. Patch files live in
# m150/patches/, resolved relative to this script's own directory (not cwd),
# so apply-series.sh can be invoked from anywhere.
#
# The series applies to every variant unconditionally - there is no
# per-variant column in the series file (the LiveKit-prefixed -lk variants and
# their prefixing patch are gone).
#
# Fails CLOSED: a sequential dry run (every patch, in series order, applied to a
# throw-away copy of the index with `git apply --cached`) runs first - if ANY
# patch fails there, we stop before applying anything so the tree is never left
# half-patched. A missing/empty series file, an unsafe dir/filename, or a
# dry-run failure all exit non-zero.
#
# usage: apply-series.sh <src_dir> <android|ios|win>
# ("win" = Windows x64 desktop: the plain series, no LiveKit patch.)
# exit: 0 clean apply | 1 patch/context problem | 2 usage/contract problem
set -eu

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
SERIES="$SELF_DIR/series"
PATCH_DIR="$SELF_DIR/patches"

usage() {
  echo "usage: $0 <src_dir> <android|ios|win>" >&2
  exit 2
}

[ $# -eq 2 ] || usage
SRC=$1
VARIANT=$2

case "$VARIANT" in
  android|ios|win) ;;
  *) echo "::error::apply-series: unknown variant '$VARIANT' (want android|ios|win)" >&2; exit 2 ;;
esac

[ -d "$SRC" ] || { echo "::error::apply-series: src_dir '$SRC' does not exist" >&2; exit 2; }
SRC=$(CDPATH= cd -- "$SRC" && pwd)

if [ ! -f "$SERIES" ]; then
  echo "::error::apply-series: $SERIES not found." >&2
  echo "  This file is owned by the SOURCE-PATCH author (P1-P8, one line" >&2
  echo "  '<dir> <patch>' per patch, in apply order). Nothing to apply until" >&2
  echo "  it exists - refusing to guess, failing closed." >&2
  exit 2
fi

# Resolve one series line's <dir> token to an absolute apply-root, rejecting
# anything that is not exactly one of the contract's known values (no '..',
# no absolute paths, no surprises).
resolve_dir() {
  case "$1" in
    .) printf '%s\n' "$SRC" ;;
    third_party) printf '%s\n' "$SRC/third_party" ;;
    third_party/opus/src) printf '%s\n' "$SRC/third_party/opus/src" ;;
    third_party/boringssl/src) printf '%s\n' "$SRC/third_party/boringssl/src" ;;
    build) printf '%s\n' "$SRC/build" ;;
    *) return 1 ;;
  esac
}

# safe_name: patch file name may not contain '/', '..' or start with '-'
# (git apply flag injection) or whitespace.
safe_name() {
  case "$1" in
    ''|*/*|*..*|-*|*[[:space:]]*) return 1 ;;
    *) return 0 ;;
  esac
}

N=0
PLAN_TMP=$(mktemp)
trap 'rm -f "$PLAN_TMP"' EXIT

# --- pass 1: parse + validate the whole series, build the apply plan -------
while IFS= read -r line || [ -n "$line" ]; do
  # strip a trailing '#'-comment and surrounding whitespace
  line=${line%%#*}
  # trim leading/trailing blanks (POSIX-safe, no bashisms)
  line=$(printf '%s' "$line" | sed -e 's/^[[:space:]]*//' -e 's/[[:space:]]*$//')
  [ -n "$line" ] || continue
  set -- $line
  if [ $# -ne 2 ]; then
    echo "::error::apply-series: malformed series line (want '<dir> <patch>'): $line" >&2
    exit 2
  fi
  d=$1
  p=$2
  if ! ROOT=$(resolve_dir "$d"); then
    echo "::error::apply-series: series line has an unknown dir '$d' (want '.', 'third_party', 'third_party/opus/src', 'third_party/boringssl/src' or 'build')" >&2
    exit 2
  fi
  if ! safe_name "$p"; then
    echo "::error::apply-series: unsafe patch file name '$p'" >&2
    exit 2
  fi
  PF="$PATCH_DIR/$p"
  [ -f "$PF" ] || { echo "::error::apply-series: patch file not found: $PF" >&2; exit 2; }
  [ -d "$ROOT" ] || { echo "::error::apply-series: apply root does not exist (gclient sync incomplete?): $ROOT" >&2; exit 2; }
  printf '%s\t%s\t%s\n' "$ROOT" "$PF" "$p" >> "$PLAN_TMP"
  N=$((N + 1))
done < "$SERIES"

if [ "$N" -eq 0 ]; then
  echo "::error::apply-series: $SERIES has zero patch lines - nothing to apply" >&2
  exit 2
fi

echo "apply-series: $(wc -l < "$PLAN_TMP" | tr -d ' ') patch(es) planned for variant=$VARIANT"

# --- pass 2: dry run the WHOLE series first (fail closed, nothing applied) --
# A plain `git apply --check` of every patch against the untouched tree cannot
# work once two patches change the same file with overlapping context (P12 sits
# on top of P1/P2/P3 in api/crypto/frame_crypto_transformer.{h,cc}). So the
# series is applied IN ORDER to a throw-away COPY of each root's index
# (`git apply --cached`); the working tree is not touched.
echo "::group::apply-series: sequential dry run (git apply --cached on a throw-away index copy)"
ROOTS_TMP=$(mktemp)
cut -f1 "$PLAN_TMP" | awk '!seen[$0]++' > "$ROOTS_TMP"
while IFS= read -r root; do
  gitdir=$(git -C "$root" rev-parse --absolute-git-dir) || { echo "::error::apply-series: $root is not a git checkout" >&2; exit 1; }
  [ -f "$gitdir/index" ] || { echo "::error::apply-series: no git index at $gitdir/index" >&2; exit 1; }
  IDX=$(mktemp)
  cp "$gitdir/index" "$IDX"
  while IFS="$(printf '\t')" read -r r pf name; do
    [ "$r" = "$root" ] || continue
    echo "-- dry run: $name  (root=$root)"
    if ! GIT_INDEX_FILE="$IDX" git -C "$root" apply --cached -v "$pf" < /dev/null; then
      rm -f "$IDX" "$ROOTS_TMP"
      echo "::error::apply-series: dry run failed for $name against $root (after the patches before it) - stopping before applying anything" >&2
      exit 1
    fi
  done < "$PLAN_TMP"
  rm -f "$IDX"
done < "$ROOTS_TMP"
rm -f "$ROOTS_TMP"
echo "::endgroup::"

# --- pass 3: real apply, in order ------------------------------------------
echo "::group::apply-series: git apply (real)"
i=0
while IFS="$(printf '\t')" read -r root pf name; do
  i=$((i + 1))
  echo "-- [$i] applying: $name  (root=$root)"
  git -C "$root" apply -v "$pf"
done < "$PLAN_TMP"
echo "::endgroup::"

echo "apply-series: $i patch(es) applied cleanly for variant=$VARIANT"
