import { test } from 'node:test';
import assert from 'node:assert/strict';
import { execFile } from 'node:child_process';
import { promisify } from 'node:util';
import { fileURLToPath } from 'node:url';
import { startMockJanus } from './helpers/mock-janus-http.mjs';
import {
  JanusAdmin, JanusAdminError, SPEC_ROOM_DEFAULTS, VIDEOROOM_ERROR, createRooms, destroyRooms,
} from '../src/janus-admin.mjs';
import { runCli } from '../bin/qjanus-admin.mjs';
import { verifySessionToken } from '../src/token.mjs';
import { roomPlan, roomId, roomSecret, pseudonym, joinToken, e2eeKey } from '../src/ids.mjs';

const TOKEN_SECRET = 'tok-secret-unit-test-0123456789';
const ADMIN_KEY = 'adm-key-unit-test-abcdef';
const SEED = 'load-seed-unit-test';
const CLI = fileURLToPath(new URL('../bin/qjanus-admin.mjs', import.meta.url));

const EXPECTED_SPEC = {
  is_private: true, bitrate: 1500000, fir_freq: 10, audiocodec: 'opus', videocodec: 'vp8', opus_fec: true,
  opus_dtx: false, audiolevel_ext: false, audiolevel_event: false, videoorient_ext: false, playoutdelay_ext: false,
  transport_wide_cc_ext: true, record: false, lock_record: true, require_pvtid: true, require_e2ee: true,
  notify_joining: false,
};

/** Mock Janus + a JanusAdmin pointed at it, both cleaned up when the test ends. */
async function setup(t, { mock: mockOpts = {}, admin: adminOpts = {} } = {}) {
  const mock = await startMockJanus({ tokenSecret: TOKEN_SECRET, adminKey: ADMIN_KEY, ...mockOpts });
  const admin = new JanusAdmin({ baseUrl: mock.url, tokenSecret: TOKEN_SECRET, adminKey: ADMIN_KEY, ...adminOpts });
  t.after(async () => {
    await admin.close();
    await mock.close();
  });
  return { mock, admin };
}

/** Every secret value the tests know about; none of them may ever show up in errors, logs or CLI output. */
function allSecrets(rooms = 12, size = 12) {
  const list = [TOKEN_SECRET, ADMIN_KEY, SEED];
  for (let k = 0; k < rooms; k++) {
    list.push(roomSecret(SEED, roomId(SEED, k)), e2eeKey(SEED, k));
    for (let i = 0; i < size; i++) list.push(joinToken(SEED, k, i));
  }
  return list;
}
function assertNoSecrets(text, what) {
  for (const s of allSecrets()) assert.ok(!text.includes(s), `${what} leaks a secret (${s.slice(0, 6)}...)`);
}

// ------------------------------------------------------------------- session

test('open(): create + attach over HTTP, a token on every request, fresh token and random transaction each time', async (t) => {
  let clock = Date.now();
  const now = () => (clock += 1000); // every minted token gets a later expiry
  const { mock, admin } = await setup(t, { admin: { now } });
  const { sessionId, handleId } = await admin.open();
  assert.ok(Number.isSafeInteger(sessionId) && Number.isSafeInteger(handleId));
  assert.deepEqual(mock.requests.map((r) => r.janus), ['create', 'attach']);
  assert.equal(mock.requests[0].path, '/janus');
  assert.equal(mock.requests[1].path, `/janus/${sessionId}`);
  assert.equal(mock.requests[1].body, undefined);
  await admin.open(); // idempotent: no new session
  assert.equal(mock.requests.length, 2);

  await admin.roomExists('aaaa');
  await admin.roomExists('bbbb');
  const last = mock.requests.at(-1);
  assert.equal(last.path, `/janus/${sessionId}/${handleId}`);
  assert.equal(last.janus, 'message');

  const expiries = mock.requests.map((r) => Number(r.token.split(',')[0]));
  for (let n = 1; n < expiries.length; n++) assert.equal(expiries[n], expiries[n - 1] + 1, 'each request mints its own token');
  assert.equal(new Set(mock.requests.map((r) => r.token)).size, mock.requests.length);
  for (const r of mock.requests) {
    assert.match(r.transaction, /^[0-9a-f]{16,}$/, 'transaction has at least 64 bit');
    assert.equal(verifySessionToken(r.token, { secret: TOKEN_SECRET, plugin: 'janus.plugin.videoroom', nowMs: clock }), true);
  }
  assert.equal(new Set(mock.requests.map((r) => r.transaction)).size, mock.requests.length);
});

test('close(): detach + destroy the session; idempotent and never throws, even with the server gone', async (t) => {
  const { mock, admin } = await setup(t);
  await admin.open();
  assert.equal(mock.sessions.size, 1);
  await admin.close();
  assert.equal(mock.sessions.size, 0);
  assert.deepEqual(mock.requests.slice(-2).map((r) => r.janus), ['detach', 'destroy']);
  await admin.close();
  await admin.open();
  await mock.close();
  await admin.close(); // connection refused: swallowed
});

test('info() and ping() need no token and no secret', async (t) => {
  const { mock } = await setup(t);
  const bare = new JanusAdmin({ baseUrl: mock.url });
  const info = await bare.info();
  assert.equal(info.janus, 'server_info');
  assert.equal(info.version_string, '1.4.2');
  const ms = await bare.ping();
  assert.ok(Number.isInteger(ms) && ms >= 0);
  assert.deepEqual(mock.requests.map((r) => [r.janus, r.token]), [['info', undefined], ['ping', undefined]]);
  await assert.rejects(bare.open(), (e) => e instanceof JanusAdminError && e.code === 'config');
});

test('constructor validates baseUrl, hash and ttl', () => {
  assert.throws(() => new JanusAdmin({}), /baseUrl/);
  assert.throws(() => new JanusAdmin({ baseUrl: 'not a url' }));
  assert.throws(() => new JanusAdmin({ baseUrl: 'ftp://example.invalid/janus' }), /http/);
  assert.throws(() => new JanusAdmin({ baseUrl: 'http://example.invalid/janus', tokenSecret: 'k', hash: 'md5' }), /hash/);
  assert.throws(() => new JanusAdmin({ baseUrl: 'http://example.invalid/janus', tokenSecret: 'k', tokenTtlSec: 0 }), /ttl/);
});

// -------------------------------------------------------------------- tokens

test('an expired token is rejected with 403 (client clock in the past)', async (t) => {
  const { mock } = await setup(t);
  const stale = new JanusAdmin({
    baseUrl: mock.url, tokenSecret: TOKEN_SECRET, tokenTtlSec: 1, now: () => Date.now() - 60_000,
  });
  await assert.rejects(stale.open(), (e) => {
    assert.ok(e instanceof JanusAdminError);
    assert.equal(e.code, 403);
    assert.equal(e.request, 'janus.create');
    assert.ok(!e.message.includes(TOKEN_SECRET));
    return true;
  });
  assert.equal(mock.sessions.size, 0);
});

test('a token that expires while the session lives is rejected on the next request; a fresh one works again', async (t) => {
  const { mock, admin } = await setup(t);
  await admin.open();
  mock.state.now = () => Date.now() + 3600_000; // server clock jumps an hour ahead: every 600 s token is expired
  await assert.rejects(admin.roomExists('aaaa'), (e) => e.code === 403 && e.request === 'videoroom.exists');
  const before = mock.requests.length;
  await assert.rejects(admin.roomExists('aaaa'), (e) => e.code === 403);
  assert.equal(mock.requests.length, before + 1, '403 is not retried');
  mock.state.now = Date.now;
  assert.equal(await admin.roomExists('aaaa'), false);
});

test('a wrong token secret or hash is rejected with 403; sha1 servers work with hash: "sha1"', async (t) => {
  const { mock } = await setup(t);
  const wrong = new JanusAdmin({ baseUrl: mock.url, tokenSecret: 'wrong-secret-value-zzz' });
  await assert.rejects(wrong.open(), (e) => e.code === 403 && !e.message.includes('wrong-secret-value-zzz'));
  const wrongHash = new JanusAdmin({ baseUrl: mock.url, tokenSecret: TOKEN_SECRET, hash: 'sha1' });
  await assert.rejects(wrongHash.open(), (e) => e.code === 403);

  const sha1 = await startMockJanus({ tokenSecret: TOKEN_SECRET, adminKey: ADMIN_KEY, hash: 'sha1' });
  t.after(() => sha1.close());
  const ok = new JanusAdmin({ baseUrl: sha1.url, tokenSecret: TOKEN_SECRET, hash: 'sha1' });
  await ok.open();
  assert.equal(await ok.roomExists('x'), false);
  await ok.close();
});

// ------------------------------------------------------------------ videoroom

test('SPEC_ROOM_DEFAULTS is exactly the spec section 3 parameter set', () => {
  assert.deepEqual({ ...SPEC_ROOM_DEFAULTS }, EXPECTED_SPEC);
  assert.ok(Object.isFrozen(SPEC_ROOM_DEFAULTS));
});

test('VIDEOROOM_ERROR carries the codes verified in janus_videoroom.c', () => {
  assert.equal(VIDEOROOM_ERROR.NO_SUCH_ROOM, 426);
  assert.equal(VIDEOROOM_ERROR.ROOM_EXISTS, 427);
  assert.equal(VIDEOROOM_ERROR.NO_SUCH_FEED, 428);
  assert.equal(VIDEOROOM_ERROR.MISSING_ELEMENT, 429);
  assert.equal(VIDEOROOM_ERROR.INVALID_ELEMENT, 430);
  assert.equal(VIDEOROOM_ERROR.UNAUTHORIZED, 433);
});

test('createRoom sends the spec parameters, the ACL and the admin_key', async (t) => {
  const { mock, admin } = await setup(t);
  const plan = roomPlan(SEED, 0, 4);
  const tokens = plan.members.map((m) => m.joinToken);
  const res = await admin.createRoom({ room: plan.room, secret: plan.secret, publishers: 4, allowed: tokens });
  assert.deepEqual(res, { videoroom: 'created', room: plan.room, permanent: false, existed: false });

  const sent = mock.requests.at(-1).body;
  assert.equal(sent.request, 'create');
  assert.equal(sent.admin_key, ADMIN_KEY);
  assert.deepEqual(sent.allowed, tokens);
  const room = mock.rooms.get(plan.room);
  assert.deepEqual(room.params, { ...EXPECTED_SPEC, publishers: 4 });
  assert.equal(room.secret, plan.secret);
  assert.deepEqual([...room.allowed], tokens);
  assert.equal(room.checkAllowed, true);
});

test('createRoom: overrides win over the defaults, no allowed key means no ACL, admin_key comes from the constructor', async (t) => {
  const { mock, admin } = await setup(t);
  await admin.createRoom({
    room: 'r1', secret: 's-1', publishers: 2, bitrate: 200000, require_e2ee: false, fir_freq: 5, admin_key: 'ignored-by-design',
  });
  const room = mock.rooms.get('r1');
  assert.equal(room.params.bitrate, 200000);
  assert.equal(room.params.require_e2ee, false);
  assert.equal(room.params.fir_freq, 5);
  assert.equal(room.params.videocodec, 'vp8');
  assert.equal(room.checkAllowed, false);
  assert.equal(mock.requests.at(-1).body.admin_key, ADMIN_KEY);
});

test('createRoom/destroyRoom/allowed/kick validate arguments before sending anything', async (t) => {
  const { mock, admin } = await setup(t);
  await assert.rejects(admin.createRoom({ secret: 's', publishers: 2 }), TypeError);
  await assert.rejects(admin.createRoom({ room: 'r', publishers: 2 }), TypeError);
  await assert.rejects(admin.createRoom({ room: 'r', secret: 's', publishers: 0 }), RangeError);
  await assert.rejects(admin.createRoom({ room: 'r', secret: 's', publishers: 2, allowed: [1] }), TypeError);
  await assert.rejects(admin.destroyRoom({ room: 'r' }), TypeError);
  await assert.rejects(admin.allowed({ room: 'r', secret: 's', action: 'nuke' }), RangeError);
  await assert.rejects(admin.allowed({ room: 'r', secret: 's', action: 'add' }), TypeError);
  await assert.rejects(admin.kick({ room: 'r', secret: 's' }), TypeError);
  await assert.rejects(admin.request({}), TypeError);
  assert.equal(mock.requests.length, 0);
});

test('wrong or missing admin_key: create refused (433 / 429) and nothing is created', async (t) => {
  const { mock } = await setup(t);
  const wrong = new JanusAdmin({ baseUrl: mock.url, tokenSecret: TOKEN_SECRET, adminKey: 'wrong-admin-key-123' });
  await assert.rejects(wrong.createRoom({ room: 'r', secret: 's', publishers: 2 }), (e) => {
    assert.equal(e.code, VIDEOROOM_ERROR.UNAUTHORIZED);
    assert.equal(e.request, 'videoroom.create');
    assert.match(e.reason, /wrong admin_key/);
    assert.ok(!e.message.includes('wrong-admin-key-123'));
    return true;
  });
  const none = new JanusAdmin({ baseUrl: mock.url, tokenSecret: TOKEN_SECRET });
  await assert.rejects(none.createRoom({ room: 'r', secret: 's', publishers: 2 }), (e) => e.code === VIDEOROOM_ERROR.MISSING_ELEMENT);
  assert.equal(mock.rooms.size, 0);
  await wrong.close();
  await none.close();
});

test('create is idempotent with ignoreExists, an error (427) without; roomExists follows', async (t) => {
  const { mock, admin } = await setup(t);
  const args = { room: roomId(SEED, 0), secret: roomSecret(SEED, roomId(SEED, 0)), publishers: 3 };
  assert.equal(await admin.roomExists(args.room), false);
  await admin.createRoom(args);
  assert.equal(await admin.roomExists(args.room), true);
  await assert.rejects(admin.createRoom(args), (e) => {
    assert.equal(e.code, VIDEOROOM_ERROR.ROOM_EXISTS);
    assert.ok(e.message.includes(`${args.room.slice(0, 8)}...`), 'room id is shortened');
    assert.ok(!e.message.includes(args.room), 'full room id is not echoed');
    return true;
  });
  const again = await admin.createRoom({ ...args, ignoreExists: true });
  assert.deepEqual(again, { videoroom: 'exists', room: args.room, existed: true });
  assert.equal(mock.rooms.size, 1);
});

test('destroy is idempotent with ignoreMissing, an error (426) without; the room secret is enforced (433)', async (t) => {
  const { mock, admin } = await setup(t);
  await admin.createRoom({ room: 'r1', secret: 'the-room-secret', publishers: 2 });
  await assert.rejects(admin.destroyRoom({ room: 'r1', secret: 'not-the-secret' }), (e) => {
    assert.equal(e.code, VIDEOROOM_ERROR.UNAUTHORIZED);
    assert.ok(!e.message.includes('not-the-secret'));
    return true;
  });
  assert.ok(mock.rooms.has('r1'));
  assert.deepEqual(await admin.destroyRoom({ room: 'r1', secret: 'the-room-secret' }), {
    videoroom: 'destroyed', room: 'r1', permanent: false, missing: false,
  });
  await assert.rejects(admin.destroyRoom({ room: 'r1', secret: 'the-room-secret' }), (e) => e.code === VIDEOROOM_ERROR.NO_SUCH_ROOM);
  assert.deepEqual(await admin.destroyRoom({ room: 'r1', secret: 'x', ignoreMissing: true }), { videoroom: 'missing', room: 'r1', missing: true });
});

test('allowed: add / remove / disable / enable maintain the ACL and need the room secret', async (t) => {
  const { mock, admin } = await setup(t);
  await admin.createRoom({ room: 'r1', secret: 'sec-r1', publishers: 2, allowed: ['t1'] });
  const base = { room: 'r1', secret: 'sec-r1' };
  assert.deepEqual((await admin.allowed({ ...base, action: 'add', tokens: ['t2', 't3'] })).allowed.sort(), ['t1', 't2', 't3']);
  assert.deepEqual((await admin.allowed({ ...base, action: 'remove', tokens: ['t1'] })).allowed.sort(), ['t2', 't3']);
  assert.equal((await admin.allowed({ ...base, action: 'disable' })).allowed, undefined);
  assert.equal(mock.rooms.get('r1').checkAllowed, false);
  assert.deepEqual((await admin.allowed({ ...base, action: 'enable' })).allowed.sort(), ['t2', 't3']);
  assert.equal(mock.rooms.get('r1').checkAllowed, true);
  await assert.rejects(admin.allowed({ room: 'r1', secret: 'bad', action: 'enable' }), (e) => e.code === 433);
  await assert.rejects(admin.allowed({ room: 'nope', secret: 's', action: 'enable' }), (e) => e.code === 426);
});

test('kick removes the participant; unknown participant (428) and wrong secret (433) fail', async (t) => {
  const { mock, admin } = await setup(t);
  await admin.createRoom({ room: 'r1', secret: 'sec-r1', publishers: 2 });
  mock.addParticipant('r1', { id: 'p1' });
  mock.addParticipant('r1', { id: 'p2' });
  assert.deepEqual((await admin.listParticipants('r1')).participants.map((p) => p.id), ['p1', 'p2']);
  await assert.rejects(admin.kick({ room: 'r1', secret: 'bad', id: 'p1' }), (e) => e.code === 433);
  assert.deepEqual(await admin.kick({ room: 'r1', secret: 'sec-r1', id: 'p1' }), { videoroom: 'success' });
  assert.deepEqual((await admin.listParticipants('r1')).participants.map((p) => p.id), ['p2']);
  await assert.rejects(admin.kick({ room: 'r1', secret: 'sec-r1', id: 'p1' }), (e) => e.code === VIDEOROOM_ERROR.NO_SUCH_FEED);
  await assert.rejects(admin.listParticipants('missing'), (e) => e.code === VIDEOROOM_ERROR.NO_SUCH_ROOM);
});

test('listRooms includes private rooms only because the admin_key is sent', async (t) => {
  const { mock, admin } = await setup(t);
  await admin.createRoom({ room: 'r-private', secret: 's', publishers: 2 });
  await admin.createRoom({ room: 'r-public', secret: 's', publishers: 2, is_private: false });
  const rooms = (await admin.listRooms()).list.map((r) => r.room).sort();
  assert.deepEqual(rooms, ['r-private', 'r-public']);
  assert.equal(mock.requests.at(-1).body.admin_key, ADMIN_KEY);
  const anonymous = new JanusAdmin({ baseUrl: mock.url, tokenSecret: TOKEN_SECRET });
  assert.deepEqual((await anonymous.listRooms()).list.map((r) => r.room), ['r-public']);
  await anonymous.close();
});

test('an "ack" answer (asynchronous plugin request) is a clear error', async (t) => {
  const { admin } = await setup(t);
  await assert.rejects(admin.request({ request: 'join', ptype: 'publisher', room: 'r' }), (e) => {
    assert.equal(e.code, 'ack');
    assert.match(e.message, /asynchronous/);
    return true;
  });
});

// ---------------------------------------------------------------- re-open

test('a session Janus dropped (458) is re-opened transparently once', async (t) => {
  const { mock, admin } = await setup(t);
  const first = await admin.open();
  await admin.createRoom({ room: 'r1', secret: 's', publishers: 2 });
  mock.dropSessions();
  assert.equal(await admin.roomExists('r1'), true, 'rooms live on; the request itself succeeds after the re-open');
  const second = await admin.open();
  assert.notEqual(second.sessionId, first.sessionId);
  assert.equal(mock.requests.filter((r) => r.janus === 'create').length, 2);
});

test('a dropped handle (459) is re-opened as well', async (t) => {
  const { mock, admin } = await setup(t);
  const first = await admin.open();
  mock.dropHandles();
  await admin.createRoom({ room: 'r1', secret: 's', publishers: 2 });
  assert.notEqual((await admin.open()).handleId, first.handleId);
  assert.ok(mock.rooms.has('r1'));
});

test('parallel callers share ONE re-open after a drop', async (t) => {
  const { mock, admin } = await setup(t, { mock: { latencyMs: 15 } });
  await admin.open();
  mock.dropSessions();
  const results = await Promise.all(Array.from({ length: 8 }, (_, n) => admin.roomExists(`room-${n}`)));
  assert.deepEqual(results, new Array(8).fill(false));
  assert.equal(mock.requests.filter((r) => r.janus === 'create').length, 2, 'initial session + exactly one re-open');
  assert.equal(mock.sessions.size, 1);
});

test('re-open is attempted only once per request: a persistent 458 surfaces', async (t) => {
  const { mock } = await setup(t);
  const always458 = async (url, init) => {
    const body = JSON.parse(init.body);
    if (body.janus === 'message') {
      return new Response(JSON.stringify({ janus: 'error', transaction: body.transaction, error: { code: 458, reason: 'No such session 1' } }));
    }
    return fetch(url, init);
  };
  const admin = new JanusAdmin({ baseUrl: mock.url, tokenSecret: TOKEN_SECRET, fetchImpl: always458 });
  await assert.rejects(admin.roomExists('x'), (e) => e.code === 458);
  assert.equal(mock.requests.filter((r) => r.janus === 'create').length, 2);
  await admin.close();
});

// -------------------------------------------------------------- transport errors

test('timeouts, network failures, HTTP errors and garbage answers get distinct codes', async () => {
  const base = { baseUrl: 'http://example.invalid/janus', tokenSecret: TOKEN_SECRET };
  // A real fetch holds a socket open; AbortSignal.timeout's own timer is unref'd, so the fake needs a ref'd one.
  const hang = (_url, init) => new Promise((_, reject) => {
    const keepAlive = setTimeout(() => {}, 30_000);
    init.signal.addEventListener('abort', () => {
      clearTimeout(keepAlive);
      reject(init.signal.reason);
    });
  });
  await assert.rejects(
    new JanusAdmin({ ...base, fetchImpl: hang, requestTimeoutMs: 40 }).ping(),
    (e) => e.code === 'timeout' && e.request === 'janus.ping',
  );
  const refused = () => Promise.reject(Object.assign(new TypeError('fetch failed'), { cause: { code: 'ECONNREFUSED' } }));
  await assert.rejects(new JanusAdmin({ ...base, fetchImpl: refused }).ping(), (e) => e.code === 'network' && /ECONNREFUSED/.test(e.message));
  const status502 = async () => new Response('bad gateway', { status: 502 });
  await assert.rejects(new JanusAdmin({ ...base, fetchImpl: status502 }).ping(), (e) => e.code === 'http_502');
  const html = async () => new Response('<html></html>');
  await assert.rejects(new JanusAdmin({ ...base, fetchImpl: html }).ping(), (e) => e.code === 'bad_response');
  const wrongTx = async () => new Response(JSON.stringify({ janus: 'pong', transaction: 'someone-else' }));
  await assert.rejects(new JanusAdmin({ ...base, fetchImpl: wrongTx }).ping(), (e) => e.code === 'bad_response');
  const notPong = async (_url, init) => new Response(JSON.stringify({ janus: 'ack', transaction: JSON.parse(init.body).transaction }));
  await assert.rejects(new JanusAdmin({ ...base, fetchImpl: notPong }).ping(), (e) => e.code === 'bad_response');
});

test('a server that is gone is a "network" error', async (t) => {
  const { mock, admin } = await setup(t);
  await mock.close();
  await assert.rejects(admin.open(), (e) => e.code === 'network');
});

// ------------------------------------------------------------- bulk helpers

test('createRooms: ids/secrets/ACL derived from the seed; publishers defaults to size; idempotent', async (t) => {
  const { mock, admin } = await setup(t);
  const res = await createRooms(admin, { seed: SEED, first: 2, count: 3, size: 4 });
  assert.deepEqual(res.map((r) => r.k), [2, 3, 4]);
  assert.deepEqual(res.map((r) => r.existed), [false, false, false]);
  for (const { k, room } of res) {
    const plan = roomPlan(SEED, k, 4);
    assert.equal(room, plan.room);
    const stored = mock.rooms.get(plan.room);
    assert.equal(stored.secret, plan.secret);
    assert.equal(stored.params.publishers, 4);
    assert.deepEqual([...stored.allowed].sort(), plan.members.map((m) => m.joinToken).sort());
    assert.equal(stored.params.require_e2ee, true);
  }
  const again = await createRooms(admin, { seed: SEED, first: 2, count: 3, size: 4 });
  assert.deepEqual(again.map((r) => r.existed), [true, true, true]);
  assert.equal(mock.rooms.size, 3);
});

test('createRooms: explicit publishers, allowed:false and an ACL topped up on an existing room', async (t) => {
  const { mock, admin } = await setup(t);
  await createRooms(admin, { seed: SEED, count: 1, size: 2, publishers: 5, allowed: false });
  const room = mock.rooms.get(roomId(SEED, 0));
  assert.equal(room.params.publishers, 5);
  assert.equal(room.checkAllowed, false);
  assert.equal(room.allowed.size, 0);
  // a later run with allowed on and a bigger size: same room is kept, ACL now enforced and complete
  await createRooms(admin, { seed: SEED, count: 1, size: 3 });
  assert.equal(room.checkAllowed, true);
  assert.deepEqual([...room.allowed].sort(), roomPlan(SEED, 0, 3).members.map((m) => m.joinToken).sort());
});

test('createRooms: bounded concurrency', async (t) => {
  const { mock, admin } = await setup(t, { mock: { latencyMs: 25 } });
  await createRooms(admin, { seed: SEED, count: 20, size: 2, concurrency: 4 });
  assert.equal(mock.rooms.size, 20);
  assert.ok(mock.state.peakInFlight <= 4, `peak ${mock.state.peakInFlight} exceeds the limit`);
  assert.ok(mock.state.peakInFlight >= 2, 'requests really ran in parallel');
  assert.equal(mock.requests.filter((r) => r.janus === 'create').length, 1, 'one shared session');
  await assert.rejects(createRooms(admin, { seed: SEED, count: 1, size: 2, concurrency: 0 }), /concurrency/);
});

test('createRooms: per-room failures are collected, every room is tried, then one aggregate error is thrown', async (t) => {
  const { mock, admin } = await setup(t);
  // room k=2 already exists under a foreign secret: creating it is "fine", topping up its ACL is refused
  await admin.createRoom({ room: roomId(SEED, 2), secret: 'somebody-elses-secret', publishers: 2, allowed: [] });
  await assert.rejects(createRooms(admin, { seed: SEED, count: 5, size: 2, concurrency: 2 }), (e) => {
    assert.ok(e instanceof JanusAdminError);
    assert.equal(e.code, 'partial_failure');
    assert.equal(e.failures.length, 1);
    assert.equal(e.failures[0].k, 2);
    assert.equal(e.failures[0].code, VIDEOROOM_ERROR.UNAUTHORIZED);
    assert.deepEqual(e.results.map((r) => r.k).sort(), [0, 1, 3, 4]);
    assert.match(e.message, /1 of 5 rooms failed/);
    assertNoSecrets(e.message + JSON.stringify(e.failures), 'aggregate error');
    return true;
  });
  assert.equal(mock.rooms.size, 5, 'the healthy rooms were created despite the failure');
});

test('createRooms with a wrong admin_key fails every room (aggregate of 433)', async (t) => {
  const { mock } = await setup(t);
  const admin = new JanusAdmin({ baseUrl: mock.url, tokenSecret: TOKEN_SECRET, adminKey: 'wrong-admin-key-123' });
  await assert.rejects(createRooms(admin, { seed: SEED, count: 4, size: 2 }), (e) => {
    assert.equal(e.failures.length, 4);
    assert.ok(e.failures.every((f) => f.code === 433));
    return true;
  });
  assert.equal(mock.rooms.size, 0);
  await admin.close();
});

test('destroyRooms: destroys the derived rooms, missing rooms are fine, foreign-secret rooms are aggregated', async (t) => {
  const { mock, admin } = await setup(t);
  await createRooms(admin, { seed: SEED, count: 4, size: 2 });
  const res = await destroyRooms(admin, { seed: SEED, first: 1, count: 4 }); // k=1..4, k=4 never existed
  assert.deepEqual(res.map((r) => [r.k, r.existed]), [[1, true], [2, true], [3, true], [4, false]]);
  assert.deepEqual([...mock.rooms.keys()], [roomId(SEED, 0)]);
  await admin.createRoom({ room: roomId(SEED, 7), secret: 'somebody-elses-secret', publishers: 2 });
  await assert.rejects(destroyRooms(admin, { seed: SEED, first: 6, count: 3 }), (e) => {
    assert.equal(e.failures.length, 1);
    assert.equal(e.failures[0].k, 7);
    assert.equal(e.failures[0].code, 433);
    assert.equal(e.results.length, 2);
    return true;
  });
});

test('helper argument validation', async (t) => {
  const { admin } = await setup(t);
  await assert.rejects(createRooms(admin, { seed: SEED, count: 0, size: 2 }), /count/);
  await assert.rejects(createRooms(admin, { seed: SEED, count: 1, size: 0 }), /size/);
  await assert.rejects(createRooms(admin, { seed: SEED, first: -1, count: 1, size: 2 }), /first/);
  await assert.rejects(destroyRooms(admin, { seed: SEED }), /count/);
});

test('no secret, token, room secret or join token in errors or log lines', async (t) => {
  const lines = [];
  const { mock } = await setup(t);
  const admin = new JanusAdmin({
    baseUrl: mock.url, tokenSecret: TOKEN_SECRET, adminKey: 'wrong-admin-key-123', log: (l) => lines.push(l),
  });
  const plan = roomPlan(SEED, 0, 3);
  const good = new JanusAdmin({ baseUrl: mock.url, tokenSecret: TOKEN_SECRET, adminKey: ADMIN_KEY, log: (l) => lines.push(l) });
  const errors = [];
  const attempt = async (p) => p.catch((e) => errors.push(`${e.message} | ${e.reason} | ${e.request}`));
  await attempt(admin.createRoom({ room: plan.room, secret: plan.secret, publishers: 3, allowed: plan.members.map((m) => m.joinToken) }));
  await good.createRoom({ room: plan.room, secret: plan.secret, publishers: 3 });
  await attempt(good.createRoom({ room: plan.room, secret: plan.secret, publishers: 3 }));
  await attempt(good.destroyRoom({ room: plan.room, secret: 'wrong' }));
  await attempt(good.allowed({ room: plan.room, secret: 'wrong', action: 'add', tokens: [plan.members[0].joinToken] }));
  await attempt(good.kick({ room: plan.room, secret: plan.secret, id: 'ghost' }));
  mock.dropSessions();
  await good.roomExists(plan.room);
  await good.close();
  await admin.close();
  assert.ok(errors.length >= 5 && lines.length >= 3);
  assertNoSecrets(errors.join('\n'), 'errors');
  assertNoSecrets(lines.join('\n'), 'log lines');
  assert.ok(!errors.join('\n').includes('wrong-admin-key-123'));
  for (const line of lines) assert.ok(!/\d+,janus,/.test(line), 'no signed token in logs');
});

// ------------------------------------------------------------------------ CLI

const execFileP = promisify(execFile);

/** Environment the CLI sees: the standard QJANUS_* variables plus overrides (undefined removes one). */
function cliEnv(mock, envOverrides) {
  const env = {
    QJANUS_ADMIN_URL: mock.url,
    QJANUS_TOKEN_SECRET: TOKEN_SECRET,
    QJANUS_ADMIN_KEY: ADMIN_KEY,
    QJANUS_LOADTEST_SEED: SEED,
    ...envOverrides,
  };
  for (const key of Object.keys(env)) if (env[key] === undefined) delete env[key];
  return env;
}

/**
 * Run the CLI in-process (fast: starting a node process takes seconds on a loaded machine) and
 * assert that no known secret shows up in stdout/stderr.
 */
async function cli(mock, args, envOverrides = {}) {
  let stdout = '';
  let stderr = '';
  const code = await runCli(args, cliEnv(mock, envOverrides), {
    stdout: (text) => { stdout += text; },
    stderr: (text) => { stderr += text; },
  });
  assertNoSecrets(stdout, `stdout of "${args.join(' ')}"`);
  assertNoSecrets(stderr, `stderr of "${args.join(' ')}"`);
  return { code, stdout, stderr };
}

/** Run the real entry point `node bin/qjanus-admin.mjs` in a child process. */
async function spawnCli(mock, args, envOverrides = {}) {
  const env = { ...process.env };
  for (const key of Object.keys(env)) if (key.startsWith('QJANUS_')) delete env[key];
  Object.assign(env, cliEnv(mock, envOverrides));
  let result;
  try {
    const { stdout, stderr } = await execFileP(process.execPath, [CLI, ...args], { env, timeout: 120_000 });
    result = { code: 0, stdout, stderr };
  } catch (e) {
    result = { code: e.code, stdout: e.stdout ?? '', stderr: e.stderr ?? '' };
  }
  assertNoSecrets(result.stdout, `stdout of spawned "${args.join(' ')}"`);
  assertNoSecrets(result.stderr, `stderr of spawned "${args.join(' ')}"`);
  return result;
}

test('CLI as a real process (node bin/qjanus-admin.mjs): usage, missing env, token, create/list/destroy, failure', { timeout: 600_000 }, async (t) => {
  const { mock } = await setup(t);
  const help = await spawnCli(mock, ['--help']);
  assert.equal(help.code, 0);
  assert.match(help.stdout, /Usage: qjanus-admin/);

  const noSecret = await spawnCli(mock, ['create-rooms', '--rooms', '1', '--size', '2'], { QJANUS_TOKEN_SECRET: undefined });
  assert.equal(noSecret.code, 2);
  assert.match(noSecret.stderr, /QJANUS_TOKEN_SECRET/);

  const token = await spawnCli(mock, ['token']);
  assert.equal(token.code, 0);
  assert.equal(verifySessionToken(token.stdout.trim(), { secret: TOKEN_SECRET, plugin: 'janus.plugin.videoroom' }), true);

  const created = await spawnCli(mock, ['create-rooms', '--rooms', '2', '--size', '3']);
  assert.equal(created.code, 0, created.stderr);
  assert.match(created.stdout, /2 rooms ready \(2 created\), size 3, publishers 3, allowed on/);
  assert.equal(mock.rooms.size, 2);
  assert.equal(mock.sessions.size, 0, 'the process closed its Janus session');
  const listed = await spawnCli(mock, ['list-rooms']);
  assert.match(listed.stdout, /2 rooms/);

  const wrongKey = await spawnCli(mock, ['create-rooms', '--rooms', '1', '--first', '9', '--size', '2'], { QJANUS_ADMIN_KEY: 'wrong-admin-key-123' });
  assert.equal(wrongKey.code, 1);
  assert.match(wrongKey.stderr, /433/);

  const destroyed = await spawnCli(mock, ['destroy-rooms', '--rooms', '2']);
  assert.equal(destroyed.code, 0, destroyed.stderr);
  assert.equal(mock.rooms.size, 0);
});

test('CLI --help prints usage (exit 0); no command, unknown command and unknown flag are usage errors (exit 2)', async (t) => {
  const { mock } = await setup(t);
  const help = await cli(mock, ['--help']);
  assert.equal(help.code, 0);
  assert.match(help.stdout, /Usage: qjanus-admin/);
  for (const cmd of ['token', 'info', 'ping', 'create-rooms', 'destroy-rooms', 'list-rooms', 'participants', 'kick', 'allowed']) {
    assert.ok(help.stdout.includes(cmd), `help mentions ${cmd}`);
  }
  assert.equal((await cli(mock, [])).code, 2);
  const unknown = await cli(mock, ['frobnicate']);
  assert.equal(unknown.code, 2);
  assert.match(unknown.stderr, /unknown command/);
  assert.equal((await cli(mock, ['ping', '--nope'])).code, 2);
  assert.equal((await cli(mock, ['create-rooms', '--rooms', 'many', '--size', '2'])).code, 2);
  assert.equal((await cli(mock, ['create-rooms', '--rooms', '2'])).code, 2, '--size is required');
});

test('CLI: missing environment exits 2 and names the variable, never a value', async (t) => {
  const { mock } = await setup(t);
  const cases = [
    [['token'], { QJANUS_TOKEN_SECRET: undefined }, 'QJANUS_TOKEN_SECRET'],
    [['create-rooms', '--rooms', '1', '--size', '2'], { QJANUS_TOKEN_SECRET: '' }, 'QJANUS_TOKEN_SECRET'],
    [['create-rooms', '--rooms', '1', '--size', '2'], { QJANUS_ADMIN_KEY: undefined }, 'QJANUS_ADMIN_KEY'],
    [['create-rooms', '--rooms', '1', '--size', '2'], { QJANUS_LOADTEST_SEED: undefined }, 'QJANUS_LOADTEST_SEED'],
    [['destroy-rooms', '--rooms', '1'], { QJANUS_LOADTEST_SEED: undefined }, 'QJANUS_LOADTEST_SEED'],
    [['list-rooms'], { QJANUS_ADMIN_KEY: undefined }, 'QJANUS_ADMIN_KEY'],
    [['ping'], { QJANUS_ADMIN_URL: undefined }, 'QJANUS_ADMIN_URL'],
    [['info'], { QJANUS_ADMIN_URL: undefined }, 'QJANUS_ADMIN_URL'],
  ];
  for (const [args, env, name] of cases) {
    const r = await cli(mock, args, env);
    assert.equal(r.code, 2, `${args.join(' ')} without ${name}`);
    assert.ok(r.stderr.includes(name), `stderr names ${name}`);
  }
  // custom variable names via --*-env, and a secret from a file
  const custom = await cli(mock, ['token', '--token-secret-env', 'MY_OWN_SECRET'], { QJANUS_TOKEN_SECRET: undefined, MY_OWN_SECRET: 'custom-name-secret' });
  assert.equal(custom.code, 0);
  assert.equal(verifySessionToken(custom.stdout.trim(), { secret: 'custom-name-secret' }), true);
  const missingCustom = await cli(mock, ['token', '--token-secret-env', 'NOT_SET_ANYWHERE']);
  assert.equal(missingCustom.code, 2);
  assert.match(missingCustom.stderr, /NOT_SET_ANYWHERE/);
});

test('CLI token: prints a valid signed token (only secret-derived output), honours --ttl-sec/--plugin/--json', async (t) => {
  const { mock } = await setup(t);
  const r = await cli(mock, ['token']);
  assert.equal(r.code, 0);
  const token = r.stdout.trim();
  assert.equal(r.stdout, `${token}\n`);
  assert.equal(verifySessionToken(token, { secret: TOKEN_SECRET, plugin: 'janus.plugin.videoroom' }), true);
  const exp = Number(token.split(',')[0]);
  assert.ok(Math.abs(exp - (Date.now() / 1000 + 600)) < 30);

  const short = await cli(mock, ['token', '--ttl-sec', '5', '--plugin', 'janus.plugin.echotest', '--json']);
  const j = JSON.parse(short.stdout);
  assert.equal(j.ok, true);
  assert.ok(j.token.includes(',janus,janus.plugin.echotest:'));
  assert.ok(Math.abs(j.expiresAtUnix - (Date.now() / 1000 + 5)) < 30);
  assert.equal((await cli(mock, ['token', '--ttl-sec', '0'])).code, 2);
  assert.equal((await cli(mock, ['token', '--hash', 'md5'])).code, 2);
});

test('CLI info and ping work without secrets and never print server addresses', async (t) => {
  const { mock } = await setup(t);
  const noSecrets = { QJANUS_TOKEN_SECRET: undefined, QJANUS_ADMIN_KEY: undefined, QJANUS_LOADTEST_SEED: undefined };
  const info = await cli(mock, ['info'], noSecrets);
  assert.equal(info.code, 0);
  assert.match(info.stdout, /janus 1\.4\.2/);
  assert.match(info.stdout, /janus\.plugin\.videoroom/);
  const infoJson = await cli(mock, ['info', '--json'], noSecrets);
  const parsed = JSON.parse(infoJson.stdout);
  assert.equal(parsed.ok, true);
  assert.equal(parsed.info.version_string, '1.4.2');
  assert.ok(!('local-ip' in parsed.info));
  assert.ok(!infoJson.stdout.includes('127.0.0.1'));
  const ping = await cli(mock, ['ping'], noSecrets);
  assert.equal(ping.code, 0);
  assert.match(ping.stdout, /^pong \d+ ms\n$/);
  assert.equal(JSON.parse((await cli(mock, ['ping', '--json'], noSecrets)).stdout).ok, true);
  const flagUrl = await cli(mock, ['ping', '--admin-url', mock.url], { QJANUS_ADMIN_URL: undefined, ...noSecrets });
  assert.equal(flagUrl.code, 0);
});

test('CLI create-rooms / list-rooms / participants / kick / allowed / destroy-rooms against the mock', async (t) => {
  const { mock } = await setup(t);
  const created = await cli(mock, ['create-rooms', '--rooms', '3', '--size', '4']);
  assert.equal(created.code, 0, created.stderr);
  const lines = created.stdout.trim().split('\n');
  assert.equal(lines.length, 4);
  for (let k = 0; k < 3; k++) {
    assert.ok(lines[k].includes(`room=${roomId(SEED, k).slice(0, 8)} created`), lines[k]);
    assert.ok(!lines[k].includes(roomId(SEED, k)), 'only 8 characters of the room id');
    const room = mock.rooms.get(roomId(SEED, k));
    assert.equal(room.params.publishers, 4);
    assert.equal(room.allowed.size, 4);
    assert.equal(room.secret, roomSecret(SEED, roomId(SEED, k)));
  }
  assert.match(lines[3], /3 rooms ready \(3 created\), size 4, publishers 4, allowed on/);

  const again = await cli(mock, ['create-rooms', '--rooms', '3', '--size', '4']);
  assert.equal(again.code, 0);
  assert.equal(again.stdout.match(/ exists/g).length, 3);

  const json = JSON.parse((await cli(mock, ['create-rooms', '--rooms', '2', '--first', '3', '--size', '2', '--publishers', '2', '--no-allowed', '--json'])).stdout);
  assert.equal(json.ok, true);
  assert.deepEqual(json.rooms, [3, 4].map((k) => ({ k, room: roomId(SEED, k), existed: false })));
  assert.equal(json.allowed, false);
  assert.equal(mock.rooms.get(roomId(SEED, 3)).checkAllowed, false);
  assert.equal(mock.rooms.get(roomId(SEED, 3)).params.publishers, 2);

  const listed = await cli(mock, ['list-rooms']);
  assert.equal(listed.code, 0);
  assert.match(listed.stdout, /5 rooms/);
  assert.match(listed.stdout, /e2ee=true private=true/);
  const listedJson = JSON.parse((await cli(mock, ['list-rooms', '--json'])).stdout);
  assert.deepEqual(listedJson.rooms.map((r) => r.room).sort(), [0, 1, 2, 3, 4].map((k) => roomId(SEED, k)).sort());

  mock.addParticipant(roomId(SEED, 1), { id: pseudonym(SEED, 1, 0) });
  mock.addParticipant(roomId(SEED, 1), { id: pseudonym(SEED, 1, 2) });
  const parts = await cli(mock, ['participants', '--room-index', '1']);
  assert.equal(parts.code, 0);
  assert.match(parts.stdout, /2 participants in room/);
  assert.ok(parts.stdout.includes(`id=${pseudonym(SEED, 1, 0).slice(0, 8)}`));

  const kicked = await cli(mock, ['kick', '--room-index', '1', '--index', '2']);
  assert.equal(kicked.code, 0, kicked.stderr);
  assert.deepEqual([...mock.rooms.get(roomId(SEED, 1)).participants.keys()], [pseudonym(SEED, 1, 0)]);
  assert.equal((await cli(mock, ['kick', '--room-index', '1', '--index', '2'])).code, 1, 'kicking a ghost fails with exit 1');

  const removed = await cli(mock, ['allowed', '--room-index', '0', '--action', 'remove', '--index', '0', '--index', '1']);
  assert.equal(removed.code, 0, removed.stderr);
  assert.match(removed.stdout, /action=remove allowed=2/);
  assert.deepEqual([...mock.rooms.get(roomId(SEED, 0)).allowed].sort(), [2, 3].map((i) => joinToken(SEED, 0, i)).sort());
  const added = await cli(mock, ['allowed', '--room-index', '0', '--action', 'add', '--index', '0']);
  assert.match(added.stdout, /allowed=3/);
  assert.equal((await cli(mock, ['allowed', '--room-index', '0', '--action', 'disable'])).code, 0);
  assert.equal(mock.rooms.get(roomId(SEED, 0)).checkAllowed, false);
  assert.equal((await cli(mock, ['allowed', '--room-index', '0', '--action', 'enable'])).code, 0);
  assert.equal(mock.rooms.get(roomId(SEED, 0)).checkAllowed, true);
  assert.equal((await cli(mock, ['allowed', '--room-index', '0', '--action', 'add'])).code, 2, 'add needs --index');
  assert.equal((await cli(mock, ['allowed', '--room-index', '0', '--action', 'bogus'])).code, 2);

  const destroyed = await cli(mock, ['destroy-rooms', '--rooms', '6']); // k=5 never existed
  assert.equal(destroyed.code, 0, destroyed.stderr);
  assert.match(destroyed.stdout, /k=5 room=\w{8} missing/);
  assert.match(destroyed.stdout, /6 rooms handled \(5 destroyed\)/);
  assert.equal(mock.rooms.size, 0);
  assert.equal((await cli(mock, ['destroy-rooms', '--rooms', '2', '--json'])).code, 0, 'destroying again is fine');
  assert.equal(mock.sessions.size, 0, 'the CLI closes its Janus session');
});

test('CLI failures exit 1: wrong admin_key, wrong token secret, unreachable server; --json reports them', async (t) => {
  const { mock } = await setup(t);
  const badKey = await cli(mock, ['create-rooms', '--rooms', '2', '--size', '2'], { QJANUS_ADMIN_KEY: 'wrong-admin-key-123' });
  assert.equal(badKey.code, 1);
  assert.match(badKey.stderr, /433/);
  assert.match(badKey.stderr, /2 of 2 rooms failed/);
  assert.ok(!badKey.stderr.includes('wrong-admin-key-123'));
  assert.equal(mock.rooms.size, 0);

  const badKeyJson = await cli(mock, ['create-rooms', '--rooms', '2', '--size', '2', '--json'], { QJANUS_ADMIN_KEY: 'wrong-admin-key-123' });
  assert.equal(badKeyJson.code, 1);
  const j = JSON.parse(badKeyJson.stdout);
  assert.equal(j.ok, false);
  assert.equal(j.failures.length, 2);

  const badToken = await cli(mock, ['list-rooms'], { QJANUS_TOKEN_SECRET: 'wrong-token-secret-xyz' });
  assert.equal(badToken.code, 1);
  assert.match(badToken.stderr, /403/);
  assert.ok(!badToken.stderr.includes('wrong-token-secret-xyz'));

  const url = mock.url;
  await mock.close();
  const gone = await cli(mock, ['ping'], { QJANUS_ADMIN_URL: url });
  assert.equal(gone.code, 1);
  assert.match(gone.stderr, /network/);
});

test('CLI reads secrets from files and honours custom variable names', async (t) => {
  const { mock } = await setup(t);
  const { mkdtempSync, writeFileSync, rmSync } = await import('node:fs');
  const { tmpdir } = await import('node:os');
  const { join } = await import('node:path');
  const dir = mkdtempSync(join(tmpdir(), 'qjanus-admin-'));
  t.after(() => rmSync(dir, { recursive: true, force: true }));
  const files = { secret: join(dir, 'secret'), key: join(dir, 'key'), seed: join(dir, 'seed') };
  writeFileSync(files.secret, `${TOKEN_SECRET}\n`);
  writeFileSync(files.key, `${ADMIN_KEY}\r\n`);
  writeFileSync(files.seed, SEED);
  const none = { QJANUS_TOKEN_SECRET: undefined, QJANUS_ADMIN_KEY: undefined, QJANUS_LOADTEST_SEED: undefined };
  const r = await cli(mock, [
    'create-rooms', '--rooms', '1', '--size', '2',
    '--token-secret-file', files.secret, '--admin-key-file', files.key, '--seed-file', files.seed,
  ], none);
  assert.equal(r.code, 0, r.stderr);
  assert.ok(mock.rooms.has(roomId(SEED, 0)));
  const missing = await cli(mock, ['token', '--token-secret-file', join(dir, 'nope')], none);
  assert.equal(missing.code, 2);
  const viaEnvNames = await cli(mock, [
    'destroy-rooms', '--rooms', '1', '--token-secret-env', 'S1', '--seed-env', 'S2',
  ], { ...none, S1: TOKEN_SECRET, S2: SEED });
  assert.equal(viaEnvNames.code, 0, viaEnvNames.stderr);
  assert.equal(mock.rooms.size, 0);
});
