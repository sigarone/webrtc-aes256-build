// Trivial "E2EE": XOR the encoded payload (keeping the first bytes, which the
// RTP packetiser/depacketiser and the codec headers need) on both the sender
// and the receiver side. Janus only ever sees the XORed bytes.
const KEY = 0x5a;
const counters = { sender: { audio: 0, video: 0 }, receiver: { audio: 0, video: 0 } };
function xorFrame(frame, kind) {
  const skip = kind === 'video' ? 10 : 1;
  const d = new Uint8Array(frame.data);
  for (let i = skip; i < d.length; i++) d[i] ^= KEY;
  frame.data = d.buffer;
  return frame;
}
function setup(transformer) {
  const { readable, writable, options } = transformer;
  const { kind, role } = options;
  readable
    .pipeThrough(new TransformStream({
      transform(frame, controller) {
        counters[role][kind]++;
        controller.enqueue(xorFrame(frame, kind));
      },
    }))
    .pipeTo(writable);
}
self.onrtctransform = (event) => setup(event.transformer);
self.onmessage = (e) => {
  if (e.data === 'counters') self.postMessage({ counters });
  else if (e.data && e.data.readable) setup(e.data); // legacy createEncodedStreams path
};
