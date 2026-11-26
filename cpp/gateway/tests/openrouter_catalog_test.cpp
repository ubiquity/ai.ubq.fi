// Links: tests/openrouter_catalog_test.cpp
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
// Port of all nine tests/openrouter-catalog.test.ts cases, in source order.
#include "uos/catalog/index.hpp"
#include "uos/catalog/models.hpp"
#include "uos/catalog/store.hpp"
#include "uos/catalog/types.hpp"
#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/deepseek/waterfall.hpp"
#include "uos/kv.hpp"
#include "uos/models/catalog.hpp"
#include "uos/models/codex_models_whitelist.hpp"
#include "uos/models/openrouter_models.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/gzip.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "tests/helpers/codex_diff_kv.hpp"
#include <memory>
#include <string>
#include <vector>
namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace ci = uos::catalog::index;
  namespace cs = uos::catalog::store;
  namespace ct = uos::catalog::types;
  namespace orouter = uos::models::openrouter_models;
  namespace selection = uos::provider::selection;
  namespace diff = uos::tests::helpers::codex_diff_kv;
  const std::string VERSION = "0.100.0";
  const std::string AUTH_GENERATION = "auth-generation-openrouter-catalog";
  const std::string WATERFALL(uos::deepseek::waterfall::DEEPSEEK_WATERFALL_MODEL_ID);
#define AWAIT(name, ...) auto name##Task = (__VA_ARGS__); auto name = co_await std::move(name##Task)
  class EmptyList final : public rt::KvListIterator {
    rt::Task<std::optional<rt::KvEntry<>>> next() override { co_await rt::microtask(); co_return std::nullopt; }
    std::string cursor() const override { return ""; }
    rt::Task<void> return_() override { co_return; }
  };
  class CatalogKv final : public diff::DiffKv {
  public:
    void put(const rt::KvKey& key, const rt::KvValue& value) { rows.set(diff::keyOf(key), Row{key, value, stamp()}); }
    using rt::Kv::get;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions = {}) override { auto value = entryOf(key); co_await rt::microtask(); co_return value; }
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions = {}) override {
      put(key, value); rt::KvCommitResult result{true, *entryOf(key).versionstamp}; co_await rt::microtask(); co_return result;
    }
    rt::Task<void> delete_(rt::KvKey key) override { rows.delete_(diff::keyOf(key)); co_await rt::microtask(); }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions = {}) override { return std::make_unique<EmptyList>(); }
    class Atomic final : public rt::ForwardingAtomicOperation {
    public:
      explicit Atomic(std::unique_ptr<rt::AtomicOperation> inner_) : rt::ForwardingAtomicOperation(std::move(inner_)) {}
      rt::Task<rt::KvCommitResult> commit() override { auto result = co_await inner->commit(); co_await rt::microtask(); co_return result; }
    };
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<Atomic>(diff::DiffKv::atomic()); }
  };
  rt::Fetch payloadFetch(json::value payload) { return rt::Fetch([payload](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(payload); }); }
  rt::Request request(const std::optional<std::string>& etag = std::nullopt) {
    rt::RequestInit init; init.headers = rt::Headers{{"Authorization", "Bearer gateway-client-token"}};
    if (etag) init.headers->set("If-None-Match", *etag);
    return rt::Request("https://ai.ubq.fi/v1/models?client_version=" + VERSION, init);
  }
  rt::Task<rt::Response> catalog(const std::optional<std::string>& etag = std::nullopt) { return ci::handleCodexCatalogModels(request(etag), VERSION); }
  json::array fields(const json::array& rows, std::string_view field) { json::array result; for (const auto& row : rows) { const auto* item = rt::get(row, field); result.push_back(item ? *item : json::value(nullptr)); } return result; }
  const json::value* model(const json::value& body, std::string_view slug) { for (const auto& row : body.at("models").as_array()) if (rt::getString(row, "slug") == slug) return &row; return nullptr; }
  bool has(const json::value& body, std::string_view slug) { return model(body, slug) != nullptr; }
  std::string etagFor(const std::string& body) { return "\"uos-catalog-" + rt::crypto::createHash("sha256").update(body).digest("hex").substr(0,32) + "\""; }
  rt::Task<void> setOpenRouter(json::array rows) { orouter::FetchOpenRouterModelsOptions options; options.force = true; options.fetcher = payloadFetch(json::object{{"data", rows}}); AWAIT(snapshot, orouter::fetchOpenRouterModels(options)); (void)snapshot; }
  rt::Task<void> servedCatalogue() {
    json::array rows{json::object{{"id", "vendor/alpha"}}, json::object{{"id", "vendor/beta"}, {"context_length", 32000}, {"top_provider", json::object{{"context_length",32000}}}, {"reasoning",json::object{{"supported_efforts",json::array{"high"}}, {"default_effort","high"}}}}};
    auto task = setOpenRouter(rows); co_await std::move(task);
  }
  rt::Fetch upstreamFetch() { return payloadFetch(json::object{{"models", json::array{json::object{{"slug","gpt-0.100.0"}}, json::object{{"slug","hidden-0.100.0"}}}}}); }
  rt::Task<void> leaseReleased(std::shared_ptr<CatalogKv> kv) {
    const rt::KvKey key{ct::CODEX_CATALOG_LEASE_PREFIX, VERSION};
    for (int attempt = 0; attempt < 200; ++attempt) { if (!kv->rows.has(diff::keyOf(key))) co_return; co_await rt::sleep(5); }
    throw rt::JsError("catalog refresh did not release its lease");
  }
  struct Fixture {
    rt::env::ScopedEnvironment env{std::vector<std::pair<std::string,std::string>>{{"HOME",rt::env::get("HOME").value_or("/nonexistent/w07c4-catalog-b")},{"TMPDIR",rt::env::get("TMPDIR").value_or("/tmp")},{"OPENROUTER_API_KEY","fixture-openrouter-key"}}};
    std::string previousAuth = uos::config::config.codexAuthJsonB64;
    std::shared_ptr<CatalogKv> kv = std::make_shared<CatalogKv>();
    explicit Fixture(const json::array& whitelist) {
      uos::config::config.codexAuthJsonB64.clear(); reset(); uos::kv::setKvForTest(kv);
      uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const auto&) {});
      kv->put(ct::CODEX_CATALOG_AUTH_GENERATION_KEY, AUTH_GENERATION);
      const auto now = rt::dateNow();
      const json::object account{{"access_token","server-access"},{"refresh_token","server-refresh"},{"account_id","server-account"},{"updated_at_ms",now}};
      kv->put(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY,json::object{{"accounts",json::array{account}},{"updated_at_ms",now}});
      setWhitelist(whitelist,1);
    }
    void setWhitelist(const json::array& ids, double updated) { kv->put(uos::models::codex_models_whitelist::CODEX_MODELS_WHITELIST_KV_KEY,json::object{{"model_ids",ids},{"updated_at_ms",updated}}); }
    void providers(const json::array& ids) { kv->put(selection::PROVIDER_SELECTION_KV_KEY,json::object{{"provider_ids",ids},{"updated_at_ms",1}}); selection::resetProviderSelectionCacheForTest(); }
    static void reset() { uos::codex::auth::resetCodexAuthCacheForTest(); cs::resetCodexCatalogMemoForTest(); orouter::resetOpenRouterModelsCacheForTest(); selection::resetProviderSelectionCacheForTest(); uos::runtime_config::resetRuntimeConfigCacheForTest(); uos::provider::metered::resetMeteredModelsCacheForTest(); uos::provider::surplus::resetSurplusModelsCacheForTest(); uos::provider::claude_code_models::resetClaudeCodeModelsCacheForTest(); uos::provider::health::resetProviderHealthThrottleForTest(); }
    ~Fixture() { uos::kv::setKvForTest(nullptr); reset(); uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest({}); uos::config::config.codexAuthJsonB64 = previousAuth; }
    rt::Task<bool> store(std::string body, std::optional<std::string> etag = std::nullopt, std::optional<double> fetched = std::nullopt) {
      cs::StoreCodexCatalogInput input{.clientVersion=VERSION,.authGeneration=AUTH_GENERATION,.body=std::move(body),.etag=etag,.contentType="application/json",.fetchedAtMs=fetched};
      auto task=cs::storeCodexCatalog(*kv,input); auto result=co_await std::move(task); co_return result;
    }
  };
}
ASYNC_TEST("codex catalog: the versioned catalog advertises the pooled union narrowed by the operator whitelist") {
  Fixture fixture(json::array{"pooled-shared"}); co_await servedCatalogue(); const auto now=rt::dateNow();
  const json::array accounts{json::object{{"access_token","pool-a-access"},{"refresh_token","pool-a-refresh"},{"account_id","pool-account-a"},{"updated_at_ms",now}},json::object{{"access_token","pool-b-access"},{"refresh_token","pool-b-refresh"},{"account_id","pool-account-b"},{"updated_at_ms",now}}};
  fixture.kv->put(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY,json::object{{"accounts",accounts},{"updated_at_ms",now}}); uos::codex::auth::resetCodexAuthCacheForTest();
  rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo,rt::RequestInit init) { const auto id=init.headers->get("ChatGPT-Account-ID"); json::array rows; if(id=="pool-account-a") rows.push_back(json::object{{"slug","gpt-daybreak-blue-latest"}}); rows.push_back(json::object{{"slug","pooled-shared"}}); return rt::Response::json(json::object{{"models",rows}}); }));
  AWAIT(hidden,catalog()); EXPECT_EQ(hidden.status(),200); AWAIT(hiddenBody,hidden.json()); EXPECT(has(hiddenBody,"pooled-shared")); EXPECT(!has(hiddenBody,"gpt-daybreak-blue-latest"));
  fixture.setWhitelist(json::array{"pooled-shared","gpt-daybreak-blue-latest"},2); AWAIT(enabled,catalog()); EXPECT_EQ(enabled.status(),200); AWAIT(enabledBody,enabled.json()); EXPECT(has(enabledBody,"gpt-daybreak-blue-latest"));
}
ASYNC_TEST("codex catalog: OpenRouter rows follow the operator whitelist and keep their snapshot metadata") {
  Fixture fixture(json::array{"gpt-0.100.0","vendor/beta"}); co_await servedCatalogue(); rt::GlobalFetchOverride fetch(upstreamFetch());
  AWAIT(response,catalog()); EXPECT_EQ(response.status(),200); AWAIT(payload,response.json()); EXPECT(has(payload,"gpt-0.100.0")); EXPECT(!has(payload,"hidden-0.100.0")); EXPECT(!has(payload,"vendor/alpha")); const auto* beta=model(payload,"vendor/beta"); ASSERT(beta);
  EXPECT(rt::deepEqual(beta->at("supported_endpoint_types"),json::array{"openai-response","openai-chat"})); EXPECT(rt::deepEqual(fields(beta->at("supported_reasoning_levels").as_array(),"effort"),json::array{"high"})); EXPECT_EQ(rt::getString(*beta,"default_reasoning_level"),"high"); EXPECT_EQ(rt::getNumber(*beta,"context_window"),32000);
}
ASYNC_TEST("codex catalog: an empty whitelist is no filter and OpenRouter rows still list") {
  Fixture fixture(json::array{}); co_await servedCatalogue(); rt::GlobalFetchOverride fetch(upstreamFetch()); AWAIT(response,catalog()); EXPECT_EQ(response.status(),200); AWAIT(payload,response.json()); EXPECT(rt::deepEqual(fields(payload.at("models").as_array(),"slug"),json::array{"gpt-0.100.0","hidden-0.100.0",WATERFALL,"vendor/alpha","vendor/beta"}));
}
ASYNC_TEST("codex catalog: the metered fallback omits disabled OpenRouter rows with an empty whitelist") {
  Fixture fixture(json::array{}); co_await servedCatalogue(); fixture.providers(json::array{"codex","deepseek"}); rt::env::set("DEEPSEEK_API_KEY","fixture-deepseek-key"); rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo,rt::RequestInit) { return rt::Response("unavailable",rt::ResponseInit{.status=503}); }));
  AWAIT(response,catalog()); EXPECT_EQ(response.status(),200); AWAIT(payload,response.json()); EXPECT(!has(payload,"vendor/alpha")); EXPECT(!has(payload,"vendor/beta")); EXPECT_EQ(response.headers().get("x-uos-upstream"),"metered");
}
ASYNC_TEST("codex catalog: an active whitelist filters a fresh stored-catalog hit instead of short-circuiting") {
  Fixture fixture(json::array{"gpt-0.100.0"}); co_await servedCatalogue(); fixture.providers(json::array{"codex"}); rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo,rt::RequestInit)->rt::Response { throw rt::JsError("a fresh stored-catalog hit must not reach upstream"); }));
  const std::string body=R"({"models":[{"slug":"gpt-0.100.0"},{"slug":"hidden-0.100.0"}]})"; AWAIT(stored,fixture.store(body,std::nullopt,rt::dateNow())); EXPECT(stored); AWAIT(response,catalog()); EXPECT_EQ(response.status(),200); EXPECT_EQ(response.headers().get("x-uos-cache"),"hit"); AWAIT(payload,response.json()); EXPECT(rt::deepEqual(fields(payload.at("models").as_array(),"slug"),json::array{"gpt-0.100.0"})); EXPECT(rt::Regex("^\"uos-catalog-").test(response.headers().get("ETag").value_or("")));
}
ASYNC_TEST("codex catalog: served-byte ETags change when OpenRouter enriches an unchanged upstream catalog") {
  Fixture fixture(json::array{}); co_await servedCatalogue(); fixture.providers(json::array{"codex","openrouter","ubiquity"}); rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo,rt::RequestInit)->rt::Response { throw rt::JsError("a fresh catalog must not reach upstream"); }));
  const std::string body=R"({"models":[{"slug":"gpt-stored"}]})"; orouter::resetOpenRouterModelsCacheForTest(); const json::array empty; auto emptyTask=setOpenRouter(empty); co_await std::move(emptyTask); AWAIT(stored,fixture.store(body,std::string("\"upstream-only\""),rt::dateNow())); EXPECT(stored);
  const std::string expected=rt::jsonStringify(json::object{{"models",json::array{json::object{{"slug","gpt-stored"}},uos::catalog::models::buildDeepSeekWaterfallCodexRecord()}}});
  AWAIT(first,uos::models::catalog::handleModels(request())); EXPECT_EQ(first.status(),200); AWAIT(firstText,first.text()); EXPECT_EQ(firstText,expected); const auto oldTag=first.headers().get("ETag").value_or(""); EXPECT_EQ(oldTag,etagFor(expected));
  const json::array beta{json::object{{"id","vendor/beta"},{"context_length",128000}}}; auto update=setOpenRouter(beta); co_await std::move(update); AWAIT(changed,uos::models::catalog::handleModels(request(oldTag))); EXPECT_EQ(changed.status(),200); AWAIT(changedBody,changed.text()); EXPECT(changedBody!=body); const auto changedTag=changed.headers().get("ETag").value_or(""); EXPECT_EQ(changedTag,etagFor(changedBody)); EXPECT(changedTag!=oldTag); const auto parsed=rt::jsonParse(changedBody); EXPECT_EQ(rt::getNumber(parsed.at("models").as_array()[2],"context_window"),128000);
  AWAIT(current,uos::models::catalog::handleModels(request(changedTag))); EXPECT_EQ(current.status(),304); EXPECT_EQ(current.headers().get("ETag"),changedTag); AWAIT(currentText,current.text()); EXPECT_EQ(currentText,"");
}
ASYNC_TEST("codex catalog: revalidation reapplies narrower OpenRouter windows to the source body") {
  Fixture fixture(json::array{"gpt-6-astra"}); co_await servedCatalogue(); double now=rt::dateNow(); rt::globalDateNow()=[&now]{return now;}; orouter::resetOpenRouterModelsCacheForTest();
  const json::array wide{json::object{{"id","openai/gpt-6-astra"},{"context_length",1050000}}}; auto wideTask=setOpenRouter(wide); co_await std::move(wideTask);
  const std::string source=R"({"models":[{"slug":"gpt-6-astra","context_window":272000,"max_context_window":872000}]})"; AWAIT(stored,fixture.store(source,std::string("\"catalog-etag\""),now-ct::CODEX_CATALOG_FRESH_MS-1)); EXPECT(stored);
  std::optional<std::string> upstreamTag; rt::GlobalFetchOverride fetch(rt::Fetch([&upstreamTag](rt::RequestInfo,rt::RequestInit init) { upstreamTag=init.headers->get("If-None-Match"); return rt::Response(nullptr,rt::ResponseInit{.status=304,.headers=rt::Headers{{"ETag","\"catalog-etag\""}}}); }));
  AWAIT(first,catalog()); AWAIT(firstBody,first.json()); const auto& firstModel=firstBody.at("models").as_array()[0]; EXPECT_EQ(rt::getNumber(firstModel,"context_window"),1050000); EXPECT_EQ(rt::getNumber(firstModel,"max_context_window"),1050000); EXPECT_EQ(rt::getNumber(firstModel,"auto_compact_token_limit"),945000); auto firstLease=leaseReleased(fixture.kv); co_await std::move(firstLease); EXPECT_EQ(upstreamTag,"\"catalog-etag\""); AWAIT(loaded,cs::loadCatalog(*fixture.kv,VERSION,AUTH_GENERATION,now)); ASSERT(loaded); AWAIT(storedSource,cs::loadCatalogSource(*fixture.kv,loaded)); EXPECT_EQ(storedSource,source); EXPECT_EQ(loaded->metadata.etag,"\"catalog-etag\"");
  const json::array narrow{json::object{{"id","openai/gpt-6-astra"},{"context_length",128000}}}; auto narrowTask=setOpenRouter(narrow); co_await std::move(narrowTask); now+=ct::CODEX_CATALOG_FRESH_MS+1; AWAIT(stale,catalog()); EXPECT_EQ(stale.headers().get("x-uos-cache"),"stale"); auto secondLease=leaseReleased(fixture.kv); co_await std::move(secondLease); AWAIT(second,catalog()); AWAIT(secondBody,second.json()); const auto& secondModel=secondBody.at("models").as_array()[0]; EXPECT_EQ(rt::getNumber(secondModel,"context_window"),872000); EXPECT_EQ(rt::getNumber(secondModel,"max_context_window"),872000); EXPECT_EQ(rt::getNumber(secondModel,"auto_compact_token_limit"),784800);
}
ASYNC_TEST("codex catalog: stored widened GPT rows carry the earliest source or policy compaction limit") {
  Fixture fixture(json::array{"gpt-6-astra","gpt-6.1-sol"}); co_await servedCatalogue(); const json::array wide{json::object{{"id","openai/gpt-6-astra"},{"context_length",1050000}},json::object{{"id","openai/gpt-6.1-sol"},{"context_length",1050000}}}; auto wideTask=setOpenRouter(wide); co_await std::move(wideTask);
  const std::vector<std::optional<double>> limits{std::nullopt,200000,990000}; for(const auto sourceLimit:limits) { json::array rows; for(const auto& slug:std::vector<std::string>{"gpt-6-astra","gpt-6.1-sol"}) { json::object row{{"slug",slug},{"context_window",272000},{"max_context_window",872000}}; if(sourceLimit) row["auto_compact_token_limit"]=*sourceLimit; rows.push_back(row); } const auto source=rt::jsonStringify(json::object{{"models",rows}}); AWAIT(stored,fixture.store(source,std::string("\"original\""))); EXPECT(stored); AWAIT(response,catalog()); AWAIT(payload,response.json()); EXPECT_EQ(payload.at("models").as_array().size(),2u); for(const auto& row:payload.at("models").as_array()) { EXPECT_EQ(rt::getNumber(row,"context_window"),1050000); EXPECT_EQ(rt::getNumber(row,"auto_compact_token_limit"),sourceLimit==200000?200000:945000); } AWAIT(loaded,cs::loadCatalog(*fixture.kv,VERSION,AUTH_GENERATION,rt::dateNow())); ASSERT(loaded); AWAIT(storedSource,cs::loadCatalogSource(*fixture.kv,loaded)); EXPECT_EQ(storedSource,source); EXPECT_EQ(loaded->metadata.etag,"\"original\""); }
}
ASYNC_TEST("codex catalog: existing cached widened rows acquire policy limits before client validation") {
  Fixture fixture(json::array{"gpt-6-astra","gpt-6.1-sol"}); co_await servedCatalogue(); const std::string source=R"({"models":[{"slug":"gpt-6-astra","context_window":272000,"max_context_window":872000},{"slug":"gpt-6.1-sol","context_window":272000,"max_context_window":872000}]})"; AWAIT(stored,fixture.store(source,std::string("\"original\""))); EXPECT(stored);
  const rt::KvKey key{ct::CODEX_CATALOG_PREFIX,VERSION}; AWAIT(entry,fixture.kv->get(key)); auto metadata=entry.value.toJson().as_object(); const auto generation=rt::getString(metadata,"body_generation").value(); const std::string legacy=R"({"models":[{"slug":"gpt-6-astra","context_window":1050000,"max_context_window":1050000},{"slug":"gpt-6.1-sol","context_window":1050000,"max_context_window":1050000}]})"; const auto compressed=rt::gzip::gzipSync(rt::encodeUtf8(legacy)); fixture.kv->put(rt::KvKey{ct::CODEX_CATALOG_CHUNK_PREFIX,VERSION,generation,0},compressed); metadata["chunk_count"]=1; metadata["compressed_bytes"]=compressed.size(); metadata["body_bytes"]=legacy.size(); metadata["sha256"]=rt::crypto::createHash("sha256").update(legacy).digest("hex"); fixture.kv->put(key,metadata); cs::resetCodexCatalogMemoForTest(); rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo,rt::RequestInit)->rt::Response { throw rt::JsError("fresh cache must not reach upstream"); }));
  const auto oldTag=etagFor(legacy); AWAIT(response,catalog(oldTag)); EXPECT_EQ(response.status(),200); AWAIT(body,response.text()); const auto parsed=rt::jsonParse(body); EXPECT(rt::deepEqual(fields(parsed.at("models").as_array(),"auto_compact_token_limit"),json::array{945000,945000})); const auto newTag=etagFor(body); EXPECT_EQ(response.headers().get("ETag"),newTag); EXPECT(newTag!=oldTag); AWAIT(loaded,cs::loadCatalog(*fixture.kv,VERSION,AUTH_GENERATION,rt::dateNow())); ASSERT(loaded); AWAIT(storedSource,cs::loadCatalogSource(*fixture.kv,loaded)); EXPECT_EQ(storedSource,source); EXPECT_EQ(loaded->metadata.etag,"\"original\""); AWAIT(unchanged,catalog(newTag)); EXPECT_EQ(unchanged.status(),304);
}
