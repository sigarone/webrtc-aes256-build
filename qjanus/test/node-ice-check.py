#!/usr/bin/env python3
"""node-ice-check.py: live check of the ICE candidates of a RUNNING qjanus node (python3 standard library only).

It does what a group-call client does, without any media: creates a throw-away room through the local server
API (HTTP), opens a WebSocket session (plain ws:// on the node, or wss:// through Caddy: the public path), joins
as a publisher with a token bound to its pseudonym, publishes a synthetic E2EE offer and reads the ICE candidates
of Janus's answer. It then asserts that

  * every candidate is an address of the ONE enforced interface (QJANUS_ICE_ENFORCE_IFACE) and nothing else,
  * no address of any OTHER interface of this host (VPN, tailscale, bridge, ...) is offered,
  * no private address is offered unless it belongs to the enforced interface,
  * the DTLS fingerprint in the answer is the pinned certificate of the node.

Run it as root on the node (it reads /etc/qjanus/qjanus.env for the token secret and the admin key; they are
used in memory and never printed). The room and the session are destroyed again. Addresses are printed masked.

  sudo python3 node-ice-check.py [--ws ws://127.0.0.1:8188 | wss://<host>/janus] [--env /etc/qjanus/qjanus.env]
"""
import argparse
import base64
import hashlib
import hmac
import ipaddress
import json
import os
import re
import secrets
import socket
import ssl
import struct
import subprocess
import sys
import time
import urllib.request
from urllib.parse import urlparse

PLUGIN = 'janus.plugin.videoroom'


def die(msg):
    print(f'FAIL  {msg}')
    sys.exit(1)


def mask(a):
    a = str(a)
    return a.split(':')[0] + ':x' if ':' in a else '.'.join(a.split('.')[:2]) + '.x.x'


def read_env(path):
    env = {}
    with open(path) as f:
        for line in f:
            line = line.strip()
            if line and not line.startswith('#') and '=' in line:
                k, v = line.split('=', 1)
                env[k] = v
    return env


def mint_token(secret, ttl=300):
    data = f'{int(time.time()) + ttl},janus,{PLUGIN}'
    sig = base64.b64encode(hmac.new(secret.encode(), data.encode(), hashlib.sha256).digest()).decode()
    return f'{data}:{sig}'


# ------------------------------------------------------------------------------------------ HTTP API
class Http:
    def __init__(self, base, token, admin_key):
        self.base, self.token, self.admin_key = base, token, admin_key

    def post(self, path, body):
        body = {'transaction': secrets.token_hex(6), 'token': self.token, **body}
        req = urllib.request.Request(self.base + path, data=json.dumps(body).encode(), headers={'content-type': 'application/json'})
        with urllib.request.urlopen(req, timeout=10) as r:
            return json.load(r)

    def get(self, path):
        with urllib.request.urlopen(self.base + path, timeout=10) as r:
            return json.load(r)


def plugin_data(resp):
    return (resp.get('plugindata') or {}).get('data') or {}


# --------------------------------------------------------------------- minimal WebSocket client (RFC 6455)
class Ws:
    def __init__(self, url, protocol='janus-protocol', timeout=15):
        u = urlparse(url)
        tls = u.scheme == 'wss'
        host, port = u.hostname, u.port or (443 if tls else 80)
        raw = socket.create_connection((host, port), timeout=timeout)
        self.sock = ssl.create_default_context().wrap_socket(raw, server_hostname=host) if tls else raw
        self.sock.settimeout(timeout)
        key = base64.b64encode(os.urandom(16)).decode()
        path = (u.path or '/') + (f'?{u.query}' if u.query else '')
        self.sock.sendall((f'GET {path} HTTP/1.1\r\nHost: {u.netloc}\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                           f'Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Protocol: {protocol}\r\n\r\n').encode())
        head = b''
        while b'\r\n\r\n' not in head:
            chunk = self.sock.recv(4096)
            if not chunk:
                die('the WebSocket handshake was closed by the peer')
            head += chunk
        head, self.buf = head.split(b'\r\n\r\n', 1)
        if not head.startswith(b'HTTP/1.1 101'):
            status_line = head.split(b'\r\n')[0].decode(errors='replace')
            die('WebSocket upgrade refused: ' + status_line)

    def _read(self, n):
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError('websocket closed')
            self.buf += chunk
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def send(self, obj):
        payload = json.dumps(obj).encode()
        hdr = bytearray([0x81])
        n = len(payload)
        if n < 126:
            hdr.append(0x80 | n)
        elif n < 65536:
            hdr += bytes([0x80 | 126]) + struct.pack('>H', n)
        else:
            hdr += bytes([0x80 | 127]) + struct.pack('>Q', n)
        mask_key = os.urandom(4)
        hdr += mask_key
        self.sock.sendall(bytes(hdr) + bytes(b ^ mask_key[i % 4] for i, b in enumerate(payload)))

    def recv(self):
        """next text message as a dict (control frames are handled here)"""
        message = b''
        while True:
            b1, b2 = self._read(2)
            opcode, length = b1 & 0x0F, b2 & 0x7F
            if length == 126:
                length = struct.unpack('>H', self._read(2))[0]
            elif length == 127:
                length = struct.unpack('>Q', self._read(8))[0]
            payload = self._read(length)
            if opcode == 0x9:     # ping -> pong (masked, as every client frame)
                key = os.urandom(4)
                self.sock.sendall(bytes([0x8A, 0x80 | len(payload)]) + key + bytes(b ^ key[i % 4] for i, b in enumerate(payload)))
                continue
            if opcode == 0xA:
                continue
            if opcode == 0x8:
                raise ConnectionError('websocket closed by the peer')
            message += payload
            if b1 & 0x80:
                return json.loads(message)

    def close(self):
        try:
            self.sock.sendall(bytes([0x88, 0x80]) + os.urandom(4))
            self.sock.close()
        except OSError:
            pass


class Janus:
    """request/response over the WebSocket; unsolicited events are kept"""
    def __init__(self, ws, token):
        self.ws, self.token, self.sid, self.events = ws, token, 0, []

    def request(self, obj, ack_only=False, timeout=20):
        tx = secrets.token_hex(6)
        self.ws.send({**obj, 'transaction': tx, **({'token': self.token} if obj['janus'] not in ('info', 'ping') else {})})
        end = time.time() + timeout
        while time.time() < end:
            m = self.ws.recv()
            if m.get('transaction') == tx and (m.get('janus') != 'ack' or ack_only):
                return m
            if m.get('janus') != 'ack':
                self.events.append(m)
        die(f'timeout waiting for the reply to {obj["janus"]}')


def synthetic_offer():
    fp = 'sha-256 ' + ':'.join(f'{(i * 7 + 3) & 255:02X}' for i in range(32))

    def common(mid):
        return ['c=IN IP4 0.0.0.0', 'a=rtcp:9 IN IP4 0.0.0.0', 'a=ice-ufrag:qjt1', 'a=ice-pwd:qjtestqjtestqjtestqjtest',
                'a=ice-options:trickle', f'a=fingerprint:{fp}', 'a=setup:actpass', f'a=mid:{mid}',
                'a=extmap:2 urn:ietf:params:rtp-hdrext:sdes:mid',
                'a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01']
    lines = ['v=0', 'o=- 4611731400430051336 2 IN IP4 127.0.0.1', 's=-', 't=0 0', 'a=group:BUNDLE 0 1', 'a=extmap-allow-mixed',
             'a=msid-semantic: WMS qjtest', 'm=audio 9 UDP/TLS/RTP/SAVPF 111', *common(0), 'a=sendonly', 'a=msid:qjtest a0', 'a=rtcp-mux',
             'a=rtpmap:111 opus/48000/2', 'a=rtcp-fb:111 transport-cc',
             'a=fmtp:111 minptime=60;useinbandfec=1;usedtx=0;cbr=1;stereo=0;maxaveragebitrate=32000', 'a=ptime:60', 'a=ssrc:1001 cname:qjtest',
             'm=video 9 UDP/TLS/RTP/SAVPF 96', *common(1), 'a=sendonly', 'a=msid:qjtest v0', 'a=rtcp-mux', 'a=rtcp-rsize',
             'a=rtpmap:96 VP8/90000', 'a=rtcp-fb:96 goog-remb', 'a=rtcp-fb:96 transport-cc', 'a=rtcp-fb:96 ccm fir', 'a=rtcp-fb:96 nack',
             'a=rtcp-fb:96 nack pli', 'a=ssrc:2001 cname:qjtest']
    return '\r\n'.join(lines) + '\r\n'


# ------------------------------------------------------------------------------------ addresses of this host
def host_addresses():
    """{interface: [ip_address, ...]} from `ip -o addr` (every scope: a link-local one must not be offered either)"""
    out = subprocess.run(['ip', '-o', 'addr', 'show'], capture_output=True, text=True, check=True).stdout
    res = {}
    for line in out.splitlines():
        f = line.split()
        if len(f) >= 4 and f[2] in ('inet', 'inet6'):
            res.setdefault(f[1], []).append(ipaddress.ip_address(f[3].split('/')[0].split('%')[0]))
    return res


def is_private(a):
    return a.is_private or a.is_loopback or a.is_link_local or (a.version == 4 and a in ipaddress.ip_network('100.64.0.0/10'))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--env', default='/etc/qjanus/qjanus.env')
    ap.add_argument('--ws', default='ws://127.0.0.1:8188')
    ap.add_argument('--http', help='server API base (default: http://<QJANUS_HTTP_BIND>:8088/janus)')
    ap.add_argument('--iface', help='the enforced interface (default: QJANUS_ICE_ENFORCE_IFACE of the env file)')
    ap.add_argument('--cert', default='/etc/qjanus/dtls.crt')
    args = ap.parse_args()

    env = read_env(args.env)
    secret, admin_key = env.get('QJANUS_TOKEN_SECRET'), env.get('QJANUS_ADMIN_KEY')
    iface = args.iface or env.get('QJANUS_ICE_ENFORCE_IFACE')
    nat = env.get('QJANUS_NAT_1_1')
    if not (secret and admin_key):
        die(f'{args.env} has no QJANUS_TOKEN_SECRET / QJANUS_ADMIN_KEY (run as root on the node)')
    if not iface:
        die('no enforced interface (QJANUS_ICE_ENFORCE_IFACE)')
    bind = env.get('QJANUS_HTTP_BIND', '127.0.0.1')
    bind = '127.0.0.1' if bind in ('0.0.0.0', '::', '') else bind
    base = args.http or f'http://{bind}:8088/janus'
    token = mint_token(secret)
    api = Http(base, token, admin_key)

    info = api.get('/info')
    print(f'node: qjanus (janus {info.get("version_string")}), client API {args.ws.split("?")[0]}, enforced interface {iface}')

    addrs = host_addresses()
    own = addrs.get(iface, [])
    if not own:
        die(f'interface {iface} carries no address')
    others = {n: a for n, a in addrs.items() if n != iface}
    print(f'interface {iface}: {len(own)} addresses ({", ".join(mask(a) for a in own)}); other interfaces: ' +
          (', '.join(f'{n} ({len(a)})' for n, a in sorted(others.items())) or 'none'))
    foreign_private = [(n, a) for n, al in others.items() for a in al if is_private(a) and not a.is_loopback]
    print(f'addresses of other interfaces that must never be offered: {sum(len(a) for a in others.values())} '
          f'(of which private/VPN/link-local: {len(foreign_private)})')

    room, room_secret = secrets.token_hex(16), secrets.token_hex(16)
    pseudonym = secrets.token_hex(16)
    join_token = f'{pseudonym}:{secrets.token_hex(16)}'
    sid = handle = None
    janus = None
    try:
        sid = api.post('', {'janus': 'create'})['data']['id']
        handle = api.post(f'/{sid}', {'janus': 'attach', 'plugin': PLUGIN})['data']['id']
        created = plugin_data(api.post(f'/{sid}/{handle}', {'janus': 'message', 'body': {
            'request': 'create', 'room': room, 'secret': room_secret, 'publishers': 2, 'is_private': True, 'bitrate': 1500000,
            'audiocodec': 'opus', 'videocodec': 'vp8', 'require_e2ee': True, 'require_pvtid': True, 'record': False, 'lock_record': True,
            'transport_wide_cc_ext': True, 'audiolevel_ext': False, 'allowed': [join_token], 'admin_key': admin_key}}))
        if created.get('videoroom') != 'created':
            die(f'room creation failed: {created.get("error_code")} {created.get("error")}')

        janus = Janus(Ws(args.ws), token)
        janus.sid = janus.request({'janus': 'create'})['data']['id']
        h = janus.request({'janus': 'attach', 'session_id': janus.sid, 'plugin': PLUGIN})['data']['id']
        joined = plugin_data(janus.request({'janus': 'message', 'session_id': janus.sid, 'handle_id': h, 'body': {
            'request': 'join', 'ptype': 'publisher', 'room': room, 'id': pseudonym, 'display': pseudonym, 'token': join_token}}))
        if joined.get('videoroom') != 'joined':
            die(f'join refused: {joined.get("error_code")} {joined.get("error")}')
        pub = janus.request({'janus': 'message', 'session_id': janus.sid, 'handle_id': h,
                             'body': {'request': 'publish', 'audio': True, 'video': True},
                             'jsep': {'type': 'offer', 'sdp': synthetic_offer(), 'e2ee': True}})
        jsep = pub.get('jsep')
        if not jsep:
            die(f'publish gave no answer: {plugin_data(pub).get("error_code")} {plugin_data(pub).get("error")}')
        sdp = jsep['sdp']
    finally:
        if janus is not None:
            try:
                janus.request({'janus': 'destroy', 'session_id': janus.sid}, timeout=5)
            except BaseException:
                pass
            janus.ws.close()
        if sid is not None:
            try:
                if handle is not None:
                    api.post(f'/{sid}/{handle}', {'janus': 'message', 'body': {'request': 'destroy', 'room': room, 'secret': room_secret}})
            except BaseException:
                pass
            try:
                api.post(f'/{sid}', {'janus': 'destroy'})
            except BaseException:
                pass

    cands = re.findall(r'^a=candidate:\S+ \d+ (\S+) \d+ (\S+) \d+ typ (\S+)', sdp, re.M | re.I)
    if not cands:
        die('the answer carries no ICE candidate (nothing to check)')
    print(f'candidates in the answer: {len(cands)}: ' + ', '.join(f'{t}/{p.lower()}/{mask(a)}' for p, a, t in cands))
    problems = []
    offered = []
    for proto, addr, typ in cands:
        a = ipaddress.ip_address(addr.split('%')[0])
        offered.append(a)
        if typ != 'host' or proto.lower() != 'udp':
            problems.append(f'unexpected candidate kind {typ}/{proto}')
        if a not in own and str(a) != nat:
            owner = next((n for n, al in others.items() if a in al), None)
            problems.append(f'{mask(a)} is not an address of {iface}' + (f' (it belongs to {owner})' if owner else '') + (' and is private' if is_private(a) else ''))
    if not any(a in own for a in offered) and not nat:
        problems.append(f'no address of {iface} is offered at all')
    leaked = [a for a in offered if any(a in al for al in others.values())]
    fp = re.search(r'a=fingerprint:(\S+) ([0-9A-F:]+)', sdp, re.I)
    if args.cert and os.path.exists(args.cert):
        want = subprocess.run(['openssl', 'x509', '-in', args.cert, '-noout', '-fingerprint', '-sha256'], capture_output=True, text=True).stdout.strip().split('=', 1)[-1].lower()
        print(f'DTLS fingerprint in the answer equals the node certificate: {"yes" if fp and fp.group(2).lower() == want else "NO"}')
        if not fp or fp.group(2).lower() != want:
            problems.append('the fingerprint in the answer is not the pinned certificate')
    if problems or leaked:
        for p in problems:
            print(f'FAIL  {p}')
        die('the ICE candidates are not restricted to the enforced interface')
    print(f'PASS  every candidate is an address of {iface}; none of the {sum(len(a) for a in others.values())} addresses of '
          f'{len(others)} other interfaces is offered; no foreign private address')


if __name__ == '__main__':
    main()
