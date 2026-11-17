// Port of tests/reliability-harness.test.ts; all transports and tool backends are injected fakes.
// Links: tests/reliability_harness_test.cpp src/harmony/reliability/harness.cpp
// src/harmony/reliability/{context,failure,feedback,hash,loops,retry,state,surfaces,verify}.cpp
// src/harmony/tools/{backend,fakes,result,router,schemas}.cpp
// src/harmony/{adapter,classifier,conversation,parse,render,types}.cpp
// src/provider/cerebras.cpp src/sentinel/upstream_capture.cpp src/utils.cpp
#include "uos/harmony/reliability/harness.hpp"
#include "uos/harmony/reliability/surfaces.hpp"
#include "uos/harmony/tools/fakes.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace {
  using namespace uos;
  using namespace uos::harmony::reliability::harness;
  namespace adapter = uos::harmony::adapter;
  namespace context = uos::harmony::reliability::context;
  namespace fakes = uos::harmony::tools::fakes;
  namespace retry = uos::harmony::reliability::retry;
  namespace verify = uos::harmony::reliability::verify;
  using uos::harmony::tools::backend::ToolBackends;
  using fakes::createFakeToolBackends;
  using fakes::FakeShell;
  using fakes::FakeShellEntry;
  using fakes::FakeToolBackendsOptions;

  rt::OrderedMap<std::string, std::string> files() {
    return {{"a.txt", "a=0\n"}, {"b.txt", "b=0\n"}, {"notes.txt", "note body\n"}};
  }

  ToolBackends backends() {
    return createFakeToolBackends(FakeToolBackendsOptions{.files = files(), .shell = std::make_shared<FakeShell>()});
  }

  rt::Response completion(json::object message) {
    const bool toolCalls = rt::truthy(rt::get(message, "tool_calls"));
    json::object assistant{{"role", "assistant"}};
    rt::objectAssign(assistant, message);
    return rt::Response::json(json::object{
      {"id", "cmpl-c"}, {"object", "chat.completion"}, {"created", 1}, {"model", "gpt-oss-120b"},
      {"choices", json::array{json::object{{"index", 0}, {"message", assistant}, {"finish_reason", toolCalls ? "tool_calls" : "stop"}}}},
    });
  }

  json::object callStep(std::string name, json::object arguments) {
    return {{"toolCalls", json::array{json::object{{"name", name}, {"arguments", arguments}}}}};
  }

  /** Deterministic scripted transport: one response per request. */
  adapter::HarmonyTransport scriptedTransport(json::array script) {
    struct Script { json::array steps; std::size_t index = 0; };
    auto state = std::make_shared<Script>(Script{std::move(script)});
    return [state](json::object) -> rt::Task<rt::Response> {
      const auto& step = state->steps.at(std::min(state->index, state->steps.size() - 1));
      state->index += 1;
      json::object message{{"content", rt::getString(step, "content").value_or("")}};
      if (const auto analysis = rt::getString(step, "analysis")) message["reasoning_content"] = *analysis;
      if (const auto* calls = rt::getArray(step, "toolCalls")) {
        json::array toolCalls;
        for (std::size_t index = 0; index < calls->size(); ++index) {
          const auto& call = calls->at(index);
          toolCalls.push_back(json::object{
            {"id", "call-" + rt::numberToString(static_cast<double>(state->index)) + "-" + rt::numberToString(static_cast<double>(index))},
            {"type", "function"}, {"function", json::object{
              {"name", *rt::get(call, "name")}, {"arguments", rt::getString(call, "rawArguments") ? std::string(*rt::getString(call, "rawArguments")) : rt::jsonStringify(*rt::get(call, "arguments"))},
            }},
          });
        }
        message["tool_calls"] = std::move(toolCalls);
      }
      return rt::fulfilledTask(completion(std::move(message)));
    };
  }

  HarnessOptions baseOptions(json::array script) {
    return {
      .systemPrompt = renderCanonicalPolicy({.tools = {"filesystem.read", "editor.apply_patch", "shell.exec"}, .budget = context::ContextBudgetKind::medium}),
      .userPrompt = "Fix a.txt and verify.",
      .transport = scriptedTransport(std::move(script)),
      .backends = backends(),
      .verificationCommand = "sh tests/run.sh",
    };
  }

  template <class T> std::vector<const T*> events(const HarnessOutcome& outcome) {
    std::vector<const T*> selected;
    for (const auto& event : outcome.events) if (const auto* value = std::get_if<T>(&event)) selected.push_back(value);
    return selected;
  }

  const HarnessEventToolResult* resultFor(const HarnessOutcome& outcome, const std::string& id) {
    for (const auto* result : events<HarnessEventToolResult>(outcome)) if (result->id == id) return result;
    return nullptr;
  }

  auto isolate() { return rt::env::ScopedEnvironment({{"HOME", "/nonexistent/w07c3-no-credentials"}}); }
}

#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)

ASYNC_TEST("harness: a clean scripted run completes with verified writes") {
  auto isolation = isolate();
  auto opts = baseOptions({
    callStep("filesystem.read", {{"path", "a.txt"}}),
    callStep("editor.apply_patch", {{"path", "a.txt"}, {"old", "a=0"}, {"new", "a=1"}}),
    callStep("filesystem.read", {{"path", "a.txt"}}),
    json::object{{"content", "Fixed a.txt."}},
  });
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "completed");
  EXPECT_EQ(outcome.finalContent.value_or(""), "Fixed a.txt.");
  EXPECT_EQ(outcome.state.pendingVerification.size(), 0u);
  EXPECT_EQ(outcome.state.writes.at(0).verified, true);
  EXPECT_EQ(outcome.modelCalls, 4.0);
  EXPECT_EQ(events<HarnessEventGuard>(outcome).size(), 0u);
  EXPECT(!outcome.classification.failure_class);
}

ASYNC_TEST("harness: invalid calls are never executed and feedback is deterministic") {
  auto isolation = isolate();
  auto opts = baseOptions({
    callStep("filesystem.read", {{"path", 42}}),
    callStep("filesystem.read", {{"path", "notes.txt"}}),
    json::object{{"content", "done"}},
  });
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "completed");
  const auto calls = events<HarnessEventToolCall>(outcome);
  const auto invalid = std::find_if(calls.begin(), calls.end(), [](const auto* call) { return !call->valid; });
  EXPECT(invalid != calls.end());
  EXPECT_EQ((*invalid)->invalidReason.value_or(""), "wrong_type:arguments.path");
  const auto* result = resultFor(outcome, (*invalid)->id);
  EXPECT(result);
  EXPECT_EQ(result->result.error_code.value_or(""), "invalid_args");
  EXPECT(rt::Regex("invalid arguments \\(1 issue\\(s\\)\\)").test(result->result.error.value_or("")));
  EXPECT_EQ(outcome.state.invalidCalls, 1.0);
}

ASYNC_TEST("harness: exact duplicates after success are blocked, not re-executed") {
  auto isolation = isolate();
  auto opts = baseOptions({
    callStep("filesystem.read", {{"path", "a.txt"}}),
    callStep("filesystem.read", {{"path", "a.txt"}}),
    callStep("editor.apply_patch", {{"path", "a.txt"}, {"old", "a=0"}, {"new", "a=1"}}),
    callStep("filesystem.read", {{"path", "a.txt"}}),
    json::object{{"content", "done"}},
  });
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "completed");
  const auto calls = events<HarnessEventToolCall>(outcome);
  const auto repeated = std::find_if(calls.begin(), calls.end(), [](const auto* call) { return call->repeated.hasValue(); });
  EXPECT(repeated != calls.end());
  EXPECT_EQ(*(*repeated)->repeated, "repeat_after_success");
  const auto* result = resultFor(outcome, (*repeated)->id);
  EXPECT(result);
  EXPECT_EQ(result->result.error_code.value_or(""), "duplicate_call");
  EXPECT(rt::Regex("duplicate of the previous call").test(result->result.error.value_or("")));
  EXPECT_EQ(outcome.state.duplicateCalls, 1.0);
}

ASYNC_TEST("harness: repeated calls after deterministic failures are blocked as repeated_failure") {
  auto isolation = isolate();
  auto opts = baseOptions({
    callStep("editor.apply_patch", {{"path", "missing.txt"}, {"old", "x"}, {"new", "y"}}),
    callStep("editor.apply_patch", {{"path", "missing.txt"}, {"old", "x"}, {"new", "y"}}),
    callStep("editor.apply_patch", {{"path", "missing.txt"}, {"add", true}, {"new", "y"}}),
    callStep("filesystem.read", {{"path", "missing.txt"}}),
    json::object{{"content", "done"}},
  });
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "completed");
  const auto results = events<HarnessEventToolResult>(outcome);
  EXPECT_EQ(results.at(0)->result.error_code.value_or(""), "patch_failed");
  EXPECT_EQ(results.at(1)->result.error_code.value_or(""), "repeated_failure");
  EXPECT(rt::Regex("change the arguments or approach").test(results.at(1)->result.error.value_or("")));
}

ASYNC_TEST("harness: a final without verification is rejected, then accepted after verification") {
  auto isolation = isolate();
  auto opts = baseOptions({
    callStep("editor.apply_patch", {{"path", "a.txt"}, {"old", "a=0"}, {"new", "a=1"}}),
    json::object{{"content", "done"}},
    callStep("filesystem.read", {{"path", "a.txt"}}),
    json::object{{"content", "done"}},
  });
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "completed");
  const auto guards = events<HarnessEventGuard>(outcome);
  EXPECT_EQ(guards.size(), 1u);
  EXPECT_EQ(guards.at(0)->kind, "unverified_write");
  const auto finals = events<HarnessEventFinal>(outcome);
  EXPECT_EQ(finals.size(), 2u);
  EXPECT_EQ(finals.at(0)->accepted, false);
  EXPECT_EQ(finals.at(1)->accepted, true);
  EXPECT_EQ(outcome.state.writes.at(0).verified, true);
}

ASYNC_TEST("harness: two identical finals with no intervening action abort as false_completion") {
  auto isolation = isolate();
  auto opts = baseOptions({
    callStep("editor.apply_patch", {{"path", "a.txt"}, {"old", "a=0"}, {"new", "a=1"}}),
    json::object{{"content", "done"}}, json::object{{"content", "done"}},
  });
  opts.maxGuardRejections = 8;
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "failed");
  EXPECT_EQ(outcome.abortedReason.value_or(""), "false_completion");
  EXPECT_EQ(outcome.classification.failure_class.value_or(""), "false_completion");
  const auto guards = events<HarnessEventGuard>(outcome);
  EXPECT(std::any_of(guards.begin(), guards.end(), [](const auto* guard) { return guard->kind == "false_completion"; }));
}

ASYNC_TEST("harness: transient timeouts are auto-retried once, then blocked") {
  auto isolation = isolate();
  auto shell = std::make_shared<FakeShell>(std::vector<FakeShellEntry>{
    {.command = std::string("slow-cmd"), .timed_out = true},
    {.command = std::string("true"), .exit_code = 0, .stdout = "ok"},
  });
  auto opts = baseOptions({
    callStep("shell.exec", {{"command", "slow-cmd"}}), callStep("shell.exec", {{"command", "slow-cmd"}}),
    callStep("shell.exec", {{"command", "slow-cmd"}}), callStep("shell.exec", {{"command", "true"}}),
    json::object{{"content", "ok"}},
  });
  opts.backends = createFakeToolBackends(FakeToolBackendsOptions{.files = files(), .shell = shell});
  opts.retryPolicy = retry::RetryPolicy{1, 0, {"timeout", "internal", "unavailable", "transport"}};
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "completed");
  const auto results = events<HarnessEventToolResult>(outcome);
  EXPECT_EQ(std::count_if(results.begin(), results.end(), [](const auto* result) { return result->result.error_code == "timeout"; }), 2);
  EXPECT_EQ(std::count_if(results.begin(), results.end(), [](const auto* result) { return result->result.error_code == "repeated_failure"; }), 1);
  EXPECT(std::any_of(results.begin(), results.end(), [](const auto* result) { return result->result.error_code == "timeout"; }));
}

ASYNC_TEST("harness: semantic loops are detected, guarded, and recoverable") {
  auto isolation = isolate();
  json::array script;
  for (int i = 0; i < 5; ++i) {
    script.push_back(callStep("filesystem.read", {{"path", "a.txt"}}));
    script.push_back(callStep("editor.apply_patch", {{"path", "missing.txt"}, {"old", "x"}, {"new", "y"}}));
  }
  script.push_back(callStep("task.update_plan", {{"plan", json::array{"created result.txt instead"}}}));
  script.push_back(callStep("editor.apply_patch", {{"path", "result.txt"}, {"add", true}, {"new", "done"}}));
  script.push_back(callStep("filesystem.read", {{"path", "result.txt"}}));
  script.push_back(json::object{{"content", "done"}});
  auto opts = baseOptions(script);
  opts.loopThreshold = 3;
  opts.maxTurns = 30;
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT(outcome.state.semanticLoops > 0);
  const auto guards = events<HarnessEventGuard>(outcome);
  EXPECT(std::any_of(guards.begin(), guards.end(), [](const auto* guard) { return guard->kind == "loop"; }));
  EXPECT_EQ(outcome.phase, "completed");
}

ASYNC_TEST("harness: unknown experimental tools on the broad surface get deterministic feedback") {
  auto isolation = isolate();
  auto opts = baseOptions({
    callStep("filesystem.write", {{"path", "a.txt"}, {"content", "x"}}),
    callStep("filesystem.read", {{"path", "notes.txt"}}), json::object{{"content", "done"}},
  });
  opts.tools = harmony::reliability::surfaces::broadToolSurface().definitions;
  AWAIT(outcome, runReliabilityHarness(opts));
  const auto calls = events<HarnessEventToolCall>(outcome);
  const auto invalid = std::find_if(calls.begin(), calls.end(), [](const auto* call) { return call->tool == "filesystem.write"; });
  EXPECT(invalid != calls.end());
  EXPECT_EQ((*invalid)->valid, false);
  EXPECT_EQ((*invalid)->invalidReason.value_or(""), "unknown_tool:tool");
  const auto* result = resultFor(outcome, (*invalid)->id);
  EXPECT(result);
  EXPECT(rt::Regex("unknown_tool").test(result->result.error.value_or("")));
}

ASYNC_TEST("harness: guard rejection budget exhausts deterministically") {
  auto isolation = isolate();
  auto opts = baseOptions({
    callStep("editor.apply_patch", {{"path", "a.txt"}, {"old", "a=0"}, {"new", "a=1"}}),
    json::object{{"content", "first"}}, json::object{{"content", "second"}}, json::object{{"content", "third"}},
  });
  opts.maxGuardRejections = 2;
  opts.maxTurns = 12;
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "failed");
  EXPECT_EQ(outcome.abortedReason.value_or(""), "guard_exhausted");
  EXPECT_EQ(outcome.classification.failure_class.value_or(""), "guard_exhausted");
  EXPECT(events<HarnessEventGuard>(outcome).size() >= 2);
}

ASYNC_TEST("harness: configured final-attempt limit is enforced") {
  auto isolation = isolate();
  auto opts = baseOptions({
    callStep("editor.apply_patch", {{"path", "a.txt"}, {"old", "a=0"}, {"new", "a=1"}}),
    json::object{{"content", "unverified"}}, json::object{{"content", "must not be requested"}},
  });
  opts.maxGuardRejections = 8;
  auto policy = verify::DEFAULT_VERIFICATION_POLICY;
  policy.maxRepeatedFinals = 8;
  policy.maxFinalAttempts = 1;
  policy.verificationCommand = "sh tests/run.sh";
  opts.verificationPolicy = policy;
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "failed");
  EXPECT_EQ(outcome.abortedReason.value_or(""), "guard_exhausted");
  EXPECT_EQ(events<HarnessEventFinal>(outcome).size(), 1u);
}

ASYNC_TEST("harness: deterministic HTTP failures are not retried") {
  auto isolation = isolate();
  auto calls = std::make_shared<int>(0);
  auto opts = baseOptions({});
  opts.transport = [calls](json::object) -> rt::Task<rt::Response> {
    ++*calls;
    return rt::fulfilledTask(rt::Response("unauthorized", {.status = 401}));
  };
  opts.retryPolicy = retry::RetryPolicy{3, 0, {"transport"}};
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(*calls, 1);
  EXPECT_EQ(outcome.abortedReason.value_or(""), "transport_failed");
  EXPECT_EQ(events<HarnessEventModelRequest>(outcome).size(), 1u);
}

ASYNC_TEST("harness: whole-run cancellation aborts a stalled injected transport") {
  auto isolation = isolate();
  rt::AbortController controller;
  auto observedSignal = std::make_shared<std::optional<rt::AbortSignal>>();
  auto opts = baseOptions({});
  opts.transport = [observedSignal](json::object, std::optional<adapter::HarmonyTransportRequestOptions> options) -> rt::Task<rt::Response> {
    *observedSignal = options ? options->signal : std::nullopt;
    auto pending = rt::withResolvers<rt::Response>();
    if (options && options->signal) {
      const auto signal = *options->signal;
      if (signal.aborted()) {
        pending.reject(rt::makeReason(rt::withCause(rt::JsError("stalled transport aborted before dispatch"), signal.reason())));
      } else {
        rt::EventListener onAbort([signal, reject = pending.reject] {
          reject(rt::makeReason(rt::withCause(rt::JsError("stalled transport aborted while awaiting the response"), signal.reason())));
        });
        signal.addEventListener("abort", onAbort, {.once = true});
      }
    }
    return pending.promise.wait();
  };
  opts.signal = controller.signal();
  const double started = rt::dateNow();
  auto watchdog = rt::withResolvers<HarnessOutcome>();
  const auto timeoutTimer = rt::setTimeout([reject = watchdog.reject] { reject(rt::makeReason(rt::JsError("stalled transport ignored cancellation"))); }, 500);
  auto cleanup = rt::scopeExit([timeoutTimer] { rt::clearTimeout(timeoutTimer); });
  rt::setTimeout([controller] { controller.abort(rt::makeReason(rt::DOMException("task deadline", "TimeoutError"))); }, 20);
  rt::Promise<HarnessOutcome> running(runReliabilityHarness(opts));
  auto raced = rt::race(running, watchdog.promise);
  AWAIT(outcome, raced.wait());
  EXPECT(*observedSignal == controller.signal());
  EXPECT_EQ(outcome.phase, "aborted");
  EXPECT_EQ(outcome.abortedReason.value_or(""), "signal");
  EXPECT(rt::dateNow() - started < 500);
}

ASYNC_TEST("harness: a stalled transport that ignores cancellation stops at the run deadline") {
  auto isolation = isolate();
  rt::AbortController controller;
  auto observedSignal = std::make_shared<std::optional<rt::AbortSignal>>();
  auto opts = baseOptions({});
  opts.transport = [observedSignal](json::object, std::optional<adapter::HarmonyTransportRequestOptions> options) -> rt::Task<rt::Response> {
    *observedSignal = options ? options->signal : std::nullopt;
    return rt::Promise<rt::Response>().wait();
  };
  opts.signal = controller.signal();
  auto watchdog = rt::withResolvers<HarnessOutcome>();
  const auto watchdogTimer = rt::setTimeout([reject = watchdog.reject] { reject(rt::makeReason(rt::JsError("stalled transport ignored cancellation"))); }, 1000);
  const auto abortTimer = rt::setTimeout([controller] { controller.abort(rt::makeReason(rt::DOMException("task deadline", "TimeoutError"))); }, 20);
  auto cleanup = rt::scopeExit([watchdogTimer, abortTimer] { rt::clearTimeout(watchdogTimer); rt::clearTimeout(abortTimer); });
  const double started = rt::dateNow();
  rt::Promise<HarnessOutcome> running(runReliabilityHarness(opts));
  auto raced = rt::race(running, watchdog.promise);
  AWAIT(outcome, raced.wait());
  EXPECT(*observedSignal == controller.signal());
  EXPECT_EQ(outcome.phase, "aborted");
  EXPECT_EQ(outcome.abortedReason.value_or(""), "signal");
  EXPECT(rt::dateNow() - started < 1000);
}

ASYNC_TEST("harness: structured mode requests carry deterministically fewer tokens than full mode") {
  auto isolation = isolate();
  json::array script;
  rt::OrderedMap<std::string, std::string> bigFiles;
  for (int i = 0; i < 12; ++i) {
    const auto path = "file-" + rt::padStart(rt::numberToString(i), 2, "0") + ".txt";
    bigFiles.set(path, rt::repeat("line " + rt::numberToString(i) + "\n", 150));
    script.push_back(callStep("filesystem.read", {{"path", path}}));
  }
  script.push_back(json::object{{"content", "done"}});
  const auto bigBackends = createFakeToolBackends(FakeToolBackendsOptions{.files = bigFiles, .shell = std::make_shared<FakeShell>()});
  auto fullOptions = baseOptions(script);
  fullOptions.contextMode = "full";
  fullOptions.backends = bigBackends;
  AWAIT(full, runReliabilityHarness(fullOptions));
  auto structuredOptions = baseOptions(script);
  structuredOptions.contextMode = "structured";
  structuredOptions.backends = bigBackends;
  AWAIT(structured, runReliabilityHarness(structuredOptions));
  EXPECT_EQ(full.phase, "completed");
  EXPECT_EQ(structured.phase, "completed");
  const auto fullRequests = events<HarnessEventModelRequest>(full);
  const auto structuredRequests = events<HarnessEventModelRequest>(structured);
  EXPECT(structuredRequests.back()->estimatedTokens < fullRequests.back()->estimatedTokens);
}

ASYNC_TEST("harness: verificationCommand declared in the harness option satisfies writes") {
  auto isolation = isolate();
  auto shell = std::make_shared<FakeShell>(std::vector<FakeShellEntry>{{.command = std::string("sh tests/run.sh"), .exit_code = 0, .stdout = "ok"}});
  auto opts = baseOptions({
    callStep("editor.apply_patch", {{"path", "a.txt"}, {"old", "a=0"}, {"new", "a=1"}}),
    callStep("shell.exec", {{"command", "sh tests/run.sh"}}), json::object{{"content", "done"}},
  });
  opts.backends = createFakeToolBackends(FakeToolBackendsOptions{.files = files(), .shell = shell});
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "completed");
  EXPECT_EQ(outcome.state.writes.at(0).verified, true);
  EXPECT_EQ(outcome.state.writes.at(0).verifiedBy.value_or(""), "shell");
}

ASYNC_TEST("harness: maxTurns aborts as stalled turn_limit") {
  auto isolation = isolate();
  auto opts = baseOptions({callStep("filesystem.read", {{"path", "a.txt"}})});
  opts.maxTurns = 2;
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "failed");
  EXPECT_EQ(outcome.abortedReason.value_or(""), "turn_limit");
  EXPECT_EQ(outcome.classification.failure_class.value_or(""), "stalled");
}

TEST("harness: policy preambles are deterministic and reference the surface") {
  auto isolation = isolate();
  const auto a = renderCanonicalPolicy({.tools = {"filesystem.read"}, .budget = context::ContextBudgetKind::large});
  const auto b = renderCanonicalPolicy({.tools = {"filesystem.read"}, .budget = context::ContextBudgetKind::large});
  EXPECT_EQ(a, b);
  EXPECT(rt::Regex("filesystem\\.read").test(a));
  EXPECT(rt::Regex("Context budget tier: large").test(a));
}

namespace {
  json::value nullable(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }
  json::value nullable(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }

  json::array toolCallsJson(const std::vector<harmony::types::ToolCall>& calls) {
    json::array out;
    for (const auto& call : calls) out.push_back(json::object{{"id", call.id}, {"name", call.name}, {"arguments", call.arguments}});
    return out;
  }

  json::object normalizedJson(const harmony::types::NormalizedAssistantResponse& response) {
    json::array turns;
    for (const auto& turn : response.turns) turns.push_back(std::visit([](const auto& item) -> json::value {
      json::object out{{"kind", std::string(std::decay_t<decltype(item)>::kind)}};
      if constexpr (requires { item.recipient; }) {
        out["recipient"] = item.recipient; out["name"] = item.name; out["arguments"] = item.arguments;
      } else out["text"] = item.text;
      return out;
    }, turn));
    const auto& shape = response.shape;
    return {
      {"turns", turns}, {"analysis", json::value_from(response.analysis)}, {"content", nullable(response.content)},
      {"toolCalls", toolCallsJson(response.toolCalls)}, {"finishReason", nullable(response.finishReason)}, {"refusal", nullable(response.refusal)},
      {"shape", json::object{
        {"contentPresent", shape.contentPresent}, {"contentChars", rt::makeNumber(shape.contentChars)}, {"reasoningField", shape.reasoningField},
        {"reasoningChars", rt::makeNumber(shape.reasoningChars)}, {"toolCallsField", shape.toolCallsField}, {"toolCallCount", rt::makeNumber(shape.toolCallCount)},
        {"finishReason", nullable(shape.finishReason)}, {"refusal", shape.refusal},
      }},
      {"id", response.id}, {"model", response.model}, {"created", rt::makeNumber(response.created)},
    };
  }

  json::object toolResultJson(const harmony::tools::result::ToolResult& result) {
    json::object out{{"ok", result.ok}};
    rt::objectSet(out, "output", result.output); rt::objectSet(out, "error", result.error); rt::objectSet(out, "error_code", result.error_code);
    rt::objectSet(out, "exit_code", result.exit_code); rt::objectSet(out, "stdout", result.stdout); rt::objectSet(out, "stderr", result.stderr);
    return out;
  }

  json::object eventJson(const HarnessEvent& event) {
    return std::visit([](const auto& item) -> json::object {
      using T = std::decay_t<decltype(item)>;
      json::object out{{"type", std::string(T::type)}};
      if constexpr (std::is_same_v<T, HarnessEventModelRequest>) {
        out["id"] = rt::makeNumber(item.id); out["mode"] = item.mode; out["body"] = item.built.body; out["estimatedTokens"] = rt::makeNumber(item.estimatedTokens);
      } else if constexpr (std::is_same_v<T, HarnessEventModelResponse>) {
        out["requestId"] = rt::makeNumber(item.requestId); out["normalized"] = normalizedJson(item.normalized); out["estimatedTokens"] = rt::makeNumber(item.estimatedTokens);
      } else if constexpr (std::is_same_v<T, HarnessEventToolCall>) {
        out["id"] = item.id; out["tool"] = item.tool; out["arguments"] = item.arguments; out["valid"] = item.valid;
        rt::objectSet(out, "invalidReason", item.invalidReason); rt::objectSet(out, "repeated", item.repeated);
      } else if constexpr (std::is_same_v<T, HarnessEventToolResult>) {
        out["id"] = item.id; out["result"] = toolResultJson(item.result); rt::objectSet(out, "durationMs", item.durationMs);
      } else if constexpr (std::is_same_v<T, HarnessEventGuard>) {
        out["kind"] = item.kind; out["message"] = item.message; out["attempt"] = rt::makeNumber(item.attempt); out["phase"] = item.phase;
      } else {
        out["content"] = item.content; out["accepted"] = item.accepted; out["attempt"] = rt::makeNumber(item.attempt);
      }
      return out;
    }, event);
  }

  json::object conversationJson(const harmony::conversation::Conversation& conversation) {
    json::array turns;
    for (const auto& turn : conversation.turns) turns.push_back(std::visit([](const auto& item) -> json::value {
      using T = std::decay_t<decltype(item)>;
      json::object out{{"role", std::string(T::role)}};
      if constexpr (std::is_same_v<T, harmony::types::AssistantTurn>) {
        out["content"] = nullable(item.content); out["analysis"] = json::value_from(item.analysis);
        out["toolCalls"] = toolCallsJson(item.toolCalls); out["finishReason"] = nullable(item.finishReason);
      } else if constexpr (std::is_same_v<T, harmony::types::ToolResultTurn>) {
        out["toolCallId"] = item.toolCallId; out["name"] = item.name; out["content"] = item.content;
      } else out["content"] = item.content;
      return out;
    }, turn));
    return {{"turns", turns}};
  }

  json::object stateJson(const harmony::reliability::state::StructuredTaskState& state) {
    json::array reads, writes, errors, pending, commands, edits, finals;
    for (const auto& item : state.reads) reads.push_back(json::object{{"path", item.path}, {"digest", item.digest}, {"seq", rt::makeNumber(item.seq)}});
    for (const auto& item : state.writes) writes.push_back(json::object{
      {"path", item.path}, {"marker", item.marker}, {"add", item.add}, {"verified", item.verified}, {"verifiedBy", nullable(item.verifiedBy)}, {"seq", rt::makeNumber(item.seq)},
    });
    for (const auto& item : state.errors) errors.push_back(json::object{{"code", item.code}, {"tool", item.tool}, {"seq", rt::makeNumber(item.seq)}, {"digest", item.digest}});
    for (const auto& item : state.pendingVerification) pending.push_back(json::object{{"path", item.path}, {"seq", rt::makeNumber(item.seq)}});
    for (const auto& item : state.unresolvedCommands) commands.push_back(json::object{{"command", item.command}, {"seq", rt::makeNumber(item.seq)}});
    for (const auto& item : state.unresolvedEdits) edits.push_back(json::object{{"path", item.path}, {"seq", rt::makeNumber(item.seq)}});
    for (const auto& item : state.finals) finals.push_back(json::object{{"content", item.content}, {"accepted", item.accepted}, {"seq", rt::makeNumber(item.seq)}});
    return {
      {"phase", state.phase}, {"modelCalls", rt::makeNumber(state.modelCalls)}, {"toolCalls", rt::makeNumber(state.toolCalls)},
      {"invalidCalls", rt::makeNumber(state.invalidCalls)}, {"invalidCallStreak", rt::makeNumber(state.invalidCallStreak)},
      {"duplicateCalls", rt::makeNumber(state.duplicateCalls)}, {"semanticLoops", rt::makeNumber(state.semanticLoops)}, {"semanticLoopStreak", rt::makeNumber(state.semanticLoopStreak)},
      {"plan", json::object{{"items", json::value_from(state.plan.items)}, {"seq", nullable(state.plan.seq)}}},
      {"reads", reads}, {"writes", writes}, {"errors", errors}, {"pendingVerification", pending}, {"unresolvedCommands", commands}, {"unresolvedEdits", edits},
      {"lastActionSeq", rt::makeNumber(state.lastActionSeq)}, {"finals", finals}, {"finalAttempts", rt::makeNumber(state.finalAttempts)},
    };
  }

  json::value outcomeJson(const HarnessOutcome& outcome) {
    json::array emitted;
    for (const auto& event : outcome.events) emitted.push_back(eventJson(event));
    return rt::canonicalNumbers(json::object{
      {"phase", outcome.phase}, {"finalContent", nullable(outcome.finalContent)}, {"conversation", conversationJson(outcome.conversation)},
      {"state", stateJson(outcome.state)}, {"stateContract", harmony::reliability::state::stateContract(outcome.state)},
      {"classification", json::value_from(outcome.classification)}, {"events", emitted}, {"modelCalls", rt::makeNumber(outcome.modelCalls)}, {"abortedReason", nullable(outcome.abortedReason)},
    });
  }

  //20 complete Bun1.4.0 outcome/event/transcript/request vectors. JSON SHA-256:
  //af074e050a51d1c5c72a7ee1bb31f90fbefb1c669892cb8997a15781d8392815
  const json::array& oracleVectors() {
    static const auto vectors = [] {
      const auto compressed = rt::fromBase64("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");
      const auto bytes = rt::gzip::gunzipSync(compressed);
      return rt::jsonParse(std::string(bytes.begin(), bytes.end())).as_array();
    }();
    return vectors;
  }

  adapter::HarmonyTransport responseTransport(json::array steps) {
    struct Script { json::array steps; std::size_t index = 0; };
    auto script = std::make_shared<Script>(Script{std::move(steps)});
    return [script](json::object) -> rt::Task<rt::Response> {
      const auto& step = script->steps.at(std::min(script->index++, script->steps.size() - 1));
      if (const auto error = rt::getString(step, "error")) return rt::Promise<rt::Response>::reject(rt::makeReason(rt::JsError(std::string(*error)))).wait();
      if (const auto status = rt::getNumber(step, "status")) return rt::fulfilledTask(rt::Response("upstream", {.status = *status}));
      if (const auto text = rt::getString(step, "text")) return rt::fulfilledTask(rt::Response(std::string(*text)));
      return rt::fulfilledTask(completion({{"content", rt::getString(step, "content").value_or("")}}));
    };
  }

  rt::Task<void> runVector(std::size_t index) {
    rt::globalDateNow() = [] { return 1000.0; };
    const auto& vector = oracleVectors().at(index);
    const auto* script = rt::getArray(vector, "script");
    auto opts = baseOptions(script ? *script : json::array{});
    opts.retryPolicy = retry::RetryPolicy{1, 0, {"timeout", "internal", "unavailable", "transport"}};
    opts.backends = createFakeToolBackends(FakeToolBackendsOptions{
      .files = files(), .shell = std::make_shared<FakeShell>(std::vector<FakeShellEntry>{{.command = std::string("policy-check"), .stdout = "ok"}}),
    });
    if (const auto* responses = rt::getArray(vector, "responses")) opts.transport = responseTransport(*responses);
    if (const auto* options = rt::getObject(vector, "options")) {
      if (const auto value = rt::getString(*options, "systemPrompt")) opts.systemPrompt = *value;
      if (const auto value = rt::getString(*options, "userPrompt")) opts.userPrompt = *value;
      if (const auto value = rt::getString(*options, "contextMode")) opts.contextMode = *value;
      if (const auto value = rt::getString(*options, "transcriptBudget")) opts.transcriptBudget = context::parseContextBudgetKind(*value);
      if (const auto value = rt::getNumber(*options, "invalidCallStreakLimit")) opts.invalidCallStreakLimit = *value;
      if (const auto value = rt::getNumber(*options, "maxToolCalls")) opts.maxToolCalls = *value;
      if (const auto value = rt::getNumber(*options, "maxTurns")) opts.maxTurns = *value;
      if (rt::getBool(*options, "requirePlanBeforeWrites").value_or(false) || rt::hasOwn(*options, "policyCommand")) {
        auto policy = verify::DEFAULT_VERIFICATION_POLICY;
        policy.requirePlanBeforeWrites = rt::getBool(*options, "requirePlanBeforeWrites").value_or(false);
        if (const auto command = rt::getString(*options, "policyCommand")) policy.verificationCommand = *command;
        opts.verificationPolicy = policy;
      }
    }
    auto emitted = std::make_shared<json::array>();
    opts.emit = [emitted](const HarnessEvent& event) { emitted->push_back(eventJson(event)); };
    AWAIT(outcome, runReliabilityHarness(opts));
    const auto actual = outcomeJson(outcome);
    const auto* expected = rt::get(vector, "expected");
    EXPECT(expected);
    EXPECT(rt::deepEqual(actual, *expected));
    EXPECT_EQ(rt::jsonStringify(actual), rt::jsonStringify(*expected));
    EXPECT(rt::deepEqual(*emitted, *rt::get(actual, "events")));
  }
}

#define VECTOR_TEST(name, index) ASYNC_TEST(name) { auto isolation = isolate(); co_await runVector(index); }
VECTOR_TEST("harness differential: verified-full-short", 0)
VECTOR_TEST("harness differential: verified-structured-large", 1)
VECTOR_TEST("harness differential: invalid-streak-terminal-state-quirk", 2)
VECTOR_TEST("harness differential: pre-turn-tool-budget", 3)
VECTOR_TEST("harness differential: parallel-tool-budget", 4)
VECTOR_TEST("harness differential: turn-budget-zero", 5)
VECTOR_TEST("harness differential: unicode-whitespace-empty-final", 6)
VECTOR_TEST("harness differential: unicode-estimates-analysis", 7)
VECTOR_TEST("harness differential: invalid-json-arguments", 8)
VECTOR_TEST("harness differential: plan-required-final", 9)
VECTOR_TEST("harness differential: verification-policy-command-precedes-option", 10)
VECTOR_TEST("harness differential: transient-http408", 11)
VECTOR_TEST("harness differential: transient-http425", 12)
VECTOR_TEST("harness differential: transient-http429", 13)
VECTOR_TEST("harness differential: transient-http500", 14)
VECTOR_TEST("harness differential: thrown-transport", 15)
VECTOR_TEST("harness differential: invalid-json-body", 16)
VECTOR_TEST("harness differential: null-json-body", 17)
VECTOR_TEST("harness differential: invalid-normalized-body", 18)
VECTOR_TEST("harness differential: retry-exhaustion", 19)

ASYNC_TEST("harness cancellation differential: an already aborted run never dispatches") {
  auto isolation = isolate();
  rt::AbortController controller;
  controller.abort();
  auto calls = std::make_shared<int>(0);
  auto opts = baseOptions({});
  opts.signal = controller.signal();
  opts.transport = [calls](json::object) -> rt::Task<rt::Response> { ++*calls; throw rt::JsError("unreachable"); };
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "aborted");
  EXPECT_EQ(outcome.abortedReason.value_or(""), "signal");
  EXPECT_EQ(*calls, 0);
  EXPECT(outcome.events.empty());
}

ASYNC_TEST("harness cancellation differential: a stalled response body stops without cancellation") {
  auto isolation = isolate();
  rt::AbortController controller;
  auto cancelled = std::make_shared<bool>(false);
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.cancel = [cancelled](rt::Reason) { *cancelled = true; };
  rt::ByteStream body(source);
  rt::Response response{rt::BodyInit(body)};
  auto opts = baseOptions({});
  opts.signal = controller.signal();
  opts.transport = [response](json::object) { return rt::fulfilledTask(response); };
  rt::setTimeout([controller] { controller.abort(); }, 5);
  const double started = rt::dateNow();
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "aborted");
  EXPECT_EQ(outcome.abortedReason.value_or(""), "signal");
  EXPECT_EQ(outcome.events.size(), 1u);
  EXPECT_EQ(*cancelled, false);
  EXPECT(rt::dateNow() - started < 500);
}

ASYNC_TEST("harness cancellation differential: an abandoned transport late rejection stays observed") {
  auto isolation = isolate();
  rt::AbortController controller;
  auto provider = rt::withResolvers<rt::Response>();
  auto opts = baseOptions({});
  opts.signal = controller.signal();
  opts.transport = [pending = provider.promise](json::object) { return pending.wait(); };
  rt::setTimeout([reject = provider.reject] { reject(rt::makeReason(rt::JsError("late transport rejection"))); }, 10);
  rt::setTimeout([controller] { controller.abort(); }, 5);
  AWAIT(outcome, runReliabilityHarness(opts));
  co_await rt::sleep(20);
  EXPECT_EQ(outcome.phase, "aborted");
  EXPECT_EQ(outcome.abortedReason.value_or(""), "signal");
  EXPECT_EQ(outcome.events.size(), 1u);
  EXPECT(provider.promise.rejected());
}

ASYNC_TEST("harness cancellation differential: an abandoned body late rejection stays observed") {
  auto isolation = isolate();
  rt::AbortController controller;
  auto cancelled = std::make_shared<bool>(false);
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.start = [](rt::ReadableStreamDefaultController<rt::ByteChunk> streamController) {
    rt::setTimeout([streamController] { streamController.error(rt::makeReason(rt::JsError("late body rejection"))); }, 10);
  };
  source.cancel = [cancelled](rt::Reason) { *cancelled = true; };
  rt::ByteStream body(source);
  rt::Response response{rt::BodyInit(body)};
  auto opts = baseOptions({});
  opts.signal = controller.signal();
  opts.transport = [response](json::object) { return rt::fulfilledTask(response); };
  rt::setTimeout([controller] { controller.abort(); }, 5);
  AWAIT(outcome, runReliabilityHarness(opts));
  co_await rt::sleep(20);
  EXPECT_EQ(outcome.phase, "aborted");
  EXPECT_EQ(outcome.abortedReason.value_or(""), "signal");
  EXPECT_EQ(outcome.events.size(), 1u);
  EXPECT_EQ(*cancelled, false);
}

ASYNC_TEST("harness cancellation differential: a rejected HTTP body cancellation is swallowed") {
  auto isolation = isolate();
  auto cancelled = std::make_shared<int>(0);
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.cancel = [cancelled](rt::Reason) { ++*cancelled; throw rt::JsError("cancel rejected"); };
  rt::ByteStream body(source);
  rt::Response response(rt::BodyInit(body), {.status = 401});
  auto opts = baseOptions({});
  opts.transport = [response](json::object) { return rt::fulfilledTask(response); };
  opts.retryPolicy = retry::RetryPolicy{0, 0, {"transport"}};
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "failed");
  EXPECT_EQ(outcome.abortedReason.value_or(""), "transport_failed");
  EXPECT_EQ(*cancelled, 1);
  EXPECT_EQ(outcome.events.size(), 1u);
}

ASYNC_TEST("harness cancellation differential: cancellation waits for the active transport backoff") {
  auto isolation = isolate();
  rt::AbortController controller;
  auto calls = std::make_shared<int>(0);
  auto opts = baseOptions({});
  opts.signal = controller.signal();
  opts.retryPolicy = retry::RetryPolicy{1, 25, {"transport"}};
  opts.transport = [calls](json::object) -> rt::Task<rt::Response> {
    ++*calls;
    return rt::Promise<rt::Response>::reject(rt::makeReason(rt::JsError("offline"))).wait();
  };
  rt::setTimeout([controller] { controller.abort(); }, 5);
  const double started = rt::dateNow();
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "aborted");
  EXPECT_EQ(outcome.abortedReason.value_or(""), "signal");
  EXPECT_EQ(*calls, 1);
  EXPECT_EQ(rt::dateNow() - started, 25.0);
}

ASYNC_TEST("harness: a transient tool retry backs off before its call event and duration start") {
  auto isolation = isolate();
  auto opts = baseOptions({
    callStep("shell.exec", {{"command", "slow-cmd"}}), callStep("shell.exec", {{"command", "slow-cmd"}}),
    callStep("shell.exec", {{"command", "true"}}), json::object{{"content", "done"}},
  });
  opts.backends = createFakeToolBackends(FakeToolBackendsOptions{
    .files = files(), .shell = std::make_shared<FakeShell>(std::vector<FakeShellEntry>{
      {.command = std::string("slow-cmd"), .timed_out = true}, {.command = std::string("true"), .stdout = "ok"},
    }),
  });
  opts.retryPolicy = retry::RetryPolicy{1, 25, {"timeout", "internal", "unavailable", "transport"}};
  auto times = std::make_shared<std::vector<double>>();
  opts.emit = [times](const HarnessEvent& event) { if (std::holds_alternative<HarnessEventToolCall>(event)) times->push_back(rt::dateNow()); };
  AWAIT(outcome, runReliabilityHarness(opts));
  EXPECT_EQ(outcome.phase, "completed");
  EXPECT_EQ(times->size(), 3u);
  EXPECT_EQ(times->at(1) - times->at(0), 25.0);
  EXPECT_EQ(times->at(2), times->at(1));
  const auto results = events<HarnessEventToolResult>(outcome);
  EXPECT_EQ(results.at(1)->result.error_code.value_or(""), "timeout");
  EXPECT_EQ(results.at(1)->durationMs.value_or(-1), 0.0);
}
