cmake_minimum_required(VERSION 3.20)
project(hevc_probe LANGUAGES CXX)

# Static CRT (/MT), no third-party dependencies: the resulting exe runs on a
# plain Windows 10/11 machine without any redistributable.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(PROBE_GIT_SHA "unknown" CACHE STRING "Commit the probe was built from (short sha)")

add_executable(hevc-probe hevc_probe.cpp)

target_compile_definitions(hevc-probe PRIVATE
  UNICODE _UNICODE
  WIN32_LEAN_AND_MEAN NOMINMAX
  WINVER=0x0A00 _WIN32_WINNT=0x0A00 NTDDI_VERSION=0x0A000006
  PROBE_GIT_SHA="${PROBE_GIT_SHA}")

if(MSVC)
  target_compile_options(hevc-probe PRIVATE /W4 /utf-8 /EHsc /guard:cf /GS /permissive- /Brepro)
  # Media Foundation is delay-loaded so that the probe can report
  # "media_foundation_available": false on Windows Server images that do not
  # ship it, instead of failing to start.
  target_link_options(hevc-probe PRIVATE
    /DELAYLOAD:mfplat.dll /Brepro /DYNAMICBASE /NXCOMPAT /guard:cf /CETCOMPAT)
endif()

target_link_libraries(hevc-probe PRIVATE
  mfplat mfuuid strmiids d3d11 dxgi ole32 delayimp)
