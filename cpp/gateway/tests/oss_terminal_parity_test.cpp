// Links: tests/oss_terminal_parity_test.cpp
// src/inference_deadline.cpp src/auth/relay.cpp src/authority/native_codex.cpp
// src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp
// src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/bounded_response_body.cpp
// src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_read.cpp src/codex/account_routing.cpp
// src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp
// src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp
// src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/native_auth.cpp
// src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp
// src/codex/reset_settings.cpp src/codex/responses_error.cpp src/codex/routing_evaluation.cpp
// src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp
// src/config.cpp src/deepseek/index.cpp src/deepseek/responses.cpp
// src/defaults.cpp src/http.cpp src/kv.cpp
// src/metered_quota.cpp src/models/codex_models.cpp src/models/codex_models_availability.cpp
// src/models/codex_models_fetch.cpp src/openai_telemetry.cpp src/paid_fallback/ledger_stats.cpp
// src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp
// src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity.cpp
// src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp
// src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
// src/provider/cerebras.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp
// src/provider/claude_capacity_rollups.cpp src/provider/claude_code.cpp src/provider/claude_code_accounts.cpp
// src/provider/claude_code_credentials.cpp src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp
// src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/lithos.cpp
// src/provider/lithos_rate_limits.cpp src/provider/metered.cpp src/provider/removed_provider_telemetry.cpp
// src/provider/selection.cpp src/provider/surplus.cpp src/provider/surplus_claude_chat_request.cpp
// src/provider/surplus_claude_request.cpp src/release.cpp src/request_policy.cpp
// src/responses_failover_stream.cpp src/responses_stream.cpp src/runtime/websocket.cpp
// src/runtime_config.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/types.cpp
// src/utils.cpp src/cache/scope_targets.cpp src/deepseek/forwarded_payload_policy.cpp
// src/deepseek/responses_payload.cpp src/deepseek/chat_projection.cpp src/deepseek/responses_stream.cpp
// src/deepseek/flash_aliases.cpp src/provider/lithos_models.cpp src/lib/jev_compaction/state.cpp
// src/models/openrouter_models.cpp src/lib/jev_compaction/types.cpp
// src/deepseek/stream.cpp
// src/upstream_wire.cpp
// Deadline assertions preserved; adapter API parity below. Transport-specific assertions added separately.
#include "uos/inference_deadline.hpp"
#include "uos/upstream_wire.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"
#include <memory>
#include <set>

namespace {
  using namespace uos::inference_deadline;
  namespace rt = uos::rt;

  rt::Promise<void> awaitAbort(rt::AbortSignal signal) {
    return rt::newPromise<void>([signal](rt::Resolve<void> resolve, rt::Reject) {
      if (signal.aborted()) {
        resolve();
        return;
      }
      signal.addEventListener("abort", rt::EventListener([resolve] { resolve(); }), {.once = true});
    });
  }

  std::string reasonName(const rt::AbortSignal& signal) { return rt::errorName(signal.reason()).value_or("undefined"); }
}

ASYNC_TEST("m04 deadline: a queued wait spends the shared attempt budget instead of restarting it") {
  const rt::AbortController request;
  const StreamDeadline shared = createStreamFirstEventDeadline(request.signal(), 1'000);
  // Stand-in for queue waiting: the wait happens inside the caller's budget.
  co_await rt::sleep(150);
  const double remaining = shared.remainingMs();
  EXPECT(remaining > 0 && remaining < 1'000);

  const StreamDeadline attempt = createStreamSemanticDeadline(shared.signal, rt::Math::ceil(remaining));
  rt::Promise<void> attemptAborted = awaitAbort(attempt.signal);
  co_await attemptAborted.wait();
  EXPECT_EQ(reasonName(attempt.signal), "TimeoutError");
  // The attempt's own budget is the shared remainder, so both deadlines expire
  // together instead of the attempt outliving the caller's budget.
  rt::Promise<void> sharedAborted = awaitAbort(shared.signal);
  co_await sharedAborted.wait();
  EXPECT_EQ(shared.signal.aborted(), true);
  attempt.clear();
  shared.clear();
}

ASYNC_TEST("m04 deadline: a wait that outlives the budget leaves nothing to dispatch") {
  const StreamDeadline shared = createStreamFirstEventDeadline(rt::AbortController().signal(), 40);
  co_await rt::sleep(90);
  EXPECT_EQ(shared.signal.aborted(), true);
  EXPECT_EQ(shared.remainingMs(), 0);

  // Composing the remainder is what makes the already-spent budget visible to
  // the dispatch it guards; a fresh full timeout here would send a request the
  // caller has already abandoned.
  const StreamDeadline attempt = createStreamSemanticDeadline(shared.signal, rt::Math::ceil(shared.remainingMs()));
  rt::Promise<void> attemptAborted = awaitAbort(attempt.signal);
  co_await attemptAborted.wait();
  EXPECT_EQ(attempt.signal.aborted(), true);
  attempt.clear();
  shared.clear();
}

ASYNC_TEST("m04 deadline: client cancellation composes through every attempt deadline") {
  const rt::AbortController request;
  const StreamDeadline shared = createStreamFirstEventDeadline(request.signal(), 60'000);
  const StreamDeadline attempt = createStreamSemanticDeadline(shared.signal, 60'000);
  const StreamDeadline paid = createPaidProviderAttemptDeadline(shared.signal);
  const rt::Reason reason = rt::makeReason(rt::DOMException("client disconnected", "AbortError"));

  request.abort(reason);

  EXPECT(shared.signal.reason() == reason);
  EXPECT(attempt.signal.reason() == reason);
  EXPECT(paid.signal.reason() == reason);
  attempt.clear();
  paid.clear();
  shared.clear();
  co_return;
}

ASYNC_TEST("m04 deadline: an attempt-local abort does not cancel the shared budget") {
  const rt::AbortController request;
  const StreamDeadline shared = createStreamFirstEventDeadline(request.signal(), 60'000);
  const StreamDeadline attempt = createStreamSemanticDeadline(shared.signal, 60'000);
  const rt::Reason reason = rt::makeReason(rt::DOMException("attempt abandoned", "AbortError"));

  attempt.abort(reason);

  EXPECT_EQ(attempt.signal.aborted(), true);
  EXPECT(attempt.signal.reason() == reason);
  // A single failed attempt must leave the caller's budget usable for the
  // fallback attempt that composes its own remainder from `remainingMs()`.
  EXPECT_EQ(shared.signal.aborted(), false);
  EXPECT(shared.remainingMs() > 0);
  attempt.clear();
  shared.clear();
  co_return;
}

ASYNC_TEST("m04 deadline: aborting a deadline releases its timer") {
  const rt::SetTimeoutFunction originalSetTimeout = rt::globalSetTimeout();
  const rt::ClearTimeoutFunction originalClearTimeout = rt::globalClearTimeout();
  const auto armed = std::make_shared<std::set<double>>();
  // The deadline budget is deliberately short so a leaked timer cannot keep the
  // test process waiting on a 30-minute handle.
  const double budgetMs = 250;
  rt::globalSetTimeout() = [originalSetTimeout, armed](std::function<void()> handler, double timeout) {
    rt::Timeout id = originalSetTimeout(std::move(handler), timeout);
    armed->insert(id.id());
    return id;
  };
  rt::globalClearTimeout() = [originalClearTimeout, armed](const rt::Timeout& id) {
    if (id) armed->erase(id.id());
    originalClearTimeout(id);
  };
  {
    const auto restore = rt::scopeExit([&] {
      rt::globalSetTimeout() = originalSetTimeout;
      rt::globalClearTimeout() = originalClearTimeout;
    });
    const StreamDeadline deadline = createStreamSemanticDeadline(rt::AbortController().signal(), budgetMs);
    EXPECT_EQ(armed->size(), 1u);  // creating a deadline arms its budget timer
    deadline.abort(rt::makeReason(rt::DOMException("attempt abandoned", "AbortError")));
    EXPECT_EQ(deadline.signal.aborted(), true);
    // An abandoned attempt must not keep its timer armed; `clear()` is not
    // required for the abort path to release the handle.
    EXPECT_EQ(armed->size(), 0u);  // abort() must release the attempt deadline timer
  }
  co_return;
}

ASYNC_TEST("m04 deadline: abort still aborts after clear") {
  const StreamDeadline shared = createStreamFirstEventDeadline(rt::AbortController().signal(), 60'000);
  const StreamDeadline attempt = createStreamSemanticDeadline(shared.signal, 60'000);
  attempt.clear();
  EXPECT_EQ(attempt.signal.aborted(), false);

  // Clearing the timer must not remove the ability to abort a composed signal.
  const rt::Reason reason = rt::makeReason(rt::DOMException("attempt abandoned", "AbortError"));
  attempt.abort(reason);
  EXPECT_EQ(attempt.signal.aborted(), true);
  EXPECT(attempt.signal.reason() == reason);
  shared.clear();
  co_return;
}

#include "helpers/deepseek_port_replay.hpp"
namespace {
#include "oss_terminal_parity_vectors.inc"
 const bool recorded = uos::tests::helpers::deepseek_port_replay::registerReplay(recordedVectors);
}

#include "uos/deepseek/stream.hpp"
namespace {
  namespace ds = uos::deepseek::stream;
  namespace di = uos::deepseek::index;
  namespace rs = uos::deepseek::responses_stream;
  namespace rp = uos::deepseek::responses_payload;
  namespace json = uos::json;
  struct CredentialScope {
    rt::env::ScopedEnvironment environment{{{"HOME", "/nonexistent/w05c6-parity"}}};
    std::string previous = uos::config::config.codexAuthJsonB64;
    CredentialScope() { uos::config::config.codexAuthJsonB64.clear(); }
    ~CredentialScope() { uos::config::config.codexAuthJsonB64 = previous; }
  };
  json::object parityChunk(const json::object& delta, const json::value& finish = nullptr) {
    return {{"id", "chatcmpl-stream"}, {"object", "chat.completion.chunk"}, {"created", 1780000001}, {"model", "deepseek-flash"},
      {"choices", json::array{json::object{{"index", 0}, {"delta", delta}, {"finish_reason", finish}}}}};
  }
  json::object parityCompletion(const json::object& message) {
    return {{"id", "chatcmpl-1"}, {"object", "chat.completion"}, {"created", 1780000000}, {"model", "deepseek-flash"},
      {"choices", json::array{json::object{{"index", 0}, {"message", message}, {"finish_reason", "stop"}}}},
      {"usage", json::object{{"prompt_tokens", 10}, {"completion_tokens", 4}, {"total_tokens", 14}}}};
  }
  std::string paritySse(const json::object& value) { return "data: " + rt::jsonStringify(value) + "\n\n"; }
  rt::Task<json::array> readChatStream(std::string body) {
    rt::Response response(body, rt::ResponseInit{.headers = rt::Headers{{"Content-Type", "text/event-stream"}}});
    auto frames = ds::iterateDeepSeekChatCompletionStream(response, "deepseek-flash");
    json::array chunks;
    for (;;) {
      auto frame = co_await frames.next(); if (frame.done) break;
      if (const auto* chunk = std::get_if<ds::DeepSeekStreamFrameChunk>(&*frame.value)) chunks.push_back(chunk->value);
    }
    co_return chunks;
  }
  rt::Task<std::string> streamFailureKind(std::string body) {
    std::string kind;
    try { auto chunks = co_await readChatStream(body); static_cast<void>(chunks); } catch (const ds::DeepSeekStreamError& error) { kind = error.kind; }
    co_return kind;
  }
}
ASYNC_TEST("m04 parity: a normalized refusal delta reaches the streamed translator") {
  CredentialScope isolated;
  const std::string refusal = "I will not do that.";
  const auto body = paritySse(parityChunk(json::object{{"role", "assistant"}, {"refusal", refusal}})) + paritySse(parityChunk(json::object{}, "stop")) + "data: [DONE]\n\n";
  auto task = readChatStream(body); auto chunks = co_await std::move(task);
  const auto* choices = rt::getArray(chunks[0], "choices");
  EXPECT_EQ(rt::getString(*rt::getObject((*choices)[0], "delta"), "refusal"), std::optional<std::string_view>(refusal));
  const rp::DeepSeekResponsesEcho echo{nullptr, nullptr, true, std::nullopt};
  auto translator = rs::createDeepSeekResponsesStreamTranslator("deepseek-flash", "resp_refusal_norm", echo, 1780000000);
  std::vector<json::object> events;
  for (const auto& chunk : chunks) { auto added = translator.push(chunk.as_object()); events.insert(events.end(), added.begin(), added.end()); }
  auto terminalEvents = translator.finish(); events.insert(events.end(), terminalEvents.begin(), terminalEvents.end());
  const auto found = std::find_if(events.begin(), events.end(), [](const auto& event) { return rt::getString(event, "type") == "response.refusal.delta"; });
  EXPECT(found != events.end());
  EXPECT_EQ(rt::getString(*found, "item_id"), std::optional<std::string_view>("msg_resp_refusal_norm_0"));
  EXPECT_EQ(rt::getNumber(*found, "output_index"), std::optional(0.0));
  EXPECT_EQ(rt::getNumber(*found, "content_index"), std::optional(0.0));
  EXPECT_EQ(rt::getString(*found, "delta"), std::optional<std::string_view>(refusal));
  EXPECT_EQ(rt::getString(events.back(), "type"), std::optional<std::string_view>("response.completed"));
  const auto view = translator.answerBearingOutput();
  EXPECT_EQ(view.text, ""); EXPECT_EQ(view.refusal, refusal); EXPECT_EQ(view.toolCallCount, 0);
  EXPECT(uos::upstream_wire::isAnswerBearingCompletion({view.text, view.refusal, static_cast<double>(view.toolCallCount)}));
  const auto* output = rt::getArray(*rt::getObject(events.back(), "response"), "output");
  EXPECT_EQ(output->size(), 1u);
  EXPECT_EQ(rt::getString((*output)[0], "type"), std::optional<std::string_view>("message"));
  const json::array expected{json::object{{"type", "refusal"}, {"refusal", refusal}}};
  EXPECT(rt::deepEqual(*rt::get((*output)[0], "content"), expected));
}
ASYNC_TEST("m04 parity: a non-string refusal is rejected by the transport, not dropped") {
  CredentialScope isolated;
  const auto body = paritySse(parityChunk(json::object{{"role", "assistant"}, {"refusal", 42}})) + "data: [DONE]\n\n";
  auto task = streamFailureKind(body); auto kind = co_await std::move(task); EXPECT_EQ(kind, "invalid_chunk");
  const auto completion = parityCompletion(json::object{{"role", "assistant"}, {"content", "hi"}, {"refusal", 42}});
  const auto buffered = di::normalizeDeepSeekChatCompletion(completion, "deepseek-flash");
  EXPECT(std::holds_alternative<di::NormalizationResultNotOk>(buffered));
}
ASYNC_TEST("m04 parity: a null refusal is absence, not a malformed chunk") {
  CredentialScope isolated;
  const auto body = paritySse(parityChunk(json::object{{"role", "assistant"}, {"content", "hi"}, {"refusal", nullptr}})) + paritySse(parityChunk(json::object{}, "stop")) + "data: [DONE]\n\n";
  auto task = readChatStream(body); auto chunks = co_await std::move(task);
  const auto* choices = rt::getArray(chunks[0], "choices");
  const auto* delta = rt::getObject((*choices)[0], "delta"); EXPECT(!rt::hasOwn(*delta, "refusal"));
  const auto buffered = di::normalizeDeepSeekChatCompletion(parityCompletion(json::object{{"role", "assistant"}, {"content", "hi"}, {"refusal", nullptr}}), "deepseek-flash");
  EXPECT(std::holds_alternative<di::NormalizationResultOk<json::object>>(buffered));
  const auto& value = std::get<di::NormalizationResultOk<json::object>>(buffered).value;
  const auto* message = rt::getObject((*rt::getArray(value, "choices"))[0], "message");
  EXPECT(!rt::hasOwn(*message, "refusal")); EXPECT_EQ(rt::getString(*message, "content"), std::optional<std::string_view>("hi"));
}
ASYNC_TEST("m04 parity: malformed terminals and abrupt EOF cannot reach a completed terminal") {
  CredentialScope isolated;
  const auto chunk = parityChunk(json::object{{"role", "assistant"}, {"content", "partial"}});
  auto eofTask = streamFailureKind(paritySse(chunk)); auto eof = co_await std::move(eofTask); EXPECT_EQ(eof, "premature_eof");
  const auto malformed = paritySse(parityChunk(json::object{{"content", "x"}}, 42)) + "data: [DONE]\n\n";
  auto invalidTask = streamFailureKind(malformed); auto invalid = co_await std::move(invalidTask); EXPECT_EQ(invalid, "invalid_chunk");
  const std::string malformedJson = "data: {not json}\n\n";
  auto jsonTask = streamFailureKind(malformedJson); auto badJson = co_await std::move(jsonTask); EXPECT_EQ(badJson, "malformed_event");
  const rp::DeepSeekResponsesEcho echo{nullptr, nullptr, true, std::nullopt};
  auto translator = rs::createDeepSeekResponsesStreamTranslator("deepseek-flash", "resp_unknown", echo, 1780000000);
  translator.push(parityChunk(json::object{{"role", "assistant"}, {"content", "partial"}}, "something_new"));
  const auto events = translator.finish();
  EXPECT_EQ(rt::getString(events.back(), "type"), std::optional<std::string_view>("response.failed"));
  const auto* response = rt::getObject(events.back(), "response");
  EXPECT_EQ(rt::getString(*response, "status"), std::optional<std::string_view>("failed"));
  const json::object error{{"code", "unrecognized_finish_reason:something_new"}, {"message", "DeepSeek stopped generating: unrecognized_finish_reason:something_new"}};
  EXPECT(rt::deepEqual(*rt::get(*response, "error"), error));
  EXPECT(rt::get(*response, "incomplete_details")->is_null());
}

TEST("m04 parity: refusal and reasoning-only adapter views use the shared answer-bearing predicate") {
  CredentialScope isolated;
  const rp::DeepSeekResponsesEcho echo{nullptr, nullptr, true, std::nullopt};
  auto refusal = rs::createDeepSeekResponsesStreamTranslator("deepseek-flash", "resp_shared_refusal", echo, 1780000000);
  refusal.push(parityChunk(json::object{{"role", "assistant"}, {"refusal", "No."}}));
  refusal.push(parityChunk(json::object{}, "stop"));
  auto view = refusal.answerBearingOutput();
  EXPECT(uos::upstream_wire::isAnswerBearingCompletion({view.text, view.refusal, static_cast<double>(view.toolCallCount)}));
  auto reasoning = rs::createDeepSeekResponsesStreamTranslator("deepseek-flash", "resp_shared_reasoning", echo, 1780000000);
  reasoning.push(parityChunk(json::object{{"role", "assistant"}, {"reasoning_content", "thinking only"}}));
  reasoning.push(parityChunk(json::object{}, "stop"));
  view = reasoning.answerBearingOutput();
  EXPECT(!uos::upstream_wire::isAnswerBearingCompletion({view.text, view.refusal, static_cast<double>(view.toolCallCount)}));
  const auto buffered = di::normalizeDeepSeekChatCompletion(parityCompletion(json::object{{"role", "assistant"}, {"reasoning_content", "thinking only"}, {"content", ""}}), "deepseek-flash");
  ASSERT(std::holds_alternative<di::NormalizationResultOk<json::object>>(buffered));
  EXPECT(!uos::upstream_wire::chatCompletionHasAnswerBearingOutput(std::get<di::NormalizationResultOk<json::object>>(buffered).value));
}
