// Tests for src/config.mjs (flags, env, redaction, shard math, schedule),
// src/scenarios.mjs (BotConfig) and the CLI exit codes.

import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { createHash } from 'node:crypto';
import { test } from 'node:test';
import { fileURLToPath } from 'node:url';
import { ConfigError, buildSchedule, ownsRoom, parseCli, parseShard, redactedConfig, scrubSecrets, shardRoomCount } from '../src/config.mjs';
import { AUDIO_PROFILE, VIDEO_PROFILES, botId, buildBotConfig, videoConfig } from '../src/scenarios.mjs';

const WS = 'wss://sfu-secret-host.example.invalid/janus';
const ENV = {
  QJANUS_WS_URL: WS,
  QJANUS_TOKEN_SECRET: 'TOKENSECRETVALUE',
  QJANUS_LOADTEST_SEED: 'SEEDVALUE123',
};
const NOW = () => new Date('2026-09-30T12:34:56.789Z');

const cfgOf = (argv, env = ENV) => parseCli(argv, env, { now: NOW }).cfg;

test('run defaults', () => {
  const cfg = cfgOf(['run', '--rooms', '3']);
  assert.equal(cfg.command, 'run');
  assert.equal(cfg.scenario, 'audio8');
  assert.equal(cfg.roomSize, 8);
  assert.deepEqual(cfg.shard, { index: 0, count: 1 });
  assert.equal(cfg.holdSec, 60);
  assert.equal(cfg.settleSec, 10);
  assert.equal(cfg.joinTimeoutSec, 60);
  assert.equal(cfg.joinRate, 2);
  assert.equal(cfg.botsPerBrowser, 16);
  assert.equal(cfg.botsPerPage, 1);
  assert.equal(cfg.cpu.limit, 180);
  assert.deepEqual(cfg.limits, { lossPct: 1, freezeTolerance: 0, breachWindows: 2, joinFailPct: 5 });
  assert.deepEqual(cfg.token, { ttlSec: 600, refreshSec: 300 });
  assert.equal(cfg.e2ee, 'aesgcm');
  assert.equal(cfg.expectTransport, 'strict');
  assert.equal(cfg.fieldTrials, 'WebRTC-EnableDtlsPqc/Enabled/WebRTC-LegacySimulcastLayerLimit/Disabled/');
  assert.equal(cfg.runId, '20260930T123456Z-audio8');
  assert.equal(cfg.out, 'out/20260930T123456Z-audio8');
  assert.equal(cfg.schedule.steps.length, 1);
  assert.equal(cfg.schedule.steps[0].rooms, 3);
  assert.equal(cfg.secrets.wsUrl, WS);
});

test('video scenarios: room size and video settings', () => {
  assert.equal(cfgOf(['run', '--scenario', 'video4']).roomSize, 4);
  const v8 = cfgOf(['ramp', '--scenario', 'video8', '--video-profile', 'lite', '--simulcast', 'lm']);
  assert.equal(v8.roomSize, 8);
  assert.deepEqual(v8.video, { profile: 'lite', simulcast: 'lm' });
});

test('missing required env names the variable and never a value', () => {
  for (const name of ['QJANUS_TOKEN_SECRET', 'QJANUS_LOADTEST_SEED']) {
    const env = { ...ENV };
    delete env[name];
    assert.throws(() => parseCli(['run'], env), (e) => e instanceof ConfigError && e.message.includes(name) && !e.message.includes('SEEDVALUE') && !e.message.includes('TOKENSECRET'));
  }
  const noUrl = { ...ENV };
  delete noUrl.QJANUS_WS_URL;
  assert.throws(() => parseCli(['run'], noUrl), /QJANUS_WS_URL/);
  // custom env names are honoured and reported by name
  assert.throws(() => parseCli(['run', '--token-secret-env', 'MY_SECRET'], ENV), /MY_SECRET/);
  assert.equal(parseCli(['run', '--token-secret-env', 'MY_SECRET'], { ...ENV, MY_SECRET: 'abc' }).cfg.secrets.tokenSecret, 'abc');
});

test('an env NAME flag refuses a value that looks like a secret without echoing it', () => {
  const secretLooking = 'sk-live/abc+def==';
  assert.throws(() => parseCli(['run', '--token-secret-env', secretLooking], ENV), (e) => e instanceof ConfigError && !e.message.includes(secretLooking));
});

test('invalid values are usage errors that do not echo the value', () => {
  const bad = (argv, re) => assert.throws(() => parseCli(argv, ENV), (e) => e instanceof ConfigError && re.test(e.message));
  bad(['run', '--rooms', 'abc'], /--rooms/);
  bad(['run', '--rooms', '0'], /--rooms/);
  bad(['run', '--hold-sec', '-5'], /--hold-sec/);
  bad(['run', '--scenario', 'video9'], /--scenario/);
  bad(['run', '--shard', '2/2'], /--shard/);
  bad(['run', '--shard', 'x'], /--shard/);
  bad(['run', '--bots-per-page', '4', '--bots-per-browser', '2'], /bots-per-page/);
  bad(['run', '--token-ttl-sec', '100', '--token-refresh-sec', '100'], /token-refresh-sec/);
  bad(['run', '--cpu-file', 'a.csv', '--cpu-cmd', 'echo 1'], /cpu-file or --cpu-cmd/);
  bad(['run', '--ramp-max', '5'], /--ramp-max/);
  bad(['ramp', '--rooms', '5'], /--rooms/);
  bad(['ramp', '--ramp-start', '5', '--ramp-max', '3'], /ramp-max/);
  bad(['run', '--nonsense'], /nonsense/);
  bad(['run', 'positional'], /positional/);
  bad(['frobnicate'], /unknown command/);
  bad(['run', '--dtls-fingerprint', 'zz'], /dtls-fingerprint/);
  bad(['run', '--ice-servers-json', '{'], /ice-servers/);
  bad(['run', '--manage-rooms'], /QJANUS_ADMIN_URL|admin-url/);
  assert.throws(() => parseCli(['run', '--ws-url', 'http://x.example.invalid/'], ENV), /--ws-url/);
});

test('--manage-rooms reads the admin URL and key', () => {
  const env = { ...ENV, QJANUS_ADMIN_URL: 'http://127.0.0.1:8088/janus', QJANUS_ADMIN_KEY: 'ADMINKEYVALUE' };
  const cfg = parseCli(['run', '--manage-rooms'], env, { now: NOW }).cfg;
  assert.equal(cfg.manageRooms, true);
  assert.equal(cfg.secrets.adminUrl, 'http://127.0.0.1:8088/janus');
  assert.equal(cfg.secrets.adminKey, 'ADMINKEYVALUE');
  assert.throws(() => parseCli(['run', '--manage-rooms'], { ...ENV, QJANUS_ADMIN_URL: 'http://127.0.0.1:8088/janus' }), /QJANUS_ADMIN_KEY/);
});

test('repeatable --chromium-arg and fingerprint normalisation', () => {
  const fp = Array.from({ length: 32 }, (_, i) => i.toString(16).padStart(2, '0')).join(':');
  const cfg = cfgOf(['run', '--chromium-arg=--disable-gpu', '--chromium-arg=--host-resolver-rules=MAP a 10.0.0.1', '--dtls-fingerprint', fp]);
  assert.deepEqual(cfg.chromiumArgs, ['--disable-gpu', '--host-resolver-rules=MAP a 10.0.0.1']);
  assert.equal(cfg.secrets.dtlsFingerprint, `sha-256 ${fp.toUpperCase()}`);
});

test('redactedConfig contains no URL, secret, seed, admin data, ICE credential, path or command', () => {
  const env = {
    ...ENV,
    QJANUS_ADMIN_URL: 'http://10.9.8.7:8088/janus',
    QJANUS_ADMIN_KEY: 'ADMINKEYVALUE',
    QJANUS_ICE_SERVERS: JSON.stringify([{ urls: 'turn:turn.example.invalid:3478', username: 'ICEUSER99', credential: 'ICECRED99' }]),
  };
  const fp = Array.from({ length: 32 }, () => 'AB').join(':');
  const cfg = parseCli(['ramp', '--manage-rooms', '--cpu-cmd', 'ssh root@10.1.2.3 cat /x', '--chromium-path', 'C:/Users/someone/chrome.exe', '--chromium-arg=--proxy-server=10.1.1.1:3128', '--dtls-fingerprint', fp, '--out', 'C:/Users/someone/out'], env, { now: NOW }).cfg;
  const text = JSON.stringify(redactedConfig(cfg));
  for (const forbidden of ['sfu-secret-host', 'wss://', 'TOKENSECRETVALUE', 'SEEDVALUE123', '10.9.8.7', 'ADMINKEYVALUE', 'ICEUSER99', 'ICECRED99', 'turn.example', '10.1.2.3', 'ssh', 'someone', '10.1.1.1', 'ABAB', 'secret']) {
    assert.ok(!text.includes(forbidden), `redacted config leaks ${forbidden}`);
  }
  const r = redactedConfig(cfg);
  assert.equal(r.targetId, createHash('sha256').update(WS).digest('hex').slice(0, 12));
  assert.equal(r.iceServers, 1);
  assert.equal(r.dtlsPin, true);
  assert.deepEqual(r.cpu, { source: 'cmd', limit: 180 });
  assert.deepEqual(r.chromiumArgs, ['--proxy-server']);
  assert.equal(r.manageRooms, true);
});

test('scrubSecrets removes known secrets, URLs, IPs and session tokens', () => {
  const cfg = cfgOf(['run']);
  const text = scrubSecrets(`connect to ${WS} failed; TOKENSECRETVALUE 1.2.3.4:443 http://a.b/c token 1893456000,janus,janus.plugin.videoroom:AbC+/= end`, cfg);
  assert.equal(text, 'connect to <redacted> failed; <redacted> <ip> <url> token <token> end');
});

test('shard math: k % S == i', () => {
  assert.deepEqual(parseShard('1/4'), { index: 1, count: 4 });
  assert.deepEqual(parseShard(undefined), { index: 0, count: 1 });
  const owned = (shard, K) => Array.from({ length: K }, (_, k) => k).filter((k) => ownsRoom(shard, k));
  assert.deepEqual(owned({ index: 1, count: 3 }, 8), [1, 4, 7]);
  for (const count of [1, 2, 3, 5]) {
    for (let K = 0; K <= 12; K++) {
      let total = 0;
      for (let index = 0; index < count; index++) {
        const shard = { index, count };
        assert.equal(shardRoomCount(shard, K), owned(shard, K).length, `K=${K} ${index}/${count}`);
        total += shardRoomCount(shard, K);
      }
      assert.equal(total, K);
    }
  }
});

test('run schedule: a single step with the deterministic period', () => {
  const cfg = cfgOf(['run', '--rooms', '3', '--hold-sec', '30', '--settle-sec', '5', '--join-rate', '4']);
  const [s] = cfg.schedule.steps;
  assert.equal(s.step, 1);
  assert.equal(s.participants, 24);
  assert.deepEqual(s.newRooms, [0, 1, 2]);
  assert.equal(s.newBots, 24);
  assert.equal(s.joinWindowSec, 6); // 24 bots / 4 per second
  assert.equal(s.stepSec, 6 + 5 + 30);
  assert.equal(cfg.schedule.totalSec, 41);
});

test('ramp schedule: K_j = start + j*step up to max, offsets accumulate', () => {
  const cfg = cfgOf(['ramp', '--ramp-start', '2', '--ramp-step', '3', '--ramp-max', '10', '--hold-sec', '20', '--settle-sec', '4', '--join-rate', '2']);
  assert.deepEqual(cfg.schedule.steps.map((s) => s.rooms), [2, 5, 8]);
  assert.deepEqual(cfg.schedule.steps.map((s) => s.newRooms), [[0, 1], [2, 3, 4], [5, 6, 7]]);
  const joins = cfg.schedule.steps.map((s) => s.joinWindowSec);
  assert.deepEqual(joins, [8, 12, 12]);
  const periods = cfg.schedule.steps.map((s) => s.stepSec);
  assert.deepEqual(periods, [32, 36, 36]);
  assert.deepEqual(cfg.schedule.steps.map((s) => s.offsetSec), [0, 32, 68]);
  assert.equal(cfg.schedule.totalSec, 104);
});

test('all shards share the step schedule; the join window is sized for the busiest shard', () => {
  const args = ['ramp', '--scenario', 'video4', '--ramp-start', '1', '--ramp-step', '2', '--ramp-max', '7', '--hold-sec', '20', '--settle-sec', '5', '--join-rate', '1'];
  const shards = [0, 1, 2].map((i) => cfgOf([...args, '--shard', `${i}/3`]));
  const shape = (c) => c.schedule.steps.map((s) => [s.step, s.rooms, s.joinWindowSec, s.stepSec, s.offsetSec]);
  assert.deepEqual(shape(shards[1]), shape(shards[0]));
  assert.deepEqual(shape(shards[2]), shape(shards[0]));
  // step 2 adds rooms 1 and 2: shards 1 and 2 start one room (4 bots) each, shard 0 none
  assert.deepEqual(shards.map((c) => c.schedule.steps[1].newRooms), [[], [1], [2]]);
  assert.equal(shards[0].schedule.steps[1].joinWindowSec, 4);
  // participants is the total over shards, the shard fields are this shard's part
  const last = shards.map((c) => c.schedule.steps[3]);
  assert.deepEqual(last.map((s) => s.participants), [28, 28, 28]);
  assert.equal(last.reduce((a, s) => a + s.shardParticipants, 0), 28);
  assert.deepEqual(last.map((s) => s.shardRooms), [3, 2, 2]);
});

test('buildSchedule is pure and handles an empty shard step', () => {
  const sched = buildSchedule({ ks: [1, 2], roomSize: 8, shard: { index: 1, count: 2 }, joinRate: 2, settleSec: 10, holdSec: 60 });
  assert.equal(sched.steps[0].newBots, 0);
  assert.equal(sched.steps[0].shardRooms, 0);
  assert.equal(sched.steps[0].joinWindowSec, 4);
  assert.deepEqual(sched.steps[1].newRooms, [1]);
});

test('start-at and out defaults for shards', () => {
  const cfg = cfgOf(['run', '--start-at', '1893456000', '--shard', '1/2', '--run-id', 'abc-1']);
  assert.equal(cfg.startAt, 1893456000);
  assert.equal(cfg.out, 'out/abc-1/shard-1');
  assert.throws(() => parseCli(['run', '--run-id', '../x'], ENV), /--run-id/);
});

test('plan and report parsing', () => {
  assert.deepEqual(parseCli(['plan', '--shards', '4'], {}), { command: 'plan', shards: 4 });
  assert.deepEqual(parseCli(['plan', '--shard', '0/3'], {}), { command: 'plan', shards: 3 });
  const r = parseCli(['report', '--in', 'a', '--in', 'b', '--sampler', 's.csv', '--out', 'o'], {});
  assert.deepEqual(r.report, { inDirs: ['a', 'b'], sampler: 's.csv', out: 'o', cpuLimit: 180 });
  assert.throws(() => parseCli(['report', '--out', 'o'], {}), /--in/);
  assert.equal(parseCli([], {}).command, 'help');
  assert.equal(parseCli(['run', '--help'], {}).command, 'help');
});

// ----------------------------------------------------------------- scenarios

test('video profiles match the spec ladder and the CI/tiny variants', () => {
  const spec = videoConfig('spec', 'lmh');
  assert.deepEqual(spec.capture, { width: 1280, height: 720, frameRate: 25 });
  assert.deepEqual(spec.encodings.map((e) => e.rid), ['h', 'm', 'l']);
  assert.deepEqual(spec.encodings.map((e) => [e.scaleResolutionDownBy, e.maxBitrate, e.maxFramerate]), [[1, 1_200_000, 25], [2, 450_000, 20], [4, 150_000, 15]]);
  const lite = videoConfig('lite', 'lmh');
  assert.deepEqual(lite.capture, { width: 640, height: 360, frameRate: 20 });
  assert.deepEqual(lite.encodings.map((e) => [e.rid, e.maxBitrate, e.maxFramerate]), [['h', 250_000, 20], ['m', 110_000, 15], ['l', 40_000, 15]]);
  const tiny = videoConfig('tiny', 'lmh');
  assert.deepEqual(tiny.capture, { width: 320, height: 180, frameRate: 10 });
  assert.equal(tiny.encodings.length, 1);
  assert.equal(tiny.encodings[0].rid, undefined);
  assert.equal(tiny.encodings[0].scaleResolutionDownBy, 2);
  assert.equal(tiny.encodings[0].maxFramerate, 10);
  assert.deepEqual(Object.keys(VIDEO_PROFILES), ['spec', 'lite', 'tiny']);
});

test('--simulcast subsets', () => {
  assert.deepEqual(videoConfig('spec', 'lm').encodings.map((e) => e.rid), ['m', 'l']);
  assert.deepEqual(videoConfig('spec', 'l').encodings.map((e) => e.rid), ['l']);
  const none = videoConfig('spec', 'none');
  assert.equal(none.encodings.length, 1);
  assert.equal(none.encodings[0].rid, undefined);
  assert.equal(none.encodings[0].maxBitrate, 1_200_000);
  assert.equal(videoConfig('spec', 'lm').layerCount, 2);
});

function fakePlan(k, size) {
  const hex = (label) => createHash('sha256').update(label).digest('hex');
  return {
    k,
    room: hex(`room${k}`).slice(0, 32),
    secret: hex(`secret${k}`),
    e2eeKey: hex(`e2ee${k}`),
    members: Array.from({ length: size }, (_, i) => ({ i, pseudonym: hex(`p${k}|${i}`).slice(0, 32), joinToken: hex(`j${k}|${i}`).slice(0, 32) })),
  };
}

test('buildBotConfig: audio bot', () => {
  const cfg = cfgOf(['run']);
  const plan = fakePlan(7, 8);
  const bot = buildBotConfig({ cfg, plan, index: 3, sessionToken: 'TOK' });
  assert.equal(bot.id, 'r0007-b03');
  assert.equal(botId(7, 3), 'r0007-b03');
  assert.equal(bot.wsUrl, WS);
  assert.equal(bot.sessionToken, 'TOK');
  assert.equal(bot.room, plan.room);
  assert.equal(bot.pseudonym, plan.members[3].pseudonym);
  assert.equal(bot.joinToken, plan.members[3].joinToken);
  assert.equal(bot.peers.length, 7);
  assert.ok(!bot.peers.includes(bot.pseudonym));
  assert.deepEqual(bot.media, { audio: true, video: false });
  assert.deepEqual(bot.audio, AUDIO_PROFILE);
  assert.deepEqual(bot.audio, { ptime: 60, maxaveragebitrate: 32000, cbr: true, fec: true, dtx: false });
  assert.equal(bot.video, undefined);
  assert.deepEqual(bot.e2ee, { mode: 'aesgcm', keyHex: plan.e2eeKey });
  assert.equal(bot.expectTransport, 'strict');
  assert.equal(bot.dtlsFingerprint, null);
  assert.equal(bot.subscribe.speaker, null);
  assert.deepEqual(bot.iceServers, []);
});

test('buildBotConfig: video bots, speaker is member 0, substreams clamp to the published layers', () => {
  const cfg = cfgOf(['run', '--scenario', 'video4']);
  const plan = fakePlan(0, 4);
  const b0 = buildBotConfig({ cfg, plan, index: 0, sessionToken: 't' });
  const b2 = buildBotConfig({ cfg, plan, index: 2, sessionToken: 't' });
  assert.equal(b0.subscribe.speaker, null);
  assert.equal(b2.subscribe.speaker, plan.members[0].pseudonym);
  assert.deepEqual(b2.subscribe, { substream: 1, speakerSubstream: 2, temporal: 2, speaker: plan.members[0].pseudonym });
  assert.equal(b2.video.encodings.length, 3);
  assert.deepEqual(b2.media, { audio: true, video: true });
  const lm = buildBotConfig({ cfg: cfgOf(['run', '--scenario', 'video4', '--simulcast', 'lm']), plan, index: 2, sessionToken: 't' });
  assert.equal(lm.subscribe.speakerSubstream, 1);
  const none = buildBotConfig({ cfg: cfgOf(['run', '--scenario', 'video4', '--simulcast', 'none']), plan, index: 2, sessionToken: 't' });
  assert.deepEqual([none.subscribe.substream, none.subscribe.speakerSubstream], [0, 0]);
  assert.equal(none.video.encodings.length, 1);
});

// ----------------------------------------------------------------------- CLI

const BIN = fileURLToPath(new URL('../bin/qjanus-load.mjs', import.meta.url));
const run = (args, env = {}) => spawnSync(process.execPath, [BIN, ...args], { encoding: 'utf8', env: { PATH: process.env.PATH, SystemRoot: process.env.SystemRoot, ...env } });

test('CLI: plan prints the shard list', () => {
  const r = run(['plan', '--shards', '3']);
  assert.equal(r.status, 0);
  assert.deepEqual(JSON.parse(r.stdout), [0, 1, 2]);
});

test('CLI: missing env exits 2 and names only the variable', () => {
  const r = run(['run', '--ws-url', 'wss://sfu-secret-host.example.invalid/janus'], { QJANUS_LOADTEST_SEED: 'SEEDVALUE123' });
  assert.equal(r.status, 2);
  assert.match(r.stderr, /QJANUS_TOKEN_SECRET/);
  assert.ok(!r.stderr.includes('SEEDVALUE123'));
  assert.ok(!r.stderr.includes('sfu-secret-host'));
});

test('CLI: help lists the contract flags and exits 0; unknown flag exits 2', () => {
  const r = run(['--help']);
  assert.equal(r.status, 0);
  for (const flag of ['--ws-url', '--shard', '--start-at', '--join-rate', '--video-profile', '--simulcast', '--manage-rooms', '--cpu-file', '--cpu-cmd', '--cpu-limit', '--loss-limit-pct', '--breach-windows', '--token-refresh-sec', '--dtls-fingerprint', '--field-trials', '--chromium-arg', '--chromium-path', '--headed', '--duration-cap-sec', '--ice-servers-json', '--quiet', '--sampler']) {
    assert.ok(r.stdout.includes(flag), `help misses ${flag}`);
  }
  assert.match(r.stdout, /^[\x20-\x7e\n]*$/, 'help must be ASCII');
  assert.equal(run(['run', '--bogus']).status, 2);
});
