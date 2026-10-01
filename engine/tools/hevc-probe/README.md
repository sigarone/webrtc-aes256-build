# hevc-probe

A small Windows console tool that reports what a machine can do with HEVC (H.265)
through Media Foundation. It is a generic diagnostic and contains no application
protocol logic.

## What it measures

- The HEVC encoder and decoder MFTs that `MFTEnumEx` returns, with the flags
  hardware / async / sync, the vendor id string and the friendly name.
- The DXGI adapters (description, vendor and device id, dedicated memory, driver
  version).
- Whether the HEVC Video Extensions package is installed for the current user and
  whether a software decoder MFT is registered.
- A real encode of 60 synthetic NV12 frames (30 fps) at 1280x720 and 1920x1080 with
  every hardware encoder (up to four), trying a D3D11 device manager first and
  system memory as a fallback; a software encoder is only tried when no hardware
  encoder worked. The encoded stream is then decoded by every decoder MFT (up to
  six, hardware first). Each test reports success, frames in and out, bytes, fps
  and, on failure, the stage and the HRESULT. Every test runs on its own thread with
  a 45 second watchdog and a structured-exception guard, so a hanging or crashing
  driver is reported instead of ending the probe.
- When no encoder worked there is no bitstream to decode: the decoders are then only
  created and configured for HEVC input (`kind: configure_only`).

The whole run is capped at 30 minutes (the start-up enumeration loads vendor
drivers and is not covered by the per-test watchdog). Media Foundation and the other
system DLLs are loaded from System32 only.

## Running it

Double-click `run-probe.cmd` (it keeps the window open), or from a terminal:

    hevc-probe.exe > report.json

The JSON goes to stdout and to `hevc-probe-report.json` next to the exe (the
location can be overridden with `--out <file name>`). Progress goes to stderr.
The `summary` object at the end answers the questions that matter:
`hevc_hardware_encode_720p_ok`, `hevc_decode_720p_ok`, `video_send_possible`,
`video_receive_possible`.

`--codec h264` runs the identical pipeline on H.264. CI uses it as a self-test of
the encode and decode machinery, because the hosted runner has no HEVC MFT.

## Privacy

The report contains no user name, machine name, serial numbers, MAC or IP addresses
and no file system paths. It does contain product names (CPU, GPU, MFTs), the
Windows version number, driver versions and package version strings.

## Build

CMake, MSVC, static CRT, no third-party dependencies. CI (`hevc-probe.yml`) builds
it on `windows-2022`, runs it there, and uploads the exe with its sha256 as the
workflow artifact `hevc-probe-windows-x64`.
