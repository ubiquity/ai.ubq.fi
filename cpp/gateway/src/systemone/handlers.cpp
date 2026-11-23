// System One (Typesafe Jev) decisions route.
// Served as a terminal inference route with the gateway's normal admission,
// telemetry and usage accounting. The OpenRouter provider owns the credential.
#include "uos/systemone/handlers.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/http.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/selection.hpp"
#include "uos/request.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/utils.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace uos::systemone::handlers {
  namespace {
    namespace telemetry = openai_telemetry;
    namespace router = provider::openrouter;
    constexpr std::array<std::string_view, 3> SYSTEMONE_REQUEST_KEYS{"state", "questions", "model"};
    constexpr std::array<std::string_view, 3> SYSTEMONE_QUESTION_TYPES{"noul", "choice", "score"};
    const rt::Regex SYSTEMONE_MODEL_PATTERN(R"(^~?typesafe/[a-z0-9][a-z0-9._-]{0,63}$)", "u");

    bool validQuestion(const json::value& value) {
      const auto type = rt::getString(value, "type");
      return utils::isRecord(value) && type && std::find(SYSTEMONE_QUESTION_TYPES.begin(), SYSTEMONE_QUESTION_TYPES.end(), *type) != SYSTEMONE_QUESTION_TYPES.end();
    }

    std::vector<std::pair<std::string, const json::value*>> entriesOf(const json::value& value) {
      std::vector<std::pair<std::string, const json::value*>> entries;
      if (value.is_object()) {
        for (const auto* entry : rt::objectEntries(value.as_object())) entries.emplace_back(std::string(entry->key()), &entry->value());
      } else if (value.is_array()) {
        const auto& array = value.as_array();
        for (std::size_t index = 0; index < array.size(); ++index) entries.emplace_back(rt::numberToString(static_cast<double>(index)), &array[index]);
      }
      return entries;
    }

    std::optional<rt::Response> unsupportedKeyError(const json::value& raw) {
      for (const auto& [key, value] : entriesOf(raw)) {
        (void)value;
        if (std::find(SYSTEMONE_REQUEST_KEYS.begin(), SYSTEMONE_REQUEST_KEYS.end(), key) == SYSTEMONE_REQUEST_KEYS.end())
          return http::openaiError(400, "Unsupported key: " + key, "invalid_request_error");
      }
      return std::nullopt;
    }

    std::optional<rt::Response> questionsError(const json::value* questions) {
      if (!utils::isRecord(questions)) return http::openaiError(400, "questions must be an object", "invalid_request_error");
      const auto entries = entriesOf(*questions);
      if (entries.empty() || static_cast<double>(entries.size()) > SYSTEMONE_MAX_QUESTIONS)
        return http::openaiError(400, "questions must hold between 1 and 24 questions", "invalid_request_error");
      for (const auto& [name, question] : entries) {
        if (rt::trim(name).empty() || rt::length(name) > 120 || !validQuestion(*question))
          return http::openaiError(400, "questions must be typed noul, choice, or score objects", "invalid_request_error");
      }
      return std::nullopt;
    }

    std::optional<rt::Response> modelError(const json::value& raw) {
      const auto* model = rt::get(raw, "model");
      if (!model) return std::nullopt;
      const auto requested = utils::getString(model);
      if (!requested) return http::openaiError(400, "model must be a string", "invalid_request_error");
      if (static_cast<double>(rt::length(*requested)) > SYSTEMONE_MAX_MODEL_LENGTH || !SYSTEMONE_MODEL_PATTERN.test(*requested))
        return http::openaiError(400, "model must be a typesafe model id", "invalid_request_error");
      return std::nullopt;
    }

    std::string modelOf(const json::value& raw) { return utils::getString(rt::get(raw, "model")).value_or(std::string(SYSTEMONE_DEFAULT_MODEL)); }

    // OpenRouter requires the tilde-prefixed alias; the catalog advertises the same id without it.
    std::string upstreamModelFor(const std::string& model) { return model.starts_with("~") ? model : "~" + model; }

    // System One reports no prompt caching, so cache reads are an explicit zero.
    std::optional<json::object> normalizedUsage(const json::value* value) {
      if (!utils::isRecord(value)) return std::nullopt;
      const auto input = rt::getNumber(*value, "input_tokens");
      const auto output = rt::getNumber(*value, "output_tokens");
      if (!input || !output) return std::nullopt;
      json::object usage{{"input_tokens", rt::makeNumber(*input)}, {"output_tokens", rt::makeNumber(*output)},
                         {"total_tokens", rt::makeNumber(*input + *output)}, {"input_tokens_details", json::object{{"cached_tokens", 0}}}};
      if (const auto cost = rt::getNumber(*value, "cost")) rt::objectSet(usage, "cost", *cost);
      return usage;
    }

    struct SystemOneRequest { std::string model; json::value state; json::value questions; };

    const std::array<std::pair<std::string_view, std::string_view>, 4> SYSTEMONE_ERROR_MESSAGES{{
      {"openrouter_api_key_missing", "System One upstream is not configured"},
      {"openrouter_upstream_unreachable", "System One upstream unreachable"},
      {"openrouter_upstream_error", "System One upstream error"},
      {"openrouter_upstream_invalid_response", "System One upstream invalid response"}}};

    rt::Response systemOneDispatchFailure(const rt::Reason& failure) {
      try { std::rethrow_exception(failure); }
      catch (const api_key_policy::ApiKeyQuotaDispatchError& error) { return upstream_wire::apiKeyQuotaDispatchErrorResponse(error); }
      catch (const router::OpenRouterError& error) {
        if (error.code == "openrouter_upstream_error" && error.upstreamStatus) recordSystemOneResponseHealth(*error.upstreamStatus);
        else if (!error.beforeTransport && error.code != "openrouter_api_key_missing")
          (void)provider::health::recordOpenRouterProviderHealth("upstream_error", std::nullopt, rt::dateNow);
        for (const auto& [code, message] : SYSTEMONE_ERROR_MESSAGES)
          if (code == error.code) return http::openaiError(error.status, std::string(message), error.code);
        return http::openaiError(error.status, "", error.code);
      }
    }

    std::variant<SystemOneRequest, rt::Response> parseSystemOneRequest(const json::value& raw) {
      if (auto unsupported = unsupportedKeyError(raw)) return *unsupported;
      if (!utils::isRecord(rt::get(raw, "state"))) return http::openaiError(400, "state must be an object", "invalid_request_error");
      if (auto failure = questionsError(rt::get(raw, "questions"))) return *failure;
      if (auto failure = modelError(raw)) return *failure;
      return SystemOneRequest{modelOf(raw), *rt::get(raw, "state"), *rt::get(raw, "questions")};
    }

    rt::Task<rt::Response> handleSystemOneImpl(rt::Request req, std::optional<telemetry::UsageContext> usageContext, SystemOneHandlerDeps deps) {
      rt::Promise<json::value> reading(request::readJsonBody(req, SYSTEMONE_MAX_BODY_BYTES));
      const auto raw = co_await reading.wait();
      if (!utils::isRecord(raw)) co_return http::openaiError(400, "Invalid JSON body", "invalid_request_error");
      const auto parsed = parseSystemOneRequest(raw);
      if (const auto* failure = std::get_if<rt::Response>(&parsed)) co_return *failure;
      const auto request = std::get<SystemOneRequest>(parsed);
      rt::Promise<std::optional<provider::selection::ProviderSelection>> loading(provider::selection::loadProviderSelectionCached());
      const auto selected = co_await loading.wait();
      if (!provider::selection::isProviderEnabled("openrouter", selected))
        co_return http::openaiError(503, "System One provider is disabled by the provider selection", "systemone_provider_disabled");
      const auto state = usageContext && usageContext->responseTelemetry ? usageContext->responseTelemetry : telemetry::createResponseTelemetryState();
      state->provider = "openrouter";
      const auto context = std::make_shared<telemetry::UsageContext>(usageContext.value_or(telemetry::UsageContext{.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt}));
      context->responseTelemetry = state;
      const telemetry::RecordRequestUsageDetails details{request.model, "systemone", false, std::nullopt};
      auto recording = telemetry::recordRequestUsage(context.get(), details);
      co_await recording.wait();
      json::object payload;
      rt::Reason failure;
      try {
        router::OpenRouterDecisionInput input;
        input.body = json::object{{"model", upstreamModelFor(request.model)}, {"state", request.state}, {"questions", request.questions}};
        if (deps.apiKey) input.apiKey = rt::orNull(deps.apiKey());
        if (deps.fetcher) input.fetcher = deps.fetcher;
        router::OpenRouterDispatchHooks hooks;
        hooks.beforeDispatch = [context] {
          if (context->beforeProviderDispatch) return context->beforeProviderDispatch("openrouter");
          return rt::fulfilledTask(std::optional<api_key_policy::ApiKeyProviderDispatch>{});
        };
        hooks.onDispatch = [context] { telemetry::recordFirstProviderDispatch(context.get()); };
        hooks.onHeaders = [context] { telemetry::recordFirstProviderHeaders(context.get()); };
        input.hooks = hooks;
        rt::Promise<json::object> fetching(router::fetchOpenRouterSystemOne(input));
        payload = co_await fetching.wait();
      } catch (...) { failure = std::current_exception(); }
      if (failure) co_return systemOneDispatchFailure(failure);
      if (!utils::isRecord(rt::get(payload, "answers"))) {
        (void)provider::health::recordOpenRouterProviderHealth("upstream_error", std::nullopt, rt::dateNow);
        co_return http::openaiError(502, "System One upstream invalid response", "openrouter_upstream_invalid_response");
      }
      recordSystemOneResponseHealth(200);
      const auto normalized = normalizedUsage(rt::get(payload, "usage"));
      const json::value usageValue = normalized ? json::value(*normalized) : json::value(nullptr);
      const auto usage = telemetry::extractUsageTokens(usageValue);
      auto completion = telemetry::recordCompletionUsage(context.get(), usage);
      co_await completion.wait();
      co_return telemetry::attachResponseTelemetry(rt::Response::json(payload), state);
    }
  }

  void recordSystemOneResponseHealth(double status) {
    const std::string event = status == 401 || status == 403 ? "auth_invalid" : status == 402 || status == 429 ? "quota_exhausted" :
                              status >= 500 ? "upstream_error" : status >= 400 ? "reachable" : "success";
    (void)provider::health::recordOpenRouterProviderHealth(event, status, rt::dateNow);
  }

  rt::Task<rt::Response> handleSystemOne(rt::Request req, std::optional<telemetry::UsageContext> usageContext) {
    return handleSystemOneImpl(std::move(req), std::move(usageContext), SystemOneHandlerDeps{});
  }
  rt::Task<rt::Response> handleSystemOne(rt::Request req, std::optional<telemetry::UsageContext> usageContext, const SystemOneHandlerDeps& deps) {
    return handleSystemOneImpl(std::move(req), std::move(usageContext), deps);
  }
}
