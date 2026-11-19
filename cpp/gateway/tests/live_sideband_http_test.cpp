// Links: tests/live_sideband_http_test.cpp
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
// Port of tests/live-sideband-http.test.ts through final handler authentication and real ephemeral loopback upgrades.
#include "tests/helpers/responses_conformance_client.hpp"
#include "tests/helpers/live_relay_harness.hpp"
#include "uos/live/sideband.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/url.hpp"
#include "uos/rt/websocket.hpp"
#include "uos/runtime/http.hpp"
#include "uos/handler/serve_handler.hpp"
#include "tests/helpers/loopback_http.hpp"
#include <array>
#include <cstdint>
namespace rt=uos::rt;namespace json=uos::json;namespace harness=uos::tests::helpers::live_relay_harness;namespace live=uos::live;namespace ws=rt::ws;namespace http=uos::runtime::http;namespace upgrade=uos::runtime::http_upgrade;
namespace {
using Socket=std::shared_ptr<ws::WebSocket>;
rt::Request joinRequest(bool upgradeHeader=true){rt::RequestInit init;if(upgradeHeader)init.headers=rt::Headers{{"upgrade","websocket"}};return rt::Request("http://fixture.invalid/v1/live/"+std::string(harness::CALL_ID),init);}
rt::Task<double> statusOf(harness::Fixture& fixture,std::string callId,std::string principal,bool websocket=true){
  uos::tests::helpers::responses_conformance_client::stageGatewayAssets();
  auto keySeed=fixture.seedApiKey();co_await std::move(keySeed);
  uos::tests::helpers::loopback_http::ScopedLoopbackFetch network;
  std::string token(harness::CLIENT_TOKEN);
  if(principal!=""&&principal!=harness::PRINCIPAL){
    token="u_"+std::string(64,'e');const auto primaryHash=co_await uos::utils::sha256Base64Url(std::string(harness::CLIENT_TOKEN));const auto otherHash=co_await uos::utils::sha256Base64Url(token);
    auto read=fixture.kv->get(rt::KvKey{"ubq_ai","api_keys","hash",primaryHash});const auto primary=co_await std::move(read);auto row=primary.value.toJson().as_object();row["id"]="other";
    auto readingKey=fixture.kv->get(rt::KvKey{"ubq_ai","api_keys","id","live-calls-http-key"});const auto primaryKey=co_await std::move(readingKey);auto otherKey=primaryKey.value.toJson().as_object();otherKey["id"]="other";otherKey["hash"]=otherHash;otherKey["prefix"]=token.substr(0,10);const rt::KvValue keyRow(otherKey);auto writingKey=fixture.kv->set(rt::KvKey{"ubq_ai","api_keys","id","other"},keyRow);co_await std::move(writingKey);
    const rt::KvValue hashRow(row);auto writing=fixture.kv->set(rt::KvKey{"ubq_ai","api_keys","hash",otherHash},hashRow);co_await std::move(writing);
  }
  int upstreamAttempts=0;http::ServeHttpOptions mockOptions;mockOptions.hostname="127.0.0.1";mockOptions.port=0;mockOptions.upgrade=[&upstreamAttempts](rt::Request,upgrade::UpgradeInfo){++upstreamAttempts;return rt::fulfilledTask<upgrade::UpgradeResult>(rt::Response("unexpected mock dial",rt::ResponseInit{.status=500}));};
  const auto mock=http::serveHttp(mockOptions,[](rt::Request,http::ServeHandlerInfo){return rt::fulfilledTask(rt::Response("mock",rt::ResponseInit{.status=500}));});const auto mockAddress=co_await mock.ready.wait();live::upstream::setLiveUpstreamBasesForTest({.callsBaseUrl="http://live-provider.invalid",.sidebandBaseUrl="ws://127.0.0.1:"+rt::numberToString(mockAddress.port)+"/v1/live"});
  http::ServeHttpOptions options;options.hostname="127.0.0.1";options.port=0;options.upgrade=uos::handler::serve_handler::createServeUpgradeHandler();
  const auto server=http::serveHttp(options,uos::handler::serve_handler::createServeHandler());const auto address=co_await server.ready.wait();
  const auto base="127.0.0.1:"+rt::numberToString(address.port)+"/v1/live/"+callId;
  double status=0;
  if(websocket){
    auto answered=rt::withResolvers<double>();ws::ClientOptions clientOptions;if(!principal.empty())clientOptions.headers={{"authorization","Bearer "+token}};
    const auto client=ws::WebSocket::create("ws://"+base,clientOptions);
    client->once("open",[client,resolve=answered.resolve]{resolve(101);client->terminate();});
    client->once("unexpected-response",[client,resolve=answered.resolve](const ws::EventArgs& event){resolve(event.response->statusCode);event.response->resume();client->terminate();});
    client->on("error",[reject=answered.reject](const ws::EventArgs& event){reject(event.error);});
    status=co_await answered.promise.wait();
  }else{
    rt::RequestInit init;if(!principal.empty())init.headers=rt::Headers{{"authorization","Bearer "+token}};auto fetching=rt::defaultFetch()("http://"+base,init);auto response=co_await std::move(fetching);status=response.status();auto reading=response.text();co_await std::move(reading);
  }
  const auto stopping=server.shutdown();co_await stopping.wait();const auto stoppingMock=mock.shutdown();co_await stoppingMock.wait();EXPECT_EQ(upstreamAttempts,0);auto settling=fixture.settle();co_await std::move(settling);co_return status;
}
struct SocketFixture : harness::Fixture {
  std::optional<http::HttpServer> gateway,upstream;
  std::string url;
  std::vector<Socket> upstreamSockets,clients;
  std::vector<rt::Headers> upstreamHeaders;
  std::vector<std::string> upstreamPaths;
  std::vector<std::string> messages;
  rt::Resolvers<Socket> joined=rt::withResolvers<Socket>();
  std::optional<rt::Resolvers<void>> frames;
  std::size_t wantedFrames=0;
  std::size_t clientFrameCount=0,wantedClientFrames=0;
  std::optional<rt::Resolvers<void>> clientFrames;
  rt::Resolvers<void> upstreamHandshake=rt::withResolvers<void>();
  std::optional<rt::Promise<void>> holdHandshake;
  double upstreamStatus=101;
  rt::Task<void> start(){
    uos::tests::helpers::responses_conformance_client::stageGatewayAssets();
    auto seeding=seed();co_await std::move(seeding);auto mapping=live::upstream::writeLiveCallAccount(std::string(harness::CALL_ID),std::string(harness::ACCOUNT_A),std::string(harness::PRINCIPAL));co_await std::move(mapping);
    http::ServeHttpOptions options;options.hostname="127.0.0.1";options.port=0;
    options.upgrade=[this](rt::Request request,upgrade::UpgradeInfo)->rt::Task<upgrade::UpgradeResult>{
      upstreamHandshake.resolve();
      if(holdHandshake)co_await holdHandshake->wait();
      if(upstreamStatus!=101)co_return rt::Response("ended",rt::ResponseInit{.status=upstreamStatus});
      upstreamPaths.push_back(rt::URL(request.url()).pathname());upstreamHeaders.push_back(request.headers());upgrade::WebSocketUpgrade accepted;accepted.maxPayloadBytes=live::upstream::LIVE_SIDEBAND_MAX_PAYLOAD_BYTES;
      accepted.accept=[this](Socket socket){upstreamSockets.push_back(socket);socket->on("message",[this](const ws::EventArgs& event){messages.push_back(rt::decodeUtf8(std::get<ws::Bytes>(event.data)));if(frames&&messages.size()>=wantedFrames)frames->resolve();});socket->on("error",[]{});joined.resolve(socket);};
      co_return accepted;
    };
    upstream=http::serveHttp(options,[](rt::Request,http::ServeHandlerInfo){return rt::fulfilledTask(rt::Response("upstream"));});
    const auto upstreamAddr=co_await upstream->ready.wait();live::upstream::setLiveUpstreamBasesForTest({.callsBaseUrl="http://live-provider.invalid",.sidebandBaseUrl="ws://127.0.0.1:"+rt::numberToString(upstreamAddr.port)+"/v1/live"});
    http::ServeHttpOptions gatewayOptions;gatewayOptions.hostname="127.0.0.1";gatewayOptions.port=0;
    const auto realUpgrade=uos::handler::serve_handler::createServeUpgradeHandler();
    gatewayOptions.upgrade=[this,realUpgrade](rt::Request req,upgrade::UpgradeInfo info)->rt::Task<upgrade::UpgradeResult>{
      auto preparing=realUpgrade(req,info);auto result=co_await std::move(preparing);
      if(auto* accepted=std::get_if<upgrade::WebSocketUpgrade>(&result)){
        const auto accept=accepted->accept;accepted->accept=[this,accept](Socket socket){socket->on("message",[this]{++clientFrameCount;if(clientFrames&&clientFrameCount>=wantedClientFrames)clientFrames->resolve();});accept(socket);};
      }
      co_return result;
    };
    gateway=http::serveHttp(gatewayOptions,uos::handler::serve_handler::createServeHandler());
    const auto addr=co_await gateway->ready.wait();url="ws://127.0.0.1:"+rt::numberToString(addr.port)+"/v1/live/"+std::string(harness::CALL_ID);
  }
  rt::Task<Socket> connect(){
    auto opened=rt::withResolvers<Socket>();ws::ClientOptions options;options.headers={{"authorization","Bearer "+std::string(harness::CLIENT_TOKEN)}};const auto client=ws::WebSocket::create(url,options);clients.push_back(client);
    client->once("open",[resolve=opened.resolve,client]{resolve(client);});client->on("error",[reject=opened.reject](const ws::EventArgs& event){reject(event.error);});
    auto socket=co_await opened.promise.wait();co_return socket;
  }
  rt::Task<void> waitForFrames(std::size_t count){
    wantedFrames=count;frames=rt::withResolvers<void>();if(messages.size()>=count)frames->resolve();
    const auto reject=frames->reject;auto timeout=rt::setTimeout([reject]{reject(rt::makeReason(rt::JsError("waiting for upstream frames timed out")));},2000);
    const auto cleanup=rt::scopeExit([&]{rt::clearTimeout(timeout);frames.reset();});co_await frames->promise.wait();
  }
  rt::Task<void> waitForClientFrames(std::size_t count){
    wantedClientFrames=count;clientFrames=rt::withResolvers<void>();if(clientFrameCount>=count)clientFrames->resolve();
    const auto reject=clientFrames->reject;auto timeout=rt::setTimeout([reject]{reject(rt::makeReason(rt::JsError("waiting for client frames timed out")));},2000);
    const auto cleanup=rt::scopeExit([&]{rt::clearTimeout(timeout);clientFrames.reset();});co_await clientFrames->promise.wait();
  }
  rt::Task<void> close(){for(const auto& client:clients)client->terminate();for(const auto& socket:upstreamSockets)socket->terminate();if(gateway){auto stopped=gateway->shutdown();co_await stopped.wait();}if(upstream){auto stopped=upstream->shutdown();co_await stopped.wait();}auto settled=settle();co_await std::move(settled);}
};
rt::Promise<ws::EventArgs> nextClose(const Socket& socket){auto value=rt::withResolvers<ws::EventArgs>();socket->once("close",[resolve=value.resolve](const ws::EventArgs& event){resolve(event);});return value.promise;}
}
namespace {
class FailingRetirementKv : public rt::ForwardingKv {
public:
  int failures;
  explicit FailingRetirementKv(rt::Kv& innerKv,int failCount=100):rt::ForwardingKv(innerKv),failures(failCount){}
  int attempts=0;
  rt::Task<void> delete_(rt::KvKey key) override {if(key==live::upstream::liveCallAccountKey(std::string(harness::CALL_ID))&&attempts<failures){++attempts;throw rt::JsError("fixture retirement failure");}auto task=inner.delete_(key);co_await std::move(task);}
};
}
ASYNC_TEST("GET /v1/live/<call_id> keeps the frames a client sends immediately after the 101, in order"){
  SocketFixture fixture;auto held=rt::withResolvers<void>();fixture.holdHandshake=held.promise;auto starting=fixture.start();co_await std::move(starting);
  const std::vector<std::string> frames{R"({"type":"input_audio.append","seq":1})",R"({"type":"input_audio.append","seq":2})",R"({"type":"input_audio.append","seq":3})"};
  auto upgraded=rt::withResolvers<std::string>();const auto port=static_cast<int>(fixture.gateway->addr().port);const auto socket=uos::tests::helpers::loopback_http::RawClient::connect(port);
  auto response=std::make_shared<std::string>();auto sent=std::make_shared<bool>(false);
  socket->once("connect",[socket](const std::string&){socket->write("GET /v1/live/"+std::string(harness::CALL_ID)+" HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: MDEyMzQ1Njc4OWFiY2RlZg==\r\nSec-WebSocket-Version: 13\r\nAuthorization: Bearer "+std::string(harness::CLIENT_TOKEN)+"\r\n\r\n");});
  socket->on("error",[reject=upgraded.reject](const std::string& error){reject(rt::makeReason(rt::JsError(error)));});
  socket->on("data",[socket,response,sent,frames,resolve=upgraded.resolve](const std::string& bytes){
    if(*sent)return;
    *response+=bytes;if(response->find("\r\n\r\n")==std::string::npos)return;
    *sent=true;resolve(response->substr(0,response->find("\r\n")));std::string wire;
    for(std::size_t index=0;index<frames.size();++index){const auto& text=frames[index];const std::uint32_t seed=0x9a2b7c4dU+static_cast<std::uint32_t>(index);const std::array<unsigned char,4> mask{static_cast<unsigned char>(seed>>24),static_cast<unsigned char>(seed>>16),static_cast<unsigned char>(seed>>8),static_cast<unsigned char>(seed)};wire.push_back(static_cast<char>(0x81));wire.push_back(static_cast<char>(0x80U|text.size()));for(auto byte:mask)wire.push_back(static_cast<char>(byte));for(std::size_t offset=0;offset<text.size();++offset)wire.push_back(static_cast<char>(static_cast<unsigned char>(text[offset])^mask[offset%4]));}
    socket->write(wire);
  });
  const auto status=co_await upgraded.promise.wait();EXPECT(rt::startsWith(status,"HTTP/1.1 101 "));EXPECT(*sent);
  auto clientFrames=fixture.waitForClientFrames(frames.size());co_await std::move(clientFrames);EXPECT_EQ(fixture.upstreamSockets.size(),0U);held.resolve();auto waiting=fixture.waitForFrames(frames.size());co_await std::move(waiting);EXPECT_EQ(fixture.messages,frames);socket->destroy();auto closing=fixture.close();co_await std::move(closing);
}

ASYNC_TEST("GET /v1/live/<call_id> flushes a delayed-open queue at the UTF-8 payload byte boundary and keeps relaying"){
  SocketFixture fixture;auto held=rt::withResolvers<void>();fixture.holdHandshake=held.promise;auto starting=fixture.start();co_await std::move(starting);auto dial=fixture.connect();auto client=co_await std::move(dial);
  const auto frame=rt::repeat("é",2*1'048'576);client->send(frame);client->send(std::string(R"({"type":"input_audio.append","audio":"AAAA"})"));auto clientWaiting=fixture.waitForClientFrames(2);co_await std::move(clientWaiting);EXPECT_EQ(fixture.upstreamSockets.size(),0U);held.resolve();
  auto waiting=fixture.waitForFrames(2);co_await std::move(waiting);EXPECT_EQ(fixture.messages[0],frame);EXPECT_EQ(fixture.messages[1],R"({"type":"input_audio.append","audio":"AAAA"})");client->send(std::string(R"({"type":"input_audio.append","audio":"AAAA"})"));auto again=fixture.waitForFrames(3);co_await std::move(again);EXPECT_EQ(fixture.messages[2],R"({"type":"input_audio.append","audio":"AAAA"})");auto upstream=co_await fixture.joined.promise.wait();auto reply=rt::withResolvers<std::string>();client->once("message",[resolve=reply.resolve](const ws::EventArgs& event){resolve(rt::decodeUtf8(std::get<ws::Bytes>(event.data)));});upstream->send(std::string(R"({"type":"output_audio.delta","audio":"BBBB"})"));const auto replied=co_await reply.promise.wait();EXPECT_EQ(replied,R"({"type":"output_audio.delta","audio":"BBBB"})");auto closing=fixture.close();co_await std::move(closing);
}

ASYNC_TEST("GET /v1/live/<call_id> rejects oversized UTF-8 and binary client frames before the delayed upstream opens"){
  for(int variant=0;variant<2;++variant){
    SocketFixture fixture;auto held=rt::withResolvers<void>();fixture.holdHandshake=held.promise;auto starting=fixture.start();co_await std::move(starting);auto dial=fixture.connect();auto client=co_await std::move(dial);co_await fixture.upstreamHandshake.promise.wait();auto closed=nextClose(client);
    if(variant==0)client->send(rt::repeat("é",2*1'048'576+1));else client->send(rt::Bytes(4*1'048'576+1,0));
    const auto result=co_await closed.wait();EXPECT_EQ(result.code,1011);EXPECT_EQ(rt::decodeUtf8(result.reason),"realtime sideband frame exceeds payload limit");EXPECT_EQ(fixture.upstreamSockets.size(),0U);held.resolve();auto closing=fixture.close();co_await std::move(closing);
  }
}

ASYNC_TEST("GET /v1/live/<call_id> caps a delayed-open queue by bytes below the frame-count limit"){
  SocketFixture fixture;auto held=rt::withResolvers<void>();fixture.holdHandshake=held.promise;auto starting=fixture.start();co_await std::move(starting);auto dial=fixture.connect();auto client=co_await std::move(dial);auto closed=nextClose(client);
  const auto frame=rt::repeat("€",1'048'576);co_await fixture.upstreamHandshake.promise.wait();client->send(frame);client->send(frame);client->send(frame);const auto result=co_await closed.wait();EXPECT_EQ(result.code,1011);EXPECT_EQ(rt::decodeUtf8(result.reason),"realtime sideband frame queue overflow");EXPECT_EQ(fixture.upstreamSockets.size(),0U);held.resolve();auto closing=fixture.close();co_await std::move(closing);
}

ASYNC_TEST("GET /v1/live/<call_id> checks pending bytes plus the next frame during queued flush and direct send"){
  const std::string frame=R"({"type":"input_audio.append","audio":"AAAA"})";
  const std::string reply=R"({"type":"output_audio.delta","audio":"BBBB"})";
  const double pendingBytes=live::upstream::LIVE_SIDEBAND_MAX_BUFFERED_BYTES-static_cast<double>(rt::encodeUtf8(frame).size())+1;
  const std::vector<bool> delays{true,false};
  for(const bool delayed:delays){
    SocketFixture fixture;auto held=rt::withResolvers<void>();if(delayed)fixture.holdHandshake=held.promise;auto starting=fixture.start();co_await std::move(starting);auto dial=fixture.connect();auto client=co_await std::move(dial);co_await fixture.upstreamHandshake.promise.wait();
    const auto original=ws::WebSocket::bufferedAmountGetter();const auto reset=rt::scopeExit([original]{ws::WebSocket::bufferedAmountGetter()=original;});
    ws::WebSocket::bufferedAmountGetter()=[original,&fixture,pendingBytes](const ws::WebSocket& socket){if(socket.url()&&*socket.url()!=fixture.url&&rt::includes(*socket.url(),"/v1/live/"))return pendingBytes;return original(socket);};
    if(!delayed){auto upstream=co_await fixture.joined.promise.wait();auto received=rt::withResolvers<std::string>();client->once("message",[resolve=received.resolve](const ws::EventArgs& event){resolve(rt::decodeUtf8(std::get<ws::Bytes>(event.data)));});upstream->send(reply);const auto ready=co_await received.promise.wait();EXPECT_EQ(ready,reply);}
    auto closed=nextClose(client);client->send(frame);auto receiving=fixture.waitForClientFrames(1);co_await std::move(receiving);held.resolve();const auto result=co_await closed.wait();EXPECT_EQ(result.code,1011);EXPECT_EQ(rt::decodeUtf8(result.reason),"realtime upstream is not draining frames");EXPECT_EQ(fixture.messages.size(),0U);auto closing=fixture.close();co_await std::move(closing);
  }
}

ASYNC_TEST("GET /v1/live/<call_id> bridges the sideband to the mapped account's upstream and relays frames both ways"){
  SocketFixture fixture;auto starting=fixture.start();co_await std::move(starting);auto dial=fixture.connect();auto client=co_await std::move(dial);auto upstream=co_await fixture.joined.promise.wait();
  EXPECT_EQ(fixture.upstreamPaths[0],"/v1/live/"+std::string(harness::CALL_ID));EXPECT_EQ(fixture.upstreamHeaders[0].get("authorization"),std::optional<std::string>("Bearer "+std::string(harness::ACCOUNT_A)+"-access-token"));EXPECT_EQ(fixture.upstreamHeaders[0].get("chatgpt-account-id"),std::optional<std::string>(std::string(harness::ACCOUNT_A)));EXPECT_EQ(fixture.upstreamHeaders[0].get("originator"),std::optional<std::string>("codex_cli_rs"));EXPECT_EQ(fixture.upstreamHeaders[0].get("openai-alpha"),std::optional<std::string>("quicksilver=v2"));EXPECT(rt::startsWith(fixture.upstreamHeaders[0].get("user-agent").value_or(""),"codex_cli_rs/"));
  client->send(std::string(R"({"type":"input_audio.append","audio":"AAAA"})"));auto waiting=fixture.waitForFrames(1);co_await std::move(waiting);EXPECT_EQ(fixture.messages[0],R"({"type":"input_audio.append","audio":"AAAA"})");
  auto received=rt::withResolvers<std::string>();client->once("message",[resolve=received.resolve](const ws::EventArgs& event){resolve(rt::decodeUtf8(std::get<ws::Bytes>(event.data)));});upstream->send(std::string(R"({"type":"output_audio.delta","audio":"BBBB"})"));const auto message=co_await received.promise.wait();EXPECT_EQ(message,R"({"type":"output_audio.delta","audio":"BBBB"})");
  auto clientClosed=nextClose(client),upstreamClosed=nextClose(upstream);client->close(1000,std::string("session complete"));const auto a=co_await clientClosed.wait();const auto b=co_await upstreamClosed.wait();EXPECT_EQ(a.code,1000);EXPECT_EQ(b.code,1000);auto closing=fixture.close();co_await std::move(closing);
}

ASYNC_TEST("GET /v1/live/<call_id> rejects an unauthenticated upgrade without dialing upstream"){
  harness::Fixture fixture;auto seed=fixture.seed();co_await std::move(seed);auto statusTask=statusOf(fixture,std::string(harness::CALL_ID),"");const auto status=co_await std::move(statusTask);EXPECT_EQ(status,401);EXPECT_EQ(fixture.calls.size(),0U);
}

ASYNC_TEST("GET /v1/live/<call_id> answers 404 for an unknown call id without dialing upstream"){
  harness::Fixture fixture;auto seed=fixture.seed();co_await std::move(seed);auto task=statusOf(fixture,"unknown",std::string(harness::PRINCIPAL));const auto status=co_await std::move(task);EXPECT_EQ(status,404);EXPECT_EQ(fixture.calls.size(),0U);
}

ASYNC_TEST("GET /v1/live/<call_id> retires a terminal upstream mapping before closing and allowing reconnect"){
  SocketFixture fixture;auto held=rt::withResolvers<void>();fixture.holdHandshake=held.promise;fixture.upstreamStatus=404;auto starting=fixture.start();co_await std::move(starting);auto dial=fixture.connect();auto client=co_await std::move(dial);co_await fixture.upstreamHandshake.promise.wait();
  const auto faulty=std::make_shared<FailingRetirementKv>(*fixture.kv,1);auto unavailable=std::make_shared<bool>(true);uos::kv::KvOpener opener=[faulty,unavailable]()->rt::Task<std::shared_ptr<rt::Kv>>{if(*unavailable){*unavailable=false;co_return nullptr;}co_return faulty;};uos::kv::setKvForTest(nullptr,opener);
  auto closed=nextClose(client);held.resolve();const auto result=co_await closed.wait();EXPECT_EQ(result.code,1011);EXPECT_EQ(rt::decodeUtf8(result.reason),"realtime upstream rejected the sideband");EXPECT_EQ(faulty->attempts,1);
  auto read=live::upstream::readLiveCallMapping(std::string(harness::CALL_ID));const auto mapping=co_await std::move(read);EXPECT(!mapping);auto statusTask=statusOf(fixture,std::string(harness::CALL_ID),std::string(harness::PRINCIPAL));const auto status=co_await std::move(statusTask);EXPECT_EQ(status,404);EXPECT_EQ(fixture.upstreamSockets.size(),0U);uos::kv::setKvForTest(fixture.kv);auto closing=fixture.close();co_await std::move(closing);
}

ASYNC_TEST("GET /v1/live/<call_id> closes when terminal mapping retirement keeps failing"){
  const std::vector<double> statuses{404,410};
  for(const auto status:statuses){
    SocketFixture fixture;auto held=rt::withResolvers<void>();fixture.holdHandshake=held.promise;fixture.upstreamStatus=status;auto starting=fixture.start();co_await std::move(starting);const auto faulty=std::make_shared<FailingRetirementKv>(*fixture.kv);uos::kv::setKvForTest(faulty);
    auto dial=fixture.connect();auto client=co_await std::move(dial);co_await fixture.upstreamHandshake.promise.wait();auto closed=nextClose(client);held.resolve();const auto result=co_await closed.wait();EXPECT_EQ(result.code,1011);EXPECT_EQ(rt::decodeUtf8(result.reason),"realtime upstream rejected the sideband");EXPECT_EQ(faulty->attempts,4);uos::kv::setKvForTest(fixture.kv);auto closing=fixture.close();co_await std::move(closing);
  }
}

ASYNC_TEST("GET /v1/live/<call_id> answers 403 for a different valid principal without dialing upstream"){
  harness::Fixture fixture;auto seed=fixture.seed();co_await std::move(seed);auto mapping=live::upstream::writeLiveCallAccount(std::string(harness::CALL_ID),std::string(harness::ACCOUNT_A),std::string(harness::PRINCIPAL));co_await std::move(mapping);
  auto task=statusOf(fixture,std::string(harness::CALL_ID),"api-key:other");const auto status=co_await std::move(task);EXPECT_EQ(status,403);EXPECT_EQ(fixture.calls.size(),0U);
}

ASYNC_TEST("GET /v1/live/<call_id> fails closed for a mapping with no recorded principal"){
  harness::Fixture fixture;auto seed=fixture.seed();co_await std::move(seed);const auto key=live::upstream::liveCallAccountKey(std::string(harness::CALL_ID));const rt::KvValue legacy(json::object{{"account_id",std::string(harness::ACCOUNT_A)}});auto saved=fixture.kv->set(key,legacy);co_await std::move(saved);
  auto task=statusOf(fixture,std::string(harness::CALL_ID),std::string(harness::PRINCIPAL));const auto status=co_await std::move(task);EXPECT_EQ(status,403);EXPECT_EQ(fixture.calls.size(),0U);
}

ASYNC_TEST("GET /v1/live/<call_id> lets the creating principal reconnect on the same upstream account"){
  SocketFixture fixture;auto starting=fixture.start();co_await std::move(starting);auto firstDial=fixture.connect();auto first=co_await std::move(firstDial);auto firstUp=co_await fixture.joined.promise.wait();EXPECT_EQ(fixture.upstreamHeaders[0].get("authorization"),std::optional<std::string>("Bearer "+std::string(harness::ACCOUNT_A)+"-access-token"));EXPECT_EQ(fixture.upstreamHeaders[0].get("chatgpt-account-id"),std::optional<std::string>(std::string(harness::ACCOUNT_A)));auto closed=nextClose(first);first->close(1000,std::string("reconnect"));co_await closed.wait();firstUp->terminate();fixture.joined=rt::withResolvers<Socket>();
  auto secondDial=fixture.connect();auto second=co_await std::move(secondDial);auto secondUp=co_await fixture.joined.promise.wait();(void)second;(void)secondUp;EXPECT_EQ(fixture.upstreamHeaders.size(),2U);EXPECT_EQ(fixture.upstreamPaths[1],"/v1/live/"+std::string(harness::CALL_ID));EXPECT_EQ(fixture.upstreamHeaders[0].get("authorization"),fixture.upstreamHeaders[1].get("authorization"));EXPECT_EQ(fixture.upstreamHeaders[1].get("chatgpt-account-id"),std::optional<std::string>(std::string(harness::ACCOUNT_A)));auto closing=fixture.close();co_await std::move(closing);
}

ASYNC_TEST("GET /v1/live/<call_id> refreshes the mapped account's expired token before dialing upstream"){
  SocketFixture fixture;auto starting=fixture.start();co_await std::move(starting);const std::vector<uos::types::CodexAuthState> accounts{harness::expiredCodexAccount(std::string(harness::ACCOUNT_A))};auto seed=fixture.setAuthPool(accounts);co_await std::move(seed);
  auto dial=fixture.connect();auto client=co_await std::move(dial);auto upstream=co_await fixture.joined.promise.wait();(void)client;(void)upstream;
  EXPECT_EQ(fixture.upstreamHeaders[0].get("authorization"),std::optional<std::string>("Bearer live-calls-refreshed-access-token"));EXPECT_EQ(fixture.upstreamHeaders[0].get("chatgpt-account-id"),std::optional<std::string>(std::string(harness::ACCOUNT_A)));EXPECT_EQ(fixture.refreshTokens.size(),1U);EXPECT_EQ(fixture.refreshTokens[0],std::string(harness::ACCOUNT_A)+"-refresh-token");
  auto reading=fixture.kv->get(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY);const auto pool=co_await std::move(reading);const auto poolJson=pool.value.toJson();const auto accountsNow=rt::getArray(poolJson,"accounts");ASSERT(accountsNow&&accountsNow->size()==1);EXPECT_EQ(rt::getString((*accountsNow)[0],"access_token"),std::optional<std::string_view>("live-calls-refreshed-access-token"));EXPECT_EQ(rt::getString((*accountsNow)[0],"refresh_token"),std::optional<std::string_view>("live-calls-refreshed-refresh-token"));
  auto readingMapping=live::upstream::readLiveCallMapping(std::string(harness::CALL_ID));const auto mapping=co_await std::move(readingMapping);ASSERT(mapping);EXPECT_EQ(mapping->accountId,harness::ACCOUNT_A);EXPECT_EQ(mapping->principalId,std::optional<std::string>(std::string(harness::PRINCIPAL)));auto closing=fixture.close();co_await std::move(closing);
}


ASYNC_TEST("GET /v1/live/<call_id> refreshes again on reconnect when the mapped credential expired again"){
  SocketFixture fixture;auto starting=fixture.start();co_await std::move(starting);
  fixture.refresh=[&fixture](std::string){const auto count=rt::numberToString(static_cast<double>(fixture.refreshTokens.size()));return rt::fulfilledTask(rt::Response::json(json::object{{"access_token","refreshed-"+count},{"refresh_token","refresh-"+count}}));};
  const std::vector<uos::types::CodexAuthState> expired{harness::expiredCodexAccount(std::string(harness::ACCOUNT_A)),harness::codexAccount(std::string(harness::ACCOUNT_B))};auto seed=fixture.setAuthPool(expired);co_await std::move(seed);
  auto firstDial=fixture.connect();auto first=co_await std::move(firstDial);auto firstUp=co_await fixture.joined.promise.wait();EXPECT_EQ(fixture.upstreamHeaders[0].get("authorization"),std::optional<std::string>("Bearer refreshed-1"));auto closed=nextClose(first);first->close(1000,std::string("refresh reconnect"));co_await closed.wait();EXPECT_EQ(fixture.refreshTokens.size(),1U);firstUp->terminate();fixture.joined=rt::withResolvers<Socket>();
  auto reseed=fixture.setAuthPool(expired);co_await std::move(reseed);auto secondDial=fixture.connect();auto second=co_await std::move(secondDial);auto secondUp=co_await fixture.joined.promise.wait();(void)second;(void)secondUp;
  EXPECT_EQ(fixture.upstreamHeaders[1].get("chatgpt-account-id"),std::optional<std::string>(std::string(harness::ACCOUNT_A)));EXPECT_EQ(fixture.upstreamHeaders[1].get("authorization"),std::optional<std::string>("Bearer refreshed-2"));EXPECT_EQ(fixture.refreshTokens.size(),2U);EXPECT_EQ(fixture.refreshTokens[1],std::string(harness::ACCOUNT_A)+"-refresh-token");auto closing=fixture.close();co_await std::move(closing);
}


ASYNC_TEST("GET /v1/live/<call_id> answers 404 when the mapped account is no longer configured"){
  harness::Fixture fixture;auto seed=fixture.seed();co_await std::move(seed);auto mapping=live::upstream::writeLiveCallAccount(std::string(harness::CALL_ID),"removed",std::string(harness::PRINCIPAL));co_await std::move(mapping);
  auto task=statusOf(fixture,std::string(harness::CALL_ID),std::string(harness::PRINCIPAL));const auto status=co_await std::move(task);EXPECT_EQ(status,404);EXPECT_EQ(fixture.refreshTokens.size(),0U);
}

ASYNC_TEST("Ordinary live sideband HTTP never manufactures a switching-protocols response"){
  harness::Fixture fixture;auto seed=fixture.seed();co_await std::move(seed);auto mapping=live::upstream::writeLiveCallAccount(std::string(harness::CALL_ID),std::string(harness::ACCOUNT_A),std::string(harness::PRINCIPAL));co_await std::move(mapping);
  auto task=live::sideband::handleLiveSideband(joinRequest(),std::string(harness::CALL_ID),std::string(harness::PRINCIPAL));auto response=co_await std::move(task);EXPECT_EQ(response.status(),426);
  auto noUpgrade=statusOf(fixture,std::string(harness::CALL_ID),std::string(harness::PRINCIPAL),false);const auto status=co_await std::move(noUpgrade);EXPECT_EQ(status,426);
}

TEST("live upstream URL, location and header helpers match Bun"){
  const auto restore=rt::scopeExit([]{live::upstream::setLiveUpstreamBasesForTest({});});
  live::upstream::setLiveUpstreamBasesForTest({.callsBaseUrl="http://fixture.invalid/base///",.sidebandBaseUrl="ws://127.0.0.1:1/live///"});
  const auto expected=rt::jsonParse(R"oracle({"urls":["http://fixture.invalid/base/realtime/calls?intent=quicksilver&architecture=avas","ws://127.0.0.1:1/live/rtc_%2Fa%3F%F0%9F%A6%84"],"locations":[{"input":"rtc_","id":null},{"input":"/v1/realtime/calls/rtc_hello?x=1","id":"rtc_hello"},{"input":"/foo/12345678-1234-1234-1234-123456789abc/end","id":"12345678-1234-1234-1234-123456789abc"},{"input":"/foo/invalid","id":null},{"input":"/rtc_a/rtc_b","id":"rtc_b"}],"headers":[["content-type","application/json"],["openai-alpha","custom"],["originator","codex_cli_rs"],["thread-id","thread"],["user-agent","codex_cli_rs/0.100.0 (ai.ubq.fi)"],["x-session-id","session"]]})oracle");
  const auto* urls=rt::getArray(expected,"urls");EXPECT_EQ(live::upstream::liveCallsUrl(),std::string((*urls)[0].get_string()));EXPECT_EQ(live::upstream::liveSidebandUrl("rtc_/a?🦄"),std::string((*urls)[1].get_string()));
  for(const auto& fixture:*rt::getArray(expected,"locations")){const auto actual=live::upstream::parseLiveCallIdFromLocation(std::string(*rt::getString(fixture,"input")));const auto* wanted=rt::get(fixture,"id");if(wanted->is_null())EXPECT(!actual);else EXPECT_EQ(actual,std::optional<std::string>(std::string(wanted->get_string())));}
  rt::RequestInit init;init.headers=rt::Headers{{"authorization","Bearer client"},{"cookie","skip"},{"openai-alpha"," custom "},{"x-session-id","session"},{"thread-id","thread"},{"x-ignore","ignore"}};rt::Request request("http://fixture.invalid",init);
  json::array headers;for(const auto& [key,value]:live::upstream::liveCallsUpstreamHeaders(request).entries())headers.push_back(json::array{key,value});EXPECT_EQ(rt::jsonStringify(headers),rt::jsonStringify(*rt::get(expected,"headers")));
}

ASYNC_TEST("live sideband preparation matches Bun status, error bytes and upgrade options"){
  const auto vectors=rt::jsonParse(R"oracle([{"scenario":"unknown","status":404,"body":"{\"error\":{\"message\":\"Unknown realtime call.\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\"}}"},{"scenario":"principal","status":403,"body":"{\"error\":{\"message\":\"The realtime call was not created by this gateway principal.\",\"type\":\"invalid_request_error\",\"code\":\"forbidden\"}}"},{"scenario":"legacy","status":403,"body":"{\"error\":{\"message\":\"The realtime call was not created by this gateway principal.\",\"type\":\"invalid_request_error\",\"code\":\"forbidden\"}}"},{"scenario":"removed","status":404,"body":"{\"error\":{\"message\":\"The realtime call's account is no longer configured.\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\"}}"},{"scenario":"ordinary","status":426,"body":"{\"error\":{\"message\":\"The realtime sideband requires a WebSocket upgrade.\",\"type\":\"invalid_request_error\",\"code\":\"invalid_request_error\"}}"},{"scenario":"upgrade","maxPayloadBytes":4194304,"payloadLimitClose":{"code":1011,"reason":"realtime sideband frame exceeds payload limit"},"hasAccept":true}])oracle");
  for(const auto& expected:vectors.get_array()){
    harness::Fixture fixture;auto seeding=fixture.seed();co_await std::move(seeding);const std::string scenario(*rt::getString(expected,"scenario"));
    if(scenario!="unknown"){
      json::object record{{"account_id",scenario=="removed"?"removed":std::string(harness::ACCOUNT_A)}};if(scenario!="legacy")record["principal_id"]=std::string(harness::PRINCIPAL);
      const auto key=live::upstream::liveCallAccountKey(std::string(harness::CALL_ID));const rt::KvValue row(record);auto saving=fixture.kv->set(key,row);co_await std::move(saving);
    }
    const auto req=joinRequest(scenario!="ordinary");const auto principal=scenario=="principal"?"other":std::string(harness::PRINCIPAL);
    auto preparing=live::sideband::prepareLiveSidebandUpgrade(req,std::string(harness::CALL_ID),principal);auto result=co_await std::move(preparing);
    if(const auto* response=std::get_if<rt::Response>(&result)){EXPECT_EQ(response->status(),rt::getNumber(expected,"status").value());auto reading=response->text();const auto body=co_await std::move(reading);EXPECT_EQ(body,std::string(*rt::getString(expected,"body")));}
    else{const auto& prepared=std::get<upgrade::WebSocketUpgrade>(result);EXPECT_EQ(prepared.maxPayloadBytes,rt::getNumber(expected,"maxPayloadBytes").value());EXPECT(static_cast<bool>(prepared.accept));EXPECT(prepared.payloadLimitClose);if(prepared.payloadLimitClose){const auto* limit=rt::get(expected,"payloadLimitClose");EXPECT_EQ(prepared.payloadLimitClose->code,rt::getNumber(*limit,"code").value());EXPECT_EQ(prepared.payloadLimitClose->reason,std::string(*rt::getString(*limit,"reason")));}}
    auto settling=fixture.settle();co_await std::move(settling);
  }
}

ASYNC_TEST("GET /v1/live/<call_id> rejects a UTF-8 frame above its payload byte limit before opening upstream"){
  SocketFixture fixture;auto held=rt::withResolvers<void>();fixture.holdHandshake=held.promise;auto starting=fixture.start();co_await std::move(starting);auto dial=fixture.connect();auto client=co_await std::move(dial);auto closed=nextClose(client);
  const auto frame=rt::repeat("é",2*1'048'576)+"x";client->send(frame);const auto result=co_await closed.wait();EXPECT_EQ(result.code,1011);EXPECT_EQ(rt::decodeUtf8(result.reason),"realtime sideband frame exceeds payload limit");EXPECT_EQ(fixture.upstreamSockets.size(),0U);held.resolve();auto closing=fixture.close();co_await std::move(closing);
}

ASYNC_TEST("GET /v1/live/<call_id> refuses a frame before exceeding the upstream pending-byte bound"){
  SocketFixture fixture;auto starting=fixture.start();co_await std::move(starting);auto dial=fixture.connect();auto client=co_await std::move(dial);auto upstream=co_await fixture.joined.promise.wait();(void)upstream;
  const auto original=ws::WebSocket::bufferedAmountGetter();const auto reset=rt::scopeExit([original]{ws::WebSocket::bufferedAmountGetter()=original;});
  ws::WebSocket::bufferedAmountGetter()=[original,&fixture](const ws::WebSocket& socket){if(socket.url()&&*socket.url()!=fixture.url&&rt::includes(*socket.url(),"/v1/live/"))return live::upstream::LIVE_SIDEBAND_MAX_BUFFERED_BYTES;return original(socket);};
  auto closed=nextClose(client);client->send(std::string("x"));const auto result=co_await closed.wait();EXPECT_EQ(result.code,1011);EXPECT_EQ(rt::decodeUtf8(result.reason),"realtime upstream is not draining frames");EXPECT_EQ(fixture.messages.size(),0U);auto closing=fixture.close();co_await std::move(closing);
}

