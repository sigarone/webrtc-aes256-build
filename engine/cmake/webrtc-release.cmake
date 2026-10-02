# The libwebrtc release the engine links, pinned here. Every file is verified against these
# sha256 values before it is used; the SHA256SUMS file of the release is verified too and must
# list exactly the same digests. Changing the tag means changing this file in the same commit.
#
# Release (public repository): webrtc-windows-m150-a256-dplc-10, from main commit 137212c.
# Patches P1-P12 and P9 (strict transport) are in it; the library is built with Chromium's
# clang-cl, Chromium libc++ (std::__Cr), /MT, C++20, RTTI on, exceptions off.

set(QMEDIA_WEBRTC_REPO "sigarone/webrtc-aes256-build")
set(QMEDIA_WEBRTC_TAG "webrtc-windows-m150-a256-dplc-10")

set(QMEDIA_WEBRTC_FILES
  webrtc.lib libcxx.lib webrtc-headers.zip build-flags.json BUILDINFO.json PINS.txt SHA256SUMS)

set(QMEDIA_SHA256_webrtc.lib          "d44a002639bf0c563afcae44f0d8bd251d74b0bb4e4557529b2e115e6cab1ae7")
set(QMEDIA_SHA256_libcxx.lib          "1ad7cddb6e3a8f89e14bacd9479fc8e654b23130e814c57767b48826ea6295e2")
set(QMEDIA_SHA256_webrtc-headers.zip  "0cd209176c6a8c0de3ac51127ba69ec88445d26333334cd46c90fe4b10b70860")
set(QMEDIA_SHA256_build-flags.json    "b089f824f01ab806551d8989d9a7231a6a1ba9da8c774dd1178c4f88eb23b7ce")
set(QMEDIA_SHA256_BUILDINFO.json      "a18e90266fcf98fb2a3d0640e20e25df0df7cfb435c6488c97ba86eab2b280bf")
set(QMEDIA_SHA256_PINS.txt            "3d913bb763b04a1d28e3e39c1811866a924bda5d96eea2928d6cbb72cb3eb93a")
set(QMEDIA_SHA256_SHA256SUMS          "5c429e6943b5967e9d20e8d31b913e0f88f8ee451e2205775fefa5836b45bcd5")
