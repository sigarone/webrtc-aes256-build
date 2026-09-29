# desktop-opus

Builds qaudion-desktop's Opus N-API addon (libopus, deep PLC + OSCE) on this
public repo's free GitHub-hosted runners, the same reason the M150 WebRTC
builds live in `../m150/` instead of the private repos: so recompiling this
does not spend the owner's own private-repo Actions minutes.

Today's addon in the private `qaudion-desktop` repo (`native/opus_addon.cc`,
`.github/workflows/build-opus-addon.yml`) has deep PLC (FARGAN) but not OSCE
(LACE/NoLACE). This directory adds OSCE on top, using the exact same Opus
source commit the phone (M150) builds already ship — see `PINS.md` — so
desktop's concealment/enhancement quality matches Android/iOS instead of
merely also having OSCE from an unrelated revision.

## What's here

| File | Purpose |
|---|---|
| `PINS.md` | The Opus commit + DNN weights sha256 this builds, and why |
| `fetch-opus-source.sh` | Fetches xiph/opus at the pinned commit from the official GitHub mirror |
| `fetch-opus-dnn-weights.sh` | Verbatim copy of `../m150/fetch-opus-dnn-weights.sh` — downloads + sha256-verifies the DNN weight tarball |
| `binding.gyp`, `config.h`, `opus_addon.cc`, `index.cjs`, `package.json` | The addon itself — derived from the private repo's `native/` (see PINS.md's provenance section) |
| `check-symbols.mjs` | CI gate: the built `.node` must contain deep-PLC and OSCE (LACE + NoLACE) weight data, not just report it |
| `buildinfo.sh` | Emits `BUILDINFO.json` for a build (source/weights pins, artifact hash, toolchain versions) |

## What this does NOT do

- Does not touch the private `qaudion-desktop` repo. That repo's own
  `feat/opus-osce-prebuilt` branch is a separate change that CONSUMES this
  repo's release (by tag + sha256), once the owner dispatches
  `build-desktop-opus.yml` and fills in the resulting tag/hash there.
- Does not vendor anything from qaudion-desktop's `native/opus/` — the Opus
  source and weights are fetched fresh from their official public hosts at
  build time (see `PINS.md`'s provenance section for exactly what that means
  for each file in this directory).

## Dispatching a build

Actions tab → `build-desktop-opus` → Run workflow → give it a
`release_suffix` (e.g. `osce-1`; the resulting tag is
`desktop-opus-osce-1`). `opus_commit` / `opus_weights_sha256` default to the
verified pins in `PINS.md` and normally do not need to change.

Output: a GitHub Release with `qaudion-opus-win32-x64.node`,
`BUILDINFO.json`, `SHA256SUMS`, and a build-provenance attestation. Take the
release tag and the `.node`'s sha256 from `SHA256SUMS` and fill them into
qaudion-desktop's `scripts/fetch-opus-prebuilt.mjs` (see that repo's
`feat/opus-osce-prebuilt` branch) — that file has a single clearly-marked
place for both.
