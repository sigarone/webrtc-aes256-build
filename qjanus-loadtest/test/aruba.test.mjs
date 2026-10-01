// aruba/aruba_tool.py against the Node modules it mirrors: tokens verify exactly like Janus does
// (src/token.mjs), ids and room parameters equal src/ids.mjs / src/janus-admin.mjs, and the room
// commands drive a fake Janus HTTP API that checks every token. Pure-logic tests (gate, guard,
// window) are in test/aruba_tool_test.py, run from here too.

import test from 'node:test';
import assert from 'node:assert/strict';
import http from 'node:http';
import path from 'node:path';
import os from 'node:os';
import { spawnSync, spawn } from 'node:child_process';
import { mkdtemp, writeFile, rm } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';

import { mintSessionToken, verifySessionToken } from '../src/token.mjs';
import { roomPlan, roomId } from '../src/ids.mjs';
import { SPEC_ROOM_DEFAULTS } from '../src/janus-admin.mjs';
import { sessionTokenExpiry } from '../src/config.mjs';

const HERE = path.dirname(fileURLToPath(import.meta.url));
const TOOL = path.join(HERE, '..', 'aruba', 'aruba_tool.py');
const PYTEST = path.join(HERE, 'aruba_tool_test.py');

const python = ['python3', 'python', 'py'].find((c) => {
  const r = spawnSync(c, ['-c', 'import sys; sys.exit(0 if sys.version_info >= (3, 8) else 1)'], { encoding: 'utf8' });
  return r.status === 0;
});
const opts = { skip: python ? false : 'no python 3 on this machine' };

const SECRET = 'node-secret-for-the-test';
const ADMIN = 'admin-key-for-the-test';
const SEED = 'aruba-test-seed-0123456789';

async function envFile(dir, extra = '') {
  const f = path.join(dir, 'qjanus.env');
  await writeFile(f, `QJANUS_TOKEN_SECRET=${SECRET}\nQJANUS_ADMIN_KEY=${ADMIN}\nQJANUS_HTTP_BIND=127.0.0.1\n${extra}`);
  return f;
}

function run(args, { env = {}, input = '' } = {}) {
  return new Promise((resolve) => {
    const child = spawn(python, [TOOL, ...args], { env: { ...process.env, ...env }, stdio: ['pipe', 'pipe', 'pipe'] });
    let stdout = '';
    let stderr = '';
    child.stdout.on('data', (d) => (stdout += d));
    child.stderr.on('data', (d) => (stderr += d));
    child.on('close', (status) => resolve({ status, stdout, stderr }));
    child.stdin.end(input);
  });
}

test('the Python unit tests (gate, guard, window, metrics, token format) pass', opts, () => {
  const r = spawnSync(python, [PYTEST], { encoding: 'utf8' });
  assert.equal(r.status, 0, r.stdout + r.stderr);
});

test('mint: tokens verify like Janus does, honour the lifetime cap, and nothing secret is printed on stderr', opts, async () => {
  const dir = await mkdtemp(path.join(os.tmpdir(), 'qjl-aruba-'));
  try {
    const env = { QJANUS_ENV_FILE: await envFile(dir) };
    const r = await run(['mint', '--count', '5', '--ttl-sec', '7200'], { env });
    assert.equal(r.status, 0, r.stderr);
    const tokens = JSON.parse(r.stdout);
    assert.equal(tokens.length, 5);
    const nowMs = Date.now();
    for (const t of tokens) {
      assert.ok(verifySessionToken(t, { secret: SECRET, nowMs }), 'token verifies');
      assert.ok(verifySessionToken(t, { secret: SECRET, nowMs, plugin: 'janus.plugin.videoroom' }));
      assert.ok(!verifySessionToken(t, { secret: 'other-secret', nowMs }));
      assert.ok(!verifySessionToken(t, { secret: SECRET, nowMs: nowMs + 7300 * 1000 }), 'expires after the ttl');
      const left = sessionTokenExpiry(t) - nowMs / 1000;
      assert.ok(left > 7190 && left <= 7200, `ttl ${left}`);
    }
    assert.equal(r.stderr.includes(SECRET), false);
    // byte-identical to the Node minter for the same instant
    const py = spawnSync(python, ['-c', `import sys; sys.path.insert(0, ${JSON.stringify(path.dirname(TOOL))}); import aruba_tool as t; print(t.mint_token(${JSON.stringify(SECRET)}, 600, now=1700000000.0))`], { encoding: 'utf8' });
    assert.equal(py.stdout.trim(), mintSessionToken({ secret: SECRET, ttlSec: 600, nowMs: 1700000000 * 1000 }));
    // refused: ttl above 3 h, count above the cap, no secret
    assert.equal((await run(['mint', '--count', '1', '--ttl-sec', '10801'], { env })).status, 1);
    assert.equal((await run(['mint', '--count', '513'], { env })).status, 1);
    const empty = path.join(dir, 'empty.env');
    await writeFile(empty, 'QJANUS_ADMIN_KEY=x\n');
    assert.equal((await run(['mint', '--count', '1'], { env: { QJANUS_ENV_FILE: empty } })).status, 1);
  } finally {
    await rm(dir, { recursive: true, force: true });
  }
});

test('ids: the Python derivation equals src/ids.mjs for every room and member', opts, () => {
  const code = `
import sys, json; sys.path.insert(0, ${JSON.stringify(path.dirname(TOOL))})
import aruba_tool as t
seed = ${JSON.stringify(SEED)}
out = []
for k in range(0, 40, 7):
    room = t.room_id(seed, k)
    out.append([k, room, t.room_secret(seed, room), [[t.pseudonym(seed, k, i), t.join_token(seed, k, i)] for i in range(8)]])
print(json.dumps(out))`;
  const r = spawnSync(python, ['-c', code], { encoding: 'utf8' });
  assert.equal(r.status, 0, r.stderr);
  for (const [k, room, secret, members] of JSON.parse(r.stdout)) {
    const plan = roomPlan(SEED, k, 8);
    assert.equal(room, plan.room);
    assert.equal(secret, plan.secret);
    members.forEach(([p, j], i) => {
      assert.equal(p, plan.members[i].pseudonym);
      assert.equal(j, plan.members[i].joinToken);
    });
  }
});

// -------------------------------------------------------------- a fake Janus HTTP API

function fakeJanus() {
  const state = { rooms: new Map(), sessions: new Set(), seen: [], badTokens: 0 };
  let nextId = 1000;
  const server = http.createServer((req, res) => {
    let body = '';
    req.on('data', (d) => (body += d));
    req.on('end', () => {
      const msg = JSON.parse(body);
      const reply = (extra) => {
        res.setHeader('content-type', 'application/json');
        res.end(JSON.stringify({ transaction: msg.transaction, ...extra }));
      };
      const attach = msg.janus === 'attach';
      if (!verifySessionToken(msg.token, { secret: SECRET, plugin: attach ? 'janus.plugin.videoroom' : undefined })) {
        state.badTokens += 1;
        return reply({ janus: 'error', error: { code: 403, reason: 'Unauthorized request' } });
      }
      if (msg.janus === 'create') {
        const id = nextId++;
        state.sessions.add(id);
        return reply({ janus: 'success', data: { id } });
      }
      if (msg.janus === 'attach') return reply({ janus: 'success', data: { id: nextId++ } });
      if (msg.janus === 'destroy') {
        state.sessions.delete(Number(req.url.split('/')[2]));
        return reply({ janus: 'success' });
      }
      const b = msg.body;
      state.seen.push(b.request);
      const data = (d) => reply({ janus: 'success', plugindata: { plugin: 'janus.plugin.videoroom', data: d } });
      const err = (code) => data({ videoroom: 'event', error_code: code, error: 'x' });
      if (b.request === 'create') {
        if (b.admin_key !== ADMIN) return err(433);
        if (state.rooms.has(b.room)) return err(427);
        state.rooms.set(b.room, { ...b, participants: 0 });
        return data({ videoroom: 'created', room: b.room });
      }
      if (b.request === 'destroy') {
        const r = state.rooms.get(b.room);
        if (!r) return err(426);
        if (r.secret !== b.secret) return err(433);
        state.rooms.delete(b.room);
        return data({ videoroom: 'destroyed', room: b.room });
      }
      if (b.request === 'allowed') {
        const r = state.rooms.get(b.room);
        if (!r) return err(426);
        if (b.action === 'add') r.allowed = [...new Set([...(r.allowed || []), ...b.allowed])];
        return data({ videoroom: 'success', room: b.room, allowed: r.allowed });
      }
      if (b.request === 'list') {
        if (b.admin_key !== ADMIN) return err(433);
        return data({ videoroom: 'success', list: [...state.rooms.values()].map((r) => ({ room: r.room, num_participants: r.participants })) });
      }
      return err(423);
    });
  });
  return new Promise((resolve) => server.listen(0, '127.0.0.1', () => resolve({ server, state, port: server.address().port })));
}

test('rooms: create / list / destroy drive Janus with valid tokens, spec parameters and seed-derived ids', opts, async () => {
  const dir = await mkdtemp(path.join(os.tmpdir(), 'qjl-aruba-'));
  const { server, state, port } = await fakeJanus();
  try {
    const env = { QJANUS_ENV_FILE: await envFile(dir), QJANUS_HTTP_PORT: String(port) };
    // a room of someone else (a "group call"), must be reported as foreign and never touched
    state.rooms.set('f'.repeat(32), { room: 'f'.repeat(32), secret: 'x', participants: 3 });

    let r = await run(['rooms', 'create', '--rooms', '3', '--size', '8'], { env, input: `${SEED}\n` });
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /created 3 rooms \(0 existed already\)/);
    assert.equal(state.badTokens, 0);
    for (let k = 0; k < 3; k++) {
      const plan = roomPlan(SEED, k, 8);
      const got = state.rooms.get(plan.room);
      assert.ok(got, `room ${k}`);
      assert.equal(got.secret, plan.secret);
      assert.equal(got.publishers, 8);
      assert.deepEqual(got.allowed, plan.members.map((m) => m.joinToken));
      for (const [key, value] of Object.entries(SPEC_ROOM_DEFAULTS)) assert.deepEqual(got[key], value, key);
    }

    // idempotent: running it again tops the ACL up and reports the rooms as existing
    r = await run(['rooms', 'create', '--rooms', '3', '--size', '8'], { env, input: `${SEED}\n` });
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /created 3 rooms \(3 existed already\)/);

    state.rooms.get(roomId(SEED, 1)).participants = 4;
    r = await run(['rooms', 'list'], { env, input: `${SEED}\n` });
    assert.deepEqual(JSON.parse(r.stdout), { foreign_rooms: 1, foreign_participants: 3, harness_rooms: 3, harness_participants: 4 });

    r = await run(['rooms', 'destroy'], { env, input: `${SEED}\n` });
    assert.equal(r.status, 0, r.stderr);
    assert.match(r.stdout, /destroyed 3 rooms/);
    assert.deepEqual([...state.rooms.keys()], ['f'.repeat(32)], 'only the harness rooms are destroyed');
    assert.equal(state.badTokens, 0);
    assert.equal(state.sessions.size, 0, 'every session is closed');

    // no output of any command contains a secret or a full id
    for (const text of [r.stdout, r.stderr]) {
      assert.ok(!text.includes(SECRET) && !text.includes(ADMIN) && !text.includes(SEED) && !text.includes(roomId(SEED, 0)));
    }
    // a seed that is too short is refused
    r = await run(['rooms', 'list'], { env, input: 'short\n' });
    assert.equal(r.status, 1);
    // the seed may also come from a file (the controller keeps it root-only on the node)
    const seedFile = path.join(dir, 'seed');
    await writeFile(seedFile, `${SEED}\n`);
    r = await run(['rooms', 'create', '--rooms', '1', '--size', '8'], { env: { ...env, QJANUS_LOADTEST_SEED_FILE: seedFile } });
    assert.equal(r.status, 0, r.stderr);
    assert.ok(state.rooms.has(roomId(SEED, 0)));
    r = await run(['rooms', 'list'], { env: { ...env, QJANUS_LOADTEST_SEED_FILE: path.join(dir, 'missing') } });
    assert.equal(r.status, 1);
  } finally {
    server.close();
    await rm(dir, { recursive: true, force: true });
  }
});

test('rooms: a wrong admin key is a failure and no secret is echoed', opts, async () => {
  const dir = await mkdtemp(path.join(os.tmpdir(), 'qjl-aruba-'));
  const { server, port } = await fakeJanus();
  try {
    const f = path.join(dir, 'qjanus.env');
    await writeFile(f, `QJANUS_TOKEN_SECRET=${SECRET}\nQJANUS_ADMIN_KEY=wrong-admin-key\nQJANUS_HTTP_BIND=127.0.0.1\n`);
    const r = await run(['rooms', 'create', '--rooms', '1', '--size', '4'], { env: { QJANUS_ENV_FILE: f, QJANUS_HTTP_PORT: String(port) }, input: `${SEED}\n` });
    assert.equal(r.status, 1);
    assert.match(r.stderr, /code 433/);
    assert.ok(!r.stderr.includes('wrong-admin-key') && !r.stdout.includes('wrong-admin-key'));
    const gone = await run(['rooms', 'create', '--rooms', '65'], { env: { QJANUS_ENV_FILE: await envFile(dir), QJANUS_HTTP_PORT: String(port) }, input: `${SEED}\n` });
    assert.equal(gone.status, 1);
  } finally {
    server.close();
    await rm(dir, { recursive: true, force: true });
  }
});
