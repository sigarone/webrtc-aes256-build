import { test } from 'node:test';
import assert from 'node:assert/strict';
import {
  ALLOWED_EXTMAPS, applyOpusProfile, fingerprintMatches, getFingerprints, getMLines, getMids,
  getOpusFmtp, getPtime, getSendRids, mungeSdp, normalizeFingerprint, stripAudioRed, stripExtmaps,
} from '../page/sdp.mjs';

const FP = 'sha-256 98:3A:A2:B8:DE:0B:66:D3:A4:21:F9:10:98:D3:59:9C:06:30:83:D2:52:5C:3B:F3:06:25:13:13:7F:AF:8A:D4';

// A Chrome 153 publisher offer (audio + 3-layer simulcast VP8), trimmed to a few video codecs.
const CHROME_OFFER = [
  'v=0',
  'o=- 4598723965279998637 2 IN IP4 127.0.0.1',
  's=-',
  't=0 0',
  'a=group:BUNDLE 0 1',
  'a=extmap-allow-mixed',
  'a=msid-semantic: WMS 72e73248-7787-4840-b9c5-6aa092764e2b',
  'm=audio 9 UDP/TLS/RTP/SAVPF 111 63 9 0 8 13 110 126',
  'c=IN IP4 0.0.0.0',
  'a=rtcp:9 IN IP4 0.0.0.0',
  'a=ice-ufrag:V0EI',
  'a=ice-pwd:qCe5BDYzvxJ+zVb8UxUvxMoZ',
  'a=ice-options:trickle',
  `a=fingerprint:${FP}`,
  'a=setup:actpass',
  'a=mid:0',
  'a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level',
  'a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time',
  'a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01',
  'a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid',
  'a=extmap:15 http://www.webrtc.org/experiments/rtp-hdrext/abs-capture-time',
  'a=sendonly',
  'a=msid:72e73248-7787-4840-b9c5-6aa092764e2b fa090166-6696-41a6-a6d2-d46c7b8d4155',
  'a=rtcp-mux',
  'a=rtcp-rsize',
  'a=rtcp-xr:rcvr-rtt=all',
  'a=rtpmap:111 opus/48000/2',
  'a=rtcp-fb:111 transport-cc',
  'a=fmtp:111 minptime=10;useinbandfec=1',
  'a=rtpmap:63 red/48000/2',
  'a=fmtp:63 111/111',
  'a=rtpmap:9 G722/8000',
  'a=rtpmap:0 PCMU/8000',
  'a=rtpmap:8 PCMA/8000',
  'a=rtpmap:13 CN/8000',
  'a=rtpmap:110 telephone-event/48000',
  'a=rtpmap:126 telephone-event/8000',
  'a=ssrc:3841247884 cname:tn3kPyX+njvlMrR7',
  'a=ssrc:3841247884 msid:72e73248-7787-4840-b9c5-6aa092764e2b fa090166-6696-41a6-a6d2-d46c7b8d4155',
  'm=video 9 UDP/TLS/RTP/SAVPF 96 97 102 103 118 119 120',
  'c=IN IP4 0.0.0.0',
  'a=rtcp:9 IN IP4 0.0.0.0',
  'a=ice-ufrag:V0EI',
  'a=ice-pwd:qCe5BDYzvxJ+zVb8UxUvxMoZ',
  'a=ice-options:trickle',
  `a=fingerprint:${FP}`,
  'a=setup:actpass',
  'a=mid:1',
  'a=extmap:14 urn:ietf:params:rtp-hdrext:toffset',
  'a=extmap:2 http://www.webrtc.org/experiments/rtp-hdrext/abs-send-time',
  'a=extmap:13 urn:3gpp:video-orientation',
  'a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01',
  'a=extmap:5 http://www.webrtc.org/experiments/rtp-hdrext/playout-delay',
  'a=extmap:6 http://www.webrtc.org/experiments/rtp-hdrext/video-content-type',
  'a=extmap:7 http://www.webrtc.org/experiments/rtp-hdrext/video-timing',
  'a=extmap:8 http://www.webrtc.org/experiments/rtp-hdrext/color-space',
  'a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid',
  'a=extmap:10 urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id',
  'a=extmap:11 urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id',
  'a=extmap:16 http://www.webrtc.org/experiments/rtp-hdrext/abs-capture-time',
  'a=extmap:12 https://aomediacodec.github.io/av1-rtp-spec/#dependency-descriptor-rtp-header-extension',
  'a=extmap:9 http://www.webrtc.org/experiments/rtp-hdrext/video-layers-allocation00',
  'a=sendonly',
  'a=msid:72e73248-7787-4840-b9c5-6aa092764e2b 4623823e-ef0e-4d7d-b594-4b6a345057a5',
  'a=rtcp-mux',
  'a=rtcp-rsize',
  'a=rtpmap:96 VP8/90000',
  'a=rtcp-fb:96 goog-remb',
  'a=rtcp-fb:96 transport-cc',
  'a=rtcp-fb:96 nack',
  'a=rtcp-fb:96 nack pli',
  'a=rtpmap:97 rtx/90000',
  'a=fmtp:97 apt=96',
  'a=rtpmap:102 H264/90000',
  'a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f',
  'a=rtpmap:103 rtx/90000',
  'a=fmtp:103 apt=102',
  'a=rtpmap:118 red/90000',
  'a=rtpmap:119 rtx/90000',
  'a=fmtp:119 apt=118',
  'a=rtpmap:120 ulpfec/90000',
  'a=rid:h send',
  'a=rid:m send',
  'a=rid:l send',
  'a=simulcast:send h;m;l',
  '',
].join('\r\n');

// A Janus-style answer to the publisher (Opus without our parameters, extmaps it accepted).
const JANUS_ANSWER = [
  'v=0',
  'o=- 1 1 IN IP4 127.0.0.1',
  's=VideoRoom',
  't=0 0',
  'a=group:BUNDLE 0 1',
  'm=audio 9 UDP/TLS/RTP/SAVPF 111',
  'c=IN IP4 203.0.113.9',
  'a=mid:0',
  'a=recvonly',
  'a=rtcp-mux',
  'a=ice-ufrag:abcd',
  'a=ice-pwd:0123456789abcdef012345',
  `a=fingerprint:${FP}`,
  'a=setup:active',
  'a=rtpmap:111 opus/48000/2',
  'a=extmap:3 http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01',
  'a=extmap:4 urn:ietf:params:rtp-hdrext:sdes:mid',
  'a=extmap:1 urn:ietf:params:rtp-hdrext:ssrc-audio-level',
  'a=fmtp:111 useinbandfec=1',
  'm=video 9 UDP/TLS/RTP/SAVPF 96',
  'c=IN IP4 203.0.113.9',
  'a=mid:1',
  'a=recvonly',
  'a=rtcp-mux',
  'a=rtpmap:96 VP8/90000',
  'a=rtcp-fb:96 nack',
  'a=extmap:13 urn:3gpp:video-orientation',
  '',
].join('\r\n');

const lines = (sdp) => sdp.split('\r\n');
const OPUS = { ptime: 60, fec: true, dtx: false, cbr: true, maxaveragebitrate: 32000 };
const WANT_FMTP = 'minptime=60;useinbandfec=1;usedtx=0;cbr=1;stereo=0;maxaveragebitrate=32000';

const EXTMAP_URIS = (sdp) => sdp.split('\r\n').filter((l) => l.startsWith('a=extmap:')).map((l) => l.split(' ')[1]);

test('stripExtmaps is an allow-list: only mid, rid, repaired-rid and transport-wide-cc remain', () => {
  const out = stripExtmaps(CHROME_OFFER);
  assert.equal(ALLOWED_EXTMAPS.length, 4);
  // every removed extension of the fixture is really gone, in both m-sections
  for (const gone of ['abs-send-time', 'toffset', 'playout-delay', 'video-content-type', 'video-timing', 'color-space',
    'ssrc-audio-level', 'abs-capture-time', 'video-orientation', 'dependency-descriptor', 'video-layers-allocation']) {
    assert.ok(!out.includes(gone), `${gone} must be removed`);
  }
  // the four allowed ones stay (audio: mid + twcc; video: mid + twcc + rid + repaired-rid)
  assert.deepEqual(EXTMAP_URIS(out).sort(), [
    'http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01',
    'http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01',
    'urn:ietf:params:rtp-hdrext:sdes:mid',
    'urn:ietf:params:rtp-hdrext:sdes:mid',
    'urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id',
    'urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id',
  ]);
  for (const uri of EXTMAP_URIS(out)) assert.ok(ALLOWED_EXTMAPS.includes(uri), uri);
  assert.ok(out.includes('a=extmap-allow-mixed'), 'a=extmap-allow-mixed is not an extmap line and stays');
  // nothing but extmap lines was removed: 2 audio + 10 video extmaps out of 4 audio + 15 video
  const removed = lines(CHROME_OFFER).length - lines(out).length;
  assert.equal(removed, EXTMAP_URIS(CHROME_OFFER).length - 6);
});

test('stripExtmaps handles direction suffixes, keeps allowed ones and CRLF', () => {
  const sdp = 'v=0\r\nm=audio 9 RTP/AVP 0\r\na=extmap:1/sendonly urn:ietf:params:rtp-hdrext:ssrc-audio-level\r\n'
    + 'a=extmap:2/recvonly urn:ietf:params:rtp-hdrext:sdes:mid\r\na=extmap:3 urn:x\r\n';
  assert.equal(stripExtmaps(sdp), 'v=0\r\nm=audio 9 RTP/AVP 0\r\na=extmap:2/recvonly urn:ietf:params:rtp-hdrext:sdes:mid\r\n');
  assert.ok(!/[^\r]\n/.test(stripExtmaps(sdp)));
});

test('applyOpusProfile: exact fmtp on Opus only, one ptime, rtcp-fb kept, other codecs untouched', () => {
  const out = applyOpusProfile(CHROME_OFFER, OPUS);
  const l = lines(out);
  const fmtp111 = l.filter((x) => x.startsWith('a=fmtp:111 '));
  assert.deepEqual(fmtp111, [`a=fmtp:111 ${WANT_FMTP}`]);
  assert.equal(l.filter((x) => x.startsWith('a=ptime:')).length, 1);
  assert.ok(l.includes('a=ptime:60'));
  // ptime belongs to the audio section
  const audioEnd = l.findIndex((x) => x.startsWith('m=video'));
  assert.ok(l.indexOf('a=ptime:60') < audioEnd);
  // rtcp-fb of the Opus payload still directly follows the rtpmap, fmtp follows them
  const i = l.indexOf('a=rtpmap:111 opus/48000/2');
  assert.equal(l[i + 1], 'a=rtcp-fb:111 transport-cc');
  assert.equal(l[i + 2], `a=fmtp:111 ${WANT_FMTP}`);
  // untouched lines
  assert.ok(l.includes('a=fmtp:63 111/111'));
  assert.ok(l.includes('a=fmtp:102 level-asymmetry-allowed=1;packetization-mode=1;profile-level-id=42001f'));
  assert.ok(l.includes('a=rtpmap:9 G722/8000'));
  // the video section is byte-identical
  assert.equal(out.slice(out.indexOf('m=video')), CHROME_OFFER.slice(CHROME_OFFER.indexOf('m=video')));
});

test('applyOpusProfile parameters: fec/dtx/cbr flags and idempotence', () => {
  const out = applyOpusProfile(CHROME_OFFER, { ptime: 20, fec: false, dtx: true, cbr: false, maxaveragebitrate: 24000 });
  assert.deepEqual(getOpusFmtp(out), {
    minptime: '20', useinbandfec: '0', usedtx: '1', cbr: '0', stereo: '0', maxaveragebitrate: '24000',
  });
  assert.equal(getPtime(out), 20);
  const twice = applyOpusProfile(applyOpusProfile(CHROME_OFFER, OPUS), OPUS);
  assert.equal(twice, applyOpusProfile(CHROME_OFFER, OPUS));
});

test('applyOpusProfile replaces an existing ptime and stereo/other Opus params (Janus answer)', () => {
  const answer = JANUS_ANSWER.replace('a=recvonly\r\na=rtcp-mux\r\na=ice-ufrag', 'a=ptime:20\r\na=recvonly\r\na=rtcp-mux\r\na=ice-ufrag')
    .replace('a=fmtp:111 useinbandfec=1', 'a=fmtp:111 useinbandfec=1;stereo=1;sprop-stereo=1');
  const out = applyOpusProfile(answer, OPUS);
  assert.deepEqual(getOpusFmtp(out), {
    minptime: '60', useinbandfec: '1', usedtx: '0', cbr: '1', stereo: '0', maxaveragebitrate: '32000',
  });
  assert.equal(lines(out).filter((x) => x.startsWith('a=ptime:')).length, 1);
  assert.equal(getPtime(out), 60);
});

test('applyOpusProfile adds fmtp when the Opus payload has none, and skips audio without Opus', () => {
  const noFmtp = CHROME_OFFER.replace('a=fmtp:111 minptime=10;useinbandfec=1\r\n', '');
  const out = applyOpusProfile(noFmtp, OPUS);
  assert.deepEqual(lines(out).filter((x) => x.startsWith('a=fmtp:111 ')), [`a=fmtp:111 ${WANT_FMTP}`]);
  const pcmu = 'v=0\r\nm=audio 9 RTP/AVP 0\r\na=mid:0\r\na=rtpmap:0 PCMU/8000\r\n';
  assert.equal(applyOpusProfile(pcmu, OPUS), pcmu);
});

test('stripAudioRed removes the Opus RED payload and its lines from audio only', () => {
  const out = stripAudioRed(CHROME_OFFER);
  const audio = out.slice(out.indexOf('m=audio'), out.indexOf('m=video'));
  assert.ok(!/red\/48000/.test(audio));
  assert.ok(!audio.includes('a=fmtp:63'));
  assert.ok(lines(out).includes('m=audio 9 UDP/TLS/RTP/SAVPF 111 9 0 8 13 110 126'));
  // video RED is a different codec and stays
  assert.ok(out.includes('a=rtpmap:118 red/90000'));
});

test('mungeSdp on a Chrome offer: extmap allow-list + Opus + no RED, simulcast rids intact, CRLF, m-line order', () => {
  const out = mungeSdp(CHROME_OFFER, { opus: OPUS });
  for (const uri of EXTMAP_URIS(out)) assert.ok(ALLOWED_EXTMAPS.includes(uri), uri);
  assert.ok(EXTMAP_URIS(out).includes('urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id'));
  assert.equal(getPtime(out), 60);
  assert.deepEqual(getSendRids(out), ['h', 'm', 'l']);
  assert.ok(out.includes('a=simulcast:send h;m;l'));
  assert.deepEqual(getMLines(out).map((m) => [m.kind, m.mid, m.direction]), [['audio', '0', 'sendonly'], ['video', '1', 'sendonly']]);
  assert.ok(out.endsWith('\r\n'));
  assert.ok(!/[^\r]\n/.test(out));
  assert.ok(!/red\/48000/.test(out));
});

test('mungeSdp on the Janus answer (remote description) forces the 60 ms profile', () => {
  const out = mungeSdp(JANUS_ANSWER, { opus: OPUS });
  assert.deepEqual(getOpusFmtp(out), {
    minptime: '60', useinbandfec: '1', usedtx: '0', cbr: '1', stereo: '0', maxaveragebitrate: '32000',
  });
  assert.equal(getPtime(out), 60);
  assert.ok(!out.includes('ssrc-audio-level') && !out.includes('video-orientation'));
  assert.ok(out.includes('sdes:mid') && out.includes('transport-wide-cc'));
});

test('mungeSdp without opus options only strips extmaps (subscriber answer)', () => {
  const out = mungeSdp(CHROME_OFFER);
  assert.equal(out, stripExtmaps(CHROME_OFFER));
  assert.equal(getPtime(out), null);
});

test('bare-LF input is normalised to CRLF', () => {
  const out = mungeSdp(CHROME_OFFER.replace(/\r\n/g, '\n'), { opus: OPUS });
  assert.ok(!/[^\r]\n/.test(out));
  assert.equal(out, mungeSdp(CHROME_OFFER, { opus: OPUS }));
});

test('readers: fingerprints, mids, m-lines, rids', () => {
  assert.deepEqual(getFingerprints(CHROME_OFFER), [FP]);
  assert.deepEqual(getMids(CHROME_OFFER), ['0', '1']);
  const ml = getMLines(CHROME_OFFER);
  assert.equal(ml.length, 2);
  assert.equal(ml[0].proto, 'UDP/TLS/RTP/SAVPF');
  assert.deepEqual(getSendRids(CHROME_OFFER), ['h', 'm', 'l']);
  assert.deepEqual(getSendRids(JANUS_ANSWER), []);
  assert.deepEqual(getOpusFmtp(CHROME_OFFER), { minptime: '10', useinbandfec: '1' });
  assert.equal(getPtime(CHROME_OFFER), null);
});

test('fingerprint pin: case-insensitive, all fingerprints must match, no fingerprint fails', () => {
  assert.equal(normalizeFingerprint('SHA-256 ab:cd:ef'), 'sha-256 AB:CD:EF');
  assert.equal(fingerprintMatches(CHROME_OFFER, FP), true);
  assert.equal(fingerprintMatches(CHROME_OFFER, FP.toLowerCase()), true);
  assert.equal(fingerprintMatches(CHROME_OFFER, FP.replace('98:3A', '99:3A')), false);
  assert.equal(fingerprintMatches('v=0\r\nm=audio 9 RTP/AVP 0\r\n', FP), false);
  const mixed = CHROME_OFFER.replace('a=setup:actpass\r\na=mid:1', 'a=setup:actpass\r\na=fingerprint:sha-256 00:11\r\na=mid:1');
  assert.equal(fingerprintMatches(mixed, FP), false);
});
