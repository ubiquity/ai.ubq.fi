// Port of tests/anthropic-chat-responses.test.ts.
// Links: src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_responses.cpp src/provider/anthropic_replay.cpp
//        src/provider/anthropic_response_wire.cpp src/bounded_response_body.cpp src/responses_stream.cpp src/utils.cpp
//        src/inference_deadline.cpp src/openai_telemetry.cpp src/auth/relay.cpp src/codex/responses_error.cpp src/config.cpp src/http.cpp
//        src/provider/cerebras_rate_limits.cpp src/provider/removed_provider_telemetry.cpp src/release.cpp src/responses_failover_stream.cpp
#include "uos/provider/anthropic_chat_responses.hpp"

#include "uos/openai_telemetry.hpp"
#include "uos/provider/anthropic_replay.hpp"
#include "uos/provider/anthropic_response_wire.hpp"
#include "uos/responses_stream.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using uos::openai_telemetry::extractUsageTokens;
using uos::openai_telemetry::UsageTokens;
using uos::provider::anthropic_chat_responses::AnthropicChatResponsesOptions;
using uos::provider::anthropic_chat_responses::normalizeAnthropicChatResponsesResponse;
using uos::provider::anthropic_chat_responses::toAnthropicChatResponsesUsage;
using uos::provider::anthropic_replay::AnthropicChatReplayEntryNativeChatAssistant;
using uos::provider::anthropic_replay::restoreAnthropicChatReplay;
using uos::provider::anthropic_replay::restoreAnthropicReplay;
using uos::provider::anthropic_response_wire::ClaudeToolName;
using uos::responses_stream::readResponsesStream;
using uos::responses_stream::responsesStreamErrorOf;
using uos::responses_stream::ResponsesStreamEvent;
using uos::responses_stream::ResponsesStreamIterator;

namespace {
  using JsonRecord = json::object;
  using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;
  const std::string model = "claude-opus-5.5";

  JsonRecord obj(std::string_view text) { return rt::jsonParse(text).as_object(); }
  json::value val(std::string_view text) { return rt::jsonParse(text); }

  const JsonRecord& signedReasoning() {
    static const JsonRecord value =
      obj(R"({"type":"reasoning.text","text":"Use the requested tool.","signature":"opaque-chat-signature","format":"provider-format","index":0})");
    return value;
  }
  const JsonRecord& usage() {
    static const JsonRecord value = obj(
      R"({"prompt_tokens":1043,"prompt_tokens_details":{"cached_tokens":1028,"cache_write_tokens":0},"completion_tokens":58,"completion_tokens_details":{"reasoning_tokens":54},"total_tokens":1101,"buyer_cost_micro":285,"cost":0,"cost_details":{"upstream_inference_cost":0.0014256}})");
    return value;
  }
  std::string signedText() { return std::string(rt::getString(signedReasoning(), "text").value_or("")); }

  JsonRecord completion(const JsonRecord& assistant, std::string finish = "stop", const JsonRecord& measured = usage()) {
    JsonRecord choice;
    rt::objectDefine(choice, "index", 0);
    rt::objectDefine(choice, "message", assistant);
    rt::objectDefine(choice, "finish_reason", json::string(finish));
    JsonRecord value;
    rt::objectDefine(value, "id", json::string("chatcmpl-native-123"));
    rt::objectDefine(value, "object", json::string("chat.completion"));
    rt::objectDefine(value, "created", 1'790'000'000);
    rt::objectDefine(value, "model", json::string(model));
    rt::objectDefine(value, "choices", json::array{choice});
    rt::objectDefine(value, "usage", measured);
    return value;
  }

  JsonRecord chunk(const JsonRecord& delta, std::optional<std::string> finish = std::nullopt) {
    JsonRecord choice;
    rt::objectDefine(choice, "index", 0);
    rt::objectDefine(choice, "delta", delta);
    rt::objectDefine(choice, "finish_reason", finish ? json::value(json::string(*finish)) : json::value(nullptr));
    JsonRecord value;
    rt::objectDefine(value, "id", json::string("chatcmpl-stream-native"));
    rt::objectDefine(value, "object", json::string("chat.completion.chunk"));
    rt::objectDefine(value, "created", 1'790'000'000);
    rt::objectDefine(value, "model", json::string(model));
    rt::objectDefine(value, "choices", json::array{choice});
    return value;
  }

  JsonRecord usageChunk(const JsonRecord& measured = usage()) {
    JsonRecord value;
    rt::objectDefine(value, "choices", json::array());
    rt::objectDefine(value, "usage", measured);
    return value;
  }

  rt::Response sse(const std::vector<JsonRecord>& chunks, bool done = true) {
    std::string body;
    for (const JsonRecord& value : chunks) body += "data: " + rt::jsonStringify(json::value(value)) + "\n\n";
    if (done) body += "data: [DONE]\n\n";
    return rt::Response(rt::BodyInit(body), rt::ResponseInit{.status = std::nullopt, .statusText = std::nullopt, .headers = rt::Headers{{"content-type", "text/event-stream"}}});
  }

  rt::Task<std::vector<JsonRecord>> collect(rt::Response response) {
    const std::optional<rt::ByteStream> body = response.body();
    if (!body) rt::test::report(__FILE__, __LINE__, "expected response.body", true);
    const ResponsesStreamIterator iterator = readResponsesStream(*body);
    std::vector<JsonRecord> result;
    for (;;) {
      rt::IteratorResult<ResponsesStreamEvent> step = co_await iterator.next();
      if (step.done) break;
      result.push_back(step.value->value);
    }
    co_return result;
  }

  rt::Task<std::vector<JsonRecord>> collectNormalized(rt::Response upstream, AnthropicChatResponsesOptions options) {
    const rt::Response response = co_await normalizeAnthropicChatResponsesResponse(upstream, std::move(options));
    std::vector<JsonRecord> events = co_await collect(response);
    co_return events;
  }

  rt::Task<json::value> normalizedJson(rt::Response upstream, AnthropicChatResponsesOptions options) {
    const rt::Response response = co_await normalizeAnthropicChatResponsesResponse(upstream, std::move(options));
    json::value payload = co_await response.json();
    co_return payload;
  }

  AnthropicChatResponsesOptions options(std::optional<std::string> provider = std::nullopt, std::optional<std::string> scope = std::nullopt) {
    AnthropicChatResponsesOptions value;
    value.model = model;
    value.provider = std::move(provider);
    value.reasoningReplayScope = std::move(scope);
    return value;
  }

  const JsonRecord& terminal(const std::vector<JsonRecord>& events) { return rt::get(events.back(), "response")->as_object(); }

  std::optional<std::string> stringAt(const json::value& value, std::string_view key) {
    const std::optional<std::string_view> found = rt::getString(value, key);
    return found ? std::optional<std::string>(std::string(*found)) : std::nullopt;
  }
  const json::value& at(const json::value& value, std::string_view key) {
    const json::value* found = rt::get(value, key);
    if (!found) throw rt::JsError("missing member " + std::string(key));
    return *found;
  }
  const json::value& at(const json::value& value, std::size_t index) {
    const json::value* found = rt::get(value.as_array(), index);
    if (!found) throw rt::JsError("missing element");
    return *found;
  }

  bool includesText(const json::value& value, std::string_view needle) { return rt::jsonStringify(value).find(needle) != std::string::npos; }
  json::value all(const std::vector<JsonRecord>& events) {
    json::array values;
    for (const JsonRecord& event : events) values.push_back(event);
    return values;
  }

  std::vector<std::string> outputTypes(const json::value& payload) {
    std::vector<std::string> types;
    for (const json::value& item : at(payload, "output").as_array()) types.push_back(stringAt(item, "type").value_or(""));
    return types;
  }

  // restoreAnthropicChatReplay(...) deepEqual [{ type: "native_chat_assistant", message }].
  bool restoresMessage(const json::array& output, const std::string& scope, const std::string& provider, const json::value& message) {
    const auto restored = restoreAnthropicChatReplay(output, {.scope = scope, .provider = provider, .model = model});
    if (!restored || restored->size() != 1) return false;
    const auto* native = std::get_if<AnthropicChatReplayEntryNativeChatAssistant>(&restored->front());
    return native && rt::deepEqual(json::value(native->message), message);
  }

  template <class T> rt::Task<std::optional<std::string>> rejectionKind(rt::Task<T> run) {
    rt::Reason failure;
    try {
      co_await std::move(run);
    } catch (...) {
      failure = std::current_exception();
    }
    if (!failure) co_return std::nullopt;
    const auto* error = responsesStreamErrorOf(failure);
    co_return error ? std::optional<std::string>(error->kind) : std::optional<std::string>("<not a ResponsesStreamError>");
  }
}

TEST("Claude Chat Responses usage stays gross and retains exact cache and buyer cost provenance") {
  const std::optional<JsonRecord> projected = toAnthropicChatResponsesUsage(json::value(usage()));
  ASSERT(projected);
  EXPECT(rt::deepEqual(
    json::value(*projected),
    val(R"({"input_tokens":1043,"input_tokens_details":{"cached_tokens":1028,"cache_write_tokens":0},"output_tokens":58,"output_tokens_details":{"reasoning_tokens":54},"total_tokens":1101,"buyer_cost_micro":285,"cost":0,"cost_details":{"upstream_inference_cost":0.0014256}})")));
  const std::optional<UsageTokens> measured = extractUsageTokens(json::value(*projected));
  ASSERT(measured);
  EXPECT_EQ(measured->inputTokens, std::optional<double>(1043));
  EXPECT_EQ(measured->cachedInputTokens, std::optional<double>(1028));
  EXPECT_EQ(measured->cacheWriteInputTokens, std::optional<double>(0));
}

TEST("Claude Chat Responses usage does not create zero counters or launder invalid measurements") {
  EXPECT(rt::deepEqual(json::value(*toAnthropicChatResponsesUsage(val(R"({"prompt_tokens":15,"completion_tokens":4,"total_tokens":19})"))),
                       val(R"({"input_tokens":15,"output_tokens":4,"total_tokens":19})")));
  const std::optional<JsonRecord> projected = toAnthropicChatResponsesUsage(
    val(R"({"prompt_tokens":15.5,"completion_tokens":4,"total_tokens":19.5,"buyer_cost_micro":"69","prompt_tokens_details":{"cached_tokens":-1}})"));
  ASSERT(projected);
  const std::optional<UsageTokens> measured = extractUsageTokens(json::value(*projected));
  EXPECT_EQ(measured ? std::optional<std::string>(measured->status) : std::nullopt, std::optional<std::string>("invalid"));
  EXPECT_EQ(stringAt(json::value(*projected), "buyer_cost_micro"), std::optional<std::string>("69"));
}

ASYNC_TEST("Claude Chat Responses JSON emits reasoning and text without duplicating signed detail text") {
  JsonRecord assistant = obj(R"({"role":"assistant","content":"OK"})");
  rt::objectDefine(assistant, "reasoning_content", json::string(signedText()));
  rt::objectDefine(assistant, "reasoning_details", json::array{signedReasoning()});
  const rt::Response upstream(
    rt::BodyInit(rt::jsonStringify(json::value(completion(assistant)))),
    rt::ResponseInit{.status = std::nullopt, .statusText = std::nullopt, .headers = rt::Headers{{"content-type", "application/json"}, {"content-length", "999"}, {"x-cost-micro", "285"}}});
  AnthropicChatResponsesOptions modelOptions = options();
  const rt::Response response = co_await normalizeAnthropicChatResponsesResponse(upstream, std::move(modelOptions));
  const json::value payload = co_await response.json();
  EXPECT_EQ(stringAt(payload, "status"), std::optional<std::string>("completed"));
  EXPECT_EQ(outputTypes(payload), (std::vector<std::string>{"reasoning", "message"}));
  json::array summary{JsonRecord{{"type", "summary_text"}, {"text", signedText()}}};
  EXPECT(rt::deepEqual(at(at(at(payload, "output"), 0), "summary"), json::value(summary)));
  EXPECT_EQ(stringAt(at(at(at(at(payload, "output"), 1), "content"), 0), "text"), std::optional<std::string>("OK"));
  EXPECT_EQ(rt::getNumber(at(payload, "usage"), "input_tokens"), std::optional<double>(1043));
  EXPECT_EQ(rt::getNumber(at(payload, "usage"), "buyer_cost_micro"), std::optional<double>(285));
  EXPECT_EQ(response.headers().get("x-cost-micro"), std::optional<std::string>("285"));
  EXPECT_EQ(response.headers().get("content-length"), std::nullopt);
  EXPECT_EQ(includesText(payload, "opaque-chat-signature"), false);
  EXPECT_EQ(includesText(payload, "encrypted_content"), false);
}

ASYNC_TEST("Claude Chat Responses JSON privately restores the original signed Chat message") {
  const std::string scope = "principal_" + rt::crypto::randomUUID();
  JsonRecord assistant = obj(R"({"role":"assistant","content":null})");
  rt::objectDefine(assistant, "reasoning_content", json::string(signedText()));
  rt::objectDefine(assistant, "reasoning_details", json::array{signedReasoning()});
  rt::objectDefine(assistant, "tool_calls", val(R"([{"id":"call_weather","type":"function","function":{"name":"weather","arguments":"{\"city\":\"Paris\"}"}}])"));
  const rt::Response upstream(rt::BodyInit(rt::jsonStringify(json::value(completion(assistant, "tool_calls")))));
  AnthropicChatResponsesOptions callOptions = options(std::string("surplus"), scope);
  const json::value payload = co_await normalizedJson(upstream, std::move(callOptions));
  const json::array& output = at(payload, "output").as_array();
  EXPECT(restoresMessage(output, scope, "surplus", json::value(assistant)));
  EXPECT(!restoreAnthropicReplay(output, {.scope = scope, .provider = "surplus", .model = model}));
  EXPECT(!restoreAnthropicChatReplay(output, {.scope = "another-principal", .provider = "surplus", .model = model}));
}

ASYNC_TEST("Claude Chat Responses respects summary none without losing private signed tool continuation") {
  const std::string scope = "principal_" + rt::crypto::randomUUID();
  JsonRecord assistant = obj(R"({"role":"assistant","content":null})");
  rt::objectDefine(assistant, "reasoning_details", json::array{signedReasoning()});
  rt::objectDefine(assistant, "tool_calls", val(R"([{"id":"call_clock","type":"function","function":{"name":"clock","arguments":"{}"}}])"));
  const rt::Response upstream(rt::BodyInit(rt::jsonStringify(json::value(completion(assistant, "tool_calls")))));
  AnthropicChatResponsesOptions noSummary = options("surplus", scope);
  noSummary.reasoningSummary = "none";
  const json::value payload = co_await normalizedJson(upstream, std::move(noSummary));
  EXPECT_EQ(outputTypes(payload), (std::vector<std::string>{"function_call"}));
  EXPECT(restoresMessage(at(payload, "output").as_array(), scope, "surplus", json::value(assistant)));
  EXPECT_EQ(includesText(payload, signedText()), false);
}

ASYNC_TEST("Claude Chat Responses JSON supports namespaced and freeform tools with exact private provenance") {
  const std::string scope = "principal_" + rt::crypto::randomUUID();
  JsonRecord assistant = obj(R"({"role":"assistant","content":null})");
  rt::objectDefine(assistant, "reasoning_details", json::array{signedReasoning()});
  rt::objectDefine(assistant, "tool_calls",
                   val(R"([{"id":"call_namespaced","type":"function","function":{"name":"weather_wrapper","arguments":"{\"city\":\"Paris\"}"}},{"id":"call_custom","type":"function","function":{"name":"patch_wrapper","arguments":"{\"input\":\"patch text\"}"}}])"));
  const rt::Response upstream(rt::BodyInit(rt::jsonStringify(json::value(completion(assistant, "tool_calls")))));
  AnthropicChatResponsesOptions withTools = options("surplus", scope);
  withTools.toolNames = rt::OrderedMap<std::string, ClaudeToolName>{
    {"weather_wrapper", ClaudeToolName{.name = "weather", .namespace_ = "functions"}},
    {"patch_wrapper", ClaudeToolName{.name = "apply_patch", .namespace_ = std::nullopt}},
  };
  withTools.customToolNames = rt::OrderedSet<std::string>{"patch_wrapper"};
  const json::value payload = co_await normalizedJson(upstream, std::move(withTools));
  EXPECT_EQ(stringAt(at(at(payload, "output"), 1), "name"), std::optional<std::string>("weather"));
  EXPECT_EQ(stringAt(at(at(payload, "output"), 1), "namespace"), std::optional<std::string>("functions"));
  EXPECT_EQ(stringAt(at(at(payload, "output"), 2), "type"), std::optional<std::string>("custom_tool_call"));
  EXPECT_EQ(stringAt(at(at(payload, "output"), 2), "name"), std::optional<std::string>("apply_patch"));
  EXPECT_EQ(stringAt(at(at(payload, "output"), 2), "input"), std::optional<std::string>("patch text"));
  EXPECT(restoresMessage(at(payload, "output").as_array(), scope, "surplus", json::value(assistant)));
}

ASYNC_TEST("Claude Chat Max reasoning-only length completion is incomplete and retains cache/cost usage") {
  const std::string scope = "principal_" + rt::crypto::randomUUID();
  JsonRecord capped = rt::objectSpread(usage());
  rt::objectDefine(capped, "completion_tokens", 128);
  rt::objectDefine(capped, "completion_tokens_details", val(R"({"reasoning_tokens":128})"));
  rt::objectDefine(capped, "total_tokens", 1171);
  rt::objectDefine(capped, "buyer_cost_micro", 565);
  JsonRecord assistant = obj(R"({"role":"assistant","content":null})");
  rt::objectDefine(assistant, "reasoning_content", json::string(signedText()));
  rt::objectDefine(assistant, "reasoning_details", json::array{signedReasoning()});
  const rt::Response upstream(rt::BodyInit(rt::jsonStringify(json::value(completion(assistant, "length", capped)))));
  AnthropicChatResponsesOptions callOptions = options(std::string("surplus"), scope);
  const json::value payload = co_await normalizedJson(upstream, std::move(callOptions));
  EXPECT_EQ(stringAt(payload, "status"), std::optional<std::string>("incomplete"));
  EXPECT(rt::deepEqual(at(payload, "incomplete_details"), val(R"({"reason":"max_output_tokens"})")));
  EXPECT_EQ(outputTypes(payload), (std::vector<std::string>{"reasoning"}));
  EXPECT_EQ(rt::getNumber(at(payload, "usage"), "input_tokens"), std::optional<double>(1043));
  EXPECT_EQ(rt::getNumber(at(at(payload, "usage"), "input_tokens_details"), "cached_tokens"), std::optional<double>(1028));
  EXPECT_EQ(rt::getNumber(at(payload, "usage"), "buyer_cost_micro"), std::optional<double>(565));
  EXPECT(!restoreAnthropicChatReplay(at(payload, "output").as_array(), {.scope = scope, .provider = "surplus", .model = model}));
}

ASYNC_TEST("Claude Chat SSE streams reasoning and text, then includes trailing cache and authoritative cost") {
  const std::string scope = "principal_" + rt::crypto::randomUUID();
  JsonRecord first = obj(R"({"role":"assistant"})");
  rt::objectDefine(first, "reasoning_content", json::string(signedText()));
  JsonRecord firstDetail = obj(R"({"type":"reasoning.text"})");
  rt::objectDefine(firstDetail, "text", json::string(signedText()));
  rt::objectDefine(firstDetail, "format", json::string("provider-format"));
  rt::objectDefine(firstDetail, "index", 0);
  rt::objectDefine(first, "reasoning_details", json::array{firstDetail});
  const std::vector<JsonRecord> chunks{
    chunk(first),
    chunk(obj(R"({"reasoning_details":[{"type":"reasoning.text","signature":"opaque-chat-signature","format":"provider-format","index":0}]})")),
    chunk(obj(R"({"content":"O"})")),
    chunk(obj(R"({"content":"K"})"), "stop"),
    usageChunk(),
  };
  const rt::Response upstream = sse(chunks);
  AnthropicChatResponsesOptions callOptions = options(std::string("surplus"), scope);
  const std::vector<JsonRecord> events = co_await collectNormalized(upstream, std::move(callOptions));
  for (std::size_t index = 0; index < events.size(); ++index) EXPECT_EQ(rt::getNumber(events[index], "sequence_number"), std::optional<double>(static_cast<double>(index)));
  const JsonRecord& payload = terminal(events);
  EXPECT_EQ(stringAt(json::value(payload), "status"), std::optional<std::string>("completed"));
  EXPECT_EQ(rt::getNumber(*rt::get(payload, "usage"), "input_tokens"), std::optional<double>(1043));
  EXPECT_EQ(rt::getNumber(*rt::get(payload, "usage"), "buyer_cost_micro"), std::optional<double>(285));
  int reasoningDeltas = 0;
  for (const JsonRecord& event : events)
    if (stringAt(json::value(event), "type") == std::optional<std::string>("response.reasoning_summary_text.delta")) reasoningDeltas += 1;
  EXPECT_EQ(reasoningDeltas, 1);
  JsonRecord expected = obj(R"({"role":"assistant","content":"OK"})");
  rt::objectDefine(expected, "reasoning_content", json::string(signedText()));
  rt::objectDefine(expected, "reasoning_details", json::array{signedReasoning()});
  EXPECT(restoresMessage(rt::get(payload, "output")->as_array(), scope, "surplus", json::value(expected)));
  EXPECT_EQ(includesText(all(events), "opaque-chat-signature"), false);
}

ASYNC_TEST("Claude Chat SSE closes text before announcing complete fragmented tool calls") {
  const std::vector<JsonRecord> chunks{
    chunk(obj(R"({"content":"Checking."})")),
    chunk(obj(R"({"tool_calls":[{"index":0,"id":"call_weather","type":"function","function":{"name":"wea","arguments":""}}]})")),
    chunk(obj(R"({"tool_calls":[{"index":0,"function":{"name":"ther","arguments":"{\"city\":"}}]})")),
    chunk(obj(R"({"tool_calls":[{"index":0,"function":{"arguments":"\"Paris\"}"}}]})"), "tool_calls"),
    usageChunk(),
  };
  const rt::Response upstream = sse(chunks);
  AnthropicChatResponsesOptions callOptions = options();
  const std::vector<JsonRecord> events = co_await collectNormalized(upstream, std::move(callOptions));
  std::ptrdiff_t doneMessage = -1, addedTool = -1;
  for (std::size_t index = 0; index < events.size(); ++index) {
    const json::value event(events[index]);
    const json::value* item = rt::get(event, "item");
    if (doneMessage < 0 && stringAt(event, "type") == std::optional<std::string>("response.output_item.done") && item &&
        stringAt(*item, "type") == std::optional<std::string>("message"))
      doneMessage = static_cast<std::ptrdiff_t>(index);
    if (addedTool < 0 && stringAt(event, "type") == std::optional<std::string>("response.output_item.added") && item &&
        stringAt(*item, "type") == std::optional<std::string>("function_call"))
      addedTool = static_cast<std::ptrdiff_t>(index);
  }
  EXPECT(doneMessage < addedTool);
  const json::array& output = rt::get(terminal(events), "output")->as_array();
  EXPECT_EQ(stringAt(output.at(1), "name"), std::optional<std::string>("weather"));
  EXPECT_EQ(stringAt(output.at(1), "arguments"), std::optional<std::string>("{\"city\":\"Paris\"}"));
  EXPECT_EQ(stringAt(output.at(1), "call_id"), std::optional<std::string>("call_weather"));
}

ASYNC_TEST("Claude Chat proven Max SSE reasoning alias renders once with actual cache-write and buyer cost") {
  const JsonRecord nativeUsage = obj(
    R"({"prompt_tokens":1043,"prompt_tokens_details":{"cached_tokens":0,"cache_write_tokens":1028},"completion_tokens":128,"completion_tokens_details":{"reasoning_tokens":128},"total_tokens":1171,"buyer_cost_micro":1552})");
  const auto details = [](std::string_view fields) {
    JsonRecord value = obj(R"({"type":"reasoning.text","format":"anthropic-claude-v1","index":0})");
    const JsonRecord parsed = obj(fields);
    for (const json::key_value_pair* entry : rt::objectEntries(parsed)) rt::objectDefine(value, entry->key(), entry->value());
    return json::value(json::array{value});
  };
  JsonRecord first = obj(R"({"role":"assistant","reasoning":"Use "})");
  rt::objectDefine(first, "reasoning_details", details(R"({"text":"Use "})"));
  JsonRecord second = obj(R"({"reasoning":"tool."})");
  rt::objectDefine(second, "reasoning_details", details(R"({"text":"tool."})"));
  JsonRecord third;
  rt::objectDefine(third, "reasoning_details", details(R"({"signature":"late-original-signature"})"));
  const rt::Response upstream = sse({chunk(first), chunk(second), chunk(third), chunk(JsonRecord(), "length"), usageChunk(nativeUsage)});
  AnthropicChatResponsesOptions modelOptions = options();
  const rt::Response response = co_await normalizeAnthropicChatResponsesResponse(upstream, std::move(modelOptions));
  EXPECT_EQ(response.headers().get("x-cost-micro"), std::nullopt);
  const std::vector<JsonRecord> events = co_await collect(response);
  std::vector<std::string> deltas;
  for (const JsonRecord& event : events)
    if (stringAt(json::value(event), "type") == std::optional<std::string>("response.reasoning_summary_text.delta"))
      deltas.push_back(stringAt(json::value(event), "delta").value_or(""));
  EXPECT_EQ(deltas, (std::vector<std::string>{"Use ", "tool."}));
  const JsonRecord& payload = terminal(events);
  EXPECT_EQ(stringAt(json::value(payload), "status"), std::optional<std::string>("incomplete"));
  EXPECT(rt::deepEqual(*rt::get(payload, "incomplete_details"), val(R"({"reason":"max_output_tokens"})")));
  EXPECT(rt::deepEqual(*rt::get(payload, "usage"), json::value(*toAnthropicChatResponsesUsage(json::value(nativeUsage)))));
  EXPECT_EQ(includesText(all(events), "late-original-signature"), false);
}

ASYNC_TEST("Claude Chat SSE privately retains original reasoning spelling and a late replacement for an empty signature") {
  const std::string scope = "principal_" + rt::crypto::randomUUID();
  JsonRecord emptySigned = rt::objectSpread(signedReasoning());
  rt::objectDefine(emptySigned, "signature", json::string(""));
  JsonRecord first = obj(R"({"role":"assistant","reasoning":"Use the requested tool."})");
  rt::objectDefine(first, "reasoning_details", json::array{emptySigned});
  JsonRecord lateSignature = obj(R"({"type":"reasoning.text"})");
  rt::objectDefine(lateSignature, "signature", *rt::get(signedReasoning(), "signature"));
  rt::objectDefine(lateSignature, "format", *rt::get(signedReasoning(), "format"));
  rt::objectDefine(lateSignature, "index", 0);
  JsonRecord second;
  rt::objectDefine(second, "reasoning_details", json::array{lateSignature});
  const std::vector<JsonRecord> chunks{
    chunk(first),
    chunk(second),
    chunk(obj(R"({"tool_calls":[{"index":0,"id":"call_weather","type":"function","function":{"name":"weather","arguments":"{}"}}]})"), "tool_calls"),
    usageChunk(),
  };
  const rt::Response upstream = sse(chunks);
  AnthropicChatResponsesOptions callOptions = options(std::string("surplus"), scope);
  const std::vector<JsonRecord> events = co_await collectNormalized(upstream, std::move(callOptions));
  JsonRecord expected = obj(R"({"role":"assistant","content":null})");
  rt::objectDefine(expected, "reasoning", json::string(signedText()));
  rt::objectDefine(expected, "reasoning_details", json::array{signedReasoning()});
  rt::objectDefine(expected, "tool_calls", val(R"([{"id":"call_weather","type":"function","function":{"name":"weather","arguments":"{}"}}])"));
  EXPECT(restoresMessage(rt::get(terminal(events), "output")->as_array(), scope, "surplus", json::value(expected)));
}

ASYNC_TEST("Claude Chat SSE length and malformed or missing terminals are not completed") {
  const rt::Response lengthUpstream = sse({chunk(obj(R"({"reasoning_content":"Partial."})"), "length"), usageChunk()});
  AnthropicChatResponsesOptions callOptions = options();
  const std::vector<JsonRecord> events = co_await collectNormalized(lengthUpstream, std::move(callOptions));
  EXPECT_EQ(stringAt(json::value(events.back()), "type"), std::optional<std::string>("response.incomplete"));
  const std::vector<rt::Response> bodies{sse({chunk(obj(R"({"content":"partial"})"))}, false), sse({chunk(obj(R"({"content":"partial"})"))})};
  for (const rt::Response& body : bodies) {
    rt::Task<std::vector<JsonRecord>> collected = collectNormalized(body, options());
    const std::optional<std::string> kind = co_await rejectionKind(std::move(collected));
    EXPECT_EQ(kind, std::optional<std::string>("premature_eof"));
  }
  const rt::Response malformedBody(rt::BodyInit(std::string("data: {\n\ndata: [DONE]\n\n")),
                                   rt::ResponseInit{.status = std::nullopt, .statusText = std::nullopt, .headers = rt::Headers{{"content-type", "text/event-stream"}}});
  rt::Task<std::vector<JsonRecord>> malformed = collectNormalized(malformedBody, options());
  const std::optional<std::string> kind = co_await rejectionKind(std::move(malformed));
  EXPECT_EQ(kind, std::optional<std::string>("malformed_event"));
}

ASYNC_TEST("Claude Chat SSE ambiguous repeated signatures do not create a signed replay receipt") {
  const std::string scope = "principal_" + rt::crypto::randomUUID();
  JsonRecord firstDetail = rt::objectSpread(signedReasoning());
  rt::objectDefine(firstDetail, "signature", json::string("first"));
  JsonRecord first;
  rt::objectDefine(first, "reasoning_details", json::array{firstDetail});
  const std::vector<JsonRecord> chunks{
    chunk(first),
    chunk(obj(R"({"reasoning_details":[{"type":"reasoning.text","text":"More.","signature":"second","index":0,"format":"provider-format"}],"content":"OK"})"), "stop"),
    usageChunk(),
  };
  const rt::Response upstream = sse(chunks);
  AnthropicChatResponsesOptions callOptions = options(std::string("surplus"), scope);
  const std::vector<JsonRecord> events = co_await collectNormalized(upstream, std::move(callOptions));
  EXPECT_EQ(stringAt(json::value(terminal(events)), "status"), std::optional<std::string>("completed"));
  EXPECT(!restoreAnthropicChatReplay(rt::get(terminal(events), "output")->as_array(), {.scope = scope, .provider = "surplus", .model = model}));
}

ASYNC_TEST("Claude Chat preserves HTTP errors and propagates downstream cancellation") {
  const rt::Response errorResponse(rt::BodyInit(std::string(R"({"error":{"code":"insufficient_funds","message":"capacity"}})")),
                                   rt::ResponseInit{.status = 429, .statusText = std::nullopt, .headers = std::nullopt});
  AnthropicChatResponsesOptions errorOptions = options();
  const rt::Response passedThrough = co_await normalizeAnthropicChatResponsesResponse(errorResponse, std::move(errorOptions));
  EXPECT(passedThrough == errorResponse);
  auto cancelled = std::make_shared<bool>(false);
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.start = [](Controller controller) {
    controller.enqueue(rt::encodeUtf8("data: " + rt::jsonStringify(json::value(chunk(obj(R"({"content":"O"})")))) + "\n\n"));
  };
  source.cancel = [cancelled](rt::Reason) { *cancelled = true; };
  const rt::Response upstream(rt::BodyInit(rt::ByteStream(std::move(source))),
                              rt::ResponseInit{.status = std::nullopt, .statusText = std::nullopt, .headers = rt::Headers{{"content-type", "text/event-stream"}}});
  AnthropicChatResponsesOptions modelOptions = options();
  const rt::Response response = co_await normalizeAnthropicChatResponsesResponse(upstream, std::move(modelOptions));
  const std::optional<rt::ByteStream> body = response.body();
  ASSERT(body);
  const auto reader = body->getReader();
  co_await reader.read();
  const rt::Reason reason = rt::thrown(json::value("client stopped"));
  co_await reader.cancel(reason);
  reader.releaseLock();
  EXPECT_EQ(*cancelled, true);
}

ASYNC_TEST("Claude Chat observed usage survives a subsequent premature stream EOF") {
  const rt::Response upstream = sse({chunk(obj(R"({"content":"Partial."})")), usageChunk()}, false);
  AnthropicChatResponsesOptions modelOptions = options();
  const rt::Response response = co_await normalizeAnthropicChatResponsesResponse(upstream, std::move(modelOptions));
  const std::optional<rt::ByteStream> body = response.body();
  ASSERT(body);
  auto observed = std::make_shared<std::vector<JsonRecord>>();
  const ResponsesStreamIterator iterator = readResponsesStream(*body);
  std::optional<std::string> kind;
  try {
    for (;;) {
      rt::IteratorResult<ResponsesStreamEvent> step = co_await iterator.next();
      if (step.done) break;
      observed->push_back(step.value->value);
    }
  } catch (...) {
    const auto* error = responsesStreamErrorOf(std::current_exception());
    kind = error ? error->kind : "<other>";
  }
  EXPECT_EQ(kind, std::optional<std::string>("premature_eof"));
  const JsonRecord* measured = nullptr;
  for (const JsonRecord& event : *observed) {
    const json::value* eventResponse = rt::get(event, "response");
    if (stringAt(json::value(event), "type") == std::optional<std::string>("response.in_progress") && eventResponse && !rt::get(*eventResponse, "usage")->is_null())
      measured = &event;
  }
  ASSERT(measured);
  EXPECT(rt::deepEqual(*rt::get(*rt::get(*measured, "response"), "usage"), json::value(*toAnthropicChatResponsesUsage(json::value(usage())))));
  bool completed = false;
  for (const JsonRecord& event : *observed) completed = completed || stringAt(json::value(event), "type") == std::optional<std::string>("response.completed");
  EXPECT_EQ(completed, false);
}
