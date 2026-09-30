// Real-Chromium self-test of the in-page bot library against a FAKE SFU.
//
// The fake SFU is a Node WebSocket server that speaks just enough Janus VideoRoom; its media
// legs are terminated by a helper page (a second page in the same Chromium): it answers the
// bot's publisher offer and, for the bot's subscriber, produces the offer (sendonly fake-device
// tracks). The point of the test is that REAL Chromium accepts every SDP the bot produces
// (munging, simulcast, transforms) and that the bot reports what the contract promises.
//
// Skipped automatically when Chromium cannot be launched. Locally set
// PLAYWRIGHT_BROWSERS_PATH to the Playwright browser cache before running.
import { test, before, after } from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { startMockJanus } from './helpers/mock-janus-ws.mjs';
import { ALLOWED_EXTMAPS, getMLines, getOpusFmtp, getPtime, getSendRids } from '../page/sdp.mjs';

const PAGE_DIR = path.resolve(path.dirname(fileURLToPath(import.meta.url)), '..', 'page');
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const CHROMIUM_ARGS = [
  '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream', '--allow-loopback-in-peer-connection',
  '--disable-features=WebRtcHideLocalIpsWithMdns', '--autoplay-policy=no-user-gesture-required', '--no-sandbox',
  // Without this trial Chromium sends only 2 simulcast layers for a 640x360 capture (3 need >= 960x540).
  '--force-fieldtrials=WebRTC-LegacySimulcastLayerLimit/Disabled/',
];

let browser = null;
let skipReason = null;
try {
  const { chromium } = await import('playwright');
  browser = await chromium.launch({ headless: true, args: CHROMIUM_ARGS });
} catch (e) {
  skipReason = `Chromium is not launchable: ${String(e && e.message).split('\n')[0]}`;
}

// ------------------------------------------------------------------ helper page

// Media legs of the fake SFU. Everything is non-trickle on the helper side (full SDP after
// gathering); candidates of the bot arrive through addRemoteCandidate().
const HELPER_HTML = `<!doctype html>
<meta charset="utf-8"><title>helper</title>
<script type="module">
const pcs = new Map();
const queued = new Map();
let worker = null;
// The transform is attached only once the worker has evaluated its script (it posts 'ready').
function getWorker() {
  if (!worker) {
    const w = new Worker('/e2ee-worker.js', { type: 'module' });
    worker = {
      w,
      ready: new Promise((resolve) => {
        w.onmessage = (e) => {
          if (e.data && e.data.type === 'ready') resolve();
        };
      }),
    };
  }
  return worker;
}
function gathered(pc) {
  return new Promise((resolve) => {
    if (pc.iceGatheringState === 'complete') return resolve();
    const t = setTimeout(resolve, 8000);
    pc.addEventListener('icegatheringstatechange', () => {
      if (pc.iceGatheringState === 'complete') { clearTimeout(t); resolve(); }
    });
  });
}
async function flush(key) {
  const rec = pcs.get(key);
  for (const c of queued.get(key) || []) await rec.pc.addIceCandidate(c).catch(() => {});
  queued.delete(key);
}
async function addFeed(rec, feed) {
  // Chromium ignores a transform that is attached later than the task that created the sender, so
  // the (already loaded) worker is awaited first and the transform is set right after addTransceiver.
  if (feed.e2ee) await getWorker().ready;
  const stream = await navigator.mediaDevices.getUserMedia({ audio: true, video: feed.video ? { width: 320, height: 180 } : false });
  const out = { audio: rec.pc.addTransceiver(stream.getAudioTracks()[0], { direction: 'sendonly', streams: [stream] }) };
  if (feed.video) out.video = rec.pc.addTransceiver(stream.getVideoTracks()[0], { direction: 'sendonly', streams: [stream] });
  if (feed.e2ee) {
    for (const kind of Object.keys(out)) {
      out[kind].sender.transform = new RTCRtpScriptTransform(getWorker().w,
        { botId: 'helper-' + feed.id, kind, role: 'sender', mode: feed.e2ee.mode, keyHex: feed.e2ee.keyHex });
    }
  }
  return out;
}
function midsOf(list) {
  const m = {};
  for (const { feed, tr } of list) { m[feed.id] = { audio: tr.audio.mid, video: tr.video ? tr.video.mid : null }; }
  return m;
}
window.helper = {
  async answerPublisher(key, sdp) {
    const pc = new RTCPeerConnection({ bundlePolicy: 'max-bundle' });
    pcs.set(key, { pc });
    await pc.setRemoteDescription({ type: 'offer', sdp });
    await pc.setLocalDescription(await pc.createAnswer());
    await gathered(pc);
    await flush(key);
    return pc.localDescription.sdp;
  },
  async subCreate(key, feeds) {
    const pc = new RTCPeerConnection({ bundlePolicy: 'max-bundle' });
    const rec = { pc };
    pcs.set(key, rec);
    const list = [];
    for (const feed of feeds) list.push({ feed, tr: await addFeed(rec, feed) });
    await pc.setLocalDescription(await pc.createOffer());
    await gathered(pc);
    return { sdp: pc.localDescription.sdp, mids: midsOf(list) };
  },
  async subAddFeeds(key, feeds) {
    const rec = pcs.get(key);
    const list = [];
    for (const feed of feeds) list.push({ feed, tr: await addFeed(rec, feed) });
    await rec.pc.setLocalDescription(await rec.pc.createOffer());
    await gathered(rec.pc);
    return { sdp: rec.pc.localDescription.sdp, mids: midsOf(list) };
  },
  async subSetAnswer(key, sdp) {
    const rec = pcs.get(key);
    await rec.pc.setRemoteDescription({ type: 'answer', sdp });
    await flush(key);
  },
  async addRemoteCandidate(key, cand) {
    const rec = pcs.get(key);
    if (rec && rec.pc.remoteDescription) await rec.pc.addIceCandidate(cand).catch(() => {});
    else queued.set(key, (queued.get(key) || []).concat([cand]));
  },
  async inbound(key) {
    const rec = pcs.get(key);
    if (!rec) return [];
    const out = [];
    (await rec.pc.getStats()).forEach((r) => {
      if (r.type === 'inbound-rtp') out.push({ kind: r.kind, packets: r.packetsReceived, bytes: r.bytesReceived, t: r.timestamp });
    });
    return out;
  },
  close(key) {
    const rec = pcs.get(key);
    if (rec) { try { rec.pc.close(); } catch (e) {} pcs.delete(key); }
  },
};
window.helperReady = true;
</script>`;

// ------------------------------------------------------------------- fake SFU

/**
 * A Chromium answerer drops simulcast, Janus accepts it (a=rid:<id> recv + a=simulcast:recv). The
 * fake SFU therefore appends what Janus would answer to the video section, so that the bot's
 * sender really starts its layers. (Only the bot sees this description; the helper PC keeps its own.)
 */
function simulcastRecv(answerSdp, offerSdp) {
  const rids = getSendRids(offerSdp);
  if (!rids.length) return answerSdp;
  const lines = answerSdp.split('\r\n').filter((l) => l.length > 0);
  const start = lines.findIndex((l) => l.startsWith('m=video'));
  let end = lines.findIndex((l, i) => i > start && l.startsWith('m='));
  if (end < 0) end = lines.length;
  lines.splice(end, 0, ...rids.map((r) => `a=rid:${r} recv`), `a=simulcast:recv ${rids.join(';')}`);
  return `${lines.join('\r\n')}\r\n`;
}

class FakeSfu {
  constructor(helperPage) {
    this.helper = helperPage;
    this.rooms = new Map(); // room -> {peers: Map(id -> {video, e2ee}), pubHandles: Set}
    this.handles = new Map(); // handleId -> {role, key, room, pseudonym, streams, awaitingAnswer}
    this.validTokens = new Set();
    this.joinTokens = new Set();
    this.privateIds = new Map(); // private_id -> handleId
    this.pvt = 4000;
    this.log = { publishes: [], joins: [], subscribes: [], unsubscribes: [], configures: [], answers: [], leaves: [] };
    this.violations = [];
    this.mock = null;
  }

  async start() {
    this.mock = await startMockJanus({
      tokenValidator: (token) => this.validTokens.has(token),
      plugin: (ctx) => this._plugin(ctx),
      onTrickle: (handleId, cand) => {
        const h = this.handles.get(handleId);
        if (h && cand && !cand.completed) this._helper('addRemoteCandidate', h.key, cand).catch(() => {});
      },
    });
    return this.mock.url;
  }

  async close() { if (this.mock) await this.mock.close(); }

  _helper(fn, ...args) {
    return this.helper.evaluate(([f, a]) => window.helper[f](...a), [fn, args]);
  }

  /** Rooms come with the standard test credentials: one session token and one join token. */
  addRoom(room) {
    this.rooms.set(room, { peers: new Map(), pubHandles: new Set() });
    this.validTokens.add('SESSION-TOKEN-ONE');
    this.joinTokens.add('JOINTOKEN-SECRET-1');
  }

  static announce(id, peer) {
    const streams = [{ type: 'audio', mindex: 0, mid: '0', codec: 'opus' }];
    if (peer.video) streams.push({ type: 'video', mindex: 1, mid: '1', codec: 'vp8', simulcast: true });
    return { id, display: id, streams };
  }

  /** A fake remote publisher appears in the room (announced to every publisher handle). */
  addPeer(room, id, peer) {
    const r = this.rooms.get(room);
    r.peers.set(id, peer);
    for (const hid of r.pubHandles) this.mock.push(hid, { videoroom: 'event', room, publishers: [FakeSfu.announce(id, peer)] });
  }

  removePeer(room, id) {
    const r = this.rooms.get(room);
    r.peers.delete(id);
    for (const hid of r.pubHandles) this.mock.push(hid, { videoroom: 'event', room, leaving: id });
  }

  pubKeyOf(pseudonym) {
    for (const h of this.handles.values()) if (h.role === 'pub' && h.pseudonym === pseudonym) return h.key;
    return null;
  }

  static summary(mids, startIndex = 0) {
    const out = [];
    let i = startIndex;
    for (const [feed, m] of Object.entries(mids)) {
      out.push({ type: 'audio', mindex: i++, mid: m.audio, feed_id: feed, feed_mid: '0', active: true, ready: false, send: true, codec: 'opus' });
      if (m.video !== null && m.video !== undefined) {
        out.push({ type: 'video', mindex: i++, mid: m.video, feed_id: feed, feed_mid: '1', active: true, ready: false, send: true, codec: 'vp8', simulcast: {} });
      }
    }
    return out;
  }

  async _plugin({ handleId, body, jsep }) {
    const room = this.rooms.get(body.room);
    const h = this.handles.get(handleId);
    const perr = (code, error) => ({ event: { videoroom: 'event', error_code: code, error } });
    switch (body.request) {
      case 'join': {
        if (!room) return perr(426, 'No such room');
        if (!this.joinTokens.has(body.token)) return perr(433, 'Unauthorized');
        if (body.ptype === 'publisher') {
          this.handles.set(handleId, { role: 'pub', key: `pub:${handleId}`, room: body.room, pseudonym: body.id });
          room.pubHandles.add(handleId);
          const private_id = ++this.pvt;
          this.privateIds.set(private_id, handleId);
          this.log.joins.push(body);
          return { event: { videoroom: 'joined', room: body.room, id: body.id, private_id, publishers: [...room.peers].map(([id, p]) => FakeSfu.announce(id, p)) } };
        }
        if (!this.privateIds.has(body.private_id)) return perr(433, 'Unauthorized (this room requires a valid private_id)');
        this.log.joins.push(body);
        const feeds = (body.streams || []).map((s) => s.feed).filter((id) => room.peers.has(id));
        if (!feeds.length) return perr(437, "Can't offer an SDP with no stream");
        const key = `sub:${handleId}`;
        const r = await this._helper('subCreate', key, feeds.map((id) => ({ id, video: room.peers.get(id).video, e2ee: room.peers.get(id).e2ee })));
        const streams = FakeSfu.summary(r.mids);
        this.handles.set(handleId, { role: 'sub', key, room: body.room, streams, awaitingAnswer: true });
        return { event: { videoroom: 'attached', room: body.room, streams }, jsep: { type: 'offer', sdp: r.sdp } };
      }
      case 'publish': {
        this.log.publishes.push({ body, jsep });
        const sdp = simulcastRecv(await this._helper('answerPublisher', h.key, jsep.sdp), jsep.sdp);
        return { event: { videoroom: 'event', room: h.room, configured: 'ok' }, jsep: { type: 'answer', sdp } };
      }
      case 'subscribe': {
        this.log.subscribes.push(body);
        if (h.awaitingAnswer) this.violations.push('subscribe while an offer is unanswered');
        const room2 = this.rooms.get(h.room);
        const feeds = body.streams.map((s) => s.feed).filter((id) => room2.peers.has(id));
        if (!feeds.length) return perr(428, 'No such feed');
        const r = await this._helper('subAddFeeds', h.key, feeds.map((id) => ({ id, video: room2.peers.get(id).video, e2ee: room2.peers.get(id).e2ee })));
        h.streams = h.streams.concat(FakeSfu.summary(r.mids, h.streams.length));
        h.awaitingAnswer = true;
        return { event: { videoroom: 'updated', room: h.room, streams: h.streams }, jsep: { type: 'offer', sdp: r.sdp } };
      }
      case 'unsubscribe': {
        this.log.unsubscribes.push(body);
        for (const s of body.streams) {
          const st = h.streams.find((x) => x.mid === s.sub_mid);
          if (st) { st.active = false; delete st.feed_id; }
        }
        return { event: { videoroom: 'updated', room: h.room } }; // no offer: "nothing to renegotiate"
      }
      case 'start': {
        if (jsep) {
          this.log.answers.push(jsep);
          h.awaitingAnswer = false;
          await this._helper('subSetAnswer', h.key, jsep.sdp);
        }
        return { event: { videoroom: 'event', room: h.room, started: 'ok' } };
      }
      case 'configure': {
        this.log.configures.push(body);
        return { event: { videoroom: 'event', room: h.room, configured: 'ok' } };
      }
      case 'leave': {
        this.log.leaves.push(h.role);
        if (h.role === 'pub') this.rooms.get(h.room).pubHandles.delete(handleId);
        this._helper('close', h.key).catch(() => {});
        return { event: { videoroom: 'event', room: h.room, [h.role === 'pub' ? 'leaving' : 'left']: 'ok' } };
      }
      default:
        return perr(423, `Unknown request ${body.request}`);
    }
  }
}

// ---------------------------------------------------------------- static page host

const MIME = { '.html': 'text/html', '.mjs': 'text/javascript', '.js': 'text/javascript' };

function startStaticServer() {
  const srv = http.createServer((req, res) => {
    const url = req.url.split('?')[0];
    if (url === '/helper.html') { res.writeHead(200, { 'content-type': 'text/html' }); res.end(HELPER_HTML); return; }
    const name = url === '/' ? 'page.html' : path.basename(url);
    const file = path.join(PAGE_DIR, name);
    if (!fs.existsSync(file) || !fs.statSync(file).isFile()) { res.writeHead(404); res.end('not found'); return; }
    res.writeHead(200, { 'content-type': MIME[path.extname(file)] || 'application/octet-stream' });
    res.end(fs.readFileSync(file));
  });
  return new Promise((resolve) => srv.listen(0, '127.0.0.1', () => resolve(srv)));
}

// ------------------------------------------------------------- contract validators

const STATES = ['init', 'ws', 'joining', 'publishing', 'subscribing', 'connected', 'steady', 'leaving', 'closed', 'failed'];
const PHASES = ['ws', 'create', 'attach', 'join', 'publish', 'subscribe', 'ice', 'dtls', 'transport', 'renegotiate', 'keepalive', 'stats', 'other'];
const TIMING_KEYS = ['wsMs', 'createMs', 'joinedMs', 'publishMs', 'pubIceMs', 'pubDtlsMs', 'pubConnectMs', 'subOfferMs', 'subIceMs', 'subDtlsMs', 'subConnectMs', 'firstAudioMs', 'firstVideoMs', 'allPeersMs'];
const D_KEYS = {
  aIn: ['packets', 'lost', 'bytes', 'concealed', 'samples', 'nack', 'jbDelayMs', 'jbEmitted'],
  vIn: ['packets', 'lost', 'bytes', 'framesDecoded', 'framesDropped', 'freezeCount', 'freezeMs', 'pauseCount', 'nack', 'pli', 'fir'],
  aOut: ['packets', 'bytes', 'nack', 'retransmitted'],
  vOut: ['packets', 'bytes', 'framesEncoded', 'nack', 'pli', 'retransmitted'],
};
const G_KEYS = ['aInStreams', 'vInStreams', 'aJitterMsMax', 'vJitterMsMax', 'vWidthMin', 'vWidthMax', 'vFps', 'pubRttMs', 'subRttMs', 'availIn', 'availOut', 'aOutPps', 'vOutLayers', 'qualityLimit'];
const TRANSPORT_KEYS = ['tlsVersion', 'dtlsCipher', 'srtpCipher', 'dtlsState', 'iceState', 'localCandType', 'remoteCandType', 'protocol', 'ok'];

const sameKeys = (o, keys, where) => assert.deepEqual(Object.keys(o).sort(), [...keys].sort(), `${where}: keys`);
const isNum = (x) => typeof x === 'number' && Number.isFinite(x);
const numOrNull = (x, where) => assert.ok(x === null || isNum(x), `${where} must be a finite number or null, got ${String(x)}`);

function validateCounters(obj, keys, where) {
  sameKeys(obj, keys, where);
  for (const k of keys) assert.ok(isNum(obj[k]) && obj[k] >= 0, `${where}.${k} must be a number >= 0, got ${String(obj[k])}`);
}

function validateSample(s) {
  sameKeys(s, ['t', 'dtMs', 'd', 'g'], 'sample');
  assert.ok(isNum(s.t) && isNum(s.dtMs) && s.dtMs > 0, 'sample t/dtMs');
  sameKeys(s.d, Object.keys(D_KEYS), 'sample.d');
  for (const [cat, keys] of Object.entries(D_KEYS)) validateCounters(s.d[cat], keys, `sample.d.${cat}`);
  sameKeys(s.g, G_KEYS, 'sample.g');
  for (const k of G_KEYS.filter((x) => x !== 'vOutLayers' && x !== 'qualityLimit')) numOrNull(s.g[k], `sample.g.${k}`);
  assert.equal(typeof s.g.vOutLayers, 'object');
  for (const [rid, l] of Object.entries(s.g.vOutLayers)) {
    assert.ok(['l', 'm', 'h'].includes(rid), `layer rid ${rid}`);
    sameKeys(l, ['packets', 'bytes', 'w', 'h'], `layer ${rid}`);
    assert.ok(isNum(l.packets) && l.packets >= 0 && isNum(l.bytes) && l.bytes >= 0);
    numOrNull(l.w, 'layer.w');
    numOrNull(l.h, 'layer.h');
  }
  assert.ok(s.g.qualityLimit === null || ['none', 'cpu', 'bandwidth', 'other'].includes(s.g.qualityLimit), 'qualityLimit');
}

function validateStatus(b) {
  sameKeys(b, ['id', 'state', 'error', 'startedAt', 'timings', 'peers', 'transport', 'tot', 'samples', 'events'], 'BotStatus');
  assert.equal(typeof b.id, 'string');
  assert.ok(STATES.includes(b.state), `state ${b.state}`);
  if (b.error !== null) {
    sameKeys(b.error, ['phase', 'code', 'message'], 'error');
    assert.ok(PHASES.includes(b.error.phase), `phase ${b.error.phase}`);
    assert.ok(typeof b.error.code === 'string' || typeof b.error.code === 'number');
    assert.equal(typeof b.error.message, 'string');
  }
  assert.ok(isNum(b.startedAt));
  sameKeys(b.timings, TIMING_KEYS, 'timings');
  for (const k of TIMING_KEYS) assert.ok(b.timings[k] === null || (isNum(b.timings[k]) && b.timings[k] >= 0), `timings.${k}=${b.timings[k]}`);
  sameKeys(b.peers, ['expected', 'subscribed', 'audioFlowing', 'videoFlowing'], 'peers');
  for (const v of Object.values(b.peers)) assert.ok(isNum(v) && v >= 0);
  sameKeys(b.transport, ['pub', 'sub'], 'transport');
  for (const t of [b.transport.pub, b.transport.sub]) {
    if (t === null) continue;
    sameKeys(t, TRANSPORT_KEYS, 'TransportInfo');
    assert.equal(typeof t.ok, 'boolean');
    assert.ok(!/\d+\.\d+\.\d+\.\d+|[0-9a-f]{1,4}:[0-9a-f]{1,4}:[0-9a-f:]+/i.test(JSON.stringify(t)), 'transport info must not carry addresses');
  }
  sameKeys(b.tot, [...Object.keys(D_KEYS), 'e2eeEnc', 'e2eeDec', 'e2eeFail'], 'tot');
  for (const [cat, keys] of Object.entries(D_KEYS)) validateCounters(b.tot[cat], keys, `tot.${cat}`);
  for (const k of ['e2eeEnc', 'e2eeDec', 'e2eeFail']) assert.ok(isNum(b.tot[k]) && b.tot[k] >= 0);
  assert.ok(Array.isArray(b.samples) && Array.isArray(b.events));
  for (const s of b.samples) validateSample(s);
  for (const e of b.events) {
    sameKeys(e, ['t', 'kind', 'msg'], 'event');
    assert.ok(['state', 'warn', 'error', 'renegotiate', 'layer', 'token'].includes(e.kind), `event kind ${e.kind}`);
    assert.equal(typeof e.msg, 'string');
  }
}

// ------------------------------------------------------------------------ harness

class BotsPage {
  constructor(page) {
    this.page = page;
    this.acc = new Map(); // id -> {status, samples, events, statuses}
    this.errors = [];
    page.on('pageerror', (e) => this.errors.push(String(e && e.message)));
  }

  start(cfg) { return this.page.evaluate((c) => window.qbot.start(c), cfg); }

  async poll() {
    const r = await this.page.evaluate(() => window.qbot.poll());
    assert.ok(isNum(r.now));
    for (const b of r.bots) {
      validateStatus(b);
      const a = this.acc.get(b.id) || { samples: [], events: [], statuses: [] };
      a.status = b;
      a.samples.push(...b.samples);
      a.events.push(...b.events);
      a.statuses.push(JSON.stringify(b));
      this.acc.set(b.id, a);
    }
    return r;
  }

  get(id) { return this.acc.get(id); }

  async waitFor(id, pred, ms, label) {
    const end = Date.now() + ms;
    let a;
    while (Date.now() < end) {
      await this.poll();
      a = this.acc.get(id);
      if (a && pred(a.status, a)) return a;
      if (a && a.status.state === 'failed' && !/failed/.test(label)) {
        throw new Error(`bot failed while waiting for ${label}: ${JSON.stringify(a.status.error)} events=${JSON.stringify(a.events.slice(-8))}`);
      }
      await sleep(400);
    }
    throw new Error(`timeout waiting for ${label}: ${JSON.stringify(a && { state: a.status.state, error: a.status.error, timings: a.status.timings, peers: a.status.peers, events: a.events.slice(-10) })}`);
  }

  /** Concatenated JSON of everything ever returned for a bot (for the secret scan). */
  everything(id) { return this.acc.get(id).statuses.join('\n'); }
}

// Spec section 10: only mid, rid, repaired-rid and transport-wide-cc header extensions may remain.
function assertAllowedExtmaps(sdp, what) {
  const allowed = new Set(ALLOWED_EXTMAPS);
  const found = sdp.split('\r\n').filter((l) => l.startsWith('a=extmap:')).map((l) => l.split(' ')[1]);
  assert.ok(found.length > 0, `${what}: some extensions must remain`);
  for (const uri of found) assert.ok(allowed.has(uri), `${what}: extension ${uri} must not be negotiated`);
}

const median = (a) => { const b = [...a].sort((x, y) => x - y); return b.length ? b[Math.floor(b.length / 2)] : null; };

// --------------------------------------------------------------------- fixtures

let staticServer;
let baseUrl;
let sfu;
let wsUrl;
let botsPage;
let helperPage;
let ctx;

before(async () => {
  if (skipReason) return;
  staticServer = await startStaticServer();
  baseUrl = `http://127.0.0.1:${staticServer.address().port}`;
  ctx = await browser.newContext();
  helperPage = await ctx.newPage();
  await helperPage.goto(`${baseUrl}/helper.html`);
  await helperPage.waitForFunction(() => window.helperReady === true);
  sfu = new FakeSfu(helperPage);
  wsUrl = await sfu.start();
  const page = await ctx.newPage();
  botsPage = new BotsPage(page);
  await page.goto(`${baseUrl}/page.html`);
  await page.waitForFunction(() => window.qbotReady === true);
});

after(async () => {
  if (skipReason) return;
  try { await botsPage.page.evaluate(() => window.qbot.stopAll({ graceful: false })); } catch (e) { /* page gone */ }
  await sfu.close();
  await new Promise((r) => staticServer.close(r));
  await browser.close();
});

const ROOM = (n) => `room-${n}`;
const KEY_A = '00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff';
const KEY_BAD = 'ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100';

function baseCfg(over = {}) {
  return {
    id: 'lt-bot', wsUrl, sessionToken: 'SESSION-TOKEN-ONE', room: ROOM(1), pseudonym: 'me-aaaaaaaa', joinToken: 'JOINTOKEN-SECRET-1',
    peers: [], media: { audio: true, video: false },
    audio: { ptime: 60, maxaveragebitrate: 32000, cbr: true, fec: true, dtx: false },
    e2ee: { mode: 'none', keyHex: KEY_A },
    subscribe: { substream: 1, speakerSubstream: 2, temporal: 2, speaker: null },
    iceServers: [], expectTransport: 'off', dtlsFingerprint: null,
    timeouts: { requestMs: 8000, connectMs: 60000, keepaliveMs: 1000 },
    statsIntervalMs: 1000, renegotiateDebounceMs: 150, render: 'attach',
    ...over,
  };
}

const LONG = { timeout: 420_000 };
const opts = (extra = {}) => ({ skip: skipReason || false, ...LONG, ...extra });

// ------------------------------------------------------------------------ tests

test('audio bot: lifecycle, 60 ms Opus, token refresh, renegotiation, clean stop', opts(), async (t) => {
  const room = ROOM('audio');
  sfu.addRoom(room);
  sfu.rooms.get(room).peers.set('peer-a', { video: false, e2ee: null });
  const cfg = baseCfg({ id: 'audio-bot', room, peers: ['peer-a'] });

  const info = await botsPage.page.evaluate(() => window.qbot.info());
  assert.deepEqual(Object.keys(info).sort(), ['chromeVersion', 'hardwareConcurrency', 'hasScriptTransform', 'ua']);
  assert.equal(info.hasScriptTransform, true);
  assert.match(info.chromeVersion, /^\d+\./);
  t.diagnostic(`browser ${info.chromeVersion}, ${info.hardwareConcurrency} cores`);

  assert.deepEqual(await botsPage.start(cfg), { ok: true });
  const a = await botsPage.waitFor(cfg.id, (s) => s.state === 'steady', 240_000, 'steady');
  const s = a.status;
  t.diagnostic(`steady timings ${JSON.stringify(s.timings)}`);
  for (const k of TIMING_KEYS.filter((x) => x !== 'firstVideoMs')) assert.ok(s.timings[k] !== null, `timing ${k} must be set`);
  assert.equal(s.timings.firstVideoMs, null);
  assert.ok(s.timings.wsMs <= s.timings.createMs && s.timings.createMs <= s.timings.joinedMs && s.timings.joinedMs <= s.timings.publishMs);
  assert.ok(s.timings.subOfferMs >= s.timings.publishMs);
  assert.ok(s.timings.allPeersMs >= s.timings.firstAudioMs);
  assert.deepEqual([s.peers.expected, s.peers.subscribed, s.peers.audioFlowing], [1, 1, 1]);

  // What the SFU received from the bot: publisher offer, subscriber answer, join bodies.
  const pub = sfu.log.publishes[0];
  assert.equal(pub.jsep.type, 'offer');
  assert.equal(pub.jsep.e2ee, false);
  assert.deepEqual(pub.body, { request: 'publish', audio: true, video: false });
  assertAllowedExtmaps(pub.jsep.sdp, 'publisher offer');
  assert.equal(getPtime(pub.jsep.sdp), 60);
  assert.deepEqual(getOpusFmtp(pub.jsep.sdp), { minptime: '60', useinbandfec: '1', usedtx: '0', cbr: '1', stereo: '0', maxaveragebitrate: '32000' });
  assert.ok(!/red\/48000/.test(pub.jsep.sdp));
  assert.ok(pub.jsep.sdp.includes('urn:ietf:params:rtp-hdrext:sdes:mid'));
  const joinPub = sfu.log.joins.find((j) => j.ptype === 'publisher');
  assert.deepEqual(joinPub, { request: 'join', ptype: 'publisher', room, id: cfg.pseudonym, display: cfg.pseudonym, token: cfg.joinToken });
  const joinSub = sfu.log.joins.find((j) => j.ptype === 'subscriber');
  assert.equal(joinSub.room, room);
  assert.equal(joinSub.token, cfg.joinToken);
  assert.equal(typeof joinSub.private_id, 'number');
  assert.deepEqual(joinSub.streams.map((x) => x.feed), ['peer-a']);
  assert.ok(sfu.log.answers.length >= 1);
  assertAllowedExtmaps(sfu.log.answers[0].sdp, 'subscriber answer');

  // Transport self-check info (policy is off in this test, the fields are still reported).
  for (const role of ['pub', 'sub']) {
    const tr = s.transport[role];
    assert.ok(tr, `${role} transport info`);
    assert.equal(tr.ok, true);
    assert.equal(tr.protocol, 'udp');
    assert.ok(['host', 'srflx', 'prflx', 'relay'].includes(tr.localCandType), tr.localCandType);
    assert.ok(['host', 'srflx', 'prflx', 'relay'].includes(tr.remoteCandType), tr.remoteCandType);
    assert.equal(tr.dtlsState, 'connected');
    assert.ok(tr.tlsVersion && tr.dtlsCipher && tr.srtpCipher);
    t.diagnostic(`${role} transport: ${JSON.stringify(tr)}`);
  }

  // Token refresh: after setTokens() the SFU only accepts the new token; keepalives (1 s) must carry it.
  sfu.validTokens.add('SESSION-TOKEN-TWO');
  const kaBefore = sfu.mock.of('keepalive').length;
  await botsPage.page.evaluate((m) => window.qbot.setTokens(m), { [cfg.id]: 'SESSION-TOKEN-TWO' });
  sfu.validTokens.delete('SESSION-TOKEN-ONE');
  await sleep(3500);
  const ka = sfu.mock.of('keepalive').slice(kaBefore);
  assert.ok(ka.length >= 2, `keepalives after refresh: ${ka.length}`);
  assert.ok(ka.every((k) => k.token === 'SESSION-TOKEN-TWO'), 'keepalives use the refreshed token');
  await botsPage.poll();
  assert.equal(botsPage.get(cfg.id).status.state, 'steady', 'the bot survives the token change');
  assert.ok(botsPage.get(cfg.id).events.some((e) => e.kind === 'token'));

  // Publisher audio: packets per second with ptime 60 (measured from samples and on the SFU side).
  await sleep(4000);
  await botsPage.poll();
  const samples = botsPage.get(cfg.id).samples.filter((x) => x.d.aOut.packets > 0).slice(-6);
  const pps = samples.map((x) => x.g.aOutPps);
  const med = median(pps);
  t.diagnostic(`audio out pps per sample: ${JSON.stringify(pps)} median ${med}`);
  // 60 ms packets = 16.7/s; 20 ms would be 50/s. The band is wide because shared CI/dev machines delay timers.
  assert.ok(med >= 10 && med <= 24, `audio pps ${med} should be about 16.7 (ptime 60)`);
  const key = sfu.pubKeyOf(cfg.pseudonym);
  const in1 = (await sfu._helper('inbound', key)).find((x) => x.kind === 'audio');
  await sleep(3000);
  const in2 = (await sfu._helper('inbound', key)).find((x) => x.kind === 'audio');
  const rxPps = ((in2.packets - in1.packets) * 1000) / (in2.t - in1.t);
  const rxSize = (in2.bytes - in1.bytes) / (in2.packets - in1.packets);
  t.diagnostic(`SFU side: audio ${rxPps.toFixed(1)} packets/s, avg payload+header ${rxSize.toFixed(0)} bytes/packet`);
  assert.ok(rxPps >= 10 && rxPps <= 24, `SFU side audio pps ${rxPps}`);

  // Late joiner: renegotiation through the serial queue (subscribe, never a second join).
  sfu.addPeer(room, 'peer-b', { video: false, e2ee: null });
  await botsPage.waitFor(cfg.id, (st) => st.peers.subscribed === 2, 60_000, 'second peer subscribed');
  assert.equal(sfu.log.subscribes.length, 1);
  assert.deepEqual(sfu.log.subscribes[0].streams.map((x) => x.feed), ['peer-b']);
  await sleep(2500);
  await botsPage.poll();
  assert.equal(botsPage.get(cfg.id).samples.at(-1).g.aInStreams, 2, 'two inbound audio streams after the renegotiation');
  // Leaver: the bot releases the m-lines by their own ids.
  sfu.removePeer(room, 'peer-a');
  await botsPage.waitFor(cfg.id, (st) => st.peers.subscribed === 1, 30_000, 'peer left');
  await sleep(600);
  assert.equal(sfu.log.unsubscribes.length, 1);
  assert.ok(sfu.log.unsubscribes[0].streams.every((x) => typeof x.sub_mid === 'string'));
  assert.deepEqual(sfu.violations, [], 'never two offers in flight');

  // tot = exact sum of the sample deltas (poll() returns tot and the drained samples of the same instant).
  await botsPage.poll();
  const all = botsPage.get(cfg.id).samples;
  const cur = botsPage.get(cfg.id).status.tot;
  assert.equal(cur.aOut.packets, all.reduce((acc, x) => acc + x.d.aOut.packets, 0));
  assert.equal(cur.aIn.packets, all.reduce((acc, x) => acc + x.d.aIn.packets, 0));
  assert.ok(cur.aIn.packets > 0);

  // Graceful stop.
  const fin = await botsPage.page.evaluate((id) => window.qbot.stop(id, { graceful: true }), cfg.id);
  validateStatus(fin);
  assert.equal(fin.state, 'closed');
  assert.equal(fin.error, null);
  assert.ok(fin.tot.aOut.packets > 0 && fin.tot.aIn.packets > 0);
  assert.deepEqual(sfu.log.leaves.sort(), ['pub', 'sub']);
  assert.equal(sfu.mock.of('destroy').length, 1);
  assert.equal(sfu.mock.of('detach').length, 2);
  const again = await botsPage.page.evaluate((id) => window.qbot.stop(id, { graceful: true }), cfg.id);
  assert.equal(again.state, 'closed');
  const list = await botsPage.poll();
  assert.ok(!list.bots.some((b) => b.id === cfg.id), 'stopped bots leave the poll list');

  // Secrets never appear anywhere in what the page returned.
  const dump = botsPage.everything(cfg.id) + JSON.stringify(fin);
  for (const secret of [wsUrl, 'SESSION-TOKEN-ONE', 'SESSION-TOKEN-TWO', cfg.joinToken, KEY_A]) {
    assert.ok(!dump.includes(secret), `secret leaked: ${secret.slice(0, 6)}...`);
  }
  assert.deepEqual(botsPage.errors, [], 'no page errors');
  assert.ok(!botsPage.get(cfg.id).events.some((e) => /unhandled/.test(e.msg)));
});

test('video bot: 3-layer simulcast offer, layers l/m/h in the stats, remote video decoded, configure', opts(), async (t) => {
  const room = ROOM('video');
  sfu.addRoom(room);
  sfu.rooms.get(room).peers.set('peer-v', { video: true, e2ee: null });
  const cfg = baseCfg({
    id: 'video-bot', room, pseudonym: 'me-vvvvvvvv', peers: ['peer-v'], media: { audio: true, video: true },
    video: {
      capture: { width: 640, height: 360, frameRate: 15 },
      encodings: [
        { rid: 'h', scaleResolutionDownBy: 1, maxBitrate: 400000, maxFramerate: 15 },
        { rid: 'm', scaleResolutionDownBy: 2, maxBitrate: 200000, maxFramerate: 15 },
        { rid: 'l', scaleResolutionDownBy: 4, maxBitrate: 100000, maxFramerate: 15 },
      ],
    },
    subscribe: { substream: 1, speakerSubstream: 2, temporal: 2, speaker: 'peer-v' },
  });
  await botsPage.start(cfg);
  const a = await botsPage.waitFor(cfg.id, (s) => s.state === 'steady', 300_000, 'steady with video');
  t.diagnostic(`video steady timings ${JSON.stringify(a.status.timings)}`);
  assert.ok(a.status.timings.firstVideoMs !== null);
  assert.equal(a.status.peers.videoFlowing, 1);
  const cap = a.events.find((e) => /^capture video/.test(e.msg));
  assert.ok(cap, 'capture settings are recorded in an event');
  t.diagnostic(cap.msg);

  const pub = sfu.log.publishes.at(-1);
  assert.deepEqual(getSendRids(pub.jsep.sdp), ['h', 'm', 'l'], 'the offer carries 3 rids');
  assert.ok(pub.jsep.sdp.includes('a=simulcast:send h;m;l'));
  assertAllowedExtmaps(pub.jsep.sdp, 'simulcast offer');
  assert.ok(pub.jsep.sdp.includes('urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id'), 'rid extension survives the allow-list');
  const videoMid = getMLines(pub.jsep.sdp).find((m) => m.kind === 'video').mid;
  assert.deepEqual(pub.body.descriptions, [{ mid: videoMid, description: 'camera' }]);
  assert.deepEqual([pub.body.audio, pub.body.video], [true, true]);

  await sleep(6000);
  await botsPage.poll();
  const samples = botsPage.get(cfg.id).samples;
  const layers = new Map();
  for (const s of samples) {
    for (const [rid, l] of Object.entries(s.g.vOutLayers)) {
      const cur = layers.get(rid) || { packets: 0, w: null };
      cur.packets += l.packets;
      cur.w = l.w || cur.w;
      layers.set(rid, cur);
    }
  }
  t.diagnostic(`publisher layers ${JSON.stringify([...layers])}`);
  for (const rid of ['l', 'm', 'h']) assert.ok(layers.get(rid) && layers.get(rid).packets > 0, `layer ${rid} sends packets`);
  const tot = botsPage.get(cfg.id).status.tot;
  assert.ok(tot.vOut.framesEncoded > 0 && tot.vOut.packets > 0);
  assert.ok(tot.vIn.framesDecoded > 0, 'remote video is decoded');
  const last = samples.filter((s) => s.g.vInStreams > 0).at(-1);
  assert.ok(last.g.vWidthMax > 0 && last.g.vWidthMin > 0);
  assert.ok(['none', 'cpu', 'bandwidth', 'other'].includes(last.g.qualityLimit));

  // The bot asks for the speaker substream (2) and temporal 2 on the remote video m-line.
  const cf = sfu.log.configures.flatMap((c) => c.streams);
  assert.ok(cf.length >= 1);
  assert.ok(cf.every((x) => x.substream === 2 && x.temporal === 2 && typeof x.mid === 'string'));
  const joinSub = sfu.log.joins.find((j) => j.ptype === 'subscriber' && j.streams[0].feed === 'peer-v');
  assert.deepEqual([joinSub.streams[0].substream, joinSub.streams[0].temporal], [2, 2]);
  assert.ok(a.events.some((e) => e.kind === 'layer'));

  // setSubstream(): runtime override for every remote video mid.
  const cfBefore = sfu.log.configures.length;
  await botsPage.page.evaluate(([id]) => window.qbot.setSubstream(id, 0, 1), [cfg.id]);
  await sleep(2500);
  assert.ok(sfu.log.configures.length > cfBefore);
  assert.ok(sfu.log.configures.at(-1).streams.every((x) => x.substream === 0 && x.temporal === 1));

  const fin = await botsPage.page.evaluate((id) => window.qbot.stop(id, { graceful: true }), cfg.id);
  assert.equal(fin.state, 'closed');
  assert.deepEqual(botsPage.errors, []);
});

test('e2ee aesgcm: sender counters, receiver decrypts with the room key, wrong key is counted and dropped', opts(), async (t) => {
  const room = ROOM('e2ee');
  sfu.addRoom(room);
  sfu.rooms.get(room).peers.set('peer-ok', { video: false, e2ee: { mode: 'aesgcm', keyHex: KEY_A } });
  const cfg = baseCfg({ id: 'e2ee-bot', room, pseudonym: 'me-eeeeeeee', peers: ['peer-ok'], e2ee: { mode: 'aesgcm', keyHex: KEY_A } });
  await botsPage.start(cfg);
  await botsPage.waitFor(cfg.id, (s) => s.state === 'steady', 240_000, 'steady with e2ee');
  assert.equal(sfu.log.publishes.at(-1).jsep.e2ee, true, 'the e2ee flag rides on the JSEP');
  assert.ok(!('e2ee' in sfu.log.publishes.at(-1).body));
  await sleep(4000);
  await botsPage.poll();
  let tot = botsPage.get(cfg.id).status.tot;
  t.diagnostic(`e2ee counters ${JSON.stringify({ enc: tot.e2eeEnc, dec: tot.e2eeDec, fail: tot.e2eeFail })}`);
  t.diagnostic(`e2ee events ${JSON.stringify(botsPage.get(cfg.id).events.filter((e) => /e2ee/.test(e.msg)))}`);
  assert.ok(tot.e2eeEnc > 0, 'publisher frames are encrypted');
  assert.ok(tot.e2eeDec > 0, 'frames of the peer decrypt with the shared key');
  // The first frames of the helper peer can race its own transform start-up, so a few failures are
  // tolerated; what matters is that the peer decrypts and the bulk is clean.
  const e2eeEvents = () => botsPage.get(cfg.id).events.filter((e) => /e2ee/.test(e.msg));
  assert.ok(tot.e2eeFail <= Math.max(3, tot.e2eeDec / 4), `clean key: ${tot.e2eeFail} failures vs ${tot.e2eeDec} decrypted ${JSON.stringify(e2eeEvents())}`);
  const before = { dec: tot.e2eeDec, fail: tot.e2eeFail };

  // A peer using another key: its frames fail authentication, are counted and dropped, nothing throws.
  sfu.addPeer(room, 'peer-bad', { video: false, e2ee: { mode: 'aesgcm', keyHex: KEY_BAD } });
  await botsPage.waitFor(cfg.id, (s) => s.peers.subscribed === 2, 60_000, 'bad-key peer subscribed');
  await sleep(4000);
  await botsPage.poll();
  tot = botsPage.get(cfg.id).status.tot;
  t.diagnostic(`e2ee counters after the wrong-key peer ${JSON.stringify({ enc: tot.e2eeEnc, dec: tot.e2eeDec, fail: tot.e2eeFail })}`);
  assert.ok(tot.e2eeFail - before.fail >= 10, 'wrong-key frames are counted as failures');
  assert.ok(tot.e2eeDec > before.dec, 'the good peer keeps decrypting');
  // (warn events are rate limited, so an earlier one may already carry the breakdown)
  assert.ok(e2eeEvents().some((e) => /e2ee frame failures so far \{.*:/.test(e.msg)), 'the failure reasons are reported in a warn event');
  assert.equal(botsPage.get(cfg.id).status.state, 'steady');

  const fin = await botsPage.page.evaluate((id) => window.qbot.stop(id, { graceful: true }), cfg.id);
  assert.equal(fin.state, 'closed');
  assert.ok(fin.tot.e2eeEnc >= tot.e2eeEnc);
  assert.deepEqual(botsPage.errors, []);
});

test('failure paths end as state failed with a contract error (no unhandled rejections)', opts(), async () => {
  const room = ROOM('fail');
  sfu.addRoom(room);
  const failed = async (over, phase, code) => {
    const cfg = baseCfg({ room, ...over });
    await botsPage.start(cfg);
    const a = await botsPage.waitFor(cfg.id, (s) => s.state === 'failed', 120_000, 'failed');
    assert.equal(a.status.error.phase, phase, JSON.stringify(a.status.error));
    assert.equal(a.status.error.code, code, JSON.stringify(a.status.error));
    const fin = await botsPage.page.evaluate((id) => window.qbot.stop(id, { graceful: true }), cfg.id);
    assert.equal(fin.state, 'failed');
    assert.deepEqual(fin.error, a.status.error);
    return a;
  };
  // start() validates the config synchronously and never echoes values
  const bad = await botsPage.page.evaluate(async (c) => {
    try { await window.qbot.start(c); return null; } catch (e) { return String(e.message); }
  }, { ...baseCfg({ id: 'bad-cfg' }), wsUrl: 'http://not-a-websocket.example.invalid/x', joinToken: '' });
  assert.match(bad, /^invalid bot config: .*wsUrl.*joinToken/);
  assert.ok(!bad.includes('not-a-websocket'));
  assert.ok(!(await botsPage.poll()).bots.some((b) => b.id === 'bad-cfg'));
  // wrong session token: Janus refuses `create` with 403
  await failed({ id: 'bad-token', pseudonym: 'me-11111111', sessionToken: 'WRONG-TOKEN-VALUE' }, 'create', 403);
  // unknown room
  await failed({ id: 'bad-room', pseudonym: 'me-22222222', room: 'no-such-room' }, 'join', 426);
  // wrong join token
  await failed({ id: 'bad-join', pseudonym: 'me-33333333', joinToken: 'NOT-ALLOWED-TOKEN' }, 'join', 433);
  // DTLS pin that the answer cannot satisfy
  await failed({ id: 'bad-pin', pseudonym: 'me-44444444', dtlsFingerprint: 'sha-256 00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF' },
    'dtls', 'dtls_pin_mismatch');
  // strict transport policy: a Chromium-to-Chromium loopback is not DTLS 1.3 + AES-256-GCM + ML-KEM
  const a = await failed({ id: 'strict', pseudonym: 'me-55555555', expectTransport: 'strict' }, 'transport', 'transport_policy_violation');
  assert.match(a.status.error.message, /policy violation/);
  assert.deepEqual(botsPage.errors, []);
  for (const st of ['bad-token', 'bad-room', 'bad-join', 'bad-pin', 'strict']) {
    assert.ok(!botsPage.everything(st).includes('WRONG-TOKEN-VALUE') && !botsPage.everything(st).includes('NOT-ALLOWED-TOKEN'));
  }
});

test('two bots in one page: one shared E2EE worker with per-bot counters, xor mode, render none, stopAll', opts(), async (t) => {
  const roomA = ROOM('multi-a');
  const roomB = ROOM('multi-b');
  sfu.addRoom(roomA);
  sfu.addRoom(roomB);
  sfu.rooms.get(roomA).peers.set('peer-x', { video: false, e2ee: { mode: 'aesgcm', keyHex: KEY_A } });
  sfu.rooms.get(roomB).peers.set('peer-y', { video: false, e2ee: { mode: 'xor', keyHex: KEY_A } });
  const c1 = baseCfg({ id: 'multi-1', room: roomA, pseudonym: 'me-mmmmmmm1', peers: ['peer-x'], e2ee: { mode: 'aesgcm', keyHex: KEY_A } });
  const c2 = baseCfg({ id: 'multi-2', room: roomB, pseudonym: 'me-mmmmmmm2', peers: ['peer-y'], e2ee: { mode: 'xor', keyHex: KEY_A }, render: 'none' });
  await Promise.all([botsPage.start(c1), botsPage.start(c2)]);
  await Promise.all([
    botsPage.waitFor(c1.id, (s) => s.state === 'steady', 300_000, 'bot 1 steady'),
    botsPage.waitFor(c2.id, (s) => s.state === 'steady', 300_000, 'bot 2 steady'),
  ]);
  await sleep(4000);
  const list = await botsPage.poll();
  assert.equal(list.bots.length, 2);
  for (const id of [c1.id, c2.id]) {
    const tot = botsPage.get(id).status.tot;
    t.diagnostic(`${id} e2ee ${JSON.stringify({ enc: tot.e2eeEnc, dec: tot.e2eeDec, fail: tot.e2eeFail })}`);
    assert.ok(tot.e2eeEnc > 0 && tot.e2eeDec > 0 && tot.e2eeFail <= Math.max(3, tot.e2eeDec / 4), `${id} counters`);
  }
  assert.equal(botsPage.get(c2.id).status.peers.audioFlowing, 1, 'render none: packets alone make the peer flow');
  const finals = await botsPage.page.evaluate(() => window.qbot.stopAll({ graceful: true }));
  assert.deepEqual(finals.map((f) => f.id).sort(), [c1.id, c2.id]);
  assert.ok(finals.every((f) => f.state === 'closed'));
  assert.equal((await botsPage.poll()).bots.length, 0);
  assert.deepEqual(botsPage.errors, []);
});
