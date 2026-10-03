# Media engine

This directory holds the generic media engine process of the desktop client, `qaudion-media.exe`,
and the IPC library that its host application uses to talk to it. It lives in the public build
repository next to the libwebrtc build so that library and engine are built, attested and
published by the same pipeline.

Status: the IPC layer (framing, deterministic CBOR codec, schema validator, session handshake,
Windows named-pipe transport, tests, fuzzers, CI) is complete, and the engine core for a 1:1 AUDIO
call is linked against the published Windows libwebrtc release (see "The engine core" and "Linking
libwebrtc"). Video, screen sharing, H.265, the PCM tap, process isolation and group calls are not
part of this build: their commands answer `err unsupported`.

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
   client. The host should pick a fresh random pipe name for every start (128 random bits). With
   `--expect-client-pid` it also checks the client process id with `GetNamedPipeClientProcessId`.
   Each option may be given once; a repeated option or a client process id of 0 is a bad command
   line (exit code 2), so the check cannot be switched off by accident.
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
2 bad command line, 3 nonce missing, short or all zero, 4 pipe creation failed, 5 no client in time, 6 client
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
  buffer, so the decoder never makes a second copy of a key. That buffer is ordinary heap memory
  (it holds up to 1 MiB): a key is in pageable memory only for the time one message is handled.
  The in-memory test stream delivers partial reads the way a pipe does, so the wipe of a partially
  received frame is exercised by the unit tests and the session fuzzer.
- The engine copies a key once from the receive buffer into a local buffer, hands it to the frame
  key provider and zeroes that local copy with `SecureZero` immediately. `retire_slot` fills the
  slot with random bytes, never with zeros, and ending a session overwrites every slot that was ever
  filled. An all-zero key in `install_key` is rejected by the schema. Limits that are not hidden:
  libwebrtc's key provider keeps its own copies of the key (and a derived key) in ordinary heap
  memory while a slot is live, and the copies it makes on the way (`SetKey` takes the vector by
  value) are freed without being zeroed. The engine cannot reach those without patching the
  library; the mitigations are the process boundary, no crash dumps (below) and the short life of
  a call's keys.
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
  creation and the pipe is refused if it is not exactly that (one allow entry, that SID, that
  access mask, no ACE flags). The access mask is the generic read
  and write set, which includes the right to create a pipe instance; the instance limit of one is
  what prevents a second instance, so do not raise it.
- The client side opens the pipe with identification-level impersonation only
  (`SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION`, as `ConnectPipe` does). The host must do the
  same: a plain file open of the pipe from the JVM grants the server full impersonation, which a
  compromised engine could use against the host.
- The pipe carries no explicit integrity label, so the default policy applies (no write up). A
  process of the same user at lower integrity can still open it for reading and so occupy the
  single instance; it cannot send a hello and the engine sends nothing before one, so this is a
  denial of service of one start, not a disclosure.
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
  `add_ice_candidate`, `restart_ice`, `update_ice_servers` (refresh relay credentials in a long call), and the events `ice_candidate`, `ice_gathering_state`,
  `ice_connection_state`, `pc_state`, `negotiation_needed`.
- Frame keys: `install_key` (participant, slot 0 to 15, 32-byte key, direction send or recv),
  `retire_slot`, `select_send_slot`, `bind_media` (ties a media section to a participant and a
  direction), and the `cryptor_state` event.
- Transport: the `transport_info` event (TLS version, DTLS cipher, group, SRTP cipher, the
  fingerprint of the remote certificate that was actually negotiated, `remote_cert_fingerprint`,
  and the fingerprint of the local certificate of the DTLS transport, `local_cert_fingerprint`;
  both are the SHA-256 of the DER certificate, the form `cert_created` returns) and
  `transport_violation`.
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
leniency), and libFuzzer with ASan and UBSan. The message fuzzer compares every verdict of the
validator with an independent second implementation of the rules (`ipc/fuzz/oracle.h`) that
descends into every nested object, array element and scalar map, so a validator that fails open
anywhere in a message is caught without needing a crash.

## The engine core

Implemented for a 1:1 audio call (`src/`): `engine.*` (command dispatch, sessions, certificates),
`peer.*` (one peer connection), `keys.*` (the per-session frame key provider), `runtime.*`
(threads, audio device, audio processing, factory), `stats.*`, `devices.*`.

- Certificates: `cert_create` makes an ECDSA P-256 certificate and returns a handle plus the raw
  SHA-256 of its DER encoding (computed with the operating system's hash). A certificate lives as
  long as its session, is never regenerated and never evicted; a peer connection created again in
  the same session with the same handle presents the same certificate.
- Peer connections: `pc_create` takes the certificate handle; the configuration is compiled in
  (Unified Plan, max-bundle, rtcp-mux, GCM-only SRTP options, continual gathering, TCP candidates
  off, jitter buffer cap 17 packets, KEEP_FIRST_READY) and mirrors the mobile apps' native audio
  call (Android `PeerConnectionHolder.kt`, RTCConfiguration block, main 4f6b3983, lines 4218-4332).
  The offerer adds the audio transceiver at `create_offer`; the answerer attaches its microphone
  after `set_remote_description(offer)`. The host owns the SDP: it may rewrite it between
  `create_*` and `set_local_description`.
- Frame keys: one libwebrtc key provider per SESSION (so the P12 replay windows survive the
  re-creation of a peer connection), configured exactly like the mobile apps' 1:1 provider:
  per-participant keys, empty ratchet salt, ratchet window 0, no magic bytes, failure tolerance -1,
  key ring 16, frames discarded while no key is installed, HKDF (Android
  `PeerConnectionHolder.kt` line 2615 of main 4f6b3983, iOS `NativeAudioFrameCryptor.swift` line 90
  of main 300ce640). Send keys and receive keys of one participant id are kept apart inside the
  provider. `bind_media` ties the cryptor of a media section (mid) to a participant and a direction;
  `select_send_slot` moves the sender cryptors of that participant; `cryptor_state` events report
  ok, missing_key, decryption_failed, encryption_failed, internal_error.
- Transport: once a peer connection is `connected` the engine reads every DTLS transport and emits
  `transport_info` (DTLS1.3, TLS_AES_256_GCM_SHA384, X25519MLKEM768, AEAD_AES_256_GCM, the SHA-256
  of the remote certificate actually negotiated and the SHA-256 of the local certificate of the
  transport) or, if anything differs from the compiled-in policy, `transport_violation` and closes
  the connection. The remote certificate comes from the DTLS transport information. libwebrtc's
  public API has no local-certificate accessor there, so the local one is read from the transport
  statistics (`transport.localCertificateId`), which report the certificate object the transport
  controller installed in the DTLS transports: it is set on all of them in one step and cannot be
  replaced, and the engine builds the connection with exactly one certificate (the `pc_create`
  handle), so under this strict factory it is the certificate the handshake used. The engine hashes
  the DER of that object with the same code as `cert_create`, so the two compare byte for byte.
  No legitimate call is ended by a short timer: the lookup asks for a statistics report every
  250 ms and keeps asking for `CONFIRM_TIMEOUT` = 15 s after each transition to `connected`
  (WIRE_SPEC section 3.8.4; one constant, `kConfirmTimeoutMs` in `src/local_cert_policy.h`), and
  every new transition opens its own 15 s, also for a lookup that is still running. A slow
  statistics collector or a loaded machine costs time, not the call. Only if no local certificate
  was readable during the whole window is the verdict `transport_violation` with reason `no_dtls`
  (and the connection is closed). Two transports that report different certificates, or a
  certificate that cannot be decoded, are a finding and give that verdict at once. The lookup is
  cancelled by `pc_close` and by the end of the session: it holds no reference to the peer.
- Audio: Windows Core Audio (see the note below), AEC3, noise suppression and AGC through the audio
  processing module. `list_devices`, `select_device` (a running stream is stopped, switched and
  restarted), `devices_changed` from the Windows endpoint notifications, `set_muted` for the
  microphone and for remote audio.
- Opus: the engine takes 60 ms, 32 kbps, CBR and in-band FEC from the host's SDP. `set_audio_tuning`
  applies `bitrate_bps` (encoder held at that rate on both bounds, adaptive packetisation off) and
  `fec_floor_pct` (0 to 20, process-wide in libwebrtc, so it affects every call of the process; the
  engine serves one host and in practice one call). `ptime_ms` and `cbr` are SDP parameters and
  are refused with `unsupported` and a static detail; nothing is applied when one is present.
- Statistics: `get_stats` passes on an allowlist of attributes of inbound/outbound/remote RTP,
  media source, transport, candidate pair and codec entries. Fractional and negative numbers are
  decimal text, ids are `n<index>`, no address, certificate, stream id or SDP detail is included.
- Ordering of replies and events (what the host may rely on, and what it may not):
  - Replies are written by the session loop after the command has been handled, in the order the
    commands arrived; one command is handled at a time, so the reply to command N is always written
    before the reply to command N+1.
  - Events (id 0) are written from libwebrtc threads as they happen. They are serialised with each
    other and with replies (a frame is never torn), but there is no order between an event and the
    reply of the command that caused it: a candidate event may arrive before the `ok` of the
    `set_local_description` that started gathering, `cryptor_state` and `pc_state` may arrive before
    the `ok` of the command that triggered them, and an event for a peer connection can arrive
    before the host has read the `pc_created` reply that names it. The host must therefore accept
    events for a pc or session it has been told about at any time, and key its state on the ids in
    the event, not on the order of messages.
  - Events of one peer connection are in the order libwebrtc reports them (they come from one
    thread). `transport_info` follows the `pc_state` event that says `connected`, normally within a
    few milliseconds but, if the statistics collector is slow, up to 15 s later: the local certificate
    is read from an asynchronous statistics report, so other events of the connection may be written
    in between. A later `connected` or an ICE restart makes
    the engine look at transports it has not reported.
  - `shutdown`: no event is written after the engine has started to shut down, and its `ok` is the
    last message. After `session_close` or `pc_close` the engine stops emitting events for that
    session or peer connection; an event that was already being written at that moment can still
    reach the host next to the reply.
  - The engine does not hold audio back until the host has bound the frame cryptors. A host that
    wants end-to-end frame encryption on a call installs the keys, and sends `bind_media` for the
    audio section as soon as its `mid` is known (the offerer after `create_offer`, the answerer
    after `set_remote_description`), before the connection comes up, and treats a `cryptor_state`
    other than `ok` as a failure. `require_frame_encryption` is off, exactly like the mobile apps:
    the DTLS-SRTP transport is always AES-256-GCM, the frame layer is the host's to bind.

Core Audio: the published library (dplc-10) contains only the original Windows Core Audio device
module (`AudioDeviceWindowsCore`, reached through `kPlatformDefaultAudio`). The newer Core Audio 2
module and its factory are not in it, so the plan's wording "Core Audio 2" does not apply to this
build. Device ids are the endpoint ids it reports plus the aliases `default` and `communications`.
This was exercised in CI only with the file device below; real hardware, Bluetooth profiles and
device removal are not verified.

Frame tests without a sound card: the CI executable `qaudion-media-ci` (compiled with
`QMEDIA_CI_BUILD`, a separate target) replaces the sound card with libwebrtc's `FileAudioDevice`
(`--ci-audio-in` raw 48 kHz stereo 16-bit file as the microphone, `--ci-audio-out` raw file for the
played-out audio) and allows ICE on the loopback interface (`--ci-allow-loopback`). The production
executable rejects these flags and a CI step checks that none of that code is in it.

Not in this build: video, screen sharing, H.265, `pcm_tap`, `request_keyframe`, `set_simulcast`,
Job Object and token restriction (host side), Control Flow Guard (libwebrtc.lib is not built with
`/guard:cf`; a guarded indirect call into it would fail, so the executable has ASLR, high-entropy
ASLR and DEP only).

## Layout

```
engine/
  CMakeLists.txt        project root (IPC library, tests, optional fuzzers, qaudion-media on Windows)
  cmake/                pinned release (webrtc-release.cmake), fetch script, libwebrtc wiring (webrtc.cmake)
  src/                  main.cpp and the engine core (engine, peer, keys, runtime, stats, devices)
  tests/call_test.cpp   two engine processes driven through a full audio call (CI)
  tests/local_cert_policy_test.cpp  the 250 ms / 15 s lookup rules of src/local_cert_policy.h (ctest, every platform)
  ipc/
    schema.cddl         the contract
    include/qmedia/ipc  public headers (limits.h lists every bound in one place)
    src                 cbor, frame, schema, session, secure, status, message, pipe (Windows)
    tests               unit tests, pipe tests (Windows), corpus generator
    fuzz                libFuzzer targets and the committed seed corpus
```

## Building and testing

CI (`.github/workflows/engine.yml`, filtered on `engine/**`) runs four jobs: unit tests on Linux
with clang under ASan and UBSan and with gcc in Release, both with warnings as errors; two
libFuzzer targets for a fixed time with the committed seed corpus; the Windows build with MSVC that
runs the unit tests and the pipe tests against the stub `qaudion-media.exe`; and `engine-webrtc`,
which builds the real engine against the pinned libwebrtc and runs `qmedia_call_test.exe`: two
engine processes, a complete call, DTLS 1.3 / TLS_AES_256_GCM_SHA384 / X25519MLKEM768 /
AEAD_AES_256_GCM asserted on both ends, remote fingerprint equal to the peer's `cert_create`
fingerprint and local fingerprint equal to the side's own, cryptors OK, decoded non-silent audio on
both sides, and the negative cases (wrong key, retired slot, a caller told a wrong fingerprint for
the callee, which must fail its handshake and report no `transport_info`).

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

The engine links the Windows release `webrtc-windows-m150-a256-dplc-10` of this repository. The
tag and the sha256 of all seven release files are pinned in `cmake/webrtc-release.cmake`; moving to
another release means changing that file.

1. `cmake -DQMEDIA_FETCH_DIR=<dir> -P cmake/fetch_webrtc.cmake` downloads the release files and
   verifies every one against the pinned sha256 (the release's own `SHA256SUMS` must agree with the
   pins and list nothing else), unpacks the headers, downloads the Chromium clang package that
   `build-flags.json` names (url pattern and sha256 checked), verifies it, unpacks it and writes
   `<dir>/toolchain.cmake`.
2. CI verifies the build provenance attestation of `webrtc.lib` with `gh attestation verify`
   (repository and builder workflow pinned, retried on transient errors, no weaker fallback: a
   failure stops the job).
   The pins are checked again at configure time by `cmake/verify_pins.cmake`, which the toolchain
   file includes before any compiler runs and `cmake/webrtc.cmake` includes too: every release
   file must exist and match its pin, the headers must come from the pinned archive and the compiler
   directory from the package `build-flags.json` names, otherwise the configure stops. CI
   runs it against a tampered copy to show it refuses (empty directory, changed file, missing file,
   wrong header stamp).
3. `cmake -S engine -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=<dir>/toolchain.cmake -DQMEDIA_WITH_WEBRTC=ON`
   from a Visual Studio developer environment (the Windows SDK and CRT headers and libraries come
   from it). `cmake/webrtc.cmake` reads `build-flags.json` and applies exactly its defines, flags
   and include directories (in the recorded order, so Chromium's libc++ headers win over the MSVC
   ones) to every target, links `webrtc.lib`, `libcxx.lib`, the system libraries and the
   compiler-rt builtins it lists and adds `/DEFAULTLIB:libcpmt.lib`. Every token from the JSON is
   validated before it reaches a command line. The whole project is built this way in this mode:
   the IPC library shares types with the engine and must use the same C++ library.
4. Targets: `qaudion-media` (production), `qaudion-media-ci` (file audio device), `qmedia_call_test`
   (the call driver). `ctest` has no entry for them; CI runs `qmedia_call_test.exe`. The timing
   rules of the local certificate lookup are in `qmedia_engine_policy_tests` (ctest `engine_policy`).

The MSVC build of the IPC layer (`QMEDIA_WITH_WEBRTC=OFF`, the default) is unchanged and still
produces the stub `qaudion-media.exe` that the pipe tests use.
