// Links: tests/codex_catalog_test.cpp
// tests/helpers/codex_diff_kv.cpp src/models/metadata.cpp src/models/catalog.cpp src/utils.cpp
// src/provider/lithos.cpp src/provider/selection.cpp src/provider/surplus.cpp src/provider/metered.cpp
// src/input_normalization.cpp src/provider/cerebras.cpp src/models/openrouter_models.cpp src/models/codex_models_whitelist.cpp
// src/provider/anthropic_responses.cpp src/codex/auth.cpp src/types.cpp src/config.cpp
// src/models/codex_models.cpp src/http.cpp src/codex/capacity_routing.cpp src/provider/anthropic_response_wire.cpp
// src/codex/native_auth.cpp src/catalog/index.cpp src/authority/native_codex_capacity.cpp src/catalog/models.cpp
// src/catalog/store.cpp src/auth/relay.cpp src/catalog/types.cpp src/request_policy.cpp
// src/authority/native_codex_contracts.cpp src/provider/claude_code_models.cpp src/defaults.cpp src/models/codex_models_fetch.cpp
// src/provider/surplus_claude_request.cpp src/provider/anthropic_replay.cpp src/models/codex_models_availability.cpp src/provider/claude_code_accounts.cpp
// src/codex/auth_refresh.cpp src/codex/routing_mutations.cpp src/runtime_config.cpp src/release.cpp
// src/responses_stream.cpp src/authority/native_codex_mutations.cpp src/codex/external_auth.cpp src/provider/claude_code_credentials.cpp
// src/codex/dispatch.cpp src/provider/surplus_claude_chat_request.cpp src/codex/native_auth_transport.cpp src/authority/native_codex_evaluation.cpp
// src/codex/rate_limit_429.cpp src/openai_telemetry.cpp src/deepseek/waterfall.cpp src/codex/routing_state.cpp
// src/kv.cpp src/bounded_response_body.cpp src/recent_model_context.cpp src/provider/health.cpp
// src/provider/openrouter.cpp src/deepseek/index.cpp src/provider/lithos_rate_limits.cpp src/provider/anthropic_chat_responses.cpp
// src/responses_failover_stream.cpp src/provider/capacity_events.cpp src/authority/native_codex_429.cpp src/provider/cerebras_rate_limits.cpp
// src/runtime/websocket.cpp src/codex/banked_reset.cpp src/provider/claude_code.cpp src/provider/claude_code_quota.cpp
// src/provider/claude_code_usage_schema.cpp src/codex/responses_error.cpp src/provider/capacity_contract.cpp src/provider/capacity.cpp
// src/provider/removed_provider_telemetry.cpp src/provider/capacity_parse.cpp src/storage/kv.cpp src/provider/claude_capacity_history.cpp
// src/codex/banked_reset_provider.cpp src/codex/reset_settings.cpp src/provider/capacity_rollups.cpp src/provider/claude_capacity_rollups.cpp
// src/cache/prompt_analytics_core.cpp src/provider/capacity_history.cpp src/codex/experiment.cpp src/paid_fallback/ledger_stats.cpp
// src/codex/account_routing.cpp src/codex/routing_serial.cpp src/provider/anthropic_chat_stream.cpp src/authority/native_codex.cpp
// src/codex/routing_evaluation.cpp src/storage/sqlite_readonly.cpp src/metered_quota.cpp
// src/provider/capacity_analytics.cpp src/cache/prompt_analytics_read.cpp src/storage/native_kv.cpp src/codex/overage_settings.cpp
// Port of every original assertion in tests/codex-catalog.test.ts, in source case order.
// The two predecessor MemoryKv store cases are preserved verbatim; other cases use the source CAS fixture.
#include "uos/catalog/store.hpp"
#include "uos/catalog/types.hpp"
#include "uos/models/openrouter_models.hpp"
#include "tests/helpers/codex_diff_kv.hpp"
#include "uos/catalog/index.hpp"
#include "uos/models/catalog.hpp"
#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/runtime_config.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/metered.hpp"
#include "uos/provider/selection.hpp"
#include "uos/provider/surplus.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/string.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <algorithm>
#include <functional>
#include <string>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::catalog::store;
using namespace uos::catalog::types;
#define AWAIT(name, ...) auto name##Task = (__VA_ARGS__); auto name = co_await std::move(name##Task)

namespace {
  const std::string AUTH_GENERATION = "auth-generation-test";

  rt::KvKey catalogKey(const std::string& version) { return rt::KvKey{"ubq_ai", "codex_catalog", version}; }

  // catalogBody(version, extra)
  std::string catalogBody(const std::string& version, const json::object& extra = {}) {
    json::object level;
    level["effort"] = "high";
    level["description"] = "deep";
    json::object model;
    model["slug"] = "gpt-" + version;
    model["display_name"] = "Rich " + version;
    model["base_instructions"] = "must remain untouched";
    model["supported_reasoning_levels"] = json::array{level};
    json::object body;
    body["models"] = json::array{model};
    for (const json::key_value_pair& member : extra) rt::objectDefine(body, member.key(), member.value());
    return rt::jsonStringify(json::value(body));
  }

  json::object generation(std::string_view value) {
    json::object extra;
    extra["generation"] = value;
    return extra;
  }

  struct Store {
    std::shared_ptr<rt::MemoryKvDatabase> database;
    std::shared_ptr<rt::MemoryKv> kv;
  };

  // seedBaseState(), reduced as the header says.
  rt::Task<Store> seedBaseState() {
    resetCodexCatalogMemoForTest();
    uos::models::openrouter_models::resetOpenRouterModelsCacheForTest();
    auto database = std::make_shared<rt::MemoryKvDatabase>([] { return rt::dateNow(); });
    auto kv = std::make_shared<rt::MemoryKv>(database);
    const rt::KvValue value(AUTH_GENERATION);
    co_await kv->set(CODEX_CATALOG_AUTH_GENERATION_KEY, value);
    Store seeded;
    seeded.database = std::move(database);
    seeded.kv = std::move(kv);
    co_return seeded;
  }

  bool has(const Store& store, const rt::KvKey& key) { return store.database->row(key).has_value(); }

  std::vector<rt::KvKey> chunkKeys(const std::string& version, const std::string& bodyGeneration, double chunkCount) {
    std::vector<rt::KvKey> keys;
    for (double index = 0; index < chunkCount; index += 1) keys.push_back(rt::KvKey{CODEX_CATALOG_CHUNK_PREFIX, version, bodyGeneration, index});
    return keys;
  }

  // Rows under [...CODEX_CATALOG_CHUNK_PREFIX, version].
  std::size_t chunkRows(const Store& store, const std::string& version) {
    const rt::KvKey prefix{CODEX_CATALOG_CHUNK_PREFIX, version};
    std::size_t count = 0;
    for (const rt::MemoryKvDatabase::Row& row : store.database->rows()) {
      if (row.key.size() < prefix.size()) continue;
      bool matches = true;
      for (std::size_t index = 0; index < prefix.size(); ++index) matches = matches && row.key[index] == prefix[index];
      if (matches) count += 1;
    }
    return count;
  }
}

namespace {
  namespace ci = uos::catalog::index;
  namespace metered = uos::provider::metered;
  namespace surplus = uos::provider::surplus;
  namespace openrouter = uos::models::openrouter_models;
  namespace rc = uos::runtime_config;
  namespace diff = uos::tests::helpers::codex_diff_kv;
  const rt::KvKey AUTH_KEY{"ubq_ai", "codex_auth"};
  const rt::KvKey SNAPSHOT_KEY{"ubq_ai", "codex_models"};

  bool matchesPrefix(const rt::KvKey& key, const rt::KvKey& prefix) {
    return key.size() >= prefix.size() && std::equal(prefix.begin(), prefix.end(), key.begin());
  }
  class CatalogKv final : public diff::DiffKv {
  public:
    std::function<void(const std::vector<rt::KvKey>&)> beforeAtomicCommit;
    void put(const rt::KvKey& key, const rt::KvValue& value) { rows.set(diff::keyOf(key), Row{key, value, stamp()}); }
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions = {}) override {
      auto result = entryOf(key); co_await rt::microtask(); co_return result;
    }
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions = {}) override {
      std::vector<rt::KvEntryMaybe<>> result; for (const auto& key : keys) result.push_back(entryOf(key));
      co_await rt::microtask(); co_return result;
    }
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions = {}) override {
      put(key, value); rt::KvCommitResult result{true, *entryOf(key).versionstamp}; co_await rt::microtask(); co_return result;
    }
    rt::Task<void> delete_(rt::KvKey key) override { rows.delete_(diff::keyOf(key)); co_await rt::microtask(); }
    class Iterator final : public rt::KvListIterator {
    public:
      Iterator(CatalogKv& owner_, rt::KvKey prefix_) : owner(owner_), prefix(std::move(prefix_)) {}
      rt::Task<std::optional<rt::KvEntry<>>> next() override {
        co_await rt::microtask();
        if (!started) { started = true; for (const auto& [encoded, row] : owner.rows) { (void)encoded; if (matchesPrefix(row.key, prefix)) entries.push_back({row.key, row.value, row.versionstamp}); } }
        std::optional<rt::KvEntry<>> result; if (index < entries.size()) result = entries[index++]; co_return result;
      }
      std::string cursor() const override { return ""; }
      rt::Task<void> return_() override { index = entries.size(); co_return; }
    private:
      CatalogKv& owner; rt::KvKey prefix; std::vector<rt::KvEntry<>> entries; std::size_t index = 0; bool started = false;
    };
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions = {}) override {
      return std::make_unique<Iterator>(*this, selector.prefix.value_or(rt::KvKey{}));
    }
    class Atomic final : public rt::ForwardingAtomicOperation {
    public:
      Atomic(CatalogKv& owner_, std::unique_ptr<rt::AtomicOperation> inner_) : rt::ForwardingAtomicOperation(std::move(inner_)), owner(owner_) {}
      rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions options = {}) override {
        keys.push_back(key); inner->set(std::move(key), std::move(value), options); return *this;
      }
      rt::AtomicOperation& delete_(rt::KvKey key) override { keys.push_back(key); inner->delete_(std::move(key)); return *this; }
      rt::Task<rt::KvCommitResult> commit() override {
        if (owner.beforeAtomicCommit) { auto callback = owner.beforeAtomicCommit; callback(keys); }
        auto task = inner->commit(); auto result = co_await std::move(task); co_await rt::microtask(); co_return result;
      }
    private:
      CatalogKv& owner; std::vector<rt::KvKey> keys;
    };
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<Atomic>(*this, diff::DiffKv::atomic()); }
  };
  json::object parsedObject(std::string_view text) { return rt::jsonParse(text).as_object(); }
  rt::Response jsonResponse(const json::value& payload) { return rt::Response::json(payload); }
  rt::Response catalogResponse(const std::string& body, int status = 200, const rt::Headers& extra = {}) {
    rt::ResponseInit init; init.status = status; init.headers = extra; init.headers->set("Content-Type", "application/json"); return rt::Response(body, init);
  }
  rt::Request request(const std::string& version, const rt::Headers& extra = {}) {
    rt::Headers headers{{"Authorization", "Bearer gateway-client-token"}, {"Cookie", "incoming=secret"}};
    for (const auto& [name, value] : extra) headers.set(name, value);
    rt::RequestInit init; init.headers = headers; return rt::Request("https://ai.ubq.fi/v1/models?client_version=" + version, init);
  }
  rt::Task<rt::Response> catalog(const std::string& version) { return ci::handleCodexCatalogModels(request(version), version); }
  rt::Task<rt::Response> catalog(const std::string& version, const rt::Headers& headers) { return ci::handleCodexCatalogModels(request(version, headers), version); }
  rt::Task<rt::Response> catalog(const std::string& version, const ci::CodexCatalogDependencies& dependencies) { return ci::handleCodexCatalogModels(request(version), version, dependencies); }
  std::string fetchUrl(const rt::RequestInfo& input) { return std::visit([](const auto& item) { using T = std::decay_t<decltype(item)>; if constexpr (std::is_same_v<T, std::string>) return item; else if constexpr (std::is_same_v<T, rt::URL>) return item.href(); else return item.url(); }, input); }
  rt::Fetch versionFetcher() { return rt::Fetch([](rt::RequestInfo input, rt::RequestInit) { const auto version = rt::URL(fetchUrl(input)).searchParams().get("client_version").value_or("missing"); return catalogResponse(catalogBody(version)); }); }
  json::array fieldValues(const json::array& rows, std::string_view name) { json::array result; for (const auto& row : rows) { const auto* value = rt::get(row, name); result.push_back(value ? *value : json::value(nullptr)); } return result; }
  const json::object* findModel(const json::value& payload, std::string_view slug) { const auto* rows = rt::getArray(payload, "models"); if (rows) for (const auto& row : *rows) if (rt::getString(row, "slug") == slug) return row.if_object(); return nullptr; }
  json::array modelIds(const auto& snapshot) { json::array ids; if (snapshot) for (const auto& model : snapshot->models) ids.push_back(json::value(model.id)); return ids; }
  rt::Fetch payloadFetcher(const json::value& payload) { return rt::Fetch([payload](rt::RequestInfo, rt::RequestInit) { return jsonResponse(payload); }); }
  rt::Task<void> waitForCatalogLeaseRelease(std::shared_ptr<CatalogKv> kv, std::string version) {
    const rt::KvKey key{CODEX_CATALOG_LEASE_PREFIX, version};
    for (int attempt = 0; attempt < 200; ++attempt) { if (!kv->rows.has(diff::keyOf(key))) co_return; co_await rt::sleep(5); }
    throw rt::JsError("catalog refresh did not release its lease");
  }
  struct CatalogFixture {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c4-catalog"}}};
    std::string savedAuth = uos::config::config.codexAuthJsonB64;
    std::shared_ptr<CatalogKv> kv = std::make_shared<CatalogKv>();
    explicit CatalogFixture(const std::string& snapshotVersion = "0.200.0") {
      uos::config::config.codexAuthJsonB64.clear(); reset(); uos::kv::setKvForTest(kv);
      uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const auto&) {});
      openrouter::setOpenRouterModelsFetchForTest(payloadFetcher(json::object{{"data", json::array{}}}));
      kv->put(CODEX_CATALOG_AUTH_GENERATION_KEY, rt::KvValue(AUTH_GENERATION));
      const auto now = rt::dateNow();
      json::object account{{"access_token", "server-access"}, {"refresh_token", "server-refresh"}, {"account_id", "server-account"}, {"updated_at_ms", now}};
      kv->put(AUTH_KEY, json::object{{"accounts", json::array{account}}, {"updated_at_ms", now}});
      json::object snapshot{{"source", "chatgpt_codex"}, {"client_version", snapshotVersion}, {"updated_at_ms", now}, {"models", json::array{json::object{{"slug", "snapshot-" + snapshotVersion}}, json::object{{"slug", "gpt-6-astra"}, {"context_window", 272000}, {"max_context_window", 872000}, {"supported_reasoning_levels", json::array{"low", "medium", "high", "xhigh", "max", "ultra"}}}}}};
      seedSnapshot(snapshot, "snapshot-" + snapshotVersion, "medium");
    }
    void seedSnapshot(const json::object& snapshot, const std::string& model, const std::string& effort) {
      kv->put(SNAPSHOT_KEY, snapshot); kv->put(rc::RUNTIME_CONFIG_V2_KEY, json::object{{"version", 2}, {"default_model", model}, {"default_reasoning_effort", effort}, {"codex_models", snapshot}, {"updated_at_ms", rt::dateNow()}}); rc::resetRuntimeConfigCacheForTest();
    }
    static void reset() {
      uos::codex::auth::resetCodexAuthCacheForTest(); resetCodexCatalogMemoForTest(); rc::resetRuntimeConfigCacheForTest(); openrouter::resetOpenRouterModelsCacheForTest();
      metered::resetMeteredModelsCacheForTest(); surplus::resetSurplusModelsCacheForTest(); uos::provider::selection::resetProviderSelectionCacheForTest(); uos::provider::claude_code_models::resetClaudeCodeModelsCacheForTest(); uos::provider::health::resetProviderHealthThrottleForTest();
    }
    ~CatalogFixture() { uos::kv::setKvForTest(nullptr); reset(); metered::setMeteredModelsFetchForTest({}); openrouter::setOpenRouterModelsFetchForTest({}); uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest({}); uos::config::config.codexAuthJsonB64 = savedAuth; }
    rt::Task<bool> store(const std::string& version, const std::string& body, std::optional<double> fetchedAt = std::nullopt, std::optional<std::string> etag = std::nullopt) {
      StoreCodexCatalogInput input{.clientVersion = version, .authGeneration = AUTH_GENERATION, .body = body, .etag = etag, .fetchedAtMs = fetchedAt}; return storeCodexCatalog(*kv, input);
    }
  };
}

ASYNC_TEST("codex catalog: unversioned models retain the exact OpenAI list shape") {
  CatalogFixture fixture; rt::Request req("https://ai.ubq.fi/v1/models");
  AWAIT(response, uos::models::catalog::handleModels(req)); EXPECT_EQ(response.status(), 200);
  AWAIT(payload, response.json()); auto keys = rt::objectKeys(payload.as_object()); rt::sort(keys); EXPECT_EQ(keys, (std::vector<std::string>{"data", "object"})); EXPECT_EQ(rt::getString(payload, "object"), "list");
}
ASYNC_TEST("codex catalog: exact versions preserve rich JSON, isolate caches, and derive client ETags") {
  CatalogFixture fixture("0.100.0"); struct Call { std::string url; rt::Headers headers; }; std::vector<Call> calls;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo input, rt::RequestInit init) {
    const rt::Request upstream(fetchUrl(input), init); calls.push_back({upstream.url(), upstream.headers()}); const auto version = rt::URL(upstream.url()).searchParams().get("client_version").value_or("missing");
    return catalogResponse(catalogBody(version, json::object{{"version_marker", version}}), 200, rt::Headers{{"ETag", "\"" + version + "\""}});
  }));
  AWAIT(first, catalog("0.144.3")); EXPECT_EQ(first.status(), 200); const auto etag = first.headers().get("etag").value_or(""); EXPECT(rt::Regex("^\"uos-catalog-[a-f0-9]{32}\"$").test(etag));
  AWAIT(firstBody, first.json()); EXPECT(rt::deepEqual(firstBody, rt::jsonParse(catalogBody("0.144.3", json::object{{"version_marker", "0.144.3"}})))); ASSERT_EQ(calls.size(), 1u);
  EXPECT_EQ(calls[0].url, "https://chatgpt.com/backend-api/codex/models?client_version=0.144.3"); EXPECT(!calls[0].headers.get("cookie")); EXPECT(calls[0].headers.get("authorization") != "Bearer gateway-client-token"); EXPECT(rt::Regex("codex_cli_rs/0\\.144\\.3").test(calls[0].headers.get("user-agent").value_or("")));
  AWAIT(hit, catalog("0.144.3")); EXPECT_EQ(hit.headers().get("x-uos-cache"), "hit"); EXPECT_EQ(calls.size(), 1u);
  AWAIT(second, catalog("0.145.0")); EXPECT_EQ(second.status(), 200); EXPECT_EQ(calls.size(), 2u); AWAIT(secondBody, second.json()); EXPECT_EQ(rt::getString(secondBody, "version_marker"), "0.145.0");
  const rt::Headers headers{{"If-None-Match", etag}}; AWAIT(notModified, catalog("0.144.3", headers)); EXPECT_EQ(notModified.status(), 304); AWAIT(empty, notModified.text()); EXPECT_EQ(empty, ""); EXPECT_EQ(calls.size(), 2u);
}
ASYNC_TEST("codex catalog: malformed versions are rejected before upstream access") {
  CatalogFixture fixture; rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("invalid versions must not fetch"); }));
  const std::vector<std::string> versions{"0.144", "v0.144.3", "0.144.3.1", "0.144.x", ""}; for (const auto& version : versions) { AWAIT(response, catalog(version)); EXPECT_EQ(response.status(), 400); }
}
ASYNC_TEST("codex catalog: stale catalogs survive refresh failures but expired catalogs do not") {
  CatalogFixture fixture; const std::string version = "0.150.0"; const auto now = rt::dateNow();
  AWAIT(stored, fixture.store(version, catalogBody(version), now - CODEX_CATALOG_FRESH_MS - 1, "\"stale\"")); EXPECT(stored);
  std::vector<double> signals; ci::CodexCatalogDependencies dependencies; dependencies.now = [now] { return now; }; dependencies.recordSentinelDegradation = [&](double timestamp) { signals.push_back(timestamp); return rt::Promise<bool>::reject(rt::makeReason(rt::JsError("sentinel unavailable"))).wait(); };
  rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) { rt::ResponseInit init; init.status = 503; return rt::Response("temporary failure", init); }));
  AWAIT(stale, catalog(version, dependencies)); EXPECT_EQ(stale.status(), 200); EXPECT_EQ(stale.headers().get("x-uos-cache"), "stale"); AWAIT(payload, stale.json()); EXPECT_EQ(rt::getString(payload.at("models").as_array()[0], "slug"), "gpt-" + version);
  co_await waitForCatalogLeaseRelease(fixture.kv, version); EXPECT_EQ(signals, std::vector<double>{now});
  auto* metadata = fixture.kv->rows.get(diff::keyOf(catalogKey(version))); ASSERT(metadata); auto value = metadata->value.toJson(); value.as_object()["fetched_at_ms"] = now - CODEX_CATALOG_RETENTION_MS - 1; metadata->value = rt::KvValue(value);
  AWAIT(expired, catalog(version, dependencies)); EXPECT_EQ(expired.status(), 502); EXPECT_EQ(signals, (std::vector<double>{now, now}));
}
ASYNC_TEST("codex catalog: stale catalogs signal masked upstream transport failures") {
  CatalogFixture fixture; const std::string version = "0.150.2"; const auto now = rt::dateNow(); AWAIT(stored, fixture.store(version, catalogBody(version), now - CODEX_CATALOG_FRESH_MS - 1)); EXPECT(stored);
  std::vector<double> signals; ci::CodexCatalogDependencies dependencies; dependencies.now = [now] { return now; }; dependencies.recordSentinelDegradation = [&](double timestamp) { signals.push_back(timestamp); return rt::fulfilledTask(true); };
  rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("connection reset"); }));
  AWAIT(response, catalog(version, dependencies)); EXPECT_EQ(response.status(), 200); EXPECT_EQ(response.headers().get("x-uos-cache"), "stale"); co_await waitForCatalogLeaseRelease(fixture.kv, version); EXPECT_EQ(signals, std::vector<double>{now});
}
ASYNC_TEST("codex catalog: account fallback signals the masked transport failure once") {
  CatalogFixture fixture; const std::string version = "0.150.3"; const auto now = rt::dateNow(); auto pool = fixture.kv->row(AUTH_KEY); pool.at("accounts").as_array().push_back(json::object{{"access_token", "fallback-access"}, {"refresh_token", "fallback-refresh"}, {"account_id", "fallback-account"}, {"updated_at_ms", now}}); fixture.kv->put(AUTH_KEY, pool);
  int calls = 0; std::vector<double> signals; ci::CodexCatalogDependencies dependencies; dependencies.now = [now] { return now; }; dependencies.recordSentinelDegradation = [&](double timestamp) { signals.push_back(timestamp); return rt::fulfilledTask(true); };
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { if (++calls == 1) throw rt::JsError("first account connection reset"); return catalogResponse(catalogBody(version)); }));
  AWAIT(response, catalog(version, dependencies)); EXPECT_EQ(response.status(), 200); EXPECT_EQ(response.headers().get("x-uos-cache"), "miss"); EXPECT_EQ(calls, 2); EXPECT_EQ(signals, std::vector<double>{now});
}
ASYNC_TEST("codex catalog: stale catalogs signal response body transport failures") {
  CatalogFixture fixture; const std::string version = "0.150.4"; const auto now = rt::dateNow(); AWAIT(stored, fixture.store(version, catalogBody(version), now - CODEX_CATALOG_FRESH_MS - 1)); EXPECT(stored);
  std::vector<double> signals; ci::CodexCatalogDependencies dependencies; dependencies.now = [now] { return now; }; dependencies.recordSentinelDegradation = [&](double timestamp) { signals.push_back(timestamp); return rt::fulfilledTask(true); };
  rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) { rt::UnderlyingSource<rt::ByteChunk> source; source.start = [](auto controller) { controller.error(rt::makeReason(rt::JsError("body socket reset"))); }; rt::ByteStream stream(source); rt::ResponseInit init; init.status = 200; init.headers = rt::Headers{{"Content-Type", "application/json"}}; return rt::Response(stream, init); }));
  AWAIT(response, catalog(version, dependencies)); EXPECT_EQ(response.status(), 200); EXPECT_EQ(response.headers().get("x-uos-cache"), "stale"); co_await waitForCatalogLeaseRelease(fixture.kv, version); EXPECT_EQ(signals, std::vector<double>{now});
}
ASYNC_TEST("codex catalog: stale upstream ETags revalidate upstream without validating client bytes") {
  CatalogFixture fixture; const std::string version = "0.150.1"; AWAIT(stored, fixture.store(version, catalogBody(version), rt::dateNow() - CODEX_CATALOG_FRESH_MS - 1, "\"catalog-etag\"")); EXPECT(stored);
  std::optional<std::string> upstreamTag; rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo input, rt::RequestInit init) { const rt::Request upstream(fetchUrl(input), init); upstreamTag = upstream.headers().get("If-None-Match"); rt::ResponseInit ri; ri.status = 304; ri.headers = rt::Headers{{"ETag", "\"catalog-etag\""}}; return rt::Response(nullptr, ri); }));
  const rt::Headers headers{{"If-None-Match", "\"catalog-etag\""}}; AWAIT(response, catalog(version, headers)); EXPECT_EQ(response.status(), 200); EXPECT_EQ(response.headers().get("x-uos-cache"), "stale"); AWAIT(body, response.text()); EXPECT_EQ(body, catalogBody(version)); co_await waitForCatalogLeaseRelease(fixture.kv, version); EXPECT_EQ(upstreamTag, "\"catalog-etag\"");
}
ASYNC_TEST("codex catalog: refresh leases suppress duplicate upstream requests") {
  CatalogFixture fixture; const std::string version = "0.151.0"; AWAIT(stored, fixture.store(version, catalogBody(version), rt::dateNow() - CODEX_CATALOG_FRESH_MS - 1)); EXPECT(stored);
  fixture.kv->put(rt::KvKey{CODEX_CATALOG_LEASE_PREFIX, version}, json::object{{"owner", "other-request"}, {"lease_until_ms", rt::dateNow() + 10000}});
  int calls = 0, signals = 0; ci::CodexCatalogDependencies dependencies; dependencies.recordSentinelDegradation = [&](double) { ++signals; return rt::fulfilledTask(true); }; rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++calls; return catalogResponse(catalogBody(version)); }));
  AWAIT(response, catalog(version, dependencies)); EXPECT_EQ(response.status(), 200); EXPECT_EQ(response.headers().get("x-uos-cache"), "stale"); EXPECT_EQ(calls, 0); EXPECT_EQ(signals, 0);
}
ASYNC_TEST("codex catalog: concurrent cold misses share one upstream refresh") {
  CatalogFixture fixture; const std::string version = "0.151.1"; auto [started, markStarted, rejectStarted] = rt::withResolvers<void>(); (void)rejectStarted; auto [deferred, release, reject] = rt::withResolvers<rt::Response>(); (void)reject; int calls = 0;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++calls; markStarted(); return deferred.wait(); }));
  rt::Promise<rt::Response> first(catalog(version)); co_await started.wait(); rt::Promise<rt::Response> second(catalog(version)); release(catalogResponse(catalogBody(version)));
  AWAIT(firstResponse, first.wait()); AWAIT(secondResponse, second.wait()); EXPECT_EQ(firstResponse.status(), 200); EXPECT_EQ(secondResponse.status(), 200); EXPECT_EQ(secondResponse.headers().get("x-uos-cache"), "wait"); EXPECT_EQ(calls, 1);
}
ASYNC_TEST("codex catalog: auth rotation discards an in-flight old-generation refresh") {
  CatalogFixture fixture; const std::string version = "0.201.1", replacementGeneration = "replacement-auth-generation"; const auto replacementBody = catalogBody(version, json::object{{"account", "replacement"}});
  auto [started, markStarted, rejectStarted] = rt::withResolvers<void>(); (void)rejectStarted; auto [deferred, release, reject] = rt::withResolvers<rt::Response>(); (void)reject;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { markStarted(); return deferred.wait(); })); rt::Promise<rt::Response> pending(catalog(version)); co_await started.wait();
  fixture.kv->put(CODEX_CATALOG_AUTH_GENERATION_KEY, rt::KvValue(replacementGeneration)); fixture.kv->put(SNAPSHOT_KEY, json::object{{"source", "chatgpt_codex"}, {"client_version", version}, {"updated_at_ms", rt::dateNow()}, {"models", json::array{json::object{{"slug", "replacement-snapshot"}}}}});
  StoreCodexCatalogInput input{.clientVersion = version, .authGeneration = replacementGeneration, .body = replacementBody}; AWAIT(stored, storeCodexCatalog(*fixture.kv, input)); EXPECT(stored); release(catalogResponse(catalogBody(version, json::object{{"account", "old"}})));
  AWAIT(response, pending.wait()); EXPECT_EQ(response.status(), 200); EXPECT_EQ(response.headers().get("x-uos-cache"), "rotated"); AWAIT(payload, response.json()); EXPECT_EQ(rt::getString(payload, "account"), "replacement"); EXPECT_EQ(rt::getString(fixture.kv->row(catalogKey(version)), "auth_generation"), replacementGeneration); const auto snapshot = fixture.kv->row(SNAPSHOT_KEY); EXPECT_EQ(rt::getString(snapshot.at("models").as_array()[0], "slug"), "replacement-snapshot");
}
ASYNC_TEST("codex catalog: slow refreshes renew their lease") {
  CatalogFixture fixture; const std::string version = "0.151.2"; const rt::KvKey leaseKey{CODEX_CATALOG_LEASE_PREFIX, version}; auto [started, markStarted, rejectStarted] = rt::withResolvers<void>(); (void)rejectStarted; auto [deferred, release, reject] = rt::withResolvers<rt::Response>(); (void)reject;
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { markStarted(); return deferred.wait(); })); rt::Promise<rt::Response> pending(catalog(version)); co_await started.wait(); const auto initial = fixture.kv->row(leaseKey); co_await rt::sleep(5100); const auto renewed = fixture.kv->row(leaseKey); EXPECT(rt::getNumber(renewed, "lease_until_ms") > rt::getNumber(initial, "lease_until_ms")); release(catalogResponse(catalogBody(version))); AWAIT(response, pending.wait()); EXPECT_EQ(response.status(), 200);
}
ASYNC_TEST("codex catalog: gzip chunks are bounded and integrity failures force a refresh") {
  CatalogFixture fixture; const std::string version = "0.152.0"; json::array noisy; for (int index = 0; index < 4000; ++index) noisy.push_back(json::object{{"slug", "model-" + std::to_string(index) + "-" + rt::crypto::randomUUID()}, {"description", rt::crypto::randomUUID()}}); const auto body = rt::jsonStringify(json::object{{"models", noisy}}); AWAIT(stored, fixture.store(version, body)); EXPECT(stored);
  std::vector<std::string> chunks; const rt::KvKey prefix{CODEX_CATALOG_CHUNK_PREFIX, version}; for (const auto& [encoded, row] : fixture.kv->rows) if (matchesPrefix(row.key, prefix)) { chunks.push_back(encoded); EXPECT(static_cast<double>(row.value.asBytes().size()) <= CODEX_CATALOG_CHUNK_BYTES); } ASSERT(chunks.size() > 1); auto* first = fixture.kv->rows.get(chunks[0]); ASSERT(first); auto corrupted = first->value.asBytes(); corrupted[0] ^= 0xff; first->value = rt::KvValue(corrupted);
  metered::resetMeteredModelsCacheForTest(); surplus::resetSurplusModelsCacheForTest(); int calls = 0; std::vector<std::string> fetched; const auto urlPrefix = uos::config::config.codexBaseUrl + "/models?client_version="; const std::vector<std::string> recognized{"https://api.openlux.ai/v1/models", "https://api.surplusintelligence.ai/v1/models"};
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo input, rt::RequestInit) { const auto url = fetchUrl(input); fetched.push_back(url); if (rt::startsWith(url, urlPrefix)) { ++calls; rt::ResponseInit init; init.status = 503; return rt::Response("unavailable", init); } if (rt::includes(recognized, url)) return jsonResponse(json::object{{"data", json::array{}}}); rt::ResponseInit init; init.status = 500; return rt::Response("unexpected fetch " + url, init); }));
  AWAIT(response, catalog(version)); EXPECT_EQ(response.status(), 502); EXPECT_EQ(calls, 1); std::vector<std::string> unexpected; for (const auto& url : fetched) if (!rt::startsWith(url, urlPrefix) && !rt::includes(recognized, url)) unexpected.push_back(url); EXPECT(unexpected.empty());
}

ASYNC_TEST("codex catalog: replacing metadata reclaims superseded chunks") {
  const Store store = co_await seedBaseState();
  const std::string version = "0.152.1";
  const StoreCodexCatalogInput first{.clientVersion = version, .authGeneration = AUTH_GENERATION, .body = catalogBody(version, generation("first"))};
  auto storingFirst = storeCodexCatalog(*store.kv, first);
  const bool firstStored = co_await std::move(storingFirst);
  EXPECT(firstStored);
  const std::optional<rt::MemoryKvDatabase::Row> firstMetadata = store.database->row(catalogKey(version));
  ASSERT(firstMetadata.has_value());
  const std::string firstGeneration = firstMetadata->value["body_generation"].asString();
  const std::vector<rt::KvKey> firstChunkKeys = chunkKeys(version, firstGeneration, firstMetadata->value["chunk_count"].asNumber());
  bool allPresent = true;
  for (const rt::KvKey& key : firstChunkKeys) allPresent = allPresent && has(store, key);
  EXPECT(allPresent);

  const StoreCodexCatalogInput second{.clientVersion = version, .authGeneration = AUTH_GENERATION, .body = catalogBody(version, generation("second"))};
  auto storingSecond = storeCodexCatalog(*store.kv, second);
  const bool secondStored = co_await std::move(storingSecond);
  EXPECT(secondStored);
  const std::optional<rt::MemoryKvDatabase::Row> secondMetadata = store.database->row(catalogKey(version));
  ASSERT(secondMetadata.has_value());
  const std::string secondGeneration = secondMetadata->value["body_generation"].asString();
  EXPECT(secondGeneration != firstGeneration);
  bool allGone = true;
  for (const rt::KvKey& key : firstChunkKeys) allGone = allGone && !has(store, key);
  EXPECT(allGone);
  for (const rt::KvKey& key : chunkKeys(version, secondGeneration, secondMetadata->value["chunk_count"].asNumber())) EXPECT_EQ(has(store, key), true);
}

ASYNC_TEST("codex catalog: rejected old-generation writes reclaim their chunks") {
  const Store store = co_await seedBaseState();
  const std::string version = "0.152.2";
  const StoreCodexCatalogInput input{.clientVersion = version, .authGeneration = "superseded-generation", .body = catalogBody(version)};
  auto storing = storeCodexCatalog(*store.kv, input);
  const bool stored = co_await std::move(storing);
  EXPECT_EQ(stored, false);
  EXPECT_EQ(chunkRows(store, version), std::size_t{0});
  EXPECT_EQ(has(store, catalogKey(version)), false);
}

ASYNC_TEST("codex catalog: version cache evicts the oldest catalog beyond its bound") {
  CatalogFixture fixture; const int total = static_cast<int>(CODEX_CATALOG_MAX_VERSIONS) + 3;
  for (int index = 0; index < total; ++index) { const auto version = "1.0." + std::to_string(index); AWAIT(stored, fixture.store(version, catalogBody(version), rt::dateNow() + index)); EXPECT(stored); AWAIT(loaded, catalog(version)); EXPECT_EQ(loaded.status(), 200); if (auto body = loaded.body()) co_await body->cancel(); }
  std::size_t count = 0; for (const auto& [encoded, row] : fixture.kv->rows) { (void)encoded; if (matchesPrefix(row.key, CODEX_CATALOG_PREFIX) && row.key.size() == CODEX_CATALOG_PREFIX.size() + 1 && row.value["body_generation"].isString()) ++count; } EXPECT_EQ(static_cast<double>(count), CODEX_CATALOG_MAX_VERSIONS);
  for (int index = 0; index < 3; ++index) { const auto version = "1.0." + std::to_string(index); EXPECT(!fixture.kv->rows.has(diff::keyOf(catalogKey(version)))); const rt::KvKey prefix{CODEX_CATALOG_CHUNK_PREFIX, version}; std::size_t chunks = 0; for (const auto& [encoded, row] : fixture.kv->rows) { (void)encoded; if (matchesPrefix(row.key, prefix)) ++chunks; } EXPECT_EQ(chunks, 0u); }
  const auto latest = "1.0." + std::to_string(total - 1); EXPECT(fixture.kv->rows.has(diff::keyOf(catalogKey(latest)))); const auto memo = getCodexCatalogMemoVersionsForTest(); EXPECT_EQ(static_cast<double>(memo.size()), CODEX_CATALOG_MAX_VERSIONS); for (int index = 0; index < 3; ++index) EXPECT(!rt::includes(memo, "1.0." + std::to_string(index))); ASSERT(!memo.empty()); EXPECT_EQ(memo.back(), latest);
}
ASYNC_TEST("codex catalog: only same-or-newer clients update the normalized snapshot") {
  CatalogFixture fixture; rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo input, rt::RequestInit) { const auto version = rt::URL(fetchUrl(input)).searchParams().get("client_version"); ASSERT(version); json::array rows{json::object{{"slug", "gpt-" + *version}, {"display_name", "Rich " + *version}, {"supported_reasoning_levels", json::array{json::object{{"effort", "high"}, {"description", "deep"}}}}}, json::object{{"slug", "codex-auto-review"}, {"display_name", "Codex Auto Review"}, {"visibility", "hide"}, {"supported_in_api", true}}, json::object{{"slug", "codex-internal-evals"}, {"visibility", "hide"}, {"supported_in_api", false}}}; return catalogResponse(catalogBody(*version, json::object{{"models", rows}})); }));
  AWAIT(older, catalog("0.199.0")); EXPECT_EQ(older.status(), 200); EXPECT_EQ(rt::getString(fixture.kv->row(SNAPSHOT_KEY), "client_version"), "0.200.0");
  AWAIT(newer, catalog("0.201.0")); EXPECT_EQ(newer.status(), 200); const auto snapshot = fixture.kv->row(SNAPSHOT_KEY), runtime = fixture.kv->row(rc::RUNTIME_CONFIG_V2_KEY); EXPECT_EQ(rt::getString(snapshot, "client_version"), "0.201.0"); const json::array wanted{"gpt-0.201.0", "codex-auto-review"}; EXPECT(rt::deepEqual(fieldValues(snapshot.at("models").as_array(), "slug"), wanted)); EXPECT_EQ(rt::getString(runtime, "default_model"), "gpt-0.201.0"); EXPECT_EQ(rt::getString(runtime, "default_reasoning_effort"), "medium"); EXPECT_EQ(rt::getString(runtime.at("codex_models"), "client_version"), "0.201.0"); EXPECT(rt::deepEqual(fieldValues(runtime.at("codex_models").at("models").as_array(), "slug"), wanted));
  auto stale = runtime; stale.as_object()["default_model"] = "stale-uncommitted-value"; fixture.kv->put(rc::RUNTIME_CONFIG_V2_KEY, stale); const std::shared_ptr<rt::Kv> kv = fixture.kv; AWAIT(cached, rc::loadRuntimeConfig(kv)); ASSERT(cached); EXPECT_EQ(cached->default_model, "gpt-0.201.0");
}
ASYNC_TEST("codex catalog: normalized snapshot retry preserves a concurrent admin default") {
  CatalogFixture fixture; const json::array models{json::object{{"slug", "shared-default"}, {"supported_reasoning_levels", json::array{"medium"}}}, json::object{{"slug", "admin-default"}, {"supported_reasoning_levels", json::array{"high"}}}}; const json::object snapshot{{"source", "chatgpt_codex"}, {"client_version", "0.200.0"}, {"updated_at_ms", rt::dateNow()}, {"models", models}}; fixture.seedSnapshot(snapshot, "shared-default", "medium");
  rt::GlobalFetchOverride fetch(rt::Fetch([models](rt::RequestInfo, rt::RequestInit) { return catalogResponse(catalogBody("0.201.0", json::object{{"models", models}})); }));
  fixture.kv->beforeAtomicCommit = [&](const std::vector<rt::KvKey>& keys) { if (std::find(keys.begin(), keys.end(), rc::RUNTIME_CONFIG_V2_KEY) == keys.end()) return; fixture.kv->beforeAtomicCommit = {}; auto current = fixture.kv->row(rc::RUNTIME_CONFIG_V2_KEY); ASSERT(current.is_object()); current.as_object()["default_model"] = "admin-default"; current.as_object()["default_reasoning_effort"] = "high"; current.as_object()["updated_at_ms"] = rt::dateNow(); fixture.kv->put(rc::RUNTIME_CONFIG_V2_KEY, current); };
  AWAIT(response, catalog("0.201.0")); EXPECT_EQ(response.status(), 200); const auto saved = fixture.kv->row(SNAPSHOT_KEY), runtime = fixture.kv->row(rc::RUNTIME_CONFIG_V2_KEY); EXPECT_EQ(rt::getString(saved, "client_version"), "0.201.0"); EXPECT_EQ(rt::getString(runtime.at("codex_models"), "client_version"), "0.201.0"); EXPECT_EQ(rt::getString(runtime, "default_model"), "admin-default"); EXPECT_EQ(rt::getString(runtime, "default_reasoning_effort"), "high");
}
ASYNC_TEST("codex catalog: normalized refresh retries preserve same-slug prompt-cache probe evidence") {
  CatalogFixture fixture; const auto promptCache = parsedObject(R"({"version":1,"providers":[{"id":"codex_chatgpt","scope":{"probe_profile":"responses_implicit_input_text_keyed_cycle_isolated_v5","account_slots":"unknown","token_refresh":"preserved","conversation_id":"independent","reproducible_cycles":3,"source":"live_probe","verified_at_ms":2000}}]})");
  const json::object snapshot{{"source", "chatgpt_codex"}, {"client_version", "0.200.0"}, {"updated_at_ms", rt::dateNow()}, {"models", json::array{json::object{{"slug", "gpt-cache-probe"}, {"prompt_cache", promptCache}}}}}; fixture.seedSnapshot(snapshot, "gpt-cache-probe", "medium");
  rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return catalogResponse(catalogBody("0.201.0", json::object{{"models", json::array{json::object{{"slug", "gpt-cache-probe"}, {"supported_reasoning_levels", json::array{"medium"}}}}}})); }));
  fixture.kv->beforeAtomicCommit = [&](const std::vector<rt::KvKey>& keys) { if (std::find(keys.begin(), keys.end(), SNAPSHOT_KEY) == keys.end()) return; fixture.kv->beforeAtomicCommit = {}; auto next = snapshot; next["updated_at_ms"] = rt::dateNow() + 1; fixture.kv->put(SNAPSHOT_KEY, next); };
  AWAIT(response, catalog("0.201.0")); EXPECT_EQ(response.status(), 200); const auto saved = fixture.kv->row(SNAPSHOT_KEY), runtime = fixture.kv->row(rc::RUNTIME_CONFIG_V2_KEY); EXPECT_EQ(rt::getString(saved, "client_version"), "0.201.0"); const auto& savedModel = saved.at("models").as_array()[0]; EXPECT(rt::deepEqual(savedModel.at("prompt_cache"), promptCache)); EXPECT(!rt::get(runtime.at("codex_models").at("models").as_array()[0], "prompt_cache"));
}
namespace {
  rt::Task<std::optional<metered::MeteredModelsSnapshot>> seedMetered(const json::array& rows) { metered::FetchMeteredModelsOptions options; options.force = true; options.fetcher = payloadFetcher(json::object{{"data", rows}}); return metered::fetchMeteredModels(options); }
  rt::Task<std::optional<surplus::SurplusModelsSnapshot>> seedSurplus(const json::array& rows) { surplus::FetchSurplusModelsOptions options; options.apiKey = std::string("surplus-catalog-test-key"); options.force = true; options.fetcher = payloadFetcher(json::object{{"data", rows}}); return surplus::fetchSurplusModels(options); }
  rt::Task<std::shared_ptr<const openrouter::OpenRouterModelsSnapshot>> seedOpenRouter(const json::array& rows) { openrouter::FetchOpenRouterModelsOptions options; options.force = true; options.fetcher = payloadFetcher(json::object{{"data", rows}}); return openrouter::fetchOpenRouterModels(options); }
  json::object meteredRow(const std::string& id) { return json::object{{"id", id}, {"owned_by", "openlux"}, {"supported_endpoint_types", json::array{"openai-response"}}}; }
  json::array reasoningEfforts(const json::object& model) { const auto* levels = rt::getArray(model, "supported_reasoning_levels"); return levels ? fieldValues(*levels, "effort") : json::array{}; }
}
ASYNC_TEST("codex catalog: model picker receives the complete unique union of paid provider models") {
  CatalogFixture fixture; rt::env::set("METERED_API_KEY", "metered-catalog-test-key"); rt::env::set("SURPLUS_API_KEY", "surplus-catalog-test-key");
  auto shared = meteredRow("shared-paid-model"); shared["description"] = "OpenLux route"; const json::array meteredRows{shared, meteredRow("openlux-only-model"), json::object{{"id", "chat-only-model"}, {"owned_by", "openlux"}, {"supported_endpoint_types", json::array{"openai"}}}}; AWAIT(meteredSnapshot, seedMetered(meteredRows)); ASSERT(meteredSnapshot);
  const json::array surplusRows{json::object{{"id", "shared-paid-model"}, {"provider", "surplus"}}, json::object{{"id", "surplus-only-model"}, {"provider", "surplus"}}, json::object{{"id", "deepseek-v4-flash"}, {"provider", "surplus"}}, json::object{{"id", "deepseek-v4-flash-0731"}, {"provider", "surplus"}}, json::object{{"id", "deepseek-v4-flash:web"}, {"provider", "surplus"}}, json::object{{"id", "minimax-m2.7"}, {"provider", "surplus"}}}; AWAIT(surplusSnapshot, seedSurplus(surplusRows)); ASSERT(surplusSnapshot); rt::GlobalFetchOverride fetch(versionFetcher());
  AWAIT(response, catalog("0.148.0")); EXPECT_EQ(response.status(), 200); AWAIT(payload, response.json()); const auto slugs = fieldValues(payload.at("models").as_array(), "slug"); const json::array wanted{"gpt-0.148.0", "shared-paid-model", "openlux-only-model", "surplus-only-model", "deepseek-v4-flash", "deepseek-v4-flash-0731", "deepseek-v4-flash:web", "minimax-m2.7", "ubiquity/deepseek-flash"}; EXPECT(rt::deepEqual(slugs, wanted)); rt::OrderedSet<std::string> unique; for (const auto& slug : slugs) unique.add(std::string(slug.as_string())); EXPECT_EQ(unique.size(), slugs.size());
  const auto* sharedModel = findModel(payload, "shared-paid-model"); ASSERT(sharedModel); EXPECT_EQ(rt::getString(*sharedModel, "visibility"), "list"); const auto* surplusModel = findModel(payload, "surplus-only-model"); ASSERT(surplusModel); const json::array noReasoning{json::object{{"effort", "none"}, {"description", "No reasoning"}}}; EXPECT(rt::deepEqual(surplusModel->at("supported_reasoning_levels"), noReasoning));
  const std::vector<std::string> deepseekIds{"deepseek-v4-flash", "deepseek-v4-flash-0731", "deepseek-v4-flash:web"}; for (const auto& slug : deepseekIds) { const auto* model = findModel(payload, slug); ASSERT(model); EXPECT(rt::deepEqual(model->at("supported_reasoning_levels"), noReasoning)); EXPECT_EQ(rt::getString(*model, "default_reasoning_level"), "none"); EXPECT(!rt::get(*model, "context_window")); EXPECT(!rt::get(*model, "max_context_window")); EXPECT(!rt::get(*model, "auto_compact_token_limit")); }
  const auto* minimax = findModel(payload, "minimax-m2.7"); ASSERT(minimax); EXPECT(!rt::get(*minimax, "context_window")); EXPECT(!rt::get(*minimax, "auto_compact_token_limit")); EXPECT(!rt::includes(slugs, json::value("chat-only-model")));
}
ASYNC_TEST("codex catalog: cold provider caches cannot publish the incomplete Codex-only catalog") {
  CatalogFixture fixture; rt::env::set("METERED_API_KEY", "metered-cold-catalog-test-key"); rt::env::set("SURPLUS_API_KEY", "surplus-cold-catalog-test-key"); metered::setMeteredModelsFetchForTest(payloadFetcher(json::object{{"data", json::array{meteredRow("cold-openlux-model")}}}));
  rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo input, rt::RequestInit) { const auto url = fetchUrl(input); if (url == "https://api.surplusintelligence.ai/v1/models") return jsonResponse(json::object{{"data", json::array{json::object{{"id", "deepseek-v4-flash"}, {"provider", "surplus"}}}}}); const auto version = rt::URL(url).searchParams().get("client_version").value_or("missing"); return catalogResponse(catalogBody(version)); }));
  AWAIT(response, catalog("0.203.0")); EXPECT_EQ(response.status(), 200); AWAIT(payload, response.json()); const auto slugs = fieldValues(payload.at("models").as_array(), "slug"); EXPECT(rt::includes(slugs, json::value("cold-openlux-model"))); const auto* deepseek = findModel(payload, "deepseek-v4-flash"); ASSERT(deepseek); const json::array wanted{"none"}; EXPECT(rt::deepEqual(reasoningEfforts(*deepseek), wanted));
}
ASYNC_TEST("paid-provider model caches ignore out-of-order Metered refreshes") {
  CatalogFixture fixture; rt::env::set("METERED_API_KEY", "metered-cache-race-test-key"); int calls = 0; auto [olderResponse, resolveOlder, rejectOlder] = rt::withResolvers<rt::Response>(); (void)rejectOlder; auto [newerResponse, resolveNewer, rejectNewer] = rt::withResolvers<rt::Response>(); (void)rejectNewer;
  rt::Fetch fetcher([&](rt::RequestInfo, rt::RequestInit) { ++calls; return (calls == 1 ? olderResponse : newerResponse).wait(); }); metered::FetchMeteredModelsOptions options; options.force = true; options.fetcher = fetcher; rt::Promise<std::optional<metered::MeteredModelsSnapshot>> older(metered::fetchMeteredModels(options)), newer(metered::fetchMeteredModels(options)); EXPECT_EQ(calls, 2);
  resolveNewer(jsonResponse(json::object{{"data", json::array{meteredRow("new-metered-model")}}})); AWAIT(newSnapshot, newer.wait()); EXPECT(rt::deepEqual(modelIds(newSnapshot), json::array{"new-metered-model"})); resolveOlder(jsonResponse(json::object{{"data", json::array{meteredRow("old-metered-model")}}})); AWAIT(oldSnapshot, older.wait()); (void)oldSnapshot;
  options.force.reset(); AWAIT(cached, metered::fetchMeteredModels(options)); EXPECT(rt::deepEqual(modelIds(cached), json::array{"new-metered-model"})); EXPECT_EQ(calls, 2);
}
ASYNC_TEST("paid-provider model caches ignore out-of-order Surplus refreshes") {
  CatalogFixture fixture; int calls = 0; auto [olderResponse, resolveOlder, rejectOlder] = rt::withResolvers<rt::Response>(); (void)rejectOlder; auto [newerResponse, resolveNewer, rejectNewer] = rt::withResolvers<rt::Response>(); (void)rejectNewer;
  rt::Fetch fetcher([&](rt::RequestInfo, rt::RequestInit) { ++calls; return (calls == 1 ? olderResponse : newerResponse).wait(); }); surplus::FetchSurplusModelsOptions options; options.apiKey = std::string("surplus-cache-race-test-key"); options.force = true; options.fetcher = fetcher; rt::Promise<std::optional<surplus::SurplusModelsSnapshot>> older(surplus::fetchSurplusModels(options)), newer(surplus::fetchSurplusModels(options)); EXPECT_EQ(calls, 2);
  resolveNewer(jsonResponse(json::object{{"data", json::array{json::object{{"id", "new-surplus-model"}}}}})); AWAIT(newSnapshot, newer.wait()); EXPECT(rt::deepEqual(modelIds(newSnapshot), json::array{"new-surplus-model"})); resolveOlder(jsonResponse(json::object{{"data", json::array{json::object{{"id", "old-surplus-model"}}}}})); AWAIT(oldSnapshot, older.wait()); (void)oldSnapshot;
  options.force.reset(); AWAIT(cached, surplus::fetchSurplusModels(options)); EXPECT(rt::deepEqual(modelIds(cached), json::array{"new-surplus-model"})); EXPECT_EQ(calls, 2);
}
ASYNC_TEST("paid-provider model refresh failures use a cooldown") {
  CatalogFixture fixture; rt::env::set("METERED_API_KEY", "metered-cache-backoff-test-key"); const double base = rt::dateNow(); rt::globalDateNow() = [base] { return base; }; const json::array meteredRows{meteredRow("stale-metered-model")}, surplusRows{json::object{{"id", "stale-surplus-model"}}}; AWAIT(first, seedMetered(meteredRows)); ASSERT(first); AWAIT(second, seedSurplus(surplusRows)); ASSERT(second); rt::globalDateNow() = [base] { return base + std::max(metered::METERED_MODELS_CACHE_TTL_MS, surplus::SURPLUS_MODELS_CACHE_TTL_MS) + 1; };
  int meteredFailures = 0, surplusFailures = 0; metered::FetchMeteredModelsOptions mo; mo.fetcher = rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++meteredFailures; rt::ResponseInit init; init.status = 503; return rt::Response("unavailable", init); }); surplus::FetchSurplusModelsOptions so; so.apiKey = std::string("surplus-catalog-test-key"); so.fetcher = rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++surplusFailures; rt::ResponseInit init; init.status = 503; return rt::Response("unavailable", init); });
  AWAIT(m1, metered::fetchMeteredModels(mo)); AWAIT(m2, metered::fetchMeteredModels(mo)); AWAIT(s1, surplus::fetchSurplusModels(so)); AWAIT(s2, surplus::fetchSurplusModels(so)); (void)m1; (void)m2; (void)s1; (void)s2; EXPECT_EQ(meteredFailures, 1); EXPECT_EQ(surplusFailures, 1);
}
ASYNC_TEST("codex catalog: expired paid-provider caches schedule background refreshes") {
  CatalogFixture fixture; const double base = rt::dateNow(), stale = base + std::max(metered::METERED_MODELS_CACHE_TTL_MS, surplus::SURPLUS_MODELS_CACHE_TTL_MS) + 1; int meteredRefreshes = 0, surplusRefreshes = 0; auto [mp, resolveMetered, rejectMetered] = rt::withResolvers<rt::Response>(); (void)rejectMetered; auto [sp, resolveSurplus, rejectSurplus] = rt::withResolvers<rt::Response>(); (void)rejectSurplus;
  rt::env::set("METERED_API_KEY", "metered-catalog-stale-test-key"); rt::env::set("SURPLUS_API_KEY", "surplus-catalog-test-key"); rt::globalDateNow() = [base] { return base; }; const json::array meteredRows{meteredRow("stale-openlux-model")}, surplusRows{json::object{{"id", "stale-surplus-model"}}}; AWAIT(m, seedMetered(meteredRows)); ASSERT(m); AWAIT(s, seedSurplus(surplusRows)); ASSERT(s); rt::globalDateNow() = [stale] { return stale; };
  metered::setMeteredModelsFetchForTest(rt::Fetch([&](rt::RequestInfo input, rt::RequestInit) -> rt::Task<rt::Response> { if (fetchUrl(input) != "https://api.openlux.ai/v1/models") { rt::ResponseInit init; init.status = 500; co_return rt::Response("unexpected Metered URL", init); } ++meteredRefreshes; auto response = co_await mp.wait(); co_return response; }));
  rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo input, rt::RequestInit) -> rt::Task<rt::Response> { const auto url = fetchUrl(input); if (url == "https://api.openlux.ai/v1/models") { ++meteredRefreshes; co_return jsonResponse(json::object{{"data", json::array{meteredRow("fresh-openlux-model")}}}); } if (url == "https://api.surplusintelligence.ai/v1/models") { ++surplusRefreshes; auto response = co_await sp.wait(); co_return response; } const auto version = rt::URL(url).searchParams().get("client_version").value_or("missing"); co_return catalogResponse(catalogBody(version)); }));
  AWAIT(response, catalog("0.202.0")); EXPECT_EQ(response.status(), 200); AWAIT(concurrent, catalog("0.202.0")); EXPECT_EQ(concurrent.status(), 200); EXPECT_EQ(meteredRefreshes, 1); EXPECT_EQ(surplusRefreshes, 1);
  resolveMetered(jsonResponse(json::object{{"data", json::array{meteredRow("fresh-openlux-model")}}})); resolveSurplus(jsonResponse(json::object{{"data", json::array{json::object{{"id", "fresh-surplus-model"}}}}})); co_await rt::sleep(1);
  AWAIT(refreshed, catalog("0.202.0")); AWAIT(payload, refreshed.json()); const auto slugs = fieldValues(payload.at("models").as_array(), "slug"); EXPECT(rt::includes(slugs, json::value("fresh-openlux-model"))); EXPECT(rt::includes(slugs, json::value("fresh-surplus-model")));
}
ASYNC_TEST("codex catalog: third-party enrichment fills rows no first-party source describes") {
  CatalogFixture fixture; rt::env::set("SURPLUS_API_KEY", "surplus-catalog-test-key"); const json::array rows{json::object{{"id", "deepseek-v4-flash"}, {"provider", "surplus"}}}; AWAIT(s, seedSurplus(rows)); ASSERT(s); const json::array enrichment{parsedObject(R"({"id":"deepseek/deepseek-v4-flash","context_length":1048576,"top_provider":{"context_length":1048576,"max_completion_tokens":384000},"reasoning":{"supported_efforts":["max","high","low"],"default_effort":"high","mandatory":false}})")}; AWAIT(o, seedOpenRouter(enrichment)); ASSERT(o); rt::GlobalFetchOverride fetch(versionFetcher());
  AWAIT(response, catalog("0.148.0")); EXPECT_EQ(response.status(), 200); AWAIT(payload, response.json()); const auto* deepseek = findModel(payload, "deepseek-v4-flash"); ASSERT(deepseek); const json::array levels{"low", "high", "max"}; EXPECT(rt::deepEqual(reasoningEfforts(*deepseek), levels)); EXPECT_EQ(rt::getString(*deepseek, "default_reasoning_level"), "high"); EXPECT_EQ(rt::getNumber(*deepseek, "context_window"), 1048576); EXPECT_EQ(rt::getNumber(*deepseek, "max_context_window"), 1048576); EXPECT_EQ(rt::getNumber(*deepseek, "auto_compact_token_limit"), 943718); EXPECT_EQ(rt::getNumber(*deepseek, "effective_context_window_percent"), 95); EXPECT(!rt::get(*deepseek, "model_class"));
}
ASYNC_TEST("codex catalog: a Codex-served id advertises the widest window a source states") {
  CatalogFixture fixture; rt::env::set("SURPLUS_API_KEY", "surplus-catalog-test-key"); const json::array rows{json::object{{"id", "gpt-6-astra"}, {"provider", "surplus"}}}; AWAIT(s, seedSurplus(rows)); ASSERT(s); const json::array enrichment{parsedObject(R"({"id":"openai/gpt-6-astra","context_length":1050000,"top_provider":{"context_length":1050000},"reasoning":{"supported_efforts":["max","high","low"],"default_effort":"high","mandatory":false}})")}; AWAIT(o, seedOpenRouter(enrichment)); ASSERT(o); rt::GlobalFetchOverride fetch(versionFetcher());
  AWAIT(response, catalog("0.155.0")); EXPECT_EQ(response.status(), 200); AWAIT(payload, response.json()); const auto* astra = findModel(payload, "gpt-6-astra"); ASSERT(astra); EXPECT_EQ(rt::getNumber(*astra, "context_window"), 1050000); EXPECT_EQ(rt::getNumber(*astra, "max_context_window"), 1050000); EXPECT_EQ(rt::getNumber(*astra, "auto_compact_token_limit"), 945000); const json::array levels{"low", "medium", "high", "xhigh", "max", "ultra"}; EXPECT(rt::deepEqual(reasoningEfforts(*astra), levels)); if (const auto* untouched = findModel(payload, "gpt-reserve")) { EXPECT_EQ(rt::getNumber(*untouched, "context_window"), 272000); EXPECT_EQ(rt::getNumber(*untouched, "max_context_window"), 872000); }
}
