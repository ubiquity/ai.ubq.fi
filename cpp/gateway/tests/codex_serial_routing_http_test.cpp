// Links: tests/codex_serial_routing_http_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
//   src/handler/serve_handler.cpp src/auth/index.cpp src/auth/passport.cpp src/api_key_policy.cpp src/utils.cpp src/auth/passkeys.cpp src/authority/native_api_key.cpp src/auth/passport_sessions.cpp src/agent_messages.cpp
//   src/auth/local_admin.cpp src/kernel/attestation.cpp src/types.cpp src/authority/native_api_key_policy.cpp src/request.cpp src/auth/local_development_key.cpp src/api_keys.cpp src/models/catalog.cpp src/authority/native_paid_policy.cpp
//   src/provider/metered.cpp src/analytics.cpp src/provider/claude_code_models.cpp src/provider/claude_code_credentials.cpp src/auth/relay.cpp src/authority/native_paid.cpp src/provider/selection.cpp src/models/codex_models_whitelist.cpp src/deepseek/index.cpp
//   src/input_normalization.cpp src/provider/openrouter.cpp src/provider/surplus_claude_request.cpp src/paid_fallback/index.cpp src/paid_fallback/ledger_admission.cpp src/request_policy.cpp src/provider/cerebras.cpp src/provider/claude_code_accounts.cpp src/models/codex_models.cpp
//   src/models/openrouter_models.cpp src/provider/surplus.cpp src/models/codex_models_fetch.cpp src/models/codex_models_availability.cpp src/deepseek/waterfall.cpp src/provider/claude_code.cpp src/provider/claude_code_quota.cpp src/models/metadata.cpp src/paid_fallback/ledger_backfill.cpp
//   src/runtime_config.cpp src/codex/dispatch.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_settlement.cpp src/authority/native_paid_contracts.cpp src/provider/claude_code_usage_schema.cpp src/responses_stream.cpp src/codex/auth.cpp src/codex/auth_refresh.cpp
//   src/provider/lithos.cpp src/codex/routing_mutations.cpp src/authority/native_codex_mutations.cpp src/provider/surplus_claude_chat_request.cpp src/handler/index.cpp src/admin/codex.cpp src/bounded_response_body.cpp src/codex/rate_limit_429.cpp src/provider/capacity_parse.cpp
//   src/kernel/policy_queue.cpp src/authority/native_codex_capacity.cpp src/http.cpp src/codex/native_auth.cpp src/authority/native_codex_contracts.cpp src/config.cpp src/admin/api_keys.cpp src/kernel/quota_reservations.cpp src/admin/claude_code.cpp
//   src/authority/native_kernel.cpp src/codex/capacity_routing.cpp src/paid_fallback/ledger_stats.cpp src/admin/kernel.cpp src/cache/scope_experiment_targets.cpp src/provider/capacity.cpp src/recent_model_context.cpp src/admin/api_key_mutations.cpp src/provider/lithos_models.cpp
//   src/codex/experiment.cpp src/admin/capacity_observation_sync.cpp src/catalog/types.cpp src/release.cpp src/cache/scope_experiment_model.cpp src/provider/claude_capacity_history.cpp src/admin/api_key_audit.cpp src/codex/account_routing.cpp src/provider/lithos_rate_limits.cpp
//   src/provider/presentation.cpp src/codex/routing_state.cpp src/provider/claude_capacity_rollups.cpp src/health.cpp src/codex/native_auth_transport.cpp src/codex/supervisor.cpp src/metered_quota.cpp src/codex/supervisor_inventory.cpp src/kv.cpp
//   src/provider/capacity_history.cpp src/codex/supervisor_config.cpp src/authority/native_kernel_contracts.cpp src/cache/prompt_analytics_read.cpp src/authority/native_codex_429.cpp src/provider/capacity_contract.cpp src/catalog/index.cpp src/openai_telemetry.cpp src/provider/health.cpp
//   src/catalog/store.cpp src/debug_routing.cpp src/defaults.cpp src/quota_projection.cpp src/codex/routing_serial.cpp src/cache/prompt_analytics_core.cpp src/provider/capacity_analytics.cpp src/provider/cerebras_rate_limits.cpp src/codex/supervisor_follow.cpp
//   src/catalog/models.cpp src/admin/defaults.cpp src/codex/routing_evaluation.cpp src/provider/capacity_events.cpp src/codex/banked_reset_provider.cpp src/kernel/quota_write.cpp src/provider/capacity_rollups.cpp src/admin/sentinel_incident.cpp src/cache/kv_migration_base.cpp
//   src/kernel/usage.cpp src/codex/external_auth.cpp src/provider/anthropic_responses.cpp src/provider/anthropic_response_wire.cpp src/authority/native_api_key_audit.cpp src/provider/anthropic_replay.cpp src/responses_failover_stream.cpp src/sentinel/incident_outbox.cpp src/kernel/quota_v2.cpp
//   src/codex/supervisor_brief.cpp src/provider/anthropic_chat_responses.cpp src/authority/native_codex.cpp src/provider/claude_code_access.cpp src/codex/overage_settings.cpp src/static.cpp src/admin/error_log.cpp src/sentinel/replay_retention_schema.cpp src/sentinel/replay_admin.cpp
//   src/sentinel/replay_read.cpp src/admin/codex_reset.cpp src/cache/scope_experiment.cpp src/codex/supervisor_secret.cpp src/codex/banked_reset.cpp src/codex/reset_settings.cpp src/cache/kv_migration_validation.cpp src/handler/http.cpp src/codex/supervisor_transport.cpp
//   src/admin/chart_source_proxy.cpp src/sentinel/replay_envelope.cpp src/paid_fallback/rollups.cpp src/codex/manual_reset.cpp src/handler/terminal_route.cpp src/responses_handler.cpp src/responses_attempts.cpp src/sentinel/replay_model.cpp src/provider/removed_provider_telemetry.cpp
//   src/codex/responses_error.cpp src/authority/native_codex_evaluation.cpp src/sentinel/replay_store.cpp src/cache/kv_migration_usage.cpp src/responses_request.cpp src/cache/telemetry_gate.cpp src/cache/kv_migration_paid_fallback.cpp src/paid_fallback/routing.cpp src/runtime/websocket.cpp
//   src/codex/banked_reset_pool.cpp src/live/handler.cpp src/paid_fallback/health.cpp src/provider/anthropic_chat_stream.cpp src/openai.cpp src/codex/supervisor_log.cpp src/codex/banked_reset_claim.cpp src/paid_fallback/removed_provider.cpp src/codex/banked_reset_submission.cpp
//   src/sentinel/replay_observation.cpp src/deepseek/flash_aliases.cpp src/cache/scope_targets.cpp src/decisions/handlers.cpp src/kv_conversion/profile_json.cpp src/provider/removed_provider_circuit.cpp src/sentinel/replay_retention.cpp src/upstream_wire.cpp src/inference_deadline.cpp
//   src/sentinel/replay_retention_bootstrap.cpp src/sentinel/upstream_capture.cpp src/codex/quota.cpp src/storage/kv.cpp src/provider/openrouter_handlers.cpp src/handler/terminal_log.cpp src/deepseek/waterfall_handler.cpp src/codex/manual_reset_state.cpp src/cache/prompt_analytics_queue.cpp
//   src/sentinel/replay_limits.cpp src/openai_schema.cpp src/images.cpp src/provider/openrouter_streams.cpp src/responses_native_headers.cpp src/sentinel/replay_retention_metadata.cpp src/codex/index.cpp src/storage/sqlite_readonly.cpp src/embeddings/jobs.cpp
//   src/codex/responses_state.cpp src/runtime/http_early_sse.cpp src/chat/stream_translation.cpp src/provider/lithos_handlers.cpp src/deepseek/chat_projection.cpp src/paid_fallback/claude_code_route.cpp src/live/sideband.cpp src/catalog/promotion.cpp src/deepseek/context_admission_failure.cpp
//   src/embeddings/ledger.cpp src/systemone/handlers.cpp src/deepseek/forwarded_payload_policy.cpp src/live/upstream.cpp src/handler/admission.cpp src/embeddings/voyage.cpp src/codex/responses_runners.cpp src/provider/lithos_streams.cpp src/provider/openrouter_request_controls.cpp
//   src/provider/openrouter_web_tools.cpp src/chat/envelope.cpp src/runtime/http_tasks.cpp src/deepseek/responses.cpp src/embeddings/handlers.cpp src/live/calls.cpp src/deepseek/handlers.cpp src/lib/jev_compaction/state.cpp src/provider/stream_relay.cpp
//   src/provider/cerebras_responses.cpp src/kernel/quota_reserve.cpp src/lib/jev_compaction/types.cpp src/responses_buffered.cpp src/chat/completions.cpp src/optional_telemetry_queue.cpp src/provider/cerebras_handlers.cpp src/live/routes.cpp src/storage/native_kv.cpp
// src/deepseek/responses_stream.cpp src/deepseek/responses_payload.cpp src/lib/jev_compaction/codex_items.cpp src/inference_admission.cpp src/codex/responses_operations.cpp src/lib/jev_compaction/compact.cpp src/deepseek/stream.cpp src/lib/jev_compaction/request.cpp
//   tests/helpers/loopback_http.cpp src/runtime/http.cpp src/runtime/http_body.cpp src/runtime/http_upgrade.cpp
// Port of the one original registration in tests/codex-serial-routing-http.test.ts.
#include "tests/helpers/responses_conformance_client.hpp"
#include "tests/helpers/loopback_http.hpp"
#include "uos/handler/serve_handler.hpp"
#include "uos/codex/account_routing.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/dispatch.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/runtime_config.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/health.hpp"
#include "uos/models/codex_models_availability.hpp"
#include "uos/storage/native_kv.hpp"
#include "uos/rt/kv_sqlite.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"
#include "uos/utils.hpp"
namespace rt=uos::rt;namespace json=uos::json;namespace wire=uos::runtime::http;namespace routing=uos::codex::account_routing;
namespace {
const std::string MODEL="gpt-5.6-sol",ACCOUNT_A="serial-routing-http-account-a",ACCOUNT_B="serial-routing-http-account-b";
std::string contentText(const json::value* value){if(!value)return "";if(value->is_string())return std::string(value->as_string());std::string out;if(value->is_array())for(const auto& item:value->as_array())if(const auto* content=rt::getArray(item,"content"))for(const auto& part:*content)out+=rt::getString(part,"text").value_or("");return out;}
rt::Task<std::string> routingAccountIdHash(std::string id){std::string input="uos_ai";input.push_back('\0');input+="codex_routing_account";input.push_back('\0');input+=id;const auto hash=co_await uos::utils::sha256Hex(input);co_return hash;}
rt::Task<json::object> routingSlot(uos::types::CodexAuthState auth,double usedPercent){
  const auto hash=co_await routingAccountIdHash(auth.account_id);const auto version=co_await routing::codexCredentialVersion(auth);
  co_return json::object{{"account_id_hash",hash},{"credential_version",version},{"quota_blocked_until_ms",nullptr},{"quota_block_source",nullptr},{"quota_blocked_classes",json::array{}},{"quota_blocks_by_class",json::object{}},{"invalid_credential_version",nullptr},{"primary_used_percent",usedPercent},{"secondary_used_percent",nullptr},{"quota_signal_observed_at_ms",nullptr},{"capacity_observed_at_ms",nullptr},{"upstream_timeout_blocked_until_ms",nullptr},{"observed_reset_at_ms",nullptr},{"observed_reset_at_is_stable",false},{"banked_reset_generation_ambiguous",false},{"banked_reset_recovery_probe_pending",false},{"generation",0},{"probe_lease",nullptr}};
}
struct UpstreamCall{std::optional<std::string> accountId,sessionId,conversationId,promptCacheKey;int status;std::string sentinel;};
struct GatewayCall{int status;bool completed;std::string text;};
struct Fixture{
  uos::tests::helpers::loopback_http::ScopedLoopbackFetch network;
  std::shared_ptr<rt::Kv> kv=uos::storage::native_kv::guardNativeKv(std::make_shared<rt::SqliteKv>(":memory:"));
  bool savedDeploy=uos::config::config.isDeploy;
  std::string savedAuth=uos::config::config.codexAuthJsonB64;
  rt::console::Capture logs{{rt::console::Method::info,rt::console::Method::warn}};
  std::optional<wire::HttpServer> provider,gateway;
  std::unique_ptr<rt::GlobalFetchOverride> fetch;
  std::vector<UpstreamCall> calls;
  std::string token="u_"+std::string(64,'b'),endpoint;
  bool exhaustAccountA=false;int requestIndex=0;
  bool failFirstStream = false;
  double now=rt::dateNow();
  uos::types::CodexAuthState accountA{.access_token=ACCOUNT_A+"-access-token",.refresh_token=ACCOUNT_A+"-refresh-token",.account_id=ACCOUNT_A,.updated_at_ms=now};
  uos::types::CodexAuthState accountB{.access_token=ACCOUNT_B+"-access-token",.refresh_token=ACCOUNT_B+"-refresh-token",.account_id=ACCOUNT_B,.updated_at_ms=now};
  Fixture(){
    uos::tests::helpers::responses_conformance_client::stageGatewayAssets();
    uos::config::config.isDeploy=true;uos::config::config.codexAuthJsonB64.clear();uos::kv::setKvForTest(kv);routing::resetCodexAccountRoutingForTest();uos::codex::auth::resetCodexAuthCacheForTest();uos::api_key_policy::resetApiKeyPolicyCacheForTest();uos::runtime_config::resetRuntimeConfigCacheForTest();uos::provider::selection::resetProviderSelectionCacheForTest();uos::models::codex_models_availability::resetCodexAccountModelsCacheForTest();uos::provider::health::resetProviderHealthThrottleForTest();uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const auto&){});
  }
  ~Fixture(){fetch.reset();uos::kv::setKvForTest(nullptr);uos::config::config.isDeploy=savedDeploy;uos::config::config.codexAuthJsonB64=savedAuth;routing::resetCodexAccountRoutingForTest();uos::codex::auth::resetCodexAuthCacheForTest();uos::api_key_policy::resetApiKeyPolicyCacheForTest();uos::runtime_config::resetRuntimeConfigCacheForTest();uos::provider::selection::resetProviderSelectionCacheForTest();uos::models::codex_models_availability::resetCodexAccountModelsCacheForTest();uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest(nullptr);kv->close();}
  rt::Task<void> set(rt::KvKey key,json::value value){const rt::KvValue stored(value);auto writing=kv->set(key,stored);co_await std::move(writing);}
  rt::Task<void> start(){
    const json::object pool{{"accounts",json::array{json::value_from(accountA),json::value_from(accountB)}},{"updated_at_ms",now}};auto auth=set(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY,pool);co_await std::move(auth);
    const auto slotA=co_await routingSlot(accountA,90);const auto slotB=co_await routingSlot(accountB,10);const json::object state{{"v",2},{"updated_at_ms",now},{"banked_reset_legacy_identity_unresolved",false},{"slots",json::array{slotA,slotB}}};auto route=set(routing::CODEX_ACCOUNT_ROUTING_KV_KEY,state);co_await std::move(route);
    const auto tokenHash=co_await uos::utils::sha256Base64Url(token);json::object common{{"id","serial-routing-http-key"},{"expires_at_ms",-1},{"revoked_at_ms",nullptr},{"usage_limit_requests",-1},{"usage_requests",0},{"usage_reset_at_ms",now+3'600'000},{"window_ms",3'600'000},{"usage_quota_version",3},{"paid_fallback_enabled",false},{"paid_fallback_limit_microcredits",0},{"paid_fallback_spent_microcredits",0},{"paid_fallback_reserved_microcredits",0},{"paid_fallback_reservation_request_id",nullptr}};
    auto key=common;key["name"]="Serial routing HTTP key";key["prefix"]=token.substr(0,10);key["hash"]=tokenHash;key["created_at_ms"]=now;key["paid_fallback_model_ids"]=json::array{};key["paid_fallback_quota_per_credit"]=0;key["paid_fallback_max_exposure_microcredits"]=json::object{};key["paid_fallback_pricing_checked_at_ms"]=now;
    auto keyWriting=set(rt::KvKey{"ubq_ai","api_keys","id","serial-routing-http-key"},key);co_await std::move(keyWriting);auto hashWriting=set(rt::KvKey{"ubq_ai","api_keys","hash",tokenHash},common);co_await std::move(hashWriting);
    const json::object catalog{{"source","codex_cli"},{"client_version","0.145.0"},{"updated_at_ms",now},{"models",json::array{json::object{{"slug",MODEL},{"context_window",272'000},{"max_context_window",1'000'000},{"auto_compact_token_limit",nullptr},{"default_reasoning_level","low"},{"supported_reasoning_levels",json::array{"none","low","medium","high","xhigh","max","ultra"}},{"reasoning_effort_wire_map",json::object{{"ultra","max"}}}}}}};
    auto catalogWriting=set(rt::KvKey{"ubq_ai","codex_models"},catalog);co_await std::move(catalogWriting);const json::object runtime{{"version",2},{"default_model",MODEL},{"default_reasoning_effort","low"},{"codex_models",catalog},{"updated_at_ms",now}};auto runtimeWriting=set(uos::runtime_config::RUNTIME_CONFIG_V2_KEY,runtime);co_await std::move(runtimeWriting);
    wire::ServeHttpOptions options;options.hostname="127.0.0.1";options.port=0;options.onListen=[](const auto&){};
    provider=wire::serveHttp(options,[this](rt::Request request,wire::ServeHandlerInfo)->rt::Task<rt::Response>{
      if(rt::URL(request.url()).pathname()!="/codex/responses")co_return rt::Response("not found",rt::ResponseInit{.status=404});
      auto reading=request.json();const auto body=co_await std::move(reading);const auto id=request.headers().get("chatgpt-account-id");const auto sentinel=contentText(rt::get(body,"input"));const bool exhausted=exhaustAccountA&&id==ACCOUNT_A;
      calls.push_back({id,request.headers().get("session-id"),request.headers().get("conversation_id"),rt::getString(body,"prompt_cache_key")?std::optional<std::string>(std::string(*rt::getString(body,"prompt_cache_key"))):std::nullopt,exhausted?429:200,sentinel});
      if(exhausted)co_return rt::Response::json(json::object{{"error",json::object{{"message","Codex subscription quota exhausted"},{"type","usage_limit_reached"}}}},rt::ResponseInit{.status=429,.headers=rt::Headers{{"Retry-After",rt::Date(rt::dateNow()+90'000).toUTCString()}}});
      if (failFirstStream && calls.size() == 1) {
        const std::string preamble =
          "data: {\"type\":\"response.created\",\"response\":{\"id\":\"resp_abandoned_loopback\"}}\n\n"
          "data: {\"type\":\"response.in_progress\",\"response\":{\"id\":\"resp_abandoned_loopback\"}}\n\n";
        rt::UnderlyingSource<rt::ByteChunk> source;
        source.start = [preamble](auto controller) {
          controller.enqueue(rt::encodeUtf8(preamble));
          rt::setTimeout([controller] {
            controller.enqueue(rt::encodeUtf8("data: {\"type\":\"error\",\"code\":\"server_error\",\"message\":\"loopback failure\",\"param\":null}\n\n"));
            controller.close();
          }, 25);
        };
        co_return rt::Response(rt::ByteStream(source), {.headers = rt::Headers{{"Content-Type", "text/event-stream"}}});
      }
      const auto responseId="resp_serial_routing_http_"+std::to_string(calls.size());const json::object completed{{"id",responseId},{"object","response"},{"created_at",rt::Math::trunc(rt::dateNow()/1000)},{"status","completed"},{"model",MODEL},{"output",json::array{json::object{{"type","message"},{"id","msg_"+responseId},{"status","completed"},{"role","assistant"},{"content",json::array{json::object{{"type","output_text"},{"text",sentinel},{"annotations",json::array{}}}}}}}},{"usage",json::object{{"input_tokens",2},{"output_tokens",1},{"total_tokens",3}}}};
      const auto sse="data: "+rt::jsonStringify(json::object{{"type","response.created"},{"response",json::object{{"id",responseId}}}})+"\n\ndata: "+rt::jsonStringify(json::object{{"type","response.output_text.delta"},{"delta",sentinel}})+"\n\ndata: "+rt::jsonStringify(json::object{{"type","response.completed"},{"response",completed}})+"\n\n";
      co_return rt::Response(sse,rt::ResponseInit{.headers=rt::Headers{{"Content-Type","text/event-stream"}}});
    });
    const auto providerAddress=co_await provider->ready.wait();const auto providerOrigin="http://127.0.0.1:"+rt::numberToString(providerAddress.port);
    fetch=std::make_unique<rt::GlobalFetchOverride>(rt::Fetch([providerOrigin](rt::RequestInfo input,rt::RequestInit init)->rt::Task<rt::Response>{std::string url;if(const auto* text=std::get_if<std::string>(&input))url=*text;else if(const auto* value=std::get_if<rt::URL>(&input))url=value->href();else url=std::get<rt::Request>(input).url();if(url==uos::config::config.codexBaseUrl+"/responses")url=providerOrigin+"/codex/responses";if(rt::startsWith(url,"https://auth.openai.com/"))co_return rt::Response::json(json::object{{"error","unexpected_token_refresh"}},rt::ResponseInit{.status=500});if(rt::URL(url).hostname()!="127.0.0.1")throw rt::JsError("Unmapped external URL refused by loopback HTTP fixture");auto responseTask=rt::defaultFetch()(url,init);auto response=co_await std::move(responseTask);co_return response;}));
    gateway=wire::serveHttp(options,uos::handler::serve_handler::createServeHandler());const auto address=co_await gateway->ready.wait();endpoint="http://127.0.0.1:"+rt::numberToString(address.port);
  }
  rt::Task<GatewayCall> callGateway(std::optional<std::string> cache){const auto sentinel="serial-routing-http-"+std::to_string(requestIndex++);json::object body{{"model",MODEL},{"input",sentinel},{"stream",false},{"reasoning",json::object{{"effort","low"}}}};if(cache)body["prompt_cache_key"]=*cache;rt::RequestInit init;init.method="POST";init.headers=rt::Headers{{"authorization","Bearer "+token},{"content-type","application/json"}};init.body=rt::jsonStringify(body);auto fetching=rt::fetch(endpoint+"/v1/responses",init);auto response=co_await std::move(fetching);auto reading=response.json();const auto payload=co_await std::move(reading);co_return GatewayCall{response.status(),rt::getString(payload,"status")=="completed",contentText(rt::get(payload,"output"))};}
  rt::Task<int> callChatGateway(){const json::object body{{"model",MODEL},{"messages",json::array{json::object{{"role","user"},{"content","serial-routing-http-chat-"+std::to_string(requestIndex++)}}}},{"stream",false}};rt::RequestInit init;init.method="POST";init.headers=rt::Headers{{"authorization","Bearer "+token},{"content-type","application/json"}};init.body=rt::jsonStringify(body);auto fetching=rt::fetch(endpoint+"/v1/chat/completions",init);auto response=co_await std::move(fetching);auto reading=response.text();co_await std::move(reading);co_return response.status();}
  void servedBy(const std::string& account){ASSERT(!calls.empty());EXPECT_EQ(calls.back().accountId,std::optional<std::string>(account));EXPECT_EQ(calls.back().status,200);}
  rt::Task<void> stop(){if(gateway){auto stopping=gateway->shutdown();co_await stopping.wait();}if(provider){auto stopping=provider->shutdown();co_await stopping.wait();}auto draining=uos::codex::dispatch::awaitPendingCodexProbeTransitions();co_await std::move(draining);}
};
}
ASYNC_TEST("serial Codex routing keeps one active account across cache keys, idle time, exhaustion, and recovery over real loopback HTTP"){
  Fixture fixture;rt::Reason failure;try{auto starting=fixture.start();co_await std::move(starting);const std::string keyOne="serial-routing-http-key-one",keyTwo="serial-routing-http-key-two";
  auto first=co_await fixture.callGateway(keyOne);EXPECT_EQ(first.status,200);EXPECT(first.completed);EXPECT_EQ(first.text,"serial-routing-http-0");fixture.servedBy(ACCOUNT_A);EXPECT_EQ(fixture.calls.back().sentinel,"serial-routing-http-0");const auto firstSessionId=fixture.calls.back().sessionId;ASSERT(firstSessionId&&!firstSessionId->empty());
  auto repeated=co_await fixture.callGateway(keyOne);EXPECT_EQ(repeated.status,200);EXPECT_EQ(repeated.text,"serial-routing-http-1");fixture.servedBy(ACCOUNT_A);EXPECT_EQ(fixture.calls.back().sessionId,firstSessionId);EXPECT_EQ(fixture.calls.back().conversationId,fixture.calls.back().sessionId);EXPECT_EQ(fixture.calls.back().promptCacheKey,std::optional<std::string>(keyOne));
  auto different=co_await fixture.callGateway(keyTwo);EXPECT_EQ(different.status,200);fixture.servedBy(ACCOUNT_A);EXPECT(fixture.calls.back().sessionId!=firstSessionId);
  auto keyless=co_await fixture.callGateway(std::nullopt);EXPECT_EQ(keyless.status,200);fixture.servedBy(ACCOUNT_A);EXPECT(!fixture.calls.back().sessionId);
  const auto chatStatus=co_await fixture.callChatGateway();EXPECT_EQ(chatStatus,200);fixture.servedBy(ACCOUNT_A);
  const auto realNow=rt::globalDateNow();rt::globalDateNow()=[now=fixture.now]{return now+360'000;};uos::codex::auth::resetCodexAuthCacheForTest();routing::resetCodexAccountRoutingForTest();auto idle=co_await fixture.callGateway(keyOne);rt::globalDateNow()=realNow;EXPECT_EQ(idle.status,200);fixture.servedBy(ACCOUNT_A);EXPECT_EQ(fixture.calls.back().sessionId,firstSessionId);
  fixture.exhaustAccountA=true;auto exhausted=co_await fixture.callGateway(keyTwo);EXPECT_EQ(exhausted.status,200);EXPECT(exhausted.completed);EXPECT_EQ(exhausted.text,"serial-routing-http-"+std::to_string(fixture.requestIndex-1));fixture.servedBy(ACCOUNT_B);EXPECT(std::any_of(fixture.calls.begin(),fixture.calls.end(),[](const auto& call){return call.accountId==ACCOUNT_A&&call.status==429;}));
  fixture.exhaustAccountA=false;rt::globalDateNow()=[now=fixture.now]{return now+120'000;};auto recovered=co_await fixture.callGateway(keyOne);rt::globalDateNow()=realNow;EXPECT_EQ(recovered.status,200);fixture.servedBy(ACCOUNT_B);EXPECT(std::none_of(fixture.logs.calls().begin(),fixture.logs.calls().end(),[](const auto& call){return rt::includes(call.text,"quota_accounting_failed");}));
  auto readingPool=fixture.kv->get(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY);const auto pool=co_await std::move(readingPool);const auto hashA=co_await routingAccountIdHash(ACCOUNT_A);const auto credential=co_await routing::codexCredentialVersion(fixture.accountA);
  const json::object active{{"v",1},{"account_id_hash",hashA},{"credential_version",credential},{"pool_versionstamp",pool.versionstamp.value()},{"slot",0},{"routing_generation",0},{"generation",1},{"transition_reason",nullptr},{"updated_at_ms",rt::dateNow()}};auto writingActive=fixture.set(routing::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY,active);co_await std::move(writingActive);
  const auto subHash=co_await uos::provider::selection::codexSubscriptionHash(ACCOUNT_B);const json::object selected{{"provider_ids",json::array{uos::provider::selection::codexSubscriptionSelectionId(subHash)}},{"updated_at_ms",rt::dateNow()}};auto writingSelection=fixture.set(uos::provider::selection::PROVIDER_SELECTION_KV_KEY,selected);co_await std::move(writingSelection);uos::provider::selection::resetProviderSelectionCacheForTest();
  auto narrowed=co_await fixture.callGateway(keyTwo);EXPECT_EQ(narrowed.status,200);EXPECT(narrowed.completed);EXPECT_EQ(narrowed.text,"serial-routing-http-"+std::to_string(fixture.requestIndex-1));fixture.servedBy(ACCOUNT_B);
  auto readingActive=fixture.kv->get(routing::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY);const auto activeRow=co_await std::move(readingActive);const auto narrowedActive=routing::parseCodexActiveAccountSelection(activeRow.value.toJson());ASSERT(narrowedActive);EXPECT_EQ(narrowedActive->slot,1);const auto hashB=co_await routingAccountIdHash(ACCOUNT_B);EXPECT_EQ(narrowedActive->account_id_hash,hashB);
  auto narrowedAgain=co_await fixture.callGateway(keyTwo);EXPECT_EQ(narrowedAgain.status,200);fixture.servedBy(ACCOUNT_B);

  }catch(...){failure=std::current_exception();}
  auto cleanupTask=fixture.stop();co_await std::move(cleanupTask);if(failure)std::rethrow_exception(failure);
}

ASYNC_TEST("Native Codex SSE headers stay open across a pre-output retry over loopback HTTP") {
  Fixture fixture;
  fixture.failFirstStream = true;
  rt::Reason failure;
  try {
    auto starting = fixture.start();
    co_await std::move(starting);
    const json::object payload{{"model", MODEL}, {"input", "loopback retry"}, {"stream", true}};
    const rt::RequestInit init{
      .method = "POST",
      .headers = rt::Headers{{"authorization", "Bearer " + fixture.token}, {"content-type", "application/json"},
                             {"x-codex-turn-metadata", R"({"session_id":"retry","thread_id":"retry","turn_id":"retry"})"}},
      .body = rt::jsonStringify(payload),
    };
    const auto url = fixture.endpoint + "/v1/responses";
    auto fetching = rt::fetch(url, init);
    const auto response = co_await std::move(fetching);
    EXPECT_EQ(response.status(), 200);
    EXPECT_EQ(response.headers().get("content-type"), "text/event-stream");
    EXPECT(!response.headers().has("x-uos-upstream"));

    auto reading = response.text();
    const auto body = co_await std::move(reading);
    EXPECT(!rt::includes(body, "resp_abandoned_loopback"));
    std::vector<json::value> events;
    for (const auto& match : rt::matchAll(body, rt::Regex("^data: (.+)$", "gm"))) events.push_back(rt::jsonParse(*match[1]));
    ASSERT_EQ(events.size(), 3U);
    EXPECT_EQ(rt::getString(events.front(), "type"), "response.created");
    EXPECT_EQ(rt::getString(events[1], "type"), "response.output_text.delta");
    EXPECT_EQ(rt::getString(events.back(), "type"), "response.completed");
    EXPECT_EQ(rt::getString(*rt::get(events.front(), "response"), "id"), "resp_serial_routing_http_2");
    EXPECT_EQ(rt::getString(*rt::get(events.back(), "response"), "id"), "resp_serial_routing_http_2");
    ASSERT_EQ(fixture.calls.size(), 2U);
    EXPECT_EQ(fixture.calls[0].conversationId, fixture.calls[1].conversationId);
    EXPECT_EQ(fixture.calls[0].accountId, fixture.calls[1].accountId);
  } catch (...) {
    failure = std::current_exception();
  }
  auto stopping = fixture.stop();
  co_await std::move(stopping);
  if (failure) std::rethrow_exception(failure);
}
