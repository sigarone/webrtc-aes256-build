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

`no-key-log.patch` deletes both statements. Every workflow applies it (after
`aes256-framecryptor.patch` / `native-pli.patch`), asserts the source no longer
contains the statements, and after the build asserts that the binary contains
no `slat << ` string (upstream's own typo of "salt", unique to those
statements). Do not drop this patch when rebasing on a newer upstream ref; re-check
with `grep -n "derived_key " api/crypto/frame_crypto_transformer.cc`.
