// API conformance test: exercises exactly the server-facing calls of GROUP_CALLS_V2 section 3 over the
// node-local HTTP transport, and the client-facing calls of section 4 over the WebSocket transport, against
// the real (installed) qjanus, and asserts behaviour, not just shapes. Every request/response that the
// server implementation depends on is printed at the end (SHAPES) with the secrets redacted.
//   QJANUS_HTTP           http://127.0.0.1:8088/janus
//   QJANUS_WS             ws://127.0.0.1:8188
//   QJANUS_TOKEN_SECRET   token_auth_secret          QJANUS_ADMIN_KEY   VideoRoom admin_key
//   QJANUS_FINGERPRINT    "sha-256 AB:CD:..." of the node's DTLS certificate
//   QJANUS_SLOW=0         skip the check that waits out the 20 s session reclaim window
//   SECRETS_FILE          where to write the secrets the test handed to Janus (for the log-hygiene check)
import assert from 'node:assert/strict';
import fs from 'node:fs';
import net from 'node:net';
import path from 'node:path';
import { HttpApi, ServerApi, WsClient, mintToken, hex, sleep, pluginData, remember, secretsSeen, PLUGIN } from '../lib/janus.mjs';
import { syntheticOffer } from '../lib/sdp.mjs';

const HTTP = process.env.QJANUS_HTTP || 'http://127.0.0.1:8088/janus';
const WS = process.env.QJANUS_WS || 'ws://127.0.0.1:8188';
const TOKEN_SECRET = process.env.QJANUS_TOKEN_SECRET;
const ADMIN_KEY = process.env.QJANUS_ADMIN_KEY;
const FINGERPRINT = process.env.QJANUS_FINGERPRINT;
const SLOW = process.env.QJANUS_SLOW !== '0';
const OUT = process.env.OUT_DIR || './conformance-out';
if (!TOKEN_SECRET || !ADMIN_KEY) throw new Error('QJANUS_TOKEN_SECRET and QJANUS_ADMIN_KEY are required');
fs.mkdirSync(OUT, { recursive: true });

const http = new HttpApi(HTTP, { tokenSecret: TOKEN_SECRET, adminKey: ADMIN_KEY });
const api = new ServerApi(http);
const results = [];
const shapes = {};
const clients = [];

// ------------------------------------------------------------------------------------------ runner
async function check(name, fn) {
  const t0 = Date.now();
  try {
    await fn();
    results.push({ name, ok: true });
    console.log(`PASS  ${name} (${Date.now() - t0} ms)`);
  } catch (e) {
    results.push({ name, ok: false, error: String((e && e.message) || e) });
    console.log(`FAIL  ${name}\n      ${String((e && e.message) || e).split('\n').join('\n      ')}`);
  }
}
const redact = (o) => JSON.parse(JSON.stringify(o, (k, v) =>
  (['token', 'admin_key', 'secret'].includes(k) && typeof v === 'string') ? `<${k}>` : v));
const shape = (name, req, resp) => { shapes[name] = { request: redact(req), response: redact(resp) }; };
const codeOf = (m) => (m.error && m.error.code) || pluginData(m).error_code || 0;
async function ws(opts) {
  const c = await new WsClient(WS, { tokenSecret: TOKEN_SECRET, ...opts }).open();
  clients.push(c);
  return c;
}

// one participant of a test room: WebSocket session + publisher handle
async function joinAs(room, id, joinToken) {
  const c = await ws();
  await c.create();
  const handle = await c.attach();
  const m = await c.message(handle, { request: 'join', ptype: 'publisher', room, id, display: id, token: joinToken });
  return { c, handle, m, data: pluginData(m) };
}

// ------------------------------------------------------------------------------------------ state
const room = hex(16);           // 32 hex, random, not the call id
const roomSecret = hex(32);
const idA = hex(16); const idB = hex(16); const idC = hex(16);   // pseudonyms
const tokA = hex(16); const tokB = hex(16); const tokC = hex(16); // join tokens
remember(room, roomSecret, idA, idB, idC, tokA, tokB, tokC);
const S = {};

// ============================================================================== core, tokens, info
await check('info answers without a token: version, VideoRoom + HTTP + WebSockets only, hardened core, nothing identifying', async () => {
  const { status, json } = await http.info();
  shape('info', { method: 'GET', path: '/janus/info' }, json);
  assert.equal(status, 200);
  assert.equal(json.janus, 'server_info');
  assert.equal(json.version_string, '1.4.2');
  assert.deepEqual(Object.keys(json.plugins).sort(), [PLUGIN]);
  assert.deepEqual(Object.keys(json.transports).sort(), ['janus.transport.http', 'janus.transport.websockets']);
  assert.deepEqual(Object.keys(json.events), []);
  assert.equal(json.data_channels, false);
  assert.equal(json['ice-lite'], true);
  assert.equal(json['ice-tcp'], false);
  assert.equal(json.ipv6, true);
  assert.equal(json['dtls-mtu'], 1200);
  assert.equal(json['session-timeout'], 60);
  assert.equal(json['reclaim-session-timeout'], 20);
  assert.equal(json['min-nack-queue'], 500);
  assert.equal(json['twcc-period'], 200);
  assert.equal(json.auth_token, true);
  assert.equal(json.api_secret, false);
  assert.equal(json.event_handlers, false);
  for (const k of ['dependencies', 'local-ip', 'public-ip', 'public-ips', 'commit-hash', 'compile-time']) {
    assert.ok(!(k in json), `info must not expose ${k}`);
  }
});

await check('ping answers pong without a token (health probe)', async () => {
  const { json } = await http.ping();
  shape('ping', { janus: 'ping' }, json);
  assert.equal(json.janus, 'pong');
});

await check('session creation needs a valid signed token: none / wrong secret / expired / wrong realm are refused with 403', async () => {
  for (const [what, token] of [
    ['none', undefined],
    ['wrong secret', mintToken('0'.repeat(64))],
    ['expired', mintToken(TOKEN_SECRET, { ttl: -5 })],
    ['wrong realm', mintToken(TOKEN_SECRET, { realm: 'other' })],
    ['garbage', 'not-a-token'],
  ]) {
    const { json } = await http.post('', { janus: 'create', ...(token ? { token } : {}) });
    assert.equal(json.janus, 'error', `${what}: expected an error, got ${JSON.stringify(json)}`);
    assert.equal(json.error.code, 403, what);
  }
});

await check('a token without the VideoRoom descriptor creates a session but cannot attach the plugin', async () => {
  const token = mintToken(TOKEN_SECRET, { plugins: [] });
  const sid = await http.createSession(token);
  const { json } = await http.post(`/${sid}`, { janus: 'attach', plugin: PLUGIN, token });
  shape('attach_without_plugin_descriptor', { janus: 'attach', plugin: PLUGIN }, json);
  assert.equal(json.janus, 'error');
  assert.ok([403, 405].includes(json.error.code), `unexpected code ${json.error.code}`);
  await http.destroySession(sid, http.token());
});

await check('session: create, attach VideoRoom, keepalive; the response shapes', async () => {
  const token = http.token();
  const c = await http.post('', { janus: 'create', token });
  shape('create_session', { janus: 'create', token }, c.json);
  assert.equal(c.json.janus, 'success');
  const sid = c.json.data.id;
  assert.equal(typeof sid, 'number');
  const a = await http.post(`/${sid}`, { janus: 'attach', plugin: PLUGIN, token });
  shape('attach', { janus: 'attach', plugin: PLUGIN, token }, a.json);
  assert.equal(a.json.janus, 'success');
  const ka = await http.post(`/${sid}`, { janus: 'keepalive', token });
  shape('keepalive', { janus: 'keepalive', token }, ka.json);
  assert.equal(ka.json.janus, 'ack');
  const d = await http.destroySession(sid, token);
  shape('destroy_session', { janus: 'destroy', token }, d.json);
  assert.equal(d.json.janus, 'success');
  const again = await http.post(`/${sid}`, { janus: 'keepalive', token });
  assert.equal(again.json.error.code, 458, 'a destroyed session is gone');
});

await check('the token is validated on EVERY request (not just at create): expiry mid-session, and a refreshed token works on the same session', async () => {
  const shortLived = mintToken(TOKEN_SECRET, { ttl: 3 });
  const sid = await http.createSession(shortLived);
  const ok = await http.post(`/${sid}`, { janus: 'keepalive', token: shortLived });
  assert.equal(ok.json.janus, 'ack');
  await sleep(5000);
  const expired = await http.post(`/${sid}`, { janus: 'keepalive', token: shortLived });
  shape('request_with_expired_token', { janus: 'keepalive', token: 'expired' }, expired.json);
  assert.equal(expired.json.janus, 'error');
  assert.equal(expired.json.error.code, 403);
  const fresh = await http.post(`/${sid}`, { janus: 'keepalive', token: http.token() });
  assert.equal(fresh.json.janus, 'ack', 'a fresh token continues the same session');
  await http.destroySession(sid, http.token());
});

await check('no Admin API and no api_secret: /admin and the Admin API ports are not served', async () => {
  const base = new URL(HTTP);
  const r = await fetch(`${base.origin}/admin/info`).catch((e) => ({ status: 0, err: e }));
  assert.ok(r.status === 404 || r.status === 0, `/admin answered ${r.status}`);
  for (const port of [7088, 7188]) {
    const open = await new Promise((resolve) => {
      const s = net.connect({ host: base.hostname, port, timeout: 1500 });
      s.on('connect', () => { s.destroy(); resolve(true); });
      s.on('error', () => resolve(false));
      s.on('timeout', () => { s.destroy(); resolve(false); });
    });
    assert.equal(open, false, `port ${port} must be closed`);
  }
});

// ================================================================================== room lifecycle
await check('create a room with ALL spec parameters (admin_key + room secret + allowed tokens), string room id', async () => {
  const req = {
    request: 'create', room, is_private: true, secret: roomSecret, publishers: 8, bitrate: 1500000, fir_freq: 10,
    audiocodec: 'opus', videocodec: 'vp8', opus_fec: true, opus_dtx: false, audiolevel_ext: false,
    audiolevel_event: false, videoorient_ext: false, playoutdelay_ext: false, transport_wide_cc_ext: true,
    record: false, lock_record: true, require_pvtid: true, require_e2ee: true, notify_joining: false, allowed: [tokA],
  };
  await api.session(async ({ vr, sid, handle }) => {
    const { json } = await http.message(sid, handle, http.token(), { ...req, admin_key: ADMIN_KEY });
    shape('videoroom_create', req, json);
    assert.equal(json.janus, 'success');
    const d = pluginData(json);
    assert.equal(d.videoroom, 'created');
    assert.equal(d.room, room);
    assert.equal(d.permanent, false);
    S.created = d;
    // duplicates are refused
    const dup = await vr({ ...req }, { admin: true });
    assert.equal(dup.error_code, 427, JSON.stringify(dup));
  });
});

await check('create needs admin_key: a missing key is refused (429), a wrong key is unauthorized (433)', async () => {
  await api.session(async ({ vr }) => {
    const base = { request: 'create', room: hex(16), secret: hex(8), publishers: 2 };
    const none = await vr(base);
    shape('videoroom_create_without_admin_key', base, none);
    assert.equal(none.error_code, 429, JSON.stringify(none));
    const wrong = await vr({ ...base, admin_key: hex(32) });
    shape('videoroom_create_wrong_admin_key', { ...base, admin_key: '<wrong>' }, wrong);
    assert.equal(wrong.error_code, 433, JSON.stringify(wrong));
  });
});

await check('list: the private room is hidden without admin_key; with it every parameter is reflected', async () => {
  await api.session(async ({ vr }) => {
    const hidden = await vr({ request: 'list' });
    assert.ok(!hidden.list.some((r) => r.room === room), 'a private room must not be listed without admin_key');
    const shown = await vr({ request: 'list' }, { admin: true });
    shape('videoroom_list', { request: 'list', admin_key: '<admin_key>' }, shown);
    const r = shown.list.find((x) => x.room === room);
    assert.ok(r, 'room listed with admin_key');
    assert.equal(r.max_publishers, 8);
    assert.equal(r.bitrate, 1500000);
    assert.equal(r.fir_freq, 10);
    assert.equal(r.audiocodec, 'opus');
    assert.equal(r.videocodec, 'vp8');
    assert.equal(r.opus_fec, true);
    assert.ok(!r.opus_dtx, 'DTX off');
    assert.equal(r.audiolevel_ext, false);
    assert.equal(r.audiolevel_event, false);
    assert.equal(r.videoorient_ext, false);
    assert.equal(r.playoutdelay_ext, false);
    assert.equal(r.transport_wide_cc_ext, true);
    assert.equal(r.record, false);
    assert.equal(r.lock_record, true);
    assert.equal(r.require_pvtid, true);
    assert.equal(r.require_e2ee, true);
    assert.equal(r.notify_joining, false);
    assert.equal(r.is_private, true);
    assert.equal(r.pin_required, false);
    assert.equal(r.num_participants, 0);
    assert.ok(!('secret' in r) && !('allowed' in r), 'the secret and the allowed list never appear in a list');
  });
});

await check('exists: true for the room, false for another id', async () => {
  const yes = await api.exists(room);
  shape('videoroom_exists', { request: 'exists', room }, yes);
  assert.equal(yes.exists, true);
  assert.equal((await api.exists(hex(16))).exists, false);
});

await check('allowed add / remove needs the room secret (429 missing, 433 wrong); the response lists the tokens', async () => {
  await api.session(async ({ vr }) => {
    const noSecret = await vr({ request: 'allowed', room, action: 'add', allowed: [tokB] });
    assert.equal(noSecret.error_code, 429, JSON.stringify(noSecret));
    const wrongSecret = await vr({ request: 'allowed', room, secret: hex(32), action: 'add', allowed: [tokB] });
    assert.equal(wrongSecret.error_code, 433);
    const req = { request: 'allowed', room, secret: roomSecret, action: 'add', allowed: [tokB, tokC] };
    const added = await vr(req);
    shape('videoroom_allowed_add', req, added);
    assert.equal(added.videoroom, 'success');
    assert.deepEqual([...added.allowed].sort(), [tokA, tokB, tokC].sort());
    const req2 = { request: 'allowed', room, secret: roomSecret, action: 'remove', allowed: [tokC] };
    const removed = await vr(req2);
    shape('videoroom_allowed_remove', req2, removed);
    assert.deepEqual([...removed.allowed].sort(), [tokA, tokB].sort());
  });
});

// ==================================================================================== the clients
await check('publisher join over WebSocket: id = pseudonym (string_ids), display, join token; joined carries private_id and the publishers', async () => {
  const a = await joinAs(room, idA, tokA);
  shape('publisher_join', { request: 'join', ptype: 'publisher', room, id: '<pseudonym>', display: '<pseudonym>', token: '<join_token>' }, a.m);
  assert.equal(a.m.janus, 'event');
  assert.equal(a.data.videoroom, 'joined');
  assert.equal(a.data.room, room);
  assert.equal(a.data.id, idA, 'the participant id is the pseudonym');
  assert.equal(typeof a.data.private_id, 'number');
  assert.deepEqual(a.data.publishers, []);
  S.a = a;
});

await check('join is refused without a valid join token (433), for a token removed from allowed, and for a duplicate id (436)', async () => {
  const noTok = await joinAs(room, hex(16), undefined);
  shape('join_without_token', { request: 'join', ptype: 'publisher', room }, noTok.m);
  assert.equal(codeOf(noTok.m), 433, JSON.stringify(noTok.m));
  const badTok = await joinAs(room, hex(16), hex(16));
  assert.equal(codeOf(badTok.m), 433);
  const dupId = await joinAs(room, idA, tokB);
  shape('join_duplicate_id', { request: 'join', ptype: 'publisher', room, id: '<pseudonym>' }, dupId.m);
  assert.equal(codeOf(dupId.m), 436, JSON.stringify(dupId.m));
  const noRoom = await joinAs(hex(16), hex(16), tokB);
  shape('join_unknown_room', { request: 'join', ptype: 'publisher', room: '<unknown>' }, noRoom.m);
  assert.equal(codeOf(noRoom.m), 426, JSON.stringify(noRoom.m));
  [noTok, badTok, dupId, noRoom].forEach((x) => x.c.close());
});

await check('publish enforces require_e2ee in the JSEP: an offer without e2ee is refused (and that handle cannot publish again: use a new one)', async () => {
  await api.allow(room, roomSecret, 'add', [tokC]);
  const x = await joinAs(room, idC, tokC);
  assert.equal(x.data.videoroom, 'joined');
  const plain = await x.c.message(x.handle, { request: 'publish', audio: true, video: true }, { type: 'offer', sdp: syntheticOffer() });
  shape('publish_without_e2ee', { request: 'publish' }, plain);
  assert.equal(codeOf(plain), 433, JSON.stringify(plain));
  assert.match(pluginData(plain).error, /end-to-end/i);
  const retry = await x.c.message(x.handle, { request: 'publish', audio: true, video: true }, { type: 'offer', sdp: syntheticOffer(), e2ee: true });
  shape('publish_after_refused_publish', { request: 'publish' }, retry);
  assert.ok(codeOf(retry) > 0, 'a handle whose publish was refused cannot publish again (Janus: 490 "Error setting ICE locally" or 434): clients attach a new handle');
  x.c.close();
});

await check('publish with e2ee: the answer carries e2ee, the pinned node fingerprint, DTLS client role, ICE lite and no unwanted RTP extensions', async () => {
  const { c, handle } = S.a;
  const offer = syntheticOffer({ withBadExtmaps: true });
  const body = { request: 'publish', audio: true, video: true, descriptions: [{ mid: '0', description: 'mic' }, { mid: '1', description: 'camera' }] };
  const ok = await c.message(handle, body, { type: 'offer', sdp: offer, e2ee: true });
  shape('publish_with_e2ee', { ...body, jsep: '<offer, e2ee:true>' }, { ...ok, jsep: ok.jsep ? { type: ok.jsep.type, e2ee: ok.jsep.e2ee, sdp: '<answer>' } : undefined });
  assert.equal(ok.janus, 'event');
  assert.equal(pluginData(ok).configured, 'ok');
  assert.equal(ok.jsep.type, 'answer');
  assert.equal(ok.jsep.e2ee, true, 'the answer is flagged e2ee');
  const sdp = ok.jsep.sdp;
  S.answerSdp = sdp;
  assert.match(sdp, /a=ice-lite/);
  assert.match(sdp, /a=setup:active/, 'Janus is the DTLS client for a publisher offer');
  assert.match(sdp, /a=rtpmap:111 opus\/48000\/2/);
  assert.match(sdp, /a=rtpmap:96 VP8\/90000/);
  assert.match(sdp, /a=fmtp:111 [^\r\n]*useinbandfec=1/);
  assert.ok(!/usedtx=1/.test(sdp), 'DTX must not be negotiated');
  assert.ok(!/ssrc-audio-level/.test(sdp), 'the audio-level extension must not be negotiated (audiolevel_ext=false)');
  assert.ok(!/video-orientation/.test(sdp), 'video-orientation must not be negotiated');
  assert.ok(!/playout-delay/.test(sdp), 'playout-delay must not be negotiated');
  assert.match(sdp, /transport-wide-cc-extensions/, 'transport-wide-cc is kept');
  assert.match(sdp, /a=extmap:\d+ urn:ietf:params:rtp-hdrext:sdes:mid/, 'mid is kept');
  const fp = /a=fingerprint:(\S+ [0-9A-F:]+)/i.exec(sdp);
  assert.ok(fp, 'the answer carries a=fingerprint');
  if (FINGERPRINT) assert.equal(fp[1].toLowerCase(), FINGERPRINT.toLowerCase(), 'the fingerprint in the SDP is the pinned node certificate');
  assert.match(fp[1], /^sha-256 /i);
});

await check('listparticipants (server API): id, display, publisher flag; and the publisher cap (432)', async () => {
  const list = await api.listParticipants(room);
  shape('videoroom_listparticipants', { request: 'listparticipants', room }, list);
  assert.equal(list.videoroom, 'participants');
  assert.equal(list.room, room);
  const me = list.participants.find((p) => p.id === idA);
  assert.ok(me, 'the joined pseudonym is listed');
  assert.equal(me.display, idA);
  assert.equal(typeof me.publisher, 'boolean');
  // a room of 1 publisher: the second publish is refused
  const r1 = hex(16); const s1 = hex(16); const t1 = hex(16); const t2 = hex(16);
  remember(r1, s1, t1, t2);
  await api.session(async ({ vr }) => {
    const c = await vr({ request: 'create', room: r1, secret: s1, publishers: 1, require_e2ee: true, allowed: [t1, t2], is_private: true }, { admin: true });
    assert.equal(c.videoroom, 'created');
  });
  const p1 = await joinAs(r1, hex(16), t1); const p2 = await joinAs(r1, hex(16), t2);
  assert.equal(p1.data.videoroom, 'joined');
  assert.equal(p2.data.videoroom, 'joined');
  const first = await p1.c.message(p1.handle, { request: 'publish', audio: true, video: true }, { type: 'offer', sdp: syntheticOffer(), e2ee: true });
  assert.equal(pluginData(first).configured, 'ok', JSON.stringify(first).slice(0, 300));
  // "starting" already counts against the cap, before any media flows
  const second = await p2.c.message(p2.handle, { request: 'publish', audio: true, video: true }, { type: 'offer', sdp: syntheticOffer(), e2ee: true });
  shape('publish_publishers_full', { request: 'publish' }, second);
  assert.equal(codeOf(second), 432, JSON.stringify(second).slice(0, 300));
  p1.c.close(); p2.c.close();
  await api.destroyRoom(r1, s1);
});

await check('subscriber join: an unknown or not yet publishing feed is refused (428)', async () => {
  const b = await joinAs(room, idB, tokB);
  assert.equal(b.data.videoroom, 'joined');
  S.b = b;
  const sub = await ws();
  await sub.create();
  const h = await sub.attach();
  const req = { request: 'join', ptype: 'subscriber', room, private_id: b.data.private_id, token: tokB, streams: [{ feed: idA }] };
  const m = await sub.message(h, req);
  shape('subscriber_join_feed_not_started', req, m);
  assert.equal(codeOf(m), 428, JSON.stringify(m));
  const m2 = await sub.message(h, { ...req, streams: [{ feed: hex(16) }] });
  assert.equal(codeOf(m2), 428);
  sub.close();
});

await check('rtp_forward is for the server only (lock_rtp_forward + admin_key): refused without the key (429) and with a wrong one (433), from a client session as over HTTP; the room secret is no substitute', async () => {
  const req = { request: 'rtp_forward', room, publisher_id: idA, host: '127.0.0.1', audio_port: 5002, audio_pt: 111 };
  const c = await ws();
  await c.create();
  const h = await c.attach();
  const none = await c.message(h, req);
  shape('rtp_forward_without_admin_key', req, none);
  assert.equal(codeOf(none), 429, JSON.stringify(none));
  const wrong = await c.message(h, { ...req, admin_key: hex(32) });
  assert.equal(codeOf(wrong), 433, JSON.stringify(wrong));
  const secretOnly = await c.message(h, { ...req, secret: roomSecret });
  assert.equal(codeOf(secretOnly), 429, JSON.stringify(secretOnly));
  const viaHttp = await api.session(({ vr }) => vr(req));
  assert.equal(viaHttp.error_code, 429, JSON.stringify(viaHttp));
});

await check('every request that changes a room needs the room secret (429 missing, 433 wrong): edit, enable_recording, stop_rtp_forward, listforwarders - a client cannot turn recording on', async () => {
  const c = await ws();
  await c.create();
  const h = await c.attach();
  for (const body of [
    { request: 'edit', room, new_description: 'x' },
    { request: 'enable_recording', room, record: true },
    { request: 'stop_rtp_forward', room, publisher_id: idA, stream_id: 1 },
    { request: 'listforwarders', room },
  ]) {
    const none = await c.message(h, body);
    assert.equal(codeOf(none), 429, `${body.request} without the secret: ${JSON.stringify(none)}`);
    const wrong = await c.message(h, { ...body, secret: hex(32) });
    assert.equal(codeOf(wrong), 433, `${body.request} with a wrong secret: ${JSON.stringify(wrong)}`);
  }
  // and the room did not change
  const shown = await api.session(({ vr }) => vr({ request: 'list' }, { admin: true }));
  const r = shown.list.find((x) => x.room === room);
  assert.equal(r.record, false);
  assert.equal(r.lock_record, true);
});

await check('leave: the leaver gets leaving/ok, the others get leaving <id>; the id is free again', async () => {
  const idD = hex(16); const tokD = hex(16);
  remember(idD, tokD);
  await api.allow(room, roomSecret, 'add', [tokD]);
  const d = await joinAs(room, idD, tokD);
  assert.equal(d.data.videoroom, 'joined');
  const left = await d.c.message(d.handle, { request: 'leave' });
  shape('publisher_leave', { request: 'leave' }, left);
  assert.equal(pluginData(left).leaving, 'ok');
  const ev = await S.a.c.waitEvent((e) => pluginData(e).leaving === idD, 8000);
  shape('event_participant_left', {}, ev);
  assert.equal(pluginData(ev).room, room);
  d.c.close();
  const list = await api.listParticipants(room);
  assert.ok(!list.participants.some((p) => p.id === idD));
});

await check('kick: allowed remove + kick; the kicked handle gets leaving/kicked, the others get leaving, the id is gone, and the token cannot rejoin', async () => {
  const { c: cb, handle: hb, data: db } = S.b;
  const res = await api.session(async ({ vr }) => {
    const removed = await vr({ request: 'allowed', room, secret: roomSecret, action: 'remove', allowed: [tokB] });
    assert.ok(!removed.allowed.includes(tokB));
    const req = { request: 'kick', room, secret: roomSecret, id: idB };
    const kicked = await vr(req);
    shape('videoroom_kick', req, kicked);
    assert.equal(kicked.videoroom, 'success');
    // kicking twice / an unknown id
    const again = await vr(req);
    assert.equal(again.error_code, 428, JSON.stringify(again));
    const noSecret = await vr({ request: 'kick', room, id: idA });
    assert.equal(noSecret.error_code, 429);
    return kicked;
  });
  assert.equal(res.videoroom, 'success');
  const evB = await cb.waitEvent((e) => pluginData(e).leaving === 'ok' && pluginData(e).reason === 'kicked', 8000);
  shape('event_kicked', {}, evB);
  assert.equal(pluginData(evB).room, room);
  const evA = await S.a.c.waitEvent((e) => pluginData(e).kicked === idB, 8000);
  shape('event_participant_kicked', {}, evA);
  assert.equal(pluginData(evA).room, room);
  const list = await api.listParticipants(room);
  assert.ok(!list.participants.some((p) => p.id === idB), 'the kicked pseudonym is gone from the room');
  // cannot come back: new session, the same token, the same id
  const back = await joinAs(room, idB, tokB);
  shape('join_after_kick', { request: 'join', ptype: 'publisher', room, id: '<pseudonym>', token: '<removed join_token>' }, back.m);
  assert.equal(codeOf(back.m), 433, JSON.stringify(back.m));
  back.c.close();
  // the kicked handle cannot publish any more
  const pub = await cb.message(hb, { request: 'publish', audio: true, video: true }, { type: 'offer', sdp: syntheticOffer(), e2ee: true }).catch((e) => ({ refused: String(e) }));
  assert.ok(pub.refused || codeOf(pub) > 0, `a kicked handle must not publish: ${JSON.stringify(pub).slice(0, 200)}`);
  void db;
});

await check('destroy: needs the room secret (429 missing, 433 wrong); participants get destroyed; the room is gone (426 for late joiners)', async () => {
  const noSecret = await api.session(({ vr }) => vr({ request: 'destroy', room }));
  assert.equal(noSecret.error_code, 429, JSON.stringify(noSecret));
  const wrong = await api.session(({ vr }) => vr({ request: 'destroy', room, secret: hex(32) }));
  assert.equal(wrong.error_code, 433);
  const gone = await api.destroyRoom(room, roomSecret);
  shape('videoroom_destroy', { request: 'destroy', room, secret: '<secret>' }, gone);
  assert.equal(gone.videoroom, 'destroyed');
  assert.equal(gone.room, room);
  const ev = await S.a.c.waitEvent((e) => pluginData(e).videoroom === 'destroyed', 8000);
  shape('event_room_destroyed', {}, ev);
  assert.equal(pluginData(ev).room, room);
  assert.equal((await api.exists(room)).exists, false);
  const late = await joinAs(room, hex(16), tokA);
  assert.equal(codeOf(late.m), 426);
  late.c.close();
  // destroying twice
  const twice = await api.destroyRoom(room, roomSecret);
  assert.equal(twice.error_code, 426);
});

// ============================================================================ WebSocket transport
await check('WebSocket: create + attach + keepalive; info without a token; session bound to the connection', async () => {
  const c = await ws();
  const info = await c.info();
  assert.equal(info.janus, 'server_info');
  const sid = await c.create();
  assert.equal(typeof sid, 'number');
  const h = await c.attach();
  assert.equal(typeof h, 'number');
  const ka = await c.keepalive();
  assert.equal(ka.janus, 'ack');
  const bad = await new WsClient(WS, { token: mintToken('1'.repeat(64)) }).open();
  clients.push(bad);
  const refused = await bad.send({ janus: 'create' });
  assert.equal(refused.janus, 'error');
  assert.equal(refused.error.code, 403);
  const none = await new WsClient(WS, { token: null }).open();
  clients.push(none);
  const refused2 = await none.send({ janus: 'create' });
  assert.equal(refused2.error.code, 403);
});

await check('WebSocket: the session survives a dropped socket for reclaim_session_timeout (claim on a new socket), and is gone after 20 s', async () => {
  if (!SLOW) return;
  const c1 = await ws();
  const sid = await c1.create();
  await c1.attach();
  c1.close();
  await sleep(1500);
  const c2 = await ws();
  const claimed = await c2.claim(sid);
  shape('claim', { janus: 'claim', session_id: '<sid>', token: '<token>' }, claimed);
  assert.equal(claimed.janus, 'success', JSON.stringify(claimed));
  const ka = await c2.keepalive();
  assert.equal(ka.janus, 'ack');
  c2.close();
  await sleep(23000);   // > reclaim_session_timeout (20 s)
  const c3 = await ws();
  const late = await c3.claim(sid);
  shape('claim_too_late', { janus: 'claim', session_id: '<sid>' }, late);
  assert.equal(late.janus, 'error');
  assert.equal(late.error.code, 458);
});

await check('WebSocket: a fresh token can be presented mid-session; the old expired one is refused (per-request validation)', async () => {
  const short = mintToken(TOKEN_SECRET, { ttl: 3 });
  const c = await ws({ token: short });
  await c.create();
  assert.equal((await c.keepalive()).janus, 'ack');
  await sleep(5000);
  const expired = await c.keepalive();
  assert.equal(expired.janus, 'error');
  assert.equal(expired.error.code, 403);
  c.token = mintToken(TOKEN_SECRET);
  assert.equal((await c.keepalive()).janus, 'ack');
});

// ======================================================================= results and shapes
for (const c of clients) c.close();
fs.writeFileSync(path.join(OUT, 'results.json'), JSON.stringify(results, null, 1));
fs.writeFileSync(path.join(OUT, 'shapes.json'), JSON.stringify(shapes, null, 1));
if (process.env.SECRETS_FILE) fs.writeFileSync(process.env.SECRETS_FILE, JSON.stringify([...secretsSeen]), { mode: 0o600 });   // consumed by the log-hygiene check
const failed = results.filter((r) => !r.ok);
console.log(`\nconformance: ${results.length - failed.length}/${results.length} passed`);
console.log('=== SHAPES ===');
console.log(JSON.stringify(shapes, null, 1));
process.exit(failed.length ? 1 : 0);
