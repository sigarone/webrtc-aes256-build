import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { mintSessionToken, verifySessionToken } from '../src/token.mjs';

const SECRET = 'test-secret';
const NOW = 1_700_000_000_000; // ms; expiry with ttl 600 = 1700000600

// Literal vectors computed independently with `openssl dgst -<hash> -hmac test-secret -binary | base64`.
const VECTOR_SHA256 = '1700000600,janus,janus.plugin.videoroom:HG9NirbYc2WOstmYJM9IvPHqq/ws2hbdVzBvlbAGyVE=';
const VECTOR_SHA1 = '1700000600,janus,janus.plugin.videoroom:rYf8gm8r0lmFYa9YCGmh0mudxeM=';
const VECTOR_REALM_ONLY = '1700000600,janus:lXEiUUCTPUZ9T6hdxIm+Kyad2+BWvzTP87qAmrEcucs=';
const VECTOR_TWO_PLUGINS = '1700000600,janus,janus.plugin.videoroom,janus.plugin.echotest:5EYBmLF3D6RAKn+zNZM2hIZ2OAAs+MS9EESjSRfdAtM=';

/** Straight port of janus_auth_check_signature[_contains] (auth.c), written independently of src/token.mjs. */
function janusCheck(token, { secret, hash, nowSec, desc }) {
  const gSplit = (s, sep, max) => {
    const out = [];
    let rest = s;
    while (max <= 0 || out.length < max - 1) {
      const i = rest.indexOf(sep);
      if (i < 0) break;
      out.push(rest.slice(0, i));
      rest = rest.slice(i + sep.length);
    }
    out.push(rest);
    return s === '' ? [] : out;
  };
  const parts = gSplit(token, ':', 2);
  if (!parts[0] || parts[1] === undefined) return false;
  const data = gSplit(parts[0], ',', desc === undefined ? 3 : 0);
  if (data[0] === undefined || data[1] === undefined) return false;
  const expiry = parseInt(data[0], 10) || 0;
  if (expiry < 0 || nowSec > expiry) return false;
  if (data[1] !== 'janus') return false;
  if (desc !== undefined && !data.slice(2).includes(desc)) return false;
  return createHmac(hash, secret).update(parts[0]).digest('base64') === parts[1];
}

test('mintSessionToken matches the independent fixed vectors', () => {
  assert.equal(mintSessionToken({ secret: SECRET, nowMs: NOW }), VECTOR_SHA256);
  assert.equal(mintSessionToken({ secret: SECRET, nowMs: NOW, hash: 'sha1' }), VECTOR_SHA1);
  assert.equal(mintSessionToken({ secret: SECRET, nowMs: NOW, plugins: [] }), VECTOR_REALM_ONLY);
  assert.equal(
    mintSessionToken({ secret: SECRET, nowMs: NOW, plugins: ['janus.plugin.videoroom', 'janus.plugin.echotest'] }),
    VECTOR_TWO_PLUGINS,
  );
});

test('mintSessionToken layout: expiry, realm, plugins, standard padded base64 of HMAC over the data part', () => {
  const token = mintSessionToken({ secret: 'k', ttlSec: 30, nowMs: 5_000_999 });
  const [data, sig] = token.split(':');
  assert.equal(data, '5030,janus,janus.plugin.videoroom'); // floor(5000.999 + 30)
  assert.equal(sig, createHmac('sha256', 'k').update(data).digest('base64'));
  assert.match(sig, /^[A-Za-z0-9+/]{43}=$/);
});

test('the secret is used as raw utf-8 bytes, not hex-decoded', () => {
  const hexLooking = 'a'.repeat(64);
  const token = mintSessionToken({ secret: hexLooking, nowMs: NOW });
  const data = token.split(':')[0];
  assert.equal(token.split(':')[1], createHmac('sha256', hexLooking).update(data).digest('base64'));
  assert.notEqual(token.split(':')[1], createHmac('sha256', Buffer.from(hexLooking, 'hex')).update(data).digest('base64'));
  const secret = `p${String.fromCharCode(0xe4)}ss`; // a non-ASCII character, built so this file stays ASCII
  const utf8 = mintSessionToken({ secret, nowMs: NOW });
  assert.equal(utf8.split(':')[1], createHmac('sha256', Buffer.from(secret, 'utf8')).update(utf8.split(':')[0]).digest('base64'));
});

test('mintSessionToken validates its inputs', () => {
  assert.throws(() => mintSessionToken({ secret: '' }), /secret/);
  assert.throws(() => mintSessionToken({}), /secret/);
  assert.throws(() => mintSessionToken({ secret: 42 }), /secret/);
  for (const ttlSec of [0, -1, NaN, Infinity, '600']) {
    assert.throws(() => mintSessionToken({ secret: 'k', ttlSec }), /ttlSec/);
  }
  assert.throws(() => mintSessionToken({ secret: 'k', hash: 'md5' }), /hash/);
  assert.throws(() => mintSessionToken({ secret: 'k', plugins: ['a,b'] }), /plugins/);
  assert.throws(() => mintSessionToken({ secret: 'k', plugins: ['a:b'] }), /plugins/);
  assert.throws(() => mintSessionToken({ secret: 'k', plugins: [''] }), /plugins/);
  assert.throws(() => mintSessionToken({ secret: 'k', nowMs: NaN }), /nowMs/);
});

test('the secret never leaks into validation errors', () => {
  try {
    mintSessionToken({ secret: 'super-secret-value', ttlSec: -5 });
    assert.fail('should have thrown');
  } catch (e) {
    assert.ok(!String(e.message).includes('super-secret-value'));
  }
});

test('verifySessionToken accepts what mintSessionToken produces', () => {
  for (const hash of ['sha256', 'sha1']) {
    const token = mintSessionToken({ secret: SECRET, hash, nowMs: NOW });
    assert.equal(verifySessionToken(token, { secret: SECRET, hash, nowMs: NOW }), true);
    assert.equal(verifySessionToken(token, { secret: SECRET, hash, nowMs: NOW, plugin: 'janus.plugin.videoroom' }), true);
  }
});

test('verifySessionToken rejects expired, tampered, wrong-secret, wrong-hash, wrong-realm and malformed tokens', () => {
  const opts = { secret: SECRET, hash: 'sha256' };
  const good = VECTOR_SHA256;
  assert.equal(verifySessionToken(good, { ...opts, nowMs: 1_700_000_600_999 }), true, 'valid up to the end of the expiry second');
  assert.equal(verifySessionToken(good, { ...opts, nowMs: 1_700_000_601_000 }), false, 'expired one second later');
  assert.equal(verifySessionToken(good, { ...opts, nowMs: NOW, secret: 'other' }), false);
  assert.equal(verifySessionToken(good, { ...opts, nowMs: NOW, hash: 'sha1' }), false);
  assert.equal(verifySessionToken(good.replace('1700000600', '1700000601'), { ...opts, nowMs: NOW }), false, 'expiry is covered by the HMAC');
  assert.equal(verifySessionToken(good.slice(0, -2) + 'A=', { ...opts, nowMs: NOW }), false);
  assert.equal(verifySessionToken(good.replace(',janus,', ',other,'), { ...opts, nowMs: NOW }), false);
  const wrongRealm = (() => {
    const data = '1700000600,other,janus.plugin.videoroom';
    return `${data}:${createHmac('sha256', SECRET).update(data).digest('base64')}`;
  })();
  assert.equal(verifySessionToken(wrongRealm, { ...opts, nowMs: NOW }), false);
  for (const bad of ['', 'nocolon', ':', 'x:y', '1700000600:sig', undefined, null, 42]) {
    assert.equal(verifySessionToken(bad, { ...opts, nowMs: NOW }), false, `rejects ${String(bad)}`);
  }
  assert.equal(verifySessionToken(good, { secret: '', nowMs: NOW }), false);
});

test('verifySessionToken checks the plugin descriptor like janus_auth_check_signature_contains', () => {
  const opts = { secret: SECRET, nowMs: NOW };
  assert.equal(verifySessionToken(VECTOR_REALM_ONLY, opts), true, 'session-level check only needs expiry+realm');
  assert.equal(verifySessionToken(VECTOR_REALM_ONLY, { ...opts, plugin: 'janus.plugin.videoroom' }), false);
  assert.equal(verifySessionToken(VECTOR_SHA256, { ...opts, plugin: 'janus.plugin.echotest' }), false);
  assert.equal(verifySessionToken(VECTOR_TWO_PLUGINS, { ...opts, plugin: 'janus.plugin.echotest' }), true);
  assert.equal(verifySessionToken(VECTOR_TWO_PLUGINS, { ...opts, plugin: 'janus.plugin.videoroom' }), true);
  assert.equal(verifySessionToken(VECTOR_TWO_PLUGINS, { ...opts, plugin: 'janus.plugin' }), false, 'exact match, not prefix');
});

test('verifySessionToken agrees with a straight port of the Janus C check on a table of tokens', () => {
  const sign = (data, secret = SECRET, hash = 'sha256') => `${data}:${createHmac(hash, secret).update(data).digest('base64')}`;
  const tokens = [
    VECTOR_SHA256,
    VECTOR_SHA1,
    VECTOR_REALM_ONLY,
    VECTOR_TWO_PLUGINS,
    sign('1700000600,janus,janus.plugin.videoroom,extra'),
    sign('1700000600x,janus,janus.plugin.videoroom'), // strtoll ignores trailing garbage
    sign('-5,janus,janus.plugin.videoroom'),
    sign('abc,janus,janus.plugin.videoroom'),
    sign('0,janus,janus.plugin.videoroom'),
    sign('1700000600,janus,'),
    sign('1700000600'),
    sign('1700000600,other,janus.plugin.videoroom'),
    sign('1700000600,janus,janus.plugin.videoroom', 'wrong'),
    `${VECTOR_SHA256}:extra`, // extra colon ends up inside the signature piece
    'garbage',
    ':',
    '',
  ];
  for (const nowMs of [NOW, 1_700_000_600_500, 1_700_000_601_000, 0]) {
    for (const hash of ['sha256', 'sha1']) {
      for (const plugin of [undefined, 'janus.plugin.videoroom', 'janus.plugin.echotest']) {
        for (const token of tokens) {
          const expected = janusCheck(token, { secret: SECRET, hash, nowSec: Math.floor(nowMs / 1000), desc: plugin });
          const got = verifySessionToken(token, { secret: SECRET, hash, nowMs, plugin });
          assert.equal(got, expected, `token=${token} hash=${hash} now=${nowMs} plugin=${plugin}`);
        }
      }
    }
  }
});

test('a freshly minted token is valid now and expires after its ttl', () => {
  const token = mintSessionToken({ secret: SECRET, ttlSec: 2, nowMs: NOW });
  assert.equal(verifySessionToken(token, { secret: SECRET, nowMs: NOW + 2000 }), true);
  assert.equal(verifySessionToken(token, { secret: SECRET, nowMs: NOW + 3000 }), false);
});

test('mintSessionToken nonce: every token is its own string (qjanus counts live sessions per token string) and Janus still accepts it', () => {
  const a = mintSessionToken({ secret: SECRET, nowMs: NOW, nonce: true });
  const b = mintSessionToken({ secret: SECRET, nowMs: NOW, nonce: true });
  assert.notEqual(a, b);
  assert.equal(mintSessionToken({ secret: SECRET, nowMs: NOW }), mintSessionToken({ secret: SECRET, nowMs: NOW }), 'without it the same second gives the same string');
  for (const t of [a, b]) {
    assert.match(t, /^1700000600,janus,janus\.plugin\.videoroom,n\.[0-9a-f]{12}:/);
    assert.equal(janusCheck(t, { secret: SECRET, hash: 'sha256', nowSec: 1_700_000_000 }), true);
    assert.equal(janusCheck(t, { secret: SECRET, hash: 'sha256', nowSec: 1_700_000_000, desc: 'janus.plugin.videoroom' }), true);
    assert.equal(verifySessionToken(t, { secret: SECRET, nowMs: NOW, plugin: 'janus.plugin.videoroom' }), true);
  }
});
