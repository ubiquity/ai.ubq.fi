#include "uos/provider/metered.hpp"
#include "uos/rt/memory.hpp"

#include "uos/inference_deadline.hpp"
#include "uos/provider/anthropic_responses.hpp"
#include "uos/provider/surplus_claude_request.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"

#include <array>
#include <cmath>
#include <exception>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace uos::provider::metered {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    using JsonRecord = json::object;
    using surplus_claude_request::SurplusClaudeRequest;
    using AttemptHandle = sentinel::upstream_capture::SentinelUpstreamAttemptHandle;

    constexpr std::string_view METERED_API_KEY_ENV = "METERED_API_KEY";
    constexpr std::string_view OPENLUX_API_KEY_ENV = "OPENLUX_API_KEY";
    // `${METERED_BASE_URL}/...`
    constexpr std::string_view METERED_MODELS_URL = "https://api.openlux.ai/v1/models";
    constexpr std::string_view METERED_RATIO_CONFIG_URL = "https://api.openlux.ai/api/ratio_config";
    constexpr std::string_view METERED_STATUS_URL = "https://api.openlux.ai/api/status";
    constexpr std::string_view METERED_RESPONSES_URL = "https://api.openlux.ai/v1/responses";
    constexpr std::string_view METERED_MESSAGES_URL = "https://api.openlux.ai/v1/messages";
    constexpr std::string_view METERED_TOKEN_LOGS_URL = "https://api.openlux.ai/api/log/token";
    constexpr double METERED_MODELS_FAILURE_BACKOFF_MS = 30'000;

    // Helpers the TS declares further down (`isRecord`, `nonEmptyString`, ...), used from here on.
    bool isRecord(const json::value* value) { return rt::isRecord(value); }

    std::optional<std::string> nonEmptyText(std::string_view value) {
      std::string trimmed = rt::trim(value);
      if (trimmed.empty()) return std::nullopt;
      return trimmed;
    }
    std::optional<std::string> nonEmptyString(const json::value* value) {
      if (!value || !value->is_string()) return std::nullopt;
      return nonEmptyText(value->get_string());
    }
    std::optional<std::string> nonEmptyString(const std::optional<std::string>& value) { return value ? nonEmptyText(*value) : std::nullopt; }

    bool isPositiveFiniteNumber(const json::value* value) {
      return value && rt::isNumber(*value) && rt::Number::isFinite(*value) && rt::toNumber(*value) > 0;
    }
    bool isNonNegativeSafeInteger(const json::value* value) {
      return value && rt::isNumber(*value) && rt::Number::isSafeInteger(*value) && rt::toNumber(*value) >= 0;
    }
    bool isNonNegativeSafeInteger(double value) { return rt::Number::isSafeInteger(value) && value >= 0; }

    // `reason ?? fallback` on a caught value: undefined (a null Reason) and null are nullish.
    bool nullishReason(const rt::Reason& reason) {
      if (!reason) return true;
      try {
        std::rethrow_exception(reason);
      } catch (const rt::ThrownValue& thrown) {
        return !thrown.value || thrown.value->is_null();
      } catch (...) {
        return false;
      }
    }
    rt::Reason reasonOr(const rt::Reason& reason, const rt::Reason& fallback) { return nullishReason(reason) ? fallback : reason; }

    bool isApiKeyQuotaDispatchError(const rt::Reason& reason) {
      if (!reason) return false;
      try {
        std::rethrow_exception(reason);
      } catch (const api_key_policy::ApiKeyQuotaDispatchError&) {
        return true;
      } catch (...) {
        return false;
      }
    }

    std::optional<MeteredModelsSnapshot> meteredModelsCache;
    std::optional<rt::Promise<std::optional<MeteredModelsSnapshot>>> meteredModelsFetchInFlight;
    double meteredModelsFetchGeneration = 0;
    double meteredModelsCacheGeneration = 0;
    double meteredModelsRetryAfterMs = 0;
    MeteredFetch meteredModelsFetchForTest;
    // `const defaultMeteredFetch: MeteredFetch = globalThis.fetch` (the load-time global) is
    // rt::defaultFetch(), read at call time.

    std::optional<std::string> readModelDiscoveryApiKey() {
      std::optional<std::string> openLuxApiKey;
      std::optional<std::string> meteredApiKey;
      try {
        openLuxApiKey = nonEmptyString(rt::env::get(OPENLUX_API_KEY_ENV));
      } catch (...) {
        // The deployment may grant only the legacy Metered key permission.
      }
      try {
        meteredApiKey = nonEmptyString(rt::env::get(METERED_API_KEY_ENV));
      } catch (...) {
        // Keep discovery unavailable only when neither credential is readable.
      }
      return openLuxApiKey ? openLuxApiKey : meteredApiKey;
    }

    rt::AbortSignal boundedModelDiscoverySignal(const std::optional<rt::AbortSignal>& signal) {
      const rt::AbortSignal timeout = rt::AbortSignal::timeout(METERED_FETCH_TIMEOUT_MS);
      return signal ? rt::AbortSignal::any({*signal, timeout}) : timeout;
    }

    void markMeteredModelsFetchFailure(double requestGeneration) {
      if (requestGeneration < meteredModelsCacheGeneration) return;
      meteredModelsRetryAfterMs = rt::dateNow() + METERED_MODELS_FAILURE_BACKOFF_MS;
    }

    std::optional<MeteredModel> meteredModelFromUpstream(const json::value& value) {
      if (!isRecord(&value)) return std::nullopt;
      const JsonRecord& upstream = value.get_object();
      std::optional<std::string> id = nonEmptyString(rt::get(upstream, "id"));
      if (!id) return std::nullopt;
      std::vector<std::string> endpoints;
      if (const json::array* listed = rt::getArray(upstream, "supported_endpoint_types"))
        for (const json::value& entry : *listed)
          if (entry.is_string()) endpoints.emplace_back(entry.get_string());
      const json::value* createdValue = rt::get(upstream, "created");
      const double created = isNonNegativeSafeInteger(createdValue) ? rt::toNumber(*createdValue) : 0;
      std::string ownedBy = nonEmptyString(rt::get(upstream, "owned_by")).value_or("openlux");
      std::optional<std::string> modelType = nonEmptyString(rt::get(upstream, "model_type"));
      std::optional<std::string> description = nonEmptyString(rt::get(upstream, "description"));
      std::optional<std::string> tags = nonEmptyString(rt::get(upstream, "tags"));
      const json::value* topProvider = rt::get(upstream, "top_provider");
      if (!isRecord(topProvider)) topProvider = nullptr;
      // `value.max_completion_tokens ?? value.max_output_tokens ?? topProvider?.max_completion_tokens`
      const json::value* maxOutputTokens = rt::get(upstream, "max_completion_tokens");
      if (rt::isNullish(maxOutputTokens)) maxOutputTokens = rt::get(upstream, "max_output_tokens");
      if (rt::isNullish(maxOutputTokens)) maxOutputTokens = topProvider ? rt::get(*topProvider, "max_completion_tokens") : nullptr;
      MeteredModel model{
        .id = std::move(*id),
        .object = "model",
        .created = created,
        .owned_by = std::move(ownedBy),
        .supported_endpoint_types = std::move(endpoints),
      };
      if (modelType) model.model_type = std::move(modelType);
      if (description) model.description = std::move(description);
      if (tags) model.tags = std::move(tags);
      if (isNonNegativeSafeInteger(maxOutputTokens) && rt::toNumber(*maxOutputTokens) > 0) model.max_output_tokens = rt::toNumber(*maxOutputTokens);
      return model;
    }

    std::vector<MeteredModel> listedMeteredModels(const json::array& data) {
      std::vector<MeteredModel> models;
      std::unordered_set<std::string> seen;
      for (const json::value& value : data) {
        std::optional<MeteredModel> model = meteredModelFromUpstream(value);
        if (!model || seen.contains(model->id)) continue;
        seen.insert(model->id);
        models.push_back(std::move(*model));
      }
      return models;
    }

    std::optional<MeteredModelsSnapshot> publishMeteredModelsSnapshot(double requestGeneration, std::vector<MeteredModel> models) {
      MeteredModelsSnapshot snapshot{.models = std::move(models), .updated_at_ms = rt::dateNow()};
      if (requestGeneration > meteredModelsCacheGeneration) {
        meteredModelsCacheGeneration = requestGeneration;
        meteredModelsCache = std::move(snapshot);
        const auto& cachedModels = meteredModelsCache->models;
        std::size_t bytes = cachedModels.capacity() * sizeof(cachedModels.front());
        for (const auto& model : cachedModels) {
          bytes += rt::memory::bytes(model.id) + rt::memory::bytes(model.object) + rt::memory::bytes(model.owned_by) +
                   rt::memory::bytes(model.supported_endpoint_types);
          for (const auto* field : {&model.description, &model.model_type, &model.tags}) if (*field) bytes += rt::memory::bytes(**field);
        }
        rt::memory::setCache(rt::memory::Cache::openlux_models, cachedModels.size(), bytes);
        meteredModelsRetryAfterMs = 0;
      }
      return meteredModelsCache;
    }

    /**
     * Rejection reasons must be `Error` instances. Deno's `DOMException` already is
     * one, so abort and timeout reasons (including `AbortSignal.timeout`'s
     * `TimeoutError`) pass through unchanged; anything else is carried as `cause`
     * so no information is lost.
     */
    rt::Reason asRejectionReason(rt::Reason value) {
      if (rt::isError(value)) return value;
      return rt::makeReason(rt::withCause(rt::JsError("Metered request failed"), std::move(value)));
    }

    // The settled flag and the abort listener of one awaitWithAbort call. The listener holds
    // the state weakly; the operation's reactions hold it until the operation settles.
    struct AbortRace {
      rt::AbortSignal signal;
      bool settled = false;
      rt::EventListener onAbort;

      explicit AbortRace(rt::AbortSignal signal_) : signal(std::move(signal_)) {}
      ~AbortRace() {
        if (!settled) signal.removeEventListener("abort", onAbort);
      }

      bool finish() {
        if (settled) return false;
        settled = true;
        signal.removeEventListener("abort", onAbort);
        return true;
      }
    };

    // `new Promise((resolve, reject) => { ... })` racing `operation` against `signal`.
    template <class T> rt::Promise<T> awaitWithAbort(rt::Promise<T> operation, rt::AbortSignal signal) {
      const rt::Resolvers<T> resolvers = rt::withResolvers<T>();
      const rt::Resolve<T> resolve = resolvers.resolve;
      const rt::Reject reject = resolvers.reject;
      auto race = std::make_shared<AbortRace>(std::move(signal));
      const std::weak_ptr<AbortRace> weakRace = race;
      race->onAbort = rt::EventListener([weakRace, reject]() {
        const std::shared_ptr<AbortRace> state = weakRace.lock();
        if (!state || !state->finish()) return;
        reject(asRejectionReason(reasonOr(state->signal.reason(), rt::makeReason(rt::DOMException("Aborted", "AbortError")))));
      });
      if (race->signal.aborted()) {
        race->onAbort();
        return resolvers.promise;
      }
      race->signal.addEventListener("abort", race->onAbort, {.once = true});
      (void)operation.then(
        [race, resolve](T value) {
          if (race->finish()) resolve(std::move(value));
        },
        [race, reject](rt::Reason error) {
          if (race->finish()) reject(asRejectionReason(std::move(error)));
        });
      return resolvers.promise;
    }

    void rethrowCancellation(const rt::Reason& error, const std::optional<rt::AbortSignal>& signal) {
      if ((signal && signal->aborted()) || (rt::isError(error) && rt::errorName(error) == "AbortError")) rt::rethrow(error);
    }

    rt::AbortSignal boundedTokenLogSignal(const std::optional<rt::AbortSignal>& signal) {
      const rt::AbortSignal timeout = rt::AbortSignal::timeout(METERED_FETCH_TIMEOUT_MS);
      return signal ? rt::AbortSignal::any({*signal, timeout}) : timeout;
    }

    // The discovery request `fetchMeteredModels` starts (the TS's inline async function).
    rt::Task<std::optional<MeteredModelsSnapshot>> requestMeteredModels(MeteredFetch fetcher, std::string apiKey, std::optional<rt::AbortSignal> callerSignal,
                                                                       double requestGeneration) {
      const rt::AbortSignal signal = boundedModelDiscoverySignal(callerSignal);
      bool failed = false;
      try {
        rt::RequestInit init;
        init.method = "GET";
        init.headers = rt::Headers{{"Authorization", "Bearer " + apiKey}, {"Accept", "application/json"}};
        init.signal = signal;
        rt::Promise<rt::Response> fetching(fetcher(std::string(METERED_MODELS_URL), std::move(init)));
        rt::Promise<rt::Response> responded = awaitWithAbort(fetching, signal);
        const rt::Response response = co_await responded.wait();
        if (!response.ok()) {
          markMeteredModelsFetchFailure(requestGeneration);
          co_return meteredModelsCache;
        }
        rt::Promise<json::value> reading(response.json());
        rt::Promise<json::value> read = awaitWithAbort(reading, signal);
        const json::value payload = co_await read.wait();
        const json::array* data = isRecord(&payload) ? rt::getArray(payload, "data") : nullptr;
        if (!data) {
          markMeteredModelsFetchFailure(requestGeneration);
          co_return meteredModelsCache;
        }
        co_return publishMeteredModelsSnapshot(requestGeneration, listedMeteredModels(*data));
      } catch (...) {
        failed = true;
      }
      if (failed) {
        if (!(signal.aborted() && callerSignal && callerSignal->aborted())) markMeteredModelsFetchFailure(requestGeneration);
      }
      co_return meteredModelsCache;
    }

    const std::array<std::pair<std::string_view, double>, 1>& meteredNativeMaxOutputTokens() {
      // OpenLux omits output limits from its catalog. The same exact Opus 5.5
      // weights publish 128000 at the primary marketplace model endpoint:
      // https://api.surplusintelligence.ai/v1/models/claude-opus-5.5 (2026-10-11).
      // Use the model maximum, not an invented smaller generation budget.
      static constexpr std::array<std::pair<std::string_view, double>, 1> METERED_NATIVE_MAX_OUTPUT_TOKENS{{{"claude-opus-5-5", 128'000}}};
      return METERED_NATIVE_MAX_OUTPUT_TOKENS;
    }

    constexpr std::array<std::string_view, 1> METERED_REASONING_SUFFIX_MODELS{"gpt-5.6-sol"};

    JsonRecord toMeteredResponsesBody(const JsonRecord& body) {
      std::optional<std::string> model = nonEmptyString(rt::get(body, "model"));
      const json::value* reasoningValue = rt::get(body, "reasoning");
      std::optional<std::string> reasoning = isRecord(reasoningValue) ? nonEmptyString(rt::get(*reasoningValue, "effort")) : std::nullopt;
      bool suffixModel = false;
      for (const std::string_view candidate : METERED_REASONING_SUFFIX_MODELS) suffixModel = suffixModel || (model && *model == candidate);
      if (!model || !reasoning || !suffixModel) return body;

      std::string suffix = *reasoning;
      if (*reasoning == "none" || *reasoning == "minimal") {
        suffix = "low";
      } else if (*reasoning == "ultra") {
        suffix = "max";
      }
      if (suffix != "low" && suffix != "medium" && suffix != "high" && suffix != "xhigh" && suffix != "max") return body;

      JsonRecord result = rt::objectSpread(body);
      rt::objectDefine(result, "model", json::value(*model + "-" + suffix));
      rt::objectDelete(result, "reasoning");
      return result;
    }

    std::optional<std::string> requireMeteredApiKeyValue(const rt::Nullable<std::string>& supplied) {
      if (supplied.isUndefined()) return readMeteredApiKey();
      return supplied.hasValue() ? nonEmptyText(*supplied) : std::nullopt;
    }

    std::string requireMeteredApiKey(const rt::Nullable<std::string>& supplied) {
      std::optional<std::string> apiKey = requireMeteredApiKeyValue(supplied);
      if (apiKey) return *apiKey;
      throw MeteredError("Metered paid fallback is unavailable because METERED_API_KEY or OPENLUX_API_KEY is not configured.", "metered_api_key_missing", 503);
    }

    rt::Headers metadataHeaders() {
      rt::Headers headers;
      headers.set("Accept", "application/json");
      return headers;
    }

    rt::Headers authenticatedHeaders(std::string_view apiKey, std::string_view accept) {
      rt::Headers headers;
      headers.set("Authorization", "Bearer " + std::string(apiKey));
      headers.set("Accept", accept);
      return headers;
    }

    rt::Task<json::value> fetchMetadataJson(std::string url, MeteredFetch fetcher, std::optional<rt::AbortSignal> signal, std::string unavailableCode,
                                            std::string invalidCode) {
      std::optional<rt::Response> response;
      rt::Reason failure;
      try {
        rt::RequestInit init;
        init.method = "GET";
        init.headers = metadataHeaders();
        init.redirect = "manual";
        init.signal = signal;
        rt::Promise<rt::Response> fetching(fetcher(url, std::move(init)));
        response = co_await fetching.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        rethrowCancellation(failure, signal);
        throw MeteredError("Metered pricing initialization could not reach the metadata service.", unavailableCode, 502);
      }

      if (!response->ok()) {
        throw MeteredError("Metered pricing initialization received an unsuccessful metadata response.", unavailableCode, 502, response->status());
      }

      std::optional<json::value> payload;
      try {
        rt::Promise<json::value> reading(response->json());
        payload = co_await reading.wait();
      } catch (...) {
      }
      if (!payload) throw MeteredError("Metered pricing initialization received invalid metadata.", invalidCode, 502, response->status());
      co_return std::move(*payload);
    }

    JsonRecord unwrapSuccessfulEnvelope(const json::value& value, const std::string& code) {
      const json::value* success = rt::get(value, "success");
      const json::value* data = rt::get(value, "data");
      if (!isRecord(&value) || !(success && success->is_bool() && success->get_bool()) || !isRecord(data)) {
        throw MeteredError("Metered pricing initialization received an invalid metadata envelope.", code, 502);
      }
      return data->get_object();
    }

    std::unordered_set<std::string> pricedModelIds(const JsonRecord& pricing) {
      const json::value* ratioMap = rt::get(pricing, "model_ratio");
      const json::value* fixedPriceMap = rt::get(pricing, "model_price");
      if (!isRecord(ratioMap) || !isRecord(fixedPriceMap)) {
        throw MeteredError("Metered pricing initialization received an invalid pricing configuration.", "metered_pricing_invalid", 502);
      }

      std::unordered_set<std::string> result;
      for (const json::value* pricingMap : {ratioMap, fixedPriceMap}) {
        for (const json::key_value_pair* entry : rt::objectEntries(pricingMap->get_object())) {
          if (nonEmptyText(entry->key()) && isPositiveFiniteNumber(&entry->value())) result.insert(std::string(entry->key()));
        }
      }
      return result;
    }

    // `Record<string, number>`: a plain object written with [[Set]].
    JsonRecord modelQuotaCoefficients(const JsonRecord& pricing) {
      const json::value* ratios = rt::get(pricing, "model_ratio");
      const json::value* fixedPrices = rt::get(pricing, "model_price");
      const json::value* completionRatios = rt::get(pricing, "completion_ratio");
      if (!isRecord(ratios) || !isRecord(fixedPrices)) {
        throw MeteredError("Metered pricing initialization received an invalid pricing configuration.", "metered_pricing_invalid", 502);
      }
      JsonRecord result;
      for (const json::key_value_pair* entry : rt::objectEntries(ratios->get_object())) {
        if (!isPositiveFiniteNumber(&entry->value())) continue;
        const json::value* completionRatio = isRecord(completionRatios) ? rt::get(*completionRatios, entry->key()) : nullptr;
        const double completion = isRecord(completionRatios) && isPositiveFiniteNumber(completionRatio) ? rt::toNumber(*completionRatio) : 1;
        const double coefficient = rt::toNumber(entry->value()) * (1 + completion);
        if (std::isfinite(coefficient) && coefficient > 0) rt::objectSet(result, entry->key(), rt::makeNumber(coefficient));
      }
      for (const json::key_value_pair* entry : rt::objectEntries(fixedPrices->get_object())) {
        if (isPositiveFiniteNumber(&entry->value())) rt::objectSet(result, entry->key(), rt::makeNumber(rt::toNumber(entry->value())));
      }
      return result;
    }

    // The `!Array.isArray(value)` guard of the TS protects untyped callers; a std::vector
    // parameter cannot hold anything else.
    std::vector<std::string> normalizeCodexModelIds(const std::vector<std::string>& value) {
      std::vector<std::string> result;
      std::unordered_set<std::string> seen;
      for (const std::string& rawModelId : value) {
        std::optional<std::string> modelId = nonEmptyText(rawModelId);
        if (!modelId) {
          throw MeteredError("Metered pricing initialization received an invalid Codex model identifier.", "metered_pricing_invalid", 502);
        }
        if (seen.contains(*modelId)) continue;
        seen.insert(*modelId);
        result.push_back(std::move(*modelId));
      }
      return result;
    }

    std::optional<SurplusClaudeRequest> meteredClaudeRequest(const JsonRecord& body, const MeteredAuthenticatedFetchOptions& options) {
      const json::value* modelValue = rt::get(body, "model");
      if (!surplus_claude_request::isSurplusClaudeModel(modelValue)) return std::nullopt;
      const std::string model(modelValue->get_string());
      const MeteredModel* catalogModel = nullptr;
      if (meteredModelsCache) {
        for (const MeteredModel& candidate : meteredModelsCache->models) {
          if (candidate.id == model) {
            catalogModel = &candidate;
            break;
          }
        }
      }
      std::optional<double> trustedCap;
      for (const auto& [id, cap] : meteredNativeMaxOutputTokens())
        if (id == model) trustedCap = cap;
      bool anthropicEndpoint = false;
      if (catalogModel)
        for (const std::string& endpoint : catalogModel->supported_endpoint_types) anthropicEndpoint = anthropicEndpoint || endpoint == "anthropic";
      if (!trustedCap && !anthropicEndpoint) return std::nullopt;
      const std::optional<double> catalogCap = catalogModel ? catalogModel->max_output_tokens : std::nullopt;
      surplus_claude_request::SurplusClaudeRequestOptions requestOptions;
      requestOptions.provider = "metered";
      requestOptions.reasoningReplayScope = options.reasoningReplayScope;
      // Native adaptive thinking is observed, but the route's explicit effort
      // contract needs separate proof before it can replace Responses requests.
      requestOptions.supportsNativeReasoningEffort = false;
      return surplus_claude_request::buildSurplusClaudeRequest(body, catalogCap ? catalogCap : trustedCap, requestOptions);
    }

    rt::Task<rt::Response> deliverMeteredResponse(JsonRecord body, rt::Response response, std::optional<std::string> requestId,
                                                  MeteredAuthenticatedFetchOptions options, std::optional<SurplusClaudeRequest> native) {
      if (native) {
        anthropic_responses::AnthropicResponsesOptions normalizeOptions;
        normalizeOptions.model = rt::toString(rt::get(body, "model"));
        normalizeOptions.requestId = requestId;
        normalizeOptions.signal = options.signal;
        const json::value* reasoning = rt::get(body, "reasoning");
        normalizeOptions.reasoningSummary = isRecord(reasoning) ? nonEmptyString(rt::get(*reasoning, "summary")) : std::nullopt;
        normalizeOptions.reasoningReplayScope = options.reasoningReplayScope;
        normalizeOptions.provider = "metered";
        normalizeOptions.toolNames = native->toolNames;
        rt::Promise<rt::Response> normalizing(anthropic_responses::normalizeAnthropicResponsesResponse(response, std::move(normalizeOptions)));
        rt::Response normalized = co_await normalizing.wait();
        co_return normalized;
      }
      if (!surplus_claude_request::isSurplusClaudeModel(rt::get(body, "model"))) co_return response;
      rt::Headers warnings(response.headers());
      const std::optional<std::string> previous = warnings.get("x-uos-warning");
      const std::string warning = "openlux_claude_native_prompt_cache_unavailable";
      warnings.set("x-uos-warning", previous && !previous->empty() ? *previous + ", " + warning : warning);
      rt::ResponseInit init;
      init.status = response.status();
      init.statusText = response.statusText();
      init.headers = std::move(warnings);
      co_return rt::Response(rt::bodyInit(response.body()), std::move(init));
    }

    std::string encodeMeteredResponseBody(const JsonRecord& body) {
      std::optional<std::string> encodedBody;
      try {
        encodedBody = rt::jsonStringify(json::value(body));
      } catch (...) {
        throw MeteredError("Metered Responses requests must use a JSON-serializable body.", "metered_request_invalid", 400);
      }
      if (!encodedBody) {
        throw MeteredError("Metered Responses requests must use a JSON-serializable body.", "metered_request_invalid", 400);
      }
      return *encodedBody;
    }

    rt::Headers meteredResponsesHeaders(const std::string& apiKey, const std::optional<SurplusClaudeRequest>& native) {
      const json::value* stream = native ? rt::get(native->body, "stream") : nullptr;
      const bool streaming = stream && stream->is_bool() && stream->get_bool();
      rt::Headers headers = authenticatedHeaders(apiKey, native && !streaming ? "application/json" : "text/event-stream");
      headers.set("Content-Type", "application/json");
      if (native) headers.set("anthropic-version", "2023-06-01");
      return headers;
    }

    std::optional<JsonRecord> tokenLogMetadata(const json::value* value) {
      if (isRecord(value)) return value->get_object();
      if (!value || !value->is_string()) return std::nullopt;
      try {
        json::value parsed = rt::jsonParse(value->get_string());
        if (!isRecord(&parsed)) return std::nullopt;
        return std::move(parsed.as_object());
      } catch (...) {
        // Ignore malformed provider metadata, retaining authoritative quota.
        return std::nullopt;
      }
    }

    // `Pick<MeteredTokenLogEntry, "prompt_tokens" | "cached_prompt_tokens" | "cache_write_prompt_tokens">`
    struct TokenLogCacheCounts {
      double prompt_tokens;
      std::optional<double> cached_prompt_tokens{};
      std::optional<double> cache_write_prompt_tokens{};
    };

    TokenLogCacheCounts tokenLogCacheCounts(const std::string& model, double promptTokens, const json::value* cachedPromptTokens,
                                            const std::optional<JsonRecord>& other) {
      TokenLogCacheCounts fallback{.prompt_tokens = promptTokens};
      if (isNonNegativeSafeInteger(cachedPromptTokens)) fallback.cached_prompt_tokens = rt::toNumber(*cachedPromptTokens);
      // OpenLux's native Claude logs report uncached input in prompt_tokens, with
      // cache reads/writes separately in `other`. The actual Messages receipts
      // and settled rows agree on this contract. Never apply it to a model or row
      // whose native Claude flag/counters are absent or invalid.
      const json::value* claude = other ? rt::get(*other, "claude") : nullptr;
      const json::value* cacheTokens = other ? rt::get(*other, "cache_tokens") : nullptr;
      if (!surplus_claude_request::isSurplusClaudeModel(std::string_view(model)) || !(claude && claude->is_bool() && claude->get_bool()) ||
          !isNonNegativeSafeInteger(cacheTokens))
        return fallback;
      const json::value* cacheCreationTokens = rt::get(*other, "cache_creation_tokens");
      const json::value zero(0);
      const json::value* write = cacheCreationTokens ? cacheCreationTokens : &zero;
      if (!isNonNegativeSafeInteger(write)) return fallback;
      const double gross = promptTokens + rt::toNumber(*cacheTokens) + rt::toNumber(*write);
      if (!isNonNegativeSafeInteger(gross)) return fallback;
      TokenLogCacheCounts counts{.prompt_tokens = gross, .cached_prompt_tokens = rt::toNumber(*cacheTokens)};
      if (cacheCreationTokens) counts.cache_write_prompt_tokens = rt::toNumber(*write);
      return counts;
    }

    std::optional<MeteredTokenLogEntry> normalizeTokenLogEntry(const json::value& value) {
      if (!isRecord(&value)) return std::nullopt;
      const JsonRecord& row = value.get_object();
      const std::optional<JsonRecord> other = tokenLogMetadata(rt::get(row, "other"));
      std::optional<std::string> requestId = nonEmptyString(rt::get(row, "request_id"));
      if (!requestId && other) requestId = nonEmptyString(rt::get(*other, "request_id"));
      std::optional<std::string> model = nonEmptyString(rt::get(row, "model_name"));
      const json::value* quota = rt::get(row, "quota");
      const json::value* promptTokens = rt::get(row, "prompt_tokens");
      const json::value* completionTokens = rt::get(row, "completion_tokens");
      const json::value* createdAt = rt::get(row, "created_at");
      if (!requestId || !model || !isNonNegativeSafeInteger(quota) || !isNonNegativeSafeInteger(promptTokens) || !isNonNegativeSafeInteger(completionTokens) ||
          !isNonNegativeSafeInteger(createdAt)) {
        return std::nullopt;
      }

      const TokenLogCacheCounts counts = tokenLogCacheCounts(*model, rt::toNumber(*promptTokens), rt::get(row, "cached_prompt_tokens"), other);
      return MeteredTokenLogEntry{
        .request_id = std::move(*requestId),
        .quota = rt::toNumber(*quota),
        .prompt_tokens = counts.prompt_tokens,
        .cached_prompt_tokens = counts.cached_prompt_tokens,
        .cache_write_prompt_tokens = counts.cache_write_prompt_tokens,
        .completion_tokens = rt::toNumber(*completionTokens),
        .model = std::move(*model),
        .created_at = rt::toNumber(*createdAt),
      };
    }

    rt::URL meteredTokenLogPageUrl(const std::string& apiKey, double page, double pageSize, std::optional<double> startAtMs, std::optional<double> endAtMs) {
      rt::URL url{std::string(METERED_TOKEN_LOGS_URL)};
      url.searchParams().set("key", apiKey);
      url.searchParams().set("page", rt::numberToString(page));
      url.searchParams().set("page_size", rt::numberToString(pageSize));
      if (startAtMs && std::isfinite(*startAtMs)) {
        url.searchParams().set("start_timestamp", rt::numberToString(rt::Math::max(0.0, rt::Math::floor(*startAtMs / 1'000))));
      }
      if (endAtMs && std::isfinite(*endAtMs)) {
        url.searchParams().set("end_timestamp", rt::numberToString(rt::Math::max(0.0, rt::Math::ceil(*endAtMs / 1'000))));
      }
      return url;
    }

    const json::array* envelopeItems(const json::value* data) {
      if (data && data->is_array()) return &data->get_array();
      if (isRecord(data)) return rt::getArray(*data, "items");
      return nullptr;
    }

    // `Readonly<{ items: readonly unknown[]; total: number }>`
    struct MeteredTokenLogPage {
      json::array items;
      double total;
    };

    rt::Task<MeteredTokenLogPage> fetchMeteredTokenLogPage(std::string apiKey, double page, double pageSize, MeteredTokenLogFetchOptions options,
                                                           rt::AbortSignal signal) {
      rt::URL url = meteredTokenLogPageUrl(apiKey, page, pageSize, options.startAtMs, options.endAtMs);
      std::optional<rt::Response> response;
      rt::Reason failure;
      try {
        rt::RequestInit init;
        init.method = "GET";
        init.headers = metadataHeaders();
        init.redirect = "manual";
        init.signal = signal;
        const MeteredFetch fetcher = options.fetcher ? options.fetcher : rt::globalFetch();
        rt::Promise<rt::Response> responsePromise(fetcher(url, std::move(init)));
        rt::Promise<rt::Response> responded = awaitWithAbort(responsePromise, signal);
        response = co_await responded.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        rethrowCancellation(failure, signal);
        throw MeteredError("Metered billing logs could not be reached.", "metered_logs_unavailable", 502);
      }

      if (!response->ok()) {
        throw MeteredError("Metered billing logs returned an unsuccessful response.", "metered_logs_unavailable", 502, response->status());
      }

      std::optional<json::value> envelope;
      try {
        rt::Promise<json::value> reading(response->json());
        rt::Promise<json::value> read = awaitWithAbort(reading, signal);
        envelope = co_await read.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        rethrowCancellation(failure, signal);
        throw MeteredError("Metered billing logs returned invalid JSON.", "metered_logs_invalid", 502, response->status());
      }
      const json::value* success = rt::get(*envelope, "success");
      if (!isRecord(&*envelope) || !(success && success->is_bool() && success->get_bool())) {
        throw MeteredError("Metered billing logs returned an invalid response envelope.", "metered_logs_invalid", 502, response->status());
      }
      const json::value* data = rt::get(*envelope, "data");
      const json::array* items = envelopeItems(data);
      if (!items) {
        throw MeteredError("Metered billing logs returned an invalid response envelope.", "metered_logs_invalid", 502, response->status());
      }
      const json::value* total = isRecord(data) ? rt::get(*data, "total") : nullptr;
      MeteredTokenLogPage result{.items = *items, .total = isNonNegativeSafeInteger(total) ? rt::toNumber(*total) : static_cast<double>(items->size())};
      co_return result;
    }
  }

  rt::Task<std::optional<MeteredModelsSnapshot>> fetchMeteredModels() { return fetchMeteredModels(FetchMeteredModelsOptions{}); }

  rt::Task<std::optional<MeteredModelsSnapshot>> fetchMeteredModels(FetchMeteredModelsOptions options) {
    const bool force = options.force.value_or(false);
    if (!force && meteredModelsCache && rt::dateNow() - meteredModelsCache->updated_at_ms < METERED_MODELS_CACHE_TTL_MS) {
      co_return meteredModelsCache;
    }
    if (!force && rt::dateNow() < meteredModelsRetryAfterMs) co_return meteredModelsCache;
    const std::optional<std::string> apiKey = readModelDiscoveryApiKey();
    if (!apiKey) co_return meteredModelsCache;
    if (options.cachedOnly.value_or(false)) co_return meteredModelsCache;
    MeteredFetch fetcher = options.fetcher;
    if (!fetcher) fetcher = meteredModelsFetchForTest;
    if (!fetcher && rt::globalFetch() == rt::defaultFetch()) fetcher = rt::defaultFetch();
    if (!fetcher) co_return meteredModelsCache;
    // Only the ordinary discovery path shares an upstream request. Callers that
    // supply a signal, fetcher, or force a refresh retain their own request
    // semantics and do not join a request owned by another caller. `cachedOnly`
    // callers have already returned above, so it is not repeated here.
    const bool shouldCoalesce = !force && !options.fetcher && !options.signal;
    if (shouldCoalesce && meteredModelsFetchInFlight) {
      const rt::Promise<std::optional<MeteredModelsSnapshot>> inFlight = *meteredModelsFetchInFlight;
      std::optional<MeteredModelsSnapshot> shared = co_await inFlight.wait();
      co_return shared;
    }

    const double requestGeneration = ++meteredModelsFetchGeneration;
    const rt::Promise<std::optional<MeteredModelsSnapshot>> request(requestMeteredModels(fetcher, *apiKey, options.signal, requestGeneration));
    if (!shouldCoalesce) {
      std::optional<MeteredModelsSnapshot> own = co_await request.wait();
      co_return own;
    }
    meteredModelsFetchInFlight = request;
    const auto cleanup = rt::scopeExit([request] {
      if (meteredModelsFetchInFlight && *meteredModelsFetchInFlight == request) meteredModelsFetchInFlight.reset();
    });
    auto result = co_await request.wait();
    co_return result;
  }

  std::optional<MeteredModelsSnapshot> meteredModelsSnapshot() { return meteredModelsCache; }

  void resetMeteredModelsCacheForTest() {
    meteredModelsFetchInFlight.reset();
    meteredModelsFetchGeneration += 1;
    meteredModelsCacheGeneration = meteredModelsFetchGeneration;
    meteredModelsRetryAfterMs = 0;
    meteredModelsCache.reset();
    rt::memory::setCache(rt::memory::Cache::openlux_models, 0, 0);
  }

  void setMeteredModelsFetchForTest(MeteredFetch fetcher) { meteredModelsFetchForTest = std::move(fetcher); }

  std::optional<std::string> readMeteredApiKey() {
    try {
      std::optional<std::string> metered = nonEmptyString(rt::env::get(METERED_API_KEY_ENV));
      return metered ? metered : nonEmptyString(rt::env::get(OPENLUX_API_KEY_ENV));
    } catch (...) {
      return std::nullopt;
    }
  }

  rt::Task<MeteredPricingSnapshot> initializeMeteredPricing(InitializeMeteredPricingOptions options) {
    const MeteredFetch fetcher = options.fetcher ? options.fetcher : rt::globalFetch();
    std::vector<rt::Promise<json::value>> envelopes;
    envelopes.push_back(rt::Promise<json::value>(
      fetchMetadataJson(std::string(METERED_RATIO_CONFIG_URL), fetcher, options.signal, "metered_pricing_unavailable", "metered_pricing_invalid")));
    envelopes.push_back(rt::Promise<json::value>(
      fetchMetadataJson(std::string(METERED_STATUS_URL), fetcher, options.signal, "metered_status_unavailable", "metered_status_invalid")));
    const rt::Promise<std::vector<json::value>> both = rt::all(std::move(envelopes));
    const std::vector<json::value> settled = co_await both.wait();
    const json::value& pricingEnvelope = settled.at(0);
    const json::value& statusEnvelope = settled.at(1);

    const JsonRecord pricing = unwrapSuccessfulEnvelope(pricingEnvelope, "metered_pricing_invalid");
    const JsonRecord status = unwrapSuccessfulEnvelope(statusEnvelope, "metered_status_invalid");
    const json::value* setup = rt::get(status, "setup");
    const json::value* quotaPerUnit = rt::get(status, "quota_per_unit");
    if (!(setup && setup->is_bool() && setup->get_bool()) || !isPositiveFiniteNumber(quotaPerUnit) || !rt::Number::isSafeInteger(*quotaPerUnit)) {
      throw MeteredError("Metered pricing initialization received an invalid quota conversion.", "metered_status_invalid", 502);
    }

    const std::unordered_set<std::string> availableModelIds = pricedModelIds(pricing);
    const JsonRecord coefficients = modelQuotaCoefficients(pricing);
    std::vector<std::string> eligibleModelIds;
    for (std::string& modelId : normalizeCodexModelIds(options.codexModelIds))
      if (availableModelIds.contains(modelId)) eligibleModelIds.push_back(std::move(modelId));
    const double checkedAtMs = rt::Math::trunc(options.now ? options.now() : rt::dateNow());
    if (!rt::Number::isSafeInteger(checkedAtMs) || checkedAtMs < 0) {
      throw MeteredError("Metered pricing initialization received an invalid clock value.", "metered_status_invalid", 502);
    }

    std::vector<std::pair<std::string, json::value>> coefficientEntries;
    for (const std::string& model : eligibleModelIds) {
      const json::value* coefficient = rt::get(coefficients, model);
      if (coefficient && rt::truthy(*coefficient)) coefficientEntries.emplace_back(model, *coefficient);
    }
    MeteredPricingSnapshot snapshot{
      .eligible_model_ids = std::move(eligibleModelIds),
      .quota_per_credit = rt::toNumber(*quotaPerUnit),
      .model_quota_coefficients = rt::objectFromEntries(coefficientEntries),
      .checked_at_ms = checkedAtMs,
    };
    co_return snapshot;
  }

  rt::Task<MeteredResponsesResult> fetchMeteredResponses(json::value body) { return fetchMeteredResponses(std::move(body), MeteredAuthenticatedFetchOptions{}); }

  rt::Task<MeteredResponsesResult> fetchMeteredResponses(json::value body, MeteredAuthenticatedFetchOptions options) {
    if (!isRecord(&body)) {
      throw MeteredError("Metered Responses requests must use a canonical JSON object body.", "metered_request_invalid", 400);
    }
    const JsonRecord record = body.get_object();

    std::optional<SurplusClaudeRequest> native = meteredClaudeRequest(record, options);
    const std::string encodedBody = encodeMeteredResponseBody(native ? native->body : toMeteredResponsesBody(record));
    const rt::Headers headers = meteredResponsesHeaders(requireMeteredApiKey(options.apiKey), native);

    std::optional<rt::Response> response;
    std::optional<AttemptHandle> upstreamAttempt;
    const rt::AbortController headersDeadline;
    {
      const rt::Timeout headersTimer = rt::setTimeout(
        [headersDeadline] { headersDeadline.abort(rt::makeReason(rt::DOMException("Metered response headers timed out.", "TimeoutError"))); },
        inference_deadline::STREAM_FIRST_EVENT_DEADLINE_MS);
      // The deadline covers only request dispatch and response headers. Once a
      // streaming body exists, the shared SSE reader owns renewable inactivity.
      const auto clearHeadersTimer = rt::scopeExit([headersTimer] { rt::clearTimeout(headersTimer); });
      const rt::AbortSignal signal = options.signal ? rt::AbortSignal::any({*options.signal, headersDeadline.signal()}) : headersDeadline.signal();
      rt::Reason failure;
      try {
        // Dispatch-based quota accounting must settle immediately before this
        // real provider transport, after all local validation has succeeded.
        // Do not `await undefined`: that would insert a microtask boundary before
        // a normal fetch and let an immediately aborted caller race past a custom
        // transport's abort listener. When a quota hook exists it remains the
        // final awaited operation before transport.
        std::optional<api_key_policy::ApiKeyProviderDispatch> dispatch;
        if (options.beforeDispatch) {
          const rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>> admitted(options.beforeDispatch());
          dispatch = co_await admitted.wait();
        }
        if (signal.aborted()) {
          if (dispatch) {
            const rt::Promise<void> cancelled(dispatch->cancelBeforeTransport());
            co_await cancelled.wait();
          } else {
            co_await rt::microtask();
          }
          rt::rethrow(reasonOr(signal.reason(), rt::makeReason(rt::DOMException("The request was aborted.", "AbortError"))));
        }
        if (dispatch) dispatch->markTransportStarted();
        if (options.onDispatch) options.onDispatch();
        if (options.sentinelUpstreamRecorder) upstreamAttempt = options.sentinelUpstreamRecorder->startAttempt("metered");
        rt::RequestInit init;
        init.method = "POST";
        init.headers = headers;
        init.body = rt::BodyInit(encodedBody);
        init.redirect = "manual";
        init.signal = signal;
        const MeteredFetch fetcher = options.fetcher ? options.fetcher : rt::globalFetch();
        const rt::Promise<rt::Response> fetching(fetcher(std::string(native ? METERED_MESSAGES_URL : METERED_RESPONSES_URL), std::move(init)));
        response = co_await fetching.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        if (upstreamAttempt) upstreamAttempt->recordFetchError();
        if (isApiKeyQuotaDispatchError(failure)) rt::rethrow(failure);
        if (options.signal && options.signal->aborted()) rt::rethrow(reasonOr(options.signal->reason(), failure));
        if (headersDeadline.signal().aborted()) rt::rethrow(reasonOr(headersDeadline.signal().reason(), failure));
        rethrowCancellation(failure, signal);
        throw MeteredError("Metered Responses request could not reach the upstream service.", "metered_upstream_unreachable", 502);
      }
    }

    std::optional<std::string> requestId = nonEmptyString(response->headers().get("X-Api-Request-Id"));
    if (!requestId) requestId = nonEmptyString(response->headers().get("X-Oneapi-Request-Id"));
    if (!requestId) requestId = nonEmptyString(response->headers().get("X-Request-Id"));
    const rt::Response recorded = upstreamAttempt ? upstreamAttempt->wrap(*response) : *response;
    const rt::Promise<rt::Response> delivering(deliverMeteredResponse(record, recorded, requestId, options, native));
    rt::Response delivered = co_await delivering.wait();
    MeteredResponsesResult result{.response = std::move(delivered), .request_id = std::move(requestId)};
    if (native) {
      result.prompt_cache_mode = native->promptCacheMode;
      result.explicit_breakpoint_count = native->explicitBreakpointCount;
    }
    co_return result;
  }

  rt::Task<std::vector<MeteredTokenLogEntry>> fetchMeteredTokenLogs() { return fetchMeteredTokenLogs(MeteredTokenLogFetchOptions{}); }

  rt::Task<std::vector<MeteredTokenLogEntry>> fetchMeteredTokenLogs(MeteredTokenLogFetchOptions options) {
    const std::string apiKey = requireMeteredApiKey(options.apiKey);
    const rt::AbortSignal signal = boundedTokenLogSignal(options.signal);
    std::unordered_set<std::string> requestedIds;
    if (options.requestIds)
      for (const std::string& id : *options.requestIds)
        if (std::optional<std::string> trimmed = nonEmptyText(id)) requestedIds.insert(std::move(*trimmed));
    std::unordered_set<std::string> foundIds;
    std::vector<MeteredTokenLogEntry> logs;
    const double pageSize = 100;

    for (double page = 1; page <= 100; page += 1) {
      const rt::Promise<MeteredTokenLogPage> fetched(fetchMeteredTokenLogPage(apiKey, page, pageSize, options, signal));
      const MeteredTokenLogPage logPage = co_await fetched.wait();
      for (const json::value& item : logPage.items) {
        std::optional<MeteredTokenLogEntry> log = normalizeTokenLogEntry(item);
        if (!log) continue;
        if (requestedIds.contains(log->request_id)) foundIds.insert(log->request_id);
        logs.push_back(std::move(*log));
      }
      if (!requestedIds.empty() && foundIds.size() == requestedIds.size()) break;
      if (page * pageSize >= logPage.total || logPage.items.empty()) break;
    }
    co_return logs;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredModel& value) {
    out = Writer()
            .required("id", value.id)
            .required("object", value.object)
            .required("created", value.created)
            .required("owned_by", value.owned_by)
            .required("supported_endpoint_types", value.supported_endpoint_types)
            .orUndefined("model_type", value.model_type)
            .orUndefined("description", value.description)
            .orUndefined("tags", value.tags)
            .orUndefined("max_output_tokens", value.max_output_tokens)
            .done();
  }

  MeteredModel tag_invoke(const json::value_to_tag<MeteredModel>&, const json::value& value) {
    const Reader in(value, "MeteredModel");
    MeteredModel model{
      .id = in.required<std::string>("id"),
      .object = in.required<std::string>("object"),
      .created = in.required<double>("created"),
      .owned_by = in.required<std::string>("owned_by"),
      .supported_endpoint_types = in.required<std::vector<std::string>>("supported_endpoint_types"),
      .model_type = in.orUndefined<std::string>("model_type"),
      .description = in.orUndefined<std::string>("description"),
      .tags = in.orUndefined<std::string>("tags"),
      .max_output_tokens = in.orUndefined<double>("max_output_tokens"),
    };
    return model;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredModelsSnapshot& value) {
    out = Writer().required("models", value.models).required("updated_at_ms", value.updated_at_ms).done();
  }

  MeteredModelsSnapshot tag_invoke(const json::value_to_tag<MeteredModelsSnapshot>&, const json::value& value) {
    const Reader in(value, "MeteredModelsSnapshot");
    MeteredModelsSnapshot snapshot{
      .models = in.required<std::vector<MeteredModel>>("models"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
    return snapshot;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredPricingSnapshot& value) {
    out = Writer()
            .required("eligible_model_ids", value.eligible_model_ids)
            .required("quota_per_credit", value.quota_per_credit)
            .required("model_quota_coefficients", value.model_quota_coefficients)
            .required("checked_at_ms", value.checked_at_ms)
            .done();
  }

  MeteredPricingSnapshot tag_invoke(const json::value_to_tag<MeteredPricingSnapshot>&, const json::value& value) {
    const Reader in(value, "MeteredPricingSnapshot");
    MeteredPricingSnapshot snapshot{
      .eligible_model_ids = in.required<std::vector<std::string>>("eligible_model_ids"),
      .quota_per_credit = in.required<double>("quota_per_credit"),
      .model_quota_coefficients = in.required<json::object>("model_quota_coefficients"),
      .checked_at_ms = in.required<double>("checked_at_ms"),
    };
    return snapshot;
  }
}
