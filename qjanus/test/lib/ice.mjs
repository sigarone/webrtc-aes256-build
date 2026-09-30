// ICE candidate policy of qjanus, as the tests check it in every SDP that comes out of Janus: the
// candidates are the addresses of the ONE enforced interface (QJANUS_ICE_ENFORCE_IFACE, nat.ice_enforce_list)
// and of nothing else. In particular never the private address of a VPN / tailscale / bridge / other
// interface: a 10.x, 100.64-127.x, 172.16-31.x, 192.168.x (or fc00::/7, fe80::/10) candidate is only
// acceptable when it is an address of the enforced interface itself.
//   QJANUS_ICE_ENFORCE_IFACE   the enforced interface (the same variable as in /etc/qjanus/qjanus.env)
//   QJANUS_NAT_1_1             optional: the public address a node behind a 1:1 NAT advertises instead
//   QJANUS_DECOYS              optional: comma separated addresses of decoy interfaces that exist on this host
//                              only to prove they are never offered
import assert from 'node:assert/strict';
import os from 'node:os';

export const ICE_IFACE = process.env.QJANUS_ICE_ENFORCE_IFACE || '';

// "a=candidate:<foundation> <component> <transport> <priority> <address> <port> typ <type> ..."
export function sdpCandidates(sdp) {
  return [...sdp.matchAll(/^a=candidate:(\S+) (\d+) (\S+) (\d+) (\S+) (\d+) typ (\S+)/gim)].map((m) => ({
    foundation: m[1], component: Number(m[2]), transport: m[3].toLowerCase(), priority: Number(m[4]),
    address: m[5], port: Number(m[6]), type: m[7].toLowerCase(),
  }));
}

// canonical text of an address: IPv4 as is, IPv6 expanded to 8 lowercase groups, zone dropped,
// IPv4-mapped IPv6 as the IPv4 address
export function normIp(address) {
  const a = String(address).toLowerCase().replace(/%.*$/, '');
  if (!a.includes(':')) return a;
  const mapped = /^::ffff:(\d+\.\d+\.\d+\.\d+)$/.exec(a);
  if (mapped) return mapped[1];
  const [head, tail] = a.split('::');
  const h = head ? head.split(':') : [];
  const t = tail === undefined ? [] : (tail ? tail.split(':') : []);
  const fill = tail === undefined ? [] : Array(8 - h.length - t.length).fill('0');
  return [...h, ...fill, ...t].map((g) => g.replace(/^0+(?=.)/, '')).join(':');
}

// addresses that cannot be reached from the internet (what the installer refuses as an ICE interface)
export function isPrivateAddress(address) {
  const a = normIp(address);
  const v4 = /^(\d+)\.(\d+)\.(\d+)\.(\d+)$/.exec(a);
  if (v4) {
    const [o1, o2] = [Number(v4[1]), Number(v4[2])];
    return o1 === 10 || o1 === 127 || o1 === 0 || (o1 === 172 && o2 >= 16 && o2 <= 31) || (o1 === 192 && o2 === 168)
      || (o1 === 100 && o2 >= 64 && o2 <= 127) || (o1 === 169 && o2 === 254);
  }
  const first = parseInt(a.split(':')[0], 16);
  return a === '0:0:0:0:0:0:0:1' || (first & 0xfe00) === 0xfc00 || (first & 0xffc0) === 0xfe80;
}

// the addresses this host carries on an interface (what Janus may offer for it)
export function interfaceAddresses(name) {
  return (os.networkInterfaces()[name] || []).map((i) => normIp(i.address));
}

const mask = (a) => (String(a).includes(':') ? `${String(a).split(':')[0]}:x` : `${String(a).split('.').slice(0, 2).join('.')}.x.x`);

// assertIceCandidates(sdp, label): every candidate of the SDP is an address of the enforced interface
// (or the NAT 1:1 address), none is a foreign private address, none belongs to a decoy interface, and the
// enforced interface is actually offered (an over-filtering node would pass the other checks with nothing).
// require:false = an SDP that may legitimately carry no candidate (a renegotiation): only what it has is checked
export function assertIceCandidates(sdp, label, { require = true } = {}) {
  assert.ok(ICE_IFACE, 'QJANUS_ICE_ENFORCE_IFACE must be set for the ICE candidate check');
  const own = new Set(interfaceAddresses(ICE_IFACE));
  assert.ok(own.size > 0, `the enforced interface ${ICE_IFACE} carries no address on this host`);
  const nat = process.env.QJANUS_NAT_1_1 ? normIp(process.env.QJANUS_NAT_1_1) : null;
  const decoys = (process.env.QJANUS_DECOYS || '').split(',').filter(Boolean).map(normIp);
  const cands = sdpCandidates(sdp);
  if (!require && cands.length === 0) return cands;   // a renegotiation may carry none: what it does carry is still checked
  assert.ok(cands.length > 0, `${label}: the SDP carries no ICE candidate (the policy check would prove nothing)`);
  for (const c of cands) {
    const a = normIp(c.address);
    assert.ok(!decoys.includes(a), `${label}: the address ${mask(a)} of a decoy interface is offered as an ICE candidate`);
    const allowed = own.has(a) || a === nat;
    assert.ok(allowed, `${label}: candidate ${mask(a)}${isPrivateAddress(a) ? ' (private)' : ''} is not an address of the enforced interface ${ICE_IFACE}`);
    assert.equal(c.type, 'host', `${label}: ice-lite offers host candidates only, got ${c.type}`);
    assert.equal(c.transport, 'udp', `${label}: ice_tcp is off, got ${c.transport}`);
  }
  const offered = new Set(cands.map((c) => normIp(c.address)));
  const globalOwn = (os.networkInterfaces()[ICE_IFACE] || []).filter((i) => !i.internal && !/^fe80/i.test(i.address)).map((i) => normIp(i.address));
  assert.ok(nat ? offered.has(nat) : globalOwn.some((a) => offered.has(a)), `${label}: no address of the enforced interface ${ICE_IFACE} is offered`);
  return cands;
}

// a one-line summary for the CI log (masked: no full addresses in logs)
export function describeCandidates(cands) {
  return cands.map((c) => `${c.type}/${c.transport}/${mask(c.address)}${isPrivateAddress(c.address) ? '(private, on the enforced interface)' : ''}`).join(' ');
}
