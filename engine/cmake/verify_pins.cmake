# Re-checks, at configure time and whoever produced the directories, that what the engine is about
# to be built against is exactly the pinned release:
#   - every release file is present and matches the sha256 pinned in webrtc-release.cmake;
#   - the unpacked headers come from the pinned archive (stamp written by fetch_webrtc.cmake);
#   - the compiler directory comes from the package build-flags.json (itself pinned) names.
# A missing or different file stops the configure. Included by the toolchain file that
# fetch_webrtc.cmake writes (so it runs before any compiler is invoked) and by webrtc.cmake (so a
# configure that uses another toolchain file is stopped too).
#
# Inputs: QMEDIA_WEBRTC_DIR, QMEDIA_CLANG_ROOT.

include("${CMAKE_CURRENT_LIST_DIR}/webrtc-release.cmake")

if(NOT QMEDIA_WEBRTC_DIR OR NOT QMEDIA_CLANG_ROOT)
  message(FATAL_ERROR "QMEDIA_WEBRTC_DIR and QMEDIA_CLANG_ROOT must be set")
endif()

foreach(f IN LISTS QMEDIA_WEBRTC_FILES)
  if(NOT EXISTS "${QMEDIA_WEBRTC_DIR}/${f}")
    message(FATAL_ERROR "pinned release file is missing: ${f}")
  endif()
  file(SHA256 "${QMEDIA_WEBRTC_DIR}/${f}" qm_have_sha)
  if(NOT DEFINED QMEDIA_SHA256_${f} OR NOT qm_have_sha STREQUAL "${QMEDIA_SHA256_${f}}")
    message(FATAL_ERROR "pinned release file does not match its sha256: ${f}")
  endif()
endforeach()

if(NOT EXISTS "${QMEDIA_WEBRTC_DIR}/hdr/.qmedia-archive-sha256")
  message(FATAL_ERROR "the header directory has no archive stamp (run cmake/fetch_webrtc.cmake)")
endif()
file(READ "${QMEDIA_WEBRTC_DIR}/hdr/.qmedia-archive-sha256" qm_hdr_stamp)
if(NOT qm_hdr_stamp STREQUAL "${QMEDIA_SHA256_webrtc-headers.zip}")
  message(FATAL_ERROR "the header directory does not come from the pinned archive")
endif()

file(READ "${QMEDIA_WEBRTC_DIR}/build-flags.json" qm_flags_json)
string(JSON qm_clang_sha GET "${qm_flags_json}" toolchain clang_package_sha256)
if(NOT EXISTS "${QMEDIA_CLANG_ROOT}/.qmedia-package-sha256")
  message(FATAL_ERROR "the compiler directory has no package stamp (run cmake/fetch_webrtc.cmake)")
endif()
file(READ "${QMEDIA_CLANG_ROOT}/.qmedia-package-sha256" qm_clang_stamp)
if(NOT qm_clang_stamp STREQUAL "${qm_clang_sha}")
  message(FATAL_ERROR "the compiler directory does not come from the package build-flags.json names")
endif()
