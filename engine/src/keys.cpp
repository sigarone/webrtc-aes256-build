#include "keys.h"

#include <algorithm>

#include "api/make_ref_counted.h"
#include "qmedia/ipc/secure.h"

namespace qmedia::engine {

SessionKeys::SessionKeys() {
  webrtc::KeyProviderOptions o;
  o.shared_key = false;
  o.ratchet_salt = {};
  o.ratchet_window_size = 0;
  o.uncrypted_magic_bytes = {};
  o.failure_tolerance = -1;
  o.key_ring_size = kKeyRingSlots;
  o.discard_frame_when_cryptor_not_ready = true;
  o.key_derivation_algorithm = webrtc::kHKDF;
  provider_ = webrtc::make_ref_counted<webrtc::DefaultKeyProviderImpl>(o);
}

SessionKeys::~SessionKeys() { WipeAll(); }

std::string SessionKeys::ProviderId(const std::string& participant, bool send) {
  return (send ? "s:" : "r:") + participant;
}

bool SessionKeys::Install(const std::string& participant, int slot, std::span<const uint8_t> key,
                          bool send) {
  if (slot < 0 || slot >= kKeyRingSlots || key.size() != 32) return false;
  std::vector<uint8_t> copy(key.begin(), key.end());
  const std::string id = ProviderId(participant, send);
  // SetKey takes its key by value (a copy); the local one is wiped as soon as the call returns.
  const bool ok = provider_->SetKey(id, slot, copy);
  ipc::SecureZero(copy.data(), copy.size());
  if (ok) filled_.insert({id, slot});
  return ok;
}

bool SessionKeys::Retire(const std::string& participant, int slot, bool send) {
  if (slot < 0 || slot >= kKeyRingSlots) return false;
  std::vector<uint8_t> noise(32);
  if (!ipc::FillRandom(noise.data(), noise.size())) return false;
  const std::string id = ProviderId(participant, send);
  // Random, never zeros: the cryptor accepts 32 zero bytes as a valid key, so a zeroed slot would
  // hold a key derived from a public value.
  const bool ok = provider_->SetKey(id, slot, noise);
  ipc::SecureZero(noise.data(), noise.size());
  if (ok) filled_.insert({id, slot});
  return ok;
}

void SessionKeys::WipeAll() {
  if (!provider_) return;
  for (const auto& entry : filled_) {
    std::vector<uint8_t> noise(32);
    if (ipc::FillRandom(noise.data(), noise.size())) {
      provider_->SetKey(entry.first, entry.second, noise);
    }
    ipc::SecureZero(noise.data(), noise.size());
  }
  filled_.clear();
  senders_.clear();
  provider_ = nullptr;
}

void SessionKeys::SelectSendSlot(const std::string& participant, int slot) {
  send_slot_[participant] = slot;
  const std::string id = ProviderId(participant, true);
  for (auto& s : senders_) {
    if (s.first == id) s.second->SetKeyIndex(slot);
  }
}

int SessionKeys::SendSlot(const std::string& participant) const {
  const auto it = send_slot_.find(participant);
  return it == send_slot_.end() ? 0 : it->second;
}

void SessionKeys::RegisterSender(const std::string& participant,
                                 webrtc::scoped_refptr<webrtc::FrameCryptorTransformer> transformer) {
  senders_.emplace_back(ProviderId(participant, true), std::move(transformer));
}

void SessionKeys::UnregisterSender(const webrtc::FrameCryptorTransformer* transformer) {
  senders_.erase(std::remove_if(senders_.begin(), senders_.end(),
                                [&](const auto& s) { return s.second.get() == transformer; }),
                 senders_.end());
}

}  // namespace qmedia::engine
