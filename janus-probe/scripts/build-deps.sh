#!/usr/bin/env bash
# Builds the crypto/transport stack Janus links against, all from source, all
# against the SAME BoringSSL (the one webrtc M150 pins), so that exactly one
# TLS/crypto implementation is present in the janus process:
#   BoringSSL f91f1447 (static, -fPIC)  ->  $BSSL
#   libsrtp 2.x, OpenSSL crypto backend = BoringSSL (AES-GCM)  ->  $PREFIX
#   libnice (GnuTLS, no OpenSSL)  ->  $PREFIX
set -euxo pipefail

BSSL_SHA=f91f1447397c6719f9774dfb8e67329378e1f3d3
LIBSRTP_TAG=${LIBSRTP_TAG:-v2.8.1}
LIBNICE_TAG=${LIBNICE_TAG:-0.1.24}
BSSL=${BSSL:-/opt/boringssl}
PREFIX=${PREFIX:-/opt/janus-probe}
SRC=${SRC:-$HOME/jp-src}
NPROC=$(nproc)
mkdir -p "$SRC" "$PREFIX/lib/pkgconfig"
sudo mkdir -p "$BSSL" "$PREFIX" && sudo chown -R "$USER" "$BSSL" "$PREFIX"
VERS=$PREFIX/versions.txt; : > "$VERS"

# ---------------------------------------------------------------- BoringSSL
cd "$SRC"
rm -rf boringssl && mkdir boringssl && cd boringssl
git init -q . && git remote add origin https://github.com/google/boringssl.git
git fetch -q --depth 1 origin "$BSSL_SHA" && git checkout -q FETCH_HEAD
test "$(git rev-parse HEAD)" = "$BSSL_SHA"
echo "boringssl $(git rev-parse HEAD)" >> "$VERS"
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
cd "$SRC"
rm -rf libsrtp && mkdir libsrtp && cd libsrtp
git init -q . && git remote add origin https://github.com/cisco/libsrtp.git
git fetch -q --depth 1 origin "$LIBSRTP_TAG" && git checkout -q FETCH_HEAD
echo "libsrtp $LIBSRTP_TAG $(git rev-parse HEAD)" >> "$VERS"
export PKG_CONFIG_PATH="$BSSL/lib/pkgconfig"
./configure --prefix="$PREFIX" --enable-openssl --with-openssl-dir="$BSSL" --disable-pcap 2>&1 | tail -30
make -j"$NPROC" libsrtp2.a
make install
ls -l "$PREFIX/lib" | head; cat "$PREFIX/lib/pkgconfig/libsrtp2.pc"

# ------------------------------------------------------------------ libnice
cd "$SRC"
rm -rf libnice && git clone -q --depth 1 --branch "$LIBNICE_TAG" https://github.com/libnice/libnice.git
cd libnice
echo "libnice $LIBNICE_TAG $(git rev-parse HEAD)" >> "$VERS"

meson setup build --prefix="$PREFIX" --libdir=lib -Dgupnp=disabled -Dgstreamer=disabled \
  -Dintrospection=disabled -Dtests=disabled -Dexamples=disabled -Dgtk_doc=disabled \
  -Dcrypto-library=gnutls
ninja -C build install
cat "$VERS"
