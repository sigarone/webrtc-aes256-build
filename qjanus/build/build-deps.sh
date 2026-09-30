#!/usr/bin/env bash
# Builds the crypto/transport stack Janus links against, all from source, all
# against the SAME BoringSSL (the one webrtc M150 pins), so that exactly one
# TLS/crypto implementation is present in the janus process:
#   BoringSSL (static, -fPIC)                                   -> $BSSL   (build time only)
#   libsrtp 2.x, OpenSSL crypto backend = BoringSSL (static)    -> $PREFIX (build time only)
#   libnice (GnuTLS, no OpenSSL)                                -> $PREFIX (shipped)
#   libwebsockets (no TLS library at all)                       -> $PREFIX (shipped)
# Ubuntu 24.04 x86_64, run as a user that may sudo (CI) or as root.
set -euxo pipefail

HERE=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=pins.env
. "$HERE/pins.env"

BSSL=${BSSL:-/opt/boringssl}
PREFIX=${PREFIX:-/opt/qjanus}
SRC=${SRC:-$HOME/qjanus-src}
NPROC=$(nproc)
SUDO=""; [ "$(id -u)" = 0 ] || SUDO=sudo
$SUDO mkdir -p "$BSSL" "$PREFIX/lib/pkgconfig"
$SUDO chown -R "$(id -u):$(id -g)" "$BSSL" "$PREFIX"
mkdir -p "$SRC"
VERS=$PREFIX/versions.txt; : > "$VERS"

# fetch_pinned <url> <sha> <dir>: shallow fetch of exactly one commit, refuses anything else
fetch_pinned() {
  local url=$1 sha=$2 dir=$3
  rm -rf "$dir" && mkdir -p "$dir"
  git -C "$dir" init -q .
  git -C "$dir" remote add origin "$url"
  git -C "$dir" fetch -q --depth 1 origin "$sha"
  git -C "$dir" checkout -q FETCH_HEAD
  test "$(git -C "$dir" rev-parse HEAD)" = "$sha"
}

# ---------------------------------------------------------------- BoringSSL
fetch_pinned https://github.com/google/boringssl.git "$BORINGSSL_SHA" "$SRC/boringssl"
echo "boringssl $BORINGSSL_SHA" >> "$VERS"
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

# ------------------------------------------------------------------ libsrtp
fetch_pinned https://github.com/cisco/libsrtp.git "$LIBSRTP_SHA" "$SRC/libsrtp"
echo "libsrtp $LIBSRTP_TAG $LIBSRTP_SHA" >> "$VERS"
cd "$SRC/libsrtp"
export PKG_CONFIG_PATH="$BSSL/lib/pkgconfig"
./configure --prefix="$PREFIX" --enable-openssl --with-openssl-dir="$BSSL" --disable-pcap 2>&1 | tail -30
make -j"$NPROC" libsrtp2.a
make install
ls -l "$PREFIX/lib"; cat "$PREFIX/lib/pkgconfig/libsrtp2.pc"

# ------------------------------------------------------------------ libnice
fetch_pinned https://github.com/libnice/libnice.git "$LIBNICE_SHA" "$SRC/libnice"
echo "libnice $LIBNICE_TAG $LIBNICE_SHA" >> "$VERS"
cd "$SRC/libnice"
meson setup build --prefix="$PREFIX" --libdir=lib --buildtype=release \
  -Dgupnp=disabled -Dgstreamer=disabled -Dintrospection=disabled -Dtests=disabled \
  -Dexamples=disabled -Dgtk_doc=disabled -Dcrypto-library=gnutls
ninja -C build install

# ---------------------------------------------------------- libwebsockets
fetch_pinned https://github.com/warmcat/libwebsockets.git "$LWS_SHA" "$SRC/libwebsockets"
echo "libwebsockets $LWS_TAG $LWS_SHA" >> "$VERS"
cd "$SRC/libwebsockets"
cmake -GNinja -B build -DCMAKE_INSTALL_PREFIX="$PREFIX" -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_LIBDIR=lib \
  -DLWS_WITH_SSL=OFF -DLWS_WITH_SHARED=ON -DLWS_WITH_STATIC=OFF \
  -DLWS_WITHOUT_TESTAPPS=ON -DLWS_WITHOUT_TEST_SERVER=ON -DLWS_WITHOUT_TEST_SERVER_EXTPOLL=ON \
  -DLWS_WITHOUT_TEST_PING=ON -DLWS_WITHOUT_TEST_CLIENT=ON -DLWS_WITH_MINIMAL_EXAMPLES=OFF \
  -DLWS_WITH_LIBUV=OFF -DLWS_WITH_LIBEVENT=OFF -DLWS_WITH_LIBEV=OFF -DLWS_WITH_GLIB=OFF \
  -DLWS_WITH_ZLIB=OFF -DLWS_WITH_HTTP2=OFF -DLWS_WITH_IPV6=ON
ninja -C build install
ls -l "$PREFIX/lib" | grep -i websockets

cat "$VERS"
