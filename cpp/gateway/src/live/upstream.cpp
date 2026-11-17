#include "uos/live/upstream.hpp"

#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <array>
#include <exception>
#include <utility>

namespace uos::live::upstream {
  namespace {
    constexpr double LIVE_CALL_RETIRE_RETRY_MS = 250;
    constexpr int LIVE_CALL_RETIRE_MAX_ATTEMPTS = 4;
    constexpr std::string_view DEFAULT_SIDEBAND_BASE_URL = "wss://api.openai.com/v1/live";
    constexpr std::string_view LIVE_CALLS_ROUTE_PATH = "/realtime/calls?intent=quicksilver&architecture=avas";
    constexpr std::array<std::string_view, 6> LIVE_FORWARDED_CLIENT_HEADERS{
      "openai-alpha", "x-session-id", "session-id", "thread-id", "x-codex-turn-metadata", "x-oai-attestation"};
    std::optional<std::string> callsBaseUrlOverride;
    std::optional<std::string> sidebandBaseUrlOverride;
    rt::OrderedSet<std::string>& pendingLiveCallRetirements() { static rt::OrderedSet<std::string> values; return values; }
    std::string withoutTrailingSlashes(const std::string& value) {
      auto end = rt::length(value); while (end > 0 && rt::charAt(value, end - 1) == "/") --end; return rt::slice(value, 0, end);
    }
    std::string forwardedOpenAiAlpha(const rt::Request& req) {
      const auto alpha = rt::trim(req.headers().get("openai-alpha").value_or(""));
      return alpha.empty() ? std::string(LIVE_OPENAI_ALPHA_DEFAULT) : alpha;
    }
  }
  void setLiveUpstreamBasesForTest(const LiveUpstreamBases& overrides) { callsBaseUrlOverride = overrides.callsBaseUrl; sidebandBaseUrlOverride = overrides.sidebandBaseUrl; }
  std::string liveCallsUrl() { return withoutTrailingSlashes(callsBaseUrlOverride.value_or(config::config.codexBaseUrl)) + std::string(LIVE_CALLS_ROUTE_PATH); }
  std::string liveSidebandUrl(const std::string& callId) {
    return withoutTrailingSlashes(sidebandBaseUrlOverride.value_or(std::string(DEFAULT_SIDEBAND_BASE_URL))) + "/" + rt::encodeURIComponent(callId);
  }
  rt::Headers liveCallsUpstreamHeaders(const rt::Request& req) {
    rt::Headers headers;
    headers.set("content-type", "application/json"); headers.set("originator", std::string(codex::auth::CODEX_ORIGINATOR));
    headers.set("user-agent", codex::auth::codexUserAgent()); headers.set("openai-alpha", forwardedOpenAiAlpha(req));
    for (const auto name : LIVE_FORWARDED_CLIENT_HEADERS) if (const auto value = req.headers().get(name); value && !rt::trim(*value).empty()) headers.set(name, *value);
    return headers;
  }
  json::object liveSidebandUpstreamHeaders(const std::string& accessToken, const std::string& accountId) {
    return {{"authorization", "Bearer " + accessToken}, {"chatgpt-account-id", accountId}, {"originator", std::string(codex::auth::CODEX_ORIGINATOR)},
            {"user-agent", codex::auth::codexUserAgent()}, {"openai-alpha", std::string(LIVE_OPENAI_ALPHA_DEFAULT)}};
  }
  std::optional<std::string> parseLiveCallIdFromLocation(const std::string& location) {
    const auto withoutQuery = rt::split(location, "?").front(); const auto segments = rt::split(withoutQuery, "/");
    static const rt::Regex UUID_PATTERN("^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$", "iu");
    for (auto i = segments.size(); i > 0; --i) {
      const auto& segment = segments[i - 1];
      if ((rt::startsWith(segment, "rtc_") && rt::length(segment) > 4) || UUID_PATTERN.test(segment)) return segment;
    }
    return std::nullopt;
  }
  std::string liveCallLocation(const std::string& callId) { return "/v1/live/" + rt::encodeURIComponent(callId); }
  rt::KvKey liveCallAccountKey(const std::string& callId) { return rt::KvKey{LIVE_CALL_ACCOUNT_KV_PREFIX, callId}; }
  rt::Task<void> writeLiveCallAccount(std::string callId, std::string accountId, std::string principalId) {
    auto pending = kv::getKv(); auto store = co_await pending.wait();
    if (!store) {
      rt::console::warn("[ai.ubq.fi] live_call_account_mapping_skipped", rt::jsonStringify(json::object{{"call_id", callId}, {"reason", "kv_unavailable"}})); co_return;
    }
    try {
      const auto key = liveCallAccountKey(callId);
      const rt::KvValue value(json::object{{"account_id", accountId}, {"principal_id", principalId}, {"created_at_ms", rt::makeNumber(rt::dateNow())}});
      const rt::KvSetOptions options{.expireIn = LIVE_CALL_ACCOUNT_TTL_MS};
      rt::Promise<rt::KvCommitResult> saved(store->set(key, value, options)); co_await saved.wait();
    } catch (...) {
      const auto error = std::current_exception();
      rt::console::warn("[ai.ubq.fi] live_call_account_mapping_failed", rt::jsonStringify(json::object{{"call_id", callId}, {"reason", rt::isError(error) ? rt::errorName(error).value_or("Error") : "unknown"}}));
    }
  }
  rt::Task<std::optional<LiveCallMapping>> readLiveCallMapping(std::string callId) {
    if (pendingLiveCallRetirements().has(callId)) co_return std::nullopt;
    auto pending = kv::getKv(); auto store = co_await pending.wait(); if (!store) co_return std::nullopt;
    std::optional<LiveCallMapping> result;
    try {
      const auto key = liveCallAccountKey(callId); rt::Promise<rt::KvEntryMaybe<>> read(store->get(key)); auto entry = co_await read.wait();
      if (!pendingLiveCallRetirements().has(callId) && rt::kvIsObject(entry.value)) {
        const auto view = rt::storedJsonView(entry.value); const auto accountId = utils::getString(rt::get(view, "account_id"));
        if (accountId) result = LiveCallMapping{*accountId, utils::getString(rt::get(view, "principal_id"))};
      }
    } catch (...) {}
    co_return result;
  }
  rt::Task<void> retireLiveCallMapping(std::string callId) {
    pendingLiveCallRetirements().add(callId);
    const auto cleanup = rt::scopeExit([&callId] { pendingLiveCallRetirements().delete_(callId); });
    for (int attempt = 0; attempt < LIVE_CALL_RETIRE_MAX_ATTEMPTS; ++attempt) {
      auto pending = kv::getKv(); auto store = co_await pending.wait();
      if (store) {
        bool deleted = false;
        try { const auto key = liveCallAccountKey(callId); rt::Promise<void> deletion(store->delete_(key)); co_await deletion.wait(); deleted = true; }
        catch (...) {
          const auto error = std::current_exception();
          rt::console::warn("[ai.ubq.fi] live_call_account_mapping_retire_failed", rt::jsonStringify(json::object{{"call_id", callId}, {"reason", rt::isError(error) ? rt::errorName(error).value_or("Error") : "unknown"}}));
        }
        if (deleted) co_return;
      }
      if (attempt + 1 < LIVE_CALL_RETIRE_MAX_ATTEMPTS) { rt::Promise<void> delay(rt::sleep(LIVE_CALL_RETIRE_RETRY_MS)); co_await delay.wait(); }
    }
    rt::console::warn("[ai.ubq.fi] live_call_account_mapping_retire_exhausted", rt::jsonStringify(json::object{{"call_id", callId}}));
  }
}
