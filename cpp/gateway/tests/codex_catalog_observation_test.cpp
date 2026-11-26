// Links: tests/codex_catalog_observation_test.cpp
// src/models/metadata.cpp src/models/catalog.cpp src/utils.cpp src/provider/lithos.cpp
// src/provider/selection.cpp src/provider/surplus.cpp src/provider/metered.cpp src/input_normalization.cpp
// src/provider/cerebras.cpp src/models/openrouter_models.cpp src/models/codex_models_whitelist.cpp src/provider/anthropic_responses.cpp
// src/codex/auth.cpp src/types.cpp src/config.cpp src/models/codex_models.cpp
// src/http.cpp src/codex/capacity_routing.cpp src/provider/anthropic_response_wire.cpp src/codex/native_auth.cpp
// src/catalog/index.cpp src/authority/native_codex_capacity.cpp src/catalog/models.cpp src/catalog/store.cpp
// src/auth/relay.cpp src/catalog/types.cpp src/request_policy.cpp src/authority/native_codex_contracts.cpp
// src/provider/claude_code_models.cpp src/defaults.cpp src/models/codex_models_fetch.cpp src/provider/surplus_claude_request.cpp
// src/provider/anthropic_replay.cpp src/models/codex_models_availability.cpp src/provider/claude_code_accounts.cpp src/codex/auth_refresh.cpp
// src/codex/routing_mutations.cpp src/runtime_config.cpp src/release.cpp src/responses_stream.cpp
// src/authority/native_codex_mutations.cpp src/codex/external_auth.cpp src/provider/claude_code_credentials.cpp src/codex/dispatch.cpp
// src/provider/surplus_claude_chat_request.cpp src/codex/native_auth_transport.cpp src/authority/native_codex_evaluation.cpp src/codex/rate_limit_429.cpp
// src/openai_telemetry.cpp src/deepseek/waterfall.cpp src/codex/routing_state.cpp src/kv.cpp
// src/bounded_response_body.cpp src/recent_model_context.cpp src/provider/health.cpp src/provider/openrouter.cpp
// src/deepseek/index.cpp src/provider/lithos_rate_limits.cpp src/provider/anthropic_chat_responses.cpp src/responses_failover_stream.cpp
// src/provider/capacity_events.cpp src/authority/native_codex_429.cpp src/provider/cerebras_rate_limits.cpp src/runtime/websocket.cpp
// src/codex/banked_reset.cpp src/provider/claude_code.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp
// src/codex/responses_error.cpp src/provider/capacity_contract.cpp src/provider/capacity.cpp src/provider/removed_provider_telemetry.cpp
// src/provider/capacity_parse.cpp src/storage/kv.cpp src/provider/claude_capacity_history.cpp src/codex/banked_reset_provider.cpp
// src/codex/reset_settings.cpp src/provider/capacity_rollups.cpp src/provider/claude_capacity_rollups.cpp src/cache/prompt_analytics_core.cpp
// src/provider/capacity_history.cpp src/codex/experiment.cpp src/paid_fallback/ledger_stats.cpp src/codex/account_routing.cpp
// src/codex/routing_serial.cpp src/provider/anthropic_chat_stream.cpp src/authority/native_codex.cpp src/codex/routing_evaluation.cpp
// src/storage/sqlite_readonly.cpp src/metered_quota.cpp src/provider/capacity_analytics.cpp
// src/cache/prompt_analytics_read.cpp src/storage/native_kv.cpp src/codex/overage_settings.cpp tests/helpers/codex_diff_kv.cpp
// src/health.cpp src/inference_admission.cpp src/provider/claude_code_access.cpp
// Port of all six tests/codex-catalog-observation.test.ts cases, in source order.
// Representation-only gap: TS lines77-116 monkeypatch Response.clone/text and clone reader methods.
// The instance clone/read/release call counts and clone-only cancellation Promise are not exposed by
// rt::Response's value methods (assertions137-140,187-190,217-218,255). Real stream pulls, tee
// cancellation reasons, elapsed deadline, bodyUsed, response identity and availability remain tested.
// The native source's overflow pull bound was freshly recorded:4MiB+3*64KiB source bytes, of which
//4MiB+64KiB reached the observer and two chunks were prefetched (bun-observation-pull.log).
#include "uos/catalog/index.hpp"
#include "uos/catalog/store.hpp"
#include "uos/catalog/types.hpp"
#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/models/catalog.hpp"
#include "uos/models/codex_models_availability.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "tests/helpers/codex_diff_kv.hpp"
#include <memory>
#include <optional>
#include <string>
#include <vector>
namespace {
  namespace rt=uos::rt;
  namespace json=uos::json;
  namespace mf=uos::models::codex_models_fetch;
  namespace availability=uos::models::codex_models_availability;
  namespace diff=uos::tests::helpers::codex_diff_kv;
  const std::string ACCOUNT="bounded-catalog-account";
  const std::string MODEL="bounded-catalog-model";
  const std::string VERSION="0.160.0";
  constexpr std::size_t BYTE_CAP=4*1024*1024;
  const std::string CATALOG=R"({"models":[{"slug":"bounded-catalog-model","supported_reasoning_levels":[{"effort":"ultra","description":"Native tier"}]}]})";
#define AWAIT(name, ...) auto name##Task=(__VA_ARGS__); auto name=co_await std::move(name##Task)
  class ObservationKv final:public diff::DiffKv {
  public:
    bool rejectFull=false;
    void put(const rt::KvKey& key,const rt::KvValue& value){rows.set(diff::keyOf(key),Row{key,value,stamp()});}
    using rt::Kv::get;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key,rt::KvGetOptions={}) override {co_await rt::microtask();if(rejectFull&&key==uos::codex::auth::CODEX_MODELS_KV_KEY)throw rt::JsError("synthetic optional read failure");co_return entryOf(key);}
    rt::Task<rt::KvCommitResult> set(rt::KvKey key,rt::KvValue value,rt::KvSetOptions={}) override {put(key,value);rt::KvCommitResult result{true,*entryOf(key).versionstamp};co_await rt::microtask();co_return result;}
  };
  struct Fixture {
    rt::env::ScopedEnvironment env{std::vector<std::pair<std::string,std::string>>{{"HOME",rt::env::get("HOME").value_or("/nonexistent/w07c4-observation")},{"TMPDIR",rt::env::get("TMPDIR").value_or("/tmp")}}};
    std::string auth=uos::config::config.codexAuthJsonB64;
    std::shared_ptr<ObservationKv> kv=std::make_shared<ObservationKv>();
    rt::GlobalFetchOverride rejecting{rt::Fetch([](rt::RequestInfo,rt::RequestInit)->rt::Response{throw rt::JsError("Provider discovery requires an explicit local fixture");})};
    Fixture(){uos::config::config.codexAuthJsonB64.clear();reset();uos::kv::setKvForTest(kv);uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const auto&){});const auto now=rt::dateNow();const json::object account{{"account_id",ACCOUNT},{"access_token","synthetic-access"},{"refresh_token","synthetic-refresh"},{"updated_at_ms",now}};kv->put(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY,json::object{{"accounts",json::array{account}},{"updated_at_ms",now}});}
    static void reset(){uos::codex::auth::resetCodexAuthCacheForTest();availability::resetCodexAccountModelsCacheForTest();uos::catalog::store::resetCodexCatalogMemoForTest();uos::runtime_config::resetRuntimeConfigCacheForTest();uos::models::openrouter_models::resetOpenRouterModelsCacheForTest();uos::provider::metered::resetMeteredModelsCacheForTest();uos::provider::surplus::resetSurplusModelsCacheForTest();uos::provider::selection::resetProviderSelectionCacheForTest();uos::provider::health::resetProviderHealthThrottleForTest();uos::provider::claude_code_models::resetClaudeCodeModelsCacheForTest();}
    ~Fixture(){uos::kv::setKvForTest(nullptr);reset();uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest({});uos::config::config.codexAuthJsonB64=auth;}
  };
  rt::Task<void> seedRejection(){auto task=availability::recordCodexModelUnsupported(ACCOUNT,MODEL);co_await std::move(task);}
  bool unavailable(const std::string& model=MODEL,const std::string& account=ACCOUNT){return availability::codexModelUnavailableAccounts(model,std::vector<std::string>{account}).has(account);}
  bool hasAccount(const std::string& account=ACCOUNT){const auto cache=availability::getCodexAccountModelsCacheForTest();return cache.store&&cache.store->accounts.has(account);}
  rt::Task<rt::Response> fetchModels(const std::optional<std::string>& etag=std::nullopt){mf::FetchCodexModelsOptions options;options.clientVersion=VERSION;options.ifNoneMatch=etag;return mf::fetchCodexModels(options);}
  rt::Task<rt::Response> withinBudget(rt::Task<rt::Response> task){rt::Promise<rt::Response> pending(std::move(task));rt::Timeout timer;auto timeout=rt::newPromise<rt::Response>([&timer](auto,auto reject){timer=rt::setTimeout([reject]{reject(rt::makeReason(rt::JsError("Catalog observation exceeded its one-second deadline plus tolerance")));},2000);});auto clear=rt::scopeExit([timer]{rt::clearTimeout(timer);});std::vector<rt::Promise<rt::Response>> racers{pending,timeout};auto race=rt::race(std::move(racers));auto response=co_await race.wait();co_return response;}
  rt::Fetch returning(rt::Response response){return rt::Fetch([response](rt::RequestInfo,rt::RequestInit){return response;});}
  rt::Request request(const std::optional<std::string>& etag=std::nullopt){rt::RequestInit init;if(etag)init.headers=rt::Headers{{"If-None-Match",*etag}};return rt::Request("https://ai.ubq.fi/v1/models?client_version="+VERSION,init);}
  struct OpenStream {std::optional<rt::ReadableStreamDefaultController<rt::ByteChunk>> controller;bool canceled=false;rt::Reason reason;};
  rt::Response openResponse(const std::shared_ptr<OpenStream>& state){rt::UnderlyingSource<rt::ByteChunk> source;source.start=[state](auto controller){state->controller=controller;controller.enqueue(rt::encodeUtf8(CATALOG));};source.cancel=[state](rt::Reason reason){state->canceled=true;state->reason=reason;};rt::ByteStream stream(source);return rt::Response(stream,rt::ResponseInit{.status=200,.statusText="Catalog ready",.headers=rt::Headers{{"Content-Type","application/json"},{"ETag","\"raw-stalled\""}}});}
  rt::Task<void> verifyTeeCancellation(){auto state=std::make_shared<OpenStream>();const auto upstream=openResponse(state);rt::GlobalFetchOverride fetch(returning(upstream));auto pending=fetchModels();AWAIT(response,withinBudget(std::move(pending)));EXPECT(!state->canceled);const auto body=response.body();ASSERT(body);auto cancel=body->cancel();co_await std::move(cancel);EXPECT(state->canceled);bool matched=false;try{std::rethrow_exception(state->reason);}catch(const rt::TeeCancelReason& reason){matched=rt::reasonToString(reason.reason2)=="Bounded upstream body read incomplete";}catch(...){}EXPECT(matched);}
}
ASYNC_TEST("single-account catalog: stalled observation returns the original stream without learning partial JSON") {
  Fixture fixture;co_await seedRejection();auto state=std::make_shared<OpenStream>();const auto upstream=openResponse(state);rt::GlobalFetchOverride fetch(returning(upstream));const auto started=rt::performanceNow();auto pending=fetchModels();AWAIT(response,withinBudget(std::move(pending)));const auto elapsed=rt::performanceNow()-started;EXPECT(elapsed>=900&&elapsed<2000);EXPECT(response==upstream);EXPECT_EQ(response.statusText(),"Catalog ready");EXPECT_EQ(response.headers().get("ETag"),"\"raw-stalled\"");EXPECT(!response.bodyUsed());EXPECT(!response.body()->locked());EXPECT(!state->canceled);EXPECT(unavailable());EXPECT(!hasAccount());state->controller->enqueue(rt::encodeUtf8("\n"));state->controller->close();AWAIT(text,response.text());EXPECT_EQ(text,CATALOG+"\n");co_await rt::microtask();co_await verifyTeeCancellation();
}
ASYNC_TEST("single-account catalog: oversized observation stops at 4 MiB and preserves the full original") {
  Fixture fixture;co_await seedRejection();const auto content=std::make_shared<rt::Bytes>(rt::encodeUtf8(CATALOG+std::string(BYTE_CAP*2,' ')));constexpr std::size_t CHUNK=64*1024;auto offset=std::make_shared<std::size_t>(0);auto canceled=std::make_shared<bool>(false);rt::UnderlyingSource<rt::ByteChunk> source;source.pull=[content,offset](auto controller){if(*offset==content->size()){controller.close();return;}const auto end=std::min(*offset+CHUNK,content->size());controller.enqueue(rt::ByteChunk(content->begin()+static_cast<std::ptrdiff_t>(*offset),content->begin()+static_cast<std::ptrdiff_t>(end)));*offset=end;};source.cancel=[canceled](rt::Reason){*canceled=true;};rt::ByteStream stream(source);rt::Response upstream(stream,rt::ResponseInit{.headers=rt::Headers{{"Content-Type","application/json"},{"ETag","\"raw-oversized\""}}});rt::GlobalFetchOverride fetch(returning(upstream));auto pending=fetchModels();AWAIT(response,withinBudget(std::move(pending)));EXPECT(response==upstream);EXPECT_EQ(response.headers().get("ETag"),"\"raw-oversized\"");EXPECT(!response.bodyUsed());EXPECT(!response.body()->locked());EXPECT_EQ(*offset,BYTE_CAP+3*CHUNK);EXPECT(!*canceled);EXPECT(!hasAccount());EXPECT(unavailable());AWAIT(actual,response.bytes());EXPECT_EQ(actual,*content);co_await rt::microtask();EXPECT(!*canceled);
}
ASYNC_TEST("single-account catalog: complete evidence restores only the advertised account and model before return") {
  Fixture fixture;co_await seedRejection();auto absentTask=availability::recordCodexModelUnsupported(ACCOUNT,std::string("absent-model"));co_await std::move(absentTask);auto siblingTask=availability::recordCodexModelUnsupported(std::string("sibling"),MODEL);co_await std::move(siblingTask);const std::vector<availability::CodexAccountCatalogInput> entries{{.accountId="sibling",.clientVersion=VERSION,.slugs=json::array{"sibling-only"}}};auto record=availability::recordCodexAccountCatalogs(entries);co_await std::move(record);rt::Response upstream(CATALOG,rt::ResponseInit{.headers=rt::Headers{{"Content-Type","application/json"},{"ETag","\"raw-complete\""}}});rt::GlobalFetchOverride fetch(returning(upstream));AWAIT(response,fetchModels());EXPECT(response==upstream);EXPECT_EQ(response.headers().get("ETag"),"\"raw-complete\"");EXPECT(!unavailable());EXPECT(unavailable("absent-model"));EXPECT(unavailable(MODEL,"sibling"));const auto cache=availability::getCodexAccountModelsCacheForTest();ASSERT(cache.store);const auto* sibling=cache.store->accounts.get("sibling");ASSERT(sibling);EXPECT_EQ(sibling->slugs,(std::vector<std::string>{"sibling-only"}));AWAIT(text,response.text());EXPECT_EQ(text,CATALOG);
}
ASYNC_TEST("single-account catalog: 304, malformed, failed and errored bodies never create availability evidence") {
  for(int scenario=0;scenario<5;++scenario){Fixture fixture;co_await seedRejection();std::optional<rt::Response> upstream;if(scenario==0)upstream=rt::Response(nullptr,rt::ResponseInit{.status=304,.headers=rt::Headers{{"ETag","\"raw-not-modified\""}}});else if(scenario==1)upstream=rt::Response("not json");else if(scenario==2)upstream=rt::Response(R"({"data":[]})");else if(scenario==3)upstream=rt::Response(CATALOG,rt::ResponseInit{.status=500});else{rt::UnderlyingSource<rt::ByteChunk> source;source.start=[](auto controller){controller.error(rt::makeReason(rt::JsError("synthetic stream failure")));};rt::ByteStream stream(source);upstream=rt::Response(stream);}rt::GlobalFetchOverride fetch(returning(*upstream));AWAIT(response,fetchModels(std::string("\"raw-not-modified\"")));EXPECT(response==*upstream);EXPECT(unavailable());EXPECT(!hasAccount());try{auto read=response.text();auto text=co_await std::move(read);(void)text;}catch(...){}co_await rt::microtask();}
}
ASYNC_TEST("catalog observation preserves the optional full-snapshot read failure boundary") {
  Fixture fixture;co_await seedRejection();fixture.kv->put(uos::runtime_config::RUNTIME_CONFIG_V2_KEY,json::object{{"version",2},{"default_model",MODEL},{"default_reasoning_effort","none"},{"codex_models",json::object{{"source","chatgpt_codex"},{"client_version",VERSION},{"updated_at_ms",rt::dateNow()},{"models",json::array{json::object{{"slug",MODEL}}}}}},{"updated_at_ms",rt::dateNow()}});fixture.kv->rejectFull=true;AWAIT(optional,mf::loadFullCodexModelsSnapshot(std::shared_ptr<rt::Kv>(fixture.kv)));EXPECT(!optional);AWAIT(snapshot,uos::models::catalog::buildModelCatalogSnapshot());bool found=false;for(const auto& row:snapshot.models)found=found||row.id==MODEL;EXPECT(found);
}
ASYNC_TEST("bounded single-account observation keeps versioned client validators separate from upstream revalidation") {
  Fixture fixture;co_await seedRejection();const std::string rawEtag="\"upstream-bounded-catalog\"";std::optional<std::string> upstreamTag;rt::GlobalFetchOverride fetch(rt::Fetch([&upstreamTag,rawEtag](rt::RequestInfo,rt::RequestInit init){upstreamTag=init.headers->get("If-None-Match");return rt::Response(CATALOG,rt::ResponseInit{.headers=rt::Headers{{"Content-Type","application/json"},{"ETag",rawEtag}}});}));
  AWAIT(first,uos::catalog::index::handleCodexCatalogModels(request(),VERSION));EXPECT_EQ(first.status(),200);const auto clientEtag=first.headers().get("ETag");EXPECT(rt::Regex("^\"uos-catalog-[a-f0-9]{32}\"$").test(clientEtag.value_or("")));EXPECT(clientEtag!=rawEtag);AWAIT(firstText,first.text());EXPECT_EQ(firstText,CATALOG);const rt::KvKey key{"ubq_ai","codex_catalog",VERSION};AWAIT(entry,fixture.kv->get(key));auto metadata=entry.value.toJson().as_object();EXPECT_EQ(rt::getString(metadata,"etag"),rawEtag);metadata["fetched_at_ms"]=rt::dateNow()-310000;fixture.kv->put(key,metadata);uos::catalog::store::resetCodexCatalogMemoForTest();rt::GlobalFetchOverride revalidate(rt::Fetch([&upstreamTag,rawEtag](rt::RequestInfo,rt::RequestInit init){upstreamTag=init.headers->get("If-None-Match");return rt::Response(nullptr,rt::ResponseInit{.status=304,.headers=rt::Headers{{"ETag",rawEtag}}});}));
  AWAIT(matched,uos::catalog::index::handleCodexCatalogModels(request(clientEtag),VERSION));EXPECT_EQ(upstreamTag,rawEtag);EXPECT_EQ(matched.status(),304);EXPECT_EQ(matched.headers().get("ETag"),clientEtag);AWAIT(matchedText,matched.text());EXPECT_EQ(matchedText,"");const rt::KvKey lease{uos::catalog::types::CODEX_CATALOG_LEASE_PREFIX,VERSION};for(int attempts=0;attempts<200&&fixture.kv->rows.has(diff::keyOf(lease));++attempts)co_await rt::sleep(5);EXPECT(!fixture.kv->rows.has(diff::keyOf(lease)));
}
