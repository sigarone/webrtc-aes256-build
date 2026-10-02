# Links the published libwebrtc into the engine, with exactly the compile settings the release
# records in build-flags.json.
#
# Inputs (set by the toolchain file that fetch_webrtc.cmake writes):
#   QMEDIA_WEBRTC_DIR   directory with the verified release files and the unpacked headers (hdr/)
#   QMEDIA_CLANG_ROOT   the Chromium clang package (compiler, linker, compiler-rt builtins)
#
# libwebrtc is built with Chromium's libc++ (std::__Cr), the static CRT (/MT), C++20, RTTI on and
# exceptions off. Anything that shares a type with it must be compiled the same way, so in this mode
# the WHOLE project (the IPC library included) is compiled with these settings, not only the engine.
# The MSVC build of the IPC library (QMEDIA_WITH_WEBRTC=OFF) stays as it was.

# Called once from the top-level CMakeLists, before any target is created.
function(qmedia_webrtc_setup)
  if(NOT QMEDIA_WEBRTC_DIR OR NOT QMEDIA_CLANG_ROOT)
    message(FATAL_ERROR
      "QMEDIA_WITH_WEBRTC=ON needs the toolchain file written by cmake/fetch_webrtc.cmake "
      "(it sets QMEDIA_WEBRTC_DIR and QMEDIA_CLANG_ROOT)")
  endif()
  if(NOT CMAKE_CXX_COMPILER_ID STREQUAL "Clang" OR NOT CMAKE_CXX_COMPILER_FRONTEND_VARIANT STREQUAL "MSVC")
    message(FATAL_ERROR "the engine must be compiled with the clang-cl of the Chromium package")
  endif()

  # libwebrtc is a Release build with the static release CRT (/MT, NDEBUG): a debug configuration
  # would pull in the debug CRT (/MTd) and an incompatible STL mode.
  if(NOT CMAKE_BUILD_TYPE STREQUAL "Release")
    message(FATAL_ERROR "QMEDIA_WITH_WEBRTC needs -DCMAKE_BUILD_TYPE=Release")
  endif()

  file(READ "${QMEDIA_WEBRTC_DIR}/build-flags.json" json)
  string(JSON schema GET "${json}" schema)
  if(NOT schema STREQUAL "qaudion-webrtc-buildflags/1")
    message(FATAL_ERROR "unexpected build-flags schema")
  endif()
  set(hdr "${QMEDIA_WEBRTC_DIR}/hdr")
  string(JSON header_root GET "${json}" compile header_root)
  set(root "${hdr}/${header_root}")
  if(NOT IS_DIRECTORY "${root}")
    message(FATAL_ERROR "header root ${header_root} is not in the header archive")
  endif()

  # Defines. build-flags.json is data: only plain tokens may reach the compiler.
  string(JSON n LENGTH "${json}" compile defines)
  math(EXPR last "${n} - 1")
  set(defs "")
  foreach(i RANGE 0 ${last})
    string(JSON d GET "${json}" compile defines ${i})
    if(NOT d MATCHES "^[A-Za-z_][A-Za-z0-9_]*(=[A-Za-z0-9_.:+-]*)?$")
      message(FATAL_ERROR "build-flags.json carries a define that is not a plain token: ${d}")
    endif()
    list(APPEND defs "${d}")
  endforeach()

  # Flags.
  string(JSON n LENGTH "${json}" compile flags)
  math(EXPR last "${n} - 1")
  set(flags "")
  foreach(i RANGE 0 ${last})
    string(JSON f GET "${json}" compile flags ${i})
    if(NOT f MATCHES "^(/[A-Za-z][A-Za-z0-9:_+.=-]*|-[A-Za-z][A-Za-z0-9_+.=-]*)$")
      message(FATAL_ERROR "build-flags.json carries a flag that is not a plain token: ${f}")
    endif()
    list(APPEND flags "${f}")
  endforeach()

  # Include directories, in the order the library was built with. GN wrote them relative to its
  # build directory: "../.." is the source root = <headers>/include.
  string(JSON n LENGTH "${json}" compile include_dirs)
  math(EXPR last "${n} - 1")
  set(incs "")
  foreach(i RANGE 0 ${last})
    string(JSON d GET "${json}" compile include_dirs ${i})
    if(d STREQUAL "../..")
      set(p "${root}")
    elseif(d MATCHES "^\\.\\./\\.\\./([A-Za-z0-9_.+/-]+)$")
      set(p "${root}/${CMAKE_MATCH_1}")
    else()
      continue()  # generated directories the header archive does not carry
    endif()
    if(IS_DIRECTORY "${p}")
      list(APPEND incs "${p}")
    endif()
  endforeach()

  add_compile_definitions(${defs})
  add_compile_options(${flags} -fno-exceptions -Wno-everything)
  include_directories(${incs})

  # No exception handling, as in the library (CMake's default for MSVC-like compilers adds /EHsc).
  string(REPLACE "/EHsc" "" CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}" PARENT_SCOPE)

  # Link line pieces, kept for qmedia_link_webrtc.
  string(JSON n LENGTH "${json}" link static_libs)
  math(EXPR last "${n} - 1")
  set(libs "")
  foreach(i RANGE 0 ${last})
    string(JSON l GET "${json}" link static_libs ${i})
    if(NOT l MATCHES "^[A-Za-z0-9_.+-]+\\.lib$")
      message(FATAL_ERROR "unexpected static library name in build-flags.json: ${l}")
    endif()
    list(APPEND libs "${QMEDIA_WEBRTC_DIR}/${l}")
  endforeach()
  string(JSON n LENGTH "${json}" link system_libs)
  math(EXPR last "${n} - 1")
  foreach(i RANGE 0 ${last})
    string(JSON l GET "${json}" link system_libs ${i})
    if(NOT l MATCHES "^[A-Za-z0-9_.+-]+\\.lib$")
      message(FATAL_ERROR "unexpected system library name in build-flags.json: ${l}")
    endif()
    list(APPEND libs "${l}")
  endforeach()
  string(JSON builtins GET "${json}" link compiler_rt_builtins)
  if(NOT builtins MATCHES "^lib/clang/[0-9]+/lib/windows/[A-Za-z0-9_.-]+\\.lib$")
    message(FATAL_ERROR "unexpected compiler-rt builtins path in build-flags.json")
  endif()
  if(NOT EXISTS "${QMEDIA_CLANG_ROOT}/${builtins}")
    message(FATAL_ERROR "compiler-rt builtins not found: ${builtins}")
  endif()
  list(APPEND libs "${QMEDIA_CLANG_ROOT}/${builtins}")
  string(JSON n LENGTH "${json}" link default_libs)
  math(EXPR last "${n} - 1")
  set(defaults "")
  foreach(i RANGE 0 ${last})
    string(JSON l GET "${json}" link default_libs ${i})
    if(NOT l MATCHES "^[A-Za-z0-9_.+-]+\\.lib$")
      message(FATAL_ERROR "unexpected default library name in build-flags.json")
    endif()
    list(APPEND defaults "/DEFAULTLIB:${l}")
  endforeach()
  set(QMEDIA_WEBRTC_LIBS "${libs}" CACHE INTERNAL "")
  set(QMEDIA_WEBRTC_LINK_OPTIONS "${defaults}" CACHE INTERNAL "")
  message(STATUS "libwebrtc: ${QMEDIA_WEBRTC_DIR} (defines ${n}, include dirs resolved)")
endfunction()

# Links one executable against the library.
function(qmedia_link_webrtc target)
  if(NOT QMEDIA_WITH_WEBRTC)
    return()
  endif()
  target_link_libraries(${target} PRIVATE ${QMEDIA_WEBRTC_LIBS})
  target_link_options(${target} PRIVATE ${QMEDIA_WEBRTC_LINK_OPTIONS} /SUBSYSTEM:CONSOLE /MACHINE:X64)
endfunction()
