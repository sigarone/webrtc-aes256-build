# hevc-probe

A small Windows console tool that reports what a machine can do with HEVC (H.265) or
H.264 through Media Foundation. It is a generic diagnostic and contains no application
protocol logic.

## What it measures

- The encoder and decoder MFTs that `MFTEnumEx` returns, with the flags hardware /
  async / sync, the vendor id string and the friendly name.
- The DXGI adapters (description, vendor and device id, dedicated memory, driver
  version).
- Whether the HEVC Video Extensions are registered for the current user (see "HEVC
  Video Extensions" below) and whether a software decoder MFT is registered.
- A real encode of 60 synthetic NV12 frames (30 fps) at 1280x720 and 1920x1080 with
  every hardware encoder (up to four), trying a D3D11 device manager first and system
  memory as a fallback; a software encoder is only tried at 720p when no hardware
  encoder worked. At 1080p each encoder that worked at 720p is tried in both memory
  models; when no hardware encoder produced a 1080p stream, a software encoder (the
  in-box H.264 encoder, if the codec has one) makes the stream so that the decoders can
  still be tested, and the report says so (`stream_source_1080p`).
- A decode of the encoded stream by every decoder MFT (up to six, hardware first), at
  each resolution, on the stream of that resolution. A hardware decoder is tried with a
  D3D11 manager and falls back to system memory. A software decoder is tested in both
  memory models: with a D3D11 device manager when it is D3D11 aware (`MF_SA_D3D11_AWARE`,
  which is what the in-box decoders are and what the GPU decode path on Windows is), on
  each of up to two hardware adapters, and in system memory.
- When no encoder worked there is no bitstream to decode: the decoders are then only
  created and configured for the codec (`kind: configure_only`).

Each test reports its result, frames in and out, bytes, fps and, on failure, the stage
and the HRESULT. Every test runs on its own thread with a 45 second watchdog and a
structured-exception guard, so a hanging or crashing driver is reported instead of
ending the probe. The whole run is capped at 30 minutes. Media Foundation and the other
system DLLs are loaded from System32 only.

## Every attempt is independent

An attempt never reuses anything from an earlier one. It makes its own D3D11 device
(`D3D11_CREATE_DEVICE_VIDEO_SUPPORT`, multithread protection on, on an adapter chosen
explicitly: the one whose vendor matches the MFT, else the first hardware adapter) and
its own DXGI device manager, enumerates the MFT again (new activation object), activates
it, and at the end shuts it down through `IMFActivate::ShutdownObject` (after releasing
the D3D manager and stopping the stream), releases every COM object and the device
manager, flushes the device, and pauses 300 ms before the next attempt starts. The report
carries what the teardown found: `mft_refs_left_after_release` is the reference count the
MFT had left after the last release (0 is clean; more means something still holds the
instance), and a hung earlier attempt is noted on the attempts after it. The result of an
attempt is taken before its teardown runs and the teardown is waited for separately (10
seconds), so a call that blocks in the teardown costs the step trace of that attempt, not
the result of an attempt that worked.

Why. In version 1 of the probe, a 1080p encode failed in
`ProcessMessage(MFT_MESSAGE_SET_D3D_MANAGER)` with `E_FAIL` seconds after a 720p encode
with the same MFT and the same call sequence had worked. That call cannot depend on the
resolution (no media type is set yet), so something left over from the earlier attempt
is the first suspect. What the code did: the 720p MFT instance was never shut down (the
activation object had been detached, so `ShutdownObject` could not do it either), the
same activation object was activated again for 1080p, the DXGI manager and the device
were only released by going out of scope, and the three calls of the manager setup
(create the manager, reset the device, set it on the MFT) shared one stage label, so the
report could not say which one failed. The device itself was already created with video
support and multithread protection on an adapter of the MFT's vendor, so that is not
the cause. The independence described above removes every one of those leftovers, and
the split stages and the per-step HRESULTs say exactly which call fails if one still
does. Whether leftover state was the cause is decided on the hardware: compare a full
run with `--resolutions 1080` (`run-hevc-probe.ps1` does that comparison by itself when
1080p fails after a successful 720p).

## Results are ok, failed or not_attempted

Every test and every summary value is one of

- `ok`: the attempt ran and succeeded;
- `failed`: the attempt ran and failed (`failed_stage`, `hresult`, and the `steps`);
- `not_attempted`: the attempt was not made, with `reason_code` and `reason` (for
  example no D3D11 adapter, the decoder is not D3D11 aware, no bitstream to decode, the
  earlier 720p run of the same mode failed).

A `not_attempted` result is never `failed`, and nothing is reported `false` because it
was not tried. The old boolean `ok` field of a test and the boolean `*_ok` summary fields
are gone: the report is `report_version` 2 and has no consumer to keep compatible.

Each test also lists its `steps`, one entry per call with its own HRESULT:
`create_d3d_device`, `create_dxgi_manager`, `reset_device`, `enumerate_fresh`, `activate`,
`async_unlock`, `query_event_generator`, `set_d3d_manager`, `set_output_type`,
`set_input_type`, `begin_streaming`, then the teardown (`release_d3d_manager`,
`shutdown_object`). `failed_stage` names the call that failed.

## Reading the summary

Every key is prefixed with the codec (`hevc_` or `h264_`).

| Key | Meaning |
| --- | --- |
| `hardware_encoder_present`, `software_encoder_present` | an encoder MFT of that kind is enumerated |
| `hardware_decoder_present`, `software_decoder_present` | a decoder MFT of that kind is enumerated. "Hardware" here means a vendor decoder MFT flagged hardware by `MFTEnumEx`. On Windows, GPU decoding normally goes through the D3D11-aware in-box decoder instead, so `hardware_decoder_present: false` does **not** mean there is no hardware decode: read `dxva_decode_*` and `hardware_accelerated_decode_status` |
| `hardware_encode_720p_status`, `hardware_encode_1080p_status` | a hardware encoder encoded the 60 frames |
| `any_encode_720p_status`, `any_encode_1080p_status` | any encoder did, software ones included |
| `decode_720p_status`, `decode_1080p_status` | any decoder decoded the stream, in any memory model |
| `dxva_decode_720p_status`, `dxva_decode_1080p_status` | a decoder took a D3D11 device manager on a real (non-software) adapter and delivered every frame as a D3D11 texture (`IMFDXGIBuffer`): the GPU decode path |
| `hardware_mft_decode_720p_status`, `hardware_mft_decode_1080p_status` | a hardware-flagged decoder MFT decoded |
| `hardware_accelerated_decode_status` | ok when either of the two above is ok |
| `decoder_configure_only_status` | only when there was no bitstream: a decoder could be created and configured |
| `best_encode_fps_*`, `best_encoder` | throughput of the best working encoder |
| `stream_source_720p`, `stream_source_1080p` | which encoder produced the bitstream the decoders got (null when none) |
| `video_send_possible`, `video_send_1080p_possible` | a hardware encoder works at 720p (1080p) |
| `video_receive_possible`, `video_receive_basis` | some decoder works at 720p, DXVA included. The basis is `d3d11_gpu_decode`, `hardware_mft`, `decoder_without_gpu_path`, `configure_only` or `none` |
| `reasons` | for every status that is not `ok`, why |

## HEVC Video Extensions

The package families are `Microsoft.HEVCVideoExtension_8wekyb3d8bbwe` and
`Microsoft.HEVCVideoExtensions_8wekyb3d8bbwe`. The packages are registered per user, and
an application sees only those registered for the user it runs as. The report says:

- `registered_current_user`: the packages registered for the current user
  (`GetPackagesByPackageFamily`), with `packages_registered_current_user` and
  `family_names_checked`. This is the value that counts.
- `provisioned_system_image`: `"unknown"`. Whether a package is provisioned in the system
  image needs elevation to read; `hw-inventory.ps1` reports it (and registration for any
  user) with the same definition as `setup-hwlab.ps1`.

## Hybrid GPU laptops

A vendor MFT belongs to one adapter. On a laptop with an integrated and a discrete GPU
the discrete GPU's MFTs often fail to activate for a process that does not run on that
GPU. The probe records the failure and goes on; when the MFT's vendor is not the vendor
of the first adapter (or the D3D11 device had to be made on another vendor's adapter) the
test carries a `hint` explaining it, with the adapter that was used. It is a finding, not
a probe failure.

## Running it

Double-click `run-probe.cmd` (it keeps the window open), or from a terminal:

    hevc-probe.exe > report.json

The JSON goes to stdout and to `hevc-probe-report.json` next to the exe (the location can
be overridden with `--out <file name>`). Progress goes to stderr. The `summary` object at
the end answers the questions that matter.

| Option | Effect |
| --- | --- |
| `--codec h264` | run the identical pipeline on H.264 (the control run; CI uses it because the hosted runner has no HEVC MFT) |
| `--resolutions 720,1080` | run only the listed resolutions (default both). `1080` alone shows whether a 1080p failure depends on the 720p run before it |
| `--allow-software-adapter` | let the D3D11 attempts use a software adapter (WARP) on a machine without a GPU. It runs the code path and proves nothing about hardware; a decode on it is never counted as a GPU decode |
| `--ci` | `--allow-software-adapter`, and exit code 4 when an attempt crashed or timed out. Without it the exit code is 0 whenever the report was written, a machine without a GPU included |
| `--pause` | wait for Enter at the end |

## Privacy

The report contains no user name, machine name, serial numbers, MAC or IP addresses and
no file system paths. It does contain product names (CPU, GPU, MFTs), the Windows version
number, driver versions and package version strings.

## Build

CMake, MSVC, static CRT, no third-party dependencies. CI (`hevc-probe.yml`) builds it on
`windows-2022`, runs it there (HEVC, then the H.264 self-test with the D3D11 path on the
software adapter, then 720p alone and 1080p alone) and uploads the exe with its sha256 as
the workflow artifact `hevc-probe-windows-x64`.
