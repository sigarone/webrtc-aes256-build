// In-page load-test bot library. Loaded by page.html; exposes `window.qbot`.
//
// One Bot = one Janus WebSocket session + a publisher PeerConnection (send only) + one
// multistream subscriber PeerConnection (receive only), behaving like a group-call client of
// GROUP_CALLS_V2 (spec sections 4 and 5.3). Many bots can live in one page; they share one
// E2EE Worker. The orchestrator drives everything through Playwright page.evaluate():
//   start(cfg) / poll() / setTokens() / stop() / stopAll() / setSubstream() / info()
// (shapes: BotConfig, BotStatus and Sample of the shared contract).
//
// Design rules worth knowing before changing anything:
//  - Every async failure ends in state 'failed' with {phase, code, message}; nothing may escape
//    as an unhandled rejection.
//  - Secrets (ws url, tokens, join token, e2ee key) never leave the Bot: every string that goes
//    into a status, an event or the console passes through scrub().
//  - All subscriber renegotiations go through ONE serial queue: Janus allows a single offer/answer
//    exchange in flight per subscriber handle, so two concurrent ones would corrupt the SDP state.
import { JanusSession, JanusError } from './janus-client.mjs';
import { fingerprintMatches, getMLines, mungeSdp } from './sdp.mjs';

const PLUGIN = 'janus.plugin.videoroom';
const SRTP_OK = new Set(['AEAD_AES_256_GCM', 'SRTP_AEAD_AES_256_GCM']);
const MAX_SAMPLES = 900;
const MAX_EVENTS = 400;
const NO_SUCH_FEED = 428;
const INVALID_SDP = 437;
const STATE_ORDER = ['init', 'ws', 'joining', 'publishing', 'subscribing', 'connected', 'steady'];
const QUALITY_RANK = { none: 0, other: 1, bandwidth: 2, cpu: 3 };

const hasScriptTransform = typeof RTCRtpScriptTransform !== 'undefined';
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
const num = (x) => (typeof x === 'number' && Number.isFinite(x) ? x : null);
const round = (x, digits) => { const k = 10 ** digits; return Math.round(x * k) / k; };

// ----------------------------------------------------------------- config

/** Validate and complete a BotConfig. Throws Error listing the problems (field names only, no values). */
function normalizeConfig(cfg) {
  const problems = [];
  const need = (ok, msg) => { if (!ok) problems.push(msg); };
  if (!cfg || typeof cfg !== 'object') throw new Error('invalid bot config: not an object');
  const str = (v) => typeof v === 'string' && v.length > 0;
  need(str(cfg.id) && /^[\w.:-]{1,80}$/.test(cfg.id), 'id');
  need(str(cfg.wsUrl) && /^wss?:\/\//i.test(cfg.wsUrl), 'wsUrl');
  need(cfg.sessionToken === undefined || cfg.sessionToken === null || typeof cfg.sessionToken === 'string', 'sessionToken');
  need(str(cfg.room), 'room');
  need(str(cfg.pseudonym), 'pseudonym');
  need(str(cfg.joinToken), 'joinToken');
  need(cfg.peers === undefined || (Array.isArray(cfg.peers) && cfg.peers.every(str)), 'peers');
  const media = { audio: true, video: false, ...(cfg.media || {}) };
  need(media.audio || media.video, 'media (audio and video both off)');
  const e2ee = { mode: 'none', keyHex: null, ...(cfg.e2ee || {}) };
  need(['none', 'xor', 'aesgcm'].includes(e2ee.mode), 'e2ee.mode');
  need(e2ee.mode !== 'aesgcm' || (typeof e2ee.keyHex === 'string' && /^[0-9a-fA-F]{64}$/.test(e2ee.keyHex)), 'e2ee.keyHex');
  const expectTransport = cfg.expectTransport || 'strict';
  need(expectTransport === 'strict' || expectTransport === 'off', 'expectTransport');
  const render = cfg.render || 'attach';
  need(render === 'attach' || render === 'none', 'render');
  need(cfg.dtlsFingerprint === undefined || cfg.dtlsFingerprint === null || str(cfg.dtlsFingerprint), 'dtlsFingerprint');
  need(cfg.iceServers === undefined || Array.isArray(cfg.iceServers), 'iceServers');
  if (problems.length) throw new Error(`invalid bot config: ${problems.join(', ')}`);
  const video = { capture: { width: 640, height: 360, frameRate: 15 }, encodings: [], ...(cfg.video || {}) };
  video.capture = { width: 640, height: 360, frameRate: 15, ...(video.capture || {}) };
  video.encodings = Array.isArray(video.encodings) ? video.encodings : [];
  return {
    id: cfg.id,
    wsUrl: cfg.wsUrl,
    sessionToken: cfg.sessionToken || '',
    room: cfg.room,
    pseudonym: cfg.pseudonym,
    joinToken: cfg.joinToken,
    peers: [...(cfg.peers || [])],
    media,
    audio: { ptime: 60, maxaveragebitrate: 32000, cbr: true, fec: true, dtx: false, ...(cfg.audio || {}) },
    video,
    e2ee,
    subscribe: { substream: 1, speakerSubstream: 2, temporal: 2, speaker: null, ...(cfg.subscribe || {}) },
    iceServers: cfg.iceServers || [],
    expectTransport,
    dtlsFingerprint: cfg.dtlsFingerprint || null,
    timeouts: { requestMs: 8000, connectMs: 20000, keepaliveMs: 25000, ...(cfg.timeouts || {}) },
    statsIntervalMs: Math.max(200, cfg.statsIntervalMs || 2000),
    renegotiateDebounceMs: cfg.renegotiateDebounceMs === undefined ? 150 : Math.max(0, cfg.renegotiateDebounceMs),
    render,
  };
}

// ------------------------------------------------------------- E2EE worker host

/** One shared module Worker per page; senders/receivers of all bots are attached to it. */
class E2eeHost {
  constructor() {
    this.worker = null;
    this.readyPromise = null;
    this.markReady = null;
    this.seq = 0;
    this.waiters = new Map();
    this.legacy = new WeakSet(); // receivers/senders already wired through createEncodedStreams
  }

  _get() {
    if (!this.worker) {
      this.worker = new Worker(new URL('./e2ee-worker.js', import.meta.url), { type: 'module' });
      this.readyPromise = new Promise((resolve) => {
        this.markReady = resolve;
        setTimeout(resolve, 15000); // never block a bot forever on a worker that fails to load
      });
      this.worker.onmessage = (ev) => {
        const m = ev.data;
        if (m && m.type === 'ready') this.markReady();
        if (m && m.type === 'counters' && this.waiters.has(m.id)) {
          const w = this.waiters.get(m.id);
          this.waiters.delete(m.id);
          clearTimeout(w.timer);
          w.resolve(m.counters);
        }
      };
      this.worker.onerror = () => { /* frames are then dropped by the missing transform; counters stay at 0 */ };
    }
    return this.worker;
  }

  /**
   * Resolves once the worker has evaluated its script. Transforms are attached only after that, so
   * that no frame can leave (or be delivered) untransformed while the worker is still loading.
   */
  ready() {
    this._get();
    return this.readyPromise;
  }

  /** Attach the frame transform to an RTCRtpSender/RTCRtpReceiver. options: {botId, kind, role, mode, keyHex}. */
  attach(rtp, options) {
    if (hasScriptTransform) {
      if (!rtp.transform) rtp.transform = new RTCRtpScriptTransform(this._get(), options);
      return;
    }
    if (this.legacy.has(rtp)) return;
    this.legacy.add(rtp);
    const s = rtp.createEncodedStreams(); // needs encodedInsertableStreams:true on the PeerConnection
    this._get().postMessage({ readable: s.readable, writable: s.writable, options }, [s.readable, s.writable]);
  }

  /** Counters of one bot: {enc,dec,fail}:{audio,video}, or null when the worker does not answer. */
  counters(botId) {
    if (!this.worker) return Promise.resolve(null);
    return new Promise((resolve) => {
      const id = ++this.seq;
      const timer = setTimeout(() => { this.waiters.delete(id); resolve(null); }, 1500);
      this.waiters.set(id, { resolve, timer });
      this.worker.postMessage({ type: 'counters', id, botId });
    });
  }

  forget(botId) {
    if (this.worker) this.worker.postMessage({ type: 'forget', botId });
  }
}

// ------------------------------------------------------------------ small helpers

/** Runs async tasks strictly one after the other; a failing task reports to onError and the queue goes on. */
class SerialQueue {
  constructor(onError) {
    this._tail = Promise.resolve();
    this._onError = onError;
    this.closed = false;
  }

  push(fn) {
    this._tail = this._tail.then(async () => {
      if (this.closed) return;
      try { await fn(); } catch (e) { this._onError(e); }
    }).catch(() => { /* onError itself failed: the queue must keep running */ });
    return this._tail;
  }
}

/**
 * Local ICE candidates are held back until the request that carries the SDP has been sent:
 * Janus queues early trickles, but sending them first would race the offer/answer on the wire.
 */
class TrickleGate {
  constructor(handle, onError) {
    this.handle = handle;
    this.onError = onError;
    this.held = [];
    this.open = false;
  }

  push(cand) { // cand: RTCIceCandidate or null (end of candidates)
    if (this.open) this._send(cand); else this.held.push(cand);
  }

  release() {
    this.open = true;
    for (const c of this.held.splice(0)) this._send(c);
  }

  _send(cand) {
    this.handle.trickle(cand).catch((e) => this.onError(e));
  }
}

function zeroCat(spec) {
  const o = {};
  for (const [key] of spec) o[key] = 0;
  return o;
}

// Sample counters: [sample key, getStats field, optional scale]
const SPEC = {
  aIn: [['packets', 'packetsReceived'], ['lost', 'packetsLost'], ['bytes', 'bytesReceived'], ['concealed', 'concealedSamples'],
    ['samples', 'totalSamplesReceived'], ['nack', 'nackCount'], ['jbDelayMs', 'jitterBufferDelay', 1000], ['jbEmitted', 'jitterBufferEmittedCount']],
  vIn: [['packets', 'packetsReceived'], ['lost', 'packetsLost'], ['bytes', 'bytesReceived'], ['framesDecoded', 'framesDecoded'],
    ['framesDropped', 'framesDropped'], ['freezeCount', 'freezeCount'], ['freezeMs', 'totalFreezesDuration', 1000],
    ['pauseCount', 'pauseCount'], ['nack', 'nackCount'], ['pli', 'pliCount'], ['fir', 'firCount']],
  aOut: [['packets', 'packetsSent'], ['bytes', 'bytesSent'], ['nack', 'nackCount'], ['retransmitted', 'retransmittedPacketsSent']],
  vOut: [['packets', 'packetsSent'], ['bytes', 'bytesSent'], ['framesEncoded', 'framesEncoded'], ['nack', 'nackCount'],
    ['pli', 'pliCount'], ['retransmitted', 'retransmittedPacketsSent']],
};
const CATS = Object.keys(SPEC);

function zeroTot() {
  const t = {};
  for (const c of CATS) t[c] = zeroCat(SPEC[c]);
  t.e2eeEnc = 0;
  t.e2eeDec = 0;
  t.e2eeFail = 0;
  return t;
}

/** getStats() RTCStatsReport -> Map(id -> report). */
function statsById(stats) {
  const m = new Map();
  stats.forEach((r) => m.set(r.id, r));
  return m;
}

/** Collection time (epoch ms) of a stats snapshot; falls back to now when the timestamps are not epoch based. */
function statsTime(byId) {
  let ts = 0;
  for (const r of byId.values()) if (typeof r.timestamp === 'number' && r.timestamp > ts) ts = r.timestamp;
  return Math.abs(ts - Date.now()) < 3600 * 1000 ? ts : Date.now();
}

function selectedPair(byId) {
  let transport = null;
  for (const r of byId.values()) if (r.type === 'transport') { transport = r; break; }
  let pair = transport && transport.selectedCandidatePairId ? byId.get(transport.selectedCandidatePairId) : null;
  if (!pair) {
    for (const r of byId.values()) {
      if (r.type === 'candidate-pair' && (r.selected || (r.nominated && r.state === 'succeeded'))) { pair = r; break; }
    }
  }
  return { transport, pair: pair || null };
}

/** Negotiated transport parameters. Candidate TYPES and protocol only, never addresses. */
function transportInfo(byId, pc, expect) {
  const { transport, pair } = selectedPair(byId);
  if (!transport) return null;
  const local = pair && byId.get(pair.localCandidateId);
  const remote = pair && byId.get(pair.remoteCandidateId);
  const info = {
    tlsVersion: transport.tlsVersion || null,
    dtlsCipher: transport.dtlsCipher || null,
    srtpCipher: transport.srtpCipher || null,
    dtlsState: transport.dtlsState || null,
    iceState: transport.iceState || pc.iceConnectionState || null,
    localCandType: (local && local.candidateType) || null,
    remoteCandType: (remote && remote.candidateType) || null,
    protocol: (local && local.protocol) || null,
    ok: true,
  };
  if (expect === 'strict') {
    info.ok = info.tlsVersion === 'FEFC' && info.dtlsCipher === 'TLS_AES_256_GCM_SHA384'
      && SRTP_OK.has(info.srtpCipher) && info.dtlsState === 'connected';
  }
  return info;
}

const isComplete = (t) => !!(t && t.tlsVersion && t.dtlsCipher && t.srtpCipher && t.dtlsState);

// ---------------------------------------------------------------------------- Bot

class Bot {
  constructor(cfg, host) {
    this.cfg = cfg;
    this.id = cfg.id;
    this.host = host;
    this.startedAt = Date.now();
    this.state = 'init';
    this.error = null;
    this.timings = {
      wsMs: null, createMs: null, joinedMs: null, publishMs: null, pubIceMs: null, pubDtlsMs: null, pubConnectMs: null,
      subOfferMs: null, subIceMs: null, subDtlsMs: null, subConnectMs: null, firstAudioMs: null, firstVideoMs: null, allPeersMs: null,
    };
    this.peersInfo = { expected: cfg.peers.length, subscribed: 0, audioFlowing: 0, videoFlowing: 0 };
    this.transport = { pub: null, sub: null };
    this.tot = zeroTot();
    this.samples = [];
    this.events = [];
    // Everything that must never appear in an output string: fixed secrets plus the latest session tokens.
    this.secrets = [cfg.wsUrl, cfg.joinToken, cfg.e2ee.keyHex].filter((x) => typeof x === 'string' && x.length >= 6);
    this.tokens = cfg.sessionToken ? [cfg.sessionToken] : [];

    this.session = null;
    this.pubHandle = null;
    this.subHandle = null;
    this.pub = null; // {pc, ...} publisher PeerConnection record
    this.sub = null; // subscriber PeerConnection record
    this.privateId = null;
    this.stream = null; // local getUserMedia stream

    // Subscriber bookkeeping
    this.feeds = new Map(); // feed id -> 'wanted' | 'pending' | 'subscribed'
    this.subMids = new Map(); // subscriber mid -> {type, feed}
    this.trackInfo = new Map(); // remote track id -> {mid, kind}
    this.mediaEls = new Map(); // remote track id -> media element
    this.configured = new Map(); // subscriber mid -> "substream/temporal" last sent
    this.layerOverride = null;
    this.subJoined = false;
    this.subEnabled = false; // becomes true once the publisher is published
    this.pendingSub = new Set();
    this.pendingUnsubMids = new Set();
    this.flushTimer = null;
    this.subq = new SerialQueue((e) => this._onSubError(e));

    // Sampling / progress
    this.prev = new Map(); // counter baseline of the previous sample: "role|report id|field" -> value
    this.seen = new Map(); // counters read in the sample being built
    this.lastSampleT = null;
    this.sampleTimer = null;
    this.progressTimer = null;
    this.progressSlowTimer = null;
    this.sampling = false;
    this.progressBusy = false;
    this.warnAt = new Map();
    this.layerTotals = new Map(); // simulcast rid -> packets sent so far (see _checkLayers)
    this.layerSamples = 0;

    this.phase = 'other';
    this.stopping = false;
    this.stopPromise = null;
    this.final = null;
  }

  // ---------------------------------------------------------------- reporting

  scrub(text) {
    let s = String(text === undefined || text === null ? '' : text);
    for (const secret of [...this.secrets, ...this.tokens]) s = s.split(secret).join('<redacted>');
    s = s.replace(/\b[a-z][a-z0-9+.-]*:\/\/\S+/gi, '<url>').replace(/[0-9a-fA-F]{40,}/g, '<hex>');
    return s.length > 240 ? `${s.slice(0, 237)}...` : s;
  }

  _event(kind, msg) {
    this.events.push({ t: Date.now(), kind, msg: this.scrub(msg) });
    if (this.events.length > MAX_EVENTS) this.events.splice(0, this.events.length - MAX_EVENTS);
  }

  /** Warn events of one kind are rate limited (a flapping link must not flood the poll results). */
  _warn(key, msg, everyMs = 5000) {
    const t = Date.now();
    if (t - (this.warnAt.get(key) || 0) < everyMs) return;
    this.warnAt.set(key, t);
    this._event('warn', msg);
  }

  get terminal() { return this.state === 'failed' || this.state === 'closed'; }

  _since() { return Date.now() - this.startedAt; }

  _advance(state) {
    if (this.terminal || this.state === 'leaving') return;
    if (STATE_ORDER.indexOf(state) > STATE_ORDER.indexOf(this.state)) {
      this.state = state;
      this._event('state', state);
    }
  }

  /** Terminal failure: record the contract error object, tear everything down in the background. */
  _fail(phase, err, code) {
    if (this.terminal || this.stopping) return;
    if (err && err.phase) phase = err.phase; // errors may name their own phase (e.g. dtls_pin_mismatch)
    const c = code !== undefined ? code : (err && err.code !== undefined ? err.code : (err && err.name) || 'error');
    this.error = {
      phase,
      code: typeof c === 'number' ? c : this.scrub(c),
      message: this.scrub((err && err.message) || String(err)),
    };
    this.state = 'failed';
    this._event('error', `${phase} ${this.error.code}: ${this.error.message}`);
    this._teardown(false).catch(() => {});
  }

  /** Public status; drains samples and events. */
  status(drain = true) {
    const samples = this.samples;
    const events = this.events;
    if (drain) { this.samples = []; this.events = []; }
    const tot = {};
    for (const c of CATS) tot[c] = { ...this.tot[c] };
    tot.e2eeEnc = this.tot.e2eeEnc;
    tot.e2eeDec = this.tot.e2eeDec;
    tot.e2eeFail = this.tot.e2eeFail;
    return {
      id: this.id,
      state: this.state,
      error: this.error ? { ...this.error } : null,
      startedAt: this.startedAt,
      timings: { ...this.timings },
      peers: { ...this.peersInfo },
      transport: { pub: this.transport.pub && { ...this.transport.pub }, sub: this.transport.sub && { ...this.transport.sub } },
      tot,
      samples: drain ? samples : [...samples],
      events: drain ? events : [...events],
    };
  }

  /** Session token refresh: used for every following Janus request of this bot. */
  setToken(token) {
    if (this.session) this.session.setToken(token);
    this.tokens.push(token);
    if (this.tokens.length > 4) this.tokens.shift();
    this._event('token', 'session token refreshed');
  }

  // ------------------------------------------------------------------ lifecycle

  /** Runs the whole join sequence; never rejects. */
  async run() {
    try {
      await this._connect();
      await this._joinPublisher();
      await this._publish();
      this._enableSubscriber();
    } catch (e) {
      if (!this.stopping) this._fail(this.phase, e);
    }
  }

  _guard() {
    if (this.stopping || this.terminal) throw new Error('bot stopped');
  }

  async _connect() {
    const cfg = this.cfg;
    this._advance('ws');
    this.phase = 'ws';
    this.session = new JanusSession({
      url: cfg.wsUrl,
      token: cfg.sessionToken || null,
      keepaliveMs: cfg.timeouts.keepaliveMs,
      requestTimeoutMs: cfg.timeouts.requestMs,
      onClose: (info) => this._onSessionClosed(info),
      onKeepaliveError: (e, n) => this._onKeepaliveError(e, n),
      log: (m) => this._warn('janus-log', m, 2000),
    });
    try {
      await this.session.connect({ timeoutMs: cfg.timeouts.requestMs });
    } catch (e) {
      this.phase = this.session.openedAt ? 'create' : 'ws';
      throw e;
    }
    this.timings.wsMs = this.session.openedAt - this.startedAt;
    this.timings.createMs = this.session.createdAt - this.startedAt;
    this._guard();
  }

  async _joinPublisher() {
    const cfg = this.cfg;
    this._advance('joining');
    this.phase = 'attach';
    this.pubHandle = await this.session.attach(PLUGIN);
    this._guard();
    this.pubHandle.on('event', ({ data }) => this._onPublisherEvent(data));
    this.pubHandle.on('trickle', (c) => this._onRemoteCandidate('pub', c));
    this.pubHandle.on('slowlink', (m) => this._warn('slowlink-pub', `slowlink on publisher (uplink=${m.uplink}, nacks=${m.nacks})`));
    this.pubHandle.on('hangup', (m) => this._onHangup('pub', m.reason));
    this.phase = 'join';
    const { data } = await this.pubHandle.message({
      request: 'join', ptype: 'publisher', room: cfg.room, id: cfg.pseudonym, display: cfg.pseudonym, token: cfg.joinToken,
    });
    this._guard();
    if (!data || data.videoroom !== 'joined') throw new JanusError('unexpected join answer', 'bad_response');
    this.privateId = data.private_id;
    this.timings.joinedMs = this._since();
    this._advance('publishing');
    this._onNewPublishers(data.publishers || []);
  }

  // ------------------------------------------------------------------ publisher

  async _getMedia() {
    const { media, video } = this.cfg;
    const constraints = {
      // Audio processing is off: it costs CPU per bot and does not change what the SFU sees.
      audio: media.audio ? { echoCancellation: false, noiseSuppression: false, autoGainControl: false } : false,
      video: media.video ? {
        width: { ideal: video.capture.width }, height: { ideal: video.capture.height }, frameRate: { ideal: video.capture.frameRate },
      } : false,
    };
    const stream = await navigator.mediaDevices.getUserMedia(constraints);
    if (this.stopping || this.terminal) { stream.getTracks().forEach((t) => t.stop()); throw new Error('bot stopped'); }
    this.stream = stream;
    const vt = stream.getVideoTracks()[0];
    if (vt) {
      const s = vt.getSettings();
      this._event('state', `capture video ${s.width}x${s.height}@${Math.round(s.frameRate || 0)}`);
    }
    return stream;
  }

  _newPc(role) {
    const conf = { iceServers: this.cfg.iceServers, bundlePolicy: 'max-bundle', rtcpMuxPolicy: 'require' };
    if (this.cfg.e2ee.mode !== 'none' && !hasScriptTransform) conf.encodedInsertableStreams = true;
    const pc = new RTCPeerConnection(conf);
    const rec = {
      role, pc, gate: null, pendingRemote: [], tIceChecking: null, tIceConnected: null, tConnected: null, tLocalSet: null,
      watchdog: null, checked: false,
    };
    pc.addEventListener('iceconnectionstatechange', () => this._onIceState(rec));
    pc.addEventListener('connectionstatechange', () => this._onConnState(rec));
    pc.addEventListener('icecandidate', (ev) => { if (rec.gate) rec.gate.push(ev.candidate); });
    return rec;
  }

  _attachTransform(rtp, kind, role) {
    const { mode, keyHex } = this.cfg.e2ee;
    if (mode !== 'none') this.host.attach(rtp, { botId: this.id, kind, role, mode, keyHex });
  }

  async _publish() {
    const cfg = this.cfg;
    this.phase = 'publish';
    const stream = await this._getMedia();
    if (cfg.e2ee.mode !== 'none') { await this.host.ready(); this._guard(); }
    const rec = this._newPc('pub');
    this.pub = rec;
    rec.gate = new TrickleGate(this.pubHandle, (e) => this._warn('trickle-pub', `publisher trickle failed (${e.code})`));

    // The transform goes on every sender BEFORE createOffer, so no frame is ever sent unprotected.
    // It is attached synchronously after addTransceiver: Chromium silently ignores a sender
    // transform that is set after an await in between (seen in the loopback test helper).
    if (cfg.media.audio) {
      const t = rec.pc.addTransceiver(stream.getAudioTracks()[0], { direction: 'sendonly', streams: [stream] });
      this._attachTransform(t.sender, 'audio', 'sender');
    }
    if (cfg.media.video) {
      const init = { direction: 'sendonly', streams: [stream] };
      if (cfg.video.encodings.length) {
        init.sendEncodings = cfg.video.encodings.map((e) => {
          const enc = { rid: e.rid, active: true };
          for (const k of ['scaleResolutionDownBy', 'maxBitrate', 'maxFramerate']) if (e[k] !== undefined && e[k] !== null) enc[k] = e[k];
          return enc;
        });
      }
      const t = rec.pc.addTransceiver(stream.getVideoTracks()[0], init);
      this._attachTransform(t.sender, 'video', 'sender');
    }

    const offer = await rec.pc.createOffer();
    this._guard();
    const sdp = mungeSdp(offer.sdp, { opus: cfg.audio });
    rec.tLocalSet = Date.now();
    await rec.pc.setLocalDescription({ type: 'offer', sdp });
    this._guard();

    const body = { request: 'publish', audio: cfg.media.audio, video: cfg.media.video };
    const videoLine = getMLines(sdp).find((m) => m.kind === 'video');
    if (videoLine && videoLine.mid !== null) body.descriptions = [{ mid: videoLine.mid, description: 'camera' }];
    // The e2ee flag belongs to the JSEP, not to the body (Janus rooms require it with require_e2ee).
    const pending = this.pubHandle.message(body, { jsep: { type: 'offer', sdp, e2ee: cfg.e2ee.mode !== 'none' } });
    rec.gate.release();
    const { jsep } = await pending;
    this._guard();
    if (!jsep || jsep.type !== 'answer') throw new JanusError('publish: no answer', 'no_answer');
    this._checkPin(jsep.sdp, 'publisher answer');
    // libwebrtc takes the send-side Opus ptime/bitrate/cbr from the REMOTE description, so the
    // answer gets the same Opus munge as our offer (see sdp.mjs).
    await rec.pc.setRemoteDescription({ type: 'answer', sdp: mungeSdp(jsep.sdp, { opus: cfg.audio }) });
    this._guard();
    await this._flushRemoteCandidates(rec);
    this.timings.publishMs = this._since();
    this._armWatchdog(rec);
    this._advance('subscribing');
    this._startTimers();
  }

  _checkPin(sdp, what) {
    const pin = this.cfg.dtlsFingerprint;
    if (pin && !fingerprintMatches(sdp, pin)) {
      const e = new Error(`DTLS fingerprint of the ${what} does not match the pin`);
      e.code = 'dtls_pin_mismatch';
      e.phase = 'dtls';
      throw e;
    }
  }

  // ------------------------------------------------------------ publisher events

  _onPublisherEvent(data) {
    if (!data || this.stopping || this.terminal) return;
    if (data.publishers) this._onNewPublishers(data.publishers);
    for (const key of ['leaving', 'unpublished']) {
      if (data[key] !== undefined && data[key] !== 'ok') this._onPeerGone(String(data[key]));
    }
    if (data.kicked !== undefined) {
      if (String(data.kicked) === this.cfg.pseudonym) this._fail('other', new Error('kicked from the room'), 'kicked');
      else this._onPeerGone(String(data.kicked));
    }
    if (data.videoroom === 'destroyed') this._fail('other', new Error('room destroyed'), 'room_destroyed');
    if (data.error_code !== undefined) this._warn('plugin-error', `publisher handle error ${data.error_code}`);
  }

  _onHangup(role, reason) {
    if (this.stopping || this.terminal) return;
    const rec = role === 'pub' ? this.pub : this.sub;
    const dtls = /dtls/i.test(reason || '') || (rec && rec.tIceConnected);
    this._fail(dtls ? 'dtls' : 'ice', new Error(`janus hung up the ${role} peer connection: ${reason || 'no reason'}`), 'janus_hangup');
  }

  _onSessionClosed(info) {
    if (this.stopping || this.terminal) return;
    this._fail('ws', new Error(info && info.code === 'session_timeout' ? 'janus session timed out' : 'websocket closed'),
      info && info.code === 'session_timeout' ? 'session_timeout' : 'ws_closed');
  }

  _onKeepaliveError(e, n) {
    if (this.stopping || this.terminal) return;
    this._warn('keepalive', `keepalive failed (${e.code}), consecutive ${n}`, 0);
    // An unauthorized keepalive means the session token was not refreshed in time: not transient.
    if (e.code === 403 || n >= 3) this._fail('keepalive', e);
  }

  // ------------------------------------------------------- PeerConnection events

  _onIceState(rec) {
    const s = rec.pc.iceConnectionState;
    const now = Date.now();
    if (s === 'checking' && rec.tIceChecking === null) rec.tIceChecking = now;
    if ((s === 'connected' || s === 'completed') && rec.tIceConnected === null) {
      rec.tIceConnected = now;
      if (rec.tIceChecking !== null) this.timings[`${rec.role}IceMs`] = rec.tIceConnected - rec.tIceChecking;
    }
    if (s === 'failed') this._fail('ice', new Error(`${rec.role} ICE failed`), 'ice_failed');
    if (s === 'disconnected') this._warn(`ice-disc-${rec.role}`, `${rec.role} ICE disconnected`, 10000);
  }

  _onConnState(rec) {
    const s = rec.pc.connectionState;
    if (s === 'connected' && rec.tConnected === null) {
      rec.tConnected = Date.now();
      clearTimeout(rec.watchdog);
      if (rec.tIceConnected !== null) this.timings[`${rec.role}DtlsMs`] = rec.tConnected - rec.tIceConnected;
      if (rec.tLocalSet !== null) this.timings[`${rec.role}ConnectMs`] = rec.tConnected - rec.tLocalSet;
      this._transportCheck(rec).catch(() => {});
      this._checkState();
    } else if (s === 'failed') {
      this._fail(rec.tIceConnected === null ? 'ice' : 'dtls', new Error(`${rec.role} peer connection failed`), 'pc_failed');
    }
  }

  /** Neither ICE nor DTLS may take longer than connectMs after the SDP exchange. */
  _armWatchdog(rec) {
    if (rec.watchdog) return;
    rec.watchdog = setTimeout(() => {
      if (rec.tConnected === null) {
        this._fail(rec.tIceConnected === null ? 'ice' : 'dtls', new Error(`${rec.role} peer connection not connected in time`), 'connect_timeout');
      }
    }, this.cfg.timeouts.connectMs);
  }

  /** Transport self-check of spec 4.5, once per PC after it connected. */
  async _transportCheck(rec) {
    if (rec.checked) return;
    rec.checked = true;
    let info = null;
    for (let i = 0; i < 5 && !this.terminal && !this.stopping; i++) {
      info = transportInfo(statsById(await rec.pc.getStats()), rec.pc, this.cfg.expectTransport);
      if (isComplete(info)) break;
      await sleep(200);
    }
    if (!info || this.terminal) return;
    this.transport[rec.role] = info;
    if (!info.ok) {
      this._fail('transport', new Error(`${rec.role} transport policy violation: tls=${info.tlsVersion} dtls=${info.dtlsCipher} `
        + `srtp=${info.srtpCipher} state=${info.dtlsState}`), 'transport_policy_violation');
    }
  }

  _onRemoteCandidate(role, cand) {
    const rec = role === 'pub' ? this.pub : this.sub;
    if (!rec) return; // candidate for a PC that does not exist yet: the SDP carries the same info
    if (!rec.pc.remoteDescription) { rec.pendingRemote.push(cand); return; }
    this._addCandidate(rec, cand);
  }

  _addCandidate(rec, cand) {
    const c = cand && !cand.completed ? cand : null; // null = end of candidates
    rec.pc.addIceCandidate(c).catch(() => { /* late or duplicate candidates are harmless */ });
  }

  async _flushRemoteCandidates(rec) {
    for (const c of rec.pendingRemote.splice(0)) this._addCandidate(rec, c);
  }

  // ------------------------------------------------------------------ subscriber

  _enableSubscriber() {
    this.subEnabled = true;
    if (this.pendingSub.size) this._scheduleFlush();
  }

  _onNewPublishers(list) {
    for (const p of list) {
      const id = String(p.id);
      if (id === this.cfg.pseudonym || this.feeds.has(id)) continue;
      this.feeds.set(id, 'wanted');
      this.pendingSub.add(id);
    }
    if (this.subEnabled) this._scheduleFlush();
  }

  _onPeerGone(id) {
    this.pendingSub.delete(id);
    const wasSubscribed = this.feeds.get(id) === 'subscribed' || this.feeds.get(id) === 'pending';
    this.feeds.delete(id);
    this._event('renegotiate', `peer ${id.slice(0, 8)} left`);
    if (!wasSubscribed) return;
    // Janus removes a leaver from its participants table, so the subscription must be addressed
    // by our own m-line ids (sub_mid); the feed id would no longer resolve.
    for (const [mid, m] of this.subMids) if (m.feed === id) this.pendingUnsubMids.add(mid);
    this._dropMedia(id);
    this._recountSubscribed();
    if (this.subEnabled) this._scheduleFlush();
  }

  /** Batch every change of the next renegotiateDebounceMs into one subscribe/unsubscribe/update request. */
  _scheduleFlush() {
    if (this.flushTimer || this.stopping || this.terminal) return;
    this.flushTimer = setTimeout(() => {
      this.flushTimer = null;
      this.subq.push(() => this._flushTask());
    }, this.cfg.renegotiateDebounceMs);
  }

  _layerFor(feed) {
    if (this.layerOverride) return this.layerOverride;
    const s = this.cfg.subscribe;
    return { substream: s.speaker && feed === s.speaker ? s.speakerSubstream : s.substream, temporal: s.temporal };
  }

  _streamEntry(feed) {
    // Asking for the target layer in the request itself avoids a burst of the highest layer
    // before the explicit configure below lands.
    return { feed, ...this._layerFor(feed) };
  }

  async _flushTask() {
    if (this.stopping || this.terminal) return;
    const subs = [...this.pendingSub].filter((id) => this.feeds.get(id) === 'wanted');
    const unsubs = [...this.pendingUnsubMids];
    this.pendingSub.clear();
    this.pendingUnsubMids.clear();
    if (!subs.length && !unsubs.length) return;
    for (const id of subs) this.feeds.set(id, 'pending');
    try {
      if (!this.subJoined) {
        if (!subs.length) return;
        await this._subJoin(subs);
      } else {
        await this._subUpdate(subs, unsubs);
      }
    } catch (e) {
      // A feed that left between the event and the request is not an error: drop it and go on.
      const gone = e instanceof JanusError && (e.code === NO_SUCH_FEED || (e.code === INVALID_SDP && !this.subJoined));
      if (!gone) throw e;
      this._event('warn', `subscribe skipped, feed gone (${e.code})`);
      for (const m of unsubs) this.pendingUnsubMids.add(m);
      if (subs.length > 1) await this._retryEach(subs);
      else for (const id of subs) this.feeds.delete(id);
      if (this.pendingUnsubMids.size) this._scheduleFlush();
    }
  }

  /** After a batch failed with "no such feed": find the leavers by subscribing feed by feed. */
  async _retryEach(subs) {
    for (const id of subs) {
      if (!this.feeds.has(id)) continue; // left meanwhile
      this.feeds.set(id, 'pending');
      try {
        if (!this.subJoined) await this._subJoin([id]); else await this._subUpdate([id], []);
      } catch (e) {
        if (e instanceof JanusError && (e.code === NO_SUCH_FEED || e.code === INVALID_SDP)) this.feeds.delete(id);
        else throw e;
      }
    }
  }

  async _subJoin(feeds) {
    this.phase = 'subscribe';
    if (!this.subHandle) {
      this.subHandle = await this.session.attach(PLUGIN);
      this.subHandle.on('event', ({ data, jsep }) => this._onSubscriberEvent(data, jsep));
      this.subHandle.on('trickle', (c) => this._onRemoteCandidate('sub', c));
      this.subHandle.on('slowlink', (m) => this._warn('slowlink-sub', `slowlink on subscriber (uplink=${m.uplink}, nacks=${m.nacks})`));
      this.subHandle.on('hangup', (m) => this._onHangup('sub', m.reason));
    }
    const { data, jsep } = await this.subHandle.message({
      request: 'join', ptype: 'subscriber', room: this.cfg.room, private_id: this.privateId, token: this.cfg.joinToken,
      streams: feeds.map((f) => this._streamEntry(f)),
    });
    this._guard();
    if (!jsep || jsep.type !== 'offer') throw new JanusError('subscriber join: no offer', 'no_offer');
    this.subJoined = true;
    this._ingestStreams(data && data.streams);
    await this._answerOffer(jsep);
    await this._configureLayers();
  }

  async _subUpdate(subs, unsubs) {
    this.phase = 'renegotiate';
    let body;
    if (subs.length && unsubs.length) {
      body = { request: 'update', subscribe: subs.map((f) => this._streamEntry(f)), unsubscribe: unsubs.map((m) => ({ sub_mid: m })) };
    } else if (subs.length) {
      body = { request: 'subscribe', streams: subs.map((f) => this._streamEntry(f)) };
    } else {
      body = { request: 'unsubscribe', streams: unsubs.map((m) => ({ sub_mid: m })) };
    }
    this._event('renegotiate', `${body.request} +${subs.length} -${unsubs.length}`);
    const { data, jsep } = await this.subHandle.message(body);
    this._guard();
    if (data && data.streams) this._ingestStreams(data.streams);
    // `updating` = Janus still waits for our previous answer and will push the offer by itself;
    // it then arrives as an unsolicited event and is queued by _onSubscriberEvent().
    if (jsep && jsep.type === 'offer') await this._answerOffer(jsep);
    await this._configureLayers();
  }

  /** Unsolicited events on the subscriber handle: offers pushed by Janus (autoupdate, pending offers) and layer notices. */
  _onSubscriberEvent(data, jsep) {
    if (this.stopping || this.terminal) return;
    if (jsep && jsep.type === 'offer') {
      this.subq.push(async () => {
        this.phase = 'renegotiate';
        this._ingestStreams(data && data.streams);
        await this._answerOffer(jsep);
        await this._configureLayers();
      });
    } else if (data && data.error_code !== undefined) {
      this._warn('sub-plugin-error', `subscriber handle error ${data.error_code}`);
    } else if (data && data.videoroom === 'destroyed') {
      this._fail('other', new Error('room destroyed'), 'room_destroyed');
    }
  }

  _onSubError(e) {
    if (this.stopping || this.terminal) return;
    this._fail(this.subJoined ? 'renegotiate' : 'subscribe', e);
  }

  /** Apply one Janus offer to the subscriber PC and send the answer (start + answer JSEP). */
  async _answerOffer(jsep) {
    this._checkPin(jsep.sdp, 'subscriber offer');
    if (this.timings.subOfferMs === null) this.timings.subOfferMs = this._since();
    if (!this.sub) this.sub = this._newSubPc();
    const rec = this.sub;
    await rec.pc.setRemoteDescription({ type: 'offer', sdp: jsep.sdp });
    this._guard();
    await this._flushRemoteCandidates(rec);
    const answer = await rec.pc.createAnswer();
    this._guard();
    const sdp = mungeSdp(answer.sdp); // extmap removals only (spec 4.4)
    if (rec.tLocalSet === null) rec.tLocalSet = Date.now();
    await rec.pc.setLocalDescription({ type: 'answer', sdp });
    this._guard();
    const pending = this.subHandle.message({ request: 'start' }, { jsep: { type: 'answer', sdp } });
    rec.gate.release();
    await pending;
    this._armWatchdog(rec);
  }

  _newSubPc() {
    const rec = this._newPc('sub');
    rec.gate = new TrickleGate(this.subHandle, (e) => this._warn('trickle-sub', `subscriber trickle failed (${e.code})`));
    rec.pc.addEventListener('track', (ev) => this._onTrack(ev));
    return rec;
  }

  _onTrack(ev) {
    const kind = ev.track.kind;
    this.trackInfo.set(ev.track.id, { mid: ev.transceiver.mid, kind });
    // The transform must be in place BEFORE the track reaches a media element (nothing is
    // decoded or rendered without going through the decryption).
    this._attachTransform(ev.receiver, kind, 'receiver');
    if (this.cfg.render === 'attach') this._attachMedia(ev.track, kind);
  }

  _attachMedia(track, kind) {
    if (this.mediaEls.has(track.id)) return;
    const el = document.createElement(kind === 'video' ? 'video' : 'audio');
    el.muted = true;
    el.autoplay = true;
    el.playsInline = true;
    el.srcObject = new MediaStream([track]);
    (document.getElementById('media') || document.body).appendChild(el);
    el.play().catch(() => { /* autoplay is allowed by the launch flags; a failure only skips rendering */ });
    this.mediaEls.set(track.id, el);
  }

  _dropMedia(feed) {
    const mids = new Set();
    for (const [mid, m] of this.subMids) if (m.feed === feed) mids.add(mid);
    for (const [trackId, info] of this.trackInfo) {
      if (!mids.has(info.mid)) continue;
      this._removeEl(trackId);
    }
  }

  _removeEl(trackId) {
    const el = this.mediaEls.get(trackId);
    if (!el) return;
    el.srcObject = null;
    el.remove();
    this.mediaEls.delete(trackId);
  }

  /** Map subscriber m-line ids to feeds from the `streams` summary Janus sends with every offer. */
  _ingestStreams(streams) {
    if (!Array.isArray(streams)) return;
    for (const s of streams) {
      if (!s || s.mid === undefined || s.mid === null) continue;
      const mid = String(s.mid);
      if (s.active === false || s.feed_id === undefined || s.feed_id === null) {
        this.subMids.delete(mid);
        this.configured.delete(mid);
      } else {
        const feed = String(s.feed_id);
        this.subMids.set(mid, { type: s.type, feed });
        // A peer that left while its subscription was in flight: release the m-line again.
        if (!this.feeds.has(feed)) { this.pendingUnsubMids.add(mid); this._scheduleFlush(); }
      }
    }
    for (const [id, st] of this.feeds) {
      if (st !== 'wanted' && [...this.subMids.values()].some((m) => m.feed === id)) this.feeds.set(id, 'subscribed');
    }
    this._recountSubscribed();
  }

  _recountSubscribed() {
    const feeds = new Set();
    for (const m of this.subMids.values()) if (this.feeds.has(m.feed)) feeds.add(m.feed);
    this.peersInfo.subscribed = feeds.size;
  }

  /** Ask Janus for the wanted substream/temporal layer on every remote video m-line not yet configured that way. */
  async _configureLayers() {
    const streams = [];
    for (const [mid, m] of this.subMids) {
      if (m.type !== 'video') continue;
      const l = this._layerFor(m.feed);
      const key = `${l.substream}/${l.temporal}`;
      if (this.configured.get(mid) !== key) streams.push({ mid, substream: l.substream, temporal: l.temporal, _key: key });
    }
    if (!streams.length) return;
    try {
      await this.subHandle.message({ request: 'configure', streams: streams.map(({ mid, substream, temporal }) => ({ mid, substream, temporal })) });
      for (const s of streams) this.configured.set(s.mid, s._key);
      this._event('layer', `configure ${streams.length} video stream(s): ${streams.map((s) => `${s.mid}=${s._key}`).join(' ')}`);
    } catch (e) {
      // Not fatal: the stream keeps working on the default layer.
      this._warn('configure', `configure failed (${e.code})`);
    }
  }

  setSubstream(substream, temporal) {
    this.layerOverride = { substream, temporal: temporal === undefined || temporal === null ? this.cfg.subscribe.temporal : temporal };
    this.configured.clear();
    this.subq.push(() => this._configureLayers());
  }

  // ------------------------------------------------------- progress and sampling

  _startTimers() {
    if (this.sampleTimer) return;
    this.lastSampleT = Date.now();
    this.sampleTimer = setInterval(() => { this._sampleTick().catch(() => {}); }, this.cfg.statsIntervalMs);
    // Fine grained (250 ms) watch of the inbound media until every peer delivers, so that the
    // join timings are not quantised to the sampling interval. A bot that still waits after 20 s
    // (a peer that never joins) drops to 1 Hz: the regular sampler covers the rest.
    this.progressTimer = setInterval(() => { this._progressTick().catch(() => {}); }, 250);
    this.progressSlowTimer = setTimeout(() => {
      clearInterval(this.progressTimer);
      this.progressTimer = setInterval(() => { this._progressTick().catch(() => {}); }, 1000);
    }, 20000);
  }

  _stopTimers() {
    clearInterval(this.sampleTimer);
    clearInterval(this.progressTimer);
    clearTimeout(this.progressSlowTimer);
    this.sampleTimer = null;
    this.progressTimer = null;
    clearTimeout(this.flushTimer);
    this.flushTimer = null;
    for (const rec of [this.pub, this.sub]) if (rec) clearTimeout(rec.watchdog);
  }

  async _progressTick() {
    if (this.progressBusy || this.terminal || this.stopping) return;
    if (this.state === 'steady') { clearInterval(this.progressTimer); clearTimeout(this.progressSlowTimer); this.progressTimer = null; return; }
    if (!this.sub || this.peersInfo.expected === 0) { this._checkState(); return; }
    this.progressBusy = true;
    try {
      const byId = statsById(await this.sub.pc.getStats());
      this._applyFlow([...byId.values()].filter((r) => r.type === 'inbound-rtp'));
    } finally {
      this.progressBusy = false;
    }
  }

  /** Which expected peers deliver audio / video, judged from the inbound-rtp reports of the subscriber PC. */
  _applyFlow(inbound) {
    const audio = new Set();
    const video = new Set();
    for (const r of inbound) {
      if (r.isRemote) continue;
      const kind = r.kind || r.mediaType;
      const mid = r.mid !== undefined && r.mid !== null ? String(r.mid) : (this.trackInfo.get(r.trackIdentifier) || {}).mid;
      const m = mid !== undefined && mid !== null ? this.subMids.get(String(mid)) : null;
      if (!m) continue;
      if (kind === 'audio' && (r.packetsReceived || 0) > 0) audio.add(m.feed);
      // With render 'attach' the video must actually decode; without a sink, packets are all we can tell.
      if (kind === 'video' && (this.cfg.render === 'attach' ? (r.framesDecoded || 0) > 0 : (r.packetsReceived || 0) > 0)) video.add(m.feed);
    }
    let a = 0;
    let v = 0;
    for (const p of this.cfg.peers) { if (audio.has(p)) a++; if (video.has(p)) v++; }
    this.peersInfo.audioFlowing = a;
    this.peersInfo.videoFlowing = v;
    if (a > 0 && this.timings.firstAudioMs === null) this.timings.firstAudioMs = this._since();
    if (v > 0 && this.timings.firstVideoMs === null) this.timings.firstVideoMs = this._since();
    this._checkState();
  }

  /** connected = both PCs connected; steady = additionally every expected peer delivers media. */
  _checkState() {
    if (this.terminal || this.stopping) return;
    const expected = this.peersInfo.expected;
    const pubOk = !!this.pub && this.pub.pc.connectionState === 'connected';
    const subOk = expected === 0 || (!!this.sub && this.sub.pc.connectionState === 'connected');
    if (!(pubOk && subOk)) return;
    this._advance('connected');
    const flowOk = this.peersInfo.audioFlowing >= expected && (!this.cfg.media.video || this.peersInfo.videoFlowing >= expected);
    if (flowOk) {
      this._advance('steady');
      if (this.timings.allPeersMs === null) this.timings.allPeersMs = this._since();
    }
  }

  /** Per-stream delta of one cumulative counter; never negative (a reset counts from zero). */
  _delta(role, r, field, scale = 1) {
    const cur = num(r[field]);
    const key = `${role}|${r.id}|${field}`;
    const prev = this.prev.get(key);
    this.seen.set(key, cur === null ? 0 : cur);
    if (cur === null) return 0;
    const raw = prev === undefined || cur < prev ? cur : cur - prev;
    return Math.max(0, raw) * scale;
  }

  _accumulate(cat, role, r, d) {
    const out = {};
    for (const [key, field, scale] of SPEC[cat]) {
      let v = this._delta(role, r, field, scale || 1);
      if (scale) v = round(v, 3);
      d[cat][key] += v;
      out[key] = v;
    }
    return out;
  }

  async _sampleTick(force = false) {
    if (this.sampling || this.terminal || (this.stopping && !force) || !this.pub) return;
    this.sampling = true;
    try {
      const pubById = statsById(await this.pub.pc.getStats());
      const subById = this.sub ? statsById(await this.sub.pc.getStats()) : null;
      const e2ee = this.cfg.e2ee.mode !== 'none' ? await this.host.counters(this.id) : null;
      if (this.terminal) return;
      // The interval is measured between the publisher stats snapshots themselves (their own
      // timestamps), not between the moments this async code resumed: on a loaded machine the two
      // differ by seconds and would skew every per-second rate.
      const t = Math.round(statsTime(pubById));
      const dtMs = t - this.lastSampleT;
      if (dtMs <= 0) return;
      this.lastSampleT = t;
      const sample = this._buildSample(pubById, subById, t, dtMs);
      for (const c of CATS) for (const [key] of SPEC[c]) this.tot[c][key] += sample.d[c][key];
      if (e2ee) {
        this.tot.e2eeEnc = e2ee.enc.audio + e2ee.enc.video;
        this.tot.e2eeDec = e2ee.dec.audio + e2ee.dec.video;
        const fail = e2ee.fail.audio + e2ee.fail.video;
        if (fail > this.tot.e2eeFail) this._warn('e2ee-fail', `e2ee frame failures so far ${JSON.stringify(e2ee.reasons || {})}`, 10000);
        this.tot.e2eeFail = fail;
      }
      this._checkLayers(sample);
      this._checkEncrypting(e2ee);
      this.samples.push(sample);
      if (this.samples.length > MAX_SAMPLES) this.samples.splice(0, this.samples.length - MAX_SAMPLES);
      if (subById) this._applyFlow([...subById.values()].filter((r) => r.type === 'inbound-rtp'));
    } catch (e) {
      this._warn('stats', `stats sampling failed (${(e && e.name) || 'error'})`, 10000);
    } finally {
      this.sampling = false;
    }
  }

  /**
   * A sender transform that Chromium ignores (it must be attached synchronously after the sender is
   * created) would put clear frames on the wire and silently invalidate an E2EE run: fail loudly.
   */
  _checkEncrypting(e2ee) {
    if (!e2ee || this.tot.aOut.packets + this.tot.vOut.packets < 60) return;
    if (e2ee.enc.audio + e2ee.enc.video === 0) {
      this._fail('other', new Error('the e2ee sender transform processed no frame although packets were sent'), 'e2ee_inactive');
    }
  }

  /**
   * Chromium silently drops simulcast layers for small captures (legacy layer limit: 3 layers need
   * >= 960x540), which would make the load unrealistically light. Say so once, early.
   */
  _checkLayers(sample) {
    const want = this.cfg.video.encodings.map((e) => e.rid);
    if (!this.cfg.media.video || want.length < 2) return;
    for (const [rid, l] of Object.entries(sample.g.vOutLayers)) this.layerTotals.set(rid, (this.layerTotals.get(rid) || 0) + l.packets);
    this.layerSamples += 1;
    if (this.layerSamples !== 6) return;
    const missing = want.filter((rid) => !(this.layerTotals.get(rid) > 0));
    if (missing.length && missing.length < want.length) {
      this._event('warn', `simulcast layer(s) ${missing.join(',')} send nothing (Chromium limits layers by capture size; `
        + 'launch it with --force-fieldtrials=WebRTC-LegacySimulcastLayerLimit/Disabled/ or capture at least 960x540)');
    }
  }

  _buildSample(pubById, subById, t, dtMs) {
    this.seen = new Map(); // becomes the baseline of the next sample
    const d = {};
    for (const c of CATS) d[c] = zeroCat(SPEC[c]);
    const g = {
      aInStreams: 0, vInStreams: 0, aJitterMsMax: null, vJitterMsMax: null, vWidthMin: null, vWidthMax: null, vFps: null,
      pubRttMs: null, subRttMs: null, availIn: null, availOut: null, aOutPps: null, vOutLayers: {}, qualityLimit: null,
    };
    const fps = [];
    const maxOf = (cur, x) => (x === null ? cur : (cur === null || x > cur ? x : cur));
    const minOf = (cur, x) => (x === null ? cur : (cur === null || x < cur ? x : cur));

    if (subById) {
      for (const r of subById.values()) {
        if (r.type !== 'inbound-rtp' || r.isRemote) continue;
        const kind = r.kind || r.mediaType;
        if (kind === 'audio') {
          g.aInStreams++;
          this._accumulate('aIn', 'sub', r, d);
          const j = num(r.jitter);
          g.aJitterMsMax = maxOf(g.aJitterMsMax, j === null ? null : round(j * 1000, 1));
        } else if (kind === 'video') {
          g.vInStreams++;
          this._accumulate('vIn', 'sub', r, d);
          const j = num(r.jitter);
          g.vJitterMsMax = maxOf(g.vJitterMsMax, j === null ? null : round(j * 1000, 1));
          const w = num(r.frameWidth);
          g.vWidthMin = minOf(g.vWidthMin, w);
          g.vWidthMax = maxOf(g.vWidthMax, w);
          const f = num(r.framesPerSecond);
          if (f !== null) fps.push(f);
        }
      }
      const { pair } = selectedPair(subById);
      if (pair) {
        const rtt = num(pair.currentRoundTripTime);
        g.subRttMs = rtt === null ? null : round(rtt * 1000, 1);
        g.availIn = num(pair.availableIncomingBitrate);
      }
    }
    if (fps.length) g.vFps = round(fps.reduce((a, b) => a + b, 0) / fps.length, 1);

    let quality = null;
    for (const r of pubById.values()) {
      if (r.type !== 'outbound-rtp') continue;
      const kind = r.kind || r.mediaType;
      if (kind === 'audio') {
        this._accumulate('aOut', 'pub', r, d);
      } else if (kind === 'video') {
        const dv = this._accumulate('vOut', 'pub', r, d);
        if (r.rid) g.vOutLayers[r.rid] = { packets: dv.packets, bytes: dv.bytes, w: num(r.frameWidth), h: num(r.frameHeight) };
        const q = r.qualityLimitationReason;
        if (q && (quality === null || (QUALITY_RANK[q] || 0) > (QUALITY_RANK[quality] || 0))) quality = q;
      }
    }
    g.qualityLimit = quality;
    const { pair } = selectedPair(pubById);
    if (pair) {
      const rtt = num(pair.currentRoundTripTime);
      g.pubRttMs = rtt === null ? null : round(rtt * 1000, 1);
      g.availOut = num(pair.availableOutgoingBitrate);
    }
    if (g.pubRttMs === null) {
      const rtts = [];
      for (const r of pubById.values()) if (r.type === 'remote-inbound-rtp' && num(r.roundTripTime) !== null) rtts.push(r.roundTripTime * 1000);
      if (rtts.length) g.pubRttMs = round(rtts.reduce((a, b) => a + b, 0) / rtts.length, 1);
    }
    g.aOutPps = dtMs > 0 ? round((d.aOut.packets * 1000) / dtMs, 1) : null;
    this.prev = this.seen; // streams that vanished drop out of the baseline
    return { t, dtMs, d, g };
  }

  // ------------------------------------------------------------------- stop

  /** Stop the bot; resolves with the final status (BotFinal). Idempotent. */
  stop({ graceful = true } = {}) {
    if (this.final) return Promise.resolve(this.final);
    if (this.stopPromise) return this.stopPromise;
    this.stopPromise = (async () => {
      const wasFailed = this.state === 'failed';
      const wasTerminal = this.terminal;
      this.stopping = true;
      if (!wasTerminal) { this.state = 'leaving'; this._event('state', 'leaving'); }
      this.subq.closed = true;
      this._stopTimers();
      if (graceful && !wasTerminal && this.pub) {
        // One last sample so that tot includes the final partial interval.
        for (let i = 0; i < 75 && this.sampling; i++) await sleep(20);
        await this._sampleTick(true).catch(() => {});
      }
      await this._teardown(graceful && !wasTerminal);
      if (!wasFailed && this.state !== 'closed') { this.state = 'closed'; this._event('state', 'closed'); }
      this.final = this.status(true);
      this.host.forget(this.id);
      return this.final;
    })();
    return this.stopPromise;
  }

  /** Release everything. graceful = leave both handles, detach and destroy the Janus session first. */
  _teardown(graceful) {
    if (this._teardownPromise) return this._teardownPromise;
    this._teardownPromise = (async () => {
      this._stopTimers();
      this.subq.closed = true;
      const s = this.session;
      if (s && graceful && !s.closed) {
        const handles = [this.subHandle, this.pubHandle].filter(Boolean);
        await Promise.allSettled(handles.map((h) => h.message({ request: 'leave' }, { timeoutMs: 2000 })));
        await Promise.allSettled(handles.map((h) => h.detach()));
        await s.destroy();
      } else if (s) {
        s.close();
      }
      for (const rec of [this.pub, this.sub]) {
        if (!rec) continue;
        try { rec.pc.close(); } catch (e) { /* ignore */ }
      }
      if (this.stream) this.stream.getTracks().forEach((t) => t.stop());
      for (const id of [...this.mediaEls.keys()]) this._removeEl(id);
    })();
    return this._teardownPromise;
  }
}

// ---------------------------------------------------------------------- window.qbot

const host = new E2eeHost();
const bots = new Map(); // id -> Bot, until stop() returns its final status
const finals = new Map(); // id -> final status of stopped bots (makes stop() idempotent)

function rememberFinal(id, fin) {
  finals.set(id, fin);
  if (finals.size > 2000) finals.delete(finals.keys().next().value);
}

// A stray rejection must never surface as an unhandled page error; it is only noted on the bots.
window.addEventListener('unhandledrejection', (ev) => {
  ev.preventDefault();
  const reason = ev.reason;
  for (const b of bots.values()) b._warn('unhandled', `unhandled rejection: ${(reason && reason.message) || reason}`, 10000);
});

async function stopBot(botId, opts) {
  const b = bots.get(botId);
  if (!b) return finals.get(botId) || null;
  const fin = await b.stop(opts || {});
  bots.delete(botId);
  rememberFinal(botId, fin);
  return fin;
}

window.qbot = {
  info() {
    const m = /(?:Headless)?Chrome\/(\d+(?:\.\d+)*)/.exec(navigator.userAgent);
    return {
      ua: navigator.userAgent,
      chromeVersion: m ? m[1] : null,
      hasScriptTransform,
      hardwareConcurrency: navigator.hardwareConcurrency,
    };
  },

  async start(cfg) {
    const c = normalizeConfig(cfg);
    if (bots.has(c.id)) throw new Error('a bot with this id is already running');
    finals.delete(c.id);
    const bot = new Bot(c, host);
    bots.set(c.id, bot);
    bot.run(); // runs in the background; failures end as state 'failed'
    return { ok: true };
  },

  poll() {
    return { now: Date.now(), bots: [...bots.values()].map((b) => b.status(true)) };
  },

  async setTokens(map) {
    for (const [id, token] of Object.entries(map || {})) {
      const b = bots.get(id);
      if (!b || typeof token !== 'string' || !token) continue;
      b.setToken(token);
    }
  },

  stop: stopBot,

  async stopAll(opts) {
    return Promise.all([...bots.keys()].map((id) => stopBot(id, opts)));
  },

  setSubstream(botId, substream, temporal) {
    const b = bots.get(botId);
    if (b && !b.terminal) b.setSubstream(substream, temporal);
  },
};
window.qbotReady = true;
