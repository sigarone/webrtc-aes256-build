// Minimal Janus WebSocket API client (session + plugin handles).
//
// Isomorphic on purpose: it only touches globalThis.WebSocket / crypto / timers, so the
// same file runs in the bot page (Chromium) and in plain Node unit tests (with an injected
// WebSocketImpl such as the `ws` package).
//
// Janus protocol facts this client encodes (verified against Janus v1.4.2):
//  - Every request carries a random `transaction`; replies are matched by it.
//  - Plugin requests (`message`) over WebSocket are answered by `{"janus":"ack"}` first and,
//    later, by an `event` (async plugin request) or a `success` (sync plugin request) with the
//    SAME transaction. The promise returned by JanusHandle.message() resolves on that second
//    frame, never on the ack.
//  - With token authentication on, EVERY session-level request needs the signed token at the
//    JSON root (`token`), and the token must be unexpired at that moment. Tokens are short
//    lived, so the caller refreshes them with setToken(); the LATEST token goes on every
//    request, including keepalives, trickles and the final destroy.
//  - Plugin errors come back inside a normal `event`: plugindata.data.error_code/error.
//  - Frames without a matching transaction are unsolicited: plugin events (a new offer, a new
//    publisher...), `trickle`, `webrtcup`, `media`, `slowlink`, `hangup`, `detached`, `timeout`.
//    They are routed to the handle named in `sender`.

/** Error raised for Janus/WebSocket failures. `code` is the Janus error code (number) or a string. */
export class JanusError extends Error {
  constructor(message, code, extra = {}) {
    super(message);
    this.name = 'JanusError';
    /** Janus numeric error code, or one of 'timeout' | 'ws_closed' | 'ws_error' | 'detached' | 'session_timeout' | 'bad_response'. */
    this.code = code;
    Object.assign(this, extra);
  }
}

const WS_OPEN = 1;

/** 96-bit random transaction id (the spec asks for >= 64 bit). */
function newTransaction() {
  const b = new Uint8Array(12);
  globalThis.crypto.getRandomValues(b);
  let s = '';
  for (const x of b) s += x.toString(16).padStart(2, '0');
  return s;
}

/** Janus error frames carry {code, reason}. */
function errorFrame(msg) {
  const e = msg.error || {};
  return new JanusError(String(e.reason || 'janus error'), typeof e.code === 'number' ? e.code : 'bad_response');
}

/** A plugin answer carrying error_code is an error, even though Janus framed it as `event`. */
function pluginError(data) {
  if (data && typeof data === 'object' && data.error_code !== undefined) {
    return new JanusError(String(data.error || 'plugin error'), data.error_code, { plugin: true });
  }
  return null;
}

export class JanusSession {
  /**
   * @param {object} o
   * @param {string} o.url            ws:// or wss:// URL (never logged)
   * @param {string|null} o.token     signed session token, sent as root `token` on every request
   * @param {number} [o.keepaliveMs]
   * @param {number} [o.requestTimeoutMs]
   * @param {Function} [o.WebSocketImpl]   injectable for tests
   * @param {Function} [o.onClose]         called once when the socket dies unexpectedly ({code, reason})
   * @param {Function} [o.onKeepaliveError] called with (error, consecutiveFailures) when a keepalive fails
   * @param {Function} [o.log]             log(message); never receives the URL or tokens
   */
  constructor({ url, token = null, keepaliveMs = 25000, requestTimeoutMs = 8000,
    WebSocketImpl = globalThis.WebSocket, onClose = null, onKeepaliveError = null, log = null } = {}) {
    if (!url) throw new JanusError('missing url', 'bad_config');
    this._url = url;
    this._token = token || null;
    this.keepaliveMs = keepaliveMs;
    this.requestTimeoutMs = requestTimeoutMs;
    this._WebSocket = WebSocketImpl;
    this._onClose = onClose;
    this._onKeepaliveError = onKeepaliveError;
    this._log = log || (() => {});
    this._ws = null;
    this._pending = new Map(); // transaction -> pending request
    this._handles = new Map(); // handle id -> JanusHandle
    this._keepaliveTimer = null;
    this._keepaliveFailures = 0;
    this._closing = false; // set when WE close the socket, so onClose stays silent
    this.closed = false;
    this.sessionId = null;
    /** epoch ms when the socket opened / the session was created (for the bot timings). */
    this.openedAt = null;
    this.createdAt = null;
  }

  /** Replace the token used by every following request. */
  setToken(token) {
    this._token = token || null;
  }

  /** Open the socket (subprotocol `janus-protocol`) and create the Janus session. */
  async connect({ timeoutMs = this.requestTimeoutMs } = {}) {
    await this._open(timeoutMs);
    this.openedAt = Date.now();
    const r = await this._request({ janus: 'create' }, { expect: 'sync', timeoutMs });
    const id = r.data && r.data.id;
    if (id === undefined || id === null) throw new JanusError('create: no session id', 'bad_response');
    this.sessionId = id;
    this.createdAt = Date.now();
    this._startKeepalive();
  }

  /** Attach to a plugin and return its handle. */
  async attach(plugin) {
    const r = await this._request({ janus: 'attach', plugin }, { expect: 'sync' });
    const id = r.data && r.data.id;
    if (id === undefined || id === null) throw new JanusError('attach: no handle id', 'bad_response');
    const handle = new JanusHandle(this, id, plugin);
    this._handles.set(id, handle);
    return handle;
  }

  /** Destroy the Janus session (best effort) and close the socket. Never rejects. */
  async destroy() {
    if (this.closed) return;
    this._stopKeepalive();
    if (this._ws && this._ws.readyState === WS_OPEN && this.sessionId !== null) {
      try {
        await this._request({ janus: 'destroy' }, { expect: 'sync', timeoutMs: Math.min(this.requestTimeoutMs, 2000) });
      } catch (e) { /* the session dies with the socket anyway */ }
    }
    this.close();
  }

  /** Hard close, no Janus round trip. Idempotent. */
  close() {
    this._closing = true;
    this._teardown({ code: 'closed', reason: 'closed by client' });
  }

  // ------------------------------------------------------------------ internals

  _open(timeoutMs) {
    return new Promise((resolve, reject) => {
      let settled = false;
      let ws;
      const done = (err) => {
        if (settled) return;
        settled = true;
        clearTimeout(timer);
        if (err) {
          this._closing = true; // the failed socket must not fire onClose
          try { ws.close(); } catch (e) { /* ignore */ }
          this.closed = true;
          reject(err);
        } else {
          resolve();
        }
      };
      const timer = setTimeout(() => done(new JanusError('websocket open timed out', 'timeout')), timeoutMs);
      try {
        ws = new this._WebSocket(this._url, 'janus-protocol');
      } catch (e) {
        clearTimeout(timer);
        this.closed = true;
        reject(new JanusError('websocket could not be created', 'ws_error'));
        return;
      }
      this._ws = ws;
      ws.addEventListener('open', () => done(null));
      ws.addEventListener('error', () => done(new JanusError('websocket connection failed', 'ws_error')));
      ws.addEventListener('message', (ev) => this._onMessage(ev));
      ws.addEventListener('close', (ev) => {
        // A close before `open` is a connection failure, after it a dropped session.
        done(new JanusError('websocket closed during connect', 'ws_error'));
        this._teardown({ code: 'ws_closed', reason: ev && ev.reason ? String(ev.reason) : '' });
      });
    });
  }

  _onMessage(ev) {
    let msg;
    try {
      const raw = typeof ev.data === 'string' ? ev.data : String(ev.data);
      msg = JSON.parse(raw);
    } catch (e) {
      this._log('dropped a non-JSON frame');
      return;
    }
    if (!msg || typeof msg !== 'object') return;
    const p = msg.transaction ? this._pending.get(msg.transaction) : undefined;
    if (p && this._settle(p, msg)) return;
    this._route(msg);
  }

  /**
   * Try to complete a pending request with `msg`. Returns true when the frame belonged to it
   * (even if the request stays pending, e.g. an ack while waiting for the event).
   */
  _settle(p, msg) {
    switch (msg.janus) {
      case 'ack':
        if (p.expect === 'ack') return this._finish(p, msg);
        p.acked = true; // keep waiting for the event/success of the same transaction
        return true;
      case 'error':
        return this._finish(p, null, errorFrame(msg));
      case 'success':
      case 'event': {
        const data = msg.plugindata ? msg.plugindata.data : (msg.data || null);
        const perr = msg.plugindata ? pluginError(data) : null;
        if (perr) return this._finish(p, null, perr);
        return this._finish(p, { msg, data, jsep: msg.jsep || null });
      }
      default:
        return this._finish(p, { msg, data: msg.data || null, jsep: null });
    }
  }

  _finish(p, value, err) {
    clearTimeout(p.timer);
    this._pending.delete(p.transaction);
    if (err) p.reject(err); else p.resolve(value);
    return true;
  }

  /** Unsolicited frames: handle events and session-level notifications. */
  _route(msg) {
    if (msg.janus === 'ack' || msg.janus === 'success') return; // late duplicate of a settled request
    if (msg.janus === 'timeout') {
      // Janus reaped the session (keepalives did not get through, or the token expired).
      this._teardown({ code: 'session_timeout', reason: 'janus session timed out' });
      return;
    }
    if (msg.sender !== undefined) {
      const h = this._handles.get(msg.sender);
      if (h) h._dispatch(msg);
    }
  }

  /**
   * Send one request and wait for its answer.
   * expect: 'sync' (success), 'ack' (keepalive/trickle) or 'event' (plugin message: ack, then event/success)
   */
  _request(body, { expect = 'sync', timeoutMs = this.requestTimeoutMs, handleId = null } = {}) {
    if (this.closed || !this._ws || this._ws.readyState !== WS_OPEN) {
      return Promise.reject(new JanusError('websocket is closed', 'ws_closed'));
    }
    const transaction = newTransaction();
    const frame = { ...body, transaction };
    if (this.sessionId !== null && body.janus !== 'create') frame.session_id = this.sessionId;
    if (handleId !== null) frame.handle_id = handleId;
    if (this._token) frame.token = this._token; // the latest token, on EVERY request
    return new Promise((resolve, reject) => {
      const p = { transaction, expect, acked: false, handleId, resolve, reject, timer: null };
      p.timer = setTimeout(() => {
        this._pending.delete(transaction);
        reject(new JanusError(`${body.janus} request timed out`, 'timeout'));
      }, timeoutMs);
      this._pending.set(transaction, p);
      try {
        this._ws.send(JSON.stringify(frame));
      } catch (e) {
        this._finish(p, null, new JanusError('websocket send failed', 'ws_error'));
      }
    });
  }

  _startKeepalive() {
    this._stopKeepalive();
    this._keepaliveTimer = setInterval(async () => {
      try {
        await this._request({ janus: 'keepalive' }, { expect: 'ack' });
        this._keepaliveFailures = 0;
      } catch (e) {
        this._keepaliveFailures += 1;
        this._log(`keepalive failed (${e.code})`);
        if (this._onKeepaliveError) {
          try { this._onKeepaliveError(e, this._keepaliveFailures); } catch (e2) { /* ignore */ }
        }
      }
    }, this.keepaliveMs);
  }

  _stopKeepalive() {
    if (this._keepaliveTimer) clearInterval(this._keepaliveTimer);
    this._keepaliveTimer = null;
  }

  /** Common exit path for every way the session can end. */
  _teardown(info) {
    if (this.closed && !this._ws) return;
    const wasClosed = this.closed;
    this.closed = true;
    this._stopKeepalive();
    const ws = this._ws;
    this._ws = null;
    if (ws) { try { ws.close(); } catch (e) { /* ignore */ } }
    for (const p of [...this._pending.values()]) {
      this._finish(p, null, new JanusError('websocket closed', 'ws_closed'));
    }
    for (const h of this._handles.values()) h._markDetached();
    this._handles.clear();
    if (!wasClosed && !this._closing && this._onClose) {
      try { this._onClose(info); } catch (e) { /* ignore */ }
    }
  }
}

export class JanusHandle {
  constructor(session, id, plugin) {
    this.session = session;
    this.id = id;
    this.plugin = plugin;
    this.detached = false;
    this._listeners = new Map(); // event name -> Set<callback>
  }

  /** Subscribe to unsolicited events: 'event', 'trickle', 'webrtcup', 'media', 'slowlink', 'hangup', 'detached'. Returns an unsubscribe function. */
  on(evt, cb) {
    let set = this._listeners.get(evt);
    if (!set) { set = new Set(); this._listeners.set(evt, set); }
    set.add(cb);
    return () => set.delete(cb);
  }

  /**
   * Send a plugin request. Resolves with {data, jsep} of the answer carrying the same
   * transaction (after the ack). Rejects with JanusError.
   */
  message(body, { jsep = null, timeoutMs } = {}) {
    if (this.detached) return Promise.reject(new JanusError('handle is detached', 'detached'));
    const frame = { janus: 'message', body };
    if (jsep) frame.jsep = jsep;
    return this.session._request(frame, { expect: 'event', handleId: this.id, timeoutMs: timeoutMs || this.session.requestTimeoutMs })
      .then((r) => ({ data: r.data, jsep: r.jsep }));
  }

  /** Send a local ICE candidate ({candidate, sdpMid, sdpMLineIndex}) or, with null, end-of-candidates. */
  trickle(candidate) {
    if (this.detached) return Promise.resolve();
    const c = candidate
      ? { candidate: candidate.candidate, sdpMid: candidate.sdpMid, sdpMLineIndex: candidate.sdpMLineIndex }
      : { completed: true };
    return this.session._request({ janus: 'trickle', candidate: c }, { expect: 'ack', handleId: this.id }).then(() => undefined);
  }

  /** Detach the handle (best effort, never rejects). */
  async detach() {
    if (this.detached) return;
    try {
      await this.session._request({ janus: 'detach' }, { expect: 'sync', handleId: this.id, timeoutMs: Math.min(this.session.requestTimeoutMs, 2000) });
    } catch (e) { /* the session teardown detaches anyway */ }
    this._markDetached(true);
    this.session._handles.delete(this.id);
  }

  _emit(evt, payload) {
    const set = this._listeners.get(evt);
    if (!set) return;
    for (const cb of [...set]) {
      try { cb(payload); } catch (e) { this.session._log(`listener for ${evt} threw`); }
    }
  }

  _dispatch(msg) {
    switch (msg.janus) {
      case 'event': {
        const data = msg.plugindata ? msg.plugindata.data : null;
        this._emit('event', { data, jsep: msg.jsep || null });
        break;
      }
      case 'trickle': this._emit('trickle', msg.candidate || { completed: true }); break;
      case 'webrtcup': this._emit('webrtcup', {}); break;
      case 'media': this._emit('media', { type: msg.type, receiving: msg.receiving, mid: msg.mid }); break;
      case 'slowlink': this._emit('slowlink', { uplink: msg.uplink, nacks: msg.nacks, mid: msg.mid }); break;
      case 'hangup': this._emit('hangup', { reason: msg.reason || '' }); break;
      case 'detached':
        this._markDetached(true);
        this.session._handles.delete(this.id);
        break;
      default: break;
    }
  }

  _markDetached(notify) {
    if (this.detached) return;
    this.detached = true;
    // Requests still waiting on this handle can never be answered.
    for (const p of [...this.session._pending.values()]) {
      if (p.handleId === this.id) this.session._finish(p, null, new JanusError('handle detached', 'detached'));
    }
    if (notify) this._emit('detached', {});
  }
}
