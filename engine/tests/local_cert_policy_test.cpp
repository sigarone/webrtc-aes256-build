// The local certificate lookup of the engine must never end a legitimate call by a short timer
// (WIRE_SPEC section 3.8.4: retry every 250 ms for up to CONFIRM_TIMEOUT = 15 s after each
// transition to connected). The decision logic lives in src/local_cert_policy.h without libwebrtc,
// so this test runs everywhere with a made-up clock: it pins the constants and replays the retry
// loop against a fake statistics source that shows the certificate only from the Nth request on.
#include <cstdint>
#include <functional>

#include "local_cert_policy.h"
#include "testing.h"

namespace {

using qmedia::engine::CertDigest;
using qmedia::engine::kConfirmTimeoutMs;
using qmedia::engine::kLocalCertRetryMs;
using qmedia::engine::LocalCertAction;
using qmedia::engine::LocalCertScan;
using qmedia::engine::LocalCertStep;
using qmedia::engine::NextLocalCertStep;

CertDigest Digest(uint8_t fill) {
  CertDigest d{};
  d.fill(fill);
  return d;
}

struct Outcome {
  LocalCertAction action = LocalCertAction::kRetry;
  int requests = 0;    // statistics requests made
  int64_t at_ms = -1;  // time of the verdict, counted from the transition to connected
};

// Replays the lookup the way Peer runs it: a request at once, then one per delay the policy names.
// source(n, &scan) fills the scan that the n-th request (1-based) returns.
Outcome Replay(const std::function<void(int, LocalCertScan*)>& source) {
  Outcome o;
  int64_t now = 0;
  for (;;) {
    ++o.requests;
    LocalCertScan scan;
    source(o.requests, &scan);
    const LocalCertStep step = NextLocalCertStep(scan, now);
    if (step.action != LocalCertAction::kRetry) {
      o.action = step.action;
      o.at_ms = now;
      return o;
    }
    CHECK(step.delay_ms > 0);
    CHECK(step.delay_ms <= kLocalCertRetryMs);
    now += step.delay_ms;
    if (o.requests > 100000) {  // a policy that never ends is a failure, not a hang
      CHECK(false);
      return o;
    }
  }
}

}  // namespace

QTEST(local_cert_constants_are_pinned) {
  // One named constant for the window, the value of WIRE_SPEC section 3.8.4 CONFIRM_TIMEOUT.
  CHECK_EQ(kConfirmTimeoutMs, 15000);
  CHECK_EQ(kLocalCertRetryMs, 250);
}

QTEST(local_cert_found_on_first_request_is_emitted_at_once) {
  const Outcome o = Replay([](int, LocalCertScan* s) { s->Add(Digest(7)); });
  CHECK(o.action == LocalCertAction::kEmit);
  CHECK_EQ(o.requests, 1);
  CHECK_EQ(o.at_ms, 0);
}

QTEST(local_cert_found_on_the_nth_request) {
  // The collector shows nothing for 39 requests (9.75 s), then the certificate.
  const Outcome o = Replay([](int n, LocalCertScan* s) {
    if (n >= 40) s->Add(Digest(1));
  });
  CHECK(o.action == LocalCertAction::kEmit);
  CHECK_EQ(o.requests, 40);
  CHECK_EQ(o.at_ms, 39 * kLocalCertRetryMs);
}

QTEST(local_cert_slow_collector_far_beyond_three_seconds_is_not_a_violation) {
  // The old 3 s cutoff would have ended this call.
  const Outcome o = Replay([](int n, LocalCertScan* s) {
    if (n >= 50) s->Add(Digest(2));  // 12.25 s
  });
  CHECK(o.action == LocalCertAction::kEmit);
  CHECK(o.at_ms > 3000);
  CHECK(o.at_ms < kConfirmTimeoutMs);
}

QTEST(local_cert_found_on_the_last_request_at_the_window_end) {
  // Requests at 0, 250, ..., 15000 ms: the 61st is the last, and it still counts.
  const Outcome o = Replay([](int n, LocalCertScan* s) {
    if (n >= 61) s->Add(Digest(3));
  });
  CHECK(o.action == LocalCertAction::kEmit);
  CHECK_EQ(o.requests, 61);
  CHECK_EQ(o.at_ms, kConfirmTimeoutMs);
}

QTEST(local_cert_never_found_ends_only_when_the_window_is_over) {
  const Outcome o = Replay([](int, LocalCertScan*) {});
  CHECK(o.action == LocalCertAction::kViolationTimeout);
  CHECK_EQ(o.requests, 61);
  CHECK_EQ(o.at_ms, kConfirmTimeoutMs);  // never earlier
}

QTEST(local_cert_window_edges) {
  const LocalCertScan none;
  LocalCertStep s = NextLocalCertStep(none, 0);
  CHECK(s.action == LocalCertAction::kRetry);
  CHECK_EQ(s.delay_ms, kLocalCertRetryMs);
  s = NextLocalCertStep(none, 3000);  // the old cutoff: nothing happens
  CHECK(s.action == LocalCertAction::kRetry);
  s = NextLocalCertStep(none, kConfirmTimeoutMs - 1);
  CHECK(s.action == LocalCertAction::kRetry);
  CHECK_EQ(s.delay_ms, 1);  // the last request lands exactly on the window's end
  s = NextLocalCertStep(none, kConfirmTimeoutMs - 100);
  CHECK(s.action == LocalCertAction::kRetry);
  CHECK_EQ(s.delay_ms, 100);
  s = NextLocalCertStep(none, kConfirmTimeoutMs);
  CHECK(s.action == LocalCertAction::kViolationTimeout);
  s = NextLocalCertStep(none, kConfirmTimeoutMs + 5000);
  CHECK(s.action == LocalCertAction::kViolationTimeout);
  s = NextLocalCertStep(none, -50);  // a clock that stepped back is treated as the window's start
  CHECK(s.action == LocalCertAction::kRetry);
}

QTEST(local_cert_each_connected_transition_opens_its_own_window) {
  // The first transition is at 0 ms and the certificate never shows. At 10 s the connection goes
  // through disconnected and connected again: the lookup measures from 10 s, so it ends at 25 s of
  // call time, not at 15 s.
  int64_t window_start = 0;
  int64_t now = 0;
  int requests = 0;
  LocalCertAction last = LocalCertAction::kRetry;
  bool restarted = false;
  while (last == LocalCertAction::kRetry) {
    if (now >= 10000 && !restarted) {
      window_start = now;
      restarted = true;
    }
    ++requests;
    const LocalCertStep step = NextLocalCertStep(LocalCertScan(), now - window_start);
    last = step.action;
    if (last == LocalCertAction::kRetry) now += step.delay_ms;
    CHECK(requests < 1000);
    if (requests >= 1000) break;
  }
  CHECK(last == LocalCertAction::kViolationTimeout);
  CHECK_EQ(now, 10000 + kConfirmTimeoutMs);
}

QTEST(local_cert_two_different_certificates_are_immediate) {
  // The first request already shows two transports with different certificates.
  Outcome o = Replay([](int, LocalCertScan* s) {
    s->Add(Digest(1));
    s->Add(Digest(2));
  });
  CHECK(o.action == LocalCertAction::kViolationConflict);
  CHECK_EQ(o.requests, 1);
  CHECK_EQ(o.at_ms, 0);
  // Also when it only shows up after a few empty requests: no waiting for the window.
  o = Replay([](int n, LocalCertScan* s) {
    if (n >= 5) {
      s->Add(Digest(1));
      s->Add(Digest(2));
    }
  });
  CHECK(o.action == LocalCertAction::kViolationConflict);
  CHECK_EQ(o.requests, 5);
  CHECK_EQ(o.at_ms, 4 * kLocalCertRetryMs);
}

QTEST(local_cert_scan_rules) {
  LocalCertScan s;
  CHECK(s.state() == LocalCertScan::State::kNone);
  s.Add(Digest(9));
  CHECK(s.state() == LocalCertScan::State::kFound);
  CHECK(s.digest() == Digest(9));
  s.Add(Digest(9));  // the same certificate on a second transport is the normal case
  CHECK(s.state() == LocalCertScan::State::kFound);
  s.Add(Digest(8));
  CHECK(s.state() == LocalCertScan::State::kConflict);
  s.Add(Digest(9));  // a conflict stays a conflict
  CHECK(s.state() == LocalCertScan::State::kConflict);

  LocalCertScan u;
  u.Add(Digest(1));
  u.AddUndecodable();
  CHECK(u.state() == LocalCertScan::State::kUndecodable);
  u.Add(Digest(1));
  CHECK(u.state() == LocalCertScan::State::kUndecodable);
  CHECK(NextLocalCertStep(u, 0).action == LocalCertAction::kViolationInvalid);
}
