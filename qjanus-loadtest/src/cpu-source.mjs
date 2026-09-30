// CPU sources.
// - Janus/qjanus CPU: read from the node sampler CSV (--cpu-file) or from a
//   shell command (--cpu-cmd) that prints a number or a JSON object with cpu_pct.
// - Client machine CPU: os.cpus() deltas, sampled once per poll.
// The CSV parser is shared with src/report.mjs.

import { exec as execCb } from 'node:child_process';
import { readFile } from 'node:fs/promises';
import os from 'node:os';
import { promisify } from 'node:util';

const execP = promisify(execCb);

const CMD_TIMEOUT_MS = 4000;

const isNum = (v) => typeof v === 'number' && Number.isFinite(v);

/**
 * Parse the sampler CSV (header names decide the columns; empty fields are
 * null). Robust to a partial last line (a writer caught mid-line), blank
 * lines, CRLF and a BOM. Rows without a usable `ts` are dropped.
 * Returns { header, rows }, every row = { tsMs, ...one key per column }.
 */
export function parseSamplerCsv(text) {
  const lines = String(text ?? '').replace(/^\uFEFF/, '').split(/\r?\n/);
  const header = (lines.shift() ?? '').split(',').map((h) => h.trim());
  if (!header.includes('ts')) return { header: [], rows: [] };
  const rows = [];
  for (const line of lines) {
    if (line.trim() === '') continue;
    const fields = line.split(',');
    if (fields.length !== header.length) continue; // partial or corrupt line
    const row = {};
    header.forEach((name, i) => {
      const raw = fields[i].trim();
      if (name === 'iso') row[name] = raw === '' ? null : raw;
      else row[name] = raw === '' || !isNum(Number(raw)) ? null : Number(raw);
    });
    if (row.ts === null) continue;
    row.tsMs = Math.round(row.ts * 1000);
    rows.push(row);
  }
  return { header, rows };
}

/** { avg, max, n } of column `col` over rows with fromMs <= tsMs <= toMs; null when no value. */
export function windowStats(rows, fromMs, toMs, col = 'cpu_pct') {
  const values = rows.filter((r) => r.tsMs >= fromMs && r.tsMs <= toMs).map((r) => r[col]).filter(isNum);
  if (values.length === 0) return null;
  const sum = values.reduce((a, b) => a + b, 0);
  return { avg: sum / values.length, max: Math.max(...values), n: values.length };
}

/** Extract a CPU percentage from the stdout of --cpu-cmd (a bare number or JSON with cpu_pct). */
export function parseCpuOutput(stdout) {
  const text = String(stdout ?? '').trim();
  if (text === '') return null;
  if (text.startsWith('{')) {
    try {
      const v = Number(JSON.parse(text).cpu_pct);
      return isNum(v) ? v : null;
    } catch {
      return null;
    }
  }
  const v = Number(text.split(/\s+/)[0]);
  return isNum(v) ? v : null;
}

/**
 * Janus CPU source. Call `await refresh()` once per poll (and before reading a
 * window); `sample(fromMs, toMs)` is synchronous over what was collected.
 * Times are epoch milliseconds. `exec`, `readFileFn` and `now` are injectable.
 */
export function createCpuSource({ file, cmd, exec = execP, readFileFn = readFile, now = Date.now } = {}) {
  if (file) {
    let rows = [];
    return {
      kind: 'file',
      async refresh() {
        try {
          rows = parseSamplerCsv(await readFileFn(file, 'utf8')).rows;
        } catch {
          // Missing file: the sampler may not have started yet. Keep what we have.
        }
      },
      sample: (fromMs, toMs) => windowStats(rows, fromMs, toMs),
      latest: () => (rows.length ? rows[rows.length - 1].cpu_pct : null),
    };
  }
  if (cmd) {
    const readings = [];
    return {
      kind: 'cmd',
      async refresh() {
        try {
          const { stdout } = await exec(cmd, { timeout: CMD_TIMEOUT_MS, windowsHide: true });
          const cpu = parseCpuOutput(stdout);
          if (cpu !== null) readings.push({ tsMs: now(), cpu_pct: cpu });
        } catch {
          // A failing command is a missing reading, not a test failure.
        }
      },
      sample: (fromMs, toMs) => windowStats(readings, fromMs, toMs),
      latest: () => (readings.length ? readings[readings.length - 1].cpu_pct : null),
    };
  }
  return { kind: 'none', async refresh() {}, sample: () => null, latest: () => null };
}

/**
 * Client machine CPU: percent busy (0-100, all cores averaged) between two
 * consecutive sample() calls; the first call has no baseline and returns null.
 */
export function createClientCpuMonitor(osModule = os) {
  let prev = null;
  const totals = () => {
    let idle = 0;
    let total = 0;
    for (const c of osModule.cpus()) {
      const t = c.times;
      idle += t.idle;
      total += t.user + t.nice + t.sys + t.idle + t.irq;
    }
    return { idle, total };
  };
  return {
    sample() {
      const cur = totals();
      const before = prev;
      prev = cur;
      if (!before) return null;
      const dTotal = cur.total - before.total;
      if (dTotal <= 0) return null;
      return Math.round(1000 * (1 - (cur.idle - before.idle) / dTotal)) / 10;
    },
  };
}
