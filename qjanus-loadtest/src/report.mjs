// Report generator: merges the output directories of all shards (and an
// optional node-sampler CSV) into report.json and report.md. The markdown is
// written to be usable as a GitHub job summary.
//
// Merge rules (conservative on purpose):
// - steps are matched across shards by step number; a shard without a row for
//   a step makes that step "incomplete" and it never counts as sustained;
// - join times, loss and client CPU use the worst shard, counts are summed;
// - the global maximum is the last step of the unbroken prefix of steps where
//   every shard is ok and (with a sampler) the Janus CPU maximum in the step
//   window stays within the limit.

import { mkdir, readFile, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { parseSamplerCsv, windowStats } from './cpu-source.mjs';

const isNum = (v) => typeof v === 'number' && Number.isFinite(v);
const nums = (values) => values.filter(isNum);
const maxOf = (values) => (nums(values).length ? Math.max(...nums(values)) : null);
const sumOf = (values) => nums(values).reduce((a, b) => a + b, 0);
const avgOf = (values) => (nums(values).length ? sumOf(values) / nums(values).length : null);
const round = (v, d = 1) => (isNum(v) ? Math.round(v * 10 ** d) / 10 ** d : null);
const f = (v, d = 1) => (isNum(v) ? String(round(v, d)) : '-');

// ------------------------------------------------------------------- loading

async function readJson(file) {
  return JSON.parse(await readFile(file, 'utf8'));
}

async function readJsonl(file) {
  const text = await readFile(file, 'utf8');
  const rows = [];
  for (const line of text.split(/\r?\n/)) {
    if (line.trim() === '') continue;
    try {
      rows.push(JSON.parse(line));
    } catch {
      // A partially written last line (killed run) is skipped.
    }
  }
  return rows;
}

/** Read one shard directory: summary.json is required, the rest optional. */
export async function loadShardDir(dir) {
  const opt = async (name, reader) => {
    try {
      return await reader(path.join(dir, name));
    } catch {
      return null;
    }
  };
  let summary;
  try {
    summary = await readJson(path.join(dir, 'summary.json'));
  } catch {
    throw new Error(`${path.basename(dir)}: summary.json is missing or unreadable`);
  }
  const steps = (await opt('steps.jsonl', readJsonl)) ?? summary.steps ?? [];
  return {
    dir: path.basename(path.resolve(dir)),
    run: await opt('run.json', readJson),
    summary,
    steps,
    bots: (await opt('bots.jsonl', readJsonl)) ?? [],
  };
}

// ------------------------------------------------------------------- merging

const VERDICT_RANK = { ok: 0, inconclusive_client_saturated: 1, breach: 2 };

/** Element-wise sum of per-slice counts; null unless every shard reported the same number of slices (old rows have none). */
function sumSlices(lists) {
  if (lists.some((l) => !Array.isArray(l)) || new Set(lists.map((l) => l.length)).size !== 1) return null;
  return lists[0].map((_, i) => sumOf(lists.map((l) => l[i])));
}

function mergeCounts(dicts) {
  const out = {};
  for (const d of dicts) for (const [k, v] of Object.entries(d || {})) out[k] = (out[k] || 0) + v;
  return out;
}

function samplerStepStats(rows, fromMs, toMs) {
  const cpu = windowStats(rows, fromMs, toMs, 'cpu_pct');
  if (!cpu) return null;
  const inWin = rows.filter((r) => r.tsMs >= fromMs && r.tsMs <= toMs);
  const col = (c) => inWin.map((r) => r[c]);
  return {
    cpuAvgPct: cpu.avg,
    cpuMaxPct: cpu.max,
    rssMaxMb: maxOf(col('rss_mb')),
    rxMbpsMax: maxOf(col('rx_mbps')),
    txMbpsMax: maxOf(col('tx_mbps')),
    udpDrops: sumOf([...col('udp_in_errors_d'), ...col('udp_rcvbuf_errors_d'), ...col('udp_sndbuf_errors_d')]),
    nicDrops: sumOf([...col('rx_drop_d'), ...col('tx_drop_d')]),
    cgThrottledMs: sumOf(col('cg_throttled_ms_d')),
  };
}

function mergeStep(stepNo, shards, expected, samplerRows, cpuLimit) {
  const rows = new Map(); // shard index -> row
  for (const s of shards) {
    const row = s.steps.find((r) => r.step === stepNo);
    if (row) rows.set(s.summary.shard.index, row);
  }
  const list = [...rows.values()];
  const missingShards = expected.filter((i) => !rows.has(i));
  const ref = list[0];

  const verdicts = list.map((r) => r.verdict);
  const worst = verdicts.reduce((a, v) => ((VERDICT_RANK[v] ?? 2) > (VERDICT_RANK[a] ?? 0) ? v : a), 'ok');
  const reasons = [...new Set(list.flatMap((r) => r.breachReasons || []))];

  const fromMs = Math.min(...list.map((r) => r.tMeasureStart));
  const toMs = Math.max(...list.map((r) => r.tEnd));
  const sampler = samplerRows ? samplerStepStats(samplerRows, fromMs, toMs) : null;
  const shardCpuMax = maxOf(list.map((r) => r.janusCpu && r.janusCpu.maxPct));
  const samplerBreach = Boolean(sampler && sampler.cpuMaxPct > cpuLimit);
  if (samplerBreach && !reasons.includes('cpu')) reasons.push('cpu');

  let verdict = worst;
  if (verdict === 'ok' && (missingShards.length > 0 || samplerBreach)) verdict = missingShards.length > 0 ? 'incomplete' : 'breach';

  return {
    step: stepNo,
    rooms: ref.rooms,
    participants: ref.participants,
    botsAttempted: sumOf(list.map((r) => r.join.attempted)),
    botsOk: sumOf(list.map((r) => r.join.ok)),
    botsFailed: sumOf(list.map((r) => r.join.failed + r.join.notSteady)),
    joinP50Ms: maxOf(list.map((r) => r.join.p50Ms)),
    joinP95Ms: maxOf(list.map((r) => r.join.p95Ms)),
    iceP95Ms: maxOf(list.map((r) => r.join.iceP95Ms)),
    dtlsP95Ms: maxOf(list.map((r) => r.join.dtlsP95Ms)),
    lossPct: maxOf(list.map((r) => r.window.lossPct)),
    freezeEvents: sumOf(list.map((r) => r.window.freezeEvents)),
    freezeWindows: sumSlices(list.map((r) => r.window.freezeWindows)),
    clientCpuMaxPct: maxOf(list.map((r) => r.clientCpu && r.clientCpu.maxPct)),
    janusCpu: sampler
      ? { avgPct: sampler.cpuAvgPct, maxPct: sampler.cpuMaxPct, source: 'sampler' }
      : { avgPct: avgOf(list.map((r) => r.janusCpu && r.janusCpu.avgPct)), maxPct: shardCpuMax, source: shardCpuMax === null ? null : 'shard' },
    rssMaxMb: sampler ? sampler.rssMaxMb : null,
    nicRxMbpsMax: sampler ? sampler.rxMbpsMax : null,
    nicTxMbpsMax: sampler ? sampler.txMbpsMax : null,
    udpDrops: sampler ? sampler.udpDrops : null,
    nicDrops: sampler ? sampler.nicDrops : null,
    cgThrottledMs: sampler ? sampler.cgThrottledMs : null,
    lateStartSec: maxOf(list.map((r) => r.lateStartSec)),
    verdict,
    breachReasons: reasons,
    missingShards,
    shardVerdicts: Object.fromEntries([...rows].map(([i, r]) => [i, r.verdict])),
    sustained: verdict === 'ok',
  };
}

const LIMITS = [
  'Fake media: the bots use Chromium fake capture devices (synthetic audio and video). Encoder and decoder cost on the client is not the cost of a phone.',
  'Shared test key: every bot of a room uses one E2EE test key (AES-256-GCM frame transform with the real frame layout). Per-sender keys, epochs, rotation and re-keying are not modelled; the SFU forwards opaque frames either way.',
  'Layer selection is static: each bot requests a fixed substream per role (grid or speaker). The client-driven policy of spec 4.6 (step down on loss, step up after 10 s clean) is not exercised, so bandwidth adaptation under congestion is not part of this result.',
  'Client machine: bots run on one or a few machines. If the client CPU is saturated (above 90 %) a breach may be caused by the client, and the step is reported as inconclusive_client_saturated.',
  'Network path: the bots reach the server over the runner network. NIC, UDP and RSS figures come from the sampler on the server node; they are only present when a sampler CSV was supplied.',
  'Merged shards: values are the worst shard (join times, loss, client CPU) or sums (bots, freezes). Shards evaluate their own stop conditions, so a shard may stop earlier than the others.',
  'Sampler join: the sampler CSV is matched to step windows by timestamp, which assumes the clocks of the client machines and the server node agree within a second or two.',
];

/**
 * Merge loaded shard directories into the report object.
 * @param {Array} shards        results of loadShardDir()
 * @param {object} [o]
 * @param {Array|null} [o.samplerRows]  parsed sampler rows (parseSamplerCsv().rows)
 * @param {number} [o.cpuLimit]         Janus CPU limit in percent of one core (default 180)
 * @param {string} [o.generatedAt]
 */
export function buildReport(shards, { samplerRows = null, cpuLimit = 180, generatedAt = new Date().toISOString() } = {}) {
  if (shards.length === 0) throw new Error('no shard directories given');
  const ref = shards[0].summary;
  const expectedCount = Math.max(...shards.map((s) => s.summary.shard.count));
  const expected = Array.from({ length: expectedCount }, (_, i) => i);
  const present = new Set(shards.map((s) => s.summary.shard.index));
  const missingShards = expected.filter((i) => !present.has(i));

  const stepNumbers = [...new Set(shards.flatMap((s) => s.steps.map((r) => r.step)))].sort((a, b) => a - b);
  const steps = stepNumbers.map((n) => mergeStep(n, shards, expected, samplerRows, cpuLimit));

  // Unbroken prefix of sustained steps; a missing shard stops the prefix too.
  let lastOk = null;
  let firstBreach = null;
  for (const s of steps) {
    if (s.sustained) lastOk = s;
    else {
      firstBreach = s;
      break;
    }
  }

  const summaries = shards.map((s) => s.summary);
  const notes = [];
  if (new Set(summaries.map((s) => s.runId)).size > 1) notes.push('The shard directories carry different run ids.');
  if (missingShards.length) notes.push(`No output for shard(s) ${missingShards.join(', ')}.`);
  if (samplerRows === null) notes.push('No sampler CSV: Janus CPU comes from the shards --cpu-file/--cpu-cmd (if any); RSS, NIC and UDP figures are unavailable.');

  return {
    schema: 1,
    generatedAt,
    runId: ref.runId,
    scenario: ref.scenario,
    mode: ref.mode,
    expectedShards: expectedCount,
    missingShards,
    cpuLimit,
    samplerUsed: samplerRows !== null,
    samplerRows: samplerRows ? samplerRows.length : 0,
    shards: shards
      .map((s) => ({
        index: s.summary.shard.index,
        dir: s.dir,
        ok: s.summary.ok,
        stopReason: s.summary.stopReason,
        steps: s.steps.length,
        bots: s.summary.bots,
        transportPolicyOk: s.summary.transport ? s.summary.transport.policyOk : null,
        clientSaturated: Boolean(s.summary.client && s.summary.client.saturated),
        clientCpuMaxPct: s.summary.client ? s.summary.client.cpuMaxPct : null,
        lateStartSec: maxOf(s.steps.map((r) => r.lateStartSec)),
      }))
      .sort((a, b) => a.index - b.index),
    steps,
    maxSustainable: lastOk ? { step: lastOk.step, rooms: lastOk.rooms, participants: lastOk.participants } : null,
    firstBreach: firstBreach
      ? { step: firstBreach.step, rooms: firstBreach.rooms, participants: firstBreach.participants, verdict: firstBreach.verdict, reasons: firstBreach.breachReasons, missingShards: firstBreach.missingShards }
      : null,
    bots: {
      attempted: sumOf(summaries.map((s) => s.bots.attempted)),
      steady: sumOf(summaries.map((s) => s.bots.steady)),
      failed: sumOf(summaries.map((s) => s.bots.failed)),
      transportViolations: sumOf(summaries.map((s) => s.bots.transportViolations)),
    },
    transport: {
      tlsVersion: mergeCounts(summaries.map((s) => s.transport && s.transport.tlsVersion)),
      dtlsCipher: mergeCounts(summaries.map((s) => s.transport && s.transport.dtlsCipher)),
      srtpCipher: mergeCounts(summaries.map((s) => s.transport && s.transport.srtpCipher)),
      policyOk: summaries.every((s) => s.transport && s.transport.policyOk === true),
    },
    join: {
      p50Ms: maxOf(summaries.map((s) => s.join && s.join.p50Ms)),
      p95Ms: maxOf(summaries.map((s) => s.join && s.join.p95Ms)),
      maxMs: maxOf(summaries.map((s) => s.join && s.join.maxMs)),
    },
    errors: mergeErrors(summaries),
    notes,
    limits: LIMITS,
    ok: missingShards.length === 0 && summaries.every((s) => s.ok === true),
  };
}

function mergeErrors(summaries) {
  const groups = new Map();
  for (const s of summaries) {
    for (const e of s.errors || []) {
      const key = `${e.phase}|${e.code}`;
      const g = groups.get(key) || { phase: e.phase, code: e.code, count: 0, sample: e.sample };
      g.count += e.count;
      groups.set(key, g);
    }
  }
  return [...groups.values()].sort((a, b) => b.count - a.count);
}

// ------------------------------------------------------------------ markdown

const cell = (v) => String(v).replace(/\|/g, '/');

function table(head, rows) {
  const line = (cols) => `| ${cols.map(cell).join(' | ')} |`;
  return [line(head), line(head.map(() => '---')), ...rows.map(line)].join('\n');
}

const dict = (d) => (Object.keys(d).length ? Object.entries(d).map(([k, v]) => `${k} x${v}`).join(', ') : '-');

export function renderMarkdown(r) {
  const out = [];
  out.push('# qjanus load test report', '');
  out.push(`Run \`${r.runId}\` | scenario \`${r.scenario}\` | mode \`${r.mode}\` | ${r.expectedShards} shard(s) | generated ${r.generatedAt}`, '');

  out.push('## Result', '');
  if (r.maxSustainable) {
    out.push(`**Maximum sustainable: ${r.maxSustainable.rooms} rooms, ${r.maxSustainable.participants} participants** (step ${r.maxSustainable.step}).`);
  } else {
    out.push('**No sustainable step: even the first step breached or is incomplete.**');
  }
  if (r.firstBreach) {
    const why = r.firstBreach.reasons.length ? r.firstBreach.reasons.join(', ') : 'incomplete data';
    const miss = r.firstBreach.missingShards.length ? `, no row from shard(s) ${r.firstBreach.missingShards.join(', ')}` : '';
    out.push(`First breach: step ${r.firstBreach.step} (${r.firstBreach.rooms} rooms, ${r.firstBreach.participants} participants), verdict ${r.firstBreach.verdict}, reason: ${why}${miss}.`);
  } else if (r.steps.length > 0) {
    out.push('No breach in the measured steps (the ramp reached its maximum or the run is a single step).');
  }
  out.push(`Janus CPU limit: ${r.cpuLimit} % of one core${r.samplerUsed ? ` (sampler, ${r.samplerRows} rows)` : ' (no sampler CSV)'}. Run ok: ${r.ok ? 'yes' : 'NO'}.`);
  for (const n of r.notes) out.push(`- Note: ${n}`);
  out.push('');

  out.push('## Steps', '');
  out.push(table(
    ['Step', 'Rooms', 'Participants', 'Bots ok/failed', 'Join p50/p95 ms', 'ICE p95 ms', 'DTLS p95 ms', 'Loss %', 'Freezes', 'Client CPU max %', 'Janus CPU avg/max %', 'RSS max MB', 'NIC rx/tx Mbps max', 'UDP drops', 'Verdict'],
    r.steps.map((s) => [
      s.step,
      s.rooms,
      s.participants,
      `${s.botsOk}/${s.botsFailed}`,
      `${f(s.joinP50Ms, 0)}/${f(s.joinP95Ms, 0)}`,
      f(s.iceP95Ms, 0),
      f(s.dtlsP95Ms, 0),
      f(s.lossPct, 2),
      s.freezeEvents > 0 && s.freezeWindows ? `${s.freezeEvents} (${s.freezeWindows.join(',')})` : s.freezeEvents,
      f(s.clientCpuMaxPct, 0),
      `${f(s.janusCpu.avgPct, 0)}/${f(s.janusCpu.maxPct, 0)}`,
      f(s.rssMaxMb, 0),
      `${f(s.nicRxMbpsMax, 1)}/${f(s.nicTxMbpsMax, 1)}`,
      f(s.udpDrops, 0),
      s.missingShards.length ? `incomplete (no row from shard ${s.missingShards.join(',')})` : `${s.verdict}${s.breachReasons.length ? ` (${s.breachReasons.join(',')})` : ''}`,
    ]),
  ));
  out.push('', 'Join, loss and client CPU columns show the worst shard; bots and freezes are summed over shards. Freezes in brackets are the counts per 10 s slice of the measurement window.', '');

  out.push('## Shards', '');
  out.push(table(
    ['Shard', 'Ok', 'Stop reason', 'Steps', 'Bots attempted/steady/failed', 'Transport policy', 'Client CPU max %', 'Client saturated', 'Late start s'],
    r.shards.map((s) => [s.index, s.ok ? 'yes' : 'NO', s.stopReason ?? '-', s.steps, `${s.bots.attempted}/${s.bots.steady}/${s.bots.failed}`, s.transportPolicyOk === null ? '-' : s.transportPolicyOk ? 'ok' : 'VIOLATED', f(s.clientCpuMaxPct, 0), s.clientSaturated ? 'YES' : 'no', f(s.lateStartSec, 0)]),
  ));
  for (const i of r.missingShards) out.push('', `Shard ${i}: no output directory was supplied.`);
  out.push('');

  out.push('## Transport', '');
  out.push(`- TLS version: ${dict(r.transport.tlsVersion)}`);
  out.push(`- DTLS cipher: ${dict(r.transport.dtlsCipher)}`);
  out.push(`- SRTP profile: ${dict(r.transport.srtpCipher)}`);
  out.push(`- Policy ok on every peer connection: ${r.transport.policyOk ? 'yes' : 'NO'}; violations: ${r.bots.transportViolations}`);
  out.push(`- Bots: ${r.bots.attempted} attempted, ${r.bots.steady} steady at the end, ${r.bots.failed} failed`);
  out.push('');

  if (r.errors.length) {
    out.push('## Errors', '');
    out.push(table(['Phase', 'Code', 'Count', 'Sample'], r.errors.map((e) => [e.phase, e.code, e.count, e.sample ?? '-'])));
    out.push('');
  }

  out.push('## Limits of this result', '');
  for (const l of r.limits) out.push(`- ${l}`);
  out.push('');
  return out.join('\n');
}

// -------------------------------------------------------------------- driver

/**
 * Load the shard directories (and the sampler CSV), write report.json and
 * report.md into `out`, and return the report object.
 */
export async function writeReport({ inDirs, sampler = null, out, cpuLimit = 180, generatedAt }) {
  const shards = [];
  for (const dir of inDirs) shards.push(await loadShardDir(dir));
  let samplerRows = null;
  if (sampler) {
    try {
      samplerRows = parseSamplerCsv(await readFile(sampler, 'utf8')).rows;
    } catch {
      samplerRows = null; // reported as "no sampler CSV" in the notes
    }
  }
  const report = buildReport(shards, { samplerRows, cpuLimit, ...(generatedAt ? { generatedAt } : {}) });
  if (sampler && samplerRows === null) report.notes.push('The sampler CSV could not be read.');
  await mkdir(out, { recursive: true });
  await writeFile(path.join(out, 'report.json'), `${JSON.stringify(report, null, 2)}\n`);
  await writeFile(path.join(out, 'report.md'), renderMarkdown(report));
  return report;
}
