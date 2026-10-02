// Frame-key store of one engine session.
//
// One libwebrtc key provider per SESSION, never per peer connection, so that its replay windows
// (patch P12 keeps them inside the provider's per-participant key handlers) survive the
// re-creation of a peer connection. The provider is configured exactly like the one the mobile
// apps build for a 1:1 call:
//
//   shared key mode off (per-participant keys), empty ratchet salt, ratchet window 0, no
//   uncrypted magic bytes, failure tolerance -1, key ring of 16 slots, frames are discarded while
//   the cryptor has no key, key derivation HKDF.
//
// Android: PeerConnectionHolder.kt createFrameCryptorKeyProvider (call at line 2615 of main
// 4f6b3983). iOS: NativeAudioFrameCryptor.swift RTCFrameCryptorKeyProvider (line 90 of main
// 300ce640). Those values are protocol constants, not secrets.
//
// The host names participants freely. The store keeps the two directions apart by prefixing the
// participant id inside the provider ("s:" for keys of the sending direction, "r:" for keys of the
// receiving direction), so one participant id may hold a send key and a receive key at the same
// ring slot without one overwriting the other.
//
// Key hygiene: key bytes are copied exactly once into a local buffer, handed to the provider and
// the local copy is zeroed with SecureZero immediately. Retiring a slot overwrites it with random
// bytes, never zeros. Ending the session overwrites every slot that was ever filled.
#pragma once

#include <cstdint>
#include <map>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "api/crypto/frame_crypto_transformer.h"
#include "api/scoped_refptr.h"

namespace qmedia::engine {

inline constexpr int kKeyRingSlots = 16;

class SessionKeys {
 public:
  SessionKeys();
  ~SessionKeys();
  SessionKeys(const SessionKeys&) = delete;
  SessionKeys& operator=(const SessionKeys&) = delete;

  webrtc::scoped_refptr<webrtc::KeyProvider> provider() const {
    std::lock_guard<std::mutex> l(mu_);
    return provider_;
  }

  // The id under which the provider knows (participant, direction).
  static std::string ProviderId(const std::string& participant, bool send);

  // Returns false if the provider refused the key. slot is 0..15, key is exactly 32 bytes.
  bool Install(const std::string& participant, int slot, std::span<const uint8_t> key, bool send);
  // Overwrites the slot with random bytes. Returns false if no random bytes could be produced.
  bool Retire(const std::string& participant, int slot, bool send);
  // Overwrites every slot that was filled. Called when the session ends.
  void WipeAll();

  // The ring slot the sender cryptors of this participant encrypt with (default 0).
  void SelectSendSlot(const std::string& participant, int slot);
  int SendSlot(const std::string& participant) const;

  // Sender cryptors follow SelectSendSlot while they are registered.
  void RegisterSender(const std::string& participant,
                      webrtc::scoped_refptr<webrtc::FrameCryptorTransformer> transformer);
  void UnregisterSender(const webrtc::FrameCryptorTransformer* transformer);

 private:
  mutable std::mutex mu_;  // the host thread owns this object today; the lock keeps it true if that changes
  webrtc::scoped_refptr<webrtc::KeyProvider> provider_;
  std::set<std::pair<std::string, int>> filled_;  // (provider id, slot)
  std::map<std::string, int> send_slot_;
  std::vector<std::pair<std::string, webrtc::scoped_refptr<webrtc::FrameCryptorTransformer>>> senders_;
};

}  // namespace qmedia::engine
