// Port of tests/responses-failover-branch-coverage.test.ts.
// Branch-coverage tests for src/responses-failover-stream.ts.
//
// The module is a pure event classifier plus one owned ReadableStream. These
// tests drive the classifier with each provider event shape and then drive the
// stream through success, premature EOF, empty completion, cancellation and the
// RemovedProvider warning preamble.
// Links: src/responses_failover_stream.cpp src/codex/responses_error.cpp src/responses_stream.cpp src/utils.cpp
//        src/inference_deadline.cpp
#include "uos/responses_failover_stream.hpp"

#include "uos/responses_stream.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::responses_failover_stream;
using uos::responses_stream::responsesStreamErrorOf;
using uos::responses_stream::ResponsesStreamError;
using uos::responses_stream::ResponsesStreamEvent;
using uos::responses_stream::ResponsesStreamIterator;

namespace {
  json::object obj(std::string_view text) { return rt::jsonParse(text).as_object(); }
  json::value val(std::string_view text) { return rt::jsonParse(text); }

  ResponsesStreamEvent event(std::string_view value) { return responseEventFromValue(obj(value)); }

  // The iterator contract is an async generator, so the fixture drains through an
  // asynchronous source exactly like the upstream reader does.
  ResponsesStreamIterator iterator(std::vector<ResponsesStreamEvent> events) {
    return ResponsesStreamIterator([events](rt::Yield<ResponsesStreamEvent> yield) -> rt::Task<void> {
      rt::UnderlyingSource<ResponsesStreamEvent> source;
      source.start = [events](rt::ReadableStreamDefaultController<ResponsesStreamEvent> controller) {
        for (const ResponsesStreamEvent& item : events) controller.enqueue(item);
        controller.close();
      };
      const rt::ReadableStream<ResponsesStreamEvent> stream(std::move(source));
      const rt::AsyncGenerator<ResponsesStreamEvent> values = stream.values();
      co_await yield.from(values);
    });
  }

  /** An iterator whose first read rejects (its return() then resolves done). */
  ResponsesStreamIterator failingIterator(rt::Reason error) {
    return ResponsesStreamIterator([error](rt::Yield<ResponsesStreamEvent>) -> rt::Task<void> {
      std::rethrow_exception(error);
      co_return;
    });
  }

  rt::Task<std::string> textOf(rt::ByteStream stream) {
    const auto reader = stream.getReader();
    std::string text;
    rt::TextDecoder decoder;
    for (;;) {
      const rt::ReadableStreamReadResult<rt::ByteChunk> chunk = co_await reader.read();
      if (chunk.done) co_return text;
      text += decoder.decode(*chunk.value, {.stream = true});
    }
  }

  /**
   * The failure callbacks run after the stream releases its iterator, so a test
   * that inspects them drains the pending promise jobs first. This is deterministic:
   * no timers and no wall-clock input.
   */
  rt::Task<void> flushMicrotasks() {
    for (int turn = 0; turn < 50; turn += 1) co_await rt::microtask();
  }

  std::vector<json::object> sseData(const std::string& text) {
    static const rt::Regex DATA("^data: (.+)$", "gm");
    std::vector<json::object> values;
    for (const rt::RegexMatch& match : rt::matchAll(text, DATA)) values.push_back(rt::jsonParse(*match[1]).as_object());
    return values;
  }

  std::optional<std::string> stringAt(const json::object& value, std::string_view key) {
    const std::optional<std::string_view> found = rt::getString(value, key);
    return found ? std::optional<std::string>(std::string(*found)) : std::nullopt;
  }

  std::vector<std::string> typesOf(const std::vector<json::object>& values) {
    std::vector<std::string> types;
    for (const json::object& value : values) types.push_back(stringAt(value, "type").value_or(""));
    return types;
  }

  bool sameObject(const ResponsesStreamEvent& left, const ResponsesStreamEvent& right) { return left.identity() == right.identity(); }
  bool sameObject(const std::optional<ResponsesStreamEvent>& left, const ResponsesStreamEvent& right) { return left && sameObject(*left, right); }
  bool sameContent(const ResponsesStreamEvent& left, const ResponsesStreamEvent& right) {
    return left.raw == right.raw && left.type == right.type && left.terminal == right.terminal && rt::deepEqual(json::value(left.value), json::value(right.value));
  }

  bool isKind(const rt::Reason& error, std::string_view kind) {
    const ResponsesStreamError* streamError = responsesStreamErrorOf(error);
    return streamError && streamError->kind == kind;
  }

  template <class F> bool throwsKind(F run, std::string_view kind) {
    try {
      run();
    } catch (...) {
      return isKind(std::current_exception(), kind);
    }
    return false;
  }

  const std::optional<std::string> TEXT = "text";
  const std::optional<std::string> TOOL_CALL = "tool_call";

  struct FailureRecord {
    std::string kind;
    std::optional<std::string> terminal;
    bool operator==(const FailureRecord&) const = default;
  };
}

TEST("semantic classifier rejects incomplete executable tool call items") {
  const std::vector<ResponsesStreamEvent> kinds{
    event(R"({"type":"response.output_item.done","item":{"type":"function_call","name":"lookup","arguments":"{}"}})"),
    event(R"({"type":"response.output_item.done","item":{"type":"function_call","call_id":"call_1","arguments":"{}"}})"),
    event(R"({"type":"response.output_item.done","item":{"type":"function_call","call_id":"call_1","name":"  ","arguments":"{}"}})"),
    event(R"({"type":"response.output_item.done","item":{"type":"function_call","call_id":"call_1","name":"lookup","arguments":null}})"),
    event(R"({"type":"response.output_item.done","item":{"type":"custom_tool_call","call_id":"call_2","name":"shell","input":null}})"),
    event(R"({"type":"response.output_item.done","item":{"type":"not_a_tool","call_id":"call_3","name":"lookup"}})"),
  };
  for (const ResponsesStreamEvent& value : kinds) EXPECT_EQ(responsesEventSemanticKind(value), std::nullopt);

  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.output_item.done","item":{"type":"custom_tool_call","call_id":"call_2","name":"shell","input":"ls"}})")),
            TOOL_CALL);
  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.output_item.done","item":{"type":"function_call","call_id":"call_1","name":"lookup","arguments":""}})")),
            TOOL_CALL);
}

TEST("semantic classifier accepts hosted tool lifecycles and ignores other statuses") {
  for (const std::string status : {"in_progress", "completed", "failed"}) {
    json::object item = obj(R"({"type":"web_search_call"})");
    item["status"] = status;
    json::object value = obj(R"({"type":"response.output_item.done"})");
    value["item"] = item;
    EXPECT_EQ(responsesEventSemanticKind(responseEventFromValue(value)), TOOL_CALL);
  }
  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.output_item.done","item":{"type":"web_search_call"}})")), std::nullopt);
  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.output_item.done","item":{"type":"web_search_call","status":"queued"}})")), std::nullopt);
  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.web_search_call.completed"})")), TOOL_CALL);
  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.code_interpreter_call.failed"})")), TOOL_CALL);
  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.web_search_call.searching"})")), std::nullopt);
}

TEST("semantic classifier reads output payloads, content parts and image partials") {
  const auto payload = [](std::string_view output) {
    json::object response = obj(R"({"id":"resp_1"})");
    response["output"] = val(output);
    json::object value = obj(R"({"type":"response.completed"})");
    value["response"] = response;
    return responseEventFromValue(value);
  };
  EXPECT_EQ(responsesEventSemanticKind(payload(R"([{"type":"message","content":[{"type":"output_text","text":"hello"}]}])")), TEXT);
  EXPECT_EQ(responsesEventSemanticKind(payload(R"([{"type":"message","content":[{"type":"output_text","text":""}]}])")), std::nullopt);
  EXPECT_EQ(responsesEventSemanticKind(payload(R"([{"type":"message","content":"not-an-array"}])")), std::nullopt);
  EXPECT_EQ(responsesEventSemanticKind(payload(R"([{"type":"reasoning","content":[{"type":"output_text","text":"hidden"}]}])")), std::nullopt);
  EXPECT_EQ(responsesEventSemanticKind(payload(R"([7,{"type":"message","content":[{"type":"refusal","refusal":"no"}]}])")), TEXT);
  EXPECT_EQ(responsesEventSemanticKind(payload(R"("not-an-array")")), std::nullopt);

  // The compatibility `output` field on the event itself is the second source.
  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.output","output":[{"type":"message","content":[{"type":"output_text","text":"hi"}]}]})")), TEXT);

  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.content_part.done","part":{"type":"output_text","text":"part"}})")), TEXT);
  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.content_part.done","part":{"type":"refusal","refusal":"stop"}})")), TEXT);
  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.content_part.done","part":{"type":"output_text","text":""}})")), std::nullopt);
  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.content_part.done","part":7})")), std::nullopt);
  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.refusal.done","refusal":"declined"})")), TEXT);
  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.refusal.delta","refusal":""})")), std::nullopt);

  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.image_generation_call.partial_image","partial_image_b64":"AAAA"})")), TOOL_CALL);
  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.image_generation_call.partial_image","result":"done"})")), TOOL_CALL);
  EXPECT_EQ(responsesEventSemanticKind(event(R"({"type":"response.image_generation_call.partial_image"})")), std::nullopt);
}

TEST("progress detector reports reasoning work and ignores unusable identifiers") {
  EXPECT_EQ(responsesEventReportsProgress(event(R"({"type":"response.reasoning_summary_part.added","summary_index":2})")), true);
  EXPECT_EQ(responsesEventReportsProgress(event(R"({"type":"response.reasoning_summary_part.added"})")), false);
  EXPECT_EQ(responsesEventReportsProgress(event(R"({"type":"response.reasoning_summary_text.delta","delta":"x","summary_index":"0"})")), false);
  EXPECT_EQ(responsesEventReportsProgress(event(R"({"type":"response.reasoning_text.delta","delta":"x","content_index":1})")), true);
  EXPECT_EQ(responsesEventReportsProgress(event(R"({"type":"response.output_item.added","item":{"id":"rs_1","type":"reasoning","summary":[]}})")), true);
  EXPECT_EQ(responsesEventReportsProgress(event(R"({"type":"response.output_item.added","item":{"id":"rs_1","type":"reasoning"}})")), false);
  EXPECT_EQ(responsesEventReportsProgress(event(R"({"type":"response.output_item.done","item":{"type":"reasoning","content":[]}})")), false);
  EXPECT_EQ(responsesEventReportsProgress(event(R"({"type":"response.output_item.done","item":"not-a-record"})")), false);
  EXPECT_EQ(responsesEventReportsProgress(event(R"({"type":"response.output_text.delta","delta":"visible"})")), false);
}

TEST("precommit buffering enforces its event and character budgets") {
  std::vector<ResponsesStreamEvent> buffered;
  const ResponsesStreamEvent single = event(R"({"type":"response.output_text.delta","delta":"x"})");
  EXPECT_EQ(appendResponsesPrecommitEvent(buffered, single, 0), static_cast<double>(rt::length(single.raw)));

  json::object oversizedValue = obj(R"({"type":"response.output_text.delta"})");
  oversizedValue["delta"] = std::string(static_cast<std::size_t>(MAX_RESPONSES_PRECOMMIT_CHARS) + 1, 'x');
  const ResponsesStreamEvent oversized = responseEventFromValue(std::move(oversizedValue));
  EXPECT(throwsKind(
    [&] {
      std::vector<ResponsesStreamEvent> none;
      appendResponsesPrecommitEvent(none, oversized, 0);
    },
    "event_too_large"));

  std::vector<ResponsesStreamEvent> many(static_cast<std::size_t>(MAX_RESPONSES_PRECOMMIT_EVENTS), single);
  EXPECT(throwsKind([&] { appendResponsesPrecommitEvent(many, single, 0); }, "event_too_large"));
}

ASYNC_TEST("precommit preparation stops at semantic output, terminal events and premature EOF") {
  const ResponsesStreamEvent semantic = event(R"({"type":"response.output_text.delta","delta":"hello"})");
  const ResponsesStreamIterator first = iterator({event(R"({"type":"response.created"})"), semantic});
  const PreparedResponsesStream prepared = co_await prepareResponsesStreamForCommit(first);
  EXPECT(sameObject(prepared.semantic, semantic));
  EXPECT_EQ(prepared.semanticKind, TEXT);
  EXPECT(!prepared.terminal);
  EXPECT_EQ(prepared.buffered.size(), std::size_t{2});

  const ResponsesStreamEvent terminal = event(R"({"type":"response.failed","sequence_number":1})");
  const ResponsesStreamIterator second = iterator({terminal});
  const PreparedResponsesStream terminalPrepared = co_await prepareResponsesStreamForCommit(second);
  EXPECT(!terminalPrepared.semantic);
  EXPECT(sameObject(terminalPrepared.terminal, terminal));

  auto progressEvents = std::make_shared<std::vector<ResponsesStreamEvent>>();
  PrepareResponsesStreamForCommitOptions releaseOptions;
  releaseOptions.onProgress = [progressEvents](const ResponsesStreamEvent& progressEvent) { progressEvents->push_back(progressEvent); };
  releaseOptions.releaseOnProgress = true;
  const ResponsesStreamIterator third = iterator({event(R"({"type":"response.reasoning_summary_text.delta","delta":"x","summary_index":0})")});
  const PreparedResponsesStream released = co_await prepareResponsesStreamForCommit(third, std::move(releaseOptions));
  EXPECT(!released.semantic);
  EXPECT(!released.terminal);
  EXPECT_EQ(progressEvents->size(), std::size_t{1});

  // Lifecycle-keyed progress is reported once; keyless progress is reported per event.
  auto progressCount = std::make_shared<int>(0);
  const ResponsesStreamEvent repeatedPart = event(R"({"type":"response.reasoning_summary_part.added","summary_index":0})");
  PrepareResponsesStreamForCommitOptions heldOptions;
  heldOptions.onProgress = [progressCount](const ResponsesStreamEvent&) { *progressCount += 1; };
  const ResponsesStreamIterator fourth = iterator({repeatedPart, repeatedPart, terminal});
  const PreparedResponsesStream held = co_await prepareResponsesStreamForCommit(fourth, std::move(heldOptions));
  EXPECT_EQ(*progressCount, 1);
  EXPECT(sameObject(held.terminal, terminal));

  auto keylessCount = std::make_shared<int>(0);
  const ResponsesStreamEvent repeatedDelta = event(R"({"type":"response.reasoning_summary_text.delta","delta":"y","summary_index":0})");
  PrepareResponsesStreamForCommitOptions keylessOptions;
  keylessOptions.onProgress = [keylessCount](const ResponsesStreamEvent&) { *keylessCount += 1; };
  const ResponsesStreamIterator fifth = iterator({repeatedDelta, repeatedDelta, terminal});
  co_await prepareResponsesStreamForCommit(fifth, std::move(keylessOptions));
  EXPECT_EQ(*keylessCount, 2);

  bool prematureEof = false;
  try {
    const ResponsesStreamIterator empty = iterator({});
    co_await prepareResponsesStreamForCommit(empty);
  } catch (...) {
    prematureEof = isKind(std::current_exception(), "premature_eof");
  }
  EXPECT(prematureEof);

  // A failing iterator is released and the original error surfaces.
  const rt::Reason failure = rt::makeReason(rt::JsError("upstream read failed"));
  const ResponsesStreamIterator failing = failingIterator(failure);
  EXPECT_REJECTS_REASON(prepareResponsesStreamForCommit(failing), failure);
}

TEST("warning helpers identify exactly the injected assistant notice") {
  const FailoverWarningEvents warning = buildFailoverWarningEvents("gpt-5.6-sol", "resp_1", 5);
  const json::object& item = warning.item;
  EXPECT_EQ(isGatewayFailoverWarningItem(json::value(item)), true);
  EXPECT_EQ(warning.events.size(), std::size_t{6});
  EXPECT_EQ(rt::getNumber(warning.events[0].value, "sequence_number"), std::optional<double>(5));
  EXPECT_EQ(rt::getNumber(warning.events.back().value, "sequence_number"), std::optional<double>(10));
  EXPECT_EQ(isSyntheticResponsesFailureEvent(warning.events[0]), false);

  const auto with = [&item](std::string_view key, json::value value) {
    json::object copy = rt::objectSpread(item);
    rt::objectDefine(copy, key, std::move(value));
    return json::value(std::move(copy));
  };
  EXPECT_EQ(isGatewayFailoverWarningItem(json::value(nullptr)), false);
  EXPECT_EQ(isGatewayFailoverWarningItem(json::value(json::array{item})), false);
  EXPECT_EQ(isGatewayFailoverWarningItem(with("type", json::string("function_call"))), false);
  EXPECT_EQ(isGatewayFailoverWarningItem(with("role", json::string("user"))), false);
  EXPECT_EQ(isGatewayFailoverWarningItem(with("id", json::string("msg_other"))), false);
  EXPECT_EQ(isGatewayFailoverWarningItem(with("content", json::array())), false);
  EXPECT_EQ(isGatewayFailoverWarningItem(with("content", val(R"([{"type":"output_text","text":"x"},{"type":"output_text","text":"y"}])"))), false);
  json::array inputText{json::object{{"type", "input_text"}, {"text", failoverWarningText("m")}}};
  EXPECT_EQ(isGatewayFailoverWarningItem(with("content", inputText)), false);
  EXPECT_EQ(isGatewayFailoverWarningItem(with("content", val(R"([{"type":"output_text","text":"not the notice"}])"))), false);
  json::array truncated{json::object{{"type", "output_text"}, {"text", rt::slice(failoverWarningText("m"), 0, -1)}}};
  EXPECT_EQ(isGatewayFailoverWarningItem(with("content", truncated)), false);
}

TEST("warning rewrites increment output indexes and prefix terminal outputs") {
  const json::object item = buildFailoverWarningEvents("gpt-5.6-sol", "resp_1").item;
  const ResponsesStreamEvent delta = event(R"({"type":"response.output_text.delta","output_index":2,"delta":"x"})");
  const ResponsesStreamEvent rewritten = rewriteResponsesEventForWarning(delta, item, 7);
  EXPECT_EQ(rt::getNumber(rewritten.value, "sequence_number"), std::optional<double>(7));
  EXPECT_EQ(rt::getNumber(rewritten.value, "output_index"), std::optional<double>(3));
  EXPECT_EQ(stringAt(rewritten.value, "delta"), std::optional<std::string>("x"));

  const ResponsesStreamEvent nonNumericIndex = rewriteResponsesEventForWarning(event(R"({"type":"response.output_text.delta","output_index":"2","delta":"x"})"), item, 8);
  EXPECT_EQ(stringAt(nonNumericIndex.value, "output_index"), std::optional<std::string>("2"));

  const ResponsesStreamEvent completed = rewriteResponsesEventForWarning(event(R"({"type":"response.completed","response":{"id":"resp_1","output":[{"id":"msg_1"}]}})"), item, 9);
  const json::array& output = *rt::getArray(rt::get(completed.value, "response")->as_object(), "output");
  EXPECT_EQ(output.size(), std::size_t{2});
  EXPECT(rt::deepEqual(output[0], json::value(item)));
  EXPECT(rt::deepEqual(output[1], val(R"({"id":"msg_1"})")));

  const ResponsesStreamEvent noOutput = rewriteResponsesEventForWarning(event(R"({"type":"response.completed","response":{"id":"resp_1","output":"nope"}})"), item, 10);
  EXPECT_EQ(rt::getArray(rt::get(noOutput.value, "response")->as_object(), "output")->size(), std::size_t{1});

  const ResponsesStreamEvent notTerminal = rewriteResponsesEventForWarning(event(R"({"type":"response.in_progress","response":{"id":"resp_1"}})"), item, 11);
  EXPECT(rt::deepEqual(*rt::get(notTerminal.value, "response"), val(R"({"id":"resp_1"})")));

  const ResponsesStreamEvent nonRecordResponse = rewriteResponsesEventForWarning(event(R"({"type":"response.completed","response":"nope"})"), item, 12);
  EXPECT_EQ(stringAt(nonRecordResponse.value, "response"), std::optional<std::string>("nope"));

  EXPECT_EQ(rt::getNumber(rewriteResponsesEventSequence(delta, 3).value, "sequence_number"), std::optional<double>(3));
}

TEST("event builders reject unusable payloads and tag synthetic failures") {
  EXPECT(throwsKind([] { responseEventFromValue(obj(R"({"type":"  "})")); }, "malformed_event"));
  EXPECT(throwsKind([] { responseEventFromValue(json::object()); }, "malformed_event"));

  const ResponsesStreamEvent synthetic = failureEventAfterCommit("resp_1", 4, {obj(R"({"id":"msg_1"})")}, obj(R"({"model":"gpt-5.6-sol"})"));
  EXPECT_EQ(isSyntheticResponsesFailureEvent(synthetic), true);
  EXPECT_EQ(synthetic.type, std::string("response.failed"));
  EXPECT_EQ(synthetic.terminal, true);
  EXPECT(rt::deepEqual(
    *rt::get(synthetic.value, "response"),
    val(R"({"model":"gpt-5.6-sol","id":"resp_1","object":"response","status":"failed","error":{"code":"server_error","message":"The upstream stream ended unexpectedly."},"output":[{"id":"msg_1"}]})")));

  const ResponsesStreamEvent withObject = failureEventAfterCommit("resp_2", 5, {}, obj(R"({"object":"chat.completion"})"));
  EXPECT_EQ(stringAt(rt::get(withObject.value, "response")->as_object(), "object"), std::optional<std::string>("chat.completion"));
}

TEST("response identifiers are read from both places and conflicts are rejected") {
  EXPECT_EQ(responseIdFromEvents({}), std::nullopt);
  EXPECT_EQ(responseIdFromEvents({event(R"({"type":"response.created","response_id":" resp_1 "})")}), std::optional<std::string>("resp_1"));
  EXPECT_EQ(responseIdFromEvents({event(R"({"type":"response.created","response":{"id":"resp_1"}})")}), std::optional<std::string>("resp_1"));
  EXPECT_EQ(responseIdFromEvents({event(R"({"type":"response.created"})")}), std::nullopt);
  EXPECT_EQ(responseIdFromEvents({event(R"({"type":"response.created","response_id":"resp_1"})"), event(R"({"type":"response.completed","response_id":"resp_1"})")}),
            std::optional<std::string>("resp_1"));
  EXPECT(throwsKind(
    [] { responseIdFromEvents({event(R"({"type":"response.created","response_id":"resp_1"})"), event(R"({"type":"response.completed","response_id":"resp_2"})")}); },
    "malformed_event"));
}

ASYNC_TEST("owned stream forwards initial and upstream events verbatim and closes at its terminal") {
  const ResponsesStreamEvent created = event(R"({"type":"response.created","response_id":"resp_1","sequence_number":4})");
  const ResponsesStreamEvent queued = event(R"({"type":"response.in_progress","response_id":"resp_1"})");
  const ResponsesStreamEvent delta = event(R"({"type":"response.output_text.delta","delta":"hi","response_id":"resp_1","sequence_number":5})");
  const ResponsesStreamEvent completed = event(R"({"type":"response.completed","response_id":"resp_1","sequence_number":6})");
  auto delivered = std::make_shared<std::vector<ResponsesStreamEvent>>();
  OwnedResponsesStreamOptions options{.initial = {created, queued}, .iterator = iterator({delta, completed}), .responseId = std::nullopt};
  options.onEvent = [delivered](ResponsesStreamEvent deliveredEvent) { delivered->push_back(std::move(deliveredEvent)); };
  const rt::ByteStream stream = createOwnedResponsesStream(std::move(options));

  const std::string text = co_await textOf(stream);
  const std::vector<json::object> values = sseData(text);
  EXPECT_EQ(typesOf(values), (std::vector<std::string>{"response.created", "response.in_progress", "response.output_text.delta", "response.completed"}));
  EXPECT_EQ(rt::getNumber(values.at(3), "sequence_number"), std::optional<double>(6));
  ASSERT_EQ(delivered->size(), std::size_t{4});
  EXPECT(sameContent((*delivered)[0], created));
  EXPECT(sameContent((*delivered)[1], queued));
  EXPECT(sameContent((*delivered)[2], delta));
  EXPECT(sameContent((*delivered)[3], completed));
}

ASYNC_TEST("owned stream replaces a premature EOF with a terminal event") {
  auto observed = std::make_shared<std::vector<ResponsesStreamEvent>>();
  auto failures = std::make_shared<std::vector<FailureRecord>>();
  OwnedResponsesStreamOptions beforeOptions{.initial = {}, .iterator = iterator({event(R"({"type":"response.created","response_id":"resp_1"})")}), .responseId = std::nullopt};
  beforeOptions.onEvent = [observed](ResponsesStreamEvent deliveredEvent) { observed->push_back(std::move(deliveredEvent)); };
  beforeOptions.onFailure = [failures](rt::Reason, OwnedResponsesStreamFailureDetails details) {
    failures->push_back(FailureRecord{details.failureKind, details.syntheticTerminalType});
  };
  const rt::ByteStream beforeCommit = createOwnedResponsesStream(std::move(beforeOptions));
  const std::string beforeText = co_await textOf(beforeCommit);
  const std::vector<json::object> values = sseData(beforeText);
  EXPECT_EQ(typesOf(values), (std::vector<std::string>{"response.created", "error"}));
  EXPECT_EQ(stringAt(values.at(1), "code"), std::optional<std::string>("server_error"));
  EXPECT_EQ(*failures, (std::vector<FailureRecord>{FailureRecord{"premature_eof", "error"}}));
  EXPECT_EQ(observed->size(), std::size_t{2});

  // After semantic output the failure is a response.failed carrying what was seen.
  const rt::ByteStream afterCommit = createOwnedResponsesStream(OwnedResponsesStreamOptions{
    .initial = {event(R"({"type":"response.output_text.delta","delta":"partial text","response_id":"resp_9","output_index":0})")},
    .iterator = iterator({}),
    .responseId = std::nullopt,
  });
  const std::string afterText = co_await textOf(afterCommit);
  const std::vector<json::object> committed = sseData(afterText);
  EXPECT_EQ(typesOf(committed), (std::vector<std::string>{"response.output_text.delta", "response.failed"}));
  const json::object& failure = rt::get(committed.at(1), "response")->as_object();
  EXPECT_EQ(stringAt(failure, "id"), std::optional<std::string>("resp_9"));
  EXPECT_EQ(stringAt(failure, "status"), std::optional<std::string>("failed"));
  const json::array& output = *rt::getArray(failure, "output");
  EXPECT_EQ(output.size(), std::size_t{1});
  EXPECT_EQ(stringAt(output.at(0).as_object(), "id"), std::optional<std::string>("msg_recovered_0"));
  EXPECT_EQ(stringAt(rt::getArray(output.at(0).as_object(), "content")->at(0).as_object(), "text"), std::optional<std::string>("partial text"));
}

ASYNC_TEST("owned stream fails an empty upstream completion instead of forwarding it") {
  auto failures = std::make_shared<std::vector<std::string>>();
  OwnedResponsesStreamOptions options{
    .initial = {event(R"({"type":"response.created","response_id":"resp_2"})")},
    .iterator = iterator({event(R"({"type":"response.completed","response_id":"resp_2","response":{"id":"resp_2","output":[]}})")}),
    .responseId = "resp_2",
  };
  options.onFailure = [failures](rt::Reason, OwnedResponsesStreamFailureDetails details) { failures->push_back(details.failureKind); };
  const rt::ByteStream stream = createOwnedResponsesStream(std::move(options));
  const std::string text = co_await textOf(stream);
  const std::vector<json::object> values = sseData(text);
  co_await flushMicrotasks();
  EXPECT_EQ(typesOf(values), (std::vector<std::string>{"response.created", "error"}));
  EXPECT_EQ(stringAt(values.at(1), "code"), std::optional<std::string>("empty_upstream_completion"));
  EXPECT_EQ(*failures, (std::vector<std::string>{"empty_upstream_completion"}));
}

ASYNC_TEST("owned stream cancellation aborts upstream once and a cancelled read closes quietly") {
  auto abortedWith = std::make_shared<std::optional<std::string>>();
  auto cancelledWith = std::make_shared<std::optional<std::string>>();
  OwnedResponsesStreamOptions cancellableOptions{
    .initial = {event(R"({"type":"response.created","response_id":"resp_3"})")},
    .iterator = iterator({event(R"({"type":"response.output_text.delta","delta":"late","response_id":"resp_3"})")}),
    .responseId = "resp_3",
  };
  cancellableOptions.abortUpstream = [abortedWith](rt::Reason reason) { *abortedWith = rt::reasonToString(reason); };
  cancellableOptions.onCancel = [cancelledWith](rt::Reason reason) { *cancelledWith = rt::reasonToString(reason); };
  const rt::ByteStream cancellable = createOwnedResponsesStream(std::move(cancellableOptions));
  const rt::Reason clientLeft = rt::thrown(json::value("client left"));
  co_await cancellable.cancel(clientLeft);
  EXPECT_EQ(*abortedWith, std::optional<std::string>("client left"));
  EXPECT_EQ(*cancelledWith, std::optional<std::string>("client left"));
  const rt::Reason again = rt::thrown(json::value("again"));
  co_await cancellable.cancel(again);
  EXPECT_EQ(*cancelledWith, std::optional<std::string>("client left"));

  const rt::AbortController controller;
  controller.abort(rt::makeReason(rt::JsError("caller gone")));
  auto failures = std::make_shared<std::vector<FailureRecord>>();
  OwnedResponsesStreamOptions abortedOptions{
    .initial = {},
    .iterator = failingIterator(rt::makeReason(rt::JsError("read failed"))),
    .responseId = "resp_4",
    .signal = controller.signal(),
  };
  abortedOptions.onFailure = [failures](rt::Reason, OwnedResponsesStreamFailureDetails details) {
    failures->push_back(FailureRecord{details.failureKind, details.syntheticTerminalType});
  };
  const rt::ByteStream aborted = createOwnedResponsesStream(std::move(abortedOptions));
  const std::string abortedText = co_await textOf(aborted);
  EXPECT_EQ(abortedText, std::string(""));
  co_await flushMicrotasks();
  EXPECT_EQ(*failures, (std::vector<FailureRecord>{FailureRecord{"read_error", std::nullopt}}));
}

ASYNC_TEST("owned stream survives observers that throw, sync or async") {
  OwnedResponsesStreamOptions options{.initial = {}, .iterator = iterator({}), .responseId = std::nullopt};
  options.onEvent = [](ResponsesStreamEvent) { throw rt::JsError("synchronous observer failed"); };
  options.onFailure = [](rt::Reason, OwnedResponsesStreamFailureDetails) -> rt::Task<void> {
    throw rt::JsError("asynchronous observer failed");
    co_return;
  };
  const rt::ByteStream stream = createOwnedResponsesStream(std::move(options));
  const std::string text = co_await textOf(stream);
  EXPECT_EQ(typesOf(sseData(text)), (std::vector<std::string>{"error"}));
}

TEST("RemovedProvider preamble reorders setup events and refuses an unusable preamble") {
  bool omittedCreated = false;
  try {
    createOwnedResponsesStream(OwnedResponsesStreamOptions{
      .initial = {event(R"({"type":"response.in_progress","response_id":"resp_5"})")},
      .iterator = iterator({}),
      .responseId = "resp_5",
      .warning = OwnedResponsesStreamOptions::Warning{.model = "gpt-5.6-sol"},
    });
  } catch (const rt::JsError& error) {
    omittedCreated = rt::Regex("RemovedProvider stream omitted response.created").test(error.message);
  }
  EXPECT(omittedCreated);
  bool omittedIdentifier = false;
  try {
    createOwnedResponsesStream(OwnedResponsesStreamOptions{
      .initial = {event(R"({"type":"response.created"})")},
      .iterator = iterator({}),
      .responseId = std::nullopt,
      .warning = OwnedResponsesStreamOptions::Warning{.model = "gpt-5.6-sol"},
    });
  } catch (const rt::JsError& error) {
    omittedIdentifier = rt::Regex("RemovedProvider stream omitted a response identifier").test(error.message);
  }
  EXPECT(omittedIdentifier);
}

ASYNC_TEST("RemovedProvider preamble sequences created, setup, warning and payload events") {
  auto validated = std::make_shared<std::vector<ResponsesStreamEvent>>();
  OwnedResponsesStreamOptions options{
    .initial = {event(R"({"type":"response.created","response_id":"resp_6"})"), event(R"({"type":"response.in_progress","response_id":"resp_6"})"),
                event(R"({"type":"response.output_text.delta","delta":"hi","response_id":"resp_6"})")},
    .iterator = iterator({event(R"({"type":"response.output_text.done","text":"hi","response_id":"resp_6"})"),
                          event(R"({"type":"response.completed","response_id":"resp_6","response":{"id":"resp_6","output":[{"id":"msg_a","type":"message"}]}})")}),
    .responseId = "resp_6",
    .warning = OwnedResponsesStreamOptions::Warning{.model = "gpt-5.6-sol"},
  };
  options.validateEvent = [validated](const ResponsesStreamEvent& validatedEvent) { validated->push_back(validatedEvent); };
  const rt::ByteStream stream = createOwnedResponsesStream(std::move(options));

  const std::string text = co_await textOf(stream);
  const std::vector<json::object> values = sseData(text);
  EXPECT_EQ(values.size(), std::size_t{11});
  const std::vector<std::string> types = typesOf(values);
  ASSERT(types.size() >= 3);
  EXPECT_EQ(std::vector<std::string>(types.begin(), types.begin() + 3), (std::vector<std::string>{"response.created", "response.in_progress", "response.output_item.added"}));
  EXPECT_EQ(rt::getObject(values.at(2), "item") != nullptr, true);
  EXPECT_EQ(rt::getNumber(values.at(0), "sequence_number").has_value(), true);
  EXPECT_EQ(stringAt(values.at(8), "type"), std::optional<std::string>("response.output_text.delta"));
  EXPECT_EQ(stringAt(values.at(10), "type"), std::optional<std::string>("response.completed"));
  const json::array& output = *rt::getArray(rt::get(values.at(10), "response")->as_object(), "output");
  EXPECT_EQ(output.size(), std::size_t{2});
  EXPECT_EQ(rt::startsWith(stringAt(output.at(0).as_object(), "id").value_or(""), "msg_failover_"), true);
  EXPECT(rt::deepEqual(output.at(1), val(R"({"id":"msg_a","type":"message"})")));
  EXPECT_EQ(validated->size(), std::size_t{5});
}

ASYNC_TEST("owned stream rejects a changed response identifier before and after the queue") {
  auto readFailure = std::make_shared<std::vector<std::string>>();
  OwnedResponsesStreamOptions iteratorOptions{
    .initial = {event(R"({"type":"response.created","response_id":"resp_7"})")},
    .iterator = iterator({event(R"({"type":"response.completed","response_id":"resp_other"})")}),
    .responseId = "resp_7",
  };
  iteratorOptions.onFailure = [readFailure](rt::Reason, OwnedResponsesStreamFailureDetails details) { readFailure->push_back(details.failureKind); };
  const rt::ByteStream fromIterator = createOwnedResponsesStream(std::move(iteratorOptions));
  const std::string iteratorText = co_await textOf(fromIterator);
  const std::vector<json::object> iteratorValues = sseData(iteratorText);
  co_await flushMicrotasks();
  EXPECT_EQ(stringAt(iteratorValues.back(), "type"), std::optional<std::string>("error"));
  EXPECT_EQ(*readFailure, (std::vector<std::string>{"malformed_event"}));

  auto queuedFailure = std::make_shared<std::vector<std::string>>();
  OwnedResponsesStreamOptions queueOptions{
    .initial = {event(R"({"type":"response.created","response_id":"resp_8"})"), event(R"({"type":"response.completed","response_id":"resp_other"})")},
    .iterator = iterator({}),
    .responseId = "resp_8",
  };
  queueOptions.onFailure = [queuedFailure](rt::Reason, OwnedResponsesStreamFailureDetails details) { queuedFailure->push_back(details.failureKind); };
  const rt::ByteStream fromQueue = createOwnedResponsesStream(std::move(queueOptions));
  const std::string queueText = co_await textOf(fromQueue);
  const std::vector<json::object> queuedValues = sseData(queueText);
  co_await flushMicrotasks();
  EXPECT_EQ(typesOf(queuedValues), (std::vector<std::string>{"response.created", "error"}));
  EXPECT_EQ(*queuedFailure, (std::vector<std::string>{"malformed_event"}));
}

ASYNC_TEST("owned stream recovers observed items, compatibility output and streamed text into a failure") {
  const ResponsesStreamEvent recovered = event(R"({"type":"response.output_text.delta","delta":"kept","response_id":"resp_10","output_index":0})");
  const rt::ByteStream stream = createOwnedResponsesStream(OwnedResponsesStreamOptions{
    .initial =
      {
        recovered,
        event(R"({"type":"response.output_item.added","response_id":"resp_10","item":{"id":"msg_a","type":"message","content":[]}})"),
        event(
          R"({"type":"response.output_item.done","response_id":"resp_10","item":{"id":"msg_a","type":"message","status":"completed","content":[{"type":"output_text","text":"final"}]}})"),
        // A stored item that already carries text keeps its own copy (no recovered merge).
        event(R"({"type":"response.output_item.added","response_id":"resp_10","item":{"id":"msg_b","type":"message","content":[{"type":"output_text","text":"already"}]}})"),
        event(R"({"type":"response.output_text.delta","delta":"extra","response_id":"resp_10","item_id":"msg_b"})"),
        // An item without text is upgraded with the recovered copy.
        event(R"({"type":"response.output_item.added","response_id":"resp_10","item":{"id":"msg_c","type":"message","content":[]}})"),
        event(R"({"type":"response.output_text.delta","delta":"merged","response_id":"resp_10","item_id":"msg_c"})"),
        // Compatibility output, including the null form that falls back to response.output.
        event(R"({"type":"response.output","response_id":"resp_10","output":[{"id":"call_1","type":"function_call","call_id":"c1","name":"lookup","arguments":"{}"}]})"),
        event(
          R"({"type":"response.output","response_id":"resp_10","output":null,"response":{"output":[{"id":"compat_1","type":"message","content":[{"type":"output_text","text":"fallback"}]}]}})"),
        event(R"({"type":"response.output","response_id":"resp_10"})"),
        // Hosted tool lifecycles recover an identity, then update it in place, and a
        // repeated compatibility id replaces the stored entry instead of duplicating it.
        event(R"({"type":"response.web_search_call.completed","response_id":"resp_10"})"),
        event(R"({"type":"response.web_search_call.completed","response_id":"resp_10","item_id":"tool_recovered_1"})"),
        event(R"({"type":"response.file_search_call.in_progress","response_id":"resp_10","item_id":"tool_2"})"),
        event(R"({"type":"response.image_generation_call.failed","response_id":"resp_10","item_id":"tool_3"})"),
        event(R"({"type":"response.output","response_id":"resp_10","output":[7,{"id":"call_1","type":"function_call"}]})"),
        // Re-observing an id updates the stored entry instead of duplicating it.
        event(
          R"({"type":"response.output_item.done","response_id":"resp_10","item":{"id":"msg_a","type":"message","status":"completed","content":[{"type":"output_text","text":"final"}]}})"),
      },
    .iterator = iterator({}),
    .responseId = "resp_10",
  });

  const std::string text = co_await textOf(stream);
  const std::vector<json::object> values = sseData(text);
  EXPECT_EQ(stringAt(values.back(), "type"), std::optional<std::string>("response.failed"));
  const json::array& output = *rt::getArray(rt::get(values.back(), "response")->as_object(), "output");
  std::vector<std::string> ids;
  for (const json::value& item : output) ids.push_back(stringAt(item.as_object(), "id").value_or(""));
  const rt::OrderedSet<std::string> unique(ids.begin(), ids.end());
  EXPECT_EQ(unique.size(), ids.size());
  for (const std::string expected : {"msg_a", "msg_b", "msg_c", "call_1", "compat_1"}) EXPECT(unique.has(expected));

  const auto find = [&output](std::string_view id) -> const json::object* {
    for (const json::value& item : output)
      if (stringAt(item.as_object(), "id") == std::optional<std::string>(id)) return &item.as_object();
    return nullptr;
  };
  const auto firstText = [](const json::object* item) -> std::optional<std::string> {
    if (!item) return std::nullopt;
    const json::array* content = rt::getArray(*item, "content");
    if (!content || content->empty()) return std::nullopt;
    return stringAt(content->at(0).as_object(), "text");
  };
  EXPECT_EQ(firstText(find("msg_b")), std::optional<std::string>("already"));
  EXPECT_EQ(firstText(find("msg_c")), std::optional<std::string>("merged"));
  ASSERT(find("tool_2"));
  EXPECT_EQ(stringAt(*find("tool_2"), "status"), std::optional<std::string>("in_progress"));
  ASSERT(find("tool_3"));
  EXPECT_EQ(stringAt(*find("tool_3"), "status"), std::optional<std::string>("failed"));
  EXPECT_EQ(firstText(find("msg_recovered_0")), std::optional<std::string>("kept"));
  bool recoveredTool = false;
  for (const std::string& id : ids) recoveredTool = recoveredTool || rt::startsWith(id, "tool_recovered_");
  EXPECT_EQ(recoveredTool, true);
}

ASYNC_TEST("owned stream reports a failover warning item in its failure output") {
  const rt::ByteStream stream = createOwnedResponsesStream(OwnedResponsesStreamOptions{
    .initial = {event(R"({"type":"response.created","response_id":"resp_11"})"), event(R"({"type":"response.output_text.delta","delta":"seen","response_id":"resp_11"})")},
    .iterator = iterator({}),
    .responseId = "resp_11",
    .warning = OwnedResponsesStreamOptions::Warning{.model = "gpt-5.6-sol"},
  });
  const std::string text = co_await textOf(stream);
  const std::vector<json::object> values = sseData(text);
  EXPECT_EQ(stringAt(values.back(), "type"), std::optional<std::string>("response.failed"));
  const json::array& output = *rt::getArray(rt::get(values.back(), "response")->as_object(), "output");
  EXPECT_EQ(rt::startsWith(stringAt(output.at(0).as_object(), "id").value_or(""), "msg_failover_"), true);
  EXPECT_EQ(stringAt(output.at(0).as_object(), "role"), std::optional<std::string>("assistant"));
  bool recovered = false;
  for (const json::value& item : output) recovered = recovered || stringAt(item.as_object(), "id") == std::optional<std::string>("msg_recovered_0");
  EXPECT_EQ(recovered, true);
}
