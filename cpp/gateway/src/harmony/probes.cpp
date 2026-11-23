#include "uos/harmony/probes.hpp"

#include "uos/provider/cerebras.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

#include <memory>

namespace uos::harmony::probes {
  namespace {
    template <class T> std::optional<std::string> stringOf(const T& value, std::string_view key) {
      const auto text = rt::getString(value, key);
      return text ? std::optional<std::string>(std::string(*text)) : std::nullopt;
    }
    std::optional<ProbeStateSnapshot> stateSnapshot(const std::optional<conversation::Conversation>& conversation) {
      if (!conversation) return std::nullopt;
      const auto dropped = conversation::dropAnalysisBeforeCompletedFinal(*conversation);
      return ProbeStateSnapshot{.analysisLines = static_cast<double>(conversation::analysisLineCount(*conversation)),
        .analysisAfterDrop = static_cast<double>(conversation::analysisLineCount(dropped)),
        .pendingToolCalls = static_cast<double>(conversation::pendingToolCallCount(*conversation)), .completedFinal = conversation::hasCompletedFinal(*conversation)};
    }
    std::vector<ProbeRequestSummary::Tool> summarizeRawTools(const json::value* value) {
      std::vector<ProbeRequestSummary::Tool> tools;
      if (!value || !value->is_array()) return tools;
      for (const auto& tool : value->as_array()) {
        if (!tool.is_object() && !tool.is_array()) continue;
        const auto* function = rt::get(tool, "function");
        if (!function || (!function->is_object() && !function->is_array())) continue;
        tools.push_back({.name = stringOf(*function, "name").value_or("?"), .strict = rt::getBool(*function, "strict")});
      }
      return tools;
    }
    std::string rawMessageRole(const json::value& message) {
      if (!message.is_object() && !message.is_array()) return "?";
      const auto* role = rt::get(message, "role");
      if (rt::isNullish(role)) return "?";
      if (role->is_string() || rt::isNumber(*role) || role->is_bool()) return rt::toString(*role);
      return rt::jsonStringify(*role);
    }
    std::vector<std::string> projectedMessageRoles(const json::object& body) {
      const auto projected = provider::cerebras::projectCerebrasRequest(body);
      std::vector<std::string> roles;
      if (const auto* messages = rt::getArray(projected, "messages")) for (const auto& message : *messages) roles.push_back(rawMessageRole(message));
      return roles;
    }
    std::string responseFormatOf(const json::object& body) {
      const auto* format = rt::get(body, "response_format");
      const auto type = format ? stringOf(*format, "type") : std::nullopt;
      return type && (*type == "json_object" || *type == "json_schema") ? *type : "none";
    }
    ProbeRequestSummary summarizeRawBody(const json::object& body) {
      const auto projected = provider::cerebras::projectCerebrasRequest(body);
      const auto tools = summarizeRawTools(rt::get(projected, "tools"));
      const auto* messages = rt::getArray(projected, "messages");
      std::vector<std::string> roles;
      double assistantToolTurns = 0;
      double toolResultTurns = 0;
      if (messages) for (const auto& message : *messages) {
        roles.push_back(rawMessageRole(message));
        if (message.is_object() || message.is_array()) if (rt::getArray(message, "tool_calls")) assistantToolTurns += 1;
        if (roles.back() == "tool") toolResultTurns += 1;
      }
      std::vector<bool> strictness;
      for (const auto& tool : tools) strictness.push_back(tool.strict.value_or(false));
      const bool analysisInWire = rt::includes(roles, std::string("assistant"));
      return {.style = "generic", .model = stringOf(body, "model").value_or("?"), .roles = std::move(roles),
        .tools = tools.empty() ? std::nullopt : std::optional(tools), .toolStrictnessValues = std::move(strictness),
        .reasoningEffortTopLevel = stringOf(body, "reasoning_effort"), .reasoningInSystem = false, .responseFormat = responseFormatOf(body),
        .parallelToolCalls = rt::getBool(body, "parallel_tool_calls"), .maxCompletionTokens = rt::getNumber(body, "max_completion_tokens"),
        .analysisInWire = analysisInWire, .assistantToolTurns = assistantToolTurns, .toolResultTurns = toolResultTurns};
    }
    ProbeResponseSummary responseSummary(double status, const types::NormalizedAssistantResponse& normalized) {
      std::vector<ProbeResponseSummary::ToolCall> calls;
      for (const auto& call : normalized.toolCalls) calls.push_back({.id = call.id, .name = call.name,
        .argumentsChars = static_cast<double>(rt::length(call.arguments)), .argumentsJsonValid = argsJsonValid(call.arguments)});
      return {.status = status, .model = normalized.model, .contentPresent = normalized.content.has_value(),
        .contentChars = normalized.content ? static_cast<double>(rt::length(*normalized.content)) : 0,
        .contentPreview = normalized.content && !normalized.content->empty() ? std::optional(rt::slice(*normalized.content, 0, 120)) : std::nullopt,
        .reasoningPresent = !normalized.analysis.empty(), .reasoningChars = normalized.shape.reasoningChars, .toolCalls = std::move(calls),
        .refusal = normalized.refusal.has_value(), .finishReason = normalized.finishReason};
    }
    ProbeOutcome outcomeForStatus(double status) { return status >= 400 && status < 500 ? "upstream_rejected" : "upstream_error"; }
    adapter::UpstreamError upstreamErrorOf(const json::value& value) {
      return {.code = stringOf(value, "code"), .message = stringOf(value, "message")};
    }
    struct ContextState { adapter::HarmonyTransport transport; std::function<rt::Date()> now; };
    ProbeTurnRecord record(ProbeOutcome outcome, std::optional<double> status, double durationMs, std::optional<ProbeRequestSummary> request,
      std::optional<ProbeResponseSummary> response, std::optional<adapter::UpstreamError> upstreamError,
      std::optional<ProbeTurnRecord::AdapterError> adapterError, const std::optional<conversation::Conversation>& conversation,
      std::vector<std::string> notes = {}, std::optional<classifier::BootstrapClassifierVerdict> verdict = {}) {
      return {.outcome = std::move(outcome), .status = status, .durationMs = durationMs, .request = std::move(request), .response = std::move(response),
        .upstreamError = std::move(upstreamError), .adapterError = std::move(adapterError), .state = stateSnapshot(conversation), .notes = std::move(notes), .verdict = std::move(verdict)};
    }
    adapter::HarmonyRequestOptions turnOptions(const conversation::Conversation& conversation, const ProbeTurnOptions& options) {
      return {.style = options.style, .turns = conversation.turns, .reasoningEffort = options.reasoningEffort, .tools = options.tools,
        .toolStrictnessMode = options.toolStrictnessMode, .responseFormat = options.responseFormat, .nativeResponseFormat = options.nativeResponseFormat,
        .nativeToolResultStyle = options.nativeToolResultStyle, .combinationPolicy = options.combinationPolicy, .maxCompletionTokens = options.maxCompletionTokens,
        .parallelToolCalls = options.parallelToolCalls, .currentDate = options.currentDate, .instructions = options.instructions, .namespace_ = options.namespace_};
    }
    ProbeRequestSummary requestSummary(const adapter::BuiltHarmonyRequest& built) {
      const auto& metadata = built.metadata;
      std::vector<ProbeRequestSummary::Tool> tools;
      for (const auto& tool : metadata.toolEntries) tools.push_back({.name = tool.name, .strict = tool.strict});
      return {.style = built.style, .model = metadata.model, .roles = metadata.messageRoles, .tools = tools.empty() ? std::nullopt : std::optional(tools),
        .toolStrictnessValues = metadata.toolStrictnessValues, .reasoningEffortTopLevel = metadata.reasoningEffortTopLevel,
        .reasoningInSystem = metadata.reasoningEffortInSystem, .responseFormat = metadata.responseFormat, .parallelToolCalls = metadata.parallelToolCalls,
        .maxCompletionTokens = metadata.maxCompletionTokens, .analysisInWire = metadata.analysisInWire, .assistantToolTurns = metadata.assistantToolTurns,
        .toolResultTurns = metadata.toolResultTurns};
    }
    rt::Task<ProbeTurnRun> runTurn(std::shared_ptr<ContextState> context, conversation::Conversation conversation, ProbeTurnOptions options) {
      const double started = context->now().getTime();
      try {
        const auto requestOptions = turnOptions(conversation, options);
        const auto built = adapter::buildCerebrasHarmonyRequest(requestOptions);
        const auto request = requestSummary(built);
        rt::Promise<adapter::RunTurnResult> pending(adapter::runHarmonyTurn(requestOptions, context->transport));
        const auto result = co_await pending.wait();
        const double durationMs = context->now().getTime() - started;
        if (const auto* success = std::get_if<adapter::RunTurnResultOk>(&result)) co_return ProbeTurnRun{
          .record = record("ok", success->status, durationMs, request, responseSummary(success->status, success->normalized), {}, {}, conversation),
          .normalized = success->normalized, .verdict = std::nullopt};
        const auto& failure = std::get<adapter::RunTurnResultNotOk>(result);
        if (failure.upstreamError) co_return ProbeTurnRun{.record = record(outcomeForStatus(failure.status), failure.status, durationMs, request, {}, failure.upstreamError, {}, conversation),
          .normalized = std::nullopt, .verdict = std::nullopt};
        co_return ProbeTurnRun{.record = record("failed", failure.status, durationMs, request, {}, {},
          ProbeTurnRecord::AdapterError{.code = "normalization_error", .message = failure.normalizationError.value_or("unknown")}, conversation), .normalized = std::nullopt, .verdict = std::nullopt};
      } catch (const rt::JsError& error) {
        const double durationMs = context->now().getTime() - started;
        if (error.code) co_return ProbeTurnRun{.record = record("adapter_error", {}, durationMs, {}, {}, {},
          ProbeTurnRecord::AdapterError{.code = *error.code, .message = error.message}, conversation), .normalized = std::nullopt, .verdict = std::nullopt};
        co_return ProbeTurnRun{.record = record("failed", {}, durationMs, {}, {}, {},
          ProbeTurnRecord::AdapterError{.code = "probe_error", .message = error.toString()}, conversation), .normalized = std::nullopt, .verdict = std::nullopt};
      } catch (...) {
        const double durationMs = context->now().getTime() - started;
        co_return ProbeTurnRun{.record = record("failed", {}, durationMs, {}, {}, {},
          ProbeTurnRecord::AdapterError{.code = "probe_error", .message = rt::reasonToString(std::current_exception())}, conversation), .normalized = std::nullopt, .verdict = std::nullopt};
      }
    }
    rt::Task<json::value> readJson(rt::Response response) {
      try {
        rt::Promise<json::value> pending(response.json());
        const auto ignoreParseError = [](rt::Reason) { return json::value(nullptr); };
        auto safe = pending.catch_(ignoreParseError);
        auto value = co_await safe.wait();
        co_return value;
      } catch (...) { co_return json::value(nullptr); }
    }
    rt::Task<ProbeTurnRun> runRaw(std::shared_ptr<ContextState> context, json::object body, std::optional<conversation::Conversation> conversation) {
      const double started = context->now().getTime();
      const auto request = summarizeRawBody(body);
      try {
        rt::Promise<rt::Response> pending(context->transport(body));
        const auto response = co_await pending.wait();
        const double status = response.status();
        auto parsed = readJson(response);
        const auto value = co_await std::move(parsed);
        const double durationMs = context->now().getTime() - started;
        if (!response.ok()) co_return ProbeTurnRun{.record = record(outcomeForStatus(status), status, durationMs, request, {}, upstreamErrorOf(value), {}, conversation), .normalized = {}, .verdict = {}};
        if (value.is_null()) co_return ProbeTurnRun{.record = record("failed", status, durationMs, request, {}, {},
          ProbeTurnRecord::AdapterError{.code = "non_json", .message = "upstream reply is not JSON"}, conversation), .normalized = {}, .verdict = {}};
        const auto normalized = adapter::normalizeHarmonyChatCompletion(value);
        if (const auto* error = std::get_if<adapter::HarmonyNormalizationError>(&normalized)) co_return ProbeTurnRun{
          .record = record("failed", status, durationMs, request, {}, {}, ProbeTurnRecord::AdapterError{.code = "normalization_error", .message = error->error}, conversation), .normalized = {}, .verdict = {}};
        const auto& success = std::get<types::NormalizedAssistantResponse>(normalized);
        co_return ProbeTurnRun{.record = record("ok", status, durationMs, request, responseSummary(status, success), {}, {}, conversation), .normalized = success, .verdict = {}};
      } catch (...) {
        co_return ProbeTurnRun{.record = record("failed", {}, context->now().getTime() - started, request, {}, {},
          ProbeTurnRecord::AdapterError{.code = "probe_error", .message = rt::reasonToString(std::current_exception())}, conversation), .normalized = {}, .verdict = {}};
      }
    }
    std::string verdictName(const classifier::BootstrapClassifierVerdict& verdict) { return std::visit([](const auto& item) { return std::string(item.verdict); }, verdict); }
    ProbeTurnRun classifierFailure(ProbeOutcome outcome, std::optional<double> status, double durationMs, const ProbeRequestSummary& request,
      std::optional<adapter::UpstreamError> upstreamError, std::optional<ProbeTurnRecord::AdapterError> adapterError, std::string reason) {
      const classifier::BootstrapClassifierVerdict unknown = classifier::BootstrapClassifierVerdictUnknown{.raw = std::nullopt, .reason = std::move(reason)};
      return {.record = record(std::move(outcome), status, durationMs, request, {}, std::move(upstreamError), std::move(adapterError), {}, {"verdict=unknown"}, unknown), .normalized = {}, .verdict = unknown};
    }
    rt::Task<ProbeTurnRun> runClassifier(std::shared_ptr<ContextState> context, classifier::BootstrapClassifierRequestOptions options) {
      const double started = context->now().getTime();
      const auto body = classifier::buildBootstrapClassifierRequest(options);
      const ProbeRequestSummary request{.style = "classifier", .model = stringOf(body, "model").value_or("?"), .roles = projectedMessageRoles(body), .tools = {}, .toolStrictnessValues = {},
        .reasoningEffortTopLevel = {}, .reasoningInSystem = true, .responseFormat = "none", .parallelToolCalls = {}, .maxCompletionTokens = rt::getNumber(body, "max_completion_tokens"),
        .analysisInWire = false, .assistantToolTurns = 0, .toolResultTurns = 0};
      try {
        rt::Promise<rt::Response> pending(context->transport(body));
        const auto response = co_await pending.wait();
        const double status = response.status();
        auto parsed = readJson(response);
        const auto value = co_await std::move(parsed);
        const double durationMs = context->now().getTime() - started;
        if (!response.ok()) co_return classifierFailure(outcomeForStatus(status), status, durationMs, request, upstreamErrorOf(value), {}, "classifier request rejected with status " + rt::numberToString(status));
        if (value.is_null()) co_return classifierFailure("failed", status, durationMs, request, {}, ProbeTurnRecord::AdapterError{"non_json", "upstream reply is not JSON"}, "classifier reply is not JSON");
        const auto normalized = adapter::normalizeHarmonyChatCompletion(value);
        if (const auto* error = std::get_if<adapter::HarmonyNormalizationError>(&normalized)) co_return classifierFailure("failed", status, durationMs, request, {},
          ProbeTurnRecord::AdapterError{"normalization_error", error->error}, "classifier reply is invalid: " + error->error);
        const auto& success = std::get<types::NormalizedAssistantResponse>(normalized);
        const auto verdict = classifier::verdictFromBootstrapResponse(success);
        co_return ProbeTurnRun{.record = record("ok", status, durationMs, request, responseSummary(status, success), {}, {}, {}, {"verdict=" + verdictName(verdict)}, verdict), .normalized = success, .verdict = verdict};
      } catch (...) {
        const auto error = rt::reasonToString(std::current_exception());
        co_return classifierFailure("failed", {}, context->now().getTime() - started, request, {}, ProbeTurnRecord::AdapterError{"probe_error", error}, "classifier transport failed: " + error);
      }
    }
    template <class T> json::value orNull(const std::optional<T>& value) { return value ? json::value_from(*value) : json::value(nullptr); }
    json::value verdictJson(const std::optional<classifier::BootstrapClassifierVerdict>& verdict) {
      if (!verdict) return nullptr;
      return std::visit([](const auto& item) -> json::value {
        json::object out{{"verdict", item.verdict}};
        if constexpr (std::is_same_v<std::decay_t<decltype(item)>, classifier::BootstrapClassifierVerdictUnknown>) {
          out["raw"] = orNull(item.raw); out["reason"] = item.reason;
        } else out["raw"] = item.raw;
        return out;
      }, *verdict);
    }
    json::value requestJson(const std::optional<ProbeRequestSummary>& request) {
      if (!request) return nullptr;
      const auto& r = *request;
      json::value tools = nullptr;
      if (r.tools) {
        json::array list;
        for (const auto& tool : *r.tools) list.push_back(json::object{{"name", tool.name}, {"strict", orNull(tool.strict)}});
        tools = std::move(list);
      }
      return json::object{{"style", r.style}, {"model", r.model}, {"roles", json::value_from(r.roles)}, {"tools", std::move(tools)},
        {"toolStrictnessValues", json::value_from(r.toolStrictnessValues)}, {"reasoningEffortTopLevel", orNull(r.reasoningEffortTopLevel)}, {"reasoningInSystem", r.reasoningInSystem},
        {"responseFormat", r.responseFormat}, {"parallelToolCalls", orNull(r.parallelToolCalls)}, {"maxCompletionTokens", orNull(r.maxCompletionTokens)}, {"analysisInWire", r.analysisInWire},
        {"assistantToolTurns", rt::makeNumber(r.assistantToolTurns)}, {"toolResultTurns", rt::makeNumber(r.toolResultTurns)}};
    }
    json::value responseJson(const std::optional<ProbeResponseSummary>& response) {
      if (!response) return nullptr;
      const auto& r = *response;
      json::array calls;
      for (const auto& call : r.toolCalls) calls.push_back(json::object{{"id", call.id}, {"name", call.name}, {"argumentsChars", rt::makeNumber(call.argumentsChars)}, {"argumentsJsonValid", call.argumentsJsonValid}});
      return json::object{{"status", rt::makeNumber(r.status)}, {"model", r.model}, {"contentPresent", r.contentPresent}, {"contentChars", rt::makeNumber(r.contentChars)}, {"contentPreview", orNull(r.contentPreview)},
        {"reasoningPresent", r.reasoningPresent}, {"reasoningChars", rt::makeNumber(r.reasoningChars)}, {"toolCalls", std::move(calls)}, {"refusal", r.refusal}, {"finishReason", orNull(r.finishReason)}};
    }
  }
  conversation::Conversation user(std::string content) { return conversation::createConversation({types::ConversationTurnUser{.content = std::move(content)}}); }
  bool argsJsonValid(const std::string& argumentsText) { try { (void)rt::jsonParse(argumentsText); return true; } catch (...) { return false; } }
  ProbeContext createProbeContext(adapter::HarmonyTransport transport) { return createProbeContext(std::move(transport), [] { return rt::Date(rt::dateNow()); }); }
  ProbeContext createProbeContext(adapter::HarmonyTransport transport, std::function<rt::Date()> now) {
    const auto context = std::make_shared<ContextState>(ContextState{.transport = transport, .now = now});
    return {.transport = std::move(transport), .now = std::move(now),
      .runTurn = [context](conversation::Conversation conversation, ProbeTurnOptions options) { return runTurn(context, std::move(conversation), std::move(options)); },
      .runRaw = {.function = [context](json::object body, std::optional<conversation::Conversation> conversation) { return runRaw(context, std::move(body), std::move(conversation)); }},
      .runClassifier = [context](classifier::BootstrapClassifierRequestOptions options) { return runClassifier(context, std::move(options)); }};
  }
  json::object toJson(const ProbeTurnRecord& r) {
    json::value upstreamError = nullptr;
    if (r.upstreamError) upstreamError = json::object{{"code", orNull(r.upstreamError->code)}, {"message", orNull(r.upstreamError->message)}};
    json::value adapterError = nullptr;
    if (r.adapterError) adapterError = json::object{{"code", r.adapterError->code}, {"message", r.adapterError->message}};
    json::value state = nullptr;
    if (r.state) state = json::object{{"analysisLines", rt::makeNumber(r.state->analysisLines)}, {"analysisAfterDrop", rt::makeNumber(r.state->analysisAfterDrop)},
      {"pendingToolCalls", rt::makeNumber(r.state->pendingToolCalls)}, {"completedFinal", r.state->completedFinal}};
    return {{"outcome", r.outcome}, {"status", orNull(r.status)}, {"durationMs", rt::makeNumber(r.durationMs)}, {"request", requestJson(r.request)}, {"response", responseJson(r.response)},
      {"upstreamError", std::move(upstreamError)}, {"adapterError", std::move(adapterError)}, {"state", std::move(state)}, {"notes", json::value_from(r.notes)}, {"verdict", verdictJson(r.verdict)}};
  }
  json::object toJson(const ProbeScenarioResult& r) {
    json::array turns;
    for (const auto& turn : r.turns) turns.push_back(toJson(turn));
    return {{"id", r.id}, {"group", r.group}, {"style", r.style}, {"description", r.description}, {"expectedOutcome", orNull(r.expectedOutcome)}, {"outcome", r.outcome},
      {"startedAt", r.startedAt}, {"durationMs", rt::makeNumber(r.durationMs)}, {"turns", std::move(turns)}, {"verdict", verdictJson(r.verdict)}, {"failure", orNull(r.failure)}, {"notes", json::value_from(r.notes)}};
  }
}
