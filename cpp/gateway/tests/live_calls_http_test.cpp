// Links: tests/live_calls_http_test.cpp
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
// Port of tests/live-calls-http.test.ts through the real handler and createServeHandler on ephemeral loopback HTTP.
// All upstream fetches and credential refreshes use the fixture's synthetic responses; no real credentials are read.
#include "tests/helpers/responses_conformance_client.hpp"
#include "tests/helpers/live_relay_harness.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/live/calls.hpp"
#include "uos/handler/serve_handler.hpp"
#include "uos/handler/index.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"
#include "uos/sentinel/replay_read.hpp"
#include "tests/helpers/loopback_http.hpp"
#include "uos/rt/test_async.hpp"
namespace rt=uos::rt;namespace json=uos::json;namespace harness=uos::tests::helpers::live_relay_harness;namespace live=uos::live;
namespace {
namespace wire=uos::runtime::http;
using uos::tests::helpers::loopback_http::ScopedLoopbackFetch;
const std::string RESET_PATH = "/backend-api/wham/rate-limit-reset-credits";
struct HttpFixture;
HttpFixture* currentFixture=nullptr;
struct HttpFixture : harness::Fixture {
  ScopedLoopbackFetch network;
  std::optional<wire::HttpServer> gateway,upstream,refreshServer;
  std::string origin,upstreamOrigin;
  std::vector<std::string> methods;
  bool stopped=false;
  bool routeResetProvider = false;
  rt::Headers lastIncomingHeaders;
  HttpFixture(){currentFixture=this;}
  ~HttpFixture(){currentFixture=nullptr;}
  rt::Task<void> start(){
    if(gateway)co_return;
    uos::tests::helpers::responses_conformance_client::stageGatewayAssets();
    auto keySeed=seedApiKey();co_await std::move(keySeed);
    wire::ServeHttpOptions options;options.hostname="127.0.0.1";options.port=0;options.onListen=[](const auto&){};
    refreshServer=wire::serveHttp(options,[this](rt::Request request,wire::ServeHandlerInfo)->rt::Task<rt::Response>{
      auto reading=request.json();const auto body=co_await std::move(reading);const std::string token(rt::getString(body,"refresh_token").value_or(""));refreshTokens.push_back(token);
      auto responseTask=refresh(token);auto response=co_await std::move(responseTask);co_return response;
    });
    const auto refreshAddress=co_await refreshServer->ready.wait();const auto refreshUrl="http://127.0.0.1:"+rt::numberToString(refreshAddress.port)+"/oauth/token";
    upstream=wire::serveHttp(options,[this](rt::Request request,wire::ServeHandlerInfo)->rt::Task<rt::Response>{
      auto reading=request.text();const auto body=co_await std::move(reading);harness::UpstreamCall call{request.url(),request.headers(),body};calls.push_back(call);methods.push_back(request.method());
      auto responseTask=responder(call);auto response=co_await std::move(responseTask);co_return response;
    });
    const auto upstreamAddress=co_await upstream->ready.wait();upstreamOrigin="http://127.0.0.1:"+rt::numberToString(upstreamAddress.port);
    live::upstream::setLiveUpstreamBasesForTest({.callsBaseUrl=upstreamOrigin});
    fetch.reset();fetch=std::make_unique<rt::GlobalFetchOverride>(rt::Fetch([this, refreshUrl](rt::RequestInfo input,rt::RequestInit init)->rt::Task<rt::Response>{
      std::string url;if(const auto* text=std::get_if<std::string>(&input))url=*text;else if(const auto* value=std::get_if<rt::URL>(&input))url=value->href();else url=std::get<rt::Request>(input).url();
      if(url==uos::codex::auth::CODEX_REFRESH_TOKEN_URL)url=refreshUrl;
      if (routeResetProvider && (url == "https://chatgpt.com" + RESET_PATH || url == "https://chatgpt.com" + RESET_PATH + "/consume"))
        url = upstreamOrigin + rt::URL(url).pathname();
      if(rt::URL(url).hostname()!="127.0.0.1")throw rt::JsError("Unmapped external URL refused by loopback HTTP fixture");
      auto responseTask=rt::defaultFetch()(url,init);auto response=co_await std::move(responseTask);co_return response;
    }));
    uos::handler::serve_handler::DeliveryAwareHandler observed=[this](rt::Request request,uos::handler::http::RequestDeliveryInfo delivery){lastIncomingHeaders=request.headers();return uos::handler::index::handler(request,delivery);};
    gateway=wire::serveHttp(options,uos::handler::serve_handler::createServeHandler(observed));
    const auto address=co_await gateway->ready.wait();origin="http://127.0.0.1:"+rt::numberToString(address.port);
  }
  rt::Task<rt::Response> request(rt::Request req){
    auto starting=start();co_await std::move(starting);rt::RequestInit init;init.method=req.method();init.headers=req.headers();auto reading=req.text();init.body=co_await std::move(reading);
    auto responseTask=rt::fetch(origin+rt::URL(req.url()).pathname(),init);auto response=co_await std::move(responseTask);co_return response;
  }
  rt::Task<void> stopUpstream(){auto starting=start();co_await std::move(starting);const auto stopping=upstream->shutdown();co_await stopping.wait();}
  rt::Task<void> close(){
    if(stopped)co_return;
    stopped=true;
    std::vector<rt::Promise<void>> stops;if(gateway)stops.push_back(gateway->shutdown());if(upstream)stops.push_back(upstream->shutdown());if(refreshServer)stops.push_back(refreshServer->shutdown());
    const auto joined=rt::all(std::move(stops));co_await joined.wait();auto settling=settle();co_await std::move(settling);
  }
};
rt::Task<rt::Response> create(rt::Request req){ASSERT(currentFixture);auto task=currentFixture->request(req);auto response=co_await std::move(task);co_return response;}
rt::Task<json::value> errorOf(rt::Response response){auto task=response.json();const auto value=co_await std::move(task);co_return *rt::get(value,"error");}

rt::Task<void> seedWeeklyCapacity(const HttpFixture& fixture, std::string accountId, double slot, bool allowOverage) {
  namespace codex = uos::codex;
  const auto hash = co_await uos::utils::sha256Hex(accountId);
  co_await codex::overage_settings::writeOverageUsage(fixture.kv, hash, allowOverage);
  codex::overage_settings::resetOverageUsageCacheForTest();
  const double now = rt::dateNow();
  const codex::routing_state::CodexCapacityRoutingObservationInput weekly{
    .slot = slot, .account_id = std::move(accountId), .state = "available", .source_observed_at_ms = now, .snapshot_at_ms = now,
    .windows = {codex::routing_state::CodexCapacityRoutingWindow{18'000, 24, now + 3'600'000},
                codex::routing_state::CodexCapacityRoutingWindow{604'800, 100, now + 6 * 86'400'000}},
  };
  const std::vector observations{weekly};
  co_await codex::capacity_routing::recordCodexCapacityRoutingObservations(observations, now);
}
}
ASYNC_TEST("POST /v1/live answers 413 for an oversize body with a declared Content-Length"){
  HttpFixture fixture;auto seed=fixture.seed();co_await std::move(seed);
  auto req=harness::callRequest(std::string(1'048'577,'x'));auto task=create(req);auto response=co_await std::move(task);EXPECT_EQ(response.status(),413);
  auto err=errorOf(response);const auto error=co_await std::move(err);EXPECT_EQ(rt::getString(error,"message").value(),"Live call creation bodies must be no larger than 1048576 bytes.");EXPECT_EQ(fixture.calls.size(),0U);EXPECT_EQ(fixture.lastIncomingHeaders.get("content-length"),std::optional<std::string>("1048577"));EXPECT(!fixture.lastIncomingHeaders.has("transfer-encoding"));auto settle=fixture.close();co_await std::move(settle);
}
ASYNC_TEST("POST /v1/live answers 413 for an oversize streamed body that declares no length"){
  HttpFixture fixture;auto seed=fixture.seed();co_await std::move(seed);
  rt::UnderlyingSource<rt::ByteChunk> source;source.start=[](auto controller){controller.enqueue(rt::Bytes(1'048'578,'x'));controller.close();};
  rt::RequestInit init;init.method="POST";init.headers=rt::Headers{{"content-type","multipart/form-data; boundary=fixture"},{"authorization","Bearer "+std::string(harness::CLIENT_TOKEN)},{"transfer-encoding","chunked"}};init.body=rt::ByteStream(source);
  rt::Request req("http://fixture.invalid/v1/live",init);auto task=create(req);auto response=co_await std::move(task);EXPECT_EQ(response.status(),413);auto err=errorOf(response);const auto error=co_await std::move(err);EXPECT_EQ(rt::getString(error,"message").value(),"Live call creation bodies must be no larger than 1048576 bytes.");EXPECT_EQ(fixture.calls.size(),0U);EXPECT(!fixture.lastIncomingHeaders.has("content-length"));EXPECT_EQ(fixture.lastIncomingHeaders.get("transfer-encoding"),std::optional<std::string>("chunked"));auto settle=fixture.close();co_await std::move(settle);
}
ASYNC_TEST("POST /v1/live relays the multipart SDP offer as the backend JSON call and maps the call to its account"){
  HttpFixture fixture;auto seed=fixture.seed();co_await std::move(seed);
  rt::Headers headers{{"openai-alpha","quicksilver=v2"},{"x-session-id","live-calls-http-session"},{"session-id","live-calls-http-session"},{"thread-id","live-calls-http-thread"},{"x-codex-turn-metadata","{\"thread_source\":\"user\"}"},{"x-oai-attestation","live-calls-http-attestation"}};
  const auto req=harness::callRequest(harness::realtimeCallBody(std::string(harness::SDP_OFFER),harness::session()),headers);auto task=create(req);auto response=co_await std::move(task);EXPECT_EQ(response.status(),201);
  auto reading=response.text();const auto body=co_await std::move(reading);EXPECT_EQ(body,std::string(harness::SDP_ANSWER));EXPECT(response.headers().has("x-uos-request-id"));EXPECT_EQ(response.headers().get("access-control-allow-origin"),std::optional<std::string>("*"));EXPECT_EQ(response.headers().get("location"),std::optional<std::string>("/v1/live/"+std::string(harness::CALL_ID)));
  EXPECT_EQ(fixture.calls.size(),1U);if(!fixture.calls.empty()){
    const auto& upstream=fixture.calls[0];EXPECT_EQ(fixture.methods[0],"POST");EXPECT_EQ(upstream.url,fixture.upstreamOrigin+"/realtime/calls?intent=quicksilver&architecture=avas");EXPECT_EQ(upstream.headers.get("content-type"),std::optional<std::string>("application/json"));EXPECT_EQ(upstream.headers.get("chatgpt-account-id"),std::optional<std::string>(std::string(harness::ACCOUNT_A)));EXPECT_EQ(upstream.headers.get("authorization"),std::optional<std::string>("Bearer "+std::string(harness::ACCOUNT_A)+"-access-token"));EXPECT(upstream.headers.get("authorization")!=std::optional<std::string>("Bearer "+std::string(harness::CLIENT_TOKEN)));EXPECT_EQ(upstream.headers.get("originator"),std::optional<std::string>("codex_cli_rs"));EXPECT(rt::startsWith(upstream.headers.get("user-agent").value_or(""),"codex_cli_rs/"));
    for(const auto& [name,value]:headers.entries())EXPECT_EQ(upstream.headers.get(name),std::optional<std::string>(value));
    const auto payload=rt::jsonParse(upstream.bodyText);EXPECT_EQ(rt::getString(payload,"sdp").value(),harness::SDP_OFFER);EXPECT(rt::deepEqual(*rt::get(payload,"session"),harness::session()));EXPECT_EQ(payload.get_object().size(),2U);
  }
  auto lookup=live::upstream::readLiveCallMapping(std::string(harness::CALL_ID));const auto mapping=co_await std::move(lookup);EXPECT(mapping);if(mapping){EXPECT_EQ(mapping->accountId,harness::ACCOUNT_A);EXPECT_EQ(mapping->principalId,std::optional<std::string>(std::string(harness::PRINCIPAL)));}auto settle=fixture.close();co_await std::move(settle);
}
ASYNC_TEST("POST /v1/live answers 401 without client auth and never reaches upstream"){
  HttpFixture fixture;auto seed=fixture.seed();co_await std::move(seed);auto req=harness::callRequest();req.headers().delete_("authorization");
  auto task=create(req);auto response=co_await std::move(task);auto reading=errorOf(response);const auto error=co_await std::move(reading);EXPECT_EQ(response.status(),401);EXPECT_EQ(rt::getString(error,"code").value(),"invalid_api_key");EXPECT_EQ(fixture.calls.size(),0U);auto settle=fixture.close();co_await std::move(settle);
}
ASYNC_TEST("POST /v1/live rejects a non-multipart body and a multipart body without a session part"){
  HttpFixture fixture;auto seed=fixture.seed();co_await std::move(seed);
  auto jsonReq=harness::callRequest("{}",rt::Headers{{"content-type","application/json"}});auto a=create(jsonReq);auto jsonResponse=co_await std::move(a);EXPECT_EQ(jsonResponse.status(),400);auto jsonReading=errorOf(jsonResponse);const auto jsonError=co_await std::move(jsonReading);EXPECT_EQ(rt::getString(jsonError,"type"),std::optional<std::string_view>("invalid_request_error"));
  const std::string missing="--"+std::string(harness::BOUNDARY)+"\r\nContent-Disposition: form-data; name=\"sdp\"\r\n\r\nv=0\r\n--"+std::string(harness::BOUNDARY)+"--\r\n";
  auto b=create(harness::callRequest(missing));auto response=co_await std::move(b);EXPECT_EQ(response.status(),400);auto read=errorOf(response);const auto error=co_await std::move(read);EXPECT_EQ(rt::getString(error,"param").value(),"session");
  const auto bad=harness::realtimeCallBody(std::string(harness::SDP_OFFER),json::array());auto c=create(harness::callRequest(bad));const auto invalid=co_await std::move(c);EXPECT_EQ(invalid.status(),400);const auto invalidString=harness::realtimeCallBody(std::string(harness::SDP_OFFER),json::value("not-json"));auto invalidStringTask=create(harness::callRequest(invalidString));auto invalidStringResponse=co_await std::move(invalidStringTask);EXPECT_EQ(invalidStringResponse.status(),400);EXPECT_EQ(fixture.calls.size(),0U);auto settle=fixture.close();co_await std::move(settle);
}
ASYNC_TEST("POST /v1/live answers 502 when the upstream call-creation route is unreachable"){
  HttpFixture fixture;auto seed=fixture.seed();co_await std::move(seed);auto stoppingUpstream=fixture.stopUpstream();co_await std::move(stoppingUpstream);
  auto task=create(harness::callRequest());auto response=co_await std::move(task);EXPECT_EQ(response.status(),502);auto read=errorOf(response);const auto error=co_await std::move(read);EXPECT_EQ(rt::getString(error,"code").value(),"codex_upstream_unreachable");auto settle=fixture.close();co_await std::move(settle);
}
ASYNC_TEST("failed live call retains only the authenticated AI session input") {
  HttpFixture fixture;
  const rt::Bytes encryptionKey(32, 0x51);
  const rt::env::ScopedOverride replayKey({{"SENTINEL_REPLAY_KEY", uos::utils::base64UrlEncode(encryptionKey)}});
  auto seed = fixture.seed();
  co_await std::move(seed);
  fixture.responder = [](harness::UpstreamCall) {
    return rt::fulfilledTask(rt::Response("provider-secret-fixture", rt::ResponseInit{.status = 502}));
  };
  const json::object session{{"model", "realtime-model"}, {"instructions", "Answer the synthetic question"},
    {"client_secret", "session-secret-fixture"}, {"tools", json::array{json::object{{"authorization", "tool-secret-fixture"}}}}};
  const auto req = harness::callRequest(harness::realtimeCallBody("v=0\r\na=ice-pwd:rtc-secret-fixture\r\n", session));
  auto creating = create(req);
  const auto response = co_await std::move(creating);
  ASSERT_EQ(response.status(), 502);
  const auto requestId = response.headers().get("x-uos-request-id");
  ASSERT(requestId);
  co_await response.text();
  auto settling = fixture.close();
  co_await std::move(settling);

  auto lookup = uos::sentinel::replay_read::listEncryptedSentinelReplaysByRequestId(*fixture.kv, *requestId);
  const auto evidence = co_await std::move(lookup);
  ASSERT_EQ(evidence.captures.size(), 1U);
  auto decrypting = uos::sentinel::replay_read::decryptExportedSentinelReplay(evidence.captures.front(), encryptionKey);
  const auto plaintext = co_await std::move(decrypting);
  EXPECT_EQ(plaintext.request_id, *requestId);
  EXPECT_EQ(plaintext.endpoint, "/v1/live");
  EXPECT_EQ(plaintext.observation.status, 502);
  EXPECT(plaintext.compatibility_headers.empty());
  ASSERT(plaintext.body);
  const auto payload = rt::decodeUtf8(*plaintext.body);
  EXPECT_EQ(payload, R"({"schema":"uos-live-failure-projection-v1","session":{"model":"realtime-model","instructions":"Answer the synthetic question"}})");
  EXPECT(!plaintext.client_observation.terminal_body_base64);
}

ASYNC_TEST("ordinary failures journal only metadata and use the returned ID") {
  HttpFixture fixture;
  uos::tests::helpers::responses_conformance_client::stageGatewayAssets();
  const rt::console::Capture logs({rt::console::Method::info});
  rt::RequestInit init;
  init.method = "DELETE";
  init.headers = rt::Headers{{"authorization", "Bearer header-secret"}, {"cookie", "session=cookie-secret"}};
  auto first = uos::handler::index::handler(rt::Request("http://fixture.invalid/path-secret?secret=query-secret", init));
  const auto unknown = co_await std::move(first);
  EXPECT_EQ(unknown.status(), 404);
  const auto unknownId = unknown.headers().get("x-uos-request-id");
  ASSERT(unknownId);
  auto second = uos::handler::index::handler(rt::Request("http://fixture.invalid/health/providers?secret=query-secret"));
  const auto health = co_await std::move(second);
  EXPECT(health.status() >= 400);
  const auto healthId = health.headers().get("x-uos-request-id");
  ASSERT(healthId);

  std::vector<json::value> events;
  for (const auto& call : logs.calls()) {
    if (call.args.size() != 2 || call.args[0] != std::string_view("[ai.ubq.fi] request_failure")) continue;
    EXPECT(call.text.find("secret") == std::string::npos);
    events.push_back(rt::jsonParse(call.args[1].string()));
  }
  ASSERT_EQ(events.size(), 2U);
  EXPECT_EQ(rt::getString(events[0], "request_id").value(), *unknownId);
  EXPECT_EQ(rt::getString(events[0], "route").value(), "unmatched");
  EXPECT_EQ(rt::getString(events[0], "method").value(), "DELETE");
  EXPECT_EQ(rt::getString(events[1], "request_id").value(), *healthId);
  EXPECT_EQ(rt::getString(events[1], "route").value(), "health");
  EXPECT_EQ(rt::getString(events[1], "method").value(), "GET");
  for (const auto& event : events) EXPECT_EQ(rt::getBool(event, "body_present"), std::optional<bool>(false));
}

ASYNC_TEST("POST /v1/live retries a credential failure once with the next eligible account"){
  HttpFixture fixture;auto seed=fixture.seed();co_await std::move(seed);fixture.responder=[](harness::UpstreamCall call){return rt::fulfilledTask(call.headers.get("chatgpt-account-id")==std::optional<std::string>(std::string(harness::ACCOUNT_A))?rt::Response("{\"detail\":\"credential expired\"}",rt::ResponseInit{.status=401}):harness::acceptedAnswer());};
  auto task=create(harness::callRequest());auto response=co_await std::move(task);EXPECT_EQ(response.status(),201);auto bodyTask=response.text();const auto body=co_await std::move(bodyTask);EXPECT_EQ(body,harness::SDP_ANSWER);EXPECT_EQ(response.headers().get("location"),std::optional<std::string>("/v1/live/"+std::string(harness::CALL_ID)));EXPECT_EQ(fixture.calls.size(),2U);if(fixture.calls.size()==2){EXPECT_EQ(fixture.calls[0].headers.get("chatgpt-account-id"),std::optional<std::string>(std::string(harness::ACCOUNT_A)));EXPECT_EQ(fixture.calls[1].headers.get("chatgpt-account-id"),std::optional<std::string>(std::string(harness::ACCOUNT_B)));}
  auto read=live::upstream::readLiveCallMapping(std::string(harness::CALL_ID));const auto mapping=co_await std::move(read);EXPECT(mapping);if(mapping){EXPECT_EQ(mapping->accountId,harness::ACCOUNT_B);EXPECT_EQ(mapping->principalId,std::optional<std::string>(std::string(harness::PRINCIPAL)));}if(fixture.calls.size()==2){EXPECT_EQ(fixture.calls[1].headers.get("authorization"),std::optional<std::string>("Bearer "+std::string(harness::ACCOUNT_B)+"-access-token"));EXPECT_EQ(rt::URL(fixture.calls[1].url).pathname(),"/realtime/calls");}auto settle=fixture.close();co_await std::move(settle);
}
ASYNC_TEST("POST /v1/live relays an upstream credential failure verbatim when no other account is eligible"){
  HttpFixture fixture;const std::vector<uos::types::CodexAuthState> accounts{harness::codexAccount(std::string(harness::ACCOUNT_A))};auto seed=fixture.setAuthPool(accounts);co_await std::move(seed);fixture.responder=[](harness::UpstreamCall){return rt::fulfilledTask(rt::Response("{\"detail\":\"credential expired\"}",rt::ResponseInit{.status=401}));};
  auto task=create(harness::callRequest());auto response=co_await std::move(task);EXPECT_EQ(response.status(),401);auto read=response.text();const auto body=co_await std::move(read);EXPECT_EQ(body,"{\"detail\":\"credential expired\"}");EXPECT_EQ(fixture.calls.size(),1U);auto settle=fixture.close();co_await std::move(settle);
}
ASYNC_TEST("POST /v1/live refreshes the selected account's expired token before dispatch and leaves other accounts untouched"){
  HttpFixture fixture;const auto unrelated=harness::codexAccount(std::string(harness::ACCOUNT_B));const std::vector<uos::types::CodexAuthState> accounts{harness::expiredCodexAccount(std::string(harness::ACCOUNT_A)),unrelated};auto seed=fixture.setAuthPool(accounts);co_await std::move(seed);
  auto task=create(harness::callRequest());auto response=co_await std::move(task);EXPECT_EQ(response.status(),201);auto bodyTask=response.text();const auto body=co_await std::move(bodyTask);EXPECT_EQ(body,harness::SDP_ANSWER);EXPECT_EQ(fixture.calls[0].headers.get("chatgpt-account-id"),std::optional<std::string>(std::string(harness::ACCOUNT_A)));EXPECT_EQ(fixture.refreshTokens.size(),1U);EXPECT_EQ(fixture.refreshTokens[0],std::string(harness::ACCOUNT_A)+"-refresh-token");EXPECT_EQ(fixture.calls[0].headers.get("authorization"),std::optional<std::string>("Bearer live-calls-refreshed-access-token"));
  auto poolRead=fixture.kv->get(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY);const auto row=co_await std::move(poolRead);const auto pool=json::value_to<uos::types::CodexAuthPoolState>(row.value.toJson());EXPECT_EQ(pool.accounts[1],unrelated);EXPECT_EQ(pool.accounts[0].access_token,"live-calls-refreshed-access-token");EXPECT_EQ(pool.accounts[0].refresh_token,"live-calls-refreshed-refresh-token");auto mappingTask=live::upstream::readLiveCallMapping(std::string(harness::CALL_ID));const auto mapping=co_await std::move(mappingTask);ASSERT(mapping);EXPECT_EQ(mapping->accountId,harness::ACCOUNT_A);EXPECT_EQ(mapping->principalId,std::optional<std::string>(std::string(harness::PRINCIPAL)));auto settle=fixture.close();co_await std::move(settle);
}
ASYNC_TEST("POST /v1/live fails closed when the selected account disappears while its token is refreshing"){
  HttpFixture fixture;const std::vector<uos::types::CodexAuthState> accounts{harness::expiredCodexAccount(std::string(harness::ACCOUNT_A)),harness::codexAccount(std::string(harness::ACCOUNT_B))};auto seed=fixture.setAuthPool(accounts);co_await std::move(seed);
  fixture.refresh=[&fixture](std::string)->rt::Task<rt::Response>{const std::vector<uos::types::CodexAuthState> remaining{harness::codexAccount(std::string(harness::ACCOUNT_B))};auto task=fixture.setAuthPool(remaining);co_await std::move(task);co_return rt::Response::json(json::object{{"access_token","replacement"},{"refresh_token","replacement-refresh"}});};
  auto task=create(harness::callRequest());auto response=co_await std::move(task);EXPECT_EQ(response.status(),503);auto errorTask=errorOf(response);const auto error=co_await std::move(errorTask);EXPECT_EQ(rt::getString(error,"code"),std::optional<std::string_view>("codex_auth_missing"));EXPECT_EQ(fixture.calls.size(),0U);EXPECT_EQ(fixture.refreshTokens.size(),1U);auto settle=fixture.close();co_await std::move(settle);
}

ASYNC_TEST("POST /v1/live honors explicit reset opt-out and independently checks overage consent") {
  namespace codex = uos::codex;
  for (const bool allowOverage : {false, true}) {
    HttpFixture fixture;
    const std::string accountId(harness::ACCOUNT_A);
    std::vector accounts{harness::codexAccount(accountId)};
    co_await fixture.setAuthPool(std::move(accounts));
    const auto hash = co_await uos::utils::sha256Hex(accountId);
    const rt::KvValue resetConsent(json::object{{"enabled", false}});
    co_await fixture.kv->set(codex::reset_settings::codexResetUsageKey(hash), resetConsent);
    co_await seedWeeklyCapacity(fixture, accountId, 0, allowOverage);

    const auto response = co_await create(harness::callRequest());
    EXPECT_EQ(response.status(), allowOverage ? 201 : 429);
    EXPECT_EQ(fixture.calls.size(), allowOverage ? 1U : 0U);
    if (allowOverage && !fixture.calls.empty()) {
      EXPECT_EQ(rt::URL(fixture.calls.front().url).pathname(), "/realtime/calls");
      EXPECT_EQ(fixture.calls.front().headers.get("chatgpt-account-id"), std::optional<std::string>(accountId));
      EXPECT_EQ(response.headers().get("location"), std::optional<std::string>("/v1/live/" + std::string(harness::CALL_ID)));
    }
    co_await fixture.close();
  }
}

ASYNC_TEST("POST /v1/live consumes one weekly reset before dispatching the actual call") {
  HttpFixture fixture;
  fixture.routeResetProvider = true;
  const std::string accountId(harness::ACCOUNT_A);
  std::vector accounts{harness::codexAccount(accountId)};
  co_await fixture.setAuthPool(std::move(accounts));
  co_await seedWeeklyCapacity(fixture, accountId, 0, true);
  fixture.responder = [](harness::UpstreamCall call) {
    const auto path = rt::URL(call.url).pathname();
    if (path == RESET_PATH) {
      const json::object credit{{"id", "live-weekly-credit"}, {"status", "available"}, {"reset_type", "codex_rate_limits"}, {"expires_at", nullptr}};
      return rt::fulfilledTask(rt::Response::json(json::object{{"available_count", 1}, {"credits", json::array{credit}}}));
    }
    if (path == RESET_PATH + "/consume")
      return rt::fulfilledTask(rt::Response::json(json::object{{"code", "reset"}, {"windows_reset", 1}}));
    return rt::fulfilledTask(harness::acceptedAnswer());
  };
  const auto response = co_await create(harness::callRequest());
  EXPECT_EQ(response.status(), 201);
  EXPECT_EQ(response.headers().get("location"), std::optional<std::string>("/v1/live/" + std::string(harness::CALL_ID)));
  const std::vector<std::string> methods{"GET", "POST", "POST"};
  EXPECT_EQ(fixture.methods, methods);
  std::vector<std::string> paths;
  for (const auto& call : fixture.calls) {
    paths.push_back(rt::URL(call.url).pathname());
    EXPECT_EQ(call.headers.get("chatgpt-account-id"), std::optional<std::string>(accountId));
  }
  const std::vector<std::string> expected{RESET_PATH, RESET_PATH + "/consume", "/realtime/calls"};
  EXPECT_EQ(paths, expected);
  co_await fixture.close();
}

ASYNC_TEST("POST /v1/live retries a current invalid account with consented overage without redeeming a reset") {
  HttpFixture fixture;
  fixture.routeResetProvider = true;
  const std::string exhausted(harness::ACCOUNT_A);
  const std::string invalid(harness::ACCOUNT_B);
  std::vector accounts{harness::codexAccount(invalid), harness::codexAccount(exhausted)};
  co_await fixture.setAuthPool(std::move(accounts));
  co_await seedWeeklyCapacity(fixture, exhausted, 1, true);
  fixture.responder = [invalid](harness::UpstreamCall call) {
    if (call.headers.get("chatgpt-account-id") == invalid)
      return rt::fulfilledTask(rt::Response("stale realtime credentials", rt::ResponseInit{.status = 401}));
    return rt::fulfilledTask(harness::acceptedAnswer());
  };
  const auto response = co_await create(harness::callRequest());
  EXPECT_EQ(response.status(), 201);
  ASSERT(fixture.calls.size() == 2);
  EXPECT_EQ(fixture.calls[0].headers.get("chatgpt-account-id"), std::optional<std::string>(invalid));
  EXPECT_EQ(fixture.calls[1].headers.get("chatgpt-account-id"), std::optional<std::string>(exhausted));
  for (const auto& call : fixture.calls) EXPECT_EQ(rt::URL(call.url).pathname(), "/realtime/calls");
  co_await fixture.close();
}

ASYNC_TEST("live call handling matches the Bun differential response bytes"){
  const auto vectors=rt::jsonParse(R"oracle([{"scenario":"success","status":201,"body":"sdp-answer","location":"/v1/live/rtc_oracle"},{"scenario":"retry401","status":201,"body":"sdp-answer","location":"/v1/live/rtc_oracle"},{"scenario":"no-retry401","status":401,"body":"{\"detail\":\"credential expired\"}","location":null},{"scenario":"unreachable","status":502,"body":"{\"error\":{\"message\":\"Codex upstream request failed: upstream unreachable.\",\"type\":\"invalid_request_error\",\"code\":\"codex_upstream_unreachable\"}}","location":null},{"scenario":"missing-session","status":400,"body":"{\"error\":{\"message\":\"Live call creation requires a `session` part.\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\",\"param\":\"session\"}}","location":null},{"scenario":"bad-type","status":400,"body":"{\"error\":{\"message\":\"Live call creation requires multipart/form-data with a declared boundary.\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\"}}","location":null}])oracle");
  for(const auto& expected:vectors.get_array()){
    harness::Fixture fixture;const std::string scenario(*rt::getString(expected,"scenario"));
    std::vector<uos::types::CodexAuthState> accounts{harness::codexAccount(std::string(harness::ACCOUNT_A))};if(scenario!="no-retry401")accounts.push_back(harness::codexAccount(std::string(harness::ACCOUNT_B)));auto seeding=fixture.setAuthPool(accounts);co_await std::move(seeding);
    fixture.responder=[&fixture,scenario](harness::UpstreamCall)->rt::Task<rt::Response>{
      if(scenario=="unreachable")throw rt::JsError("unreachable");
      if((scenario=="retry401"&&fixture.calls.size()==1)||scenario=="no-retry401")co_return rt::Response("{\"detail\":\"credential expired\"}",rt::ResponseInit{.status=401});
      co_return rt::Response("sdp-answer",rt::ResponseInit{.status=201,.headers=rt::Headers{{"Location","/v1/realtime/calls/rtc_oracle"}}});
    };
    std::string body;
    if(scenario=="missing-session")body="--"+std::string(harness::BOUNDARY)+"\r\nContent-Disposition: form-data; name=\"sdp\"\r\n\r\nv=0\r\n--"+std::string(harness::BOUNDARY)+"--\r\n";
    else body=harness::realtimeCallBody("v=0",json::object{{"model","gpt-live-1-codex"}});
    rt::Headers headers;if(scenario=="bad-type")headers.set("content-type","application/json");const auto req=harness::callRequest(body,headers);auto task=live::calls::handleLiveCallCreate(req,std::string(harness::PRINCIPAL));auto response=co_await std::move(task);
    EXPECT_EQ(response.status(),rt::getNumber(expected,"status").value());auto reading=response.text();const auto observed=co_await std::move(reading);EXPECT_EQ(observed,std::string(*rt::getString(expected,"body")));
    const auto* location=rt::get(expected,"location");EXPECT_EQ(response.headers().get("location"),location->is_string()?std::optional<std::string>(std::string(location->get_string())):std::nullopt);auto settling=fixture.settle();co_await std::move(settling);
  }
}
