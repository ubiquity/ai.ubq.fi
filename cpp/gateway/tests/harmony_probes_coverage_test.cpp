// Port of tests/harmony-probes-coverage.test.ts. Fake transports only, no provider effects.
// Links: tests/harmony_probes_coverage_test.cpp src/harmony/adapter.cpp src/harmony/probes.cpp src/harmony/classifier.cpp
// src/harmony/conversation.cpp src/harmony/parse.cpp src/harmony/render.cpp src/harmony/types.cpp
// src/provider/cerebras.cpp src/sentinel/upstream_capture.cpp src/utils.cpp
#include "uos/harmony/probes.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace probes = uos::harmony::probes;
  namespace classifier = uos::harmony::classifier;
  namespace types = uos::harmony::types;
  namespace adapter = uos::harmony::adapter;
  #define AWAIT(name, expr) auto name##Task = (expr); auto name = co_await std::move(name##Task)
  struct Reply { std::string payload{}; double status = 200; std::string contentType = "application/json"; std::optional<std::string> reject{}; };
  struct Script { adapter::HarmonyTransport transport; std::shared_ptr<std::vector<json::object>> bodies; };
  Script scriptedTransport(std::vector<Reply> replies) {
    const auto queue = std::make_shared<std::vector<Reply>>(std::move(replies));
    const auto bodies = std::make_shared<std::vector<json::object>>();
    const auto index = std::make_shared<std::size_t>(0);
    adapter::HarmonyTransport transport = [queue, bodies, index](json::object body) -> rt::Task<rt::Response> {
      bodies->push_back(std::move(body));
      if (*index >= queue->size()) throw rt::JsError("scripted transport exhausted");
      const auto& reply = (*queue)[(*index)++];
      if (reply.reject) throw rt::JsError(*reply.reject);
      return rt::fulfilledTask(rt::Response(reply.payload, {.status = reply.status, .headers = rt::Headers{{"Content-Type", reply.contentType}}}));
    };
    return {.transport = transport, .bodies = bodies};
  }
  std::string completion(json::object message, std::string finishReason = "stop") {
    json::object assistant{{"role", "assistant"}}; rt::objectAssign(assistant, message);
    return rt::jsonStringify(json::object{{"id", "cmpl-coverage"}, {"object", "chat.completion"}, {"created", 1}, {"model", "gpt-oss-120b"},
      {"choices", json::array{json::object{{"index", 0}, {"message", std::move(assistant)}, {"finish_reason", finishReason}}}},
      {"usage", json::object{{"prompt_tokens", 3}, {"completion_tokens", 2}, {"total_tokens", 5}}}});
  }
  std::string errorBody(std::string code, std::string message) { return rt::jsonStringify(json::object{{"code", code}, {"message", message}}); }
  struct Context { probes::ProbeContext ctx; std::shared_ptr<std::vector<json::object>> bodies; };
  Context contextOf(std::vector<Reply> replies) {
    auto script = scriptedTransport(std::move(replies));
    return {.ctx = probes::createProbeContext(script.transport, [] { return rt::Date(0.0); }), .bodies = script.bodies};
  }
  const classifier::BootstrapClassifierRequestOptions classifierOptions{
    .observation = {.runId = "probe-run-42", .generation = 3, .phase = "verify", .milestone = "m01", .failureFingerprint = {}, .gitSha = {}, .ledgerVersion = {}, .retryState = {}, .verificationEvidence = {}},
    .decisionDefinition = "Answer true when the run has advanced.", .reasoningEffort = "low",
  };
  json::object basicBody() { return {{"model", "gpt-oss-120b"}}; }
  const probes::ProbeStateSnapshot EMPTY_STATE{.analysisLines = 0, .analysisAfterDrop = 0, .pendingToolCalls = 0, .completedFinal = false};
  void expectError(const probes::ProbeTurnRun& run, std::string code, std::string message) {
    EXPECT_EQ(run.record.adapterError, std::optional(probes::ProbeTurnRecord::AdapterError{.code = std::move(code), .message = std::move(message)}));
  }
  void expectUnknown(const probes::ProbeTurnRun& run, std::optional<std::string> raw, std::string reason) {
    EXPECT_EQ(run.verdict, std::optional<classifier::BootstrapClassifierVerdict>(classifier::BootstrapClassifierVerdictUnknown{.raw = std::move(raw), .reason = std::move(reason)}));
  }
}

TEST("argsJsonValid reports malformed tool arguments instead of throwing") {
  EXPECT(probes::argsJsonValid("{\"location\":\"San Francisco\"}")); EXPECT(probes::argsJsonValid("4"));
  EXPECT(!probes::argsJsonValid("")); EXPECT(!probes::argsJsonValid("{\"location\":")); EXPECT(!probes::argsJsonValid("undefined"));
}

ASYNC_TEST("a raw probe summarizes a malformed wire body without leaking unknown shapes") {
  const json::object body{{"model", 7}, {"messages", json::array{nullptr, "not-a-message", json::object{{"role", 42}}, json::object{{"role", nullptr}}, json::object{},
    json::object{{"role", json::object{{"kind", "assistant"}}}}, json::object{{"role", "user"}, {"content", "hello"}, {"tool_calls", json::array{}}},
    json::object{{"role", "assistant"}, {"content", nullptr}, {"tool_calls", json::array{json::object{{"id", "call_a"}}}}}, json::object{{"role", "tool"}, {"content", "result"}}}},
    {"tools", json::array{"not-a-tool", json::object{}, json::object{{"function", "not-a-function"}}, json::object{{"function", json::object{{"name", "get_weather"}, {"strict", true}}}}, json::object{{"function", json::object{{"strict", 1}}}}}},
    {"reasoning_effort", 9}, {"response_format", json::object{{"type", "json_object"}}}, {"parallel_tool_calls", true}, {"max_completion_tokens", 256}};
  const auto fixture = contextOf({{.payload = completion({{"content", "done"}})}});
  AWAIT(run, fixture.ctx.runRaw(body));
  EXPECT(rt::deepEqual(json::value_from(*fixture.bodies), json::array{body})); EXPECT_EQ(run.record.outcome, "ok"); EXPECT_EQ(run.record.status, std::optional<double>(200)); EXPECT_EQ(run.record.durationMs, 0);
  ASSERT(run.record.request.has_value()); const auto& request = *run.record.request;
  EXPECT_EQ(request.style, "generic"); EXPECT_EQ(request.model, "?");
  EXPECT_EQ(request.roles, (std::vector<std::string>{"?", "?", "42", "?", "?", "{\"kind\":\"assistant\"}", "user", "assistant", "tool"}));
  EXPECT_EQ(request.tools, (std::optional<std::vector<probes::ProbeRequestSummary::Tool>>{{{.name = "get_weather", .strict = true}, {.name = "?", .strict = std::nullopt}}}));
  EXPECT_EQ(request.toolStrictnessValues, (std::vector<bool>{true, false})); EXPECT(!request.reasoningEffortTopLevel); EXPECT(!request.reasoningInSystem);
  EXPECT_EQ(request.responseFormat, "json_object"); EXPECT_EQ(request.parallelToolCalls, std::optional<bool>(true)); EXPECT_EQ(request.maxCompletionTokens, std::optional<double>(256));
  EXPECT(request.analysisInWire); EXPECT_EQ(request.assistantToolTurns, 2); EXPECT_EQ(request.toolResultTurns, 1); EXPECT(!run.record.adapterError); ASSERT(run.normalized); EXPECT_EQ(run.normalized->content, std::optional<std::string>("done"));
}

ASYNC_TEST("a raw probe with no usable tools or messages summarizes an empty request") {
  const json::object body{{"model", "gpt-oss-120b"}, {"messages", "nope"}, {"tools", json::array{}}, {"parallel_tool_calls", "yes"}};
  const auto fixture = contextOf({{.payload = completion({{"content", "ok"}})}}); AWAIT(run, fixture.ctx.runRaw(body));
  EXPECT_EQ(run.record.outcome, "ok"); ASSERT(run.record.request); const auto& r = *run.record.request;
  EXPECT(r.roles.empty()); EXPECT(!r.tools); EXPECT(r.toolStrictnessValues.empty()); EXPECT(!r.parallelToolCalls); EXPECT(!r.maxCompletionTokens);
  EXPECT_EQ(r.responseFormat, "none"); EXPECT(!r.analysisInWire); EXPECT_EQ(r.assistantToolTurns, 0); EXPECT_EQ(r.toolResultTurns, 0);
}

ASYNC_TEST("a raw probe reads every structured-output wrapper as probe vocabulary") {
  const auto fixture = contextOf({{.payload = completion({{"content", "a"}})}, {.payload = completion({{"content", "b"}})}, {.payload = completion({{"content", "c"}})}, {.payload = completion({{"content", "d"}})}});
  const std::vector<json::value> formats{json::object{{"type", "json_schema"}, {"json_schema", json::object{{"name", "answer"}, {"schema", json::object{{"type", "object"}}}}}}, "json_object", nullptr, json::object{{"type", "text"}}};
  std::vector<std::string> values;
  for (const auto& format : formats) { auto body = basicBody(); body["response_format"] = format; AWAIT(run, fixture.ctx.runRaw(body)); values.push_back(run.record.request->responseFormat); }
  EXPECT_EQ(values, (std::vector<std::string>{"json_schema", "none", "none", "none"}));
}

ASYNC_TEST("a raw probe records a non-JSON success as a failed turn") {
  const auto fixture = contextOf({{.payload = "<html>gateway</html>", .contentType = "text/html"}}); const auto body = basicBody(); AWAIT(run, fixture.ctx.runRaw(body));
  EXPECT_EQ(run.record.outcome, "failed"); EXPECT_EQ(run.record.status, std::optional<double>(200)); EXPECT(!run.record.response); EXPECT(!run.record.upstreamError);
  expectError(run, "non_json", "upstream reply is not JSON"); EXPECT(!run.normalized); EXPECT(!run.verdict);
}

ASYNC_TEST("a raw probe reports an unnormalizable completion with the adapter error") {
  const auto fixture = contextOf({{.payload = "{\"id\":\"cmpl-coverage\",\"created\":1,\"model\":\"gpt-oss-120b\",\"choices\":[]}"}});
  const auto body = basicBody(); AWAIT(run, fixture.ctx.runRaw(body)); EXPECT_EQ(run.record.outcome, "failed"); EXPECT_EQ(run.record.status, std::optional<double>(200));
  expectError(run, "normalization_error", "upstream reply has no choices"); EXPECT(!run.record.response); EXPECT(!run.normalized);
}

ASYNC_TEST("a raw probe summary counts content, reasoning, refusal and tool-call validity") {
  const auto fixture = contextOf({{.payload = completion({{"content", std::string(200, 'x')}, {"reasoning_content", "Because."}, {"refusal", "cannot help"},
    {"tool_calls", json::array{json::object{{"id", "call_a"}, {"type", "function"}, {"function", json::object{{"name", "get_weather"}, {"arguments", "{\"location\":\"San Francisco\"}"}}}},
      json::object{{"id", "call_b"}, {"type", "function"}, {"function", json::object{{"name", "get_weather"}, {"arguments", "{\"location\":"}}}}}}}, "tool_calls")}});
  const auto body = basicBody(); AWAIT(run, fixture.ctx.runRaw(body)); EXPECT_EQ(run.record.outcome, "ok"); ASSERT(run.record.response); const auto& r = *run.record.response;
  EXPECT(r.contentPresent); EXPECT_EQ(r.contentChars, 200); EXPECT_EQ(r.contentPreview, std::optional<std::string>(std::string(120, 'x'))); EXPECT(r.reasoningPresent); EXPECT_EQ(r.reasoningChars, 8);
  EXPECT(r.refusal); EXPECT_EQ(r.finishReason, std::optional<std::string>("tool_calls"));
  EXPECT_EQ(r.toolCalls, (std::vector<probes::ProbeResponseSummary::ToolCall>{{.id = "call_a", .name = "get_weather", .argumentsChars = 28, .argumentsJsonValid = true}, {.id = "call_b", .name = "get_weather", .argumentsChars = 12, .argumentsJsonValid = false}})); EXPECT(!run.record.state);
}

ASYNC_TEST("a raw probe reports an upstream rejection with its sanitized error") {
  const auto fixture = contextOf({{.payload = errorBody("invalid_request_error", "unsupported tools"), .status = 400}}); const auto body = basicBody(); AWAIT(run, fixture.ctx.runRaw(body));
  EXPECT_EQ(run.record.outcome, "upstream_rejected"); EXPECT_EQ(run.record.status, std::optional<double>(400));
  EXPECT_EQ(run.record.upstreamError, std::optional(adapter::UpstreamError{.code = "invalid_request_error", .message = "unsupported tools"})); EXPECT(!run.record.response); EXPECT(!run.normalized);
}

ASYNC_TEST("a raw probe reports an upstream server error and a non-JSON error body") {
  const auto fixture = contextOf({{.payload = errorBody("overloaded", "try later"), .status = 503}, {.payload = "gateway timeout", .status = 504, .contentType = "text/plain"}}); const auto body = basicBody();
  AWAIT(unavailable, fixture.ctx.runRaw(body)); EXPECT_EQ(unavailable.record.outcome, "upstream_error"); EXPECT_EQ(unavailable.record.status, std::optional<double>(503));
  EXPECT_EQ(unavailable.record.upstreamError, std::optional(adapter::UpstreamError{.code = "overloaded", .message = "try later"}));
  AWAIT(noBody, fixture.ctx.runRaw(body)); EXPECT_EQ(noBody.record.outcome, "upstream_error"); EXPECT_EQ(noBody.record.status, std::optional<double>(504));
  EXPECT_EQ(noBody.record.upstreamError, std::optional(adapter::UpstreamError{.code = {}, .message = {}}));
}

ASYNC_TEST("a raw probe records a transport failure with the request summary intact") {
  const auto fixture = contextOf({{.reject = "socket hang up"}}); auto body = basicBody(); body["messages"] = json::array{json::object{{"role", "user"}, {"content", "hi"}}}; AWAIT(run, fixture.ctx.runRaw(body));
  EXPECT_EQ(run.record.outcome, "failed"); EXPECT(!run.record.status); ASSERT(run.record.request); EXPECT_EQ(run.record.request->model, "gpt-oss-120b"); EXPECT(!run.record.response);
  expectError(run, "probe_error", "Error: socket hang up"); EXPECT(!run.normalized);
}

ASYNC_TEST("a normalized turn of a scripted conversation reports ok with its state snapshot") {
  const auto fixture = contextOf({{.payload = completion({{"content", nullptr}, {"reasoning_content", "Call the tool."}, {"tool_calls", json::array{
    json::object{{"id", "call_a"}, {"type", "function"}, {"function", json::object{{"name", "get_weather"}, {"arguments", "{\"location\":\"Tokyo\"}"}}}}}}}, "tool_calls")}});
  const auto state = probes::user("What is the weather?"); const probes::ProbeTurnOptions options{.style = "generic", .tools = std::vector<types::ToolDefinition>{probes::WEATHER_TOOL}};
  AWAIT(run, fixture.ctx.runTurn(state, options)); EXPECT_EQ(run.record.outcome, "ok"); EXPECT_EQ(run.record.status, std::optional<double>(200)); ASSERT(run.record.request);
  EXPECT_EQ(run.record.request->style, "generic"); EXPECT_EQ(run.record.request->tools, (std::optional<std::vector<probes::ProbeRequestSummary::Tool>>{{{.name = "get_weather", .strict = false}}}));
  EXPECT_EQ(run.record.state, std::optional(EMPTY_STATE)); ASSERT(run.normalized); EXPECT_EQ(run.normalized->toolCalls[0].name, "get_weather");
  ASSERT(run.record.response); EXPECT(run.record.response->toolCalls[0].argumentsJsonValid); EXPECT_EQ(fixture.bodies->size(), std::size_t{1}); EXPECT(fixture.bodies->at(0).at("model") == "gpt-oss-120b");
}

ASYNC_TEST("a turn whose upstream reply is not JSON fails with a normalization error") {
  const auto fixture = contextOf({{.payload = "not json", .contentType = "text/plain"}}); const auto state = probes::user("hello"); const probes::ProbeTurnOptions options{.style = "generic"};
  AWAIT(run, fixture.ctx.runTurn(state, options)); EXPECT_EQ(run.record.outcome, "failed"); EXPECT_EQ(run.record.status, std::optional<double>(200)); ASSERT(run.record.request); EXPECT_EQ(run.record.request->model, "gpt-oss-120b");
  expectError(run, "normalization_error", "upstream reply is not JSON"); EXPECT(!run.normalized); EXPECT(!run.record.response); EXPECT_EQ(run.record.state, std::optional(EMPTY_STATE));
}

ASYNC_TEST("a turn records every upstream rejection and an unexplained failure") {
  const auto fixture = contextOf({{.payload = errorBody("invalid_request_error", "unsupported"), .status = 400}, {.payload = errorBody("overloaded", "later"), .status = 503}, {.payload = "gateway said no", .status = 502, .contentType = "text/html"}});
  const auto state = probes::user("hello"); const probes::ProbeTurnOptions options{.style = "generic"};
  AWAIT(rejected, fixture.ctx.runTurn(state, options)); EXPECT_EQ(rejected.record.outcome, "upstream_rejected"); EXPECT_EQ(rejected.record.status, std::optional<double>(400));
  EXPECT_EQ(rejected.record.upstreamError, std::optional(adapter::UpstreamError{.code = "invalid_request_error", .message = "unsupported"})); EXPECT(!rejected.record.adapterError); EXPECT(!rejected.record.response);
  AWAIT(unavailable, fixture.ctx.runTurn(state, options)); EXPECT_EQ(unavailable.record.outcome, "upstream_error"); EXPECT_EQ(unavailable.record.status, std::optional<double>(503)); EXPECT_EQ(unavailable.record.upstreamError, std::optional(adapter::UpstreamError{.code = "overloaded", .message = "later"}));
  AWAIT(unexplained, fixture.ctx.runTurn(state, options)); EXPECT_EQ(unexplained.record.outcome, "failed"); EXPECT_EQ(unexplained.record.status, std::optional<double>(502)); EXPECT(!unexplained.record.upstreamError);
  expectError(unexplained, "normalization_error", "unknown"); EXPECT(!unexplained.normalized);
}

ASYNC_TEST("a raw probe reads a top-level reasoning effort and defaults its clock") {
  const auto script = scriptedTransport({{.payload = completion({{"content", "ok"}})}}); const auto ctx = probes::createProbeContext(script.transport);
  EXPECT(std::abs(ctx.now().getTime() - rt::dateNow()) < 60000); auto body = basicBody(); body["reasoning_effort"] = "high";
  AWAIT(run, ctx.runRaw(body)); EXPECT_EQ(run.record.request->reasoningEffortTopLevel, std::optional<std::string>("high")); EXPECT_EQ(run.record.outcome, "ok");
}

ASYNC_TEST("a request the adapter refuses is recorded as an adapter error turn") {
  const auto fixture = contextOf({{.payload = completion({{"content", "unused"}})}}); const auto state = probes::user("hello");
  const probes::ProbeTurnOptions options{.style = "generic", .tools = std::vector<types::ToolDefinition>{probes::WEATHER_TOOL}, .responseFormat = types::ResponseFormatParamJsonObject{}};
  AWAIT(run, fixture.ctx.runTurn(state, options)); EXPECT_EQ(run.record.outcome, "adapter_error"); EXPECT(!run.record.status); EXPECT(!run.record.request);
  expectError(run, "unproven-combination", "Combining tools with a structured response format is not proven for gpt-oss-120b; use combinationPolicy 'probe' only for protocol evidence.");
  EXPECT(!run.normalized); EXPECT(fixture.bodies->empty()); EXPECT_EQ(run.record.state, std::optional(EMPTY_STATE));
}

ASYNC_TEST("a turn whose transport rejects fails without an adapter code") {
  const auto fixture = contextOf({{.reject = "connection reset"}}); const auto state = probes::user("hello"); const probes::ProbeTurnOptions options{.style = "generic"};
  AWAIT(run, fixture.ctx.runTurn(state, options)); EXPECT_EQ(run.record.outcome, "failed"); EXPECT(!run.record.status); EXPECT(!run.record.request);
  expectError(run, "probe_error", "Error: connection reset"); EXPECT(!run.normalized); EXPECT(!run.verdict);
}

ASYNC_TEST("a classifier rejection yields an unknown verdict and no normalized response") {
  const auto fixture = contextOf({{.payload = errorBody("invalid_request_error", "unknown model"), .status = 400}}); AWAIT(run, fixture.ctx.runClassifier(classifierOptions));
  EXPECT_EQ(run.record.outcome, "upstream_rejected"); EXPECT_EQ(run.record.status, std::optional<double>(400)); EXPECT_EQ(run.record.upstreamError, std::optional(adapter::UpstreamError{.code = "invalid_request_error", .message = "unknown model"}));
  ASSERT(run.record.request); EXPECT_EQ(run.record.request->style, "classifier"); EXPECT_EQ(run.record.request->model, "gpt-oss-120b"); EXPECT_EQ(run.record.request->maxCompletionTokens, std::optional<double>(128));
  EXPECT_EQ(run.record.notes, (std::vector<std::string>{"verdict=unknown"})); expectUnknown(run, {}, "classifier request rejected with status 400"); EXPECT_EQ(run.record.verdict, run.verdict);
  EXPECT(!run.normalized); EXPECT(!run.record.state); EXPECT_EQ(fixture.bodies->size(), std::size_t{1});
}

ASYNC_TEST("a classifier upstream failure is an upstream error outcome") {
  const auto fixture = contextOf({{.payload = errorBody("internal_error", "boom"), .status = 500}}); AWAIT(run, fixture.ctx.runClassifier(classifierOptions));
  EXPECT_EQ(run.record.outcome, "upstream_error"); EXPECT_EQ(run.record.status, std::optional<double>(500)); ASSERT(run.verdict); EXPECT(std::holds_alternative<classifier::BootstrapClassifierVerdictUnknown>(*run.verdict));
}

ASYNC_TEST("a classifier reply that is not JSON fails closed with an unknown verdict") {
  const auto fixture = contextOf({{.payload = "<html>", .contentType = "text/html"}}); AWAIT(run, fixture.ctx.runClassifier(classifierOptions));
  EXPECT_EQ(run.record.outcome, "failed"); EXPECT_EQ(run.record.status, std::optional<double>(200)); expectError(run, "non_json", "upstream reply is not JSON"); expectUnknown(run, {}, "classifier reply is not JSON");
  EXPECT_EQ(run.record.notes, (std::vector<std::string>{"verdict=unknown"})); EXPECT(!run.normalized); EXPECT(!run.record.state);
}

ASYNC_TEST("an invalid classifier completion fails closed with the normalization reason") {
  const auto fixture = contextOf({{.payload = "{\"id\":\"cmpl\",\"created\":1,\"model\":\"gpt-oss-120b\",\"choices\":[]}"}}); AWAIT(run, fixture.ctx.runClassifier(classifierOptions));
  EXPECT_EQ(run.record.outcome, "failed"); expectError(run, "normalization_error", "upstream reply has no choices"); expectUnknown(run, {}, "classifier reply is invalid: upstream reply has no choices");
  EXPECT_EQ(run.record.notes, (std::vector<std::string>{"verdict=unknown"})); EXPECT(!run.normalized);
}

ASYNC_TEST("a classifier transport failure fails closed with a probe error") {
  const auto fixture = contextOf({{.reject = "dns lookup failed"}}); AWAIT(run, fixture.ctx.runClassifier(classifierOptions));
  EXPECT_EQ(run.record.outcome, "failed"); EXPECT(!run.record.status); expectError(run, "probe_error", "Error: dns lookup failed"); expectUnknown(run, {}, "classifier transport failed: Error: dns lookup failed");
  EXPECT_EQ(run.record.notes, (std::vector<std::string>{"verdict=unknown"})); ASSERT(run.record.request); EXPECT_EQ(run.record.request->assistantToolTurns, 0); EXPECT_EQ(run.record.request->toolResultTurns, 0); EXPECT(!run.record.request->analysisInWire);
}

ASYNC_TEST("a classifier answer is parsed into a literal verdict") {
  const auto fixture = contextOf({{.payload = completion({{"content", "true"}, {"reasoning_content", "Decided yes."}})}, {.payload = completion({{"content", " FALSE "}})},
    {.payload = completion({{"content", "maybe"}})}, {.payload = completion({{"content", nullptr}, {"refusal", "no"}})},
    {.payload = completion({{"content", nullptr}, {"tool_calls", json::array{json::object{{"id", "call_a"}, {"type", "function"}, {"function", json::object{{"name", "get_weather"}, {"arguments", "{}"}}}}}}}, "tool_calls")},
    {.payload = completion({{"content", nullptr}, {"reasoning_content", "Only reasoning."}})}});
  AWAIT(affirmed, fixture.ctx.runClassifier(classifierOptions)); EXPECT_EQ(affirmed.record.outcome, "ok"); EXPECT_EQ(affirmed.verdict, std::optional<classifier::BootstrapClassifierVerdict>(classifier::BootstrapClassifierVerdictTrue{.raw = "true"}));
  EXPECT_EQ(affirmed.record.notes, (std::vector<std::string>{"verdict=true"})); ASSERT(affirmed.normalized); EXPECT_EQ(affirmed.normalized->content, std::optional<std::string>("true")); ASSERT(affirmed.record.response); EXPECT(affirmed.record.response->reasoningPresent);
  AWAIT(denied, fixture.ctx.runClassifier(classifierOptions)); EXPECT_EQ(denied.verdict, std::optional<classifier::BootstrapClassifierVerdict>(classifier::BootstrapClassifierVerdictFalse{.raw = "FALSE"})); EXPECT_EQ(denied.record.notes, (std::vector<std::string>{"verdict=false"}));
  AWAIT(ambiguous, fixture.ctx.runClassifier(classifierOptions)); expectUnknown(ambiguous, "maybe", "classifier final content is not exactly 'true' or 'false'");
  AWAIT(refused, fixture.ctx.runClassifier(classifierOptions)); expectUnknown(refused, {}, "classifier request was refused");
  AWAIT(called, fixture.ctx.runClassifier(classifierOptions)); expectUnknown(called, {}, "classifier emitted a tool call");
  AWAIT(reasoningOnly, fixture.ctx.runClassifier(classifierOptions)); expectUnknown(reasoningOnly, {}, "classifier produced only reasoning and no final content");
}

ASYNC_TEST("a probe context exposes the injected transport and clock") {
  const auto script = scriptedTransport({{.payload = completion({{"content", "ok"}})}}); const auto ctx = probes::createProbeContext(script.transport, [] { return rt::Date(1700000000000.0); });
  EXPECT(ctx.transport == script.transport); EXPECT_EQ(ctx.now().getTime(), 1700000000000.0); const auto body = basicBody(); AWAIT(run, ctx.runRaw(body));
  EXPECT_EQ(run.record.outcome, "ok"); EXPECT_EQ(script.bodies->size(), std::size_t{1});
}

ASYNC_TEST("a raw probe reports an adapter refusal thrown by the transport as a probe error") {
  const adapter::HarmonyTransport transport = [](json::object) -> rt::Task<rt::Response> { throw types::HarmonyAdapterError("simulated adapter refusal", "invalid-request"); };
  const auto ctx = probes::createProbeContext(transport, [] { return rt::Date(0.0); }); const auto body = basicBody(); AWAIT(run, ctx.runRaw(body));
  EXPECT_EQ(run.record.outcome, "failed"); EXPECT(!run.record.status); expectError(run, "probe_error", "HarmonyAdapterError: simulated adapter refusal"); EXPECT(!run.normalized);
}
