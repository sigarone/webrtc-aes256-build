// Tests for src/report.mjs: shard merge, sampler window join, missing shard
// rows, markdown output.

import assert from 'node:assert/strict';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { parseSamplerCsv } from '../src/cpu-source.mjs';
import { buildReport, loadShardDir, renderMarkdown, writeReport } from '../src/report.mjs';

const T = Date.UTC(2026, 8, 30, 12, 0, 0);
/** Step n measures [T + n*100 s, T + n*100 s + 60 s]. */
const measureStart = (n) => T + n * 100_000;
const measureEnd = (n) => measureStart(n) + 60_000;

function stepRow(step, { shardBots = 8, verdict = 'ok', reasons = [], loss = 0, freeze = 0, joinP95 = 900, clientMax = 30, janusMax = null, late = 0 } = {}) {
  return {
    step,
    rooms: step * 2,
    participants: step * 16,
    shardRooms: step,
    shardParticipants: step * 8,
    tStart: measureStart(step) - 20_000,
    tJoinEnd: measureStart(step) - 10_000,
    tMeasureStart: measureStart(step),
    tEnd: measureEnd(step),
    lateStartSec: late,
    join: { attempted: shardBots, ok: verdict === 'ok' ? shardBots : shardBots - 1, failed: verdict === 'ok' ? 0 : 1, notSteady: 0, p50Ms: 400, p95Ms: joinP95, maxMs: joinP95 + 50, iceP50Ms: 100, iceP95Ms: 250, dtlsP50Ms: 80, dtlsP95Ms: 160, allPeersP50Ms: 2000, allPeersP95Ms: 3000 },
    window: { seconds: 60, lossPct: loss, freezeEvents: freeze },
    clientCpu: { avgPct: clientMax - 10, maxPct: clientMax, saturated: clientMax > 90 },
    janusCpu: { avgPct: janusMax === null ? null : janusMax - 20, maxPct: janusMax, source: janusMax === null ? 'none' : 'cmd' },
    checks: {},
    breached: verdict !== 'ok',
    breachReasons: reasons,
    verdict,
  };
}

async function shardDir(root, { index, count, steps, ok = true, stopReason = null, runId = 'r1', name = `shard-${index}`, bots = 8 }) {
  const dir = path.join(root, name);
  await mkdir(dir, { recursive: true });
  const summary = {
    schema: 1,
    runId,
    scenario: 'audio8',
    mode: 'ramp',
    shard: { index, count },
    ok,
    stopReason,
    maxSustainable: null,
    steps,
    bots: { attempted: bots, steady: bots, failed: 0, transportViolations: 0 },
    transport: { tlsVersion: { FEFC: bots * 2 }, dtlsCipher: { TLS_AES_256_GCM_SHA384: bots * 2 }, srtpCipher: { AEAD_AES_256_GCM: bots * 2 }, policyOk: true },
    join: { p50Ms: 400, p95Ms: 900, maxMs: 1200 },
    media: {},
    client: { cpuAvgPct: 20, cpuMaxPct: Math.max(...steps.map((s) => s.clientCpu.maxPct)), saturated: false },
    errors: [{ phase: 'ice', code: 'timeout', count: index + 1, sample: 'ICE did not connect' }],
  };
  await writeFile(path.join(dir, 'summary.json'), JSON.stringify(summary));
  await writeFile(path.join(dir, 'steps.jsonl'), steps.map((s) => JSON.stringify(s)).join('\n') + '\n');
  return dir;
}

const HEADER = 'ts,iso,cpu_pct,rss_mb,threads,total_cpu_pct,load1,rx_mbps,tx_mbps,rx_drop_d,tx_drop_d,udp_in_pps,udp_out_pps,udp_in_errors_d,udp_rcvbuf_errors_d,udp_sndbuf_errors_d,cg_throttled_ms_d,pid';
const csvRow = (tsMs, cpu, { rss = 300, rx = 10, tx = 12, udpErr = 0, thr = 0 } = {}) => `${tsMs / 1000},x,${cpu},${rss},9,30,1,${rx},${tx},0,0,500,500,${udpErr},0,0,${thr},1`;

/** Sampler rows every 10 s inside every step window plus noise between the windows. */
function samplerCsv(cpuByStep) {
  const lines = [HEADER];
  for (const [n, cpu] of Object.entries(cpuByStep)) {
    for (let t = measureStart(n); t <= measureEnd(n); t += 10_000) lines.push(csvRow(t, cpu, { rss: 200 + Number(n) * 10, rx: 5 * Number(n), tx: 6 * Number(n), udpErr: 1, thr: 100 }));
    lines.push(csvRow(measureEnd(n) + 20_000, 999)); // between steps: must not leak into any window
  }
  return lines.join('\n') + '\n';
}

async function withRoot(fn) {
  const root = await mkdtemp(path.join(os.tmpdir(), 'qjl-report-'));
  try {
    await fn(root);
  } finally {
    await rm(root, { recursive: true, force: true });
  }
}

test('merge two shards: sums, worst-shard values, max sustainable and first breach', async () => {
  await withRoot(async (root) => {
    const a = await shardDir(root, { index: 0, count: 2, steps: [stepRow(1, { joinP95: 800 }), stepRow(2, { joinP95: 1000, loss: 0.2 }), stepRow(3, { loss: 0.1 })] });
    const b = await shardDir(root, { index: 1, count: 2, steps: [stepRow(1, { joinP95: 900, clientMax: 50 }), stepRow(2, { joinP95: 1200, loss: 0.4, freeze: 1 }), stepRow(3, { verdict: 'breach', reasons: ['loss'], loss: 3 })] });
    const report = buildReport([await loadShardDir(a), await loadShardDir(b)], { generatedAt: 'now' });
    assert.equal(report.steps.length, 3);
    const s2 = report.steps[1];
    assert.equal(s2.rooms, 4);
    assert.equal(s2.participants, 32);
    assert.equal(s2.botsOk, 16);
    assert.equal(s2.botsFailed, 0);
    assert.equal(s2.joinP95Ms, 1200); // worst shard
    assert.equal(s2.lossPct, 0.4);
    assert.equal(s2.freezeEvents, 1); // summed
    assert.equal(report.steps[0].clientCpuMaxPct, 50);
    assert.equal(s2.verdict, 'ok');
    assert.equal(report.steps[2].verdict, 'breach');
    assert.deepEqual(report.steps[2].breachReasons, ['loss']);
    assert.equal(report.steps[2].botsFailed, 1);
    assert.deepEqual(report.maxSustainable, { step: 2, rooms: 4, participants: 32 });
    assert.deepEqual(report.firstBreach, { step: 3, rooms: 6, participants: 48, verdict: 'breach', reasons: ['loss'], missingShards: [] });
    assert.equal(report.bots.attempted, 16);
    assert.deepEqual(report.transport.tlsVersion, { FEFC: 32 });
    assert.equal(report.transport.policyOk, true);
    assert.deepEqual(report.errors, [{ phase: 'ice', code: 'timeout', count: 3, sample: 'ICE did not connect' }]);
    assert.equal(report.ok, true);
    assert.deepEqual(report.missingShards, []);
    assert.equal(report.steps[0].janusCpu.source, null);
    assert.equal(report.samplerUsed, false);
    assert.ok(report.limits.length >= 5);
  });
});

test('a step counts only when every shard is ok: the prefix stops at the first non-ok step even if later ones look fine', async () => {
  await withRoot(async (root) => {
    const a = await shardDir(root, { index: 0, count: 2, steps: [stepRow(1), stepRow(2), stepRow(3)] });
    const b = await shardDir(root, { index: 1, count: 2, steps: [stepRow(1), stepRow(2, { verdict: 'inconclusive_client_saturated', reasons: ['loss'], clientMax: 95 }), stepRow(3)] });
    const report = buildReport([await loadShardDir(a), await loadShardDir(b)]);
    assert.deepEqual(report.maxSustainable, { step: 1, rooms: 2, participants: 16 });
    assert.equal(report.firstBreach.step, 2);
    assert.equal(report.steps[1].verdict, 'inconclusive_client_saturated');
    assert.equal(report.steps[2].sustained, true); // ok on its own, but not part of the unbroken prefix
  });
});

test('sampler CSV is joined to the step windows and its CPU maximum can end the sustainable range', async () => {
  await withRoot(async (root) => {
    const a = await shardDir(root, { index: 0, count: 1, steps: [stepRow(1), stepRow(2), stepRow(3)] });
    const { rows } = parseSamplerCsv(samplerCsv({ 1: 120, 2: 150, 3: 190 }));
    const report = buildReport([await loadShardDir(a)], { samplerRows: rows, cpuLimit: 180 });
    const [s1, s2, s3] = report.steps;
    assert.deepEqual([s1.janusCpu.avgPct, s1.janusCpu.maxPct, s1.janusCpu.source], [120, 120, 'sampler']); // the 999 row between steps is excluded
    assert.equal(s2.janusCpu.maxPct, 150);
    assert.equal(s1.rssMaxMb, 210);
    assert.equal(s2.nicRxMbpsMax, 10);
    assert.equal(s2.nicTxMbpsMax, 12);
    assert.equal(s1.udpDrops, 7); // 7 rows in a 60 s window at 10 s spacing, 1 error each
    assert.equal(s1.cgThrottledMs, 700);
    assert.equal(s3.verdict, 'breach');
    assert.deepEqual(s3.breachReasons, ['cpu']);
    assert.deepEqual(report.maxSustainable, { step: 2, rooms: 4, participants: 32 });
    assert.deepEqual(report.firstBreach.reasons, ['cpu']);
    assert.equal(report.samplerUsed, true);
    assert.equal(report.cpuLimit, 180);
    // exactly at the limit is fine
    const atLimit = buildReport([await loadShardDir(a)], { samplerRows: parseSamplerCsv(samplerCsv({ 1: 100, 2: 180, 3: 180 })).rows, cpuLimit: 180 });
    assert.equal(atLimit.maxSustainable.step, 3);
  });
});

test('a step window without sampler rows is not a cpu breach and keeps the shard cpu value', async () => {
  await withRoot(async (root) => {
    const a = await shardDir(root, { index: 0, count: 1, steps: [stepRow(1, { janusMax: 130 }), stepRow(2, { janusMax: 140 })] });
    const { rows } = parseSamplerCsv(samplerCsv({ 1: 100 }));
    const report = buildReport([await loadShardDir(a)], { samplerRows: rows });
    assert.equal(report.steps[0].janusCpu.source, 'sampler');
    assert.equal(report.steps[1].janusCpu.source, 'shard');
    assert.equal(report.steps[1].janusCpu.maxPct, 140);
    assert.equal(report.steps[1].rssMaxMb, null);
    assert.equal(report.maxSustainable.step, 2);
  });
});

test('a missing shard row makes the step incomplete and stops the sustainable range', async () => {
  await withRoot(async (root) => {
    const a = await shardDir(root, { index: 0, count: 2, steps: [stepRow(1), stepRow(2), stepRow(3)] });
    const b = await shardDir(root, { index: 1, count: 2, steps: [stepRow(1), stepRow(2)] });
    const report = buildReport([await loadShardDir(a), await loadShardDir(b)]);
    assert.equal(report.steps[2].verdict, 'incomplete');
    assert.deepEqual(report.steps[2].missingShards, [1]);
    assert.deepEqual(report.maxSustainable, { step: 2, rooms: 4, participants: 32 });
    assert.deepEqual(report.firstBreach.missingShards, [1]);
    const md = renderMarkdown(report);
    assert.ok(md.includes('incomplete (no row from shard 1)'));
  });
});

test('a whole shard directory that is not supplied is reported', async () => {
  await withRoot(async (root) => {
    const a = await shardDir(root, { index: 0, count: 3, steps: [stepRow(1)] });
    const c = await shardDir(root, { index: 2, count: 3, steps: [stepRow(1)] });
    const report = buildReport([await loadShardDir(a), await loadShardDir(c)]);
    assert.deepEqual(report.missingShards, [1]);
    assert.equal(report.ok, false);
    assert.equal(report.maxSustainable, null);
    assert.equal(report.steps[0].verdict, 'incomplete');
    assert.ok(renderMarkdown(report).includes('Shard 1: no output directory was supplied.'));
  });
});

test('loadShardDir: summary.json is required; partial jsonl lines and missing optional files are tolerated', async () => {
  await withRoot(async (root) => {
    await mkdir(path.join(root, 'empty'));
    await assert.rejects(() => loadShardDir(path.join(root, 'empty')), /summary\.json/);
    const dir = await shardDir(root, { index: 0, count: 1, steps: [stepRow(1)] });
    const stepsFile = path.join(dir, 'steps.jsonl');
    await writeFile(stepsFile, `${await readFile(stepsFile, 'utf8')}{"step": 2, "rooms"`);
    const shard = await loadShardDir(dir);
    assert.equal(shard.steps.length, 1);
    assert.deepEqual(shard.bots, []);
    assert.equal(shard.run, null);
  });
});

test('writeReport writes report.json and a GitHub-summary friendly report.md', async () => {
  await withRoot(async (root) => {
    const a = await shardDir(root, { index: 0, count: 2, steps: [stepRow(1), stepRow(2, { verdict: 'breach', reasons: ['cpu', 'loss'] })], stopReason: 'cpu' });
    const b = await shardDir(root, { index: 1, count: 2, steps: [stepRow(1), stepRow(2)] });
    const sampler = path.join(root, 'node.csv');
    await writeFile(sampler, samplerCsv({ 1: 100, 2: 150 }));
    const out = path.join(root, 'out');
    const report = await writeReport({ inDirs: [a, b], sampler, out, cpuLimit: 180, generatedAt: '2026-09-30T00:00:00Z' });
    assert.deepEqual(report.maxSustainable, { step: 1, rooms: 2, participants: 16 });
    const json = JSON.parse(await readFile(path.join(out, 'report.json'), 'utf8'));
    assert.equal(json.schema, 1);
    assert.equal(json.generatedAt, '2026-09-30T00:00:00Z');
    const md = await readFile(path.join(out, 'report.md'), 'utf8');
    assert.match(md, /^# qjanus load test report/);
    assert.match(md, /\*\*Maximum sustainable: 2 rooms, 16 participants\*\*/);
    for (const heading of ['## Result', '## Steps', '## Shards', '## Transport', '## Errors', '## Limits of this result']) assert.ok(md.includes(heading), heading);
    assert.match(md, /First breach: step 2 \(4 rooms, 32 participants\)/);
    assert.match(md, /breach \(cpu,loss\)/);
    // one header, one separator and one row per step in the steps table
    const stepsTable = md.split('## Steps')[1].split('##')[0].split('\n').filter((l) => l.startsWith('|'));
    assert.equal(stepsTable.length, 2 + 2);
    assert.equal(new Set(stepsTable.map((l) => l.split('|').length)).size, 1, 'every table row has the same number of columns');
    for (const word of ['saturated', 'shared test key', 'fake media', 'layer selection']) assert.ok(md.toLowerCase().includes(word.toLowerCase()), word);
    assert.match(md, /^[\x09\x0a\x20-\x7e]*$/, 'markdown must be ASCII');
    assert.ok(!/wss?:\/\//.test(md));
    // unreadable sampler: still a report, with a note
    const noSampler = await writeReport({ inDirs: [a, b], sampler: path.join(root, 'missing.csv'), out: path.join(root, 'out2') });
    assert.equal(noSampler.samplerUsed, false);
    assert.ok(noSampler.notes.some((n) => n.includes('could not be read')));
  });
});

test('report of a single run step (mode run) works without ramp data', async () => {
  await withRoot(async (root) => {
    const a = await shardDir(root, { index: 0, count: 1, steps: [stepRow(1)] });
    const report = buildReport([await loadShardDir(a)]);
    assert.equal(report.firstBreach, null);
    assert.deepEqual(report.maxSustainable, { step: 1, rooms: 2, participants: 16 });
    assert.ok(renderMarkdown(report).includes('No breach in the measured steps'));
  });
});
