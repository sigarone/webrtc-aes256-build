#!/bin/sh
# fetch-opus-dnn-weights.sh - download the Opus DNN model weight tarball for
# the pinned Opus source (55513e81d8f606bd75d0ff773d2144e5f2a732f5), verify
# its sha256, and extract ONLY the deep-PLC / FARGAN / PitchDNN weight
# sources (+ LACE/NoLACE, kept for an eventual OSCE-BWE build - see the
# comment near ALLOWED_MEMBERS) into <opus_src>/dnn/.
#
# The weights are NOT vendored in this repo (they are large generated C
# arrays, tens of MB each - see BUILDINFO note below) and are NOT in the
# webrtc/Opus source tree either (upstream's own dnn/download_model.sh fetches
# them the same way, from the same host, at build/package time). This script
# is that same fetch, hardened for CI: fixed URL, fixed sha256 (pinned below,
# taken verbatim from the pinned Opus commit's autogen.sh, which itself names
# the tarball after its own sha256 - see the verification step), retried, and
# it extracts by an explicit allow-list instead of `tar xvomf` over the whole
# archive so nothing outside dnn/*_data.[ch] ever lands on disk.
#
# usage: fetch-opus-dnn-weights.sh <opus_src>
#   <opus_src>  path to the pinned third_party/opus/src checkout (the dir
#               that contains dnn/, autogen.sh, ...).
# exit: 0 ok | 1 download/checksum failed | 2 usage/contract problem
set -eu

usage() { echo "usage: $0 <opus_src>" >&2; exit 2; }
[ $# -eq 1 ] || usage
OPUS_SRC=$1
[ -d "$OPUS_SRC" ] || { echo "::error::fetch-opus-dnn-weights: opus_src '$OPUS_SRC' does not exist" >&2; exit 2; }
OPUS_SRC=$(CDPATH= cd -- "$OPUS_SRC" && pwd)
[ -d "$OPUS_SRC/dnn" ] || { echo "::error::fetch-opus-dnn-weights: $OPUS_SRC/dnn missing - is this really the Opus checkout?" >&2; exit 2; }

# Pinned to Opus commit 55513e81d8f606bd75d0ff773d2144e5f2a732f5 (webrtc-plan.md
# v2 D6 / m150 §2.1). The hash below is BOTH the tarball's own sha256 AND the
# literal string dnn/download_model.sh at that commit passes to itself as the
# expected checksum (upstream names the file after its own hash, see the
# cross-check below) - resolved 2026-09-28 from
# https://raw.githubusercontent.com/xiph/opus/55513e81d8f606bd75d0ff773d2144e5f2a732f5/autogen.sh
# ("dnn/download_model.sh 160753e98319...") and verified live by downloading
# the tarball once into E:/DEVKIT/workspace/_scratch/opus-weights-m150/ and
# running sha256sum on it (exact match).
MODEL_SHA256=160753e983198f29f1aae67c54caa0e30bd90f1ce916a52f15bdad2df8e35e58
MODEL_URL="https://media.xiph.org/opus/models/opus_data-${MODEL_SHA256}.tar.gz"

# dnn/*_data.[ch] to extract. PLC + FARGAN + PitchDNN are load-bearing for P4
# (deep PLC, decoder complexity >=5, §1.2/§2.1 of the plan). LACE/NoLACE are
# OSCE weights: OSCE itself is compiled OFF in this build (decision D7, plan
# §7.3) so they are inert, but they are cheap enough to keep on hand for a
# possible OSCE-BWE follow-up without a second weights fetch/re-pin - if that
# ever ships, ENABLE_OSCE must be turned on at GN/BUILD.gn level too (source-
# patch author's call, not this script's). DRED (dred_rdovae_*, lossgen_data)
# is intentionally NOT extracted: DRED is off (same D7) and those files alone
# are the bulk of the tarball's weight - skipping them keeps the checked-out
# tree small.
ALLOWED_MEMBERS="dnn/plc_data.c dnn/plc_data.h dnn/fargan_data.c dnn/fargan_data.h dnn/pitchdnn_data.c dnn/pitchdnn_data.h dnn/lace_data.c dnn/lace_data.h dnn/nolace_data.c dnn/nolace_data.h"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
TARBALL="$WORK/opus_data.tar.gz"

echo "::group::fetch-opus-dnn-weights: download"
echo "url: $MODEL_URL"
i=0
ok=0
while [ "$i" -lt 5 ]; do
  i=$((i + 1))
  if curl -fsSL --connect-timeout 15 --max-time 300 -o "$TARBALL" "$MODEL_URL"; then
    ok=1
    break
  fi
  echo "download attempt $i failed, retrying..."
  sleep $((i * 3))
done
if [ "$ok" -ne 1 ]; then
  echo "::error::fetch-opus-dnn-weights: could not download $MODEL_URL after $i attempts" >&2
  exit 1
fi
ls -lh "$TARBALL"
echo "::endgroup::"

echo "::group::fetch-opus-dnn-weights: sha256 verification (fail closed)"
if command -v sha256sum >/dev/null 2>&1; then
  ACTUAL=$(sha256sum "$TARBALL" | awk '{print $1}')
elif command -v shasum >/dev/null 2>&1; then
  ACTUAL=$(shasum -a 256 "$TARBALL" | awk '{print $1}')
else
  echo "::error::fetch-opus-dnn-weights: no sha256sum/shasum on this runner - cannot verify, refusing to extract" >&2
  exit 1
fi
echo "expected: $MODEL_SHA256"
echo "actual:   $ACTUAL"
if [ "$ACTUAL" != "$MODEL_SHA256" ]; then
  echo "::error::fetch-opus-dnn-weights: sha256 MISMATCH - refusing to extract a corrupted/tampered tarball" >&2
  exit 1
fi
echo "::endgroup::"

echo "::group::fetch-opus-dnn-weights: extract (allow-list only)"
for m in $ALLOWED_MEMBERS; do
  tar tzf "$TARBALL" "$m" >/dev/null 2>&1 || { echo "::error::fetch-opus-dnn-weights: expected member '$m' not in tarball - upstream layout changed?" >&2; exit 1; }
done
# shellcheck disable=SC2086
tar xzf "$TARBALL" -C "$OPUS_SRC" $ALLOWED_MEMBERS
for m in $ALLOWED_MEMBERS; do
  f="$OPUS_SRC/$m"
  [ -s "$f" ] || { echo "::error::fetch-opus-dnn-weights: $f missing or empty after extraction" >&2; exit 1; }
done
echo "extracted into $OPUS_SRC/dnn:"
# shellcheck disable=SC2086
ls -lh $(for m in $ALLOWED_MEMBERS; do printf '%s\n' "$OPUS_SRC/$m"; done)
echo "::endgroup::"

echo "fetch-opus-dnn-weights: ok (sha256 $MODEL_SHA256, $(printf '%s\n' $ALLOWED_MEMBERS | wc -l | tr -d ' ') files)"
