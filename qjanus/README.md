# qjanus

Janus VideoRoom SFU for the Q-Audion group calls v2 (spec `GROUP_CALLS_V2`, section 6):
one Janus binary that terminates exactly the transport level of the 1:1 calls and nothing weaker.

- DTLS 1.3 only, `X25519MLKEM768` (0x11EC) required, `TLS_AES_256_GCM_SHA384` only, SRTP `AEAD_AES_256_GCM` only.
  Checked after every handshake; anything else gets alert 71 and is hung up at once (fail closed).
- Built from pinned sources (`build/pins.env`): Janus v1.4.2, BoringSSL `f91f1447`, libsrtp 2.8.1 on the
  BoringSSL backend, libnice 0.1.24 (GnuTLS), libwebsockets (no TLS library). The only TLS/crypto code in the
  process is the static BoringSSL; the build fails if a system `libssl`/`libcrypto` is linked.
- Only the VideoRoom plugin and the HTTP + WebSockets transports. No data channels, no other plugins or
  transports, no event handlers, no recordings post-processing. Nothing is ever recorded: upstream VideoRoom lets a
  participant start a recording, with a file name of its own, in the `joinandconfigure` request without the room secret
  that `lock_record` is meant to demand, so the recorder itself refuses to create any file (patch 0005).
- Content is end-to-end encrypted by the clients (`require_e2ee`); qjanus forwards SRTP and never sees keys
  or plaintext. Its logs carry no identifiers, addresses, fingerprints or keys (ids are cut to 8 characters).
- The DTLS certificate (ECDSA P-256) is fixed per node and generated ON the node by `install.sh`; its
  SHA-256 fingerprint is what the application server pins and hands to the clients.

Target: Ubuntu 24.04 x86_64 (glibc 2.39), systemd. Everything below runs as root on the node.

## Files

| Path | What |
|---|---|
| `build/` | pinned dependency + Janus build, packaging, linkage and clean-container checks (used by the workflow) |
| `patches/` | the seven patches applied to pristine Janus v1.4.2 (DTLS policy, log scrubber and no session token in any log line, `info` without addresses, WebSockets without TLS, no recordings, build guard for AES-GCM, publisher join token bound to the pseudonym) |
| `CHANGELOG.md` | release notes per version (the workflow publishes the section of the released version) |
| `conf/*.jcfg.tmpl` | config templates, rendered at every service start; nothing is edited per node |
| `conf/Caddyfile.example` | the Caddy block that publishes the client API |
| `systemd/qjanus.service` | the unit (sandbox and limits of spec section 6) |
| `install.sh` | idempotent installer, `--rollback`, `--uninstall` |
| `test/` | API conformance, browser end-to-end, netem, negative peers, log hygiene (all run in CI against the installed service) |

## Install a node

1. Download the release assets (`qjanus-<version>-ubuntu24.04-x86_64.tar.gz`, `SHA256SUMS`, `BUILDINFO.json`)
   from the GitHub release `qjanus-<version>` and verify them:

   ```
   sha256sum -c SHA256SUMS
   gh attestation verify qjanus-<version>-ubuntu24.04-x86_64.tar.gz -R sigarone/webrtc-aes256-build
   ```

2. Unpack and install. `--install-deps` lets the script `apt-get install` the runtime packages listed in
   `apt-deps.txt` (libconfig9, libmicrohttpd12t64, openssl, ...).

   ```
   mkdir -p /root/qjanus-install && tar -xzf qjanus-<version>-ubuntu24.04-x86_64.tar.gz -C /root/qjanus-install
   cd /root/qjanus-install/qjanus
   ./install.sh --install-deps --ice-iface <public-interface>                       # node on the application server host: HTTP API on 127.0.0.1 (default)
   ./install.sh --install-deps --ice-iface <public-interface> --http-bind <vpn-address>  # remote node: HTTP API on the VPN address only
   ```

   `--ice-iface` (or `QJANUS_ICE_ENFORCE_IFACE` in the environment of the script) is REQUIRED: the network interface
   that carries the node's PUBLIC address (`eth0`, `ens3`, ...). Janus would otherwise offer the address of every
   interface as an ICE candidate, the private address of a VPN (tailscale, qvpn, wg), a bridge or a container network
   included: a privacy leak, and candidates that cannot work. qjanus offers the IPv4 and IPv6 addresses of that one
   interface and nothing else (`nat.ice_enforce_list`). Without the option the script names the interface of the default
   route and stops; run it again with `--ice-iface <that name>` to confirm. It refuses an interface that does not exist,
   is down, is a loopback/VPN/bridge/container interface (`lo`, `wg*`, `tailscale*`, `qvpn*`, `docker*`, `br-*`,
   `veth*`, `virbr*`, `lxc*`, `vmnet*`), whose name is the prefix of another interface (Janus matches names by prefix),
   or that carries private addresses only (a node behind a 1:1 NAT sets `QJANUS_NAT_1_1`; `--allow-private-ice-iface`
   accepts private addresses on purpose). `ip -br addr` shows the candidates.

   The script prints ONE line on stdout, the pinned certificate fingerprint (`sha-256 AB:CD:...`); everything else
   goes to stderr, so `FP=$(./install.sh ...)` works. Give the fingerprint to the application server (per node,
   `dtls_fingerprint` of `group_call_media_ready`). It is public information; the private key never leaves
   the node.

   What it creates: `/opt/qjanus/releases/<version>` (+ `current` symlink), `/etc/qjanus/dtls.key` (0600),
   `/etc/qjanus/dtls.crt`, `/etc/qjanus/qjanus.env` (0600), `/etc/systemd/system/qjanus.service`; then it
   enables and starts the service and waits for the API to answer.

3. Give the application server the two secrets of the node. They are only in `/etc/qjanus/qjanus.env` (root, 0600):

   | Variable | Used for |
   |---|---|
   | `QJANUS_TOKEN_SECRET` | HMAC-SHA256 key of the signed session tokens (`token_auth_secret`) |
   | `QJANUS_ADMIN_KEY` | VideoRoom `admin_key` (room creation) |
   | `QJANUS_ICE_ENFORCE_IFACE` | the interface whose addresses are the ONLY ICE candidates (`nat.ice_enforce_list`); required, set by `install.sh --ice-iface`, checked at every install and every service start |
   | `QJANUS_ALLOW_PRIVATE_ICE_IFACE` | optional, `yes` accepts an ICE interface that carries private addresses only (`install.sh --allow-private-ice-iface`; a hosted CI runner is such a host) |
   | `QJANUS_HTTP_BIND` | address of the server-facing HTTP API (port 8088): loopback, private (RFC 1918), VPN (100.64.0.0/10) or unique-local only |
   | `QJANUS_ALLOW_NONPRIVATE_BIND` | optional, `yes` lets `QJANUS_HTTP_BIND` be a public address (plain HTTP, no TLS: only if you know why) |
   | `QJANUS_NAT_1_1` | optional: public IPv4, only for a node behind a 1:1 NAT (nodes that carry their public address on an interface need nothing) |

   Change a value by editing the file and running `systemctl restart qjanus`. Rotating the token secret or the
   admin key means updating the application server at the same time; running calls keep their PeerConnections but their
   WebSocket sessions need a fresh token.

   `QJANUS_HTTP_BIND` must be a loopback, private, VPN (CGNAT) or unique-local address: `install.sh --http-bind` and the
   unit's `qjanus-render-config` refuse a public or wildcard address (the server API is plain HTTP for the application
   server only). It must also be an address that an interface of the node carries and that is UP when the service starts
   (Janus compares it with the interface list; IPv6 as printed by `ip -6 addr`, compressed and lower case). Janus
   would otherwise keep running without the HTTP transport, so the unit has an `ExecStartPost`
   (`libexec/qjanus-wait-ready`) that fails the start, and lets systemd retry, until both APIs answer: a node is either
   complete or not running. On a remote node whose address lives on the VPN interface, start the service after the
   tunnel: `systemctl edit qjanus` and add `[Unit]` `After=wg-quick@<vpn-interface>.service`
   `Wants=wg-quick@<vpn-interface>.service` (or the unit that brings your VPN interface up).

4. Firewall. The media ports and nothing else:

   ```
   ufw allow 20000:20999/udp        # RTP/DTLS of the PeerConnections (rtp_port_range)
   ```

   Ports 8088 (server API) and 8188 (client API) are NEVER opened. 8188 listens on 127.0.0.1 only; 8088 listens
   on 127.0.0.1 (node on the application server host) or on the VPN address (remote node: allow it only on the VPN interface, e.g.
   `ufw allow in on <vpn-interface> to any port 8088 proto tcp from <server-vpn-address>`). qjanus itself never listens on the
   Admin API ports (7088/7188) and has no `api_secret`.

5. Caddy. Add the block of `conf/Caddyfile.example` (in the tarball: `share/qjanus/Caddyfile.example`) to the site that serves the node's host name (the address
   in `ws_url` = `wss://<host>/janus`), then `systemctl reload caddy`:

   ```
   handle /janus {
   	reverse_proxy 127.0.0.1:8188
   }
   ```

   Caddy does the TLS termination and passes the WebSocket upgrade through unchanged; the same block is what
   the CI proves against the installed node.

6. Check:

   ```
   systemctl status qjanus
   curl -s http://127.0.0.1:8088/janus/info | head -c 300      # or the VPN address on a remote node
   journalctl -u qjanus -o cat -n 50                            # DTLS-POLICY ... ok=1 per handshake
   sudo python3 /opt/qjanus/current/share/qjanus/test/node-ice-check.py   # live: the ICE candidates are only the public interface's
   ```

   `node-ice-check.py` (python3, standard library only; also in `test/`) creates a throw-away room, publishes a synthetic
   E2EE offer through the WebSocket API and asserts that every candidate Janus offers is an address of the enforced
   interface and none belongs to another interface of the host. `--ws wss://<host>/janus` runs it through Caddy.

## Upgrade and roll back

```
tar -xzf qjanus-<new>-ubuntu24.04-x86_64.tar.gz -C /root/qjanus-new && cd /root/qjanus-new/qjanus
./install.sh --install-deps        # new release dir, atomic `current` switch, restart; same key, secrets, settings
/opt/qjanus/current/install.sh --rollback     # back to the previously installed release
```

A node installed before 1.4.2-q2 has no ICE interface in its `qjanus.env` yet: the upgrade stops, names the interface of
the default route and changes nothing until it is given once, `./install.sh --install-deps --ice-iface eth0` (or
`QJANUS_ICE_ENFORCE_IFACE=eth0 ./install.sh --install-deps`); afterwards it is kept like every other setting. From 1.4.2-q2
on the application server must issue publisher join tokens as `<pseudonym>:<32 lowercase hex>` (see below).

The fingerprint printed is unchanged by an upgrade (same key). The last three releases are kept
(`--keep-releases N`). If the new release (or a changed setting such as `--http-bind`) does not come up healthy, the
script puts the previous release (with the unit it shipped) and the previous `qjanus.env` back, starts them again,
exits with an error and prints no fingerprint; `--rollback` only ever goes to a release that has run on the node
(never to one that was installed but did not start, nor to an interrupted install). Installing the same version
name with different content is refused. Only one `install.sh` runs at a time. An upgrade restarts the service, which drops the running calls' media: do it between
calls, or after moving the rooms away (spec section 2.5).

## Uninstall

```
/opt/qjanus/current/install.sh --uninstall          # stops and removes the service and /opt/qjanus, keeps /etc/qjanus
/opt/qjanus/current/install.sh --uninstall --purge  # also deletes the DTLS key and the secrets
ufw delete allow 20000:20999/udp                    # and remove the Caddy block
```

A reinstall after `--uninstall` reuses the kept key (same fingerprint); after `--purge` it creates a new one and
the fingerprint has to be published again.

## Runtime

`qjanus.service` runs janus as a `DynamicUser` with an empty capability set, `NoNewPrivileges`,
`ProtectSystem=strict`, `ProtectHome`, `PrivateTmp`/`PrivateDevices`, `RestrictAddressFamilies` (inet, inet6,
unix, netlink), `SystemCallFilter=@system-service`, `InaccessiblePaths` for the application server's
directories, `MemoryMax=2G`, `CPUQuota=200%`, `CPUWeight=50`, `LimitNOFILE=65536`. The DTLS private key reaches
the process as a systemd credential (`LoadCredential`), the rendered configuration (with the secrets) lives only
in `/run/qjanus` (0700, service user). No secret is ever on a command line.

Core settings (spec section 6): `token_auth=true` (sha256), `string_ids=true`, `admin_key`,
`lock_rtp_forward=true`, `rtp_port_range=20000-20999`, `ice_lite=true`, `ice_tcp=false`,
`ice_enforce_list=<QJANUS_ICE_ENFORCE_IFACE>` (plus `ice_ignore_list=vmnet,docker,veth,br-,virbr,lxc,wg,tailscale,qvpn,lo`
as belt and braces: Janus v1.4.2 ignores that list for interface names while an enforce list is set and logs one warning
per entry at start), `dtls_mtu=1200`,
`min_nack_queue=500`, `twcc_period=200`, `session_timeout=60`, `reclaim_session_timeout=20`, IPv6 on, no static rooms,
Admin API off. Log level 4 (INFO): nothing identifying is written at any level (patch `0002-log-scrub.patch`).

## API surface verified by the tests

The application server implements the HTTP calls, the apps implement the WebSocket calls; `test/lib/janus.mjs`
(`ServerApi`, `WsClient`) is the executable reference and `test/conformance/conformance.mjs` prints every
request/response shape it asserts (the `SHAPES` block of the CI log). Points that matter:

- Session token: `<expiry_unix>,janus,janus.plugin.videoroom:<base64 HMAC-SHA256(secret, "<expiry>,janus,janus.plugin.videoroom")>`,
  sent as `token` in EVERY request (create, attach, message, keepalive, claim, destroy) and validated each time;
  missing/wrong/expired/wrong-realm -> `{"janus":"error","error":{"code":403}}`; a token without the VideoRoom
  descriptor can create a session but not attach (405). `info` and `ping` need no token.
- HTTP (server): `POST /janus` `{janus:"create",token}` -> `data.id`; `POST /janus/<sid>` `{janus:"attach",plugin,token}`
  -> `data.id`; `POST /janus/<sid>/<handle>` `{janus:"message",token,body}`. Room management requests are answered
  synchronously (`janus:"success"`, `plugindata.data`); plugin errors are `plugindata.data.error_code`
  (426 no such room, 427 exists, 428 no such feed/participant, 429 missing element, 432 publishers full,
  433 unauthorized, 436 id exists). One session per batch of calls (session_timeout is 60 s), `destroy` afterwards.
- `create` needs `admin_key` (429 missing, 433 wrong); `allowed` (add/remove), `kick`, `destroy` need the room `secret`
  (429/433). `list` without `admin_key` never shows the private room; `listparticipants`/`exists` need nothing.
- `join` publisher: `id` = pseudonym (string), `token` = join token from `allowed`. Since 1.4.2-q2 the join token is bound
  to the pseudonym: `<id>:<32 lowercase hex>` with the `id` of the request as its prefix (patch 0007). A missing or
  non-string `id`, or any token that does not have exactly that form for that `id` (the token of member A used with the
  id of B, a token without prefix, uppercase or short hex, ...) -> 433 before anything else, `joinandconfigure` included.
  Besides that, a token that is not in `allowed` (never added, or removed) -> 433, also after a kick. `joined` carries `private_id`. `publish` without `e2ee:true` in
  the JSEP is refused (433 "Room requires end-to-end encrypted media"); a handle whose publish was refused must not be
  reused (attach a new one).
- Simulcast: Janus assumes the RIDs in the publisher's SDP are listed highest first (`hml`) unless the publish JSEP
  carries `"rid_order":"lmh"`. Substream 0 is always the lowest layer, 2 the highest. libwebrtc encodes 3 simulcast
  layers only from a source of about 720p or more (smaller sources lose the last layer of the list), and the native
  order is ascending, so a client that lists `l, m, h` MUST send `rid_order:"lmh"`, otherwise substream 0 would be the
  HIGHEST layer.
- Subscriber: `join` with `ptype:"subscriber"`, `private_id` (required, 433 if wrong), `streams:[{feed,mid}]`; a feed that
  is not publishing yet -> 428. The reply is `{videoroom:"attached", streams:[...]}` plus a JSEP OFFER (`e2ee:true`);
  the client answers with `{request:"start"}` + the JSEP answer (`started:"ok"`), then `webrtcup`. Each stream entry
  carries its subscriber `mid`, `feed_id`, `feed_mid`, `feed_description`, and for simulcast video
  `simulcast:{substream, substream-target, temporal-layer, temporal-layer-target}`.
- Changing the subscription: `{request:"update",subscribe:[{feed,mid}...]}` or `{request:"unsubscribe",streams:[{feed}]}`
  -> `{videoroom:"updated",streams:[all current entries]}` + a new JSEP offer (new mids are appended, unsubscribed
  ones stay in the SDP as `active:false`); answer it with `start` + answer. Renegotiations are serialised by the client.
- Layer selection: `{request:"configure",streams:[{mid,substream:0|1|2,temporal:0..2}]}` -> `configured:"ok"`, then the
  plugin event `{mid,substream}` / `{mid,temporal}` when the switch happened. Verified with a 1280x720 source: substream
  0/1/2 deliver exactly the l/m/h layer (320/640/1280 px) with E2EE frames.
  Rooms are created with `fir_freq=0` (no periodic keyframe request): the keyframe of the new layer comes from the PLI
  Janus sends when `configure` changes the substream. Janus sends at most one PLI per second per publisher stream and does
  not retry one it skipped, so a `configure` issued right after another PLI can end without the switch (measured with
  the 720p synthetic camera: substream 0 then 1 within a second stays on 0 for 20+ s). A client that has not seen the
  `substream` event (or the new layer) within a few seconds sends the same `configure` again.
- Events without a transaction: `webrtcup`, `media` (`mid,type,receiving`), `slowlink` (`mid,media,uplink,lost`) and the
  plugin event `slow_link` (`current-bitrate`), `hangup` (`reason`), plugin events `{publishers:[{id,display,streams}]}`
  (new publisher), `{leaving:"<id>"}`, `{unpublished:"<id>"}`, `{kicked:"<id>"}`, `{videoroom:"destroyed"}`.
- Kick order for the server (spec section 3): `allowed` remove, then `kick`; the kicked handle gets
  `{leaving:"ok",reason:"kicked"}`, the others `{kicked:"<id>"}`, and the publisher PeerConnection is hung up.

## Build (CI)

`.github/workflows/qjanus.yml` builds the tarball on ubuntu-24.04, runs it on a clean `ubuntu:24.04` container
(apt-deps complete, relocatable, no system libssl), installs it under systemd like a node and runs the API
conformance suite, headless Chromium (publisher + multistream subscribers, VP8 simulcast, E2EE frames that keep
the codec header clear, key switch-over, kick, destroy, netem loss 5 % + reorder 3 %, 30 % loss handshake soak),
the refused peers (DTLS 1.2, DTLS 1.3 without ML-KEM, AES-128-GCM), the ICE candidates of every SDP (only the enforced
interface, with decoy VPN/private interfaces on the host), the join token binding, the Caddy block, the log-hygiene check
and the install/upgrade/rollback/uninstall lifecycle. A release is created only by running the workflow manually with
`release_tag=qjanus-<version>`; assets are attested (`gh attestation verify`).
