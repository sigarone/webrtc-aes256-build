#!/usr/bin/env python3
"""buildinfo.py - emit BUILDINFO.json to stdout for one M150 build job.

Usage: buildinfo.py <android|ios|windows> <plain|lk> [--pins]

  --pins  print the plain-text PINS list (source/tool pins) instead of the JSON.
  windows has only the plain variant (x64, strict transport).

Written BEFORE SHA256SUMS is computed (finding #16: BUILDINFO.json must be
generated before SHA256SUMS and included IN it, not the other way round -
otherwise BUILDINFO.json's own hash is never covered by the checksum file
sitting next to it). The caller is responsible for that ordering; this
script only prints the JSON.

Reads (relative to this script's own directory, i.e. m150/ in this repo -
NOT the webrtc checkout):
  series           - source-patch author's apply order (dir + patch name)
  patches/*        - hashed for provenance
  lk/*.patch        - hashed too, for -lk variants

Reads from the environment (set by the calling workflow step, see
build-m150-android.yml / build-m150-ios.yml):
  WEBRTC_SRC_SHA, BORINGSSL_SHA, OPUS_SHA   - full 40-hex, should already
      have been hard-verified by m150/ci/sync.sh; this script re-derives the
      same three constants itself (see PINS below) and CROSS-CHECKS them
      against the env if present, so a drifted env var is caught here too
      rather than silently recorded as fact.
  GN_ARGS           - the exact --extra-gn-args string used for this build
  RUNNER_IMAGE       - e.g. "ubuntu-24.04" / "macos-26" (image name, not just OS)
  XCODE_VERSION       - ios only, e.g. "26.5" (output of xcodebuild -version)
  GITHUB_SHA, GITHUB_RUN_ID, GITHUB_REPOSITORY - GH Actions provenance
Missing optional env vars are recorded as null rather than failing the build
over a BUILDINFO field - this script's own errors are reserved for usage
mistakes and unreadable patch files, never for "a nice-to-have var is unset".
"""
import hashlib
import json
import os
import sys
import time

SELF_DIR = os.path.dirname(os.path.abspath(__file__))

# Pins - MUST stay identical to m150/ci/sync.sh and
# m150/fetch-opus-dnn-weights.sh (three independent copies by necessity: sh
# scripts vs this Python script; if the source author ever re-pins webrtc_ref,
# all three need updating together - see m150/README.md).
WEBRTC_SRC_PIN = "ba469aa2093ba950066258ca0a59a6fbd1295582"
BORINGSSL_PIN = "f91f1447397c6719f9774dfb8e67329378e1f3d3"
# chromium/src/third_party (DEPS 'src/third_party'); Opus is vendored inline
# there, see m150/ci/sync.sh.
THIRD_PARTY_PIN = "7c92732938de0ef7e28f5da231994723f938f407"
OPUS_PIN = "55513e81d8f606bd75d0ff773d2144e5f2a732f5"
OPUS_WEIGHTS_SHA256 = "160753e983198f29f1aae67c54caa0e30bd90f1ce916a52f15bdad2df8e35e58"
OPUS_WEIGHTS_URL = "https://media.xiph.org/opus/models/opus_data-%s.tar.gz" % OPUS_WEIGHTS_SHA256


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_series():
    """Return [(dir, patch_name), ...] from m150/series, or [] if absent
    (e.g. this script is invoked before the source author's series exists -
    BUILDINFO.json should still render, just with an empty patch list, not
    crash the workflow)."""
    path = os.path.join(SELF_DIR, "series")
    out = []
    if not os.path.isfile(path):
        return out
    with open(path, "r", encoding="utf-8") as f:
        for line in f:
            line = line.split("#", 1)[0].strip()
            if not line:
                continue
            parts = line.split()
            if len(parts) == 2:
                out.append((parts[0], parts[1]))
    return out


def patch_hashes(series, variant):
    result = []
    for d, name in series:
        p = os.path.join(SELF_DIR, "patches", name)
        result.append({
            "name": name,
            "apply_dir": d,
            "sha256": sha256_file(p) if os.path.isfile(p) else None,
            "present": os.path.isfile(p),
        })
    if variant == "lk":
        lk_name = None
        platform = sys.argv[1] if len(sys.argv) > 1 else ""
        if platform == "android":
            lk_name = "jni_prefix.patch"
        elif platform == "ios":
            lk_name = "apple_prefix.patch"
        if lk_name:
            p = os.path.join(SELF_DIR, "lk", lk_name)
            result.append({
                "name": "lk/" + lk_name,
                "apply_dir": ".",
                "sha256": sha256_file(p) if os.path.isfile(p) else None,
                "present": os.path.isfile(p),
            })
    return result


def cross_check(env_name, pinned):
    val = os.environ.get(env_name)
    if val and val != pinned:
        print(
            "::warning::buildinfo.py: env %s=%s does not match the pinned "
            "constant %s - BUILDINFO.json records the PINNED value, but this "
            "mismatch means sync.sh's verification and this script disagree "
            "and must be investigated before trusting the artifact." % (env_name, val, pinned),
            file=sys.stderr,
        )
    return pinned


def main():
    args = [a for a in sys.argv[1:] if a != "--pins"]
    want_pins = len(args) != len(sys.argv) - 1
    if len(args) != 2 or args[0] not in ("android", "ios", "windows") or args[1] not in ("plain", "lk")             or (args[0] == "windows" and args[1] != "plain"):
        print("usage: %s <android|ios|windows> <plain|lk> [--pins]" % sys.argv[0], file=sys.stderr)
        return 2

    platform, variant = args[0], args[1]
    if want_pins:
        print("# Q-Audion m150 source pins (verified by m150/ci/sync.sh before every build)")
        print("webrtc-sdk/webrtc %s" % cross_check("WEBRTC_SRC_SHA", WEBRTC_SRC_PIN))
        print("chromium/src/third_party %s" % cross_check("THIRD_PARTY_SHA", THIRD_PARTY_PIN))
        print("boringssl %s" % cross_check("BORINGSSL_SHA", BORINGSSL_PIN))
        print("opus %s" % cross_check("OPUS_SHA", OPUS_PIN))
        print("opus-dnn-weights-sha256 %s" % OPUS_WEIGHTS_SHA256)
        return 0
    series = read_series()

    info = {
        "schema": "qaudion-webrtc-buildinfo/1",
        "generated_at_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "platform": platform,
        "variant": variant,
        "transport_strict": (platform in ("android", "ios", "windows")) and (variant == "plain"),
        "target": {"os": "win", "cpu": "x64"} if platform == "windows" else None,
        "source": {
            "webrtc_src_sha": cross_check("WEBRTC_SRC_SHA", WEBRTC_SRC_PIN),
            "boringssl_sha": cross_check("BORINGSSL_SHA", BORINGSSL_PIN),
            "third_party_sha": cross_check("THIRD_PARTY_SHA", THIRD_PARTY_PIN),
            "opus_sha": cross_check("OPUS_SHA", OPUS_PIN),
        },
        "patches": patch_hashes(series, variant),
        "opus_dnn_weights": {
            "url": OPUS_WEIGHTS_URL,
            "sha256": OPUS_WEIGHTS_SHA256,
            "extracted_members": [
                "dnn/plc_data.c", "dnn/plc_data.h",
                "dnn/fargan_data.c", "dnn/fargan_data.h",
                "dnn/pitchdnn_data.c", "dnn/pitchdnn_data.h",
                "dnn/lace_data.c", "dnn/lace_data.h",
                "dnn/nolace_data.c", "dnn/nolace_data.h",
                "dnn/dred_rdovae_constants.h", "dnn/dred_rdovae_dec_data.h",
                "dnn/dred_rdovae_stats_data.h",
            ],
        },
        "gn_args": os.environ.get("GN_ARGS"),
        "ci": {
            "runner_image": os.environ.get("RUNNER_IMAGE"),
            "xcode_version": os.environ.get("XCODE_VERSION") if platform == "ios" else None,
            "github_repository": os.environ.get("GITHUB_REPOSITORY"),
            "github_run_id": os.environ.get("GITHUB_RUN_ID"),
            "github_sha_ctrl_repo": os.environ.get("GITHUB_SHA"),
        },
    }
    json.dump(info, sys.stdout, indent=2, sort_keys=True)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
