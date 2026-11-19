// Links: tests/anthropic_responses_test.cpp src/config.cpp src/release.cpp src/responses_stream.cpp src/provider/anthropic_responses.cpp
//        src/models/codex_models_fetch.cpp src/codex/dispatch.cpp src/provider/capacity_events.cpp src/models/codex_models_availability.cpp
//        src/provider/anthropic_replay.cpp src/codex/capacity_routing.cpp src/models/codex_models.cpp src/utils.cpp src/kv.cpp
//        src/authority/native_codex_capacity.cpp src/codex/routing_state.cpp src/openai_telemetry.cpp src/codex/auth.cpp
//        src/authority/native_codex_contracts.cpp src/runtime_config.cpp src/codex/routing_mutations.cpp src/codex/banked_reset.cpp
//        src/codex/native_auth.cpp src/types.cpp src/responses_failover_stream.cpp src/codex/auth_refresh.cpp
//        src/codex/banked_reset_provider.cpp src/codex/external_auth.cpp src/authority/native_codex_mutations.cpp
//        src/provider/anthropic_response_wire.cpp src/provider/health.cpp src/authority/native_codex_evaluation.cpp
//        src/provider/capacity_contract.cpp src/bounded_response_body.cpp src/provider/removed_provider_telemetry.cpp
//        src/codex/reset_settings.cpp src/storage/kv.cpp src/codex/responses_error.cpp src/provider/capacity.cpp src/provider/capacity_parse.cpp
//        src/http.cpp src/storage/sqlite_readonly.cpp src/defaults.cpp src/provider/capacity_history.cpp src/provider/capacity_analytics.cpp
//        src/provider/capacity_rollups.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_capacity_history.cpp
//        src/codex/rate_limit_429.cpp src/storage/native_kv.cpp src/provider/claude_code_accounts.cpp src/provider/cerebras_rate_limits.cpp
//        src/codex/native_auth_transport.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_credentials.cpp
//        src/authority/native_codex_429.cpp src/auth/relay.cpp src/paid_fallback/ledger_stats.cpp src/cache/prompt_analytics_read.cpp
//        src/metered_quota.cpp src/cache/prompt_analytics_core.cpp src/codex/experiment.cpp src/provider/claude_code_usage_schema.cpp
// src/runtime/websocket.cpp src/codex/account_routing.cpp src/codex/routing_serial.cpp
//        src/authority/native_codex.cpp src/codex/routing_evaluation.cpp src/provider/selection.cpp src/codex/overage_settings.cpp
// Port of tests/anthropic-responses.test.ts: all original cases and assertions.
#include "uos/provider/anthropic_responses.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/openai_telemetry.hpp"
#include "uos/config.hpp"
#include "uos/rt/env.hpp"

#include "uos/provider/anthropic_replay.hpp"
#include "uos/responses_stream.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using uos::provider::anthropic_replay::NativeReplayEntryNativeAssistant;
using uos::provider::anthropic_replay::restoreAnthropicReplay;
using uos::provider::anthropic_response_wire::ClaudeToolName;
using uos::provider::anthropic_responses::AnthropicResponsesOptions;
using uos::provider::anthropic_responses::normalizeAnthropicResponsesResponse;
using uos::provider::anthropic_responses::toAnthropicResponsesUsage;
using uos::responses_stream::readResponsesStream;
using uos::responses_stream::responsesStreamErrorOf;
using uos::responses_stream::ResponsesStreamEvent;
using uos::responses_stream::ResponsesStreamIterator;

namespace {
  struct CredentialIsolation {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    CredentialIsolation() { uos::config::config.codexAuthJsonB64.clear(); }
    ~CredentialIsolation() { uos::config::config.codexAuthJsonB64 = savedSeed; }
  };

  using JsonRecord = json::object;
  using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;
  const std::string model = "claude-opus-5.5";

  JsonRecord obj(std::string_view text) { return rt::jsonParse(text).as_object(); }
  json::value val(std::string_view text) { return rt::jsonParse(text); }

  JsonRecord message(json::array content, JsonRecord usage, std::string stopReason = "end_turn") {
    JsonRecord value;
    rt::objectDefine(value, "id", json::string("msg_native-123"));
    rt::objectDefine(value, "type", json::string("message"));
    rt::objectDefine(value, "role", json::string("assistant"));
    rt::objectDefine(value, "model", json::string(model));
    rt::objectDefine(value, "content", std::move(content));
    rt::objectDefine(value, "stop_reason", json::string(stopReason));
    rt::objectDefine(value, "stop_sequence", nullptr);
    rt::objectDefine(value, "usage", std::move(usage));
    return value;
  }

  std::string sse(const std::vector<JsonRecord>& events, std::string_view separator = "\n\n") {
    std::string out;
    for (const JsonRecord& event : events) out += "event: " + rt::toString(rt::get(event, "type")) + "\ndata: " + rt::jsonStringify(json::value(event)) + std::string(separator);
    return out;
  }

  rt::Response streamResponse(const std::vector<JsonRecord>& events, rt::Headers extraHeaders = {}) {
    rt::Headers headers(extraHeaders);
    headers.set("content-type", "text/event-stream");
    return rt::Response(rt::BodyInit(sse(events)), rt::ResponseInit{.status = std::nullopt, .statusText = std::nullopt, .headers = headers});
  }

  JsonRecord start(JsonRecord usage = obj(R"({"input_tokens":15,"output_tokens":0,"cache_read_input_tokens":1028})")) {
    JsonRecord value;
    rt::objectDefine(value, "type", json::string("message_start"));
    rt::objectDefine(value, "message", message(json::array(), std::move(usage)));
    return value;
  }

  std::vector<JsonRecord> textEvents() {
    return {
      start(),
      obj(R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"),
      obj(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"O"}})"),
      obj(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"K"}})"),
      obj(R"({"type":"content_block_stop","index":0})"),
      obj(R"({"type":"message_delta","delta":{"stop_reason":"end_turn","stop_sequence":null},"usage":{"output_tokens":4,"buyer_cost_micro":69}})"),
      obj(R"({"type":"message_stop"})"),
    };
  }

  rt::Task<std::vector<JsonRecord>> collect(rt::Response response) {
    const std::optional<rt::ByteStream> body = response.body();
    if (!body) rt::test::report(__FILE__, __LINE__, "expected response.body", true);
    const ResponsesStreamIterator iterator = readResponsesStream(*body);
    std::vector<JsonRecord> events;
    for (;;) {
      rt::IteratorResult<ResponsesStreamEvent> step = co_await iterator.next();
      if (step.done) break;
      events.push_back(step.value->value);
    }
    co_return events;
  }

  // `await normalizeAnthropicResponsesResponse(response, options)` then collect().
  rt::Task<std::vector<JsonRecord>> collectNormalized(rt::Response upstream, AnthropicResponsesOptions options) {
    const rt::Response response = co_await normalizeAnthropicResponsesResponse(upstream, std::move(options));
    std::vector<JsonRecord> events = co_await collect(response);
    co_return events;
  }

  rt::Task<json::value> normalizedJson(rt::Response upstream, AnthropicResponsesOptions options) {
    const rt::Response response = co_await normalizeAnthropicResponsesResponse(upstream, std::move(options));
    json::value payload = co_await response.json();
    co_return payload;
  }

  AnthropicResponsesOptions modelOnly() { return AnthropicResponsesOptions{.model = model}; }

  // The calls above with `{ model }` options (named locals, never temporaries inside co_await).
  rt::Task<rt::Response> normalizedResponse(rt::Response upstream) {
    AnthropicResponsesOptions options = modelOnly();
    rt::Response response = co_await normalizeAnthropicResponsesResponse(upstream, std::move(options));
    co_return response;
  }
  rt::Task<std::vector<JsonRecord>> collectNormalized(rt::Response upstream) {
    AnthropicResponsesOptions options = modelOnly();
    std::vector<JsonRecord> events = co_await collectNormalized(upstream, std::move(options));
    co_return events;
  }
  rt::Task<json::value> normalizedJson(rt::Response upstream) {
    AnthropicResponsesOptions options = modelOnly();
    json::value payload = co_await normalizedJson(upstream, std::move(options));
    co_return payload;
  }

  const JsonRecord& terminalResponse(const std::vector<JsonRecord>& events) { return rt::get(events.back(), "response")->as_object(); }

  std::optional<std::string> stringAt(const json::value& value, std::string_view key) {
    const std::optional<std::string_view> found = rt::getString(value, key);
    return found ? std::optional<std::string>(std::string(*found)) : std::nullopt;
  }
  std::optional<std::string> stringAt(const JsonRecord& value, std::string_view key) { return stringAt(json::value(value), key); }
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

  std::vector<std::string> typesOf(const std::vector<JsonRecord>& events) {
    std::vector<std::string> types;
    for (const JsonRecord& event : events) types.push_back(stringAt(event, "type").value_or(""));
    return types;
  }

  bool includesText(const json::value& value, std::string_view needle) { return rt::jsonStringify(value).find(needle) != std::string::npos; }

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

  // restoreAnthropicReplay(...) deepEqual [{ type: "native_assistant", blocks }].
  bool restoresBlocks(const json::array& output, const std::string& scope, const std::string& provider, const json::value& blocks) {
    const auto restored = restoreAnthropicReplay(output, {.scope = scope, .provider = provider, .model = model});
    if (!restored || restored->size() != 1) return false;
    const auto* native = std::get_if<NativeReplayEntryNativeAssistant>(&restored->front());
    return native && rt::deepEqual(json::value(native->blocks), blocks);
  }
}

ASYNC_TEST("Anthropic Responses restores Codex namespace tool names") {
  const CredentialIsolation isolation;
  const rt::Response upstream(rt::BodyInit(rt::jsonStringify(json::value(message(val(R"([{"type":"tool_use","id":"toolu_1","name":"clock_curr_time","input":{}}])").as_array(),
                                                                                   obj(R"({"input_tokens":4,"output_tokens":2})"), "tool_use")))),
                              rt::ResponseInit{.status = std::nullopt, .statusText = std::nullopt, .headers = rt::Headers{{"content-type", "application/json"}}});
  AnthropicResponsesOptions options = modelOnly();
  options.toolNames = rt::OrderedMap<std::string, ClaudeToolName>{{"clock_curr_time", ClaudeToolName{.name = "curr_time", .namespace_ = "clock"}}};
  const json::value payload = co_await normalizedJson(upstream, std::move(options));
  EXPECT_EQ(stringAt(at(at(payload, "output"), 0), "type"), std::optional<std::string>("function_call"));
  EXPECT_EQ(stringAt(at(at(payload, "output"), 0), "name"), std::optional<std::string>("curr_time"));
  EXPECT_EQ(stringAt(at(at(payload, "output"), 0), "namespace"), std::optional<std::string>("clock"));
}

TEST("Anthropic Responses usage counts native cache-fill input once and preserves unknown reads") {
  const CredentialIsolation isolation;
  const std::optional<JsonRecord> usage = toAnthropicResponsesUsage(val(R"({"input_tokens":15,"output_tokens":4,"cache_creation_input_tokens":1028,"buyer_cost_micro":1056})"));
  ASSERT(usage);
  EXPECT(rt::deepEqual(json::value(*usage),
                       val(R"({"input_tokens":1043,"input_tokens_details":{"cache_write_tokens":1028},"output_tokens":4,"total_tokens":1047,"buyer_cost_micro":1056})")));
  const auto parsed = uos::openai_telemetry::extractUsageTokens(json::value(*usage));
  ASSERT(parsed);
  EXPECT_EQ(parsed->inputTokens, std::optional<double>(1043));
  EXPECT_EQ(parsed->cacheWriteInputTokens, std::optional<double>(1028));
  EXPECT_EQ(parsed->cachedInputTokens, std::nullopt);
}

TEST("Anthropic Responses usage preserves measured reads and original buyer micro-cost") {
  const CredentialIsolation isolation;
  const std::optional<JsonRecord> usage = toAnthropicResponsesUsage(val(R"({"input_tokens":15,"output_tokens":4,"cache_read_input_tokens":1028,"buyer_cost_micro":69})"));
  ASSERT(usage);
  EXPECT(rt::deepEqual(json::value(*usage),
                       val(R"({"input_tokens":1043,"input_tokens_details":{"cached_tokens":1028},"output_tokens":4,"total_tokens":1047,"buyer_cost_micro":69})")));
  const auto parsed = uos::openai_telemetry::extractUsageTokens(json::value(*usage));
  EXPECT_EQ(parsed ? parsed->cacheWriteInputTokens : std::optional<double>(), std::nullopt);
}

TEST("Anthropic Responses usage adds mixed reads and writes without double counting") {
  const CredentialIsolation isolation;
  const std::optional<JsonRecord> usage = toAnthropicResponsesUsage(val(R"({"input_tokens":15,"output_tokens":4,"cache_read_input_tokens":1000,"cache_creation_input_tokens":28})"));
  ASSERT(usage);
  EXPECT(rt::deepEqual(json::value(*usage),
                       val(R"({"input_tokens":1043,"input_tokens_details":{"cached_tokens":1000,"cache_write_tokens":28},"output_tokens":4,"total_tokens":1047})")));
}

TEST("Anthropic Responses usage does not invent absent cache counters or zero invalid counts") {
  const CredentialIsolation isolation;
  EXPECT(rt::deepEqual(json::value(*toAnthropicResponsesUsage(val(R"({"input_tokens":15,"output_tokens":4})"))), val(R"({"input_tokens":15,"output_tokens":4,"total_tokens":19})")));
  const std::vector<json::value> invalids{rt::makeNumber(-1.0), rt::makeNumber(0.5), json::value("1028"), json::value(nullptr),
                                          rt::makeNumber(std::numeric_limits<double>::quiet_NaN()), rt::makeNumber(std::numeric_limits<double>::infinity())};
  for (const json::value& invalid : invalids) {
    JsonRecord input = obj(R"({"input_tokens":15,"output_tokens":4})");
    input["cache_read_input_tokens"] = invalid;
    const std::optional<JsonRecord> converted = toAnthropicResponsesUsage(json::value(input));
    ASSERT(converted);
    EXPECT(rt::get(*converted, "input_tokens")->is_null());
    EXPECT(rt::get(*converted, "total_tokens")->is_null());
    EXPECT(rt::deepEqual(*rt::get(*converted, "input_tokens_details"), val(R"({"cached_tokens":null})")));
  }
  JsonRecord huge = obj(R"({"output_tokens":4,"cache_read_input_tokens":1})");
  huge["input_tokens"] = rt::makeNumber(rt::Number::MAX_SAFE_INTEGER);
  EXPECT(rt::get(*toAnthropicResponsesUsage(json::value(huge)), "input_tokens")->is_null());
  EXPECT(rt::get(*toAnthropicResponsesUsage(val(R"({"input_tokens":15,"output_tokens":-1})")), "output_tokens")->is_null());
  EXPECT(rt::get(*toAnthropicResponsesUsage(val(R"({"input_tokens":"15","output_tokens":4})")), "input_tokens")->is_null());
  EXPECT(!toAnthropicResponsesUsage(val("null")));
}

TEST("Anthropic Responses usage preserves malformed authoritative costs for fail-closed settlement") {
  const CredentialIsolation isolation;
  const std::vector<json::value> costs{json::value(nullptr), rt::makeNumber(-1.0), json::value("69"), rt::makeNumber(0.5), val(R"({"amount":69})")};
  for (const json::value& cost : costs) {
    JsonRecord input = obj(R"({"input_tokens":15,"output_tokens":4})");
    input["buyer_cost_micro"] = cost;
    const std::optional<JsonRecord> usage = toAnthropicResponsesUsage(json::value(input));
    ASSERT(usage);
    EXPECT(rt::deepEqual(*rt::get(*usage, "buyer_cost_micro"), cost));
  }
}

TEST("Anthropic Responses maps native thinking tokens to canonical reasoning usage") {
  const CredentialIsolation isolation;
  EXPECT(rt::deepEqual(
    json::value(*toAnthropicResponsesUsage(val(R"({"input_tokens":15,"cache_read_input_tokens":1028,"output_tokens":64,"output_tokens_details":{"thinking_tokens":60}})"))),
    val(R"({"input_tokens":1043,"input_tokens_details":{"cached_tokens":1028},"output_tokens":64,"output_tokens_details":{"reasoning_tokens":60},"total_tokens":1107})")));
  const std::vector<json::value> thinkings{rt::makeNumber(-1.0), rt::makeNumber(0.5), json::value("60"), rt::makeNumber(65.0)};
  for (const json::value& thinking : thinkings) {
    JsonRecord details;
    details["thinking_tokens"] = thinking;
    JsonRecord input = obj(R"({"input_tokens":15,"output_tokens":64})");
    input["output_tokens_details"] = details;
    EXPECT(rt::deepEqual(*rt::get(*toAnthropicResponsesUsage(json::value(input)), "output_tokens_details"), val(R"({"reasoning_tokens":null})")));
  }
}

ASYNC_TEST("Anthropic Responses JSON maps text, cache read and cost provenance") {
  const CredentialIsolation isolation;
  const rt::Response upstream(
    rt::BodyInit(rt::jsonStringify(json::value(message(val(R"([{"type":"text","text":"OK"}])").as_array(),
                                                       obj(R"({"input_tokens":15,"output_tokens":4,"cache_read_input_tokens":1028,"buyer_cost_micro":69})"))))),
    rt::ResponseInit{.status = std::nullopt,
                     .statusText = std::nullopt,
                     .headers = rt::Headers{{"content-type", "application/json"}, {"content-length", "999"}, {"x-cost-micro", "69"}, {"x-request-id", "native-request"}}});
  const rt::Response response = co_await normalizedResponse(upstream);
  const json::value payload = co_await response.json();
  EXPECT_EQ(stringAt(payload, "object"), std::optional<std::string>("response"));
  EXPECT_EQ(stringAt(payload, "model"), std::optional<std::string>(model));
  EXPECT_EQ(stringAt(payload, "status"), std::optional<std::string>("completed"));
  EXPECT_EQ(stringAt(at(at(at(at(payload, "output"), 0), "content"), 0), "text"), std::optional<std::string>("OK"));
  EXPECT_EQ(rt::getNumber(at(payload, "usage"), "input_tokens"), std::optional<double>(1043));
  EXPECT_EQ(rt::getNumber(at(at(payload, "usage"), "input_tokens_details"), "cached_tokens"), std::optional<double>(1028));
  EXPECT_EQ(rt::getNumber(at(payload, "usage"), "buyer_cost_micro"), std::optional<double>(69));
  EXPECT_EQ(response.headers().get("x-cost-micro"), std::optional<std::string>("69"));
  EXPECT_EQ(response.headers().get("x-request-id"), std::optional<std::string>("native-request"));
  EXPECT_EQ(response.headers().get("content-length"), std::nullopt);
}

ASYNC_TEST("Anthropic Responses JSON preserves tool IDs and arguments without exposing thinking signatures") {
  const CredentialIsolation isolation;
  const JsonRecord native = message(val(R"([{"type":"thinking","thinking":"Use the requested tool.","signature":"provider-private-signature"},{"type":"tool_use","id":"toolu_123","name":"weather","input":{"city":"Paris"}}])")
                                      .as_array(),
                                    obj(R"({"input_tokens":5,"output_tokens":10})"), "tool_use");
  const rt::Response upstream{rt::BodyInit(rt::jsonStringify(json::value(native)))};
  const json::value payload = co_await normalizedJson(upstream);
  std::vector<std::string> types;
  for (const json::value& item : at(payload, "output").as_array()) types.push_back(stringAt(item, "type").value_or(""));
  EXPECT_EQ(types, (std::vector<std::string>{"reasoning", "function_call"}));
  EXPECT(rt::deepEqual(at(at(at(payload, "output"), 0), "summary"), val(R"([{"type":"summary_text","text":"Use the requested tool."}])")));
  EXPECT_EQ(stringAt(at(at(payload, "output"), 1), "call_id"), std::optional<std::string>("toolu_123"));
  EXPECT_EQ(stringAt(at(at(payload, "output"), 1), "name"), std::optional<std::string>("weather"));
  EXPECT_EQ(stringAt(at(at(payload, "output"), 1), "arguments"), std::optional<std::string>("{\"city\":\"Paris\"}"));
  EXPECT_EQ(includesText(payload, "provider-private-signature"), false);
  EXPECT_EQ(includesText(payload, "encrypted_content"), false);
  EXPECT(rt::Regex("^rs_resp_[A-Za-z0-9]{1,40}_\\d+$").test(stringAt(at(at(payload, "output"), 0), "id").value_or("")));
  std::vector<uos::types::ResponseInputItem> input;
  for (const json::value& item : at(payload, "output").as_array()) input.push_back(json::value_to<uos::types::ResponseInputItem>(item));
  const auto replay = uos::models::codex_models_fetch::buildCodexRequest(model, input);
  EXPECT(rt::deepEqual(*rt::get(replay, "input"), val(R"([{"type":"function_call","status":"completed","call_id":"toolu_123","name":"weather","arguments":"{\"city\":\"Paris\"}"}])")));
}

ASYNC_TEST("Anthropic Responses redacted thinking stays opaque rather than being forged into an OpenAI replay payload") {
  const CredentialIsolation isolation;
  const JsonRecord native =
    message(val(R"([{"type":"redacted_thinking","data":"opaque"},{"type":"text","text":"OK"}])").as_array(), obj(R"({"input_tokens":5,"output_tokens":4})"));
  const rt::Response upstream{rt::BodyInit(rt::jsonStringify(json::value(native)))};
  const json::value payload = co_await normalizedJson(upstream);
  EXPECT_EQ(at(payload, "output").as_array().size(), std::size_t{1});
  EXPECT_EQ(stringAt(at(at(payload, "output"), 0), "type"), std::optional<std::string>("message"));
  EXPECT_EQ(includesText(payload, "opaque"), false);
}

ASYNC_TEST("Anthropic Responses respects an explicit reasoning summary none") {
  const CredentialIsolation isolation;
  const JsonRecord native = message(val(R"([{"type":"thinking","thinking":"Do not expose this.","signature":"private"},{"type":"text","text":"OK"}])").as_array(),
                                    obj(R"({"input_tokens":5,"output_tokens":4})"));
  const rt::Response upstream{rt::BodyInit(rt::jsonStringify(json::value(native)))};
  AnthropicResponsesOptions options = modelOnly();
  options.reasoningSummary = "none";
  const json::value payload = co_await normalizedJson(upstream, std::move(options));
  std::vector<std::string> types;
  for (const json::value& item : at(payload, "output").as_array()) types.push_back(stringAt(item, "type").value_or(""));
  EXPECT_EQ(types, (std::vector<std::string>{"message"}));
  EXPECT_EQ(includesText(payload, "Do not expose this."), false);
}

ASYNC_TEST("Anthropic Responses JSON max_tokens is incomplete and unknown stops are failed") {
  const CredentialIsolation isolation;
  const std::vector<std::pair<std::string, std::string>> cases{{"max_tokens", "incomplete"}, {"pause_turn", "failed"}, {"unexpected", "failed"}};
  for (const auto& [reason, status] : cases) {
    const rt::Response upstream(
      rt::BodyInit(rt::jsonStringify(json::value(message(val(R"([{"type":"text","text":"partial"}])").as_array(), obj(R"({"input_tokens":5,"output_tokens":4})"), reason)))));
    const json::value payload = co_await normalizedJson(upstream);
    EXPECT_EQ(stringAt(payload, "status"), std::optional<std::string>(status));
    if (status == "incomplete") EXPECT(rt::deepEqual(at(payload, "incomplete_details"), val(R"({"reason":"max_output_tokens"})")));
    else EXPECT_EQ(stringAt(at(payload, "error"), "code"), std::optional<std::string>("anthropic_unrecognized_stop_reason"));
  }
}

ASYNC_TEST("Anthropic Responses SSE emits full text lifecycle and gross terminal usage") {
  const CredentialIsolation isolation;
  const rt::Response streamed = streamResponse(textEvents());

  const std::vector<JsonRecord> events = co_await collectNormalized(streamed);
  EXPECT_EQ(typesOf(events), (std::vector<std::string>{"response.created", "response.in_progress", "response.output_item.added", "response.content_part.added",
                                                       "response.output_text.delta", "response.output_text.delta", "response.output_text.done",
                                                       "response.content_part.done", "response.output_item.done", "response.in_progress", "response.completed"}));
  for (std::size_t index = 0; index < events.size(); ++index) EXPECT_EQ(rt::getNumber(events[index], "sequence_number"), std::optional<double>(static_cast<double>(index)));
  EXPECT_EQ(stringAt(terminalResponse(events), "status"), std::optional<std::string>("completed"));
  EXPECT(rt::deepEqual(*rt::get(terminalResponse(events), "usage"),
                       val(R"({"input_tokens":1043,"input_tokens_details":{"cached_tokens":1028},"output_tokens":4,"total_tokens":1047,"buyer_cost_micro":69})")));
}

ASYNC_TEST("Anthropic Responses SSE closes signed thinking before tool calls and preserves fragmented arguments") {
  const CredentialIsolation isolation;
  const std::vector<JsonRecord> native{
    start(),
    obj(R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":""}})"),
    obj(R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Use a tool."}})"),
    obj(R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"private"}})"),
    obj(R"({"type":"content_block_stop","index":0})"),
    obj(R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_1","name":"weather","input":{}}})"),
    obj(R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"city\":"}})"),
    obj(R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"\"Paris\"}"}})"),
    obj(R"({"type":"content_block_stop","index":1})"),
    obj(R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":20}})"),
    obj(R"({"type":"message_stop"})"),
  };
  const rt::Response streamed = streamResponse(native);

  const std::vector<JsonRecord> events = co_await collectNormalized(streamed);
  std::ptrdiff_t doneReasoning = -1, addedTool = -1;
  for (std::size_t index = 0; index < events.size(); ++index) {
    const json::value* item = rt::get(events[index], "item");
    if (doneReasoning < 0 && stringAt(events[index], "type") == std::optional<std::string>("response.output_item.done") && item &&
        stringAt(*item, "type") == std::optional<std::string>("reasoning"))
      doneReasoning = static_cast<std::ptrdiff_t>(index);
    if (addedTool < 0 && stringAt(events[index], "type") == std::optional<std::string>("response.output_item.added") && item &&
        stringAt(*item, "type") == std::optional<std::string>("function_call"))
      addedTool = static_cast<std::ptrdiff_t>(index);
  }
  EXPECT(doneReasoning < addedTool);
  const json::array& output = rt::get(terminalResponse(events), "output")->as_array();
  std::vector<std::string> types;
  for (const json::value& item : output) types.push_back(stringAt(item, "type").value_or(""));
  EXPECT_EQ(types, (std::vector<std::string>{"reasoning", "function_call"}));
  EXPECT_EQ(stringAt(output.at(1), "arguments"), std::optional<std::string>("{\"city\":\"Paris\"}"));
  EXPECT_EQ(stringAt(output.at(1), "call_id"), std::optional<std::string>("toolu_1"));
  int argumentDeltas = 0;
  for (const JsonRecord& event : events)
    if (stringAt(event, "type") == std::optional<std::string>("response.function_call_arguments.delta")) argumentDeltas += 1;
  EXPECT_EQ(argumentDeltas, 2);
  json::array all;
  for (const JsonRecord& event : events) all.push_back(event);
  EXPECT_EQ(includesText(json::value(all), "private"), false);
  EXPECT_EQ(includesText(json::value(all), "encrypted_content"), false);
}

ASYNC_TEST("Anthropic Responses SSE handles initial text, multiple text blocks and zero-argument tools") {
  const CredentialIsolation isolation;
  const std::vector<JsonRecord> native{
    start(),
    obj(R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":"First."}})"),
    obj(R"({"type":"content_block_stop","index":0})"),
    obj(R"({"type":"content_block_start","index":1,"content_block":{"type":"text","text":"Second."}})"),
    obj(R"({"type":"content_block_stop","index":1})"),
    obj(R"({"type":"content_block_start","index":2,"content_block":{"type":"tool_use","id":"toolu_empty","name":"clock","input":{}}})"),
    obj(R"({"type":"content_block_stop","index":2})"),
    obj(R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":10}})"),
    obj(R"({"type":"message_stop"})"),
  };
  const rt::Response streamed = streamResponse(native);

  const std::vector<JsonRecord> events = co_await collectNormalized(streamed);
  const json::array& output = rt::get(terminalResponse(events), "output")->as_array();
  std::vector<std::string> types;
  for (const json::value& item : output) types.push_back(stringAt(item, "type").value_or(""));
  EXPECT_EQ(types, (std::vector<std::string>{"message", "message", "function_call"}));
  EXPECT_EQ(stringAt(output.at(2), "arguments"), std::optional<std::string>("{}"));
  std::vector<double> indexes;
  for (const JsonRecord& event : events)
    if (stringAt(event, "type") == std::optional<std::string>("response.output_item.added")) indexes.push_back(rt::getNumber(event, "output_index").value_or(-1));
  EXPECT_EQ(indexes, (std::vector<double>{0, 1, 2}));
}

ASYNC_TEST("Anthropic Responses SSE handles fragmented UTF-8, CRLF, ping and trailing message_stop") {
  const CredentialIsolation isolation;
  std::vector<JsonRecord> native = textEvents();
  native[2] = obj("{\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"\xF0\x9F\x8C\xA4\"}}");
  native.insert(native.begin() + 1, obj(R"({"type":"ping"})"));
  const rt::ByteChunk encoded = rt::encodeUtf8(rt::trimEnd(sse(native, "\r\n\r\n")));
  auto position = std::make_shared<std::size_t>(0);
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.pull = [encoded, position](Controller controller) {
    if (*position == encoded.size()) {
      controller.close();
    } else {
      controller.enqueue(rt::ByteChunk(encoded.begin() + static_cast<std::ptrdiff_t>(*position), encoded.begin() + static_cast<std::ptrdiff_t>(*position + 1)));
      *position += 1;
    }
  };
  const rt::Response upstream(rt::BodyInit(rt::ByteStream(std::move(source))),
                              rt::ResponseInit{.status = std::nullopt, .statusText = std::nullopt, .headers = rt::Headers{{"content-type", "text/event-stream"}}});
  const std::vector<JsonRecord> events = co_await collectNormalized(upstream);
  const json::array& output = rt::get(terminalResponse(events), "output")->as_array();
  EXPECT_EQ(stringAt(output.at(0).as_object().at("content").as_array().at(0), "text"), std::optional<std::string>("\xF0\x9F\x8C\xA4K"));
}

ASYNC_TEST("Anthropic Responses SSE max_tokens emits incomplete, not completed") {
  const CredentialIsolation isolation;
  std::vector<JsonRecord> native = textEvents();
  native[5] = obj(R"({"type":"message_delta","delta":{"stop_reason":"max_tokens"},"usage":{"output_tokens":4}})");
  const rt::Response streamed = streamResponse(native);

  const std::vector<JsonRecord> events = co_await collectNormalized(streamed);
  EXPECT_EQ(stringAt(events.back(), "type"), std::optional<std::string>("response.incomplete"));
  EXPECT(rt::deepEqual(*rt::get(terminalResponse(events), "incomplete_details"), val(R"({"reason":"max_output_tokens"})")));
}

ASYNC_TEST("Anthropic Responses preserves HTTP error status and translates native SSE errors") {
  const CredentialIsolation isolation;
  const rt::Response errorResponse(rt::BodyInit(std::string(R"({"type":"error","error":{"type":"overloaded_error","message":"capacity"}})")),
                                   rt::ResponseInit{.status = 529, .statusText = std::nullopt, .headers = std::nullopt});
  const rt::Response normalized = co_await normalizedResponse(errorResponse);
  EXPECT_EQ(normalized.status(), 529);
  const json::value body = co_await normalized.json();
  EXPECT(rt::deepEqual(body, val(R"({"error":{"type":"overloaded_error","message":"capacity","code":"overloaded_error"}})")));
  const std::vector<JsonRecord> errorEvents{obj(R"({"type":"error","error":{"type":"overloaded_error","message":"capacity"}})")};
  const rt::Response streamed = streamResponse(errorEvents);
  const std::vector<JsonRecord> events = co_await collectNormalized(streamed);
  ASSERT_EQ(events.size(), std::size_t{1});
  EXPECT(rt::deepEqual(json::value(events[0]), val(R"({"type":"error","code":"overloaded_error","message":"capacity","param":null,"sequence_number":0})")));
}

ASYNC_TEST("Anthropic Responses HTTP error normalization retains provider codes and retry/cost provenance") {
  const CredentialIsolation isolation;
  const rt::Response upstream(
    rt::BodyInit(std::string(R"({"type":"error","error":{"type":"quota_error","code":"insufficient_funds","message":"capacity","param":null}})")),
    rt::ResponseInit{.status = 429,
                     .statusText = std::nullopt,
                     .headers = rt::Headers{{"content-type", "application/json"}, {"retry-after", "60"}, {"x-cost-micro", "0"}, {"x-request-id", "native-error"}}});
  const rt::Response normalized = co_await normalizedResponse(upstream);
  EXPECT_EQ(normalized.status(), 429);
  EXPECT_EQ(normalized.headers().get("retry-after"), std::optional<std::string>("60"));
  EXPECT_EQ(normalized.headers().get("x-cost-micro"), std::optional<std::string>("0"));
  EXPECT_EQ(normalized.headers().get("x-request-id"), std::optional<std::string>("native-error"));
  const json::value body = co_await normalized.json();
  EXPECT(rt::deepEqual(body, val(R"({"error":{"type":"quota_error","code":"insufficient_funds","message":"capacity","param":null}})")));
  const rt::Response unrecognized(rt::BodyInit(std::string("unrecognized upstream body")), rt::ResponseInit{.status = 502, .statusText = std::nullopt, .headers = std::nullopt});
  const rt::Response passedThrough = co_await normalizedResponse(unrecognized);
  EXPECT(passedThrough == unrecognized);
  const std::string text = co_await unrecognized.text();
  EXPECT_EQ(text, std::string("unrecognized upstream body"));
}

ASYNC_TEST("Anthropic Responses fails closed on malformed JSON, unknown blocks and invalid message lifecycles") {
  const CredentialIsolation isolation;
  const std::vector<std::string> bodies{"{", R"({"type":"message","role":"assistant","content":[{"type":"unknown","data":"x"}]})", R"({"type":"message","role":"user","content":[]})"};
  for (const std::string& body : bodies) {
    const rt::Response upstream(rt::BodyInit{body});
    rt::Task<rt::Response> normalized = normalizeAnthropicResponsesResponse(upstream, modelOnly());
    const std::optional<std::string> kind = co_await rejectionKind(std::move(normalized));
    EXPECT_EQ(kind, std::optional<std::string>("malformed_event"));
  }
  const std::vector<std::vector<JsonRecord>> natives{
    {start(), obj(R"({"type":"content_block_delta","index":0,"delta":{"type":"text_delta","text":"x"}})")},
    {start(), obj(R"({"type":"content_block_start","index":0,"content_block":{"type":"text","text":""}})"), obj(R"({"type":"message_stop"})")},
    {start(), obj(R"({"type":"content_block_start","index":-1,"content_block":{"type":"text","text":""}})")},
  };
  for (const std::vector<JsonRecord>& native : natives) {
    rt::Task<std::vector<JsonRecord>> collected = collectNormalized(streamResponse(native), modelOnly());
    const std::optional<std::string> kind = co_await rejectionKind(std::move(collected));
    EXPECT_EQ(kind, std::optional<std::string>("malformed_event"));
  }
}

ASYNC_TEST("Anthropic Responses detects premature EOF instead of manufacturing completed usage") {
  const CredentialIsolation isolation;
  std::vector<JsonRecord> native = textEvents();
  native.pop_back();
  rt::Task<std::vector<JsonRecord>> collected = collectNormalized(streamResponse(native), modelOnly());
  const std::optional<std::string> kind = co_await rejectionKind(std::move(collected));
  EXPECT_EQ(kind, std::optional<std::string>("premature_eof"));
}

ASYNC_TEST("Anthropic Responses propagates client cancellation to the upstream reader") {
  const CredentialIsolation isolation;
  auto cancelled = std::make_shared<bool>(false);
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.start = [](Controller controller) { controller.enqueue(rt::encodeUtf8(sse({start()}))); };
  source.cancel = [cancelled](rt::Reason) { *cancelled = true; };
  const rt::Response upstream(rt::BodyInit(rt::ByteStream(std::move(source))),
                              rt::ResponseInit{.status = std::nullopt, .statusText = std::nullopt, .headers = rt::Headers{{"content-type", "text/event-stream"}}});
  const rt::Response response = co_await normalizedResponse(upstream);
  const std::optional<rt::ByteStream> body = response.body();
  ASSERT(body);
  const auto reader = body->getReader();
  co_await reader.read();
  const rt::Reason reason = rt::thrown(json::value("client stopped"));
  co_await reader.cancel(reason);
  EXPECT_EQ(*cancelled, true);
  reader.releaseLock();
}

ASYNC_TEST("Anthropic Responses preserves an external abort while a native stream stalls") {
  const CredentialIsolation isolation;
  auto cancelled = std::make_shared<bool>(false);
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.cancel = [cancelled](rt::Reason) { *cancelled = true; };
  const rt::AbortController cancellation;
  const rt::Response upstream(rt::BodyInit(rt::ByteStream(std::move(source))),
                              rt::ResponseInit{.status = std::nullopt, .statusText = std::nullopt, .headers = rt::Headers{{"content-type", "text/event-stream"}}});
  AnthropicResponsesOptions options = modelOnly();
  options.signal = cancellation.signal();
  const rt::Response response = co_await normalizeAnthropicResponsesResponse(upstream, std::move(options));
  const std::optional<rt::ByteStream> body = response.body();
  ASSERT(body);
  const auto reader = body->getReader();
  rt::Task<rt::ReadableStreamReadResult<rt::ByteChunk>> pending = reader.read();
  cancellation.abort(rt::makeReason(rt::DOMException("requested stop", "AbortError")));
  bool matched = false;
  try {
    co_await std::move(pending);
  } catch (const rt::DOMException& error) {
    matched = error.name == "AbortError" && error.message == "requested stop";
  } catch (...) {
  }
  EXPECT(matched);
  EXPECT_EQ(*cancelled, true);
  reader.releaseLock();
}

ASYNC_TEST("Anthropic Responses cancellation before its first pull still cancels the upstream") {
  const CredentialIsolation isolation;
  auto cancelled = std::make_shared<bool>(false);
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.cancel = [cancelled](rt::Reason) { *cancelled = true; };
  const rt::Response upstream(rt::BodyInit(rt::ByteStream(std::move(source))),
                              rt::ResponseInit{.status = std::nullopt, .statusText = std::nullopt, .headers = rt::Headers{{"content-type", "text/event-stream"}}});
  const rt::Response response = co_await normalizedResponse(upstream);
  const std::optional<rt::ByteStream> body = response.body();
  ASSERT(body);
  const rt::Reason reason = rt::thrown(json::value("stop immediately"));
  co_await body->cancel(reason);
  EXPECT_EQ(*cancelled, true);
}

ASYNC_TEST("Anthropic Responses JSON privately preserves signed assistant blocks for scoped replay") {
  const CredentialIsolation isolation;
  const std::string scope = "principal_" + rt::crypto::randomUUID();
  const json::value content = val(
    R"([{"type":"thinking","thinking":"Use the requested tool.","signature":"opaque-signature"},{"type":"text","text":"Checking the weather."},{"type":"tool_use","id":"toolu_replay","name":"weather","input":{"city":"Paris"}}])");
  const rt::Response upstream(rt::BodyInit(rt::jsonStringify(json::value(message(content.as_array(), obj(R"({"input_tokens":5,"output_tokens":20})"), "tool_use")))));
  AnthropicResponsesOptions options = modelOnly();
  options.provider = "surplus";
  options.reasoningReplayScope = scope;
  const json::value payload = co_await normalizedJson(upstream, std::move(options));
  const json::array& output = at(payload, "output").as_array();
  EXPECT(restoresBlocks(output, scope, "surplus", content));
  EXPECT_EQ(includesText(payload, "opaque-signature"), false);
  EXPECT_EQ(includesText(payload, "encrypted_content"), false);
  EXPECT(!restoreAnthropicReplay(output, {.scope = "another-principal", .provider = "surplus", .model = model}));
  EXPECT(!restoreAnthropicReplay(output, {.scope = scope, .provider = "metered", .model = model}));
}

ASYNC_TEST("Anthropic Responses SSE privately reconstructs fragmented thinking signatures and tool arguments") {
  const CredentialIsolation isolation;
  const std::string scope = "principal_" + rt::crypto::randomUUID();
  const std::vector<JsonRecord> native{
    start(),
    obj(R"({"type":"content_block_start","index":0,"content_block":{"type":"thinking","thinking":"Initial. ","signature":"prefix-"}})"),
    obj(R"({"type":"content_block_delta","index":0,"delta":{"type":"thinking_delta","thinking":"Use a tool."}})"),
    obj(R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"sig"}})"),
    obj(R"({"type":"content_block_delta","index":0,"delta":{"type":"signature_delta","signature":"nature"}})"),
    obj(R"({"type":"content_block_stop","index":0})"),
    obj(R"({"type":"content_block_start","index":1,"content_block":{"type":"tool_use","id":"toolu_signed","name":"weather","input":{}}})"),
    obj(R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"{\"city\":"}})"),
    obj(R"({"type":"content_block_delta","index":1,"delta":{"type":"input_json_delta","partial_json":"\"Paris\"}"}})"),
    obj(R"({"type":"content_block_stop","index":1})"),
    obj(R"({"type":"message_delta","delta":{"stop_reason":"tool_use"},"usage":{"output_tokens":20}})"),
    obj(R"({"type":"message_stop"})"),
  };
  AnthropicResponsesOptions options = modelOnly();
  options.provider = "surplus";
  options.reasoningReplayScope = scope;
  const rt::Response streamed = streamResponse(native);
  const std::vector<JsonRecord> events = co_await collectNormalized(streamed, std::move(options));
  const json::array& output = rt::get(terminalResponse(events), "output")->as_array();
  EXPECT(restoresBlocks(output, scope, "surplus",
                        val(R"([{"type":"thinking","thinking":"Initial. Use a tool.","signature":"prefix-signature"},{"type":"tool_use","id":"toolu_signed","name":"weather","input":{"city":"Paris"}}])")));
  json::array all;
  for (const JsonRecord& event : events) all.push_back(event);
  EXPECT_EQ(includesText(json::value(all), "prefix-signature"), false);
}

ASYNC_TEST("Anthropic Responses summary none retains private signed and redacted blocks for native tool continuation") {
  const CredentialIsolation isolation;
  const std::string scope = "principal_" + rt::crypto::randomUUID();
  const json::value content = val(
    R"([{"type":"redacted_thinking","data":"opaque-redacted"},{"type":"thinking","thinking":"Hidden summary.","signature":"private-signature"},{"type":"tool_use","id":"toolu_hidden","name":"clock","input":{}}])");
  const rt::Response upstream(rt::BodyInit(rt::jsonStringify(json::value(message(content.as_array(), obj(R"({"input_tokens":5,"output_tokens":20})"), "tool_use")))));
  AnthropicResponsesOptions options = modelOnly();
  options.provider = "metered";
  options.reasoningReplayScope = scope;
  options.reasoningSummary = "none";
  const json::value payload = co_await normalizedJson(upstream, std::move(options));
  std::vector<std::string> types;
  for (const json::value& item : at(payload, "output").as_array()) types.push_back(stringAt(item, "type").value_or(""));
  EXPECT_EQ(types, (std::vector<std::string>{"function_call"}));
  EXPECT(restoresBlocks(at(payload, "output").as_array(), scope, "metered", content));
  EXPECT_EQ(includesText(payload, "private-signature"), false);
  EXPECT_EQ(includesText(payload, "opaque-redacted"), false);
  EXPECT_EQ(includesText(payload, "Hidden summary."), false);
}

ASYNC_TEST("Anthropic Responses does not retain incomplete signed turns as native replay receipts") {
  const CredentialIsolation isolation;
  const std::string scope = "principal_" + rt::crypto::randomUUID();
  const json::value content =
    val(R"([{"type":"thinking","thinking":"Partial.","signature":"incomplete-signature"},{"type":"tool_use","id":"toolu_partial","name":"clock","input":{}}])");
  const rt::Response upstream(rt::BodyInit(rt::jsonStringify(json::value(message(content.as_array(), obj(R"({"input_tokens":5,"output_tokens":20})"), "max_tokens")))));
  AnthropicResponsesOptions options = modelOnly();
  options.provider = "surplus";
  options.reasoningReplayScope = scope;
  const json::value payload = co_await normalizedJson(upstream, std::move(options));
  EXPECT_EQ(stringAt(payload, "status"), std::optional<std::string>("incomplete"));
  EXPECT(!restoreAnthropicReplay(at(payload, "output").as_array(), {.scope = scope, .provider = "surplus", .model = model}));
}

ASYNC_TEST("Anthropic Messages observed usage survives a subsequent premature stream EOF") {
  const CredentialIsolation isolation;
  std::vector<JsonRecord> native = textEvents();
  native.pop_back();
  const rt::Response streamed = streamResponse(native);
  const rt::Response response = co_await normalizedResponse(streamed);
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
    if (stringAt(event, "type") == std::optional<std::string>("response.in_progress") && eventResponse && !rt::get(*eventResponse, "usage")->is_null()) measured = &event;
  }
  ASSERT(measured);
  EXPECT(rt::deepEqual(*rt::get(*rt::get(*measured, "response"), "usage"),
                       val(R"({"input_tokens":1043,"input_tokens_details":{"cached_tokens":1028},"output_tokens":4,"total_tokens":1047,"buyer_cost_micro":69})")));
  bool completed = false;
  for (const JsonRecord& event : *observed) completed = completed || stringAt(event, "type") == std::optional<std::string>("response.completed");
  EXPECT_EQ(completed, false);
}
