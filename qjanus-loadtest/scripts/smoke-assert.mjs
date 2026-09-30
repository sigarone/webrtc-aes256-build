#!/usr/bin/env node
// Assertions over the outputs of scripts/smoke.sh.
//
//   node scripts/smoke-assert.mjs --out DIR [--steps negative,a,b,c,d,e,f,g,i,h]
//                                 [--janus-log FILE] [--json FILE]
//
// DIR holds what smoke.sh wrote (a/ b/ c/ d/ e/ f0/ f1/ g/ i0/ i1/ h/ report/ report-i/ sampler.csv
// negative.json rooms-after-g.json). --steps lists the steps that were run
// (default: all); the checks of the others are skipped. Every check is printed as
// PASS/FAIL, all checks run, the exit code is 1 if any failed.
//
// Steps: a = audio8 run, b = video4 run (lite), c = video8 run (tiny), d = audio8
// ramp to 2 rooms, e = audio8 ramp that must stop on the CPU limit, f = two shards
// of one 2-room run + merged report, g = audio8 run with --manage-rooms, i = the
// workflow's remote-mode shards (scripts/run-shard.sh, 2 shards, ramp 1..2 rooms) +
// merged report-i, h = audio8 ramp under 6 % injected loopback UDP loss that must stop on loss.
// The Janus log must show only fully compliant transport negotiations. Secret
// values (read from the environment, if present) must not appear in any file.

import { existsSync, readdirSync, readFileSync, statSync, writeFileSync } from 'node:fs';
import path from 'node:path';
import { parseArgs } from 'node:util';
import { parseSamplerCsv } from '../src/cpu-source.mjs';

const { values: flags } = parseArgs({
  options: {
    out: { type: 'string' },
    steps: { type: 'string', default: 'negative,a,b,c,d,e,f,g,i,h' },
    'janus-log': { type: 'string' },
    json: { type: 'string' },
  },
  allowPositionals: false,
});
if (!flags.out) {
  console.error('smoke-assert: --out DIR is required');
  process.exit(2);
}
const OUT = flags.out;
const STEPS = new Set(flags.steps.split(',').map((s) => s.trim()).filter(Boolean));

// ------------------------------------------------------------------ plumbing

const results = [];
async function check(name, fn) {
  try {
    await fn();
    results.push({ name, ok: true });
    console.log(`PASS ${name}`);
  } catch (err) {
    const detail = String(err && err.message ? err.message : err).slice(0, 400);
    results.push({ name, ok: false, detail });
    console.log(`FAIL ${name}: ${detail}`);
  }
}
const expect = (cond, message) => {
  if (!cond) throw new Error(message);
};
const isNum = (v) => typeof v === 'number' && Number.isFinite(v);
const at = (...p) => path.join(OUT, ...p);

function readText(file) {
  if (!existsSync(file)) throw new Error(`missing file ${path.relative(OUT, file)}`);
  return readFileSync(file, 'utf8');
}
const readJson = (file) => JSON.parse(readText(file));
const readJsonl = (file) => readText(file).split(/\r?\n/).filter((l) => l.trim() !== '').map((l) => JSON.parse(l));

// -------------------------------------------------------------- run directories

const SHARD_FILES = ['run.json', 'steps.jsonl', 'bots.jsonl', 'timeseries.jsonl', 'events.log', 'summary.json'];
const STRICT_TLS = ['FEFC'];
const STRICT_DTLS_CIPHER = ['TLS_AES_256_GCM_SHA384'];
const STRICT_SRTP = ['AEAD_AES_256_GCM', 'SRTP_AEAD_AES_256_GCM'];

/** Every key of a { value: count } dictionary must be one of `allowed`, and there must be at least one. */
function onlyKeys(dict, allowed, what) {
  const keys = Object.keys(dict || {});
  expect(keys.length > 0, `${what}: nothing was negotiated`);
  const bad = keys.filter((k) => !allowed.includes(k));
  expect(bad.length === 0, `${what}: unexpected value(s) ${bad.join(', ')}`);
}

function outputFilesPresent(dir) {
  for (const f of SHARD_FILES) {
    const p = path.join(dir, f);
    expect(existsSync(p) && statSync(p).size > 0, `${path.basename(dir)}/${f} is missing or empty`);
  }
}

/** The checks every plain `run` directory must pass. */
function runChecks(dir, { attempted, mode = 'run' }) {
  const name = path.basename(dir);
  outputFilesPresent(dir);
  const s = readJson(path.join(dir, 'summary.json'));
  expect(s.ok === true, `${name}: summary.ok is ${s.ok} (stopReason ${s.stopReason}, errors ${JSON.stringify(s.errors ?? []).slice(0, 200)})`);
  expect(s.mode === mode, `${name}: mode ${s.mode}, wanted ${mode}`);
  expect(s.bots.attempted === attempted, `${name}: ${s.bots.attempted} bots attempted, wanted ${attempted}`);
  expect(s.bots.failed === 0, `${name}: ${s.bots.failed} bots failed`);
  expect(s.bots.steady === attempted, `${name}: ${s.bots.steady} of ${attempted} bots reached steady`);
  expect(s.bots.transportViolations === 0, `${name}: ${s.bots.transportViolations} transport violations`);
  expect(s.transport.policyOk === true, `${name}: transport.policyOk is ${s.transport.policyOk}`);
  onlyKeys(s.transport.tlsVersion, STRICT_TLS, `${name}: tlsVersion`);
  onlyKeys(s.transport.dtlsCipher, STRICT_DTLS_CIPHER, `${name}: dtlsCipher`);
  onlyKeys(s.transport.srtpCipher, STRICT_SRTP, `${name}: srtpCipher`);
  return s;
}

function mediaChecks(dir, s, { video }) {
  const name = path.basename(dir);
  expect(isNum(s.media.lossPct) && s.media.lossPct < 1, `${name}: media.lossPct is ${s.media.lossPct}, wanted < 1`);
  const p50 = s.media.audioOutPps?.p50;
  expect(isNum(p50) && p50 >= 12 && p50 <= 22, `${name}: audioOutPps.p50 is ${p50}, wanted 12..22 (60 ms packets = 16.7 pps)`);
  const bots = readJsonl(path.join(dir, 'bots.jsonl'));
  expect(bots.length === s.bots.attempted, `${name}: bots.jsonl has ${bots.length} rows, wanted ${s.bots.attempted}`);
  const fails = bots.reduce((a, b) => a + (b.derived?.e2ee?.fail ?? 0), 0);
  const decs = bots.reduce((a, b) => a + (b.derived?.e2ee?.dec ?? 0), 0);
  expect(fails === 0, `${name}: ${fails} E2EE-like decrypt failures`);
  expect(decs > 0, `${name}: no E2EE-like frame was ever decrypted (transform not running?)`);
  const steps = readJsonl(path.join(dir, 'steps.jsonl'));
  expect(steps.length === 1, `${name}: ${steps.length} step rows, wanted 1`);
  const cpu = steps[0].janusCpu;
  expect(cpu && cpu.source === 'file', `${name}: janusCpu.source is ${cpu && cpu.source}, wanted file`);
  expect(isNum(cpu.maxPct) && cpu.maxPct > 0, `${name}: Janus CPU from the sampler is ${cpu.maxPct}, wanted > 0`);
  if (video) {
    const fps = steps[0].window?.framesDecodedPerSec;
    expect(isNum(fps) && fps > 0, `${name}: framesDecodedPerSec is ${fps}, no video was decoded`);
  }
}

// ---------------------------------------------------------------- the checks

const skip = (step, why) => console.log(`SKIP ${step}: ${why}`);

if (STEPS.has('negative')) {
  await check('negative: every access-control check passed', async () => {
    const n = readJson(at('negative.json'));
    expect(n.failed === 0, `${n.failed} of ${n.passed + n.failed} negative checks failed: ${n.checks.filter((c) => !c.ok).map((c) => c.name).join('; ')}`);
    expect(n.passed >= 20, `only ${n.passed} negative checks ran`);
  });
} else skip('negative', 'not run');

const RUNS = [
  ['a', 'a: audio8 run, 1 room', { attempted: 8 }],
  ['b', 'b: video4 run, 1 room, lite', { attempted: 4, video: true }],
  ['c', 'c: video8 run, 1 room, tiny', { attempted: 8, video: true }],
  ['g', 'g: audio8 run with --manage-rooms', { attempted: 8 }],
];
for (const [id, label, o] of RUNS) {
  if (!STEPS.has(id)) {
    skip(id, 'not run');
    continue;
  }
  await check(`${label}: ok, all bots steady, strict transport`, () => {
    const s = runChecks(at(id), o);
    expect(s.stopReason === null, `${id}: stopReason ${s.stopReason}, wanted null`);
  });
  await check(`${label}: loss, packet rate (60 ms), E2EE, Janus CPU`, () => {
    mediaChecks(at(id), readJson(at(id, 'summary.json')), o);
  });
}
if (STEPS.has('b')) {
  // timeseries rows: { t, step, bots, up, steady, d: { aIn, vIn, aOut, vOut }, g: { vWidthMin, vWidthMax, ... }, ... }
  const series = () => readJsonl(at('b', 'timeseries.jsonl'));
  const maxOf = (rows, pick) => Math.max(...rows.map(pick).filter(isNum));
  await check('b: layer selection works (widest received layer > narrowest, simulcast lmh)', () => {
    const rows = series();
    const wMax = maxOf(rows, (r) => r.g?.vWidthMax);
    const wMin = maxOf(rows, (r) => r.g?.vWidthMin);
    expect(Number.isFinite(wMax) && Number.isFinite(wMin), 'vWidthMin / vWidthMax were never reported');
    expect(wMax > wMin, `max vWidthMax ${wMax} is not greater than max vWidthMin ${wMin}: every subscriber got the same layer`);
  });
  // A shared CI runner can stall a renderer for a few hundred ms, and Chromium then reports a freeze: the
  // smoke tolerates a handful (the harness stop condition itself needs freezes in consecutive windows).
  await check('b: video was decoded and (almost) never froze', () => {
    const rows = series();
    const sum = (pick) => rows.reduce((a, r) => a + (isNum(pick(r)) ? pick(r) : 0), 0);
    const decoded = sum((r) => r.d?.vIn?.framesDecoded);
    const freezes = sum((r) => r.d?.vIn?.freezeCount);
    expect(decoded > 0, `framesDecoded total is ${decoded}`);
    expect(freezes <= 10, `freezeCount total is ${freezes}, wanted at most 10 on an idle runner`);
  });
}

if (STEPS.has('g')) {
  await check('g: the harness destroyed the rooms it created', () => {
    const after = readJson(at('rooms-after-g.json'));
    expect(Array.isArray(after.rooms) && after.rooms.length === 0, `${after.rooms?.length} room(s) left after the run`);
  });
}

if (STEPS.has('d')) {
  await check('d: audio8 ramp 1..2 rooms stops at max_rooms with 2 rooms sustainable', () => {
    const s = runChecks(at('d'), { attempted: 16, mode: 'ramp' });
    expect(s.stopReason === 'max_rooms', `stopReason ${s.stopReason}, wanted max_rooms`);
    expect(s.maxSustainable && s.maxSustainable.rooms === 2, `maxSustainable ${JSON.stringify(s.maxSustainable)}, wanted rooms 2`);
    expect(s.steps.length === 2, `${s.steps.length} steps, wanted 2`);
    const bad = s.steps.filter((st) => st.verdict !== 'ok').map((st) => `step ${st.step}: ${st.verdict} ${st.breachReasons.join(',')}`);
    expect(bad.length === 0, `steps not ok: ${bad.join('; ')}`);
  });
} else skip('d', 'not run');

if (STEPS.has('i')) {
  await check('i: remote-mode shards (run-shard.sh) ramp 1..2 rooms, each shard only its own rooms', () => {
    // 2 shards, ramp 1..2: step 1 = room 0 (shard 0), step 2 adds room 1 (shard 1); room k belongs to shard k % 2
    const wantRooms = [[1, 1], [0, 1]];             // shardRooms per step, for shard 0 and shard 1
    const rooms = [];
    for (const i of [0, 1]) {
      const dir = at(`i${i}`);
      const s = runChecks(dir, { attempted: 8, mode: 'ramp' });
      expect(s.shard.index === i && s.shard.count === 2, `i${i}: shard ${JSON.stringify(s.shard)}, wanted ${i}/2`);
      expect(s.stopReason === 'max_rooms', `i${i}: stopReason ${s.stopReason}, wanted max_rooms`);
      expect(s.maxSustainable && s.maxSustainable.rooms === 2 && s.maxSustainable.participants === 16, `i${i}: maxSustainable ${JSON.stringify(s.maxSustainable)}, wanted 2 rooms / 16 participants`);
      expect(s.steps.length === 2, `i${i}: ${s.steps.length} steps, wanted 2`);
      const own = s.steps.map((st) => st.shardRooms);
      expect(JSON.stringify(own) === JSON.stringify(wantRooms[i]), `i${i}: shardRooms per step ${JSON.stringify(own)}, wanted ${JSON.stringify(wantRooms[i])}`);
      rooms.push(new Set(readJsonl(path.join(dir, 'bots.jsonl')).map((b) => b.room)));
    }
    expect(rooms[0].size === 1 && rooms[1].size === 1, `each shard must drive exactly 1 room (got ${rooms[0].size} and ${rooms[1].size})`);
    expect(!rooms[1].has([...rooms[0]][0]), 'both shards drove the same room');
  });
  await check('i: merged report-i covers both shards: max sustainable 2 rooms / 16 participants', () => {
    const r = readJson(at('report-i', 'report.json'));
    expect(r.expectedShards === 2 && r.missingShards.length === 0, `shards ${JSON.stringify({ expected: r.expectedShards, missing: r.missingShards })}`);
    expect(r.bots.attempted === 16 && r.bots.failed === 0, `bots ${JSON.stringify(r.bots)}`);
    expect(r.samplerUsed === true && r.samplerRows > 0, 'the sampler CSV was not used by the report');
    expect(r.maxSustainable && r.maxSustainable.rooms === 2 && r.maxSustainable.participants === 16, `maxSustainable ${JSON.stringify(r.maxSustainable)}, wanted 2 rooms / 16 participants`);
    expect(r.ok === true, 'report.ok is not true');
    const md = readText(at('report-i', 'report.md'));
    expect(md.startsWith('# qjanus load test report') && md.length > 300, 'report.md is empty or has no title');
  });
} else skip('i', 'not run');

if (STEPS.has('h')) {
  await check('h: under 6 % loopback UDP loss the ramp stops on loss at the first step', () => {
    // every bot still completes DTLS 1.3 and reaches steady (runChecks), so 'join' is not the reason
    const s = runChecks(at('h'), { attempted: 8, mode: 'ramp' });
    const st = s.steps[0];
    expect(s.stopReason === 'loss', `stopReason ${s.stopReason} (breach reasons ${JSON.stringify(st?.breachReasons)}), wanted loss`);
    expect(s.steps.length === 1, `${s.steps.length} steps, wanted 1`);
    expect(s.maxSustainable === null, `maxSustainable ${JSON.stringify(s.maxSustainable)}, wanted null`);
    expect(st.checks.loss.breached === true && st.checks.loss.value > 1, `loss check ${JSON.stringify(st.checks.loss)}, wanted breached with value > 1`);
    expect(st.checks.cpu.breached === false, `cpu check breached (${JSON.stringify(st.checks.cpu)}): CPU must not be the reason`);
    expect(st.checks.join.breached === false, `join check breached (${JSON.stringify(st.checks.join)})`);
  });
} else skip('h', 'not run');

if (STEPS.has('e')) {
  await check('e: audio8 ramp stops on the CPU limit at the first step', () => {
    const s = runChecks(at('e'), { attempted: 8, mode: 'ramp' });
    expect(s.stopReason === 'cpu', `stopReason ${s.stopReason}, wanted cpu`);
    expect(s.steps.length === 1, `${s.steps.length} steps, wanted 1`);
    expect(s.maxSustainable === null, `maxSustainable ${JSON.stringify(s.maxSustainable)}, wanted null`);
    const cpu = s.steps[0].checks.cpu;
    expect(cpu.breached === true, `cpu check not breached (value ${cpu.value}, limit ${cpu.limit})`);
    expect(s.steps[0].janusCpu.source === 'file', 'janusCpu.source is not file');
  });
} else skip('e', 'not run');

if (STEPS.has('f')) {
  await check('f: two shards split the rooms and both are ok', () => {
    const rooms = [];
    for (const i of [0, 1]) {
      const dir = at(`f${i}`);
      const s = runChecks(dir, { attempted: 8 });
      expect(s.shard.index === i && s.shard.count === 2, `f${i}: shard ${JSON.stringify(s.shard)}, wanted ${i}/2`);
      rooms.push(new Set(readJsonl(path.join(dir, 'bots.jsonl')).map((b) => b.room)));
    }
    expect(rooms[0].size === 1 && rooms[1].size === 1, `each shard must drive exactly 1 room (got ${rooms[0].size} and ${rooms[1].size})`);
    const [r0] = rooms[0];
    expect(!rooms[1].has(r0), 'both shards drove the same room');
  });
  await check('f: merged report (report.json, report.md) covers both shards and the sampler', () => {
    const r = readJson(at('report', 'report.json'));
    expect(r.expectedShards === 2 && r.missingShards.length === 0, `shards ${JSON.stringify({ expected: r.expectedShards, missing: r.missingShards })}`);
    expect(r.bots.attempted === 16 && r.bots.failed === 0, `bots ${JSON.stringify(r.bots)}`);
    expect(r.samplerUsed === true && r.samplerRows > 0, 'the sampler CSV was not used by the report');
    expect(r.transport.policyOk === true, 'report transport.policyOk is not true');
    expect(r.ok === true, 'report.ok is not true');
    const md = readText(at('report', 'report.md'));
    expect(md.startsWith('# qjanus load test report') && md.length > 300, 'report.md is empty or has no title');
  });
} else skip('f', 'not run');

// The sampler CSV must be readable by the harness itself.
await check('sampler: >= 10 rows, finite cpu_pct, parsed by the harness CSV reader', () => {
  const text = readText(at('sampler.csv'));
  const lines = text.split(/\r?\n/).filter((l) => l.trim() !== '');
  const data = lines.slice(1);
  expect(lines[0].startsWith('ts,iso,cpu_pct,rss_mb,threads,total_cpu_pct,load1,rx_mbps,tx_mbps,'), 'unexpected CSV header');
  expect(data.length >= 10, `only ${data.length} sampler rows`);
  const { rows } = parseSamplerCsv(text);
  expect(rows.length >= data.length - 1, `the harness parsed ${rows.length} of ${data.length} rows`);
  const finite = rows.filter((r) => isNum(r.cpu_pct));
  expect(finite.length >= 10, `only ${finite.length} rows with a finite cpu_pct`);
  expect(Math.max(...finite.map((r) => r.cpu_pct)) > 0, 'cpu_pct is 0 in every row');
  expect(rows.every((r) => r.pid === null || Number.isInteger(r.pid)), 'a pid field is not an integer');
  expect(rows.filter((r) => isNum(r.total_cpu_pct) && isNum(r.rx_mbps)).length >= 10, 'host CPU / NIC columns are empty');
});

// Janus side: only fully compliant DTLS negotiations, and there were some.
if (flags['janus-log']) {
  await check('janus log: only DTLS 1.3 / AES-256-GCM / ML-KEM negotiations (ok=1), no ok=0', () => {
    const lines = readText(flags['janus-log']).split(/\r?\n/).filter((l) => l.includes('DTLS-POLICY'));
    expect(lines.length >= 10, `only ${lines.length} DTLS-POLICY lines in the Janus log`);
    const good = /DTLS-POLICY version=0xfefc cipher=0x1302 srtp=0x0008 group=0x11ec role=(client|server) ok=1( |$)/;
    const bad = lines.filter((l) => !good.test(l));
    expect(bad.length === 0, `${bad.length} of ${lines.length} DTLS-POLICY lines are not compliant, e.g. ${bad[0]}`);
  });
} else skip('janus log', 'no --janus-log given');

// No secret may leak into anything that gets uploaded.
await check('no secret, URL, token or loopback address in any output file', () => {
  const needles = [
    ['token secret', process.env.QJANUS_TOKEN_SECRET],
    ['admin key', process.env.QJANUS_ADMIN_KEY],
    ['seed', process.env.QJANUS_LOADTEST_SEED],
    ['WebSocket URL', process.env.QJANUS_WS_URL],
  ].filter(([, v]) => typeof v === 'string' && v.length >= 6);
  const files = readdirSync(OUT, { recursive: true, withFileTypes: true }).filter((d) => d.isFile());
  const hits = [];
  for (const f of files) {
    const p = path.join(f.parentPath ?? f.path, f.name);
    if (/\.(png|jpe?g|webm|zip)$/i.test(p)) continue;
    const text = readFileSync(p, 'utf8');
    for (const [what, v] of needles) if (text.includes(v)) hits.push(`${what} in ${path.relative(OUT, p)}`);
    if (/\d{9,},janus,/.test(text)) hits.push(`signed session token in ${path.relative(OUT, p)}`);
    if (text.includes('127.0.0.1')) hits.push(`loopback address in ${path.relative(OUT, p)}`);
  }
  expect(hits.length === 0, hits.slice(0, 5).join('; '));
});

// ------------------------------------------------------------------- summary
const failed = results.filter((r) => !r.ok);
console.log(`\nsmoke-assert: ${results.length - failed.length}/${results.length} checks passed`);
if (flags.json) writeFileSync(flags.json, `${JSON.stringify({ passed: results.length - failed.length, failed: failed.length, checks: results }, null, 2)}\n`);
process.exit(failed.length === 0 ? 0 : 1);
