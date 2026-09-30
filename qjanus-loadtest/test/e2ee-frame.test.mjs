import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  AES_OVERHEAD, XOR_OVERHEAD, IV_LENGTH, createCodec, decryptFrame, deriveKey, encryptFrame,
  headerLength, hexToBytes, vp8IsKeyFrame, xorDecryptFrame, xorEncryptFrame,
} from '../page/e2ee-frame.mjs';

const KEY_HEX = '00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff';
const OTHER_HEX = 'ffeeddccbbaa99887766554433221100ffeeddccbbaa99887766554433221100';

function payload(n, first = 0x00) {
  const d = new Uint8Array(n);
  for (let i = 0; i < n; i++) d[i] = (i * 31 + 7) & 0xff;
  d[0] = first;
  return d;
}

test('hexToBytes rejects malformed keys', () => {
  assert.deepEqual([...hexToBytes('00ff10')], [0, 255, 16]);
  assert.throws(() => hexToBytes('abc'));
  assert.throws(() => hexToBytes('zz'));
});

test('header lengths: Opus 1, VP8 10 (key) / 3 (delta)', () => {
  assert.equal(headerLength('audio', false), 1);
  assert.equal(headerLength('video', true), 10);
  assert.equal(headerLength('video', false), 3);
  assert.equal(vp8IsKeyFrame(Uint8Array.of(0x10)), true); // bit 0 clear
  assert.equal(vp8IsKeyFrame(Uint8Array.of(0x11)), false);
});

test('HKDF key: same key hex -> interoperable, different -> not', async () => {
  const a = await deriveKey(KEY_HEX);
  const a2 = await deriveKey(KEY_HEX);
  const b = await deriveKey(OTHER_HEX);
  const ct = await encryptFrame(a, 'audio', payload(80));
  assert.equal((await decryptFrame(a2, 'audio', ct)).ok, true);
  const bad = await decryptFrame(b, 'audio', ct);
  assert.equal(bad.ok, false);
  assert.equal(bad.reason, 'auth');
});

test('aesgcm audio round trip, 1 clear header byte, format and size', async () => {
  const key = await deriveKey(KEY_HEX);
  const frame = payload(120, 0x78); // Opus TOC byte first
  const enc = await encryptFrame(key, 'audio', frame);
  assert.equal(enc.length, frame.length + AES_OVERHEAD);
  assert.equal(AES_OVERHEAD, 16 + 12 + 2);
  assert.equal(enc[0], 0x78); // header byte in the clear
  assert.notDeepEqual([...enc.subarray(1, 100)], [...frame.subarray(1, 100)]);
  assert.equal(enc[enc.length - 2], 12); // ivLength
  assert.equal(enc[enc.length - 1], 0); // keyIndex
  const dec = await decryptFrame(key, 'audio', enc);
  assert.equal(dec.ok, true);
  assert.deepEqual([...dec.data], [...frame]);
});

test('aesgcm video: key frames keep 10 clear bytes, delta frames 3', async () => {
  const key = await deriveKey(KEY_HEX);
  const keyFrame = payload(500, 0x10); // tag bit 0 = 0 -> key frame (fallback detection)
  const deltaFrame = payload(300, 0x11);
  const ek = await encryptFrame(key, 'video', keyFrame);
  const ed = await encryptFrame(key, 'video', deltaFrame);
  assert.deepEqual([...ek.subarray(0, 10)], [...keyFrame.subarray(0, 10)]);
  assert.notEqual(ek[10], keyFrame[10]);
  assert.deepEqual([...ed.subarray(0, 3)], [...deltaFrame.subarray(0, 3)]);
  assert.notEqual(ed[3], deltaFrame[3]);
  assert.equal(ek.length, 500 + AES_OVERHEAD);
  assert.equal(ed.length, 300 + AES_OVERHEAD);
  assert.deepEqual([...(await decryptFrame(key, 'video', ek)).data], [...keyFrame]);
  assert.deepEqual([...(await decryptFrame(key, 'video', ed)).data], [...deltaFrame]);
});

test('aesgcm video: the platform frame type overrides the tag bit', async () => {
  const key = await deriveKey(KEY_HEX);
  const f = payload(200, 0x11); // looks like a delta frame...
  const enc = await encryptFrame(key, 'video', f, { isKey: true }); // ...but the encoder says key
  assert.deepEqual([...enc.subarray(0, 10)], [...f.subarray(0, 10)]);
  assert.equal((await decryptFrame(key, 'video', enc, { isKey: true })).ok, true);
  // decrypting with the wrong header assumption must fail (AAD mismatch), never throw
  assert.equal((await decryptFrame(key, 'video', enc, { isKey: false })).ok, false);
});

test('aesgcm: a fresh random IV per frame', async () => {
  const key = await deriveKey(KEY_HEX);
  const f = payload(64);
  const a = await encryptFrame(key, 'audio', f);
  const b = await encryptFrame(key, 'audio', f);
  const iv = (e) => [...e.subarray(e.length - 2 - IV_LENGTH, e.length - 2)].join(',');
  assert.notEqual(iv(a), iv(b));
  assert.notDeepEqual([...a], [...b]);
});

test('aesgcm tamper detection: ciphertext, header (AAD), iv, tag, trailer, truncation', async () => {
  const key = await deriveKey(KEY_HEX);
  const enc = await encryptFrame(key, 'audio', payload(90, 0x33));
  const flip = (i) => { const c = new Uint8Array(enc); c[i] ^= 0x01; return c; };
  assert.equal((await decryptFrame(key, 'audio', flip(0))).ok, false); // header is authenticated as AAD
  assert.equal((await decryptFrame(key, 'audio', flip(5))).ok, false); // ciphertext
  assert.equal((await decryptFrame(key, 'audio', flip(enc.length - 16))).ok, false); // iv
  assert.equal((await decryptFrame(key, 'audio', flip(enc.length - 2 - 12 - 3))).ok, false); // tag
  assert.equal((await decryptFrame(key, 'audio', flip(enc.length - 2))).reason, 'bad_trailer');
  assert.equal((await decryptFrame(key, 'audio', flip(enc.length - 1))).reason, 'key_index');
  assert.equal((await decryptFrame(key, 'audio', enc.subarray(0, 20))).reason, 'too_short');
  assert.equal((await decryptFrame(key, 'audio', new Uint8Array(0))).ok, false);
});

test('aesgcm handles frames shorter than the clear header (empty Opus payload)', async () => {
  const key = await deriveKey(KEY_HEX);
  for (const [kind, n] of [['audio', 0], ['audio', 1], ['video', 2], ['video', 3]]) {
    const f = payload(n, 0x10);
    const enc = await encryptFrame(key, kind, f);
    const dec = await decryptFrame(key, kind, enc);
    assert.equal(dec.ok, true, `${kind} ${n}`);
    assert.deepEqual([...dec.data], [...f]);
  }
});

test('xor mode: clear header bytes, checksum trailer, round trip, tamper detection', () => {
  const f = payload(200, 0x10);
  const enc = xorEncryptFrame('video', f);
  assert.equal(enc.length, 200 + XOR_OVERHEAD);
  assert.deepEqual([...enc.subarray(0, 10)], [...f.subarray(0, 10)]);
  assert.notEqual(enc[10], f[10]);
  const dec = xorDecryptFrame('video', enc);
  assert.equal(dec.ok, true);
  assert.deepEqual([...dec.data], [...f]);
  const bad = new Uint8Array(enc);
  bad[50] ^= 1;
  assert.equal(xorDecryptFrame('video', bad).reason, 'checksum');
  assert.equal(xorDecryptFrame('audio', new Uint8Array(2)).reason, 'too_short');
  const a = xorEncryptFrame('audio', payload(40, 0x55));
  assert.equal(a[0], 0x55);
  assert.equal(xorDecryptFrame('audio', a).ok, true);
});

test('createCodec: aesgcm / xor / none, unknown mode throws', async () => {
  const f = payload(150, 0x10);
  for (const mode of ['aesgcm', 'xor', 'none']) {
    const c = await createCodec({ mode, keyHex: KEY_HEX });
    const e = await c.encrypt('video', f, undefined);
    const d = await c.decrypt('video', e, undefined);
    assert.equal(d.ok, true, mode);
    assert.deepEqual([...d.data], [...f]);
    if (mode === 'none') assert.equal(e, f);
    else assert.notDeepEqual([...e.subarray(10, 60)], [...f.subarray(10, 60)]);
  }
  await assert.rejects(createCodec({ mode: 'rot13', keyHex: KEY_HEX }));
  await assert.rejects(createCodec({ mode: 'aesgcm', keyHex: 'nothex' }));
});
