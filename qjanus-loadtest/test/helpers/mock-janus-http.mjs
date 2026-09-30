// In-process mock of the Janus v1.4.2 HTTP transport (POST <base>, <base>/<sid>,
// <base>/<sid>/<hid>) with the videoroom plugin in string_ids mode. It
// reproduces the REAL semantics the admin helper depends on:
//   - signed-token verification on every session-level request (403 otherwise),
//     in Janus' order: token -> session (458) -> handle (459) -> command
//   - "attach" additionally requires the plugin descriptor in the token (405)
//   - videoroom create (admin_key, exists=427, parameter validation),
//     destroy/allowed/kick (room secret, 426/428/429/433), list (private rooms
//     only with a valid admin_key), listparticipants, exists
//   - plugin errors travel as janus:"success" + plugindata.data.error_code
//   - async plugin requests (join, publish, ...) answer janus:"ack"
// Error codes and texts are the ones from apierror.h / janus_videoroom.c.

import http from 'node:http';
import { verifySessionToken, VIDEOROOM_PLUGIN } from '../../src/token.mjs';

const ASYNC_REQUESTS = new Set([
  'join', 'joinandconfigure', 'update', 'configure', 'publish', 'unpublish',
  'start', 'pause', 'switch', 'subscribe', 'unsubscribe', 'leave',
]);
const BOOL_PARAMS = [
  'is_private', 'require_pvtid', 'signed_tokens', 'bitrate_cap', 'opus_fec', 'opus_dtx', 'audiolevel_ext',
  'audiolevel_event', 'videoorient_ext', 'playoutdelay_ext', 'transport_wide_cc_ext', 'record', 'lock_record',
  'permanent', 'notify_joining', 'require_e2ee',
];
const POSITIVE_INT_PARAMS = ['bitrate', 'fir_freq', 'publishers', 'threads'];
const STRING_PARAMS = ['description', 'secret', 'pin', 'audiocodec', 'videocodec', 'rec_dir'];

/**
 * @param {object} o
 * @param {string} o.tokenSecret  core token secret
 * @param {string} [o.adminKey]   videoroom admin_key (unset = anyone may create)
 * @param {'sha1'|'sha256'} [o.hash]
 * @param {()=>number} [o.now]    clock used to judge token expiry
 * @param {number} [o.latencyMs]  artificial delay per request (to observe concurrency)
 */
export async function startMockJanus({
  tokenSecret,
  adminKey,
  hash = 'sha256',
  now = Date.now,
  latencyMs = 0,
  basePath = '/janus',
} = {}) {
  const sessions = new Map(); // sid -> { handles: Map<hid, {plugin}> }
  const rooms = new Map(); // room id -> room
  const requests = []; // one entry per HTTP request, in arrival order
  const state = { now, latencyMs, inFlight: 0, peakInFlight: 0 };
  let idCounter = 1000;
  // Janus session/handle ids are random <= 2^53; the mock hands out large distinct numbers.
  const newId = () => 1e12 + (++idCounter) * 7919 + Math.floor(Math.random() * 1000) * 1e6;

  const tokenOk = (root, plugin) =>
    verifySessionToken(root.token, { secret: tokenSecret, hash, nowMs: state.now(), plugin });

  // ------------------------------------------------------------------ videoroom

  const err = (code, message) => ({ error_code: code, error: message });

  function checkSecret(actual, given, member) {
    if (actual === undefined) return null;
    if (given === undefined) return err(429, `Missing mandatory element (${member})`);
    if (typeof given !== 'string') return err(430, `Invalid element type (${member} should be a string)`);
    return given === actual ? null : err(433, `Unauthorized (wrong ${member})`);
  }

  function validateCreate(body) {
    for (const p of BOOL_PARAMS) {
      if (body[p] !== undefined && typeof body[p] !== 'boolean') return err(430, `Invalid element type (${p} should be a boolean)`);
    }
    for (const p of POSITIVE_INT_PARAMS) {
      if (body[p] !== undefined && !(Number.isInteger(body[p]) && body[p] > 0)) {
        return err(430, `Invalid element type (${p} should be a positive integer)`);
      }
    }
    for (const p of STRING_PARAMS) {
      if (body[p] !== undefined && typeof body[p] !== 'string') return err(430, `Invalid element type (${p} should be a string)`);
    }
    if (body.room !== undefined && typeof body.room !== 'string') return err(430, 'Invalid element type (room should be a string)');
    if (body.allowed !== undefined) {
      if (!Array.isArray(body.allowed)) return err(430, 'Invalid element type (allowed should be an array)');
      if (body.allowed.some((t) => typeof t !== 'string')) return err(430, 'Invalid element in the allowed array (not a string)');
    }
    return null;
  }

  /** Room lookup + optional secret check, as janus_videoroom_access_room(root, check_modify, ...). */
  function accessRoom(body, checkModify) {
    if (body.room === undefined) return { e: err(429, 'Missing mandatory element (room)') };
    if (typeof body.room !== 'string') return { e: err(430, 'Invalid element type (room should be a string)') };
    const room = rooms.get(body.room);
    if (!room) return { e: err(426, `No such room (${body.room})`) };
    if (checkModify) {
      const e = checkSecret(room.secret, body.secret, 'secret');
      if (e) return { e };
    }
    return { room };
  }

  function videoroom(body) {
    switch (body.request) {
      case 'create': {
        let e = validateCreate(body);
        if (e) return e;
        if (adminKey !== undefined) {
          if (body.admin_key === undefined) return err(429, 'Missing mandatory element (admin_key)');
          if (typeof body.admin_key !== 'string') return err(430, 'Invalid element type (admin_key should be a string)');
          e = checkSecret(adminKey, body.admin_key, 'admin_key');
          if (e) return e;
        }
        const id = body.room ?? crypto.randomUUID();
        if (rooms.has(id)) return err(427, `Room ${id} already exists`);
        const { secret, allowed, admin_key: _ignored, request: _req, room: _room, ...params } = body;
        rooms.set(id, {
          id,
          secret,
          params, // every stored create parameter (require_e2ee, publishers, bitrate, ...)
          allowed: new Set(allowed ?? []),
          checkAllowed: allowed !== undefined,
          participants: new Map(),
        });
        return { videoroom: 'created', room: id, permanent: false };
      }
      case 'destroy': {
        const { room, e } = accessRoom(body, true);
        if (e) return e;
        rooms.delete(room.id);
        return { videoroom: 'destroyed', room: room.id, permanent: false };
      }
      case 'exists': {
        if (typeof body.room !== 'string') return err(429, 'Missing mandatory element (room)');
        return { videoroom: 'success', room: body.room, exists: rooms.has(body.room) };
      }
      case 'allowed': {
        if (typeof body.action !== 'string') return err(429, 'Missing mandatory element (action)');
        if (body.allowed !== undefined && !Array.isArray(body.allowed)) return err(430, 'Invalid element type (allowed should be an array)');
        if (!['enable', 'disable', 'add', 'remove'].includes(body.action)) {
          return err(430, `Unsupported action '${body.action}' (allowed)`);
        }
        const { room, e } = accessRoom(body, true);
        if (e) return e;
        if (body.action === 'enable') room.checkAllowed = true;
        else if (body.action === 'disable') room.checkAllowed = false;
        else {
          if (body.allowed?.some((t) => typeof t !== 'string')) return err(430, 'Invalid element in the allowed array (not a string)');
          for (const t of body.allowed ?? []) {
            if (body.action === 'add') room.allowed.add(t);
            else room.allowed.delete(t);
          }
        }
        return {
          videoroom: 'success',
          room: room.id,
          ...(body.action === 'disable' ? {} : { allowed: [...room.allowed] }),
        };
      }
      case 'kick': {
        if (typeof body.id !== 'string') return err(429, 'Missing mandatory element (id)');
        const { room, e } = accessRoom(body, true);
        if (e) return e;
        if (!room.participants.delete(body.id)) return err(428, `No such user ${body.id} in room ${room.id}`);
        return { videoroom: 'success' };
      }
      case 'list': {
        let lockPrivate = true;
        if (adminKey !== undefined && typeof body.admin_key === 'string' && body.admin_key.length > 0) {
          const e = checkSecret(adminKey, body.admin_key, 'admin_key');
          if (e) return e;
          lockPrivate = false;
        }
        const list = [];
        for (const r of rooms.values()) {
          if (r.params.is_private && lockPrivate) continue;
          list.push({
            room: r.id,
            description: r.params.description ?? r.id,
            pin_required: r.params.pin !== undefined,
            is_private: r.params.is_private ?? false,
            max_publishers: r.params.publishers ?? 3,
            bitrate: r.params.bitrate ?? 0,
            fir_freq: r.params.fir_freq ?? 0,
            require_pvtid: r.params.require_pvtid ?? false,
            require_e2ee: r.params.require_e2ee ?? false,
            notify_joining: r.params.notify_joining ?? false,
            audiocodec: r.params.audiocodec ?? 'opus',
            videocodec: r.params.videocodec ?? 'vp8',
            record: r.params.record ?? false,
            lock_record: r.params.lock_record ?? false,
            num_participants: r.participants.size,
          });
        }
        return { videoroom: 'success', list };
      }
      case 'listparticipants': {
        const { room, e } = accessRoom(body, false);
        if (e) return e;
        return {
          videoroom: 'participants',
          room: room.id,
          participants: [...room.participants.values()].map((p) => ({ id: p.id, display: p.display, publisher: true })),
        };
      }
      default:
        return err(423, `Unknown request '${body.request}'`);
    }
  }

  // ------------------------------------------------------------------ core API

  const fail = (code, reason, tx, sid) => ({
    janus: 'error',
    ...(sid === undefined ? {} : { session_id: sid }),
    transaction: tx,
    error: { code, reason },
  });
  const UNAUTHORIZED = 'Unauthorized request (wrong or missing secret/token)';

  /** @returns {object} the JSON reply for one parsed request */
  function handle(pathIds, root) {
    const tx = root.transaction;
    if (typeof tx !== 'string' || typeof root.janus !== 'string') {
      return fail(456, 'Missing mandatory element (transaction|janus)', tx);
    }
    const [sid, hid] = pathIds;
    if (sid === undefined) {
      if (root.janus === 'info') {
        return {
          janus: 'server_info',
          transaction: tx,
          name: 'Janus WebRTC Server',
          version_string: '1.4.2',
          'local-ip': '127.0.0.1',
          auth_token: true,
          plugins: { [VIDEOROOM_PLUGIN]: { name: 'JANUS VideoRoom plugin', version_string: '0.0.0' } },
          transports: { 'janus.transport.http': { name: 'JANUS REST (HTTP/HTTPS) transport' } },
        };
      }
      if (root.janus === 'ping') return { janus: 'pong', transaction: tx };
      if (root.janus !== 'create') return fail(457, `Unhandled request '${root.janus}' at this path`, tx);
      if (!tokenOk(root)) return fail(403, UNAUTHORIZED, tx);
      const id = newId();
      sessions.set(id, { handles: new Map() });
      return { janus: 'success', transaction: tx, data: { id } };
    }
    if (!tokenOk(root)) return fail(403, UNAUTHORIZED, tx, sid);
    const session = sessions.get(sid);
    if (!session) return fail(458, `No such session ${sid}`, tx, sid);
    let handleObj;
    if (hid !== undefined) {
      handleObj = session.handles.get(hid);
      if (!handleObj) return fail(459, `No such handle ${hid} in session ${sid}`, tx, sid);
    }
    switch (root.janus) {
      case 'keepalive':
        return { janus: 'ack', session_id: sid, transaction: tx };
      case 'attach': {
        if (hid !== undefined) return fail(457, "Unhandled request 'attach' at this path", tx, sid);
        if (root.plugin !== VIDEOROOM_PLUGIN) return fail(460, `No such plugin '${root.plugin}'`, tx, sid);
        if (!tokenOk(root, root.plugin)) {
          return fail(405, `Provided token can't access plugin '${root.plugin}'`, tx, sid);
        }
        const id = newId();
        session.handles.set(id, { plugin: root.plugin });
        return { janus: 'success', session_id: sid, transaction: tx, data: { id } };
      }
      case 'destroy':
        if (hid !== undefined) return fail(457, "Unhandled request 'destroy' at this path", tx, sid);
        sessions.delete(sid);
        return { janus: 'success', session_id: sid, transaction: tx };
      case 'detach':
        if (hid === undefined) return fail(457, "Unhandled request 'detach' at this path", tx, sid);
        session.handles.delete(hid);
        return { janus: 'success', session_id: sid, transaction: tx };
      case 'message': {
        if (hid === undefined) return fail(457, "Unhandled request 'message' at this path", tx, sid);
        const body = root.body;
        if (body === null || typeof body !== 'object') return fail(456, 'Missing mandatory element (body)', tx, sid);
        if (typeof body.request !== 'string') {
          return plugindata(sid, hid, tx, err(429, 'Missing mandatory element (request)'));
        }
        if (ASYNC_REQUESTS.has(body.request)) return { janus: 'ack', session_id: sid, transaction: tx };
        return plugindata(sid, hid, tx, videoroom(body));
      }
      default:
        return fail(453, `Unknown request '${root.janus}'`, tx, sid);
    }
  }

  function plugindata(sid, hid, tx, data) {
    const payload = data.error_code === undefined ? data : { videoroom: 'event', ...data };
    return { janus: 'success', session_id: sid, transaction: tx, sender: hid, plugindata: { plugin: VIDEOROOM_PLUGIN, data: payload } };
  }

  // -------------------------------------------------------------------- server

  const server = http.createServer((req, res) => {
    const chunks = [];
    req.on('data', (c) => chunks.push(c));
    req.on('end', async () => {
      state.inFlight++;
      state.peakInFlight = Math.max(state.peakInFlight, state.inFlight);
      try {
        if (state.latencyMs > 0) await new Promise((r) => setTimeout(r, state.latencyMs));
        const reply = respond(req, Buffer.concat(chunks).toString('utf8'));
        res.writeHead(reply.status, { 'content-type': 'application/json' });
        res.end(reply.body === undefined ? '' : JSON.stringify(reply.body));
      } finally {
        state.inFlight--;
      }
    });
  });

  function respond(req, text) {
    const path = new URL(req.url, 'http://mock').pathname;
    if (req.method !== 'POST' || !(path === basePath || path.startsWith(`${basePath}/`))) return { status: 404 };
    const ids = path.slice(basePath.length).split('/').filter(Boolean).map(Number);
    if (ids.length > 2 || ids.some((n) => !Number.isSafeInteger(n) || n < 1)) return { status: 404 };
    let root;
    try {
      root = JSON.parse(text);
    } catch {
      return { status: 200, body: fail(454, 'JSON error: not valid JSON') };
    }
    requests.push({
      path, ids, janus: root?.janus, body: root?.body, token: root?.token, transaction: root?.transaction,
    });
    if (root === null || typeof root !== 'object') return { status: 200, body: fail(455, 'JSON error: not an object') };
    return { status: 200, body: handle(ids, root) };
  }

  await new Promise((resolve) => server.listen(0, '127.0.0.1', resolve));
  const { port } = server.address();

  return {
    /** Base URL to give to JanusAdmin / QJANUS_ADMIN_URL. */
    url: `http://127.0.0.1:${port}${basePath}`,
    sessions,
    rooms,
    requests,
    state,
    /** Janus forgets every session (idle timeout / restart): the next request answers 458. */
    dropSessions: () => sessions.clear(),
    /** Janus forgets every handle but keeps the sessions: the next plugin request answers 459. */
    dropHandles: () => sessions.forEach((s) => s.handles.clear()),
    /** Put a participant into a room (join happens over WebSocket in production). */
    addParticipant(room, { id, display = id }) {
      rooms.get(room).participants.set(id, { id, display });
    },
    close: () => new Promise((resolve) => {
      server.closeAllConnections();
      server.close(resolve);
    }),
  };
}
