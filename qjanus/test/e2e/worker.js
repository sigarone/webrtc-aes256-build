// Reference implementation of the group-call frame crypto of GROUP_CALLS_V2 section 5.3, as an
// RTCRtpScriptTransform worker (WebCrypto only). It is what qjanus must forward untouched:
//   aes_key = HKDF-SHA256(ikm = K[sender,epoch], salt = empty, info = 128 x 0x00, L = 32) -> AES-256-GCM
//   frame   = header(unencrypted_bytes) || ciphertext || tag(16) || iv(12) || [12, keyIndex]     AAD = header
//   unencrypted_bytes: Opus 1, VP8 10 (key frame) / 3 (delta)   (qjanus needs the codec header to find
//   key frames and to switch simulcast layers)
// Senders have ONE key (index + key); receivers hold a ring of 16 keys per remote participant.
const HKDF_INFO = new Uint8Array(128);
const enc = { subtle: self.crypto.subtle };
const ring = new Map();              // participant -> Map(keyIndex -> CryptoKey)
const sendKey = { index: 0, key: null };
const counters = {
  encrypted: { audio: 0, video: 0 },
  decrypted: { audio: 0, video: 0 },
  authFail: { audio: 0, video: 0 },
  missingKey: { audio: 0, video: 0 },
  tooShort: { audio: 0, video: 0 },
  perParticipant: {},
};

const hexToBytes = (hex) => Uint8Array.from(hex.match(/../g).map((h) => parseInt(h, 16)));
async function deriveKey(keyHex) {
  const base = await enc.subtle.importKey('raw', hexToBytes(keyHex), 'HKDF', false, ['deriveKey']);
  return enc.subtle.deriveKey({ name: 'HKDF', hash: 'SHA-256', salt: new Uint8Array(0), info: HKDF_INFO },
    base, { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']);
}
const headerSize = (kind, frame) => (kind === 'audio' ? 1 : (frame.type === 'key' ? 10 : 3));
const bump = (group, kind, participant) => {
  counters[group][kind]++;
  if (participant) {
    const p = counters.perParticipant[participant] || (counters.perParticipant[participant] = { ok: 0, authFail: 0, missingKey: 0 });
    if (group === 'decrypted') p.ok++;
    if (group === 'authFail') p.authFail++;
    if (group === 'missingKey') p.missingKey++;
  }
};

async function encryptFrame(frame, kind) {
  if (!sendKey.key) return null;                                   // never send a frame without a key
  const data = new Uint8Array(frame.data);
  const n = Math.min(headerSize(kind, frame), data.length);
  const iv = self.crypto.getRandomValues(new Uint8Array(12));
  const header = data.subarray(0, n);
  const ct = new Uint8Array(await enc.subtle.encrypt({ name: 'AES-GCM', iv, additionalData: header, tagLength: 128 }, sendKey.key, data.subarray(n)));
  const out = new Uint8Array(n + ct.length + 12 + 2);
  out.set(header, 0);
  out.set(ct, n);
  out.set(iv, n + ct.length);
  out[out.length - 2] = 12;
  out[out.length - 1] = sendKey.index;
  frame.data = out.buffer;
  counters.encrypted[kind]++;
  return frame;
}

async function decryptFrame(frame, kind, participant) {
  const data = new Uint8Array(frame.data);
  if (data.length < 1 + 16 + 12 + 2) { bump('tooShort', kind, participant); return null; }
  const ivLen = data[data.length - 2];
  const keyIndex = data[data.length - 1];
  const key = ring.get(participant) && ring.get(participant).get(keyIndex);
  if (ivLen !== 12 || !key) { bump('missingKey', kind, participant); return null; }   // discardFrameWhenCryptorNotReady
  const n = headerSize(kind, frame);
  const iv = data.subarray(data.length - 2 - ivLen, data.length - 2);
  const header = data.subarray(0, n);
  try {
    const pt = new Uint8Array(await enc.subtle.decrypt({ name: 'AES-GCM', iv, additionalData: header, tagLength: 128 }, key, data.subarray(n, data.length - 2 - ivLen)));
    const out = new Uint8Array(n + pt.length);
    out.set(header, 0);
    out.set(pt, n);
    frame.data = out.buffer;
    bump('decrypted', kind, participant);
    return frame;
  } catch (e) {
    bump('authFail', kind, participant);
    return null;
  }
}

function setup(transformer) {
  const { readable, writable, options } = transformer;
  const { kind, role, participant } = options;
  readable.pipeThrough(new TransformStream({
    async transform(frame, controller) {
      const out = role === 'sender' ? await encryptFrame(frame, kind) : await decryptFrame(frame, kind, participant);
      if (out) controller.enqueue(out);
    },
  })).pipeTo(writable);
}

self.onrtctransform = (event) => setup(event.transformer);
self.onmessage = async (e) => {
  const m = e.data;
  if (m && m.readable) { setup(m); return; }                      // createEncodedStreams path
  if (m.type === 'sendKey') { sendKey.key = await deriveKey(m.keyHex); sendKey.index = m.index; }
  else if (m.type === 'recvKey') {
    if (!ring.has(m.participant)) ring.set(m.participant, new Map());
    ring.get(m.participant).set(m.index, await deriveKey(m.keyHex));
  } else if (m.type === 'stats') { self.postMessage({ type: 'stats', counters }); return; }
  self.postMessage({ type: 'ack', id: m.id });
};
