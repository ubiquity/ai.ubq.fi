#pragma once
// Port of src/provider/metered.ts.
//
// `fetcher?: MeteredFetch` options are rt::Fetch (an empty Fetch is undefined); `signal?:
// AbortSignal` is std::optional<rt::AbortSignal>; `options = {}` parameters are overloads.
#include "uos/api_key_policy.hpp"
#include "uos/authority/native_paid_contracts.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::provider::metered {
  inline constexpr std::string_view METERED_BASE_URL = "https://api.openlux.ai";

  // Billing reconciliation runs after the client response and must not hold a
  // queue delivery indefinitely when the provider stalls.
  inline constexpr double METERED_FETCH_TIMEOUT_MS = 10'000;
  inline constexpr double METERED_MODELS_CACHE_TTL_MS = 5 * 60'000;

  struct MeteredModel {
    std::string id;
    std::string object = "model";  // "model"
    double created;
    std::string owned_by;
    std::vector<std::string> supported_endpoint_types;
    std::optional<std::string> model_type{};
    std::optional<std::string> description{};
    std::optional<std::string> tags{};
    std::optional<double> max_output_tokens{};
    friend bool operator==(const MeteredModel&, const MeteredModel&) = default;
  };

  struct MeteredModelsSnapshot {
    std::vector<MeteredModel> models;
    double updated_at_ms;
    friend bool operator==(const MeteredModelsSnapshot&, const MeteredModelsSnapshot&) = default;
  };

  // `export type MeteredFetch = (input: RequestInfo | URL, init?: RequestInit) => Promise<Response>`
  // (declared further down in the TS, before its first use here).
  using MeteredFetch = rt::Fetch;

  // The `options` of fetchMeteredModels.
  struct FetchMeteredModelsOptions {
    MeteredFetch fetcher{};
    std::optional<rt::AbortSignal> signal{};
    std::optional<bool> force{};
    std::optional<bool> cachedOnly{};
  };

  // std::nullopt is null.
  rt::Task<std::optional<MeteredModelsSnapshot>> fetchMeteredModels();
  rt::Task<std::optional<MeteredModelsSnapshot>> fetchMeteredModels(FetchMeteredModelsOptions options);

  std::optional<MeteredModelsSnapshot> meteredModelsSnapshot();

  void resetMeteredModelsCacheForTest();

  // `fetcher: MeteredFetch | null`: an empty Fetch is null.
  void setMeteredModelsFetchForTest(MeteredFetch fetcher);

  // "metered_api_key_missing" | "metered_pricing_unavailable" | "metered_pricing_invalid" |
  // "metered_status_unavailable" | "metered_status_invalid" | "metered_request_invalid" |
  // "metered_upstream_unreachable" | "metered_logs_unavailable" | "metered_logs_invalid"
  using MeteredErrorCode = std::string;

  // `code` is also stored in rt::JsError::code, so code that reads `error.code` off any caught
  // error sees it as JS does.
  class MeteredError : public rt::JsError {
  public:
    const MeteredErrorCode code;
    const double status;
    const std::optional<double> upstream_status;  // number | null

    // `upstreamStatus: number | null = null`
    MeteredError(std::string message_, MeteredErrorCode code_, double status_, std::optional<double> upstreamStatus = std::nullopt)
      : rt::JsError(std::move(message_), "MeteredError"), code(std::move(code_)), status(status_), upstream_status(upstreamStatus) {
      rt::JsError::code = code;
    }
  };


  struct MeteredPricingSnapshot {
    std::vector<std::string> eligible_model_ids;
    double quota_per_credit;
    json::object model_quota_coefficients;  // Readonly<Record<string, number>>
    double checked_at_ms;
    friend bool operator==(const MeteredPricingSnapshot&, const MeteredPricingSnapshot&) = default;
  };

  struct InitializeMeteredPricingOptions {
    std::vector<std::string> codexModelIds;
    MeteredFetch fetcher{};
    // `now?: () => number`: an empty function is undefined (Date.now).
    std::function<double()> now{};
    std::optional<rt::AbortSignal> signal{};
  };

  struct MeteredAuthenticatedFetchOptions {
    // `apiKey?: string | null`: undefined reads the environment; null fails as unconfigured.
    rt::Nullable<std::string> apiKey{};
    MeteredFetch fetcher{};
    std::optional<rt::AbortSignal> signal{};
    /** Private authenticated identity for signed native reasoning replay; never sent upstream. */
    std::optional<std::string> reasoningReplayScope{};
    /**
     * Quota hook. Producers fulfil with `undefined` when no dispatch result is
     * available; `Promise.resolve(undefined)` at the call sites keeps that
     * explicit, which is why this is not spelled `void`-in-a-union.
     */
    // An empty function is undefined; a std::nullopt result is undefined.
    std::function<rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>>()> beforeDispatch{};
    std::function<void()> onDispatch{};
    /** Request-owned passive recorder; best effort, never required. */
    std::optional<sentinel::upstream_capture::SentinelUpstreamRecorder> sentinelUpstreamRecorder{};
  };

  // `MeteredAuthenticatedFetchOptions & Readonly<{ requestIds?; startAtMs?; endAtMs? }>`
  struct MeteredTokenLogFetchOptions : MeteredAuthenticatedFetchOptions {
    std::optional<std::vector<std::string>> requestIds{};
    std::optional<double> startAtMs{};
    std::optional<double> endAtMs{};
  };

  struct MeteredResponsesResult {
    rt::Response response;
    std::optional<std::string> request_id;
    std::optional<openai_telemetry::PromptCacheMode> prompt_cache_mode{};
    std::optional<double> explicit_breakpoint_count{};
  };

  // `Readonly<{ request_id; quota; prompt_tokens; cached_prompt_tokens?; cache_write_prompt_tokens?;
  // completion_tokens; model; created_at }>`: the same record as the native paid authority's
  // NativePaidProviderLog (the paid ledger hands one to the other), so the C++ type is that struct.
  using MeteredTokenLogEntry = authority::native_paid_contracts::NativePaidProviderLog;

  // `string | null`: std::nullopt is null.
  std::optional<std::string> readMeteredApiKey();

  // Throws MeteredError as the TS does.
  rt::Task<MeteredPricingSnapshot> initializeMeteredPricing(InitializeMeteredPricingOptions options);

  // `body: unknown`; throws MeteredError "metered_request_invalid" for a body that is not an object.
  rt::Task<MeteredResponsesResult> fetchMeteredResponses(json::value body);
  rt::Task<MeteredResponsesResult> fetchMeteredResponses(json::value body, MeteredAuthenticatedFetchOptions options);

  rt::Task<std::vector<MeteredTokenLogEntry>> fetchMeteredTokenLogs();
  rt::Task<std::vector<MeteredTokenLogEntry>> fetchMeteredTokenLogs(MeteredTokenLogFetchOptions options);

  // ---- JSON conversions of the snake_case records (members in declaration order) ----------
  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredModel& value);
  MeteredModel tag_invoke(const json::value_to_tag<MeteredModel>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredModelsSnapshot& value);
  MeteredModelsSnapshot tag_invoke(const json::value_to_tag<MeteredModelsSnapshot>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredPricingSnapshot& value);
  MeteredPricingSnapshot tag_invoke(const json::value_to_tag<MeteredPricingSnapshot>&, const json::value& value);
}
