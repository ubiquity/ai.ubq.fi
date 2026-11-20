// Links: tests/kv_budget_part1_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
// src/utils.cpp tests/helpers/kv_budget_harness.cpp src/auth/index.cpp src/authority/native_kernel.cpp src/auth/passport.cpp src/auth/passkeys.cpp src/kernel/attestation.cpp src/api_key_policy.cpp
// src/models/catalog.cpp src/provider/metered.cpp src/authority/native_api_key.cpp src/provider/claude_code_models.cpp src/auth/passport_sessions.cpp src/input_normalization.cpp src/kv.cpp src/models/codex_models_fetch.cpp
// src/provider/claude_code_accounts.cpp src/models/metadata.cpp src/provider/selection.cpp src/handler/index.cpp src/request_policy.cpp src/admin/defaults.cpp src/auth/relay.cpp src/provider/claude_code_credentials.cpp
// src/codex/auth_refresh.cpp src/types.cpp src/admin/kernel.cpp src/sentinel/replay_admin.cpp src/codex/auth.cpp src/cache/prompt_analytics_queue.cpp src/auth/local_admin.cpp src/agent_messages.cpp
// src/admin/codex.cpp src/admin/api_keys.cpp src/quota_projection.cpp src/deepseek/waterfall.cpp src/models/codex_models.cpp src/paid_fallback/ledger_stats.cpp src/codex/dispatch.cpp src/cache/prompt_analytics_core.cpp
// src/bounded_response_body.cpp src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp src/runtime_config.cpp src/catalog/index.cpp src/authority/native_paid_contracts.cpp src/kernel/quota_reserve.cpp src/codex/native_auth.cpp
// src/models/openrouter_models.cpp src/kernel/quota_v2.cpp src/paid_fallback/ledger_state.cpp src/authority/native_paid.cpp src/models/codex_models_whitelist.cpp src/metered_quota.cpp src/provider/capacity_events.cpp src/provider/anthropic_responses.cpp
// src/catalog/store.cpp src/codex/reset_settings.cpp src/admin/claude_code.cpp src/kernel/usage.cpp src/catalog/models.cpp src/admin/sentinel_incident.cpp src/provider/capacity.cpp src/provider/health.cpp
// src/config.cpp src/provider/anthropic_response_wire.cpp src/admin/api_key_mutations.cpp src/authority/native_api_key_policy.cpp src/api_keys.cpp src/codex/banked_reset_provider.cpp src/cache/prompt_analytics_read.cpp src/health.cpp
// src/provider/cerebras.cpp src/provider/surplus.cpp src/cache/kv_migration_validation.cpp src/codex/banked_reset.cpp src/provider/openrouter.cpp src/cache/kv_migration_base.cpp src/provider/capacity_history.cpp src/openai_telemetry.cpp
// src/analytics.cpp src/provider/removed_provider_telemetry.cpp src/optional_telemetry_queue.cpp src/sentinel/replay_observation.cpp src/kernel/quota_reservations.cpp src/provider/capacity_contract.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
// src/models/codex_models_availability.cpp src/live/handler.cpp src/storage/kv.cpp src/cache/kv_migration_usage.cpp src/provider/anthropic_chat_responses.cpp src/cache/kv_migration_paid_fallback.cpp src/debug_routing.cpp src/paid_fallback/ledger_admission.cpp
// src/sentinel/replay_read.cpp src/paid_fallback/index.cpp src/codex/supervisor.cpp src/admin/api_key_audit.cpp src/admin/chart_source_proxy.cpp src/live/sideband.cpp src/paid_fallback/rollups.cpp src/admin/codex_reset.cpp
// src/cache/scope_experiment_targets.cpp src/kernel/quota_write.cpp src/codex/supervisor_follow.cpp src/handler/http.cpp src/authority/native_paid_policy.cpp src/sentinel/incident_outbox.cpp src/cache/scope_experiment_model.cpp src/provider/surplus_claude_request.cpp
// src/codex/supervisor_config.cpp src/provider/lithos_models.cpp src/recent_model_context.cpp src/codex/supervisor_transport.cpp src/provider/capacity_analytics.cpp src/http.cpp src/handler/terminal_route.cpp src/provider/claude_code_quota.cpp
// src/defaults.cpp src/sentinel/replay_store.cpp src/sentinel/replay_envelope.cpp src/release.cpp src/provider/presentation.cpp src/auth/local_development_key.cpp src/embeddings/jobs.cpp src/responses_stream.cpp
// src/sentinel/replay_model.cpp src/embeddings/handlers.cpp src/codex/supervisor_inventory.cpp src/codex/manual_reset.cpp src/authority/native_codex.cpp src/storage/sqlite_readonly.cpp src/provider/anthropic_replay.cpp src/codex/supervisor_brief.cpp
// src/provider/surplus_claude_chat_request.cpp src/sentinel/replay_retention_schema.cpp src/provider/claude_code.cpp src/codex/experiment.cpp src/codex/capacity_routing.cpp src/sentinel/replay_retention.cpp src/codex/routing_mutations.cpp src/provider/claude_capacity_history.cpp
// src/admin/capacity_observation_sync.cpp src/authority/native_codex_capacity.cpp src/embeddings/ledger.cpp src/responses_handler.cpp src/static.cpp src/paid_fallback/health.cpp src/codex/account_routing.cpp src/provider/removed_provider_circuit.cpp
// src/handler/terminal_log.cpp src/provider/claude_capacity_rollups.cpp src/sentinel/replay_limits.cpp src/codex/external_auth.cpp src/authority/native_api_key_audit.cpp src/provider/lithos.cpp src/provider/openrouter_handlers.cpp src/codex/rate_limit_429.cpp
// src/codex/quota.cpp src/authority/native_codex_mutations.cpp src/deepseek/handlers.cpp src/embeddings/voyage.cpp src/sentinel/replay_retention_bootstrap.cpp src/provider/claude_code_access.cpp src/responses_attempts.cpp src/catalog/types.cpp
// src/authority/native_codex_contracts.cpp src/sentinel/upstream_capture.cpp src/upstream_wire.cpp src/provider/cerebras_rate_limits.cpp src/openai.cpp src/deepseek/index.cpp src/codex/routing_serial.cpp src/authority/native_kernel_contracts.cpp
// src/deepseek/chat_projection.cpp src/authority/native_codex_evaluation.cpp src/provider/lithos_rate_limits.cpp src/deepseek/responses_payload.cpp src/cache/telemetry_gate.cpp src/cache/scope_experiment.cpp src/admin/error_log.cpp src/deepseek/context_admission_failure.cpp
// src/responses_native_headers.cpp src/inference_deadline.cpp src/kernel/policy_queue.cpp src/provider/anthropic_chat_stream.cpp src/authority/native_codex_429.cpp src/codex/manual_reset_state.cpp src/codex/supervisor_log.cpp src/codex/routing_state.cpp
// src/responses_request.cpp src/codex/routing_evaluation.cpp src/provider/stream_relay.cpp src/codex/overage_settings.cpp src/provider/claude_code_usage_schema.cpp src/responses_failover_stream.cpp src/paid_fallback/removed_provider.cpp src/live/upstream.cpp
// src/chat/envelope.cpp src/codex/native_auth_transport.cpp src/deepseek/responses_stream.cpp src/deepseek/stream.cpp src/chat/stream_translation.cpp src/live/routes.cpp src/request.cpp src/provider/openrouter_web_tools.cpp
// src/provider/openrouter_streams.cpp src/codex/banked_reset_pool.cpp src/paid_fallback/routing.cpp src/sentinel/replay_retention_metadata.cpp src/deepseek/forwarded_payload_policy.cpp src/cache/scope_targets.cpp src/runtime/websocket.cpp src/codex/banked_reset_submission.cpp
// src/codex/banked_reset_claim.cpp src/kv_conversion/profile_json.cpp src/codex/supervisor_secret.cpp src/handler/admission.cpp src/responses_buffered.cpp src/deepseek/responses.cpp src/provider/cerebras_handlers.cpp src/openai_schema.cpp
// src/runtime/http_early_sse.cpp src/provider/cerebras_responses.cpp src/inference_admission.cpp src/deepseek/waterfall_handler.cpp src/images.cpp src/live/calls.cpp src/storage/native_kv.cpp
// src/runtime/http_tasks.cpp src/lib/jev_compaction/codex_items.cpp src/provider/openrouter_request_controls.cpp src/chat/completions.cpp src/catalog/promotion.cpp src/systemone/handlers.cpp src/decisions/handlers.cpp src/deepseek/flash_aliases.cpp
// src/lib/jev_compaction/state.cpp src/lib/jev_compaction/types.cpp src/provider/lithos_handlers.cpp src/provider/lithos_streams.cpp src/codex/responses_error.cpp src/codex/index.cpp src/codex/responses_operations.cpp src/paid_fallback/claude_code_route.cpp
// src/lib/jev_compaction/compact.cpp src/codex/responses_state.cpp src/codex/responses_runners.cpp src/lib/jev_compaction/request.cpp
// Port of tests/kv-budget-part1.test.ts; original cases are appended in source order.
#include "tests/helpers/kv_budget_harness.hpp"
#include "tests/helpers/responses_conformance_client.hpp"
#include "uos/config.hpp"
#include "uos/handler/admission.hpp"
#include "uos/kernel/attestation.hpp"
#include "uos/kernel/quota_v2.hpp"
#include "uos/cache/prompt_analytics_queue.hpp"
#include "uos/paid_fallback/ledger_backfill.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"
using namespace uos;
namespace h=tests::helpers::kv_budget_harness;
#define AWAIT(name,expression) auto name##Task=(expression);auto name=co_await std::move(name##Task)
namespace {
  struct Scope {
    std::string tmp=rt::env::get("TMPDIR").value_or("/tmp");
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string,std::string>>{{"TMPDIR",tmp},{"HOME",tmp+"/h"}}};
    config::Config saved=config::config;
    rt::console::Capture logs;
    Scope(){tests::helpers::responses_conformance_client::stageGatewayAssets();config::config.codexAuthJsonB64.clear();config::config.isDeploy=true;h::resetHarness();h::kv->resetCounts();h::resetApiKeyPolicyCacheForTest();h::resetRuntimeConfigCacheForTest();h::resetCodexAuthCacheForTest();h::resetMeteredModelsCacheForTest();h::resetSurplusModelsCacheForTest();h::resetProviderHealthThrottleForTest();}
    ~Scope(){h::kv->failApiKeyV3Reads=false;h::kv->failKernelQuotaReads=false;h::kv->kernelQuotaReadGate.reset();h::kv->apiKeyV3DispatchCommitGate.reset();h::kv->onApiKeyV3DispatchCommit={};h::kv->onKernelReservationCommit={};h::cleanupHarness();h::resetApiKeyPolicyCacheForTest();h::resetRuntimeConfigCacheForTest();h::resetCodexAuthCacheForTest();cache::prompt_analytics_queue::resetOptionalPromptCacheAnalyticsForTest();config::config=saved;}
  };
  json::value value(const rt::KvKey& key){const auto* row=h::kv->values.get(h::encodeKey(key));return row?rt::storedJsonView(*row):json::value(nullptr);}
  void put(const rt::KvKey& key,const json::value& record){h::kv->values.set(h::encodeKey(key),rt::KvValue(record));}
  std::vector<json::value> terminalPayloads(const rt::console::Capture& logs){std::vector<json::value> rows;for(const auto& call:logs.calls())if(call.args.size()>1&&call.args[0]=="[ai.ubq.fi] request_terminal")rows.push_back(rt::jsonParse(call.args[1].string()));return rows;}
  void expectWindow(const h::PreparedApiKeyInference& prepared,double committed,double reserved){const json::object expected{{"committed_requests",committed},{"reserved_requests",reserved},{"window_reset_at_ms",prepared.policy.usage_reset_at_ms}};EXPECT(rt::deepEqual(h::usageWindow(prepared.policy),expected));}
  void seedPasskey(const std::string& token,const std::string& id){const auto now=rt::dateNow();put(h::passkeyUserKey(id),json::object{{"id",id},{"handle",id},{"is_admin",true},{"credential_ids",json::array{"quota-failure-credential"}},{"created_at_ms",now},{"updated_at_ms",now}});put(h::passkeySessionKey(token),json::object{{"token",token},{"user_id",id},{"created_at_ms",now},{"expires_at_ms",now+60'000}});}
  rt::Task<rt::Request> githubRequest(std::string token,std::string cookie,std::string owner,std::string repo){const rt::Request request("https://ai.ubq.fi/uos/auth",rt::RequestInit{.headers=rt::Headers{{"Authorization","Bearer "+token},{"Cookie",std::string(h::PASSKEY_RELAY_COOKIE_NAME)+"="+cookie},{"X-GitHub-Owner",owner},{"X-GitHub-Repo",repo}}});auto signing=h::withKernelTestToken(request,token,owner,repo);auto result=co_await std::move(signing);co_return result;}
}
ASYNC_TEST("GitHub quota lookup failures do not fall back to relay passkey cookies") {
  Scope scope;auto seeding=h::seedKernelTestPublicKey();co_await std::move(seeding);const std::string owner="relay-fallback-org",repo="relay-fallback-repo",token="ghp_relay_fallback_quota_1234567890abcdefghijklmnopqrstuvwxyz",cookie="uos_ai_session_quota_failure_fallback";seedPasskey(cookie,"quota-failure-passkey-user");rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo,rt::RequestInit){return rt::Response(nullptr,rt::ResponseInit{.status=200});}));AWAIT(initialRequest,githubRequest(token,cookie,owner,repo));AWAIT(initial,h::authenticateClient(initialRequest));ASSERT(std::holds_alternative<auth::index::AuthenticateClientResultOk>(initial));EXPECT(std::holds_alternative<auth::index::ClientAuthMethodGithubToken>(std::get<auth::index::AuthenticateClientResultOk>(initial).method));h::kv->failKernelQuotaReads=true;AWAIT(unavailableRequest,githubRequest(token,cookie,owner,repo));AWAIT(unavailable,h::authenticateClient(unavailableRequest));ASSERT(std::holds_alternative<auth::index::AuthenticateClientResultNotOk>(unavailable));EXPECT_EQ(std::get<auth::index::AuthenticateClientResultNotOk>(unavailable).response.status(),503);h::kv->failKernelQuotaReads=false;
}
ASYNC_TEST("GitHub HTTP verification failures do not fall back to relay passkey cookies") {
  Scope scope;auto seeding=h::seedKernelTestPublicKey();co_await std::move(seeding);const std::string owner="relay-http-fallback-org",repo="relay-http-fallback-repo",cookie="uos_ai_session_github_http_failure_fallback";seedPasskey(cookie,"github-http-failure-passkey-user");auto response=rt::Response(nullptr,rt::ResponseInit{.status=401});rt::GlobalFetchOverride fetch(rt::Fetch([&response](rt::RequestInfo,rt::RequestInit){return response;}));auto request=[&](std::string suffix){return githubRequest("ghp_relay_http_"+suffix+"_1234567890abcdefghijklmnopqrstuvwxyz",cookie,owner,repo);};
  AWAIT(rejectedRequest,request("rejected"));AWAIT(rejected,h::authenticateClient(rejectedRequest));ASSERT(std::holds_alternative<auth::index::AuthenticateClientResultOk>(rejected));EXPECT(std::holds_alternative<auth::index::ClientAuthMethodPasskeySession>(std::get<auth::index::AuthenticateClientResultOk>(rejected).method));AWAIT(adminRequest,request("admin_rejected"));AWAIT(admin,h::authenticateAdmin(adminRequest));ASSERT(std::holds_alternative<auth::index::AdminAuthResultOk>(admin));EXPECT(std::holds_alternative<auth::index::ClientAuthMethodPasskeySession>(std::get<auth::index::AdminAuthResultOk>(admin).method));
  response=rt::Response(nullptr,rt::ResponseInit{.status=200});h::kv->resetCounts();AWAIT(validRequest,request("admin_valid"));AWAIT(valid,h::authenticateAdmin(validRequest));ASSERT(std::holds_alternative<auth::index::AdminAuthResultNotOk>(valid));EXPECT_EQ(std::get<auth::index::AdminAuthResultNotOk>(valid).response.status(),401);EXPECT(!std::any_of(h::kv->readKeys.begin(),h::kv->readKeys.end(),[](const auto& key){return key.size()>2&&key[0].toString()=="uos_ai"&&key[1].toString()=="kernel_quota"&&key[2].toString()=="v2";}));EXPECT(!std::any_of(h::kv->writeKeys.begin(),h::kv->writeKeys.end(),[](const auto& key){return h::encodeKey(key)==h::encodeKey(rt::KvKey{"uos_ai","kernel_policy_queue"});}));
  response=rt::Response(nullptr,rt::ResponseInit{.status=503});AWAIT(uncachedRequest,request("admin_valid"));AWAIT(uncached,h::authenticateClient(uncachedRequest));ASSERT(std::holds_alternative<auth::index::AuthenticateClientResultNotOk>(uncached));EXPECT_EQ(std::get<auth::index::AuthenticateClientResultNotOk>(uncached).response.status(),502);
  const std::vector<double> statuses{408,429,403,500,502,503,403};for(std::size_t index=0;index<statuses.size();++index){rt::Headers headers;if(index==6)headers.set("x-ratelimit-remaining","0");response=rt::Response(nullptr,rt::ResponseInit{.status=statuses[index],.headers=headers});AWAIT(clientRequest,request("unavailable_"+rt::numberToString(static_cast<double>(index))));AWAIT(client,h::authenticateClient(clientRequest));ASSERT(std::holds_alternative<auth::index::AuthenticateClientResultNotOk>(client));EXPECT_EQ(std::get<auth::index::AuthenticateClientResultNotOk>(client).response.status(),502);AWAIT(adminUnavailableRequest,request("admin_unavailable_"+rt::numberToString(static_cast<double>(index))));AWAIT(adminUnavailable,h::authenticateAdmin(adminUnavailableRequest));ASSERT(std::holds_alternative<auth::index::AdminAuthResultNotOk>(adminUnavailable));EXPECT_EQ(std::get<auth::index::AdminAuthResultNotOk>(adminUnavailable).response.status(),502);}
}
ASYNC_TEST("V3 dispatch ledger commits unlimited API-key requests exactly once") {
  Scope scope;AWAIT(prepared,h::prepareApiKeyInference("1","unlimited",-1));rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo,rt::RequestInit){return h::sse();}));AWAIT(first,h::handler(h::request(prepared.token)));EXPECT_EQ(first.status(),200);AWAIT(second,h::handler(h::request(prepared.token)));EXPECT_EQ(second.status(),200);expectWindow(prepared,2,0);bool v2=false;for(const auto& key:h::kv->values.keys())if(rt::includes(key,"\"api_key_usage\",\"v2\""))v2=true;EXPECT(!v2);
}
ASYNC_TEST("V3 unlimited concurrent reservations avoid local CAS exhaustion and lease scans") {
  Scope scope;AWAIT(prepared,h::prepareApiKeyInference("9","unlimited-concurrent",-1));const int concurrency=100;h::kv->resetCounts();std::vector<rt::Promise<api_key_policy::ApiKeyUsageReservationDecision>> admissions;api_key_policy::ReserveApiKeyUsageV3Options options;options.kv=h::kv;for(int index=0;index<concurrency;++index){auto admitting=h::reserveApiKeyUsageV3(prepared.policy,"unlimited-concurrent-"+rt::numberToString(static_cast<double>(index)),"responses",options);admissions.emplace_back(std::move(admitting));}auto all=rt::all(admissions);const auto results=co_await all.wait();std::vector<api_key_policy::ApiKeyUsageReservation> reservations;for(const auto& result:results){ASSERT(std::holds_alternative<api_key_policy::ApiKeyUsageReservationDecisionOk>(result));reservations.push_back(std::get<api_key_policy::ApiKeyUsageReservationDecisionOk>(result).reservation);}EXPECT_EQ(reservations.size(),static_cast<std::size_t>(concurrency));std::vector<rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>>> dispatches;const std::string provider="metered";for(const auto& reservation:reservations)dispatches.emplace_back(reservation.beforeProviderDispatch(provider));auto allDispatches=rt::all(dispatches);co_await allDispatches.wait();expectWindow(prepared,concurrency,0);EXPECT_EQ(h::kv->listCalls,0);
}
ASYNC_TEST("V3 reservations release validation failures before any provider dispatch") {
  Scope scope;AWAIT(prepared,h::prepareApiKeyInference("2","release-before-dispatch",1));int fetchCalls=0;rt::GlobalFetchOverride fetch(rt::Fetch([&fetchCalls](rt::RequestInfo,rt::RequestInit){++fetchCalls;return h::sse();}));const rt::Request invalid("https://ai.ubq.fi/v1/responses",rt::RequestInit{.method="POST",.headers=rt::Headers{{"Authorization","Bearer "+prepared.token},{"Content-Type","application/json"}},.body=R"({"input":42})"});AWAIT(refused,h::handler(invalid));EXPECT_EQ(refused.status(),400);EXPECT_EQ(fetchCalls,0);expectWindow(prepared,0,0);json::value released;const auto prefix=h::encodeKey(h::API_KEY_USAGE_V3_REQUEST_PREFIX);for(const auto& [key,row]:h::kv->values)if(rt::includes(key,prefix.substr(1,prefix.size()-2))){released=rt::storedJsonView(row);break;}EXPECT_EQ(rt::getString(released,"state"),"released");EXPECT_EQ(rt::getString(released,"release_reason"),"route_completed_without_provider_dispatch");AWAIT(accepted,h::handler(h::request(prepared.token)));EXPECT_EQ(accepted.status(),200);EXPECT_EQ(fetchCalls,1);EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"committed_requests"),1);EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"reserved_requests"),0);
}
ASYNC_TEST("V3 dispatch is idempotent across retries and remains consumed after provider failure") {
  Scope scope;AWAIT(prepared,h::prepareApiKeyInference("a","dispatch-once",2));api_key_policy::ReserveApiKeyUsageV3Options options;options.kv=h::kv;AWAIT(admission,h::reserveApiKeyUsageV3(prepared.policy,"request-dispatch-once","responses",options));ASSERT(std::holds_alternative<api_key_policy::ApiKeyUsageReservationDecisionOk>(admission));const auto reservation=std::get<api_key_policy::ApiKeyUsageReservationDecisionOk>(admission).reservation;const std::string codex="chatgpt_codex",metered="metered";auto first=reservation.beforeProviderDispatch(codex);co_await std::move(first);auto second=reservation.beforeProviderDispatch(metered);co_await std::move(second);const std::optional<std::string> reason="provider_http_failure";auto releasing=reservation.release(reason);co_await std::move(releasing);expectWindow(prepared,1,0);const auto record=value(h::apiKeyUsageV3RequestKey(prepared.policy,"request-dispatch-once"));EXPECT_EQ(rt::getString(record,"state"),"dispatched");EXPECT_EQ(rt::getString(record,"provider"),"chatgpt_codex");EXPECT(rt::getNumber(record,"dispatched_at_ms"));
}
ASYNC_TEST("V3 cancellation during the Codex dispatch commit releases quota before fetch") {
  Scope scope;AWAIT(prepared,h::prepareApiKeyInference("e","dispatch-cancelled",1));int fetchCalls=0;const auto gate=h::deferred(),started=h::deferred();h::kv->apiKeyV3DispatchCommitGate=gate.promise;h::kv->onApiKeyV3DispatchCommit=started.resolve;const rt::AbortController controller;rt::GlobalFetchOverride fetch(rt::Fetch([&fetchCalls](rt::RequestInfo,rt::RequestInit){++fetchCalls;return h::sse();}));const rt::Request req("https://ai.ubq.fi/v1/responses",rt::RequestInit{.method="POST",.headers=rt::Headers{{"Authorization","Bearer "+prepared.token},{"Content-Type","application/json"}},.body=R"({"input":"cancel before transport"})",.signal=controller.signal()});rt::Promise<rt::Response> pending(h::handler(req));co_await started.promise.wait();controller.abort(rt::makeReason(rt::DOMException("cancelled","AbortError")));gate.resolve();auto response=co_await pending.wait();EXPECT_EQ(response.status(),499);EXPECT_EQ(fetchCalls,0);const auto logs=terminalPayloads(scope.logs);ASSERT_EQ(logs.size(),1U);EXPECT_EQ(rt::getNumber(logs[0],"status"),499);EXPECT_EQ(rt::getString(logs[0],"stream_terminal_type"),"cancelled");EXPECT_EQ(rt::getString(logs[0],"delivery_outcome"),"unobserved");expectWindow(prepared,0,0);gate.resolve();h::kv->apiKeyV3DispatchCommitGate.reset();h::kv->onApiKeyV3DispatchCommit={};
}
ASYNC_TEST("V3 admission reclaims expired reservations and preserves dispatch identity") {
  Scope scope;AWAIT(prepared,h::prepareApiKeyInference("c","expired-lease",1));const std::string expiredId="expired-request";const double now=rt::dateNow();auto window=json::value_from(h::makeApiKeyUsageWindowV3(prepared.policy,now)).as_object();window["reserved_requests"]=1;put(h::apiKeyUsageV3WindowKey(prepared.policy),window);put(h::apiKeyUsageV3RequestKey(prepared.policy,expiredId),json::object{{"v",3},{"key_id",prepared.policy.key_id},{"request_id",expiredId},{"route","responses"},{"state","reserved"},{"reserved_at_ms",now-10'000},{"lease_expires_at_ms",now-1},{"provider",nullptr},{"dispatched_at_ms",nullptr},{"released_at_ms",nullptr},{"release_reason",nullptr}});api_key_policy::ReserveApiKeyUsageV3Options options;options.kv=h::kv;options.nowMs=now;AWAIT(admission,h::reserveApiKeyUsageV3(prepared.policy,"replacement-request","responses",options));ASSERT(std::holds_alternative<api_key_policy::ApiKeyUsageReservationDecisionOk>(admission));const auto expired=value(h::apiKeyUsageV3RequestKey(prepared.policy,expiredId));EXPECT_EQ(rt::getString(expired,"state"),"released");EXPECT_EQ(rt::getString(expired,"release_reason"),"lease_expired");EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"reserved_requests"),1);auto releasing=std::get<api_key_policy::ApiKeyUsageReservationDecisionOk>(admission).reservation.release(std::nullopt);co_await std::move(releasing);EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"reserved_requests"),0);EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"committed_requests"),0);
}
ASYNC_TEST("V3 dispatch CAS failure prevents a provider fetch and exhausted quota does not block models") {
  Scope scope;AWAIT(prepared,h::prepareApiKeyInference("d","dispatch-cas",1));int fetchCalls=0,inference=0;rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo input,rt::RequestInit){++fetchCalls;const auto url=h::fetchInputUrl(input);if(url=="https://api.openlux.ai/v1/models"||url=="https://api.surplusintelligence.ai/v1/models")return rt::Response::json(json::object{{"data",json::array{}}});++inference;return rt::Response("unexpected inference transport "+url,rt::ResponseInit{.status=500});}));h::kv->failNextCommits=5;AWAIT(unavailable,h::handler(h::request(prepared.token)));EXPECT_EQ(unavailable.status(),503);EXPECT_EQ(fetchCalls,0);EXPECT_EQ(inference,0);h::kv->failNextCommits=0;auto window=json::value_from(h::makeApiKeyUsageWindowV3(prepared.policy)).as_object();window["committed_requests"]=1;put(h::apiKeyUsageV3WindowKey(prepared.policy),window);const rt::Request req("https://ai.ubq.fi/v1/models",rt::RequestInit{.headers=rt::Headers{{"Authorization","Bearer "+prepared.token}}});AWAIT(models,h::handler(req));EXPECT_EQ(models.status(),200);EXPECT_EQ(inference,0);
}
namespace {
  struct Upstream {std::optional<rt::ReadableStreamDefaultController<rt::ByteChunk>> controller;int cancelled=0;};
  rt::Response openSse(const std::string& id,std::shared_ptr<Upstream> state,std::optional<std::string> providerId={}){rt::UnderlyingSource<rt::ByteChunk> source;source.start=[state,id](auto controller){state->controller=controller;controller.enqueue(h::textEncoder.encode("data: "+rt::jsonStringify(json::object{{"type","response.created"},{"response",json::object{{"id",id}}}})+"\n\n"));controller.enqueue(h::textEncoder.encode(h::semanticSseEvent()));};source.cancel=[state](rt::Reason){++state->cancelled;};const rt::ByteStream body(source);rt::Headers headers{{"Content-Type","text/event-stream"}};if(providerId)headers.set("X-Request-Id",*providerId);return rt::Response(rt::BodyInit(body),rt::ResponseInit{.status=200,.headers=headers});}
  rt::Response closedSse(const std::string& text,std::optional<std::string> id={}){rt::Headers headers{{"Content-Type","text/event-stream"}};if(id)headers.set("X-Request-Id",*id);return rt::Response(text,rt::ResponseInit{.status=200,.headers=headers});}
  rt::Task<void> drain(rt::Response response){const auto text=co_await response.text();(void)text;}
}
ASYNC_TEST("streaming V3 quota is committed at dispatch, including premature and cancelled streams") {
  Scope scope;const std::vector<std::string> routes{"responses","chat"},completeDigits{"a","b"},truncatedDigits{"c","d"},cancelledDigits{"e","f"};
  for(std::size_t index=0;index<routes.size();++index){const auto route=routes[index];AWAIT(prepared,h::prepareApiKeyInference(completeDigits[index],"stream-completed-"+route,100));auto upstream=std::make_shared<Upstream>();rt::GlobalFetchOverride fetch(rt::Fetch([upstream,route](rt::RequestInfo,rt::RequestInit){return openSse(route,upstream);}));AWAIT(response,h::handler(h::streamingRequest(prepared.token,route)));EXPECT_EQ(response.status(),200);EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"committed_requests"),1);ASSERT(response.body());auto reader=response.body()->getReader();if(route=="responses"){AWAIT(created,reader.read());EXPECT(!created.done);EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"committed_requests"),1);}auto completedChunk=reader.readPromise();ASSERT(upstream->controller);upstream->controller->enqueue(h::textEncoder.encode(h::completedSseEvent(3,4)));upstream->controller->enqueue(h::textEncoder.encode(h::completedSseEvent(5,6)));upstream->controller->close();const auto chunk=co_await completedChunk.wait();EXPECT(!chunk.done);for(;;){AWAIT(part,reader.read());if(part.done)break;}EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"committed_requests"),1);EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"reserved_requests"),0);}
  for(std::size_t index=0;index<routes.size();++index){const auto route=routes[index];AWAIT(prepared,h::prepareApiKeyInference(truncatedDigits[index],"stream-truncated-"+route,100));rt::GlobalFetchOverride fetch(rt::Fetch([route](rt::RequestInfo,rt::RequestInit){return closedSse("data: "+rt::jsonStringify(json::object{{"type","response.created"},{"response",json::object{{"id",route}}}})+"\n\n"+h::semanticSseEvent());}));AWAIT(response,h::handler(h::streamingRequest(prepared.token,route)));EXPECT_EQ(response.status(),200);auto reading=drain(response);co_await std::move(reading);EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"committed_requests"),1);EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"reserved_requests"),0);}
  for(std::size_t index=0;index<routes.size();++index){const auto route=routes[index];AWAIT(prepared,h::prepareApiKeyInference(cancelledDigits[index],"stream-cancelled-"+route,100));auto upstream=std::make_shared<Upstream>();rt::GlobalFetchOverride fetch(rt::Fetch([upstream,route](rt::RequestInfo,rt::RequestInit){return openSse(route,upstream);}));AWAIT(response,h::handler(h::streamingRequest(prepared.token,route)));EXPECT_EQ(response.status(),200);ASSERT(response.body());const auto reason=rt::thrown(json::value("test cancelled before completion"));if(route=="responses"){auto reader=response.body()->getReader();AWAIT(created,reader.read());EXPECT(!created.done);auto cancel=reader.cancelPromise(reason);co_await cancel.wait();}else{auto cancel=response.body()->cancel(reason);co_await std::move(cancel);}try{if(upstream->controller)upstream->controller->close();}catch(...){}EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"committed_requests"),1);EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"reserved_requests"),0);}
}
ASYNC_TEST("Codex terminal health distinguishes completion, post-header failure, and cancellation") {
  Scope scope;const rt::KvKey healthKey{"uos_ai","provider_health","v1","codex","acct-1","current"};auto waitForHealth=[&](std::string event,std::string id)->rt::Task<provider::health::ProviderHealthView>{auto predicate=[&,event,id]{const auto current=value(healthKey);return rt::getString(current,"event")==event&&rt::getString(current,"provider_request_id")==id;};auto waiting=h::waitFor(predicate,event+" health for "+id);co_await std::move(waiting);auto reading=h::getCodexProviderHealth("acct-1");auto health=co_await std::move(reading);co_return health;};
  const std::vector<std::string> routes{"responses","chat"},digits{"2","3"};for(std::size_t index=0;index<routes.size();++index){const auto route=routes[index];AWAIT(prepared,h::prepareApiKeyInference(digits[index],"terminal-health-"+route,20));h::resetProviderHealthThrottleForTest();auto success=[&](const std::string& id){return closedSse((route=="chat"?h::semanticSseEvent():"")+h::completedSseEvent(),id);};
    const auto firstId=route+"-success-1";{rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo,rt::RequestInit){return success(firstId);}));AWAIT(response,h::handler(h::streamingRequest(prepared.token,route)));EXPECT_EQ(response.status(),200);auto reading=drain(response);co_await std::move(reading);}AWAIT(healthy,waitForHealth("success",firstId));EXPECT_EQ(healthy.state,"healthy");EXPECT_EQ(healthy.last_status,200);
    const auto failureId=route+"-post-header-failure";{rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo,rt::RequestInit){return closedSse("data: "+rt::jsonStringify(json::object{{"type","response.created"},{"response",json::object{{"id",failureId}}}})+"\n\n"+h::semanticSseEvent(),failureId);}));AWAIT(response,h::handler(h::streamingRequest(prepared.token,route)));EXPECT_EQ(response.status(),200);auto reading=drain(response);co_await std::move(reading);}AWAIT(degraded,waitForHealth("upstream_error",failureId));EXPECT_EQ(degraded.state,"degraded");EXPECT_EQ(degraded.last_status,200);
    const auto recoveredId=route+"-success-2";{rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo,rt::RequestInit){return success(recoveredId);}));AWAIT(response,h::handler(h::streamingRequest(prepared.token,route)));EXPECT_EQ(response.status(),200);auto reading=drain(response);co_await std::move(reading);}AWAIT(recovered,waitForHealth("success",recoveredId));EXPECT_EQ(recovered.state,"healthy");
    const auto terminalFailureId=route+"-response-failed";{rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo,rt::RequestInit){return closedSse("data: "+rt::jsonStringify(json::object{{"type","response.failed"},{"response",json::object{{"id",terminalFailureId},{"error",json::object{{"code","fixture_failure"}}}}}})+"\n\n",terminalFailureId);}));AWAIT(response,h::handler(h::streamingRequest(prepared.token,route)));auto reading=drain(response);co_await std::move(reading);}AWAIT(failedTerminal,waitForHealth("upstream_error",terminalFailureId));EXPECT_EQ(failedTerminal.state,"degraded");
    auto lastHealthyId=route+"-success-3";{rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo,rt::RequestInit){return success(lastHealthyId);}));AWAIT(response,h::handler(h::streamingRequest(prepared.token,route)));EXPECT_EQ(response.status(),200);auto reading=drain(response);co_await std::move(reading);}AWAIT(finalRecovery,waitForHealth("success",lastHealthyId));EXPECT_EQ(finalRecovery.state,"healthy");
    for(const auto& terminalType:std::vector<std::string>{"response.completed","response.failed"}){const auto malformedId=route+"-"+terminalType+"-array";{rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo,rt::RequestInit){return closedSse("data: "+rt::jsonStringify(json::object{{"type",terminalType},{"response",json::array{}}})+"\n\n",malformedId);}));AWAIT(response,h::handler(h::streamingRequest(prepared.token,route)));EXPECT_EQ(response.status(),502);auto reading=drain(response);co_await std::move(reading);}AWAIT(malformedHealth,waitForHealth("upstream_error",malformedId));EXPECT_EQ(malformedHealth.state,"degraded");lastHealthyId=malformedId+"-recovered";{rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo,rt::RequestInit){return success(lastHealthyId);}));AWAIT(response,h::handler(h::streamingRequest(prepared.token,route)));EXPECT_EQ(response.status(),200);auto reading=drain(response);co_await std::move(reading);}AWAIT(malformedRecovery,waitForHealth("success",lastHealthyId));EXPECT_EQ(malformedRecovery.state,"healthy");}
    const auto incompleteId=route+"-incomplete";{rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo,rt::RequestInit){return closedSse("data: "+rt::jsonStringify(json::object{{"type","response.incomplete"},{"response",json::object{{"id",incompleteId}}}})+"\n\n",incompleteId);}));AWAIT(response,h::handler(h::streamingRequest(prepared.token,route)));auto reading=drain(response);co_await std::move(reading);}co_await rt::sleep(0);AWAIT(afterIncomplete,h::getCodexProviderHealth("acct-1"));EXPECT_EQ(afterIncomplete.state,"healthy");EXPECT_EQ(afterIncomplete.last_event,"success");EXPECT_EQ(afterIncomplete.last_provider_request_id,lastHealthyId);
    auto upstream=std::make_shared<Upstream>();const auto cancelledId=route+"-cancelled";{rt::GlobalFetchOverride fetch(rt::Fetch([upstream,route,cancelledId](rt::RequestInfo,rt::RequestInit){return openSse(cancelledId,upstream,cancelledId);}));AWAIT(cancelled,h::handler(h::streamingRequest(prepared.token,route)));EXPECT_EQ(cancelled.status(),200);ASSERT(cancelled.body());const auto reason=rt::thrown(json::value("client cancelled"));if(route=="responses"){auto reader=cancelled.body()->getReader();AWAIT(created,reader.read());EXPECT(!created.done);auto cancel=reader.cancelPromise(reason);co_await cancel.wait();}else{auto cancel=cancelled.body()->cancel(reason);co_await std::move(cancel);}}auto predicate=[upstream]{return upstream->cancelled==1;};auto waiting=h::waitFor(predicate,route+" upstream cancellation");co_await std::move(waiting);co_await rt::sleep(0);AWAIT(afterCancellation,h::getCodexProviderHealth("acct-1"));EXPECT_EQ(afterCancellation.state,"healthy");EXPECT_EQ(afterCancellation.last_event,"success");EXPECT_EQ(afterCancellation.last_provider_request_id,lastHealthyId);
  }h::resetProviderHealthThrottleForTest();h::resetCodexAuthCacheForTest();
}
ASYNC_TEST("provider dispatch commits API-key V3 while kernel completion writes only the split window") {
  Scope scope;AWAIT(prepared,h::prepareApiKeyInference("0","stream-kernel-and-key",100));auto seeding=h::seedKernelTestPublicKey();co_await std::move(seeding);AWAIT(kernelToken,h::makeKernelTestToken(prepared.token,"lifecycle-org","lifecycle-repo"));auto upstream=std::make_shared<Upstream>();
  {rt::GlobalFetchOverride fetch(rt::Fetch([upstream](rt::RequestInfo,rt::RequestInit){return openSse("kernel",upstream);}));auto base=h::streamingRequest(prepared.token,"responses");auto headers=base.headers();headers.set("X-Ubiquity-Kernel-Token",kernelToken);const rt::Request req(base,rt::RequestInit{.headers=headers});AWAIT(response,h::handler(req));EXPECT_EQ(response.status(),200);const auto key=h::kernelOrgWindowKey("lifecycle-org");EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"committed_requests"),1);EXPECT_EQ(rt::getNumber(value(key),"usage_requests"),0);EXPECT_EQ(rt::getNumber(value(key),"reserved_requests"),1);rt::Promise<std::string> body(response.text());ASSERT(upstream->controller);upstream->controller->enqueue(h::textEncoder.encode(h::completedSseEvent(2,3)));upstream->controller->enqueue(h::textEncoder.encode(h::completedSseEvent(4,5)));upstream->controller->close();co_await body.wait();EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"committed_requests"),1);EXPECT_EQ(rt::getNumber(value(key),"usage_requests"),1);EXPECT_EQ(rt::getNumber(value(key),"reserved_requests"),0);}
  int imageFetches=0;rt::GlobalFetchOverride fetch(rt::Fetch([&imageFetches](rt::RequestInfo,rt::RequestInit){++imageFetches;const json::object event{{"type","response.completed"},{"response",json::object{{"model",h::MODEL},{"created_at",1787431659},{"output",json::array{json::object{{"type","image_generation_call"},{"status","completed"},{"result","SU1BR0U="}}}},{"usage",json::object{{"input_tokens",3},{"output_tokens",4},{"total_tokens",7}}}}}};return closedSse("data: "+rt::jsonStringify(event)+"\n\n");}));const rt::Request request("https://ai.ubq.fi/v1/images/generations",rt::RequestInit{.method="POST",.headers=rt::Headers{{"Authorization","Bearer "+prepared.token},{"Content-Type","application/json"},{"X-Ubiquity-Kernel-Token",kernelToken}},.body=R"({"prompt":"five telemetry regression images","n":5,"user":"kernel-image-user"})"});rt::Promise<rt::Response> pending(h::handler(request));auto image=co_await pending.wait();EXPECT_EQ(image.status(),200);AWAIT(payload,image.json());const json::array expected{json::object{{"b64_json","SU1BR0U="}},json::object{{"b64_json","SU1BR0U="}},json::object{{"b64_json","SU1BR0U="}},json::object{{"b64_json","SU1BR0U="}},json::object{{"b64_json","SU1BR0U="}}};EXPECT(rt::deepEqual(payload.at("data"),expected));EXPECT_EQ(image.headers().get("x-uos-warning"),"user_ignored");EXPECT_EQ(imageFetches,5);EXPECT_EQ(rt::getNumber(h::usageWindow(prepared.policy),"committed_requests"),2);const auto key=h::kernelOrgWindowKey("lifecycle-org");EXPECT_EQ(rt::getNumber(value(key),"usage_requests"),2);EXPECT_EQ(rt::getNumber(value(key),"reserved_requests"),0);
}
ASYNC_TEST("Kernel quota reserves one concurrent limit-one request and commits only its semantic completion") {
  Scope scope;AWAIT(prepared,h::prepareApiKeyInference("1","kernel-concurrent-limit-one",100));auto seeding=h::seedKernelTestPublicKey();co_await std::move(seeding);h::seedKernelDefaultLimit(1);const std::string owner="kernel-concurrency-org",repo="kernel-concurrency-repo";auto upstream=std::make_shared<Upstream>();int calls=0;rt::GlobalFetchOverride fetch(rt::Fetch([upstream,&calls](rt::RequestInfo,rt::RequestInit){++calls;return openSse("kernel-concurrent",upstream);}));std::vector<rt::Promise<rt::Request>> signing;for(int index=0;index<8;++index)signing.emplace_back(h::withKernelTestToken(h::streamingRequest(prepared.token,"responses"),prepared.token,owner,repo));auto signedAll=rt::all(signing);const auto requests=co_await signedAll.wait();std::vector<rt::Promise<rt::Response>> pending;for(const auto& request:requests)pending.emplace_back(h::handler(request));auto allResponses=rt::all(pending);const auto responses=co_await allResponses.wait();EXPECT_EQ(calls,1);std::vector<int> statuses;std::optional<rt::Response> admitted;for(const auto& response:responses){statuses.push_back(response.status());if(response.status()==200)admitted=response;}std::sort(statuses.begin(),statuses.end());const std::vector<int> expected{200,429,429,429,429,429,429,429};EXPECT_EQ(statuses,expected);const auto key=h::kernelOrgWindowKey(owner);EXPECT_EQ(rt::getNumber(value(key),"usage_requests"),0);EXPECT_EQ(rt::getNumber(value(key),"reserved_requests"),1);ASSERT(admitted);rt::Promise<std::string> body(admitted->text());ASSERT(upstream->controller);upstream->controller->enqueue(h::textEncoder.encode(h::completedSseEvent()));upstream->controller->close();co_await body.wait();auto predicate=[key]{const auto window=value(key);return rt::getNumber(window,"usage_requests")==1&&rt::getNumber(window,"reserved_requests")==0;};auto waiting=h::waitFor(predicate,"Kernel semantic completion commit");co_await std::move(waiting);
}
namespace {
  using KernelDecision=kernel::quota_v2::KernelQuotaReservationDecision;
  kernel::quota_v2::KernelQuotaReservation reservation(const KernelDecision& decision){ASSERT(std::holds_alternative<kernel::quota_v2::KernelQuotaReservationDecisionOk>(decision));return std::get<kernel::quota_v2::KernelQuotaReservationDecisionOk>(decision).reservation;}
  void refused(const KernelDecision& decision,int status){ASSERT(std::holds_alternative<kernel::quota_v2::KernelQuotaReservationDecisionNotOk>(decision));EXPECT_EQ(std::get<kernel::quota_v2::KernelQuotaReservationDecisionNotOk>(decision).response.status(),status);}
}
ASYNC_TEST("Kernel quota reconstructs reservations after an older writer erases the aggregate field") {
  Scope scope;h::seedKernelDefaultLimit(2);const std::string owner="kernel-mixed-revision-org";kernel::quota_reserve::ReserveKernelUsageLimitOptions options;options.kv=h::kv;AWAIT(first,h::reserveKernelOrgUsageLimit(owner,"mixed-revision-first","responses",options));const auto firstReservation=reservation(first);const auto key=h::kernelOrgWindowKey(owner);auto reserved=value(key).as_object();EXPECT_EQ(rt::getNumber(reserved,"reserved_requests"),1);rt::objectDelete(reserved,"reserved_requests");reserved["usage_requests"]=1;reserved["updated_at_ms"]=rt::dateNow();put(key,reserved);AWAIT(blocked,h::reserveKernelOrgUsageLimit(owner,"mixed-revision-second","responses",options));refused(blocked,429);const auto release=firstReservation.release(std::string("mixed_revision_test_complete"));co_await release.wait();EXPECT_EQ(rt::getNumber(value(key),"usage_requests"),1);EXPECT_EQ(rt::getNumber(value(key),"reserved_requests"),0);
}
ASYNC_TEST("Kernel quota releases a cancelled stream before admitting its replacement") {
  Scope scope;AWAIT(prepared,h::prepareApiKeyInference("2","kernel-cancel-release",100));auto seeding=h::seedKernelTestPublicKey();co_await std::move(seeding);h::seedKernelDefaultLimit(1);const std::string owner="kernel-cancel-org",repo="kernel-cancel-repo";int calls=0;auto upstream=std::make_shared<Upstream>();rt::GlobalFetchOverride fetch(rt::Fetch([&calls,upstream](rt::RequestInfo,rt::RequestInit){++calls;return calls>1?h::validStreamingSse():openSse("kernel-cancel",upstream);}));AWAIT(firstRequest,h::withKernelTestToken(h::streamingRequest(prepared.token,"responses"),prepared.token,owner,repo));AWAIT(first,h::handler(firstRequest));EXPECT_EQ(first.status(),200);ASSERT(first.body());const auto reason=rt::thrown(json::value("client cancelled"));auto cancelling=first.body()->cancel(reason);co_await std::move(cancelling);const auto key=h::kernelOrgWindowKey(owner);auto released=[key]{return rt::getNumber(value(key),"reserved_requests")==0;};auto waiting=h::waitFor(released,"Kernel cancellation release");co_await std::move(waiting);auto cancelled=[upstream]{return upstream->cancelled==1;};auto upstreamWaiting=h::waitFor(cancelled,"Kernel upstream cancellation");co_await std::move(upstreamWaiting);AWAIT(replacementRequest,h::withKernelTestToken(h::streamingRequest(prepared.token,"responses"),prepared.token,owner,repo));AWAIT(replacement,h::handler(replacementRequest));EXPECT_EQ(replacement.status(),200);auto reading=drain(replacement);co_await std::move(reading);EXPECT_EQ(rt::getNumber(value(key),"usage_requests"),1);EXPECT_EQ(rt::getNumber(value(key),"reserved_requests"),0);EXPECT_EQ(calls,2);
}
ASYNC_TEST("Kernel quota releases validation failures and admits a later valid request") {
  Scope scope;AWAIT(prepared,h::prepareApiKeyInference("3","kernel-validation-release",100));auto seeding=h::seedKernelTestPublicKey();co_await std::move(seeding);h::seedKernelDefaultLimit(1);const std::string owner="kernel-validation-org",repo="kernel-validation-repo";int calls=0;rt::GlobalFetchOverride fetch(rt::Fetch([&calls](rt::RequestInfo,rt::RequestInit){++calls;return h::validStreamingSse();}));const rt::Request invalidBase("https://ai.ubq.fi/v1/responses",rt::RequestInit{.method="POST",.headers=rt::Headers{{"Authorization","Bearer "+prepared.token},{"Content-Type","application/json"}},.body=R"({"input":"ping","unsupported_kernel_test_field":true})"});AWAIT(invalidRequest,h::withKernelTestToken(invalidBase,prepared.token,owner,repo));AWAIT(invalid,h::handler(invalidRequest));EXPECT_EQ(invalid.status(),400);EXPECT_EQ(calls,0);const auto key=h::kernelOrgWindowKey(owner);EXPECT_EQ(rt::getNumber(value(key),"usage_requests"),0);EXPECT_EQ(rt::getNumber(value(key),"reserved_requests"),0);AWAIT(replacementRequest,h::withKernelTestToken(h::streamingRequest(prepared.token,"responses"),prepared.token,owner,repo));AWAIT(replacement,h::handler(replacementRequest));EXPECT_EQ(replacement.status(),200);auto reading=drain(replacement);co_await std::move(reading);EXPECT_EQ(rt::getNumber(value(key),"usage_requests"),1);EXPECT_EQ(rt::getNumber(value(key),"reserved_requests"),0);EXPECT_EQ(calls,1);
}
ASYNC_TEST("Kernel quota reclaims an expired reservation lease before admission") {
  Scope scope;AWAIT(prepared,h::prepareApiKeyInference("4","kernel-lease-recovery",100));auto seeding=h::seedKernelTestPublicKey();co_await std::move(seeding);h::seedKernelDefaultLimit(1);const std::string owner="kernel-lease-org",repo="kernel-lease-repo",id="expired-kernel-request";const double now=rt::dateNow(),created=now-10'000,reset=now+60'000;const auto key=h::kernelOrgWindowKey(owner),expiredKey=h::kernelOrgReservationKey(owner,created,id);put(key,json::object{{"v",2},{"scope","org"},{"owner",owner},{"usage_requests",0},{"reserved_requests",1},{"usage_reset_at_ms",reset},{"applied_window_ms",60'000},{"created_at_ms",created},{"updated_at_ms",now}});put(expiredKey,json::object{{"v",2},{"scope","org"},{"owner",owner},{"request_id",id},{"route","responses"},{"window_created_at_ms",created},{"window_reset_at_ms",reset},{"state","reserved"},{"reserved_at_ms",now-10'000},{"lease_expires_at_ms",now-1},{"committed_at_ms",nullptr},{"released_at_ms",nullptr},{"release_reason",nullptr}});rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo,rt::RequestInit){return h::validStreamingSse();}));AWAIT(req,h::withKernelTestToken(h::streamingRequest(prepared.token,"responses"),prepared.token,owner,repo));AWAIT(response,h::handler(req));EXPECT_EQ(response.status(),200);auto reading=drain(response);co_await std::move(reading);EXPECT_EQ(rt::getString(value(expiredKey),"state"),"released");EXPECT_EQ(rt::getString(value(expiredKey),"release_reason"),"lease_expired");EXPECT_EQ(rt::getNumber(value(key),"usage_requests"),1);EXPECT_EQ(rt::getNumber(value(key),"reserved_requests"),0);
}
ASYNC_TEST("Kernel quota reservation fails closed when KV is unavailable") {Scope scope;kernel::quota_reserve::ReserveKernelUsageLimitOptions options;options.kv=std::shared_ptr<rt::Kv>();AWAIT(decision,h::reserveKernelOrgUsageLimit("kernel-unavailable-org","request-id","responses",options));refused(decision,503);}
ASYNC_TEST("Kernel quota rejects malformed effective-scope policy records") {Scope scope;h::kv->resetCounts();h::seedKernelDefaultLimit(10);const std::string owner="kernel-malformed-scope-org",repo="kernel-malformed-scope-repo";put(h::kernelRepoPolicyKey(owner,repo),json::object{{"v",2},{"scope","repo"},{"owner",owner},{"repo",repo},{"usage_limit_requests","not-a-limit"}});kernel::quota_reserve::ReserveKernelUsageLimitOptions options;options.kv=h::kv;AWAIT(decision,h::reserveEffectiveKernelUsageLimit(owner,repo,"malformed-scope-request","responses",options));refused(decision,503);EXPECT(!h::kv->values.has(h::encodeKey(h::kernelOrgWindowKey(owner))));}
ASYNC_TEST("Kernel quota CAS-checks repo-policy absence before org admission") {Scope scope;h::kv->resetCounts();h::seedKernelDefaultLimit(10);const std::string owner="kernel-scope-race-org",repo="kernel-scope-race-repo";h::kv->onKernelReservationCommit=[owner,repo]{const auto now=rt::dateNow();put(h::kernelRepoPolicyKey(owner,repo),json::object{{"v",2},{"scope","repo"},{"owner",owner},{"repo",repo},{"usage_limit_requests",0},{"window_ms",60'000},{"expires_at_ms",-1},{"created_at_ms",now},{"updated_at_ms",now}});};kernel::quota_reserve::ReserveKernelUsageLimitOptions options;options.kv=h::kv;AWAIT(decision,h::reserveEffectiveKernelUsageLimit(owner,repo,"scope-race-request","responses",options));refused(decision,503);EXPECT(!h::kv->values.has(h::encodeKey(h::kernelOrgWindowKey(owner))));}
ASYNC_TEST("Kernel quota CAS-checks default policy entries before admission") {Scope scope;h::kv->resetCounts();h::seedKernelDefaultLimit(10);const std::string owner="kernel-default-race-org";h::kv->onKernelReservationCommit=[] {put(h::DEFAULT_KERNEL_POLICY_LIMIT_KEY,0);};kernel::quota_reserve::ReserveKernelUsageLimitOptions options;options.kv=h::kv;AWAIT(decision,h::reserveKernelOrgUsageLimit(owner,"default-race-request","responses",options));refused(decision,429);EXPECT(!h::kv->values.has(h::encodeKey(h::kernelOrgWindowKey(owner))));}
ASYNC_TEST("Kernel quota renews an active reservation and aborts if renewal cannot reach KV before expiry") {
  Scope scope;h::kv->resetCounts();h::seedKernelDefaultLimit(1,h::KERNEL_QUOTA_RESERVATION_LEASE_MS*2);const std::string owner="kernel-renewal-org",id="kernel-renewal-request";kernel::quota_reserve::ReserveKernelUsageLimitOptions options;options.kv=h::kv;options.nowMs=rt::dateNow()-h::KERNEL_QUOTA_RESERVATION_LEASE_MS+30;options.renewalIntervalMs=5;AWAIT(decision,h::reserveKernelOrgUsageLimit(owner,id,"responses",options));const auto active=reservation(decision);const auto window=value(h::kernelOrgWindowKey(owner));const auto key=h::kernelOrgReservationKey(owner,*rt::getNumber(window,"created_at_ms"),id);const auto initial=*rt::getNumber(value(key),"lease_expires_at_ms");auto renewed=[key,initial]{return rt::getNumber(value(key),"lease_expires_at_ms")!=initial;};auto waiting=h::waitFor(renewed,"Kernel lease renewal");co_await std::move(waiting);EXPECT(!active.signal.aborted());EXPECT(rt::getNumber(value(key),"lease_expires_at_ms")>initial);auto release=active.release(std::string("test_cleanup"));co_await release.wait();
  options.nowMs=rt::dateNow()-h::KERNEL_QUOTA_RESERVATION_LEASE_MS+30;AWAIT(failing,h::reserveKernelOrgUsageLimit("kernel-renewal-failure-org","kernel-renewal-failure-request","responses",options));const auto failingReservation=reservation(failing);h::kv->failKernelQuotaReads=true;auto aborted=[failingReservation]{return failingReservation.signal.aborted();};auto waitingFailure=h::waitFor(aborted,"Kernel lease renewal fail-closed abort");co_await std::move(waitingFailure);h::kv->failKernelQuotaReads=false;auto failureRelease=failingReservation.release(std::string("test_cleanup"));co_await failureRelease.wait();
}
ASYNC_TEST("Kernel quota lease expiry aborts independently while a renewal read hangs") {Scope scope;h::kv->resetCounts();h::seedKernelDefaultLimit(1,h::KERNEL_QUOTA_RESERVATION_LEASE_MS*2);kernel::quota_reserve::ReserveKernelUsageLimitOptions options;options.kv=h::kv;options.nowMs=rt::dateNow()-h::KERNEL_QUOTA_RESERVATION_LEASE_MS+80;options.renewalIntervalMs=5;AWAIT(decision,h::reserveKernelOrgUsageLimit("kernel-hung-renewal-org","kernel-hung-renewal-request","responses",options));const auto active=reservation(decision);const auto gate=h::deferred();bool readUnblocked=false;h::kv->kernelQuotaReadGate=gate.promise;auto aborted=[active]{return active.signal.aborted();};auto waiting=h::waitFor(aborted,"Kernel hung-renewal lease expiry abort");co_await std::move(waiting);EXPECT(!readUnblocked);h::kv->kernelQuotaReadGate.reset();readUnblocked=true;gate.resolve();auto release=active.release(std::string("test_cleanup"));co_await release.wait();}
ASYNC_TEST("first bounded paid fallback response exposes settled spend and consumes one V3 dispatch") {
  Scope scope;put(h::RUNTIME_CONFIG_V2_KEY,h::runtime());const auto pool=h::codexAuthPool(2);put(h::CODEX_AUTH_POOL_KV_KEY,pool);for(const auto& account:pool.at("accounts").as_array()){const auto id=std::string(*rt::getString(account,"account_id"));AWAIT(hash,h::sha256Hex(id));put(codex::reset_settings::codexResetUsageKey(hash),json::object{{"enabled",false}});}const std::string token="u_"+std::string(64,'8'),keyId="first-fallback-quota";AWAIT(seeded,h::seedPaidFallbackKey(token,keyId));const auto policy=h::apiKeyPolicyFromHashRecord(seeded.hash,json::value(seeded.record),rt::dateNow());ASSERT(policy);int calls=0;std::vector<std::string> accountIds;rt::env::set("METERED_API_KEY","metered-test-key");rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo input,rt::RequestInit init){++calls;const rt::Request request=std::holds_alternative<rt::Request>(input)?rt::Request(std::get<rt::Request>(input),init):rt::Request(h::fetchInputUrl(input),init);if(request.url()=="https://api.openlux.ai/v1/responses"){const auto response=h::sse();auto headers=response.headers();headers.set("X-Oneapi-Request-Id","first-fallback-provider-request");return rt::Response(rt::bodyInit(response.body()),rt::ResponseInit{.status=200,.headers=headers});}accountIds.push_back(request.headers().get("chatgpt-account-id").value_or(""));return h::authoritativeCodexQuotaResponse();}));h::kv->resetCounts();const rt::Request request("https://ai.ubq.fi/v1/responses",rt::RequestInit{.method="POST",.headers=rt::Headers{{"Authorization","Bearer "+token},{"Content-Type","application/json"}},.body=R"({"input":"ping","stream":true})"});AWAIT(response,h::handler(request));EXPECT_EQ(response.status(),200);EXPECT_EQ(response.headers().get("x-codex-primary-used-percent"),"0");EXPECT_EQ(calls,3);const std::vector<std::string> expected{"acct-1","acct-2"};EXPECT_EQ(accountIds,expected);EXPECT_EQ(rt::getNumber(h::usageWindow(*policy),"committed_requests"),1);EXPECT_EQ(rt::getNumber(h::usageWindow(*policy),"reserved_requests"),0);if(auto body=response.body()){auto cancel=body->cancel();co_await std::move(cancel);}auto backfill=paid_fallback::ledger_backfill::scheduledPaidFallbackBackfillForTest();if(backfill)co_await backfill->wait();
}

namespace {
  std::string retryFixtureEvent(const json::object& event) {
    return "data: " + rt::jsonStringify(event) + "\n\n";
  }

  std::string retryFixturePreamble(const std::string& id) {
    const json::object response{{"id", id}, {"status", "in_progress"}, {"output", json::array{}}};
    return retryFixtureEvent({{"type", "response.created"}, {"sequence_number", 0}, {"response", response}})
         + retryFixtureEvent({{"type", "response.in_progress"}, {"sequence_number", 1}, {"response", response}});
  }

  std::string retryFixtureFailure(const std::string& code, int sequence = 2) {
    return retryFixtureEvent({{"type", "error"}, {"sequence_number", sequence}, {"code", code},
                              {"message", "fixture upstream failure detail"}, {"param", nullptr}});
  }

  std::string retryFixtureDelta() {
    return retryFixtureEvent({{"type", "response.output_text.delta"}, {"sequence_number", 2}, {"delta", "finished"}});
  }

  std::string retryFixtureSuccess(const std::string& id) {
    const json::object response{{"id", id}, {"status", "completed"}, {"usage", json::object{{"input_tokens", 3}, {"output_tokens", 4}, {"total_tokens", 7}}}};
    return retryFixturePreamble(id) + retryFixtureDelta()
         + retryFixtureEvent({{"type", "response.completed"}, {"sequence_number", 3}, {"response", response}});
  }

  std::string retryFixtureRateLimit(double retryAfterMs) {
    const json::object error{{"type", "rate_limit_error"}, {"code", "rate_limit_exceeded"}, {"retry_after_ms", retryAfterMs},
                             {"message", "fixture upstream failure detail"}};
    const json::object response{{"id", "resp_rate_limited"}, {"status", "failed"}, {"error", error}, {"output", json::array{}},
                                {"usage", json::object{{"input_tokens", 99}, {"output_tokens", 0}, {"total_tokens", 99}}}};
    return retryFixturePreamble("resp_rate_limited")
         + retryFixtureEvent({{"type", "response.failed"}, {"sequence_number", 2}, {"response", response}});
  }

  struct ScriptedCodexStream {
    struct Attempt {
      double atMs;
      std::string requestBody;
      std::optional<std::string> conversation;
      std::optional<std::string> account;
    };
    std::vector<Attempt> attempts;
    std::vector<rt::Response> replies;
    rt::GlobalFetchOverride mock;

    explicit ScriptedCodexStream(std::vector<rt::Response> script) : replies(std::move(script)),
      mock(rt::Fetch([this](rt::RequestInfo input, rt::RequestInit init) {
        EXPECT_EQ(h::fetchInputUrl(input), config::config.codexBaseUrl + "/responses");
        ASSERT(init.body && init.headers);
        attempts.push_back({rt::performanceNow(), std::get<std::string>(*init.body),
                            init.headers->get("conversation_id"), init.headers->get("chatgpt-account-id")});
        ASSERT(attempts.size() <= replies.size());
        return replies[attempts.size() - 1];
      })) {}
  };

  std::vector<json::value> retryFixtureLogs(const rt::console::Capture& logs, const std::string& label) {
    std::vector<json::value> records;
    for (const auto& call : logs.calls()) {
      if (call.args.size() < 2 || call.args[0] != label) continue;
      auto record = rt::jsonParse(call.args[1].string());
      if (label != "[ai.ubq.fi] codex_routing" || rt::getString(record, "event") == "codex_attempt") records.push_back(std::move(record));
    }
    return records;
  }

  void expectStreamRetryAttempts(const rt::console::Capture& logs, const std::vector<double>& statuses) {
    const auto attempts = retryFixtureLogs(logs, "[ai.ubq.fi] codex_routing");
    ASSERT_EQ(attempts.size(), statuses.size());
    for (std::size_t index = 0; index < attempts.size(); ++index) {
      const auto& attempt = attempts[index];
      EXPECT_EQ(rt::getNumber(attempt, "attempt"), static_cast<double>(index + 1));
      EXPECT_EQ(rt::getNumber(attempt, "status"), statuses[index]);
      EXPECT_EQ(rt::getString(attempt, "phase"), index == 0 ? "initial" : "stream_retry");
      EXPECT_EQ(rt::getNumber(attempt, "slot"), 1);
      if (statuses[index] != 200) EXPECT_EQ(rt::getNumber(attempt, "http_status"), 200);
    }
  }

  struct DeliveredRetryReply {
    int status;
    std::string body;
    json::value terminal;
  };

  rt::Task<DeliveredRetryReply> deliverStreamRetryRequest(const rt::console::Capture& logs, rt::Request request) {
    const auto delivered = h::deferred();
    const handler::http::RequestDeliveryInfo info{delivered.promise, request.signal()};
    AWAIT(response, h::handler(request, info));
    AWAIT(body, response.text());
    delivered.resolve();
    auto waiting = h::waitFor([&logs] { return terminalPayloads(logs).size() == 1; }, "stream retry terminal delivery");
    co_await std::move(waiting);
    co_return DeliveredRetryReply{response.status(), std::move(body), terminalPayloads(logs).front()};
  }

  json::array retryFailureEvents(const json::value& terminal) {
    const auto* failures = rt::getArray(terminal, "upstream_failure_events");
    ASSERT(failures);
    for (const auto& failure : *failures) EXPECT(!rt::get(failure, "message"));
    return *failures;
  }

  void expectSelectedStream(const std::string& body, const std::string& id) {
    std::vector<json::value> events;
    for (const auto& match : rt::matchAll(body, rt::Regex("^data: (.+)$", "gm"))) events.push_back(rt::jsonParse(*match[1]));
    ASSERT_EQ(events.size(), 4U);
    const std::vector<std::string_view> types{"response.created", "response.in_progress", "response.output_text.delta", "response.completed"};
    for (std::size_t index = 0; index < events.size(); ++index) {
      EXPECT_EQ(rt::getString(events[index], "type"), types[index]);
      EXPECT_EQ(rt::getNumber(events[index], "sequence_number"), static_cast<double>(index));
      if (const auto* response = rt::get(events[index], "response")) EXPECT_EQ(rt::getString(*response, "id"), id);
    }
  }
}

ASYNC_TEST("Codex retries an early error and delivers one Responses stream and one quota debit") {
  Scope scope;
  AWAIT(key, h::prepareApiKeyInference("1", "codex-stream-retry", 5));
  auto seeding = h::seedKernelTestPublicKey();
  co_await std::move(seeding);
  h::seedKernelDefaultLimit(1);
  const std::string org = "stream-retry-success-org";
  const auto unknown = retryFixtureEvent({{"type", "response.fixture_unknown"}, {"sequence_number", 2}});
  ScriptedCodexStream upstream({
    closedSse(retryFixturePreamble("resp_discarded") + unknown + unknown + retryFixtureFailure("server_error", 3), "failed-attempt"),
    closedSse(retryFixtureSuccess("resp_selected"), "selected-attempt"),
  });

  const auto request = h::streamingRequest(key.token, "responses");
  AWAIT(signedRequest, h::withKernelTestToken(request, key.token, org, "stream-retry"));
  AWAIT(result, deliverStreamRetryRequest(scope.logs, signedRequest));
  EXPECT_EQ(result.status, 200);
  EXPECT(!rt::includes(result.body, "resp_discarded"));
  expectSelectedStream(result.body, "resp_selected");
  expectStreamRetryAttempts(scope.logs, {502, 200});
  ASSERT_EQ(upstream.attempts.size(), 2U);
  EXPECT_EQ(upstream.attempts[0].requestBody, upstream.attempts[1].requestBody);
  EXPECT_EQ(upstream.attempts[0].conversation, upstream.attempts[1].conversation);
  EXPECT_EQ(upstream.attempts[0].account, upstream.attempts[1].account);
  EXPECT(upstream.attempts[1].atMs - upstream.attempts[0].atMs >= 250);
  const auto unknownLogs = retryFixtureLogs(scope.logs, "[ai.ubq.fi] unrecognized_responses_event");
  ASSERT_EQ(unknownLogs.size(), 1U);
  EXPECT_EQ(rt::getString(unknownLogs[0], "type"), "response.fixture_unknown");
  EXPECT_EQ(rt::getNumber(result.terminal, "in_stream_retries"), 1);
  EXPECT_EQ(rt::getString(result.terminal, "stream_terminal_type"), "response.completed");
  EXPECT_EQ(rt::getString(result.terminal, "delivery_outcome"), "delivered");
  EXPECT_EQ(rt::getString(result.terminal, "provider_request_id"), "selected-attempt");
  EXPECT_EQ(rt::getNumber(result.terminal, "total_tokens"), 7);
  const json::array providers{"chatgpt_codex"};
  EXPECT(rt::deepEqual(*rt::get(result.terminal, "attempted_providers"), providers));
  const json::array failures = retryFailureEvents(result.terminal);
  ASSERT_EQ(failures.size(), 1U);
  EXPECT_EQ(rt::getString(failures[0], "type"), "error");
  EXPECT_EQ(rt::getString(failures[0], "code"), "server_error");
  expectWindow(key, 1, 0);
  const auto kernelWindow = value(h::kernelOrgWindowKey(org));
  EXPECT_EQ(rt::getNumber(kernelWindow, "usage_requests"), 1);
  EXPECT_EQ(rt::getNumber(kernelWindow, "reserved_requests"), 0);
  const auto failedHealth = value(rt::KvKey{"uos_ai", "provider_health", "v1", "codex", "acct-1", "upstream_error"});
  EXPECT_EQ(rt::getNumber(failedHealth, "status"), 502);
}

ASYNC_TEST("Codex delivers the last pre-output error after three stream retries") {
  Scope scope;
  AWAIT(key, h::prepareApiKeyInference("2", "codex-stream-retry-bound", 5));
  std::vector<rt::Response> script;
  for (const std::string id : {"resp_first", "resp_second", "resp_third", "resp_last"}) {
    script.push_back(closedSse(retryFixturePreamble(id) + retryFixtureFailure("overloaded_error"), id));
  }
  ScriptedCodexStream upstream(std::move(script));
  const auto request = h::streamingRequest(key.token, "responses");
  AWAIT(result, deliverStreamRetryRequest(scope.logs, request));
  EXPECT_EQ(result.status, 200);
  EXPECT_EQ(result.body, retryFixturePreamble("resp_last") + retryFixtureFailure("overloaded_error"));
  expectStreamRetryAttempts(scope.logs, {503, 503, 503, 503});
  ASSERT_EQ(upstream.attempts.size(), 4U);
  EXPECT_EQ(rt::getNumber(result.terminal, "in_stream_retries"), 3);
  EXPECT_EQ(rt::getString(result.terminal, "stream_terminal_type"), "error");
  EXPECT(rt::get(result.terminal, "first_semantic_commitment_ms")->is_null());
  const json::array failures = retryFailureEvents(result.terminal);
  ASSERT_EQ(failures.size(), 4U);
  for (const auto& failure : failures) EXPECT_EQ(rt::getString(failure, "code"), "overloaded_error");
  AWAIT(health, h::getCodexProviderHealth("acct-1"));
  EXPECT_EQ(health.state, "degraded");
  EXPECT_EQ(health.last_status, 503);
  expectWindow(key, 1, 0);
}

ASYNC_TEST("Codex passes through early deterministic error codes") {
  const std::vector<std::string> codes{"invalid_request_error", "authorization_error", "content_policy_violation", "fixture_not_retryable", "rate_limit_exceeded"};
  for (const auto& code : codes) {
    Scope scope;
    AWAIT(key, h::prepareApiKeyInference("3", "codex-stream-no-retry", 5));
    const auto original = retryFixturePreamble("resp_deterministic") + retryFixtureFailure(code);
    ScriptedCodexStream upstream({closedSse(original)});
    const auto request = h::streamingRequest(key.token, "responses");
    AWAIT(result, deliverStreamRetryRequest(scope.logs, request));
    EXPECT_EQ(result.body, original);
    expectStreamRetryAttempts(scope.logs, {200});
    EXPECT_EQ(upstream.attempts.size(), 1U);
    EXPECT_EQ(rt::getNumber(result.terminal, "in_stream_retries"), 0);
    const json::array failures = retryFailureEvents(result.terminal);
    ASSERT_EQ(failures.size(), 1U);
    EXPECT_EQ(rt::getString(failures[0], "code"), code);
    expectWindow(key, 1, 0);
  }
}

ASYNC_TEST("Codex relays a server error after the first output delta without retrying") {
  Scope scope;
  AWAIT(key, h::prepareApiKeyInference("4", "codex-stream-committed", 5));
  const auto original = retryFixturePreamble("resp_committed") + retryFixtureDelta() + retryFixtureFailure("server_error", 3);
  ScriptedCodexStream upstream({closedSse(original)});
  const auto request = h::streamingRequest(key.token, "responses");
  AWAIT(result, deliverStreamRetryRequest(scope.logs, request));
  EXPECT_EQ(result.body, original);
  expectStreamRetryAttempts(scope.logs, {200});
  EXPECT_EQ(upstream.attempts.size(), 1U);
  EXPECT_EQ(rt::getNumber(result.terminal, "in_stream_retries"), 0);
  EXPECT_EQ(rt::getString(result.terminal, "stream_terminal_type"), "error");
  EXPECT(rt::getNumber(result.terminal, "first_semantic_commitment_ms").has_value());
  const json::array failures = retryFailureEvents(result.terminal);
  ASSERT_EQ(failures.size(), 1U);
  EXPECT_EQ(rt::getString(failures[0], "code"), "server_error");
  expectWindow(key, 1, 0);
}

ASYNC_TEST("Codex retries a stream that ends silently before output") {
  const std::vector<bool> withPreamble{true, false};
  for (const bool hasPreamble : withPreamble) {
    Scope scope;
    AWAIT(key, h::prepareApiKeyInference("5", "codex-stream-eof-retry", 5));
    auto ended = hasPreamble ? closedSse(retryFixturePreamble("resp_eof"))
      : rt::Response(nullptr, {.headers = rt::Headers{{"Content-Type", "text/event-stream"}}});
    ended.headers().set("Retry-After", "1");
    ScriptedCodexStream upstream({ended, closedSse(retryFixtureSuccess("resp_after_eof"))});
    const auto request = h::streamingRequest(key.token, "responses");
    AWAIT(result, deliverStreamRetryRequest(scope.logs, request));
    EXPECT_EQ(result.status, 200);
    expectSelectedStream(result.body, "resp_after_eof");
    expectStreamRetryAttempts(scope.logs, {502, 200});
    ASSERT_EQ(upstream.attempts.size(), 2U);
    EXPECT(upstream.attempts[1].atMs - upstream.attempts[0].atMs >= 1000);
    EXPECT_EQ(rt::getNumber(result.terminal, "in_stream_retries"), 1);
    expectWindow(key, 1, 0);
  }
}

ASYNC_TEST("Codex retries a failed response in Chat Completions after the longer Retry-After hint") {
  Scope scope;
  AWAIT(key, h::prepareApiKeyInference("6", "codex-chat-stream-retry", 5));
  auto refused = closedSse(retryFixtureRateLimit(600));
  refused.headers().set("Retry-After", "1");
  ScriptedCodexStream upstream({refused, closedSse(retryFixtureSuccess("resp_chat"))});
  const auto request = h::streamingRequest(key.token, "chat");
  AWAIT(result, deliverStreamRetryRequest(scope.logs, request));
  EXPECT_EQ(result.status, 200);
  EXPECT(rt::includes(result.body, "\"content\":\"finished\""));
  EXPECT(rt::includes(result.body, "data: [DONE]"));
  EXPECT(!rt::includes(result.body, "rate_limit_exceeded"));
  expectStreamRetryAttempts(scope.logs, {429, 200});
  ASSERT_EQ(upstream.attempts.size(), 2U);
  EXPECT(upstream.attempts[1].atMs - upstream.attempts[0].atMs >= 1000);
  EXPECT_EQ(rt::getNumber(result.terminal, "in_stream_retries"), 1);
  EXPECT_EQ(rt::getNumber(result.terminal, "total_tokens"), 7);
  const json::array failures = retryFailureEvents(result.terminal);
  ASSERT_EQ(failures.size(), 1U);
  EXPECT_EQ(rt::getString(failures[0], "type"), "response.failed");
  EXPECT_EQ(rt::getString(failures[0], "code"), "rate_limit_exceeded");
  EXPECT_EQ(rt::getString(failures[0], "error_type"), "rate_limit_error");
  expectWindow(key, 1, 0);
}

ASYNC_TEST("Client abort during the Codex stream retry wait releases the pending kernel reservation") {
  Scope scope;
  AWAIT(key, h::prepareApiKeyInference("7", "codex-stream-retry-abort", 5));
  auto seeding = h::seedKernelTestPublicKey();
  co_await std::move(seeding);
  h::seedKernelDefaultLimit(1);
  const std::string org = "stream-retry-abort-org";
  const auto window = h::kernelOrgWindowKey(org);
  const auto request = h::streamingRequest(key.token, "responses");
  AWAIT(signedRequest, h::withKernelTestToken(request, key.token, org, "stream-retry"));
  const rt::AbortController client;
  const rt::Request cancellable(signedRequest, rt::RequestInit{.signal = client.signal()});
  constexpr double retryAfter = 2345;
  ScriptedCodexStream upstream({closedSse(retryFixtureRateLimit(retryAfter))});
  const auto timer = rt::globalSetTimeout();
  const auto restore = rt::scopeExit([timer] { rt::globalSetTimeout() = timer; });
  const auto sleeping = h::deferred();
  rt::globalSetTimeout() = [timer, sleeping](std::function<void()> callback, double ms) {
    auto handle = timer(std::move(callback), ms);
    if (ms == retryAfter) sleeping.resolve();
    return handle;
  };

  const rt::Promise<rt::Response> pending(h::handler(cancellable));
  const auto watchdog = rt::setTimeout([sleeping] { sleeping.reject(rt::makeReason(rt::JsError("retry wait was never scheduled"))); }, 100);
  const auto clearWatchdog = rt::scopeExit([watchdog] { rt::clearTimeout(watchdog); });
  co_await sleeping.promise.wait();
  rt::clearTimeout(watchdog);
  EXPECT_EQ(rt::getNumber(value(window), "reserved_requests"), 1);
  client.abort(rt::makeReason(rt::DOMException("fixture downstream disconnect", "AbortError")));
  const auto response = co_await pending.wait();
  EXPECT_EQ(response.status(), 499);
  auto reading = response.text();
  (void)co_await std::move(reading);
  co_await rt::sleep(retryAfter + 1);
  expectStreamRetryAttempts(scope.logs, {429});
  EXPECT_EQ(upstream.attempts.size(), 1U);
  EXPECT_EQ(rt::getNumber(value(window), "reserved_requests"), 0);
  EXPECT_EQ(rt::getNumber(value(window), "usage_requests"), 0);
  expectWindow(key, 1, 0);
  const auto terminals = terminalPayloads(scope.logs);
  ASSERT_EQ(terminals.size(), 1U);
  EXPECT_EQ(rt::getNumber(terminals[0], "in_stream_retries"), 0);
  EXPECT_EQ(rt::getString(terminals[0], "stream_terminal_type"), "cancelled");
}

ASYNC_TEST("The original request deadline interrupts Codex stream retries") {
  Scope scope;
  AWAIT(key, h::prepareApiKeyInference("8", "codex-stream-retry-deadline", 5));
  h::setStreamFirstEventDeadlineMsForTest(1000);
  const auto restore = rt::scopeExit([] { h::setStreamFirstEventDeadlineMsForTest(std::nullopt); });
  ScriptedCodexStream upstream({
    closedSse(retryFixturePreamble("resp_first") + retryFixtureFailure("server_error")),
    closedSse(retryFixtureRateLimit(3000)),
  });
  const auto request = h::streamingRequest(key.token, "responses");
  AWAIT(result, deliverStreamRetryRequest(scope.logs, request));
  EXPECT_EQ(result.status, 504);
  EXPECT_EQ(rt::getString(*rt::get(rt::jsonParse(result.body), "error"), "code"), "gateway_timeout");
  expectStreamRetryAttempts(scope.logs, {502, 429});
  EXPECT_EQ(upstream.attempts.size(), 2U);
  EXPECT_EQ(rt::getNumber(result.terminal, "in_stream_retries"), 1);
  EXPECT_EQ(rt::getString(result.terminal, "stream_terminal_type"), "deadline");
  EXPECT_EQ(retryFailureEvents(result.terminal).size(), 2U);
  EXPECT(rt::get(result.terminal, "first_semantic_commitment_ms")->is_null());
  expectWindow(key, 1, 0);
}
