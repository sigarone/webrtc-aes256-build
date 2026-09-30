import { test } from 'node:test';
import assert from 'node:assert/strict';
import { createHmac } from 'node:crypto';
import { roomId, roomSecret, pseudonym, joinToken, e2eeKey, roomPlan } from '../src/ids.mjs';

const SEED = 'seed-one';
const hmac = (seed, msg) => createHmac('sha256', seed).update(msg).digest('hex');

test('ids follow the contract formulas (fixed vectors from openssl)', () => {
  // printf 'room|0' | openssl dgst -sha256 -hmac 'seed-one'  (first 32 hex chars)
  assert.equal(roomId(SEED, 0), 'ac51824bf5d867b6072fb04edda4b662');
  assert.equal(roomSecret(SEED, 'abc'), '0ebe16edd5f5fd77f93a89d10bc9e49203265051ee2f6242c34da1d65e225849');
  assert.equal(pseudonym(SEED, 3, 5), 'df4817b4328a7144a28c1d277195965e');
  assert.equal(joinToken(SEED, 3, 5), 'a599107f6bdd0e6b10988bc029c6eb67');
  assert.equal(e2eeKey(SEED, 3), '8a28eea340cb4044df69ace744af0df3f4dbdd34fd851f9cef74cc9e2d2e0f25');
});

test('ids equal the HMAC-SHA256 definitions computed independently', () => {
  for (const k of [0, 1, 7, 1234]) {
    const room = roomId(SEED, k);
    assert.equal(room, hmac(SEED, `room|${k}`).slice(0, 32));
    assert.equal(roomSecret(SEED, room), hmac(SEED, `secret|${room}`));
    assert.equal(e2eeKey(SEED, k), hmac(SEED, `e2ee|${k}`));
    for (const i of [0, 1, 15]) {
      assert.equal(pseudonym(SEED, k, i), hmac(SEED, `pseudo|${k}|${i}`).slice(0, 32));
      assert.equal(joinToken(SEED, k, i), hmac(SEED, `join|${k}|${i}`).slice(0, 32));
    }
  }
});

test('length and charset: lowercase hex of 32 or 64 chars', () => {
  const room = roomId(SEED, 4);
  const cases = [
    [room, 32], [roomSecret(SEED, room), 64], [pseudonym(SEED, 4, 2), 32], [joinToken(SEED, 4, 2), 32], [e2eeKey(SEED, 4), 64],
  ];
  for (const [value, len] of cases) assert.match(value, new RegExp(`^[0-9a-f]{${len}}$`));
});

test('deterministic: same inputs give the same ids, in any call order', () => {
  const a = roomPlan(SEED, 3, 5);
  pseudonym(SEED, 9, 9); // unrelated calls must not influence anything
  const b = roomPlan(SEED, 3, 5);
  assert.deepEqual(a, b);
});

test('distinct: rooms, members, kinds and seeds never collide', () => {
  const seen = new Set();
  const add = (v) => {
    assert.ok(!seen.has(v), `collision on ${v}`);
    seen.add(v);
  };
  for (let k = 0; k < 20; k++) {
    add(roomId(SEED, k));
    add(e2eeKey(SEED, k));
    for (let i = 0; i < 20; i++) {
      add(pseudonym(SEED, k, i));
      add(joinToken(SEED, k, i)); // same shape as pseudonym, different label
    }
  }
  assert.equal(seen.size, 20 * 2 + 20 * 20 * 2);
  assert.notEqual(roomId('seed-two', 0), roomId(SEED, 0));
  assert.notEqual(roomSecret('seed-two', roomId(SEED, 0)), roomSecret(SEED, roomId(SEED, 0)));
  assert.notEqual(pseudonym('seed-two', 1, 1), pseudonym(SEED, 1, 1));
  assert.notEqual(joinToken('seed-two', 1, 1), joinToken(SEED, 1, 1));
  assert.notEqual(e2eeKey('seed-two', 1), e2eeKey(SEED, 1));
  // (k, i) = (1, 23) and (12, 3) must not alias although "1|23" / "12|3" concatenate alike without separators
  assert.notEqual(pseudonym(SEED, 1, 23), pseudonym(SEED, 12, 3));
});

test('roomPlan bundles the same values as the single functions', () => {
  const plan = roomPlan(SEED, 2, 3);
  assert.equal(plan.k, 2);
  assert.equal(plan.room, roomId(SEED, 2));
  assert.equal(plan.secret, roomSecret(SEED, plan.room));
  assert.equal(plan.e2eeKey, e2eeKey(SEED, 2));
  assert.deepEqual(plan.members, [0, 1, 2].map((i) => ({ i, pseudonym: pseudonym(SEED, 2, i), joinToken: joinToken(SEED, 2, i) })));
  assert.equal(roomPlan(SEED, 2, 1).members.length, 1);
});

test('an empty or non-string seed throws everywhere', () => {
  for (const seed of ['', undefined, null, 42, {}]) {
    assert.throws(() => roomId(seed, 0), /seed/);
    assert.throws(() => roomSecret(seed, 'abc'), /seed/);
    assert.throws(() => pseudonym(seed, 0, 0), /seed/);
    assert.throws(() => joinToken(seed, 0, 0), /seed/);
    assert.throws(() => e2eeKey(seed, 0), /seed/);
    assert.throws(() => roomPlan(seed, 0, 1), /seed/);
  }
});

test('invalid indexes and sizes throw', () => {
  for (const bad of [-1, 1.5, NaN, '1', undefined]) {
    assert.throws(() => roomId(SEED, bad), /k must/);
    assert.throws(() => pseudonym(SEED, 0, bad), /i must/);
  }
  assert.throws(() => roomSecret(SEED, ''), /room id/);
  assert.throws(() => roomPlan(SEED, 0, 0), /size/);
  assert.throws(() => roomPlan(SEED, 0, 2.5), /size/);
});
