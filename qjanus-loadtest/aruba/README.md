# Load test against a production node (pre-minted token mode)

The normal harness (see `../README.md`) mints its Janus session tokens from the node's core token secret,
which therefore has to be a GitHub secret. A production node must not expose that secret and rotating it
would mean restarting the production server. This directory is the variant that never moves the secret:

| piece | where it runs | what it does |
| --- | --- | --- |
| `aruba_tool.py mint` | on the node | prints a JSON array of core session tokens, one per bot, expiry at most 3 h (2 h in the controller). The secret is read from `/etc/qjanus/qjanus.env`, never printed. |
| `aruba_tool.py rooms create / destroy / list` | on the node | creates the load-test rooms with the same ids, join tokens and room parameters as `qjanus-admin` (cross-checked against `src/ids.mjs` and `src/janus-admin.mjs` by `test/aruba.test.mjs`). The admin key stays on the node. |
| `aruba_tool.py gate` | on the node | one-shot "may the run start" check. |
| `aruba_tool.py guard` | on the node, during the run | watchdog, every 5 s. On a breach it destroys the load-test rooms at once and writes `ABORT`. It never touches a service. |
| `node-sampler.sh` (x2) | on the node | CPU / RSS / NIC / UDP of the SFU process and of the production server process. |
| `run-aruba.sh` | on the PC (Git Bash) | waits for the window and the gates, mints tokens into a repository secret, creates rooms, starts guard and samplers, dispatches the workflow, watches it, cancels it on an abort, cleans up and verifies. |

On the runner side the workflow input `session_tokens_secret_name` selects the mode: the secret holds the
token list, `QJANUS_TOKEN_SECRET` is not read, `run-shard.sh` unsets it, `qjanus-load` takes bot `(room k,
member i)` its token number `k * roomSize + i`, never refreshes a token and refuses to start when the list is
too short or a token would expire before the planned end of the run (plus a 120 s margin).

## Rules the run obeys

* Window: only between 23:30 and 06:00 Europe/Rome. The gate refuses outside it, the guard stops at 06:00.
* Gates before the run (every one fails closed, an unreadable value counts as closed): `bcrypto_active_calls`
  is 0, no foreign Janus room has participants (group call), no call line in the production journal in the
  last 15 minutes, load average 1 min <= 1.5, `/api/v1/health` answers 200, no room of this seed exists.
  Outside the window or with a closed gate the controller polls every 10 minutes (`MAX_WAIT_MIN`).
* Gates again before every scenario, and continuously (every 5 s) through the guard.
* Guard aborts: a real call starts (`bcrypto_active_calls` > 0, or `bcrypto_total_calls` or the `call_offer` /
  `call_answer` / `call_accepted` / `group*` message counters move), a foreign room appears or gets
  participants, load average 1 min > 3.5, `/api/v1/health` not 200 or slower than max(150 ms, 6 x the baseline)
  three samples in a row, bcrypto-server above 40 % of one core three samples in a row, the metrics
  unreadable, the Janus room list unreadable for a minute, the window over, or a `STOP` file (the normal end).
  A controller that dies leaves the guard running: it ends at 06:00 at the latest, destroying the rooms, and the samplers stop after 8 h.
* Never restarts or reloads anything; the controller only reads the node, creates / destroys its own rooms and
  starts / stops its own processes under `/root/aruba-loadtest`.
* Cleanup, also on Ctrl-C or any error, and VERIFIED: guard and samplers stopped, rooms destroyed, the three
  `ARUBA_*` repository secrets deleted (the token secret right after each scenario), the node work directory
  removed. The script prints `CLEANUP VERIFIED` or says what is left.

## Run

```
NODE=<ssh host> ARUBA_WS_URL=wss://<host>/janus REF=<branch with this mode> bash qjanus-loadtest/aruba/run-aruba.sh
NODE=<ssh host> DRY_GATE=1 bash qjanus-loadtest/aruba/run-aruba.sh      # evaluate the gates once, ignoring the window
touch /root/aruba-loadtest/guard/STOP                   # on the node: end a run cleanly and at once
```

Scenarios and ramps (override the last step with `RAMP_MAX_audio8=16` and so on): `audio8` ramp 2..24 rooms
(12 shards), `video4` 2..12 rooms (12 shards), `video8lite` 1..6 rooms (6 shards, lite profile), hold 60 s per
step. Results land in `$OUT/<scenario>/` (artifacts of the workflow run) and `$OUT/{janus,bcrypto}-sampler.csv`,
`guard.csv`, `ABORT.txt` (only after an abort). `window.txt` of a scenario holds its start / end epoch: slice the
sampler CSVs with it and pass them to `qjanus-load report --sampler` to get CPU, RSS and NIC per step.

## Checks of this directory

`test/aruba.test.mjs` (CI, job `unit`): tokens minted by the tool verify like Janus verifies them and equal the
Node minter byte for byte, the ids and room parameters equal the Node ones, room create / destroy / list work
against a fake Janus that checks every token, nothing secret reaches stdout or stderr. `test/aruba_tool_test.py`:
the gate, the guard and the window (summer and winter time) decisions. Smoke step `p` runs the whole harness
against the real Janus built by CI with tokens minted by `aruba_tool.py` and no token secret in the runner's
environment, and checks that no token reaches the output files.
