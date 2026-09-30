#!/usr/bin/env bash
# Builds the crypto/transport stack the load-test Janus links against, all from
# source and all against the SAME BoringSSL (the one the hardened WebRTC M150
# pins), so that exactly one TLS/crypto implementation is present in the process:
#
#   BoringSSL   (static, -fPIC)                             -> $BSSL   (build time only)
#   libsrtp 2.x (crypto backend = BoringSSL, static)        -> $PREFIX
#   libnice     (crypto backend = GnuTLS, never OpenSSL)    -> $PREFIX
#   libwebsockets (built WITHOUT any TLS library)           -> $PREFIX
#
# The Janus WebSocket server of the smoke test is plain ws on 127.0.0.1, so
# libwebsockets gets no TLS at all; this keeps a system OpenSSL out of the janus
# process. Every source is fetched by full commit sha and refused if it differs.
#
# Environment: BSSL (default /opt/boringssl), PREFIX (default /opt/qjanus-loadtest),
# SRC (scratch checkout dir, default $HOME/qjl-src), FORCE=1 (rebuild even when
# a component is already installed from the same pin).
# Ubuntu 24.04 x86_64, run as a user that may sudo (CI) or as root. Idempotent.
set -euxo pipefail

# ---- pins: changing one is a deliberate, reviewed change (they also key the CI cache)
BORINGSSL_SHA=f91f1447397c6719f9774dfb8e67329378e1f3d3
LIBSRTP_TAG=v2.8.1
LIBSRTP_SHA=6ff02afa8d2dc3f2e8896af391d137c1c66ff6fa
LIBNICE_TAG=0.1.24
LIBNICE_SHA=af62014d520ac09c8fe1974bda1b253114c68843
LWS_TAG=v4.3.10
LWS_SHA=2288cf200bc1c28680765bd4f07e437356106c2d

BSSL=${BSSL:-/opt/boringssl}
PREFIX=${PREFIX:-/opt/qjanus-loadtest}
SRC=${SRC:-$HOME/qjl-src}
FORCE=${FORCE:-0}
NPROC=$(nproc)
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
$SUDO mkdir -p "$BSSL" "$PREFIX/lib/pkgconfig"
$SUDO chown -R "$(id -u):$(id -g)" "$BSSL" "$PREFIX"
mkdir -p "$SRC"
VERS=$PREFIX/versions.txt
touch "$VERS"

# record NAME TEXT: one line per component in versions.txt (replaced on a rebuild)
record() {
  grep -v "^$1 " "$VERS" > "$VERS.tmp" || true
  echo "$1 $2" >> "$VERS.tmp"
  mv "$VERS.tmp" "$VERS"
}

# fetch_pinned URL SHA DIR: shallow fetch of exactly one commit, refuses anything else
fetch_pinned() {
  local url=$1 sha=$2 dir=$3
  rm -rf "$dir" && mkdir -p "$dir"
  git -C "$dir" init -q .
  git -C "$dir" remote add origin "$url"
  git -C "$dir" fetch -q --depth 1 origin "$sha"
  git -C "$dir" checkout -q FETCH_HEAD
  test "$(git -C "$dir" rev-parse HEAD)" = "$sha"
}

# done_already DIR NAME SHA: true when this pin is already installed (FORCE=1 disables the shortcut)
done_already() { [ "$FORCE" != 1 ] && [ -e "$1/.built-$2-$3" ]; }
mark_done() { rm -f "$1"/.built-"$2"-*; touch "$1/.built-$2-$3"; }

# ---------------------------------------------------------------- BoringSSL
if done_already "$BSSL" boringssl "$BORINGSSL_SHA"; then
  echo "BoringSSL $BORINGSSL_SHA already installed"
else
  fetch_pinned https://github.com/google/boringssl.git "$BORINGSSL_SHA" "$SRC/boringssl"
  cd "$SRC/boringssl"
  cmake -GNinja -B build -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DBUILD_SHARED_LIBS=OFF \
    -DCMAKE_C_COMPILER=clang -DCMAKE_CXX_COMPILER=clang++ \
    -DCMAKE_C_FLAGS=-Wno-error -DCMAKE_CXX_FLAGS=-Wno-error
  ninja -C build ssl crypto
  mkdir -p "$BSSL/lib/pkgconfig" "$BSSL/include"
  cp -r include/openssl "$BSSL/include/"
  cp "$(find build -name libssl.a | head -1)" "$(find build -name libcrypto.a | head -1)" "$BSSL/lib/"
  # pkg-config files so that libsrtp/janus pick BoringSSL (and never a system OpenSSL)
  cat > "$BSSL/lib/pkgconfig/libcrypto.pc" <<PC
prefix=$BSSL
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: BoringSSL-crypto
Description: BoringSSL libcrypto
Version: 1.1.1
Libs: -L\${libdir} -lcrypto -lpthread
Cflags: -I\${includedir}
PC
  cat > "$BSSL/lib/pkgconfig/libssl.pc" <<PC
prefix=$BSSL
libdir=\${prefix}/lib
includedir=\${prefix}/include
Name: BoringSSL-ssl
Description: BoringSSL libssl
Version: 1.1.1
Libs: -L\${libdir} -lssl -lcrypto -lstdc++ -lpthread
Cflags: -I\${includedir}
PC
  ls -l "$BSSL/lib"
  mark_done "$BSSL" boringssl "$BORINGSSL_SHA"
fi
record boringssl "$BORINGSSL_SHA"

# ------------------------------------------------------------------ libsrtp
if done_already "$PREFIX" libsrtp "$LIBSRTP_SHA"; then
  echo "libsrtp $LIBSRTP_SHA already installed"
else
  fetch_pinned https://github.com/cisco/libsrtp.git "$LIBSRTP_SHA" "$SRC/libsrtp"
  cd "$SRC/libsrtp"
  PKG_CONFIG_PATH="$BSSL/lib/pkgconfig" \
    ./configure --prefix="$PREFIX" --enable-openssl --with-openssl-dir="$BSSL" --disable-pcap 2>&1 | tail -30
  make -j"$NPROC" libsrtp2.a
  make install
  ls -l "$PREFIX/lib"; cat "$PREFIX/lib/pkgconfig/libsrtp2.pc"
  mark_done "$PREFIX" libsrtp "$LIBSRTP_SHA"
fi
record libsrtp "$LIBSRTP_TAG $LIBSRTP_SHA"

# ------------------------------------------------------------------ libnice
if done_already "$PREFIX" libnice "$LIBNICE_SHA"; then
  echo "libnice $LIBNICE_SHA already installed"
else
  fetch_pinned https://github.com/libnice/libnice.git "$LIBNICE_SHA" "$SRC/libnice"
  cd "$SRC/libnice"
  meson setup build --prefix="$PREFIX" --libdir=lib --buildtype=release \
    -Dgupnp=disabled -Dgstreamer=disabled -Dintrospection=disabled -Dtests=disabled \
    -Dexamples=disabled -Dgtk_doc=disabled -Dcrypto-library=gnutls
  ninja -C build install
  mark_done "$PREFIX" libnice "$LIBNICE_SHA"
fi
record libnice "$LIBNICE_TAG $LIBNICE_SHA"

# ---------------------------------------------------------- libwebsockets
# No TLS library at all (LWS_WITH_SSL=OFF): nothing to configure, nothing that
# could pull in a system OpenSSL. CMAKE_POLICY_VERSION_MINIMUM lets the old
# cmake_minimum_required(2.8.12) of this release configure with CMake >= 4.
if done_already "$PREFIX" libwebsockets "$LWS_SHA"; then
  echo "libwebsockets $LWS_SHA already installed"
else
  fetch_pinned https://github.com/warmcat/libwebsockets.git "$LWS_SHA" "$SRC/libwebsockets"
  cd "$SRC/libwebsockets"
  cmake -GNinja -B build -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_LIBDIR=lib -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
    -DLWS_WITH_SSL=OFF -DLWS_WITH_SHARED=ON -DLWS_WITH_STATIC=OFF \
    -DLWS_WITHOUT_TESTAPPS=ON -DLWS_WITHOUT_TEST_SERVER=ON -DLWS_WITHOUT_TEST_SERVER_EXTPOLL=ON \
    -DLWS_WITHOUT_TEST_PING=ON -DLWS_WITHOUT_TEST_CLIENT=ON -DLWS_WITH_MINIMAL_EXAMPLES=OFF \
    -DLWS_WITH_LIBUV=OFF -DLWS_WITH_LIBEVENT=OFF -DLWS_WITH_LIBEV=OFF -DLWS_WITH_GLIB=OFF \
    -DLWS_WITH_ZLIB=OFF -DLWS_WITH_HTTP2=OFF -DLWS_IPV6=ON -DDISABLE_WERROR=ON
  ninja -C build install
  mark_done "$PREFIX" libwebsockets "$LWS_SHA"
fi
record libwebsockets "$LWS_TAG $LWS_SHA"
find "$PREFIX/lib" -maxdepth 1 -name "*websockets*" -exec ls -l {} +
test -e "$PREFIX/lib/pkgconfig/libwebsockets.pc"

# libwebsockets must really be TLS-free: no TLS switch in its config header, no
# libssl/libcrypto/gnutls/mbedtls/wolfssl in its dependency list
if grep -Eq '^[[:space:]]*#[[:space:]]*define[[:space:]]+LWS_WITH_(TLS|SSL)\b' "$PREFIX/include/lws_config.h"; then
  echo "ERROR: libwebsockets was built with TLS support"; exit 1
fi
LWS_SO=$(find "$PREFIX/lib" -maxdepth 1 -name 'libwebsockets.so*' -type f | sed -n 1p)
test -n "$LWS_SO"
if readelf -d "$LWS_SO" | grep -E 'NEEDED.*(ssl|crypto|gnutls|mbedtls|wolfssl)'; then
  echo "ERROR: $LWS_SO depends on a TLS library"; exit 1
fi
if LD_LIBRARY_PATH="$PREFIX/lib" ldd "$LWS_SO" | grep -E 'libssl|libcrypto|gnutls|mbedtls|wolfssl'; then
  echo "ERROR: $LWS_SO resolves a TLS library"; exit 1
fi

cat "$VERS"
