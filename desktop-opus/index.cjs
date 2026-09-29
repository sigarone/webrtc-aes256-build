// JS face of the desktop-opus addon — built here for CI self-test only (the
// private qaudion-desktop repo has its own native/index.cjs; this file's job
// is to let build-desktop-opus.yml prove the compiled .node actually works,
// under both Node and Electron, before it is ever attached to a release).
//
// Derived from qaudion-desktop's private native/index.cjs. Difference: a
// `complexity` option on the Decoder (passed straight to the native addon),
// a `set complexity` on the Decoder class (symmetric with the Encoder's, and
// what a consumer needs to move between 5/6/7 live), and the `osceCompiled`
// load-time guard.
'use strict';

const path = require('node:path');

const binding = require('node-gyp-build')(path.join(__dirname));

const REQ = Object.freeze({
  SET_BITRATE: 4002,
  GET_BITRATE: 4003,
  SET_MAX_BANDWIDTH: 4004,
  GET_MAX_BANDWIDTH: 4005,
  SET_VBR: 4006,
  GET_VBR: 4007,
  SET_BANDWIDTH: 4008,
  GET_BANDWIDTH: 4009,
  SET_COMPLEXITY: 4010,
  GET_COMPLEXITY: 4011,
  SET_INBAND_FEC: 4012,
  GET_INBAND_FEC: 4013,
  SET_PACKET_LOSS_PERC: 4014,
  GET_PACKET_LOSS_PERC: 4015,
  SET_DTX: 4016,
  GET_DTX: 4017,
  SET_FORCE_CHANNELS: 4022,
  GET_FORCE_CHANNELS: 4023,
  SET_SIGNAL: 4024,
  GET_SIGNAL: 4025,
  GET_LAST_PACKET_DURATION: 4039,
  SET_EXPERT_FRAME_DURATION: 4040,
  GET_EXPERT_FRAME_DURATION: 4041,
});

const BANDWIDTH = Object.freeze({
  narrowband: 1101, mediumband: 1102, wideband: 1103,
  superwideband: 1104, fullband: 1105, auto: -1000,
});
const SIGNAL = Object.freeze({ auto: -1000, voice: 3001, music: 3002 });
const FRAME_DURATION = Object.freeze({
  arg: 5000, argument: 5000,
  2.5: 5001, 5: 5002, 10: 5003, 20: 5004, 40: 5005,
  60: 5006, 80: 5007, 100: 5008, 120: 5009,
  '2.5ms': 5001, '5ms': 5002, '10ms': 5003, '20ms': 5004, '40ms': 5005,
  '60ms': 5006, '80ms': 5007, '100ms': 5008, '120ms': 5009,
});
const FRAME_DURATION_MS = Object.freeze({
  5000: 'arg', 5001: 2.5, 5002: 5, 5003: 10, 5004: 20,
  5005: 40, 5006: 60, 5007: 80, 5008: 100, 5009: 120,
});

const nameOf = (table, value) =>
  Object.keys(table).find((k) => table[k] === value) ?? value;

function assertOk(code, what) {
  if (code !== 0) throw new Error(`${what}: opus ctl failed (${code})`);
}

class Encoder {
  constructor(options = {}) {
    this._e = new binding.Encoder({
      channels: options.channels ?? 1,
      sample_rate: options.sample_rate ?? 48000,
      application: options.application ?? 'voip',
    });
  }

  encode(pcm) { return this._e.encode(pcm); }
  reset() { this._e.reset(); }
  ctl(cmd, value) { return this._e.ctl(cmd, value); }

  get bitrate() { return this._e.ctl(REQ.GET_BITRATE); }
  set bitrate(v) { assertOk(this._e.ctl(REQ.SET_BITRATE, v), 'set bitrate'); }

  get complexity() { return this._e.ctl(REQ.GET_COMPLEXITY); }
  set complexity(v) { assertOk(this._e.ctl(REQ.SET_COMPLEXITY, v), 'set complexity'); }

  get vbr() { return this._e.ctl(REQ.GET_VBR) === 1; }
  set vbr(v) { assertOk(this._e.ctl(REQ.SET_VBR, v ? 1 : 0), 'set vbr'); }

  get dtx() { return this._e.ctl(REQ.GET_DTX) === 1; }
  set dtx(v) { assertOk(this._e.ctl(REQ.SET_DTX, v ? 1 : 0), 'set dtx'); }

  get inband_fec() { return this._e.ctl(REQ.GET_INBAND_FEC) === 1; }
  set inband_fec(v) { assertOk(this._e.ctl(REQ.SET_INBAND_FEC, v ? 1 : 0), 'set inband_fec'); }

  get packet_loss() { return this._e.ctl(REQ.GET_PACKET_LOSS_PERC); }
  set packet_loss(v) { assertOk(this._e.ctl(REQ.SET_PACKET_LOSS_PERC, v), 'set packet_loss'); }

  get force_channels() {
    const v = this._e.ctl(REQ.GET_FORCE_CHANNELS);
    return v === -1000 ? 'auto' : v;
  }
  set force_channels(v) {
    assertOk(this._e.ctl(REQ.SET_FORCE_CHANNELS, v === 'auto' ? -1000 : v), 'set force_channels');
  }

  get signal() { return nameOf(SIGNAL, this._e.ctl(REQ.GET_SIGNAL)); }
  set signal(v) {
    assertOk(this._e.ctl(REQ.SET_SIGNAL, typeof v === 'string' ? SIGNAL[v] : v), 'set signal');
  }

  get bandwidth() { return nameOf(BANDWIDTH, this._e.ctl(REQ.GET_BANDWIDTH)); }
  set bandwidth(v) {
    assertOk(this._e.ctl(REQ.SET_BANDWIDTH, typeof v === 'string' ? BANDWIDTH[v] : v), 'set bandwidth');
  }

  get max_bandwidth() { return nameOf(BANDWIDTH, this._e.ctl(REQ.GET_MAX_BANDWIDTH)); }
  set max_bandwidth(v) {
    assertOk(this._e.ctl(REQ.SET_MAX_BANDWIDTH, typeof v === 'string' ? BANDWIDTH[v] : v), 'set max_bandwidth');
  }

  get expert_frame_duration() {
    return FRAME_DURATION_MS[this._e.ctl(REQ.GET_EXPERT_FRAME_DURATION)]
      ?? this._e.ctl(REQ.GET_EXPERT_FRAME_DURATION);
  }
  set expert_frame_duration(v) {
    const code = FRAME_DURATION[v];
    if (code === undefined) throw new Error(`set expert_frame_duration: unsupported value ${v}`);
    assertOk(this._e.ctl(REQ.SET_EXPERT_FRAME_DURATION, code), 'set expert_frame_duration');
  }
}

class Decoder {
  constructor(options = {}) {
    this._d = new binding.Decoder({
      channels: options.channels ?? 1,
      sample_rate: options.sample_rate ?? 48000,
      // OSCE-aware callers pass 6 (LACE) / 7 (NoLACE) here; anything else
      // (including omitted) is the deep-PLC-only default of 5 — identical
      // to the private repo's addon.
      complexity: options.complexity ?? 5,
    });
  }

  decode(packet) { return this._d.decode(packet); }
  decodePlc(frameSize) { return this._d.decodePlc(frameSize); }
  decodeFec(packet, frameSize) { return this._d.decodeFec(packet, frameSize); }

  reset() { this._d.reset(); }
  ctl(cmd, value) { return this._d.ctl(cmd, value); }

  get last_packet_duration() { return this._d.ctl(REQ.GET_LAST_PACKET_DURATION); }
  get complexity() { return this._d.ctl(REQ.GET_COMPLEXITY); }
  /** Move between 5 (deep PLC only) / 6 (LACE) / 7 (NoLACE) live. */
  set complexity(v) { assertOk(this._d.ctl(REQ.SET_COMPLEXITY, v), 'set complexity'); }
}

function buildInfo() { return binding.buildInfo(); }

// Load-time guards — this package exists ONLY to ship deep PLC + OSCE, so a
// build missing either is a regression against its own reason for existing,
// same standard the private repo's addon holds itself to.
{
  const info = binding.buildInfo();
  if (!info.deepPlcCompiled) {
    throw new Error(
      'desktop-opus: built WITHOUT ENABLE_DEEP_PLC — check config.h reached the compiler.'
    );
  }
  if (!info.decoderAcceptsComplexity) {
    throw new Error(
      'desktop-opus: decoder rejects OPUS_SET_COMPLEXITY — the linked libopus is ' +
      `older than 1.5 (reports "${info.version}").`
    );
  }
  if (!info.osceCompiled) {
    throw new Error(
      'desktop-opus: built WITHOUT ENABLE_OSCE — this package exists specifically to ' +
      'add OSCE over the private repo\'s deep-PLC-only addon. Check config.h reached the compiler.'
    );
  }
  if (!info.decoderAcceptsOsceComplexity) {
    throw new Error(
      'desktop-opus: decoder rejects complexity 6/7 even though osceCompiled=true — ' +
      'inconsistent build.'
    );
  }
}

module.exports = { Encoder, Decoder, buildInfo, REQ, BANDWIDTH, SIGNAL, FRAME_DURATION };
