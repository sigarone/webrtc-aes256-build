// Module worker holding the E2EE-like frame transforms of every bot of a page.
//
// Loaded with `new Worker(url, {type: 'module'})`. Chromium delivers the encoded frames of
// each RTCRtpSender/RTCRtpReceiver here, either through RTCRtpScriptTransform
// (`onrtctransform`) or, on older builds, through createEncodedStreams() streams posted as
// {readable, writable, options}. `options` = {botId, kind: 'audio'|'video', role: 'sender'|'receiver', mode, keyHex}.
//
// Rules: frame order is kept (the async transform() blocks the next chunk of its TransformStream),
// nothing here ever throws into the stream, and a frame that cannot be transformed is COUNTED and
// DROPPED (never forwarded in the clear).
import { createCodec } from './e2ee-frame.mjs';

// botId -> {enc:{audio,video}, dec:{audio,video}, fail:{audio,video}, reasons:{<why>: count}}
const counters = new Map();
// "mode|keyHex" -> Promise<codec>: the HKDF/AES key is derived once per room key, not per frame or per bot
const codecs = new Map();

function countersFor(botId) {
  let c = counters.get(botId);
  if (!c) {
    c = { enc: { audio: 0, video: 0 }, dec: { audio: 0, video: 0 }, fail: { audio: 0, video: 0 }, reasons: {} };
    counters.set(botId, c);
  }
  return c;
}

function codecFor(mode, keyHex) {
  const k = `${mode}|${keyHex}`;
  let p = codecs.get(k);
  if (!p) {
    p = createCodec({ mode, keyHex });
    p.catch(() => codecs.delete(k)); // a bad key must not poison the cache forever
    codecs.set(k, p);
  }
  return p;
}

/** Exact-size ArrayBuffer for a Uint8Array (frame.data must not carry extra bytes). */
function toBuffer(u8) {
  return u8.byteOffset === 0 && u8.byteLength === u8.buffer.byteLength ? u8.buffer : u8.slice().buffer;
}

function setup({ readable, writable, options }) {
  const { botId, kind, role, mode, keyHex } = options;
  const c = countersFor(botId);
  const fail = (why) => { c.fail[kind]++; c.reasons[`${role}:${kind}:${why}`] = (c.reasons[`${role}:${kind}:${why}`] || 0) + 1; };
  const codecP = codecFor(mode, keyHex);
  readable
    .pipeThrough(new TransformStream({
      async transform(frame, controller) {
        try {
          const codec = await codecP;
          const data = new Uint8Array(frame.data);
          // RTCEncodedVideoFrame.type is 'key' | 'delta' (| 'empty'); audio frames have no type.
          const isKey = frame.type === 'key' ? true : (frame.type === 'delta' || frame.type === 'empty') ? false : undefined;
          if (role === 'sender') {
            frame.data = toBuffer(await codec.encrypt(kind, data, isKey));
            c.enc[kind]++;
            controller.enqueue(frame);
          } else {
            const r = await codec.decrypt(kind, data, isKey);
            if (r.ok) {
              frame.data = toBuffer(r.data);
              c.dec[kind]++;
              controller.enqueue(frame);
            } else {
              fail(r.reason);
            }
          }
        } catch (e) {
          fail('exception');
        }
      },
    }))
    .pipeTo(writable)
    .catch(() => { /* the sender/receiver was closed */ });
}

// Must be assigned synchronously at evaluation time, before any transform event can arrive.
self.onrtctransform = (event) => setup(event.transformer);

self.onmessage = (e) => {
  const m = e.data;
  if (!m) return;
  if (m.readable && m.writable) {
    setup(m); // createEncodedStreams() fallback
  } else if (m.type === 'counters') {
    const c = countersFor(m.botId);
    self.postMessage({ type: 'counters', id: m.id, counters: { enc: { ...c.enc }, dec: { ...c.dec }, fail: { ...c.fail }, reasons: { ...c.reasons } } });
  } else if (m.type === 'forget') {
    counters.delete(m.botId);
  }
};

// Tells the page that transforms can be attached now (see E2eeHost.ready in bot.mjs).
self.postMessage({ type: 'ready' });
