// Frame-level E2EE-like transforms for the load-test bots (WebCrypto only; runs in a Worker
// and in Node 22 unit tests).
//
// Mode 'aesgcm' mimics the real frame format of spec section 5.3, so the SFU forwards frames
// with the same size and header layout as in production:
//     aes_key = HKDF-SHA256(ikm = key bytes, salt = empty, info = 128 x 0x00, L = 32)
//     frame   = header(unencrypted) || ciphertext || tag(16) || iv(12) || [12, keyIndex]
//     AAD     = header, AES-256-GCM, random 12-byte IV per frame
// The header stays in the clear so the SFU can find VP8 key frames and simulcast layers:
// Opus 1 byte, VP8 10 bytes on key frames and 3 on delta frames.
//
// Mode 'xor' is the cheap variant of the feasibility probe (same clear header bytes, XOR of the
// rest, 4-byte FNV-1a trailer over the encrypted bytes). Mode 'none' does not transform at all.

export const IV_LENGTH = 12;
export const TAG_LENGTH = 16;
/** Trailer after the IV: [ivLength, keyIndex]. */
export const TRAILER_LENGTH = 2;
export const AES_OVERHEAD = TAG_LENGTH + IV_LENGTH + TRAILER_LENGTH;
export const XOR_OVERHEAD = 4;

const XOR_KEY = 0x5a;

/** Hex string -> bytes. Throws on malformed input so a bad test key is caught at start. */
export function hexToBytes(hex) {
  if (typeof hex !== 'string' || hex.length % 2 !== 0 || !/^[0-9a-fA-F]*$/.test(hex)) {
    throw new Error('key must be an even-length hex string');
  }
  const out = new Uint8Array(hex.length / 2);
  for (let i = 0; i < out.length; i++) out[i] = parseInt(hex.substr(i * 2, 2), 16);
  return out;
}

/** aes_key = HKDF-SHA256(ikm, salt = empty, info = 128 zero bytes) -> AES-256-GCM CryptoKey. */
export async function deriveKey(keyHex) {
  const ikm = await crypto.subtle.importKey('raw', hexToBytes(keyHex), 'HKDF', false, ['deriveKey']);
  return crypto.subtle.deriveKey(
    { name: 'HKDF', hash: 'SHA-256', salt: new Uint8Array(0), info: new Uint8Array(128) },
    ikm, { name: 'AES-GCM', length: 256 }, false, ['encrypt', 'decrypt']);
}

/** VP8 frame tag: bit 0 of the first byte is 0 for key frames. That byte is inside the clear header. */
export function vp8IsKeyFrame(data) {
  return data.length > 0 && (data[0] & 1) === 0;
}

/** Number of leading bytes that stay unencrypted. */
export function headerLength(kind, isKeyFrame) {
  if (kind === 'video') return isKeyFrame ? 10 : 3;
  return 1;
}

function resolveKey(kind, data, isKey) {
  // Use the frame type when the platform provides it, else the VP8 tag bit of the clear byte.
  return kind === 'video' ? (isKey === undefined || isKey === null ? vp8IsKeyFrame(data) : !!isKey) : false;
}

/** Encrypt one encoded frame (Uint8Array) -> Uint8Array. */
export async function encryptFrame(key, kind, data, { isKey, keyIndex = 0 } = {}) {
  const hl = Math.min(headerLength(kind, resolveKey(kind, data, isKey)), data.length);
  const iv = crypto.getRandomValues(new Uint8Array(IV_LENGTH));
  const header = data.subarray(0, hl);
  const ct = new Uint8Array(await crypto.subtle.encrypt(
    { name: 'AES-GCM', iv, additionalData: header, tagLength: TAG_LENGTH * 8 }, key, data.subarray(hl)));
  const out = new Uint8Array(hl + ct.length + IV_LENGTH + TRAILER_LENGTH);
  out.set(header, 0);
  out.set(ct, hl); // ciphertext || tag
  out.set(iv, hl + ct.length);
  out[out.length - 2] = IV_LENGTH;
  out[out.length - 1] = keyIndex & 0xff;
  return out;
}

/**
 * Decrypt one frame. Returns {ok:true, data} or {ok:false, reason}; never throws for bad input
 * (tampering, wrong key, truncated frames).
 */
export async function decryptFrame(key, kind, data, { isKey, keyIndex = 0 } = {}) {
  if (data.length < AES_OVERHEAD) return { ok: false, reason: 'too_short' };
  if (data[data.length - 2] !== IV_LENGTH) return { ok: false, reason: 'bad_trailer' };
  if (data[data.length - 1] !== (keyIndex & 0xff)) return { ok: false, reason: 'key_index' };
  const hl = Math.min(headerLength(kind, resolveKey(kind, data, isKey)), data.length - AES_OVERHEAD);
  const ivStart = data.length - TRAILER_LENGTH - IV_LENGTH;
  const iv = data.subarray(ivStart, ivStart + IV_LENGTH);
  const header = data.subarray(0, hl);
  const body = data.subarray(hl, ivStart); // ciphertext || tag
  try {
    const pt = new Uint8Array(await crypto.subtle.decrypt(
      { name: 'AES-GCM', iv, additionalData: header, tagLength: TAG_LENGTH * 8 }, key, body));
    const out = new Uint8Array(hl + pt.length);
    out.set(header, 0);
    out.set(pt, hl);
    return { ok: true, data: out };
  } catch (e) {
    return { ok: false, reason: 'auth' };
  }
}

// ------------------------------------------------------------------------------ xor mode

function fnv1a(d, n) {
  let h = 0x811c9dc5;
  for (let i = 0; i < n; i++) { h ^= d[i]; h = Math.imul(h, 0x01000193) >>> 0; }
  return h >>> 0;
}

export function xorEncryptFrame(kind, data, { isKey } = {}) {
  const skip = Math.min(headerLength(kind, resolveKey(kind, data, isKey)), data.length);
  const out = new Uint8Array(data.length + XOR_OVERHEAD);
  out.set(data);
  for (let i = skip; i < data.length; i++) out[i] ^= XOR_KEY;
  const h = fnv1a(out, data.length);
  out[data.length] = h >>> 24;
  out[data.length + 1] = (h >>> 16) & 255;
  out[data.length + 2] = (h >>> 8) & 255;
  out[data.length + 3] = h & 255;
  return out;
}

export function xorDecryptFrame(kind, data, { isKey } = {}) {
  if (data.length < XOR_OVERHEAD) return { ok: false, reason: 'too_short' };
  const n = data.length - XOR_OVERHEAD;
  const want = ((data[n] << 24) | (data[n + 1] << 16) | (data[n + 2] << 8) | data[n + 3]) >>> 0;
  if (fnv1a(data, n) !== want) return { ok: false, reason: 'checksum' };
  const skip = Math.min(headerLength(kind, resolveKey(kind, data, isKey)), n);
  const out = new Uint8Array(data.subarray(0, n));
  for (let i = skip; i < n; i++) out[i] ^= XOR_KEY;
  return { ok: true, data: out };
}

// ---------------------------------------------------------------------------- unified API

/**
 * Codec for one (mode, key): encrypt(kind, data, isKey) -> Uint8Array,
 * decrypt(kind, data, isKey) -> {ok, data|reason}. Mode 'none' passes frames through.
 */
export async function createCodec({ mode, keyHex }) {
  if (mode === 'aesgcm') {
    const key = await deriveKey(keyHex);
    return {
      mode,
      encrypt: (kind, data, isKey) => encryptFrame(key, kind, data, { isKey }),
      decrypt: (kind, data, isKey) => decryptFrame(key, kind, data, { isKey }),
    };
  }
  if (mode === 'xor') {
    return {
      mode,
      encrypt: async (kind, data, isKey) => xorEncryptFrame(kind, data, { isKey }),
      decrypt: async (kind, data, isKey) => xorDecryptFrame(kind, data, { isKey }),
    };
  }
  if (mode === 'none') {
    return {
      mode,
      encrypt: async (kind, data) => data,
      decrypt: async (kind, data) => ({ ok: true, data }),
    };
  }
  throw new Error(`unknown e2ee mode: ${mode}`);
}
