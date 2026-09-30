#!/usr/bin/env node
// qjanus-load: load-test CLI for the patched Janus VideoRoom SFU.
//   run    fixed number of rooms for a hold time (one step)
//   ramp   rooms grow step by step until a stop condition or the maximum
//   report merge shard output directories into report.json / report.md
//   plan   print the shard list [0,1,...,S-1] for a CI matrix
// Exit codes: 0 ok, 1 not ok (or failure), 2 usage/config error.

import { ConfigError, parseCli, scrubSecrets } from '../src/config.mjs';

const USAGE = `qjanus-load <run|ramp|report|plan> [flags]

Commands
  run     --rooms K            fixed K rooms for --hold-sec (single step)
  ramp    --ramp-start N --ramp-step N --ramp-max N   grow rooms until a stop condition
  report  --in DIR [--in DIR ...] [--sampler CSV] [--cpu-limit N] --out DIR
  plan    --shards S           prints [0,1,...,S-1]

Target (secrets come from the environment or --*-file, are never printed or written)
  --ws-url URL                 Janus WebSocket URL              [QJANUS_WS_URL]
  --token-secret-env NAME      env var holding the token secret [default QJANUS_TOKEN_SECRET]
  --token-secret-file FILE     read the token secret from a file instead
  --seed-env NAME              env var holding the seed         [default QJANUS_LOADTEST_SEED]
  --seed-file FILE             read the seed from a file instead
  --ice-servers-json JSON      RTCIceServer array               [QJANUS_ICE_SERVERS]
  --dtls-fingerprint FP        pin: "sha-256 AB:CD:..."         [QJANUS_DTLS_FINGERPRINT]

Scenario
  --scenario audio8|video4|video8      rooms of 8 audio bots / 4 video bots / 8 video bots (default audio8)
  --rooms K                    (run) rooms, default 1
  --ramp-start 1 --ramp-step 1 --ramp-max 20   (ramp) rooms per step
  --hold-sec 60 --settle-sec 10 --join-timeout-sec 60
  --join-rate 2                bots started per second per shard
  --leave-rate 10              bots stopped per second at the end
  --video-profile spec|lite|tiny   spec = spec 4.4 simulcast, lite = 1/4 sizes for CI, tiny = one 160x90@10 layer
  --simulcast lmh|lm|l|none    published layers (default lmh)
  --e2ee aesgcm|xor|none       frame transform (default aesgcm)
  --expect-transport strict|off    enforce DTLS 1.3 / AES-256-GCM per PC (default strict)
  --substream 1 --speaker-substream 2 --temporal 2   requested layers (speaker = member 0 of the room)

Sharding
  --shard i/S                  this process runs the rooms k with k % S == i (default 0/1)
  --start-at EPOCH_SEC         all shards wait for this instant so their steps stay in lock step

Rooms and server metrics
  --manage-rooms --admin-url URL --admin-key-env NAME   create the shard's rooms before, destroy them after
                                                        [QJANUS_ADMIN_URL, default key env QJANUS_ADMIN_KEY]
  --admin-key-file FILE        read the admin key from a file instead
  --cpu-file CSV | --cpu-cmd "cmd"    Janus CPU from the sampler CSV, or from a command printing a number
                                      or a JSON object with cpu_pct (not both)
  --cpu-limit 180              Janus CPU limit, percent of ONE core (200 = the whole quota)

Stop conditions
  --loss-limit-pct 1           subscriber loss above this for --breach-windows consecutive 10 s windows
  --breach-windows 2
  --freeze-tolerance 0         freeze events tolerated in the measurement window
  --join-fail-limit-pct 5      (failed + not steady bots) / attempted

Tokens
  --token-ttl-sec 600 --token-refresh-sec 300   session token lifetime and refresh period

Browser
  --bots-per-browser 16 --bots-per-page 1
  --field-trials "WebRTC-EnableDtlsPqc/Enabled/WebRTC-LegacySimulcastLayerLimit/Disabled/"   Chromium field trials ("" = none);
                               the second one lets small captures (lite/tiny) send all 3 simulcast layers
  --chromium-arg=--flag        extra Chromium flag, repeatable (use the = form)
  --chromium-path PATH         Chromium executable
  --headed                     show the browser windows

Output
  --out DIR                    default out/<run-id> (out/<run-id>/shard-<i> with several shards)
  --run-id ID                  default <timestamp>-<scenario>
  --duration-cap-sec N         stop the run when it would exceed N seconds
  --quiet                      no progress lines
  --help
`;

const shortMs = (v) => (v === null || v === undefined ? '-' : String(v));

async function runLoad(parsed) {
  const { cfg } = parsed;
  const { BrowserPoolHost } = await import('../src/browser-pool.mjs');
  const { runShard } = await import('../src/orchestrator.mjs');

  const log = (kind, line) => {
    if (kind === 'warn') console.error(line);
    else if (!cfg.quiet) console.log(line);
  };
  const host = new BrowserPoolHost({
    botsPerBrowser: cfg.botsPerBrowser,
    botsPerPage: cfg.botsPerPage,
    headed: cfg.headed,
    chromiumPath: cfg.chromiumPath,
    fieldTrials: cfg.fieldTrials,
    extraArgs: cfg.chromiumArgs,
    log: (level, msg) => log(level, scrubSecrets(msg, cfg)),
  });

  const abort = new AbortController();
  let signals = 0;
  for (const sig of ['SIGINT', 'SIGTERM']) {
    process.on(sig, () => {
      if (++signals > 1) process.exit(130);
      console.error(`qjanus-load: ${sig} received, stopping gracefully (send it again to force)`);
      abort.abort();
    });
  }

  const summary = await runShard(cfg, host, { signal: abort.signal, log });
  const max = summary.maxSustainable;
  console.log(`qjanus-load: finished ok=${summary.ok} stopReason=${summary.stopReason} maxSustainable=${max ? `${max.rooms} rooms/${max.participants} participants` : 'none'} steps=${summary.steps.length} joinP95Ms=${shortMs(summary.join.p95Ms)} out=${cfg.out}`);
  return summary.ok ? 0 : 1;
}

async function runReport(parsed) {
  const { writeReport } = await import('../src/report.mjs');
  const r = parsed.report;
  const report = await writeReport({ inDirs: r.inDirs, sampler: r.sampler, out: r.out, cpuLimit: r.cpuLimit });
  const max = report.maxSustainable;
  console.log(`qjanus-load: report written to ${r.out} (report.json, report.md); maxSustainable=${max ? `${max.rooms} rooms/${max.participants} participants` : 'none'} ok=${report.ok}`);
  return 0;
}

async function main() {
  let parsed;
  try {
    parsed = parseCli(process.argv.slice(2));
  } catch (err) {
    if (err instanceof ConfigError) {
      console.error(`qjanus-load: ${err.message}\nRun "qjanus-load --help" for usage.`);
      return 2;
    }
    throw err;
  }
  try {
    switch (parsed.command) {
      case 'help':
        console.log(USAGE);
        return 0;
      case 'plan':
        console.log(JSON.stringify(Array.from({ length: parsed.shards }, (_, i) => i)));
        return 0;
      case 'report':
        return await runReport(parsed);
      default:
        return await runLoad(parsed);
    }
  } catch (err) {
    console.error(`qjanus-load: ${scrubSecrets(err && err.message, parsed.cfg)}`);
    return 1;
  }
}

main().then((code) => {
  process.exitCode = code;
  // Playwright leaves no handles after close(), but never hang a CI job on a stray one.
  setTimeout(() => process.exit(code), 5000).unref();
});
