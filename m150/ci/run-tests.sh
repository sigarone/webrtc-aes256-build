#!/bin/sh
# run-tests.sh - run the T1-T6 unit-test filters (webrtc-plan.md v2 §6.1)
# against already-built rtc_unittests / modules_unittests. Shared by
# test-m150-patches.yml (Linux x64) and build-m150-windows.yml (Windows x64)
# so the filters live in exactly one place.
#
# usage: run-tests.sh <out_dir> <strict|switchable> [exe_suffix]
#   <out_dir>      GN output dir holding rtc_unittests, modules_unittests and
#                  peerconnection_unittests
#   <config>       strict | switchable (rtc_qaudion_transport_strict)
#   [exe_suffix]   ".exe" on Windows, empty elsewhere
# exit: 0 all selected filters passed | 1 a test failed or a filter matched
#       nothing (a filter that matches no test would silently pass) | 2 usage
set -eu

usage() { echo "usage: $0 <out_dir> <strict|switchable> [exe_suffix]" >&2; exit 2; }
[ $# -ge 2 ] && [ $# -le 3 ] || usage
OUT=$1
CONFIG=$2
EXE=${3:-}
case "$CONFIG" in strict|switchable) ;; *) usage ;; esac
[ -d "$OUT" ] || { echo "::error::run-tests: $OUT is not a directory" >&2; exit 2; }

# T1 (SRTP): api/crypto/crypto_options_unittest.cc already covers exactly
# the function P6 rewrites (CryptoOptions::GetSupportedDtlsSrtpCryptoSuites).
T1_FILTER="CryptoOptionsTest.GetSupportedDtlsSrtpCryptoSuites*"
# T2 (DTLS): rtc_base/ssl_stream_adapter_unittest.cc's value-parameterized
# DTLS-version fixture, run across DTLS 1.2 x 1.3 (client x server) - covers
# the version/cipher outcomes in the plan's §6.1 table, plus
# TestGetSslGroupIdWithPqc (X25519MLKEM768 negotiation, P7's other half).
# It does not cover the plan's stock-client/INCOMPATIBLE_CIPHERSUITE/
# QaudionHandshakeMeetsPolicy rows - those need real cross-version fixtures
# this file doesn't have; still open.
T2_FILTER="*SSLStreamAdapterTestDTLSHandshakeVersion*"
# T3 (FEC floor): closest existing coverage for the packet-loss-rate bound
# P5's OpusMinPacketLossPercent() feeds into (0.2 = 20%, the same ceiling
# P5 clamps its own floor to).
T3_FILTER="*AudioEncoderOpusTest.PacketLossRateUpperBounded*"
# T4 (decoder PLC): closest existing coverage that exercises the PLC/FEC
# decode paths P4b's WebRtcOpus_Decode wraps (deep PLC/OSCE themselves are
# only compiled in on arm64/x64, see P4a).
T4_FILTER="*AudioDecoderOpusTest*Plc*:*AudioDecoderOpusTest*Fec*"
# T5 (encoder complexity): direct existing coverage for
# AudioEncoderOpusImpl::GetNewComplexity(), which P5's qaudion override
# sits next to (SetTargetBitrate must not clobber an active override).
T5_FILTER="AudioEncoderOpusTest.ConfigComplexityAdaptation"
# T6 (BuildInfo/transport markers, runtime tuning API): rtc_base/
# qaudion_tuning_unittest.cc, added to rtc_base_unittests by m150/ci/
# apply-tests.py (the tests are not part of the shipped patch series). Both
# configs: the marker and the pinned/tighten-only semantics differ by
# QAUDION_TRANSPORT_STRICT inside the test itself.
T6_FILTER="QaudionTuning.*"
# T7-T9 (P12, frame anti-replay). Unlike T1/T2 these run in BOTH configs: the
# FrameCryptor code path is identical in strict and switchable builds.
# T7 (KAT): the real FrameCryptorTransformer opens every wire vector of the
# shared frame-crypto KAT (api/crypto/frame_crypto_kat_vectors.inc, generated
# by m150/ci/kat2inc.py from m150/kat/group-calls-v2-frame-crypto.json),
# re-seals every plaintext byte-for-byte and replays the replay scenarios.
T7_FILTER="FrameCryptorKat.*"
# T8 (replay window): window logic (reorder 255/256, duplicate, shift >= 256,
# top = 0xFFFFFFFF, cap 64, GC on key change, reflection) and real
# FrameCryptorTransformer pairs (duplicate dropped without any failure state,
# second receiver of a participant, counter continuity, exhaustion, H.264 IV
# that needs RBSP escaping).
T8_FILTER="FrameReplayWindow.*:FrameCryptorReplay.*"
# T9 (upstream gtests): the 7 pre-existing FrameCryptor / KeyProvider /
# DataPacketCryptor tests (compiled but never run before P12), moved to the
# 32-byte keys P1 requires, plus the P1/P12 key-provider checks.
T9_FILTER="FrameCryptor.KeyProvider:KeyProvider.*:DataPacketCryptor.*"
# T10 (P9): the certificate stats cache must not keep a pair taken before
# the remote certificate was known, plus the upstream cache test it must not
# break. Both configs.
T10_FILTER="RTCStatsCollectorTest*CertificateStatsCache*"
# T11 (strict transport, negative tests): a stock-like peer (DTLS 1.2 only,
# AES-128 SRTP only, no DTLS-SRTP) must NOT connect to a strict peer, plus the
# positive controls (strict peers negotiate DTLS 1.3 / TLS_AES_256_GCM_SHA384 /
# AEAD_AES_256_GCM / X25519MLKEM768) and the strict CryptoOptions. Spliced into
# ssl_stream_adapter_unittest.cc by m150/ci/apply-tests.py. Strict config only
# (the tests are compiled out of the switchable one).
T11_FILTER="*QaudionStrict*"

any=0
# <name>:<binary>:<filter>. In the strict config, T1/T2 are the upstream
# expectations that P6/P7 deliberately change (AES-128 / non-GCM SRTP suites,
# DTLS 1.2 handshakes), so they are expected to fail there by design and are
# run only in the switchable config, where level 0 must stay byte-for-byte
# upstream (LiveKit default).
for spec in "T1:rtc_unittests:$T1_FILTER" "T2:rtc_unittests:$T2_FILTER" \
            "T3:modules_unittests:$T3_FILTER" "T4:modules_unittests:$T4_FILTER" \
            "T5:modules_unittests:$T5_FILTER" "T6:rtc_unittests:$T6_FILTER" \
            "T7:rtc_unittests:$T7_FILTER" "T8:rtc_unittests:$T8_FILTER" \
            "T9:rtc_unittests:$T9_FILTER" \
            "T10:peerconnection_unittests:$T10_FILTER" \
            "T11:rtc_unittests:$T11_FILTER"; do
  name=${spec%%:*}
  rest=${spec#*:}
  bin=${rest%%:*}
  filt=${rest#*:}
  if [ -z "$filt" ]; then
    echo "::warning::$name: no gtest filter set yet - skipped"
    continue
  fi
  if [ "$CONFIG" = strict ] && { [ "$name" = T1 ] || [ "$name" = T2 ]; }; then
    echo "::notice::$name: upstream-behaviour test, intentionally changed by P6/P7 in strict builds - run in the switchable config only"
    continue
  fi
  if [ "$CONFIG" = switchable ] && [ "$name" = T11 ]; then
    echo "::notice::$name: strict-transport negative tests, compiled out of the switchable config - run in the strict config only"
    continue
  fi
  n=$("$OUT/$bin$EXE" "--gtest_filter=$filt" --gtest_list_tests | grep -c '^  ' || true)
  if [ "${n:-0}" -eq 0 ]; then
    echo "::error::$name: filter '$filt' matches no test in $bin - the check would silently pass"
    exit 1
  fi
  echo "::group::$name ($bin, $n tests, $filt)"
  "$OUT/$bin$EXE" "--gtest_filter=$filt"
  echo "::endgroup::"
  any=$((any + 1))
done
if [ "$any" -eq 0 ]; then
  echo "::warning::no T1-T11 filters ran - build-only smoke passed, no tests ran"
fi
echo "run-tests: $any filter group(s) passed ($CONFIG)"
