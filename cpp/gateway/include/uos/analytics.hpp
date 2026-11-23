#pragma once
// Port of src/analytics.ts: the API key request log (the paid-fallback ledger rows written per
// request) and the retired v1 analytics key names.
// `kvOverride?: Kv | null` is std::optional<std::shared_ptr<rt::Kv>> (std::nullopt is undefined,
// a null pointer is null).
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::analytics {
  // v1 analytics keys remain named for deletion and migration only. The serving path never reads or writes them.
  inline const rt::KvKey API_KEY_USAGE_PREFIX{"ubq_ai", "api_keys", "usage"};
  inline const rt::KvKey API_KEY_USAGE_DAILY_PREFIX{"ubq_ai", "api_keys", "usage_daily"};
  inline const rt::KvKey LEGACY_API_KEY_REQUEST_LOG_PREFIX{"ubq_ai", "api_keys", "request_log"};
  inline const rt::KvKey API_KEY_REQUEST_LOG_PREFIX{"uos_ai", "paid_fallback", "ledger"};

  rt::KvKey apiKeyUsageKey(std::string_view id);
  rt::KvKey apiKeyUsageDailyKey(std::string_view id);
  rt::KvKey legacyApiKeyRequestLogPrefix(std::string_view id);
  rt::KvKey apiKeyRequestLogPrefix(std::string_view id);
  rt::KvKey apiKeyRequestLogKey(std::string_view id, double createdAtMs, std::string_view requestId);

  // `ApiKeyRequestLogInput` (Readonly). An unset optional member is an absent property, so a
  // record merge keeps the stored value for it.
  struct ApiKeyRequestLogInput {
    std::optional<std::string> id{};
    std::string route;
    std::string path;
    std::string method;
    double status_code;
    bool stream;
    rt::Nullable<std::string> model{};
    rt::Nullable<std::string> reasoning{};
    double created_at_ms;
    // "cerebras" | "chatgpt_codex" | "deepseek" | "lithos" | "openrouter" | "voyage" | PaidFallbackProvider
    std::optional<std::string> provider{};
    rt::Nullable<std::string> fallback_reason{};
    rt::Nullable<std::string> provider_request_id{};
    rt::Nullable<double> completed_at_ms{};
    rt::Nullable<double> latency_ms{};
    rt::Nullable<double> input_tokens{};
    rt::Nullable<double> output_tokens{};
    rt::Nullable<double> provider_quota{};
    rt::Nullable<double> quota_per_credit{};
    rt::Nullable<double> spend_microcredits{};
    rt::Nullable<double> paid_fallback_window_reset_at_ms{};
    // ApiKeyRequestLogRecord["billing_status"]
    std::optional<std::string> billing_status{};
    friend bool operator==(const ApiKeyRequestLogInput&, const ApiKeyRequestLogInput&) = default;
  };

  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyRequestLogInput& value);
  ApiKeyRequestLogInput tag_invoke(const json::value_to_tag<ApiKeyRequestLogInput>&, const json::value& value);

  // `ApiKeyRequestLogPatch = Partial<Omit<ApiKeyRequestLogInput, "id" | "created_at_ms">>`
  struct ApiKeyRequestLogPatch {
    std::optional<std::string> route{};
    std::optional<std::string> path{};
    std::optional<std::string> method{};
    std::optional<double> status_code{};
    std::optional<bool> stream{};
    rt::Nullable<std::string> model{};
    rt::Nullable<std::string> reasoning{};
    std::optional<std::string> provider{};
    rt::Nullable<std::string> fallback_reason{};
    rt::Nullable<std::string> provider_request_id{};
    rt::Nullable<double> completed_at_ms{};
    rt::Nullable<double> latency_ms{};
    rt::Nullable<double> input_tokens{};
    rt::Nullable<double> output_tokens{};
    rt::Nullable<double> provider_quota{};
    rt::Nullable<double> quota_per_credit{};
    rt::Nullable<double> spend_microcredits{};
    rt::Nullable<double> paid_fallback_window_reset_at_ms{};
    std::optional<std::string> billing_status{};
    friend bool operator==(const ApiKeyRequestLogPatch&, const ApiKeyRequestLogPatch&) = default;
  };

  void tag_invoke(const json::value_from_tag&, json::value& out, const ApiKeyRequestLogPatch& value);
  ApiKeyRequestLogPatch tag_invoke(const json::value_to_tag<ApiKeyRequestLogPatch>&, const json::value& value);

  inline constexpr double API_KEY_REQUEST_LOG_RETENTION_MS = 365.0 * 24 * 60 * 60 * 1000;

  // Throws rt::JsError "paid fallback ledger changed concurrently: <requestId>" after three lost commits.
  rt::Task<void> recordApiKeyRequestLog(std::string keyId, ApiKeyRequestLogInput input);
  rt::Task<void> recordApiKeyRequestLog(std::string keyId, ApiKeyRequestLogInput input, std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  rt::Task<void> updateApiKeyRequestLog(std::string keyId, double createdAtMs, std::string requestId, ApiKeyRequestLogPatch patch);
  rt::Task<void> updateApiKeyRequestLog(std::string keyId, double createdAtMs, std::string requestId, ApiKeyRequestLogPatch patch,
                                        std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  // The `options` of listApiKeyRequestLogs: `{ limit?: number; kv?: Kv | null }`.
  struct ListApiKeyRequestLogsOptions {
    std::optional<double> limit{};
    std::optional<std::shared_ptr<rt::Kv>> kv{};
  };

  // `listApiKeyRequestLogs(keyId, options = {})`: newest first.
  rt::Task<std::vector<types::ApiKeyRequestLogRecord>> listApiKeyRequestLogs(std::string keyId);
  rt::Task<std::vector<types::ApiKeyRequestLogRecord>> listApiKeyRequestLogs(std::string keyId, ListApiKeyRequestLogsOptions options);
}
