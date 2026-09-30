# webrtc-aes256-build

CI to build the LiveKit/webrtc-sdk fork of libwebrtc (M144) with an AES-256-GCM
FrameCryptor patch → Android AAR.

- `aes256-framecryptor.patch` — single hunk: `SetKeyFromMaterial` derives the
  per-frame key to 256 bits when the shared key material is 32 bytes (the AEAD
  already picks AES-256-GCM by key size). 16-byte material keeps AES-128. No
  Java/JNI API change — the app selects the cipher by the key length it passes
  to `setSharedKey()`.
- `.github/workflows/build.yml` — free `ubuntu-latest`, disk reclaimed via
  `maximize-build-space`, 12 GB swap for the link step. Run via **Actions →
  build-webrtc-aes256-aar → Run workflow**. Artifact: `libwebrtc-aes256-aar`.

Public repo → unlimited Actions minutes. Build ~3-4 h (single ABI, but see this
repo's git log — the first working `build.yml` took ~15 CI-fix iterations to
stabilize; live runs have since landed well under that in ~26-32min). Upstream
context: livekit/client-sdk-android#952.

## LiveKit (group-call SFU) variant — `build-livekit-android.yml`

The plain `libwebrtc-aes256.aar` above is `org.webrtc`-packaged and already
covers 1:1 calls (both mobile apps consume it directly). It is **not**
consumable by `io.livekit:livekit-android`, which transitively pulls
`io.github.webrtc-sdk:android-prefixed` — a build of the SAME upstream
(`webrtc-sdk/webrtc@m144_release`) that is package-relocated to
`livekit.org.webrtc` and native-lib-renamed to
`liblkjingle_peerconnection_so.so`, specifically so it can coexist with a
plain `org.webrtc` build in the same app (confirmed live, see
qaudion-android-new `W-LIVEKIT-DUALWEBRTC`). That relocation is what makes the
group-call (LiveKit SFU) media path today hard-locked to **AES-128**: LiveKit's
own `BaseKeyProvider` has no `keySize` knob — the native FrameCryptor always
derives a fixed-length key from PBKDF2 output.

`build-livekit-android.yml` builds a drop-in replacement for that artifact
with the SAME `aes256-framecryptor.patch` applied, so a future 32-byte
`setSharedKey()` call gets AES-256-GCM on the group-call path too, matching
1:1 calls. It additionally applies `jni_prefix.patch` (vendored verbatim from
`webrtc-sdk/webrtc-build`, MIT license — the exact patch their own
`android_prefixed` GN target uses) and then reproduces
`webrtc-sdk/android`'s own post-build repackage step
(`android-prefixed/shadow/build.gradle`'s Shadow-plugin class relocation +
the `.so` rename done by their `downloadAar_prefixed.sh`/`prefixmove.sh`) via
`shadow-relocate/` in this repo. Artifacts:
`libwebrtc-aes256-livekit-raw-aar` (diagnostic, still `org.webrtc`) and
`libwebrtc-aes256-livekit-prefixed-aar` (the drop-in replacement).

**This is new and UNTESTED as of authoring.** Two real, unverified risks,
flagged rather than hidden:
1. `jni_prefix.patch` was captured from `webrtc-sdk/webrtc-build`'s pipeline,
   not re-verified against `m144_release` specifically — `git apply` may need
   a context-line refresh.
2. The `shadow-relocate/` Gradle step has never run in this CI before —
   Shadow 7.1.2 is pinned to Gradle 7.6 in the workflow to match
   `webrtc-sdk/android`'s own known-working combo, but this is the first live
   run.

Consuming this artifact in `qaudion-android-new` (Gradle dependency
substitution of `io.github.webrtc-sdk:android-prefixed` → this local AAR) is
tracked separately — not done by this workflow.

## no-key-log.patch (SECURITY)

Upstream `webrtc-sdk/webrtc` (m144_release .. m150_release) logs the frame-cryptor
input secret, the salt and the DERIVED AES key as decimal byte lists at
`RTC_LOG(LS_INFO)` (`api/crypto/frame_crypto_transformer.cc`,
`DeriveHkdfSha256FromSecret` and `DerivePBKDF2KeyFromRawKey`). Any app that
enables WebRTC INFO logging leaks the per-call E2EE key into its log capture.

`no-key-log.patch` deletes both statements. It is written for the m144 line and
applies to every m144 ref built here (`m150_release` has the same statements in
a different call form and needs a rebased patch). Every workflow applies it
(after `aes256-framecryptor.patch` / `native-pli.patch`) and asserts the source
no longer contains the statements. After the build, `ci/assert-no-key-strings.sh`
scans EVERY produced binary (each Mach-O slice of the xcframeworks, each
`jni/<abi>/*.so` of the AARs; the final renamed AAR too) for `derived_key`,
`slat << ` (upstream's own typo of "salt") and `raw_key`, and fails the job,
failing closed on a missing/tiny binary or a missing positive control.
`ci/test-assert-no-key-strings.sh` is its self-test. Do not drop this patch when
rebasing on a newer upstream ref; re-check with
`grep -n "derived_key " api/crypto/frame_crypto_transformer.cc`.

## Source pins and release tags

The workflows fetch `sigarone/webrtc` by a TAG (GitHub rejects a shallow fetch of
a bare SHA), never a rolling branch, so a rebuild differs from the shipped
binary only by the patches in this repo:

| workflow | default `webrtc_ref` | upstream commit |
| --- | --- | --- |
| `build-ios.yml`, `build.yml` | `df1011beabae-livekit-aes256-7559.14` | `webrtc-sdk/webrtc@df1011beabae` (m144.7559.14) |
| `build-livekit-android.yml` | `df1011beabae-livekit-aes256-7559.14` | same |
| `build-livekit-ios.yml` | `f47af7bc9658-livekit-aes256-7559.10` | `webrtc-sdk/webrtc@f47af7bc9658` (m144.7559.10) |

The two iOS release workflows refuse to overwrite an existing release asset
(earlier releases are rollback binaries the app pins by checksum): every build
needs a NEW `release_tag`, and the tag is created at the built commit.

## qjanus (group-call SFU node) — `qjanus/`, `.github/workflows/qjanus.yml`

`qjanus/` builds the patched Janus VideoRoom SFU of the group calls v2 for Ubuntu 24.04 x86_64 from pinned
sources (Janus v1.4.2, BoringSSL `f91f1447`, libsrtp 2.8.1 on the BoringSSL backend, libnice 0.1.24):
DTLS 1.3 + X25519MLKEM768 + TLS_AES_256_GCM_SHA384 + SRTP AEAD_AES_256_GCM only, fail closed. The workflow builds
a relocatable tarball, installs it under systemd like a node, and runs an API conformance suite, headless
Chromium end-to-end tests (E2EE frames, simulcast, netem loss) and negative tests. Node install, upgrade and
uninstall: `qjanus/README.md`. A release is created only by running the workflow with `release_tag=qjanus-<version>`.
