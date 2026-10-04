# hwlab

A kit that turns a second, modern Windows PC into a hardware test lab for the
desktop media engine in this repository.

The machine that runs the day to day work is old and has no GPU, so the checks that
need real hardware cannot run there. The lab PC covers them:

- H.265 (HEVC) encode and decode through Media Foundation on a real GPU, using the
  probe in `engine/tools/hevc-probe/`.
- Core Audio endpoints and Bluetooth audio devices.
- The camera.
- Fast local builds of the engine, on a machine with more cores than a hosted CI
  runner would give for a quick edit-and-try loop.

The lab runs its own Claude Code session (Claude desktop app). Another session, the
orchestrator, sends it requests through cross-session messages (Remote Control). The
lab session runs the requested script or build and sends back the result as text or
JSON.

## Trust model

The lab is deliberately a low-privilege, low-value machine from the project's point of
view. These rules hold for the kit and for the session that runs it.

- The lab only reads this public repository. It clones it over HTTPS without
  credentials and updates it with fast-forward pulls. The kit sets the push URL of the
  clone to an invalid value, so a push cannot succeed even by mistake.
- The lab holds no secrets, no SSH keys and no GitHub tokens of the project. Nothing
  in the kit asks for one and nothing in the kit needs one.
- The lab never connects to production servers.
- The lab never pushes to any repository.
- Results leave the lab only as text or JSON, delivered to the orchestrator session by
  the lab session. No installer, binary or log is uploaded anywhere else.
- Reports carry no user name, no machine name, no serial numbers, no MAC addresses and
  no IP addresses. The scripts of this kit strip those values and then check the final
  text for them before it is written. One caveat: friendly names of devices are
  reported as the operating system shows them, and a Bluetooth headset can carry the
  name its owner gave it. The inventory script replaces the account name, the machine
  name and possessive owner names (such as `<owner>'s`) but cannot know every name, so
  a person reads an inventory once before it is shared.
- The scripts of this kit send no telemetry and download nothing except through
  winget (the five packages listed below), the `git clone` and `git pull` of this
  repository, and, only when the owner has agreed, the engine build download of
  `engine-local-build.ps1 -AllowDownload`: the pinned libwebrtc release from github.com
  (which redirects to `release-assets.githubusercontent.com`) and the Chromium clang package
  from `commondatastorage.googleapis.com`, every file checked against a sha256 pinned in the
  repository. That script prints the hosts and URLs first and refuses to download without
  `-AllowDownload`, so the lab session can show them to the owner. The hosts the lab session
  may contact are listed exactly in `CLAUDE.md`, never with a wildcard: `github.com`,
  `release-assets.githubusercontent.com` (the redirect target of the release files, checked
  by CI against a real release asset on every run of `hwlab-engine-build.yml`; if GitHub ever
  changes it, CI fails and the list is updated in a reviewed commit) and
  `raw.githubusercontent.com` (first start only, to read the guides before the clone exists).

## Steps

1. Install Claude desktop app on the lab PC and sign in with the owner's own account.
   Turn on Remote Control so that the orchestrator session can reach it.
2. Open a new session and paste the message from `PROMPT-PRIMO-AVVIO.md`. The session
   reads this file and `CLAUDE.md` from the repository on GitHub, then does the rest.
   The owner only approves the UAC prompts.
3. The session runs `setup-hwlab.ps1` once, from an elevated PowerShell. It checks the
   machine, installs the toolchain with winget, creates `C:\hwlab` and clones the
   repository into `C:\hwlab\work\webrtc-aes256-build`.
4. The session runs `hw-inventory.ps1` and `run-hevc-probe.ps1` and shows the owner
   the two summaries.
5. The session copies `CLAUDE.md` into its project folder and waits for requests.

By hand the same steps are, in an elevated PowerShell:

    powershell -NoProfile -ExecutionPolicy Bypass -File setup-hwlab.ps1

and then, in any PowerShell:

    cd C:\hwlab\work\webrtc-aes256-build\engine\tools\hwlab
    powershell -NoProfile -ExecutionPolicy Bypass -File hw-inventory.ps1
    powershell -NoProfile -ExecutionPolicy Bypass -File run-hevc-probe.ps1

`-ExecutionPolicy Bypass` applies to that one process only. It does not change the
execution policy of the machine.

Setup, the Visual Studio Build Tools download in particular, can take a long time.
Anything that may run longer than a couple of minutes (setup, a build, the probe) is
started in the background and followed through its log or JSON file. The local engine
build (`engine-local-build.ps1`, below) is not part of the first start: it needs the
owner's agreement to one more host.

## What is installed

All five packages come from winget, with exact ids and silent installs. `setup-hwlab.ps1`
verifies every id it is about to install with `winget show` before it installs anything.

| Package id | Used for |
| --- | --- |
| `Git.Git` | cloning and updating this repository |
| `Python.Python.3.12` | build and test helper scripts |
| `Kitware.CMake` | configuring the probe and the engine |
| `Ninja-build.Ninja` | fast engine builds |
| `Microsoft.VisualStudio.2022.BuildTools` | the MSVC compiler and the Windows SDK (C++ workload) |

The HEVC Video Extensions package from the Microsoft Store is deliberately not
installed. The probe has to see the machine as it is, because that is the situation of
a user who installs the application on a fresh Windows.

## Scripts

All scripts are Windows PowerShell 5.1 compatible, ASCII only, and keep their files
under `C:\hwlab`. CI parses every one of them with Windows PowerShell 5.1
(`hevc-probe.yml`, job `hwlab-scripts`). `hwlab-common.ps1` is not run by itself: the
other scripts dot-source it (scrubbing of names, the HEVC Video Extensions definition,
a logged process runner). `setup-hwlab.ps1` is downloaded and run on its own before the
repository exists, so it carries a word-for-word copy of the extensions definition; CI
compares the two copies.

### setup-hwlab.ps1

Run once, from an elevated PowerShell. It is idempotent: it skips whatever is already
present, and a second run is safe.

- Checks the machine: Windows 10 22H2 or Windows 11, x64, at least 8 GB of RAM (a
  little under 8 GiB is accepted because of memory reserved by the hardware) and at
  least 40 GB free on the system drive.
- Skips the packages that are already present. For the missing ones it checks that winget exists, then verifies each package id with `winget show` before installing any of them.
- Installs the missing packages with `--accept-package-agreements
  --accept-source-agreements --silent`. The Visual Studio Build Tools get the C++
  workload through the installer override
  `--quiet --wait --norestart --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended`.
  A Build Tools instance without the C++ workload is extended with the Visual Studio
  installer instead of being installed twice.
- Creates `C:\hwlab\work` and `C:\hwlab\reports`.
- Clones this repository into `C:\hwlab\work\webrtc-aes256-build`, or updates an
  existing clone with a fast-forward pull. It never resets or deletes a clone; a clone
  that has local changes or has diverged stops the script with a clear message.
- Prints a final summary and writes the same summary to
  `C:\hwlab\reports\setup-last.json` and the step log to
  `C:\hwlab\reports\setup-last.log`, so that a non-elevated session can read the
  result of an elevated run.

Exit code 0 means the lab is ready, 1 means a check failed and nothing was installed,
2 means an install or the clone failed. A restart may be asked for by the Visual Studio
installer; the summary says so.

### hw-inventory.ps1

Writes `C:\hwlab\reports\inventory-<date>.json` (and prints a short summary). It
contains:

- Operating system caption, version and build.
- CPU name, cores and threads, and RAM in GB.
- GPUs: name, driver version, adapter RAM and the PNP vendor id (four hex digits)
  only. WMI caps the adapter RAM at 4 GiB, so the script also reads the real
  dedicated memory from the display class registry key when it is there. The probe
  reports the DXGI figure too.
- Audio render and capture endpoints by friendly name, with the Windows default flags,
  read through the Core Audio API.
- Whether a Bluetooth radio is present and enabled, and the paired audio devices by
  friendly name (classic profiles A2DP, HFP and AVRCP; LE Audio devices are not
  recognized as audio).
- Cameras by friendly name.
- The HEVC Video Extensions, with one definition shared with `setup-hwlab.ps1` (see
  "The HEVC Video Extensions" below): the three facts, every registered package with HEVC in
  its name (known family or not), and, per package, the media codecs it declares in its
  manifest (`hevc_package_media_codecs`: category, and whether it takes or produces HEVC).

It does not read or write serial numbers, the machine name, the user name, MAC
addresses, IP addresses or paths that contain the user name.

### run-hevc-probe.ps1

Builds the probe in `engine/tools/hevc-probe` with CMake and the Visual Studio 2022
Build Tools (Release, static CRT, as its `CMakeLists.txt` sets) into
`C:\hwlab\work\build-hevc-probe`. It then runs the exe for HEVC and again with
`--codec h264`, saves both JSON reports in `C:\hwlab\reports` as
`hevc-probe-hevc-<stamp>.json` and `hevc-probe-h264-<stamp>.json`, and prints the
`summary` object of each. The H.264 run is the control: if H.264 passes and HEVC does
not, the machine lacks an HEVC path, not the probe. The probe has its own watchdogs
and can take a few minutes on a machine with several encoders.

Results are `ok`, `failed` or `not_attempted` (the reason is in `summary.reasons`). An
attempt that was not made, such as the 1080p decode when no 1080p bitstream exists, is
`not_attempted`, never `failed`. The fields, and what `dxva_decode_*` and
`hardware_accelerated_decode_status` mean, are in `engine/tools/hevc-probe/README.md`.
Besides the encode and decode tests the probe answers two questions about HEVC decoding
that the encode and decode tests could not:

- Is a decoder that is installed as a Store package returned to the probe at all? The
  summary has the number of decoder MFTs that `MFTEnumEx` returns by default, with
  `MFT_ENUM_FLAG_UNTRUSTED_STOREMFT`, and unfiltered (`hevc_decoder_mft_count_*`), the MFTs
  that only the store flag returns (`hevc_store_mft_decoders`), and whether one of them can
  be activated and decodes (`hevc_store_mft_activation_status`,
  `hevc_store_mft_decode_720p_status`, `..._1080p_status`). A refused activation is reported
  as such (`activation_refused`), not as a missing decoder.
- Can each GPU decode HEVC through D3D11 video decoding directly, without any MFT?
  `hevc_d3d11va_decode_supported` (and `..._by_adapter`, one entry per adapter, so the Intel
  and the NVIDIA adapter of a hybrid laptop are told apart; `hevc_d3d11va_main10_...` for
  10 bit) and the `d3d11va` section with the profiles each driver lists.
When a codec's 1080p hardware encode is not `ok` although its 720p one was, the script
runs the probe once more for 1080p alone (`--resolutions 1080`) in a fresh process,
saves it as `hevc-probe-<codec>-1080only-<stamp>.json` and writes
`hevc-probe-1080-verdict-<stamp>.json`: it says whether 1080p works in a fresh process
but not after the 720p attempts (leftover state), or fails in a fresh process too.

The script fails with a clear message when CMake, the MSVC tools or the Windows SDK
are missing. It does not fetch the repository; pass `-Update` to run a fast-forward
pull first, and `-Clean` to rebuild from scratch.

### engine-local-build.ps1

Builds the media engine against the pinned libwebrtc release and runs its call test, the
same steps as the `engine-webrtc` job of `.github/workflows/engine.yml`, on the lab PC.
No administrator rights. The steps:

1. Reads the pinned release (tag and sha256 of every file) from
   `engine/cmake/webrtc-release.cmake` and prints the hosts and URLs the download will
   contact. **Without `-AllowDownload` nothing is downloaded**: the script prints this
   plan and exits with code 10. A complete, verified download already in the fetch
   folder needs no download and no switch.
2. Finds Visual Studio with `vswhere` (any edition, Build Tools included, no hard-coded
   path), takes the build environment from its `vcvars64.bat`, finds `cmake` and `ninja`.
3. `cmake -P engine/cmake/fetch_webrtc.cmake` into `C:\hwlab\work\webrtc-fetch`: the
   release files and the Chromium clang package, each checked against its pinned sha256,
   then `engine/cmake/verify_pins.cmake` once more.
4. Configures (Ninja, the downloaded `toolchain.cmake`, `QMEDIA_WITH_WEBRTC=ON`,
   `QMEDIA_BUILD_TESTS=ON`) and builds `qaudion-media`, `qaudion-media-ci` and
   `qmedia_call_test` into `C:\hwlab\work\build-engine`.
5. Runs `qmedia_call_test.exe`: two engine processes and one full call over loopback.
   Windows Firewall may ask about `qaudion-media-ci.exe`; the test uses loopback only.
6. Writes `C:\hwlab\reports\engine-build-<stamp>.json` (ok, the step timings, the test
   result with its last output lines, the pins verified, the toolchain versions, the
   repository commit) and `engine-build-<stamp>.log`. Names and addresses are scrubbed
   with the same rules as the inventory.

Attestation: CI verifies the build provenance of `webrtc.lib` with `gh attestation
verify`. The lab has no `gh` and no token, by design, so locally that check is skipped,
and the report says so (`attestation.skipped`, with the reason). What applies locally is
the sha256 pin of every file, enforced by `fetch_webrtc.cmake` and again by
`verify_pins.cmake` at configure time.

How the lab session runs it. The download and the build take a long time, so the script
is started in the background and its log is followed, not waited for:

    # 1. The plan. Downloads nothing. -ResolveClangUrl fetches only build-flags.json from
    #    github.com (sha256 checked) so that the exact clang url is printed too.
    powershell -NoProfile -ExecutionPolicy Bypass -File engine-local-build.ps1 -PlanOnly -ResolveClangUrl

    # 2. Show the owner the hosts of the plan, commondatastorage.googleapis.com in
    #    particular. Only after the owner agrees in the current session:
    Start-Process powershell.exe -WindowStyle Hidden -ArgumentList '-NoProfile','-ExecutionPolicy','Bypass','-File','C:\hwlab\work\webrtc-aes256-build\engine\tools\hwlab\engine-local-build.ps1','-AllowDownload'

    # 3. Follow the log, then read the JSON.
    $log = Get-ChildItem C:\hwlab\reports\engine-build-*.log | Sort-Object LastWriteTime | Select-Object -Last 1
    Get-Content $log.FullName -Wait -Tail 40

CI runs this script end to end on a hosted runner (`.github/workflows/hwlab-engine-build.yml`:
download, verify, build, call test, report, and a second run that reuses the verified
download), so a change to it is tested before the lab PC ever runs it.

Exit codes: 0 built and tested, 1 toolchain or repository problem, 2 download failed,
3 configure failed, 4 build failed, 5 call test failed, 10 plan printed and nothing
downloaded. Options: `-PreflightOnly` (toolchain check only), `-Update` (fast-forward
the clone first), `-Clean` (delete the build folder first).

### audio-bench.ps1

The first real-audio bench of the engine. Two engine processes (the hardware bench build,
`qaudion-media-hw`: the real sound card like the production engine, plus a loopback flag) call each
other with the strict transport and the frame keys of `qmedia_call_test`. The first captures from a
real microphone, the second plays to real speakers or Bluetooth earbuds. The offer is `sendonly`, so
the capturing process opens no speakers and the playing process opens no microphone.

    .udio-bench.ps1 -Build                       # build into <WorkDir>uild-audiobench (no download)
    .udio-bench.ps1 -List                        # list the capture and render devices, opens nothing
    .udio-bench.ps1 -SelfCheck                   # list + session/certificate round trip, opens nothing
    .udio-bench.ps1 -Dry                         # the whole call with file devices, no hardware
    .udio-bench.ps1 -Real -Seconds 10            # the real test, system default devices
    .udio-bench.ps1 -Real -Capture 'Headset' -Render 'Headphones' -Seconds 15

Only `-Real` records and plays; run it when someone is at the machine to speak, and start it in the
background (it prints one line per second). The report is `<ReportDir>udio-bench-<stamp>.json`:
capture and playout levels in dBFS (per second, average, peak, fraction of active seconds), concealed
samples as the measure of glitches (the engine statistics do not expose sound card underruns), a
latency estimate (round trip plus jitter buffer; device and Bluetooth buffers are not included), the
DTLS 1.3 / AES-256 SRTP / fingerprint verdict, the endpoint format of each device and an overall
verdict (`pass`, `transport-ok-no-signal`, `fail`). A Bluetooth endpoint that only offers the
hands-free profile (16 kHz mono) is written to `observations` as a platform limit. Device names are
scrubbed like in the inventory report.

The code is outside the production executable: `qaudion-media-hw` (compile definition
`QMEDIA_HW_BENCH`) and `qmedia_audio_bench` are separate targets, and the engine CI job checks that
`qaudion-media.exe` contains none of their strings and still exits with code 2 for `--ci-*` and
`--hw-allow-loopback`.

## The HEVC Video Extensions

The three package families are `Microsoft.HEVCVideoExtension_8wekyb3d8bbwe`,
`Microsoft.HEVCVideoExtensions_8wekyb3d8bbwe` and
`Microsoft.HEVCVideoExtensionFirstParty_8wekyb3d8bbwe` (the one the lab PC has). The list is in
`hwlab-common.ps1` and `setup-hwlab.ps1` (word for word, compared by CI) and in the probe (its
list is compared with the scripts' by CI). The packages are registered per user, so three
different facts exist, and the scripts and the probe report them under their own names:

| Field | Meaning |
| --- | --- |
| `registered_current_user` | registered for the current user. **This is the value the summaries use** (`installed`), because it is what an application process of this user sees |
| `provisioned_system_image` | part of the system image and installed for users when they first sign in (`Get-AppxProvisionedPackage -Online`). Needs elevation; `null` means unknown |
| `registered_any_user` | registered for some account (`Get-AppxPackage -AllUsers`). Needs elevation; `null` means unknown. Registered for another account does not help the current user |

`null` always means "could not be read", never "no", in the scripts and in the probe (the probe
does not read `provisioned_system_image` and reports `null` with a note).

Whatever the family list says, every package registered for the current user whose name matches
`*HEVC*` is listed too (`hevc_named_packages_current_user`, with `in_known_family_list` and
`hevc_named_packages_not_in_family_list`), so that a new variant is seen the day it appears and
is not reported as "not installed" because nobody has added its name yet.
`hw-inventory.ps1` also reads each such package's manifest and reports the media codecs it
declares (`hevc_package_media_codecs`). That matters because Media Foundation can only return a
Store codec that its package declares; "registered" and "declares a video decoder for HEVC" are
different facts.
`setup-hwlab.ps1`, `hw-inventory.ps1` and the probe agree on this. An earlier elevated
`-AllUsers` query reported "installed" for a machine on which the probe, running as the
current user, saw nothing: the two had asked different questions.

## Reports

Everything the lab produces goes to `C:\hwlab\reports`. The lab session sends the
relevant JSON or a summary of it back to the session that asked, and deletes large
files (build trees, downloaded packages) when it is told to.
