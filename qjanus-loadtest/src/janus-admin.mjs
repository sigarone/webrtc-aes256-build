// Room/admin helper for the Janus HTTP transport. It talks to Janus the way the
// production bcrypto-lite server will (spec section 3): one core session with a
// signed token (a fresh token on EVERY request), one videoroom plugin handle,
// and only the synchronous VideoRoom requests (create, destroy, allowed, kick,
// list, listparticipants, exists) that answer directly with "success".
//
// Secrets (token secret, admin_key, room secrets, join tokens) never appear in
// errors or log lines: everything that leaves this module goes through mask().

import { randomBytes } from 'node:crypto';
import { mintSessionToken, VIDEOROOM_PLUGIN } from './token.mjs';
import { roomPlan, roomId, roomSecret } from './ids.mjs';

export { VIDEOROOM_PLUGIN };

/** Janus core API error codes (src/apierror.h) the helper reacts to. */
export const JANUS_ERROR = Object.freeze({
  UNAUTHORIZED: 403, // missing/expired/invalid session token
  UNAUTHORIZED_PLUGIN: 405, // token may not attach to this plugin
  SESSION_NOT_FOUND: 458,
  HANDLE_NOT_FOUND: 459,
});

/** VideoRoom plugin error codes (src/plugins/janus_videoroom.c, JANUS_VIDEOROOM_ERROR_*). */
export const VIDEOROOM_ERROR = Object.freeze({
  UNKNOWN_ERROR: 499,
  NO_MESSAGE: 421,
  INVALID_JSON: 422,
  INVALID_REQUEST: 423,
  JOIN_FIRST: 424,
  ALREADY_JOINED: 425,
  NO_SUCH_ROOM: 426,
  ROOM_EXISTS: 427,
  NO_SUCH_FEED: 428,
  MISSING_ELEMENT: 429,
  INVALID_ELEMENT: 430,
  INVALID_SDP_TYPE: 431,
  PUBLISHERS_FULL: 432,
  UNAUTHORIZED: 433, // wrong room secret or admin_key
  ALREADY_PUBLISHED: 434,
  NOT_PUBLISHED: 435,
  ID_EXISTS: 436,
  INVALID_SDP: 437,
  INVALID_FEED: 438,
});

/** Room parameters of spec section 3 (everything except room, secret, publishers, allowed, admin_key). */
export const SPEC_ROOM_DEFAULTS = Object.freeze({
  is_private: true,
  bitrate: 1500000,
  fir_freq: 10,
  audiocodec: 'opus',
  videocodec: 'vp8',
  opus_fec: true,
  opus_dtx: false,
  audiolevel_ext: false,
  audiolevel_event: false,
  videoorient_ext: false,
  playoutdelay_ext: false,
  transport_wide_cc_ext: true,
  record: false,
  lock_record: true,
  require_pvtid: true,
  require_e2ee: true,
  notify_joining: false,
});

const ALLOWED_ACTIONS = ['add', 'remove', 'enable', 'disable'];

/**
 * Failure of an admin call. `code` is the Janus/plugin error code (number) or a
 * string for local failures ('timeout', 'network', 'http_<status>',
 * 'bad_response', 'ack', 'config', 'partial_failure'); `reason` is the masked
 * Janus text; `request` names the call, e.g. "videoroom.create".
 */
export class JanusAdminError extends Error {
  constructor(message, { code, reason = '', request = '' } = {}) {
    super(message);
    this.name = 'JanusAdminError';
    this.code = code;
    this.reason = reason;
    this.request = request;
  }
}

const TOKEN_RE = /\d+,janus,[^\s:"']*:[A-Za-z0-9+/=]+/g; // a signed session token
const HEX_RUN_RE = /[0-9a-fA-F]{32,}/g; // room ids, room secrets, join tokens, seeds

/** Remove secrets from free text: known secret values, signed tokens, long hex ids (kept to 8 chars). */
function mask(text, secrets = []) {
  let out = String(text);
  for (const s of secrets) {
    if (typeof s === 'string' && s.length >= 4) out = out.split(s).join('***');
  }
  return out.replace(TOKEN_RE, '<token>').replace(HEX_RUN_RE, (m) => `${m.slice(0, 8)}...`);
}

function requireNonEmpty(name, value) {
  if (typeof value !== 'string' || value.length === 0) throw new TypeError(`${name} must be a non-empty string`);
}

function requirePositiveInt(name, value) {
  if (!Number.isSafeInteger(value) || value < 1) throw new RangeError(`${name} must be an integer >= 1`);
}

function requireTokens(name, tokens) {
  if (!Array.isArray(tokens) || tokens.some((t) => typeof t !== 'string' || t === '')) {
    throw new TypeError(`${name} must be an array of non-empty strings`);
  }
  return tokens;
}

/** Values of a plugin request body that must never be logged or echoed. */
function secretsOf(body) {
  return [body.secret, body.admin_key, ...(Array.isArray(body.allowed) ? body.allowed : [])];
}

export class JanusAdmin {
  /**
   * @param {object} o
   * @param {string} o.baseUrl  Janus HTTP base, e.g. "http://127.0.0.1:8088/janus"
   * @param {string} [o.tokenSecret]  core token_auth_secret (needed for everything except info/ping)
   * @param {string} [o.adminKey]  videoroom admin_key (sent with create and list)
   * @param {'sha1'|'sha256'} [o.hash='sha256']  token_auth_hash
   * @param {number} [o.tokenTtlSec=600]  lifetime of each freshly minted token
   * @param {typeof fetch} [o.fetchImpl]  fetch replacement (tests)
   * @param {number} [o.requestTimeoutMs=8000]
   * @param {(line:string)=>void} [o.log]  receives masked progress lines
   * @param {()=>number} [o.now]  clock in ms (tests: expired-token scenarios)
   */
  constructor({
    baseUrl,
    tokenSecret,
    adminKey,
    hash = 'sha256',
    tokenTtlSec = 600,
    fetchImpl,
    requestTimeoutMs = 8000,
    log,
    now = Date.now,
  } = {}) {
    requireNonEmpty('baseUrl', baseUrl);
    const url = new URL(baseUrl); // throws on garbage
    if (url.protocol !== 'http:' && url.protocol !== 'https:') throw new TypeError('baseUrl must be http(s)');
    this.baseUrl = baseUrl.replace(/\/+$/, '');
    this.tokenSecret = tokenSecret || undefined;
    this.adminKey = adminKey || undefined;
    this.hash = hash;
    this.tokenTtlSec = tokenTtlSec;
    this.requestTimeoutMs = requestTimeoutMs;
    this._fetch = fetchImpl ?? ((...args) => globalThis.fetch(...args));
    this._log = typeof log === 'function' ? (line) => log(mask(line, [this.tokenSecret, this.adminKey])) : () => {};
    this._now = now;
    this._sess = null; // { gen, p: Promise<{gen, sid, hid}> }
    this._gen = 0;
    if (this.tokenSecret) this._token(); // fail fast on a bad hash/ttl
  }

  // ---------------------------------------------------------------- transport

  _token() {
    if (!this.tokenSecret) {
      throw new JanusAdminError('a token secret is required for session-level requests', { code: 'config' });
    }
    return mintSessionToken({
      secret: this.tokenSecret,
      ttlSec: this.tokenTtlSec,
      hash: this.hash,
      nowMs: this._now(),
    });
  }

  _fail(label, code, reason, extraSecrets = []) {
    const safe = mask(reason, [this.tokenSecret, this.adminKey, ...extraSecrets]);
    return new JanusAdminError(`${label}: ${safe || 'failed'} (code ${code})`, { code, reason: safe, request: label });
  }

  /** POST one JSON request; resolves with the reply, throws JanusAdminError for janus:"error" and transport failures. */
  async _post(path, payload, label, extraSecrets = []) {
    const transaction = randomBytes(12).toString('hex'); // 96 bit
    let res;
    try {
      res = await this._fetch(this.baseUrl + path, {
        method: 'POST',
        headers: { 'content-type': 'application/json' },
        body: JSON.stringify({ ...payload, transaction }),
        signal: AbortSignal.timeout(this.requestTimeoutMs),
      });
    } catch (err) {
      if (err?.name === 'TimeoutError' || err?.name === 'AbortError') {
        throw this._fail(label, 'timeout', `no answer within ${this.requestTimeoutMs} ms`);
      }
      throw this._fail(label, 'network', `request failed (${err?.cause?.code ?? err?.code ?? 'unknown'})`);
    }
    if (!res.ok) throw this._fail(label, `http_${res.status}`, `HTTP status ${res.status}`);
    let reply;
    try {
      reply = JSON.parse(await res.text());
    } catch {
      throw this._fail(label, 'bad_response', 'answer is not valid JSON');
    }
    if (reply === null || typeof reply !== 'object') throw this._fail(label, 'bad_response', 'answer is not a JSON object');
    if (reply.janus === 'error') {
      throw this._fail(label, reply.error?.code ?? 'unknown', reply.error?.reason ?? '', extraSecrets);
    }
    if (reply.transaction !== transaction) throw this._fail(label, 'bad_response', 'transaction mismatch');
    return reply;
  }

  /** Session-level request: same as _post plus a freshly minted root "token". */
  _sessionPost(path, payload, label, extraSecrets = []) {
    return this._post(path, { ...payload, token: this._token() }, label, extraSecrets);
  }

  // ------------------------------------------------------------------ session

  _ensureOpen() {
    if (!this._sess) {
      const gen = ++this._gen;
      const p = this._openNew(gen);
      this._sess = { gen, p };
      p.catch(() => {
        if (this._sess?.gen === gen) this._sess = null; // let the next call retry
      });
    }
    return this._sess.p;
  }

  async _openNew(gen) {
    const created = await this._sessionPost('', { janus: 'create' }, 'janus.create');
    const sid = created.data?.id;
    if (!Number.isSafeInteger(sid)) throw this._fail('janus.create', 'bad_response', 'no session id in answer');
    const attached = await this._sessionPost(`/${sid}`, { janus: 'attach', plugin: VIDEOROOM_PLUGIN }, 'janus.attach');
    const hid = attached.data?.id;
    if (!Number.isSafeInteger(hid)) throw this._fail('janus.attach', 'bad_response', 'no handle id in answer');
    this._log(`session opened (generation ${gen})`);
    return { gen, sid, hid };
  }

  /** Creates the session and the videoroom handle (idempotent; every request opens them lazily anyway). */
  async open() {
    const { sid, hid } = await this._ensureOpen();
    return { sessionId: sid, handleId: hid };
  }

  /** Best-effort detach + destroy. Never throws. */
  async close() {
    const sess = this._sess;
    this._sess = null;
    if (!sess) return;
    let s;
    try {
      s = await sess.p;
    } catch {
      return;
    }
    await this._sessionPost(`/${s.sid}/${s.hid}`, { janus: 'detach' }, 'janus.detach').catch(() => {});
    await this._sessionPost(`/${s.sid}`, { janus: 'destroy' }, 'janus.destroy').catch(() => {});
    this._log('session closed');
  }

  /** Runs fn on the current session; if Janus dropped session/handle (458/459), re-opens once and retries. */
  async _withSession(fn) {
    for (let attempt = 0; ; attempt++) {
      const sess = await this._ensureOpen();
      try {
        return await fn(sess);
      } catch (err) {
        const gone = err instanceof JanusAdminError
          && (err.code === JANUS_ERROR.SESSION_NOT_FOUND || err.code === JANUS_ERROR.HANDLE_NOT_FOUND);
        if (!gone || attempt > 0) throw err;
        if (this._sess?.gen === sess.gen) this._sess = null; // parallel callers share one re-open
        this._log(`janus dropped the session (code ${err.code}), re-opening`);
      }
    }
  }

  // --------------------------------------------------------------- core calls

  /** Core "info" reply (no token needed). */
  info() {
    return this._post('', { janus: 'info' }, 'janus.info');
  }

  /** Core "ping" (no token needed); resolves with the round trip in ms. */
  async ping() {
    const t0 = performance.now();
    const reply = await this._post('', { janus: 'ping' }, 'janus.ping');
    if (reply.janus !== 'pong') throw this._fail('janus.ping', 'bad_response', `unexpected answer "${reply.janus}"`);
    return Math.round(performance.now() - t0);
  }

  // ---------------------------------------------------------- videoroom calls

  /**
   * Send one synchronous VideoRoom request; resolves with the plugin `data` object.
   * @throws {JanusAdminError} janus:"error", plugin error_code, or an "ack" (async request)
   */
  async request(body) {
    if (body === null || typeof body !== 'object' || typeof body.request !== 'string') {
      throw new TypeError('body must be an object with a string "request"');
    }
    const label = `videoroom.${body.request}`;
    const secrets = secretsOf(body);
    return this._withSession(async ({ sid, hid }) => {
      const reply = await this._sessionPost(`/${sid}/${hid}`, { janus: 'message', body }, label, secrets);
      if (reply.janus === 'ack') {
        throw this._fail(label, 'ack', 'answered with "ack": asynchronous requests are not supported here');
      }
      const data = reply.plugindata?.data;
      if (reply.janus !== 'success' || data === null || typeof data !== 'object') {
        throw this._fail(label, 'bad_response', 'no plugin data in answer');
      }
      if (data.error_code !== undefined) throw this._fail(label, data.error_code, data.error ?? '', secrets);
      return data;
    });
  }

  /**
   * Create a room with the spec section 3 parameters (SPEC_ROOM_DEFAULTS), the
   * given per-room values, `allowed` join tokens and the constructor's admin_key.
   * `overrides` replace any default. With ignoreExists an existing room is not an
   * error; the result then has existed:true.
   */
  async createRoom({ room, secret, publishers, bitrate, allowed, ignoreExists = false, ...overrides } = {}) {
    requireNonEmpty('room', room);
    requireNonEmpty('secret', secret);
    requirePositiveInt('publishers', publishers);
    const body = {
      ...SPEC_ROOM_DEFAULTS,
      ...(bitrate === undefined ? {} : { bitrate }),
      ...(allowed === undefined ? {} : { allowed: requireTokens('allowed', allowed) }),
      ...overrides,
      request: 'create',
      room,
      secret,
      publishers,
      ...(this.adminKey === undefined ? {} : { admin_key: this.adminKey }),
    };
    try {
      return { ...(await this.request(body)), existed: false };
    } catch (err) {
      if (ignoreExists && err instanceof JanusAdminError && err.code === VIDEOROOM_ERROR.ROOM_EXISTS) {
        return { videoroom: 'exists', room, existed: true };
      }
      throw err;
    }
  }

  /** Destroy a room. With ignoreMissing an unknown room is not an error; the result then has missing:true. */
  async destroyRoom({ room, secret, ignoreMissing = false } = {}) {
    requireNonEmpty('room', room);
    requireNonEmpty('secret', secret);
    try {
      return { ...(await this.request({ request: 'destroy', room, secret })), missing: false };
    } catch (err) {
      if (ignoreMissing && err instanceof JanusAdminError && err.code === VIDEOROOM_ERROR.NO_SUCH_ROOM) {
        return { videoroom: 'missing', room, missing: true };
      }
      throw err;
    }
  }

  /** Edit the join-token ACL: action add|remove (with tokens) or enable|disable. Data carries the full `allowed` list except for disable. */
  async allowed({ room, secret, action, tokens } = {}) {
    requireNonEmpty('room', room);
    requireNonEmpty('secret', secret);
    if (!ALLOWED_ACTIONS.includes(action)) throw new RangeError(`action must be one of ${ALLOWED_ACTIONS.join('|')}`);
    const needsTokens = action === 'add' || action === 'remove';
    if (needsTokens) requireTokens('tokens', tokens);
    return this.request({ request: 'allowed', room, secret, action, ...(needsTokens ? { allowed: tokens } : {}) });
  }

  /** Kick a participant (id = its pseudonym, string ids are on) out of the room. */
  async kick({ room, secret, id } = {}) {
    requireNonEmpty('room', room);
    requireNonEmpty('secret', secret);
    requireNonEmpty('id', id);
    return this.request({ request: 'kick', room, secret, id });
  }

  /** All rooms, private ones included (that needs the admin_key); data.list is the array. */
  async listRooms() {
    return this.request({ request: 'list', ...(this.adminKey === undefined ? {} : { admin_key: this.adminKey }) });
  }

  /** Participants of a room; data.participants is the array. */
  async listParticipants(room) {
    requireNonEmpty('room', room);
    return this.request({ request: 'listparticipants', room });
  }

  /** @returns {Promise<boolean>} */
  async roomExists(room) {
    requireNonEmpty('room', room);
    return (await this.request({ request: 'exists', room })).exists === true;
  }
}

// ------------------------------------------------------------------ bulk helpers

/** Runs fn over items with at most `limit` in flight; never rejects, returns [{ok, value|error}] in order. */
async function mapSettled(items, limit, fn) {
  requirePositiveInt('concurrency', limit);
  const out = new Array(items.length);
  let next = 0;
  const worker = async () => {
    while (next < items.length) {
      const idx = next++;
      try {
        out[idx] = { ok: true, value: await fn(items[idx], idx) };
      } catch (error) {
        out[idx] = { ok: false, error };
      }
    }
  };
  await Promise.all(Array.from({ length: Math.min(limit, items.length) }, worker));
  return out;
}

/** Turn mapSettled output into the results array, or throw one aggregate error after every item was tried. */
function collect(what, ks, settled) {
  const results = [];
  const failures = [];
  settled.forEach((s, n) => {
    if (s.ok) {
      results.push(s.value);
    } else {
      const e = s.error;
      failures.push({ k: ks[n], code: e?.code ?? 'error', reason: e?.reason || String(e?.message ?? e) });
    }
  });
  if (failures.length === 0) return results;
  const first = failures[0];
  const err = new JanusAdminError(
    `${what}: ${failures.length} of ${ks.length} rooms failed (first: k=${first.k} code=${first.code} ${first.reason})`,
    { code: 'partial_failure', request: what },
  );
  err.failures = failures;
  err.results = results;
  throw err;
}

function range(first, count) {
  if (!Number.isSafeInteger(first) || first < 0) throw new RangeError('first must be an integer >= 0');
  requirePositiveInt('count', count);
  return Array.from({ length: count }, (_, n) => first + n);
}

/**
 * Create rooms k = first .. first+count-1 with ids, secrets and join tokens derived from `seed`.
 * Idempotent: an existing room is kept, and (with `allowed`) its ACL is topped up with this run's tokens.
 * @returns {Promise<{k:number, room:string, existed:boolean}[]>}
 */
export async function createRooms(admin, { seed, first = 0, count, size, publishers = size, allowed = true, concurrency = 8 } = {}) {
  requirePositiveInt('size', size);
  const ks = range(first, count);
  const settled = await mapSettled(ks, concurrency, async (k) => {
    const plan = roomPlan(seed, k, size);
    const tokens = allowed ? plan.members.map((m) => m.joinToken) : undefined;
    const res = await admin.createRoom({
      room: plan.room, secret: plan.secret, publishers, allowed: tokens, ignoreExists: true,
    });
    if (res.existed && tokens) {
      await admin.allowed({ room: plan.room, secret: plan.secret, action: 'add', tokens });
      await admin.allowed({ room: plan.room, secret: plan.secret, action: 'enable' });
    }
    return { k, room: plan.room, existed: res.existed };
  });
  return collect('createRooms', ks, settled);
}

/**
 * Destroy rooms k = first .. first+count-1 (a missing room is fine).
 * @returns {Promise<{k:number, room:string, existed:boolean}[]>}
 */
export async function destroyRooms(admin, { seed, first = 0, count, concurrency = 8 } = {}) {
  const ks = range(first, count);
  const settled = await mapSettled(ks, concurrency, async (k) => {
    const room = roomId(seed, k);
    const res = await admin.destroyRoom({ room, secret: roomSecret(seed, room), ignoreMissing: true });
    return { k, room, existed: !res.missing };
  });
  return collect('destroyRooms', ks, settled);
}
