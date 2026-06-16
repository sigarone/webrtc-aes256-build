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

Public repo → unlimited Actions minutes. Build ~3-4 h (single ABI). Upstream
context: livekit/client-sdk-android#952.
