// Log hygiene check for a qjanus node (GROUP_CALLS_V2 section 7 + task rules): after the whole test
// run, the service journal must contain no key material, tokens, full ids (max 8 characters),
// addresses, SDP fingerprints or pseudonyms - only the DTLS-POLICY lines with ids and numbers.
//   node check-logs.mjs <journal.txt> <secrets.json>...
// secrets.json = the values the tests handed to Janus (token secret, admin key, room secrets, join tokens,
// pseudonyms, room ids, session tokens...).
import fs from 'node:fs';
import assert from 'node:assert/strict';

const [logFile, ...secretFiles] = process.argv.slice(2);
const log = fs.readFileSync(logFile, 'utf8');
const secrets = [...new Set(secretFiles.flatMap((f) => JSON.parse(fs.readFileSync(f, 'utf8'))))];
const lines = log.split('\n').filter(Boolean);
const problems = [];
const bad = (what, line) => problems.push(`${what}: ${line.slice(0, 200)}`);

for (const l of lines) {
  if (/[0-9a-fA-F]{12,}/.test(l)) bad('hex run of 12+ characters', l);
  if (/(^|[^0-9.])\d{1,3}\.\d{1,3}\.\d{1,3}\.\d{1,3}([^0-9.]|$)/.test(l)) bad('IPv4 address', l);
  if (/([0-9A-Fa-f]{2}:){7,}[0-9A-Fa-f]{2}/.test(l)) bad('fingerprint-like colon hex', l);
  if (/([0-9a-fA-F]{1,4}:){3,}[0-9a-fA-F]{0,4}/.test(l) && !/\d\d:\d\d:\d\d/.test(l)) bad('IPv6-like address', l);
  if (/ice-pwd|ice-ufrag|a=fingerprint|BEGIN (EC |RSA )?PRIVATE KEY/i.test(l)) bad('ICE credential / fingerprint / key marker', l);
  if (/\d{9,11},janus/.test(l)) bad('a signed session token (bearer credential)', l);
}
let checked = 0;
for (const s of secrets) {
  if (!s || s.length < 8) continue;
  checked++;
  // identifiers may appear with their first 8 characters only; everything else must never appear
  const probe = /^[0-9a-f]+$/i.test(s) ? s.slice(0, 9) : s;
  if (log.includes(probe)) bad(`a secret/id (${s.slice(0, 4)}...) appears with more than 8 characters`, lines.find((l) => l.includes(probe)) || '');
}
const policy = lines.filter((l) => /DTLS-POLICY/.test(l));
const ok = policy.filter((l) => /version=0xfefc cipher=0x1302 srtp=0x0008 group=0x11ec/.test(l) && /ok=1/.test(l));
const refused = policy.filter((l) => /ok=0/.test(l));
console.log(`journal lines: ${lines.length}; secrets/ids checked: ${checked}; DTLS-POLICY ok=1: ${ok.length}, ok=0: ${refused.length}`);
console.log('policy line sample:', ok[0] || '(none)');
if (refused[0]) console.log('refusal sample:  ', refused[0]);
const fatal = lines.filter((l) => /FATAL/.test(l));
console.log(`FATAL lines: ${fatal.length}`);
assert.equal(fatal.length, 0, `fatal errors in the log:\n${fatal.slice(0, 5).join('\n')}`);
assert.ok(ok.length >= 10, 'the run must have produced DTLS-POLICY ok=1 lines');
assert.ok(problems.length === 0, `log hygiene violations (${problems.length}):\n  ${problems.slice(0, 20).join('\n  ')}`);
console.log('log hygiene: clean');
