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
| `.github/workflows/*`, `m150/*.sh`, `m150/lk/*`, `m150/README.md` | read-only | yes |

## Layout

```
m150/series                 <- SOURCE-PATCH author's apply order (does not exist yet)
m150/patches/P1..P8-*.patch <- SOURCE-PATCH author's patches (P1-P3 exist; P4-P8 in progress)
m150/apply-series.sh        <- reads series, applies it, then the LK patch below if requested
m150/fetch-opus-dnn-weights.sh
m150/gates.sh                <- G1-G9, see webrtc-plan.md §2.5
m150/buildinfo.py
m150/ci/sync.sh              <- pinned depot_tools + gclient sync + hard pin verification
m150/ci/package-android.sh   <- plain passthrough / lk shadow-relocate+rename
m150/ios/reclaim-disk.sh
m150/ios/check-dsym-uuids.sh
m150/lk/jni_prefix.patch     <- vendored verbatim from webrtc-sdk/webrtc-build@66ed9c7 (MIT)
m150/lk/apple_prefix.patch   <- idem
m150/lk/xcframework.sh       <- idem (build/apple/xcframework.sh)
m150/lk/LICENSE.webrtc-build <- idem (LICENSE)
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
| `webrtc-sdk/webrtc-build` (for `m150/lk/*`) | `66ed9c7` (as given by the plan; vendored files match) |
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

## Build variants and how they differ

| Variant | GN arg | Extra patch |
|---|---|---|
| `android` (plain) | `rtc_qaudion_transport_strict=true` | none |
| `android-lk` | `rtc_qaudion_transport_strict=false` | `m150/lk/jni_prefix.patch` + shadow-relocate/rename |
| `ios` (plain) | `rtc_qaudion_transport_strict=true` | none |
| `ios-lk` | `rtc_qaudion_transport_strict=false` | `m150/lk/apple_prefix.patch` (via `xcframework.sh ... LiveKit`) |

P1-P8 (from `m150/series`) apply identically to all four variants - only the
GN strict flag and the LK-only prefixing patch differ. `apply-series.sh`
enforces this split; it is NOT encoded per-line in `series` itself.

## Dispatching a build

1. The repo owner must have already run Appendix B of `webrtc-plan.md`
   (`gh repo sync sigarone/webrtc -b m150_release` + create the
   `ba469aa2093b-qaudion-m150` tag) - **this pipeline cannot do that itself**
   (no push access from CI, and this session had none either - see "What the
   orchestrator must do publicly" below).
2. Actions -> `build-webrtc-android-m150-hardened` (or `-ios-`) -> Run
   workflow, fill in `release_suffix` (e.g. `a256-dplc-1`).
3. Both `plain` and `lk` variants build in parallel (matrix), then publish to
   `webrtc-android-m150-<suffix>` and `webrtc-android-lk-m150-<suffix>`
   (iOS: `webrtc-ios-m150-<suffix>` / `webrtc-ios-lk-m150-<suffix>`).

`test-webrtc-m150-patches` (ubuntu, free) should be green first - it applies
the same series against a host x64 build in both GN configs and runs T1-T6.

## Open items (for the orchestrator / source-patch author, not done here)

1. **depot_tools pin (finding #18).** `m150/ci/sync.sh` REQUIRES
   `DEPOT_TOOLS_SHA` (env `M150_DEPOT_TOOLS_SHA` as an Actions variable) and
   fails closed if it is unset - by design, rather than silently cloning
   `HEAD` like the M144 workflows do. Resolve once with
   `git ls-remote https://chromium.googlesource.com/chromium/tools/depot_tools.git HEAD`
   (a commit contemporary with the m150 cut, ~2026-05) from a host that can
   reach `*.googlesource.com` - this sandbox could not (503 on every
   googlesource endpoint tried, GitHub raw/api worked fine throughout).
2. **T1-T6 gtest filters** in `test-m150-patches.yml` are empty placeholders:
   the tests land as part of P4-P8, which are still being written in
   parallel (SOURCE-PATCH author's side, out of this script's ownership).
3. **G4 detection method is unverified against a real artifact.** No M150
   binary exists yet to test `nm`/raw-byte-scan symbol detection against; see
   the long comment in `gates.sh` for the fallback plan if `symbol_level=1`
   turns out to strip the weight-array names entirely.
4. **G7 size baselines** exist only for `android-plain` (13,648,040 B, this
   repo's own shipped M144 artifact). `android-lk`/`ios-plain`/`ios-lk` have
   no M144 build in this workspace to diff against, so G7 is
   informational-only for those three until a real baseline is pinned.
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
- Set the `M150_DEPOT_TOOLS_SHA` repository/organization Actions variable
  once item 1 above is resolved.
- Push this branch (`m150-ci`) and open/merge whatever PR review process is
  normally used, then dispatch `test-webrtc-m150-patches` first.
- Confirm `sigarone/webrtc-aes256-build` repo settings match what these
  workflows assume: Actions enabled, `workflow_dispatch` allowed, default
  `GITHUB_TOKEN` permissions are fine to leave as read-only at the repo level
  (each job sets its own `permissions:` block explicitly either way).
