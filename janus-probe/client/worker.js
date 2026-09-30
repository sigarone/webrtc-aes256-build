// Trivial "E2EE": XOR the encoded payload (keeping the first bytes, which the
// RTP packetiser/depacketiser and the codec headers need) on the sender, append
// a 4-byte FNV-1a trailer over the *encrypted* bytes, and on the receiver verify
// the trailer (byte-exact integrity of what Janus forwarded) before un-XORing.
// Janus only ever sees the XORed bytes.
const KEY = 0x5a;
const counters = {
  sender: { audio: 0, video: 0 },
  receiver: { audio: 0, video: 0 },
  integrityOk: { audio: 0, video: 0 },
  integrityBad: { audio: 0, video: 0 },
};
function fnv(d, n) {
  let h = 0x811c9dc5;
  for (let i = 0; i < n; i++) { h ^= d[i]; h = Math.imul(h, 0x01000193) >>> 0; }
  return h >>> 0;
}
function encrypt(frame, kind) {
  const skip = kind === 'video' ? 10 : 1;
  const d = new Uint8Array(frame.data);
  const out = new Uint8Array(d.length + 4);
  out.set(d);
  for (let i = skip; i < d.length; i++) out[i] ^= KEY;
  const h = fnv(out, d.length);
  out[d.length] = h >>> 24; out[d.length + 1] = (h >>> 16) & 255; out[d.length + 2] = (h >>> 8) & 255; out[d.length + 3] = h & 255;
  frame.data = out.buffer;
  return frame;
}
function decrypt(frame, kind) {
  const skip = kind === 'video' ? 10 : 1;
  const d = new Uint8Array(frame.data);
  if (d.length < 4) { counters.integrityBad[kind]++; return frame; }
  const n = d.length - 4;
  const want = ((d[n] << 24) | (d[n + 1] << 16) | (d[n + 2] << 8) | d[n + 3]) >>> 0;
  if (fnv(d, n) === want) counters.integrityOk[kind]++; else counters.integrityBad[kind]++;
  const out = new Uint8Array(d.subarray(0, n));
  for (let i = skip; i < n; i++) out[i] ^= KEY;
  frame.data = out.buffer;
  return frame;
}
function setup(transformer) {
  const { readable, writable, options } = transformer;
  const { kind, role } = options;
  readable
    .pipeThrough(new TransformStream({
      transform(frame, controller) {
        counters[role][kind]++;
        controller.enqueue(role === 'sender' ? encrypt(frame, kind) : decrypt(frame, kind));
      },
    }))
    .pipeTo(writable);
}
self.onrtctransform = (event) => setup(event.transformer);
self.onmessage = (e) => {
  if (e.data === 'counters') self.postMessage({ counters });
  else if (e.data && e.data.readable) setup(e.data); // legacy createEncodedStreams path
};
