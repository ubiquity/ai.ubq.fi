#include "uos/provider/openrouter.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace uos::provider::openrouter {
  namespace {
    namespace catalogue = models::openrouter_models;
    using ModelResolver = std::optional<std::string> (*)(std::string_view);
    rt::Task<std::optional<std::string>> resolveOpenRouterModel(std::string model, ModelResolver resolveWarm) {
      const auto resolved = resolveWarm(model);
      const auto trimmed = rt::trim(model);
      static const rt::Regex OPENROUTER_MODEL_ID_PATTERN(R"(^[^/\s]+/[^/\s]+$)");
      if (resolved || catalogue::openRouterModelsSnapshot() || !OPENROUTER_MODEL_ID_PATTERN.test(trimmed)) co_return resolved;
      if (!readOpenRouterApiKey()) co_return std::nullopt;
      try {
        const catalogue::FetchOpenRouterModelsOptions options;
        rt::Promise<std::shared_ptr<const catalogue::OpenRouterModelsSnapshot>> refresh(catalogue::fetchOpenRouterModels(options));
        auto ignoring = refresh.catch_([](rt::Reason) { return std::shared_ptr<const catalogue::OpenRouterModelsSnapshot>(); });
        auto ignored = co_await ignoring.wait();
        (void)ignored;
      } catch (...) {
      }
      co_return resolveWarm(model);
    }
    rt::Task<rt::Response> dispatchOpenRouter(std::string url, json::object body, OpenRouterDispatchInit init) {
      const auto apiKey = readOpenRouterApiKey();
      if (!apiKey) throw OpenRouterError("openrouter_api_key_missing", 503);
      std::optional<api_key_policy::ApiKeyProviderDispatch> dispatch;
      if (init.hooks && init.hooks->beforeDispatch) {
        rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>> admission(init.hooks->beforeDispatch());
        dispatch = co_await admission.wait();
      }
      if (init.signal.aborted()) {
        if (dispatch) {
          rt::Promise<void> cancellation(dispatch->cancelBeforeTransport());
          co_await cancellation.wait();
        } else {
          co_await rt::microtask();
        }
        throw rt::DOMException("Aborted", "AbortError");
      }
      if (dispatch) dispatch->markTransportStarted();
      if (init.hooks && init.hooks->onDispatch) init.hooks->onDispatch();
      std::optional<sentinel::upstream_capture::SentinelUpstreamAttemptHandle> upstreamAttempt;
      if (init.hooks && init.hooks->sentinelUpstreamRecorder) upstreamAttempt = init.hooks->sentinelUpstreamRecorder->startAttempt("openrouter");
      rt::Response response;
      try {
        rt::RequestInit request;
        request.method = "POST";
        request.headers = rt::Headers{{"accept", "application/json"}, {"authorization", "Bearer " + *apiKey}, {"content-type", "application/json"}};
        request.body = rt::jsonStringify(body);
        request.signal = init.signal;
        rt::RequestInfo input(url);
        rt::Promise<rt::Response> transport(rt::fetch(input, request));
        response = co_await transport.wait();
      } catch (const rt::DOMException& error) {
        if (upstreamAttempt) upstreamAttempt->recordFetchError();
        if (error.name == "AbortError") throw;
        throw OpenRouterError("openrouter_upstream_unreachable", 502);
      } catch (...) {
        if (upstreamAttempt) upstreamAttempt->recordFetchError();
        throw OpenRouterError("openrouter_upstream_unreachable", 502);
      }
      if (init.hooks && init.hooks->onHeaders) init.hooks->onHeaders();
      co_return upstreamAttempt ? upstreamAttempt->wrap(response) : response;
    }
    rt::Task<rt::Response> fetchOpenRouterText(std::string url, json::object body, OpenRouterDispatchInit init) {
      rt::Promise<rt::Response> dispatched(dispatchOpenRouter(std::move(url), std::move(body), std::move(init)));
      auto response = co_await dispatched.wait();
      co_return response;
    }
    rt::Task<json::object> fetchOpenRouterDecisionPayload(std::string url, OpenRouterDecisionInput input) {
      std::optional<std::string> apiKey;
      if (input.apiKey.isUndefined()) apiKey = readOpenRouterApiKey();
      else if (input.apiKey.hasValue()) apiKey = *input.apiKey;
      if (!apiKey || apiKey->empty()) throw OpenRouterError("openrouter_api_key_missing", 503);
      const auto deadline = rt::AbortSignal::timeout(input.timeoutMs.value_or(OPENROUTER_FETCH_TIMEOUT_MS));
      const auto signal = input.signal ? rt::AbortSignal::any({*input.signal, deadline}) : deadline;
      std::optional<api_key_policy::ApiKeyProviderDispatch> dispatch;
      if (input.hooks && input.hooks->beforeDispatch) {
        rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>> admission(input.hooks->beforeDispatch());
        dispatch = co_await admission.wait();
      }
      if (signal.aborted()) {
        if (dispatch) {
          rt::Promise<void> cancellation(dispatch->cancelBeforeTransport());
          co_await cancellation.wait();
        } else {
          co_await rt::microtask();
        }
        throw OpenRouterError("openrouter_upstream_unreachable", 502, std::nullopt, true);
      }
      if (dispatch) dispatch->markTransportStarted();
      if (input.hooks && input.hooks->onDispatch) input.hooks->onDispatch();
      const auto fetcher = input.fetcher ? input.fetcher : rt::globalFetch();
      rt::Response response;
      try {
        rt::RequestInit request;
        request.method = "POST";
        request.headers = rt::Headers{{"accept", "application/json"}, {"authorization", "Bearer " + *apiKey}, {"content-type", "application/json"}};
        request.body = rt::jsonStringify(input.body);
        request.signal = signal;
        rt::RequestInfo destination(url);
        rt::Promise<rt::Response> transport(fetcher(destination, request));
        response = co_await transport.wait();
      } catch (...) {
        if (input.signal && input.signal->aborted()) throw;
        throw OpenRouterError("openrouter_upstream_unreachable", 502);
      }
      if (input.hooks && input.hooks->onHeaders) input.hooks->onHeaders();
      if (!response.ok()) {
        if (const auto body = response.body()) {
          auto cancellation = body->cancelPromise();
          auto ignoring = cancellation.catch_([](rt::Reason) {});
          co_await ignoring.wait();
        } else {
          co_await rt::microtask();
        }
        const double status = response.status() == 429 || response.status() == 400 || response.status() == 413 ? response.status() : 502;
        throw OpenRouterError("openrouter_upstream_error", status, response.status());
      }
      json::value payload;
      rt::Promise<json::value> decoding(response.json());
      auto ignoring = decoding.catch_([](rt::Reason) { return json::value(nullptr); });
      payload = co_await ignoring.wait();
      if (!utils::isRecord(payload) || payload.is_array()) {
        if (const auto body = response.body()) {
          auto cancellation = body->cancelPromise();
          (void)cancellation.catch_([](rt::Reason) {});
        }
        throw OpenRouterError("openrouter_upstream_invalid_response", 502);
      }
      co_return payload.as_object();
    }
    rt::Task<json::object> fetchOpenRouterDecision(std::string url, OpenRouterDecisionInput input) {
      rt::Promise<json::object> dispatched(fetchOpenRouterDecisionPayload(std::move(url), std::move(input)));
      auto payload = co_await dispatched.wait();
      co_return payload;
    }
  }

  std::vector<std::string> openRouterTextModelIds() {
    std::vector<std::string> ids;
    const auto snapshot = catalogue::openRouterModelsSnapshot();
    if (snapshot) for (const auto& model : snapshot->models) if (catalogue::openRouterModelSupportsText(model)) ids.push_back(model.id);
    return ids;
  }
  std::vector<std::string> openRouterDecisionModelIds() {
    std::vector<std::string> ids;
    const auto snapshot = catalogue::openRouterModelsSnapshot();
    if (snapshot) for (const auto& model : snapshot->models) if (catalogue::openRouterModelSupportsDecisions(model)) ids.push_back(model.id);
    return ids;
  }
  std::optional<std::string> openRouterUpstreamModelFor(std::string_view model) {
    const auto trimmed = rt::trim(model);
    if (trimmed.empty()) return std::nullopt;
    const auto ids = openRouterTextModelIds();
    return std::find(ids.begin(), ids.end(), trimmed) != ids.end() ? std::optional<std::string>(trimmed) : std::nullopt;
  }
  std::optional<std::string> openRouterDecisionUpstreamModelFor(std::string_view model) {
    const auto trimmed = rt::trim(model);
    if (trimmed.empty()) return std::nullopt;
    const auto ids = openRouterDecisionModelIds();
    if (std::find(ids.begin(), ids.end(), trimmed) != ids.end()) return trimmed;
    if (trimmed == "typesafe/jev-latest" && std::find(ids.begin(), ids.end(), "~typesafe/jev-latest") != ids.end()) return "~typesafe/jev-latest";
    return std::nullopt;
  }
  rt::Task<std::optional<std::string>> resolveOpenRouterUpstreamModel(std::string model) {
    rt::Promise<std::optional<std::string>> resolving(resolveOpenRouterModel(std::move(model), openRouterUpstreamModelFor));
    auto result = co_await resolving.wait();
    co_return result;
  }
  rt::Task<std::optional<std::string>> resolveOpenRouterDecisionUpstreamModel(std::string model) {
    rt::Promise<std::optional<std::string>> resolving(resolveOpenRouterModel(std::move(model), openRouterDecisionUpstreamModelFor));
    auto result = co_await resolving.wait();
    co_return result;
  }
  rt::Task<rt::Response> fetchOpenRouterChatCompletions(const json::object& body, const OpenRouterDispatchInit& init) {
    return fetchOpenRouterText(std::string(OPENROUTER_CHAT_COMPLETIONS_URL), body, init);
  }
  rt::Task<rt::Response> fetchOpenRouterResponses(const json::object& body, const OpenRouterDispatchInit& init) {
    return fetchOpenRouterText(std::string(OPENROUTER_RESPONSES_URL), body, init);
  }
  std::optional<std::string> readOpenRouterApiKey() {
    try {
      const auto value = rt::env::get(OPENROUTER_API_KEY_ENV);
      if (!value) return std::nullopt;
      const auto trimmed = rt::trim(*value);
      return trimmed.empty() ? std::nullopt : std::optional<std::string>(trimmed);
    } catch (...) { return std::nullopt; }
  }
  rt::Task<json::object> fetchOpenRouterSystemOne(const OpenRouterDecisionInput& input) {
    return fetchOpenRouterDecision(std::string(OPENROUTER_SYSTEMONE_URL), input);
  }
  rt::Task<json::object> fetchOpenRouterDecisions(const OpenRouterDecisionInput& input) {
    auto options = input;
    options.timeoutMs = input.timeoutMs.value_or(OPENROUTER_DECISIONS_TIMEOUT_MS);
    return fetchOpenRouterDecision(std::string(OPENROUTER_DECISIONS_URL), std::move(options));
  }
}
