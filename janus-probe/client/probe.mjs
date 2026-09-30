// Drives headless browsers against a local, patched Janus and records what
// transport level was really negotiated. Signalling is done here in Node
// (Janus HTTP API); the pages only hold the RTCPeerConnections.
import { chromium } from 'playwright';
import http from 'node:http';
import fs from 'node:fs';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const JANUS = process.env.JANUS_URL || 'http://127.0.0.1:8088/janus';
const JANUS_LOG = process.env.JANUS_LOG || '';
const OUT = process.env.OUT_DIR || path.join(HERE, 'out');
const PORT = 8199;
const PAGE = `http://127.0.0.1:${PORT}/page.html`;
const ONLY = (process.env.ONLY || '').split(',').filter(Boolean);
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
  try {
    const buf = fs.readFileSync(JANUS_LOG);
    return buf.subarray(off).toString('utf8');
  } catch (e) { return ''; }
}
function policyLines(text) {
  return text.split('\n')
    .filter((l) => /DTLS-POLICY|Handshake error|DTLS timer expired|DTLS handshake failure|DTLS alert|DTLS timeout|Fingerprint is NOT/.test(l))
    .map((l) => l.replace(/\[\d+\]/g, '[h]').replace(/\d{1,3}(\.\d{1,3}){3}/g, '<ip>').trim());
}

// ------------------------------------------------------------ browser helpers
const BASE_ARGS = [
  '--use-fake-device-for-media-stream', '--use-fake-ui-for-media-stream',
  '--disable-features=WebRtcHideLocalIpsWithMdns',
  '--allow-loopback-in-peer-connection', '--no-sandbox',
  '--autoplay-policy=no-user-gesture-required',
];
async function launchChromium(fieldTrials) {
  const args = [...BASE_ARGS];
  if (fieldTrials) args.push(`--force-fieldtrials=${fieldTrials}`);
  return chromium.launch({ headless: true, args });
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
const transportOf = (s) => (s.transport && s.transport[0]) || {};
const EXPECT = { tlsVersion: 'FEFC', dtlsCipher: 'TLS_AES_256_GCM_SHA384', srtpCipher: 'AEAD_AES_256_GCM' };
function levelOk(t) {
  return t.tlsVersion === EXPECT.tlsVersion && t.dtlsCipher === EXPECT.dtlsCipher &&
    t.srtpCipher === EXPECT.srtpCipher && t.dtlsState === 'connected';
}
function janusLevelOk(lines, n) {
  const ok = lines.filter((l) => /DTLS-POLICY/.test(l) &&
    /version=0xfefc cipher=0x1302 srtp=0x0008 group=0x11ec/.test(l) && /ok=1/.test(l));
  return ok.length >= n;
}

// ------------------------------------------------------------------- scenarios
async function echoScenario(browser, { name, setup, expect }) {
  const t0 = Date.now(); const off = logSize();
  const res = { name, expect, kind: 'echotest', chromiumRole: setup === 'active' ? 'client' : 'server (default)' };
  const ctx = await browser.newContext();
  const janus = new JanusClient(JANUS);
  try {
    const page = await newPage(ctx);
    res.pageInfo = await page.evaluate(() => window.probe.info());
    await janus.create();
    const h = await janus.attach('janus.plugin.echotest');
    janus.trickle[h] = (c) => page.evaluate(([i, cc]) => window.probe.addCandidate(i, cc), ['p', c]).catch(() => {});
    const offer = await page.evaluate((o) => window.probe.makeOffer('p', o), { audio: true, video: true, setup });
    await janus.send(h, { audio: true, video: true }, { type: 'offer', sdp: offer });
    const ev = await janus.waitFor((e) => e.sender === h && e.jsep, 15000);
    await page.evaluate(([i, s]) => window.probe.setAnswer(i, s), ['p', ev.jsep.sdp]);
    res.state = await waitConnected(page, 'p', 15000);
    await sleep(3000);
    const w = await statsWindow(page, 'p');
    res.transport = transportOf(w.s2);
    res.inbound = flow(w, 'in');
    res.outbound = flow(w, 'out');
    res.state = await page.evaluate(() => window.probe.state('p'));
    await janus.send(h, { request: 'leave' }).catch(() => {});
  } catch (e) { res.error = String(e.message || e); }
  await janus.destroy();
  await ctx.close();
  await sleep(500);
  res.janusLog = policyLines(logSince(off));
  res.seconds = Math.round((Date.now() - t0) / 100) / 10;
  const media = !!(res.inbound && res.inbound.audioPackets > 0 && res.inbound.videoPackets > 0);
  if (expect === 'pass') {
    res.ok = !!(res.transport && levelOk(res.transport) && media && janusLevelOk(res.janusLog, 1));
  } else if (expect === 'fail') {
    // fail-closed: no media may flow, Janus must not report an ok=1 handshake,
    // and it must have logged the refusal
    res.ok = !media && !janusLevelOk(res.janusLog, 1) &&
      res.janusLog.some((l) => /ok=0|Handshake error|handshake failure|DTLS alert/.test(l));
  } else {
    res.ok = null;
  }
  return res;
}

async function roomScenario(browser, { name, roleP, roleS, room, e2ee }) {
  const t0 = Date.now(); const off = logSize();
  const res = {
    name, expect: 'pass', kind: 'videoroom', room: room === 2345 ? 'e2ee-required' : 'plain',
    publisherChromiumRole: roleP === 'active' ? 'client' : 'server (default)',
    subscriberChromiumRole: roleS === 'passive' ? 'server' : 'client (default)', e2ee: !!e2ee,
  };
  const ctx = await browser.newContext();
  const janus = new JanusClient(JANUS);
  try {
    const pa = await newPage(ctx); const pb = await newPage(ctx);
    res.pageInfo = await pa.evaluate(() => window.probe.info());
    await janus.create();
    // publisher
    const hp = await janus.attach('janus.plugin.videoroom');
    janus.trickle[hp] = (c) => pa.evaluate(([i, cc]) => window.probe.addCandidate(i, cc), ['pub', c]).catch(() => {});
    await janus.send(hp, { request: 'join', ptype: 'publisher', room, display: 'pub' });
    const joined = await janus.waitFor((e) => e.sender === hp && pluginData(e).videoroom === 'joined', 10000);
    const feed = pluginData(joined).id;
    const offer = await pa.evaluate((o) => window.probe.makeOffer('pub', o),
      { audio: true, video: true, setup: roleP, e2ee });
    await janus.send(hp, { request: 'publish', audio: true, video: true },
      { type: 'offer', sdp: offer, ...(e2ee ? { e2ee: true } : {}) });
    const ans = await janus.waitFor((e) => e.sender === hp && e.jsep, 15000);
    await pa.evaluate(([i, s]) => window.probe.setAnswer(i, s), ['pub', ans.jsep.sdp]);
    res.publisherState = await waitConnected(pa, 'pub', 15000);
    // subscriber
    const hs = await janus.attach('janus.plugin.videoroom');
    janus.trickle[hs] = (c) => pb.evaluate(([i, cc]) => window.probe.addCandidate(i, cc), ['sub', c]).catch(() => {});
    await janus.send(hs, { request: 'join', ptype: 'subscriber', room, streams: [{ feed }] });
    const att = await janus.waitFor((e) => e.sender === hs && e.jsep, 15000);
    res.subscriberJsepE2ee = !!att.jsep.e2ee;
    const answer = await pb.evaluate(([s, o]) => window.probe.makeAnswer('sub', s, o),
      [att.jsep.sdp, { setup: roleS, e2ee }]);
    await janus.send(hs, { request: 'start', room }, { type: 'answer', sdp: answer });
    res.subscriberState = await waitConnected(pb, 'sub', 15000);
    await sleep(4000);
    const wp = await statsWindow(pa, 'pub');
    const wsub = await statsWindow(pb, 'sub');
    res.publisherTransport = transportOf(wp.s2);
    res.subscriberTransport = transportOf(wsub.s2);
    res.publisherOut = flow(wp, 'out');
    res.subscriberIn = flow(wsub, 'in');
    if (e2ee) {
      res.workerCountersPublisher = await pa.evaluate(() => window.probe.workerCounters());
      res.workerCountersSubscriber = await pb.evaluate(() => window.probe.workerCounters());
    }
    res.subscriberVideoDecoded = sumIn(wsub.s2, 'video', 'framesDecoded');
    await janus.send(hs, { request: 'leave' }).catch(() => {});
    await janus.send(hp, { request: 'leave' }).catch(() => {});
  } catch (e) { res.error = String(e.message || e); }
  await janus.destroy();
  await ctx.close();
  await sleep(500);
  res.janusLog = policyLines(logSince(off));
  res.seconds = Math.round((Date.now() - t0) / 100) / 10;
  const media = !!(res.subscriberIn && res.subscriberIn.audioPackets > 0 && res.subscriberIn.videoPackets > 0 &&
    res.subscriberVideoDecoded > 0);
  res.ok = !!(res.publisherTransport && levelOk(res.publisherTransport) && res.subscriberTransport &&
    levelOk(res.subscriberTransport) && media && janusLevelOk(res.janusLog, 2));
  if (e2ee) {
    const c = res.workerCountersSubscriber;
    res.ok = res.ok && !!c && c.receiver.video > 0 && c.receiver.audio > 0 && res.subscriberJsepE2ee;
  }
  return res;
}

// ------------------------------------------------------------------------ main
const results = [];
const want = (n) => ONLY.length === 0 || ONLY.some((o) => n.startsWith(o));
async function run(fn, browser, spec) {
  if (!want(spec.name)) return;
  console.log('== ' + spec.name);
  let r;
  try { r = await fn(browser, spec); } catch (e) { r = { name: spec.name, ok: false, error: String(e && e.stack || e) }; }
  console.log(JSON.stringify(r, null, 1));
  results.push(r);
  fs.writeFileSync(path.join(OUT, 'results.json'), JSON.stringify(results, null, 1));
}

const srv = await startServer();
const versions = {};
try {
  // Chromium with the PQC field trial (what our apps use) ---------------
  const pqc = await launchChromium('WebRTC-EnableDtlsPqc/Enabled/');
  versions.chromium = pqc.version();
  await run(echoScenario, pqc, { name: 'A1-echotest-chromium=DTLS-server', setup: null, expect: 'pass' });
  await run(echoScenario, pqc, { name: 'A2-echotest-chromium=DTLS-client', setup: 'active', expect: 'pass' });
  await run(roomScenario, pqc, { name: 'B1-videoroom-pub=server,sub=client', roleP: null, roleS: null, room: 1234 });
  await run(roomScenario, pqc, { name: 'B2-videoroom-pub=client,sub=server', roleP: 'active', roleS: 'passive', room: 1234 });
  await run(roomScenario, pqc, { name: 'E1-videoroom-e2ee-xor', roleP: null, roleS: null, room: 2345, e2ee: true });
  await pqc.close();
  // Negative: Chromium with the PQC group disabled -----------------------
  const nopqc = await launchChromium('WebRTC-EnableDtlsPqc/Disabled/');
  await run(echoScenario, nopqc, { name: 'N1a-no-mlkem-chromium=DTLS-server', setup: null, expect: 'fail' });
  await run(echoScenario, nopqc, { name: 'N1b-no-mlkem-chromium=DTLS-client', setup: 'active', expect: 'fail' });
  await nopqc.close();
  // Informative: stock Chromium, no field trial at all --------------------
  const stock = await launchChromium('');
  await run(echoScenario, stock, { name: 'I1-stock-chromium-no-trial-server', setup: null, expect: 'info' });
  await run(echoScenario, stock, { name: 'I2-stock-chromium-no-trial-client', setup: 'active', expect: 'info' });
  await stock.close();
} catch (e) {
  console.error('FATAL', e);
  results.push({ name: 'fatal', ok: false, error: String((e && e.stack) || e) });
}
srv.close();
fs.writeFileSync(path.join(OUT, 'versions.json'), JSON.stringify(versions, null, 1));
fs.writeFileSync(path.join(OUT, 'results.json'), JSON.stringify(results, null, 1));

// summary table
const fmt = (x) => (x ? `${x.tlsVersion || '-'} ${x.dtlsCipher || '-'} ${x.srtpCipher || '-'} ${x.tlsGroup || ''}`.trim() : '-');
const rows = results.map((r) => {
  const t = r.transport || r.publisherTransport || null;
  const t2 = r.subscriberTransport || null;
  let med = '-';
  if (r.inbound) med = `a${r.inbound.audioPackets}/v${r.inbound.videoPackets}`;
  else if (r.subscriberIn) med = `a${r.subscriberIn.audioPackets}/v${r.subscriberIn.videoPackets} dec${r.subscriberVideoDecoded}`;
  const verdict = r.ok === true ? 'PASS' : (r.expect === 'info' ? 'INFO' : 'FAIL');
  return `${verdict} | ${r.name} | ${fmt(t)}${t2 ? ' || ' + fmt(t2) : ''} | media ${med}${r.error ? ' | ERR ' + r.error : ''}`;
});
const table = rows.join('\n');
console.log('\n===== SUMMARY =====\n' + table);
fs.writeFileSync(path.join(OUT, 'summary.txt'), table + '\n');
const failed = results.filter((r) => r.expect !== 'info' && r.ok !== true);
process.exit(failed.length ? 1 : 0);
