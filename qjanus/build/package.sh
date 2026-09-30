#!/usr/bin/env bash
# package.sh: assembles the relocatable qjanus tarball from a finished build ($PREFIX).
#   env: PREFIX (default /opt/qjanus), OUT (default ./qjanus-out), BUILD_ID (default dev-<sha7>)
# Output ($OUT):
#   qjanus-<BUILD_ID>-ubuntu24.04-x86_64.tar.gz   top directory "qjanus/", relocatable
#   BUILDINFO.json, SHA256SUMS
# Everything that is not a distro library is bundled and found through $ORIGIN-relative RUNPATHs;
# the distro libraries the binaries need are listed in apt-deps.txt (inside the tarball).
set -euxo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
QJ=$(cd "$HERE/.." && pwd)
# shellcheck source=pins.env
. "$HERE/pins.env"

PREFIX=${PREFIX:-/opt/qjanus}
OUT=${OUT:-$PWD/qjanus-out}
SHORT=$(git -C "$QJ" rev-parse --short=7 HEAD 2> /dev/null || echo unknown)
BUILD_ID=${BUILD_ID:-dev-$SHORT}
[[ $BUILD_ID =~ ^[A-Za-z0-9._+-]+$ ]] || { echo "invalid BUILD_ID"; exit 1; }
STAGE=$OUT/stage/qjanus
TARBALL=qjanus-$BUILD_ID-ubuntu24.04-x86_64.tar.gz

rm -rf "$OUT/stage"
mkdir -p "$STAGE"/{bin,lib/janus/plugins,lib/janus/transports,lib/janus/events,lib/janus/loggers,libexec,share/qjanus/conf,share/qjanus/systemd,share/qjanus/test,licenses}

# ---- binaries and the libraries that are not the distro's
install -m 0755 "$PREFIX/bin/janus" "$STAGE/bin/janus"
install -m 0755 "$PREFIX"/lib/janus/plugins/libjanus_videoroom.so "$STAGE/lib/janus/plugins/"
install -m 0755 "$PREFIX"/lib/janus/transports/libjanus_http.so "$PREFIX"/lib/janus/transports/libjanus_websockets.so "$STAGE/lib/janus/transports/"
cp -a "$PREFIX"/lib/libnice.so* "$PREFIX"/lib/libwebsockets.so* "$STAGE/lib/"
for f in "$STAGE"/bin/janus "$STAGE"/lib/*.so.* "$STAGE"/lib/janus/*/*.so; do
  [ -L "$f" ] && continue
  strip --strip-unneeded "$f"
done

# relocatable: RUNPATHs relative to each object, never an absolute build path
patchelf --set-rpath '$ORIGIN/../lib' "$STAGE/bin/janus"
for f in "$STAGE"/lib/janus/plugins/*.so "$STAGE"/lib/janus/transports/*.so; do
  patchelf --set-rpath '$ORIGIN/../..' "$f"
done
for f in "$STAGE"/lib/*.so.*; do
  [ -L "$f" ] || patchelf --set-rpath '$ORIGIN' "$f"
done
if readelf -d "$STAGE/bin/janus" "$STAGE"/lib/janus/*/*.so "$STAGE"/lib/*.so.* | grep -E 'RPATH|RUNPATH' | grep -E "$PREFIX|/opt/|/home/"; then
  echo "ERROR: an absolute build path is left in a RUNPATH"; exit 1
fi

# ---- files that ship with it
install -m 0755 "$QJ/install.sh" "$STAGE/install.sh"
install -m 0755 "$QJ/libexec/qjanus-render-config" "$STAGE/libexec/qjanus-render-config"
install -m 0755 "$QJ/libexec/qjanus-wait-ready" "$STAGE/libexec/qjanus-wait-ready"
install -m 0644 "$QJ/libexec/qjanus-lib.sh" "$STAGE/libexec/qjanus-lib.sh"
install -m 0644 "$QJ"/conf/*.jcfg.tmpl "$STAGE/share/qjanus/conf/"
install -m 0644 "$QJ/systemd/qjanus.service" "$STAGE/share/qjanus/systemd/qjanus.service"
install -m 0644 "$QJ/conf/Caddyfile.example" "$STAGE/share/qjanus/Caddyfile.example"
install -m 0755 "$QJ/test/node-ice-check.py" "$STAGE/share/qjanus/test/node-ice-check.py"
install -m 0644 "$QJ/README.md" "$STAGE/README.md"
printf '%s\n' "$BUILD_ID" > "$STAGE/VERSION"
cp "${SRC:-$HOME/qjanus-src}/janus/COPYING" "$STAGE/licenses/janus-gateway-COPYING"
cat > "$STAGE/licenses/NOTICE.txt" <<EOF
qjanus bundles or statically links: Janus gateway ($JANUS_TAG, GPL-3.0-or-later with the OpenSSL
exception granted upstream), BoringSSL ($BORINGSSL_SHA), libsrtp ($LIBSRTP_TAG, BSD-3-Clause),
libnice ($LIBNICE_TAG, MPL-1.1/LGPL-2.1), libwebsockets ($LWS_TAG, MIT). The complete corresponding
source is the upstream tag/commit named in BUILDINFO.json plus qjanus/patches/*.patch and the build
scripts in qjanus/build of the repository commit named there.
EOF

# ---- runtime packages: the distro libraries the binaries name directly (apt pulls the rest)
elfs=$(find "$STAGE" -type f \( -path '*/bin/janus' -o -name '*.so' -o -name '*.so.*' \) | sort)
: > "$OUT/needed.txt"
for f in $elfs; do readelf -d "$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' >> "$OUT/needed.txt"; done
sort -u "$OUT/needed.txt" -o "$OUT/needed.txt"
: > "$OUT/ldd-all.txt"
for f in $elfs; do ldd "$f" >> "$OUT/ldd-all.txt" 2>&1 || true; done
: > "$OUT/apt-deps.raw"
while read -r so; do
  [ -n "$so" ] || continue
  path=$(awk -v so="$so" '$1 == so && $2 == "=>" {print $3; exit}' "$OUT/ldd-all.txt")
  [ -n "$path" ] || { echo "ERROR: cannot resolve $so"; exit 1; }
  case "$path" in "$STAGE"/*) continue ;; esac   # bundled
  pkg=$(dpkg -S "$(readlink -f "$path")" | sed -n '1p' | cut -d: -f1)
  [ -n "$pkg" ] || { echo "ERROR: no package owns $path"; exit 1; }
  echo "$pkg" >> "$OUT/apt-deps.raw"
done < "$OUT/needed.txt"
# openssl: the DTLS certificate is created with it; iproute2: install.sh and qjanus-render-config check the ICE interface with `ip`
{ cat "$OUT/apt-deps.raw"; echo openssl; echo iproute2; } | sort -u > "$STAGE/apt-deps.txt"
cat "$STAGE/apt-deps.txt"

# ---- BUILDINFO.json
patches='['
for p in "$QJ"/patches/*.patch; do
  patches="$patches{\"name\":\"$(basename "$p")\",\"sha256\":\"$(sha256sum "$p" | cut -d' ' -f1)\"},"
done
patches="${patches%,}]"
cat > "$OUT/BUILDINFO.json" <<EOF
{
  "name": "qjanus",
  "build_id": "$BUILD_ID",
  "repo": "${GITHUB_REPOSITORY:-unknown}",
  "repo_commit": "${GITHUB_SHA:-$(git -C "$QJ" rev-parse HEAD 2> /dev/null || echo unknown)}",
  "workflow_run": "${GITHUB_RUN_ID:-local}",
  "built_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "target": "ubuntu-24.04 x86_64 (glibc $(ldd --version | sed -n '1p' | awk '{print $NF}'))",
  "janus": {"tag": "$JANUS_TAG", "sha": "$JANUS_SHA"},
  "boringssl": {"sha": "$BORINGSSL_SHA"},
  "libsrtp": {"tag": "$LIBSRTP_TAG", "sha": "$LIBSRTP_SHA", "crypto_backend": "boringssl"},
  "libnice": {"tag": "$LIBNICE_TAG", "sha": "$LIBNICE_SHA", "crypto_backend": "gnutls"},
  "libwebsockets": {"tag": "$LWS_TAG", "sha": "$LWS_SHA", "tls": "none"},
  "modules": ["plugins/libjanus_videoroom.so", "transports/libjanus_http.so", "transports/libjanus_websockets.so"],
  "patches": $patches
}
EOF
python3 -c 'import json,sys; json.load(open(sys.argv[1]))' "$OUT/BUILDINFO.json"
install -m 0644 "$OUT/BUILDINFO.json" "$STAGE/BUILDINFO.json"
cat "$OUT/BUILDINFO.json"

# ---- checks on the staged tree, exactly as it will be shipped: no LD_LIBRARY_PATH
env -u LD_LIBRARY_PATH "$HERE/check-linkage.sh" "$STAGE/bin/janus" "$STAGE"/lib/janus/*/*.so "$STAGE"/lib/*.so.*
env -u LD_LIBRARY_PATH "$STAGE/bin/janus" --version > "$OUT/janus-version.txt"
cat "$OUT/janus-version.txt"
for n in bin/janus lib/janus/plugins/libjanus_videoroom.so lib/janus/transports/libjanus_http.so lib/janus/transports/libjanus_websockets.so; do
  test -x "$STAGE/$n" || { echo "missing $n"; exit 1; }
done
python3 -c 'import ast,sys; ast.parse(open(sys.argv[1]).read())' "$STAGE/share/qjanus/test/node-ice-check.py"
bash -n "$STAGE/install.sh" && bash -n "$STAGE/libexec/qjanus-render-config" && bash -n "$STAGE/libexec/qjanus-wait-ready" && bash -n "$STAGE/libexec/qjanus-lib.sh"

# ---- tarball: reproducible ordering/ownership, relocatable (top directory qjanus/)
export SOURCE_DATE_EPOCH=${SOURCE_DATE_EPOCH:-$(git -C "$QJ" log -1 --format=%ct 2> /dev/null || date +%s)}
( cd "$OUT/stage" && tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$SOURCE_DATE_EPOCH" -cf - qjanus | gzip -n -9 > "$OUT/$TARBALL" )
( cd "$OUT" && sha256sum "$TARBALL" BUILDINFO.json > SHA256SUMS && cat SHA256SUMS )
ls -l "$OUT"
