// Mock Janus WebSocket server for tests (built on the `ws` package).
//
// It implements the Janus core behaviour the client depends on: create / attach / detach /
// destroy / keepalive / trickle / message, the ack-then-event answer flow for plugin requests,
// error frames, token checking on every session-level request and unsolicited events.
// The plugin logic is injected: `plugin(ctx)` decides how each plugin message is answered.
//
// plugin(ctx) receives {handleId, body, jsep, transaction, session, server} and returns one of
//   {event: <plugindata.data>, jsep?}   ack now, event with the same transaction next
//   {sync: <plugindata.data>}           a synchronous `success` carrying plugindata
//   {error: {code, reason}}             a janus error frame
//   {silent: true}                      never answer (timeout tests)
//   undefined                           same as {event: {}}
// It may be async, and may call server.push(...) to send unsolicited events.
// `onTrickle(handleId, candidate, request)` is called for every trickle request (after the ack).
import { WebSocketServer } from 'ws';

const PLUGIN = 'janus.plugin.videoroom';

export async function startMockJanus({ plugin = null, tokenValidator = null, ackDelayMs = 0, onTrickle = null } = {}) {
  const wss = new WebSocketServer({ host: '127.0.0.1', port: 0, handleProtocols: (protocols) => (protocols.has('janus-protocol') ? 'janus-protocol' : false) });
  await new Promise((r) => wss.once('listening', r));
  const port = wss.address().port;
  let nextId = 1000;
  const sessions = new Map(); // id -> {id, ws, handles:Set}
  const handles = new Map(); // id -> {id, sessionId, plugin}
  const requests = []; // every root JSON message received, in order
  const protocols = [];
  const server = {
    url: `ws://127.0.0.1:${port}`,
    port,
    requests,
    protocols,
    sessions,
    handles,
    plugin,
    tokenValidator,
    onTrickle,
    /** Requests of one kind, e.g. server.of('keepalive'). */
    of: (janus) => requests.filter((r) => r.janus === janus),
    /** Send an unsolicited plugin event on a handle (optionally with a jsep / a transaction). */
    push(handleId, data, { jsep = null, transaction = null } = {}) {
      const h = handles.get(handleId);
      const s = h && sessions.get(h.sessionId);
      if (!s) return false;
      const frame = { janus: 'event', session_id: s.id, sender: handleId, plugindata: { plugin: h.plugin, data } };
      if (transaction) frame.transaction = transaction;
      if (jsep) frame.jsep = jsep;
      s.ws.send(JSON.stringify(frame));
      return true;
    },
    /** Send an arbitrary frame to a handle's session (webrtcup, media, slowlink, hangup, trickle...). */
    pushRaw(handleId, frame) {
      const h = handles.get(handleId);
      const s = h && sessions.get(h.sessionId);
      if (!s) return false;
      s.ws.send(JSON.stringify({ session_id: s.id, sender: handleId, ...frame }));
      return true;
    },
    /** Close every client socket from the server side. */
    dropAll(code = 1011) {
      for (const c of wss.clients) c.close(code, 'test drop');
    },
    async close() {
      for (const c of wss.clients) c.terminate();
      await new Promise((r) => wss.close(r));
    },
  };

  const reply = (ws, frame) => ws.send(JSON.stringify(frame));
  const err = (ws, req, code, reason) => reply(ws, { janus: 'error', session_id: req.session_id, transaction: req.transaction, error: { code, reason } });

  wss.on('connection', (ws) => {
    protocols.push(ws.protocol);
    ws.on('message', async (raw) => {
      let req;
      try { req = JSON.parse(raw.toString()); } catch (e) { return; }
      requests.push(req);
      const tx = req.transaction;
      // Like Janus: `info` and `ping` need no token; everything else does when a validator is set.
      if (server.tokenValidator && req.janus !== 'info' && req.janus !== 'ping' && !server.tokenValidator(req.token, req)) {
        err(ws, req, 403, 'Unauthorized request (wrong or missing secret/token)');
        return;
      }
      switch (req.janus) {
        case 'create': {
          const id = nextId++;
          sessions.set(id, { id, ws, handles: new Set() });
          reply(ws, { janus: 'success', transaction: tx, data: { id } });
          break;
        }
        case 'attach': {
          const s = sessions.get(req.session_id);
          if (!s) { err(ws, req, 458, 'No such session'); break; }
          const id = nextId++;
          handles.set(id, { id, sessionId: s.id, plugin: req.plugin || PLUGIN });
          s.handles.add(id);
          reply(ws, { janus: 'success', session_id: s.id, transaction: tx, data: { id } });
          break;
        }
        case 'keepalive':
          reply(ws, { janus: 'ack', session_id: req.session_id, transaction: tx });
          break;
        case 'trickle':
          reply(ws, { janus: 'ack', session_id: req.session_id, transaction: tx });
          if (server.onTrickle) server.onTrickle(req.handle_id, req.candidate, req);
          break;
        case 'detach':
          handles.delete(req.handle_id);
          reply(ws, { janus: 'success', session_id: req.session_id, transaction: tx });
          break;
        case 'destroy':
          sessions.delete(req.session_id);
          reply(ws, { janus: 'success', session_id: req.session_id, transaction: tx });
          break;
        case 'message': {
          const h = handles.get(req.handle_id);
          if (!h) { err(ws, req, 459, 'No such handle'); break; }
          const ctx = { handleId: h.id, body: req.body, jsep: req.jsep || null, transaction: tx, session: sessions.get(h.sessionId), server };
          if (ackDelayMs) await new Promise((r) => setTimeout(r, ackDelayMs));
          reply(ws, { janus: 'ack', session_id: h.sessionId, transaction: tx });
          let res;
          try { res = server.plugin ? await server.plugin(ctx) : undefined; } catch (e) { res = { error: { code: 500, reason: String(e && e.message) } }; }
          if (ws.readyState !== 1 || (res && res.silent)) break;
          res = res || { event: {} };
          if (res.error) { err(ws, req, res.error.code, res.error.reason); break; }
          const base = { session_id: h.sessionId, transaction: tx, sender: h.id, plugindata: { plugin: h.plugin, data: res.sync || res.event || {} } };
          if (res.sync) reply(ws, { janus: 'success', ...base });
          else reply(ws, { janus: 'event', ...base, ...(res.jsep ? { jsep: res.jsep } : {}) });
          break;
        }
        default:
          err(ws, req, 453, 'Unknown request');
      }
    });
  });
  return server;
}
