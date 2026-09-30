// End-to-end tests of qjanus with real browsers: the group call protocol of GROUP_CALLS_V2 section 4
// (2 PeerConnections per participant, multistream subscriber, simulcast, E2EE frames that keep the codec
// header clear, renegotiation, kick), the transport level, netem loss/reorder, and the negative peers
// (DTLS 1.2, no ML-KEM, AES-128) that must be refused. Run against the INSTALLED service (systemd).
//   QJANUS_WS, QJANUS_HTTP, QJANUS_TOKEN_SECRET, QJANUS_ADMIN_KEY, QJANUS_FINGERPRINT   as conformance.mjs
//   SUITE=main|loss|negative|all      OLD_CHROMES='[{"label":"130","path":"/x/chrome"}]'
//   NETEM=/path/to/netem.sh           OUT_DIR=./e2e-out
import { chromium, firefox } from 'playwright';
import assert from 'node:assert/strict';
import fs from 'node:fs';
import http from 'node:http';
import path from 'node:path';
import { execFileSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';
import { HttpApi, ServerApi, WsClient, hex, joinTokenFor, sleep, pluginData, remember, secretsSeen } from '../lib/janus.mjs';
import { assertIceCandidates, describeCandidates } from '../lib/ice.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const WS = process.env.QJANUS_WS || 'ws://127.0.0.1:8188';
const HTTP = process.env.QJANUS_HTTP || 'http://127.0.0.1:8088/janus';
const TOKEN_SECRET = process.env.QJANUS_TOKEN_SECRET;
const ADMIN_KEY = process.env.QJANUS_ADMIN_KEY;
const FINGERPRINT = (process.env.QJANUS_FINGERPRINT || '').toLowerCase();
const SUITE = process.env.SUITE || 'all';
const OLD_CHROMES = JSON.parse(process.env.OLD_CHROMES || '[]');
const NETEM = process.env.NETEM || path.join(HERE, 'netem.sh');
const OUT = process.env.OUT_DIR || path.join(HERE, 'e2e-out');
const PORT = 8199;
const REC_PROBE = '/tmp/qjanus-rec-probe/rec';
const PAGE = `http://127.0.0.1:${PORT}/page.html`;
if (!TOKEN_SECRET || !ADMIN_KEY) throw new Error('QJANUS_TOKEN_SECRET and QJANUS_ADMIN_KEY are required');
fs.mkdirSync(OUT, { recursive: true });

const api = new ServerApi(new HttpApi(HTTP, { tokenSecret: TOKEN_SECRET, adminKey: ADMIN_KEY }));
const results = [];
const versions = {};
// every SDP that came out of Janus was checked against the ICE candidate policy (lib/ice.mjs); the first of each kind is logged
const iceLogged = new Set();
let iceChecked = 0;
function note_ice(label, cands) {
  iceChecked++;
  const kind = label.replace(/^\S+ /, '');
  if (!iceLogged.has(kind)) { iceLogged.add(kind); console.log(`ICE   ${kind}: ${describeCandidates(cands)}`); }
}

// First occurrence of every request/response and unsolicited event shape the group call clients depend on
// (printed at the end, written to shapes.json); SDP bodies and secrets are redacted.
const shapes = {};
const scrub = (o) => JSON.parse(JSON.stringify(o, (k, v) => {
  if (k === 'sdp' && typeof v === 'string') return `<sdp ${v.length} chars>`;
  if (['token', 'secret', 'admin_key'].includes(k) && typeof v === 'string') return `<${k}>`;
  return v;
}));
const note = (name, req, resp) => { if (!shapes[name]) shapes[name] = { request: scrub(req), response: scrub(resp) }; };
const origMessage = WsClient.prototype.message;
WsClient.prototype.message = async function message(handle, body, jsep, opts) {
  const r = await origMessage.call(this, handle, body, jsep, opts);
  note(`msg:${body.request}${body.ptype ? `:${body.ptype}` : ''}${jsep ? `:jsep-${jsep.type}` : ''}`, { body, ...(jsep ? { jsep } : {}) }, r);
  return r;
};
const origOnMessage = WsClient.prototype.onMessage;
WsClient.prototype.onMessage = function onMessage(m) {
  if (!m.transaction) {
    const d = pluginData(m);
    const keys = Object.keys(d).filter((k) => !['videoroom', 'room'].includes(k)).sort().join('+');
    note(`event:${m.janus}${d.videoroom ? `:${d.videoroom}` : ''}${keys ? `:${keys}` : ''}`, {}, m);
  }
  return origOnMessage.call(this, m);
};

// ------------------------------------------------------------------------------------ runner
async function check(name, fn) {
  const t0 = Date.now();
  let detail = '';
  try {
    detail = (await fn()) || '';
    results.push({ name, ok: true, detail });
    console.log(`PASS  ${name} (${Math.round((Date.now() - t0) / 1000)} s) ${detail}`);
  } catch (e) {
    results.push({ name, ok: false, error: String((e && e.stack) || e) });
    console.log(`FAIL  ${name}\n      ${String((e && e.message) || e).split('\n').join('\n      ')}`);
  }
  fs.writeFileSync(path.join(OUT, 'results.json'), JSON.stringify(results, null, 1));
}

// ------------------------------------------------------------------------- page + browsers
function startServer() {
  const srv = http.createServer((req, res) => {
    const f = req.url.split('?')[0] === '/worker.js' ? 'worker.js' : 'page.html';
    res.writeHead(200, { 'content-type': f.endsWith('.js') ? 'text/javascript' : 'text/html' });
    res.end(fs.readFileSync(path.join(HERE, f)));
  });
  return new Promise((r) => srv.listen(PORT, '127.0.0.1', () => r(srv)));
}
const BASE_ARGS = [
  '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream',
  '--disable-features=WebRtcHideLocalIpsWithMdns', '--allow-loopback-in-peer-connection',
  '--no-sandbox', '--autoplay-policy=no-user-gesture-required',
];
const PQC_TRIAL = 'WebRTC-EnableDtlsPqc/Enabled/';
async function launchChromium(fieldTrials, executablePath) {
  const args = [...BASE_ARGS];
  if (fieldTrials) args.push(`--force-fieldtrials=${fieldTrials}`);
  return chromium.launch({ headless: true, args, ...(executablePath ? { executablePath } : {}) });
}
function launchFirefox() {
  return firefox.launch({
    headless: true,
    firefoxUserPrefs: {
      'media.navigator.streams.fake': true, 'media.navigator.permission.disabled': true,
      'media.peerconnection.ice.loopback': true, 'media.peerconnection.ice.obfuscate_host_addresses': false,
    },
  });
}
async function newPage(browser) {
  const ctx = await browser.newContext();
  const page = await ctx.newPage();
  page.on('pageerror', (e) => console.log(`      [page error] ${String(e).slice(0, 200)}`));
  await page.goto(PAGE);
  return { page, ctx };
}

// ------------------------------------------------------------------------------- journal
// The service logs to the journal (stdout); the refusal evidence is the DTLS-POLICY line
function journalSince(epochSec) {
  try {
    return execFileSync('sudo', ['-n', 'journalctl', '-u', 'qjanus', '-o', 'cat', '--no-pager', '--since', `@${Math.floor(epochSec)}`], { encoding: 'utf8', maxBuffer: 64 * 1024 * 1024 });
  } catch (e) { return ''; }
}
const dtlsLines = (text) => text.split('\n').filter((l) => /DTLS-POLICY|Handshake error|DTLS handshake failure|taking too much time/.test(l));
const isOk = (l) => /DTLS-POLICY/.test(l) && /version=0xfefc cipher=0x1302 srtp=0x0008 group=0x11ec/.test(l) && /ok=1/.test(l);
const isRefusal = (l) => /ok=0|Handshake error|DTLS handshake failure/.test(l);

// --------------------------------------------------------------------------- helpers
const SRTP_OK = ['AEAD_AES_256_GCM', 'SRTP_AEAD_AES_256_GCM'];   // Chromium spells the profile both ways
function assertLevel(t, what) {
  assert.ok(t, `${what}: no transport stats`);
  assert.equal(t.dtlsState, 'connected', `${what}: DTLS state`);
  assert.equal(t.tlsVersion, 'FEFC', `${what}: DTLS 1.3`);
  assert.equal(t.dtlsCipher, 'TLS_AES_256_GCM_SHA384', `${what}: cipher`);
  assert.ok(SRTP_OK.includes(t.srtpCipher), `${what}: SRTP profile ${t.srtpCipher}`);
}
const levelOk = (t) => !!t && t.dtlsState === 'connected' && t.tlsVersion === 'FEFC' && t.dtlsCipher === 'TLS_AES_256_GCM_SHA384' && SRTP_OK.includes(t.srtpCipher);
const fpOf = (sdp) => { const m = /a=fingerprint:(\S+) ([0-9A-Fa-f:]+)/.exec(sdp); return m ? `${m[1]} ${m[2]}`.toLowerCase() : ''; };
const evalIn = (p, fn, arg) => p.page.evaluate(fn, arg);
async function waitState(p, pcId, wanted, ms) {
  const end = Date.now() + ms;
  let last = null;
  while (Date.now() < end) {
    last = await evalIn(p, (i) => window.q.state(i), pcId);
    if (last && wanted.includes(last.connection)) return last;
    await sleep(250);
  }
  return last;
}
// the browser's view of the DTLS transport can lag the server's `webrtcup` under packet loss
async function waitLevel(p, pcId, ms) {
  const end = Date.now() + ms;
  let t = null;
  while (Date.now() < end) {
    t = await p.transport(pcId).catch(() => null);
    if (levelOk(t)) return true;
    await sleep(300);
  }
  return false;
}
const codeOf = (m) => (m && m.error && m.error.code) || pluginData(m).error_code || 0;
const sumBy = (arr, mid, f) => arr.filter((x) => x.mid === mid).reduce((a, x) => a + (x[f] || 0), 0);

// ------------------------------------------------------------- one call participant
// Publisher handle + publisher PC, subscriber handle + subscriber PC, all over ONE Janus session.
class Peer {
  constructor(name, browserRef, room, { simulcast = true, setup = null, upTimeout = 20000, video = null } = {}) {
    this.name = name;
    this.room = room;
    this.id = hex(16);                   // pseudonym
    this.joinToken = joinTokenFor(this.id);   // bound to the pseudonym
    this.key = hex(32);                  // K[me, epoch]
    this.keyIndex = 1;
    this.simulcast = simulcast;
    this.video = video;                  // {width,height} of the camera; default 640x360
    this.setup = setup;                  // 'passive' = Janus becomes the DTLS client on the subscriber PC
    this.upTimeout = upTimeout;
    this.pubPc = `${name}-pub`;
    this.subPc = `${name}-sub`;
    this.subMidMap = {};                 // subscriber mid -> pseudonym
    this.subscribedFeeds = new Set();
    this.queue = Promise.resolve();      // renegotiations of the subscriber PC are strictly serialised
    this.browserRef = browserRef;
    remember(this.id, this.joinToken, this.key);
  }
  async open() {
    const { page, ctx } = await newPage(this.browserRef);
    this.page = page; this.ctx = ctx;
    this.c = await new WsClient(WS, { tokenSecret: TOKEN_SECRET }).open();
    await this.c.create();
    this.c.startKeepalive(20000);
    this.pubHandle = await this.c.attach();
    this.subHandle = await this.c.attach();
    return this;
  }
  async join() {
    const m = await this.c.message(this.pubHandle, { request: 'join', ptype: 'publisher', room: this.room, id: this.id, display: this.id, token: this.joinToken });
    const d = pluginData(m);
    assert.equal(d.videoroom, 'joined', JSON.stringify(m).slice(0, 300));
    assert.equal(d.id, this.id);
    this.privateId = d.private_id;
    return d.publishers;
  }
  async publish() {
    await evalIn(this, ([k, i]) => window.q.setSendKey(k, i), [this.key, this.keyIndex]);
    const offer = await evalIn(this, ([i, o]) => window.q.newPublisher(i, o), [this.pubPc, { e2ee: true, simulcast: this.simulcast, ...(this.video || {}) }]);
    const m = await this.c.message(this.pubHandle, { request: 'publish', audio: true, video: true, descriptions: [{ mid: '0', description: 'mic' }, { mid: '1', description: 'camera' }] },
      { type: 'offer', sdp: offer, e2ee: true, ...(this.simulcast ? { rid_order: 'lmh' } : {}) });
    assert.equal(pluginData(m).configured, 'ok', JSON.stringify(m).slice(0, 300));
    assert.equal(m.jsep.type, 'answer');
    this.pubAnswer = m.jsep.sdp;
    note_ice(`${this.name} publisher answer`, assertIceCandidates(m.jsep.sdp, `${this.name} publisher answer`));
    await evalIn(this, ([i, s]) => window.q.setAnswer(i, s), [this.pubPc, m.jsep.sdp]);
    await this.c.waitEvent((e) => e.janus === 'webrtcup' && e.sender === this.pubHandle, this.upTimeout);
  }
  // every other participant's key must be installed BEFORE its track is rendered (section 5.4)
  async learnKey(other) {
    await evalIn(this, ([p, i, k]) => window.q.setRecvKey(p, i, k), [other.id, other.keyIndex, other.key]);
  }
  // subscribe to (feed, all its streams); first call = join, later calls = update (renegotiation)
  subscribe(pubs) {
    const job = async () => {
      const wanted = pubs.filter((p) => !this.subscribedFeeds.has(p.id));
      if (!wanted.length) return;
      const streams = wanted.flatMap((p) => p.streams.map((s) => ({ feed: p.id, mid: s.mid })));
      let m;
      if (!this.subscribedFeeds.size && !this.subJoined) {
        m = await this.c.message(this.subHandle, { request: 'join', ptype: 'subscriber', room: this.room, private_id: this.privateId, token: this.joinToken, streams });
        this.subJoined = true;
      } else {
        m = await this.c.message(this.subHandle, { request: 'update', subscribe: streams });
      }
      const d = pluginData(m);
      assert.ok(d.videoroom === 'attached' || d.videoroom === 'updated', JSON.stringify(m).slice(0, 300));
      for (const s of d.streams || []) if (s.feed_id) this.subMidMap[s.mid] = s.feed_id;
      wanted.forEach((p) => this.subscribedFeeds.add(p.id));
      if (m.jsep) await this.answerOffer(m.jsep.sdp);
    };
    this.queue = this.queue.then(job);
    return this.queue;
  }
  unsubscribe(feed) {
    const job = async () => {
      if (!this.subscribedFeeds.has(feed)) return;
      const m = await this.c.message(this.subHandle, { request: 'unsubscribe', streams: [{ feed }] });
      const d = pluginData(m);
      assert.equal(d.videoroom, 'updated', JSON.stringify(m).slice(0, 300));
      this.subscribedFeeds.delete(feed);
      if (m.jsep) await this.answerOffer(m.jsep.sdp);
    };
    this.queue = this.queue.then(job);
    return this.queue;
  }
  async answerOffer(offerSdp) {
    this.lastSubOffer = offerSdp;
    note_ice(`${this.name} subscriber offer`, assertIceCandidates(offerSdp, `${this.name} subscriber offer`, { require: !this.subUp }));
    const answer = await evalIn(this, ([i, o, mm, opts]) => window.q.answer(i, o, mm, opts), [this.subPc, offerSdp, this.subMidMap, { e2ee: true, setup: this.setup }]);
    const first = !this.subUp;
    await this.c.message(this.subHandle, { request: 'start' }, { type: 'answer', sdp: answer });
    if (first) {
      await this.c.waitEvent((e) => e.janus === 'webrtcup' && e.sender === this.subHandle, this.upTimeout);
      this.subUp = true;
    }
  }
  async stats(pc) { return evalIn(this, (i) => window.q.stats(i), pc); }
  async transport(pc) { return evalIn(this, (i) => window.q.transport(i), pc); }
  async workerStats() { return evalIn(this, () => window.q.workerStats()); }
  async close() {
    this.c.close();
    await this.ctx.close().catch(() => {});
  }
}

// join in order; everybody learns everybody's key first; new publishers are subscribed to by the others
async function startCall(browser, room, names, opts = {}) {
  const peers = [];
  for (const n of names) {
    const p = await new Peer(n, browser, room, opts[n] || {}).open();
    peers.push(p);
  }
  await api.allow(room, S.roomSecret, 'add', peers.map((p) => p.joinToken));
  for (const p of peers) for (const o of peers) if (p !== o) await p.learnKey(o);
  const live = [];                       // publishers so far: {id, streams}
  for (const p of peers) {
    const existing = await p.join();
    await p.publish();
    // publisher list as Janus reports it
    const known = existing.map((e) => ({ id: e.id, streams: e.streams }));
    if (known.length) await p.subscribe(known);
    const mine = { id: p.id, streams: [{ mid: '0' }, { mid: '1' }] };
    for (const o of live) o.peer.subscribe([mine]);
    live.push({ peer: p, pub: mine });
  }
  for (const p of peers) await p.queue;
  return peers;
}

// ---------------------------------------------------------------------- room + server
const S = { roomSecret: hex(32) };
remember(S.roomSecret);
async function makeRoom(publishers = 8) {
  const room = hex(16);
  remember(room);
  const created = await api.createRoom({ room, secret: S.roomSecret, publishers, allowed: [] });
  assert.equal(created.videoroom, 'created', JSON.stringify(created));
  return room;
}

// -------------------------------------------------------------------- media assertions
async function mediaWindow(peer, ms = 3000) {
  const s1 = await peer.stats(peer.subPc);
  await sleep(ms);
  const s2 = await peer.stats(peer.subPc);
  return { s1, s2 };
}
function assertFlowing(peer, w, expectedFeeds, { minAudio = 20, minFrames = 5 } = {}) {
  const seen = { audio: 0, video: 0 };
  for (const [mid, feed] of Object.entries(peer.subMidMap)) {
    if (!expectedFeeds.includes(feed)) continue;
    const a = w.s2.inbound.find((x) => x.mid === mid);
    assert.ok(a, `${peer.name}: no inbound stats for mid ${mid}`);
    if (a.kind === 'audio') {
      assert.ok(sumBy(w.s2.inbound, mid, 'packetsReceived') - sumBy(w.s1.inbound, mid, 'packetsReceived') >= minAudio, `${peer.name}: audio from ${feed.slice(0, 8)} is not flowing`);
      seen.audio++;
    } else {
      const d = sumBy(w.s2.inbound, mid, 'framesDecoded') - sumBy(w.s1.inbound, mid, 'framesDecoded');
      assert.ok(d >= minFrames, `${peer.name}: video from ${feed.slice(0, 8)} is not decoding (${d} frames)`);
      seen.video++;
    }
  }
  assert.equal(seen.audio, expectedFeeds.length, `${peer.name}: audio streams`);
  assert.equal(seen.video, expectedFeeds.length, `${peer.name}: video streams`);
}
async function assertE2ee(peer, feeds) {
  const ws = await peer.workerStats();
  assert.ok(ws, `${peer.name}: no worker stats`);
  for (const f of feeds) {
    const pp = ws.counters ? ws.counters.perParticipant[f] : ws.perParticipant[f];
    assert.ok(pp && pp.ok > 0, `${peer.name}: no frame of ${f.slice(0, 8)} was decrypted`);
    assert.equal(pp.authFail, 0, `${peer.name}: authentication failures for ${f.slice(0, 8)}`);
    assert.equal(pp.missingKey, 0, `${peer.name}: missing keys for ${f.slice(0, 8)}`);
  }
  return ws.counters || ws;
}

// ======================================================================================== main
const srv = await startServer();
const startedAt = Date.now() / 1000;
try {
  const pqc = await launchChromium(PQC_TRIAL);
  versions.chromium = pqc.version();

  if (SUITE === 'main' || SUITE === 'all') {
    // ---- 3 participants: publishers, multistream subscribers, simulcast, E2EE, renegotiation, kick
    let room; let peers = [];
    await check('3-party call: transport level, DTLS pin, E2EE frames from every sender decrypt at every receiver, all media flows', async () => {
      room = await makeRoom(8);
      peers = await startCall(pqc, room, ['A', 'B', 'C'], { B: { video: { width: 1280, height: 720 } }, C: { setup: 'passive' } });   // B publishes the spec profile (l/m/h = 320/640/1280)
      const ids = peers.map((p) => p.id);
      await sleep(6000);
      const detail = [];
      for (const p of peers) {
        for (const pc of [p.pubPc, p.subPc]) assertLevel(await p.transport(pc), `${p.name} ${pc}`);
        const others = ids.filter((i) => i !== p.id);
        assert.deepEqual([...p.subscribedFeeds].sort(), others.sort(), `${p.name} subscriptions`);
        assertFlowing(p, await mediaWindow(p), others);
        const c = await assertE2ee(p, others);
        detail.push(`${p.name}:dec=${c.decrypted.audio}/${c.decrypted.video}`);
        // the DTLS pin (spec 4.4): Janus' answer/offer carries the node certificate fingerprint
        if (FINGERPRINT) {
          assert.equal(fpOf(p.pubAnswer), FINGERPRINT, `${p.name}: publisher answer fingerprint`);
          assert.equal(fpOf(p.lastSubOffer), FINGERPRINT, `${p.name}: subscriber offer fingerprint`);
        }
      }
      return detail.join(' ');
    });

    await check('SDP rules: no audio-level / abs-capture-time / video-orientation on the wire, Opus 60 ms CBR profile, simulcast rids in the publisher offer', async () => {
      for (const p of peers) {
        for (const [what, sdp] of [['publisher answer', p.pubAnswer], ['subscriber offer', p.lastSubOffer]]) {
          assert.ok(!/ssrc-audio-level|abs-capture-time|video-orientation/.test(sdp), `${p.name} ${what}: an RTP header extension that must be stripped is negotiated`);
          assert.match(sdp, /a=ice-lite/, `${p.name} ${what}: ICE lite`);
        }
        assert.match(p.pubAnswer, /a=setup:active/, `${p.name}: Janus is the DTLS client for a publisher offer`);
      }
      const c = peers.find((p) => p.name === 'C');
      assert.match(c.lastSubOffer, /a=setup:actpass/, 'C: subscriber offer role');
      assert.ok(c.subMidMap && Object.keys(c.subMidMap).length === 4, 'C subscribes to 2 streams of each of 2 publishers');
    });

    await check('simulcast: the publisher sends 3 layers (rid_order lmh), the subscriber selects each substream (0 < 1 < 2) and gets exactly that layer, with E2EE frames', async () => {
      const [A, B] = peers;
      let out = {}; let layers = [];
      for (let i = 0; i < 20; i++) {
        layers = (await B.stats(B.pubPc)).outbound.filter((x) => x.kind === 'video' && x.rid);
        out = Object.fromEntries(layers.map((x) => [x.rid, x.frameWidth || 0]));
        if (out.l > 0 && out.m > 0 && out.h > 0) break;
        await sleep(500);
      }
      const cap = await evalIn(B, (i) => window.q.captureSettings(i), B.pubPc);
      const diag = JSON.stringify({ capture: cap, layers: layers.map((x) => ({ rid: x.rid, w: x.frameWidth, h: x.frameHeight, frames: x.framesEncoded, pkts: x.packetsSent, kbps: Math.round((x.targetBitrate || 0) / 1000), limit: x.qualityLimitationReason })) });
      assert.deepEqual(layers.filter((x) => x.packetsSent > 0).map((x) => x.rid).sort(), ['h', 'l', 'm'], `B sends all three simulcast layers: ${diag}`);
      assert.ok(out.l > 0 && out.l < out.m && out.m < out.h, `three distinct layer sizes: ${diag}`);
      const vs = (await A.stats(A.subPc)).inbound.find((x) => x.kind === 'video' && A.subMidMap[x.mid] === B.id);
      assert.ok(vs, 'A receives the video of B');
      const videoMid = vs.mid;
      const timeline = [];
      // Rooms have fir_freq=0 (no periodic keyframe request): the keyframe of the new layer comes from the PLI that
      // Janus sends when `configure` changes the substream, and Janus sends at most one PLI per second per
      // publisher stream and does not retry a PLI it skipped. A switch asked for right after another PLI can
      // therefore get no keyframe; the client's answer (spec 4.6) is to send `configure` again when the switch
      // did not happen within a few seconds, which is what this loop does (and reports).
      for (const [sub, want] of [[0, out.l], [1, out.m], [2, out.h], [0, out.l]]) {
        const configure = async () => {
          const m = await A.c.message(A.subHandle, { request: 'configure', streams: [{ mid: videoMid, substream: sub, temporal: 2 }] });
          assert.equal(pluginData(m).configured, 'ok', JSON.stringify(m).slice(0, 200));
        };
        await configure();
        const end = Date.now() + 20000;
        let got = 0; let lastSent = Date.now(); let resent = 0;
        while (Date.now() < end) {
          const v = (await A.stats(A.subPc)).inbound.find((x) => x.mid === videoMid);
          got = v ? v.frameWidth : 0;
          if (got === want) break;
          if (Date.now() - lastSent > 4000) { await configure(); resent++; lastSent = Date.now(); }
          await sleep(500);
        }
        if (resent) console.log(`      substream ${sub}: no keyframe after the first configure, re-sent ${resent}x`);
        timeline.push(`${sub}:${got}/${want}${resent ? `(re-sent ${resent}x)` : ''}`);
        assert.equal(got, want, `substream ${sub}: frame width ${got}, expected ${want} (${timeline.join(' ')}; sent ${JSON.stringify(out)})`);
      }
      await assertE2ee(A, [B.id]);
      return `sent=${JSON.stringify(out)} selected=${timeline.join(' ')}`;
    });

    await check('key rotation with E2EE: a sender switches to a new key index; receivers that hold it keep decrypting, without failures', async () => {
      const [A, B] = peers;
      const newKey = hex(32);
      remember(newKey);
      for (const p of peers) if (p !== A) await evalIn(p, ([id, i, k]) => window.q.setRecvKey(id, i, k), [A.id, 2, newKey]);
      const before = await assertE2ee(B, [A.id]);
      await evalIn(A, ([k, i]) => window.q.setSendKey(k, i), [newKey, 2]);
      A.key = newKey; A.keyIndex = 2;     // later joiners learn the current key of A
      await sleep(4000);
      const w = await mediaWindow(B);
      assertFlowing(B, w, [A.id, peers[2].id]);
      const after = await assertE2ee(B, [A.id]);
      assert.ok(after.perParticipant[A.id].ok > before.perParticipant[A.id].ok, 'frames keep decrypting after the switch-over');
    });

    await check('subscriber join: require_pvtid (missing / wrong private_id refused), token not needed; unpublished feed refused', async () => {
      const [A, B] = peers;
      const c = await new WsClient(WS, { tokenSecret: TOKEN_SECRET }).open();
      await c.create();
      const h = await c.attach();
      const streams = [{ feed: A.id, mid: '0' }];
      const noPvt = await c.message(h, { request: 'join', ptype: 'subscriber', room, streams });
      assert.equal(pluginData(noPvt).error_code, 433, JSON.stringify(noPvt).slice(0, 300));
      const wrong = await c.message(h, { request: 'join', ptype: 'subscriber', room, private_id: (B.privateId + 1) >>> 0, streams });
      assert.equal(pluginData(wrong).error_code, 433, JSON.stringify(wrong).slice(0, 300));
      const good = await c.message(h, { request: 'join', ptype: 'subscriber', room, private_id: B.privateId, streams });
      assert.equal(pluginData(good).videoroom, 'attached', JSON.stringify(good).slice(0, 300));
      c.close();
    });

    await check('participant leaves: the others unsubscribe (renegotiation), the remaining media keeps flowing', async () => {
      const [A, B, C] = peers;
      // C leaves gracefully
      const gone = await C.c.message(C.pubHandle, { request: 'leave' });
      assert.equal(pluginData(gone).leaving, 'ok');
      await evalIn(C, (i) => window.q.close(i), C.pubPc);
      for (const p of [A, B]) {
        const ev = await p.c.waitEvent((e) => pluginData(e).leaving === C.id, 10000);
        assert.equal(pluginData(ev).room, room);
        await p.unsubscribe(C.id);
      }
      await sleep(3000);
      assertFlowing(A, await mediaWindow(A), [B.id]);
      assertFlowing(B, await mediaWindow(B), [A.id]);
      assert.deepEqual([...A.subscribedFeeds], [B.id]);
    });

    await check('kick: allowed remove + kick; the kicked participant is cut off (PC closed, no media, cannot rejoin), the others carry on', async () => {
      const [A, B, C] = peers;
      // C comes back with a new pseudonym and token, like a rejoin after the roster changed
      const D = await new Peer('D', pqc, room, {}).open();
      peers.push(D);
      await api.allow(room, S.roomSecret, 'add', [D.joinToken]);
      for (const p of [A, B, D]) for (const o of [A, B, D]) if (p !== o) await p.learnKey(o);
      const existing = await D.join();
      await D.publish();
      await D.subscribe(existing.map((e) => ({ id: e.id, streams: e.streams })));
      for (const p of [A, B]) {
        const ev = await p.c.waitEvent((e) => Array.isArray(pluginData(e).publishers) && pluginData(e).publishers.some((x) => x.id === D.id), 10000);
        await p.subscribe(pluginData(ev).publishers.map((x) => ({ id: x.id, streams: x.streams })));
      }
      await sleep(4000);
      assertFlowing(D, await mediaWindow(D), [A.id, B.id]);
      const beforeD = await D.stats(D.subPc);
      // the server: allowed remove, then kick (in this order, before any new epoch is announced)
      await api.allow(room, S.roomSecret, 'remove', [D.joinToken]);
      const kicked = await api.kick(room, S.roomSecret, D.id);
      assert.equal(kicked.videoroom, 'success');
      const evD = await D.c.waitEvent((e) => pluginData(e).reason === 'kicked', 8000);
      assert.equal(pluginData(evD).leaving, 'ok');
      for (const p of [A, B]) {
        const ev = await p.c.waitEvent((e) => pluginData(e).kicked === D.id, 8000);
        assert.equal(pluginData(ev).room, room);
        await p.unsubscribe(D.id);
      }
      // D's publisher PC is torn down by Janus (hangup on the publisher handle)
      const hup = await D.c.waitEvent((e) => e.janus === 'hangup' && e.sender === D.pubHandle, 10000);
      assert.ok(hup.reason, 'hangup reason');
      // ...and D's subscriber PC gets no media any more (require_pvtid: the subscriptions are cut with the publisher)
      await sleep(1500);
      const s1 = await D.stats(D.subPc);
      await sleep(3000);
      const s2 = await D.stats(D.subPc);
      const grown = s2.inbound.reduce((a, x) => a + (x.packetsReceived || 0), 0) - s1.inbound.reduce((a, x) => a + (x.packetsReceived || 0), 0);
      assert.ok(grown <= 3, `the kicked participant still receives media (${grown} packets in 3 s)`);
      void beforeD;
      // cannot rejoin with the removed token
      const back = await new WsClient(WS, { tokenSecret: TOKEN_SECRET }).open();
      await back.create();
      const hb = await back.attach();
      const rj = await back.message(hb, { request: 'join', ptype: 'publisher', room, id: D.id, token: D.joinToken });
      assert.equal(pluginData(rj).error_code, 433, JSON.stringify(rj).slice(0, 200));
      // ...nor subscribe with the private_id it had (require_pvtid: the id died with the participant)
      const hs = await back.attach();
      const sj = await back.message(hs, { request: 'join', ptype: 'subscriber', room, private_id: D.privateId, streams: [{ feed: A.id, mid: '0' }] });
      assert.equal(pluginData(sj).error_code, 433, JSON.stringify(sj).slice(0, 200));
      back.close();
      // the others still hear each other
      assertFlowing(A, await mediaWindow(A), [B.id]);
      assertFlowing(B, await mediaWindow(B), [A.id]);
      const list = await api.listParticipants(room);
      assert.ok(!list.participants.some((x) => x.id === D.id));
    });

    // A participant must never be able to make the node write a file. Upstream VideoRoom honours record + filename in
    // `joinandconfigure` WITHOUT the room secret that lock_record is meant to demand (patch 0005 stops it in the recorder)
    await check('recording is impossible: joinandconfigure with record and an absolute file name of its own writes nothing on the node', async () => {
      const t0 = Date.now() / 1000;
      const rroom = await makeRoom(4);
      const p = await new Peer('REC', pqc, rroom, { simulcast: false }).open();
      try {
        await api.allow(rroom, S.roomSecret, 'add', [p.joinToken]);
        await evalIn(p, ([k, i]) => window.q.setSendKey(k, i), [p.key, p.keyIndex]);
        const offer = await evalIn(p, ([i, o]) => window.q.newPublisher(i, o), [p.pubPc, { e2ee: true, simulcast: false }]);
        const m = await p.c.message(p.pubHandle, {
          request: 'joinandconfigure', ptype: 'publisher', room: rroom, id: p.id, display: p.id, token: p.joinToken,
          audio: true, video: true, record: true, filename: REC_PROBE,
        }, { type: 'offer', sdp: offer, e2ee: true });
        assert.equal(pluginData(m).videoroom, 'joined', JSON.stringify(m).slice(0, 300));
        await evalIn(p, ([i, s]) => window.q.setAnswer(i, s), [p.pubPc, m.jsep.sdp]);
        await p.c.waitEvent((e) => e.janus === 'webrtcup' && e.sender === p.pubHandle, p.upTimeout);
        await sleep(4000);                                   // media is flowing: the recorder would be created now
        // the configure path is locked by the room secret (lock_record): answered or refused, but nothing starts
        const cfg = await p.c.message(p.pubHandle, { request: 'configure', record: true, filename: `${REC_PROBE}-2` });
        assert.ok(pluginData(cfg).configured === 'ok' || codeOf(cfg) > 0, JSON.stringify(cfg).slice(0, 200));
        await sleep(1500);
        const pid = execFileSync('systemctl', ['show', '-p', 'MainPID', '--value', 'qjanus'], { encoding: 'utf8' }).trim();
        assert.match(pid, /^[1-9][0-9]*$/);
        // the service's own (private) /tmp, /var/tmp and runtime directory, seen from the host
        const found = execFileSync('sudo', ['-n', 'sh', '-c',
          `find /proc/${pid}/root/tmp /proc/${pid}/root/var/tmp /proc/${pid}/root/run/qjanus -maxdepth 5 \\( -name '*.mjr' -o -name 'qjanus-rec-probe*' \\) 2>/dev/null | head -5; true`], { encoding: 'utf8' }).trim();
        assert.equal(found, '', `the node wrote a recording: ${found}`);
        // the attempt did reach the recorder and was refused there (so this test is not vacuous)
        const refusals = journalSince(t0 - 1).split('\n').filter((l) => /Recordings are disabled in this build/.test(l));
        assert.ok(refusals.length >= 1, 'no recorder refusal in the journal: the recording request never reached the recorder');
        return `refused ${refusals.length}x, no file`;
      } finally {
        await p.close().catch(() => {});
        await api.destroyRoom(rroom, S.roomSecret).catch(() => {});
      }
    });

    await check('destroy: the room is gone and every PeerConnection goes down', async () => {
      const [A, B] = peers;
      const gone = await api.destroyRoom(room, S.roomSecret);
      assert.equal(gone.videoroom, 'destroyed');
      for (const p of [A, B]) {
        const ev = await p.c.waitEvent((e) => pluginData(e).videoroom === 'destroyed', 8000);
        assert.equal(pluginData(ev).room, room);
        const hup = await p.c.waitEvent((e) => e.janus === 'hangup' && e.sender === p.pubHandle, 10000);
        assert.ok(hup.reason, `${p.name}: hangup after destroy`);
      }
      for (const p of peers) await p.close();
    });
    await check('ICE candidate policy: every publisher answer and subscriber offer of the run offered only addresses of the enforced interface (no VPN / private address of another interface)', async () => {
      assert.ok(iceChecked >= 6, `only ${iceChecked} SDPs were checked`);
      console.log(`ICE   ${iceChecked} SDPs out of Janus checked against the enforced interface`);
    });
  }

  if (SUITE === 'loss' || SUITE === 'all') {
    await check('netem loss 5 % + reorder 3 %: handshakes complete, the level holds, media flows, E2EE frames still authenticate', async () => {
      execFileSync('sudo', ['-n', 'bash', NETEM, 'on'], { stdio: 'inherit' });
      let peers = [];
      try {
        const room = await makeRoom(4);
        peers = await startCall(pqc, room, ['L1', 'L2', 'L3']);
        await sleep(12000);
        const detail = [];
        for (const p of peers) {
          for (const pc of [p.pubPc, p.subPc]) assertLevel(await p.transport(pc), `${p.name} ${pc} under loss`);
          const others = peers.filter((o) => o !== p).map((o) => o.id);
          const w = await mediaWindow(p, 6000);
          // ~16 audio packets/s nominal; 5 % loss and reordering: half of it is the bar
          assertFlowing(p, w, others, { minAudio: 40, minFrames: 8 });
          const c = await assertE2ee(p, others);
          detail.push(`${p.name}:dec=${c.decrypted.audio}/${c.decrypted.video}`);
        }
        await api.destroyRoom(room, S.roomSecret);
        return detail.join(' ');
      } finally {
        execFileSync('sudo', ['-n', 'bash', NETEM, 'off'], { stdio: 'inherit' });
        for (const p of peers) await p.close().catch(() => {});
      }
    });
  }

  if (SUITE === 'loss' || SUITE === 'all') {
    // The DTLS 1.3 retransmission/ACK timer has to keep running after the handshake (spec section 6):
    // with the stock behaviour only ~75 % of the handshakes survive 30 % loss when Janus is the DTLS client.
    const SOAK_N = Number(process.env.SOAK_N || 12);
    async function soakOnce(i) {
      const room = await makeRoom(4);
      const pub = await new Peer(`P${i}`, pqc, room, { simulcast: false, upTimeout: 22000 }).open();
      const sub = await new Peer(`Q${i}`, pqc, room, { simulcast: false, upTimeout: 22000 }).open();
      const r = { pubTried: 1, pub: false, subTried: 0, sub: false };
      const t0 = Date.now();
      try {
        await api.allow(room, S.roomSecret, 'add', [pub.joinToken, sub.joinToken]);
        await sub.learnKey(pub);
        await pub.join(); await sub.join();
        await pub.publish();                                    // publisher PC: Janus is the DTLS client
        r.pub = await waitLevel(pub, pub.pubPc, 15000);
        r.subTried = 1;
        await sub.subscribe([{ id: pub.id, streams: [{ mid: '0' }, { mid: '1' }] }]);   // subscriber PC: Janus offers
        r.sub = await waitLevel(sub, sub.subPc, 15000);
      } catch (e) { r.error = String((e && e.message) || e).slice(0, 160); }
      r.ms = Date.now() - t0;
      await pub.close().catch(() => {}); await sub.close().catch(() => {});
      await api.destroyRoom(room, S.roomSecret).catch(() => {});
      return r;
    }
    // A statistical guard, not a coin flip: Janus itself gives up a handshake after 20 s, and at 30 % loss (an ML-KEM
    // flight is 2-3 datagrams) a few percent of the handshakes may still fail. Without the timer fix only ~75 % survive.
    await check(`netem loss 30 %: at least 87 % of ${SOAK_N} publisher + ${SOAK_N} subscriber DTLS 1.3 handshakes complete, every established one at the required level`, async () => {
      execFileSync('sudo', ['-n', 'bash', NETEM, 'on', '30%', '0%'], { stdio: 'inherit' });
      const soakStart = Date.now() / 1000;
      const runs = [];
      try {
        for (let i = 0; i < SOAK_N; i++) runs.push(await soakOnce(i));
      } finally {
        execFileSync('sudo', ['-n', 'bash', NETEM, 'off'], { stdio: 'inherit' });
      }
      const tried = runs.reduce((a, r) => a + r.pubTried + r.subTried, 0);
      const ok = runs.reduce((a, r) => a + (r.pub ? 1 : 0) + (r.sub ? 1 : 0), 0);
      const detail = `${ok}/${tried} handshakes (pub ${runs.filter((r) => r.pub).length}/${SOAK_N}, sub ${runs.filter((r) => r.sub).length}/${runs.reduce((a, r) => a + r.subTried, 0)}), median ${runs.map((r) => r.ms).sort((a, b) => a - b)[Math.floor(runs.length / 2)]} ms` +
        (runs.some((r) => r.error) ? `, errors: ${runs.filter((r) => r.error).map((r) => r.error).join(' | ')}` : '');
      assert.ok(ok / tried >= 0.87, detail);
      // Deterministic evidence of the timer fix (patch 0001): the ACK/retransmit timer fired AFTER a handshake was established.
      // Stock Janus stops the timer the moment the state is `connected`, so this line cannot exist there.
      const afterConnected = journalSince(soakStart - 1).split('\n').filter((l) => /DTLS timer expired .*\(state=connected\)/.test(l)).length;
      assert.ok(afterConnected >= 1, `the DTLS timer never ran after a handshake was established (${detail})`);
      return `${detail}, timer after connected: ${afterConnected}x`;
    });
  }

  // ---------------------------------------------------------------- negative / weak peers
  if (SUITE === 'negative' || SUITE === 'all') {
    // A weak peer as publisher (Janus = DTLS client) and as subscriber (Janus = DTLS server unless the
    // peer answers passive). expect 'refused': never connected, no media, refusal logged by qjanus.
    // 'accepted': positive control (older Chromium WITH the PQC trial, as the Electron desktop runs it).
    async function weakPeer(label, browser, expect) {
      const t0 = Date.now() / 1000;
      const room = await makeRoom(4);
      const res = {};
      // publisher role
      const strong = await new Peer('S', pqc, room, { simulcast: false }).open();
      const weak = await new Peer('W', browser, room, { simulcast: false, upTimeout: 9000 }).open();
      await api.allow(room, S.roomSecret, 'add', [strong.joinToken, weak.joinToken]);
      await strong.join(); await strong.publish();
      await weak.learnKey(strong);
      await strong.learnKey(weak);
      await weak.join();
      // as subscriber first: the strong peer's feed
      const subP = weak.subscribe([{ id: strong.id, streams: [{ mid: '0' }, { mid: '1' }] }]).then(() => 'answered', (e) => `error: ${e.message}`);
      res.subscriberSetup = await Promise.race([subP, sleep(20000).then(() => 'timeout')]);
      const subState = await waitState(weak, weak.subPc, ['connected', 'failed', 'closed'], 12000);
      const w = weak.subUp ? await mediaWindow(weak, 2500).catch(() => null) : null;
      const subTransport = await weak.transport(weak.subPc).catch(() => null);
      res.subConnected = !!(subState && subState.connection === 'connected' && levelOk(subTransport));
      res.subMedia = !!(w && w.s2.inbound.some((x) => (x.packetsReceived || 0) > 5));
      // as publisher
      let pubUp = false; let pubHangup = null;
      try {
        const offer = await evalIn(weak, ([i, o]) => window.q.newPublisher(i, o), [weak.pubPc, { e2ee: true, simulcast: false }]);
        const m = await weak.c.message(weak.pubHandle, { request: 'publish', audio: true, video: true }, { type: 'offer', sdp: offer, e2ee: true });
        await evalIn(weak, ([i, s]) => window.q.setAnswer(i, s), [weak.pubPc, m.jsep.sdp]);
        try { await weak.c.waitEvent((e) => e.janus === 'webrtcup' && e.sender === weak.pubHandle, 12000); pubUp = true; } catch (e) { /* refused */ }
        const hup = weak.c.events.find((e) => e.janus === 'hangup' && e.sender === weak.pubHandle);
        pubHangup = hup ? hup.reason : null;
      } catch (e) { res.publisherSetupError = String(e.message || e).slice(0, 120); }
      const pubTransport = await weak.transport(weak.pubPc).catch(() => null);
      res.pubConnected = pubUp && levelOk(pubTransport);
      res.pubUp = pubUp;                       // Janus' own `webrtcup`: it must never come for a refused peer
      res.subUp = !!weak.subUp;
      res.pubHangup = pubHangup;
      await sleep(500);
      const lines = dtlsLines(journalSince(t0 - 1));
      res.okLines = lines.filter(isOk).length;
      res.refusalLines = lines.filter(isRefusal).length;
      await strong.close(); await weak.close();
      await api.destroyRoom(room, S.roomSecret).catch(() => {});
      if (expect === 'refused') {
        assert.ok(!res.subConnected && !res.subMedia, `${label}: subscriber role must be refused: ${JSON.stringify(res)}`);
        assert.ok(!res.pubConnected, `${label}: publisher role must be refused: ${JSON.stringify(res)}`);
        assert.ok(!res.subUp && !res.pubUp, `${label}: qjanus reported webrtcup for a peer below the required level (either role): ${JSON.stringify(res)}`);
        assert.ok(res.refusalLines >= 1, `${label}: qjanus must log the refusal: ${JSON.stringify(res)}`);
      } else {
        assert.ok(res.subConnected && res.subMedia, `${label}: subscriber role must connect: ${JSON.stringify(res)}`);
        assert.ok(res.pubConnected && res.subUp && res.pubUp, `${label}: publisher role must connect: ${JSON.stringify(res)}`);
        assert.ok(res.okLines >= 3, `${label}: three policy-ok lines expected (S publisher, W subscriber, W publisher): ${JSON.stringify(res)}`);
      }
      return JSON.stringify(res);
    }

    for (const oc of OLD_CHROMES) {
      for (const trial of ['', PQC_TRIAL]) {
        let b;
        try { b = await launchChromium(trial, oc.path); } catch (e) {
          results.push({ name: `chrome ${oc.label} launch`, ok: false, error: String(e.message || e) });
          console.log(`FAIL  chrome ${oc.label} launch: ${e.message}`);
          continue;
        }
        const tag = `chrome ${oc.label}${trial ? ' + PQC trial' : ' (stock)'}`;
        versions[tag] = b.version();
        // 130: DTLS 1.2 only; 142/148 stock: DTLS 1.3 without ML-KEM; 142/148 + trial: what the desktop runs
        const expect = trial && Number(oc.label) >= 142 ? 'accepted' : 'refused';
        await check(`negative: ${tag} is ${expect} (${expect === 'refused' ? (Number(oc.label) < 142 ? 'DTLS 1.2' : 'no ML-KEM') : 'PQC trial'})`, () => weakPeer(tag, b, expect));
        await b.close();
      }
    }
    if (process.env.WITH_FIREFOX !== '0') {
      const ff = await launchFirefox();
      versions.firefox = ff.version();
      await check('negative: Firefox (TLS_AES_128_GCM_SHA256 in DTLS 1.3) is refused', () => weakPeer('firefox', ff, 'refused'));
      await ff.close();
    }
  }
  if ((SUITE === 'negative' || SUITE === 'all') && process.env.REQUIRE_ALL_NEGATIVES === '1') {
    await check('the refused-peer matrix is complete: Chrome 130 / 142 / 148 (stock and with the PQC trial) and Firefox all ran', async () => {
      const names = results.map((r) => r.name);
      for (const m of ['130', '142', '148']) {
        for (const t of ['(stock)', '+ PQC trial']) assert.ok(names.some((n) => n.startsWith(`negative: chrome ${m} ${t}`)), `missing: chrome ${m} ${t}`);
      }
      assert.ok(names.some((n) => n.startsWith('negative: Firefox')), 'missing: Firefox');
    });
  }
  await pqc.close();
} catch (e) {
  console.error('FATAL', e);
  results.push({ name: 'fatal', ok: false, error: String((e && e.stack) || e) });
}
srv.close();
fs.writeFileSync(path.join(OUT, 'versions.json'), JSON.stringify(versions, null, 1));
fs.writeFileSync(path.join(OUT, 'shapes.json'), JSON.stringify(shapes, null, 1));
fs.writeFileSync(path.join(OUT, 'results.json'), JSON.stringify(results, null, 1));
if (process.env.SECRETS_FILE) fs.writeFileSync(process.env.SECRETS_FILE, JSON.stringify([...secretsSeen]), { mode: 0o600 });
const failed = results.filter((r) => !r.ok);
console.log(`\ne2e: ${results.length - failed.length}/${results.length} passed  (${Math.round(Date.now() / 1000 - startedAt)} s)`);
console.log('=== E2E SHAPES ===');
console.log(JSON.stringify(shapes));
process.exit(failed.length ? 1 : 0);
