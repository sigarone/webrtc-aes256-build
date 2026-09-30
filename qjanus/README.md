# qjanus

Janus VideoRoom SFU for the Q-Audion group calls v2 (spec `GROUP_CALLS_V2`, section 6):
one Janus binary that terminates exactly the transport level of the 1:1 calls and nothing weaker.

- DTLS 1.3 only, `X25519MLKEM768` (0x11EC) required, `TLS_AES_256_GCM_SHA384` only, SRTP `AEAD_AES_256_GCM` only.
  Checked after every handshake; anything else gets alert 71 and is hung up at once (fail closed).
- Built from pinned sources (`build/pins.env`): Janus v1.4.2, BoringSSL `f91f1447`, libsrtp 2.8.1 on the
  BoringSSL backend, libnice 0.1.24 (GnuTLS), libwebsockets (no TLS library). The only TLS/crypto code in the
  process is the static BoringSSL; the build fails if a system `libssl`/`libcrypto` is linked.
- Only the VideoRoom plugin and the HTTP + WebSockets transports. No data channels, no other plugins or
  transports, no event handlers, no recordings post-processing.
- Content is end-to-end encrypted by the clients; qjanus forwards SRTP and never sees keys or plaintext.

Work in progress on branch `feat/group-calls-v2`; sections below are completed with the release.
