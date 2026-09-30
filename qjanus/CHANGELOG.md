# qjanus release notes

Every release is published by the `qjanus` workflow (`release_tag=qjanus-<version>`); the section below whose
heading is `## <version>` becomes the text of the GitHub release (the workflow refuses a release without one).
The Janus base is always the pinned upstream v1.4.2 (`build/pins.env`); `-qN` counts the qjanus builds of it.

## 1.4.2-q2

ICE candidates, join token, build guard. An upgrade from q1 needs ONE new setting per node (see Upgrade).

- ICE interface (new, required). Janus offers the address of every interface as an ICE candidate, the private
  addresses of VPN (tailscale0, qvpn0, wg*), bridge and container interfaces included (a privacy leak and
  candidates that can never work). The node now names the ONE interface that carries its public address,
  `QJANUS_ICE_ENFORCE_IFACE` in `/etc/qjanus/qjanus.env` (for example `eth0`), rendered as `nat.ice_enforce_list`:
  only the IPv4 and IPv6 addresses of that interface are offered. `ice_ignore_list` stays (now
  `vmnet,docker,veth,br-,virbr,lxc,wg,tailscale,qvpn,lo`) as belt and braces; Janus v1.4.2 does not look at it for
  interface names while an enforce list is set.
- Fail-safe selection. `install.sh` (and the service's `qjanus-render-config`, at every start) refuse an interface
  that does not exist, is down or has no carrier, is a loopback/VPN/bridge/container interface, is the prefix of
  another interface (Janus matches the name by prefix), carries no address, or carries private addresses only
  (unless `QJANUS_NAT_1_1` is set or `--allow-private-ice-iface` / `QJANUS_ALLOW_PRIVATE_ICE_IFACE=yes` says so).
  Without the setting the installer names the interface of the default route and stops; confirm it with
  `--ice-iface <name>`. A node that would offer the wrong addresses does not start.
- Join token bound to the pseudonym (GROUP_CALLS_V2 section 12.2, H3). A publisher `join`/`joinandconfigure` is
  refused with error 433 before anything else unless `id` is present and the `token` is exactly
  `<id>:<32 lowercase hex>` (and, as before, is in the room's `allowed` list): the token handed to one member can no
  longer be used under the pseudonym of another. The application server must issue tokens in this form.
  This q2 is not compatible with a server that still issues plain random tokens.
- Build guard (section 12.10, L10). The build fails with `#error` when `HAVE_SRTP_AESGCM` is undefined, so a libsrtp
  without AES-GCM can never produce a binary with zero-length SRTP keys (new patch 0006).
- `iproute2` is a runtime package (`apt-deps.txt`): the ICE interface is checked with `ip`.
- Tests: the ICE interface rules (fake `ip`/sysfs), conformance and end-to-end assertions that every SDP out of Janus
  offers only addresses of the enforced interface (decoy VPN/private interfaces are created on the CI host),
  `test/node-ice-check.py` (the live check used on real nodes), the H3 cases (own token accepted; the token of A under
  the id of B, a missing or numeric id, every malformed token refused with 433), and the upgrade path of a node
  that has no ICE interface yet.

Upgrade (per node, the DTLS key and fingerprint, the token secret and the admin key stay):

```
QJANUS_ICE_ENFORCE_IFACE=eth0 ./install.sh --install-deps      # or: ./install.sh --install-deps --ice-iface eth0
```

## 1.4.2-q1

First release: Janus v1.4.2 VideoRoom SFU on BoringSSL `f91f1447` (DTLS 1.3, X25519MLKEM768, TLS_AES_256_GCM_SHA384,
SRTP AEAD_AES_256_GCM only, fail closed), libsrtp 2.8.1 on the BoringSSL backend, libnice 0.1.24, VideoRoom plus the
HTTP and WebSockets transports only, no recordings, scrubbed logs, `install.sh` with upgrade, rollback and uninstall.
