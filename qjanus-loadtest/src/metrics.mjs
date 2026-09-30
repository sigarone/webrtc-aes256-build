// Pure metric helpers: no I/O, no clocks. Everything the orchestrator and the
// report compute from bot statuses and samples lives here so it can be unit
// tested with hand-made data (see test/metrics.test.mjs).
//
// Conventions
// - Timestamps are epoch milliseconds, windows are half open [fromMs, toMs).
// - A sample is { id?, t, dtMs, d:{aIn,vIn,aOut,vOut}, g:{...} } exactly as
//   window.qbot.poll() returns it (plus the bot id added by the orchestrator).
// - Missing values are null, never NaN/undefined.

/** Client machine CPU above this (percent, all cores averaged) makes a step inconclusive. */
export const CLIENT_CPU_SATURATION_PCT = 90;

/** Slice length of the consecutive-breach rule for packet loss. */
export const LOSS_WINDOW_MS = 10_000;

/** Breach reasons in priority order: the first one becomes the stop reason. */
export const BREACH_PRIORITY = ['transport', 'join', 'loss', 'freeze', 'cpu'];

/** Counter fields of Sample.d, per direction/kind. */
export const DELTA_KEYS = {
  aIn: ['packets', 'lost', 'bytes', 'concealed', 'samples', 'nack', 'jbDelayMs', 'jbEmitted'],
  vIn: ['packets', 'lost', 'bytes', 'framesDecoded', 'framesDropped', 'freezeCount', 'freezeMs', 'pauseCount', 'nack', 'pli', 'fir'],
  aOut: ['packets', 'bytes', 'nack', 'retransmitted'],
  vOut: ['packets', 'bytes', 'framesEncoded', 'nack', 'pli', 'retransmitted'],
};

const isNum = (v) => typeof v === 'number' && Number.isFinite(v);
const numOr0 = (v) => (isNum(v) ? v : 0);

/** Round to `digits` decimals; null stays null. */
export function round(v, digits = 2) {
  if (!isNum(v)) return null;
  const f = 10 ** digits;
  return Math.round(v * f) / f;
}

/** Nearest-rank percentile (p in 0..100) of the finite numbers in `values`; null when there are none. */
export function percentile(values, p) {
  const v = values.filter(isNum).sort((a, b) => a - b);
  if (v.length === 0) return null;
  const rank = Math.min(v.length, Math.max(1, Math.ceil((p / 100) * v.length)));
  return v[rank - 1];
}

/** { n, min, max, avg, p50, p95 } of the finite numbers in `values` (all null when empty). */
export function summarize(values) {
  const v = values.filter(isNum);
  if (v.length === 0) return { n: 0, min: null, max: null, avg: null, p50: null, p95: null };
  const sum = v.reduce((a, b) => a + b, 0);
  return {
    n: v.length,
    min: Math.min(...v),
    max: Math.max(...v),
    avg: sum / v.length,
    p50: percentile(v, 50),
    p95: percentile(v, 95),
  };
}

/** lost / (lost + packets) in percent; null when no packet was seen at all. */
export function lossPct(lost, packets) {
  const total = numOr0(lost) + numOr0(packets);
  return total > 0 ? (100 * numOr0(lost)) / total : null;
}

/** Samples with fromMs <= t < toMs. */
export function samplesInWindow(samples, fromMs, toMs) {
  return samples.filter((s) => isNum(s.t) && s.t >= fromMs && s.t < toMs);
}

/** Sum every counter of Sample.d over `samples`; missing values count as 0. */
export function sumDeltas(samples) {
  const out = {};
  for (const [group, keys] of Object.entries(DELTA_KEYS)) {
    out[group] = Object.fromEntries(keys.map((k) => [k, 0]));
    for (const s of samples) {
      const src = s.d && s.d[group];
      if (!src) continue;
      for (const k of keys) out[group][k] += numOr0(src[k]);
    }
  }
  return out;
}

/** Maximum of every numeric gauge of Sample.g over `samples` (null when never reported). */
export function maxGauges(samples) {
  const out = {};
  for (const s of samples) {
    if (!s.g) continue;
    for (const [k, v] of Object.entries(s.g)) {
      if (!isNum(v)) continue;
      out[k] = out[k] === undefined ? v : Math.max(out[k], v);
    }
  }
  return out;
}

/**
 * Split [fromMs, toMs) into slices of sliceMs. A trailing remainder shorter
 * than half a slice is merged into the last slice; a window shorter than one
 * slice yields a single (shorter) slice. Empty windows yield [].
 */
export function sliceWindows(fromMs, toMs, sliceMs = LOSS_WINDOW_MS) {
  if (!(toMs > fromMs)) return [];
  const span = toMs - fromMs;
  const full = Math.floor(span / sliceMs);
  if (full === 0) return [{ fromMs, toMs }];
  const rest = span - full * sliceMs;
  const slices = [];
  for (let i = 0; i < full; i++) slices.push({ fromMs: fromMs + i * sliceMs, toMs: fromMs + (i + 1) * sliceMs });
  if (rest >= sliceMs / 2) slices.push({ fromMs: fromMs + full * sliceMs, toMs });
  else slices[slices.length - 1].toMs = toMs;
  return slices;
}

/** Subscriber-side loss (audio + video) per slice of the measurement window. */
export function lossWindows(samples, fromMs, toMs, sliceMs = LOSS_WINDOW_MS) {
  return sliceWindows(fromMs, toMs, sliceMs).map((w) => {
    const sums = sumDeltas(samplesInWindow(samples, w.fromMs, w.toMs));
    return {
      fromMs: w.fromMs,
      toMs: w.toMs,
      lossPct: lossPct(sums.aIn.lost + sums.vIn.lost, sums.aIn.packets + sums.vIn.packets),
    };
  });
}

/** Freeze events (vIn.freezeCount) per slice of the measurement window, same slicing as lossWindows. */
export function freezeWindows(samples, fromMs, toMs, sliceMs = LOSS_WINDOW_MS) {
  return sliceWindows(fromMs, toMs, sliceMs).map((w) => ({
    fromMs: w.fromMs,
    toMs: w.toMs,
    freezeCount: sumDeltas(samplesInWindow(samples, w.fromMs, w.toMs)).vIn.freezeCount,
  }));
}

/**
 * Largest value that is sustained for `n` consecutive entries:
 * max over i of min(values[i..i+n-1]). Null entries count as 0 (no traffic is
 * no evidence of loss). With fewer than n entries the whole series is used.
 * This is exactly the statistic behind "limit exceeded for n consecutive windows".
 */
export function sustainedMax(values, n) {
  if (values.length === 0) return null;
  const size = Math.max(1, Math.min(n, values.length));
  const v = values.map((x) => (isNum(x) ? x : 0));
  let best = 0;
  for (let i = 0; i + size <= v.length; i++) best = Math.max(best, Math.min(...v.slice(i, i + size)));
  return best;
}

/** Aggregate metrics over the measurement window (the `window` object of a step row). */
export function windowMetrics(samples, fromMs, toMs) {
  const w = samplesInWindow(samples, fromMs, toMs);
  const seconds = Math.max(0, (toMs - fromMs) / 1000);
  const empty = w.length === 0 || seconds === 0;
  const sums = sumDeltas(w);
  const bots = new Set(w.map((s) => s.id ?? '_')).size;
  const freezeBots = new Set(w.filter((s) => numOr0(s.d?.vIn?.freezeCount) > 0).map((s) => s.id ?? '_')).size;
  const pps = summarize(w.map((s) => s.g?.aOutPps));
  const rate = (total) => (empty ? null : total / seconds);
  const kbpsPerBot = (bytes) => (empty || bots === 0 ? null : (bytes * 8) / 1000 / seconds / bots);
  return {
    seconds: round(seconds, 1),
    lossPct: round(lossPct(sums.aIn.lost + sums.vIn.lost, sums.aIn.packets + sums.vIn.packets), 3),
    audioLossPct: round(lossPct(sums.aIn.lost, sums.aIn.packets), 3),
    videoLossPct: round(lossPct(sums.vIn.lost, sums.vIn.packets), 3),
    freezeEvents: empty ? null : sums.vIn.freezeCount,
    freezeBots: empty ? null : freezeBots,
    freezeWindows: freezeWindows(w, fromMs, toMs).map((s) => s.freezeCount),
    aJitterMsP95: round(percentile(w.map((s) => s.g?.aJitterMsMax), 95), 2),
    vJitterMsP95: round(percentile(w.map((s) => s.g?.vJitterMsMax), 95), 2),
    concealedPct: round(sums.aIn.samples > 0 ? (100 * sums.aIn.concealed) / sums.aIn.samples : null, 3),
    nackPerSec: round(rate(sums.aIn.nack + sums.vIn.nack), 2),
    pliPerSec: round(rate(sums.vIn.pli + sums.vIn.fir), 3),
    audioPubPps: { p50: round(pps.p50, 2), min: round(pps.min, 2), max: round(pps.max, 2) },
    rxKbpsPerBot: round(kbpsPerBot(sums.aIn.bytes + sums.vIn.bytes), 1),
    txKbpsPerBot: round(kbpsPerBot(sums.aOut.bytes + sums.vOut.bytes), 1),
    framesDecodedPerSec: round(rate(sums.vIn.framesDecoded), 2),
  };
}

// ---------------------------------------------------------------- stop checks
// Every check returns { value, limit, breached }; a missing measurement is
// value null and never a breach.

/** Loss: breached when the limit is exceeded (strictly) in `breachWindows` consecutive windows. */
export function evalLoss(windows, { limitPct, breachWindows }) {
  const value = sustainedMax(windows.map((w) => w.lossPct), breachWindows);
  return { value: round(value, 3), limit: limitPct, breached: value !== null && value > limitPct };
}

/**
 * Freeze: breached when more freeze events than `tolerance` (strictly) are seen
 * in `breachWindows` consecutive slices, like loss. One isolated slice (a
 * transient client hiccup) is not a breach. `value` is the sustained count.
 */
export function evalFreeze(windows, { tolerance, breachWindows }) {
  const value = sustainedMax(windows.map((w) => w.freezeCount), breachWindows);
  return { value, limit: tolerance, breached: value !== null && value > tolerance };
}

/** Janus CPU (percent of ONE core): breached when the maximum in the window exceeds the limit. */
export function evalCpu(maxCpuPct, limit) {
  return { value: round(maxCpuPct, 1), limit, breached: isNum(maxCpuPct) && maxCpuPct > limit };
}

/** Join: (failed + not steady) / attempted in percent against the limit. */
export function evalJoin({ failed, notSteady, attempted }, limitPct) {
  const value = attempted > 0 ? (100 * (failed + notSteady)) / attempted : null;
  return { value: round(value, 2), limit: limitPct, breached: value !== null && value > limitPct };
}

/** Transport policy: any violation is fatal (not a capacity result). */
export function evalTransport(violations) {
  return { value: violations, limit: 0, breached: violations > 0 };
}

/**
 * Combine the checks into breach reasons and a verdict. A transport
 * violation is always a plain 'breach'; any other breach becomes
 * 'inconclusive_client_saturated' when the client machine was saturated.
 */
export function stepVerdict(checks, { clientSaturated }) {
  const breachReasons = BREACH_PRIORITY.filter((r) => checks[r] && checks[r].breached);
  if (breachReasons.length === 0) return { breached: false, breachReasons, verdict: 'ok' };
  const saturated = clientSaturated && !breachReasons.includes('transport');
  return { breached: true, breachReasons, verdict: saturated ? 'inconclusive_client_saturated' : 'breach' };
}

// ----------------------------------------------------------------- client CPU

/** { avgPct, maxPct, saturated } from CPU readings (percent 0-100); nulls are ignored. */
export function clientCpuStats(readings, thresholdPct = CLIENT_CPU_SATURATION_PCT) {
  const s = summarize(readings);
  return { avgPct: round(s.avg, 1), maxPct: round(s.max, 1), saturated: s.max !== null && s.max > thresholdPct };
}

// ------------------------------------------------------------------- bot data

const TERMINAL = new Set(['failed', 'closed']);

/** True when the bot can no longer make progress. */
export function isTerminal(state) {
  return TERMINAL.has(state);
}

/**
 * Classify bots for the join check. `bots` = [{ state, allPeersMs }]:
 * failed/closed -> failed; steady within joinTimeoutMs -> ok; anything else
 * (still joining, or steady only after the timeout) -> notSteady.
 */
export function classifyBots(bots, joinTimeoutMs) {
  const out = { attempted: bots.length, ok: 0, failed: 0, notSteady: 0 };
  for (const b of bots) {
    if (isTerminal(b.state)) out.failed++;
    else if (b.state === 'steady' && (!isNum(b.allPeersMs) || b.allPeersMs <= joinTimeoutMs)) out.ok++;
    else out.notSteady++;
  }
  return out;
}

/** Join-time distributions over bot statuses (ms; ICE and DTLS pool publisher and subscriber PCs). */
export function joinDistributions(statuses) {
  const col = (f) => statuses.map((s) => s.timings && s.timings[f]).filter(isNum);
  const joined = col('joinedMs');
  const ice = [...col('pubIceMs'), ...col('subIceMs')];
  const dtls = [...col('pubDtlsMs'), ...col('subDtlsMs')];
  const all = col('allPeersMs');
  const max = joined.length ? Math.max(...joined) : null;
  return {
    p50Ms: percentile(joined, 50),
    p95Ms: percentile(joined, 95),
    maxMs: max,
    iceP50Ms: percentile(ice, 50),
    iceP95Ms: percentile(ice, 95),
    dtlsP50Ms: percentile(dtls, 50),
    dtlsP95Ms: percentile(dtls, 95),
    allPeersP50Ms: percentile(all, 50),
    allPeersP95Ms: percentile(all, 95),
  };
}

/** True when the bot reported a transport policy violation (spec 4.5). */
export function hasTransportViolation(status) {
  const t = status.transport || {};
  return (status.error && status.error.phase === 'transport') || (t.pub && t.pub.ok === false) || (t.sub && t.sub.ok === false) || false;
}

/** Count of every reported TLS version / DTLS cipher / SRTP profile, plus the violation count. */
export function transportSummary(statuses) {
  const out = { tlsVersion: {}, dtlsCipher: {}, srtpCipher: {}, violations: 0, seen: 0 };
  const bump = (bucket, v) => {
    if (v === null || v === undefined) return;
    out[bucket][v] = (out[bucket][v] || 0) + 1;
  };
  for (const s of statuses) {
    if (hasTransportViolation(s)) out.violations++;
    for (const info of [s.transport && s.transport.pub, s.transport && s.transport.sub]) {
      if (!info) continue;
      out.seen++;
      bump('tlsVersion', info.tlsVersion);
      bump('dtlsCipher', info.dtlsCipher);
      bump('srtpCipher', info.srtpCipher);
    }
  }
  return out;
}

/** Group bot errors by phase and code: [{ phase, code, count, sample }], most frequent first. */
export function errorSummary(statuses) {
  const groups = new Map();
  for (const s of statuses) {
    if (!s.error) continue;
    const key = `${s.error.phase}|${s.error.code}`;
    const g = groups.get(key) || { phase: s.error.phase, code: s.error.code, count: 0, sample: s.error.message ?? null };
    g.count++;
    groups.set(key, g);
  }
  return [...groups.values()].sort((a, b) => b.count - a.count);
}

/** Received-packet loss over the cumulative counters of one bot (`tot`), in percent. */
export function totalLossPct(tot) {
  if (!tot) return null;
  const lost = numOr0(tot.aIn?.lost) + numOr0(tot.vIn?.lost);
  const packets = numOr0(tot.aIn?.packets) + numOr0(tot.vIn?.packets);
  return round(lossPct(lost, packets), 3);
}

/** Average audio packets per second sent by one bot; `sumDtMs` = summed sample intervals. */
export function audioOutPpsAvg(tot, sumDtMs) {
  if (!tot || !(sumDtMs > 0)) return null;
  return round(numOr0(tot.aOut?.packets) / (sumDtMs / 1000), 2);
}
