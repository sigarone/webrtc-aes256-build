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
  Video Extensions" below), which other registered packages have HEVC in their name, and
  whether a software decoder MFT is registered.
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
- `MFTEnumEx` run four ways for the codec, so that a codec that is installed as a Microsoft
  Store package but is not returned by default can be seen, and decoded with when it is
  returned (see "Store MFTs and the enumeration variants" below).
- What the D3D11 video device of each adapter offers without any MFT: decoder profiles,
  output formats, decoder configurations, and a decoder object made and released (see
  "D3D11VA capability per adapter" below).

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
are gone. `report_version` is 3: version 2 introduced the three-state results, version 3
makes `video_send_possible`, `video_send_1080p_possible` and `video_receive_possible`
three-state strings too (they were booleans, which said `false` for "not tried"), turns
`provisioned_system_image` into `null`, and adds the sections below. The report has no
consumer to keep compatible.

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
| `video_send_possible`, `video_send_1080p_possible` | **derived**, three-state like the values they come from (`ok`, `failed`, `not_attempted`; never `false` for "not tried"): the value of `hardware_encode_720p_status` and `hardware_encode_1080p_status`, which are the truth |
| `video_receive_possible`, `video_receive_basis` | **derived**, three-state: `decode_720p_status`, or (without a 720p bitstream) the first of it and `decoder_configure_only_status` that says more than `not_attempted`. The basis is `d3d11_gpu_decode`, `hardware_mft`, `decoder_without_gpu_path`, `configure_only` or `none` |
| `decoder_mft_count_default`, `..._with_store_flag`, `..._unfiltered`, `..._unfiltered_with_store_flag`, `encoder_mft_count_default`, `encoder_mft_count_with_store_flag` | how many distinct MFTs each enumeration variant returned (-1: the variant could not run) |
| `store_mft_decoders`, `store_mft_encoders` | `name (clsid)` of the MFTs that **only** a variant with `MFT_ENUM_FLAG_UNTRUSTED_STOREMFT` returned |
| `store_mft_activation_status` | ok when `ActivateObject` created one of them, failed when every one that was reached was refused (the HRESULT is in `reasons`, and the test says `activation_refused`), not_attempted when none was returned |
| `store_mft_decode_720p_status`, `store_mft_decode_1080p_status` | those MFTs decoded the stream (same rules as the other decode values) |
| `other_flags_mft_decoders`, `other_flags_mft_activation_status`, `other_flags_mft_decode_*_status` | the same for MFTs that a variant **without** the store flag returned and the default one did not (field-of-use, transcode-only, local or blocked MFTs) |
| `d3d11va_decode_supported` | ok when a **non-software** adapter passed every D3D11VA check for the codec's main profile (`HEVC_VLD_MAIN`, `H264_VLD_NOFGT` for the H.264 run): the profile is listed, its output format is supported, a 1080p decoder configuration exists and a decoder object could be made. failed when a non-software adapter was asked and none passed, not_attempted when none was asked. A software adapter never counts |
| `d3d11va_decode_by_adapter` | the same, adapter by adapter (description, vendor id, status, reason), so that the Intel and the NVIDIA adapter of a hybrid laptop are told apart |
| `d3d11va_main10_decode_supported`, `d3d11va_main10_decode_by_adapter` | HEVC run only: the same for `HEVC_VLD_MAIN10` (P010) |
| `reasons` | for every status that is not `ok`, why |

The decode values (`decode_*`, `dxva_decode_*`, `hardware_accelerated_decode_status`, ...)
include the MFTs that only a non-default enumeration returned (up to three are tested, after
the up to six default ones); every test says which class its MFT is in (`mft_class`:
`default`, `store_flag_only`, `needs_other_flags`).

## HEVC Video Extensions

The package families the probe and the lab scripts check (one list, compared by CI) are
`Microsoft.HEVCVideoExtension_8wekyb3d8bbwe`, `Microsoft.HEVCVideoExtensions_8wekyb3d8bbwe`
and `Microsoft.HEVCVideoExtensionFirstParty_8wekyb3d8bbwe` (the last one is the package the
lab PC has; no public documentation of it was found when it was added). The packages are
registered per user, and an application sees only those registered for the user it runs as.
The report says:

- `registered_current_user`: the packages of the three families registered for the current
  user (`GetPackagesByPackageFamily`), with `packages_registered_current_user` and
  `family_names_checked`. This is the value that counts.
- `hevc_named_packages_current_user`: every package registered for the current user whose
  name contains `HEVC`, family known or not (`in_known_family_list`), with
  `not_in_family_list_count`, so that a new variant is never missed again. It is read from the
  per-user package repository in the registry, which is **not a documented API** (observed on
  Windows 10: one sub key per registered package, named by the package full name); `readable`
  says whether that worked. The scripts list the same packages with `Get-AppxPackage -Name
  *HEVC*`.
- `provisioned_system_image`: `null`. Whether a package is provisioned in the system image
  needs elevation to read, so the probe does not read it (`null` means "could not be read",
  never "no", as in the scripts); `hw-inventory.ps1` reports it, and registration for any
  user, with the same definition as `setup-hwlab.ps1`.

When no decoder MFT is returned, the reason in `summary.reasons` says what was measured
("MFTEnumEx returns no HEVC decoder MFT to this process (default enumeration, with
MFT_ENUM_FLAG_UNTRUSTED_STOREMFT, and unfiltered)") and adds what is known about the packages:
that one is registered (so registration is not what is missing), that a package with HEVC in
its name is registered but not in the family list, or that none is. A registered package does
not mean that its decoder is enumerated.

`hw-inventory.ps1` also reads the manifest of each of those packages and reports the media
codecs it declares (`hevc_package_media_codecs`: category `videoDecoder` or `videoEncoder`,
whether it takes or produces HEVC), because an enumerated Store MFT can only come from what a
package declares.

## Store MFTs and the enumeration variants

Why a Store-installed HEVC decoder may not be returned to a Win32 desktop process, with the
status of each statement. "Per docs" is Microsoft Learn (pages read on 2026-10-04); "observed"
says where.

- Per docs: the HEVC decoder is a Media Foundation transform that is created through `MFTEnum`
  or `MFTEnumEx`, takes `MFVideoFormat_HEVC` or `MFVideoFormat_HEVC_ES`, delivers NV12 or P010,
  supports Main, Main Still Picture and Main10, "supports DX11 and DX12 DXVA, but not DXVA
  version 2 or DXVA version 1", and ships as `hevcdecoder.dll` and `hevcdecoder_store.dll`.
  ([H.265 / HEVC Video Decoder](https://learn.microsoft.com/en-us/windows/win32/medfound/h-265---hevc-video-decoder))
- Per docs: `MFT_ENUM_FLAG_UNTRUSTED_STOREMFT` exists in `_MFT_ENUM_FLAG` with the value
  `0x00000400`, and the page gives it no description. The flags that do have one include MFTs
  that are "otherwise excluded": field-of-use MFTs, local MFTs and transcode-only MFTs;
  `MFT_ENUM_FLAG_SORTANDFILTER` drops MFTs on the blocked list; `MFTEnumEx` itself does not
  mention Store packages.
  ([_MFT_ENUM_FLAG](https://learn.microsoft.com/en-us/windows/win32/api/mfapi/ne-mfapi-_mft_enum_flag),
  [MFTEnumEx](https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mftenumex),
  [Registering and Enumerating MFTs](https://learn.microsoft.com/en-us/windows/win32/medfound/registering-and-enumerating-mfts))
- Per docs: `MFStartup` must be called before Media Foundation is used (the probe does, with
  `MFSTARTUP_FULL`); the page says nothing about Store packages, COM security or the process.
  ([MFStartup](https://learn.microsoft.com/en-us/windows/win32/api/mfapi/nf-mfapi-mfstartup))
- Third-party article, not Microsoft documentation (2019): a Store codec extension declares its
  codecs in its package manifest (a `windows.mediaCodec` extension), `MFTEnumEx` merges those
  with the registered MFTs, they are activated through a special activation object
  (`CMFWinrtInprocActivate`), and they disappeared from `MFTEnumEx` in a process that called
  `CoInitializeSecurity` with `RPC_C_IMP_LEVEL_ANONYMOUS`. The probe does not call
  `CoInitializeSecurity`; whether any other property of the process matters is not known.
  ([Media Foundation API primitive styling of WinRT windows.mediaCodec](https://alax.info/blog/1979))
- User report on Microsoft Q&A: a Store HEVC MFT that `MFTEnumEx` returned could not be
  activated (`E_ACCESSDENIED`) on one machine because the DLL in the package folder had its own
  non-inherited ACL; fixing the ACL fixed it. This is why a refused `ActivateObject` is
  recorded as such (`activation_refused`, with a note for `E_ACCESSDENIED`) and not treated as
  "no decoder".
  ([thread](https://learn.microsoft.com/en-us/answers/questions/210699/mftenumex-can-get-hevc-decoder-and-or-encoder-but))
- Observed on the CI runner (hosted `windows-2022`, no HEVC package): `MFTEnumEx` accepts
  `MFT_ENUM_FLAG_UNTRUSTED_STOREMFT` without an error, and for H.264 it returns the same single
  in-box encoder and decoder with the flag as without it. What the flag does on a machine that
  has a Store codec is not known; that is what the lab measures.

So the probe does not assume. For the encoders and the decoders of the codec it runs four
enumerations (`store_mft_enumeration` in the report; counts per query, the flags that were
passed, and every MFT with its name and CLSID):

| Variant | Flags (besides the category and the media type) |
| --- | --- |
| `default` | hardware, async, sync, each with `SORTANDFILTER` (what every earlier version of the probe did; the `decoders` and `encoders` objects) |
| `store_flag` | the same, plus `MFT_ENUM_FLAG_UNTRUSTED_STOREMFT` |
| `unfiltered_all` | one query with `MFT_ENUM_FLAG_ALL` and no `SORTANDFILTER` (field-of-use, local and transcode-only MFTs included, blocked ones not dropped) |
| `unfiltered_all_store_flag` | the same, plus the store flag |

An MFT that a variant other than `default` returned and `default` did not is listed under
`only_outside_the_default_enumeration` and tested like the others, found again through the same
flags: `mft_class` is `store_flag_only` when only variants with the store flag returned it,
`needs_other_flags` when a variant without it did. `default_mfts_missing_with_store_flag` lists
default MFTs that the store flag variant did not return, in case the flag restricts instead of
adds. Every MFT also carries `returned_by`, the variants that returned it.

How to read it on the lab PC when the HEVC decoder count is 0:

- `decoder_mft_count_with_store_flag` above 0 (`store_mft_decoders` names it): the flag is
  needed to see the decoder. `store_mft_activation_status` says whether it can be created and
  `store_mft_decode_*_status` whether it decodes the 720p and 1080p streams, per memory model
  and adapter like any other decoder.
- `other_flags_mft_decoders` not empty: it was only hidden by the default filters.
- Every count 0 although `hevc_named_packages_current_user` lists a package: look at
  `hevc_package_media_codecs` of `hw-inventory.ps1` (does the package declare a video decoder
  for HEVC at all?), and at `d3d11va` below, which does not depend on any MFT.

## D3D11VA capability per adapter

`d3d11va` answers whether the GPU can decode a codec through D3D11 video decoding directly,
with no MFT at all, adapter by adapter (the Intel and the NVIDIA adapter of a hybrid laptop are
separate entries, each saying which adapter it used). It is read-only capability probing: for
every DXGI adapter the probe makes a D3D11 device with `D3D11_CREATE_DEVICE_VIDEO_SUPPORT`
(software adapters only with `--ci` or `--allow-software-adapter`, and they never count), asks
`ID3D11VideoDevice`, and releases everything. No frame is decoded.

- `profiles`: every decoder profile the driver lists (`GetVideoDecoderProfileCount`,
  `GetVideoDecoderProfile`), by GUID and, for the known ones, by the `D3D11_DECODER_PROFILE_`
  name. The names and GUIDs the probe knows are in `profile_names_known`.
- `checked`: six profiles get the full check: `H264_VLD_NOFGT`, `HEVC_VLD_MAIN`,
  `HEVC_VLD_MAIN10`, `VP9_VLD_PROFILE0`, `VP9_VLD_10BIT_PROFILE2`, `AV1_VLD_PROFILE0`. For each:
  is it listed; `CheckVideoDecoderFormat` for NV12 and P010 (per docs it returns `E_INVALIDARG` for
  a profile the driver does not support, which is why an unlisted profile is not asked and its
  answers are `null`); `GetVideoDecoderConfigCount` at 1920x1080 and 3840x2160 for the native
  format (NV12 for 8 bit profiles, P010 for the 10 bit ones); and a decoder object
  (`GetVideoDecoderConfig` and `CreateVideoDecoder`) made at 1920x1080, then at 1920x1088 if
  the driver refuses that, and released at once (`decoder_object.tries` lists each try).
  ([CheckVideoDecoderFormat](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11videodevice-checkvideodecoderformat),
  [GetVideoDecoderConfigCount](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11videodevice-getvideodecoderconfigcount),
  [CreateVideoDecoder](https://learn.microsoft.com/en-us/windows/win32/api/d3d11/nf-d3d11-id3d11videodevice-createvideodecoder))
- A profile is `ok` when it is listed, its native output format is supported, a 1080p decoder
  configuration exists and the decoder object could be made; otherwise `failed` with
  `failed_stage` (`profile_not_listed`, `output_format_nv12_not_supported`,
  `output_format_p010_not_supported`, `no_decoder_config_1080p`, `get_config`,
  `create_decoder`), `hresult` and `reason`. An adapter is `failed` when its D3D11 device or video
  device could not be made (`create_d3d_device`, `query_video_device`), and `not_attempted` for a
  software adapter that was not asked, or a software adapter without a video device.
- The summary values `d3d11va_decode_supported` and `d3d11va_main10_decode_supported` (see the
  table above) fold this into one tri-state value for the codec, with `..._by_adapter` per adapter.

On a hybrid laptop this separates three questions the MFT tests mix: does the adapter offer the
profile at all, can it make the decoder object, and does the D3D11-aware in-box MFT work with
that adapter (the `dxva_decode_*` tests). An adapter that passes here and fails at
`set_d3d_manager` in the MFT test points at the MFT or the adapter selection, not at the driver.

`--selftest-d3d11va-logic` runs the decision logic of these checks (what `ok` means, which stage
a failure is blamed on, that a software adapter never counts) against a scripted mock of
`ID3D11VideoDevice` and prints what it decided; CI uses it because the hosted runner has no video
device. The profile GUIDs are written out in the source and compared by CI with `d3d11.h` of the
Windows SDK of the runner.

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
| `--selftest-d3d11va-logic` | run the D3D11VA decision logic against a scripted mock device and print what it decided (CI only: no driver is touched, no report file is written) |

## Privacy

The report contains no user name, machine name, serial numbers, MAC or IP addresses and
no file system paths. It does contain product names (CPU, GPU, MFTs, package names), the
Windows version number, driver versions and package version strings.

## Build

CMake, MSVC, static CRT, no third-party dependencies. CI (`hevc-probe.yml`) builds it on
`windows-2022`, runs it there (HEVC, then the decoder profile GUIDs against the SDK header, the
D3D11VA decision logic against the mock device, the H.264 self-test with the D3D11 path on the
software adapter, then 720p alone and 1080p alone) and uploads the exe with its sha256 as
the workflow artifact `hevc-probe-windows-x64`. The hosted runner has no GPU and no HEVC
package: it proves the shape of every section, that nothing is reported `ok` that was not
measured, and the decision logic; what a Store HEVC decoder or a GPU does is measured on the
lab PC.
