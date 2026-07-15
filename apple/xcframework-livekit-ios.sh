#!/bin/bash
#
# iOS-only trim of webrtc-sdk/webrtc-build's build/apple/xcframework.sh
# (MIT license, https://github.com/webrtc-sdk/webrtc-build) — the SAME script
# livekit/webrtc-xcframework's own release binaries are built with (see that
# repo's README: "Built using: webrtc-sdk/webrtc + webrtc-sdk/webrtc-build").
# Upstream builds 8 platform slices (iOS/macOS/catalyst/tvOS/xrOS); Q-Audion's
# SwiftPM `LiveKitWebRTC` binaryTarget only ever consumes the iOS device+
# simulator slice (same shape as the existing plain WebRTC.xcframework this
# repo already publishes for the 1:1-call path — see build-ios.yml), so this
# trim keeps ONLY the iOS-arm64-device / iOS-arm64-simulator / iOS-x64-
# simulator platforms and drops the macOS/catalyst/tvOS/xrOS lipo+Versions/
# post-processing that has no corresponding slice here. Logic otherwise
# unchanged: gn gen --ide=xcode + ninja per-slice, lipo into universal
# device/simulator libs, xcodebuild -create-xcframework, zip.
#
# Usage: $0 'debug'|'release' <source_dir> <out_dir> ['prefix']
#   prefix="LiveKit" -> FRAMEWORK_NAME=LiveKitWebRTC (matches the LK-prefix
#   patch applied to sdk/objc/base/RTCMacros.h beforehand).

set -e

if [[ -z "$1" ]]; then
  echo "Usage: $0 'debug' | 'release' 'source_dir' 'out_dir' ['prefix']"
  exit 0
fi

MODE="$1"
SOURCE_DIR="$(realpath "$2")"
OUT_DIR="$(realpath "$3")"
PREFIX="${4:-""}"

if [ -z "$PREFIX" ]; then
  FRAMEWORK_NAME="WebRTC"
else
  FRAMEWORK_NAME="${PREFIX}WebRTC"
fi

DEBUG="false"
if [[ "$MODE" == "debug" ]]; then
  DEBUG="true"
fi

PARALLEL_BUILDS=6

echo "xcframework-livekit-ios.sh: MODE=$MODE, DEBUG=$DEBUG, SOURCE_DIR=$SOURCE_DIR, OUT_DIR=$OUT_DIR, PREFIX=$PREFIX, FRAMEWORK_NAME=$FRAMEWORK_NAME"

start_group() {
  if [[ "$CI" == "true" ]]; then
    echo "::group::$1"
  else
    echo "=== $1 ==="
  fi
}

end_group() {
  if [[ "$CI" == "true" ]]; then
    echo "::endgroup::"
  fi
}

COMMON_ARGS="
      enable_dsyms = $DEBUG
      enable_libaom = true
      enable_stripping = true
      ios_enable_code_signing = false
      is_component_build = false
      is_debug = $DEBUG
      rtc_build_examples = false
      rtc_enable_protobuf = false
      rtc_enable_symbol_export = true
      rtc_include_dav1d_in_internal_decoder_factory = true
      rtc_include_tests = false
      rtc_libvpx_build_vp9 = true
      rtc_use_h264 = true
      rtc_use_h265 = true
      treat_warnings_as_errors = false
      use_siso = false
      use_rtti = true"
# rtc_use_h264/h265=true (upstream trims h264 off; Q-Audion needs both, see
# build-ios.yml's H265 rationale). treat_warnings_as_errors relaxed to false
# and use_siso pinned off for the same GN/Siso iOS dep-scan reason build-ios.yml
# had to patch around in build_ios_libs.py (see "Force real ninja" step there)
# — this script calls `gn gen`/`ninja` directly so there is no equivalent
# Python wrapper to patch; setting the gn arg here is the direct equivalent.

PLATFORMS=(
  "iOS-arm64-device:target_os=\"ios\" target_environment=\"device\" target_cpu=\"arm64\" ios_deployment_target=\"13.0\""
  "iOS-arm64-simulator:target_os=\"ios\" target_environment=\"simulator\" target_cpu=\"arm64\" ios_deployment_target=\"13.0\""
  "iOS-x64-simulator:target_os=\"ios\" target_environment=\"simulator\" target_cpu=\"x64\" ios_deployment_target=\"13.0\""
)

cd "$SOURCE_DIR"

end_group

for platform_config in "${PLATFORMS[@]}"; do
  platform="${platform_config%%:*}"
  config="${platform_config#*:}"

  start_group "Building $platform"

  gn gen "$OUT_DIR/$platform" --args="$COMMON_ARGS $config" --ide=xcode

  build_target="ios_framework_bundle"

  ninja -C "$OUT_DIR/$platform" "$build_target" -j $PARALLEL_BUILDS --quiet || exit 1
  end_group
done

start_group "Creating universal binaries (device arm64 / simulator arm64+x64)"

mkdir -p "$OUT_DIR/iOS-device-lib"
cp -R "$OUT_DIR/iOS-arm64-device/$FRAMEWORK_NAME.framework" "$OUT_DIR/iOS-device-lib/$FRAMEWORK_NAME.framework"
lipo -create -output "$OUT_DIR/iOS-device-lib/$FRAMEWORK_NAME.framework/$FRAMEWORK_NAME" "$OUT_DIR/iOS-arm64-device/$FRAMEWORK_NAME.framework/$FRAMEWORK_NAME"
if [ -d "$OUT_DIR/iOS-arm64-device/$FRAMEWORK_NAME.dSYM" ]; then
  cp -R "$OUT_DIR/iOS-arm64-device/$FRAMEWORK_NAME.dSYM" "$OUT_DIR/iOS-device-lib/$FRAMEWORK_NAME.dSYM"
  lipo -create -output "$OUT_DIR/iOS-device-lib/$FRAMEWORK_NAME.dSYM/Contents/Resources/DWARF/$FRAMEWORK_NAME" "$OUT_DIR/iOS-arm64-device/$FRAMEWORK_NAME.dSYM/Contents/Resources/DWARF/$FRAMEWORK_NAME"
fi

mkdir -p "$OUT_DIR/iOS-simulator-lib"
cp -R "$OUT_DIR/iOS-arm64-simulator/$FRAMEWORK_NAME.framework" "$OUT_DIR/iOS-simulator-lib/$FRAMEWORK_NAME.framework"
lipo -create -output "$OUT_DIR/iOS-simulator-lib/$FRAMEWORK_NAME.framework/$FRAMEWORK_NAME" "$OUT_DIR/iOS-arm64-simulator/$FRAMEWORK_NAME.framework/$FRAMEWORK_NAME" "$OUT_DIR/iOS-x64-simulator/$FRAMEWORK_NAME.framework/$FRAMEWORK_NAME"
if [ -d "$OUT_DIR/iOS-arm64-simulator/$FRAMEWORK_NAME.dSYM" ]; then
  cp -R "$OUT_DIR/iOS-arm64-simulator/$FRAMEWORK_NAME.dSYM" "$OUT_DIR/iOS-simulator-lib/$FRAMEWORK_NAME.dSYM"
  lipo -create -output "$OUT_DIR/iOS-simulator-lib/$FRAMEWORK_NAME.dSYM/Contents/Resources/DWARF/$FRAMEWORK_NAME" "$OUT_DIR/iOS-arm64-simulator/$FRAMEWORK_NAME.dSYM/Contents/Resources/DWARF/$FRAMEWORK_NAME" "$OUT_DIR/iOS-x64-simulator/$FRAMEWORK_NAME.dSYM/Contents/Resources/DWARF/$FRAMEWORK_NAME"
fi

end_group

start_group "Creating XCFramework"

XCFRAMEWORK_ARGS=(-create-xcframework)

FRAMEWORK_PATHS=(
  "$OUT_DIR/iOS-device-lib/$FRAMEWORK_NAME.framework"
  "$OUT_DIR/iOS-simulator-lib/$FRAMEWORK_NAME.framework"
)

DSYM_PATHS=(
  "$OUT_DIR/iOS-device-lib/$FRAMEWORK_NAME.dSYM"
  "$OUT_DIR/iOS-simulator-lib/$FRAMEWORK_NAME.dSYM"
)

for i in "${!FRAMEWORK_PATHS[@]}"; do
  XCFRAMEWORK_ARGS+=(-framework "${FRAMEWORK_PATHS[$i]}")

  if [[ "$DEBUG" == "true" ]] && [[ -d "${DSYM_PATHS[$i]}" ]]; then
    XCFRAMEWORK_ARGS+=(-debug-symbols "${DSYM_PATHS[$i]}")
  fi
done

XCFRAMEWORK_ARGS+=(-output "$OUT_DIR/$FRAMEWORK_NAME.xcframework")

xcodebuild "${XCFRAMEWORK_ARGS[@]}"

end_group

start_group "Post-processing XCFramework"

# iOS-only slices are flat frameworks (no Versions/ symlink farm) — the
# upstream script's macOS/catalyst "mv into Versions/A + relink Current"
# step only applies to the bundle-style macOS/catalyst frameworks this trim
# does not build, so it is intentionally omitted here.
if [ -f "$SOURCE_DIR/LICENSE" ]; then
  cp "$SOURCE_DIR/LICENSE" "$OUT_DIR/$FRAMEWORK_NAME.xcframework/"
fi

cd "$OUT_DIR"
zip --symlinks -9 -r "$FRAMEWORK_NAME.xcframework.zip" "$FRAMEWORK_NAME.xcframework"

end_group

if [[ "$CI" == "true" ]]; then
  echo "framework_name=$FRAMEWORK_NAME" >> "$GITHUB_OUTPUT"
fi
