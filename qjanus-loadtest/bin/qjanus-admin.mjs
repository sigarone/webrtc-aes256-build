#!/usr/bin/env node
// qjanus-admin: create/destroy/inspect load-test rooms through the Janus HTTP
// transport with a signed-token session (exactly what the production server does).
// Secrets come from the environment (or --*-file) and are never printed; the
// only secret-derived output is the `token` command's token.

import { parseArgs } from 'node:util';
import { readFileSync, realpathSync } from 'node:fs';
import { pathToFileURL } from 'node:url';
import { mintSessionToken, VIDEOROOM_PLUGIN } from '../src/token.mjs';
import { JanusAdmin, JanusAdminError, createRooms, destroyRooms } from '../src/janus-admin.mjs';
import { roomId, roomSecret, pseudonym, joinToken } from '../src/ids.mjs';

const USAGE = `Usage: qjanus-admin <command> [flags]

Commands
  token          print a freshly minted session token   [--ttl-sec 600] [--plugin janus.plugin.videoroom]
  info           print core info (no token needed)
  ping           ping the core (no token needed)
  create-rooms   create rooms   --rooms K --size N [--first 0] [--publishers N] [--allowed|--no-allowed] [--concurrency 8]
  destroy-rooms  destroy rooms  --rooms K [--first 0] [--concurrency 8]
  list-rooms     list all rooms (private ones included)
  participants   --room-index k
  kick           --room-index k --index i
  allowed        --room-index k --action add|remove|enable|disable [--index i ...]

Global flags
  --admin-url URL          Janus HTTP base, e.g. http://127.0.0.1:8088/janus   [env QJANUS_ADMIN_URL]
  --token-secret-env NAME  env var holding the core token secret               [QJANUS_TOKEN_SECRET]
  --admin-key-env NAME     env var holding the videoroom admin_key             [QJANUS_ADMIN_KEY]
  --seed-env NAME          env var holding the id seed                         [QJANUS_LOADTEST_SEED]
  --token-secret-file F, --admin-key-file F, --seed-file F   read a secret from a file instead
  --hash sha1|sha256       token_auth_hash of the server (default sha256)
  --json                   machine-readable output (full room ids)
  -h, --help

Rooms, secrets and join tokens are derived from the seed, so every run and every
shard sees the same ones. Output shows room ids as 8 characters unless --json.
Exit codes: 0 ok, 1 failure, 2 usage error / missing environment.
`;

const OPTIONS = {
  'admin-url': { type: 'string' },
  'token-secret-env': { type: 'string', default: 'QJANUS_TOKEN_SECRET' },
  'admin-key-env': { type: 'string', default: 'QJANUS_ADMIN_KEY' },
  'seed-env': { type: 'string', default: 'QJANUS_LOADTEST_SEED' },
  'token-secret-file': { type: 'string' },
  'admin-key-file': { type: 'string' },
  'seed-file': { type: 'string' },
  hash: { type: 'string', default: 'sha256' },
  json: { type: 'boolean', default: false },
  help: { type: 'boolean', short: 'h', default: false },
  'ttl-sec': { type: 'string' },
  plugin: { type: 'string', multiple: true },
  rooms: { type: 'string' },
  size: { type: 'string' },
  first: { type: 'string' },
  publishers: { type: 'string' },
  allowed: { type: 'boolean' }, // --allowed / --no-allowed
  concurrency: { type: 'string' },
  'room-index': { type: 'string' },
  index: { type: 'string', multiple: true },
  action: { type: 'string' },
};

class UsageError extends Error {}

// Every secret value read in this process; scrubbed from any text before it is printed.
const knownSecrets = [];
const remember = (value) => {
  knownSecrets.push(value);
  return value;
};
const scrub = (text) => knownSecrets.reduce((t, s) => (s.length >= 4 ? t.split(s).join('***') : t), String(text));

const short = (id) => String(id).slice(0, 8);
// Output sinks; runCli() swaps them so the tests can capture what would be printed.
let sink = { stdout: (text) => process.stdout.write(text), stderr: (text) => process.stderr.write(text) };
const out = (line) => sink.stdout(`${line}\n`);
const errLine = (line) => sink.stderr(`${scrub(line)}\n`);

function readSecret(o, env, kind, what) {
  const file = o[`${kind}-file`];
  if (file !== undefined) {
    let text;
    try {
      text = readFileSync(file, 'utf8').replace(/\r?\n$/, '');
    } catch {
      throw new UsageError(`cannot read the file given with --${kind}-file`);
    }
    if (!text) throw new UsageError(`the file given with --${kind}-file is empty`);
    return remember(text);
  }
  const name = o[`${kind}-env`];
  if (!env[name]) throw new UsageError(`environment variable ${name} (${what}) is not set or empty`);
  return remember(env[name]);
}

function intFlag(o, name, { min = 1, def } = {}) {
  const raw = o[name];
  if (raw === undefined) {
    if (def === undefined) throw new UsageError(`--${name} is required`);
    return def;
  }
  if (!/^\d+$/.test(raw) || Number(raw) < min) throw new UsageError(`--${name} must be an integer >= ${min}`);
  return Number(raw);
}

function makeAdmin(o, env, { secret, adminKey }) {
  const baseUrl = o['admin-url'] ?? env.QJANUS_ADMIN_URL;
  if (!baseUrl) throw new UsageError('no Janus URL: pass --admin-url or set QJANUS_ADMIN_URL');
  if (o.hash !== 'sha1' && o.hash !== 'sha256') throw new UsageError('--hash must be sha1 or sha256');
  // Read the secrets first so a missing variable is reported as such (not as a bad URL).
  const tokenSecret = secret ? readSecret(o, env, 'token-secret', 'core token secret') : undefined;
  const key = adminKey ? readSecret(o, env, 'admin-key', 'videoroom admin_key') : undefined;
  try {
    return new JanusAdmin({ baseUrl, hash: o.hash, tokenSecret, adminKey: key });
  } catch (e) {
    if (e instanceof JanusAdminError) throw e;
    throw new UsageError('the Janus URL is not a valid http(s) URL');
  }
}

const roomOf = (o, env) => {
  const seed = readSecret(o, env, 'seed', 'id seed');
  const k = intFlag(o, 'room-index', { min: 0 });
  const room = roomId(seed, k);
  return { seed, k, room, secret: roomSecret(seed, room) };
};

function report(o, human, data) {
  if (o.json) out(JSON.stringify({ ok: true, ...data }));
  else for (const line of human) out(line);
}

// ------------------------------------------------------------------- commands

const COMMANDS = {
  async token(o, env) {
    const secret = readSecret(o, env, 'token-secret', 'core token secret');
    const ttl = o['ttl-sec'] === undefined ? 600 : Number(o['ttl-sec']);
    if (!(ttl > 0) || !Number.isFinite(ttl)) throw new UsageError('--ttl-sec must be a number > 0');
    if (o.hash !== 'sha1' && o.hash !== 'sha256') throw new UsageError('--hash must be sha1 or sha256');
    const token = mintSessionToken({ secret, ttlSec: ttl, hash: o.hash, plugins: o.plugin ?? [VIDEOROOM_PLUGIN] });
    report(o, [token], { token, expiresAtUnix: Number(token.split(',')[0]) });
    return 0;
  },

  async info(o, env) {
    const info = await makeAdmin(o, env, {}).info();
    // Never echo addresses of the server host (this output ends up in public CI logs).
    const { transaction, 'local-ip': _l, 'public-ip': _p, ...safe } = info;
    const names = (x) => Object.keys(x ?? {}).join(', ') || '-';
    report(o, [
      `janus ${safe.version_string ?? '?'} (${safe.name ?? '?'})`,
      `plugins: ${names(safe.plugins)}`,
      `transports: ${names(safe.transports)}`,
      `auth_token: ${safe.auth_token ?? '?'}`,
    ], { info: safe });
    return 0;
  },

  async ping(o, env) {
    const ms = await makeAdmin(o, env, {}).ping();
    report(o, [`pong ${ms} ms`], { ms });
    return 0;
  },

  async 'create-rooms'(o, env) {
    const count = intFlag(o, 'rooms');
    const size = intFlag(o, 'size');
    const first = intFlag(o, 'first', { min: 0, def: 0 });
    const publishers = intFlag(o, 'publishers', { def: size });
    const concurrency = intFlag(o, 'concurrency', { def: 8 });
    const allowed = o.allowed ?? true;
    const seed = readSecret(o, env, 'seed', 'id seed');
    const admin = makeAdmin(o, env, { secret: true, adminKey: true });
    try {
      const rooms = await createRooms(admin, { seed, first, count, size, publishers, allowed, concurrency });
      const created = rooms.filter((r) => !r.existed).length;
      report(o, [
        ...rooms.map((r) => `k=${r.k} room=${short(r.room)} ${r.existed ? 'exists' : 'created'}`),
        `${rooms.length} rooms ready (${created} created), size ${size}, publishers ${publishers}, allowed ${allowed ? 'on' : 'off'}`,
      ], { rooms, size, publishers, allowed });
      return 0;
    } finally {
      await admin.close();
    }
  },

  async 'destroy-rooms'(o, env) {
    const count = intFlag(o, 'rooms');
    const first = intFlag(o, 'first', { min: 0, def: 0 });
    const concurrency = intFlag(o, 'concurrency', { def: 8 });
    const seed = readSecret(o, env, 'seed', 'id seed');
    const admin = makeAdmin(o, env, { secret: true });
    try {
      const rooms = await destroyRooms(admin, { seed, first, count, concurrency });
      const gone = rooms.filter((r) => r.existed).length;
      report(o, [
        ...rooms.map((r) => `k=${r.k} room=${short(r.room)} ${r.existed ? 'destroyed' : 'missing'}`),
        `${rooms.length} rooms handled (${gone} destroyed)`,
      ], { rooms });
      return 0;
    } finally {
      await admin.close();
    }
  },

  async 'list-rooms'(o, env) {
    const admin = makeAdmin(o, env, { secret: true, adminKey: true });
    try {
      const { list } = await admin.listRooms();
      report(o, [
        ...list.map((r) => `room=${short(r.room)} participants=${r.num_participants} publishers=${r.max_publishers} e2ee=${r.require_e2ee} private=${r.is_private}`),
        `${list.length} rooms`,
      ], { rooms: list });
      return 0;
    } finally {
      await admin.close();
    }
  },

  async participants(o, env) {
    const { room } = roomOf(o, env);
    const admin = makeAdmin(o, env, { secret: true });
    try {
      const { participants } = await admin.listParticipants(room);
      report(o, [
        ...participants.map((p) => `id=${short(p.id)} display=${short(p.display ?? '')} publisher=${p.publisher}`),
        `${participants.length} participants in room ${short(room)}`,
      ], { room, participants });
      return 0;
    } finally {
      await admin.close();
    }
  },

  async kick(o, env) {
    const { seed, k, room, secret } = roomOf(o, env);
    const i = intFlag(o, 'index', { min: 0 });
    const admin = makeAdmin(o, env, { secret: true });
    try {
      await admin.kick({ room, secret, id: pseudonym(seed, k, i) });
      report(o, [`kicked index ${i} from room ${short(room)}`], { room, index: i });
      return 0;
    } finally {
      await admin.close();
    }
  },

  async allowed(o, env) {
    const { seed, k, room, secret } = roomOf(o, env);
    const action = o.action;
    if (!['add', 'remove', 'enable', 'disable'].includes(action)) throw new UsageError('--action must be add, remove, enable or disable');
    const indexes = (o.index ?? []).map((v) => {
      if (!/^\d+$/.test(v)) throw new UsageError('--index must be an integer >= 0');
      return Number(v);
    });
    if ((action === 'add' || action === 'remove') && indexes.length === 0) throw new UsageError(`--action ${action} needs at least one --index`);
    const admin = makeAdmin(o, env, { secret: true });
    try {
      const data = await admin.allowed({ room, secret, action, tokens: indexes.map((i) => joinToken(seed, k, i)) });
      const count = Array.isArray(data.allowed) ? data.allowed.length : null; // the tokens themselves are never printed
      report(o, [`room ${short(room)} action=${action}${count === null ? '' : ` allowed=${count}`}`], { room, action, count });
      return 0;
    } finally {
      await admin.close();
    }
  },
};

// ----------------------------------------------------------------------- main

async function main(argv, env) {
  let values;
  let positionals;
  try {
    ({ values, positionals } = parseArgs({ args: argv, options: OPTIONS, allowPositionals: true, allowNegative: true }));
  } catch (e) {
    throw new UsageError(e.message);
  }
  if (values.help) {
    out(USAGE);
    return 0;
  }
  const [cmd, ...extra] = positionals;
  if (!cmd) throw new UsageError('no command given');
  if (!Object.hasOwn(COMMANDS, cmd)) throw new UsageError(`unknown command "${cmd}"`);
  if (extra.length > 0) throw new UsageError(`unexpected argument "${extra[0]}"`);
  return COMMANDS[cmd](values, env);
}

/**
 * Run the CLI. Resolves with the exit code (0 ok, 1 failure, 2 usage error / missing env); never throws.
 * @param {string[]} argv  arguments after the script name
 * @param {Record<string, string|undefined>} env  environment the secrets are read from
 * @param {{stdout?: (text: string) => void, stderr?: (text: string) => void}} [streams]  output capture (tests)
 */
export async function runCli(argv, env = process.env, streams = {}) {
  sink = {
    stdout: streams.stdout ?? ((text) => process.stdout.write(text)),
    stderr: streams.stderr ?? ((text) => process.stderr.write(text)),
  };
  knownSecrets.length = 0;
  try {
    return await main(argv, env);
  } catch (e) {
    if (e instanceof UsageError) {
      errLine(`qjanus-admin: ${e.message}\nRun with --help for usage.`);
      return 2;
    }
    errLine(`qjanus-admin: ${e?.message ?? e}`);
    for (const f of e?.failures ?? []) errLine(`  k=${f.k} code=${f.code} ${f.reason}`);
    if (argv.includes('--json')) {
      out(scrub(JSON.stringify({ ok: false, error: { code: e?.code ?? 'error', message: e?.message ?? String(e) }, failures: e?.failures ?? [] })));
    }
    return 1;
  }
}

// Run only when executed directly (node bin/qjanus-admin.mjs), not when the tests import runCli.
if (process.argv[1] && import.meta.url === pathToFileURL(realpathSync(process.argv[1])).href) {
  process.exitCode = await runCli(process.argv.slice(2), process.env);
}
