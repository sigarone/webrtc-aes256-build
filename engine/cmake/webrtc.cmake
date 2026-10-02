# Hook for linking the published libwebrtc into the engine.
#
# Planned inputs (all from one release of the Windows library, tag suffix a256-dplc-9 or later):
#   QMEDIA_WEBRTC_DIR          directory with the unpacked release: webrtc.lib and the headers
#   QMEDIA_WEBRTC_BUILD_FLAGS  path to build-flags.json from the same release. It records the
#                              compile definitions, CRT mode, STL and RTTI settings of the
#                              library, and this module must apply exactly those to the engine
#                              target (a mismatch breaks the link or the runtime).
#
# NOT WIRED YET: the build-flags.json file is produced by the library workflow (stream A of
# lot 1) and its schema was not available when this skeleton was written. Until then the engine
# links only the IPC library and QMEDIA_WITH_WEBRTC=ON stops the configure step on purpose.
#
# When wiring this up:
#   1. Read the JSON with string(JSON ...) and turn definitions, CRT (/MT or /MD), iterator debug
#      level and RTTI into target_compile_definitions / target_compile_options.
#   2. target_include_directories(<t> SYSTEM PRIVATE ${QMEDIA_WEBRTC_DIR}/include ...).
#   3. target_link_libraries(<t> PRIVATE ${QMEDIA_WEBRTC_DIR}/webrtc.lib plus the Windows system
#      libraries the library needs: winmm, dmoguids, wmcodecdspuuid, secur32, msdmo, ole32,
#      oleaut32, strmiids, d3d11, dxgi, ...).
#   4. Verify the sha256 of webrtc.lib against SHA256SUMS of the release before using it.

function(qmedia_link_webrtc target)
  if(QMEDIA_WITH_WEBRTC)
    message(FATAL_ERROR
      "QMEDIA_WITH_WEBRTC is not wired yet: needs build-flags.json from the Windows library "
      "release (see engine/cmake/webrtc.cmake).")
  endif()
endfunction()
