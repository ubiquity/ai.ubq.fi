// Links: tests/transport_normalization_coverage_test.cpp
// src/upstream_wire.cpp src/input_normalization.cpp src/models/metadata.cpp src/provider/lithos.cpp src/provider/lithos_rate_limits.cpp
// src/models/codex_models.cpp src/models/openrouter_models.cpp src/provider/cerebras.cpp src/provider/openrouter.cpp src/provider/cerebras_rate_limits.cpp
// src/types.cpp src/utils.cpp src/request_policy.cpp src/provider/metered.cpp src/deepseek/waterfall.cpp
// src/provider/selection.cpp src/defaults.cpp src/deepseek/index.cpp src/bounded_response_body.cpp src/provider/surplus_claude_request.cpp
// src/provider/surplus.cpp src/http.cpp src/provider/claude_code_models.cpp src/provider/anthropic_responses.cpp src/provider/anthropic_response_wire.cpp
// src/kv.cpp src/config.cpp src/openai_telemetry.cpp src/provider/claude_code.cpp src/responses_failover_stream.cpp
// src/recent_model_context.cpp src/provider/claude_code_quota.cpp src/provider/surplus_claude_chat_request.cpp src/provider/claude_code_credentials.cpp src/models/codex_models_fetch.cpp
// src/provider/anthropic_replay.cpp src/runtime_config.cpp src/codex/auth_refresh.cpp src/codex/native_auth.cpp src/codex/external_auth.cpp
// src/auth/relay.cpp src/codex/dispatch.cpp src/codex/auth.cpp src/provider/claude_code_accounts.cpp src/codex/responses_error.cpp
// src/responses_stream.cpp src/codex/routing_mutations.cpp src/authority/native_codex_mutations.cpp src/codex/capacity_routing.cpp src/provider/claude_code_usage_schema.cpp
// src/authority/native_codex_capacity.cpp src/codex/native_auth_transport.cpp src/authority/native_codex_contracts.cpp src/provider/anthropic_chat_responses.cpp src/release.cpp
// src/storage/kv.cpp src/storage/sqlite_readonly.cpp src/provider/removed_provider_telemetry.cpp src/storage/native_kv.cpp src/runtime/websocket.cpp
// src/provider/capacity_events.cpp src/provider/capacity.cpp src/codex/banked_reset.cpp src/authority/native_codex_evaluation.cpp src/provider/capacity_contract.cpp
// src/provider/claude_capacity_rollups.cpp src/cache/prompt_analytics_read.cpp src/provider/capacity_rollups.cpp src/cache/prompt_analytics_core.cpp src/provider/capacity_history.cpp
// src/models/codex_models_availability.cpp src/provider/capacity_parse.cpp src/provider/claude_capacity_history.cpp src/provider/capacity_analytics.cpp src/codex/routing_state.cpp
// src/provider/health.cpp src/provider/anthropic_chat_stream.cpp src/codex/rate_limit_429.cpp src/paid_fallback/ledger_stats.cpp src/codex/banked_reset_provider.cpp
// src/metered_quota.cpp src/authority/native_codex_429.cpp src/codex/reset_settings.cpp src/codex/experiment.cpp
// src/codex/account_routing.cpp src/codex/routing_serial.cpp src/authority/native_codex.cpp src/codex/routing_evaluation.cpp src/codex/overage_settings.cpp
// Original assertions passed under isolated Bun1.4.0; full owned API outputs replay below.
// The three async/graph cases follow as explicit tests. No higher handler stand-ins.
#include "uos/input_normalization.hpp"
#include "uos/upstream_wire.hpp"
#include "uos/config.hpp"
#include "uos/codex/auth.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/utils.hpp"
#include <map>
namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace n = uos::input_normalization;
  namespace w = uos::upstream_wire;
  struct Isolated {
    rt::env::ScopedEnvironment environment{{{"HOME", "/nonexistent/w06c7-transport"}}};
    std::string saved = uos::config::config.codexAuthJsonB64;
    Isolated() { uos::config::config.codexAuthJsonB64.clear(); }
    ~Isolated() { uos::config::config.codexAuthJsonB64 = saved; }
  };
  json::value undefinedValue() { return json::object{{"$undefined", true}}; }
  const json::value* arg(const json::array& args, std::size_t index) {
    const auto* value = rt::get(args, index);
    return value && rt::getBool(*value, "$undefined") != true ? value : nullptr;
  }
  std::string str(const json::array& args, std::size_t index, const std::string& fallback = "") { return uos::utils::getString(arg(args, index)).value_or(fallback); }
  json::value raw(const json::array& args, std::size_t index) { const auto* value = arg(args, index); return value ? *value : json::value(nullptr); }
  std::optional<std::string> optionalStr(const json::array& args, std::size_t index) { return uos::utils::getString(arg(args, index)); }
  rt::Reason reason(const json::value* value) {
    if (!value) return nullptr;
    const auto kind = rt::getString(*value, "$error");
    if (!kind) return rt::thrown(*value);
    const auto message = uos::utils::getString(rt::get(*value, "message")).value_or("");
    const auto code = uos::utils::getString(rt::get(*value, "code")).value_or("");
    const auto status = rt::getNumber(*value, "status").value_or(0);
    if (*kind == "CodexError") return rt::makeReason(uos::codex::auth::CodexError(message, code, status));
    if (*kind == "MeteredError") return rt::makeReason(uos::provider::metered::MeteredError(message, code, status));
    if (*kind == "SurplusError") return rt::makeReason(uos::provider::surplus::SurplusError(message, code, status));
    if (*kind == "CerebrasError") return rt::makeReason(uos::provider::cerebras::CerebrasError(message, code, status));
    if (*kind == "DeepSeekError") return rt::makeReason(uos::deepseek::index::DeepSeekError(message, code, status));
    if (*kind == "LithosError") return rt::makeReason(uos::provider::lithos::LithosError(message, code, status));
    if (*kind == "ApiKeyQuotaDispatchError") {
      uos::api_key_policy::ApiKeyQuotaDispatchErrorOptions options;
      options.status = status; options.code = code; options.errorType = uos::utils::getString(rt::get(*value, "errorType"));
      rt::OrderedMap<std::string, std::string> headers;
      if (const auto* fields = rt::getObject(*value, "headers")) for (const auto& entry : *fields) headers.set(std::string(entry.key()), rt::toString(entry.value()));
      options.headers = headers;
      return rt::makeReason(uos::api_key_policy::ApiKeyQuotaDispatchError(message, options));
    }
    if (*kind == "DOMException") return rt::makeReason(rt::DOMException(message, uos::utils::getString(rt::get(*value, "name")).value_or("Error")));
    if (*kind == "TypeError") return rt::makeReason(rt::TypeError(message));
    if (const auto* statusValue = rt::get(*value, "status")) return rt::makeReason(w::ErrorWithStatus(message, *statusValue));
    return rt::makeReason(rt::JsError(message));
  }
  json::value consoleValue(const rt::console::Value& value) {
    using K = rt::console::Value::Kind;
    if (value.kind() == K::null) return nullptr;
    if (value.kind() == K::string) return json::value(value.string());
    if (value.kind() == K::number) return rt::makeNumber(value.number());
    if (value.kind() == K::boolean) return value.boolean();
    if (value.kind() == K::array) { json::array result; for (const auto& part : value.elements()) result.push_back(consoleValue(part)); return result; }
    if (value.kind() == K::object) { json::object result; for (const auto& [key, part] : value.properties()) rt::objectDefine(result, key, consoleValue(part)); return result; }
    return undefinedValue();
  }
  template<class T> json::value normalResult(const n::NormalizationResult<T>& result) {
    if (const auto* failure = std::get_if<n::NormalizationResultNotOk>(&result)) return json::object{{"ok", false}, {"message", failure->message}, {"param", failure->param}};
    const auto& value = std::get<n::NormalizationResultOk<T>>(result).value;
    if constexpr (std::is_void_v<T>) { static_cast<void>(value); return json::object{{"ok", true}, {"value", undefinedValue()}}; }
    else if constexpr (std::is_same_v<T, n::NormalizedChatMessage>) {
      const auto instruction = value.instruction ? json::value(*value.instruction) : json::value(nullptr);
      const auto content = value.instructionContent ? json::value_from(*value.instructionContent) : json::value(nullptr);
      return json::object{{"ok", true}, {"value", json::object{{"instruction", instruction}, {"instructionContent", content}, {"input", json::value_from(value.input)}}}};
    } else return json::object{{"ok", true}, {"value", json::value_from(value)}};
  }
  rt::Task<json::value> responseValue(rt::Response response) {
    json::array headers;
    for (const auto& [key, value] : response.headers()) headers.push_back(json::array{json::value(key), json::value(value)});
    const auto text = co_await response.text();
    co_return json::object{{"$response", true}, {"status", response.status()}, {"headers", headers}, {"body", text}};
  }
  rt::Task<json::value> invoke(const json::object& row) {
    const auto name = rt::getString(row, "name").value();
    const auto& args = row.at("args").as_array();
    const auto value = raw(args, 0);
    if (name == "configuredCerebrasModelCapabilities") { const auto result = n::configuredCerebrasModelCapabilities(str(args, 0)); co_return result ? json::value(*result) : json::value(nullptr); }
    if (name == "configuredOpenRouterModels") co_return n::configuredOpenRouterModels();
    if (name == "configuredOpenRouterModelCapabilities") co_return n::configuredOpenRouterModelCapabilities();
    if (name == "normalizeModelCapabilitiesEntry") { const auto result = n::normalizeModelCapabilitiesEntry(arg(args, 0)); co_return result ? json::value(*result) : json::value(nullptr); }
    if (rt::startsWith(name, "withConfigured")) {
      const auto& models = value.as_array(); const bool enabled = rt::truthy(raw(args, 1));
      if (name == "withConfiguredCerebrasModel") co_return n::withConfiguredCerebrasModel(models, enabled);
      if (name == "withConfiguredDeepSeekModels") co_return n::withConfiguredDeepSeekModels(models, enabled);
      if (name == "withConfiguredDeepSeekCapabilities") co_return n::withConfiguredDeepSeekCapabilities(models, enabled);
      if (name == "withConfiguredLithosModels") co_return n::withConfiguredLithosModels(models, enabled);
      if (name == "withConfiguredLithosCapabilities") co_return n::withConfiguredLithosCapabilities(models, enabled);
      if (name == "withConfiguredDeepSeekWaterfallModels") co_return n::withConfiguredDeepSeekWaterfallModels(models, enabled);
      if (name == "withConfiguredDeepSeekWaterfallCapabilities") co_return n::withConfiguredDeepSeekWaterfallCapabilities(models, enabled);
    }
    if (name == "validatePromptCacheControls") co_return normalResult(n::validatePromptCacheControls(value.as_object()));
    if (name == "normalizeChatMessage") co_return normalResult(n::normalizeChatMessage(arg(args, 0), rt::toNumber(raw(args, 1))));
    if (name == "normalizeResponseContentItem") co_return normalResult(n::normalizeResponseContentItem(arg(args, 0), str(args, 1), str(args, 2)));
    if (name == "normalizeResponseMessageItem") co_return normalResult(n::normalizeResponseMessageItem(arg(args, 0), str(args, 1)));
    if (name == "normalizeFunctionCallOutputItem") co_return normalResult(n::normalizeFunctionCallOutputItem(value.as_object(), str(args, 1)));
    if (name == "normalizeModelList") { const auto result = n::normalizeModelList(arg(args, 0)); co_return result ? json::value(*result) : json::value(nullptr); }
    if (name == "formatErrorSnippet") {
      const auto limit = arg(args, 1) ? rt::toNumber(*arg(args, 1)) : 280;
      co_return json::value(rt::get(value, "$error") ? w::formatErrorSnippet(reason(arg(args, 0)), limit) : w::formatErrorSnippet(arg(args, 0), limit));
    }
    if (name == "normalizeProviderRequestId") { const auto result = w::normalizeProviderRequestId(arg(args, 0)); co_return result ? json::value(*result) : json::value(nullptr); }
    if (name == "logRedactedUpstreamError") {
      rt::console::Capture capture({rt::console::Method::error});
      w::logRedactedUpstreamError(str(args, 0), reason(arg(args, 1)));
      json::array calls;
      for (const auto& call : capture.calls()) { json::array parts; for (const auto& part : call.args) parts.push_back(consoleValue(part)); calls.push_back(std::move(parts)); }
      EXPECT(rt::deepEqual(calls, row.at("console")));
      co_return undefinedValue();
    }
    if (name == "isAnswerBearingCompletion") {
      const w::CompletionAnswerBearingOutput output{uos::utils::getString(rt::get(value, "text")).value(), uos::utils::getString(rt::get(value, "refusal")), rt::getNumber(value, "toolCallCount").value()};
      co_return w::isAnswerBearingCompletion(output);
    }
    if (name == "chatCompletionHasAnswerBearingOutput") co_return w::chatCompletionHasAnswerBearingOutput(value.as_object());
    if (name == "chatChunkHasAnswerBearingOutput") co_return w::chatChunkHasAnswerBearingOutput(value.as_object());
    if (name == "deepSeekChatBodyDiagnostic") co_return w::deepSeekChatBodyDiagnostic(value.as_object());
    std::optional<rt::Response> response;
    if (name == "toCodexErrorResponse") response = w::toCodexErrorResponse(reason(arg(args, 0)), optionalStr(args, 1));
    else if (name == "toPreHeaderErrorResponse") response = w::toPreHeaderErrorResponse(reason(arg(args, 0)), str(args, 1), optionalStr(args, 2));
    else if (name == "toCerebrasErrorResponse") response = w::toCerebrasErrorResponse(reason(arg(args, 0)));
    else if (name == "toDeepSeekErrorResponse") response = w::toDeepSeekErrorResponse(reason(arg(args, 0)));
    else if (name == "toLithosErrorResponse") response = w::toLithosErrorResponse(reason(arg(args, 0)));
    else if (name == "apiKeyQuotaDispatchErrorResponse") { const auto error = reason(arg(args, 0)); try { std::rethrow_exception(error); } catch (const uos::api_key_policy::ApiKeyQuotaDispatchError& typed) { response = w::apiKeyQuotaDispatchErrorResponse(typed); } }
    else if (name == "streamCerebrasChatCompletion") {
      rt::Headers headers;
      const auto fields = raw(args, 2);
      if (const auto* pairs = rt::getArray(fields, "$headers")) for (const auto& pair : *pairs) headers.append(rt::toString(pair.as_array()[0]), rt::toString(pair.as_array()[1]));
      else if (fields.is_object()) for (const auto& field : fields.as_object()) headers.set(std::string(field.key()), rt::toString(field.value()));
      response = w::streamCerebrasChatCompletion(value.as_object(), rt::truthy(raw(args, 1)), headers);
    }
    if (!response) throw rt::JsError("unhandled recorded function " + std::string(name));
    auto task = responseValue(*response); const auto result = co_await std::move(task); co_return result;
  }
  rt::Task<void> replay(json::object group) {
    for (const auto& row : group.at("calls").as_array()) {
      const auto before = rt::jsonStringify(row.at("args"));
      auto task = invoke(row.as_object()); const auto actual = co_await std::move(task);
      const auto& expected = row.at("result");
      if (!rt::deepEqual(actual, expected)) rt::test::report(__FILE__, __LINE__, std::string(rt::getString(row, "name").value()) + "\nactual: " + rt::jsonStringify(actual) + "\nexpected: " + rt::jsonStringify(expected), false);
      EXPECT_EQ(rt::jsonStringify(row.at("args")), before);
    }
  }
#include "transport_normalization_vectors.inc"
  const bool recorded = [] {
    const auto bytes = rt::gzip::gunzipSync(rt::fromBase64(recordedVectors)); const auto vectors = rt::jsonParse(rt::decodeUtf8(bytes));
    for (const auto& group : vectors.as_array()) rt::test::registry().push_back({rt::toString(rt::get(group, "name")), [group] {
      Isolated isolated; rt::FakeClock clock; rt::Loop loop(clock); auto task = replay(group.as_object()); rt::blockOn(loop, std::move(task));
    }});
    return true;
  }();
}
ASYNC_TEST("upstream wire: provider error bodies are bounded, whitelisted and never reflected wholesale") {
  Isolated isolated;
  const rt::console::Capture quiet({rt::console::Method::warn});
  const auto translate = [](const std::string& provider, rt::Response response, std::optional<json::object> diagnostic = std::nullopt) {
    return provider == "cerebras" ? w::toCerebrasUpstreamErrorResponse(response) : provider == "deepseek" ? w::toDeepSeekUpstreamErrorResponse(response, std::nullopt, diagnostic) : w::toLithosUpstreamErrorResponse(response, std::nullopt, diagnostic);
  };
  const rt::Response cerebrasInput(R"({"error":{"message":"  upstream exploded  ","code":"invalid_request"}})", {.status = 429, .headers = rt::Headers{{"Retry-After", "7"}, {"x-ratelimit-requests-limit", "100"}}});
  auto first = translate("cerebras", cerebrasInput); auto cerebras = co_await std::move(first);
  EXPECT_EQ(cerebras.status(), 429); EXPECT_EQ(cerebras.headers().get("Retry-After"), std::optional<std::string>("7"));
  auto cerebrasBody = co_await cerebras.json(); EXPECT_EQ(rt::getString(*rt::getObject(cerebrasBody, "error"), "message"), std::optional<std::string_view>("upstream exploded")); EXPECT_EQ(rt::getString(*rt::getObject(cerebrasBody, "error"), "code"), std::optional<std::string_view>("invalid_request"));
  for (const auto& pair : std::vector<std::pair<std::string, double>>{{"<html>bad gateway</html>", 502}, {"7", 408}}) {
    const rt::Response input(pair.first, {.status = pair.second}); auto task = translate("cerebras", input); auto response = co_await std::move(task); const auto body = co_await response.json();
    EXPECT_EQ(rt::getString(*rt::getObject(body, "error"), "message"), std::optional<std::string_view>("Cerebras upstream returned an error."));
    EXPECT_EQ(rt::getString(*rt::getObject(body, "error"), "code"), std::optional<std::string_view>("cerebras_upstream_error"));
    EXPECT_EQ(rt::getString(*rt::getObject(body, "error"), "type"), std::optional<std::string_view>("server_error"));
  }
  const rt::Response deepseekInput(R"({"error":{"message":"deepseek exploded","code":"deepseek_code"}})", {.status = 503, .headers = rt::Headers{{"Retry-After", "3"}}});
  const std::optional<json::object> deepseekDiagnostic = json::object{{"model", "deepseek-chat"}};
  auto deepseekTask = translate("deepseek", deepseekInput, deepseekDiagnostic); auto deepseek = co_await std::move(deepseekTask); const auto deepseekBody = co_await deepseek.json();
  EXPECT_EQ(deepseek.status(), 503); EXPECT_EQ(rt::getString(*rt::getObject(deepseekBody, "error"), "code"), std::optional<std::string_view>("deepseek_code")); EXPECT_EQ(deepseek.headers().get("x-uos-upstream"), std::optional<std::string>("deepseek"));
  const rt::Response deepseekNonJson("[]", {.status = 502}); auto nonJsonTask = translate("deepseek", deepseekNonJson); auto nonJson = co_await std::move(nonJsonTask); const auto nonJsonBody = co_await nonJson.json(); EXPECT_EQ(rt::getString(*rt::getObject(nonJsonBody, "error"), "code"), std::optional<std::string_view>("deepseek_upstream_error"));
  const rt::Response lithosInput(R"({"object":"error","message":"engine refused","type":"BadRequestError","code":400})", {.status = 429, .headers = rt::Headers{{"retry-after-ms", "1500"}, {"Retry-After", "2"}, {"x-ratelimit-requests-remaining", "0"}}});
  const std::optional<json::object> lithosDiagnostic = json::object{{"model", "moonshotai/Kimi-K3"}};
  auto lithosTask = translate("lithos", lithosInput, lithosDiagnostic); auto lithos = co_await std::move(lithosTask); const auto lithosBody = co_await lithos.json();
  EXPECT_EQ(lithos.status(), 429); EXPECT(!lithos.headers().get("x-should-retry")); EXPECT_EQ(lithos.headers().get("retry-after-ms"), std::optional<std::string>("1500"));
  EXPECT_EQ(rt::getString(*rt::getObject(lithosBody, "error"), "message"), std::optional<std::string_view>("engine refused")); EXPECT_EQ(rt::getString(*rt::getObject(lithosBody, "error"), "code"), std::optional<std::string_view>("rate_limit_exceeded")); EXPECT_EQ(rt::getString(*rt::getObject(lithosBody, "error"), "type"), std::optional<std::string_view>("rate_limit_error"));
  struct Refusal { double status; std::string text; std::optional<std::string> retry; std::string code; };
  const std::vector<Refusal> refusals{{402, R"({"error":{"message":"no credit","code":"insufficient_credit"}})", "false", "insufficient_credit"}, {402, "{}", "true", "insufficient_quota"}, {401, "{}", "false", "auth_invalid"}, {404, "{}", "false", "model_not_found"}, {429, R"({"error":{"type":"input_tokens"}})", std::nullopt, "rate_limit_exceeded"}, {429, R"({"error":{"type":"provider_overloaded"}})", std::nullopt, "provider_overloaded"}, {500, "not json", std::nullopt, "lithos_upstream_error"}};
  for (std::size_t index = 0; index < refusals.size(); ++index) {
    const auto& fixture = refusals[index]; rt::Headers headers; if (index == 1) headers.set("x-should-retry", "true");
    const rt::Response input(fixture.text, {.status = fixture.status, .headers = headers}); auto task = translate("lithos", input); auto response = co_await std::move(task); const auto body = co_await response.json();
    EXPECT_EQ(response.headers().get("x-should-retry"), fixture.retry); EXPECT_EQ(uos::utils::getString(rt::get(*rt::getObject(body, "error"), "code")), std::optional(fixture.code));
    if (index == refusals.size() - 1) EXPECT_EQ(rt::getString(*rt::getObject(body, "error"), "message"), std::optional<std::string_view>("LithosAI upstream returned an error."));
  }
}
ASYNC_TEST("upstream wire: large chat diagnostics log a bounded private tail with original indices") {
  Isolated isolated;
  // json::value is a tree, so the source's shared50k-by50k graph cannot be represented.
  // Keep all observed counts/tail positions and private nested markers; omitted history uses
  // small unused arrays and repeated nested slots are null. The unchanged Bun assertion passed.
  const std::vector<std::string> markers{"PRIVATE_CONTENT_666", "PRIVATE_TOOL_NAME_666", "PRIVATE_ID_666", "PRIVATE_COOKIE_666", "PRIVATE_METADATA_666"};
  json::array parts(50'000, nullptr), calls(50'000, nullptr);
  parts[0] = json::object{{"type", markers[4]}, {"text", markers[0]}};
  calls[0] = json::object{{"id", markers[2]}, {"function", json::object{{"name", markers[1]}, {"arguments", markers[0]}}}};
  json::array messages;
  for (std::size_t index = 0; index < 50'000; ++index) {
    if (index == 0) messages.push_back(json::value(markers[4]));
    else if (index == 7) messages.push_back(json::object{{"role", "user"}, {"content", markers[0]}});
    else if (index < 49'984) messages.push_back(json::object{{"role", "assistant"}});
    else messages.push_back(json::object{{"role", index == 49'999 ? rt::repeat(markers[4], 10'000) : "assistant"}, {"content", parts}, {"reasoning_content", markers[0]}, {"tool_calls", calls}, {"cookie", markers[3]}});
  }
  const auto diagnostic = w::deepSeekChatBodyDiagnostic({{"model", rt::repeat(markers[4], 10'000)}, {"reasoning_effort", markers[4]}, {"tool_choice", markers[4]}, {"tools", calls}, {"max_tokens", rt::makeNumber(rt::Number::POSITIVE_INFINITY)}, {"messages", messages}});
  EXPECT_EQ(rt::getNumber(diagnostic, "message_count"), std::optional(50'000.0)); EXPECT_EQ(rt::getNumber(diagnostic, "last_user_index"), std::optional(7.0)); EXPECT_EQ(rt::getNumber(diagnostic, "omitted_message_count"), std::optional(49'984.0)); EXPECT_EQ(rt::getNumber(diagnostic, "tools"), std::optional(50'000.0));
  for (const auto key : {"model", "reasoning_effort", "tool_choice", "max_tokens"}) EXPECT(rt::get(diagnostic, key)->is_null());
  const auto* tail = rt::getArray(diagnostic, "messages"); ASSERT(tail); EXPECT_EQ(tail->size(), 16u);
  for (std::size_t index = 0; index < tail->size(); ++index) {
    const auto& row = (*tail)[index]; EXPECT_EQ(rt::getNumber(row, "index"), std::optional(49'984.0 + static_cast<double>(index))); EXPECT_EQ(rt::getBool(row, "after_last_user"), std::optional(true)); EXPECT_EQ(rt::getString(row, "content"), std::optional<std::string_view>("parts")); EXPECT_EQ(rt::getNumber(row, "tool_calls"), std::optional(50'000.0));
  }
  EXPECT(rt::get(tail->back(), "role")->is_null());
  const rt::console::Capture capture({rt::console::Method::warn});
  const auto body = rt::jsonStringify(json::object{{"error", json::object{{"message", std::string(2'000, '\0')}, {"code", std::string(400, '\0')}}}});
  const std::optional<json::object> view = diagnostic;
  for (int provider = 0; provider < 2; ++provider) {
    const rt::Response input(body, {.status = 400});
    auto task = provider == 0 ? w::toDeepSeekUpstreamErrorResponse(input, std::nullopt, view) : w::toLithosUpstreamErrorResponse(input, std::nullopt, view);
    auto response = co_await std::move(task); EXPECT_EQ(response.status(), 400); w::cancelResponseBody(response);
  }
  EXPECT_EQ(capture.calls().size(), 2u);
  for (const auto& call : capture.calls()) {
    ASSERT_EQ(call.args.size(), 2u); EXPECT(call.args[1].isString()); const auto& line = call.args[1].string(); EXPECT(rt::utf8ByteLength(line) < 16'384);
    const auto parsed = rt::jsonParse(line); EXPECT(rt::deepEqual(*rt::get(parsed, "request"), diagnostic));
    for (const auto& marker : markers) EXPECT(!rt::includes(line, marker));
  }
}
ASYNC_TEST("upstream wire: cancelling an already-closed response body is swallowed") {
  Isolated isolated;
  int cancelled = 0;
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.cancel = [&](rt::Reason) { ++cancelled; throw rt::JsError("already closed"); };
  const rt::ByteStream stream(source); const rt::Response response(stream);
  w::cancelResponseBody(response); co_await rt::microtask(); co_await rt::microtask();
  EXPECT_EQ(cancelled, 1);
  w::cancelResponseBody(response); co_await rt::microtask();
}
TEST("upstream wire: response request-id aliases retain nullish precedence") {
  Isolated isolated;
  const rt::Response one("", {.headers = rt::Headers{{"X-Oneapi-Request-Id", "req-2"}}});
  EXPECT_EQ(w::providerRequestIdFromResponse(one), std::optional<std::string>("req-2"));
  const rt::Response absent("", {.headers = rt::Headers{}}); EXPECT(!w::providerRequestIdFromResponse(absent));
  const rt::Response firstBlank("", {.headers = rt::Headers{{"X-Request-Id", ""}, {"X-Api-Request-Id", "later"}}}); EXPECT(!w::providerRequestIdFromResponse(firstBlank));
}

namespace {
#include "input_normalization_models_vectors.inc"
  const bool recordedModels = [] {
    const auto bytes = rt::gzip::gunzipSync(rt::fromBase64(modelVectors)); const auto vectors = rt::jsonParse(rt::decodeUtf8(bytes));
    for (const auto& group : vectors.as_array()) rt::test::registry().push_back({rt::toString(rt::get(group, "name")), [group] {
      Isolated isolated;
      if (const auto* environment = rt::getObject(group, "env")) for (const auto& entry : *environment) rt::env::set(std::string(entry.key()), rt::toString(entry.value()));
      rt::FakeClock clock; rt::Loop loop(clock); auto task = replay(group.as_object()); rt::blockOn(loop, std::move(task));
    }});
    return true;
  }();
}
ASYNC_TEST("OpenAI upstream refusals preserve envelope fields auth warnings and bounded payload truth") {
  Isolated isolated;
#include "upstream_wire_extra_vectors.inc"
  const auto bytes = rt::gzip::gunzipSync(rt::fromBase64(wireExtraVectors)); const auto rows = rt::jsonParse(rt::decodeUtf8(bytes));
  ASSERT_EQ(rows.as_array().size(), 7u);
  for (const auto& row : rows.as_array()) {
    const auto& input = row.at("input"); rt::Headers headers;
    for (const auto& field : input.at("headers").as_object()) headers.set(std::string(field.key()), rt::toString(field.value()));
    const rt::Response upstream(rt::toString(input.at("body")), {.status = rt::getNumber(input, "status").value(), .statusText = "Fixture Status", .headers = headers});
    const std::string provider = "metered"; const rt::AbortController controller;
    auto task = w::toOpenAiUpstreamErrorResponse(upstream, provider, controller.signal()); auto response = co_await std::move(task);
    auto encodedTask = responseValue(response); auto actual = co_await std::move(encodedTask); auto expected = row.at("result"); rt::objectDelete(actual.as_object(), "$response");
    EXPECT(rt::deepEqual(actual, expected));
  }
}
ASYNC_TEST("Lithos upstream504 provenance wins over a later request abort") {
  Isolated isolated;
  const rt::console::Capture quiet({rt::console::Method::warn});
  const rt::AbortController controller;
  const auto local = w::toLithosErrorResponse(rt::makeReason(rt::DOMException("late", "TimeoutError")));
  EXPECT(w::isGatewayDeadlineResponse(local, controller.signal()));
  const rt::Response input(R"({"error":{"message":"provider timed out"}})", {.status = 504});
  auto task = w::toLithosUpstreamErrorResponse(input); const auto upstream = co_await std::move(task);
  EXPECT(!w::isGatewayDeadlineResponse(upstream, controller.signal()));
  const rt::Response unmarked("", {.status = 504}); EXPECT(!w::isGatewayDeadlineResponse(unmarked, controller.signal()));
  controller.abort(); EXPECT(!w::isGatewayDeadlineResponse(upstream, controller.signal())); EXPECT(w::isGatewayDeadlineResponse(local, controller.signal())); EXPECT(w::isGatewayDeadlineResponse(unmarked, controller.signal()));
  EXPECT(!w::isGatewayDeadlineResponse(rt::Response("", {.status = 503}), controller.signal()));
}
