# desktop-opus pins

| What | Value |
|---|---|
| `xiph/opus` source commit | `55513e81d8f606bd75d0ff773d2144e5f2a732f5` |
| Opus DNN weights tarball | `https://media.xiph.org/opus/models/opus_data-160753e983198f29f1aae67c54caa0e30bd90f1ce916a52f15bdad2df8e35e58.tar.gz` |
| Opus DNN weights sha256 | `160753e983198f29f1aae67c54caa0e30bd90f1ce916a52f15bdad2df8e35e58` |

## Why this exact commit, not the newest tagged release (v1.6.1)

Verified live from the GitHub API (2026-09-29): `xiph/opus` tag `v1.6.1` resolves
to commit `22244de5a79bd1d6d623c32e72bf1954b56235be`, and its
`dnn/download_model.sh` pins a *different* weights sha256
(`a5177ec6fb7d15058e99e57029746100121f68e4890b1467d4094aa336b6013e`) than the
one this addon uses. Both are legitimate upstream pins for their own revision,
but the owner's actual goal ("desktop aligned on audio quality too") is
served by matching **the phones**, not by picking whatever the newest tag
happens to be:

`m150/README.md` (this same repo, already CI-proven building both Android
variants) pins `third_party/opus/src` to exactly
`55513e81d8f606bd75d0ff773d2144e5f2a732f5` with the `160753e9…` weights
tarball, verified there live on 2026-09-28. Using a different Opus
commit for desktop would mean desktop's deep-PLC/OSCE behavior — model
version, any bug fixes either side of that commit, LACE vs NoLACE
architecture revisions — could subtly diverge from what Android/iOS
actually run, which is the opposite of "aligned." So desktop-opus
deliberately reuses m150's exact pin instead of independently resolving
"newest release with OSCE."

Re-verified independently in this session:
- `GET https://api.github.com/repos/xiph/opus/commits/55513e81d8f606bd75d0ff773d2144e5f2a732f5` → `200` (commit exists on the official mirror).
- `dnn/osce.h` at that commit declares the real, exported `osce_enhance_frame` /
  `osce_load_models` / `osce_reset` API and `OSCE_METHOD_LACE` /
  `OSCE_METHOD_NOLACE` — matches what `desktop-opus/check-symbols.mjs` gates on.
- The weights tarball above was downloaded in full and its sha256 matched
  `160753e983198f29f1aae67c54caa0e30bd90f1ce916a52f15bdad2df8e35e58` byte for
  byte; its member list is exactly `fetch-opus-dnn-weights.sh`'s
  `ALLOWED_MEMBERS` plus DRED/lossgen files that script deliberately does not
  extract (DRED is off — see that script's own comment) and the `.pth`
  PyTorch checkpoints the C arrays were generated from (also not extracted).
- No `bbwenet_*` member exists in that tarball, confirming P4a's own note
  that OSCE-BWE does not exist at this Opus revision.

## Source-file provenance (why nothing here was copied from a private repo)

- `opus/` (all of `celt/`, `silk/`, `src/`, `dnn/*.c`/`*.h` EXCEPT the
  `*_data.[ch]` weight files) is fetched fresh from
  `https://github.com/xiph/opus` at the pinned commit by
  `fetch-opus-source.sh`, which runs in `build-desktop-opus.yml`'s `build`
  job — it is not, and must never become, a copy of qaudion-desktop's own
  `native/opus/`.
- `dnn/*_data.[ch]` (the trained weights) come from the media.xiph.org
  tarball above, via `fetch-opus-dnn-weights.sh` (itself a verbatim copy of
  this same repo's `m150/fetch-opus-dnn-weights.sh` — see that file's
  header).
- `opus_addon.cc`, `binding.gyp`, `index.cjs`, `config.h` in this directory
  are original files written for this repo (derived in structure from
  qaudion-desktop's private `native/opus_addon.cc` / `binding.gyp` /
  `index.cjs` / `native/opus/config.h`, which are themselves original
  Node-API glue the owner's team wrote — NOT libopus source — audited before
  writing these copies to confirm they contain no call IDs, device IDs,
  hostnames, IPs, tokens or other private-repo internals; they don't, being
  a Node-API wrapper over a public codec library).

## Re-deriving these pins

If the phone builds (`m150/README.md`) ever re-pin `third_party/opus/src`,
update `DEFAULT_OPUS_COMMIT` in `fetch-opus-source.sh` and re-copy
`m150/fetch-opus-dnn-weights.sh` here, in the same change, so desktop and
phones never silently drift onto different Opus revisions.
