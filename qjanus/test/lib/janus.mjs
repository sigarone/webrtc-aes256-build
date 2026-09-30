// Janus API clients for the qjanus tests. They are also the executable reference of the two API
// surfaces of GROUP_CALLS_V2: ServerApi is what the application server implements (section 3, over
// the node-local HTTP transport), WsClient is what the apps implement (section 4, over WebSockets).
// No dependencies beyond Node >= 22 (global fetch and WebSocket).
import crypto from 'node:crypto';

export const PLUGIN = 'janus.plugin.videoroom';
export const hex = (bytes) => crypto.randomBytes(bytes).toString('hex');
export const sleep = (ms) => new Promise((r) => setTimeout(r, ms));
// The join token of a publisher (spec section 12.2): "<pseudonym>:<32 lowercase hex>". qjanus refuses a
// publisher join whose id is missing or differs from the prefix of its token, so the token a member was
// given cannot be used under the pseudonym of another member.
export const joinTokenFor = (id) => `${id}:${hex(16)}`;

// Janus core signed token: "<expiry>,janus,<plugin>:<base64 HMAC>" with token_auth_hash = sha256; the
// HMAC key is the secret string itself. `now` is in ms; a negative ttl mints an already expired token.
export function mintToken(secret, { ttl = 600, plugins = [PLUGIN], realm = 'janus', now = Date.now() } = {}) {
  const expiry = Math.floor(now / 1000) + ttl;
  const data = [expiry, realm, ...plugins].join(',');
  return `${data}:${crypto.createHmac('sha256', secret).update(data).digest('base64')}`;
}

// what the log-hygiene checks must never find in a log: every secret the tests handed to Janus
export const secretsSeen = new Set();
export const remember = (...values) => values.forEach((v) => v && secretsSeen.add(String(v)));

export class JanusError extends Error {
  constructor(what, resp) {
    super(`${what}: ${JSON.stringify(resp)}`);
    this.resp = resp;
    this.code = (resp && resp.error && resp.error.code) || (resp && resp.error_code) || 0;
  }
}

// Plugin level errors travel inside plugindata.data ({videoroom:"event", error_code, error})
export const pluginData = (m) => (m && m.plugindata && m.plugindata.data) || {};

// ---------------------------------------------------------------------------------- server API
// HTTP transport, one request/response per call; every plugin request is synchronous.
export class HttpApi {
  constructor(base, { tokenSecret, adminKey }) {
    this.base = base;
    this.tokenSecret = tokenSecret;
    this.adminKey = adminKey;
    remember(tokenSecret, adminKey);
  }
  token(opts) { return mintToken(this.tokenSecret, opts); }
  async post(path, body) {
    const r = await fetch(this.base + path, {
      method: 'POST', headers: { 'content-type': 'application/json' },
      body: JSON.stringify({ transaction: hex(8), ...body }),
    });
    return { status: r.status, json: await r.json() };
  }
  async get(path) {
    const r = await fetch(this.base + path);
    return { status: r.status, json: await r.json() };
  }
  info() { return this.get('/info'); }
  ping() { return this.post('', { janus: 'ping' }); }
  async createSession(token) {
    const { json } = await this.post('', { janus: 'create', ...(token ? { token } : {}) });
    if (json.janus !== 'success') throw new JanusError('create', json);
    return json.data.id;
  }
  async attach(sid, token, plugin = PLUGIN) {
    const { json } = await this.post(`/${sid}`, { janus: 'attach', plugin, ...(token ? { token } : {}) });
    if (json.janus !== 'success') throw new JanusError('attach', json);
    return json.data.id;
  }
  destroySession(sid, token) { return this.post(`/${sid}`, { janus: 'destroy', ...(token ? { token } : {}) }); }
  // raw plugin message; returns the whole envelope
  message(sid, handle, token, body) {
    return this.post(`/${sid}/${handle}`, { janus: 'message', ...(token ? { token } : {}), body });
  }
}

// The calls of spec section 3. One Janus session + one VideoRoom handle per operation batch
// (session_timeout is 60 s and the server does not keep long-poll loops).
export class ServerApi {
  constructor(http) { this.http = http; }
  // fn({ vr, sid, handle, token }); vr(body, {admin}) returns plugindata.data (plugin-level errors included)
  async session(fn, token = this.http.token()) {
    const sid = await this.http.createSession(token);
    try {
      const handle = await this.http.attach(sid, token);
      const vr = async (body, { admin = false } = {}) => {
        const req = admin ? { ...body, admin_key: this.http.adminKey } : body;
        const { json } = await this.http.message(sid, handle, token, req);
        if (json.janus !== 'success') throw new JanusError(`videoroom ${body.request}`, json);
        return pluginData(json);
      };
      return await fn({ vr, sid, handle, token });
    } finally {
      await this.http.destroySession(sid, token).catch(() => {});
    }
  }
  // create: the parameters of spec section 3
  createRoom({ room, secret, publishers, allowed }) {
    return this.session(({ vr }) => vr({
      request: 'create', room, is_private: true, secret, publishers, bitrate: 1500000, fir_freq: 0,
      audiocodec: 'opus', videocodec: 'vp8', opus_fec: true, opus_dtx: false,
      audiolevel_ext: false, audiolevel_event: false, videoorient_ext: false, playoutdelay_ext: false,
      transport_wide_cc_ext: true, record: false, lock_record: true, require_pvtid: true,
      require_e2ee: true, notify_joining: false, allowed,
    }, { admin: true }));
  }
  allow(room, secret, action, tokens) {
    return this.session(({ vr }) => vr({ request: 'allowed', room, secret, action, allowed: tokens }));
  }
  kick(room, secret, id) { return this.session(({ vr }) => vr({ request: 'kick', room, secret, id })); }
  listParticipants(room) { return this.session(({ vr }) => vr({ request: 'listparticipants', room })); }
  destroyRoom(room, secret) { return this.session(({ vr }) => vr({ request: 'destroy', room, secret })); }
  listRooms() { return this.session(({ vr }) => vr({ request: 'list' }, { admin: true })); }
  exists(room) { return this.session(({ vr }) => vr({ request: 'exists', room })); }
}

// --------------------------------------------------------------------------- client WebSocket API
// Janus JSON over WebSocket (subprotocol janus-protocol). Replies are matched by `transaction`;
// everything else (webrtcup, media, slowlink, hangup, trickle, plugin events) lands in `events`.
export class WsClient {
  constructor(url, { tokenSecret, token } = {}) {
    this.url = url;
    this.tokenSecret = tokenSecret;
    this.token = token || (tokenSecret ? mintToken(tokenSecret) : undefined);
    this.pending = new Map();
    this.events = [];
    this.waiters = [];
    this.closed = false;
    this.sid = 0;
    this.tx = 0;
  }
  open() {
    return new Promise((resolve, reject) => {
      const ws = new WebSocket(this.url, 'janus-protocol');
      this.ws = ws;
      ws.addEventListener('open', () => resolve(this));
      ws.addEventListener('error', () => reject(new Error('websocket error')));
      ws.addEventListener('close', (e) => {
        this.closed = true;
        this.closeInfo = { code: e.code };
        for (const [, p] of this.pending) p.reject(new Error('websocket closed'));
        this.pending.clear();
        this.wake();
      });
      ws.addEventListener('message', (ev) => this.onMessage(JSON.parse(ev.data)));
    });
  }
  wake() { for (const w of [...this.waiters]) w(); }
  onMessage(m) {
    const p = m.transaction ? this.pending.get(m.transaction) : null;
    if (p && (m.janus !== 'ack' || p.ackOnly)) {
      this.pending.delete(m.transaction);
      p.resolve(m);
      return;
    }
    if (m.janus === 'ack') return;
    this.events.push(m);
    this.wake();
  }
  // send({janus,...}, {ack}): resolves with the first reply that is not an ack (or the ack when
  // `ack` is set: keepalive, trickle, claim); a WS request never resolves for a dead connection
  send(obj, { ack = false, token = this.token, ms = 10000 } = {}) {
    const transaction = `t${++this.tx}-${hex(6)}`;
    const msg = { ...obj, transaction, ...(token && obj.janus !== 'info' && obj.janus !== 'ping' ? { token } : {}) };
    return new Promise((resolve, reject) => {
      const timer = setTimeout(() => { this.pending.delete(transaction); reject(new Error(`timeout: ${obj.janus} ${obj.body ? obj.body.request : ''}`)); }, ms);
      this.pending.set(transaction, { ackOnly: ack, resolve: (m) => { clearTimeout(timer); resolve(m); }, reject: (e) => { clearTimeout(timer); reject(e); } });
      this.ws.send(JSON.stringify(msg));
    });
  }
  async create(token) {
    const m = await this.send({ janus: 'create' }, { token });
    if (m.janus !== 'success') throw new JanusError('create', m);
    this.sid = m.data.id;
    return this.sid;
  }
  async attach(token) {
    const m = await this.send({ janus: 'attach', session_id: this.sid, plugin: PLUGIN }, { token });
    if (m.janus !== 'success') throw new JanusError('attach', m);
    return m.data.id;
  }
  keepalive(token) { return this.send({ janus: 'keepalive', session_id: this.sid }, { ack: true, token }); }
  info() { return this.send({ janus: 'info' }); }
  // plugin message; resolves with the first non-ack envelope (sync `success` with plugindata, or the
  // async `event`, possibly with a jsep, or `error`)
  message(handle, body, jsep, opts) {
    return this.send({ janus: 'message', session_id: this.sid, handle_id: handle, body, ...(jsep ? { jsep } : {}) }, opts);
  }
  trickle(handle, candidate) {
    return this.send({ janus: 'trickle', session_id: this.sid, handle_id: handle, ...(candidate ? { candidate } : { candidate: { completed: true } }) }, { ack: true });
  }
  detach(handle) { return this.send({ janus: 'detach', session_id: this.sid, handle_id: handle }); }
  destroy() { return this.send({ janus: 'destroy', session_id: this.sid }); }
  claim(sid, token) { this.sid = sid; return this.send({ janus: 'claim', session_id: sid }, { token }); }
  // waits for an unsolicited event; consumes it
  waitEvent(pred, ms = 15000) {
    return new Promise((resolve, reject) => {
      const t = setTimeout(() => { cleanup(); reject(new Error('timeout waiting for a janus event')); }, ms);
      const check = () => {
        const i = this.events.findIndex(pred);
        if (i >= 0) { const [e] = this.events.splice(i, 1); cleanup(); resolve(e); }
      };
      const cleanup = () => { clearTimeout(t); this.waiters = this.waiters.filter((w) => w !== check); };
      this.waiters.push(check);
      check();
    });
  }
  startKeepalive(everyMs = 25000) {
    this.ka = setInterval(() => { this.keepalive().catch(() => {}); }, everyMs);
  }
  close() {
    clearInterval(this.ka);
    try { this.ws.close(); } catch (e) { /* already closed */ }
  }
}
