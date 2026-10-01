# Media engine

This directory holds the generic media engine process of the desktop client, `qaudion-media.exe`,
and the IPC library that its host application uses to talk to it. It lives in the public build
repository next to the libwebrtc build so that library and engine are built, attested and
published by the same pipeline.

Status: the IPC layer (framing, deterministic CBOR codec, schema validator, session handshake,
Windows named-pipe transport, tests, fuzzers, CI) is complete. The engine executable currently
links only that layer: it answers `ping` and `shutdown` and replies `err unsupported` to every
other command. Linking the published libwebrtc is the next step (see "Linking libwebrtc").

## Scope

The engine is a generic media engine. It knows how to create peer connections, produce and apply
SDP, run ICE, encrypt and decrypt media frames with keys it is handed, capture and play audio,
capture camera and screen, and report statistics. It does not know any messaging protocol, user
or device identity, account, server name or address, and it never holds an identity or long-term
key. Everything that gives those operations meaning (who to call, which keys to install, whether a
remote fingerprint is acceptable) lives in the host process and arrives here as plain commands.

Rules that follow from this scope and that review should enforce:

- No hostnames, URLs, IP addresses, ids or tokens in the source, tests, fixtures or logs. ICE
  server URLs and credentials reach the engine only inside `pc_create`, from the host.
- No code that interprets application messages. The engine's only inputs are the IPC commands in
  `ipc/schema.cddl` and the network traffic of the peer connections it creates.
- No third-party application names anywhere in this directory.

## Process model

Two processes cooperate on one machine:

1. The host: a JVM process (Compose Desktop) that owns identity, ratchet and long-term keys,
   storage, the signalling network connection, the call state machine and the UI.
2. `qaudion-media.exe`: a native C++ process on libwebrtc M150 with the transport-strict patch
   series. It holds only per-call ephemeral material: the DTLS certificate of the call and the
   frame keys the host installs. It is started as a child of the host.

The host starts the engine as a full-trust child with a Job Object (kill on close), a restricted
token and process mitigations that stay compatible with audio and camera drivers. These are
host-side responsibilities and are not part of this lot; the engine does not depend on them to be
safe against a hostile pipe peer, because it authenticates its peer itself (below).

Start-up contract:

```
qaudion-media.exe --pipe \\.\pipe\<name> [--expect-client-pid <pid>]
```

1. The host generates 32 random bytes (the session nonce) and writes exactly those 32 bytes to the
   engine's standard input, which must be an anonymous pipe or a file. A console, the NUL device
   or any other handle is refused. The engine reads the nonce within 5 seconds.
2. The engine creates the named pipe (one instance only, see below) and waits 15 seconds for one
   client. The host should pick a fresh random pipe name for every start (128 random bits). With `--expect-client-pid` it also checks the client process id with
   `GetNamedPipeClientProcessId`.
3. Before sending the hello, the host must check that the process at the other end of the pipe is
   the child it started (`GetNamedPipeServerProcessId` equals the child's process id; the helper
   `ConnectPipe` does this when given the id). Another process of the same user could have
   created a pipe with that name first, and the hello carries the nonce. The engine in turn
   refuses to start if the name is already taken (exit code 4) and, with `--expect-client-pid`,
   refuses a client that is not the host.
4. The first message of the client must be `hello` carrying the same nonce. The engine compares it
   in constant time. On any failure before a valid hello the engine closes the connection without
   sending a single byte and exits.
5. Messages are then exchanged until the client sends `shutdown` or closes the pipe. The engine
   exits when the connection ends: it serves exactly one client for exactly one lifetime.

Exit codes are the only diagnostics the engine emits before the core is linked: 0 clean shutdown,
2 bad command line, 3 nonce not received, 4 pipe creation failed, 5 no client in time, 6 client
process id mismatch, 7 hello failed (wrong nonce or not a hello), 8 protocol violation after the
hello, 9 I/O error.

Video frames never travel over this channel. How decoded video reaches the UI (shared memory ring
for 1:1, a child window or a D3D11 shared handle for group grids) is decided in the video lot and
uses its own transport. The IPC carries control, SDP, ICE, keys, events, statistics and PCM taps
only, which is why a frame is capped at 1 MiB.

## Security rules

These apply to everything under this directory, including the future engine core.

Keys.
- A key travels over IPC only in `install_key`. It never appears in a reply, an event, an error
  detail or a log.
- The receive buffer is zeroed after every message and when the session ends, on every exit path
  (`FrameBuffer::Wipe`, also run by its destructor). Decoded byte strings are views into that
  buffer, so the decoder never makes a second copy of a key.
- Code that keeps a key (the frame key provider) copies it once into locked, non-pageable memory,
  zeroes it with `SecureZero` when the slot is retired or the session ends, and never copies it
  into logs, exceptions, crash reports or telemetry. `retire_slot` fills the slot with random
  bytes, never with zeros. An all-zero key in `install_key` is rejected by the schema.
- A crash must not write key material to disk. The process sets `SEM_NOGPFAULTERRORBOX` at start
  and the host must not enable Windows Error Reporting LocalDumps for `qaudion-media.exe`.

Logging.
- The engine never logs keys, SDP, ICE candidates, ICE credentials, certificate fingerprints, IP
  addresses, device identifiers or participant ids. Diagnostics are verdicts: a static error name
  (`Err` in `status.h`, `err.detail`) that is chosen by the engine and never copied from input.

Transport policy.
- DTLS 1.3 only, `TLS_AES_256_GCM_SHA384` only, `X25519MLKEM768` only, SRTP `AEAD_AES_256_GCM`
  only, no cleartext RTP. This is compiled into libwebrtc (`rtc_qaudion_transport_strict`) and is
  not configurable at runtime: no IPC message, field, flag or environment variable relaxes it.
  The schema rejects unknown fields, so a hypothetical "allow weaker cipher" option cannot be
  smuggled into `pc_create`. If a peer connection ends up on anything else, the engine closes it
  and reports `transport_violation`; the host additionally checks the `transport_info` event.

IPC hardening.
- Unknown message kinds, unknown fields, missing fields, wrong types, out-of-range values, wrong
  direction and a repeated `hello` all end the connection. There is no lenient mode.
- The decoder accepts only canonical CBOR, bounds depth (6) and the number of data items (4096),
  rejects huge length claims from the header alone, and validates UTF-8 strictly. The validator
  additionally refuses control characters (NUL, C0, DEL) in every text field, so an identifier
  can neither truncate a C string nor smuggle a line break; only SDP may contain CR, LF and HT.
- Before the nonce has been checked the peer is unauthenticated, so its first frame may be 256
  bytes at most (a hello is about 60). A larger header ends the handshake after 4 bytes, with
  nothing allocated and nothing sent. The full 1 MiB frame size applies only after a good hello.
- Timeouts: the first byte of the hello must arrive within 5 seconds, and once any frame has
  started its remaining bytes must follow within 10 seconds, so a stalled peer cannot hold the
  reader. Between messages the engine waits without limit for the host.
- The pipe has a protected DACL with one allow entry for the current user SID (no Everyone,
  Network, Anonymous, Users, Administrators or System), `PIPE_REJECT_REMOTE_CLIENTS`,
  `FILE_FLAG_FIRST_PIPE_INSTANCE` and a maximum of one instance. The DACL is read back after
  creation and the pipe is refused if it is not exactly that. The access mask is the generic read
  and write set, which includes the right to create a pipe instance; the instance limit of one is
  what prevents a second instance, so do not raise it.
- The client side opens the pipe with identification-level impersonation only.
- At start the process restricts DLL loading to its own directory and System32, turns on heap
  termination on corruption and suppresses the fault dialog.

## IPC protocol

`ipc/schema.cddl` is the contract. Every message is a CBOR map with `v` (always 1), `t` (the kind)
and `id` (request id). Requests carry an id of at least 1 and the reply echoes it. Events carry id
0. Keys of a map are sorted and unique. The framing is a 4-byte big-endian length followed by one
CBOR item, 1 byte to 1 MiB.

Command groups, all defined in the schema:

- Session: `session_create`, `session_close`. A session owns certificates, peer connections and the
  frame key provider with its replay state, so a peer connection can be rebuilt without losing
  the keys or the replay windows.
- Certificates and peer connections: `cert_create` returns a handle and the SHA-256 fingerprint;
  `pc_create` takes a session and a certificate handle; several peer connections per session
  (1:1 uses one, group calls use a publisher and a subscriber).
- Negotiation: `create_offer`, `create_answer`, `set_local_description`, `set_remote_description`,
  `add_ice_candidate`, `restart_ice`, and the events `ice_candidate`, `ice_gathering_state`,
  `ice_connection_state`, `pc_state`, `negotiation_needed`.
- Frame keys: `install_key` (participant, slot 0 to 15, 32-byte key, direction send or recv),
  `retire_slot`, `select_send_slot`, `bind_media` (ties a media section to a participant and a
  direction), and the `cryptor_state` event.
- Transport: the `transport_info` event (TLS version, DTLS cipher, group, SRTP cipher and the
  fingerprint of the remote certificate that was actually negotiated) and `transport_violation`.
- Media: `set_muted`, `list_devices`, `select_device`, `devices_changed`, `start_video` and
  `stop_video` (camera or screen), `request_keyframe`, `set_simulcast`, `pcm_tap` with `pcm_frame`
  events, `get_stats`, `set_audio_tuning`.
- Errors: `err` with a code from a closed set and an optional static detail.

The CDDL file and the validator table in `ipc/src/schema.cpp` are kept identical by a unit test
that parses the CDDL (kinds, directions, id rules, field names, optionality, ranges, enums,
sizes). Change both together.

Why the codec is hand written. The accepted CBOR subset is small and strict: unsigned integers,
byte and text strings, arrays, maps with text keys, booleans and null, definite lengths in
shortest form, sorted unique keys. General CBOR libraries accept a much larger language and leave
strictness to the caller. Using one would add a dependency to fetch, pin and audit in a repository
whose purpose is a clean supply chain, and the strictness we need would still have to be written
on top. The decoder here is about 250 lines, has no dependency, is zero-copy (needed for the key
wipe), and is covered three ways: exhaustive unit tests of every rejected form, a canonical
round-trip property (an accepted input must re-encode to the same bytes, which catches any
leniency), and libFuzzer with ASan and UBSan.

## Layout

```
engine/
  CMakeLists.txt        project root (IPC library, tests, optional fuzzers, qaudion-media on Windows)
  cmake/webrtc.cmake    hook for linking the published libwebrtc
  src/main.cpp          engine entry point
  ipc/
    schema.cddl         the contract
    include/qmedia/ipc  public headers (limits.h lists every bound in one place)
    src                 cbor, frame, schema, session, secure, status, message, pipe (Windows)
    tests               unit tests, pipe tests (Windows), corpus generator
    fuzz                libFuzzer targets and the committed seed corpus
```

## Building and testing

CI (`.github/workflows/engine.yml`, filtered on `engine/**`) runs three jobs: unit tests on Linux
with clang under ASan and UBSan and with gcc in Release, both with warnings as errors; two
libFuzzer targets for a fixed time with the committed seed corpus; and the Windows build with
MSVC that runs the unit tests and the pipe tests against the real `qaudion-media.exe`.

Local use on a machine with CMake and a C++20 compiler:

```
cmake -S engine -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug -DQMEDIA_SANITIZE=address,undefined
cmake --build build
ctest --test-dir build --output-on-failure
```

Fuzzers need clang: configure with `-DQMEDIA_BUILD_FUZZERS=ON -DQMEDIA_BUILD_TESTS=OFF`, build the
targets `fuzz_message` and `fuzz_session`, copy `ipc/fuzz/corpus/<message|session>` to a scratch
directory and pass that directory as the corpus argument.

The seed corpus is generated from the schema table. After changing the schema, run
`qmedia_corpus_gen engine/ipc/fuzz/corpus` and commit the result; the unit tests and CI fail while
the committed files differ from what the generator produces.

## Linking libwebrtc

The engine core will link `webrtc.lib` from the Windows release of this repository (tag suffix
`a256-dplc-9` or later). The hook is `cmake/webrtc.cmake`, function `qmedia_link_webrtc`. It is
not wired yet because it needs `build-flags.json` from that release (compile definitions, CRT mode,
STL and RTTI settings of the library), which the library workflow has not published at the time of
writing. Until then `-DQMEDIA_WITH_WEBRTC=ON` stops the configure step on purpose. The comment
block in `cmake/webrtc.cmake` lists the four steps to wire it, including verifying the sha256 of
`webrtc.lib` against the release before use.
