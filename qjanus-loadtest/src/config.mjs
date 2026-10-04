// CLI + environment parsing into one validated, immutable-by-convention config
// object, the ramp schedule, shard math and redaction.
//
// Secrets (Janus URL, token secret, seed, admin URL/key, ICE credentials) live
// only in `cfg.secrets`. `redactedConfig(cfg)` is the only representation that
// may be written to files or logs. Error messages name flags/variables, never values.

import { createHash } from 'node:crypto';
import { readFileSync } from 'node:fs';
import { parseArgs } from 'node:util';
import { SCENARIOS, SIMULCAST_MODES, VIDEO_PROFILES } from './scenarios.mjs';

export class ConfigError extends Error {
  constructor(message) {
    super(message);
    this.name = 'ConfigError';
  }
}

export const COMMANDS = ['run', 'ramp', 'report', 'plan'];

/** Defaults for every tunable (the flag reference in bin/qjanus-load.mjs mirrors these). */
export const DEFAULTS = {
  scenario: 'audio8',
  rooms: 1,
  rampStart: 1,
  rampStep: 1,
  rampMax: 20,
  holdSec: 60,
  settleSec: 10,
  joinTimeoutSec: 60,
  joinRate: 2,
  leaveRate: 10,
  botsPerBrowser: 16,
  botsPerPage: 1,
  videoProfile: 'spec',
  simulcast: 'lmh',
  e2ee: 'aesgcm',
  expectTransport: 'strict',
  substream: 1,
  speakerSubstream: 2,
  temporal: 2,
  cpuLimit: 180,
  lossLimitPct: 1,
  freezeTolerance: 0,
  breachWindows: 2,
  joinFailLimitPct: 5,
  tokenTtlSec: 600,
  tokenRefreshSec: 300,
  // ML-KEM DTLS (Chromium <= 148 needs the trial) + all simulcast layers even for small captures
  fieldTrials: 'WebRTC-EnableDtlsPqc/Enabled/WebRTC-LegacySimulcastLayerLimit/Disabled/',
  tokenSecretEnv: 'QJANUS_TOKEN_SECRET',
  sessionTokensEnv: 'QJANUS_SESSION_TOKENS',
  // pre-minted tokens must outlive the planned run by this much (seconds)
  sessionTokenMarginSec: 120,
  seedEnv: 'QJANUS_LOADTEST_SEED',
  adminKeyEnv: 'QJANUS_ADMIN_KEY',
};

const OPTIONS = {
  'ws-url': { type: 'string' },
  'token-secret-env': { type: 'string' },
  'token-secret-file': { type: 'string' },
  'session-tokens-env': { type: 'string' },
  'session-tokens-file': { type: 'string' },
  'seed-env': { type: 'string' },
  'seed-file': { type: 'string' },
  scenario: { type: 'string' },
  rooms: { type: 'string' },
  'ramp-start': { type: 'string' },
  'ramp-step': { type: 'string' },
  'ramp-max': { type: 'string' },
  'hold-sec': { type: 'string' },
  'settle-sec': { type: 'string' },
  'join-timeout-sec': { type: 'string' },
  shard: { type: 'string' },
  'start-at': { type: 'string' },
  'join-rate': { type: 'string' },
  'leave-rate': { type: 'string' },
  'bots-per-browser': { type: 'string' },
  'bots-per-page': { type: 'string' },
  'video-profile': { type: 'string' },
  simulcast: { type: 'string' },
  e2ee: { type: 'string' },
  'expect-transport': { type: 'string' },
  substream: { type: 'string' },
  'speaker-substream': { type: 'string' },
  temporal: { type: 'string' },
  'manage-rooms': { type: 'boolean' },
  'admin-url': { type: 'string' },
  'admin-key-env': { type: 'string' },
  'admin-key-file': { type: 'string' },
  'cpu-file': { type: 'string' },
  'cpu-cmd': { type: 'string' },
  'cpu-limit': { type: 'string' },
  'loss-limit-pct': { type: 'string' },
  'freeze-tolerance': { type: 'string' },
  'breach-windows': { type: 'string' },
  'join-fail-limit-pct': { type: 'string' },
  'token-ttl-sec': { type: 'string' },
  'token-refresh-sec': { type: 'string' },
  'dtls-fingerprint': { type: 'string' },
  'field-trials': { type: 'string' },
  'chromium-arg': { type: 'string', multiple: true },
  'chromium-path': { type: 'string' },
  headed: { type: 'boolean' },
  out: { type: 'string' },
  'run-id': { type: 'string' },
  'duration-cap-sec': { type: 'string' },
  'ice-servers-json': { type: 'string' },
  quiet: { type: 'boolean' },
  // report
  in: { type: 'string', multiple: true },
  sampler: { type: 'string' },
  // plan
  shards: { type: 'string' },
  help: { type: 'boolean', short: 'h' },
};

const RUN_ONLY = ['rooms'];
const RAMP_ONLY = ['ramp-start', 'ramp-step', 'ramp-max'];

// ------------------------------------------------------------------- helpers

/** Number option: default when absent, ConfigError (flag name only) when invalid. */
function numOpt(values, flag, def, { min = -Infinity, max = Infinity, int = false } = {}) {
  const raw = values[flag];
  if (raw === undefined) return def;
  const v = raw.trim() === '' ? NaN : Number(raw);
  if (!Number.isFinite(v) || (int && !Number.isInteger(v)) || v < min || v > max) {
    const range = Number.isFinite(min) ? ` >= ${min}` : '';
    throw new ConfigError(`invalid value for --${flag} (expected ${int ? 'an integer' : 'a number'}${range})`);
  }
  return v;
}

function enumOpt(values, flag, def, allowed) {
  const raw = values[flag] ?? def;
  if (!allowed.includes(raw)) throw new ConfigError(`invalid value for --${flag} (expected one of: ${allowed.join(', ')})`);
  return raw;
}

function envName(values, flag, def) {
  const name = values[flag] ?? def;
  if (!/^[A-Za-z_][A-Za-z0-9_]*$/.test(name)) {
    throw new ConfigError(`--${flag} takes the NAME of an environment variable, not its value`);
  }
  return name;
}

/** Secret from --<what>-file, else from the environment variable named by --<what>-env. */
function readSecret(values, env, what, defaultEnv) {
  const file = values[`${what}-file`];
  if (file) {
    try {
      const v = readFileSync(file, 'utf8').trim();
      if (v) return v;
    } catch {
      // fall through to the uniform message below
    }
    throw new ConfigError(`cannot read a value from --${what}-file`);
  }
  const name = envName(values, `${what}-env`, defaultEnv);
  const v = env[name];
  if (!v) throw new ConfigError(`missing environment variable ${name}`);
  return v;
}

/** One Janus core session token as token.mjs mints it: "<expiry>,janus[,<plugin>...]:<base64>". */
const SESSION_TOKEN_RE = /^(\d{1,12}),janus(?:,[^\s,:]+)*:[A-Za-z0-9+/]+={0,2}$/;

/**
 * Pre-minted session tokens ("pre-minted" mode): a JSON array of strings, one per bot, minted
 * elsewhere (on the node itself) so the core token secret never reaches this process. Read from
 * --session-tokens-file, else from the environment variable named by --session-tokens-env.
 * Returns null when neither is given (HMAC mode with the token secret). Errors name the flag only.
 */
function readSessionTokens(values, env) {
  let raw;
  const file = values['session-tokens-file'];
  if (file) {
    try {
      raw = readFileSync(file, 'utf8');
    } catch {
      throw new ConfigError('cannot read a value from --session-tokens-file');
    }
  } else {
    raw = env[envName(values, 'session-tokens-env', DEFAULTS.sessionTokensEnv)];
  }
  if (raw === undefined || raw.trim() === '') return null;
  const bad = new ConfigError('invalid session tokens (expected a JSON array of Janus session token strings)');
  let list;
  try {
    list = JSON.parse(raw);
  } catch {
    throw bad;
  }
  if (!Array.isArray(list) || list.length === 0 || !list.every((t) => typeof t === 'string' && SESSION_TOKEN_RE.test(t))) throw bad;
  return list;
}

/** Expiry (unix seconds) of a session token, as Janus reads it (the first field). */
export function sessionTokenExpiry(token) {
  return Number.parseInt(String(token).split(',', 1)[0], 10) || 0;
}

function readUrl(values, env, flag, envVar, protocols) {
  const raw = values[flag] ?? env[envVar];
  if (!raw) throw new ConfigError(`missing --${flag} (or environment variable ${envVar})`);
  let u;
  try {
    u = new URL(raw);
  } catch {
    throw new ConfigError(`invalid --${flag} (expected a ${protocols.join('/')} URL)`);
  }
  if (!protocols.includes(u.protocol)) throw new ConfigError(`invalid --${flag} (expected a ${protocols.join('/')} URL)`);
  return raw;
}

function readIceServers(values, env) {
  const raw = values['ice-servers-json'] ?? env.QJANUS_ICE_SERVERS;
  if (!raw) return [];
  const bad = new ConfigError('invalid --ice-servers-json / QJANUS_ICE_SERVERS (expected a JSON array of RTCIceServer objects)');
  let v;
  try {
    v = JSON.parse(raw);
  } catch {
    throw bad;
  }
  const okUrls = (u) => typeof u === 'string' || (Array.isArray(u) && u.every((x) => typeof x === 'string'));
  if (!Array.isArray(v) || !v.every((s) => s && typeof s === 'object' && okUrls(s.urls))) throw bad;
  return v;
}

function readFingerprint(values, env) {
  const raw = values['dtls-fingerprint'] ?? env.QJANUS_DTLS_FINGERPRINT;
  if (!raw) return null;
  const m = /^(?:sha-256\s+)?((?:[0-9A-Fa-f]{2}:){31}[0-9A-Fa-f]{2})$/.exec(raw.trim());
  if (!m) throw new ConfigError('invalid --dtls-fingerprint (expected "sha-256 AB:CD:..." with 32 bytes)');
  return `sha-256 ${m[1].toUpperCase()}`;
}

/** "i/S" -> { index, count }. */
export function parseShard(raw = '0/1') {
  const m = /^(\d+)\/(\d+)$/.exec(raw);
  if (!m) throw new ConfigError('invalid --shard (expected i/S, for example 0/4)');
  const index = Number(m[1]);
  const count = Number(m[2]);
  if (count < 1 || index >= count) throw new ConfigError('invalid --shard (need 0 <= i < S)');
  return { index, count };
}

/** Room k belongs to shard i iff k % S == i. */
export function ownsRoom(shard, k) {
  return k % shard.count === shard.index;
}

/** Number of rooms among indices [0, K) that belong to `shard`. */
export function shardRoomCount(shard, K) {
  return Math.max(0, Math.ceil((K - shard.index) / shard.count));
}

/** Short stable id of the target: sha256(url) first 12 hex. Never the URL itself. */
export function targetId(wsUrl) {
  return createHash('sha256').update(wsUrl).digest('hex').slice(0, 12);
}

// ------------------------------------------------------------------ schedule

/**
 * Ramp schedule shared by every shard. `ks` = total rooms after each step.
 * Step j (1-based `step`) adds rooms [ks[j-1], ks[j]); a shard starts the ones
 * with k % S == i. The step length is deterministic and identical on all
 * shards: the join window is sized for the busiest shard, then settle, then hold.
 * offsetSec is the planned start of the step relative to the schedule start.
 */
export function buildSchedule({ ks, roomSize, shard, joinRate, settleSec, holdSec }) {
  const steps = [];
  let offsetSec = 0;
  let prev = 0;
  ks.forEach((K, j) => {
    let busiest = 0;
    for (let s = 0; s < shard.count; s++) {
      const of = { index: s, count: shard.count };
      busiest = Math.max(busiest, shardRoomCount(of, K) - shardRoomCount(of, prev));
    }
    const joinWindowSec = Math.ceil((busiest * roomSize) / joinRate);
    const stepSec = joinWindowSec + settleSec + holdSec;
    const newRooms = [];
    for (let k = prev; k < K; k++) if (ownsRoom(shard, k)) newRooms.push(k);
    const shardRooms = shardRoomCount(shard, K);
    steps.push({
      step: j + 1,
      rooms: K,
      participants: K * roomSize,
      newRooms,
      newBots: newRooms.length * roomSize,
      shardRooms,
      shardParticipants: shardRooms * roomSize,
      joinWindowSec,
      stepSec,
      offsetSec,
    });
    offsetSec += stepSec;
    prev = K;
  });
  return { steps, totalSec: offsetSec };
}

// ---------------------------------------------------------------- parse CLI

function defaultRunId(scenario, now) {
  const stamp = now().toISOString().replace(/[-:]/g, '').replace(/\.\d+Z$/, 'Z');
  return `${stamp}-${scenario}`;
}

function rejectFlags(values, flags, command, context) {
  for (const f of flags) {
    if (values[f] !== undefined) throw new ConfigError(`--${f} is not valid for ${command} (${context})`);
  }
}

function buildLoadConfig(command, values, env, now) {
  const scenario = enumOpt(values, 'scenario', DEFAULTS.scenario, Object.keys(SCENARIOS));
  const roomSize = SCENARIOS[scenario].roomSize;

  // Pre-minted mode (tokens given): the core token secret is neither needed nor read.
  const sessionTokens = readSessionTokens(values, env);
  const secrets = {
    wsUrl: readUrl(values, env, 'ws-url', 'QJANUS_WS_URL', ['ws:', 'wss:']),
    tokenSecret: sessionTokens ? null : readSecret(values, env, 'token-secret', DEFAULTS.tokenSecretEnv),
    sessionTokens,
    seed: readSecret(values, env, 'seed', DEFAULTS.seedEnv),
    adminUrl: null,
    adminKey: null,
    iceServers: readIceServers(values, env),
    dtlsFingerprint: readFingerprint(values, env),
  };
  const manageRooms = values['manage-rooms'] === true;
  if (manageRooms && sessionTokens) {
    throw new ConfigError('--manage-rooms needs the token secret and cannot be combined with pre-minted session tokens');
  }
  if (manageRooms) {
    secrets.adminUrl = readUrl(values, env, 'admin-url', 'QJANUS_ADMIN_URL', ['http:', 'https:']);
    secrets.adminKey = readSecret(values, env, 'admin-key', DEFAULTS.adminKeyEnv);
  }

  const shard = parseShard(values.shard);
  const joinRate = numOpt(values, 'join-rate', DEFAULTS.joinRate, { min: 0.01 });
  const holdSec = numOpt(values, 'hold-sec', DEFAULTS.holdSec, { min: 1 });
  const settleSec = numOpt(values, 'settle-sec', DEFAULTS.settleSec, { min: 0 });

  let ks;
  if (command === 'run') {
    rejectFlags(values, RAMP_ONLY, 'run', 'use ramp');
    ks = [numOpt(values, 'rooms', DEFAULTS.rooms, { min: 1, int: true })];
  } else {
    rejectFlags(values, RUN_ONLY, 'ramp', 'use run, or --ramp-start/--ramp-step/--ramp-max');
    const start = numOpt(values, 'ramp-start', DEFAULTS.rampStart, { min: 1, int: true });
    const step = numOpt(values, 'ramp-step', DEFAULTS.rampStep, { min: 1, int: true });
    const max = numOpt(values, 'ramp-max', DEFAULTS.rampMax, { min: 1, int: true });
    if (max < start) throw new ConfigError('--ramp-max must be >= --ramp-start');
    ks = [];
    for (let k = start; k <= max; k += step) ks.push(k);
    if (ks.length > 1000) throw new ConfigError('the ramp has more than 1000 steps; raise --ramp-step');
  }

  const tokenTtlSec = numOpt(values, 'token-ttl-sec', DEFAULTS.tokenTtlSec, { min: 10, int: true });
  const tokenRefreshSec = numOpt(values, 'token-refresh-sec', DEFAULTS.tokenRefreshSec, { min: 1, int: true });
  if (tokenRefreshSec >= tokenTtlSec) throw new ConfigError('--token-refresh-sec must be smaller than --token-ttl-sec');

  const botsPerBrowser = numOpt(values, 'bots-per-browser', DEFAULTS.botsPerBrowser, { min: 1, int: true });
  const botsPerPage = numOpt(values, 'bots-per-page', DEFAULTS.botsPerPage, { min: 1, int: true });
  if (botsPerPage > botsPerBrowser) throw new ConfigError('--bots-per-page must not exceed --bots-per-browser');

  const startAtRaw = numOpt(values, 'start-at', null, { min: 0 });
  const runId = values['run-id'] ?? defaultRunId(scenario, now);
  if (!/^[A-Za-z0-9._-]{1,80}$/.test(runId)) throw new ConfigError('invalid --run-id (letters, digits, . _ - only, max 80)');
  const out = values.out ?? (shard.count > 1 ? `out/${runId}/shard-${shard.index}` : `out/${runId}`);

  const cfg = {
    command,
    mode: command,
    scenario,
    roomSize,
    holdSec,
    settleSec,
    joinTimeoutSec: numOpt(values, 'join-timeout-sec', DEFAULTS.joinTimeoutSec, { min: 1 }),
    joinRate,
    leaveRate: numOpt(values, 'leave-rate', DEFAULTS.leaveRate, { min: 0.1 }),
    shard,
    startAt: startAtRaw,
    botsPerBrowser,
    botsPerPage,
    video: {
      profile: enumOpt(values, 'video-profile', DEFAULTS.videoProfile, Object.keys(VIDEO_PROFILES)),
      simulcast: enumOpt(values, 'simulcast', DEFAULTS.simulcast, Object.keys(SIMULCAST_MODES)),
    },
    e2ee: enumOpt(values, 'e2ee', DEFAULTS.e2ee, ['aesgcm', 'xor', 'none']),
    expectTransport: enumOpt(values, 'expect-transport', DEFAULTS.expectTransport, ['strict', 'off']),
    substream: numOpt(values, 'substream', DEFAULTS.substream, { min: 0, max: 2, int: true }),
    speakerSubstream: numOpt(values, 'speaker-substream', DEFAULTS.speakerSubstream, { min: 0, max: 2, int: true }),
    temporal: numOpt(values, 'temporal', DEFAULTS.temporal, { min: 0, max: 2, int: true }),
    manageRooms,
    cpu: { file: values['cpu-file'] ?? null, cmd: values['cpu-cmd'] ?? null, limit: numOpt(values, 'cpu-limit', DEFAULTS.cpuLimit, { min: 1 }) },
    limits: {
      lossPct: numOpt(values, 'loss-limit-pct', DEFAULTS.lossLimitPct, { min: 0 }),
      freezeTolerance: numOpt(values, 'freeze-tolerance', DEFAULTS.freezeTolerance, { min: 0, int: true }),
      breachWindows: numOpt(values, 'breach-windows', DEFAULTS.breachWindows, { min: 1, int: true }),
      joinFailPct: numOpt(values, 'join-fail-limit-pct', DEFAULTS.joinFailLimitPct, { min: 0 }),
    },
    token: { ttlSec: tokenTtlSec, refreshSec: tokenRefreshSec, preminted: false },
    fieldTrials: values['field-trials'] ?? DEFAULTS.fieldTrials,
    chromiumArgs: values['chromium-arg'] ?? [],
    chromiumPath: values['chromium-path'] ?? null,
    headed: values.headed === true,
    out,
    runId,
    durationCapSec: numOpt(values, 'duration-cap-sec', null, { min: 1 }),
    quiet: values.quiet === true,
    secrets,
  };
  if (cfg.cpu.file && cfg.cpu.cmd) throw new ConfigError('use either --cpu-file or --cpu-cmd, not both');
  cfg.schedule = buildSchedule({ ks, roomSize, shard, joinRate, settleSec, holdSec });
  if (sessionTokens) {
    // bot (room k, member i) uses sessionTokens[k * roomSize + i]: all rooms the ramp can reach need one
    const needed = ks[ks.length - 1] * roomSize;
    if (sessionTokens.length < needed) {
      throw new ConfigError(`not enough pre-minted session tokens: ${needed} needed for ${ks[ks.length - 1]} room(s) of ${roomSize}, ${sessionTokens.length} given`);
    }
    // A token is checked on EVERY request, so it must outlive the whole run (no refresh in this mode).
    const nowMs = now().getTime();
    const endMs = Math.max(nowMs, startAtRaw === null ? 0 : startAtRaw * 1000) + cfg.schedule.totalSec * 1000;
    const minExpiry = Math.min(...sessionTokens.slice(0, needed).map(sessionTokenExpiry));
    if (minExpiry * 1000 < endMs + DEFAULTS.sessionTokenMarginSec * 1000) {
      throw new ConfigError('pre-minted session tokens expire before the planned end of the run (mint them with a longer lifetime or shorten the run)');
    }
    cfg.token.preminted = true;
    cfg.token.count = sessionTokens.length;
    cfg.token.minExpiresInSec = Math.floor(minExpiry - nowMs / 1000);
  }
  return cfg;
}

/**
 * Parse argv (without node and script) and the environment.
 * Returns { command: 'help' } | { command: 'plan', shards } |
 * { command: 'report', report } | { command: 'run'|'ramp', cfg }.
 * Throws ConfigError on any usage problem (exit code 2).
 */
export function parseCli(argv, env = process.env, { now = () => new Date() } = {}) {
  const [first, ...rest] = argv;
  if (!first || first === '--help' || first === '-h' || first === 'help') return { command: 'help' };
  if (!COMMANDS.includes(first)) throw new ConfigError('unknown command (expected run, ramp, report or plan)');
  let values;
  try {
    ({ values } = parseArgs({ args: rest, options: OPTIONS, allowPositionals: false, strict: true }));
  } catch (err) {
    if (err.code === 'ERR_PARSE_ARGS_UNEXPECTED_POSITIONAL') throw new ConfigError('unexpected positional argument');
    throw new ConfigError(String(err.message).split('\n')[0].slice(0, 120));
  }
  if (values.help) return { command: 'help' };

  if (first === 'plan') {
    const shards = values.shards !== undefined ? numOpt(values, 'shards', 1, { min: 1, int: true }) : parseShard(values.shard).count;
    return { command: 'plan', shards };
  }
  if (first === 'report') {
    if (!values.in || values.in.length === 0) throw new ConfigError('report needs at least one --in DIR');
    if (!values.out) throw new ConfigError('report needs --out DIR');
    return {
      command: 'report',
      report: {
        inDirs: values.in,
        sampler: values.sampler ?? null,
        out: values.out,
        cpuLimit: numOpt(values, 'cpu-limit', DEFAULTS.cpuLimit, { min: 1 }),
      },
    };
  }
  return { command: first, cfg: buildLoadConfig(first, values, env, now) };
}

// ---------------------------------------------------------------- redaction

/**
 * The only form of the config that may reach a file or a log: no Janus URL,
 * secrets, seed, admin URL/key, ICE credentials, filesystem paths or command
 * lines. `targetId` identifies the target without revealing it.
 */
export function redactedConfig(cfg) {
  return {
    command: cfg.command,
    scenario: cfg.scenario,
    roomSize: cfg.roomSize,
    schedule: {
      steps: cfg.schedule.steps.length,
      rooms: cfg.schedule.steps.map((s) => s.rooms),
      totalSec: cfg.schedule.totalSec,
    },
    holdSec: cfg.holdSec,
    settleSec: cfg.settleSec,
    joinTimeoutSec: cfg.joinTimeoutSec,
    joinRate: cfg.joinRate,
    leaveRate: cfg.leaveRate,
    shard: { ...cfg.shard },
    startAt: cfg.startAt,
    botsPerBrowser: cfg.botsPerBrowser,
    botsPerPage: cfg.botsPerPage,
    video: SCENARIOS[cfg.scenario].video ? { ...cfg.video } : null,
    e2ee: cfg.e2ee,
    expectTransport: cfg.expectTransport,
    substream: cfg.substream,
    speakerSubstream: cfg.speakerSubstream,
    temporal: cfg.temporal,
    manageRooms: cfg.manageRooms,
    cpu: { source: cfg.cpu.file ? 'file' : cfg.cpu.cmd ? 'cmd' : 'none', limit: cfg.cpu.limit },
    limits: { ...cfg.limits },
    token: { ...cfg.token },
    dtlsPin: cfg.secrets.dtlsFingerprint !== null,
    iceServers: cfg.secrets.iceServers.length,
    fieldTrials: cfg.fieldTrials,
    // Only the flag names: values may carry addresses (proxy, host-resolver-rules).
    chromiumArgs: cfg.chromiumArgs.map((a) => String(a).split('=')[0]),
    customChromium: cfg.chromiumPath !== null,
    headed: cfg.headed,
    runId: cfg.runId,
    durationCapSec: cfg.durationCapSec,
    targetId: targetId(cfg.secrets.wsUrl),
  };
}

/**
 * Remove secrets, URLs, IP addresses and Janus tokens from free text (error
 * messages that go to events.log / summary.json).
 */
export function scrubSecrets(text, cfg) {
  let out = String(text ?? '');
  const s = (cfg && cfg.secrets) || {};
  const known = [s.tokenSecret, s.seed, s.adminKey, s.wsUrl, s.adminUrl, ...(s.sessionTokens || [])];
  for (const ice of s.iceServers || []) known.push(ice.username, ice.credential);
  for (const v of known) {
    if (typeof v === 'string' && v.length >= 6) out = out.split(v).join('<redacted>');
  }
  return out
    .replace(/\b[a-z][a-z0-9+.-]*:\/\/[^\s"')]+/gi, '<url>')
    .replace(/\d{9,},janus[^\s"']*/g, '<token>')
    .replace(/\b\d{1,3}(?:\.\d{1,3}){3}(?::\d+)?\b/g, '<ip>');
}
