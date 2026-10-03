// Ramp/run state machine tests against a FakeHost and a fake clock, plus tests
// of the host side (static server, BrowserPoolHost with a fake launcher, and a
// real-Chromium smoke test that skips itself when Chromium or page/ is missing).

import assert from 'node:assert/strict';
import { existsSync } from 'node:fs';
import { mkdtemp, readFile, readdir, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { test } from 'node:test';
import { BrowserPoolHost, PAGE_DIR, buildChromiumArgs } from '../src/browser-pool.mjs';
import { parseCli } from '../src/config.mjs';
import { createCpuSource } from '../src/cpu-source.mjs';
import { pseudonym, roomId, roomPlan } from '../src/ids.mjs';
import { runShard } from '../src/orchestrator.mjs';
import { startStaticServer } from '../src/static-server.mjs';
import { verifySessionToken } from '../src/token.mjs';

const WS = 'wss://sfu-secret-host.example.invalid/janus';
const ENV = { QJANUS_WS_URL: WS, QJANUS_TOKEN_SECRET: 'TOKENSECRETVALUE', QJANUS_LOADTEST_SEED: 'SEEDVALUE123' };
const T0 = Date.UTC(2026, 8, 30, 12, 0, 0);

// ------------------------------------------------------------- fake clock/host

/** Virtual time: sleeping just advances it. An aborted signal makes sleep a no-op. */
class FakeClock {
  constructor(t = T0) {
    this.t = t;
    this.now = () => this.t;
    this.sleep = async (ms, signal) => {
      if (signal && signal.aborted) return;
      this.t += Math.max(0, ms);
    };
  }
}

const zeroTot = () => ({ aIn: { packets: 0, lost: 0 }, vIn: { packets: 0, lost: 0 }, aOut: { packets: 0 }, e2eeEnc: 0, e2eeDec: 0, e2eeFail: 0 });

/**
 * Simulated BotHost. Options (all optional):
 *  joinMs                 time until a bot is steady
 *  lossPct(live, t)       subscriber loss percent of a sample
 *  freeze(live, t)        freeze events per sample
 *  failJoin(botCfg)       bot fails 1 s after start
 *  transportBad(botCfg)   bot ends with a transport policy violation
 *  crash(bot)             page crash reported
 *  startFails(botCfg)     startBot rejects (message contains the URL on purpose)
 *  launchError, onLaunch(), pollError, onPoll(n)
 */
class FakeHost {
  constructor(clock, o = {}) {
    this.clock = clock;
    this.o = { joinMs: 3000, statsMs: 2000, ...o };
    this.bots = new Map();
    this.pollCount = 0;
    this.launched = false;
    this.closed = false;
    this.events = [];
    this.stopOrder = [];
    this.tokenCalls = [];
  }

  live() {
    return [...this.bots.values()].filter((b) => !b.stopped).length;
  }

  async launch() {
    if (this.o.onLaunch) await this.o.onLaunch();
    if (this.o.launchError) throw new Error(this.o.launchError);
    this.launched = true;
    this.events.push('launch');
  }

  async prewarm(totalBots) {
    this.events.push(`prewarm ${totalBots}`);
  }

  versions() {
    return { node: 'v-test', playwright: 'pw-test', chromium: 'chromium-test' };
  }

  async startBot(cfg) {
    if (this.o.startFails && this.o.startFails(cfg)) throw new Error(`cannot start bot for ${cfg.wsUrl}`);
    const now = this.clock.now();
    this.bots.set(cfg.id, { id: cfg.id, cfg, startedAt: now, lastSample: now + this.o.joinMs, stopped: false, tot: zeroTot() });
  }

  async pollAll() {
    this.pollCount++;
    if (this.o.onPoll) this.o.onPoll(this.pollCount);
    if (this.o.pollError) throw new Error('poll exploded');
    const now = this.clock.now();
    return [...this.bots.values()].map((b) => this.status(b, now));
  }

  status(b, now) {
    const elapsed = now - b.startedAt;
    const base = { id: b.id, error: null, startedAt: b.startedAt, peers: { expected: 7, subscribed: 0, audioFlowing: 0, videoFlowing: 0 }, transport: { pub: null, sub: null }, tot: b.tot, samples: [], events: [], browser: 0, page: 0 };
    if (b.stopped) return { ...base, state: 'closed', timings: {} };
    if (this.o.crash && this.o.crash(b)) return { ...base, state: 'failed', timings: {}, error: { phase: 'other', code: 'page_crash', message: 'page crashed' } };
    if (this.o.failJoin && this.o.failJoin(b.cfg)) {
      if (elapsed < 1000) return { ...base, state: 'joining', timings: {} };
      return { ...base, state: 'failed', timings: { joinedMs: 300 }, error: { phase: 'ice', code: 'timeout', message: 'ICE did not connect' } };
    }
    if (elapsed < this.o.joinMs) return { ...base, state: 'joining', timings: { wsMs: 50, joinedMs: elapsed >= 300 ? 300 : null } };
    const bad = Boolean(this.o.transportBad && this.o.transportBad(b.cfg));
    const info = { tlsVersion: bad ? 'FEFD' : 'FEFC', dtlsCipher: 'TLS_AES_256_GCM_SHA384', srtpCipher: 'AEAD_AES_256_GCM', dtlsState: 'connected', iceState: 'connected', localCandType: 'host', remoteCandType: 'host', protocol: 'udp', ok: !bad };
    const timings = { wsMs: 50, createMs: 100, joinedMs: 300, pubIceMs: 200, subIceMs: 250, pubDtlsMs: 100, subDtlsMs: 120, allPeersMs: this.o.joinMs };
    const live = this.live();
    const samples = [];
    while (b.lastSample + this.o.statsMs <= now) {
      b.lastSample += this.o.statsMs;
      const t = b.lastSample;
      const lossPct = this.o.lossPct ? this.o.lossPct(live, t) : 0;
      const lost = Math.round(lossPct * 10);
      const freezeCount = this.o.freeze ? this.o.freeze(live, t) : 0;
      samples.push({
        t,
        dtMs: this.o.statsMs,
        d: { aIn: { packets: 1000 - lost, lost, bytes: 40_000, concealed: 0, samples: 0, nack: 0 }, vIn: { packets: 0, lost: 0, freezeCount }, aOut: { packets: 33, bytes: 1000 }, vOut: {} },
        g: { aOutPps: 16.5, aJitterMsMax: 6 },
      });
      b.tot.aIn.packets += 1000 - lost;
      b.tot.aIn.lost += lost;
      b.tot.aOut.packets += 33;
      b.tot.e2eeEnc += 33;
      b.tot.e2eeDec += 231;
    }
    if (bad) return { ...base, state: 'failed', timings, transport: { pub: info, sub: info }, error: { phase: 'transport', code: 'policy', message: 'tls version FEFD is not allowed' }, samples };
    return { ...base, state: 'steady', timings, transport: { pub: info, sub: info }, samples, peers: { expected: 7, subscribed: 7, audioFlowing: 7, videoFlowing: 0 } };
  }

  async setTokens(map) {
    this.tokenCalls.push({ t: this.clock.now(), ids: Object.keys(map).sort(), tokens: Object.values(map) });
  }

  async stopBot(id) {
    const b = this.bots.get(id);
    this.stopOrder.push(id);
    if (b) b.stopped = true;
    return b ? { ...this.status(b, this.clock.now()), state: 'closed' } : null;
  }

  async stopAll() {
    this.events.push('stopAll');
    return [];
  }

  async close() {
    this.closed = true;
    this.events.push('close');
  }
}

// ------------------------------------------------------------------- harness

const BASE_ARGS = ['--scenario', 'audio8', '--hold-sec', '30', '--settle-sec', '10', '--join-rate', '2', '--run-id', 'test', '--quiet'];

async function harness(argv, { host: hostOpts = {}, deps = {}, clockStart = T0, env = ENV } = {}) {
  const dir = await mkdtemp(path.join(os.tmpdir(), 'qjl-ramp-'));
  const cfg = parseCli([...argv.slice(0, 1), ...BASE_ARGS, ...argv.slice(1), '--out', dir], env, { now: () => new Date(clockStart) }).cfg;
  const clock = new FakeClock(clockStart);
  const host = new FakeHost(clock, hostOpts);
  const logs = [];
  let minted = 0;
  const summary = await runShard(cfg, host, {
    now: clock.now,
    sleep: clock.sleep,
    log: (kind, line) => logs.push([kind, line]),
    roomPlan,
    mintToken: () => `TOKEN-SENTINEL-${++minted}`,
    clientCpu: { sample: () => 20 },
    ...deps,
  });
  const read = (name) => readFile(path.join(dir, name), 'utf8');
  const readJsonl = async (name) => (await read(name)).trim().split('\n').filter(Boolean).map((l) => JSON.parse(l));
  return { cfg, clock, host, summary, dir, logs, read, readJsonl, cleanup: () => rm(dir, { recursive: true, force: true }) };
}

/** Every file of a run must be free of secrets, URLs and tokens. */
async function assertNoSecrets(h) {
  for (const name of await readdir(h.dir)) {
    const text = await h.read(name);
    for (const forbidden of ['sfu-secret-host', 'wss://', 'TOKENSECRETVALUE', 'SEEDVALUE123', 'TOKEN-SENTINEL']) {
      assert.ok(!text.includes(forbidden), `${name} contains ${forbidden}`);
    }
  }
  for (const [, line] of h.logs) assert.ok(!line.includes('TOKEN-SENTINEL') && !line.includes('sfu-secret-host'));
}

const withHarness = (fn) => async (t) => {
  const made = [];
  const wrap = async (...args) => {
    const h = await harness(...args);
    made.push(h);
    return h;
  };
  try {
    await fn(wrap, t);
  } finally {
    for (const h of made) await h.cleanup();
  }
};

// -------------------------------------------------------------------- tests

test('run: one step, timeline, files, graceful leave, no secrets', withHarness(async (run) => {
  const h = await run(['run', '--rooms', '2']);
  const { summary, host } = h;
  assert.equal(summary.ok, true);
  assert.equal(summary.stopReason, null);
  assert.equal(summary.mode, 'run');
  assert.equal(summary.steps.length, 1);
  assert.deepEqual(summary.maxSustainable, { step: 1, rooms: 2, participants: 16 });
  assert.deepEqual(summary.bots, { attempted: 16, steady: 16, failed: 0, transportViolations: 0 });
  assert.equal(summary.transport.policyOk, true);
  assert.deepEqual(summary.transport.tlsVersion, { FEFC: 32 });

  const [row] = summary.steps;
  assert.equal(row.step, 1);
  assert.equal(row.participants, 16);
  assert.equal(row.tStart, T0);
  assert.equal(row.tJoinEnd, T0 + 8000); // 16 bots at 2 per second
  assert.equal(row.tMeasureStart, T0 + 18_000);
  assert.equal(row.tEnd, T0 + 48_000);
  assert.deepEqual([row.join.attempted, row.join.ok, row.join.failed, row.join.notSteady], [16, 16, 0, 0]);
  assert.equal(row.join.p95Ms, 300);
  assert.equal(row.join.allPeersP50Ms, 3000);
  assert.equal(row.window.lossPct, 0);
  assert.equal(row.window.freezeEvents, 0);
  assert.deepEqual(row.window.audioPubPps, { p50: 16.5, min: 16.5, max: 16.5 });
  assert.equal(row.verdict, 'ok');
  assert.deepEqual(row.clientCpu, { avgPct: 20, maxPct: 20, saturated: false });
  assert.equal(row.janusCpu.source, 'none');
  assert.equal(row.checks.cpu.value, null);
  assert.equal(row.lateStartSec, 0);

  // pacing: bot n starts n * 500 ms after the step start
  const startTimes = [...host.bots.values()].map((b) => b.startedAt - T0);
  assert.deepEqual(startTimes.slice(0, 4), [0, 500, 1000, 1500]);
  assert.equal(host.bots.get('r0001-b07').startedAt - T0, 7500);

  // graceful shutdown: every bot left (rate limited), host stopped and closed
  assert.equal(host.stopOrder.length, 16);
  assert.deepEqual(host.events, ['launch', 'prewarm 16', 'stopAll', 'close']);

  // files
  const names = (await readdir(h.dir)).sort();
  assert.deepEqual(names, ['bots.jsonl', 'events.log', 'run.json', 'steps.jsonl', 'summary.json', 'timeseries.jsonl']);
  const bots = await h.readJsonl('bots.jsonl');
  assert.equal(bots.length, 16);
  assert.equal(bots[0].id, 'r0000-b00');
  assert.equal(bots[0].state, 'steady');
  assert.equal(bots[0].derived.e2ee.dec > 0, true);
  assert.equal(bots[0].derived.lossPct, 0);
  assert.equal(bots[0].derived.audioOutPpsAvg, 16.5);
  const steps = await h.readJsonl('steps.jsonl');
  assert.deepEqual(steps, JSON.parse(JSON.stringify(summary.steps)));
  const ts = await h.readJsonl('timeseries.jsonl');
  assert.ok(ts.length >= 20);
  assert.deepEqual(Object.keys(ts[0]).sort(), ['bots', 'clientCpuPct', 'd', 'g', 'janusCpuPct', 'step', 'steady', 't', 'up'].sort());
  const runJson = JSON.parse(await h.read('run.json'));
  assert.equal(runJson.schema, 1);
  assert.equal(runJson.targetId.length, 12);
  assert.equal(runJson.versions.chromium, 'chromium-test');
  assert.equal(runJson.endedAt > runJson.startedAt, true);
  assert.equal(JSON.parse(await h.read('summary.json')).ok, true);
  assert.ok(h.logs.some(([k]) => k === 'progress'));
  await assertNoSecrets(h);
}));

test('ramp until a loss breach: stops there, max sustainable is the last clean step', withHarness(async (run) => {
  const h = await run(['ramp', '--ramp-start', '1', '--ramp-step', '1', '--ramp-max', '6'], { host: { lossPct: (live) => (live > 24 ? 3 : 0) } });
  const { summary } = h;
  assert.deepEqual(summary.steps.map((s) => [s.step, s.rooms, s.verdict]), [[1, 1, 'ok'], [2, 2, 'ok'], [3, 3, 'ok'], [4, 4, 'breach']]);
  assert.equal(summary.stopReason, 'loss');
  assert.deepEqual(summary.maxSustainable, { step: 3, rooms: 3, participants: 24 });
  assert.equal(summary.ok, true);
  assert.equal(summary.steps[3].checks.loss.breached, true);
  assert.equal(summary.steps[3].checks.loss.value, 3);
  assert.deepEqual(summary.steps[3].breachReasons, ['loss']);
  // steps are contiguous: each starts when the previous one ended
  for (let i = 1; i < summary.steps.length; i++) assert.equal(summary.steps[i].tStart, summary.steps[i - 1].tEnd);
  // bots of earlier steps keep running: step 4 has 32 bots
  assert.equal(summary.bots.attempted, 32);
  assert.equal((await h.readJsonl('steps.jsonl')).length, 4);
}));

test('consecutive-window rule: one bad 10 s slice does not stop the ramp, two do', withHarness(async (run) => {
  // run: tMeasureStart = T0 + 18 s (16 bots), windows are [18,28) [28,38) [38,48)
  const slice = (from, to) => (live, t) => (t >= T0 + from && t < T0 + to ? 5 : 0);
  const one = await run(['run', '--rooms', '2'], { host: { lossPct: slice(28_000, 38_000) } });
  assert.equal(one.summary.steps[0].checks.loss.breached, false);
  assert.equal(one.summary.steps[0].verdict, 'ok');
  const two = await run(['run', '--rooms', '2'], { host: { lossPct: slice(28_000, 48_000) } });
  assert.equal(two.summary.steps[0].checks.loss.breached, true);
  assert.equal(two.summary.stopReason, 'loss');
}));

test('freeze is debounced over --breach-windows slices: settle ignored, one isolated slice ok, two consecutive slices stop', withHarness(async (run) => {
  // run of 2 rooms: windows are [18,28) [28,38) [38,48) seconds after T0; every bot has one sample per 2 s
  const at = (from, to) => (live, t) => (t >= T0 + from && t < T0 + to ? 1 : 0);
  const settle = await run(['run', '--rooms', '2'], { host: { freeze: (live, t) => (t < T0 + 18_000 ? 2 : 0) } });
  assert.equal(settle.summary.steps[0].window.freezeEvents, 0);
  assert.deepEqual(settle.summary.steps[0].window.freezeWindows, [0, 0, 0]);
  assert.equal(settle.summary.steps[0].verdict, 'ok');

  // 16 bots freeze once within the same 2 s: a transient hiccup, informational only
  const isolated = await run(['run', '--rooms', '2'], { host: { freeze: at(30_000, 32_000) } });
  const row = isolated.summary.steps[0];
  assert.equal(row.window.freezeEvents, 16);
  assert.equal(row.window.freezeBots, 16);
  assert.deepEqual(row.window.freezeWindows, [0, 16, 0]);
  assert.deepEqual([row.checks.freeze.value, row.checks.freeze.limit, row.checks.freeze.breached], [0, 0, false]);
  assert.equal(row.verdict, 'ok');
  assert.equal(isolated.summary.stopReason, null);

  // the same freezes in two consecutive slices stop the run
  const two = await run(['run', '--rooms', '2'], { host: { freeze: at(36_000, 40_000) } });
  assert.deepEqual(two.summary.steps[0].window.freezeWindows, [0, 16, 16]);
  assert.equal(two.summary.steps[0].checks.freeze.value, 16);
  assert.equal(two.summary.steps[0].checks.freeze.breached, true);
  assert.equal(two.summary.stopReason, 'freeze');
  assert.equal(two.summary.steps[0].verdict, 'breach');

  // tolerance is per slice and strict: 16 per slice is tolerated at 16, not at 15
  const tolerant = await run(['run', '--rooms', '2', '--freeze-tolerance', '16'], { host: { freeze: at(36_000, 40_000) } });
  assert.equal(tolerant.summary.steps[0].checks.freeze.breached, false);
  const strict = await run(['run', '--rooms', '2', '--freeze-tolerance', '15'], { host: { freeze: at(36_000, 40_000) } });
  assert.equal(strict.summary.steps[0].checks.freeze.breached, true);

  // --breach-windows 1 makes any single slice count again
  const single = await run(['run', '--rooms', '2', '--breach-windows', '1'], { host: { freeze: at(30_000, 32_000) } });
  assert.equal(single.summary.stopReason, 'freeze');
}));

test('cpu breach from a cmd source stops the ramp; the window filter ignores earlier readings', withHarness(async (run, t) => {
  const clock = new FakeClock(T0);
  let host;
  const cpuSource = createCpuSource({
    cmd: 'fake',
    now: clock.now,
    exec: async () => ({ stdout: String(host.live() * 4) }),
  });
  const dir = await mkdtemp(path.join(os.tmpdir(), 'qjl-cpu-ramp-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  const cfg = parseCli(['ramp', ...BASE_ARGS, '--ramp-start', '1', '--ramp-max', '8', '--out', dir], ENV).cfg;
  host = new FakeHost(clock, {});
  const summary = await runShard(cfg, host, { now: clock.now, sleep: clock.sleep, roomPlan, mintToken: () => 'x', clientCpu: { sample: () => 20 }, cpuSource });
  // 4 % per bot: 5 rooms = 160 (ok), 6 rooms = 192 > 180
  assert.equal(summary.stopReason, 'cpu');
  assert.deepEqual(summary.maxSustainable, { step: 5, rooms: 5, participants: 40 });
  const last = summary.steps.at(-1);
  assert.equal(last.step, 6);
  assert.equal(last.janusCpu.source, 'cmd');
  assert.equal(last.janusCpu.maxPct, 192);
  assert.equal(last.checks.cpu.breached, true);
  assert.equal(summary.steps[4].janusCpu.maxPct, 160);
  assert.equal(summary.steps[4].checks.cpu.breached, false);
}));

test('join failures: below the limit the ramp continues, above it stops', withHarness(async (run) => {
  // one failing bot: 1/24 = 4.2 % at 3 rooms, 1/32 = 3.1 % at 4 rooms
  const few = await run(['ramp', '--ramp-max', '4'], { host: { failJoin: (cfg) => cfg.id === 'r0002-b00' } });
  assert.equal(few.summary.stopReason, 'max_rooms');
  assert.equal(few.summary.ok, true);
  assert.equal(few.summary.steps.length, 4);
  assert.equal(few.summary.steps[2].join.failed, 1);
  assert.equal(few.summary.bots.failed, 1);
  assert.equal(few.summary.errors.find((e) => e.phase === 'ice').count, 1);
  // two failing bots: 2/16 = 12.5 % at step 2
  const many = await run(['ramp', '--ramp-max', '4'], { host: { failJoin: (cfg) => cfg.id === 'r0001-b00' || cfg.id === 'r0001-b01' } });
  assert.equal(many.summary.stopReason, 'join');
  assert.deepEqual(many.summary.maxSustainable, { step: 1, rooms: 1, participants: 8 });
  assert.equal(many.summary.steps.length, 2);
  assert.equal(many.summary.ok, true);
  // a join breach in step 1 means nothing was measured: not ok
  const first = await run(['ramp', '--ramp-max', '4'], { host: { failJoin: () => true } });
  assert.equal(first.summary.stopReason, 'join');
  assert.equal(first.summary.maxSustainable, null);
  assert.equal(first.summary.ok, false);
}));

test('a bot that cannot be started counts as a failed join and its error is scrubbed', withHarness(async (run) => {
  const h = await run(['run', '--rooms', '1'], { host: { startFails: (cfg) => cfg.id.endsWith('b03') } });
  assert.equal(h.summary.steps[0].join.failed, 1);
  assert.equal(h.summary.steps[0].checks.join.breached, true);
  assert.equal(h.summary.ok, false);
  const err = h.summary.errors.find((e) => e.code === 'start_failed');
  assert.equal(err.count, 1);
  assert.ok(err.sample.includes('<redacted>') || err.sample.includes('<url>'));
  await assertNoSecrets(h);
}));

test('client saturation turns a breach into an inconclusive verdict but still stops', withHarness(async (run) => {
  const lossy = { lossPct: (live) => (live > 16 ? 4 : 0) };
  const sat = await run(['ramp', '--ramp-max', '5'], { host: lossy, deps: { clientCpu: { sample: () => 95 } } });
  assert.equal(sat.summary.stopReason, 'loss');
  assert.equal(sat.summary.steps.at(-1).verdict, 'inconclusive_client_saturated');
  assert.equal(sat.summary.steps.at(-1).clientCpu.saturated, true);
  assert.equal(sat.summary.client.saturated, true);
  assert.equal(sat.summary.client.cpuMaxPct, 95);
  // without a breach, saturation is only reported
  const calm = await run(['run', '--rooms', '1'], { deps: { clientCpu: { sample: () => 95 } } });
  assert.equal(calm.summary.steps[0].verdict, 'ok');
  assert.equal(calm.summary.steps[0].clientCpu.saturated, true);
}));

test('ramp that never breaches ends with max_rooms', withHarness(async (run) => {
  const h = await run(['ramp', '--ramp-start', '1', '--ramp-step', '2', '--ramp-max', '5']);
  assert.deepEqual(h.summary.steps.map((s) => s.rooms), [1, 3, 5]);
  assert.equal(h.summary.stopReason, 'max_rooms');
  assert.deepEqual(h.summary.maxSustainable, { step: 3, rooms: 5, participants: 40 });
  assert.equal(h.summary.ok, true);
}));

test('start-at: waits for the instant, reports lateStartSec when late', withHarness(async (run) => {
  const wait = await run(['run', '--rooms', '1', '--start-at', String(T0 / 1000 + 100)]);
  assert.equal(wait.summary.steps[0].tStart, T0 + 100_000);
  assert.equal(wait.summary.steps[0].lateStartSec, 0);
  const late = await run(['ramp', '--ramp-max', '2', '--start-at', String(T0 / 1000 - 30)]);
  assert.equal(late.summary.steps[0].tStart, T0);
  assert.equal(late.summary.steps[0].lateStartSec, 30);
  assert.ok((await late.read('events.log')).includes('lateStartSec'));
}));

test('two shards of one run produce identical step timelines', withHarness(async (run) => {
  const args = ['ramp', '--ramp-start', '1', '--ramp-step', '1', '--ramp-max', '3', '--start-at', String(T0 / 1000 + 5)];
  const a = await run([...args, '--shard', '0/2']);
  const b = await run([...args, '--shard', '1/2']);
  const timeline = (s) => s.steps.map((r) => [r.step, r.rooms, r.participants, r.tStart, r.tJoinEnd, r.tMeasureStart, r.tEnd]);
  assert.deepEqual(timeline(a.summary), timeline(b.summary));
  assert.equal(a.summary.steps.length, 3);
  // shard 0 owns rooms 0 and 2, shard 1 owns room 1
  assert.deepEqual([...a.host.bots.keys()].map((id) => id.slice(0, 5)).filter((v, i, arr) => arr.indexOf(v) === i), ['r0000', 'r0002']);
  assert.deepEqual([...b.host.bots.keys()].map((id) => id.slice(0, 5)).filter((v, i, arr) => arr.indexOf(v) === i), ['r0001']);
  for (let i = 0; i < 3; i++) {
    assert.equal(a.summary.steps[i].shardParticipants + b.summary.steps[i].shardParticipants, a.summary.steps[i].participants);
  }
}));

test('SIGINT-style abort: graceful leave, host closed, summary written, interrupted step not recorded', withHarness(async (run) => {
  const ac = new AbortController();
  const h = await run(['ramp', '--ramp-max', '4'], { host: { onPoll: (n) => n === 30 && ac.abort() }, deps: { signal: ac.signal } });
  assert.equal(h.summary.ok, false);
  assert.equal(h.summary.stopReason, 'fatal');
  assert.ok(h.summary.errors.some((e) => e.code === 'aborted'));
  assert.equal(h.host.closed, true);
  assert.equal(h.host.stopOrder.length, h.host.bots.size);
  assert.ok(h.host.bots.size > 0);
  const stored = JSON.parse(await h.read('summary.json'));
  assert.equal(stored.stopReason, 'fatal');
  assert.ok(stored.steps.length < 4);
}));

test('summary.json is written when the host throws', withHarness(async (run) => {
  const launch = await run(['run', '--rooms', '1'], { host: { launchError: `Chromium missing at ${WS}` } });
  assert.equal(launch.summary.ok, false);
  assert.equal(launch.summary.stopReason, 'fatal');
  assert.equal(launch.host.closed, true);
  assert.ok(launch.summary.errors[0].sample.includes('Chromium missing'));
  await assertNoSecrets(launch);
  assert.equal(JSON.parse(await launch.read('summary.json')).ok, false);

  // a host whose poll always throws: the run finishes, the bots are counted as not steady
  const poll = await run(['run', '--rooms', '1'], { host: { pollError: true } });
  assert.equal(poll.summary.steps[0].join.notSteady, 8);
  assert.equal(poll.summary.steps[0].breached, true);
  assert.equal(poll.host.closed, true);
  assert.ok((await poll.read('events.log')).includes('pollAll failed'));
}));

test('an internal error mid-run still leaves bots and writes summary.json', withHarness(async (run) => {
  const h = await run(['ramp', '--ramp-max', '3'], { deps: { roomPlan: (seed, k, size) => { if (k === 1) throw new Error('planner exploded'); return roomPlan(seed, k, size); } } });
  assert.equal(h.summary.stopReason, 'fatal');
  assert.equal(h.summary.ok, false);
  assert.equal(h.summary.steps.length, 1);
  assert.equal(h.host.closed, true);
  assert.equal(h.host.stopOrder.length, 8);
  assert.ok(h.summary.errors.some((e) => e.sample.includes('planner exploded')));
}));

test('duration cap: never starts a step it cannot finish', withHarness(async (run) => {
  // 1 room: join 4 s + settle 10 + hold 30 = 44 s per step, cap 100 s -> 2 steps
  const h = await run(['ramp', '--ramp-max', '5', '--duration-cap-sec', '100']);
  assert.equal(h.summary.steps.length, 2);
  assert.equal(h.summary.stopReason, 'duration_cap');
  assert.equal(h.summary.ok, true);
  const none = await run(['ramp', '--ramp-max', '5', '--duration-cap-sec', '10']);
  assert.equal(none.summary.steps.length, 0);
  assert.equal(none.summary.stopReason, 'duration_cap');
  assert.equal(none.summary.ok, false);
}));

test('session tokens are refreshed for all live bots before they expire', withHarness(async (run) => {
  const h = await run(['run', '--rooms', '1', '--hold-sec', '130', '--token-ttl-sec', '120', '--token-refresh-sec', '60']);
  const calls = h.host.tokenCalls;
  assert.ok(calls.length >= 2, `expected >= 2 refreshes, got ${calls.length}`);
  for (const c of calls) assert.equal(c.ids.length, 8);
  assert.notEqual(calls[0].tokens[0], calls[1].tokens[0]);
  for (let i = 1; i < calls.length; i++) assert.ok(calls[i].t - calls[i - 1].t <= 64_000);
  assert.ok(calls[0].t - T0 <= 62_000);
  await assertNoSecrets(h);
}));

test('pre-minted mode: bot (k, i) gets token k*size+i, nothing is minted or refreshed, no secret is needed', withHarness(async (run) => {
  const tokens = Array.from({ length: 16 }, (_, n) => `${Math.floor(T0 / 1000) + 7200},janus,janus.plugin.videoroom:PREMINTED${String(n).padStart(2, '0')}${'A'.repeat(31)}=`);
  const env = { QJANUS_WS_URL: WS, QJANUS_LOADTEST_SEED: 'SEEDVALUE123', QJANUS_SESSION_TOKENS: JSON.stringify(tokens) };
  // no mintToken dep: the orchestrator's own default (the pre-minted list) is used
  const h = await run(['run', '--rooms', '2', '--hold-sec', '700'], { env, deps: { mintToken: undefined } });
  assert.equal(h.summary.ok, true);
  assert.equal(h.host.bots.size, 16);
  for (const [id, b] of h.host.bots) {
    const k = Number(id.slice(1, 5));
    const i = Number(id.slice(7));
    assert.equal(b.cfg.sessionToken, tokens[k * 8 + i], `token of ${id}`);
  }
  assert.equal(h.host.tokenCalls.length, 0, 'pre-minted tokens are never refreshed (the run lasted > 700 s)');
  assert.equal(h.cfg.secrets.tokenSecret, null);
  for (const name of await readdir(h.dir)) {
    const text = await h.read(name);
    for (const t of tokens) assert.ok(!text.includes(t.split(':')[1]), `${name} contains a session token`);
  }
}));

test('a transport policy violation is fatal, not a capacity result, even with a saturated client', withHarness(async (run) => {
  const h = await run(['ramp', '--ramp-max', '3'], { host: { transportBad: (cfg) => cfg.id === 'r0000-b02' }, deps: { clientCpu: { sample: () => 99 } } });
  assert.equal(h.summary.stopReason, 'transport');
  assert.equal(h.summary.ok, false);
  assert.equal(h.summary.bots.transportViolations, 1);
  assert.equal(h.summary.transport.policyOk, false);
  const row = h.summary.steps[0];
  assert.deepEqual(row.breachReasons.slice(0, 1), ['transport']);
  assert.equal(row.verdict, 'breach');
  assert.equal(row.checks.transport.value, 1);
  assert.ok(row.tEnd < row.tMeasureStart + 30_000, 'the step is cut short');
  assert.equal(h.summary.maxSustainable, null);
}));

test('a page crash makes the run not ok', withHarness(async (run) => {
  const h = await run(['run', '--rooms', '1'], { host: { crash: (b) => b.id === 'r0000-b05' } });
  assert.equal(h.summary.ok, false);
  assert.ok(h.summary.errors.some((e) => e.code === 'page_crash'));
}));

test('--manage-rooms: own rooms are created before the browser starts and destroyed after it closed, also after a fatal error', withHarness(async (run) => {
  const calls = [];
  const rooms = {
    create: async (ks) => calls.push(['create', ks]),
    destroy: async (ks) => calls.push(['destroy', ks]),
  };
  const env = { ...ENV, QJANUS_ADMIN_URL: 'http://127.0.0.1:8088/janus', QJANUS_ADMIN_KEY: 'ADMINKEYVALUE' };
  const dir = await mkdtemp(path.join(os.tmpdir(), 'qjl-rooms-'));
  try {
    const cfg = parseCli(['ramp', ...BASE_ARGS, '--ramp-max', '5', '--shard', '1/2', '--manage-rooms', '--out', dir], env).cfg;
    const clock = new FakeClock();
    const host = new FakeHost(clock, { launchError: 'no chromium' });
    const order = [];
    const wrapped = { create: async (ks) => { order.push('create'); await rooms.create(ks); }, destroy: async (ks) => { order.push('destroy'); await rooms.destroy(ks); } };
    const origClose = host.close.bind(host);
    host.close = async () => { order.push('close'); await origClose(); };
    const summary = await runShard(cfg, host, { now: clock.now, sleep: clock.sleep, roomPlan, mintToken: () => 'x', clientCpu: { sample: () => 1 }, rooms: wrapped });
    assert.deepEqual(calls, [['create', [1, 3]], ['destroy', [1, 3]]]);
    assert.deepEqual(order, ['create', 'close', 'destroy']);
    assert.equal(summary.stopReason, 'fatal');
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
  // and a normal run
  const h = await run(['run', '--rooms', '2'], { deps: { rooms } });
  assert.equal(h.summary.ok, true);
}));

test('default adapters: real token, ids and janus-admin modules against the mock Janus HTTP API', async (t) => {
  const { startMockJanus } = await import('./helpers/mock-janus-http.mjs');
  const mock = await startMockJanus({ tokenSecret: ENV.QJANUS_TOKEN_SECRET, adminKey: 'ADMINKEYVALUE' });
  const dir = await mkdtemp(path.join(os.tmpdir(), 'qjl-adapters-'));
  t.after(async () => {
    await mock.close();
    await rm(dir, { recursive: true, force: true });
  });
  const env = { ...ENV, QJANUS_ADMIN_URL: mock.url, QJANUS_ADMIN_KEY: 'ADMINKEYVALUE' };
  const cfg = parseCli(['ramp', ...BASE_ARGS, '--ramp-max', '4', '--shard', '1/2', '--manage-rooms', '--out', dir], env).cfg;
  const clock = new FakeClock();
  let roomsAtLaunch = null;
  const host = new FakeHost(clock, { onLaunch: () => { roomsAtLaunch = [...mock.rooms.keys()].sort(); } });
  const summary = await runShard(cfg, host, { now: clock.now, sleep: clock.sleep, clientCpu: { sample: () => 1 } });
  assert.equal(summary.ok, true, JSON.stringify(summary.errors));
  // this shard (k % 2 == 1) owns rooms 1 and 3; both exist before the browsers start and are gone afterwards
  assert.deepEqual(roomsAtLaunch, [roomId(ENV.QJANUS_LOADTEST_SEED, 1), roomId(ENV.QJANUS_LOADTEST_SEED, 3)].sort());
  assert.equal(mock.rooms.size, 0);
  const bot = host.bots.get('r0003-b02').cfg;
  assert.equal(bot.room, roomId(ENV.QJANUS_LOADTEST_SEED, 3));
  assert.equal(bot.pseudonym, pseudonym(ENV.QJANUS_LOADTEST_SEED, 3, 2));
  assert.equal(verifySessionToken(bot.sessionToken, { secret: ENV.QJANUS_TOKEN_SECRET }), true);
  assert.deepEqual([...new Set([...host.bots.values()].map((b) => b.cfg.room))].length, 2);
});

test('the shard that owns no rooms in a step just waits and reports empty measurements', withHarness(async (run) => {
  const h = await run(['run', '--rooms', '1', '--shard', '1/2']);
  assert.equal(h.summary.steps[0].shardParticipants, 0);
  assert.equal(h.summary.steps[0].join.attempted, 0);
  assert.equal(h.summary.steps[0].verdict, 'ok');
  assert.equal(h.summary.bots.attempted, 0);
  assert.ok((await h.read('events.log')).includes('owns no rooms'));
}));

// ------------------------------------------------------------------ host side

test('static server: MIME types, index, traversal and 404', async (t) => {
  const dir = await mkdtemp(path.join(os.tmpdir(), 'qjl-static-'));
  t.after(() => rm(dir, { recursive: true, force: true }));
  await writeFile(path.join(dir, 'page.html'), '<!doctype html><title>x</title>');
  await writeFile(path.join(dir, 'bot.mjs'), 'export const a = 1;');
  await writeFile(path.join(dir, 'e2ee-worker.js'), 'self.onmessage = () => {};');
  const server = await startStaticServer(dir);
  t.after(() => server.close());
  assert.match(server.url, /^http:\/\/127\.0\.0\.1:\d+$/);
  const get = (p) => fetch(server.url + p);
  const idx = await get('/');
  assert.equal(idx.status, 200);
  assert.match(idx.headers.get('content-type'), /^text\/html/);
  assert.match((await get('/bot.mjs')).headers.get('content-type'), /^text\/javascript/);
  assert.match((await get('/e2ee-worker.js')).headers.get('content-type'), /^text\/javascript/);
  assert.equal((await get('/nope.js')).status, 404);
  assert.notEqual((await get('/..%2f..%2fetc%2fpasswd')).status, 200);
  assert.equal((await fetch(server.url + '/page.html', { method: 'POST' })).status, 405);
});

test('chromium args: base flags, field trial, extras', () => {
  const args = buildChromiumArgs({ fieldTrials: 'WebRTC-EnableDtlsPqc/Enabled/', extraArgs: ['--disable-gpu'] });
  for (const flag of ['--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--disable-features=WebRtcHideLocalIpsWithMdns', '--allow-loopback-in-peer-connection', '--no-sandbox', '--autoplay-policy=no-user-gesture-required', '--disable-background-timer-throttling', '--disable-renderer-backgrounding', '--disable-backgrounding-occluded-windows']) {
    assert.ok(args.includes(flag), flag);
  }
  assert.ok(args.includes('--force-fieldtrials=WebRTC-EnableDtlsPqc/Enabled/'));
  assert.equal(args.at(-1), '--disable-gpu');
  assert.ok(!buildChromiumArgs({ fieldTrials: '' }).some((a) => a.startsWith('--force-fieldtrials')));
});

/** Fake Playwright: pages run the qbot calls against an in-memory window. */
class FakePage {
  constructor(label, log) {
    this.label = label;
    this.log = log;
    this.dead = false;
    this.handlers = {};
    this.bots = new Map();
    this.window = {
      qbotReady: true,
      qbot: {
        start: async (cfg) => { this.bots.set(cfg.id, 'init'); return { ok: true }; },
        poll: () => ({ now: 1, bots: [...this.bots.keys()].map((id) => ({ id, state: 'steady', error: null, timings: {}, tot: null, samples: [{ t: 1 }], events: [] })) }),
        setTokens: async (m) => { this.log.push([this.label, 'tokens', Object.keys(m).sort()]); },
        stop: async (id) => { this.bots.delete(id); return { id, state: 'closed' }; },
        stopAll: async () => { const ids = [...this.bots.keys()]; this.bots.clear(); return ids.map((id) => ({ id, state: 'closed' })); },
      },
    };
  }

  on(event, handler) { this.handlers[event] = handler; }
  async goto() {}
  async waitForFunction() {}
  async evaluate(fn, arg) {
    if (this.dead) throw new Error('Target page, context or browser has been closed');
    globalThis.window = this.window;
    return fn(arg);
  }
}

function fakeLauncher(log) {
  const browsers = [];
  return {
    browsers,
    async launch(opts) {
      const idx = browsers.length;
      const browser = {
        idx,
        opts,
        pages: [],
        closed: false,
        version: () => 'Chromium/153.0',
        on() {},
        newContext: async () => ({ newPage: async () => { const p = new FakePage(`b${idx}p${browser.pages.length}`, log); browser.pages.push(p); return p; } }),
        close: async () => { browser.closed = true; },
      };
      browsers.push(browser);
      return browser;
    },
  };
}

test('BrowserPoolHost: bots are spread over browsers and pages, polling is per page and crash tolerant', async () => {
  const log = [];
  const launcher = fakeLauncher(log);
  const host = new BrowserPoolHost({ botsPerBrowser: 2, botsPerPage: 1, launcher, fieldTrials: 'X/Y/', extraArgs: ['--a'], chromiumPath: 'chrome-path' });
  await host.launch();
  assert.equal(host.versions().chromium, 'Chromium/153.0');
  assert.equal(launcher.browsers.length, 1);
  assert.equal(launcher.browsers[0].opts.headless, true);
  assert.equal(launcher.browsers[0].opts.executablePath, 'chrome-path');
  for (const id of ['a', 'b', 'c', 'd', 'e']) await host.startBot({ id });
  assert.equal(launcher.browsers.length, 3); // 2 bots per browser
  assert.equal(launcher.browsers[0].pages.length, 2); // 1 bot per page
  assert.equal(launcher.browsers[2].pages.length, 1);

  let polled = await host.pollAll();
  assert.deepEqual(polled.map((s) => `${s.id}:${s.browser}/${s.page}`).sort(), ['a:0/0', 'b:0/1', 'c:1/0', 'd:1/1', 'e:2/0']);
  assert.equal(polled[0].samples.length, 1);

  await host.setTokens({ a: 't1', b: 't2', e: 't3' });
  assert.deepEqual(log.filter((l) => l[1] === 'tokens').map((l) => l[2].join()).sort(), ['a', 'b', 'e']);

  // a page dies: its bot is reported failed, the others are unaffected, nothing throws
  launcher.browsers[1].pages[0].dead = true;
  polled = await host.pollAll();
  const c = polled.find((s) => s.id === 'c');
  assert.equal(c.state, 'failed');
  assert.deepEqual([c.error.phase, c.error.code], ['other', 'page_crash']);
  assert.equal(polled.find((s) => s.id === 'd').state, 'steady');
  // it stays failed on later polls
  assert.equal((await host.pollAll()).find((s) => s.id === 'c').state, 'failed');
  assert.equal(await host.stopBot('c', { graceful: true }), null);
  assert.equal((await host.stopBot('a', { graceful: true })).state, 'closed');
  const finals = await host.stopAll({ graceful: false });
  assert.deepEqual(finals.map((f) => f.id).sort(), ['b', 'd', 'e']);

  await host.close();
  await host.close(); // idempotent
  assert.ok(launcher.browsers.every((b) => b.closed));
});

test('BrowserPoolHost.prewarm opens every browser and page ahead of the first bot', async () => {
  const launcher = fakeLauncher([]);
  const host = new BrowserPoolHost({ botsPerBrowser: 2, botsPerPage: 1, launcher });
  await host.launch();
  await host.prewarm(5);
  assert.deepEqual(launcher.browsers.map((b) => b.pages.length), [2, 2, 1]);
  for (const id of ['a', 'b', 'c', 'd', 'e']) await host.startBot({ id });
  assert.deepEqual(launcher.browsers.map((b) => b.pages.length), [2, 2, 1]); // nothing new was opened
  await host.close();
});

test('BrowserPoolHost smoke: real Chromium loads page/page.html and window.qbotReady (skips when unavailable)', { timeout: 120_000 }, async (t) => {
  if (!existsSync(path.join(PAGE_DIR, 'page.html'))) return t.skip('page/page.html not present yet');
  const host = new BrowserPoolHost({ botsPerBrowser: 2, botsPerPage: 1, fieldTrials: 'WebRTC-EnableDtlsPqc/Enabled/' });
  try {
    try {
      await host.launch();
    } catch (err) {
      return t.skip(`Chromium is not available: ${String(err.message).split('\n')[0]}`);
    }
    const entry = await host.browserEntry(0);
    const pe = await host.pageEntry(entry, 0);
    assert.equal(await pe.page.evaluate(() => window.qbotReady), true);
    const info = await pe.page.evaluate(() => window.qbot.info());
    assert.ok(info && typeof info.chromeVersion !== 'undefined');
    assert.deepEqual(await host.pollAll(), []);
    assert.ok(host.versions().chromium);
  } finally {
    await host.close();
  }
});


test('integration: runShard drives real Chromium bots against a mock Janus that refuses the room (skips when unavailable)', { timeout: 240_000 }, async (t) => {
  if (!existsSync(path.join(PAGE_DIR, 'page.html'))) return t.skip('page/page.html not present yet');
  const { startMockJanus } = await import('./helpers/mock-janus-ws.mjs');
  const janus = await startMockJanus({ plugin: () => ({ error: { code: 426, reason: 'No such room' } }) });
  const dir = await mkdtemp(path.join(os.tmpdir(), 'qjl-it-'));
  t.after(async () => {
    await janus.close();
    await rm(dir, { recursive: true, force: true });
  });
  const cfg = parseCli(['run', '--rooms', '1', '--scenario', 'video4', '--video-profile', 'tiny', '--hold-sec', '6', '--settle-sec', '3', '--join-rate', '8', '--bots-per-browser', '4', '--bots-per-page', '2', '--e2ee', 'none', '--expect-transport', 'off', '--run-id', 'it', '--quiet', '--ws-url', janus.url, '--out', dir], ENV).cfg;
  const host = new BrowserPoolHost({ botsPerBrowser: cfg.botsPerBrowser, botsPerPage: cfg.botsPerPage, fieldTrials: cfg.fieldTrials });
  const summary = await runShard(cfg, host, { roomPlan, mintToken: () => 'TOKEN-SENTINEL-1' });
  if (summary.errors.some((e) => e.code === 'fatal' && /Executable doesn't exist|browserType\.launch/.test(e.sample || ''))) return t.skip('Chromium is not available');
  assert.equal(summary.bots.attempted, 4);
  assert.equal(summary.bots.failed, 4);
  assert.equal(summary.steps.length, 1);
  assert.equal(summary.steps[0].join.failed, 4);
  assert.equal(summary.stopReason, 'join');
  assert.equal(summary.ok, false);
  assert.ok(summary.errors.some((e) => e.phase === 'join'), JSON.stringify(summary.errors));
  const bots = (await readFile(path.join(dir, 'bots.jsonl'), 'utf8')).trim().split('\n').map((l) => JSON.parse(l));
  assert.equal(bots.length, 4);
  assert.ok(bots.every((b) => b.state === 'failed' && b.error && b.timings.wsMs !== undefined));
  for (const name of await readdir(dir)) {
    const text = await readFile(path.join(dir, name), 'utf8');
    assert.ok(!text.includes('ws://127.0.0.1') && !text.includes('TOKEN-SENTINEL'), `${name} leaks the target or a token`);
  }
});
