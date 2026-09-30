// Scenario catalogue and BotConfig construction (the cfg object handed to
// window.qbot.start(), see CONTRACT.md "BotConfig").

/** Audio profile shared by every bot: identical to the 1:1 profile (spec 4.4). */
export const AUDIO_PROFILE = { ptime: 60, maxaveragebitrate: 32000, cbr: true, fec: true, dtx: false };

/**
 * Video profiles, exported as data (treated as read-only). `capture` is the
 * fake camera size; every layer is a sendEncodings entry where
 * scaleResolutionDownBy is relative to the capture size.
 *  - spec: spec 4.4 exactly (320x180@15 150 kbps, 640x360@20 450 kbps, 1280x720@25 1200 kbps)
 *  - lite: the same ladder at 1/4 of the sizes and bitrates, meant for CI runners
 *  - tiny: one 160x90@10 layer, no simulcast (the only layer is stored under `h`)
 */
export const VIDEO_PROFILES = {
  spec: {
    simulcast: true,
    capture: { width: 1280, height: 720, frameRate: 25 },
    layers: {
      l: { scaleResolutionDownBy: 4, maxBitrate: 150_000, maxFramerate: 15 },
      m: { scaleResolutionDownBy: 2, maxBitrate: 450_000, maxFramerate: 20 },
      h: { scaleResolutionDownBy: 1, maxBitrate: 1_200_000, maxFramerate: 25 },
    },
  },
  lite: {
    simulcast: true,
    capture: { width: 640, height: 360, frameRate: 20 },
    layers: {
      l: { scaleResolutionDownBy: 4, maxBitrate: 40_000, maxFramerate: 15 },
      m: { scaleResolutionDownBy: 2, maxBitrate: 110_000, maxFramerate: 15 },
      h: { scaleResolutionDownBy: 1, maxBitrate: 250_000, maxFramerate: 20 },
    },
  },
  tiny: {
    simulcast: false,
    capture: { width: 320, height: 180, frameRate: 10 },
    layers: { h: { scaleResolutionDownBy: 2, maxBitrate: 30_000, maxFramerate: 10 } },
  },
};

/** --simulcast values: which layers are published, lowest first ('none' = one encoding without simulcast). */
export const SIMULCAST_MODES = { lmh: ['l', 'm', 'h'], lm: ['l', 'm'], l: ['l'], none: [] };

/** Scenarios: room size and whether the bots publish video. */
export const SCENARIOS = {
  audio8: { roomSize: 8, video: false },
  video4: { roomSize: 4, video: true },
  video8: { roomSize: 8, video: true },
};

/**
 * Publisher video settings for a profile and simulcast subset:
 * { capture, encodings, layerCount }. Encodings are ordered h, m, l (as
 * html/janus.js does). A single encoding (tiny profile or simulcast 'none')
 * carries no rid and is the highest layer of the profile.
 */
export function videoConfig(profileName, simulcast) {
  const profile = VIDEO_PROFILES[profileName];
  if (!profile) throw new Error(`unknown video profile: ${profileName}`);
  const rids = SIMULCAST_MODES[simulcast];
  if (!rids) throw new Error(`unknown simulcast mode: ${simulcast}`);
  const capture = { ...profile.capture };
  if (!profile.simulcast || rids.length === 0) {
    return { capture, encodings: [{ ...profile.layers.h }], layerCount: 1 };
  }
  const encodings = [...rids].reverse().map((rid) => ({ rid, ...profile.layers[rid] }));
  return { capture, encodings, layerCount: rids.length };
}

/** Bot label, no secrets: "r0007-b03" = room index 7, member 3. */
export function botId(k, i) {
  return `r${String(k).padStart(4, '0')}-b${String(i).padStart(2, '0')}`;
}

/**
 * BotConfig for member `index` of the room described by `plan`
 * (src/ids.mjs roomPlan: { k, room, secret, e2eeKey, members:[{i, pseudonym, joinToken}] }).
 * The speaker is member 0 of the room (member 0 itself has none). Substreams
 * are clamped to the number of published layers so that `--simulcast l|none`
 * asks for the only layer that exists.
 */
export function buildBotConfig({ cfg, plan, index, sessionToken }) {
  const scenario = SCENARIOS[cfg.scenario];
  const me = plan.members[index];
  const peers = plan.members.filter((m) => m.i !== me.i).map((m) => m.pseudonym);
  const botCfg = {
    id: botId(plan.k, me.i),
    wsUrl: cfg.secrets.wsUrl,
    sessionToken,
    room: plan.room,
    pseudonym: me.pseudonym,
    joinToken: me.joinToken,
    peers,
    media: { audio: true, video: scenario.video },
    audio: { ...AUDIO_PROFILE },
    e2ee: { mode: cfg.e2ee, keyHex: plan.e2eeKey },
    iceServers: cfg.secrets.iceServers,
    expectTransport: cfg.expectTransport,
    dtlsFingerprint: cfg.secrets.dtlsFingerprint ?? null,
    timeouts: { requestMs: 8000, connectMs: 20000, keepaliveMs: 25000 },
    statsIntervalMs: 2000,
    renegotiateDebounceMs: 150,
    render: 'attach',
  };
  let maxSubstream = 2;
  if (scenario.video) {
    const video = videoConfig(cfg.video.profile, cfg.video.simulcast);
    botCfg.video = { capture: video.capture, encodings: video.encodings };
    maxSubstream = video.layerCount - 1;
  }
  const speaker = scenario.video && me.i !== 0 ? plan.members[0].pseudonym : null;
  botCfg.subscribe = {
    substream: Math.min(cfg.substream, maxSubstream),
    speakerSubstream: Math.min(cfg.speakerSubstream, maxSubstream),
    temporal: cfg.temporal,
    speaker,
  };
  return botCfg;
}
