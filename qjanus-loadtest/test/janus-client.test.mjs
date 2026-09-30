import { test } from 'node:test';
import assert from 'node:assert/strict';
import { WebSocket } from 'ws';
import { JanusSession, JanusError } from '../page/janus-client.mjs';
import { startMockJanus } from './helpers/mock-janus-ws.mjs';

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

async function setup(opts = {}, sessionOpts = {}) {
  const server = await startMockJanus(opts);
  const closes = [];
  const session = new JanusSession({
    url: server.url, token: 'tok-1', WebSocketImpl: WebSocket, keepaliveMs: 60000,
    requestTimeoutMs: 1000, onClose: (i) => closes.push(i), ...sessionOpts,
  });
  return { server, session, closes };
}

async function teardown({ server, session }) {
  session.close();
  await server.close();
}

test('connect: janus-protocol subprotocol, create, attach', async () => {
  const ctx = await setup();
  try {
    await ctx.session.connect();
    assert.deepEqual(ctx.server.protocols, ['janus-protocol']);
    assert.ok(ctx.session.sessionId);
    assert.ok(ctx.session.openedAt <= ctx.session.createdAt);
    const h = await ctx.session.attach('janus.plugin.videoroom');
    assert.ok(h.id);
    assert.equal(ctx.server.of('attach')[0].plugin, 'janus.plugin.videoroom');
  } finally { await teardown(ctx); }
});

test('transactions are random and at least 64 bit', async () => {
  const ctx = await setup();
  try {
    await ctx.session.connect();
    await ctx.session.attach('janus.plugin.videoroom');
    const txs = ctx.server.requests.map((r) => r.transaction);
    assert.equal(new Set(txs).size, txs.length);
    for (const t of txs) assert.match(t, /^[0-9a-f]{16,}$/);
  } finally { await teardown(ctx); }
});

test('message: resolves on the event after the ack, with data and jsep', async () => {
  const jsep = { type: 'answer', sdp: 'v=0\r\n' };
  const ctx = await setup({ plugin: () => ({ event: { videoroom: 'joined', id: 'x' }, jsep }), ackDelayMs: 20 });
  try {
    await ctx.session.connect();
    const h = await ctx.session.attach('janus.plugin.videoroom');
    const r = await h.message({ request: 'join' }, { jsep: { type: 'offer', sdp: 'o' } });
    assert.deepEqual(r.data, { videoroom: 'joined', id: 'x' });
    assert.deepEqual(r.jsep, jsep);
    const sent = ctx.server.of('message')[0];
    assert.deepEqual(sent.body, { request: 'join' });
    assert.deepEqual(sent.jsep, { type: 'offer', sdp: 'o' });
    assert.equal(sent.handle_id, h.id);
  } finally { await teardown(ctx); }
});

test('message: a synchronous success carrying plugindata resolves too', async () => {
  const ctx = await setup({ plugin: () => ({ sync: { videoroom: 'success', list: [] } }) });
  try {
    await ctx.session.connect();
    const h = await ctx.session.attach('janus.plugin.videoroom');
    const r = await h.message({ request: 'list' });
    assert.deepEqual(r.data, { videoroom: 'success', list: [] });
    assert.equal(r.jsep, null);
  } finally { await teardown(ctx); }
});

test('message: janus error frames and plugin error_code map to JanusError', async () => {
  let mode = 'frame';
  const ctx = await setup({
    plugin: () => (mode === 'frame'
      ? { error: { code: 458, reason: 'No such session' } }
      : { event: { videoroom: 'event', error_code: 426, error: 'No such room' } }),
  });
  try {
    await ctx.session.connect();
    const h = await ctx.session.attach('janus.plugin.videoroom');
    await assert.rejects(h.message({ request: 'join' }), (e) => e instanceof JanusError && e.code === 458 && /No such session/.test(e.message));
    mode = 'plugin';
    await assert.rejects(h.message({ request: 'join' }), (e) => e instanceof JanusError && e.code === 426 && e.plugin === true && /No such room/.test(e.message));
  } finally { await teardown(ctx); }
});

test('token: the root token is on EVERY request and setToken always wins', async () => {
  const ctx = await setup();
  try {
    await ctx.session.connect();
    const h = await ctx.session.attach('janus.plugin.videoroom');
    await h.message({ request: 'join' });
    await h.trickle({ candidate: 'candidate:1 1 udp 1 127.0.0.1 9 typ host', sdpMid: '0', sdpMLineIndex: 0 });
    await h.trickle(null);
    ctx.session.setToken('tok-2');
    await h.message({ request: 'publish' });
    await h.trickle(null);
    await h.detach();
    await ctx.session.destroy();
    const kinds = ctx.server.requests.map((r) => `${r.janus}:${r.token}`);
    assert.deepEqual(kinds, [
      'create:tok-1', 'attach:tok-1', 'message:tok-1', 'trickle:tok-1', 'trickle:tok-1',
      'message:tok-2', 'trickle:tok-2', 'detach:tok-2', 'destroy:tok-2',
    ]);
    // candidate shapes
    const tr = ctx.server.of('trickle');
    assert.deepEqual(Object.keys(tr[0].candidate).sort(), ['candidate', 'sdpMLineIndex', 'sdpMid']);
    assert.deepEqual(tr[1].candidate, { completed: true });
    for (const r of ctx.server.requests.filter((x) => x.janus !== 'create')) assert.equal(r.session_id, ctx.session.sessionId);
  } finally { await teardown(ctx); }
});

test('token refresh: a rejected token becomes valid after setToken', async () => {
  let valid = 'fresh';
  const ctx = await setup({ tokenValidator: (t) => t === valid }, { token: 'stale' });
  try {
    await assert.rejects(ctx.session.connect(), (e) => e.code === 403);
    ctx.session.close();
  } finally { await ctx.server.close(); }
  const ctx2 = await setup({ tokenValidator: (t) => t === valid }, { token: 'fresh' });
  try {
    await ctx2.session.connect();
    const h = await ctx2.session.attach('janus.plugin.videoroom');
    valid = 'fresher';
    await assert.rejects(h.message({ request: 'join' }), (e) => e.code === 403);
    ctx2.session.setToken('fresher');
    await h.message({ request: 'join' });
  } finally { await teardown(ctx2); }
});

test('keepalive: sent on the interval with the latest token; failures are reported', async () => {
  const errors = [];
  let ok = true;
  const ctx = await setup({ tokenValidator: (t, req) => (req.janus === 'keepalive' ? ok : true) },
    { keepaliveMs: 40, onKeepaliveError: (e, n) => errors.push([e.code, n]) });
  const until = async (cond, ms = 5000) => {
    const end = Date.now() + ms;
    while (!cond() && Date.now() < end) await sleep(10);
    assert.ok(cond(), 'condition not reached in time');
  };
  try {
    await ctx.session.connect();
    await until(() => ctx.server.of('keepalive').length >= 3);
    ctx.session.setToken('tok-9');
    const seen = ctx.server.of('keepalive').length;
    await until(() => ctx.server.of('keepalive').length >= seen + 2);
    const ka = ctx.server.of('keepalive');
    assert.equal(ka[ka.length - 1].token, 'tok-9');
    assert.deepEqual(errors, []);
    ok = false;
    await until(() => errors.length >= 2);
    assert.equal(errors[0][0], 403);
    assert.deepEqual(errors.map((x) => x[1]).slice(0, 2), [1, 2]);
  } finally { await teardown(ctx); }
});

test('timeout: an unanswered request rejects with code timeout', async () => {
  const ctx = await setup({ plugin: () => ({ silent: true }) }, { requestTimeoutMs: 120 });
  try {
    await ctx.session.connect({ timeoutMs: 5000 }); // the short request timeout below must not race the WS handshake on a loaded machine
    const h = await ctx.session.attach('janus.plugin.videoroom');
    const t0 = Date.now();
    await assert.rejects(h.message({ request: 'join' }), (e) => e instanceof JanusError && e.code === 'timeout');
    assert.ok(Date.now() - t0 >= 100);
    // per-request override
    await assert.rejects(h.message({ request: 'join' }, { timeoutMs: 30 }), (e) => e.code === 'timeout');
    // a stale reply for a timed out transaction is ignored, the session keeps working
    ctx.server.plugin = () => ({ event: { ok: 1 } });
    assert.deepEqual((await h.message({ request: 'x' })).data, { ok: 1 });
  } finally { await teardown(ctx); }
});

test('websocket close rejects pending requests and calls onClose once', async () => {
  const ctx = await setup({ plugin: () => ({ silent: true }) }, { requestTimeoutMs: 5000 });
  try {
    await ctx.session.connect();
    const h = await ctx.session.attach('janus.plugin.videoroom');
    const pending = h.message({ request: 'join' });
    await sleep(30);
    ctx.server.dropAll();
    await assert.rejects(pending, (e) => e instanceof JanusError && e.code === 'ws_closed');
    await sleep(30);
    assert.equal(ctx.closes.length, 1);
    assert.equal(ctx.closes[0].code, 'ws_closed');
    await assert.rejects(h.message({ request: 'x' }), (e) => e.code === 'detached' || e.code === 'ws_closed');
    assert.ok(ctx.session.closed);
  } finally { await teardown(ctx); }
});

test('an intentional destroy/close does not fire onClose', async () => {
  const ctx = await setup();
  try {
    await ctx.session.connect();
    await ctx.session.destroy();
    await sleep(30);
    assert.equal(ctx.closes.length, 0);
    assert.equal(ctx.server.of('destroy').length, 1);
  } finally { await teardown(ctx); }
});

test('connect to a closed port rejects with ws_error and no onClose', async () => {
  const server = await startMockJanus();
  const url = server.url;
  await server.close();
  const closes = [];
  const session = new JanusSession({ url, WebSocketImpl: WebSocket, onClose: (i) => closes.push(i), requestTimeoutMs: 500 });
  await assert.rejects(session.connect(), (e) => e instanceof JanusError && e.code === 'ws_error');
  await sleep(20);
  assert.equal(closes.length, 0);
});

test('unsolicited frames reach the handle listeners; a settled transaction does not resolve twice', async () => {
  const ctx = await setup({ plugin: () => ({ event: { videoroom: 'updating' } }) });
  try {
    await ctx.session.connect();
    const h = await ctx.session.attach('janus.plugin.videoroom');
    const seen = [];
    h.on('event', (e) => seen.push(['event', e.data, e.jsep]));
    h.on('trickle', (c) => seen.push(['trickle', c]));
    h.on('webrtcup', () => seen.push(['webrtcup']));
    h.on('media', (m) => seen.push(['media', m.type, m.receiving]));
    h.on('slowlink', (m) => seen.push(['slowlink', m.uplink, m.nacks]));
    h.on('hangup', (m) => seen.push(['hangup', m.reason]));
    h.on('detached', () => seen.push(['detached']));
    // The plugin answers `updating` (resolves the request); the real offer follows unsolicited.
    const r = await h.message({ request: 'subscribe' });
    assert.equal(r.data.videoroom, 'updating');
    ctx.server.push(h.id, { videoroom: 'updated', streams: [] }, { jsep: { type: 'offer', sdp: 'O' } });
    ctx.server.pushRaw(h.id, { janus: 'trickle', candidate: { completed: true } });
    ctx.server.pushRaw(h.id, { janus: 'webrtcup' });
    ctx.server.pushRaw(h.id, { janus: 'media', type: 'audio', receiving: true });
    ctx.server.pushRaw(h.id, { janus: 'slowlink', uplink: true, nacks: 7 });
    ctx.server.pushRaw(h.id, { janus: 'hangup', reason: 'DTLS alert' });
    await sleep(80);
    assert.deepEqual(seen, [
      ['event', { videoroom: 'updated', streams: [] }, { type: 'offer', sdp: 'O' }],
      ['trickle', { completed: true }],
      ['webrtcup'],
      ['media', 'audio', true],
      ['slowlink', true, 7],
      ['hangup', 'DTLS alert'],
    ]);
    ctx.server.pushRaw(h.id, { janus: 'detached' });
    await sleep(40);
    assert.deepEqual(seen.at(-1), ['detached']);
    assert.ok(h.detached);
    await assert.rejects(h.message({ request: 'x' }), (e) => e.code === 'detached');
  } finally { await teardown(ctx); }
});

test('a janus session timeout event closes the session with session_timeout', async () => {
  const ctx = await setup();
  try {
    await ctx.session.connect();
    const h = await ctx.session.attach('janus.plugin.videoroom');
    ctx.server.pushRaw(h.id, { janus: 'timeout', sender: undefined });
    await sleep(60);
    assert.equal(ctx.closes.length, 1);
    assert.equal(ctx.closes[0].code, 'session_timeout');
  } finally { await teardown(ctx); }
});

test('the client never logs the url or token', async () => {
  const logs = [];
  const ctx = await setup({ tokenValidator: () => false }, { log: (m) => logs.push(m), token: 'SECRET-TOKEN-VALUE' });
  try {
    await assert.rejects(ctx.session.connect());
    assert.ok(!logs.join('\n').includes('SECRET-TOKEN-VALUE'));
    assert.ok(!logs.join('\n').includes('127.0.0.1'));
  } finally { await teardown(ctx); }
});
