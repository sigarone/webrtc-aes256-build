// A synthetic, Chrome-shaped publisher offer for the API conformance tests (no peer behind it: Janus
// answers, ICE never connects). The RTP header extensions are those the apps keep after the munging of
// spec section 4.4 (mid, transport-wide-cc) plus, on request, the ones that must NOT survive.
export const FAKE_FP = `sha-256 ${Array.from({ length: 32 }, (_, i) => ((i * 7 + 3) & 255).toString(16).padStart(2, '0').toUpperCase()).join(':')}`;

export function syntheticOffer({ fingerprint = FAKE_FP, withBadExtmaps = false } = {}) {
  const common = (mid, kind) => [
    'c=IN IP4 0.0.0.0',
    'a=rtcp:9 IN IP4 0.0.0.0',
    'a=ice-ufrag:qjt1',
    'a=ice-pwd:qjtestqjtestqjtestqjtest',
    'a=ice-options:trickle',
    `a=fingerprint:${fingerprint}`,
    'a=setup:actpass',
    `a=mid:${mid}`,
  ].concat(kind === 'audio' ? [
    ...(withBadExtmaps ? ['a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level'] : []),
    'a=extmap:2 urn:ietf:params:rtp-hdrext:sdes:mid',
    'a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01',
  ] : [
    ...(withBadExtmaps ? ['a=extmap:5 urn:3gpp:video-orientation', 'a=extmap:6 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay'] : []),
    'a=extmap:2 urn:ietf:params:rtp-hdrext:sdes:mid',
    'a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01',
  ]);
  const lines = [
    'v=0',
    'o=- 4611731400430051336 2 IN IP4 127.0.0.1',
    's=-',
    't=0 0',
    'a=group:BUNDLE 0 1',
    'a=extmap-allow-mixed',
    'a=msid-semantic: WMS qjtest',
    'm=audio 9 UDP/TLS/RTP/SAVPF 111',
    ...common(0, 'audio'),
    'a=sendonly',
    'a=msid:qjtest a0',
    'a=rtcp-mux',
    'a=rtpmap:111 opus/48000/2',
    'a=rtcp-fb:111 transport-cc',
    'a=fmtp:111 minptime=60;useinbandfec=1;usedtx=0;cbr=1;stereo=0;maxaveragebitrate=32000',
    'a=ptime:60',
    'a=ssrc:1001 cname:qjtest',
    'm=video 9 UDP/TLS/RTP/SAVPF 96',
    ...common(1, 'video'),
    'a=sendonly',
    'a=msid:qjtest v0',
    'a=rtcp-mux',
    'a=rtcp-rsize',
    'a=rtpmap:96 VP8/90000',
    'a=rtcp-fb:96 goog-remb',
    'a=rtcp-fb:96 transport-cc',
    'a=rtcp-fb:96 ccm fir',
    'a=rtcp-fb:96 nack',
    'a=rtcp-fb:96 nack pli',
    'a=ssrc:2001 cname:qjtest',
  ];
  return lines.join('\r\n') + '\r\n';
}
