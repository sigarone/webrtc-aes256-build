/*
 *  Q-Audion T6: the runtime tuning API and the build marker of
 *  rtc_base/qaudion_tuning.{h,cc} (patch P8). Copied into the checkout's
 *  rtc_base/ and added to rtc_base_unittests by m150/ci/apply-tests.py.
 *
 *  The tuning state is process-wide, so every test restores what it changes;
 *  the one-way (tighten-only) setters are not called here at all, because
 *  they cannot be undone for the other tests running in the same binary.
 */
#include "rtc_base/qaudion_tuning.h"

#include <cstdint>
#include <string>

#include "test/gtest.h"

namespace webrtc {
namespace {

// Restores every reversible knob when a test ends, however it ends.
class TuningRestorer {
 public:
  TuningRestorer()
      : enc_(qaudion::OpusEncoderComplexity()),
        dec_(qaudion::OpusDecoderComplexity()),
        loss_(qaudion::OpusMinPacketLossPercent()) {}
  ~TuningRestorer() {
    qaudion::SetOpusEncoderComplexity(enc_);
    qaudion::SetOpusDecoderComplexity(dec_);
    qaudion::SetOpusMinPacketLossPercent(loss_);
  }

 private:
  int enc_;
  int dec_;
  int loss_;
};

TEST(QaudionTuning, BuildInfoCarriesTheTransportMarker) {
  const std::string info = qaudion::BuildInfo();
#if QAUDION_TRANSPORT_STRICT
  // The release gate G2 greps the shipped binary for exactly this literal.
  EXPECT_NE(info.find("Q-AUDION build m150 transport=strict"),
            std::string::npos)
      << info;
  EXPECT_EQ(info.find("transport=switchable"), std::string::npos) << info;
  EXPECT_NE(info.find(" level=3"), std::string::npos) << info;
#else
  EXPECT_NE(info.find("Q-AUDION build m150 transport=switchable"),
            std::string::npos)
      << info;
  EXPECT_EQ(info.find("transport=strict"), std::string::npos) << info;
#endif
  EXPECT_NE(info.find(" pqc_required="), std::string::npos) << info;
  EXPECT_NE(info.find(" magic_bypass_disabled="), std::string::npos) << info;
  EXPECT_NE(info.find(" enc_cx="), std::string::npos) << info;
  EXPECT_NE(info.find(" dec_cx="), std::string::npos) << info;
  EXPECT_NE(info.find(" min_loss_pct="), std::string::npos) << info;
}

TEST(QaudionTuning, BuildInfoIsASingleLineOfPlainTokens) {
  const std::string info = qaudion::BuildInfo();
  EXPECT_EQ(info.find('\n'), std::string::npos);
  for (char c : info) {
    // Ids and small integers only: no byte outside printable ASCII can be a
    // key, a hash or a host name.
    EXPECT_GE(c, 0x20);
    EXPECT_LT(c, 0x7f);
  }
  EXPECT_LT(info.size(), 200u);
}

#if QAUDION_TRANSPORT_STRICT
TEST(QaudionTuning, StrictBuildPinsTheTransportPolicy) {
  EXPECT_EQ(qaudion::TransportLevel(), 3);
  // Nothing the application does can loosen or lower it.
  qaudion::RaiseTransportLevel(0);
  qaudion::RaiseTransportLevel(1);
  qaudion::RaiseTransportLevel(3);
  qaudion::RaiseTransportLevel(-1);
  qaudion::RaiseTransportLevel(99);
  EXPECT_EQ(qaudion::TransportLevel(), 3);
  // The unauthenticated frame-cryptor magic-bytes bypass is never allowed,
  // even though nobody called DisableMagicBytesBypass().
  EXPECT_FALSE(qaudion::MagicBytesBypassAllowed());
}
#else
TEST(QaudionTuning, SwitchableBuildStartsAtUpstreamBehaviour) {
  // Only meaningful before any other test raised the level in this process;
  // the strict config is the shipped one, this just keeps the filter
  // non-empty in the switchable config of the Linux workflow.
  EXPECT_GE(qaudion::TransportLevel(), 0);
  EXPECT_LE(qaudion::TransportLevel(), 3);
}
#endif

TEST(QaudionTuning, OpusComplexitySettersClampAndKeepTheLastValidValue) {
  TuningRestorer restore;
  qaudion::SetOpusEncoderComplexity(7);
  EXPECT_EQ(qaudion::OpusEncoderComplexity(), 7);
  // Out of range is ignored, never an RTC_CHECK and never stored.
  qaudion::SetOpusEncoderComplexity(11);
  EXPECT_EQ(qaudion::OpusEncoderComplexity(), 7);
  qaudion::SetOpusEncoderComplexity(-2);
  EXPECT_EQ(qaudion::OpusEncoderComplexity(), 7);
  // -1 means "no override".
  qaudion::SetOpusEncoderComplexity(-1);
  EXPECT_EQ(qaudion::OpusEncoderComplexity(), -1);
  qaudion::SetOpusEncoderComplexity(0);
  EXPECT_EQ(qaudion::OpusEncoderComplexity(), 0);
  qaudion::SetOpusEncoderComplexity(10);
  EXPECT_EQ(qaudion::OpusEncoderComplexity(), 10);

  qaudion::SetOpusDecoderComplexity(5);
  EXPECT_EQ(qaudion::OpusDecoderComplexity(), 5);
  qaudion::SetOpusDecoderComplexity(1000);
  EXPECT_EQ(qaudion::OpusDecoderComplexity(), 5);
  qaudion::SetOpusDecoderComplexity(-1);
  EXPECT_EQ(qaudion::OpusDecoderComplexity(), -1);
}

TEST(QaudionTuning, DecoderComplexityDefaultEnablesDeepPlc) {
  // 5 is the first complexity at which libopus runs deep PLC.
  EXPECT_GE(qaudion::kQaudionDefaultDecoderComplexity, 5);
  EXPECT_LE(qaudion::kQaudionDefaultDecoderComplexity, 10);
}

TEST(QaudionTuning, MinPacketLossPercentIsClampedToTheFecCeiling) {
  TuningRestorer restore;
  qaudion::SetOpusMinPacketLossPercent(10);
  EXPECT_EQ(qaudion::OpusMinPacketLossPercent(), 10);
  qaudion::SetOpusMinPacketLossPercent(20);
  EXPECT_EQ(qaudion::OpusMinPacketLossPercent(), 20);
  qaudion::SetOpusMinPacketLossPercent(21);
  EXPECT_EQ(qaudion::OpusMinPacketLossPercent(), 20);
  qaudion::SetOpusMinPacketLossPercent(-1);
  EXPECT_EQ(qaudion::OpusMinPacketLossPercent(), 20);
  qaudion::SetOpusMinPacketLossPercent(0);
  EXPECT_EQ(qaudion::OpusMinPacketLossPercent(), 0);
}

TEST(QaudionTuning, GenerationMovesOnlyWhenAStoredValueChanges) {
  TuningRestorer restore;
  qaudion::SetOpusMinPacketLossPercent(12);
  const uint32_t g0 = qaudion::TuningGeneration();
  // Same value again, out-of-range values and refused values do not bump it.
  qaudion::SetOpusMinPacketLossPercent(12);
  qaudion::SetOpusMinPacketLossPercent(50);
  qaudion::SetOpusEncoderComplexity(42);
  EXPECT_EQ(qaudion::TuningGeneration(), g0);
  // A real change does.
  qaudion::SetOpusMinPacketLossPercent(13);
  EXPECT_NE(qaudion::TuningGeneration(), g0);
}

}  // namespace
}  // namespace webrtc
