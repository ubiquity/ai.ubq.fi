// Links: tests/compaction_summary_forwarding_test.cpp
// src/kv.cpp src/utils.cpp src/deepseek/forwarded_payload_policy.cpp src/deepseek/chat_projection.cpp
// src/deepseek/responses_payload.cpp src/deepseek/index.cpp src/provider/cerebras_responses.cpp src/request_policy.cpp
// src/deepseek/responses.cpp src/config.cpp src/provider/cerebras_handlers.cpp src/provider/cerebras.cpp
// src/provider/selection.cpp src/deepseek/responses_stream.cpp src/deepseek/handlers.cpp src/openai_telemetry.cpp
// src/upstream_wire.cpp src/cache/scope_targets.cpp src/lib/jev_compaction/state.cpp src/openai.cpp
// src/responses_failover_stream.cpp src/chat/stream_translation.cpp src/runtime_config.cpp src/provider/health.cpp
// src/models/codex_models_fetch.cpp src/bounded_response_body.cpp src/codex/auth.cpp src/codex/auth_refresh.cpp
// src/provider/removed_provider_telemetry.cpp src/models/codex_models.cpp src/release.cpp src/provider/stream_relay.cpp
// src/codex/dispatch.cpp src/deepseek/stream.cpp src/provider/surplus.cpp src/codex/capacity_routing.cpp
// src/responses_stream.cpp src/deepseek/flash_aliases.cpp src/authority/native_codex_capacity.cpp src/provider/lithos.cpp
// src/openai_schema.cpp src/codex/routing_state.cpp src/models/openrouter_models.cpp src/types.cpp
// src/authority/native_codex_contracts.cpp src/inference_deadline.cpp src/lib/jev_compaction/types.cpp src/codex/routing_mutations.cpp
// src/authority/native_codex_mutations.cpp src/provider/cerebras_rate_limits.cpp src/models/codex_models_availability.cpp src/provider/claude_code_models.cpp
// src/provider/surplus_claude_request.cpp src/provider/surplus_claude_chat_request.cpp src/codex/external_auth.cpp src/provider/claude_code_accounts.cpp
// src/provider/anthropic_replay.cpp src/provider/capacity_events.cpp src/codex/responses_error.cpp src/provider/claude_code_credentials.cpp
// src/http.cpp src/defaults.cpp src/provider/lithos_models.cpp src/storage/kv.cpp
// src/codex/native_auth.cpp src/storage/sqlite_readonly.cpp src/provider/capacity.cpp src/provider/capacity_history.cpp
// src/deepseek/context_admission_failure.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/capacity_parse.cpp
// src/provider/capacity_contract.cpp src/provider/metered.cpp src/provider/anthropic_chat_responses.cpp src/codex/banked_reset.cpp
// src/provider/anthropic_chat_stream.cpp src/provider/claude_capacity_rollups.cpp src/codex/rate_limit_429.cpp src/provider/capacity_analytics.cpp
// src/provider/lithos_rate_limits.cpp src/provider/claude_capacity_history.cpp src/provider/capacity_rollups.cpp src/authority/native_codex_evaluation.cpp
// src/codex/native_auth_transport.cpp src/cache/prompt_analytics_read.cpp src/cache/prompt_analytics_core.cpp src/provider/anthropic_response_wire.cpp
// src/paid_fallback/ledger_stats.cpp src/codex/experiment.cpp src/auth/relay.cpp src/provider/claude_code.cpp
// src/runtime/websocket.cpp src/lib/jev_compaction/codex_items.cpp src/authority/native_codex_429.cpp
// src/codex/account_routing.cpp src/metered_quota.cpp src/storage/native_kv.cpp src/provider/anthropic_responses.cpp
// src/codex/routing_serial.cpp src/codex/banked_reset_provider.cpp src/authority/native_codex.cpp src/codex/routing_evaluation.cpp
// src/codex/reset_settings.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/request.cpp src/codex/overage_settings.cpp
// Owned API parity from the original Bun tests. Upstream/handler assertions remain separate.
#include "helpers/deepseek_port_replay.hpp"
namespace {
#include "compaction_summary_forwarding_vectors.inc"
  // The checked-in bundle combines 15 forwarding traces, expanding to about 68 MiB.
  const bool recorded = uos::tests::helpers::deepseek_port_replay::registerReplay(recordedVectors, 72 * 1024 * 1024);
}

#include "uos/rt/console.hpp"
TEST("compaction summary: the exact stock prefix and 70 KiB envelope emit the complete exemption log") {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace fp = uos::deepseek::forwarded_payload_policy;
  namespace cp = uos::deepseek::chat_projection;
  std::string text = std::string(fp::COMPACTION_SUMMARY_POLICY.prefix) + "\n";
  text += std::string(70 * 1024 - rt::utf8ByteLength(text), 's');
  const json::object body{{"input", json::array{json::object{{"type", "message"}, {"role", "user"}, {"content", json::array{json::object{{"type", "input_text"}, {"text", text}}}}}}}};
  const auto result = cp::toDeepSeekResponsesChatBody(body, "deepseek-flash", false);
  EXPECT(std::holds_alternative<uos::deepseek::responses::DeepSeekResponsesResultOk<cp::DeepSeekResponsesChatBody>>(result));
  const auto& value = std::get<uos::deepseek::responses::DeepSeekResponsesResultOk<cp::DeepSeekResponsesChatBody>>(result).value;
  EXPECT_EQ(value.exemptions.size(), 1u);
  rt::console::Capture logs({rt::console::Method::info});
  const fp::LogForwardedPayloadExemptionsContext context{.requestId = "req_test", .model = "deepseek-flash", .promptAdmission = value.promptAdmission};
  fp::logForwardedPayloadExemptions(value.exemptions, context);
  EXPECT_EQ(logs.calls().size(), 1u);
  EXPECT_EQ(logs.calls()[0].args[0].string(), "[ai.ubq.fi] forwarding_exemption");
  json::object expected{{"policy", fp::FORWARDED_PAYLOAD_POLICY.version}, {"request_id", "req_test"}, {"model", "deepseek-flash"}, {"prompt_admission", fp::toJson(value.promptAdmission)}};
  rt::objectAssign(expected, fp::toJson(value.exemptions[0])); expected["prefixBytes"] = fp::COMPACTION_SUMMARY_POLICY.prefixBytes;
  EXPECT(rt::deepEqual(rt::jsonParse(logs.calls()[0].args[1].string()), expected));
}

#include "uos/provider/cerebras_responses.hpp"
#include "uos/kv.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/rt/fetch.hpp"
ASYNC_TEST("aggregate admission: Cerebras returns 400 before any upstream dispatch") {
  namespace rt = uos::rt; namespace json = uos::json; namespace fp = uos::deepseek::forwarded_payload_policy;
  rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/c1-compaction"}}};
  uos::config::config.codexAuthJsonB64.clear(); uos::kv::setKvForTest(nullptr);
  std::string text = std::string(fp::COMPACTION_SUMMARY_POLICY.prefix) + "\n"; text += std::string(720 * 1024 - rt::utf8ByteLength(text), 's');
  json::object body{{"input", json::array{json::object{{"type", "message"}, {"role", "user"}, {"content", json::array{json::object{{"type", "input_text"}, {"text", text}}}}}}}};
  rt::Request request("https://ai.ubq.fi/v1/responses", {.method = "POST", .headers = rt::Headers{{"Content-Type", "application/json"}}, .body = rt::jsonStringify(body)});
  int calls = 0; rt::GlobalFetchOverride fetch(rt::Fetch([&calls](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> { ++calls; throw rt::JsError("aggregate overflow must not dispatch"); co_return rt::Response(); }));
  std::string model = "gpt-oss-120b"; auto task = uos::provider::cerebras_responses::handleCerebrasResponses(request, body, model); auto response = co_await std::move(task);
  EXPECT_EQ(response.status(), 400); auto payload = co_await response.json(); const auto* error = rt::getObject(payload, "error"); EXPECT(error);
  EXPECT_EQ(rt::getString(*error, "code"), "context_length_exceeded"); EXPECT_EQ(rt::getString(*error, "param"), "input"); EXPECT_EQ(calls, 0);
}
