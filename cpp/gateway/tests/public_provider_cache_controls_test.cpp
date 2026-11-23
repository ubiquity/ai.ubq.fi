// Links: tests/public_provider_cache_controls_test.cpp
// src/responses_failover_stream.cpp src/provider/stream_relay.cpp src/chat/stream_translation.cpp src/openai_telemetry.cpp src/upstream_wire.cpp
// src/deepseek/index.cpp src/provider/lithos.cpp src/provider/cerebras_rate_limits.cpp src/utils.cpp src/provider/removed_provider_telemetry.cpp
// src/codex/responses_error.cpp src/deepseek/stream.cpp src/deepseek/responses.cpp src/responses_stream.cpp src/config.cpp
// src/bounded_response_body.cpp src/deepseek/responses_stream.cpp src/provider/lithos_rate_limits.cpp src/deepseek/responses_payload.cpp src/http.cpp
// src/provider/cerebras.cpp src/release.cpp src/auth/relay.cpp src/request_policy.cpp src/provider/claude_code_models.cpp
// src/provider/surplus.cpp src/models/codex_models.cpp src/provider/surplus_claude_request.cpp src/provider/anthropic_chat_responses.cpp src/kv.cpp
// src/provider/claude_code_accounts.cpp src/provider/selection.cpp src/provider/claude_code_credentials.cpp src/defaults.cpp src/models/codex_models_fetch.cpp
// src/provider/claude_code.cpp src/provider/anthropic_response_wire.cpp src/types.cpp src/models/codex_models_availability.cpp src/storage/kv.cpp
// src/storage/sqlite_readonly.cpp src/codex/dispatch.cpp src/provider/claude_code_quota.cpp src/provider/anthropic_responses.cpp src/codex/auth.cpp
// src/provider/capacity_events.cpp src/provider/claude_code_usage_schema.cpp src/codex/external_auth.cpp src/provider/anthropic_chat_stream.cpp src/provider/metered.cpp
// src/provider/surplus_claude_chat_request.cpp src/provider/anthropic_replay.cpp src/codex/native_auth.cpp src/provider/capacity_contract.cpp src/codex/routing_mutations.cpp
// src/codex/banked_reset.cpp src/authority/native_codex_mutations.cpp src/codex/capacity_routing.cpp src/authority/native_codex_capacity.cpp src/codex/auth_refresh.cpp
// src/codex/reset_settings.cpp src/runtime_config.cpp src/authority/native_codex_contracts.cpp src/codex/banked_reset_provider.cpp
// src/storage/native_kv.cpp src/provider/capacity.cpp src/cache/prompt_analytics_read.cpp src/provider/health.cpp src/cache/prompt_analytics_core.cpp
// src/codex/native_auth_transport.cpp src/provider/capacity_history.cpp src/provider/claude_capacity_history.cpp src/codex/experiment.cpp src/provider/capacity_parse.cpp
// src/provider/claude_capacity_rollups.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity_analytics.cpp src/provider/capacity_rollups.cpp src/codex/account_routing.cpp
// src/codex/routing_serial.cpp src/codex/routing_evaluation.cpp src/authority/native_codex_evaluation.cpp src/codex/routing_state.cpp src/authority/native_codex.cpp
// src/codex/overage_settings.cpp src/metered_quota.cpp src/codex/rate_limit_429.cpp src/runtime/websocket.cpp src/authority/native_codex_429.cpp
// src/lib/jev_compaction/codex_items.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/request.cpp src/lib/jev_compaction/state.cpp src/api_key_policy.cpp
// src/api_keys.cpp src/authority/native_api_key_policy.cpp src/authority/native_api_key.cpp src/authority/native_paid_policy.cpp src/authority/native_paid.cpp
// src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics.cpp src/cache/telemetry_gate.cpp src/codex/banked_reset_claim.cpp src/codex/banked_reset_pool.cpp
// src/codex/banked_reset_submission.cpp src/codex/index.cpp src/codex/responses_operations.cpp src/codex/responses_runners.cpp src/codex/responses_state.cpp
// src/debug_routing.cpp src/deepseek/chat_projection.cpp src/deepseek/context_admission_failure.cpp src/deepseek/flash_aliases.cpp src/deepseek/forwarded_payload_policy.cpp
// src/deepseek/handlers.cpp src/deepseek/waterfall_handler.cpp src/deepseek/waterfall.cpp src/inference_deadline.cpp src/input_normalization.cpp
// src/models/metadata.cpp src/models/openrouter_models.cpp src/openai_schema.cpp src/openai.cpp src/optional_telemetry_queue.cpp
// src/paid_fallback/claude_code_route.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp
// src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/removed_provider.cpp src/paid_fallback/rollups.cpp src/paid_fallback/routing.cpp
// src/provider/cerebras_handlers.cpp src/provider/cerebras_responses.cpp src/provider/claude_code_access.cpp src/provider/lithos_handlers.cpp src/provider/lithos_models.cpp
// src/provider/lithos_streams.cpp src/provider/openrouter_handlers.cpp src/provider/openrouter_request_controls.cpp src/provider/openrouter_streams.cpp src/provider/openrouter_web_tools.cpp
// src/provider/openrouter.cpp src/provider/presentation.cpp src/provider/removed_provider_circuit.cpp src/recent_model_context.cpp src/request.cpp
// src/responses_attempts.cpp src/responses_buffered.cpp src/responses_handler.cpp src/responses_native_headers.cpp src/responses_request.cpp
// src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/runtime/http.cpp
// src/sentinel/upstream_capture.cpp src/storage/key_compatibility.cpp src/storage/value_compatibility.cpp src/authority/native_paid_contracts.cpp src/lib/jev_compaction/types.cpp
// src/chat/envelope.cpp src/chat/completions.cpp tests/helpers/openai_compat_harness.cpp
// Port of tests/public-provider-cache-controls.test.ts. All four source cases use real public module handlers.
#include "tests/helpers/openai_compat_harness.hpp"
#include "uos/chat/envelope.hpp"
#include "uos/responses_handler.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/provider/openrouter.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/provider/selection.hpp"
#include "uos/openai_schema.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/string.hpp"
namespace rt=uos::rt;namespace json=uos::json;namespace h=uos::tests::helpers::openai_compat_harness;
namespace {
  constexpr std::string_view CLAUDE_MODEL="~anthropic/claude-opus-latest";
  struct Catalogue {
    h::Fixture fixture;
    Catalogue(){rt::env::set("OPENROUTER_API_KEY","fixture-key");uos::models::openrouter_models::resetOpenRouterModelsCacheForTest();uos::models::openrouter_models::setOpenRouterModelsFetchForTest(rt::Fetch([](rt::RequestInfo,rt::RequestInit){return rt::Response::json(json::object{{"data",json::array{json::object{{"id",CLAUDE_MODEL}},json::object{{"id","vendor/alpha"}}}}});}));}
    ~Catalogue(){uos::models::openrouter_models::setOpenRouterModelsFetchForTest({});uos::models::openrouter_models::resetOpenRouterModelsCacheForTest();}
    rt::Task<void> seed(){auto task=uos::models::openrouter_models::fetchOpenRouterModels();co_await std::move(task);}
  };
  json::object bodyFor(bool chat,const std::string& model=std::string(CLAUDE_MODEL)){json::object body{{"model",model}};if(chat)body["messages"]=json::array{json::object{{"role","user"},{"content","Stable prefix"}}};else body["input"]="Stable prefix";return body;}
  rt::Task<rt::Response> call(bool chat,json::object body){const auto req=chat?h::specialProviderChatRequest(body):h::specialProviderResponsesRequest(body);return chat?uos::chat::envelope::handleChatCompletions(req):uos::responses_handler::handleResponses(req);}
}
ASYNC_TEST("public Claude OpenRouter routes preserve an explicit native cache policy through validation") {
  Catalogue fixture;co_await fixture.seed();
  for(const bool chat:std::vector<bool>{false,true}){
    const json::object policy{{"type","ephemeral"},{"ttl","1h"}};int calls=0;
    rt::GlobalFetchOverride fetch(h::createFetchMock([chat,policy,&calls](std::string url,std::optional<std::string> text,rt::RequestInit){
      ++calls;EXPECT_EQ(url,std::string(chat?uos::provider::openrouter::OPENROUTER_CHAT_COMPLETIONS_URL:uos::provider::openrouter::OPENROUTER_RESPONSES_URL));EXPECT(text);const auto body=rt::jsonParse(*text);EXPECT(rt::deepEqual(*rt::get(body,"cache_control"),policy));EXPECT_EQ(rt::getString(body,"model"),CLAUDE_MODEL);EXPECT_EQ(rt::getString(body,"prompt_cache_key"),"caller-key");
      const auto response=rt::jsonParse(chat?R"({"id":"chat_fixture","object":"chat.completion","choices":[{"index":0,"message":{"role":"assistant","content":"OK"},"finish_reason":"stop"}],"usage":{"prompt_tokens":100,"completion_tokens":1,"total_tokens":101,"prompt_tokens_details":{"cached_tokens":90}}})":R"({"id":"resp_fixture","object":"response","status":"completed","output":[{"type":"message","role":"assistant","content":[{"type":"output_text","text":"OK"}]}],"usage":{"input_tokens":100,"output_tokens":1,"total_tokens":101,"input_tokens_details":{"cached_tokens":90}}})");
      return rt::fulfilledTask(rt::Response::json(response));
    }));
    auto body=bodyFor(chat);body["cache_control"]=policy;body["prompt_cache_key"]="caller-key";auto task=call(chat,body);const auto response=co_await std::move(task);EXPECT_EQ(response.status(),200);EXPECT_EQ(response.headers().get("x-uos-upstream"),std::optional<std::string>("openrouter"));EXPECT_EQ(calls,1);auto read=response.text();co_await std::move(read);
  }
  co_await h::settleHarness();
}
ASYNC_TEST("Claude's native cache extension never widens official schemas or other provider routes") {
  for(const auto key:uos::openai_schema::CHAT_COMPLETIONS_REQUEST_KEYS)EXPECT(key!="cache_control");
  for(const auto key:uos::openai_schema::RESPONSES_REQUEST_KEYS)EXPECT(key!="cache_control");
  Catalogue fixture;co_await fixture.seed();int calls=0;rt::GlobalFetchOverride fetch(rt::Fetch([&calls](rt::RequestInfo,rt::RequestInit)->rt::Response{++calls;throw rt::JsError("invalid provider extensions must never dispatch");}));
  for(const bool chat:std::vector<bool>{false,true})for(const auto& model:std::vector<std::string>{"vendor/alpha","gpt-5-fixture","claude-opus-5.5","~anthropic/claude-unlisted"}){auto body=bodyFor(chat,model);body["cache_control"]=json::object{{"type","ephemeral"}};auto task=call(chat,body);const auto response=co_await std::move(task);EXPECT_EQ(response.status(),400);const auto payload=co_await response.json();EXPECT(rt::includes(std::string(rt::getString(*rt::get(payload,"error"),"message").value()),"Unrecognized request argument supplied: cache_control"));}
  const std::vector<std::string> ids{"codex"};h::ProviderSelectionScope selection(ids);
  for(const bool chat:std::vector<bool>{false,true}){auto body=bodyFor(chat);body["cache_control"]=json::object{{"type","ephemeral"}};auto task=call(chat,body);const auto response=co_await std::move(task);EXPECT_EQ(response.status(),400);auto read=response.text();co_await std::move(read);}EXPECT_EQ(calls,0);co_await h::settleHarness();
}
ASYNC_TEST("public Claude cache policies reject malformed native controls before transport") {
  Catalogue fixture;co_await fixture.seed();int calls=0;rt::GlobalFetchOverride fetch(rt::Fetch([&calls](rt::RequestInfo,rt::RequestInit)->rt::Response{++calls;throw rt::JsError("malformed cache policies must never dispatch");}));
  const std::vector<std::pair<json::value,std::string>> cases{{nullptr,"cache_control"},{json::object{{"type","permanent"}},"cache_control.type"},{json::object{{"type","ephemeral"},{"ttl","24h"}},"cache_control.ttl"},{json::object{{"type","ephemeral"},{"unknown",true}},"cache_control.unknown"}};
  for(const bool chat:std::vector<bool>{false,true})for(const auto& [control,param]:cases){auto body=bodyFor(chat);body["cache_control"]=control;auto task=call(chat,body);const auto response=co_await std::move(task);EXPECT_EQ(response.status(),400);const auto payload=co_await response.json();EXPECT_EQ(rt::getString(*rt::get(payload,"error"),"param"),param);}
  EXPECT_EQ(calls,0);co_await h::settleHarness();
}
ASYNC_TEST("public Cerebras Responses retains its documented cache-routing key at the actual Chat transport") {
  Catalogue fixture;co_await fixture.seed();rt::env::set("CEREBRAS_API_KEY","fixture-cerebras-key");int calls=0;
  rt::GlobalFetchOverride fetch(h::createFetchMock([&calls](std::string url,std::optional<std::string> text,rt::RequestInit){++calls;EXPECT_EQ(url,std::string(uos::provider::cerebras::CEREBRAS_CHAT_COMPLETIONS_URL));EXPECT(text);const auto body=rt::jsonParse(*text);EXPECT_EQ(rt::getString(body,"prompt_cache_key"),"caller-cerebras-key");const auto payload=rt::jsonParse(R"({"id":"cerebras_fixture","object":"chat.completion","created":1,"model":"qwen-3.8-27b","choices":[{"index":0,"message":{"role":"assistant","content":"OK"},"finish_reason":"stop"}],"usage":{"prompt_tokens":100,"completion_tokens":1,"total_tokens":101,"prompt_tokens_details":{"cached_tokens":90}}})");return rt::fulfilledTask(rt::Response::json(payload));}));
  const json::object body{{"model",uos::provider::cerebras::CEREBRAS_QWEN_3_8_27B_MODEL},{"input","Stable prefix"},{"reasoning",json::object{{"effort","none"}}},{"prompt_cache_key","caller-cerebras-key"}};auto task=call(false,body);const auto response=co_await std::move(task);EXPECT_EQ(response.status(),200);EXPECT_EQ(response.headers().get("x-uos-upstream"),std::optional<std::string>("cerebras"));const auto payload=co_await response.json();const auto* usage=rt::get(payload,"usage");const auto* details=rt::get(*usage,"input_tokens_details");EXPECT_EQ(rt::getNumber(*details,"cached_tokens"),std::optional<double>(90));EXPECT_EQ(calls,1);co_await h::settleHarness();
}
