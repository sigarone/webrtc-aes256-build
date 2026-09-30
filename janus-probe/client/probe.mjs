// Drives headless browsers against a local, patched Janus and records what
// transport level was really negotiated. Signalling is done here in Node
// (Janus HTTP API); the pages only hold the RTCPeerConnections.
import { chromium, firefox } from 'playwright';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { execSync } from 'node:child_process';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const JANUS = process.env.JANUS_URL || 'http://127.0.0.1:8088/janus';
const JANUS_LOG = process.env.JANUS_LOG || '';
const OUT = process.env.OUT_DIR || path.join(HERE, 'out');
const PORT = 8199;
const PAGE = `http://127.0.0.1:${PORT}/page.html`;
const ONLY = (process.env.ONLY || '').split(',').filter(Boolean);
const SUITE = process.env.SUITE || 'main';
// JSON: [{"label":"130","path":"/path/to/chrome"}, ...]  older Chrome-for-Testing builds
const OLD_CHROMES = JSON.parse(process.env.OLD_CHROMES || '[]');
fs.mkdirSync(OUT, { recursive: true });

const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const rnd = () => Math.random().toString(36).slice(2, 12);

// ------------------------------------------------------------------ Janus API
class JanusClient {
  constructor(base) { this.base = base; this.events = []; this.waiters = []; this.trickle = {}; this.running = false; }
  async req(p, body) {
    const r = await fetch(this.base + p, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ ...body, transaction: rnd() }),
    });
    return r.json();
  }
  async create() {
    const j = await this.req('', { janus: 'create' });
    if (!j.data) throw new Error('create failed: ' + JSON.stringify(j));
    this.sid = j.data.id;
    this.running = true;
    this.poll();
  }
  poll() {
    (async () => {
      while (this.running) {
        try {
          const r = await fetch(`${this.base}/${this.sid}?maxev=1`);
          const j = await r.json();
          for (const e of Array.isArray(j) ? j : [j]) this.push(e);
        } catch (e) { await sleep(200); }
      }
    })();
  }
  push(e) {
    if (e.janus === 'keepalive' || e.janus === 'ack') return;
    if (e.janus === 'trickle' && this.trickle[e.sender]) this.trickle[e.sender](e.candidate);
    this.events.push(e);
    for (const w of [...this.waiters]) w();
  }
  async attach(plugin) {
    const j = await this.req('/' + this.sid, { janus: 'attach', plugin });
    if (!j.data) throw new Error('attach failed: ' + JSON.stringify(j));
    return j.data.id;
  }
  send(handle, body, jsep) {
    return this.req(`/${this.sid}/${handle}`, { janus: 'message', body, ...(jsep ? { jsep } : {}) });
  }
  waitFor(pred, ms = 15000) {
    return new Promise((resolve, reject) => {
      const t = setTimeout(() => { cleanup(); reject(new Error('timeout waiting for janus event')); }, ms);
      const check = () => {
        const i = this.events.findIndex(pred);
        if (i >= 0) { const [e] = this.events.splice(i, 1); cleanup(); resolve(e); }
      };
      const cleanup = () => { clearTimeout(t); this.waiters = this.waiters.filter((w) => w !== check); };
      this.waiters.push(check);
      check();
    });
  }
  async destroy() {
    this.running = false;
    try { await this.req('/' + this.sid, { janus: 'destroy' }); } catch (e) { /* ignore */ }
  }
}
const pluginData = (e) => (e.plugindata && e.plugindata.data) || {};

// ----------------------------------------------------------- static page host
function startServer() {
  const srv = http.createServer((req, res) => {
    const f = req.url.split('?')[0] === '/worker.js' ? 'worker.js' : 'page.html';
    res.writeHead(200, { 'content-type': f.endsWith('.js') ? 'text/javascript' : 'text/html' });
    res.end(fs.readFileSync(path.join(HERE, f)));
  });
  return new Promise((r) => srv.listen(PORT, '127.0.0.1', () => r(srv)));
}

// --------------------------------------------------------------- Janus log
function logSize() { try { return fs.statSync(JANUS_LOG).size; } catch (e) { return 0; } }
function logSince(off) {
  try { return fs.readFileSync(JANUS_LOG).subarray(off).toString('utf8'); } catch (e) { return ''; }
}
function policyLines(text) {
  return text.split('\n')
    .filter((l) => /DTLS-POLICY|Handshake error|DTLS timer expired|DTLS handshake failure|DTLS timeout|taking too much time|Fingerprint is NOT/.test(l))
    .map((l) => l.replace(/\[\d+\]/g, '[h]').replace(/\d{1,3}(\.\d{1,3}){3}/g, '<ip>').replace(/^\[[^\]]*\] /, '').trim());
}
const isOkLine = (l) => /DTLS-POLICY/.test(l) && /version=0xfefc cipher=0x1302 srtp=0x0008 group=0x11ec/.test(l) && /ok=1/.test(l);
const isFailLine = (l) => /ok=0|Handshake error|DTLS handshake failure|taking too much time|DTLS timeout/.test(l);
const isTimerLine = (l) => /DTLS timer expired/.test(l);

// ------------------------------------------------------------ browser helpers
const BASE_ARGS = [
  '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream',
  '--disable-features=WebRtcHideLocalIpsWithMdns',
  '--allow-loopback-in-peer-connection', '--no-sandbox',
  '--autoplay-policy=no-user-gesture-required',
];
async function launchChromium(fieldTrials, executablePath) {
  const args = [...BASE_ARGS];
  if (fieldTrials) args.push(`--force-fieldtrials=${fieldTrials}`);
  return chromium.launch({ headless: true, args, ...(executablePath ? { executablePath } : {}) });
}
async function launchFirefox() {
  return firefox.launch({
    headless: true,
    firefoxUserPrefs: {
      'media.navigator.streams.fake': true,
      'media.navigator.permission.disabled': true,
      'media.peerconnection.ice.loopback': true,
      'media.peerconnection.ice.obfuscate_host_addresses': false,
    },
  });
}
async function newPage(ctx) {
  const page = await ctx.newPage();
  await page.goto(PAGE);
  return page;
}
async function waitConnected(page, id, ms = 15000) {
  const end = Date.now() + ms;
  let last = null;
  while (Date.now() < end) {
    last = await page.evaluate((i) => window.probe.state(i), id);
    if (last && (last.connection === 'connected' || last.connection === 'failed')) return last;
    await sleep(250);
  }
  return last;
}
function connectMs(state) {
  if (!state || !state.timeline) return null;
  const a = state.timeline.find((x) => /^ice:checking/.test(x.what));
  const b = state.timeline.find((x) => x.what === 'connection:connected');
  return a && b ? b.t - a.t : null;
}
const sumIn = (s, kind, f) => s.inbound.filter((x) => x.kind === kind).reduce((a, x) => a + (x[f] || 0), 0);
const sumOut = (s, kind, f) => s.outbound.filter((x) => x.kind === kind).reduce((a, x) => a + (x[f] || 0), 0);

async function statsWindow(page, id, ms = 2500) {
  const s1 = await page.evaluate((i) => window.probe.stats(i), id);
  await sleep(ms);
  const s2 = await page.evaluate((i) => window.probe.stats(i), id);
  return { s1, s2 };
}
function flow(w, dir) {
  const f = dir === 'in' ? sumIn : sumOut;
  const pk = dir === 'in' ? 'packetsReceived' : 'packetsSent';
  return {
    audioPackets: f(w.s2, 'audio', pk) - f(w.s1, 'audio', pk),
    videoPackets: f(w.s2, 'video', pk) - f(w.s1, 'video', pk),
    audioTotal: f(w.s2, 'audio', pk),
    videoTotal: f(w.s2, 'video', pk),
    videoFramesDecoded: dir === 'in' ? sumIn(w.s2, 'video', 'framesDecoded') : undefined,
    videoFramesEncoded: dir === 'out' ? sumOut(w.s2, 'video', 'framesEncoded') : undefined,
    audioSamples: dir === 'in' ? sumIn(w.s2, 'audio', 'totalSamplesReceived') : undefined,
  };
}
const hasMedia = (f) => !!(f && f.audioPackets > 0 && f.videoPackets > 0);
const transportOf = (s) => (s.transport && s.transport[0]) || {};
const EXPECT = { tlsVersion: 'FEFC', dtlsCipher: 'TLS_AES_256_GCM_SHA384' };
// older Chrome builds report the SRTP profile as AEAD_AES_256_GCM, newer ones as SRTP_AEAD_AES_256_GCM
const SRTP_OK = ['AEAD_AES_256_GCM', 'SRTP_AEAD_AES_256_GCM'];
function levelOk(t) {
  return !!t && t.tlsVersion === EXPECT.tlsVersion && t.dtlsCipher === EXPECT.dtlsCipher &&
    SRTP_OK.includes(t.srtpCipher) && t.dtlsState === 'connected';
}
const round1 = (ms) => Math.round(ms / 100) / 10;

// firewall helper: drop a fraction of the UDP packets delivered over loopback
// (same-host test: the browser <-> Janus datagrams all cross lo)
async function withLoss(prob, fn) {
  if (!prob) return fn();
  const rule = `INPUT -i lo -p udp -m statistic --mode random --probability ${prob} -j DROP`;
  execSync(`sudo iptables -I ${rule}`);
  try { return await fn(); } finally { execSync(`sudo iptables -D ${rule}`); }
}

// ------------------------------------------------------------------- scenarios
// Janus is the DTLS client when the browser is the offerer (echotest / publisher)
async function echoScenario(browser, spec) {
  const { name, expect, loss } = spec;
  const t0 = Date.now(); const off = logSize();
  const res = { name, expect, kind: 'echotest', janusRole: 'client', loss: loss || 0 };
  const ctx = await browser.newContext();
  const janus = new JanusClient(JANUS);
  try {
    const page = await newPage(ctx);
    res.pageInfo = await page.evaluate(() => window.probe.info());
    await janus.create();
    const h = await janus.attach('janus.plugin.echotest');
    janus.trickle[h] = (c) => page.evaluate(([i, cc]) => window.probe.addCandidate(i, cc), ['p', c]).catch(() => {});
    await withLoss(loss, async () => {
      const offer = await page.evaluate((o) => window.probe.makeOffer('p', o), { audio: true, video: true });
      await janus.send(h, { audio: true, video: true }, { type: 'offer', sdp: offer });
      const ev = await janus.waitFor((e) => e.sender === h && e.jsep, 15000);
      await page.evaluate(([i, s]) => window.probe.setAnswer(i, s), ['p', ev.jsep.sdp]);
      res.state = await waitConnected(page, 'p', loss ? 25000 : 15000);
      await sleep(3000);
    });
    const w = await statsWindow(page, 'p');
    res.transport = transportOf(w.s2);
    res.inbound = flow(w, 'in');
    res.outbound = flow(w, 'out');
    res.state = await page.evaluate(() => window.probe.state('p'));
    res.connectMs = connectMs(res.state);
    await janus.send(h, { request: 'leave' }).catch(() => {});
  } catch (e) { res.error = String(e.message || e); }
  await janus.destroy();
  await ctx.close();
  await sleep(700);
  res.janusLog = policyLines(logSince(off));
  res.seconds = round1(Date.now() - t0);
  res.timerExpiredLines = res.janusLog.filter(isTimerLine).length;
  const okLines = res.janusLog.filter(isOkLine).length;
  const failLines = res.janusLog.filter(isFailLine).length;
  const media = loss ? !!(res.inbound && res.inbound.audioPackets > 0) : hasMedia(res.inbound);
  res.accepted = !!(levelOk(res.transport) && media && okLines >= 1);
  res.refused = !media && okLines === 0;
  res.refusalLogged = failLines >= 1;
  res.consistent = res.accepted || res.refused;
  res.outcome = res.accepted ? 'accepted' : (res.refused ? 'refused' : 'INCONSISTENT');
  res.ok = expect === 'pass' ? res.accepted : expect === 'fail' ? res.refused : res.consistent;
  return res;
}

// publisher in `browser`, subscriber in `subBrowser` (default: same browser)
async function roomScenario(browser, spec) {
  const { name, roleS, room, e2ee, simulcast, expect, loss, subBrowser } = spec;
  const t0 = Date.now(); const off = logSize();
  const res = {
    name, expect, kind: 'videoroom', room: room === 2345 ? 'e2ee-required' : 'plain', e2ee: !!e2ee,
    simulcast: !!simulcast, loss: loss || 0,
    subscriberChromiumRole: roleS === 'passive' ? 'server (janus client)' : 'client (janus server)',
  };
  const ctxP = await browser.newContext();
  const ctxS = subBrowser ? await subBrowser.newContext() : ctxP;
  const janus = new JanusClient(JANUS);
  try {
    const pa = await newPage(ctxP); const pb = await newPage(ctxS);
    res.pageInfo = await pa.evaluate(() => window.probe.info());
    if (subBrowser) res.subPageInfo = await pb.evaluate(() => window.probe.info());
    await janus.create();
    await withLoss(loss, async () => {
      // publisher
      const hp = await janus.attach('janus.plugin.videoroom');
      janus.trickle[hp] = (c) => pa.evaluate(([i, cc]) => window.probe.addCandidate(i, cc), ['pub', c]).catch(() => {});
      await janus.send(hp, { request: 'join', ptype: 'publisher', room, display: 'pub' });
      const joined = await janus.waitFor((e) => e.sender === hp && pluginData(e).videoroom === 'joined', 10000);
      const feed = pluginData(joined).id;
      const offer = await pa.evaluate((o) => window.probe.makeOffer('pub', o),
        { audio: true, video: true, e2ee, simulcast });
      await janus.send(hp, { request: 'publish', audio: true, video: true },
        { type: 'offer', sdp: offer, ...(e2ee ? { e2ee: true } : {}) });
      const ans = await janus.waitFor((e) => e.sender === hp && e.jsep, 15000);
      await pa.evaluate(([i, s]) => window.probe.setAnswer(i, s), ['pub', ans.jsep.sdp]);
      res.publisherState = await waitConnected(pa, 'pub', loss ? 25000 : 15000);
      // subscriber
      const hs = await janus.attach('janus.plugin.videoroom');
      janus.trickle[hs] = (c) => pb.evaluate(([i, cc]) => window.probe.addCandidate(i, cc), ['sub', c]).catch(() => {});
      await janus.send(hs, { request: 'join', ptype: 'subscriber', room, streams: [{ feed }] });
      const att = await janus.waitFor((e) => e.sender === hs && e.jsep, 15000);
      res.subscriberJsepE2ee = !!att.jsep.e2ee;
      const answer = await pb.evaluate(([s, o]) => window.probe.makeAnswer('sub', s, o),
        [att.jsep.sdp, { setup: roleS, e2ee }]);
      await janus.send(hs, { request: 'start', room }, { type: 'answer', sdp: answer });
      res.subscriberState = await waitConnected(pb, 'sub', loss ? 25000 : 15000);
      await sleep(4000);
      if (simulcast) {
        // which layers does the publisher really send, and can the subscriber switch?
        const wp0 = await pa.evaluate(() => window.probe.stats('pub'));
        res.simulcastPublisherLayers = wp0.outbound.filter((x) => x.kind === 'video')
          .map((x) => ({ rid: x.rid, w: x.frameWidth, h: x.frameHeight, packets: x.packetsSent }));
        const m = /m=video[\s\S]*?a=mid:(\S+)/.exec(att.jsep.sdp);
        const mid = m ? m[1] : '1';
        res.simulcastSubscriberWidth = {};
        for (const sub of [0, 2, 1]) {
          await janus.send(hs, { request: 'configure', streams: [{ mid, substream: sub }] });
          await sleep(3500);
          const s = await pb.evaluate(() => window.probe.stats('sub'));
          res.simulcastSubscriberWidth[sub] = s.inbound.filter((x) => x.kind === 'video')
            .map((x) => ({ w: x.frameWidth, h: x.frameHeight, decoded: x.framesDecoded }));
        }
      }
      const wp = await statsWindow(pa, 'pub');
      const wsub = await statsWindow(pb, 'sub');
      res.publisherTransport = transportOf(wp.s2);
      res.subscriberTransport = transportOf(wsub.s2);
      res.publisherOut = flow(wp, 'out');
      res.subscriberIn = flow(wsub, 'in');
      res.subscriberVideoDecoded = sumIn(wsub.s2, 'video', 'framesDecoded');
      res.subscriberState = await pb.evaluate(() => window.probe.state('sub'));
      res.publisherState = await pa.evaluate(() => window.probe.state('pub'));
      res.publisherConnectMs = connectMs(res.publisherState);
      res.subscriberConnectMs = connectMs(res.subscriberState);
      if (e2ee) {
        res.workerCountersPublisher = await pa.evaluate(() => window.probe.workerCounters());
        res.workerCountersSubscriber = await pb.evaluate(() => window.probe.workerCounters());
      }
      await janus.send(hs, { request: 'leave' }).catch(() => {});
      await janus.send(hp, { request: 'leave' }).catch(() => {});
    });
  } catch (e) { res.error = String(e.message || e); }
  await janus.destroy();
  await ctxP.close();
  if (subBrowser) await ctxS.close();
  await sleep(700);
  res.janusLog = policyLines(logSince(off));
  res.seconds = round1(Date.now() - t0);
  res.timerExpiredLines = res.janusLog.filter(isTimerLine).length;
  // Janus is the DTLS client for the publisher and for a subscriber that answers passive,
  // and the DTLS server for a subscriber that answers active (default)
  const okLines = res.janusLog.filter(isOkLine).length;
  const failLines = res.janusLog.filter(isFailLine).length;
  const pubOk = levelOk(res.publisherTransport) && (loss ? !!(res.publisherOut && res.publisherOut.audioPackets > 0) : hasMedia(res.publisherOut));
  const subMedia = loss ? !!(res.subscriberIn && res.subscriberIn.audioPackets > 0)
    : !!(hasMedia(res.subscriberIn) && res.subscriberVideoDecoded > 0);
  res.accepted = !!(pubOk && levelOk(res.subscriberTransport) && subMedia && okLines >= 2);
  res.refused = !!(pubOk && !subMedia && okLines === 1);
  res.refusalLogged = failLines >= 1;
  res.consistent = res.accepted || res.refused;
  res.outcome = res.accepted ? 'accepted' : (res.refused ? 'refused' : 'INCONSISTENT');
  res.ok = expect === 'pass' ? res.accepted : expect === 'fail' ? res.refused : res.consistent;
  if (e2ee && res.accepted) {
    const c = res.workerCountersSubscriber;
    const e2eeOk = !!c && c.receiver.video > 0 && c.receiver.audio > 0 &&
      c.integrityOk.video > 0 && c.integrityOk.audio > 0 && c.integrityBad.video === 0 && c.integrityBad.audio === 0 &&
      res.subscriberJsepE2ee;
    res.e2eeByteExact = e2eeOk;
    res.ok = res.ok && e2eeOk;
  }
  if (simulcast && res.accepted) {
    const widths = Object.values(res.simulcastSubscriberWidth || {}).map((a) => (a[0] && a[0].w) || 0);
    res.simulcastLayersSeen = (res.simulcastPublisherLayers || []).filter((l) => l.packets > 0).length;
    res.simulcastSwitchWorks = new Set(widths.filter(Boolean)).size >= 2;
    res.ok = res.ok && res.simulcastLayersSeen >= 3;
  }
  return res;
}


// ---------------------------------------------------------------- loss soak
// Repeated publisher+subscriber handshakes under random UDP loss: how often do
// both DTLS 1.3 handshakes complete (Janus is client for the publisher and
// server for the subscriber), and how often did the retry timer have to act?
async function soakOnce(browser, timeoutMs) {
  const off = logSize(); const t0 = Date.now(); const r = {};
  const ctx = await browser.newContext();
  const janus = new JanusClient(JANUS);
  try {
    const pa = await newPage(ctx); const pb = await newPage(ctx);
    await janus.create();
    const hp = await janus.attach('janus.plugin.videoroom');
    janus.trickle[hp] = (c) => pa.evaluate(([i, cc]) => window.probe.addCandidate(i, cc), ['pub', c]).catch(() => {});
    await janus.send(hp, { request: 'join', ptype: 'publisher', room: 1234, display: 'pub' });
    const joined = await janus.waitFor((e) => e.sender === hp && pluginData(e).videoroom === 'joined', 10000);
    const offer = await pa.evaluate((o) => window.probe.makeOffer('pub', o), { audio: true, video: true });
    await janus.send(hp, { request: 'publish', audio: true, video: true }, { type: 'offer', sdp: offer });
    const ans = await janus.waitFor((e) => e.sender === hp && e.jsep, 15000);
    await pa.evaluate(([i, s]) => window.probe.setAnswer(i, s), ['pub', ans.jsep.sdp]);
    const hs = await janus.attach('janus.plugin.videoroom');
    janus.trickle[hs] = (c) => pb.evaluate(([i, cc]) => window.probe.addCandidate(i, cc), ['sub', c]).catch(() => {});
    await janus.send(hs, { request: 'join', ptype: 'subscriber', room: 1234, streams: [{ feed: pluginData(joined).id }] });
    const att = await janus.waitFor((e) => e.sender === hs && e.jsep, 15000);
    const answer = await pb.evaluate(([s, o]) => window.probe.makeAnswer('sub', s, o), [att.jsep.sdp, {}]);
    await janus.send(hs, { request: 'start', room: 1234 }, { type: 'answer', sdp: answer });
    const [sp, ss] = await Promise.all([waitConnected(pa, 'pub', timeoutMs), waitConnected(pb, 'sub', timeoutMs)]);
    await sleep(1500);
    const tp = transportOf(await pa.evaluate(() => window.probe.stats('pub')));
    const ts = transportOf(await pb.evaluate(() => window.probe.stats('sub')));
    r.pubDtls = tp.dtlsState; r.subDtls = ts.dtlsState;
    r.pubLevelOk = levelOk(tp); r.subLevelOk = levelOk(ts);
    r.pubMs = connectMs(await pa.evaluate(() => window.probe.state('pub')));
    r.subMs = connectMs(await pb.evaluate(() => window.probe.state('sub')));
    r.pubConn = sp && sp.connection; r.subConn = ss && ss.connection;
  } catch (e) { r.error = String(e.message || e); }
  await janus.destroy();
  await ctx.close();
  await sleep(500);
  const lines = policyLines(logSince(off));
  r.okLines = lines.filter(isOkLine).length;
  r.timerConnected = lines.filter((l) => isTimerLine(l) && /state=connected/.test(l)).length;
  r.timerTrying = lines.filter((l) => isTimerLine(l) && /state=trying/.test(l)).length;
  r.seconds = round1(Date.now() - t0);
  return r;
}
const median = (a) => { const b = a.filter((x) => x != null).sort((x, y) => x - y); return b.length ? b[Math.floor(b.length / 2)] : null; };
async function soakScenario(browser, spec) {
  const { name, loss, n } = spec;
  const res = { name, kind: 'soak', loss, n, runs: [] };
  await withLoss(loss, async () => { for (let i = 0; i < n; i++) res.runs.push(await soakOnce(browser, 20000)); });
  const R = res.runs;
  res.bothDtlsConnected = R.filter((r) => r.pubLevelOk && r.subLevelOk).length;
  res.pubDtlsConnected = R.filter((r) => r.pubLevelOk).length;
  res.subDtlsConnected = R.filter((r) => r.subLevelOk).length;
  res.janusOkLines = R.reduce((a, r) => a + r.okLines, 0);
  res.timerExpiredConnected = R.reduce((a, r) => a + r.timerConnected, 0);
  res.timerExpiredTrying = R.reduce((a, r) => a + r.timerTrying, 0);
  res.pubMedianMs = median(R.map((r) => r.pubMs));
  res.subMedianMs = median(R.map((r) => r.subMs));
  res.errors = R.filter((r) => r.error).length;
  res.ok = true; // informational
  return res;
}

// ------------------------------------------------------------------------ main
const results = [];
const want = (n) => ONLY.length === 0 || ONLY.some((o) => n.startsWith(o));
async function run(fn, browser, spec) {
  if (!want(spec.name)) return;
  console.log('== ' + spec.name);
  let r;
  try { r = await fn(browser, spec); } catch (e) { r = { name: spec.name, ok: false, error: String((e && e.stack) || e) }; }
  console.log(JSON.stringify(r, null, 1));
  results.push(r);
  fs.writeFileSync(path.join(OUT, 'results.json'), JSON.stringify(results, null, 1));
}

const srv = await startServer();
const versions = {};
try {
  const pqc = await launchChromium('WebRTC-EnableDtlsPqc/Enabled/');
  versions.chromium = pqc.version();
  if (SUITE === 'soak') {
    await run(soakScenario, pqc, { name: 'SOAK-30pct-udp-loss', loss: 0.3, n: 10 });
    await run(soakScenario, pqc, { name: 'SOAK-50pct-udp-loss', loss: 0.5, n: 8 });
    await pqc.close();
  } else {
  // Chromium (Playwright build) with the PQC field trial, as our apps run -----
  await run(echoScenario, pqc, { name: 'A1-echotest', expect: 'pass' });
  await run(roomScenario, pqc, { name: 'B1-videoroom-pub=janus-client,sub=janus-server', room: 1234, expect: 'pass' });
  await run(roomScenario, pqc, { name: 'B2-videoroom-sub=janus-client', roleS: 'passive', room: 1234, expect: 'pass' });
  await run(roomScenario, pqc, { name: 'E1-videoroom-e2ee-xor', room: 2345, e2ee: true, expect: 'pass' });
  await run(roomScenario, pqc, { name: 'S1-videoroom-simulcast', room: 1234, simulcast: true, expect: 'pass' });
  await run(echoScenario, pqc, { name: 'L1-echotest-30pct-udp-loss', loss: 0.3, expect: 'pass' });
  await run(roomScenario, pqc, { name: 'L2-videoroom-30pct-udp-loss', room: 1234, loss: 0.3, expect: 'pass' });
  await run(roomScenario, pqc, { name: 'L3-videoroom-sub=janus-client-30pct-udp-loss', roleS: 'passive', room: 1234, loss: 0.3, expect: 'pass' });
  // stock Chromium without any field trial ------------------------------------
  const stock = await launchChromium('');
  await run(echoScenario, stock, { name: 'I1-stock-chromium-no-trial', expect: 'auto' });
  await stock.close();
  // weaker peers: must be refused, fail closed --------------------------------
  for (const oc of OLD_CHROMES) {
    for (const trial of ['', 'WebRTC-EnableDtlsPqc/Enabled/']) {
      let b;
      try { b = await launchChromium(trial, oc.path); } catch (e) {
        results.push({ name: `N-chrome${oc.label}-launch`, ok: false, error: String(e.message || e) }); continue;
      }
      const tag = `chrome${oc.label}${trial ? '+pqc-trial' : ''}`;
      versions[tag] = b.version();
      await run(echoScenario, b, { name: `N-${tag}-echotest(peer=dtls-server)`, expect: 'auto' });
      await run(roomScenario, pqc, { name: `N-${tag}-room-subscriber(peer=dtls-client)`, room: 1234, subBrowser: b, expect: 'auto' });
      await b.close();
    }
  }
  if (process.env.WITH_FIREFOX !== '0') {
    const ff = await launchFirefox();
    versions.firefox = ff.version();
    await run(echoScenario, ff, { name: 'N-firefox-echotest(peer=dtls-server)', expect: 'auto' });
    await run(roomScenario, pqc, { name: 'N-firefox-room-subscriber(peer=dtls-client)', room: 1234, subBrowser: ff, expect: 'auto' });
    await ff.close();
  }
  await pqc.close();
  }
} catch (e) {
  console.error('FATAL', e);
  results.push({ name: 'fatal', ok: false, error: String((e && e.stack) || e) });
}
srv.close();
fs.writeFileSync(path.join(OUT, 'versions.json'), JSON.stringify(versions, null, 1));
fs.writeFileSync(path.join(OUT, 'results.json'), JSON.stringify(results, null, 1));

// summary table
const fmt = (x) => (x && x.tlsVersion ? `${x.tlsVersion} ${x.dtlsCipher || '-'} ${x.srtpCipher || '-'}` : '-');
const rows = results.map((r) => {
  if (r.kind === 'soak') {
    return `SOAK | ${r.name} | n=${r.n} both-dtls-connected=${r.bothDtlsConnected} pub(janus=client)=${r.pubDtlsConnected} sub(janus=server)=${r.subDtlsConnected} janus-ok-lines=${r.janusOkLines} timer-expired(connected)=${r.timerExpiredConnected} timer-expired(trying)=${r.timerExpiredTrying} median-connect-ms pub=${r.pubMedianMs} sub=${r.subMedianMs} errors=${r.errors}`;
  }
  const t = r.transport || r.subscriberTransport || null;
  let med = '-';
  if (r.inbound) med = `a${r.inbound.audioPackets}/v${r.inbound.videoPackets}`;
  else if (r.subscriberIn) med = `a${r.subscriberIn.audioPackets}/v${r.subscriberIn.videoPackets}/dec${r.subscriberVideoDecoded}`;
  const extra = [];
  if (r.outcome) extra.push(r.outcome + (r.refused ? (r.refusalLogged ? '(logged)' : '(silent)') : ''));
  if (r.connectMs != null) extra.push(`conn${r.connectMs}ms`);
  if (r.subscriberConnectMs != null) extra.push(`subconn${r.subscriberConnectMs}ms`);
  if (r.timerExpiredLines) extra.push(`timer${r.timerExpiredLines}`);
  if (r.e2eeByteExact !== undefined) extra.push(`e2ee-byte-exact=${r.e2eeByteExact}`);
  if (r.simulcastLayersSeen !== undefined) extra.push(`simulcast-layers=${r.simulcastLayersSeen},switch=${r.simulcastSwitchWorks}`);
  if (r.error) extra.push('ERR ' + String(r.error).slice(0, 160));
  return `${r.ok === true ? 'PASS' : 'FAIL'} | ${r.name} | ${fmt(t)} | ${med} | ${extra.join(' ')}`;
});
const table = rows.join('\n');
console.log('\n===== SUMMARY =====\n' + table);
fs.writeFileSync(path.join(OUT, 'summary.txt'), table + '\n');
process.exit(results.some((r) => r.ok !== true) ? 1 : 0);
