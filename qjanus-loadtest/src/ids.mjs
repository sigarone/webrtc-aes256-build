// Deterministic ids for load-test rooms and bots. Every shard (and the admin
// CLI) derives identical values from one seed string, so no manifest has to be
// shared between processes. `k` is the room index, `i` the bot index inside it.
//
// All values are lowercase hex of HMAC-SHA256 keyed with the seed:
//   room      32 hex (128 bit)   "room|k"
//   secret    64 hex (256 bit)   "secret|<room id>"
//   pseudonym 32 hex             "pseudo|k|i"
//   joinToken "<pseudonym>:<32 hex>"  "join|k|i" (the qjanus form: patch 0007 binds a join token to the
//                                     participant id, so the token must start with "<id>:")
//   e2eeKey   64 hex             "e2ee|k"

import { createHmac } from 'node:crypto';

function checkSeed(seed) {
  if (typeof seed !== 'string' || seed.length === 0) {
    throw new TypeError('seed must be a non-empty string');
  }
}

function checkIndex(name, n) {
  if (!Number.isSafeInteger(n) || n < 0) {
    throw new RangeError(`${name} must be a non-negative integer`);
  }
}

function derive(seed, label) {
  checkSeed(seed);
  return createHmac('sha256', seed).update(label).digest('hex');
}

export function roomId(seed, k) {
  checkIndex('k', k);
  return derive(seed, `room|${k}`).slice(0, 32);
}

export function roomSecret(seed, room) {
  if (typeof room !== 'string' || room.length === 0) throw new TypeError('room id must be a non-empty string');
  return derive(seed, `secret|${room}`);
}

export function pseudonym(seed, k, i) {
  checkIndex('k', k);
  checkIndex('i', i);
  return derive(seed, `pseudo|${k}|${i}`).slice(0, 32);
}

export function joinToken(seed, k, i) {
  checkIndex('k', k);
  checkIndex('i', i);
  return `${pseudonym(seed, k, i)}:${derive(seed, `join|${k}|${i}`).slice(0, 32)}`;
}

export function e2eeKey(seed, k) {
  checkIndex('k', k);
  return derive(seed, `e2ee|${k}`);
}

/**
 * Everything one room needs.
 * @returns {{k:number, room:string, secret:string, e2eeKey:string,
 *            members:{i:number, pseudonym:string, joinToken:string}[]}}
 */
export function roomPlan(seed, k, size) {
  if (!Number.isSafeInteger(size) || size < 1) throw new RangeError('size must be an integer >= 1');
  const room = roomId(seed, k);
  const members = [];
  for (let i = 0; i < size; i++) {
    members.push({ i, pseudonym: pseudonym(seed, k, i), joinToken: joinToken(seed, k, i) });
  }
  return { k, room, secret: roomSecret(seed, room), e2eeKey: e2eeKey(seed, k), members };
}
