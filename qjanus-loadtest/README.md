# qjanus-loadtest

Load-test harness for the patched Janus VideoRoom SFU ("qjanus", spec GROUP_CALLS_V2 sections 3, 4, 6, 10).
It answers one question: **how many rooms / participants of a given kind can one qjanus node sustain
before Janus CPU, packet loss or video freezes cross the limits?**

```
 bot shards (GitHub runners or your machines)                        the qjanus node
 +--------------------------------------------+                 +---------------------------+
 | qjanus-load  (Node)                        |   wss (Caddy)   |  Janus VideoRoom          |
 |  +- Chromium x M  (fake camera/mic)        | --------------> |  websockets 127.0.0.1:8188|
 |      +- page x P  (window.qbot)            |   SRTP / UDP    |  http       127.0.0.1:8088|
 |          +- bot x B: 1 WS session          | <-------------> |                           |
 |             publisher PC + subscriber PC   |                 |  node-sampler.sh (CSV)    |
 +--------------------------------------------+                 +---------------------------+
        ^ rooms are created beforehand with qjanus-admin (HTTP API is node-local) ----------^
```

* `bin/qjanus-load.mjs` : `run` (fixed number of rooms), `ramp` (grow until a stop condition), `report` (merge shards + sampler), `plan`.
* `bin/qjanus-admin.mjs` : room/admin helper. Talks to the Janus **HTTP** transport exactly like the bcrypto-lite server will: a
  signed-token session, `videoroom create` with the spec section 3 parameters, `allowed` join tokens, `kick`, `destroy`.
* `scripts/node-sampler.sh` (+ `sampler-summary.sh`) : runs **on the SFU host**, CSV every 2 s.
* `page/` : the in-browser bot (Janus WebSocket client, 2 PeerConnections, SDP rules of spec 4.4/10, frame transform, stats).
* `.github/workflows/qjanus-loadtest.yml` : unit tests, a **smoke run against a locally built Janus** (every push to branch
  `qjanus-loadtest`), and a sharded **remote** mode for the real node.

Nothing here deploys anything or touches production: it only opens Janus sessions with tokens you provide.

## What a bot does

Per participant (spec section 4):

* 1 WebSocket (`janus-protocol`) session with the signed session token on **every** request, keepalive every 25 s.
* **Publisher PC**: Opus 60 ms / 32 kbps CBR (`ptime=60`, `cbr=1`, `maxaveragebitrate=32000`, applied to the local offer *and* to
  Janus' answer, because libwebrtc takes the send-side ptime from the remote description), optional VP8 simulcast `l/m/h`
  (spec profile: 320x180@15 150 kbps, 640x360@20 450 kbps, 1280x720@25 1200 kbps). Chromium fake camera/microphone.
* **Subscriber PC** (multistream): every other member of the room; new/leaving publishers go through **one serialized
  renegotiation queue** (150 ms debounce, never two in flight); per remote video the requested substream (`--substream`,
  the room's "speaker" = member 0 gets `--speaker-substream`).
* **E2EE-like frame transform** (`RTCRtpScriptTransform` in a worker): AES-256-GCM with the spec 5.3 frame layout
  (`header | ciphertext | tag16 | iv12 | [12, keyIndex]`, HKDF-SHA256 key, Opus 1 clear byte, VP8 10/3 clear bytes so Janus can
  still see key frames and simulcast layers). `--e2ee xor` is a cheaper transform, `none` disables it.
* SDP rules: only `mid`, `rid`, `repaired-rid` and transport-wide-cc header extensions stay (spec section 10: Janus 1.4.2 has no cryptex).
* **Transport self-check** on both PCs (spec 4.5): `tlsVersion` must be `FEFC`, `dtlsCipher` `TLS_AES_256_GCM_SHA384`,
  `srtpCipher` `AEAD_AES_256_GCM` (or `SRTP_AEAD_AES_256_GCM`). A violation fails the bot and the whole run (`transport` is not a
  capacity result). Chromium runs with `--force-fieldtrials=WebRTC-EnableDtlsPqc/Enabled/...` (ML-KEM DTLS).
* Optional DTLS pin (spec 4.4): `--dtls-fingerprint "sha-256 AB:CD:..."`.
* Recorded per bot: WS/create/join/publish times, ICE and DTLS time of both PCs, time to first audio/video, time until every peer
  delivers media, transport stats, packets/bytes/loss/jitter/NACK/PLI, freeze count/duration, concealment, layer stats, decrypt counters.

Many lightweight bots per browser: `--bots-per-browser` (default 16) Chromium pages share one browser process, `--bots-per-page`
(default 1) bots share one page (and one E2EE worker).

## Deterministic ids: no manifest to pass around

Everything about a room (room id, room secret, participant pseudonyms, join tokens, the shared test E2EE key) is derived from a
**seed** with HMAC-SHA256 (`src/ids.mjs`). Rooms are `k = 0, 1, 2 ...`, members `i = 0 .. N-1`. The operator creates rooms with
the seed; every bot shard, on any machine, derives the same ids from the same seed. Room `k` runs on shard `k mod S`.

## Environment variables and secrets

| Name | What | Where it is needed |
| --- | --- | --- |
| `QJANUS_WS_URL` | Janus WebSocket URL (`wss://<node>/janus`, or `ws://127.0.0.1:8188/janus`) | bot shards |
| `QJANUS_TOKEN_SECRET` | core `token_auth_secret` (HMAC key of the signed session tokens) | bot shards, admin |
| `QJANUS_LOADTEST_SEED` | seed of the deterministic ids (any string, e.g. `openssl rand -hex 16`) | bot shards, admin |
| `QJANUS_ADMIN_URL` | Janus HTTP base, `http://127.0.0.1:8088/janus` (node-local) | admin (and `--manage-rooms`) |
| `QJANUS_ADMIN_KEY` | videoroom plugin `admin_key` | admin |
| `QJANUS_ICE_SERVERS` | JSON array of RTCIceServer (only if the node is not reachable directly) | bot shards, optional |
| `QJANUS_DTLS_FINGERPRINT` | `sha-256 AB:CD:...` DTLS pin | bot shards, optional |

Secrets are read from the environment (or `--*-file`), never printed, never written to output files, never passed on a command
line by the workflow. Outputs contain only `targetId` = first 12 hex of `sha256(url)`; no URL, token, IP or candidate address.

**GitHub repository secrets for the remote workflow** (create them yourself in *Settings > Secrets and variables > Actions*;
nothing in this repo creates a secret): `QJANUS_WS_URL`, `QJANUS_TOKEN_SECRET`, `QJANUS_LOADTEST_SEED`. The secret *names* are workflow
inputs, so another target can use other names. The repo is public: **workflow inputs are visible in public run logs**, so keep
the URL in the secret and leave the `target_ws_url` input empty (it is only a fallback and is masked after it has been read).

## Running against a real node, step by step

Sizes: `audio8` = rooms of 8 audio-only bots, `video4` = rooms of 4 with video, `video8` = rooms of 8 with video.
Create as many rooms as the ramp will reach (`--ramp-max`), sized like the scenario (8 / 4 / 8 seats).

1. **Pick a seed** (once per test, kept secret): `export QJANUS_LOADTEST_SEED=$(openssl rand -hex 16)`.
2. **Create the rooms** on the node (or through an SSH tunnel to its loopback HTTP port). Needs the token secret and the admin key
   (both live in the node's `/etc/qjanus/qjanus.env`):
   ```bash
   cd qjanus-loadtest && npm ci
   export QJANUS_ADMIN_URL=http://127.0.0.1:8088/janus QJANUS_TOKEN_SECRET=... QJANUS_ADMIN_KEY=...
   node bin/qjanus-admin.mjs info                                   # sanity: the node answers
   node bin/qjanus-admin.mjs create-rooms --rooms 40 --size 8       # audio8 / video8; use --size 4 for video4
   ```
   Rooms are created like the server does (private, `require_e2ee`, `require_pvtid`, `allowed` = the join tokens of the members,
   VP8/Opus, 1.5 Mbit/s cap...). `create-rooms` is idempotent.
3. **Start the sampler on the node** (no root needed; stop it with SIGTERM or `--duration`):
   ```bash
   bash scripts/node-sampler.sh --out /tmp/qjanus-sampler.csv --latest /tmp/qjanus-latest.json --quiet &
   ```
   It finds the `qjanus` (or `janus`) process by name; `--pid N` if needed. Columns: `ts,iso,cpu_pct,rss_mb,threads,total_cpu_pct,
   load1,rx_mbps,tx_mbps,rx_drop_d,tx_drop_d,udp_in_pps,udp_out_pps,udp_in_errors_d,udp_rcvbuf_errors_d,udp_sndbuf_errors_d,
   cg_throttled_ms_d,pid` (`cpu_pct` is percent of ONE core: 200 = the whole `CPUQuota=200%`; `cg_throttled_ms_d` is the cgroup
   throttling, the truest saturation signal under a CPU quota).
4. **Run the bots.** Either
   * **from GitHub Actions** (many machines): *Actions > qjanus-loadtest > Run workflow* with `mode=remote`, `scenario`, `run_mode=ramp`,
     `ramp_start/step/max`, `hold_sec`, `shards` (N runners, room `k` on shard `k mod N`), `video_profile`. The `plan` job sets a common
     start instant (`start_delay_sec`, default 240 s: runner start-up must fit in) so all shards step in lock step; every shard
     uploads `shard-<n>`; the `report` job merges them into `report.md`/`report.json` (artifact `report` + job summary).
     The workflow can only be dispatched from the default branch once the file is merged there.
   * **from your own machine(s)**: `export QJANUS_WS_URL=wss://<node>/janus` and
     ```bash
     node bin/qjanus-load.mjs ramp --scenario audio8 --ramp-start 2 --ramp-step 2 --ramp-max 40 --hold-sec 60 \
       --cpu-cmd "ssh <node> cat /tmp/qjanus-latest.json" --out out/audio8
     # several machines: add --shard 0/3 --start-at <epoch-seconds-in-the-future> on each, same flags otherwise
     ```
     `--cpu-cmd` runs a command printing a number or the sampler's `latest.json` and makes **Janus CPU > 180 %** an online stop
     condition (`--cpu-file` does the same for a local CSV). Without either, the run still stops on loss / freezes / join
     failures and the CPU limit is applied afterwards by `report` (next step).
5. **Stop the sampler and merge**:
   ```bash
   bash scripts/sampler-summary.sh /tmp/qjanus-sampler.csv          # min/avg/p50/p95/max per column (+ --from/--to/--json)
   node bin/qjanus-load.mjs report --in out/shard-0 --in out/shard-1 --sampler /tmp/qjanus-sampler.csv --out out/report
   ```
   `report.md` has one row per step (rooms, participants, bots ok/failed, join p50/p95, ICE/DTLS p95, loss, freezes, client CPU,
   Janus CPU avg/max, RSS, NIC Mbps, UDP drops, verdict), the **max sustainable rooms/participants** (last step where every shard
   was ok *and* the sampler's Janus CPU max stayed <= `--cpu-limit`), the first breach reason and a "limits of this result" section.
6. **Clean up**: `node bin/qjanus-admin.mjs destroy-rooms --rooms 40`.

`qjanus-load run --rooms K` (fixed load for `--hold-sec`) works the same way as `ramp` with a single step.

### Stop conditions (evaluated per step on the measurement window, after `--settle-sec`)

| Reason | Default | Definition |
| --- | --- | --- |
| `cpu` | `--cpu-limit 180` | Janus process CPU max in the window > 180 (percent of one core; = 90 % of the 200 % quota) |
| `loss` | `--loss-limit-pct 1` | subscriber-side loss `lost/(lost+received)` over audio+video > 1 % for `--breach-windows 2` consecutive 10 s windows |
| `freeze` | `--freeze-tolerance 0` | video freeze events (Chromium `freezeCount`) in the window > tolerance |
| `join` | `--join-fail-limit-pct 5` | (failed + not-steady bots) / attempted bots > 5 % |
| `transport` | - | any DTLS/SRTP policy violation: fatal, not a capacity result |

A breach stops the ramp; **max sustainable = the last step without breach**. If the load generator itself was saturated
(client CPU > 90 %) the step verdict is `inconclusive_client_saturated` instead of a capacity result: add shards or lower
`--bots-per-browser`.

## Local smoke run (CI does this on every push)

The smoke job builds Janus like production (BoringSSL f91f1447, libsrtp 2.8.1, libnice 0.1.24, libwebsockets without TLS, Janus
v1.4.2 + the DTLS 1.3 / ML-KEM policy patch; `smoke/`, `scripts/build-*.sh`), starts it with fresh random secrets, and drives it
with the real harness: access-control negatives (bad/expired/foreign token, admin key, join tokens, kick), `audio8`, `video4`,
`video8` runs, two ramps (one ends on `max_rooms`, one on the CPU limit), two concurrent shards + merged report, `--manage-rooms`,
the sampler and its summarizer. On Linux with the same prerequisites: `bash scripts/build-deps.sh && bash scripts/build-janus.sh &&
bash scripts/smoke.sh` (`SMOKE_ONLY=a,d` re-runs single steps).

Unit tests (no Janus): `npm ci && npx playwright install chromium && npm test` (about 6 minutes on a slow machine; the browser tests
skip themselves without Chromium), `bash test/sampler.test.sh`.

## Output of a shard (`--out DIR`)

`run.json` (config without secrets, versions, host), `steps.jsonl` (one row per step), `bots.jsonl` (final per-bot record),
`timeseries.jsonl` (one row per poll, summed over the shard's bots), `events.log`, `summary.json` (`ok`, `stopReason`,
`maxSustainable`, steps, transport histogram, join percentiles, media totals, client CPU, error groups).

## Choosing bot machines

Headless Chromium with fake media is heavy, video far more than audio: a GitHub `ubuntu-latest` runner (4 vCPU) carries a few
dozen audio bots but only a handful of full-spec simulcast video bots (each 3-layer 720p VP8 encode plus N-1 decodes).
Use `--video-profile lite|tiny` on small machines, look at `clientCpu` in `steps.jsonl`, and add shards until it stays below
90 %. Bots of one room always live on one shard. The harness measures its own CPU precisely so that a saturated generator is
never mistaken for a saturated SFU.

## Limits of this harness (read before believing a number)

* **Fake media, one shared test key per room.** Real clients have per-sender, per-epoch key rings and native FrameCryptor; the
  bots only reproduce the frame layout and the AES-GCM cost, not key distribution or rotation (epoch bumps, kicks, rekey storms).
* **No churn model** beyond the ramp: no repeated join/leave, no ICE restarts, no network change, no WebSocket reconnect, no
  `group_call_media_*` server flow (the bots use the tokens directly), no TURN by default (`QJANUS_ICE_SERVERS` for a relayed path).
* **Layer policy is fixed** (`--substream`, speaker substream), not the adaptive client policy of spec 4.6, so downlink is
  steady rather than self-limiting.
* **Loss is measured at the subscribers only** (`packetsLost` of inbound RTP); uplink loss towards Janus shows up as
  publisher-side NACK/PLI and remote-inbound reports but is not part of the stop condition. RTX/NACK recovered packets do not count as loss.
* **Freeze detection** relies on Chromium's `freezeCount` of decoded video (`render: attach`); with software decoding on an
  overloaded generator freezes can be the client's fault (hence the saturation flag).
* **Network path.** Runners reach the node over the public Internet; the measured limit includes Caddy/TLS for signalling but
  the media path is plain UDP. Runner egress is not a controlled network: loss caused by the path shows up as loss.
* **CPU stop online only with a CPU source** (`--cpu-file`/`--cpu-cmd`). GitHub runners cannot read the node, so in remote
  mode the CPU limit is applied post-hoc by `report --sampler`; the run itself continues past a CPU breach until loss/freeze/join stop it or
  `--ramp-max` is reached, so keep `--ramp-max` sane.
* Shards synchronize by a start instant, not by a control channel: a shard that starts late reports `lateStartSec`; a breach seen
  by one shard does not stop the others (they see the same node and normally breach in the same step).
* The Janus token **TTL is refreshed by the harness** (`--token-ttl-sec 600 --token-refresh-sec 300`), see the first finding below.
* `--e2ee none` needs rooms created without `require_e2ee` (the qjanus rooms have it on); `--simulcast none|l` bots publish a
  single layer.

## Findings from building this against the Janus 1.4.2 source (relevant to the spec)

1. **Signed tokens are checked on every request, including keepalive.** With `token_auth` on, `janus_request_check_secret` runs for every
   session-level request. A `session_token` with `ttl_s: 600` therefore stops working 600 s after it was minted for a call that is still running: the
   clients need a refresh path (new token pushed by the server, or a longer TTL) or every keepalive/`configure`/`trickle` after 10 minutes is answered
   with error 403. The harness models the refresh (`setTokens`), `--token-refresh-sec 0` reproduces the failure.
2. **The E2EE flag is on the JSEP** (`{type:"offer", sdp, e2ee:true}`), not in the `publish` body as spec 4.2 says. Rooms with `require_e2ee` reject a publish without it.
3. The videoroom subscriber `private_id` is an **integer**; ids/rooms are strings only with `string_ids=true`.
4. Chromium <= 148 needs the `WebRTC-LegacySimulcastLayerLimit/Disabled/` trial to send 3 simulcast layers below 960x540 capture (the harness sets it by default; irrelevant for real 720p sources).
5. `list` shows private rooms only when the `admin_key` is sent.
