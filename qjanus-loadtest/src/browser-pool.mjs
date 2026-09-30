// BotHost: the browser side of the harness.
//
// A BotHost is what the orchestrator drives (BrowserPoolHost here, a FakeHost
// in test/ramp.test.mjs):
//   launch()                   start the page server and the first browser
//   prewarm(totalBots)         optional: open every browser and page the run will need up front,
//                              so their start-up never lands inside a timed step
//   startBot(botCfg)           begin one bot (window.qbot.start); rejects when it cannot be started
//   pollAll()                  status of every started bot, one entry per bot, each
//                              { ...BotStatus, browser, page }; samples/events are drained
//   setTokens({ botId: tok })  hand fresh Janus session tokens to live bots
//   stopBot(botId, opts)       stop one bot (opts.graceful); resolves its final status or null
//   stopAll(opts)              stop every remaining bot; resolves the final statuses
//   close()                    close pages, browsers and the page server (idempotent)
//   versions()                 { node, playwright, chromium }
//
// Layout: `botsPerBrowser` bots per Chromium process (one context each), and
// `botsPerPage` bots per page; browsers and pages are created lazily as bots
// are started. A crashed or hung page never throws out of pollAll: its bots
// are reported as failed with error phase 'other', code 'page_crash'.

import { createRequire } from 'node:module';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright';
import { startStaticServer } from './static-server.mjs';

const require = createRequire(import.meta.url);

/** Default location of the bot page (Agent P: page/page.html, page/bot.mjs, ...). */
export const PAGE_DIR = fileURLToPath(new URL('../page/', import.meta.url));

/** Chromium flags every browser gets; `--force-fieldtrials` and extra flags are appended. */
export const BASE_CHROMIUM_ARGS = [
  '--use-fake-device-for-media-stream',
  '--use-fake-ui-for-media-stream',
  '--disable-features=WebRtcHideLocalIpsWithMdns',
  '--allow-loopback-in-peer-connection',
  '--no-sandbox',
  '--autoplay-policy=no-user-gesture-required',
  '--disable-background-timer-throttling',
  '--disable-renderer-backgrounding',
  '--disable-backgrounding-occluded-windows',
];

export function buildChromiumArgs({ fieldTrials, extraArgs = [] }) {
  const args = [...BASE_CHROMIUM_ARGS];
  if (fieldTrials) args.push(`--force-fieldtrials=${fieldTrials}`);
  return [...args, ...extraArgs];
}

const PAGE_READY_TIMEOUT_MS = 20_000;
const START_TIMEOUT_MS = 30_000;
const POLL_TIMEOUT_MS = 10_000;
const STOP_TIMEOUT_MS = 30_000;
const TOKEN_TIMEOUT_MS = 10_000;
const CLOSE_TIMEOUT_MS = 10_000;

function withTimeout(promise, ms, what) {
  let timer;
  const timeout = new Promise((_, reject) => {
    timer = setTimeout(() => reject(new Error(`${what} timed out`)), ms);
  });
  return Promise.race([promise, timeout]).finally(() => clearTimeout(timer));
}

function playwrightVersion() {
  try {
    return require('playwright/package.json').version;
  } catch {
    return null;
  }
}

export class BrowserPoolHost {
  /**
   * @param {object} o
   * @param {number} o.botsPerBrowser
   * @param {number} o.botsPerPage
   * @param {boolean} [o.headed]
   * @param {string|null} [o.chromiumPath]  executablePath override
   * @param {string} [o.fieldTrials]        value of --force-fieldtrials
   * @param {string[]} [o.extraArgs]        extra Chromium flags
   * @param {string} [o.pageDir]
   * @param {(level: string, msg: string) => void} [o.log]
   * @param {object} [o.launcher]           Playwright browser type (tests)
   */
  constructor({ botsPerBrowser, botsPerPage, headed = false, chromiumPath = null, fieldTrials = '', extraArgs = [], pageDir = PAGE_DIR, log = () => {}, launcher = chromium }) {
    this.botsPerBrowser = botsPerBrowser;
    this.botsPerPage = botsPerPage;
    this.headed = headed;
    this.chromiumPath = chromiumPath;
    this.args = buildChromiumArgs({ fieldTrials, extraArgs });
    this.pageDir = pageDir;
    this.log = log;
    this.launcher = launcher;
    this.server = null;
    this.browsers = new Map(); // index -> Promise<BrowserEntry>
    this.slots = new Map(); // botId -> { b, p }
    this.started = 0;
    this.chromiumVersion = null;
    this.closed = false;
  }

  async launch() {
    this.server = await startStaticServer(this.pageDir);
    await this.browserEntry(0); // fail fast: a missing Chromium shows up before the first bot
  }

  versions() {
    return { node: process.version, playwright: playwrightVersion(), chromium: this.chromiumVersion };
  }

  browserEntry(index) {
    if (!this.browsers.has(index)) this.browsers.set(index, this.launchBrowser(index));
    return this.browsers.get(index);
  }

  async launchBrowser(index) {
    const browser = await this.launcher.launch({
      headless: !this.headed,
      args: this.args,
      ...(this.chromiumPath ? { executablePath: this.chromiumPath } : {}),
    });
    this.chromiumVersion ??= browser.version();
    const context = await browser.newContext();
    const entry = { index, browser, context, pages: new Map() };
    browser.on('disconnected', () => {
      for (const p of entry.pages.values()) p.then((pe) => { pe.crashed = true; }, () => {});
    });
    return entry;
  }

  pageEntry(entry, index) {
    if (!entry.pages.has(index)) entry.pages.set(index, this.openPage(entry, index));
    return entry.pages.get(index);
  }

  async openPage(entry, index) {
    const page = await entry.context.newPage();
    const pe = { browser: entry.index, index, page, crashed: false, botIds: new Set(), last: new Map() };
    page.on('crash', () => { pe.crashed = true; });
    await page.goto(`${this.server.url}/page.html`, { waitUntil: 'load' });
    await page.waitForFunction(() => window.qbotReady === true, null, { timeout: PAGE_READY_TIMEOUT_MS });
    return pe;
  }

  /** Every page that was opened successfully. */
  async pages() {
    const out = [];
    for (const entryPromise of this.browsers.values()) {
      const entry = await entryPromise.catch(() => null);
      if (!entry) continue;
      for (const pp of entry.pages.values()) {
        const pe = await pp.catch(() => null);
        if (pe) out.push(pe);
      }
    }
    return out;
  }

  /** Browser and page index of the n-th started bot. */
  slotOf(n) {
    return { b: Math.floor(n / this.botsPerBrowser), p: Math.floor((n % this.botsPerBrowser) / this.botsPerPage) };
  }

  async prewarm(totalBots) {
    const pagesOf = new Map(); // browser index -> page indices
    for (let n = 0; n < totalBots; n++) {
      const { b, p } = this.slotOf(n);
      if (!pagesOf.has(b)) pagesOf.set(b, new Set());
      pagesOf.get(b).add(p);
    }
    for (const [b, pages] of pagesOf) {
      const entry = await this.browserEntry(b);
      await Promise.all([...pages].map((p) => this.pageEntry(entry, p)));
    }
  }

  async startBot(botCfg) {
    const slot = this.slotOf(this.started++);
    this.slots.set(botCfg.id, slot);
    const entry = await this.browserEntry(slot.b);
    const pe = await this.pageEntry(entry, slot.p);
    if (pe.crashed) throw new Error('page crashed before the bot could start');
    const res = await withTimeout(pe.page.evaluate((c) => window.qbot.start(c), botCfg), START_TIMEOUT_MS, 'qbot.start');
    if (!res || res.ok !== true) throw new Error('qbot.start did not accept the bot config');
    pe.botIds.add(botCfg.id);
  }

  crashStatus(pe, id) {
    const last = pe.last.get(id);
    if (last && (last.state === 'closed' || last.state === 'failed')) return { ...last, samples: [], events: [] };
    return {
      id,
      timings: {},
      peers: null,
      transport: { pub: null, sub: null },
      tot: null,
      ...last,
      state: 'failed',
      error: { phase: 'other', code: 'page_crash', message: 'page or browser crashed or stopped responding' },
      samples: [],
      events: [],
    };
  }

  async pollAll() {
    const out = [];
    await Promise.all((await this.pages()).map(async (pe) => {
      if (pe.botIds.size === 0) return;
      let statuses = null;
      if (!pe.crashed) {
        try {
          statuses = (await withTimeout(pe.page.evaluate(() => window.qbot.poll()), POLL_TIMEOUT_MS, 'qbot.poll')).bots;
        } catch {
          pe.crashed = true; // hung or dead: its bots are lost either way
        }
      }
      if (pe.crashed) statuses = [...pe.botIds].map((id) => this.crashStatus(pe, id));
      for (const s of statuses) {
        pe.last.set(s.id, { ...s, samples: [], events: [] });
        out.push({ ...s, browser: pe.browser, page: pe.index });
      }
    }));
    return out;
  }

  async pageOf(botId) {
    const slot = this.slots.get(botId);
    if (!slot) return null;
    const entry = await this.browsers.get(slot.b)?.catch(() => null);
    return entry ? (await entry.pages.get(slot.p)?.catch(() => null)) ?? null : null;
  }

  async setTokens(map) {
    const byPage = new Map();
    for (const [id, token] of Object.entries(map)) {
      const pe = await this.pageOf(id);
      if (!pe || pe.crashed) continue;
      if (!byPage.has(pe)) byPage.set(pe, {});
      byPage.get(pe)[id] = token;
    }
    await Promise.all([...byPage].map(async ([pe, tokens]) => {
      try {
        await withTimeout(pe.page.evaluate((m) => window.qbot.setTokens(m), tokens), TOKEN_TIMEOUT_MS, 'qbot.setTokens');
      } catch {
        this.log('warn', `token refresh failed for page ${pe.browser}/${pe.index}`);
      }
    }));
  }

  async stopBot(botId, opts = { graceful: true }) {
    const pe = await this.pageOf(botId);
    if (!pe || pe.crashed) return null;
    try {
      return await withTimeout(pe.page.evaluate(([id, o]) => window.qbot.stop(id, o), [botId, opts]), STOP_TIMEOUT_MS, 'qbot.stop');
    } catch {
      return null;
    }
  }

  async stopAll(opts = { graceful: true }) {
    const finals = [];
    await Promise.all((await this.pages()).map(async (pe) => {
      if (pe.crashed || pe.botIds.size === 0) return;
      try {
        finals.push(...(await withTimeout(pe.page.evaluate((o) => window.qbot.stopAll(o), opts), STOP_TIMEOUT_MS, 'qbot.stopAll')));
      } catch {
        // The page is going away with its browser anyway.
      }
    }));
    return finals;
  }

  async close() {
    if (this.closed) return;
    this.closed = true;
    for (const entryPromise of this.browsers.values()) {
      const entry = await entryPromise.catch(() => null);
      if (!entry) continue;
      try {
        await withTimeout(entry.browser.close(), CLOSE_TIMEOUT_MS, 'browser.close');
      } catch {
        // Best effort: the process is killed with ours.
      }
    }
    if (this.server) await this.server.close();
  }
}
