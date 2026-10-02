# Downloads and verifies everything the engine build needs, then writes a toolchain file.
#
#   cmake -DQMEDIA_FETCH_DIR=<dir> -P engine/cmake/fetch_webrtc.cmake
#
# Output in <dir>:
#   release/        the seven release files, each verified against the sha256 pinned in
#                   webrtc-release.cmake (SHA256SUMS of the release is cross-checked too)
#   release/hdr/    webrtc-headers.zip unpacked
#   clang/          the Chromium clang package named by build-flags.json, sha256 verified
#   toolchain.cmake pass it to the engine configure step with -DCMAKE_TOOLCHAIN_FILE=
#
# The attestation of webrtc.lib is verified by the CI workflow (gh attestation verify); this script
# only deals with hashes.

cmake_minimum_required(VERSION 3.22)
include("${CMAKE_CURRENT_LIST_DIR}/webrtc-release.cmake")

if(NOT QMEDIA_FETCH_DIR)
  message(FATAL_ERROR "pass -DQMEDIA_FETCH_DIR=<directory>")
endif()
get_filename_component(OUT "${QMEDIA_FETCH_DIR}" ABSOLUTE)
set(REL "${OUT}/release")
file(MAKE_DIRECTORY "${REL}")

function(qm_download url dest sha256)
  set(ok FALSE)
  foreach(attempt RANGE 1 4)
    if(EXISTS "${dest}")
      file(SHA256 "${dest}" have)
      if(have STREQUAL "${sha256}")
        set(ok TRUE)
        break()
      endif()
      file(REMOVE "${dest}")
    endif()
    message(STATUS "download (attempt ${attempt}): ${url}")
    file(DOWNLOAD "${url}" "${dest}" INACTIVITY_TIMEOUT 120 TIMEOUT 1800 TLS_VERIFY ON STATUS st)
  endforeach()
  if(NOT ok)
    if(EXISTS "${dest}")
      file(SHA256 "${dest}" have)
      if(have STREQUAL "${sha256}")
        set(ok TRUE)
      endif()
    endif()
  endif()
  if(NOT ok)
    message(FATAL_ERROR "sha256 mismatch or download failure for ${dest} (expected ${sha256})")
  endif()
endfunction()

# 1. The release files.
foreach(f IN LISTS QMEDIA_WEBRTC_FILES)
  qm_download("https://github.com/${QMEDIA_WEBRTC_REPO}/releases/download/${QMEDIA_WEBRTC_TAG}/${f}"
              "${REL}/${f}" "${QMEDIA_SHA256_${f}}")
  message(STATUS "verified ${f}")
endforeach()

# 2. The release's own SHA256SUMS must say the same as the pins (and list nothing else).
file(STRINGS "${REL}/SHA256SUMS" sums_lines)
set(listed "")
foreach(line IN LISTS sums_lines)
  if(line MATCHES "^([0-9a-f]+) \\*(.+)$")
    set(h "${CMAKE_MATCH_1}")
    set(n "${CMAKE_MATCH_2}")
    if(NOT DEFINED QMEDIA_SHA256_${n})
      message(FATAL_ERROR "SHA256SUMS lists a file that is not pinned: ${n}")
    endif()
    if(NOT h STREQUAL "${QMEDIA_SHA256_${n}}")
      message(FATAL_ERROR "SHA256SUMS disagrees with the pin for ${n}")
    endif()
    list(APPEND listed "${n}")
  elseif(NOT line STREQUAL "")
    message(FATAL_ERROR "unexpected line in SHA256SUMS")
  endif()
endforeach()
foreach(n webrtc.lib libcxx.lib webrtc-headers.zip build-flags.json BUILDINFO.json PINS.txt)
  if(NOT n IN_LIST listed)
    message(FATAL_ERROR "SHA256SUMS does not list ${n}")
  endif()
endforeach()

# 3. Headers.
if(NOT EXISTS "${REL}/hdr/include")
  file(ARCHIVE_EXTRACT INPUT "${REL}/webrtc-headers.zip" DESTINATION "${REL}/hdr")
endif()

# 4. The compiler package named by build-flags.json.
file(READ "${REL}/build-flags.json" flags_json)
string(JSON schema GET "${flags_json}" schema)
if(NOT schema STREQUAL "qaudion-webrtc-buildflags/1")
  message(FATAL_ERROR "unexpected build-flags schema")
endif()
string(JSON clang_url GET "${flags_json}" toolchain clang_package_url)
string(JSON clang_sha GET "${flags_json}" toolchain clang_package_sha256)
if(NOT clang_url MATCHES "^https://commondatastorage\\.googleapis\\.com/chromium-browser-clang/Win/clang-[A-Za-z0-9._-]+\\.tar\\.xz$")
  message(FATAL_ERROR "unexpected clang package url")
endif()
if(NOT clang_sha MATCHES "^[0-9a-f]{64}$")
  message(FATAL_ERROR "build-flags.json carries no valid clang package sha256")
endif()
qm_download("${clang_url}" "${OUT}/clang.tar.xz" "${clang_sha}")
if(NOT EXISTS "${OUT}/clang/bin/clang-cl.exe")
  file(ARCHIVE_EXTRACT INPUT "${OUT}/clang.tar.xz" DESTINATION "${OUT}/clang")
endif()
if(NOT EXISTS "${OUT}/clang/bin/clang-cl.exe" OR NOT EXISTS "${OUT}/clang/bin/lld-link.exe")
  message(FATAL_ERROR "the clang package has no clang-cl.exe / lld-link.exe")
endif()

# 5. Toolchain file for the engine configure step.
file(WRITE "${OUT}/toolchain.cmake"
"set(CMAKE_SYSTEM_NAME Windows)
set(CMAKE_C_COMPILER \"${OUT}/clang/bin/clang-cl.exe\")
set(CMAKE_CXX_COMPILER \"${OUT}/clang/bin/clang-cl.exe\")
set(CMAKE_LINKER \"${OUT}/clang/bin/lld-link.exe\")
set(CMAKE_MSVC_RUNTIME_LIBRARY MultiThreaded)
set(QMEDIA_WEBRTC_DIR \"${REL}\" CACHE PATH \"\")
set(QMEDIA_CLANG_ROOT \"${OUT}/clang\" CACHE PATH \"\")
")
message(STATUS "done: ${OUT}/toolchain.cmake")
