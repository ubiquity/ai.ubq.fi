// Links: tests/request_policy_differential_test.cpp src/auth/relay.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp
//        src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp
//        src/authority/native_codex_mutations.cpp src/bounded_response_body.cpp src/cache/prompt_analytics_core.cpp
//        src/cache/prompt_analytics_read.cpp src/codex/account_routing.cpp src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp
//        src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp
//        src/codex/external_auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp
//        src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/responses_error.cpp src/codex/routing_evaluation.cpp
//        src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp src/defaults.cpp src/http.cpp
//        src/kv.cpp src/metered_quota.cpp src/models/codex_models.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp
//        src/openai_telemetry.cpp src/paid_fallback/ledger_stats.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp
//        src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity.cpp
//        src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp
//        src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp src/provider/cerebras_rate_limits.cpp
//        src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code.cpp
//        src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_models.cpp
//        src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/metered.cpp
//        src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/surplus.cpp src/provider/surplus_claude_chat_request.cpp
//        src/provider/surplus_claude_request.cpp src/release.cpp src/request_policy.cpp src/responses_failover_stream.cpp src/responses_stream.cpp
//        src/runtime/websocket.cpp src/runtime_config.cpp src/storage/kv.cpp src/storage/native_kv.cpp
//        src/storage/sqlite_readonly.cpp src/types.cpp src/utils.cpp
//
// src/request-policy.ts's direct test (claude-code-request-policy) drives the HTTP handlers of a later wave. These differential
// vectors were recorded from the TS under Bun 1.4.0 (tests/request_policy_differential_vectors.inc): every pure export over
// generated inputs, and getCodexModelMetadata over a fake KV holding the runtime config and provider selection with Metered and
// Surplus discovery mocked, then with no KV and an environment Claude credential (the Claude subscription record). The
// metadata scenarios pin the Metered/Surplus route choice the TS makes with `routeRecord === surplusRecord` (an object identity
// the port rewrites as value logic).
#include "uos/request_policy.hpp"

#include "uos/kv.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/types.hpp"

#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "request_policy_differential_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
namespace policy = uos::request_policy;
using policy::CodexModelMetadata;
using policy::CodexModelReasoning;

namespace {
  const json::object& vectors() {
    static const json::object all = [] {
      const std::vector<std::uint8_t> packed = rt::fromBase64(kRequestPolicyVectors);
      return rt::jsonParse(rt::decodeUtf8(rt::gzip::gunzipSync(packed))).as_object();
    }();
    return all;
  }
  const json::array& section(std::string_view name) { return rt::get(vectors(), name)->as_array(); }
  const json::value& at(const json::value& value, std::string_view key) { return *rt::get(value, key); }
  std::string text(const json::value& value) { return std::string(value.get_string()); }

  // ---- the tagged encoding (see the .inc) -----------------------------------------------------------------------------
  bool isUndefinedTag(const json::value& value) { return value.is_object() && rt::hasOwn(value.get_object(), "$undefined"); }
  double number(const json::value& value) {
    if (value.is_object() && rt::hasOwn(value.get_object(), "$num")) {
      const std::string tag = text(at(value, "$num"));
      if (tag == "-0") return -0.0;
      if (tag == "NaN") return std::numeric_limits<double>::quiet_NaN();
      return tag == "Infinity" ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity();
    }
    return rt::getNumber(value).value();
  }
  json::value decode(const json::value& value) {
    if (value.is_object() && rt::hasOwn(value.get_object(), "$num")) return rt::makeNumber(number(value));
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
  json::value undefinedTag() { return json::object{{"$undefined", true}}; }
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
  json::value encString(const std::optional<std::string>& value) { return value ? json::value(json::string(*value)) : json::value(nullptr); }
  json::value encStrings(const std::vector<std::string>& values) {
    json::array out;
    for (const std::string& value : values) out.emplace_back(json::string(value));
    return out;
  }
  std::vector<std::string> strings(const json::value& value) {
    std::vector<std::string> out;
    for (const json::value& element : value.as_array()) out.push_back(text(element));
    return out;
  }
  template <class T> json::value encNullable(const rt::Nullable<T>& value) {
    if (value == rt::undefined) return undefinedTag();
    if (value == nullptr) return nullptr;
    return encJson(rt::toJsonValue(*value));
  }

  json::value encHint(const uos::models::metadata::ModelMetadataHint& hint) {
    json::object out;
    out["context_window_tokens"] = encNullable(hint.context_window_tokens);
    out["max_context_window_tokens"] = encNullable(hint.max_context_window_tokens);
    out["auto_compact_token_limit_tokens"] = encNullable(hint.auto_compact_token_limit_tokens);
    out["effective_context_window_percent"] = encNullable(hint.effective_context_window_percent);
    out["supported_reasoning_levels"] = encNullable(hint.supported_reasoning_levels);
    out["default_reasoning_effort"] = hint.default_reasoning_effort ? encJson(*hint.default_reasoning_effort) : undefinedTag();
    return out;
  }

  json::value encWireEfforts(const CodexModelReasoning& reasoning) {
    json::array out;
    for (const auto& [effort, wire] : reasoning.wireEfforts) out.push_back(json::array{json::string(effort), json::string(wire)});
    return out;
  }

  std::optional<json::object> optionalObject(const json::value& tagged) {
    const json::value decoded = decode(tagged);
    if (decoded.is_null()) return std::nullopt;
    return decoded.as_object();
  }

  // Input items as the normalizer hands them over: a message typed when the conversion keeps every member, otherwise
  // the record (see openai_telemetry_differential_test.cpp).
  std::vector<uos::types::ResponseInputItem> inputItems(const json::value* raw) {
    std::vector<uos::types::ResponseInputItem> items;
    if (!raw || !raw->is_array()) return items;
    for (const json::value& item : raw->get_array()) {
      if (!item.is_object()) continue;
      try {
        items.push_back(json::value_to<uos::types::ResponseInputItem>(item));
      } catch (const rt::TypeError&) {
        items.emplace_back(item.get_object());
      }
      if (std::holds_alternative<uos::types::ResponseMessageItem>(items.back()) && !rt::deepEqual(json::value_from(items.back()), item)) {
        items.back() = item.get_object();
      }
    }
    return items;
  }

  rt::Task<json::value> describe(std::optional<rt::Response> response) {
    if (!response) co_return nullptr;
    json::array headers;
    for (const auto& [name, value] : response->headers().entries()) headers.push_back(json::array{json::string(name), json::string(value)});
    const std::string body = co_await response->text();
    json::object out;
    out["status"] = response->status();
    out["statusText"] = json::string(response->statusText());
    out["headers"] = std::move(headers);
    out["body"] = json::string(body);
    co_return out;
  }

  bool same(const json::value& actual, const json::value& expected, const std::string& label) {
    if (rt::deepEqual(actual, expected)) return true;
    rt::test::report(__FILE__, __LINE__, label + "\n  actual:   " + rt::jsonStringify(actual) + "\n  expected: " + rt::jsonStringify(expected), false);
    return false;
  }

  template <class Ok> json::value encParse(const std::variant<Ok, policy::ParseFieldNotOk>& result) {
    json::object out;
    if (const auto* failure = std::get_if<policy::ParseFieldNotOk>(&result)) {
      out["ok"] = false;
      out["message"] = json::string(failure->message);
      return out;
    }
    const Ok& ok = std::get<Ok>(result);
    out["ok"] = true;
    if constexpr (std::is_same_v<Ok, policy::ParseChatStreamOptionsOk>) {
      out["includeUsage"] = ok.includeUsage;
    } else if constexpr (std::is_same_v<Ok, policy::ParseStreamFieldOk>) {
      out["value"] = ok.value;
    } else if constexpr (std::is_same_v<Ok, policy::ParseReasoningParamOk>) {
      out["value"] = ok.value ? encJson(json::value(*ok.value)) : undefinedTag();
    } else if constexpr (std::is_same_v<Ok, policy::ParseMaxCompletionTokensFieldOk>) {
      out["value"] = ok.value ? encNumber(*ok.value) : undefinedTag();
    } else {
      out["value"] = ok.value ? json::value(json::string(*ok.value)) : undefinedTag();
    }
    return out;
  }

  rt::Fetch listing(bool up, std::string body) {
    return rt::Fetch([up, body](rt::RequestInfo, rt::RequestInit) -> rt::Response {
      if (!up) {
        rt::ResponseInit failed;
        failed.status = 500;
        return rt::Response(rt::BodyInit(std::string("down")), std::move(failed));
      }
      rt::ResponseInit init;
      init.headers = rt::Headers{{"content-type", "application/json"}};
      return rt::Response(rt::BodyInit(body), std::move(init));
    });
  }
}

TEST("cerebras and LithosAI hints match the TS, inherited Object.prototype names included") {
  for (const json::value& vector : section("hints")) {
    const std::string id = text(at(vector, "id"));
    same(encHint(policy::cerebrasProviderHint(id)), at(vector, "hint"), "cerebrasProviderHint(" + id + ")");
  }
  same(encHint(policy::LITHOS_PROVIDER_HINT()), *rt::get(vectors(), "lithosHint"), "LITHOS_PROVIDER_HINT");
}

TEST("temporary and trusted model ids and snapshot record ids match the TS") {
  for (const json::value& vector : section("trusted")) {
    const std::string model = text(at(vector, "model"));
    same(json::value(policy::isTemporaryFreeSurplusModel(model)), at(vector, "temporary"), "temporary " + model);
    same(json::value(policy::isAdditionalTrustedCodexModel(model)), at(vector, "trusted"), "trusted " + model);
  }
  std::size_t index = 0;
  for (const json::value& vector : section("modelIds")) {
    const json::object record = decode(at(vector, "record")).as_object();
    same(encString(policy::modelIdFromSnapshotRecord(record)), at(vector, "id"), "modelIds[" + std::to_string(index++) + "]");
  }
}

TEST("Codex model reasoning, wire efforts and reasoning params match the TS") {
  static constexpr std::string_view EFFORTS[]{"none", "ultra", "xhigh", "high", "low", "medium", "max", "1", "2", "custom"};
  std::size_t index = 0;
  for (const json::value& vector : section("reasoning")) {
    const std::optional<json::object> record = optionalObject(at(vector, "record"));
    const CodexModelReasoning reasoning = policy::getCodexModelReasoning(record);
    const std::string label = "reasoning[" + std::to_string(index++) + "]";
    same(encStrings(reasoning.levels), at(vector, "levels"), label + " levels");
    same(encString(reasoning.defaultLevel), at(vector, "defaultLevel"), label + " defaultLevel");
    same(encWireEfforts(reasoning), at(vector, "wireEfforts"), label + " wireEfforts");
    json::array forCodex;
    for (const std::string_view effort : EFFORTS) forCodex.emplace_back(json::string(policy::reasoningEffortForCodexRequest(std::string(effort), reasoning)));
    same(json::value(forCodex), at(vector, "forCodex"), label + " forCodex");
    const json::value& param = at(vector, "param");
    const std::optional<json::object> reasoningParam = isUndefinedTag(param) ? std::nullopt : std::optional<json::object>(decode(param).as_object());
    const std::optional<json::object> normalized = policy::normalizeReasoningParamForCodex(reasoningParam, reasoning);
    same(normalized ? encJson(json::value(*normalized)) : undefinedTag(), at(vector, "normalizedParam"), label + " normalizedParam");
  }
  EXPECT_EQ(index, std::size_t{250});
  for (const json::value& vector : section("labels")) {
    const json::value& effortTag = at(vector, "effort");
    const json::value& paramTag = at(vector, "param");
    const std::optional<std::string> effort = isUndefinedTag(effortTag) ? std::nullopt : std::optional<std::string>(text(effortTag));
    const std::optional<json::object> param = isUndefinedTag(paramTag) ? std::nullopt : std::optional<json::object>(decode(paramTag).as_object());
    const std::string label = "labels " + rt::jsonStringify(vector);
    same(json::value(json::string(policy::resolveReasoningLabelFromEffort(effort, "medium"))), at(vector, "fromEffort"), label + " fromEffort");
    same(json::value(json::string(policy::resolveReasoningLabelFromParam(param, "medium"))), at(vector, "fromParam"), label + " fromParam");
    same(json::value(json::string(policy::resolveDefaultReasoningLabel(policy::getCodexModelReasoning(std::nullopt), effort.value_or("low")))),
         at(vector, "defaultLabel"), label + " defaultLabel");
  }
}

ASYNC_TEST("model availability errors match the TS") {
  std::size_t index = 0;
  for (const json::value& vector : section("available")) {
    const std::string model = text(at(vector, "model"));
    const std::string route = text(at(vector, "route"));
    const json::value& endpoints = at(vector, "supportedEndpoints");
    const CodexModelMetadata metadata{
      .snapshot = optionalObject(at(vector, "snapshot")),
      .record = optionalObject(at(vector, "record")),
      .reasoning = policy::getCodexModelReasoning(optionalObject(at(vector, "record"))),
      .supportedEndpoints = endpoints.is_null() ? std::nullopt : std::optional<std::vector<std::string>>(strings(endpoints)),
    };
    std::optional<rt::Response> response = policy::validateCodexModelAvailable(model, route, metadata);
    rt::Task<json::value> describing = describe(std::move(response));
    const json::value described = co_await std::move(describing);
    same(described, at(vector, "out"), "available[" + std::to_string(index++) + "] " + model + " " + route);
  }
  EXPECT_EQ(index, std::size_t{720});
}

ASYNC_TEST("known unsupported prompt cache use matches the TS") {
  std::size_t index = 0;
  std::size_t rejected = 0;
  for (const json::value& vector : section("promptCacheUse")) {
    const std::string model = text(at(vector, "model"));
    const std::string inputParam = text(at(vector, "inputParam"));
    const json::object rawRecord = decode(at(vector, "rawRecord")).as_object();
    const std::optional<json::object> record = optionalObject(at(vector, "record"));
    const CodexModelMetadata metadata{.snapshot = std::nullopt, .record = record, .reasoning = policy::getCodexModelReasoning(record), .supportedEndpoints = std::nullopt};
    const std::vector<uos::types::ResponseInputItem> input = inputItems(rt::get(rawRecord, inputParam));
    std::optional<rt::Response> response = policy::validateKnownUnsupportedPromptCacheUse(model, metadata, rawRecord, input, inputParam);
    if (response) rejected += 1;
    rt::Task<json::value> describing = describe(std::move(response));
    const json::value described = co_await std::move(describing);
    same(described, at(vector, "out"), "promptCacheUse[" + std::to_string(index++) + "] " + rt::jsonStringify(json::value(rawRecord)));
  }
  EXPECT_EQ(index, std::size_t{600});
  EXPECT_EQ(rejected, std::size_t{219});
}

ASYNC_TEST("ignored warnings, warning headers and passthrough normalization match the TS") {
  for (const json::value& vector : section("ignored")) {
    const json::object record = decode(at(vector, "record")).as_object();
    const std::vector<std::string> used = strings(at(vector, "usedKeys"));
    const rt::OrderedSet<std::string> usedKeys(used.begin(), used.end());
    same(encStrings(policy::buildIgnoredWarnings(record, usedKeys)), at(vector, "warnings"), "ignored " + rt::jsonStringify(vector));
  }
  for (const json::value& vector : section("warnings")) {
    rt::ResponseInit init;
    init.status = 201;
    init.statusText = "Created";
    if (!at(vector, "header").is_null()) init.headers = rt::Headers{{"x-uos-warning", text(at(vector, "header"))}, {"x-other", "1"}};
    const rt::Response response(rt::BodyInit(std::string("body")), std::move(init));
    const std::string label = "warnings " + rt::jsonStringify(vector);
    same(encStrings(policy::responseWarnings(response)), at(vector, "fromHeader"), label + " fromHeader");
    const rt::Response result = policy::withUosWarning(response, strings(at(vector, "added")));
    same(json::value(result == response), at(vector, "same"), label + " same");
    rt::Task<json::value> describing = describe(result);
    const json::value described = co_await std::move(describing);
    same(described, at(vector, "out"), label + " out");
  }
  std::size_t index = 0;
  for (const json::value& vector : section("passthrough")) {
    const json::object rawRecord = decode(at(vector, "rawRecord")).as_object();
    json::object codexBody;
    codexBody["model"] = "m";
    codexBody["tools"] = "existing";
    policy::applyPassthroughToCodexRequest(codexBody, rawRecord, strings(at(vector, "keys")));
    same(encJson(json::value(codexBody)), at(vector, "codexBody"), "passthrough[" + std::to_string(index++) + "] " + rt::jsonStringify(json::value(rawRecord)));
  }
}

TEST("request field parsers match the TS") {
  for (const json::value& vector : section("parse")) {
    const json::value& tagged = at(vector, "value");
    const std::optional<json::value> decoded = isUndefinedTag(tagged) ? std::nullopt : std::optional<json::value>(decode(tagged));
    const json::value* value = decoded ? &*decoded : nullptr;
    const std::string label = "parse " + rt::jsonStringify(tagged);
    same(encParse(policy::parseReasoningEffortField(value, "reasoning.effort")), at(vector, "effortField"), label + " effortField");
    same(encParse(policy::parseReasoningParam(value)), at(vector, "reasoningParam"), label + " reasoningParam");
    same(encParse(policy::parseStreamField(value)), at(vector, "stream"), label + " stream");
    same(encParse(policy::parseChatStreamOptions(value)), at(vector, "streamOptions"), label + " streamOptions");
    same(encParse(policy::parseMaxCompletionTokensField(value)), at(vector, "maxTokens"), label + " maxTokens");
  }
}

ASYNC_TEST("getCodexModelMetadata matches the TS over the runtime catalog, Claude selection and paid discovery") {
  rt::globalDateNow() = [] { return 1'790'000'000'000.0; };
  const json::value config = rt::jsonParse(
    R"({"version":2,"default_model":"gpt-6","default_reasoning_effort":"medium","codex_models":{"models":[{"slug":"gpt-6","default_reasoning_level":"high","supported_reasoning_levels":["low","high","xhigh"],"reasoning_effort_wire_map":{"xhigh":"max"}},{"slug":"gpt-6-mini","supported_reasoning_levels":[]}],"source":"catalog","updated_at_ms":1700000000000},"updated_at_ms":1700000000000})");
  const std::string meteredData =
    R"({"data":[{"id":"paid-a","supported_endpoint_types":["openai","openai-response"]},{"id":"paid-b","supported_endpoint_types":["openai"]},{"id":"paid-c","supported_endpoint_types":["anthropic"]},{"id":"claude-paid","supported_endpoint_types":["openai-response"]}]})";
  const std::string surplusData = R"({"data":[{"id":"paid-b"},{"id":"paid-d"},{"id":"paid-a"}]})";
  std::size_t index = 0;
  for (const json::value& vector : section("metadataScenarios")) {
    const std::string model = text(at(vector, "model"));
    const std::string route = text(at(vector, "route"));
    const bool withConfig = at(vector, "withConfig").get_bool();
    const bool meteredUp = at(vector, "meteredUp").get_bool();
    const bool surplusUp = at(vector, "surplusUp").get_bool();
    const json::value& selectionRow = at(vector, "selection");
    const auto kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
    if (withConfig) {
      const rt::KvValue stored(config);
      auto setting = kv->set(uos::runtime_config::RUNTIME_CONFIG_V2_KEY, stored);
      co_await std::move(setting);
    }
    if (!selectionRow.is_null()) {
      const rt::KvValue stored(selectionRow);
      auto setting = kv->set(uos::provider::selection::PROVIDER_SELECTION_KV_KEY, stored);
      co_await std::move(setting);
    }
    uos::kv::setKvForTest(kv);
    uos::runtime_config::resetRuntimeConfigCacheForTest();
    uos::provider::selection::resetProviderSelectionCacheForTest();
    uos::provider::metered::resetMeteredModelsCacheForTest();
    uos::provider::surplus::resetSurplusModelsCacheForTest();
    const rt::env::ScopedEnvironment environment{{"METERED_API_KEY", "metered-key"}, {"SURPLUS_API_KEY", "surplus-key"}};
    uos::provider::metered::setMeteredModelsFetchForTest(listing(meteredUp, meteredData));
    const rt::GlobalFetchOverride surplusFetch(listing(surplusUp, surplusData));
    rt::Task<CodexModelMetadata> loading = policy::getCodexModelMetadata(model, route);
    const CodexModelMetadata metadata = co_await std::move(loading);
    uos::provider::metered::setMeteredModelsFetchForTest(rt::Fetch());
    json::object out;
    out["snapshot"] = metadata.snapshot ? encJson(json::value(*metadata.snapshot)) : json::value(nullptr);
    out["record"] = metadata.record ? encJson(json::value(*metadata.record)) : json::value(nullptr);
    out["levels"] = encStrings(metadata.reasoning.levels);
    out["defaultLevel"] = encString(metadata.reasoning.defaultLevel);
    out["wireEfforts"] = encWireEfforts(metadata.reasoning);
    out["supportedEndpoints"] = metadata.supportedEndpoints ? encStrings(*metadata.supportedEndpoints) : json::value(nullptr);
    same(json::value(out), at(vector, "out"), "metadataScenarios[" + std::to_string(index++) + "] " + rt::jsonStringify(vector));
  }
  uos::kv::setKvForTest(nullptr);
  uos::runtime_config::resetRuntimeConfigCacheForTest();
  uos::provider::selection::resetProviderSelectionCacheForTest();
  uos::provider::metered::resetMeteredModelsCacheForTest();
  uos::provider::surplus::resetSurplusModelsCacheForTest();
  EXPECT_EQ(index, std::size_t{288});
}

ASYNC_TEST("getCodexModelMetadata's Claude subscription record matches the TS with no KV and an environment credential") {
  rt::globalDateNow() = [] { return 1'790'000'000'000.0; };
  const json::value& claude = *rt::get(vectors(), "claudeScenarios");
  const std::string claudeListing = rt::jsonStringify(at(claude, "claudeListing"));
  const std::string surplusListing = rt::jsonStringify(at(claude, "surplusListing"));
  const rt::Fetch routed([claudeListing, surplusListing](rt::RequestInfo input, rt::RequestInit) -> rt::Response {
    std::string url;
    if (const std::string* text_ = std::get_if<std::string>(&input)) url = *text_;
    else if (const rt::URL* parsed = std::get_if<rt::URL>(&input)) url = parsed->href();
    else url = std::get<rt::Request>(input).url();
    rt::ResponseInit init;
    init.headers = rt::Headers{{"content-type", "application/json"}};
    return rt::Response(rt::BodyInit(url.rfind("https://api.anthropic.com/v1/models", 0) == 0 ? claudeListing : surplusListing), std::move(init));
  });
  std::size_t index = 0;
  std::size_t found = 0;
  for (const json::value& vector : at(claude, "scenarios").as_array()) {
    const std::string model = text(at(vector, "model"));
    const std::string route = text(at(vector, "route"));
    uos::kv::setKvForTest(nullptr);
    uos::runtime_config::resetRuntimeConfigCacheForTest();
    uos::provider::selection::resetProviderSelectionCacheForTest();
    uos::provider::metered::resetMeteredModelsCacheForTest();
    uos::provider::surplus::resetSurplusModelsCacheForTest();
    uos::provider::claude_code_models::resetClaudeCodeModelsCacheForTest();
    const rt::env::ScopedEnvironment environment{{"CLAUDE_API_KEY", "sk-ant-api03-fake-key"}, {"SURPLUS_API_KEY", "surplus-key"}};
    const rt::GlobalFetchOverride fetchScope(routed);
    rt::Task<CodexModelMetadata> loading = policy::getCodexModelMetadata(model, route);
    const CodexModelMetadata metadata = co_await std::move(loading);
    if (metadata.record && !metadata.snapshot) found += 1;
    json::object out;
    out["snapshot"] = metadata.snapshot ? encJson(json::value(*metadata.snapshot)) : json::value(nullptr);
    out["record"] = metadata.record ? encJson(json::value(*metadata.record)) : json::value(nullptr);
    out["levels"] = encStrings(metadata.reasoning.levels);
    out["defaultLevel"] = encString(metadata.reasoning.defaultLevel);
    out["wireEfforts"] = encWireEfforts(metadata.reasoning);
    out["supportedEndpoints"] = metadata.supportedEndpoints ? encStrings(*metadata.supportedEndpoints) : json::value(nullptr);
    same(json::value(out), at(vector, "out"), "claudeScenarios[" + std::to_string(index++) + "] " + model + " " + route);
  }
  uos::runtime_config::resetRuntimeConfigCacheForTest();
  uos::provider::selection::resetProviderSelectionCacheForTest();
  uos::provider::surplus::resetSurplusModelsCacheForTest();
  uos::provider::claude_code_models::resetClaudeCodeModelsCacheForTest();
  EXPECT_EQ(index, std::size_t{12});
  EXPECT_EQ(found, std::size_t{6});
}
