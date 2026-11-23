// Links: tests/deepseek_responses_differential_test.cpp src/auth/relay.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp
//        src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp
//        src/authority/native_codex_mutations.cpp src/bounded_response_body.cpp src/cache/prompt_analytics_core.cpp
//        src/cache/prompt_analytics_read.cpp src/codex/account_routing.cpp src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp
//        src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp
//        src/codex/external_auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp
//        src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/responses_error.cpp src/codex/routing_evaluation.cpp
//        src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp src/deepseek/index.cpp
//        src/deepseek/responses.cpp src/defaults.cpp src/http.cpp src/kv.cpp src/metered_quota.cpp src/models/codex_models.cpp
//        src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp src/openai_telemetry.cpp src/paid_fallback/ledger_stats.cpp
//        src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp
//        src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
//        src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
//        src/provider/capacity_rollups.cpp src/provider/cerebras.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp
//        src/provider/claude_capacity_rollups.cpp src/provider/claude_code.cpp src/provider/claude_code_accounts.cpp
//        src/provider/claude_code_credentials.cpp src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp
//        src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/lithos.cpp src/provider/lithos_rate_limits.cpp
//        src/provider/metered.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/surplus.cpp
//        src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/release.cpp src/request_policy.cpp
//        src/responses_failover_stream.cpp src/responses_stream.cpp src/runtime/websocket.cpp src/runtime_config.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/types.cpp src/utils.cpp
//
// src/deepseek/responses.ts's bun tests (cerebras-responses, lithos-responses, the DeepSeek Responses suites) drive translation
// modules of a later wave. These differential vectors were recorded from the TS under Bun 1.4.0
// (tests/deepseek_responses_differential_vectors.inc): each ChatOnlyResponsesProfile's slots over the recorded input pools,
// failure/aggregateContextOverflow and the tool-name helpers. Two representations differ by design: the DeepSeek projection of
// "constructor" is the inherited `Object` function in JS and rt::undefined here; of "__proto__" it is Object.prototype in JS
// and an empty json::object here. LithosAI's requireApiKey slot returns the key in JS (the TS assigns requireLithosApiKey to a
// `() => void` slot) and nothing here: only whether it throws is compared.
#include "uos/deepseek/responses.hpp"

#include "uos/provider/cerebras.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/text_codec.hpp"

#include <cmath>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "deepseek_responses_differential_vectors.inc"

namespace rt = uos::rt;
namespace json = uos::json;
namespace responses = uos::deepseek::responses;
namespace ds = uos::deepseek::index;
using responses::ChatOnlyResponsesProfile;

namespace {
  const json::object& vectors() {
    static const json::object all = [] {
      const std::vector<std::uint8_t> packed = rt::fromBase64(kDeepSeekResponsesVectors);
      return rt::jsonParse(rt::decodeUtf8(rt::gzip::gunzipSync(packed))).as_object();
    }();
    return all;
  }
  const json::value& at(const json::value& value, std::string_view key) { return *rt::get(value, key); }
  const json::value& root(std::string_view key) { return *rt::get(vectors(), key); }
  const json::value& pool(std::string_view name) { return at(root("pools"), name); }
  std::string text(const json::value& value) { return std::string(value.get_string()); }
  bool isUndefinedTag(const json::value& value) { return value.is_object() && rt::hasOwn(value.get_object(), "$undefined"); }

  json::value decode(const json::value& value) {
    if (value.is_object() && rt::hasOwn(value.get_object(), "$num")) {
      const std::string tag = text(at(value, "$num"));
      if (tag == "-0") return rt::makeNumber(-0.0);
      if (tag == "NaN") return rt::makeNumber(std::numeric_limits<double>::quiet_NaN());
      return rt::makeNumber(tag == "Infinity" ? std::numeric_limits<double>::infinity() : -std::numeric_limits<double>::infinity());
    }
    if (value.is_array()) {
      json::array out;
      for (const json::value& element : value.get_array()) out.push_back(decode(element));
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
  // A recorded argument: nullopt for undefined.
  std::optional<json::value> argument(const json::value& tagged) { return isUndefinedTag(tagged) ? std::nullopt : std::optional<json::value>(decode(tagged)); }
  const json::value* pointer(const std::optional<json::value>& value) { return value ? &*value : nullptr; }

  json::value encOptional(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
  json::value encOptional(const std::optional<std::string>& value) { return value ? json::value(json::string(*value)) : json::value(nullptr); }

  // The recorded projection: a string, null, {"$function":"Object"} or {"$objectPrototype":true}.
  json::value encEffort(const rt::Nullable<json::value>& effort) {
    if (effort == nullptr) return nullptr;
    if (effort == rt::undefined) return json::object{{"$function", "Object"}};
    if (effort->is_object() && effort->get_object().empty()) return json::object{{"$objectPrototype", true}};
    return *effort;
  }

  json::value encDisposition(const ds::DeepSeekFinishDisposition& disposition) {
    json::object out;
    if (std::holds_alternative<ds::DeepSeekFinishDispositionCompleted>(disposition)) {
      out["kind"] = "completed";
    } else if (const auto* incomplete = std::get_if<ds::DeepSeekFinishDispositionIncomplete>(&disposition)) {
      out["kind"] = "incomplete";
      out["reason"] = json::string(incomplete->reason);
    } else if (const auto* failed = std::get_if<ds::DeepSeekFinishDispositionFailed>(&disposition)) {
      out["kind"] = "failed";
      out["code"] = json::string(failed->code);
    } else {
      out["kind"] = "unknown";
      out["value"] = json::string(std::get<ds::DeepSeekFinishDispositionUnknown>(disposition).value);
    }
    return out;
  }

  json::value encFailure(const responses::DeepSeekResponsesFailure& failure) {
    json::object out;
    out["ok"] = false;
    out["message"] = json::string(failure.message);
    out["param"] = json::string(failure.param);
    if (failure.code) out["code"] = json::string(*failure.code);
    if (failure.kind) out["kind"] = json::string(*failure.kind);
    return out;
  }

  // `{ threw: { name, message, code, status } }` or `{ value }` (not compared: see the file comment).
  json::value requireKey(const ChatOnlyResponsesProfile& profile) {
    json::object threw;
    try {
      profile.requireApiKey();
      return json::object{{"value", nullptr}};
    } catch (const ds::DeepSeekError& error) {
      threw["name"] = json::string(error.name);
      threw["message"] = json::string(error.message);
      threw["code"] = json::string(error.code);
      threw["status"] = rt::makeNumber(error.status);
    } catch (const uos::provider::lithos::LithosError& error) {
      threw["name"] = json::string(error.name);
      threw["message"] = json::string(error.message);
      threw["code"] = json::string(error.code);
      threw["status"] = rt::makeNumber(error.status);
    } catch (const uos::provider::cerebras::CerebrasError& error) {
      threw["name"] = json::string(error.name);
      threw["message"] = json::string(error.message);
      threw["code"] = json::string(error.code);
      threw["status"] = rt::makeNumber(error.status);
    }
    return json::object{{"threw", threw}};
  }

  bool same(const json::value& actual, const json::value& expected, const std::string& label) {
    if (rt::deepEqual(actual, expected)) return true;
    rt::test::report(__FILE__, __LINE__, label + "\n  actual:   " + rt::jsonStringify(actual) + "\n  expected: " + rt::jsonStringify(expected), false);
    return false;
  }

  const ChatOnlyResponsesProfile& profileNamed(std::string_view name) {
    if (name == "deepseek") return responses::DEEPSEEK_RESPONSES_PROFILE;
    if (name == "lithos") return responses::LITHOS_RESPONSES_PROFILE;
    return responses::CEREBRAS_RESPONSES_PROFILE;
  }
}

TEST("each ChatOnlyResponsesProfile matches the TS slot by slot") {
  std::size_t profiles = 0;
  for (const json::value& recorded : root("profiles").as_array()) {
    const std::string name = text(at(recorded, "name"));
    const ChatOnlyResponsesProfile& profile = profileNamed(name);
    profiles += 1;
    same(json::value(json::string(profile.id)), at(recorded, "id"), name + " id");
    same(json::value(json::string(profile.label)), at(recorded, "label"), name + " label");
    same(json::value(profile.requiresStreamUsageOption), at(recorded, "requiresStreamUsageOption"), name + " requiresStreamUsageOption");

    json::array upstream;
    for (const json::value& model : pool("models").as_array()) upstream.push_back(encOptional(profile.upstreamModelFor(text(model))));
    same(json::value(upstream), at(recorded, "upstream"), name + " upstreamModelFor");

    json::array efforts;
    for (const json::value& effort : pool("efforts").as_array()) efforts.push_back(encEffort(profile.projectReasoningEffort(text(effort))));
    same(json::value(efforts), at(recorded, "efforts"), name + " projectReasoningEffort");

    json::array finish;
    for (const json::value& reason : pool("finish").as_array()) {
      const std::optional<json::value> value = argument(reason);
      finish.push_back(encDisposition(profile.finishDisposition(pointer(value))));
    }
    same(json::value(finish), at(recorded, "finish"), name + " finishDisposition");

    std::size_t conflicts = 0;
    for (const json::value& conflict : at(recorded, "conflicts").as_array()) {
      const std::optional<json::value> effort = argument(at(conflict, "effort"));
      const std::optional<json::value> thinking = argument(at(conflict, "thinking"));
      const std::optional<json::value> choice = argument(at(conflict, "choice"));
      same(encOptional(profile.thinkingToolChoiceConflict(pointer(effort), pointer(thinking), pointer(choice))), at(conflict, "out"),
           name + " thinkingToolChoiceConflict " + rt::jsonStringify(conflict));
      conflicts += 1;
    }
    EXPECT_EQ(conflicts, std::size_t{405});
    same(json::value(json::string(profile.toolChoiceThinkingConflictMessage("required", "reasoning.effort"))), at(recorded, "message"), name + " message");

    json::array cached;
    json::array reasoning;
    for (const json::value& usage : pool("usage").as_array()) {
      const json::object value = decode(usage).as_object();
      for (const double tokens : {0.0, 5.0, 100.0}) {
        cached.push_back(encOptional(profile.cachedPromptTokens(value, tokens)));
        reasoning.push_back(encOptional(profile.reasoningTokens(value, tokens)));
      }
    }
    same(json::value(cached), at(recorded, "cached"), name + " cachedPromptTokens");
    same(json::value(reasoning), at(recorded, "reasoningTokens"), name + " reasoningTokens");

    std::size_t envIndex = 0;
    for (const json::value& expected : at(recorded, "requireKey").as_array()) {
      std::vector<std::pair<std::string, std::string>> variables;
      for (const json::key_value_pair* member : rt::objectEntries(at(expected, "env").as_object())) {
        variables.emplace_back(std::string(member->key()), text(member->value()));
      }
      const rt::env::ScopedEnvironment environment(variables);
      const json::value actual = requireKey(profile);
      const json::value& out = at(expected, "out");
      const std::string label = name + " requireApiKey env[" + std::to_string(envIndex++) + "]";
      if (rt::get(out, "threw")) same(actual, out, label);
      else same(json::value(rt::get(actual, "value") != nullptr), json::value(true), label + " does not throw");
    }
  }
  EXPECT_EQ(profiles, std::size_t{3});
}

TEST("failure results and tool-name restoration match the TS") {
  for (const json::value& recorded : root("failures").as_array()) {
    const json::array& args = at(recorded, "args").as_array();
    responses::DeepSeekResponsesFailure actual;
    if (rt::get(recorded, "overflow")) {
      actual = responses::aggregateContextOverflow(text(args.at(0)));
    } else if (args.size() == 2) {
      actual = responses::failure(text(args.at(0)), text(args.at(1)));
    } else {
      const std::optional<std::string> code = isUndefinedTag(args.at(2)) ? std::nullopt : std::optional<std::string>(text(args.at(2)));
      actual = responses::failure(text(args.at(0)), text(args.at(1)), code);
    }
    same(encFailure(actual), at(recorded, "out"), "failure " + rt::jsonStringify(json::value(args)));
  }
  const rt::OrderedMap<std::string, responses::OriginalToolName> toolNames{
    {"weather_wrapper", responses::OriginalToolName{.name = "weather", .namespace_ = "functions"}},
    {"patch", responses::OriginalToolName{.name = "apply_patch", .namespace_ = std::nullopt}},
  };
  for (const json::value& recorded : root("tools").as_array()) {
    const std::string name = text(at(recorded, "name"));
    const responses::OriginalToolName tool = responses::originalTool(name, toolNames);
    json::object out;
    out["name"] = json::string(tool.name);
    out["namespace"] = encOptional(tool.namespace_);
    same(json::value(out), at(recorded, "tool"), "originalTool(" + name + ")");
    same(json::value(json::string(responses::originalToolName(name, toolNames))), at(recorded, "toolName"), "originalToolName(" + name + ")");
  }
}
