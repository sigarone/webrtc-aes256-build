// Unit tests of the ICE candidate policy helpers (no Janus needed): node --test test/lib/ice.test.mjs
import test from 'node:test';
import assert from 'node:assert/strict';
import { sdpCandidates, normIp, isPrivateAddress } from './ice.mjs';

test('normIp canonicalises IPv6 and IPv4-mapped addresses', () => {
  assert.equal(normIp('2001:db8::5'), '2001:db8:0:0:0:0:0:5');
  assert.equal(normIp('2001:0DB8:0:0:0:0:0:5'), '2001:db8:0:0:0:0:0:5');
  assert.equal(normIp('::1'), '0:0:0:0:0:0:0:1');
  assert.equal(normIp('fe80::1%eth0'), 'fe80:0:0:0:0:0:0:1');
  assert.equal(normIp('::ffff:10.1.2.3'), '10.1.2.3');
  assert.equal(normIp('203.0.113.5'), '203.0.113.5');
});

test('isPrivateAddress: the ranges the policy forbids unless they are the enforced interface', () => {
  for (const a of ['10.0.0.1', '10.255.255.255', '100.64.0.1', '100.127.255.254', '172.16.0.1', '172.31.255.255', '192.168.1.1',
    '127.0.0.1', '169.254.1.1', 'fd00::1', 'fc00::1', 'fe80::1', '::1']) assert.equal(isPrivateAddress(a), true, a);
  for (const a of ['100.63.255.255', '100.128.0.1', '172.15.0.1', '172.32.0.1', '192.167.1.1', '11.0.0.1', '8.8.8.8', '203.0.113.5',
    '2001:db8::5', '2a01:4f9::1', '2606:4700::1']) assert.equal(isPrivateAddress(a), false, a);
});

test('sdpCandidates parses candidate lines (CRLF, IPv4 and IPv6)', () => {
  const sdp = ['v=0', 'm=audio 9 UDP/TLS/RTP/SAVPF 111', 'a=ice-lite',
    'a=candidate:1 1 udp 2015363327 203.0.113.5 20001 typ host',
    'a=candidate:2 1 udp 2015363071 2001:db8::5 20002 typ host', 'a=end-of-candidates', ''].join('\r\n');
  const c = sdpCandidates(sdp);
  assert.equal(c.length, 2);
  assert.deepEqual(c.map((x) => [x.address, x.port, x.type, x.transport]), [['203.0.113.5', 20001, 'host', 'udp'], ['2001:db8::5', 20002, 'host', 'udp']]);
});
