#!/usr/bin/env node
// Negative / access-control checks against a RUNNING smoke Janus (scripts/smoke.sh
// starts it): everything a caller without the right credentials must NOT be able
// to do, plus the positive counterpart of each check so a broken setup is not
// mistaken for a working defence. Every check is reported as PASS/FAIL, all checks
// run even after a failure, the exit code is 1 if any check failed.
//
// Environment (secrets never appear on a command line or in the output):
//   QJANUS_WS_URL           Janus WebSocket URL, e.g. ws://127.0.0.1:8188/janus
//   QJANUS_ADMIN_URL        Janus HTTP base,     e.g. http://127.0.0.1:8088/janus
//   QJANUS_TOKEN_SECRET     core token_auth_secret
//   QJANUS_ADMIN_KEY        videoroom admin_key
//   QJANUS_LOADTEST_SEED    seed for the deterministic room / participant ids
// Flags: --out FILE    also write the results as JSON (no secrets in it)
//        --room-index N  room index used for the test rooms (default 9001, 9002)
//
// The protocol is spoken directly (Node's WebSocket and fetch) on purpose: the
// point is to observe what Janus itself enforces. Only token minting, id
// derivation and the room-admin helper are shared with the harness.

import { randomBytes } from 'node:crypto';
import { writeFileSync } from 'node:fs';
import net from 'node:net';
import { parseArgs } from 'node:util';
import { JanusAdmin, JanusAdminError, JANUS_ERROR, VIDEOROOM_ERROR } from '../src/janus-admin.mjs';
import { roomPlan } from '../src/ids.mjs';
import { mintSessionToken, VIDEOROOM_PLUGIN } from '../src/token.mjs';

const { values: flags } = parseArgs({
  options: { out: { type: 'string' }, 'room-index': { type: 'string' } },
  allowPositionals: false,
});

const need = (name) => {
  const v = process.env[name];
  if (!v) {
    console.error(`smoke-negative: environment variable ${name} is not set`);
    process.exit(2);
  }
  return v;
};
const WS_URL = need('QJANUS_WS_URL');
const ADMIN_URL = need('QJANUS_ADMIN_URL');
const SECRET = need('QJANUS_TOKEN_SECRET');
const ADMIN_KEY = need('QJANUS_ADMIN_KEY');
const SEED = need('QJANUS_LOADTEST_SEED');
const K = Number(flags['room-index'] ?? 9001);

// ----------------------------------------------------------- check reporting

const results = [];
async function check(name, fn) {
  try {
    await fn();
    results.push({ name, ok: true });
    console.log(`PASS ${name}`);
  } catch (err) {
    const detail = String(err && err.message ? err.message : err).slice(0, 300);
    results.push({ name, ok: false, detail });
    console.log(`FAIL ${name}: ${detail}`);
  }
}
function expect(cond, message) {
  if (!cond) throw new Error(message);
}
function expectEqual(actual, wanted, what) {
  if (actual !== wanted) throw new Error(`${what}: got ${JSON.stringify(actual)}, wanted ${JSON.stringify(wanted)}`);
}
const sleep = (ms) => new Promise((resolve) => setTimeout(resolve, ms));

/** Error code of a Janus reply: core error, or the plugin's error_code; null when it succeeded. */
function errorCode(reply) {
  if (reply.janus === 'error') return reply.error?.code ?? 'error';
  const data = reply.plugindata?.data;
  return data && data.error_code !== undefined ? data.error_code : null;
}
async function expectRejected(promise, codes, what) {
  let reply;
  try {
    reply = await promise;
  } catch (err) {
    if (err instanceof JanusAdminError && codes.includes(err.code)) return;
    throw new Error(`${what}: failed with ${err instanceof JanusAdminError ? `code ${err.code}` : String(err.message)}, wanted code ${codes.join('/')}`);
  }
  const code = reply && typeof reply === 'object' && 'janus' in reply ? errorCode(reply) : null;
  if (code === null || !codes.includes(code)) throw new Error(`${what}: was not rejected with code ${codes.join('/')} (got ${code ?? 'success'})`);
}

// ------------------------------------------------------- minimal Janus client

const token = (o = {}) => mintSessionToken({ secret: SECRET, ttlSec: 120, ...o });

/** WebSocket session speaker: request/reply by transaction, other messages collected in `inbox`. */
class JanusWs {
  constructor() {
    this.pending = new Map();
    this.inbox = [];
    this.sessionId = null;
    this.handleId = null;
  }

  open(timeoutMs = 8000) {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(WS_URL, 'janus-protocol');
      this.ws = ws;
      const timer = setTimeout(() => reject(new Error('WebSocket did not open')), timeoutMs);
      ws.addEventListener('open', () => {
        clearTimeout(timer);
        resolve(this);
      });
      ws.addEventListener('error', () => {
        clearTimeout(timer);
        reject(new Error('WebSocket error'));
      });
      ws.addEventListener('message', (ev) => this.onMessage(String(ev.data)));
    });
  }

  onMessage(text) {
    let msg;
    try {
      msg = JSON.parse(text);
    } catch {
      return;
    }
    const p = msg.transaction ? this.pending.get(msg.transaction) : undefined;
    if (p && !(msg.janus === 'ack' && p.async)) {
      clearTimeout(p.timer);
      this.pending.delete(msg.transaction);
      p.resolve(msg);
      return;
    }
    if (!p) this.inbox.push(msg);
  }

  /** Send one request; resolves with the first non-ack reply (async: the event that follows the ack). */
  request(msg, { async = false, timeoutMs = 8000 } = {}) {
    const transaction = randomBytes(8).toString('hex');
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => {
        this.pending.delete(transaction);
        reject(new Error(`no reply to "${msg.janus}" within ${timeoutMs} ms`));
      }, timeoutMs);
      this.pending.set(transaction, { resolve, timer, async });
      this.ws.send(JSON.stringify({ ...msg, transaction }));
    });
  }

  /** Wait for a message without transaction (e.g. the "kicked" event) matching pred. */
  async waitFor(pred, timeoutMs = 6000) {
    const end = Date.now() + timeoutMs;
    for (;;) {
      const i = this.inbox.findIndex(pred);
      if (i >= 0) return this.inbox.splice(i, 1)[0];
      if (Date.now() > end) throw new Error('expected event did not arrive');
      await sleep(50);
    }
  }

  create(tok = token()) {
    return this.request({ janus: 'create', ...(tok === null ? {} : { token: tok }) });
  }

  async openSession() {
    const r = await this.create();
    expectEqual(r.janus, 'success', 'create session');
    this.sessionId = r.data.id;
    return this;
  }

  async attach() {
    const r = await this.request({ janus: 'attach', session_id: this.sessionId, plugin: VIDEOROOM_PLUGIN, token: token() });
    expectEqual(r.janus, 'success', 'attach videoroom');
    this.handleId = r.data.id;
    return this;
  }

  /** Plugin request on the attached handle; async for join/publish style requests. */
  message(body, { async = false } = {}) {
    return this.request(
      { janus: 'message', session_id: this.sessionId, handle_id: this.handleId, body, token: token() },
      { async },
    );
  }

  joinPublisher(room, id, joinToken) {
    return this.message({ request: 'join', ptype: 'publisher', room, id, display: id, ...(joinToken === undefined ? {} : { token: joinToken }) }, { async: true });
  }

  async close() {
    try {
      if (this.sessionId !== null) await this.request({ janus: 'destroy', session_id: this.sessionId, token: token() }, { timeoutMs: 3000 });
    } catch {
      // best effort
    }
    try {
      this.ws.close();
    } catch {
      // already closed
    }
  }
}

const open = [];                                 // clients to close at the end
async function session({ attach = true } = {}) {
  const c = await new JanusWs().open();
  open.push(c);
  await c.openSession();
  if (attach) await c.attach();
  return c;
}
const tcpClosed = (port) => new Promise((resolve) => {
  const s = net.connect({ host: '127.0.0.1', port });
  s.once('connect', () => { s.destroy(); resolve(false); });
  s.once('error', () => resolve(true));
});

// ---------------------------------------------------------------------- checks

const main = roomPlan(SEED, K, 3);               // room with an allowed list: members 0 and 1 may join
const other = roomPlan(SEED, K + 1, 1);          // room that must never get created
const [mA, mB, mC] = main.members;
const admin = new JanusAdmin({ baseUrl: ADMIN_URL, tokenSecret: SECRET, adminKey: ADMIN_KEY });
const adminNoKey = new JanusAdmin({ baseUrl: ADMIN_URL, tokenSecret: SECRET });
const adminBadKey = new JanusAdmin({ baseUrl: ADMIN_URL, tokenSecret: SECRET, adminKey: `${randomBytes(16).toString('hex')}` });

console.log('== session token (core token_auth, WebSocket transport)');
await check('info needs no token', async () => {
  const c = await new JanusWs().open();
  open.push(c);
  const r = await c.request({ janus: 'info' });
  expectEqual(r.janus, 'server_info', 'info reply');
  expect(typeof r.version_string === 'string', 'no version_string in info');
});
await check('create session without a token is rejected (403)', async () => {
  const c = await new JanusWs().open();
  open.push(c);
  expectEqual(errorCode(await c.create(null)), JANUS_ERROR.UNAUTHORIZED, 'error code');
});
await check('create session with an expired token is rejected (403)', async () => {
  const c = await new JanusWs().open();
  open.push(c);
  const expired = token({ ttlSec: 1, nowMs: Date.now() - 60_000 });
  expectEqual(errorCode(await c.create(expired)), JANUS_ERROR.UNAUTHORIZED, 'error code');
});
await check('create session with a token signed by another secret is rejected (403)', async () => {
  const c = await new JanusWs().open();
  open.push(c);
  const forged = mintSessionToken({ secret: `${randomBytes(16).toString('hex')}`, ttlSec: 120 });
  expectEqual(errorCode(await c.create(forged)), JANUS_ERROR.UNAUTHORIZED, 'error code');
});
await check('create session with a malformed token is rejected (403)', async () => {
  const c = await new JanusWs().open();
  open.push(c);
  expectEqual(errorCode(await c.create('not-a-token')), JANUS_ERROR.UNAUTHORIZED, 'error code');
});
await check('create session with a valid token works, then attach and keepalive', async () => {
  const c = await session();
  const r = await c.request({ janus: 'keepalive', session_id: c.sessionId, token: token() });
  expectEqual(r.janus, 'ack', 'keepalive reply');
});
await check('attach without a token on a live session is rejected (403)', async () => {
  const c = await session({ attach: false });
  const r = await c.request({ janus: 'attach', session_id: c.sessionId, plugin: VIDEOROOM_PLUGIN });
  expectEqual(errorCode(r), JANUS_ERROR.UNAUTHORIZED, 'error code');
});
await check('a token restricted to another plugin cannot attach to videoroom', async () => {
  const c = await new JanusWs().open();
  open.push(c);
  const echoOnly = token({ plugins: ['janus.plugin.echotest'] });
  const created = await c.create(echoOnly);
  expectEqual(created.janus, 'success', 'create with the restricted token');
  const r = await c.request({ janus: 'attach', session_id: created.data.id, plugin: VIDEOROOM_PLUGIN, token: echoOnly });
  const code = errorCode(r);
  expect(code === JANUS_ERROR.UNAUTHORIZED || code === JANUS_ERROR.UNAUTHORIZED_PLUGIN, `attach was not rejected (code ${code})`);
});
await check('a token that expires mid-session stops working (short TTL)', async () => {
  const c = await new JanusWs().open();
  open.push(c);
  const short = token({ ttlSec: 3 });
  const created = await c.create(short);
  expectEqual(created.janus, 'success', 'create with a 3 s token');
  const sid = created.data.id;
  expectEqual((await c.request({ janus: 'keepalive', session_id: sid, token: short })).janus, 'ack', 'keepalive while valid');
  await sleep(5000);
  expectEqual(errorCode(await c.request({ janus: 'keepalive', session_id: sid, token: short })), JANUS_ERROR.UNAUTHORIZED, 'keepalive after expiry');
  await c.request({ janus: 'destroy', session_id: sid, token: token() });
});

console.log('== HTTP transport');
await check('HTTP: create session without a token is rejected (403)', async () => {
  const res = await fetch(ADMIN_URL, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ janus: 'create', transaction: randomBytes(6).toString('hex') }),
  });
  expectEqual(errorCode(await res.json()), JANUS_ERROR.UNAUTHORIZED, 'error code');
});
await check('HTTP: create session with a valid token works', async () => {
  const res = await fetch(ADMIN_URL, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ janus: 'create', token: token(), transaction: randomBytes(6).toString('hex') }),
  });
  const body = await res.json();
  expectEqual(body.janus, 'success', 'create reply');
  await fetch(`${ADMIN_URL}/${body.data.id}`, {
    method: 'POST',
    headers: { 'content-type': 'application/json' },
    body: JSON.stringify({ janus: 'destroy', token: token(), transaction: randomBytes(6).toString('hex') }),
  });
});
await check('Admin API is not exposed (HTTP 7088 and WebSocket 7188 closed)', async () => {
  expect(await tcpClosed(7088), 'port 7088 accepts connections');
  expect(await tcpClosed(7188), 'port 7188 accepts connections');
});

console.log('== VideoRoom admin_key and room secret');
await check('room create without admin_key is rejected, no room appears', async () => {
  await expectRejected(adminNoKey.createRoom({ room: other.room, secret: other.secret, publishers: 3 }),
    [VIDEOROOM_ERROR.MISSING_ELEMENT, VIDEOROOM_ERROR.UNAUTHORIZED], 'create without admin_key');
  expectEqual(await admin.roomExists(other.room), false, 'room exists after the rejected create');
});
await check('room create with a wrong admin_key is rejected (433), no room appears', async () => {
  await expectRejected(adminBadKey.createRoom({ room: other.room, secret: other.secret, publishers: 3 }),
    [VIDEOROOM_ERROR.UNAUTHORIZED], 'create with a wrong admin_key');
  expectEqual(await admin.roomExists(other.room), false, 'room exists after the rejected create');
});
await check('room create with the right admin_key and an allowed list works', async () => {
  await admin.destroyRoom({ room: main.room, secret: main.secret, ignoreMissing: true });
  const r = await admin.createRoom({ room: main.room, secret: main.secret, publishers: 3, allowed: [mA.joinToken, mB.joinToken] });
  expectEqual(r.videoroom, 'created', 'create reply');
  expectEqual(await admin.roomExists(main.room), true, 'room exists');
});
await check('the private room is hidden from `list` without the admin_key', async () => {
  const visible = (await adminNoKey.listRooms()).list ?? [];
  expect(!visible.some((r) => r.room === main.room), 'private room listed without the admin_key');
  const all = (await admin.listRooms()).list ?? [];
  expect(all.some((r) => r.room === main.room), 'private room not listed with the admin_key');
});

console.log('== join tokens (allowed list)');
const pubA = await session().catch(() => null);
await check('publisher join with a wrong join token is rejected (433)', async () => {
  const c = await session();
  expectEqual(errorCode(await c.joinPublisher(main.room, mC.pseudonym, mC.joinToken)), VIDEOROOM_ERROR.UNAUTHORIZED, 'error code');
});
await check('publisher join without a join token is rejected (433)', async () => {
  const c = await session();
  expectEqual(errorCode(await c.joinPublisher(main.room, mC.pseudonym)), VIDEOROOM_ERROR.UNAUTHORIZED, 'error code');
});
await check('publisher join with the right join token works', async () => {
  expect(pubA !== null, 'no session');
  const r = await pubA.joinPublisher(main.room, mA.pseudonym, mA.joinToken);
  const data = r.plugindata?.data;
  expectEqual(data?.videoroom, 'joined', 'join event');
  expectEqual(data?.id, mA.pseudonym, 'participant id');
  expect(Number.isInteger(data?.private_id), 'no integer private_id');
});
await check('allowed list: a second right token joins too', async () => {
  const c = await session();
  const r = await c.joinPublisher(main.room, mB.pseudonym, mB.joinToken);
  expectEqual(r.plugindata?.data?.videoroom, 'joined', 'join event');
});
await check('allowed remove with a wrong room secret is rejected (433)', async () => {
  await expectRejected(admin.allowed({ room: main.room, secret: `${randomBytes(16).toString('hex')}`, action: 'remove', tokens: [mB.joinToken] }),
    [VIDEOROOM_ERROR.UNAUTHORIZED], 'allowed remove with a wrong secret');
});
await check('allowed remove works: the removed token can no longer join', async () => {
  await admin.allowed({ room: main.room, secret: main.secret, action: 'remove', tokens: [mB.joinToken] });
  const c = await session();
  expectEqual(errorCode(await c.joinPublisher(main.room, mC.pseudonym, mB.joinToken)), VIDEOROOM_ERROR.UNAUTHORIZED, 'error code');
});
await check('allowed add works: the re-added token can join again', async () => {
  await admin.allowed({ room: main.room, secret: main.secret, action: 'add', tokens: [mB.joinToken] });
  const c = await session();
  expectEqual((await c.joinPublisher(main.room, mC.pseudonym, mB.joinToken)).plugindata?.data?.videoroom, 'joined', 'join event');
});
await check('kick with a wrong room secret is rejected (433)', async () => {
  await expectRejected(admin.kick({ room: main.room, secret: `${randomBytes(16).toString('hex')}`, id: mA.pseudonym }),
    [VIDEOROOM_ERROR.UNAUTHORIZED], 'kick with a wrong secret');
});
await check('kick works: the participant is told it was kicked', async () => {
  expect(pubA !== null, 'no session');
  const r = await admin.kick({ room: main.room, secret: main.secret, id: mA.pseudonym });
  expectEqual(r.videoroom, 'success', 'kick reply');
  const ev = await pubA.waitFor((m) => m.plugindata?.data?.reason === 'kicked');
  expectEqual(ev.plugindata.data.leaving, 'ok', 'leaving flag of the kicked event');
});
await check('kick of an unknown participant is rejected (428)', async () => {
  await expectRejected(admin.kick({ room: main.room, secret: main.secret, id: other.members[0].pseudonym }),
    [VIDEOROOM_ERROR.NO_SUCH_FEED], 'kick of an unknown participant');
});
await check('room destroy with a wrong secret is rejected (433), the room stays', async () => {
  await expectRejected(admin.destroyRoom({ room: main.room, secret: `${randomBytes(16).toString('hex')}` }),
    [VIDEOROOM_ERROR.UNAUTHORIZED], 'destroy with a wrong secret');
  expectEqual(await admin.roomExists(main.room), true, 'room exists after the rejected destroy');
});
await check('room destroy with the right secret works', async () => {
  await admin.destroyRoom({ room: main.room, secret: main.secret });
  expectEqual(await admin.roomExists(main.room), false, 'room exists after destroy');
});

// ------------------------------------------------------------------- wrap up
for (const c of open) await c.close();
for (const a of [admin, adminNoKey, adminBadKey]) await a.close();

const failed = results.filter((r) => !r.ok);
console.log(`\nsmoke-negative: ${results.length - failed.length}/${results.length} checks passed`);
if (flags.out) writeFileSync(flags.out, `${JSON.stringify({ passed: results.length - failed.length, failed: failed.length, checks: results }, null, 2)}\n`);
process.exit(failed.length === 0 ? 0 : 1);
