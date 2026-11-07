#!/bin/sh
# fetch-opus-dnn-weights.sh - download the Opus DNN model weight tarball for
# the pinned Opus source (55513e81d8f606bd75d0ff773d2144e5f2a732f5), verify
# its sha256, and extract ONLY the deep-PLC / FARGAN / PitchDNN weight
# sources plus the OSCE LACE/NoLACE weights (see the comment near
# ALLOWED_MEMBERS) into <opus_src>/dnn/.
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
# the tarball once into a local scratch directory and
# running sha256sum on it (exact match).
MODEL_SHA256=160753e983198f29f1aae67c54caa0e30bd90f1ce916a52f15bdad2df8e35e58
MODEL_URL="https://media.xiph.org/opus/models/opus_data-${MODEL_SHA256}.tar.gz"

# dnn/*_data.[ch] to extract. PLC + FARGAN + PitchDNN are load-bearing for
# deep PLC (decoder complexity >= 5); LACE/NoLACE are the OSCE weights that
# P4a compiles in on arm64 (ENABLE_OSCE; complexity 6 = LACE, 7 = NoLACE,
# kQaudionDefaultDecoderComplexity = 7). There is no OSCE-BWE model at this
# Opus revision. DRED (dred_rdovae_*, lossgen_data) is intentionally NOT
# extracted: DRED is off and those files are the bulk of the tarball.
# Every one of these files is compiled into the library (no USE_WEIGHTS_FILE),
# so nothing is ever fetched at runtime.
# The three dred_rdovae_*.h HEADERS are extracted too (security review,
# stage 3): with ENABLE_DEEP_PLC, src/opus_decoder.c includes
# dred_rdovae_dec_data.h / dnn/dred_rdovae_dec.h (-> dred_rdovae_stats_data.h)
# and dnn/nnet.c includes dred_rdovae_constants.h unconditionally, so the
# arm64 build fails without them (verified with a real clang arm64 compile of
# third_party/opus:opus on the pinned tree). They are declarations/constants
# only; no DRED .c (and no DRED weight data) is extracted or compiled.
ALLOWED_MEMBERS="dnn/plc_data.c dnn/plc_data.h dnn/fargan_data.c dnn/fargan_data.h dnn/pitchdnn_data.c dnn/pitchdnn_data.h dnn/lace_data.c dnn/lace_data.h dnn/nolace_data.c dnn/nolace_data.h dnn/dred_rdovae_constants.h dnn/dred_rdovae_dec_data.h dnn/dred_rdovae_stats_data.h"

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT
TARBALL="$WORK/opus_data.tar.gz"

echo "::group::fetch-opus-dnn-weights: download"
# Primary source: upstream (xiph). media.xiph.org is intermittently
# unreachable from GitHub runners, so after all attempts fail we fall back to
# a byte-identical mirror kept as a release asset of THIS repo. The mirror
# tag and asset name are derived from the pinned MODEL_SHA256, so a changed
# pin can never silently pick up a mirror of different bytes (it would simply
# not exist). Whatever source is used, the sha256 check below is the only
# gate and fails closed.
MIRROR_TAG="opus-dnn-weights-$(printf '%s' "$MODEL_SHA256" | cut -c1-8)"
MIRROR_ASSET="opus_data-${MODEL_SHA256}.tar.gz"
SOURCE=""
echo "url: $MODEL_URL"
i=0
while [ "$i" -lt 5 ]; do
  i=$((i + 1))
  if curl -fsSL --connect-timeout 30 --max-time 300 -o "$TARBALL" "$MODEL_URL"; then
    SOURCE="xiph ($MODEL_URL)"
    break
  fi
  rm -f "$TARBALL"
  echo "download attempt $i from xiph failed, retrying..."
  sleep $((i * 3))
done
if [ -z "$SOURCE" ]; then
  echo "::warning::fetch-opus-dnn-weights: xiph unreachable after $i attempts, falling back to mirror release $MIRROR_TAG"
  if [ -z "${GITHUB_REPOSITORY:-}" ] || [ -z "${GH_TOKEN:-${GITHUB_TOKEN:-}}" ] || ! command -v gh >/dev/null 2>&1; then
    echo "::error::fetch-opus-dnn-weights: mirror fallback unavailable (needs gh, GITHUB_REPOSITORY and GH_TOKEN)" >&2
    exit 1
  fi
  MIRROR_DIR="$WORK/mirror"
  mkdir -p "$MIRROR_DIR"
  j=0
  while [ "$j" -lt 3 ]; do
    j=$((j + 1))
    if GH_TOKEN="${GH_TOKEN:-$GITHUB_TOKEN}" gh release download "$MIRROR_TAG" --repo "$GITHUB_REPOSITORY" --pattern "$MIRROR_ASSET" --dir "$MIRROR_DIR" --clobber && [ -s "$MIRROR_DIR/$MIRROR_ASSET" ]; then
      mv "$MIRROR_DIR/$MIRROR_ASSET" "$TARBALL"
      SOURCE="mirror (release $MIRROR_TAG of $GITHUB_REPOSITORY)"
      break
    fi
    echo "mirror download attempt $j failed, retrying..."
    sleep $((j * 5))
  done
fi
if [ -z "$SOURCE" ]; then
  echo "::error::fetch-opus-dnn-weights: could not download the weights from xiph or from the mirror" >&2
  exit 1
fi
echo "fetch-opus-dnn-weights: source used: $SOURCE"
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

echo "fetch-opus-dnn-weights: ok (source: $SOURCE, sha256 $MODEL_SHA256, $(printf '%s\n' $ALLOWED_MEMBERS | wc -l | tr -d ' ') files)"
