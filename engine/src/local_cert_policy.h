// When the engine may give up looking for the local DTLS certificate. No libwebrtc, no I/O, no
// clock: the decisions are pure functions of what the last statistics report showed and of the time
// that has passed, so the tests can drive them with a made-up clock (engine/tests/local_cert_policy_test.cpp).
//
// The rule behind it (owner's rule of 2026-10-02, WIRE_SPEC §3.8.4): no legitimate call is ended by
// a short timer. The DTLS check of a call retries every kLocalCertRetryMs for up to
// kConfirmTimeoutMs after EACH transition to connected, and every transition opens its own window.
// A loaded process or a slow statistics collector therefore costs time, never the call. Only two
// things end the lookup before the window is over, and both are findings, not delays: a certificate
// that cannot be decoded, and two transports that report different certificates.
#pragma once

#include <array>
#include <cstdint>

namespace qmedia::engine {

// WIRE_SPEC §3.8.4 CONFIRM_TIMEOUT: how long after each transition to connected the DTLS check may
// keep looking before it gives the verdict "no DTLS identity". One constant for the whole engine;
// the other timers of the engine's host use the same 15 s.
inline constexpr int64_t kConfirmTimeoutMs = 15000;

// Pause between two statistics requests of one lookup.
inline constexpr int64_t kLocalCertRetryMs = 250;

using CertDigest = std::array<uint8_t, 32>;

// What one statistics report says about the local certificate of the connection's transports.
class LocalCertScan {
 public:
  enum class State { kNone, kFound, kConflict, kUndecodable };

  // One transport's certificate, as its SHA-256.
  void Add(const CertDigest& digest) {
    if (state_ == State::kConflict || state_ == State::kUndecodable) return;
    if (state_ == State::kNone) {
      digest_ = digest;
      state_ = State::kFound;
    } else if (digest_ != digest) {
      state_ = State::kConflict;
    }
  }

  // A certificate entry that is present but cannot be turned into a digest.
  void AddUndecodable() { state_ = State::kUndecodable; }

  State state() const { return state_; }
  const CertDigest& digest() const { return digest_; }

 private:
  State state_ = State::kNone;
  CertDigest digest_{};
};

enum class LocalCertAction {
  kEmit,              // the certificate is known: report transport_info
  kRetry,             // not there yet: ask again after `delay_ms`
  kViolationConflict, // two transports report different certificates: immediate violation
  kViolationInvalid,  // a certificate that cannot be decoded: immediate violation
  kViolationTimeout,  // no certificate during the whole window: violation
};

struct LocalCertStep {
  LocalCertAction action;
  int64_t delay_ms;  // only for kRetry
};

// `elapsed_ms` is the time since the latest transition to connected. While the window is open and
// nothing was found the answer is "retry", with a delay that ends exactly on the window's end at
// the latest, so a last request is always made at or after kConfirmTimeoutMs: the lookup never
// gives up before the window is over.
inline LocalCertStep NextLocalCertStep(const LocalCertScan& scan, int64_t elapsed_ms) {
  switch (scan.state()) {
    case LocalCertScan::State::kFound:
      return {LocalCertAction::kEmit, 0};
    case LocalCertScan::State::kConflict:
      return {LocalCertAction::kViolationConflict, 0};
    case LocalCertScan::State::kUndecodable:
      return {LocalCertAction::kViolationInvalid, 0};
    case LocalCertScan::State::kNone:
      break;
  }
  if (elapsed_ms < 0) elapsed_ms = 0;
  if (elapsed_ms >= kConfirmTimeoutMs) return {LocalCertAction::kViolationTimeout, 0};
  const int64_t left = kConfirmTimeoutMs - elapsed_ms;
  return {LocalCertAction::kRetry, left < kLocalCertRetryMs ? left : kLocalCertRetryMs};
}

}  // namespace qmedia::engine
