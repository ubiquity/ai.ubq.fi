// Links: tests/openai_telemetry_differential_test.cpp src/openai_telemetry.cpp src/auth/relay.cpp src/codex/responses_error.cpp src/config.cpp
//        src/http.cpp src/inference_deadline.cpp src/provider/cerebras_rate_limits.cpp src/provider/removed_provider_telemetry.cpp
//        src/release.cpp src/responses_failover_stream.cpp src/responses_stream.cpp src/types.cpp src/utils.cpp
// src/openai-telemetry.ts has direct bun tests only for its usage parsers (cache-usage-normalization, anthropic-chat-responses);
// its other tests drive whole handlers. These differential vectors were recorded from the TS under Bun 1.4.0
// (tests/openai_telemetry_differential_vectors.inc) and are replayed here: usage parsing, prompt-cache request flags, breakpoint
// counting, aggregation (Object.is comparisons, sums, earliest/latest timings, shared state for one source), the recorders in
// sequence with performance.now replaced (the C++ keeps performance.now at 0 and moves startedAtMonotonicMs instead, so every
// elapsed time is the same double), runWithResponseTelemetry's precommit terminal rule, event telemetry, failure classification
// and the error responses.
#include "uos/openai_telemetry.hpp"

#include "uos/codex/auth.hpp"
#include "uos/responses_failover_stream.hpp"
#include "uos/responses_stream.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/types.hpp"

#include <cmath>
#include <functional>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "openai_telemetry_differential_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
namespace telemetry = uos::openai_telemetry;
using telemetry::ResponseTelemetry;
using telemetry::ResponseTelemetryState;
using telemetry::UsageContext;
using telemetry::UsageTokens;

namespace {
  const json::object& vectors() {
    static const json::object all = [] {
      const std::vector<std::uint8_t> packed = rt::fromBase64(kOpenaiTelemetryVectors);
      return rt::jsonParse(rt::decodeUtf8(rt::gzip::gunzipSync(packed))).as_object();
    }();
    return all;
  }
  const json::array& section(std::string_view name) { return rt::get(vectors(), name)->as_array(); }

  // ---- the tagged encoding ------------------------------------------------------------------------------------
  bool isUndefinedTag(const json::value& value) { return value.is_object() && rt::hasOwn(value.get_object(), "$undefined"); }
  bool isNumberTag(const json::value& value) { return value.is_object() && rt::hasOwn(value.get_object(), "$num"); }

  double number(const json::value& value) {
    if (isNumberTag(value)) {
      const std::string text(rt::getString(value, "$num").value());
      if (text == "-0") return -0.0;
      if (text == "NaN") return std::numeric_limits<double>::quiet_NaN();
      return text == "Infinity" ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
    }
    return rt::getNumber(value).value();
  }

  // The tagged JSON as a value: tagged numbers become numbers, undefined members disappear.
  json::value decode(const json::value& value) {
    if (isNumberTag(value)) return rt::makeNumber(number(value));
    if (value.is_array()) {
      json::array out;
      for (const json::value& element : value.get_array()) out.push_back(isUndefinedTag(element) ? json::value(nullptr) : decode(element));
      return out;
    }
    if (value.is_object()) {
      json::object out;
      for (const json::key_value_pair* member : rt::objectEntries(value.get_object())) {
        if (isUndefinedTag(member->value())) continue;
        rt::objectDefine(out, member->key(), decode(member->value()));
      }
      return out;
    }
    return value;
  }

  json::value encNumber(double value) {
    json::object tag;
    if (std::isnan(value)) tag["$num"] = "NaN";
    else if (std::isinf(value)) tag["$num"] = value > 0 ? "Infinity" : "-Infinity";
    else if (value == 0 && std::signbit(value)) tag["$num"] = "-0";
    else return rt::makeNumber(value);
    return tag;
  }
  json::value encJson(const json::value& value) {
    if (rt::isNumber(value)) return encNumber(rt::getNumber(value).value());
    if (value.is_array()) {
      json::array out;
      for (const json::value& element : value.get_array()) out.push_back(encJson(element));
      return out;
    }
    if (value.is_object()) {
      json::object out;
      for (const json::key_value_pair* member : rt::objectEntries(value.get_object())) rt::objectDefine(out, member->key(), encJson(member->value()));
      return out;
    }
    return value;
  }
  json::value encOptional(const std::optional<double>& value) { return value ? encNumber(*value) : json::value(nullptr); }
  json::value encOptional(const std::optional<std::string>& value) { return value ? json::value(json::string(*value)) : json::value(nullptr); }
  json::value encOptional(const std::optional<bool>& value) { return value ? json::value(*value) : json::value(nullptr); }
  json::value encNullable(const rt::Nullable<double>& value) {
    if (value == rt::undefined) return json::object{{"$undefined", true}};
    return value == nullptr ? json::value(nullptr) : encNumber(*value);
  }
  json::value encStrings(const std::vector<std::string>& values) {
    json::array out;
    for (const std::string& value : values) out.emplace_back(json::string(value));
    return out;
  }

  json::value encUsage(const std::optional<UsageTokens>& usage) {
    if (!usage) return nullptr;
    json::object out;
    out["inputTokens"] = encOptional(usage->inputTokens);
    out["cachedInputTokens"] = encOptional(usage->cachedInputTokens);
    out["cacheWriteInputTokens"] = encOptional(usage->cacheWriteInputTokens);
    out["outputTokens"] = encOptional(usage->outputTokens);
    out["totalTokens"] = encOptional(usage->totalTokens);
    out["status"] = json::string(usage->status);
    if (usage->buyerCostMicro != rt::undefined) out["buyerCostMicro"] = encNullable(usage->buyerCostMicro);
    return out;
  }
  json::value encChat(const std::optional<json::object>& chat) { return chat ? encJson(json::value(*chat)) : json::value(nullptr); }

  json::value encTelemetry(const std::optional<ResponseTelemetry>& value) {
    if (!value) return nullptr;
    const ResponseTelemetry& t = *value;
    json::object out;
    out["provider"] = json::string(t.provider);
    out["fallbackReason"] = encOptional(t.fallbackReason);
    out["model"] = encOptional(t.model);
    out["reasoning"] = encOptional(t.reasoning);
    out["outputTokenAllowance"] = encOptional(t.outputTokenAllowance);
    out["rateLimitWaitMs"] = encOptional(t.rateLimitWaitMs);
    out["rateLimitFailoverModel"] = encOptional(t.rateLimitFailoverModel);
    out["inputTokens"] = encOptional(t.inputTokens);
    out["cachedInputTokens"] = encOptional(t.cachedInputTokens);
    out["cacheWriteInputTokens"] = encOptional(t.cacheWriteInputTokens);
    out["outputTokens"] = encOptional(t.outputTokens);
    out["totalTokens"] = encOptional(t.totalTokens);
    out["usageObserved"] = t.usageObserved;
    out["usageTelemetryStatus"] = json::string(t.usageTelemetryStatus);
    out["promptCacheKeyPresent"] = t.promptCacheKeyPresent;
    out["promptCacheMode"] = json::string(t.promptCacheMode);
    out["explicitBreakpointCount"] = encNumber(t.explicitBreakpointCount);
    out["accountSlot"] = encOptional(t.accountSlot);
    out["activeGeneration"] = encOptional(t.activeGeneration);
    out["activeTransitionReason"] = encOptional(t.activeTransitionReason);
    out["quotaUsedPercent"] = encNullable(t.quotaUsedPercent);
    out["completed"] = t.completed;
    out["semanticOutputObserved"] = encOptional(t.semanticOutputObserved);
    out["upstreamEventKinds"] = encStrings(t.upstreamEventKinds);
    out["streamTerminalType"] = encOptional(t.streamTerminalType);
    out["failureKind"] = encOptional(t.failureKind);
    out["responseCreatedObserved"] = t.responseCreatedObserved;
    out["syntheticTerminalType"] = encOptional(t.syntheticTerminalType);
    out["stream"] = encOptional(t.stream);
    out["providerRequestId"] = encOptional(t.providerRequestId);
    out["firstProviderDispatchMs"] = encOptional(t.firstProviderDispatchMs);
    out["firstProviderHeadersMs"] = encOptional(t.firstProviderHeadersMs);
    out["firstCodexDispatchMs"] = encOptional(t.firstCodexDispatchMs);
    out["firstCodexHeadersMs"] = encOptional(t.firstCodexHeadersMs);
    out["firstUpstreamSseEventMs"] = encOptional(t.firstUpstreamSseEventMs);
    out["firstSemanticCommitmentMs"] = encOptional(t.firstSemanticCommitmentMs);
    out["streamTerminalMs"] = encOptional(t.streamTerminalMs);
    out["attemptedProviders"] = encStrings(t.attemptedProviders);
    out["removedProviderTriggerClass"] = encOptional(t.removedProviderTriggerClass);
    out["removedProviderCircuitTransition"] = encOptional(t.removedProviderCircuitTransition);
    out["removedProviderSelectedModel"] = encOptional(t.removedProviderSelectedModel);
    out["removedProviderTaskType"] = encOptional(t.removedProviderTaskType);
    out["removedProviderSemanticCommitment"] = encOptional(t.removedProviderSemanticCommitment);
    out["removedProviderLatencyMs"] = encOptional(t.removedProviderLatencyMs);
    out["removedProviderTerminalStatus"] = encOptional(t.removedProviderTerminalStatus);
    return out;
  }

  // ---- decoding recorded inputs -------------------------------------------------------------------------------
  std::optional<double> optionalNumber(const json::value* value) {
    if (!value || value->is_null()) return std::nullopt;
    return number(*value);
  }
  std::optional<std::string> optionalString(const json::value* value) {
    if (!value || value->is_null()) return std::nullopt;
    return std::string(value->get_string());
  }
  std::optional<bool> optionalBool(const json::value* value) {
    if (!value || value->is_null()) return std::nullopt;
    return value->get_bool();
  }
  std::vector<std::string> strings(const json::value* value) {
    std::vector<std::string> out;
    for (const json::value& element : value->as_array()) out.emplace_back(element.get_string());
    return out;
  }

  ResponseTelemetryState decodeState(const json::object& in) {
    const auto at = [&](std::string_view key) { return rt::get(in, key); };
    ResponseTelemetryState state = *telemetry::createResponseTelemetryState();
    state.provider = optionalString(at("provider"));
    state.fallbackReason = optionalString(at("fallbackReason"));
    state.model = optionalString(at("model"));
    state.reasoning = optionalString(at("reasoning"));
    state.outputTokenAllowance = optionalNumber(at("outputTokenAllowance"));
    state.rateLimitWaitMs = optionalNumber(at("rateLimitWaitMs"));
    state.rateLimitFailoverModel = optionalString(at("rateLimitFailoverModel"));
    state.inputTokens = optionalNumber(at("inputTokens"));
    state.cachedInputTokens = optionalNumber(at("cachedInputTokens"));
    state.cacheWriteInputTokens = optionalNumber(at("cacheWriteInputTokens"));
    state.outputTokens = optionalNumber(at("outputTokens"));
    state.totalTokens = optionalNumber(at("totalTokens"));
    state.usageObserved = at("usageObserved")->get_bool();
    state.usageTelemetryStatus = std::string(at("usageTelemetryStatus")->get_string());
    state.promptCacheKeyPresent = at("promptCacheKeyPresent")->get_bool();
    state.promptCacheMode = std::string(at("promptCacheMode")->get_string());
    state.explicitBreakpointCount = number(*at("explicitBreakpointCount"));
    state.accountSlot = optionalNumber(at("accountSlot"));
    state.accountCohortId = optionalString(at("accountCohortId"));
    state.activeGeneration = optionalNumber(at("activeGeneration"));
    state.activeTransitionReason = optionalString(at("activeTransitionReason"));
    const json::value& quota = *at("quotaUsedPercent");
    state.quotaUsedPercent = isUndefinedTag(quota) ? rt::Nullable<double>() : quota.is_null() ? rt::Nullable<double>(nullptr) : rt::Nullable<double>(number(quota));
    state.completed = at("completed")->get_bool();
    state.semanticOutputObserved = optionalBool(at("semanticOutputObserved"));
    state.upstreamEventKinds = strings(at("upstreamEventKinds"));
    state.streamTerminalType = optionalString(at("streamTerminalType"));
    state.failureKind = optionalString(at("failureKind"));
    state.responseCreatedObserved = at("responseCreatedObserved")->get_bool();
    state.syntheticTerminalType = optionalString(at("syntheticTerminalType"));
    state.stream = optionalBool(at("stream"));
    state.providerRequestId = optionalString(at("providerRequestId"));
    state.firstProviderDispatchMs = optionalNumber(at("firstProviderDispatchMs"));
    state.firstProviderHeadersMs = optionalNumber(at("firstProviderHeadersMs"));
    state.firstCodexDispatchMs = optionalNumber(at("firstCodexDispatchMs"));
    state.firstCodexHeadersMs = optionalNumber(at("firstCodexHeadersMs"));
    state.firstUpstreamSseEventMs = optionalNumber(at("firstUpstreamSseEventMs"));
    state.firstSemanticCommitmentMs = optionalNumber(at("firstSemanticCommitmentMs"));
    state.streamTerminalMs = optionalNumber(at("streamTerminalMs"));
    state.attemptedProviders = strings(at("attemptedProviders"));
    state.removedProviderTriggerClass = optionalString(at("removedProviderTriggerClass"));
    state.removedProviderCircuitTransition = optionalString(at("removedProviderCircuitTransition"));
    state.removedProviderSelectedModel = optionalString(at("removedProviderSelectedModel"));
    state.removedProviderTaskType = optionalString(at("removedProviderTaskType"));
    state.removedProviderSemanticCommitment = optionalString(at("removedProviderSemanticCommitment"));
    state.removedProviderLatencyMs = optionalNumber(at("removedProviderLatencyMs"));
    state.removedProviderTerminalStatus = optionalString(at("removedProviderTerminalStatus"));
    return state;
  }

  std::optional<UsageTokens> decodeUsage(const json::value& value) {
    if (value.is_null()) return std::nullopt;
    const json::object& in = value.get_object();
    UsageTokens usage{
      .inputTokens = optionalNumber(rt::get(in, "inputTokens")),
      .cachedInputTokens = optionalNumber(rt::get(in, "cachedInputTokens")),
      .cacheWriteInputTokens = optionalNumber(rt::get(in, "cacheWriteInputTokens")),
      .outputTokens = optionalNumber(rt::get(in, "outputTokens")),
      .totalTokens = optionalNumber(rt::get(in, "totalTokens")),
      .status = std::string(rt::get(in, "status")->get_string()),
    };
    if (const json::value* buyer = rt::get(in, "buyerCostMicro")) usage.buyerCostMicro = buyer->is_null() ? rt::Nullable<double>(nullptr) : rt::Nullable<double>(number(*buyer));
    return usage;
  }

  rt::Response responseWithHeader(const json::value* header, std::string body) {
    if (!header || header->is_null()) return rt::Response(rt::BodyInit(std::move(body)));
    rt::ResponseInit init;
    init.headers = rt::Headers{{"x-uos-upstream", std::string(header->get_string())}};
    return rt::Response(rt::BodyInit(std::move(body)), std::move(init));
  }

  bool same(const json::value& actual, const json::value& expected, const std::string& label) {
    if (rt::deepEqual(actual, expected)) return true;
    rt::test::report(__FILE__, __LINE__, label + "\n  actual:   " + rt::jsonStringify(actual) + "\n  expected: " + rt::jsonStringify(expected), false);
    return false;
  }

  rt::Reason makeReason(std::string_view kind) {
    using uos::codex::auth::CodexError;
    using uos::responses_stream::ResponsesStreamError;
    if (kind == "undefined") return nullptr;
    if (kind == "codex_timeout") return rt::makeReason(CodexError("t", "gateway_timeout", 504));
    if (kind == "codex_other") return rt::makeReason(CodexError("t", "codex_upstream_unreachable", 502));
    if (kind == "stream_inactivity") return rt::makeReason(ResponsesStreamError("s", {.cause = nullptr, .kind = "inactivity_timeout"}));
    if (kind == "stream_eof") return rt::makeReason(ResponsesStreamError("s", {.cause = nullptr, .kind = "premature_eof"}));
    if (kind == "stream_read") return rt::makeReason(ResponsesStreamError("s"));
    if (kind == "dom_timeout") return rt::makeReason(rt::DOMException("The operation timed out.", "TimeoutError"));
    if (kind == "dom_abort") return rt::makeReason(rt::DOMException("The operation was aborted.", "AbortError"));
    if (kind == "plain_timeout_name") return rt::makeReason(rt::JsError("x", "TimeoutError"));
    if (kind == "plain") return rt::makeReason(rt::JsError("boom"));
    if (kind == "string") return rt::thrown(json::value("TimeoutError"));
    return rt::thrown(json::value(json::object{{"name", "TimeoutError"}}));
  }

  rt::AbortSignal makeSignal(std::string_view kind) {
    const rt::AbortController controller;
    if (kind == "abort_default") controller.abort();
    if (kind == "abort_timeout") controller.abort(rt::makeReason(rt::DOMException("The operation timed out.", "TimeoutError")));
    if (kind == "abort_codex_timeout") controller.abort(rt::makeReason(uos::codex::auth::CodexError("t", "gateway_timeout", 504)));
    if (kind == "abort_string") controller.abort(rt::thrown(json::value("stop")));
    if (kind == "abort_plain") controller.abort(rt::makeReason(rt::JsError("stop")));
    return controller.signal();
  }

  std::string text(const json::value& value) { return std::string(value.get_string()); }

  UsageContext emptyContext() { return UsageContext{.keyId = std::nullopt, .kernelRepo = std::nullopt, .kernelOrg = std::nullopt}; }
}

TEST("usage parsers and the Chat projection match the TS on every recorded input") {
  std::size_t index = 0;
  for (const json::value& vector : section("usage")) {
    const json::value& input = *rt::get(vector, "input");
    const std::optional<json::value> decoded = isUndefinedTag(input) ? std::nullopt : std::optional<json::value>(decode(input));
    const json::value* argument = decoded ? &*decoded : nullptr;
    const std::optional<UsageTokens> responses = telemetry::extractUsageTokens(argument);
    const std::optional<UsageTokens> chat = telemetry::extractChatUsageTokens(argument);
    const std::string label = "usage[" + std::to_string(index++) + "] " + rt::jsonStringify(input);
    same(encUsage(responses), *rt::get(vector, "responses"), label + " responses");
    same(encChat(telemetry::toChatUsage(responses)), *rt::get(vector, "responsesChat"), label + " responsesChat");
    same(encUsage(chat), *rt::get(vector, "chat"), label + " chat");
    same(encChat(telemetry::toChatUsage(chat)), *rt::get(vector, "chatChat"), label + " chatChat");
  }
  EXPECT(index > 900);
}

TEST("prompt cache mode and key presence match the TS") {
  std::size_t index = 0;
  for (const json::value& vector : section("promptCache")) {
    const json::object record = decode(*rt::get(vector, "record")).as_object();
    const std::string label = "promptCache[" + std::to_string(index++) + "] " + rt::jsonStringify(json::value(record));
    same(json::value(json::string(telemetry::promptCacheModeFor(record))), *rt::get(vector, "mode"), label + " mode");
    same(json::value(telemetry::promptCacheKeyPresent(record)), *rt::get(vector, "keyPresent"), label + " keyPresent");
  }
  EXPECT_EQ(index, std::size_t{300});
}

TEST("explicit breakpoint counts match the TS for typed message items and raw records") {
  std::size_t index = 0;
  std::size_t typedMessages = 0;
  for (const json::value& vector : section("breakpoints")) {
    std::vector<uos::types::ResponseInputItem> input;
    const json::value items = decode(*rt::get(vector, "input"));
    for (const json::value& item : items.as_array()) {
      if (!item.is_object()) continue;  // not a record: the TS loop skips it too
      try {
        input.push_back(json::value_to<uos::types::ResponseInputItem>(item));
      } catch (const rt::TypeError&) {
        input.emplace_back(item.get_object());  // a record without a string type stays a record
      }
      // A typed message holds only the members its TS type declares (the normalizer rejects the rest, such as a
      // breakpoint on output_text): an item the conversion would change stays the raw record the TS saw.
      if (std::holds_alternative<uos::types::ResponseMessageItem>(input.back())) {
        if (rt::deepEqual(json::value_from(input.back()), item)) typedMessages += 1;
        else input.back() = item.get_object();
      }
    }
    const std::string label = "breakpoints[" + std::to_string(index++) + "]";
    same(rt::makeNumber(telemetry::countExplicitPromptCacheBreakpoints(input)), *rt::get(vector, "count"), label);
  }
  EXPECT(typedMessages > 20);
}

TEST("aggregateResponseTelemetry matches the TS over attached, raw and missing states") {
  std::size_t index = 0;
  for (const json::value& vector : section("aggregation")) {
    std::vector<rt::Response> sources;
    for (const json::value& described : rt::get(vector, "sources")->as_array()) {
      if (const json::value* repeat = rt::get(described, "repeat")) {
        sources.push_back(sources.at(static_cast<std::size_t>(rt::getNumber(*repeat).value())));
        continue;
      }
      rt::Response response = responseWithHeader(rt::get(described, "header"), "x");
      const json::value* state = rt::get(described, "state");
      if (!state->is_null()) {
        auto shared = std::make_shared<ResponseTelemetryState>(decodeState(state->as_object()));
        if (rt::get(described, "attach")->get_bool()) telemetry::attachResponseTelemetry(response, shared);
        else telemetry::responseTelemetry.set(response, shared);
      }
      sources.push_back(response);
    }
    const rt::Response target = responseWithHeader(rt::get(vector, "targetHeader"), "y");
    const rt::Response returned = telemetry::aggregateResponseTelemetry(sources, target);
    const std::string label = "aggregation[" + std::to_string(index++) + "]";
    same(json::value(returned == target), *rt::get(vector, "returnedTarget"), label + " returnedTarget");
    same(encTelemetry(telemetry::getResponseTelemetry(target)), *rt::get(vector, "telemetry"), label + " telemetry");
    same(encOptional(telemetry::getResponseAccountCohortId(target)), *rt::get(vector, "cohort"), label + " cohort");
    const auto* targetState = telemetry::responseTelemetry.find(target);
    const auto* sourceState = sources.size() == 1 ? telemetry::responseTelemetry.find(sources[0]) : nullptr;
    same(json::value(sources.size() == 1 && targetState && sourceState && *targetState == *sourceState), *rt::get(vector, "sameState"), label + " sameState");
  }
  EXPECT_EQ(index, std::size_t{400});
}

ASYNC_TEST("recorders in sequence match the TS, timings included") {
  std::size_t index = 0;
  for (const json::value& vector : section("recorders")) {
    const rt::Response response = rt::Response(rt::BodyInit(std::string("z")));
    auto state = std::make_shared<ResponseTelemetryState>(decodeState(rt::get(vector, "state")->as_object()));
    telemetry::attachResponseTelemetry(response, state);
    const json::value& started = *rt::get(vector, "startedAtMonotonicMs");
    const std::optional<double> startedAtMs = isUndefinedTag(started) ? std::nullopt : std::optional<double>(number(started));
    const bool throwInCallback = rt::get(vector, "throwInCallback")->get_bool();
    auto observed = std::make_shared<json::array>();
    UsageContext context = emptyContext();
    if (rt::get(vector, "hasTelemetry")->get_bool()) context.responseTelemetry = state;
    context.onTerminalUsage = [observed, throwInCallback](std::optional<UsageTokens> usage, bool completed) {
      json::object entry;
      entry["usage"] = encUsage(usage);
      entry["completed"] = completed;
      observed->push_back(std::move(entry));
      if (throwInCallback) throw rt::JsError("observer failed");
    };
    std::size_t step = 0;
    for (const json::value& recorded : rt::get(vector, "steps")->as_array()) {
      const double now = number(*rt::get(recorded, "now"));
      // performance.now() stays 0 here, so `0 - startedAt` must be the TS's `now - startedAt`.
      EXPECT_EQ(rt::performanceNow(), 0.0);
      context.startedAtMonotonicMs = startedAtMs ? std::optional<double>(-(now - *startedAtMs)) : std::nullopt;
      const std::string op = text(*rt::get(recorded, "op"));
      const json::value args = decode(*rt::get(recorded, "args"));
      const json::value& rawArgs = *rt::get(recorded, "args");
      if (op == "recordStreamTerminalType") {
        telemetry::recordStreamTerminalType(&context, text(args));
      } else if (op == "recordAttemptedProvider") {
        telemetry::recordAttemptedProvider(&context, text(args));
      } else if (op == "recordRemovedProviderFields") {
        telemetry::RecordRemovedProviderFieldsFields fields;
        const json::object& in = args.as_object();
        const auto nullableText = [&](std::string_view key) -> rt::Nullable<std::string> {
          const json::value* member = rt::get(in, key);
          if (!member) return rt::undefined;
          if (member->is_null()) return nullptr;
          return std::string(member->get_string());
        };
        fields.triggerClass = nullableText("triggerClass");
        fields.circuitTransition = nullableText("circuitTransition");
        fields.selectedModel = nullableText("selectedModel");
        fields.taskType = nullableText("taskType");
        fields.semanticCommitment = nullableText("semanticCommitment");
        fields.terminalStatus = nullableText("terminalStatus");
        if (const json::value* latency = rt::get(in, "latencyMs")) fields.latencyMs = latency->is_null() ? rt::Nullable<double>(nullptr) : rt::Nullable<double>(number(*latency));
        telemetry::recordRemovedProviderFields(&context, fields);
      } else if (op == "persistFailedRemovedProviderAttempt") {
        const std::string triggerClass = text(args);
        const rt::Promise<void> persisted = telemetry::persistFailedRemovedProviderAttempt(&context, 0, triggerClass);
        co_await persisted.wait();
      } else if (op == "recordRequestUsage") {
        const json::object& in = args.as_object();
        telemetry::RecordRequestUsageDetails details{
          .model = text(*rt::get(in, "model")),
          .route = text(*rt::get(in, "route")),
          .stream = rt::get(in, "stream")->get_bool(),
          .reasoning = optionalString(rt::get(in, "reasoning")),
        };
        if (const json::value* keyPresent = rt::get(in, "promptCacheKeyPresent")) details.promptCacheKeyPresent = keyPresent->get_bool();
        if (const json::value* mode = rt::get(in, "promptCacheMode")) details.promptCacheMode = text(*mode);
        if (const json::value* count = rt::get(in, "explicitBreakpointCount")) details.explicitBreakpointCount = number(*count);
        const rt::Promise<void> recorded_ = telemetry::recordRequestUsage(&context, details);
        co_await recorded_.wait();
      } else if (op == "recordCompletionUsage" || op == "recordTerminalUsage") {
        const std::optional<UsageTokens> usage = decodeUsage(*rt::get(rawArgs, "usage"));
        const json::value& completed = *rt::get(rawArgs, "completed");
        if (op == "recordCompletionUsage") {
          const rt::Promise<void> recorded_ =
            isUndefinedTag(completed) ? telemetry::recordCompletionUsage(&context, usage) : telemetry::recordCompletionUsage(&context, usage, completed.get_bool());
          co_await recorded_.wait();
        } else {
          telemetry::recordTerminalUsage(&context, usage, isUndefinedTag(completed) ? true : completed.get_bool());
        }
      } else if (op == "recordErrorUsage") {
        const rt::Promise<void> recorded_ = telemetry::recordErrorUsage(&context);
        co_await recorded_.wait();
      } else if (op == "recordFirstCodexDispatch") {
        telemetry::recordFirstCodexDispatch(&context);
      } else if (op == "recordFirstCodexHeaders") {
        telemetry::recordFirstCodexHeaders(&context);
      } else if (op == "recordFirstProviderDispatch") {
        telemetry::recordFirstProviderDispatch(&context);
      } else if (op == "recordFirstProviderHeaders") {
        telemetry::recordFirstProviderHeaders(&context);
      } else if (op == "recordFirstUpstreamSseEvent") {
        telemetry::recordFirstUpstreamSseEvent(&context);
      } else if (op == "recordFirstSemanticCommitment") {
        telemetry::recordFirstSemanticCommitment(&context);
      } else if (op == "recordStreamTerminal") {
        telemetry::recordStreamTerminal(&context);
      } else if (op == "selectRemovedProviderTelemetry") {
        telemetry::selectRemovedProviderTelemetry(&context);
      } else {
        rt::test::report(__FILE__, __LINE__, "unknown op " + op, true);
      }
      const std::string label = "recorders[" + std::to_string(index) + "].steps[" + std::to_string(step++) + "] " + op;
      same(encTelemetry(telemetry::getResponseTelemetry(response)), *rt::get(recorded, "telemetry"), label);
    }
    same(json::value(*observed), *rt::get(vector, "observed"), "recorders[" + std::to_string(index) + "] observed");
    index += 1;
  }
  EXPECT_EQ(index, std::size_t{120});
}

ASYNC_TEST("runWithResponseTelemetry records a precommit terminal time only after an observed SSE event") {
  std::size_t index = 0;
  for (const json::value& vector : section("runs")) {
    const json::object scenario = rt::get(vector, "scenario")->as_object();
    const bool sse = rt::get(scenario, "sse")->get_bool();
    const bool semantic = rt::get(scenario, "semantic")->get_bool();
    const std::optional<std::string> terminal = optionalString(rt::get(scenario, "terminal"));
    const bool terminalMs = rt::get(scenario, "terminalMs")->get_bool();
    // TS: performance.now() 200 at entry, then 201.5, 202.25 and 205.5; startedAtMonotonicMs 190.
    const double base = rt::performanceNow();
    UsageContext context = emptyContext();
    context.keyId = "k1";
    context.kernelRepo = UsageContext::KernelRepo{.owner = "o", .repo = "r"};
    context.startedAtMonotonicMs = base - (200 - 190);
    context.requestId = "req";
    std::function<rt::Task<rt::Response>(UsageContext)> run = [sse, semantic, terminal, terminalMs](UsageContext inner) -> rt::Task<rt::Response> {
      rt::test::clock().advance(1.5);
      if (sse) telemetry::recordFirstUpstreamSseEvent(&inner);
      rt::test::clock().advance(0.75);
      if (semantic) telemetry::recordFirstSemanticCommitment(&inner);
      if (terminal) inner.responseTelemetry->streamTerminalType = *terminal;
      if (terminalMs) inner.responseTelemetry->streamTerminalMs = 1;
      rt::test::clock().advance(3.25);
      rt::ResponseInit init;
      init.headers = rt::Headers{{"x-uos-upstream", "codex"}};
      co_return rt::Response(rt::BodyInit(std::string("ok")), std::move(init));
    };
    std::optional<UsageContext> passed = context;
    rt::Task<rt::Response> running = telemetry::runWithResponseTelemetry(std::move(passed), run);
    const rt::Response response = co_await std::move(running);
    same(encTelemetry(telemetry::getResponseTelemetry(response)), *rt::get(vector, "telemetry"), "runs[" + std::to_string(index++) + "]");
  }
  EXPECT_EQ(index, std::size_t{5});
}

TEST("event telemetry and incomplete failure kinds match the TS") {
  std::size_t index = 0;
  for (const json::value& vector : section("events")) {
    std::shared_ptr<ResponseTelemetryState> state = telemetry::createResponseTelemetryState();
    UsageContext context = emptyContext();
    context.responseTelemetry = state;
    std::size_t stepIndex = 0;
    for (const json::value& step : rt::get(vector, "steps")->as_array()) {
      const json::value& described = *rt::get(step, "event");
      uos::responses_stream::ResponsesStreamEvent event =
        rt::get(described, "synthetic") ? uos::responses_failover_stream::failureEventAfterCommit("resp_1", number(*rt::get(described, "sequence")))
                                         : uos::responses_failover_stream::responseEventFromValue(decode(*rt::get(described, "value")).as_object());
      telemetry::recordResponsesEventTelemetry(&context, event);
      const std::string label = "events[" + std::to_string(index) + "].steps[" + std::to_string(stepIndex++) + "]";
      same(encOptional(telemetry::responseIncompleteFailureKind(event)), *rt::get(step, "failureKind"), label + " failureKind");
      const rt::Response attached = telemetry::attachResponseTelemetry(rt::Response(rt::BodyInit(std::string(""))), state);
      same(encTelemetry(telemetry::getResponseTelemetry(attached)), *rt::get(step, "telemetry"), label + " telemetry");
    }
    index += 1;
  }
  EXPECT_EQ(index, std::size_t{150});
}

TEST("failure classification, timeout detection and failure telemetry match the TS") {
  for (const json::value& vector : section("classification")) {
    const std::string error = text(*rt::get(vector, "error"));
    const std::string signal = text(*rt::get(vector, "signal"));
    const std::string downstream = text(*rt::get(vector, "downstream"));
    const std::string label = "classification " + error + "/" + signal + "/" + downstream;
    same(json::value(json::string(telemetry::classifyStreamFailure(makeReason(error), makeSignal(signal), makeSignal(downstream)))), *rt::get(vector, "stream"),
         label + " stream");
    same(json::value(json::string(telemetry::classifyPreHeaderFailure(makeReason(error), makeSignal(signal), makeSignal(downstream)))),
         *rt::get(vector, "preHeader"), label + " preHeader");
  }
  for (const json::value& vector : section("timeouts")) {
    const std::string error = text(*rt::get(vector, "error"));
    same(json::value(telemetry::isTimeoutFailure(makeReason(error))), *rt::get(vector, "timeout"), "timeouts " + error);
  }
  std::size_t index = 0;
  for (const json::value& vector : section("failures")) {
    std::shared_ptr<ResponseTelemetryState> state = telemetry::createResponseTelemetryState();
    UsageContext context = emptyContext();
    context.responseTelemetry = state;
    const json::value& details = *rt::get(vector, "details");
    const rt::Reason reason = makeReason(text(*rt::get(vector, "error")));
    if (details.is_null()) {
      telemetry::recordResponsesFailureTelemetry(&context, reason);
    } else {
      const uos::responses_failover_stream::OwnedResponsesStreamFailureDetails owned{
        .failureKind = text(*rt::get(details, "failureKind")),
        .responseCreatedObserved = rt::get(details, "responseCreatedObserved")->get_bool(),
        .semanticCommitmentObserved = rt::get(details, "semanticCommitmentObserved")->get_bool(),
        .syntheticTerminalType = optionalString(rt::get(details, "syntheticTerminalType")),
        .upstreamTerminal = std::nullopt,
      };
      telemetry::recordResponsesFailureTelemetry(&context, reason, &owned);
    }
    const rt::Response attached = telemetry::attachResponseTelemetry(rt::Response(rt::BodyInit(std::string(""))), state);
    same(encTelemetry(telemetry::getResponseTelemetry(attached)), *rt::get(vector, "telemetry"), "failures[" + std::to_string(index++) + "]");
  }
}

ASYNC_TEST("stream error responses match the TS status, headers and body") {
  std::size_t index = 0;
  for (const json::value& vector : section("responses")) {
    const std::string fn = text(*rt::get(vector, "fn"));
    const json::array& args = rt::get(vector, "args")->as_array();
    std::optional<rt::Response> response;
    if (fn == "streamErrorResponse") {
      const double status = number(args.at(0));
      const std::string message = text(args.at(1));
      const std::string code = text(args.at(2));
      const std::string provider = text(args.at(3));
      const std::vector<std::string> warnings = strings(&args.at(4));
      const std::size_t arity = static_cast<std::size_t>(number(*rt::get(vector, "arity")));
      const std::optional<std::string> type = arity >= 6 && !isUndefinedTag(args.at(5)) ? std::optional<std::string>(text(args.at(5))) : std::nullopt;
      if (arity == 5) {
        response = telemetry::streamErrorResponse(status, message, code, provider, warnings);
      } else if (arity == 6) {
        response = telemetry::streamErrorResponse(status, message, code, provider, warnings, type);
      } else {
        const json::value& param = args.at(6);
        const rt::Nullable<std::string> nullableParam =
          isUndefinedTag(param) ? rt::Nullable<std::string>() : param.is_null() ? rt::Nullable<std::string>(nullptr) : rt::Nullable<std::string>(text(param));
        response = telemetry::streamErrorResponse(status, message, code, provider, warnings, type, nullableParam);
      }
    } else {
      const std::string terminal = text(args.at(0));
      const std::string provider = text(args.at(1));
      if (args.size() == 2) {
        response = telemetry::streamPreflightFailureResponse(terminal, provider);
      } else {
        const std::vector<std::string> warnings = strings(&args.at(2));
        response = telemetry::streamPreflightFailureResponse(terminal, provider, warnings);
      }
    }
    json::array headers;
    for (const auto& [name, value] : response->headers().entries()) headers.push_back(json::array{json::string(name), json::string(value)});
    const std::string body = co_await response->text();
    json::object described;
    described["status"] = response->status();
    described["headers"] = std::move(headers);
    described["body"] = json::string(body);
    same(json::value(described), *rt::get(vector, "out"), "responses[" + std::to_string(index++) + "] " + rt::jsonStringify(json::value(args)));
  }
  EXPECT_EQ(index, std::size_t{220});
}

TEST("reasoning progress release and completion telemetry match the TS") {
  for (const json::value& vector : section("providers")) {
    const std::string provider = text(*rt::get(vector, "provider"));
    same(json::value(telemetry::supportsReasoningProgressRelease(provider)), *rt::get(vector, "release"), "providers " + provider);
  }
  std::size_t index = 0;
  for (const json::value& vector : section("completion")) {
    const rt::Response response = responseWithHeader(rt::get(vector, "header"), "");
    const rt::Response returned = telemetry::setResponseCompletionTelemetry(response, rt::get(vector, "stream")->get_bool());
    EXPECT(returned == response);
    same(encTelemetry(telemetry::getResponseTelemetry(response)), *rt::get(vector, "telemetry"), "completion[" + std::to_string(index++) + "]");
  }
}
