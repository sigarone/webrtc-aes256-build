// Janus core signed session tokens ("token_auth_secret" mode), as verified by
// Janus v1.4.2 src/auth.c (janus_auth_check_signature[_contains]).
//
//   token = "<expiry_unix>,janus[,<plugin package>...]:<base64 HMAC>"
//
// The HMAC covers the text BEFORE the first ':' (the "data" part). The key is
// the secret string's bytes as-is (NOT hex-decoded), the digest is the
// configured token_auth_hash (sha1 or sha256; the qjanus config uses sha256)
// and the encoding is standard base64 with padding. Janus rejects a request
// when "now" (whole seconds) is greater than the expiry, so a token is valid
// up to and including its expiry second.

import { createHmac, randomBytes, timingSafeEqual } from 'node:crypto';

/** Realm string Janus checks for the core session token. */
export const TOKEN_REALM = 'janus';
/** Package name of the VideoRoom plugin (the only plugin the harness uses). */
export const VIDEOROOM_PLUGIN = 'janus.plugin.videoroom';

const HASHES = new Set(['sha1', 'sha256']);

function normalizeHash(hash) {
  const h = typeof hash === 'string' ? hash.toLowerCase() : '';
  if (!HASHES.has(h)) {
    throw new RangeError(`unsupported token hash (supported: ${[...HASHES].join(', ')})`);
  }
  return h;
}

function hmacBase64(secret, hash, data) {
  return createHmac(hash, secret).update(data).digest('base64');
}

/**
 * Mint a signed session token.
 * @param {object} o
 * @param {string} o.secret  core token_auth_secret (never included in errors)
 * @param {number} [o.ttlSec=600]  lifetime in seconds (> 0)
 * @param {string[]} [o.plugins]  plugin packages the token may attach to
 * @param {'sha1'|'sha256'} [o.hash='sha256']
 * @param {number} [o.nowMs=Date.now()]  clock override (tests)
 * @param {boolean} [o.nonce=false]  append a random descriptor ("n.<hex>", ignored by Janus) so that this token is its
 *   own string: qjanus (patch 0008) allows only a few live sessions per distinct token string, and tokens minted
 *   within the same second with the same ttl are otherwise the same string. Bots and admin sessions use it.
 * @returns {string}
 */
export function mintSessionToken({
  secret,
  ttlSec = 600,
  plugins = [VIDEOROOM_PLUGIN],
  hash = 'sha256',
  nowMs = Date.now(),
  nonce = false,
} = {}) {
  if (typeof secret !== 'string' || secret.length === 0) {
    throw new TypeError('token secret must be a non-empty string');
  }
  if (typeof ttlSec !== 'number' || !Number.isFinite(ttlSec) || ttlSec <= 0) {
    throw new RangeError('token ttlSec must be a finite number > 0');
  }
  if (typeof nowMs !== 'number' || !Number.isFinite(nowMs)) {
    throw new TypeError('nowMs must be a finite number');
  }
  if (!Array.isArray(plugins) || plugins.some((p) => typeof p !== 'string' || p === '' || /[,:]/.test(p))) {
    throw new TypeError("plugins must be an array of non-empty strings without ',' or ':'");
  }
  const h = normalizeHash(hash);
  const expiry = Math.floor(nowMs / 1000 + ttlSec);
  const data = [expiry, TOKEN_REALM, ...plugins, ...(nonce ? [`n.${randomBytes(6).toString('hex')}`] : [])].join(',');
  return `${data}:${hmacBase64(secret, h, data)}`;
}

/**
 * Verify a token exactly like Janus does (used by the mocks and the tests).
 * Without `plugin` this is janus_auth_check_signature(token, "janus") (what
 * every session-level request goes through); with `plugin` it is
 * janus_auth_check_signature_contains (what "attach" additionally checks).
 * @returns {boolean}
 */
export function verifySessionToken(token, { secret, hash = 'sha256', nowMs = Date.now(), plugin } = {}) {
  if (typeof token !== 'string' || typeof secret !== 'string' || secret.length === 0) return false;
  const h = normalizeHash(hash);
  // g_strsplit(token, ":", 2): split at the FIRST colon, both pieces required.
  const colon = token.indexOf(':');
  if (colon < 0) return false;
  const data = token.slice(0, colon);
  const signature = token.slice(colon + 1);
  // Janus splits the data on ',' (max 3 pieces without a plugin, all pieces with one).
  const fields = data.split(',');
  if (fields.length < 2) return false;
  // strtoll(): leading integer, garbage ignored, no digits -> 0.
  const expiry = Number.parseInt(fields[0], 10) || 0;
  if (expiry < 0 || Math.floor(nowMs / 1000) > expiry) return false;
  if (fields[1] !== TOKEN_REALM) return false;
  if (plugin !== undefined && !fields.slice(2).includes(plugin)) return false;
  const expected = Buffer.from(hmacBase64(secret, h, data));
  const given = Buffer.from(signature);
  return expected.length === given.length && timingSafeEqual(expected, given);
}
