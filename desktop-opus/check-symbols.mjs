#!/usr/bin/env node
// check-symbols.mjs - gate: the built .node must actually contain deep-PLC
// AND OSCE symbols, not just report them via buildInfo(). Mirrors
// m150/gates.sh's G4 (deep-PLC/FARGAN string check), extended for OSCE, but
// implemented in Node rather than `strings`+`grep` because this build runs
// on windows-latest (no binutils `strings` guaranteed on PATH there, and
// Git Bash's coreutils does not include it either).
//
// usage: node check-symbols.mjs <path-to-.node>
// exit: 0 ok | 1 a required symbol/string is missing | 2 usage problem
'use strict';
import { readFileSync } from 'node:fs';

const file = process.argv[2];
if (!file) {
  console.error('usage: node check-symbols.mjs <path-to-.node>');
  process.exit(2);
}

const buf = readFileSync(file);
// Cheap "does this byte sequence appear anywhere in the binary" — same
// signal `strings | grep` gives, without needing either tool. Case
// sensitive on purpose: these are the literal C identifiers/weight-array
// names, not prose.
function contains(str) {
  return buf.includes(Buffer.from(str, 'latin1'));
}

// Deep PLC / FARGAN — same family of symbols the private repo's own
// build-opus-addon.yml greps for ("fargan|lpcnet", case-insensitive). These
// are not function symbols: they are literal C string constants — each
// entry in fargan_data.c's `WeightArray` table carries its own name as a
// `const char*` (e.g. "fargan_..._weights_float"), which is how
// dnn/parse_lpcnet_weights.c looks weights up by name at load time, and why
// the string ends up verbatim in the binary's data section regardless of
// compiler/toolchain or symbol visibility flags.
const deepPlcHits = ['fargan', 'FARGAN', 'lpcnet', 'LPCNET'].filter(contains);

// OSCE — LACE/NoLACE. Required proof strings below were extracted and
// confirmed present, verbatim, in the REAL dnn/lace_data.c / nolace_data.c
// from the pinned weights tarball (opus-dnn-weights-160753e9 — see
// PINS.md): every weight in those files is registered under a
// `"lace_..."` / `"nolace_..."` literal name, same WeightArray mechanism as
// FARGAN above. `osce_enhance_frame` (dnn/osce.h's actual public entry
// point, confirmed present at the pinned commit) and the OSCE_METHOD_*
// #defines are logged too, but NOT required to pass: they are a function
// symbol and preprocessor constants respectively, and whether either
// survives as literal text in a given compiler/linker's output (especially
// MSVC Release, which keeps local symbol names in a separate .pdb, not the
// .node) is not something this script verified — unlike the WeightArray
// name strings, which were.
const osceRequired = ['lace_af1_gain_bias', 'nolace_af1_gain_bias'].filter(contains);
const osceBonus = ['osce_enhance_frame', 'OSCE_METHOD_LACE', 'OSCE_METHOD_NOLACE'].filter(contains);

console.log(`checked: ${file} (${buf.length} bytes)`);
console.log(`deep-PLC/FARGAN hits: ${deepPlcHits.join(', ') || '(none)'}`);
console.log(`OSCE required hits: ${osceRequired.join(', ') || '(none)'}`);
console.log(`OSCE bonus hits (informational only): ${osceBonus.join(', ') || '(none)'}`);

let failed = false;
if (deepPlcHits.length === 0) {
  console.error('FAIL: no FARGAN/LPCNet symbols in the built binary — deep PLC did not compile in');
  failed = true;
}
if (!contains('lace_af1_gain_bias')) {
  console.error('FAIL: no LACE weight names in the built binary — LACE did not compile in');
  failed = true;
}
if (!contains('nolace_af1_gain_bias')) {
  console.error('FAIL: no NoLACE weight names in the built binary — NoLACE did not compile in');
  failed = true;
}
if (failed) process.exit(1);
console.log('OK: deep-PLC, LACE and NoLACE weight data all present in the artifact');
