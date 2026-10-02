// Reduces a libwebrtc stats report to what the client needs.
//
// Only the entry types and attributes on the allowlists in stats.cpp are passed on, so no SDP, no
// address, no certificate, no stream identifier and no candidate ever leaves the engine through
// this path. Entry ids are replaced by "n<index>". Values follow the schema's scalar: unsigned
// integers and booleans stay as they are, fractional or negative numbers become decimal text.
#pragma once

#include <string_view>
#include <vector>

#include "api/stats/rtc_stats_report.h"
#include "qmedia/ipc/message.h"

namespace qmedia::engine {

enum class StatsScope { All, Transport, Audio, Video };

// Maps the schema enum ("all", "transport", "audio", "video"); anything else is All.
StatsScope ParseStatsScope(std::string_view s);

// At most 64 entries (the schema bound).
std::vector<ipc::StatEntry> ReduceStats(const webrtc::RTCStatsReport& report, StatsScope scope);

}  // namespace qmedia::engine
