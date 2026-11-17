#pragma once
// Port of src/provider/surplus.ts.
//
// `fetcher?: SurplusFetch` options are rt::Fetch (an empty Fetch is undefined); `signal?:
// AbortSignal` is std::optional<rt::AbortSignal>; `options = {}` parameters are overloads.
#include "uos/api_key_policy.hpp"
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

namespace uos::provider::surplus {
  inline constexpr std::string_view SURPLUS_BASE_URL = "https://api.surplusintelligence.ai";
  inline constexpr std::string_view SURPLUS_API_KEY_ENV = "SURPLUS_API_KEY";
  inline constexpr double SURPLUS_FETCH_TIMEOUT_MS = 10'000;
  inline constexpr double SURPLUS_MODELS_CACHE_TTL_MS = 5 * 60'000;

  struct SurplusModel {
    std::string id;
    std::string object = "model";  // "model"
    double created;
    std::string owned_by;
    std::vector<std::string> supported_endpoint_types;
    std::optional<bool> supports_tools{};                // `?: true`
    std::optional<bool> supports_parallel_tool_calls{};  // `?: true`
    std::optional<std::string> description{};
    std::optional<double> input_price_per_token{};
    std::optional<double> output_price_per_token{};
    std::optional<double> cache_read_price_per_token{};
    std::optional<double> cache_write_price_per_token{};
    std::optional<double> max_output_tokens{};
    friend bool operator==(const SurplusModel&, const SurplusModel&) = default;
  };

  struct SurplusModelsSnapshot {
    std::vector<SurplusModel> models;
    double updated_at_ms;
    friend bool operator==(const SurplusModelsSnapshot&, const SurplusModelsSnapshot&) = default;
  };

  // `(input: RequestInfo | URL, init?: RequestInit) => Promise<Response>`
  using SurplusFetch = rt::Fetch;

  using SurplusErrorCode = std::string;  // "surplus_api_key_missing" | "surplus_request_invalid" | "surplus_upstream_unreachable"

  // `code` is also stored in rt::JsError::code, so code that reads `error.code` off any caught
  // error sees it as JS does.
  class SurplusError : public rt::JsError {
  public:
    const SurplusErrorCode code;
    const double status;
    const std::optional<double> upstream_status;  // number | null

    // `upstreamStatus: number | null = null`
    SurplusError(std::string message_, SurplusErrorCode code_, double status_, std::optional<double> upstreamStatus = std::nullopt)
      : rt::JsError(std::move(message_), "SurplusError"), code(std::move(code_)), status(status_), upstream_status(upstreamStatus) {
      rt::JsError::code = code;
    }
  };

  struct SurplusAuthenticatedFetchOptions {
    // `apiKey?: string | null`: undefined reads SURPLUS_API_KEY; null fails as unconfigured.
    rt::Nullable<std::string> apiKey{};
    SurplusFetch fetcher{};
    std::optional<rt::AbortSignal> signal{};
    std::optional<bool> supportsParallelToolCalls{};
    /** Authenticated caller scope; never forwarded to an upstream provider. */
    std::optional<std::string> reasoningReplayScope{};
    /**
     * Optional pre-transport hook. It either resolves to a dispatch handle that can
     * still cancel the admitted reservation, or resolves with no value at all, so
     * the return type stays a plain `void` arm instead of a `void` union member.
     */
    // `(() => Promise<ApiKeyProviderDispatch>) | (() => void)`: the `() => void` form is a hook
    // whose Task yields std::nullopt. An empty function is undefined.
    std::function<rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>>()> beforeDispatch{};
    std::function<void()> onDispatch{};
    /** Request-owned passive recorder; best effort, never required. */
    std::optional<sentinel::upstream_capture::SentinelUpstreamRecorder> sentinelUpstreamRecorder{};
  };

  struct SurplusResponsesResult {
    rt::Response response;
    std::optional<std::string> request_id;
    std::optional<openai_telemetry::PromptCacheMode> prompt_cache_mode{};
    std::optional<double> explicit_breakpoint_count{};
  };

  // `string | null`: std::nullopt is null.
  std::optional<std::string> readSurplusApiKey();

  // The `options` of fetchSurplusModels.
  struct FetchSurplusModelsOptions {
    // `apiKey?: string | null`: undefined reads SURPLUS_API_KEY.
    rt::Nullable<std::string> apiKey{};
    SurplusFetch fetcher{};
    std::optional<rt::AbortSignal> signal{};
    std::optional<bool> force{};
    std::optional<bool> cachedOnly{};
    std::optional<bool> requireApiKey{};
  };

  // std::nullopt is null.
  rt::Task<std::optional<SurplusModelsSnapshot>> fetchSurplusModels();
  rt::Task<std::optional<SurplusModelsSnapshot>> fetchSurplusModels(FetchSurplusModelsOptions options);

  std::optional<SurplusModelsSnapshot> surplusModelsSnapshot();

  void resetSurplusModelsCacheForTest();

  // `body: unknown`; throws SurplusError "surplus_request_invalid" for a body that is not an object.
  rt::Task<SurplusResponsesResult> fetchSurplusResponses(json::value body);
  rt::Task<SurplusResponsesResult> fetchSurplusResponses(json::value body, SurplusAuthenticatedFetchOptions options);

  // ---- JSON conversions of the snake_case records (members in declaration order) ----------
  // SurplusModel writes max_output_tokens before the four pricing members: the order
  // readSurplusModel creates them in, which is what JSON.stringify of a discovered model prints.
  void tag_invoke(const json::value_from_tag&, json::value& out, const SurplusModel& value);
  SurplusModel tag_invoke(const json::value_to_tag<SurplusModel>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const SurplusModelsSnapshot& value);
  SurplusModelsSnapshot tag_invoke(const json::value_to_tag<SurplusModelsSnapshot>&, const json::value& value);
}
