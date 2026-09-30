// Unit tests for src/metrics.mjs and src/cpu-source.mjs with hand-made samples.

import assert from 'node:assert/strict';
import { mkdtemp, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { createClientCpuMonitor, createCpuSource, parseCpuOutput, parseSamplerCsv, windowStats } from '../src/cpu-source.mjs';
import * as M from '../src/metrics.mjs';

/** One sample; `in`/`out` override the interesting counters. */
function sample(t, { id = 'b', aPk = 100, aLost = 0, vPk = 0, vLost = 0, freeze = 0, pps = 16.7, jitter = 5, bytes = 0, concealed = 0, aSamples = 0 } = {}) {
  return {
    id,
    t,
    dtMs: 2000,
    d: {
      aIn: { packets: aPk, lost: aLost, bytes, concealed, samples: aSamples, nack: 0, jbDelayMs: 0, jbEmitted: 0 },
      vIn: { packets: vPk, lost: vLost, bytes: 0, framesDecoded: 0, framesDropped: 0, freezeCount: freeze, freezeMs: 0, pauseCount: 0, nack: 0, pli: 0, fir: 0 },
      aOut: { packets: 33, bytes: 0, nack: 0, retransmitted: 0 },
      vOut: { packets: 0, bytes: 0, framesEncoded: 0, nack: 0, pli: 0, retransmitted: 0 },
    },
    g: { aOutPps: pps, aJitterMsMax: jitter, vJitterMsMax: null },
  };
}

test('percentile is nearest-rank and ignores non-numbers', () => {
  assert.equal(M.percentile([], 50), null);
  assert.equal(M.percentile([null, undefined, NaN], 95), null);
  assert.equal(M.percentile([5, 1, 3, 2, 4], 50), 3);
  assert.equal(M.percentile([1, 2], 50), 1);
  assert.equal(M.percentile([1, 2, 3, 4, 5, 6, 7, 8, 9, 10], 95), 10);
  assert.equal(M.percentile([7], 95), 7);
  const s = M.summarize([2, 4, null, 6]);
  assert.deepEqual([s.n, s.min, s.max, s.avg], [3, 2, 6, 4]);
  assert.equal(M.summarize([]).p50, null);
});

test('lossPct is lost/(lost+packets) and null without packets', () => {
  assert.equal(M.lossPct(1, 99), 1);
  assert.equal(M.lossPct(0, 0), null);
  assert.equal(M.lossPct(null, null), null);
  assert.equal(M.lossPct(5, 0), 100);
});

test('sliceWindows: whole slices, merged short remainder, own long remainder, short window', () => {
  assert.equal(M.sliceWindows(0, 30_000).length, 3);
  const merged = M.sliceWindows(0, 34_000);
  assert.equal(merged.length, 3);
  assert.equal(merged[2].toMs, 34_000);
  const own = M.sliceWindows(0, 36_000);
  assert.equal(own.length, 4);
  assert.deepEqual(own[3], { fromMs: 30_000, toMs: 36_000 });
  assert.deepEqual(M.sliceWindows(0, 4_000), [{ fromMs: 0, toMs: 4_000 }]);
  assert.deepEqual(M.sliceWindows(5, 5), []);
});

test('sustainedMax is the largest value held for n consecutive entries', () => {
  assert.equal(M.sustainedMax([], 2), null);
  assert.equal(M.sustainedMax([0.5, 3, 0.5], 2), 0.5);
  assert.equal(M.sustainedMax([0.5, 3, 2, 0.5], 2), 2);
  assert.equal(M.sustainedMax([null, 3, null], 1), 3);
  assert.equal(M.sustainedMax([4], 2), 4); // fewer entries than n: whole series
});

/** Samples of 3 ten-second windows with the given per-window loss percentages (1000 packets each). */
function lossSamples(percentages) {
  return percentages.flatMap((p, w) => [sample(w * 10_000 + 1000, { aPk: 1000 - p * 10, aLost: p * 10 })]);
}

test('loss exactly at the limit is not a breach', () => {
  const w = M.lossWindows(lossSamples([1, 1, 1]), 0, 30_000);
  assert.equal(w.length, 3);
  const check = M.evalLoss(w, { limitPct: 1, breachWindows: 2 });
  assert.equal(check.breached, false);
  assert.equal(check.value, 1);
});

test('one window over the limit with breach-windows 2 is not a breach; two in a row is', () => {
  const one = M.evalLoss(M.lossWindows(lossSamples([0.2, 5, 0.2]), 0, 30_000), { limitPct: 1, breachWindows: 2 });
  assert.equal(one.breached, false);
  const two = M.evalLoss(M.lossWindows(lossSamples([0.2, 2, 3]), 0, 30_000), { limitPct: 1, breachWindows: 2 });
  assert.equal(two.breached, true);
  assert.equal(two.value, 2);
  const apart = M.evalLoss(M.lossWindows(lossSamples([2, 0.2, 2]), 0, 30_000), { limitPct: 1, breachWindows: 2 });
  assert.equal(apart.breached, false);
  const single = M.evalLoss(M.lossWindows(lossSamples([0.2, 5, 0.2]), 0, 30_000), { limitPct: 1, breachWindows: 1 });
  assert.equal(single.breached, true);
});

test('loss counts audio and video subscriber packets together; no traffic is no breach', () => {
  const s = [sample(1000, { aPk: 80, aLost: 0, vPk: 10, vLost: 10 })];
  const w = M.lossWindows(s, 0, 10_000);
  assert.equal(w[0].lossPct, 10);
  const none = M.evalLoss(M.lossWindows([], 0, 20_000), { limitPct: 1, breachWindows: 2 });
  assert.deepEqual([none.value, none.breached], [0, false]);
  const empty = M.evalLoss([], { limitPct: 1, breachWindows: 2 });
  assert.deepEqual([empty.value, empty.breached], [null, false]);
});

test('freeze tolerance', () => {
  assert.equal(M.evalFreeze(0, 0).breached, false);
  assert.equal(M.evalFreeze(1, 0).breached, true);
  assert.equal(M.evalFreeze(2, 2).breached, false);
  assert.equal(M.evalFreeze(3, 2).breached, true);
  assert.deepEqual(M.evalFreeze(null, 0), { value: null, limit: 0, breached: false });
});

test('cpu, join and transport checks', () => {
  assert.equal(M.evalCpu(180, 180).breached, false);
  assert.equal(M.evalCpu(180.1, 180).breached, true);
  assert.equal(M.evalCpu(null, 180).breached, false);
  // 1 failed + 1 not steady of 40 = 5 %: exactly at the limit is fine
  assert.equal(M.evalJoin({ failed: 1, notSteady: 1, attempted: 40 }, 5).breached, false);
  assert.equal(M.evalJoin({ failed: 2, notSteady: 1, attempted: 40 }, 5).breached, true);
  assert.equal(M.evalJoin({ failed: 0, notSteady: 0, attempted: 0 }, 5).value, null);
  assert.equal(M.evalTransport(0).breached, false);
  assert.equal(M.evalTransport(1).breached, true);
});

test('stepVerdict: priority, saturation makes a breach inconclusive, transport never', () => {
  const clean = { loss: { breached: false }, freeze: { breached: false }, cpu: { breached: false }, join: { breached: false }, transport: { breached: false } };
  assert.deepEqual(M.stepVerdict(clean, { clientSaturated: true }), { breached: false, breachReasons: [], verdict: 'ok' });
  const lossy = { ...clean, loss: { breached: true }, cpu: { breached: true } };
  assert.deepEqual(M.stepVerdict(lossy, { clientSaturated: false }), { breached: true, breachReasons: ['loss', 'cpu'], verdict: 'breach' });
  assert.equal(M.stepVerdict(lossy, { clientSaturated: true }).verdict, 'inconclusive_client_saturated');
  const bad = { ...lossy, transport: { breached: true }, join: { breached: true } };
  const v = M.stepVerdict(bad, { clientSaturated: true });
  assert.deepEqual(v.breachReasons, ['transport', 'join', 'loss', 'cpu']);
  assert.equal(v.verdict, 'breach');
});

test('client cpu saturation flag uses the maximum, threshold is exclusive', () => {
  assert.deepEqual(M.clientCpuStats([50, 60, null]), { avgPct: 55, maxPct: 60, saturated: false });
  assert.equal(M.clientCpuStats([90]).saturated, false);
  assert.equal(M.clientCpuStats([20, 90.5]).saturated, true);
  assert.deepEqual(M.clientCpuStats([]), { avgPct: null, maxPct: null, saturated: false });
});

test('windowMetrics aggregates only samples inside the window and reports null when empty', () => {
  const samples = [
    sample(500, { aLost: 50, aPk: 50 }), // before the window
    sample(1000, { id: 'a', aPk: 200, aLost: 2, freeze: 1, bytes: 25_000, pps: 16, jitter: 4, aSamples: 1000, concealed: 10 }),
    sample(3000, { id: 'b', aPk: 200, aLost: 0, freeze: 2, bytes: 25_000, pps: 17, jitter: 8, aSamples: 1000, concealed: 0 }),
    sample(10_000, { aLost: 999, aPk: 1 }), // t == toMs is outside
  ];
  const w = M.windowMetrics(samples, 1000, 10_000);
  assert.equal(w.seconds, 9);
  assert.equal(w.lossPct, 0.498); // 2 lost of 402
  assert.equal(w.freezeEvents, 3);
  assert.equal(w.freezeBots, 2);
  assert.equal(w.aJitterMsP95, 8);
  assert.equal(w.vJitterMsP95, null);
  assert.equal(w.concealedPct, 0.5);
  assert.deepEqual(w.audioPubPps, { p50: 16, min: 16, max: 17 });
  // 50000 bytes over 9 s over 2 bots = 22.2 kbps per bot
  assert.equal(w.rxKbpsPerBot, 22.2);
  assert.equal(w.videoLossPct, null);
  const empty = M.windowMetrics([], 0, 60_000);
  assert.equal(empty.lossPct, null);
  assert.equal(empty.freezeEvents, null);
  assert.equal(empty.rxKbpsPerBot, null);
  assert.deepEqual(empty.audioPubPps, { p50: null, min: null, max: null });
});

test('sumDeltas and maxGauges treat missing values as absent', () => {
  const s = [{ t: 1, d: { aIn: { packets: 3 } }, g: { aJitterMsMax: 4, aInStreams: null, qualityLimit: 'none' } }, { t: 2, g: { aJitterMsMax: 9 } }];
  assert.equal(M.sumDeltas(s).aIn.packets, 3);
  assert.equal(M.sumDeltas(s).vOut.framesEncoded, 0);
  assert.deepEqual(M.maxGauges(s), { aJitterMsMax: 9 });
});

test('classifyBots: steady in time is ok, terminal is failed, late or unfinished is not steady', () => {
  const bots = [
    { state: 'steady', allPeersMs: 3000 },
    { state: 'steady', allPeersMs: 70_000 },
    { state: 'failed', allPeersMs: null },
    { state: 'closed', allPeersMs: 5000 },
    { state: 'subscribing', allPeersMs: null },
    { state: 'steady', allPeersMs: null },
  ];
  assert.deepEqual(M.classifyBots(bots, 60_000), { attempted: 6, ok: 2, failed: 2, notSteady: 2 });
});

test('joinDistributions pools publisher and subscriber ICE/DTLS times', () => {
  const st = (j, pi, si, pd, sd, all) => ({ timings: { joinedMs: j, pubIceMs: pi, subIceMs: si, pubDtlsMs: pd, subDtlsMs: sd, allPeersMs: all } });
  const d = M.joinDistributions([st(100, 10, 20, 30, 40, 1000), st(300, 50, 60, 70, 80, 3000), { timings: {} }, { timings: null }]);
  assert.equal(d.p50Ms, 100);
  assert.equal(d.maxMs, 300);
  assert.equal(d.iceP95Ms, 60);
  assert.equal(d.dtlsP50Ms, 40);
  assert.equal(d.allPeersP95Ms, 3000);
  assert.equal(M.joinDistributions([]).p95Ms, null);
});

test('transport summary counts values and violations', () => {
  const ok = { tlsVersion: 'FEFC', dtlsCipher: 'TLS_AES_256_GCM_SHA384', srtpCipher: 'AEAD_AES_256_GCM', ok: true };
  const bad = { ...ok, tlsVersion: 'FEFD', ok: false };
  const t = M.transportSummary([
    { state: 'steady', transport: { pub: ok, sub: ok } },
    { state: 'failed', error: { phase: 'transport', code: 'x' }, transport: { pub: bad, sub: null } },
    { state: 'init', transport: { pub: null, sub: null } },
  ]);
  assert.equal(t.violations, 1);
  assert.equal(t.seen, 3);
  assert.deepEqual(t.tlsVersion, { FEFC: 2, FEFD: 1 });
  assert.equal(M.hasTransportViolation({ error: { phase: 'transport' } }), true);
  assert.equal(M.hasTransportViolation({ transport: { pub: ok, sub: ok } }), false);
});

test('errorSummary groups by phase and code', () => {
  const e = M.errorSummary([
    { error: { phase: 'ice', code: 'timeout', message: 'first' } },
    { error: { phase: 'ice', code: 'timeout', message: 'second' } },
    { error: { phase: 'join', code: 426, message: 'no room' } },
    { error: null },
  ]);
  assert.deepEqual(e[0], { phase: 'ice', code: 'timeout', count: 2, sample: 'first' });
  assert.equal(e.length, 2);
});

test('per-bot derived values', () => {
  const tot = { aIn: { packets: 990, lost: 10 }, vIn: { packets: 0, lost: 0 }, aOut: { packets: 500 } };
  assert.equal(M.totalLossPct(tot), 1);
  assert.equal(M.audioOutPpsAvg(tot, 30_000), 16.67);
  assert.equal(M.audioOutPpsAvg(tot, 0), null);
  assert.equal(M.totalLossPct(null), null);
});

// ------------------------------------------------------------------ cpu-source

const HEADER = 'ts,iso,cpu_pct,rss_mb,threads,total_cpu_pct,load1,rx_mbps,tx_mbps,rx_drop_d,tx_drop_d,udp_in_pps,udp_out_pps,udp_in_errors_d,udp_rcvbuf_errors_d,udp_sndbuf_errors_d,cg_throttled_ms_d,pid';
const row = (ts, cpu, extra = {}) => `${ts},2026-01-01T00:00:00Z,${cpu},${extra.rss ?? 100},12,20,0.5,${extra.rx ?? 1.5},${extra.tx ?? 2.5},0,0,100,100,0,0,0,${extra.thr ?? ''},4242`;

test('parseSamplerCsv: header lookup, empty fields, CRLF, partial last line', () => {
  const csv = [HEADER, row(1000.5, 120), row(1002.5, ''), row(1004.5, 150), '1006.5,2026-01-01T00:00:0'].join('\r\n');
  const { rows } = parseSamplerCsv(csv);
  assert.equal(rows.length, 3);
  assert.equal(rows[0].tsMs, 1_000_500);
  assert.equal(rows[0].cpu_pct, 120);
  assert.equal(rows[1].cpu_pct, null);
  assert.equal(rows[2].cg_throttled_ms_d, null);
  assert.equal(rows[0].iso, '2026-01-01T00:00:00Z');
});

test('parseSamplerCsv tolerates reordered columns, blank lines, a BOM, and garbage', () => {
  const { rows } = parseSamplerCsv('\uFEFFcpu_pct,ts\n\n50,10\n70,11\n');
  assert.deepEqual(rows.map((r) => [r.tsMs, r.cpu_pct]), [[10_000, 50], [11_000, 70]]);
  assert.deepEqual(parseSamplerCsv('').rows, []);
  assert.deepEqual(parseSamplerCsv('a,b\n1,2\n').rows, []);
});

test('windowStats filters rows to the window and ignores empty values', () => {
  const { rows } = parseSamplerCsv([HEADER, row(10, 100), row(12, 200), row(14, ''), row(16, 300), row(18, 500)].join('\n'));
  const w = windowStats(rows, 11_000, 16_000);
  assert.deepEqual(w, { avg: 250, max: 300, n: 2 });
  assert.equal(windowStats(rows, 20_000, 30_000), null);
  assert.equal(windowStats(rows, 14_000, 14_000), null);
});

test('parseCpuOutput accepts a number or a JSON object with cpu_pct', () => {
  assert.equal(parseCpuOutput('153.5\n'), 153.5);
  assert.equal(parseCpuOutput('  97 extra'), 97);
  assert.equal(parseCpuOutput('{"cpu_pct": 88.2, "pid": 1}'), 88.2);
  assert.equal(parseCpuOutput('{"cpu_pct": "77"}'), 77);
  assert.equal(parseCpuOutput('{nope'), null);
  assert.equal(parseCpuOutput('abc'), null);
  assert.equal(parseCpuOutput(''), null);
});

test('file cpu source: missing file is null, then values appear, partial line ignored', async () => {
  const dir = await mkdtemp(path.join(os.tmpdir(), 'qjl-cpu-'));
  try {
    const file = path.join(dir, 'node.csv');
    const src = createCpuSource({ file });
    assert.equal(src.kind, 'file');
    await src.refresh();
    assert.equal(src.sample(0, 1e15), null);
    assert.equal(src.latest(), null);
    await writeFile(file, [HEADER, row(100, 110), row(102, 190)].join('\n') + '\n103.5,2026');
    await src.refresh();
    assert.deepEqual(src.sample(99_000, 103_000), { avg: 150, max: 190, n: 2 });
    assert.equal(src.latest(), 190);
    assert.equal(src.sample(101_000, 103_000).max, 190);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test('cmd cpu source records one reading per refresh and survives failures', async () => {
  let clock = 1000;
  let n = 0;
  const exec = async () => {
    n++;
    if (n === 2) throw new Error('ssh failed');
    if (n === 3) return { stdout: 'garbage' };
    return { stdout: n === 1 ? '120' : '{"cpu_pct": 175}' };
  };
  const src = createCpuSource({ cmd: 'echo', exec, now: () => clock });
  assert.equal(src.kind, 'cmd');
  for (let i = 0; i < 4; i++) {
    await src.refresh();
    clock += 2000;
  }
  assert.deepEqual(src.sample(0, 10_000), { avg: 147.5, max: 175, n: 2 });
  assert.equal(src.latest(), 175);
  const none = createCpuSource({});
  assert.equal(none.kind, 'none');
  assert.equal(none.sample(0, 1), null);
});

test('client cpu monitor: first sample has no baseline, then percent busy', () => {
  const cpus = [{ times: { user: 0, nice: 0, sys: 0, idle: 0, irq: 0 } }, { times: { user: 0, nice: 0, sys: 0, idle: 0, irq: 0 } }];
  const monitor = createClientCpuMonitor({ cpus: () => cpus });
  assert.equal(monitor.sample(), null);
  for (const c of cpus) {
    c.times.user += 30;
    c.times.idle += 70;
  }
  assert.equal(monitor.sample(), 30);
  assert.equal(monitor.sample(), null); // no time passed
});
