// Shard orchestrator: drives one shard of a run/ramp through a BotHost.
//
// Timeline of a step (all shards use the same durations, see config.buildSchedule):
//   tStart .. tJoinEnd          new bots are started, paced by --join-rate
//   tJoinEnd .. tMeasureStart   settle (--settle-sec): ICE/DTLS/renegotiation storms pass
//   tMeasureStart .. tEnd       measurement window (--hold-sec), judged in 10 s slices
// The bots of earlier steps keep running: step j has K_j rooms in total.
// Joins that are late are not waited for: they show up as join times, as
// not-steady bots, and (if too many) as a join breach.
//
// Step numbering is 1-based everywhere in the output files.
//
// Everything with a side effect on the world (clock, sleep, token minting,
// room admin, CPU sources) is injectable so the state machine runs against a
// FakeHost and a fake clock in test/ramp.test.mjs.

import { createWriteStream } from 'node:fs';
import { mkdir, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import { redactedConfig, scrubSecrets, targetId } from './config.mjs';
import { createClientCpuMonitor, createCpuSource } from './cpu-source.mjs';
import * as M from './metrics.mjs';
import { botId, buildBotConfig } from './scenarios.mjs';

export const POLL_INTERVAL_MS = 2000;

const LEAVE_TIMEOUT_MS = 30_000;
const HOST_CLOSE_TIMEOUT_MS = 20_000;
const ROOMS_TIMEOUT_MS = 60_000;
const MAX_BOT_EVENT_LINES = 300;

// -------------------------------------------------------------- default deps

function defaultSleep(ms, signal) {
  return new Promise((resolve) => {
    if (signal && signal.aborted) return resolve();
    const done = () => {
      clearTimeout(timer);
      if (signal) signal.removeEventListener('abort', done);
      resolve();
    };
    const timer = setTimeout(done, Math.max(0, ms));
    if (signal) signal.addEventListener('abort', done, { once: true });
  });
}

/** Real-time timeout that never keeps the process alive; resolves 'timeout' when it fires first. */
function raceTimeout(promise, ms) {
  let timer;
  const timeout = new Promise((resolve) => {
    timer = setTimeout(resolve, ms, 'timeout');
    if (timer.unref) timer.unref();
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

// Adapters to src/token.mjs, src/ids.mjs and src/janus-admin.mjs, loaded lazily
// so the orchestrator can be tested without them.
async function defaultMintToken(cfg) {
  if (cfg.secrets.sessionTokens) {
    // Pre-minted mode: one token per bot, minted on the node. Bot (room k, member i) gets
    // sessionTokens[k * roomSize + i]; config.mjs checked that the list covers every room of the ramp.
    const { sessionTokens } = cfg.secrets;
    return (plan, index) => sessionTokens[plan.k * cfg.roomSize + index];
  }
  const { mintSessionToken } = await import('./token.mjs');
  return () => mintSessionToken({ secret: cfg.secrets.tokenSecret, ttlSec: cfg.token.ttlSec });
}

async function defaultRoomPlan() {
  const { roomPlan } = await import('./ids.mjs');
  return roomPlan;
}

/** Sorted room indices as contiguous runs [{ first, count }] (janus-admin works on ranges). */
function contiguousRuns(indices) {
  const runs = [];
  for (const k of indices) {
    const last = runs[runs.length - 1];
    if (last && k === last.first + last.count) last.count++;
    else runs.push({ first: k, count: 1 });
  }
  return runs;
}

async function defaultRooms(cfg) {
  const admin = await import('./janus-admin.mjs');
  const client = new admin.JanusAdmin({
    baseUrl: cfg.secrets.adminUrl,
    tokenSecret: cfg.secrets.tokenSecret,
    adminKey: cfg.secrets.adminKey,
  });
  const seed = cfg.secrets.seed;
  return {
    async create(indices) {
      for (const run of contiguousRuns(indices)) await admin.createRooms(client, { seed, ...run, size: cfg.roomSize });
    },
    async destroy(indices) {
      try {
        for (const run of contiguousRuns(indices)) await admin.destroyRooms(client, { seed, ...run });
      } finally {
        await client.close();
      }
    },
  };
}

function resolveDeps(cfg, deps) {
  return {
    now: deps.now ?? Date.now,
    sleep: deps.sleep ?? defaultSleep,
    log: deps.log ?? (() => {}),
    signal: deps.signal ?? null,
    mintToken: deps.mintToken ?? null,
    roomPlan: deps.roomPlan ?? null,
    rooms: deps.rooms ?? null,
    cpuSource: deps.cpuSource ?? createCpuSource({ file: cfg.cpu.file, cmd: cfg.cpu.cmd }),
    clientCpu: deps.clientCpu ?? createClientCpuMonitor(),
  };
}

const fmt = (v, digits = 1) => (v === null || v === undefined ? '-' : String(Math.round(v * 10 ** digits) / 10 ** digits));

// ---------------------------------------------------------------- ShardRun

class ShardRun {
  constructor(cfg, host, d) {
    this.cfg = cfg;
    this.host = host;
    this.d = d;
    this.dir = cfg.out;
    this.bots = new Map(); // id -> record (see launchBot)
    this.steps = []; // finished step rows
    this.stepAgg = []; // raw per-step sums for the summary
    this.runErrors = []; // errors that are not bot errors
    this.stopReason = null;
    this.abort = null; // null | 'signal' | 'cap'
    this.transportFatal = false;
    this.roomsTouched = false;
    this.versions = { node: process.version, playwright: null, chromium: null };
    this.startedAt = d.now();
    this.lastRefreshMs = this.startedAt;
    this.botEventLines = 0;
    this.streams = new Map(); // append-only files: events.log and *.jsonl
  }

  // ---- small helpers

  scrub(text) {
    return scrubSecrets(text, this.cfg);
  }

  event(kind, msg) {
    const line = `${new Date(this.d.now()).toISOString()} ${kind.toUpperCase()} ${this.scrub(msg)}`;
    this.appendLine('events.log', line);
    this.d.log(kind, line);
  }

  noteError(code, err) {
    const message = this.scrub(err && err.message ? err.message : String(err));
    this.runErrors.push({ phase: 'other', code, count: 1, sample: message });
    this.event('warn', `${code}: ${message}`);
  }

  sleep(ms) {
    return this.d.sleep(ms, this.d.signal);
  }

  /** Reason to leave the step loop early: 'signal' | 'cap' (aborts, sticky) | 'transport' | null. */
  mustStop() {
    if (!this.abort) {
      if (this.d.signal && this.d.signal.aborted) this.abort = 'signal';
      else if (this.cfg.durationCapSec && this.d.now() >= this.startedAt + this.cfg.durationCapSec * 1000) this.abort = 'cap';
    }
    if (this.abort) return this.abort;
    return this.transportFatal ? 'transport' : null;
  }

  async writeJson(name, obj) {
    try {
      await writeFile(path.join(this.dir, name), `${JSON.stringify(obj, null, 2)}\n`);
    } catch (err) {
      this.noteError(`write_${name}`, err);
    }
  }

  /** Append-only files share one write stream each; a failing stream never stops the run. */
  appendLine(name, text) {
    let stream = this.streams.get(name);
    if (!stream) {
      stream = createWriteStream(path.join(this.dir, name), { flags: 'a' });
      stream.on('error', () => {});
      this.streams.set(name, stream);
    }
    stream.write(`${text}\n`);
  }

  appendJsonl(name, obj) {
    this.appendLine(name, JSON.stringify(obj));
  }

  async closeStreams() {
    await Promise.all([...this.streams.values()].map((stream) => new Promise((resolve) => {
      if (stream.destroyed) return resolve();
      stream.once('close', resolve);
      stream.end();
    })));
  }

  runJson(endedAt) {
    return {
      schema: 1,
      runId: this.cfg.runId,
      scenario: this.cfg.scenario,
      mode: this.cfg.mode,
      targetId: targetId(this.cfg.secrets.wsUrl),
      shard: { ...this.cfg.shard },
      startedAt: this.startedAt,
      endedAt,
      config: redactedConfig(this.cfg),
      versions: this.versions,
      host: { cpus: os.cpus().length, memMB: Math.round(os.totalmem() / 2 ** 20), platform: process.platform },
    };
  }

  /** Room indices this shard owns among all rooms of the schedule. */
  ownRooms() {
    return this.cfg.schedule.steps.flatMap((s) => s.newRooms);
  }

  // ---- run

  async execute() {
    try {
      await this.prepare();
      await this.rampLoop();
    } catch (err) {
      this.stopReason = 'fatal';
      this.noteError('fatal', err);
    } finally {
      await this.shutdown();
    }
    return this.summary;
  }

  async prepare() {
    await mkdir(this.dir, { recursive: true });
    // Seams that default to src/token.mjs, src/ids.mjs and src/janus-admin.mjs; resolved here so a
    // failure is a normal fatal error (summary.json is still written).
    this.d.mintToken ??= await defaultMintToken(this.cfg);
    this.d.roomPlan ??= await defaultRoomPlan();
    if (this.cfg.manageRooms) this.d.rooms ??= await defaultRooms(this.cfg);
    await this.writeJson('run.json', this.runJson(null));
    const total = this.cfg.schedule.steps.length;
    this.event('info', `run ${this.cfg.runId}: ${this.cfg.scenario}, ${this.cfg.mode}, shard ${this.cfg.shard.index}/${this.cfg.shard.count}, ${total} step(s), target ${targetId(this.cfg.secrets.wsUrl)}`);
    if (this.ownRooms().length === 0) this.event('warn', 'this shard owns no rooms in this schedule');
    if (this.d.rooms) {
      this.roomsTouched = true;
      this.event('info', `creating ${this.ownRooms().length} room(s)`);
      await raceTimeout(this.d.rooms.create(this.ownRooms()), ROOMS_TIMEOUT_MS).then((r) => {
        if (r === 'timeout') throw new Error('room creation timed out');
      });
    }
    await this.host.launch();
    const maxBots = this.cfg.schedule.steps[this.cfg.schedule.steps.length - 1].shardParticipants;
    if (maxBots > 0 && typeof this.host.prewarm === 'function') {
      this.event('info', `opening browsers and pages for up to ${maxBots} bots`);
      await this.host.prewarm(maxBots);
    }
    this.versions = { ...this.versions, ...this.host.versions() };
    await this.writeJson('run.json', this.runJson(null));
  }

  async rampLoop() {
    const { cfg } = this;
    const t0 = cfg.startAt !== null ? cfg.startAt * 1000 : this.d.now();
    if (t0 > this.d.now()) this.event('info', `waiting ${Math.round((t0 - this.d.now()) / 1000)}s for --start-at`);
    for (const sp of cfg.schedule.steps) {
      const planned = t0 + sp.offsetSec * 1000;
      if (cfg.durationCapSec && planned + sp.stepSec * 1000 > this.startedAt + cfg.durationCapSec * 1000) {
        this.abort = 'cap';
      }
      while (!this.mustStop() && this.d.now() < planned) await this.sleep(planned - this.d.now());
      if (this.mustStop() === 'signal' || this.abort === 'cap') break;
      const row = await this.runStep(sp, planned);
      if (!row) break;
      this.steps.push(row);
      this.appendJsonl('steps.jsonl', row);
      this.event('info', `step ${row.step} done: verdict ${row.verdict}${row.breachReasons.length ? ` (${row.breachReasons.join(',')})` : ''}, loss ${fmt(row.window.lossPct, 3)}%, freezes ${fmt(row.window.freezeEvents, 0)}, join failures ${row.join.failed + row.join.notSteady}/${row.join.attempted}`);
      if (row.breached) {
        this.stopReason = row.breachReasons[0];
        return;
      }
    }
    if (this.abort === 'signal') {
      this.stopReason = 'fatal';
      this.runErrors.push({ phase: 'other', code: 'aborted', count: 1, sample: 'stopped by SIGINT/SIGTERM' });
    } else if (this.abort === 'cap') this.stopReason = 'duration_cap';
    else this.stopReason = cfg.mode === 'ramp' ? 'max_rooms' : null;
  }

  // ---- one step

  buildQueue(sp) {
    const queue = [];
    for (const k of sp.newRooms) {
      const plan = this.d.roomPlan(this.cfg.secrets.seed, k, this.cfg.roomSize);
      for (let i = 0; i < this.cfg.roomSize; i++) queue.push({ plan, index: i });
    }
    return queue;
  }

  async runStep(sp, plannedStart) {
    const { cfg } = this;
    const tStart = Math.max(plannedStart, this.d.now());
    const tJoinEnd = tStart + sp.joinWindowSec * 1000;
    const tMeasureStart = tJoinEnd + cfg.settleSec * 1000;
    const tEnd = tMeasureStart + cfg.holdSec * 1000;
    const ctx = { sp, tStart, tJoinEnd, tMeasureStart, tEnd, samples: [], cpuReadings: [], newBotIds: [] };
    const lateStartSec = Math.round((tStart - plannedStart) / 1000);
    if (lateStartSec > 0) this.event('warn', `step ${sp.step} starts ${lateStartSec}s late (lateStartSec)`);
    this.event('info', `step ${sp.step}/${cfg.schedule.steps.length}: ${sp.rooms} rooms, ${sp.participants} participants (this shard: ${sp.shardRooms} rooms, ${sp.newBots} new bots)`);

    const queue = this.buildQueue(sp);
    const gapMs = 1000 / cfg.joinRate;
    let next = 0;
    let nextPollAt = tStart + POLL_INTERVAL_MS;
    while (!this.mustStop()) {
      let t = this.d.now();
      if (t >= tEnd) break;
      while (next < queue.length && tStart + next * gapMs <= t && !this.mustStop()) {
        await this.launchBot(queue[next++], ctx);
        t = this.d.now();
      }
      if (t >= nextPollAt) {
        await this.pollOnce(ctx);
        nextPollAt = this.d.now() + POLL_INTERVAL_MS;
      }
      // pre-minted tokens are fixed per bot (they outlive the run, config.mjs): nothing to refresh
      if (!cfg.token.preminted && this.d.now() - this.lastRefreshMs >= cfg.token.refreshSec * 1000) await this.refreshTokens();
      const wake = Math.min(next < queue.length ? tStart + next * gapMs : Infinity, nextPollAt, tEnd);
      await this.sleep(Math.max(0, wake - this.d.now()));
    }
    const stopped = this.mustStop();
    if (stopped === 'signal' || stopped === 'cap') {
      this.event('warn', `step ${sp.step} interrupted (${stopped}); it is not recorded`);
      return null;
    }
    await this.pollOnce(ctx);
    return this.evaluateStep(ctx, stopped === 'transport' ? this.d.now() : tEnd, lateStartSec);
  }

  async launchBot({ plan, index }, ctx) {
    const id = botId(plan.k, index);
    const rec = { id, room: plan.k, index, started: false, sumDtMs: 0, final: null, status: null };
    this.bots.set(id, rec);
    ctx.newBotIds.push(id);
    try {
      const sessionToken = await this.d.mintToken(plan, index);
      await this.host.startBot(buildBotConfig({ cfg: this.cfg, plan, index, sessionToken }));
      rec.started = true;
      rec.status = { id, state: 'init', error: null, timings: {}, peers: null, transport: { pub: null, sub: null }, tot: null };
    } catch (err) {
      const message = this.scrub(err && err.message ? err.message : String(err)).slice(0, 200);
      rec.status = { id, state: 'failed', error: { phase: 'other', code: 'start_failed', message }, timings: {}, peers: null, transport: { pub: null, sub: null }, tot: null };
      this.event('warn', `bot ${id} could not be started: ${message}`);
    }
  }

  async safePoll() {
    try {
      const statuses = await this.host.pollAll();
      return Array.isArray(statuses) ? statuses : [];
    } catch (err) {
      this.event('warn', `pollAll failed: ${err && err.message}`);
      return [];
    }
  }

  /** Merge polled statuses into the bot records; returns the samples of this poll (with bot ids). */
  ingest(statuses, ctx) {
    const fresh = [];
    for (const st of statuses) {
      const rec = this.bots.get(st.id);
      if (!rec) continue;
      const { samples = [], events = [], ...status } = st;
      rec.status = status;
      for (const s of samples) {
        const sample = { id: st.id, ...s };
        rec.sumDtMs += s.dtMs || 0;
        fresh.push(sample);
        if (ctx && s.t >= ctx.tMeasureStart) ctx.samples.push(sample);
      }
      this.logBotEvents(rec, events);
    }
    return fresh;
  }

  logBotEvents(rec, events) {
    for (const e of events) {
      if (e.kind !== 'error' && e.kind !== 'warn') continue;
      if (this.botEventLines >= MAX_BOT_EVENT_LINES) return;
      this.botEventLines++;
      this.event('warn', `bot ${rec.id} ${e.kind}: ${e.msg}`);
      if (this.botEventLines === MAX_BOT_EVENT_LINES) this.event('warn', 'further bot events are not logged');
    }
  }

  async pollOnce(ctx) {
    const fresh = this.ingest(await this.safePoll(), ctx);
    const t = this.d.now();
    const statuses = [...this.bots.values()].map((r) => r.status);
    if (!this.transportFatal && statuses.some((s) => M.hasTransportViolation(s))) {
      this.transportFatal = true;
      this.event('warn', 'transport policy violation reported: stopping (fatal, not a capacity result)');
    }
    const clientPct = this.d.clientCpu.sample();
    if (clientPct !== null) ctx.cpuReadings.push({ t, pct: clientPct });
    await this.d.cpuSource.refresh();
    const janusPct = this.d.cpuSource.latest();

    const steady = statuses.filter((s) => s.state === 'steady').length;
    const up = statuses.filter((s) => s.state === 'connected' || s.state === 'steady').length;
    const failed = statuses.filter((s) => M.isTerminal(s.state)).length;
    const sums = M.sumDeltas(fresh);
    this.appendJsonl('timeseries.jsonl', {
      t,
      step: ctx.sp.step,
      bots: statuses.length,
      up,
      steady,
      d: sums,
      g: M.maxGauges(fresh),
      clientCpuPct: clientPct,
      janusCpuPct: janusPct,
    });
    const phase = t < ctx.tJoinEnd ? 'join' : t < ctx.tMeasureStart ? 'settle' : 'hold';
    const left = Math.max(0, Math.round((ctx.tEnd - t) / 1000));
    const loss = M.lossPct(sums.aIn.lost + sums.vIn.lost, sums.aIn.packets + sums.vIn.packets);
    this.d.log('progress', `[step ${ctx.sp.step}/${this.cfg.schedule.steps.length} ${phase} ${left}s left] bots=${statuses.length} steady=${steady} failed=${failed} loss=${fmt(loss, 2)}% cpu client=${fmt(clientPct, 0)}% janus=${fmt(janusPct, 0)}%`);
  }

  async refreshTokens() {
    this.lastRefreshMs = this.d.now();
    const map = {};
    for (const rec of this.bots.values()) {
      if (rec.started && !M.isTerminal(rec.status.state)) map[rec.id] = await this.d.mintToken();
    }
    const n = Object.keys(map).length;
    if (n === 0) return;
    try {
      await this.host.setTokens(map);
      this.event('info', `refreshed ${n} session token(s)`);
    } catch (err) {
      this.noteError('token_refresh_failed', err);
    }
  }

  async evaluateStep(ctx, tEnd, lateStartSec) {
    const { cfg } = this;
    const { sp, tStart, tJoinEnd, tMeasureStart } = ctx;
    await this.d.cpuSource.refresh();
    const windowSamples = M.samplesInWindow(ctx.samples, tMeasureStart, tEnd);
    const win = M.windowMetrics(windowSamples, tMeasureStart, Math.max(tEnd, tMeasureStart));
    const measureEnd = Math.max(tEnd, tMeasureStart);
    const lossSeries = M.lossWindows(windowSamples, tMeasureStart, measureEnd);
    const freezeSeries = M.freezeWindows(windowSamples, tMeasureStart, measureEnd);
    const janus = this.d.cpuSource.sample(tMeasureStart, tEnd);
    const clientCpu = M.clientCpuStats(ctx.cpuReadings.filter((r) => r.t >= tJoinEnd && r.t <= tEnd).map((r) => r.pct));

    const recs = [...this.bots.values()];
    const statuses = recs.map((r) => r.status);
    const cls = M.classifyBots(recs.map((r) => ({ state: r.status.state, allPeersMs: r.status.timings && r.status.timings.allPeersMs })), cfg.joinTimeoutSec * 1000);
    const dist = M.joinDistributions(ctx.newBotIds.map((id) => this.bots.get(id).status));
    const violations = M.transportSummary(statuses).violations;

    const checks = {
      loss: M.evalLoss(lossSeries, { limitPct: cfg.limits.lossPct, breachWindows: cfg.limits.breachWindows }),
      freeze: M.evalFreeze(freezeSeries, { tolerance: cfg.limits.freezeTolerance, breachWindows: cfg.limits.breachWindows }),
      cpu: M.evalCpu(janus ? janus.max : null, cfg.cpu.limit),
      join: M.evalJoin(cls, cfg.limits.joinFailPct),
      transport: M.evalTransport(violations),
    };
    const verdict = M.stepVerdict(checks, { clientSaturated: clientCpu.saturated });

    const sums = M.sumDeltas(windowSamples);
    this.stepAgg.push({
      lost: sums.aIn.lost + sums.vIn.lost,
      packets: sums.aIn.packets + sums.vIn.packets,
      freezeEvents: win.freezeEvents || 0,
      pps: win.audioPubPps,
      client: clientCpu,
    });

    return {
      step: sp.step,
      rooms: sp.rooms,
      participants: sp.participants,
      shardRooms: sp.shardRooms,
      shardParticipants: sp.shardParticipants,
      tStart,
      tJoinEnd,
      tMeasureStart,
      tEnd,
      lateStartSec,
      join: { attempted: cls.attempted, ok: cls.ok, failed: cls.failed, notSteady: cls.notSteady, ...dist },
      window: win,
      clientCpu,
      janusCpu: { avgPct: M.round(janus ? janus.avg : null, 1), maxPct: M.round(janus ? janus.max : null, 1), source: this.d.cpuSource.kind },
      checks,
      breached: verdict.breached,
      breachReasons: verdict.breachReasons,
      verdict: verdict.verdict,
    };
  }

  // ---- shutdown and output

  /** Leave gracefully (rate limited), close the browsers, destroy rooms, write the final files. Never throws. */
  async shutdown() {
    const guarded = async (label, fn) => {
      try {
        await fn();
      } catch (err) {
        this.noteError(label, err);
      }
    };
    await guarded('final_poll', async () => this.ingest(await this.safePoll(), null));
    await guarded('leave_failed', () => this.leaveAll());
    await guarded('stop_all_failed', async () => {
      const finals = await raceTimeout(this.host.stopAll({ graceful: false }), LEAVE_TIMEOUT_MS);
      if (Array.isArray(finals)) for (const f of finals) if (this.bots.has(f.id) && !this.bots.get(f.id).final) this.bots.get(f.id).final = f;
    });
    await guarded('host_close_failed', async () => {
      if ((await raceTimeout(this.host.close(), HOST_CLOSE_TIMEOUT_MS)) === 'timeout') throw new Error('closing the browsers timed out');
    });
    if (this.roomsTouched) {
      await guarded('rooms_destroy_failed', async () => {
        if ((await raceTimeout(this.d.rooms.destroy(this.ownRooms()), ROOMS_TIMEOUT_MS)) === 'timeout') throw new Error('room destruction timed out');
      });
    }
    this.summary = this.buildSummary();
    for (const rec of [...this.bots.values()].sort((a, b) => a.room - b.room || a.index - b.index)) {
      this.appendJsonl('bots.jsonl', this.botRow(rec));
    }
    await this.writeJson('summary.json', this.summary);
    await this.writeJson('run.json', this.runJson(this.d.now()));
    this.event('info', `finished: ok=${this.summary.ok} stopReason=${this.summary.stopReason}`);
    await this.closeStreams();
  }

  async leaveAll() {
    const gapMs = 1000 / this.cfg.leaveRate;
    const pending = [];
    for (const rec of this.bots.values()) {
      if (!rec.started) continue;
      pending.push(this.host.stopBot(rec.id, { graceful: true }).then((f) => { if (f) rec.final = f; }, () => {}));
      await this.d.sleep(gapMs);
    }
    await raceTimeout(Promise.allSettled(pending), LEAVE_TIMEOUT_MS);
  }

  botRow(rec) {
    const st = rec.status;
    const tot = (rec.final && rec.final.tot) || st.tot || null;
    return {
      id: rec.id,
      room: rec.room,
      index: rec.index,
      state: st.state,
      error: st.error ? { ...st.error, message: this.scrub(st.error.message) } : null,
      timings: st.timings || {},
      peers: st.peers || null,
      transport: st.transport || { pub: null, sub: null },
      tot,
      derived: {
        lossPct: M.totalLossPct(tot),
        audioOutPpsAvg: M.audioOutPpsAvg(tot, rec.sumDtMs),
        e2ee: { enc: tot ? tot.e2eeEnc ?? null : null, dec: tot ? tot.e2eeDec ?? null : null, fail: tot ? tot.e2eeFail ?? null : null },
      },
    };
  }

  buildSummary() {
    const { cfg } = this;
    const statuses = [...this.bots.values()].map((r) => r.status);
    const tr = M.transportSummary(statuses);
    const joinDist = M.joinDistributions(statuses);
    const lastOk = [...this.steps].reverse().find((s) => s.verdict === 'ok');
    const first = this.steps[0];
    const pageCrash = statuses.some((s) => s.error && s.error.code === 'page_crash');
    const joinProblem = Boolean(first && first.checks.join.breached);
    const emptyCap = this.stopReason === 'duration_cap' && this.steps.length === 0;
    const ok = this.stopReason !== 'fatal' && this.stopReason !== 'transport' && tr.violations === 0 && !pageCrash && !joinProblem && !emptyCap;

    const lost = this.stepAgg.reduce((a, s) => a + s.lost, 0);
    const packets = this.stepAgg.reduce((a, s) => a + s.packets, 0);
    const ppsMin = this.stepAgg.map((s) => s.pps.min).filter((v) => v !== null);
    const ppsMax = this.stepAgg.map((s) => s.pps.max).filter((v) => v !== null);
    const client = this.stepAgg.map((s) => s.client);
    const avgs = client.map((c) => c.avgPct).filter((v) => v !== null);
    const maxs = client.map((c) => c.maxPct).filter((v) => v !== null);

    const errors = [
      ...M.errorSummary(statuses).map((e) => ({ ...e, sample: e.sample === null ? null : this.scrub(e.sample) })),
      ...this.runErrors,
    ];
    return {
      schema: 1,
      runId: cfg.runId,
      scenario: cfg.scenario,
      mode: cfg.mode,
      shard: { ...cfg.shard },
      ok,
      stopReason: this.stopReason,
      maxSustainable: lastOk ? { step: lastOk.step, rooms: lastOk.rooms, participants: lastOk.participants } : null,
      steps: this.steps,
      bots: {
        attempted: statuses.length,
        steady: statuses.filter((s) => s.state === 'steady').length,
        failed: statuses.filter((s) => M.isTerminal(s.state)).length,
        transportViolations: tr.violations,
      },
      transport: { tlsVersion: tr.tlsVersion, dtlsCipher: tr.dtlsCipher, srtpCipher: tr.srtpCipher, policyOk: tr.violations === 0 && tr.seen > 0 },
      join: {
        p50Ms: joinDist.p50Ms,
        p95Ms: joinDist.p95Ms,
        maxMs: joinDist.maxMs,
        iceP50Ms: joinDist.iceP50Ms,
        iceP95Ms: joinDist.iceP95Ms,
        dtlsP50Ms: joinDist.dtlsP50Ms,
        dtlsP95Ms: joinDist.dtlsP95Ms,
      },
      media: {
        lossPct: M.round(M.lossPct(lost, packets), 3),
        freezeEvents: this.stepAgg.reduce((a, s) => a + s.freezeEvents, 0),
        audioOutPps: {
          p50: M.round(M.percentile(this.stepAgg.map((s) => s.pps.p50), 50), 2),
          min: ppsMin.length ? Math.min(...ppsMin) : null,
          max: ppsMax.length ? Math.max(...ppsMax) : null,
        },
      },
      client: {
        cpuAvgPct: avgs.length ? M.round(avgs.reduce((a, b) => a + b, 0) / avgs.length, 1) : null,
        cpuMaxPct: maxs.length ? Math.max(...maxs) : null,
        saturated: client.some((c) => c.saturated),
      },
      errors,
    };
  }
}

/**
 * Run one shard to completion and return its summary object (summary.json).
 * Always writes the output files and always stops the bots and closes the
 * host, also after an internal error or an abort.
 *
 * @param {object} cfg   from config.parseCli (mode 'run' or 'ramp')
 * @param {object} host  a BotHost (see src/browser-pool.mjs)
 * @param {object} [deps]  now, sleep(ms, signal), log(kind, line) with kind
 *   'progress'|'info'|'warn', signal (AbortSignal), mintToken(), roomPlan(seed, k, size),
 *   rooms {create(indices), destroy(indices)}, cpuSource, clientCpu
 */
export async function runShard(cfg, host, deps = {}) {
  return new ShardRun(cfg, host, resolveDeps(cfg, deps)).execute();
}
