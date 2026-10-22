// Q-Audion Opus band-limit tests (T12). Spliced into
// modules/audio_coding/codecs/opus/audio_encoder_opus_unittest.cc, inside
// namespace webrtc, by m150/ci/apply-tests.py (the tests are not part of the
// shipped patch series). They use the helpers of that file's anonymous
// namespace (kInitialTimeUs, kDefaultOpusPayloadType) and the includes
// apply-tests.py adds (<cmath>, rtc_base/qaudion_tuning.h).
//
// What they pin (patch P13): with the app's wire policy (mono, 60 ms, 32 kbps
// CBR, in-band FEC on, DTX off) a cap on the coded band makes the real encoder
// emit narrowband packets, lifting the cap lets it leave narrowband again, the
// SDP maxplaybackrate stays a hard ceiling, and through all of it every packet
// has the same size (CBR). The tuning state is process-wide, so each test
// restores it.
//
// libopus does not jump to a new band mid-call: its SILK layer changes its
// internal sample rate one step at a time (wideband, mediumband, narrowband)
// and only on frames without speech, which takes seconds, in both directions.
// So the tests either set the cap before the encoder exists (narrowband from
// the first packets) or give it up to 300 packets (18 s) for a change in the
// middle of a call, and the test signal has the pauses of real speech.
//
// Packet bands are read from the Opus TOC byte (RFC 6716 section 3.1): the
// config number is toc >> 3; 0..3 SILK narrowband, 4..7 SILK mediumband,
// 8..11 SILK wideband, 12..13 hybrid super-wideband, 14..15 hybrid fullband,
// 16..19 CELT narrowband, 20..23 CELT wideband, 24..27 CELT super-wideband,
// 28..31 CELT fullband.

class QaudionOpusBandLimit : public ::testing::Test {
 protected:
  static constexpr int kPacketMs = 60;
  static constexpr int kBitrateBps = 32000;
  // 32 kbps for 60 ms, in bytes: what a CBR packet must always be.
  static constexpr size_t kCbrPacketBytes = kBitrateBps / 8 * kPacketMs / 1000;

  QaudionOpusBandLimit() : saved_max_rate_(qaudion::OpusEncoderMaxPlaybackRate()) {
    qaudion::SetOpusEncoderMaxPlaybackRate(-1);
  }
  ~QaudionOpusBandLimit() override {
    qaudion::SetOpusEncoderMaxPlaybackRate(saved_max_rate_);
  }

  std::unique_ptr<AudioEncoderOpusImpl> MakeWireEncoder(int sdp_max_rate_hz) {
    AudioEncoderOpusConfig config;
    config.frame_size_ms = kPacketMs;
    config.sample_rate_hz = 48000;
    config.num_channels = 1;
    config.application = AudioEncoderOpusConfig::ApplicationMode::kVoip;
    config.bitrate_bps = kBitrateBps;
    config.cbr_enabled = true;
    config.fec_enabled = true;
    config.dtx_enabled = false;
    config.max_playback_rate_hz = sdp_max_rate_hz;
    config.supported_frame_lengths_ms.push_back(kPacketMs);
    return std::make_unique<AudioEncoderOpusImpl>(
        CreateTestEnvironment({.time = &time_controller_}), std::move(config),
        kDefaultOpusPayloadType);
  }

  // A voiced-speech-like test signal below 3.2 kHz (harmonics of a gliding
  // 120..170 Hz pitch, slow syllable-like level changes, a pause of 300 ms in
  // every 1.2 s, a little noise), 10 ms at a time, deterministic.
  std::vector<int16_t> NextBlock10ms() {
    constexpr int kSamples = 480;
    constexpr double kTwoPi = 6.283185307179586;
    std::vector<int16_t> block(kSamples);
    for (int i = 0; i < kSamples; ++i) {
      const double t = static_cast<double>(sample_pos_) / 48000.0;
      const double f0 = 145.0 + 25.0 * std::sin(kTwoPi * 0.7 * t);
      phase_ += kTwoPi * f0 / 48000.0;
      double s = 0;
      for (int h = 1; h <= 20; ++h) {
        s += std::sin(h * phase_) / h;
      }
      double level =
          0.5 + 0.5 * std::sin(kTwoPi * 3.0 * t);  // 3 syllables per second
      if (std::fmod(t, 1.2) > 0.9) {
        level = 0;  // the pause: only the noise below is left
      }
      noise_state_ = noise_state_ * 1664525u + 1013904223u;
      const double noise =
          (static_cast<double>(noise_state_ >> 16) / 65536.0 - 0.5) * 0.02;
      block[i] = static_cast<int16_t>(
          std::lround(6000.0 * level * s + 32768.0 * noise));
      ++sample_pos_;
    }
    return block;
  }

  struct Packet {
    size_t bytes;
    int toc_config;
  };

  // Feeds 10 ms blocks until `count` packets came out; returns them.
  std::vector<Packet> EncodePackets(AudioEncoderOpusImpl& encoder, int count) {
    std::vector<Packet> packets;
    Buffer encoded;
    while (static_cast<int>(packets.size()) < count) {
      for (int block = 0; block < kPacketMs / 10; ++block) {
        const std::vector<int16_t> audio = NextBlock10ms();
        encoded.Clear();
        const AudioEncoder::EncodedInfo info = encoder.Encode(
            rtp_timestamp_, std::span<const int16_t>(audio), &encoded);
        rtp_timestamp_ += audio.size();
        if (info.encoded_bytes > 0) {
          packets.push_back(
              {info.encoded_bytes, static_cast<int>(encoded.data()[0] >> 3)});
        }
      }
    }
    return packets;
  }

  // 0 narrowband, 1 mediumband, 2 wideband, 3 super-wideband, 4 fullband.
  static int BandOf(int toc_config) {
    if (toc_config < 12) {
      return toc_config / 4;
    }
    if (toc_config < 16) {
      return toc_config < 14 ? 3 : 4;
    }
    constexpr int kCelt[4] = {0, 2, 3, 4};
    return kCelt[(toc_config - 16) / 4];
  }

  GlobalSimulatedTimeController time_controller_{
      Timestamp::Micros(kInitialTimeUs)};

 private:
  const int saved_max_rate_;
  uint32_t rtp_timestamp_ = 0;
  uint32_t sample_pos_ = 0;
  uint32_t noise_state_ = 12345;
  double phase_ = 0;
};

TEST_F(QaudionOpusBandLimit, ACapSetBeforeTheCallIsNarrowbandFromTheStart) {
  qaudion::SetOpusEncoderMaxPlaybackRate(8000);
  auto encoder = MakeWireEncoder(/*sdp_max_rate_hz=*/48000);
  const std::vector<Packet> packets = EncodePackets(*encoder, 30);
  for (size_t i = 0; i < packets.size(); ++i) {
    EXPECT_EQ(packets[i].bytes, kCbrPacketBytes);
    EXPECT_EQ(BandOf(packets[i].toc_config), 0)
        << "packet " << i << " toc config " << packets[i].toc_config;
  }
}

TEST_F(QaudionOpusBandLimit, AWidebandCapNeverCodesAboveWideband) {
  qaudion::SetOpusEncoderMaxPlaybackRate(16000);
  auto encoder = MakeWireEncoder(/*sdp_max_rate_hz=*/48000);
  const std::vector<Packet> packets = EncodePackets(*encoder, 30);
  for (size_t i = 0; i < packets.size(); ++i) {
    EXPECT_EQ(packets[i].bytes, kCbrPacketBytes);
    EXPECT_LE(BandOf(packets[i].toc_config), 2)
        << "packet " << i << " toc config " << packets[i].toc_config;
  }
}

TEST_F(QaudionOpusBandLimit, TheSdpLimitStaysAHardCeiling) {
  // The remote asked for narrowband (maxplaybackrate=8000): an app limit that
  // is higher, or no limit at all, must not give it more than that.
  for (int app_limit_hz : {-1, 16000, 48000}) {
    qaudion::SetOpusEncoderMaxPlaybackRate(app_limit_hz);
    auto encoder = MakeWireEncoder(/*sdp_max_rate_hz=*/8000);
    const std::vector<Packet> packets = EncodePackets(*encoder, 20);
    for (size_t i = 0; i < packets.size(); ++i) {
      EXPECT_EQ(packets[i].bytes, kCbrPacketBytes);
      EXPECT_EQ(BandOf(packets[i].toc_config), 0)
          << "app limit " << app_limit_hz << " packet " << i;
    }
  }
}

TEST_F(QaudionOpusBandLimit, ACapInTheMiddleOfACallTakesHoldAndCanBeLifted) {
  auto encoder = MakeWireEncoder(/*sdp_max_rate_hz=*/48000);

  // No cap: at 32 kbps CBR libopus never picks narrowband on its own.
  for (const Packet& p : EncodePackets(*encoder, 20)) {
    EXPECT_EQ(p.bytes, kCbrPacketBytes);
    EXPECT_GE(BandOf(p.toc_config), 2) << "toc config " << p.toc_config;
  }

  // The app caps the band at 8 kHz (narrowband): within 18 s the packets are
  // narrowband, and not one of them changes size on the way.
  qaudion::SetOpusEncoderMaxPlaybackRate(8000);
  const std::vector<Packet> capped = EncodePackets(*encoder, 300);
  for (const Packet& p : capped) {
    EXPECT_EQ(p.bytes, kCbrPacketBytes);
  }
  for (size_t i = capped.size() - 20; i < capped.size(); ++i) {
    EXPECT_EQ(BandOf(capped[i].toc_config), 0)
        << "packet " << i << " toc config " << capped[i].toc_config;
  }

  // Lifting the cap lets the encoder leave narrowband again.
  qaudion::SetOpusEncoderMaxPlaybackRate(-1);
  const std::vector<Packet> lifted = EncodePackets(*encoder, 300);
  bool left_narrowband = false;
  for (const Packet& p : lifted) {
    EXPECT_EQ(p.bytes, kCbrPacketBytes);
    left_narrowband = left_narrowband || BandOf(p.toc_config) > 0;
  }
  EXPECT_TRUE(left_narrowband);
}
