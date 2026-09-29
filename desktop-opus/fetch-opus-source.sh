#!/bin/sh
# fetch-opus-source.sh - fetch xiph/opus source AT A PINNED COMMIT from the
# official GitHub mirror (never copied from any private repo) and lay it out
# at <dest>/opus, matching what desktop-opus/binding.gyp expects
# ("opus/celt/...", "opus/silk/...", "opus/src/...", "opus/dnn/...",
# "opus/include/...").
#
# Uses codeload.github.com's tarball-by-ref endpoint rather than `git fetch
# <sha>` because GitHub does not guarantee `uploadpack.allowReachableSHA1InWant`
# for an arbitrary commit that is not a branch tip or tag (only codeload's
# archive endpoint is documented to work for any ref/sha on a public repo).
#
# usage: fetch-opus-source.sh <dest> [commit]
#   <dest>    directory to create <dest>/opus in (must not already contain one)
#   [commit]  full 40-hex commit sha (default: the pin in PINS.md, duplicated
#             below because this script has no shared-include mechanism with
#             a markdown file)
# exit: 0 ok | 1 download/verify failed | 2 usage/contract problem
set -eu

# Pinned commit — see ../PINS.md for the full justification (this is the
# EXACT commit the phone builds (m150/README.md pin table) already use, not
# just "a" tagged release, so desktop's OSCE behavior matches Android/iOS
# instead of merely also having OSCE from a possibly-different revision).
DEFAULT_OPUS_COMMIT="55513e81d8f606bd75d0ff773d2144e5f2a732f5"

usage() { echo "usage: $0 <dest> [commit]" >&2; exit 2; }
[ $# -ge 1 ] && [ $# -le 2 ] || usage
DEST=$1
OPUS_COMMIT=${2:-$DEFAULT_OPUS_COMMIT}

case "$OPUS_COMMIT" in
  [0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f][0-9a-f]) ;;
  *) echo "::error::fetch-opus-source: commit must be exactly 40 lowercase hex chars, got '$OPUS_COMMIT'" >&2; exit 2 ;;
esac

mkdir -p "$DEST"
DEST=$(CDPATH= cd -- "$DEST" && pwd)
[ -e "$DEST/opus" ] && { echo "::error::fetch-opus-source: $DEST/opus already exists - refusing to overwrite" >&2; exit 2; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
TARBALL="$WORK/opus-src.tar.gz"
URL="https://codeload.github.com/xiph/opus/tar.gz/${OPUS_COMMIT}"

echo "::group::fetch-opus-source: download ($URL)"
i=0
ok=0
while [ "$i" -lt 5 ]; do
  i=$((i + 1))
  if curl -fsSL --connect-timeout 15 --max-time 180 -o "$TARBALL" "$URL"; then
    ok=1
    break
  fi
  echo "download attempt $i failed, retrying..."
  sleep $((i * 3))
done
if [ "$ok" -ne 1 ]; then
  echo "::error::fetch-opus-source: could not download $URL after $i attempts" >&2
  exit 1
fi
ls -lh "$TARBALL"
echo "::endgroup::"

echo "::group::fetch-opus-source: extract + verify commit"
tar xzf "$TARBALL" -C "$WORK"
# codeload names the top-level dir <repo>-<short-or-full-ref>; find it rather
# than assume the exact spelling.
SRC_DIR=$(find "$WORK" -mindepth 1 -maxdepth 1 -type d -name 'opus-*')
[ -n "$SRC_DIR" ] || { echo "::error::fetch-opus-source: no opus-* directory in the tarball" >&2; exit 1; }

# codeload does not embed the resolved commit sha anywhere reachable without
# .git metadata (which the archive does not include) - the strongest local
# check available is that the requested exact sha resolved to SOME archive at
# all (a wrong/unreachable sha 404s before we get here) plus a content sanity
# check: the pinned OSCE files this whole build exists for must be present.
for f in dnn/osce.c dnn/osce_features.c dnn/nndsp.c dnn/osce.h autogen.sh; do
  [ -f "$SRC_DIR/$f" ] || { echo "::error::fetch-opus-source: expected file '$f' missing from the fetched tree - wrong commit or upstream layout changed?" >&2; exit 1; }
done

mv "$SRC_DIR" "$DEST/opus"
echo "fetch-opus-source: ok (commit $OPUS_COMMIT -> $DEST/opus)"
echo "::endgroup::"
