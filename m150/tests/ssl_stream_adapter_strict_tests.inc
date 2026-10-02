// ---------------------------------------------------------------------------
// Q-Audion strict-transport tests (T11). Spliced into
// rtc_base/ssl_stream_adapter_unittest.cc, inside its anonymous namespace, by
// m150/ci/apply-tests.py, so they run on the fixtures of that file.
//
// In a strict build every SSLStreamAdapter is hardened at compile time, so
// "a stock peer" cannot be built from the adapter itself. Two kinds of
// emulation are used, both at the SSL / SRTP configuration level:
//   * SRTP: the client adapter is configured with the DTLS-SRTP profile list
//     a stock peer would offer (AES-128 only, or no DTLS-SRTP at all) against
//     a strict server. The verdict checked is the SERVER's, which depends only
//     on the server's own policy check.
//   * DTLS version: a raw BoringSSL client (not an SSLStreamAdapter) capped at
//     DTLS 1.2 talks to the strict server through the same in-memory pipes.
// None of them may reach SS_OPEN.
// ---------------------------------------------------------------------------
#if QAUDION_TRANSPORT_STRICT && defined(OPENSSL_IS_BORINGSSL)

constexpr int kQaudionTlsAes256GcmSha384 = 0x1302;

class QaudionStrictDtlsTest : public SSLStreamAdapterTestDTLS {
 protected:
  // Both ends advertise the PQC hybrid group, exactly like DtlsTransport does
  // with the default CryptoOptions of a strict build.
  void EnableDefaultGroupsOnBothEnds() {
    CryptoOptions::EphemeralKeyExchangeCipherGroups groups;
    const std::vector<uint16_t> enabled = groups.GetEnabled();
    ASSERT_NE(std::find(
                  enabled.begin(), enabled.end(),
                  CryptoOptions::EphemeralKeyExchangeCipherGroups::
                      kX25519_MLKEM768),
              enabled.end())
        << "a strict build must offer X25519MLKEM768 by default";
    ASSERT_TRUE(client_ssl_->SetSslGroupIds(enabled));
    ASSERT_TRUE(server_ssl_->SetSslGroupIds(enabled));
  }
};

TEST_F(QaudionStrictDtlsTest, StrictPeersNegotiateTheHardenedTransport) {
  SetDtlsSrtpCryptoSuites({kSrtpAeadAes256Gcm}, true);
  SetDtlsSrtpCryptoSuites({kSrtpAeadAes256Gcm}, false);
  EnableDefaultGroupsOnBothEnds();
  TestHandshake();

  for (bool client : {true, false}) {
    int version = 0;
    ASSERT_TRUE(GetSslVersionBytes(client, &version));
    EXPECT_EQ(version, kDtls13VersionBytes);
    int cipher = 0;
    ASSERT_TRUE(GetSslCipherSuite(client, &cipher));
    EXPECT_EQ(cipher, kQaudionTlsAes256GcmSha384);
    int srtp = 0;
    ASSERT_TRUE(GetDtlsSrtpCryptoSuite(client, &srtp));
    EXPECT_EQ(srtp, kSrtpAeadAes256Gcm);
  }
  EXPECT_EQ(
      client_ssl_->GetSslGroupId(),
      CryptoOptions::EphemeralKeyExchangeCipherGroups::kX25519_MLKEM768);
  EXPECT_EQ(
      server_ssl_->GetSslGroupId(),
      CryptoOptions::EphemeralKeyExchangeCipherGroups::kX25519_MLKEM768);
}

// A peer that lists AES-256-GCM next to weaker suites still connects, and the
// strict side picks AES-256-GCM: interop is not lost, only the weak outcome.
TEST_F(QaudionStrictDtlsTest, PeerListingAes256AmongOthersConnectsWithAes256) {
  SetDtlsSrtpCryptoSuites(
      {kSrtpAes128CmSha1_80, kSrtpAeadAes128Gcm, kSrtpAeadAes256Gcm}, true);
  SetDtlsSrtpCryptoSuites({kSrtpAeadAes256Gcm}, false);
  TestHandshake();

  int client_cipher = 0;
  ASSERT_TRUE(GetDtlsSrtpCryptoSuite(true, &client_cipher));
  EXPECT_EQ(client_cipher, kSrtpAeadAes256Gcm);
  int server_cipher = 0;
  ASSERT_TRUE(GetDtlsSrtpCryptoSuite(false, &server_cipher));
  EXPECT_EQ(server_cipher, kSrtpAeadAes256Gcm);
}

// A stock peer that offers only AES-128 SRTP suites, or no DTLS-SRTP at all
// (the plaintext-RTP escape), must not get a connection out of a strict peer.
class QaudionStrictSrtpMismatchTest
    : public SSLStreamAdapterTestDTLS,
      public WithParamInterface<std::vector<int>> {};

TEST_P(QaudionStrictSrtpMismatchTest, PeerWithoutAes256SrtpCannotConnect) {
  if (!GetParam().empty()) {
    SetDtlsSrtpCryptoSuites(GetParam(), true);
  }
  SetDtlsSrtpCryptoSuites({kSrtpAeadAes256Gcm}, false);
  TestHandshake(/*expect_success=*/false);

  EXPECT_NE(server_ssl_->GetState(), SS_OPEN);
  EXPECT_NE(client_ssl_->GetState(), SS_OPEN);
  int suite = 0;
  EXPECT_FALSE(GetDtlsSrtpCryptoSuite(false, &suite))
      << "the strict side must never expose a negotiated SRTP suite";
}

INSTANTIATE_TEST_SUITE_P(
    QaudionStrictSrtp,
    QaudionStrictSrtpMismatchTest,
    Values(std::vector<int>{},
           std::vector<int>{kSrtpAes128CmSha1_80},
           std::vector<int>{kSrtpAes128CmSha1_32},
           std::vector<int>{kSrtpAeadAes128Gcm},
           std::vector<int>{kSrtpAes128CmSha1_80,
                            kSrtpAes128CmSha1_32,
                            kSrtpAeadAes128Gcm}));

// A minimal stock-like DTLS client built directly on BoringSSL: capped at the
// given version, offering the given DTLS-SRTP profiles, no client certificate.
class RawStockDtlsClient {
 public:
  RawStockDtlsClient(uint16_t max_version, const char* srtp_profiles)
      : ctx_(SSL_CTX_new(DTLS_method())) {
    RTC_CHECK(ctx_);
    RTC_CHECK(SSL_CTX_set_min_proto_version(ctx_, DTLS1_2_VERSION));
    RTC_CHECK(SSL_CTX_set_max_proto_version(ctx_, max_version));
    SSL_CTX_set_verify(ctx_, SSL_VERIFY_NONE, nullptr);
    // BoringSSL: zero is success for this one function.
    RTC_CHECK_EQ(SSL_CTX_set_tlsext_use_srtp(ctx_, srtp_profiles), 0);
    ssl_ = SSL_new(ctx_);
    RTC_CHECK(ssl_);
    rbio_ = BIO_new(BIO_s_mem());
    wbio_ = BIO_new(BIO_s_mem());
    RTC_CHECK(rbio_ && wbio_);
    SSL_set_bio(ssl_, rbio_, wbio_);  // the SSL owns both BIOs from here
    SSL_set_connect_state(ssl_);
  }
  ~RawStockDtlsClient() {
    SSL_free(ssl_);
    SSL_CTX_free(ctx_);
    ERR_clear_error();
  }

  // Advances the handshake and returns the bytes the client wants to send.
  std::vector<uint8_t> Pump() {
    if (!failed_ && !established_) {
      const int r = SSL_do_handshake(ssl_);
      if (r == 1) {
        established_ = true;
      } else {
        const int err = SSL_get_error(ssl_, r);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
          failed_ = true;
          failure_reason_ = ERR_GET_REASON(ERR_peek_error());
        }
      }
    }
    std::vector<uint8_t> out;
    const size_t pending = BIO_pending(wbio_);
    if (pending > 0) {
      out.resize(pending);
      const int n = BIO_read(wbio_, out.data(), static_cast<int>(pending));
      out.resize(n > 0 ? static_cast<size_t>(n) : 0);
    }
    return out;
  }
  void Receive(const uint8_t* data, size_t size) {
    BIO_write(rbio_, data, static_cast<int>(size));
  }
  bool failed() const { return failed_; }
  bool established() const { return established_; }
  int failure_reason() const { return failure_reason_; }

 private:
  SSL_CTX* ctx_;
  SSL* ssl_ = nullptr;
  BIO* rbio_ = nullptr;
  BIO* wbio_ = nullptr;
  bool failed_ = false;
  bool established_ = false;
  int failure_reason_ = 0;
};

// A stock peer limited to DTLS 1.2 (with AES-128 SRTP, as a stock peer of
// that era offers) must be refused at the hello with a protocol_version
// alert; the strict server never reaches SS_OPEN.
TEST_F(QaudionStrictDtlsTest, StockDtls12ClientIsRefusedByAProtocolVersionAlert) {
  RawStockDtlsClient stock(DTLS1_2_VERSION, "SRTP_AES128_CM_SHA1_80");
  SetDtlsSrtpCryptoSuites({kSrtpAeadAes256Gcm}, false);
  SetPeerIdentitiesByDigest(true, true);
  // The fixture's own (never started) client adapter would also read from the
  // pipe the raw client reads from; it is not part of this test.
  client_ssl_.reset();
  server_ssl_->SetServerRole();
  ASSERT_EQ(0, server_ssl_->StartSSL());

  auto exchange = [&]() {
    const std::vector<uint8_t> out = stock.Pump();
    if (!out.empty()) {
      size_t written = 0;
      int error = 0;
      server_buffer_.Write(out, written, error);
    }
    uint8_t buffer[4096];
    size_t read = 0;
    int error = 0;
    while (client_buffer_.Read(buffer, read, error) == SR_SUCCESS) {
      stock.Receive(buffer, read);
    }
  };
  EXPECT_THAT(WaitUntil(
                  [&] {
                    exchange();
                    exchange();
                    return stock.failed() ||
                           server_ssl_->GetState() == SS_CLOSED;
                  },
                  ::testing::IsTrue(),
                  {.timeout = TimeDelta::Millis(5000),
                   .clock = &time_controller_}),
              IsRtcOk());

  EXPECT_FALSE(stock.established());
  EXPECT_TRUE(stock.failed());
  EXPECT_EQ(stock.failure_reason(), SSL_R_TLSV1_ALERT_PROTOCOL_VERSION);
  EXPECT_NE(server_ssl_->GetState(), SS_OPEN);
}

#endif  // QAUDION_TRANSPORT_STRICT && OPENSSL_IS_BORINGSSL

// T11b: the crypto options a strict build hands to every PeerConnection.
// Cheap, no handshake: runs on every platform of the strict configuration.
#if QAUDION_TRANSPORT_STRICT
TEST(QaudionStrictCryptoOptions, OnlyAes256GcmIsOfferedWhateverTheToggles) {
  CryptoOptions options;
  options.srtp.enable_gcm_crypto_suites = true;
  options.srtp.enable_aes128_sha1_32_crypto_cipher = true;
  options.srtp.enable_aes128_sha1_80_crypto_cipher = true;
  options.srtp.enable_encrypted_rtp_header_extensions = true;
  const std::vector<int> suites = options.GetSupportedDtlsSrtpCryptoSuites();
  ASSERT_EQ(suites.size(), 1u);
  EXPECT_EQ(suites[0], kSrtpAeadAes256Gcm);

  CryptoOptions all_off;
  all_off.srtp.enable_gcm_crypto_suites = false;
  all_off.srtp.enable_aes128_sha1_32_crypto_cipher = false;
  all_off.srtp.enable_aes128_sha1_80_crypto_cipher = false;
  const std::vector<int> still = all_off.GetSupportedDtlsSrtpCryptoSuites();
  ASSERT_EQ(still.size(), 1u);
  EXPECT_EQ(still[0], kSrtpAeadAes256Gcm);
}

TEST(QaudionStrictCryptoOptions, CryptexIsNegotiatedByDefault) {
  CryptoOptions options;
  EXPECT_EQ(options.srtp.cryptex_policy,
            CryptoOptions::Srtp::CryptexPolicy::kNegotiate);
}

TEST(QaudionStrictCryptoOptions, PqcHybridGroupIsEnabledByDefault) {
  CryptoOptions options;
  const std::vector<uint16_t> enabled =
      options.ephemeral_key_exchange_cipher_groups.GetEnabled();
  EXPECT_NE(std::find(
                enabled.begin(), enabled.end(),
                CryptoOptions::EphemeralKeyExchangeCipherGroups::
                    kX25519_MLKEM768),
            enabled.end());
}
#endif  // QAUDION_TRANSPORT_STRICT
