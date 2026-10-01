// Builders for the messages the engine itself sends, plus the client-side hello (tests, helpers).
#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include "qmedia/ipc/cbor.h"

namespace qmedia::ipc {

namespace errcode {
inline constexpr std::string_view kBadRequest = "bad_request";
inline constexpr std::string_view kUnsupported = "unsupported";
inline constexpr std::string_view kNotFound = "not_found";
inline constexpr std::string_view kInvalidState = "invalid_state";
inline constexpr std::string_view kInvalidKey = "invalid_key";
inline constexpr std::string_view kLimit = "limit";
inline constexpr std::string_view kInternal = "internal";
}  // namespace errcode

inline constexpr std::string_view kEngineIdent = "qaudion-media/0.1";

// A map builder that already carries the envelope fields v, t and id.
cbor::MapBuilder BeginMessage(std::string_view kind, uint32_t id);

cbor::Buf BuildHello(uint32_t id, std::span<const uint8_t> nonce);
cbor::Buf BuildHelloOk(uint32_t id);
cbor::Buf BuildPong(uint32_t id);
cbor::Buf BuildOk(uint32_t id);
// detail must be a static, content-free string (for example ErrName(...)).
cbor::Buf BuildErr(uint32_t id, std::string_view code, std::string_view detail = {});

}  // namespace qmedia::ipc
