#!/bin/sh
# buildinfo.sh - emit BUILDINFO.json for a desktop-opus build. Bash/coreutils
# version of m150/buildinfo.py's idea (that one's Python; this addon's own
# tooling is shell + Node, so this stays consistent with fetch-opus-source.sh
# / fetch-opus-dnn-weights.sh rather than adding a Python dependency for one
# script).
#
# usage: buildinfo.sh <opus_commit> <weights_sha256> <node_file>
# writes JSON to stdout.
set -eu
[ $# -eq 3 ] || { echo "usage: $0 <opus_commit> <weights_sha256> <node_file>" >&2; exit 2; }
OPUS_COMMIT=$1
WEIGHTS_SHA256=$2
NODE_FILE=$3

if command -v sha256sum >/dev/null 2>&1; then
  NODE_SHA256=$(sha256sum "$NODE_FILE" | awk '{print $1}')
else
  NODE_SHA256=$(shasum -a 256 "$NODE_FILE" | awk '{print $1}')
fi
NODE_BYTES=$(wc -c < "$NODE_FILE" | tr -d ' ')

cat <<JSON
{
  "artifact": "$(basename "$NODE_FILE")",
  "artifact_sha256": "$NODE_SHA256",
  "artifact_bytes": $NODE_BYTES,
  "opus_source": "https://github.com/xiph/opus",
  "opus_commit": "$OPUS_COMMIT",
  "opus_dnn_weights_url": "https://media.xiph.org/opus/models/opus_data-${WEIGHTS_SHA256}.tar.gz",
  "opus_dnn_weights_sha256": "$WEIGHTS_SHA256",
  "features": ["ENABLE_DEEP_PLC", "ENABLE_OSCE", "DISABLE_DEBUG_FLOAT"],
  "node_version": "${NODE_VERSION:-unknown}",
  "electron_version": "${ELECTRON_VERSION:-unknown}",
  "runner_os": "${RUNNER_OS:-unknown}",
  "runner_arch": "${RUNNER_ARCH:-unknown}",
  "workflow_ref": "${GITHUB_SHA:-unknown}",
  "built_at_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
}
JSON
