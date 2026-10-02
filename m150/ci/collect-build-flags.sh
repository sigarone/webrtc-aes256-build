#!/bin/bash
# collect-build-flags.sh - dump the real build facts and turn them into
# build-flags.json (S0.1). Windows only (Git Bash on windows-2022, depot_tools
# on PATH). Runs AFTER `gn gen <out>` and after the library was built in the
# same output directory.
#
# usage: collect-build-flags.sh <webrtc_src> <out_dir_rel> <raw_dir> <gn_args> <result.json>
#   <webrtc_src>   the patched checkout (e.g. C:/w/src)
#   <out_dir_rel>  GN output dir relative to <webrtc_src> (e.g. out/win-x64)
#   <raw_dir>      scratch dir for the raw dumps (created)
#   <gn_args>      the exact GN args string of the build
#   <result.json>  where build-flags.json is written
# exit: 0 ok | 1 a required fact is missing | 2 usage
set -euo pipefail

[ $# -eq 5 ] || { echo "usage: $0 <webrtc_src> <out_dir_rel> <raw_dir> <gn_args> <result.json>" >&2; exit 2; }
SRC=$1
OUT=$2
RAW=$3
GN_ARGS=$4
RESULT=$5

SELF_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$RAW"
RAW=$(CDPATH= cd -- "$RAW" && pwd)
case "$RESULT" in /*|?:*) ;; *) RESULT="$(pwd)/$RESULT" ;; esac

cd "$SRC"
# The resolved target description (defines/libs/ldflags of //:webrtc, public
# configs included) and the real compiler command lines of every C++ TU.
cmd //c "gn.bat desc $OUT //:webrtc --format=json" > "$RAW/desc-webrtc.json"
# depot_tools' ninja wrapper refuses an output dir that already holds Siso
# state (i.e. after autoninja built it), so talk to the ninja binary of the
# checkout directly; if it is not there, let GN export the compile commands.
NINJA_EXE=third_party/ninja/ninja.exe
if [ -f "$NINJA_EXE" ]; then
  "$NINJA_EXE" -C "$OUT" -t compdb cxx > "$RAW/compdb.json"
else
  cmd //c "gn.bat gen $OUT --export-compile-commands=pc:peer_connection_factory"
  cp "$OUT/compile_commands.json" "$RAW/compdb.json"
fi
CLANG_DIR=third_party/llvm-build/Release+Asserts
"$CLANG_DIR/bin/clang-cl.exe" --version > "$RAW/clang-version.txt"
cp "$CLANG_DIR/cr_build_revision" "$RAW/cr-build-revision"
REV=$(tr -d '\r\n' < "$RAW/cr-build-revision")
# Pin the exact compiler package a consumer must use (same URL gclient's
# tools/clang/scripts/update.py fetched it from).
case "$REV" in ''|*[!A-Za-z0-9._-]*) echo "::error::collect-build-flags: odd compiler revision" >&2; exit 1 ;; esac
if curl --proto '=https' --tlsv1.2 -fsSL "https://commondatastorage.googleapis.com/chromium-browser-clang/Win/clang-$REV.tar.xz" -o "$RAW/clang.tar.xz"; then
  sum=$(sha256sum "$RAW/clang.tar.xz")
  printf '%s\n' "${sum%% *}" > "$RAW/clang-sha256"
  rm -f "$RAW/clang.tar.xz"
else
  echo "::error::collect-build-flags: cannot download the clang package $REV to pin its sha256" >&2
  exit 1
fi

py=python
command -v python3 >/dev/null 2>&1 && py=python3
"$py" "$SELF_DIR/gen-build-flags.py" "$RAW" "$GN_ARGS" > "$RESULT"
echo "collect-build-flags: wrote $RESULT"
"$py" -c "import json,sys; d=json.load(open(sys.argv[1])); print(json.dumps({k: d[k] for k in ('target','toolchain','abi')}, indent=1)); print('defines:', len(d['compile']['defines']), 'include dirs:', len(d['compile']['include_dirs']), 'system libs:', len(d['link']['system_libs']))" "$RESULT"
