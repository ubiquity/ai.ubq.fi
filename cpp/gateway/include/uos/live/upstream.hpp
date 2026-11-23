#pragma once
// Port of src/live/upstream.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace uos::live::upstream {
  inline constexpr double LIVE_CALL_MAX_BODY_BYTES = 1'048'576;
  inline constexpr double LIVE_CALL_CAPTURED_ERROR_MAX_BYTES = 8 * 1024;
  inline constexpr double LIVE_SIDEBAND_MAX_PAYLOAD_BYTES = 4 * 1'048'576;
  inline constexpr double LIVE_SIDEBAND_MAX_BUFFERED_BYTES = 8 * 1'048'576;
  inline constexpr double LIVE_SIDEBAND_PREOPEN_MAX_FRAMES = 32;
  inline constexpr double LIVE_SIDEBAND_HANDSHAKE_TIMEOUT_MS = 10'000;
  inline constexpr double LIVE_CALL_ACCOUNT_TTL_MS = 60 * 60'000;
  inline constexpr std::string_view LIVE_OPENAI_ALPHA_DEFAULT = "quicksilver=v2";
  inline const rt::KvKey LIVE_CALL_ACCOUNT_KV_PREFIX{"uos_ai", "codex_live_calls", "v1"};

  struct LiveUpstreamBases { std::optional<std::string> callsBaseUrl{}; std::optional<std::string> sidebandBaseUrl{}; };
  void setLiveUpstreamBasesForTest(const LiveUpstreamBases& overrides);
  std::string liveCallsUrl();
  std::string liveSidebandUrl(const std::string& callId);
  rt::Headers liveCallsUpstreamHeaders(const rt::Request& req);
  json::object liveSidebandUpstreamHeaders(const std::string& accessToken, const std::string& accountId);
  std::optional<std::string> parseLiveCallIdFromLocation(const std::string& location);
  std::string liveCallLocation(const std::string& callId);
  rt::KvKey liveCallAccountKey(const std::string& callId);
  struct LiveCallMapping {
    std::string accountId;
    std::optional<std::string> principalId;
    friend bool operator==(const LiveCallMapping&, const LiveCallMapping&) = default;
  };
  rt::Task<void> writeLiveCallAccount(std::string callId, std::string accountId, std::string principalId);
  rt::Task<std::optional<LiveCallMapping>> readLiveCallMapping(std::string callId);
  rt::Task<void> retireLiveCallMapping(std::string callId);
}
