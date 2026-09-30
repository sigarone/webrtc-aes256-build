// Pure SDP string helpers (no DOM, no state): extmap stripping, Opus fmtp/ptime munging and
// small readers (fingerprints, mids, m-lines, simulcast rids).
//
// Why the SDP is munged at all (spec section 4.4 / 10):
//  - Cryptex (RFC 9335) is not supported by Janus 1.4.2, so RTP header extensions travel in the
//    clear on the client<->SFU hop. Only an allow-list of extensions (mid, rid, repaired-rid,
//    transport-wide-cc) is therefore kept in what we offer and answer.
//  - The Opus profile of the 1:1 call (60 ms, 32 kbps CBR, FEC on, no DTX, mono) is forced with
//    fmtp + a=ptime. libwebrtc derives the SEND-side Opus ptime/bitrate/cbr from the REMOTE
//    description (the receiver's preferences), not from the local one. Munging only our offer would
//    leave the bot sending 20 ms packets, so the same munge is applied to Janus' answer before
//    setRemoteDescription().
//
// All functions keep CRLF line endings and never reorder m-sections.

/**
 * The ONLY RTP header extensions allowed to remain in any description (spec section 10): mid,
 * rid, repaired-rid and transport-wide-cc. Every other a=extmap line is removed.
 */
export const ALLOWED_EXTMAPS = Object.freeze([
  'urn:ietf:params:rtp-hdrext:sdes:mid',
  'urn:ietf:params:rtp-hdrext:sdes:rtp-stream-id',
  'urn:ietf:params:rtp-hdrext:sdes:repaired-rtp-stream-id',
  'http://www.ietf.org/id/draft-holmer-rmcat-transport-wide-cc-extensions-01',
]);

const CRLF = '\r\n';

/** Split into non-empty lines, accepting CRLF or bare LF input. */
function toLines(sdp) {
  return String(sdp).split(/\r?\n/).filter((l) => l.length > 0);
}

function fromLines(lines) {
  return lines.join(CRLF) + CRLF;
}

/** Index of every m-section start, plus the end (exclusive) of each. */
function sectionBounds(lines) {
  const starts = [];
  lines.forEach((l, i) => { if (l.startsWith('m=')) starts.push(i); });
  return starts.map((s, k) => ({ start: s, end: k + 1 < starts.length ? starts[k + 1] : lines.length }));
}

/** Keep only the a=extmap lines whose URI is in `allowed` (session and media level); a=extmap-allow-mixed is not an extmap line. */
export function stripExtmaps(sdp, allowed = ALLOWED_EXTMAPS) {
  const keep = new Set(allowed);
  const out = toLines(sdp).filter((l) => {
    const m = /^a=extmap:\d+(?:\/\w+)?\s+(\S+)/.exec(l);
    return !m || keep.has(m[1]);
  });
  return fromLines(out);
}

/**
 * Force the Opus fmtp line and a=ptime in every audio m-section.
 * fmtp: minptime=<ptime>;useinbandfec=<0|1>;usedtx=<0|1>;cbr=<0|1>;stereo=0;maxaveragebitrate=<bps>
 * Only Opus payload types are touched; rtcp-fb, rtpmap and other codecs stay as they are.
 */
export function applyOpusProfile(sdp, { ptime = 60, fec = true, dtx = false, cbr = true, maxaveragebitrate = 32000 } = {}) {
  const fmtp = `minptime=${ptime};useinbandfec=${fec ? 1 : 0};usedtx=${dtx ? 1 : 0};cbr=${cbr ? 1 : 0};stereo=0;maxaveragebitrate=${maxaveragebitrate}`;
  const lines = toLines(sdp);
  const out = [];
  const bounds = sectionBounds(lines);
  out.push(...lines.slice(0, bounds.length ? bounds[0].start : lines.length));
  for (const b of bounds) {
    const sec = lines.slice(b.start, b.end);
    out.push(...(sec[0].startsWith('m=audio') ? mungeAudioSection(sec, fmtp, ptime) : sec));
  }
  return fromLines(out);
}

function mungeAudioSection(sec, fmtp, ptime) {
  const opusPts = new Set();
  for (const l of sec) {
    const m = /^a=rtpmap:(\d+)\s+opus\/48000/i.exec(l);
    if (m) opusPts.add(m[1]);
  }
  if (opusPts.size === 0) return sec;
  // Drop any existing fmtp of the Opus payload types and any existing ptime, then re-insert both
  // (no duplicates possible).
  const kept = sec.filter((l) => {
    const m = /^a=fmtp:(\d+)\s/.exec(l);
    if (m && opusPts.has(m[1])) return false;
    return !/^a=ptime:/.test(l);
  });
  // The codec block (rtpmap / rtcp-fb / fmtp lines) stays contiguous: the new fmtp goes after the
  // rtpmap and its rtcp-fb lines, ptime after the last codec line.
  let lastCodecLine = -1;
  kept.forEach((l, i) => { if (/^a=(rtpmap|rtcp-fb|fmtp):/.test(l)) lastCodecLine = i; });
  const res = [];
  kept.forEach((l, i) => {
    res.push(l);
    const m = /^a=(?:rtpmap|rtcp-fb):(\d+)\s/.exec(l);
    if (m && opusPts.has(m[1])) {
      const nm = /^a=rtcp-fb:(\d+)\s/.exec(kept[i + 1] || '');
      if (!(nm && nm[1] === m[1])) res.push(`a=fmtp:${m[1]} ${fmtp}`);
    }
    if (i === lastCodecLine) res.push(`a=ptime:${ptime}`);
  });
  return res;
}

/**
 * Remove the audio RED payload (`red/48000`) from every audio m-section: the 1:1 audio profile
 * has no RED. Chrome offers Opus RED by default; Janus would not accept it, but the offer should
 * not even carry it. Video RED (`red/90000`) is a different codec and is left alone.
 */
export function stripAudioRed(sdp) {
  const lines = toLines(sdp);
  const out = [];
  const bounds = sectionBounds(lines);
  out.push(...lines.slice(0, bounds.length ? bounds[0].start : lines.length));
  for (const b of bounds) {
    let sec = lines.slice(b.start, b.end);
    if (sec[0].startsWith('m=audio')) {
      const redPts = new Set();
      for (const l of sec) {
        const m = /^a=rtpmap:(\d+)\s+red\/48000/i.exec(l);
        if (m) redPts.add(m[1]);
      }
      if (redPts.size) {
        const head = sec[0].split(' ');
        sec = [[...head.slice(0, 3), ...head.slice(3).filter((pt) => !redPts.has(pt))].join(' '),
          ...sec.slice(1).filter((l) => {
            const m = /^a=(?:rtpmap|fmtp|rtcp-fb):(\d+)\s/.exec(l);
            return !(m && redPts.has(m[1]));
          })];
      }
    }
    out.push(...sec);
  }
  return fromLines(out);
}

/**
 * The munge applied to every description we produce or receive on the publisher PC: extmap
 * allow-list, Opus profile and no audio RED. The subscriber answer only gets the extmap part.
 */
export function mungeSdp(sdp, { allowedExtmaps = ALLOWED_EXTMAPS, opus = null } = {}) {
  let out = stripExtmaps(sdp, allowedExtmaps);
  if (opus) out = applyOpusProfile(stripAudioRed(out), opus);
  return out;
}

// ------------------------------------------------------------------------ readers

/** [{index, kind, mid, port, proto, direction}] for every m-section. */
export function getMLines(sdp) {
  const lines = toLines(sdp);
  return sectionBounds(lines).map((b, index) => {
    const head = lines[b.start].slice(2).split(' ');
    let mid = null;
    let direction = null;
    for (const l of lines.slice(b.start + 1, b.end)) {
      const mm = /^a=mid:(\S+)/.exec(l);
      if (mm) mid = mm[1];
      const dm = /^a=(sendrecv|sendonly|recvonly|inactive)$/.exec(l);
      if (dm) direction = dm[1];
    }
    return { index, kind: head[0], port: Number(head[1]), proto: head[2] || '', mid, direction };
  });
}

/** Mids in m-line order (null entries are kept out). */
export function getMids(sdp) {
  return getMLines(sdp).map((m) => m.mid).filter((m) => m !== null);
}

/** Normalised `a=fingerprint` values ("sha-256 AB:CD:..."), session and media level, deduplicated. */
export function getFingerprints(sdp) {
  const found = [];
  for (const l of toLines(sdp)) {
    const m = /^a=fingerprint:(\S+)\s+(\S+)/.exec(l);
    if (m) found.push(normalizeFingerprint(`${m[1]} ${m[2]}`));
  }
  return [...new Set(found)];
}

/** "SHA-256 ab:cd" -> "sha-256 AB:CD" so pins compare regardless of case. */
export function normalizeFingerprint(fp) {
  const m = /^\s*(\S+)\s+(\S+)\s*$/.exec(String(fp));
  if (!m) return String(fp).trim();
  return `${m[1].toLowerCase()} ${m[2].toUpperCase()}`;
}

/** True when the SDP carries at least one fingerprint and every one equals the pin. */
export function fingerprintMatches(sdp, pin) {
  const fps = getFingerprints(sdp);
  const want = normalizeFingerprint(pin);
  return fps.length > 0 && fps.every((f) => f === want);
}

/** Rids of the first video m-section as offered for sending (a=rid:<id> send). */
export function getSendRids(sdp) {
  const lines = toLines(sdp);
  for (const b of sectionBounds(lines)) {
    if (!lines[b.start].startsWith('m=video')) continue;
    const rids = [];
    for (const l of lines.slice(b.start + 1, b.end)) {
      const m = /^a=rid:(\S+)\s+send/.exec(l);
      if (m) rids.push(m[1]);
    }
    return rids;
  }
  return [];
}

/** Opus fmtp parameters of the first audio m-section as a plain object (or null). */
export function getOpusFmtp(sdp) {
  const lines = toLines(sdp);
  for (const b of sectionBounds(lines)) {
    if (!lines[b.start].startsWith('m=audio')) continue;
    const sec = lines.slice(b.start, b.end);
    const pt = sec.map((l) => /^a=rtpmap:(\d+)\s+opus\/48000/i.exec(l)).find(Boolean);
    if (!pt) return null;
    const f = sec.map((l) => new RegExp(`^a=fmtp:${pt[1]}\\s+(.*)$`).exec(l)).find(Boolean);
    if (!f) return {};
    const o = {};
    for (const kv of f[1].split(';')) {
      const i = kv.indexOf('=');
      if (i > 0) o[kv.slice(0, i).trim()] = kv.slice(i + 1).trim();
    }
    return o;
  }
  return null;
}

/** a=ptime of the first audio m-section (number) or null. */
export function getPtime(sdp) {
  const lines = toLines(sdp);
  for (const b of sectionBounds(lines)) {
    if (!lines[b.start].startsWith('m=audio')) continue;
    for (const l of lines.slice(b.start, b.end)) {
      const m = /^a=ptime:(\d+)/.exec(l);
      if (m) return Number(m[1]);
    }
    return null;
  }
  return null;
}
