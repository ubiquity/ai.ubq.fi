// First-class OpenRouter Decisions API route.
//
// Decision models answer typed questions about `state` instead of generating
// text. They use OpenRouter's alpha Decisions API, not Chat Completions or
// Responses, so this handler validates the documented envelope and dispatches
// through the shared OpenRouter credential, admission, quota, and telemetry
// treatment. `/v1/systemone` remains the Jev-only compatibility surface.
#include "uos/decisions/handlers.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/http.hpp"
#include "uos/openai.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/selection.hpp"
#include "uos/request.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/utils.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

#include <algorithm>
#include <array>
#include <memory>
#include <utility>
#include <variant>
#include <vector>

namespace uos::decisions::handlers {
  namespace {
    namespace telemetry = openai_telemetry;
    namespace router = provider::openrouter;
    constexpr double DECISIONS_MAX_QUESTIONS = 24;
    constexpr double DECISIONS_MAX_METADATA_LENGTH = 256;
    constexpr double DECISIONS_MAX_QUESTION_NAME_LENGTH = 120;
    constexpr std::array<std::string_view, 7> DECISIONS_REQUEST_KEYS{"model", "state", "questions", "provider", "session_id", "trace", "user"};
    constexpr std::array<std::string_view, 3> DECISIONS_QUESTION_KEYS{"type", "instructions", "criteria"};
    constexpr std::array<std::string_view, 4> DECISIONS_QUESTION_TYPES{"noul", "choice", "score", "box"};
    const rt::Regex DECISIONS_MODEL_PATTERN(R"(^~?[^/\s]+/[^/\s]+$)", "u");

    struct DecisionsRequest {
      std::string model;
      json::value state;
      json::object questions;
      std::optional<json::value> provider{};
      std::optional<std::string> session_id{};
      std::optional<json::object> trace{};
      std::optional<std::string> user{};
    };

    bool isPlainRecord(const json::value* value) { return utils::isRecord(value) && !value->is_array(); }

    /** A string, object, or array is the documented structured-guidance value. */
    bool isGuidanceValue(const json::value* value) { return value && (value->is_string() || isPlainRecord(value) || value->is_array()); }

    rt::Response invalid(const std::string& message, std::optional<std::string> param = std::nullopt) {
      http::OpenaiErrorOptions options;
      options.type = "invalid_request_error";
      if (param) options.param = param;
      return http::openaiError(400, message, "decisions_request_invalid", options);
    }

    std::vector<std::string> keysOf(const json::value& value) {
      if (value.is_object()) return rt::objectKeys(value.as_object());
      std::vector<std::string> keys;
      if (value.is_array()) for (std::size_t index = 0; index < value.as_array().size(); ++index) keys.push_back(rt::numberToString(static_cast<double>(index)));
      return keys;
    }

    std::optional<rt::Response> unsupportedKeyError(const json::value& raw) {
      for (const auto& key : keysOf(raw))
        if (std::find(DECISIONS_REQUEST_KEYS.begin(), DECISIONS_REQUEST_KEYS.end(), key) == DECISIONS_REQUEST_KEYS.end()) return invalid("Unsupported key: " + key);
      return std::nullopt;
    }

    std::optional<rt::Response> modelError(const json::value& raw) {
      const auto value = rt::getString(raw, "model");
      if (!value) return invalid("model must be a string", "model");
      const auto model = rt::trim(*value);
      if (model.empty() || static_cast<std::size_t>(rt::length(model)) > rt::limits::kModelIdCodeUnits || !DECISIONS_MODEL_PATTERN.test(model))
        return invalid("model must be an OpenRouter decision model id", "model");
      return std::nullopt;
    }

    std::optional<rt::Response> stateError(const json::value* state) {
      return state && (state->is_string() || utils::isRecord(state) || state->is_array()) ? std::nullopt :
        std::optional<rt::Response>(invalid("state must be a string, object, or array", "state"));
    }

    std::optional<rt::Response> instructionsError(const json::object& question, const std::string& name) {
      return isGuidanceValue(rt::get(question, "instructions")) ? std::nullopt :
        std::optional<rt::Response>(invalid("questions." + name + ".instructions must be a string, object, or array", "questions"));
    }

    std::optional<rt::Response> noulCriteriaError(const json::value* criteria, const std::string& name) {
      if (!criteria) return std::nullopt;
      if (!isPlainRecord(criteria)) return invalid("questions." + name + ".criteria must be an object", "questions");
      const auto keys = rt::objectKeys(criteria->as_object());
      if (std::any_of(keys.begin(), keys.end(), [](const std::string& key) { return key != "true" && key != "false"; }) ||
          std::find(keys.begin(), keys.end(), "true") == keys.end() || std::find(keys.begin(), keys.end(), "false") == keys.end())
        return invalid("questions." + name + ".criteria must contain exactly true and false", "questions");
      if (!isGuidanceValue(rt::get(*criteria, "true")) || !isGuidanceValue(rt::get(*criteria, "false")))
        return invalid("questions." + name + ".criteria values must be strings, objects, or arrays", "questions");
      return std::nullopt;
    }

    std::optional<rt::Response> choiceCriteriaError(const json::value* criteria, const std::string& name) {
      if (!isPlainRecord(criteria) || criteria->as_object().empty()) return invalid("questions." + name + ".criteria must be a non-empty object", "questions");
      for (const auto* entry : rt::objectEntries(criteria->as_object()))
        if (rt::trim(std::string(entry->key())).empty() || (!entry->value().is_null() && !isGuidanceValue(&entry->value())))
          return invalid("questions." + name + ".criteria entries must have names and string, object, array, or null guidance", "questions");
      return std::nullopt;
    }

    std::optional<rt::Response> scoreCriteriaError(const json::value* criteria, const std::string& name) {
      if (!criteria || !criteria->is_array() || criteria->as_array().empty()) return invalid("questions." + name + ".criteria must be a non-empty array", "questions");
      if (std::any_of(criteria->as_array().begin(), criteria->as_array().end(), [](const json::value& entry) { return !isGuidanceValue(&entry); }))
        return invalid("questions." + name + ".criteria entries must be strings, objects, or arrays", "questions");
      return std::nullopt;
    }

    std::optional<rt::Response> questionError(const std::string& name, const json::value& value) {
      if (!isPlainRecord(&value)) return invalid("questions." + name + " must be an object", "questions");
      for (const auto& key : rt::objectKeys(value.as_object()))
        if (std::find(DECISIONS_QUESTION_KEYS.begin(), DECISIONS_QUESTION_KEYS.end(), key) == DECISIONS_QUESTION_KEYS.end())
          return invalid("questions." + name + " contains an unsupported field", "questions");
      const auto type = rt::getString(value, "type");
      if (!type || std::find(DECISIONS_QUESTION_TYPES.begin(), DECISIONS_QUESTION_TYPES.end(), *type) == DECISIONS_QUESTION_TYPES.end())
        return invalid("questions." + name + ".type must be noul, choice, score, or box", "questions");
      if (auto failure = instructionsError(value.as_object(), name)) return failure;
      const auto* criteria = rt::get(value, "criteria");
      if (*type == "noul") return noulCriteriaError(criteria, name);
      if (*type == "choice") return choiceCriteriaError(criteria, name);
      if (*type == "score") return scoreCriteriaError(criteria, name);
      return !criteria ? std::nullopt : std::optional<rt::Response>(invalid("questions." + name + ".criteria is not valid for box questions", "questions"));
    }

    std::optional<rt::Response> questionsError(const json::value* questions) {
      if (!isPlainRecord(questions)) return invalid("questions must be an object", "questions");
      const auto entries = rt::objectEntries(questions->as_object());
      if (entries.empty() || static_cast<double>(entries.size()) > DECISIONS_MAX_QUESTIONS) return invalid("questions must hold between 1 and 24 questions", "questions");
      for (const auto* entry : entries) {
        const std::string name(entry->key());
        if (rt::trim(name).empty() || static_cast<double>(rt::length(name)) > DECISIONS_MAX_QUESTION_NAME_LENGTH)
          return invalid("question names must be non-empty and at most 120 characters", "questions");
        if (auto failure = questionError(name, entry->value())) return failure;
      }
      return std::nullopt;
    }

    std::optional<rt::Response> metadataError(const json::value& raw) {
      const auto* provider = rt::get(raw, "provider");
      if (provider && !provider->is_null() && !isPlainRecord(provider)) return invalid("provider must be an object or null", "provider");
      const auto* trace = rt::get(raw, "trace");
      if (trace && !isPlainRecord(trace)) return invalid("trace must be an object", "trace");
      constexpr std::array<std::string_view, 2> keys{"session_id", "user"};
      for (const auto key : keys) {
        const auto* value = rt::get(raw, key);
        if (!value) continue;
        if (!value->is_string() || value->as_string().empty() || static_cast<double>(rt::length(std::string(value->as_string()))) > DECISIONS_MAX_METADATA_LENGTH)
          return invalid(std::string(key) + " must be a non-empty string of at most 256 characters", std::string(key));
      }
      return std::nullopt;
    }

    /** Validates the documented Decisions envelope; a Response is the client-facing refusal. */
    std::variant<DecisionsRequest, rt::Response> parseDecisionsRequest(const json::value& raw) {
      if (auto failure = unsupportedKeyError(raw)) return *failure;
      if (auto failure = modelError(raw)) return *failure;
      if (auto failure = stateError(rt::get(raw, "state"))) return *failure;
      if (auto failure = questionsError(rt::get(raw, "questions"))) return *failure;
      if (auto failure = metadataError(raw)) return *failure;
      DecisionsRequest parsed{rt::trim(*rt::getString(raw, "model")), *rt::get(raw, "state"), rt::get(raw, "questions")->as_object()};
      if (const auto* value = rt::get(raw, "provider")) parsed.provider = *value;
      if (const auto value = rt::getString(raw, "session_id")) parsed.session_id = std::string(*value);
      if (const auto* value = rt::getObject(raw, "trace")) parsed.trace = *value;
      if (const auto value = rt::getString(raw, "user")) parsed.user = std::string(*value);
      return parsed;
    }

    /** Normalize Decisions token usage onto the gateway's canonical shape. */
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

    json::object forwardBody(const DecisionsRequest& request, const std::string& model) {
      json::object body{{"model", model}, {"state", request.state}, {"questions", request.questions}};
      if (request.provider) rt::objectDefine(body, "provider", *request.provider);
      if (request.session_id) rt::objectDefine(body, "session_id", json::value(*request.session_id));
      if (request.trace) rt::objectDefine(body, "trace", *request.trace);
      if (request.user) rt::objectDefine(body, "user", json::value(*request.user));
      return body;
    }

    void recordDecisionsResponseHealth(double status) {
      const std::string event = status == 401 || status == 403 ? "auth_invalid" : status == 402 || status == 429 ? "quota_exhausted" :
                                status >= 500 ? "upstream_error" : status >= 400 ? "reachable" : "success";
      (void)provider::health::recordOpenRouterProviderHealth(event, status, rt::dateNow);
    }

    constexpr std::array<std::pair<std::string_view, std::string_view>, 4> DECISIONS_ERROR_MESSAGES{{
      {"openrouter_api_key_missing", "Decisions upstream is not configured"},
      {"openrouter_upstream_unreachable", "Decisions upstream unreachable"},
      {"openrouter_upstream_error", "Decisions upstream error"},
      {"openrouter_upstream_invalid_response", "Decisions upstream invalid response"}}};

    rt::Response decisionsDispatchFailure(const rt::Reason& failure) {
      try { std::rethrow_exception(failure); }
      catch (const api_key_policy::ApiKeyQuotaDispatchError& error) { return upstream_wire::apiKeyQuotaDispatchErrorResponse(error); }
      catch (...) {}
      if (rt::isError(failure) && (rt::errorName(failure) == "AbortError" || rt::errorName(failure) == "TimeoutError"))
        return http::openaiError(499, "Request was cancelled.", "request_cancelled", {.type = "server_error", .param = std::optional<std::string>{}});
      try { std::rethrow_exception(failure); }
      catch (const router::OpenRouterError& error) {
        if (error.code == "openrouter_upstream_error" && error.upstreamStatus) recordDecisionsResponseHealth(*error.upstreamStatus);
        else if (!error.beforeTransport && error.code != "openrouter_api_key_missing")
          (void)provider::health::recordOpenRouterProviderHealth("upstream_error", std::nullopt, rt::dateNow);
        for (const auto& [code, message] : DECISIONS_ERROR_MESSAGES)
          if (code == error.code) return http::openaiError(error.status, std::string(message), error.code);
        return http::openaiError(error.status, "", error.code);
      }
    }

    rt::Task<rt::Response> handleDecisionsImpl(rt::Request req, std::optional<telemetry::UsageContext> usageContext, DecisionsHandlerDeps deps) {
      rt::Promise<request::JsonBodyReadResult> reading(request::readJsonBodyWithLimit(req, rt::limits::kHttpBodyBytes));
      const auto read = co_await reading.wait();
      if (const auto* failure = std::get_if<request::JsonBodyReadResultNotOk>(&read))
        co_return failure->kind == "too_large" ? http::openaiError(413, "The Decisions request body is too large", "decisions_request_too_large") :
          http::openaiError(400, "Invalid JSON body", "invalid_request_error");
      const auto& raw = std::get<request::JsonBodyReadResultOk>(read).value;
      if (!utils::isRecord(raw)) co_return http::openaiError(400, "Invalid JSON body", "invalid_request_error");
      const auto parsed = parseDecisionsRequest(raw);
      if (const auto* failure = std::get_if<rt::Response>(&parsed)) co_return *failure;
      const auto request = std::get<DecisionsRequest>(parsed);
      rt::Promise<std::optional<provider::selection::ProviderSelection>> loading(provider::selection::loadProviderSelectionCached());
      const auto selected = co_await loading.wait();
      if (!provider::selection::isProviderEnabled("openrouter", selected))
        co_return http::openaiError(503, "Decisions provider is disabled by the provider selection", "decisions_provider_disabled");
      rt::Promise<std::optional<std::string>> resolving(router::resolveOpenRouterDecisionUpstreamModel(request.model));
      const auto upstreamModel = co_await resolving.wait();
      if (!upstreamModel) co_return invalid("The requested model is not served by the OpenRouter Decisions API", "model");
      const auto state = usageContext && usageContext->responseTelemetry ? usageContext->responseTelemetry : telemetry::createResponseTelemetryState();
      state->provider = "openrouter";
      const auto context = std::make_shared<telemetry::UsageContext>(usageContext.value_or(telemetry::UsageContext{.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt}));
      context->responseTelemetry = state;
      const telemetry::RecordRequestUsageDetails details{request.model, "decisions", false, std::nullopt};
      auto recording = telemetry::recordRequestUsage(context.get(), details);
      co_await recording.wait();
      json::object payload;
      rt::Reason failure;
      try {
        router::OpenRouterDecisionInput input;
        input.body = forwardBody(request, *upstreamModel);
        if (deps.apiKey) input.apiKey = rt::orNull(deps.apiKey());
        if (deps.fetcher) input.fetcher = deps.fetcher;
        input.signal = openai::inferenceSignal(req, usageContext ? &*usageContext : nullptr);
        router::OpenRouterDispatchHooks hooks;
        hooks.beforeDispatch = [context] {
          if (context->beforeProviderDispatch) return context->beforeProviderDispatch("openrouter");
          return rt::fulfilledTask(std::optional<api_key_policy::ApiKeyProviderDispatch>{});
        };
        hooks.onDispatch = [context] { telemetry::recordAttemptedProvider(context.get(), "openrouter"); telemetry::recordFirstProviderDispatch(context.get()); };
        hooks.onHeaders = [context] { telemetry::recordFirstProviderHeaders(context.get()); };
        input.hooks = hooks;
        rt::Promise<json::object> fetching(router::fetchOpenRouterDecisions(input));
        payload = co_await fetching.wait();
      } catch (...) { failure = std::current_exception(); }
      if (failure) co_return decisionsDispatchFailure(failure);
      if (!isPlainRecord(rt::get(payload, "answers"))) {
        (void)provider::health::recordOpenRouterProviderHealth("upstream_error", std::nullopt, rt::dateNow);
        co_return http::openaiError(502, "Decisions upstream invalid response", "openrouter_upstream_invalid_response");
      }
      recordDecisionsResponseHealth(200);
      const auto normalized = normalizedUsage(rt::get(payload, "usage"));
      const json::value usageValue = normalized ? json::value(*normalized) : json::value(nullptr);
      const auto usage = telemetry::extractUsageTokens(usageValue);
      auto completion = telemetry::recordCompletionUsage(context.get(), usage);
      co_await completion.wait();
      rt::ResponseInit init;
      init.headers = rt::Headers{{"x-uos-upstream", "openrouter"}};
      co_return telemetry::attachResponseTelemetry(rt::Response::json(payload, init), state);
    }
  }

  rt::Task<rt::Response> handleDecisions(rt::Request req, const telemetry::UsageContext* usageContext) {
    return handleDecisionsImpl(std::move(req), usageContext ? std::optional(*usageContext) : std::nullopt, DecisionsHandlerDeps{});
  }
  rt::Task<rt::Response> handleDecisions(rt::Request req, const telemetry::UsageContext* usageContext, const DecisionsHandlerDeps& deps) {
    return handleDecisionsImpl(std::move(req), usageContext ? std::optional(*usageContext) : std::nullopt, deps);
  }
}
