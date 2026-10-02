# m150 pipeline (M150, DTLS 1.3 + AES-256, deep PLC, FEC 10%)

Companion to `webrtc-plan.md` v2 (sections 2, 4). This directory + the
`build-m150-*.yml` / `test-m150-patches.yml` workflows are the **PIPELINE
AUTHOR** deliverable. The M144 files at the repo root (`build.yml`,
`build-ios.yml`, `build-livekit-android.yml`, `build-livekit-ios.yml`,
`aes256-framecryptor.patch`, `jni_prefix.patch`, `apple-prefix-livekit.patch`,
`apple/xcframework-livekit-ios.sh`, `shadow-relocate/`) are **untouched** and
remain the rollback path.

## Ownership (do not cross - see webrtc-plan.md's "SHARED CONTRACT")

| Owns | SOURCE-PATCH author | PIPELINE author (this dir) |
|---|---|---|
| `m150/series` | yes | read-only |
| `m150/patches/*` | yes | read-only |
| `.github/workflows/*`, `m150/*.sh`, `m150/ci/*`, `m150/kat/*`, `m150/README.md` | read-only | yes |

## Layout

```
m150/series                 <- SOURCE-PATCH author's apply order (does not exist yet)
m150/patches/P1..P12-*.patch <- SOURCE-PATCH author's patches (P12 = frame anti-replay window)
m150/apply-series.sh        <- reads series and applies it
m150/fetch-opus-dnn-weights.sh
m150/gates.sh                <- G1-G9, see webrtc-plan.md §2.5
m150/buildinfo.py
m150/ci/sync.sh              <- pinned depot_tools + gclient sync + hard pin verification
m150/ci/package-android.sh   <- plain passthrough + G1 spot-check
m150/ci/kat2inc.py          <- shared frame-crypto KAT (sha256-pinned) -> api/crypto/frame_crypto_kat_vectors.inc (T7)
m150/kat/group-calls-v2-frame-crypto.json <- the KAT file (byte-identical copy; desktop, Android and iOS pin the same hash)
m150/ci/apply-tests.py       <- adds the Q-Audion unit tests (T6, T11) to a patched checkout (not part of the shipped series)
m150/tests/                  <- T6 (qaudion_tuning_unittest.cc) and T11 (ssl_stream_adapter_strict_tests.inc)
m150/ci/package-windows.py   <- Windows: webrtc.lib, libcxx.lib, webrtc-headers.zip (public headers + libc++ headers)
m150/ci/collect-build-flags.sh, gen-build-flags.py <- Windows: build-flags.json from the real GN build
m150/smoke/consumer.cc, m150/ci/link-smoke.py <- Windows: out-of-tree consumer compiled and linked from the release files only
m150/ios/reclaim-disk.sh
m150/ios/check-dsym-uuids.sh
```

## Pins (all resolved 2026-09-28, from webrtc-sdk/webrtc's own DEPS at the
commit below - re-derive with the commands in "Re-deriving the pins" if the
source author ever moves `webrtc_ref`)

| What | Value |
|---|---|
| `webrtc-sdk/webrtc` (m150_release) | `ba469aa2093ba950066258ca0a59a6fbd1295582` |
| `third_party/boringssl/src` | `f91f1447397c6719f9774dfb8e67329378e1f3d3` |
| `third_party/opus/src` | `55513e81d8f606bd75d0ff773d2144e5f2a732f5` |
| Opus DNN weights tarball | `https://media.xiph.org/opus/models/opus_data-160753e983198f29f1aae67c54caa0e30bd90f1ce916a52f15bdad2df8e35e58.tar.gz` |
| Opus DNN weights sha256 | `160753e983198f29f1aae67c54caa0e30bd90f1ce916a52f15bdad2df8e35e58` (verified live 2026-09-28 by downloading the tarball into a local scratch directory and running `sha256sum` - exact match; the hash is also the tarball's own file-name suffix, i.e. self-describing per upstream `dnn/download_model.sh`) |
| depot_tools | **NOT YET PINNED** - see "Open items" |

These three constants (webrtc/boringssl/opus SHAs) are duplicated in
`m150/ci/sync.sh` and `m150/buildinfo.py` (one shell, one Python - no shared
include mechanism across both without adding a build-time dependency). If the
source author re-pins `webrtc_ref`, update all three together plus this
table.

### Re-deriving the pins
```sh
curl -fsSL https://raw.githubusercontent.com/webrtc-sdk/webrtc/<new_ref>/DEPS | grep boringssl
# 'src/third_party' points at a Chromium googlesource split-repo that has no
# GitHub mirror this environment could reach (consistently 503 from this
# sandbox on 2026-09-28 - retry from a host with real internet access, or
# read scratchpad/webrtc-latest.md / m150-dtls.md, which resolved these two
# the same way during the original research pass and record BOTH the boringssl
# and opus SHAs already, cross-checked against a live api.github.com lookup).
```

## Build variants

| Variant | GN arg | Extra patch |
|---|---|---|
| `android` (plain) | `rtc_qaudion_transport_strict=true` | none |
| `ios` (plain) | `rtc_qaudion_transport_strict=true` | none |
| `windows` (plain, x64) | `rtc_qaudion_transport_strict=true` | none |

The LiveKit-prefixed `-lk` variants (and their prefixing patches) are gone:
the apps no longer use LiveKit. P1-P12 (from `m150/series`) apply identically
to every variant. `apply-series.sh` enforces this; it is NOT encoded per-line
in `series` itself. (`test-webrtc-m150-patches` still builds a `switchable`
config next to `strict`, because that is the only place the upstream-behaviour
T1/T2 tests can run.)

## Dispatching a build

1. The repo owner must have already run Appendix B of `webrtc-plan.md`
   (`gh repo sync sigarone/webrtc -b m150_release` + create the
   `ba469aa2093b-qaudion-m150` tag) - **this pipeline cannot do that itself**
   (no push access from CI, and this session had none either - see "What the
   orchestrator must do publicly" below).
2. Actions -> `build-webrtc-android-m150-hardened` (or `-ios-`) -> Run
   workflow, fill in `release_suffix` (e.g. `a256-dplc-1`).
3. The build publishes to `webrtc-android-m150-<suffix>` (iOS:
   `webrtc-ios-m150-<suffix>`).

### Windows release contents and the consumer contract

`build-webrtc-windows-m150-hardened` publishes `webrtc.lib`, `libcxx.lib`,
`webrtc-headers.zip`, `build-flags.json`, `BUILDINFO.json`, `PINS.txt` and
`SHA256SUMS` (lib, libcxx.lib, headers, build-flags.json and SHA256SUMS carry a
build-provenance attestation). The library is built by Chromium's clang-cl
against Chromium's libc++ (`std::__Cr`) with the static CRT (`/MT`); webrtc.lib
does not carry the libc++ runtime objects, which is why `libcxx.lib` ships
next to it, and the zip carries Chromium's libc++ headers. `build-flags.json`
is generated from the real GN build (defines, include dirs, ABI flags, C++
standard, STL and CRT mode, RTTI, exceptions, toolchain and SDK versions,
system libs, the exact compiler package and its sha256). The `link-smoke` job
builds `m150/smoke/consumer.cc` with nothing but those files and runs it: two
PeerConnections in one process, loopback, audio track, DTLS 1.3 /
TLS_AES_256_GCM_SHA384 / AEAD_AES_256_GCM / X25519MLKEM768. The release is
published only if that job is green. The group id is read from
`DtlsTransportInformation::ssl_group_id()`: getStats has no field for it.

PDBs: the library is built with `symbol_level=0`; a separate PDB artifact would
need a second build with symbols and is not produced (not cheap, not required
for S0).

`test-webrtc-m150-patches` (ubuntu, free) should be green first - it applies
the same series against a host x64 build in both GN configs and runs T1-T11
(T6: tuning API and marker; T7-T9: P12 - shared KAT through the real
FrameCryptorTransformer, replay window, upstream FrameCryptor gtests; T10: P9
certificate stats cache; T11: strict-transport negative tests, strict config
only).

## Open items (for the orchestrator / source-patch author, not done here)

1. **depot_tools pin (finding #18).** Done: `m150/ci/sync.sh` hardcodes
   `DEPOT_TOOLS_SHA=a07c06fe67a1a9d64ac4728df3a11c1ceb0cf73e` (main,
   2026-09-28) with no Actions-variable override, and runs depot_tools'
   own `ensure_bootstrap` so the hermetic python3 exists even with
   `DEPOT_TOOLS_UPDATE=0`.
2. **T1-T6 gtest filters** are set in `test-m150-patches.yml` (closest
   existing upstream coverage; T6 still has no test). T1/T2 are
   upstream-behaviour tests and run only in the switchable config; a filter
   that matches zero tests now fails the job instead of passing silently.
3. **G4 detection method is unverified against a real artifact.** No M150
   binary exists yet to test `nm`/raw-byte-scan symbol detection against; see
   the long comment in `gates.sh` for the fallback plan if `symbol_level=1`
   turns out to strip the weight-array names entirely.
4. **G7 size baselines** exist only for `android-plain` (13,648,040 B, this
   repo's own shipped M144 artifact). `ios-plain` has no M144 build in this
   workspace to diff against, so G7 is informational-only for it until a real
   baseline is pinned.
5. **Weights tarball resilience.** `media.xiph.org` is the only source right
   now (matches upstream `dnn/download_model.sh` itself). The plan mentions
   "mirrored later as a release asset" - that is a publish action, out of
   this session's scope; consider a `webrtc-weights-mirror` release in this
   repo hosting the sha256-pinned tarball as a fallback URL.
6. **`.github/workflows/build-m150-*.yml`'s `runs-on: macos-26`** was
   confirmed to exist and to ship Xcode 26.5 (default is 26.6, both satisfy
   the M150 SDK floor) via `actions/runner-images`' README + the
   `macos-26-arm64` image README, both fetched live 2026-09-28. Re-check
   before a real dispatch in case the default image has moved on.

## What the orchestrator must do publicly (this session did NOT do any of this)

- Run Appendix B (`gh repo sync sigarone/webrtc -b m150_release` +
  create tag `ba469aa2093b-qaudion-m150` via the Git Data API) on the real
  `sigarone/webrtc` fork.
- Push this branch (`m150-ci`) and open/merge whatever PR review process is
  normally used, then dispatch `test-webrtc-m150-patches` first.
- Confirm `sigarone/webrtc-aes256-build` repo settings match what these
  workflows assume: Actions enabled, `workflow_dispatch` allowed, default
  `GITHUB_TOKEN` permissions are fine to leave as read-only at the repo level
  (each job sets its own `permissions:` block explicitly either way).
