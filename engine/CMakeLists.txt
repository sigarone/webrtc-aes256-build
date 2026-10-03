cmake_minimum_required(VERSION 3.22)
project(qaudion_media LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(QMEDIA_BUILD_TESTS "Build the IPC unit and pipe tests" ON)
option(QMEDIA_BUILD_FUZZERS "Build the libFuzzer harnesses (clang only)" OFF)
option(QMEDIA_BUILD_ENGINE "Build the qaudion-media executable (Windows only)" ${WIN32})
option(QMEDIA_WITH_WEBRTC "Build the real engine against the published libwebrtc (needs the toolchain file from cmake/fetch_webrtc.cmake)" OFF)
set(QMEDIA_SANITIZE "" CACHE STRING "Sanitizers for GCC/Clang, e.g. address,undefined")

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(QMEDIA_WITH_WEBRTC)
  if(NOT WIN32)
    message(FATAL_ERROR "QMEDIA_WITH_WEBRTC is Windows only")
  endif()
  # The whole project is compiled with the settings libwebrtc was built with (Chromium clang-cl,
  # libc++, /MT). No /EHsc, no Control Flow Guard: libwebrtc.lib is not built with /guard:cf and a
  # guarded indirect call into it would fail.
  include(webrtc)
  qmedia_webrtc_setup()
  set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS}")
elseif(MSVC)
  add_compile_options(/W4 /permissive- /EHsc /utf-8 /GS)
  # Control Flow Guard needs every object that makes an indirect call to be compiled with it, so
  # it is set for the whole project (the IPC libraries included), not only for the executable.
  add_compile_options($<$<CONFIG:Release>:/guard:cf>)
  add_link_options($<$<CONFIG:Release>:/GUARD:CF>)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
  add_compile_options(-Wall -Wextra -Wpedantic -fstack-protector-strong)
  if(QMEDIA_SANITIZE)
    add_compile_options(-fsanitize=${QMEDIA_SANITIZE} -fno-sanitize-recover=all -fno-omit-frame-pointer)
    add_link_options(-fsanitize=${QMEDIA_SANITIZE})
  endif()
  if(QMEDIA_BUILD_FUZZERS)
    add_compile_options(-fsanitize=fuzzer-no-link)
  endif()
endif()
if(WIN32)
  add_compile_definitions(WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE _WIN32_WINNT=0x0A00)
endif()

enable_testing()
add_subdirectory(ipc)

# The timing rules of the local certificate lookup of the engine (no libwebrtc in it, so it is
# built and run on every platform): the constants of WIRE_SPEC section 3.8.4 and the retry loop
# against a fake statistics source.
if(QMEDIA_BUILD_TESTS)
  add_executable(qmedia_engine_policy_tests
    tests/local_cert_policy_test.cpp
    ipc/tests/test_main.cpp)
  target_include_directories(qmedia_engine_policy_tests PRIVATE src ipc/tests)
  add_test(NAME engine_policy COMMAND qmedia_engine_policy_tests)
  set_tests_properties(engine_policy PROPERTIES TIMEOUT 60)
endif()

if(QMEDIA_BUILD_ENGINE)
  if(NOT WIN32)
    message(FATAL_ERROR "qaudion-media is a Windows executable")
  endif()
  if(QMEDIA_WITH_WEBRTC)
    # The engine core, and the libwebrtc runtime twice: the production runtime and the CI runtime
    # that can replace the sound card with files (QMEDIA_CI_BUILD). The production executable has
    # no code path that reads or writes audio files.
    add_library(qmedia_engine_core STATIC
      src/engine.cpp src/peer.cpp src/keys.cpp src/stats.cpp src/devices.cpp)
    target_link_libraries(qmedia_engine_core PUBLIC qmedia_ipc)
    target_compile_definitions(qmedia_engine_core PUBLIC QMEDIA_WITH_WEBRTC_ENGINE=1)
    add_library(qmedia_runtime STATIC src/runtime.cpp)
    target_link_libraries(qmedia_runtime PUBLIC qmedia_ipc)
    add_library(qmedia_runtime_ci STATIC src/runtime.cpp)
    target_link_libraries(qmedia_runtime_ci PUBLIC qmedia_ipc)
    target_compile_definitions(qmedia_runtime_ci PUBLIC QMEDIA_CI_BUILD=1)

    add_executable(qaudion-media src/main.cpp)
    target_link_libraries(qaudion-media PRIVATE qmedia_engine_core qmedia_runtime qmedia_pipe)
    add_executable(qaudion-media-ci src/main.cpp)
    target_link_libraries(qaudion-media-ci PRIVATE qmedia_engine_core qmedia_runtime_ci qmedia_pipe)
    foreach(t qaudion-media qaudion-media-ci)
      target_link_options(${t} PRIVATE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)
      qmedia_link_webrtc(${t})
    endforeach()

    # Two engine processes call each other over their pipes (see tests/call_test.cpp).
    add_executable(qmedia_call_test tests/call_test.cpp)
    target_link_libraries(qmedia_call_test PRIVATE qmedia_pipe qmedia_ipc)
    # The driver shares the engine's C++ library (libc++ runtime), so it links the same way.
    qmedia_link_webrtc(qmedia_call_test)
    add_dependencies(qmedia_call_test qaudion-media-ci)
    target_compile_definitions(qmedia_call_test PRIVATE
      QMEDIA_ENGINE_CI_EXE="$<TARGET_FILE:qaudion-media-ci>")
  else()
    add_executable(qaudion-media src/main.cpp)
    target_link_libraries(qaudion-media PRIVATE qmedia_ipc qmedia_pipe)
    target_link_options(qaudion-media PRIVATE /DYNAMICBASE /NXCOMPAT /HIGHENTROPYVA)
  endif()
endif()
