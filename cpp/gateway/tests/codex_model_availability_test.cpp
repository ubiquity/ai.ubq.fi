// Links: tests/codex_model_availability_test.cpp src/auth/relay.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp
//        src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp
//        src/authority/native_codex_mutations.cpp src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp
//        src/bounded_response_body.cpp src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp
//        src/cache/prompt_analytics_read.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp src/codex/auth.cpp
//        src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp
//        src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/native_auth.cpp
//        src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp
//        src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp
//        src/config.cpp src/http.cpp src/inference_deadline.cpp src/kv.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp
//        src/optional_telemetry_queue.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp
//        src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp
//        src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
//        src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp
//        src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_quota.cpp
//        src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/selection.cpp src/release.cpp src/runtime/http.cpp
//        src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp
//        src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/types.cpp src/utils.cpp src/models/codex_models_fetch.cpp
//        src/runtime_config.cpp src/models/codex_models.cpp src/defaults.cpp
// Port of tests/codex-model-availability.test.ts: all 26 original cases and assertions.
// The existing store cases are preserved; each case owns its mock KV, auth caches and capacity trigger.
#include "uos/models/codex_models_availability.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/codex/account_routing.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/dispatch.hpp"
#include "uos/config.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"

#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace availability = uos::models::codex_models_availability;
  using availability::CodexAccountCatalogInput;
  using availability::CodexAccountModelCatalog;
  using availability::CodexAccountModelsStore;
  using availability::CodexModelUnsupportedOptions;
  template <class T> using Record = uos::authority::contracts::Record<T>;

  const std::string MODEL = "gpt-daybreak-blue-latest";
  const std::string UNSUPPORTED_DETAIL = "The '" + MODEL + "' model is not supported when using Codex with a ChatGPT account.";

  // `JSON.stringify(key)` for the suite's string keys.
  std::string keyToString(const rt::KvKey& key) {
    std::string out = "[";
    for (std::size_t index = 0; index < key.size(); ++index) {
      if (index > 0) out += ",";
      const rt::KvKeyPart& part = key[index];
      out += part.isString() ? rt::jsonQuote(part.string()) : part.toString();
    }
    return out + "]";
  }

  // The rows the suite's `kvStore` Map holds; values are kept as written (the stub never clones).
  struct StubState {
    struct Stored {
      rt::KvKey key;
      rt::KvValue value;
      std::string versionstamp;
    };
    rt::OrderedMap<std::string, Stored> rows;
    double versionCounter = 0;

    // `String(++versionCounter).padStart(20, "0")`
    std::string nextVersion() {
      versionCounter += 1;
      return rt::padStart(rt::numberToString(versionCounter), 20, "0");
    }
    rt::KvEntryMaybe<> entryFor(const rt::KvKey& key) const {
      if (const Stored* stored = rows.get(keyToString(key))) return {key, stored->value, stored->versionstamp};
      return {key, rt::KvValue(nullptr), std::nullopt};
    }
    std::string writeKv(const rt::KvKey& key, const rt::KvValue& value) {
      const std::string versionstamp = nextVersion();
      rows.set(keyToString(key), Stored{key, value, versionstamp});
      return versionstamp;
    }
    // `readKv(key)`: nullptr is undefined.
    const rt::KvValue* readKv(const rt::KvKey& key) const {
      const Stored* stored = rows.get(keyToString(key));
      return stored ? &stored->value : nullptr;
    }
  };

  class StubAtomicOperation final : public rt::AtomicOperation {
  public:
    explicit StubAtomicOperation(std::shared_ptr<StubState> state_) : state(std::move(state_)) {}
    using rt::AtomicOperation::check;
    using rt::AtomicOperation::max;
    using rt::AtomicOperation::min;
    using rt::AtomicOperation::sum;
    rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
      for (rt::AtomicCheck& item : items) checks.push_back(std::move(item));
      return *this;
    }
    rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { throw rt::TypeError("chain.mutate is not a function"); }
    rt::AtomicOperation& sum(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("chain.sum is not a function"); }
    rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("chain.min is not a function"); }
    rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("chain.max is not a function"); }
    rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions = {}) override {
      ops.push_back(Op{std::move(key), std::move(value), false});
      return *this;
    }
    rt::AtomicOperation& delete_(rt::KvKey key) override {
      ops.push_back(Op{std::move(key), rt::KvValue(), true});
      return *this;
    }
    rt::Task<rt::KvCommitResult> commit() override {
      bool valid = true;
      for (const rt::AtomicCheck& expected : checks) {
        const std::optional<std::string> current = state->entryFor(expected.key).versionstamp;
        const std::optional<std::string> wanted = expected.versionstamp && !expected.versionstamp->empty() ? expected.versionstamp : std::nullopt;
        if (current != wanted) valid = false;
      }
      rt::KvCommitResult result;
      if (!valid) co_return result;
      for (const Op& op : ops) {
        if (op.remove) state->rows.delete_(keyToString(op.key));
        else state->writeKv(op.key, op.value);
      }
      result.ok = true;
      co_return result;
    }

  private:
    struct Op {
      rt::KvKey key;
      rt::KvValue value;
      bool remove;
    };
    std::shared_ptr<StubState> state;
    std::vector<rt::AtomicCheck> checks;
    std::vector<Op> ops;
  };

  class StubListIterator final : public rt::KvListIterator {
  public:
    explicit StubListIterator(std::vector<rt::KvEntry<>> entries_) : entries(std::move(entries_)) {}
    rt::Task<std::optional<rt::KvEntry<>>> next() override {
      co_await rt::microtask();
      if (index >= entries.size()) co_return std::nullopt;
      co_return entries[index++];
    }
    std::string cursor() const override { return ""; }
    rt::Task<void> return_() override {
      index = entries.size();
      co_return;
    }

  private:
    std::vector<rt::KvEntry<>> entries;
    std::size_t index = 0;
  };

  // The suite's `kvStub`.
  class StubKv : public rt::Kv {
  public:
    explicit StubKv(std::shared_ptr<StubState> state_) : state(std::move(state_)) {}
    using rt::Kv::get;
    using rt::Kv::getMany;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions = {}) override { co_return state->entryFor(key); }
    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey> keys, rt::KvGetOptions = {}) override {
      std::vector<rt::KvEntryMaybe<>> entries;
      for (const rt::KvKey& key : keys) entries.push_back(state->entryFor(key));
      co_return entries;
    }
    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions = {}) override {
      state->writeKv(key, value);
      rt::KvCommitResult result;
      result.ok = true;
      co_return result;
    }
    rt::Task<void> delete_(rt::KvKey key) override {
      state->rows.delete_(keyToString(key));
      co_return;
    }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector selector, rt::KvListOptions = {}) override {
      const rt::KvKey prefix = selector.prefix.value_or(rt::KvKey());
      std::vector<rt::KvEntry<>> entries;
      for (const auto& [encoded, stored] : state->rows) {
        bool matches = stored.key.size() >= prefix.size();
        for (std::size_t index = 0; matches && index < prefix.size(); ++index) matches = !prefix[index].isBytes() && stored.key[index] == prefix[index];
        if (matches) entries.push_back(rt::KvEntry<>{stored.key, stored.value, stored.versionstamp});
      }
      return std::make_unique<StubListIterator>(std::move(entries));
    }
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<StubAtomicOperation>(state); }
    void close() override {}

  protected:
    std::shared_ptr<StubState> state;
  };

  // `{ ...kvStub, get }`: the first read of the availability key commits a concurrent catalog
  // write after taking the pre-race entry.
  class RaceKv final : public StubKv {
  public:
    RaceKv(std::shared_ptr<StubState> state_, std::shared_ptr<int> reads_) : StubKv(std::move(state_)), reads(std::move(reads_)) {}
    using rt::Kv::get;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions = {}) override {
      const rt::KvEntryMaybe<> entry = state->entryFor(key);
      if (keyToString(key) == keyToString(availability::CODEX_ACCOUNT_MODELS_KV_KEY)) {
        *reads += 1;
        if (*reads == 1) {
          json::value raced = json::parse(R"({"accounts":{"race-a":{"client_version":"0.160.0","slugs":[")" + MODEL +
                                          R"("],"updated_at_ms":1},"race-b":{"client_version":"0.160.0","slugs":["other-model"],"updated_at_ms":1}},"unsupported":{}})");
          state->writeKv(availability::CODEX_ACCOUNT_MODELS_KV_KEY, rt::KvValue(raced));
        }
      }
      co_return entry;
    }

  private:
    std::shared_ptr<int> reads;
  };

  // `rejected`: check and set chain, commit always answers `{ ok: false }`.
  class RejectedOperation final : public rt::AtomicOperation {
  public:
    using rt::AtomicOperation::check;
    using rt::AtomicOperation::max;
    using rt::AtomicOperation::min;
    using rt::AtomicOperation::sum;
    rt::AtomicOperation& check(std::vector<rt::AtomicCheck>) override { return *this; }
    rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { throw rt::TypeError("rejected.mutate is not a function"); }
    rt::AtomicOperation& sum(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("rejected.sum is not a function"); }
    rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("rejected.min is not a function"); }
    rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("rejected.max is not a function"); }
    rt::AtomicOperation& set(rt::KvKey, rt::KvValue, rt::KvSetOptions = {}) override { return *this; }
    rt::AtomicOperation& delete_(rt::KvKey) override { throw rt::TypeError("rejected.delete is not a function"); }
    rt::Task<rt::KvCommitResult> commit() override { co_return rt::KvCommitResult(); }
  };

  // `{ ...kvStub, atomic: () => rejected }`
  class RejectingKv final : public StubKv {
  public:
    using StubKv::StubKv;
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<RejectedOperation>(); }
  };

  // A store literal as the TS writes one inline (typed, no parsing).
  CodexAccountModelsStore storeFrom(const json::value& literal) {
    CodexAccountModelsStore store;
    for (const auto& [accountId, catalog] : literal.at("accounts").as_object())
      store.accounts.define(std::string(accountId), json::value_to<CodexAccountModelCatalog>(catalog));
    for (const auto& [accountId, models] : literal.at("unsupported").as_object()) {
      Record<double> observed;
      for (const auto& [model, at] : models.as_object()) observed.define(std::string(model), rt::toNumber(at));
      store.unsupported.define(std::string(accountId), std::move(observed));
    }
    return store;
  }

  CodexAccountCatalogInput catalogInput(std::string accountId, std::string clientVersion, json::array slugs) {
    return CodexAccountCatalogInput{.accountId = std::move(accountId), .clientVersion = std::move(clientVersion), .slugs = std::move(slugs)};
  }

  // `map.get(accountId)`: nullptr is undefined, an empty optional null.
  const std::optional<std::string>* detailOf(const rt::OrderedMap<std::string, std::optional<std::string>>& map, const std::string& accountId) {
    return map.get(accountId);
  }

  // Every case installs its own stub; module state outlives the test loop, so restore after.
  struct Scope {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    bool savedDeploy = uos::config::config.isDeploy;
    std::shared_ptr<StubState> state = std::make_shared<StubState>();
    std::shared_ptr<StubKv> kv = std::make_shared<StubKv>(state);
    Scope() {
      uos::config::config.codexAuthJsonB64.clear();
      uos::config::config.isDeploy = true;
      uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const uos::provider::capacity_events::ProviderCapacitySampleTriggerOptions&) {});
      uos::codex::auth::resetCodexAuthCacheForTest();
      uos::codex::account_routing::resetCodexAccountRoutingForTest();
      uos::kv::setKvForTest(kv);
      availability::resetCodexAccountModelsCacheForTest();
    }
    ~Scope() {
      uos::codex::auth::resetCodexAuthCacheForTest();
      uos::codex::account_routing::resetCodexAccountRoutingForTest();
      uos::provider::capacity_events::setProviderCapacitySampleTriggerForTest({});
      uos::config::config.codexAuthJsonB64 = savedSeed;
      uos::config::config.isDeploy = savedDeploy;
      availability::resetCodexAccountModelsCacheForTest();
      uos::kv::setKvForTest(nullptr);
    }
  };
  namespace auth = uos::codex::auth;
  namespace routing = uos::codex::routing_state;
  namespace account_routing = uos::codex::account_routing;
  namespace capacity_routing = uos::codex::capacity_routing;
  namespace dispatch = uos::codex::dispatch;
  namespace fetcher = uos::models::codex_models_fetch;
  using AuthState = uos::types::CodexAuthState;

  AuthState codexAccount(const std::string& accountId) {
    return {.access_token = accountId + "-access-token", .refresh_token = accountId + "-refresh-token", .account_id = accountId, .updated_at_ms = rt::dateNow()};
  }
  uos::types::CodexAuthPoolState poolOf(const std::vector<AuthState>& accounts) { return {.accounts = accounts, .updated_at_ms = 1}; }
  std::string seedPool(Scope& scope, const std::vector<AuthState>& accounts) {
    const std::string stamp = scope.state->writeKv(auth::CODEX_AUTH_POOL_KV_KEY, rt::KvValue(json::value_from(poolOf(accounts))));
    auth::resetCodexAuthCacheForTest();
    return stamp;
  }
  rt::Task<void> seedActiveRow(std::shared_ptr<StubState> state, AuthState account, std::string poolVersionstamp, double slot, double generation) {
    rt::Promise<capacity_routing::CodexRoutingAccountIdentity> identifying(capacity_routing::routingAccountIdentity(account));
    const auto identity = co_await identifying.wait();
    routing::CodexActiveAccountSelection active;
    active.account_id_hash = identity.accountIdHash;
    active.credential_version = identity.credentialVersion;
    active.pool_versionstamp = poolVersionstamp;
    active.slot = slot;
    active.generation = generation;
    active.updated_at_ms = 1;
    state->writeKv(routing::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, rt::KvValue(routing::toJson(active)));
  }
  std::optional<routing::CodexActiveAccountSelection> activeOf(const Scope& scope) {
    const auto* value = scope.state->readKv(routing::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY);
    if (!value) return std::nullopt;
    return routing::parseCodexActiveAccountSelection(value->toJson());
  }
  rt::Response jsonResponse(const json::value& payload, double status = 200) {
    return rt::Response(rt::BodyInit(rt::jsonStringify(payload)), rt::ResponseInit{.status = status, .headers = rt::Headers{{"Content-Type", "application/json"}}});
  }
  struct CatalogSeen { std::optional<std::string> ifNoneMatch = "unset"; int calls = 0; };
  using CatalogBodies = rt::OrderedMap<std::string, std::variant<json::value, rt::Response>>;
  rt::Fetch catalogFetch(CatalogBodies byAccount, CatalogSeen* seen = nullptr) {
    return rt::Fetch([byAccount = std::move(byAccount), seen](rt::RequestInfo, rt::RequestInit init) {
      const rt::Headers headers = init.headers.value_or(rt::Headers());
      if (seen) { seen->calls += 1; seen->ifNoneMatch = headers.get("If-None-Match"); }
      const auto* body = byAccount.get(headers.get("ChatGPT-Account-ID").value_or(""));
      if (body) {
        if (const auto* response = std::get_if<rt::Response>(body)) return rt::Promise<rt::Response>::resolve(*response).wait();
        return rt::Promise<rt::Response>::resolve(jsonResponse(std::get<json::value>(*body))).wait();
      }
      return rt::Promise<rt::Response>::resolve(jsonResponse(json::object{{"models", json::array()}})).wait();
    });
  }
  rt::Task<rt::Response> fetchModels(std::optional<std::string> ifNoneMatch = std::nullopt) {
    fetcher::FetchCodexModelsOptions options;
    options.clientVersion = "0.160.0";
    options.ifNoneMatch = std::move(ifNoneMatch);
    return fetcher::fetchCodexModels(options);
  }
  std::vector<std::string> slugsOf(const json::value& payload) {
    std::vector<std::string> slugs;
    for (const auto& model : payload.at("models").as_array()) slugs.emplace_back(rt::getString(model, "slug").value_or(""));
    return slugs;
  }
}

// ── B1: per-account catalog union ────────────────────────────────────────────

ASYNC_TEST("codex models: the pool catalog unions every account in pool order and records each catalog") {
  Scope scope;
  seedPool(scope, {codexAccount("union-a"), codexAccount("union-b")});
  const json::value daybreakRow = json::object{{"slug", MODEL}, {"context_window", 128'000}};
  CatalogSeen seen;
  const CatalogBodies bodies{{"union-a", json::value(json::object{{"models", json::array{daybreakRow, json::object{{"slug", "shared-model"}}}}, {"has_more", true}})},
                             {"union-b", json::value(rt::jsonParse(R"({"models":[{"slug":"shared-model"},{"slug":"sibling-only"}],"has_more":false})"))}};
  const rt::GlobalFetchOverride fetch(catalogFetch(bodies, &seen));
  rt::Promise<rt::Response> fetching(fetchModels(std::string("\"cached\"")));
  const auto response = co_await fetching.wait();
  EXPECT_EQ(response.status(), 200);
  const auto payload = co_await response.json();
  EXPECT_EQ(slugsOf(payload), (std::vector<std::string>{MODEL, "shared-model", "sibling-only"}));
  EXPECT(rt::deepEqual(payload.at("models").as_array()[0], daybreakRow));
  EXPECT_EQ(rt::getBool(payload, "has_more"), std::optional<bool>(true));
  EXPECT_EQ(seen.ifNoneMatch, std::nullopt);
  EXPECT_EQ(seen.calls, 2);
  const auto store = availability::getCodexAccountModelsCacheForTest().store;
  ASSERT(store);
  ASSERT(store->accounts.get("union-a"));
  ASSERT(store->accounts.get("union-b"));
  EXPECT_EQ(store->accounts.get("union-a")->slugs, (std::vector<std::string>{MODEL, "shared-model"}));
  EXPECT_EQ(store->accounts.get("union-b")->slugs, (std::vector<std::string>{"shared-model", "sibling-only"}));
}

ASYNC_TEST("codex models: unusable account rows are dropped while valid rows and sibling eligibility survive") {
  Scope scope;
  seedPool(scope, {codexAccount("rows-a"), codexAccount("rows-b")});
  const json::value firstRow = rt::jsonParse(R"({"slug":"first-account-model","context_window":128000,"supported_reasoning_levels":[{"effort":"ultra","description":"Source tier"}],"source_metadata":{"retained":true}})");
  const json::value siblingRow = json::object{{"slug", MODEL}, {"context_window", 256'000}, {"supported_reasoning_levels", rt::jsonParse(R"([{"effort":"low","description":"Sibling tier"}])")}};
  json::array invalids = rt::jsonParse(R"([null,{}, {"slug":" "},{"slug":7},"invalid",false,42,[]])").as_array();
  invalids.push_back(firstRow);
  json::object duplicate = firstRow.as_object(); duplicate["context_window"] = 1;
  const CatalogBodies bodies{{"rows-a", json::value(json::object{{"models", invalids}, {"has_more", true}})},
                             {"rows-b", json::value(json::object{{"models", json::array{duplicate, siblingRow}}, {"has_more", false}})}};
  CatalogSeen seen;
  const rt::GlobalFetchOverride fetch(catalogFetch(bodies, &seen));
  rt::Promise<rt::Response> fetching(fetchModels(std::string("\"cached\"")));
  const auto response = co_await fetching.wait();
  EXPECT_EQ(response.status(), 200);
  const auto payload = co_await response.json();
  EXPECT(rt::deepEqual(payload, json::object{{"models", json::array{firstRow, siblingRow}}, {"has_more", true}}));
  EXPECT_EQ(seen.calls, 2);
  EXPECT_EQ(seen.ifNoneMatch, std::nullopt);
  const auto store = availability::getCodexAccountModelsCacheForTest().store;
  ASSERT(store);
  ASSERT(store->accounts.get("rows-a")); ASSERT(store->accounts.get("rows-b"));
  EXPECT_EQ(store->accounts.get("rows-a")->slugs, (std::vector<std::string>{"first-account-model"}));
  EXPECT_EQ(store->accounts.get("rows-b")->slugs, (std::vector<std::string>{"first-account-model", MODEL}));
  EXPECT(availability::codexModelUnavailableAccounts(MODEL, {"rows-a", "rows-b"}).has("rows-a"));
  EXPECT(!availability::codexModelUnavailableAccounts(MODEL, {"rows-a", "rows-b"}).has("rows-b"));
  EXPECT_EQ(availability::codexModelUnavailableAccounts("unknown-model", {"rows-a", "rows-b"}).size(), std::size_t{0});
}

ASYNC_TEST("codex models: one configured account keeps the upstream response and its conditional request") {
  Scope scope;
  seedPool(scope, {codexAccount("single-account")});
  CatalogSeen seen;
  const rt::Fetch mock([&seen](rt::RequestInfo, rt::RequestInit init) {
    seen.calls += 1;
    seen.ifNoneMatch = init.headers.value_or(rt::Headers()).get("If-None-Match");
    const rt::Response response(nullptr, rt::ResponseInit{.status = 304, .headers = rt::Headers{{"ETag", "\"catalog-1\""}}});
    return rt::Promise<rt::Response>::resolve(response).wait();
  });
  const rt::GlobalFetchOverride fetch(mock);
  rt::Promise<rt::Response> fetching(fetchModels(std::string("\"catalog-1\"")));
  const auto response = co_await fetching.wait();
  EXPECT_EQ(response.status(), 304);
  EXPECT_EQ(response.headers().get("ETag"), std::optional<std::string>("\"catalog-1\""));
  EXPECT_EQ(seen.ifNoneMatch, std::optional<std::string>("\"catalog-1\""));
  EXPECT_EQ(seen.calls, 1);
  EXPECT(!availability::getCodexAccountModelsCacheForTest().store);
}

ASYNC_TEST("codex models: a fresh single-account catalog clears only its advertised model rejection") {
  Scope scope;
  const std::vector<AuthState> accounts{codexAccount("single-refresh")};
  const std::string stamp = seedPool(scope, accounts);
  auto seeding = seedActiveRow(scope.state, accounts[0], stamp, 0, 1); co_await std::move(seeding);
  std::vector<CodexAccountCatalogInput> siblings{catalogInput("sibling", "0.159.0", json::array{"sibling-only"})};
  auto recording = availability::recordCodexAccountCatalogs(siblings); co_await std::move(recording);
  CodexModelUnsupportedOptions options; options.detail = UNSUPPORTED_DETAIL;
  auto learning = availability::recordCodexModelUnsupported(accounts[0].account_id, MODEL, options); co_await std::move(learning);
  const std::string absentModel = "absent-model";
  auto absent = availability::recordCodexModelUnsupported(accounts[0].account_id, absentModel); co_await std::move(absent);
  const std::string siblingId = "sibling";
  auto sibling = availability::recordCodexModelUnsupported(siblingId, MODEL); co_await std::move(sibling);
  const auto before = availability::getCodexAccountModelsCacheForTest().store; ASSERT(before);
  auto selecting = account_routing::selectCodexRoutingAccountsStrong(poolOf(accounts), accounts, rt::dateNow(), MODEL);
  const auto unavailable = co_await std::move(selecting);
  EXPECT(std::holds_alternative<routing::RouteSelectionModelUnavailable>(unavailable));
  const std::string body = rt::jsonStringify(json::object{{"models", json::array{nullptr, json::object{{"slug", " "}}, json::object{{"slug", MODEL}, {"supported_reasoning_levels", rt::jsonParse(R"([{"effort":"ultra","description":"Native tier"}])")}, {"default_reasoning_level", "ultra"}}, json::object{{"id", "another-model"}}, json::object{{"slug", MODEL}}}}, {"has_more", false}});
  CatalogSeen seen;
  const rt::Fetch mock([&](rt::RequestInfo, rt::RequestInit init) {
    seen.calls += 1; const rt::Headers headers = init.headers.value_or(rt::Headers()); seen.ifNoneMatch = headers.get("If-None-Match");
    EXPECT_EQ(headers.get("ChatGPT-Account-ID"), std::optional<std::string>(accounts[0].account_id));
    const rt::Response response(rt::BodyInit(body), rt::ResponseInit{.headers = rt::Headers{{"Content-Type", "application/json"}, {"ETag", "\"fresh-catalog\""}}});
    return rt::Promise<rt::Response>::resolve(response).wait();
  });
  const rt::GlobalFetchOverride fetch(mock);
  rt::Promise<rt::Response> fetching(fetchModels(std::string("\"old-catalog\"")));
  const auto response = co_await fetching.wait();
  EXPECT_EQ(response.status(), 200); EXPECT_EQ(response.headers().get("ETag"), std::optional<std::string>("\"fresh-catalog\""));
  const auto text = co_await response.text(); EXPECT_EQ(text, body);
  EXPECT_EQ(seen.ifNoneMatch, std::optional<std::string>("\"old-catalog\"")); EXPECT_EQ(seen.calls, 1);
  const auto store = availability::getCodexAccountModelsCacheForTest().store; ASSERT(store);
  ASSERT(store->accounts.get(accounts[0].account_id));
  EXPECT_EQ(store->accounts.get(accounts[0].account_id)->slugs, (std::vector<std::string>{MODEL, "another-model"}));
  EXPECT_EQ(store->accounts.get(accounts[0].account_id)->client_version, "0.160.0");
  EXPECT_EQ(availability::toJson(*store).at("accounts").at("sibling"), availability::toJson(*before).at("accounts").at("sibling"));
  EXPECT_EQ(availability::toJson(*store).at("unsupported").at("sibling"), availability::toJson(*before).at("unsupported").at("sibling"));
  EXPECT(availability::codexModelUnavailableAccounts(absentModel, {accounts[0].account_id}).has(accounts[0].account_id));
  auto reselection = account_routing::selectCodexRoutingAccountsStrong(poolOf(accounts), accounts, rt::dateNow(), MODEL);
  const auto selected = co_await std::move(reselection);
  const auto* eligible = std::get_if<routing::RouteSelectionEligible>(&selected); ASSERT(eligible); ASSERT(!eligible->accounts.empty());
  EXPECT_EQ(eligible->accounts[0].auth.account_id, accounts[0].account_id);
}

ASYNC_TEST("codex models: a single-account 304, absent model or unusable catalog never clears a learned rejection") {
  Scope scope;
  seedPool(scope, {codexAccount("single-negative")});
  const std::vector<std::pair<double, std::optional<std::string>>> cases{{304, std::nullopt}, {200, "{\"models\":[{\"slug\":\"other-model\"}]}"}, {200, "not json"}, {200, "{\"data\":[]}"}, {500, "{\"models\":[{\"slug\":\"" + MODEL + "\"}]}"}};
  for (const auto& [status, body] : cases) {
    scope.state->rows.delete_(keyToString(availability::CODEX_ACCOUNT_MODELS_KV_KEY)); availability::resetCodexAccountModelsCacheForTest();
    std::vector<CodexAccountCatalogInput> entries{catalogInput("single-negative", "0.159.0", json::array{"old-model"})};
    auto recording = availability::recordCodexAccountCatalogs(entries); co_await std::move(recording);
    CodexModelUnsupportedOptions options; options.detail = UNSUPPORTED_DETAIL;
    const std::string accountId = "single-negative";
    auto learning = availability::recordCodexModelUnsupported(accountId, MODEL, options); co_await std::move(learning);
    const auto before = availability::getCodexAccountModelsCacheForTest().store; ASSERT(before);
    const rt::Response upstream(body ? rt::BodyInit(*body) : rt::BodyInit(nullptr), rt::ResponseInit{.status = status, .headers = rt::Headers{{"ETag", "\"unchanged\""}}});
    const rt::Fetch mock([upstream](rt::RequestInfo, rt::RequestInit) { return rt::Promise<rt::Response>::resolve(upstream).wait(); });
    const rt::GlobalFetchOverride fetch(mock);
    rt::Promise<rt::Response> fetching(fetchModels(std::string("\"unchanged\""))); const auto response = co_await fetching.wait();
    EXPECT_EQ(response.status(), status); EXPECT_EQ(response.headers().get("ETag"), std::optional<std::string>("\"unchanged\""));
    const auto text = co_await response.text(); EXPECT_EQ(text, body.value_or(""));
    const auto unavailable = availability::codexModelUnavailableAccounts(MODEL, {accountId}); ASSERT(unavailable.get(accountId));
    EXPECT_EQ(*unavailable.get(accountId), std::optional<std::string>(UNSUPPORTED_DETAIL));
    const auto after = availability::getCodexAccountModelsCacheForTest().store; ASSERT(after);
    EXPECT_EQ(availability::toJson(*after).at("unsupported"), availability::toJson(*before).at("unsupported"));
    if (body != std::optional<std::string>("{\"models\":[{\"slug\":\"other-model\"}]}")) EXPECT_EQ(availability::toJson(*after).at("accounts"), availability::toJson(*before).at("accounts"));
  }
}

ASYNC_TEST("codex models: a failing account keeps the sibling's rows, and every account failing returns the fallback") {
  Scope scope; seedPool(scope, {codexAccount("union-a"), codexAccount("union-b")});
  const rt::Response first(rt::BodyInit(std::string("a-quota")), rt::ResponseInit{.status = 429, .headers = rt::Headers{{"Content-Type", "application/json"}}});
  const CatalogBodies mixedBodies{{"union-a", first}, {"union-b", json::value(json::object{{"models", json::array{json::object{{"slug", MODEL}}}}})}};
  {
    const rt::GlobalFetchOverride fetch(catalogFetch(mixedBodies));
    rt::Promise<rt::Response> fetching(fetchModels()); const auto response = co_await fetching.wait(); EXPECT_EQ(response.status(), 200);
    const auto payload = co_await response.json(); EXPECT_EQ(slugsOf(payload), (std::vector<std::string>{MODEL}));
  }
  const rt::Response a(rt::BodyInit(std::string("a-quota")), rt::ResponseInit{.status = 429, .headers = rt::Headers{{"Content-Type", "application/json"}}});
  const rt::Response b(rt::BodyInit(std::string("b-quota")), rt::ResponseInit{.status = 429, .headers = rt::Headers{{"Content-Type", "application/json"}}});
  const CatalogBodies failedBodies{{"union-a", a}, {"union-b", b}};
  const rt::GlobalFetchOverride fetch(catalogFetch(failedBodies));
  rt::Promise<rt::Response> fetching(fetchModels()); const auto response = co_await fetching.wait(); EXPECT_EQ(response.status(), 429);
  const auto text = co_await response.text(); EXPECT_EQ(text, "b-quota");
}

ASYNC_TEST("codex models: a malformed account body does not poison the union") {
  Scope scope; seedPool(scope, {codexAccount("union-a"), codexAccount("union-b")});
  const rt::Response malformed(rt::BodyInit(std::string("not json")), rt::ResponseInit{.status = 200, .headers = rt::Headers{{"Content-Type", "application/json"}}});
  const CatalogBodies bodies{{"union-a", malformed}, {"union-b", json::value(json::object{{"models", json::array{json::object{{"slug", MODEL}}, json::object{{"slug", "shared-model"}}}}})}};
  const rt::GlobalFetchOverride fetch(catalogFetch(bodies));
  rt::Promise<rt::Response> fetching(fetchModels()); const auto response = co_await fetching.wait(); EXPECT_EQ(response.status(), 200);
  const auto payload = co_await response.json(); EXPECT_EQ(slugsOf(payload), (std::vector<std::string>{MODEL, "shared-model"}));
  const auto store = availability::getCodexAccountModelsCacheForTest().store; ASSERT(store);
  EXPECT(!store->accounts.get("union-a")); ASSERT(store->accounts.get("union-b"));
  EXPECT_EQ(store->accounts.get("union-b")->slugs, (std::vector<std::string>{MODEL, "shared-model"}));
}

// ── B2: per-account availability store ───────────────────────────────────────

ASYNC_TEST("codex account models: the durable round-trip keeps only non-secret catalog fields") {
  Scope scope;
  std::vector<CodexAccountCatalogInput> entries;
  entries.push_back(catalogInput("store-a", "0.160.0", json::array{json::value(MODEL), json::value(MODEL), json::value("shared-model")}));
  auto recording = availability::recordCodexAccountCatalogs(std::move(entries));
  co_await std::move(recording);
  const rt::KvValue* stored = scope.state->readKv(availability::CODEX_ACCOUNT_MODELS_KV_KEY);
  ASSERT(stored != nullptr);
  EXPECT((*stored)["accounts"]["store-a"]["slugs"] == rt::KvValue(json::array{json::value(MODEL), json::value("shared-model")}));
  EXPECT((*stored)["accounts"]["store-a"]["client_version"] == rt::KvValue("0.160.0"));
  EXPECT((*stored)["accounts"]["store-a"]["updated_at_ms"].isNumber());
  const std::string serialized = rt::jsonStringify(stored->toJson());
  for (const std::string forbidden : {"access_token", "refresh_token", "authorization", "secret"}) {
    EXPECT_EQ(rt::includes(serialized, forbidden), false);
  }
  const std::optional<CodexAccountModelsStore> parsed = availability::parseCodexAccountModelsStore(*stored);
  ASSERT(parsed.has_value());
  EXPECT(rt::KvValue(availability::toJson(*parsed)) == *stored);
}

ASYNC_TEST("codex account models: malformed durable values are rejected or dropped") {
  Scope scope;
  EXPECT(!availability::parseCodexAccountModelsStore(rt::KvValue("nope")).has_value());
  EXPECT(!availability::parseCodexAccountModelsStore(rt::KvValue(42)).has_value());
  EXPECT(!availability::parseCodexAccountModelsStore(rt::KvValue(json::parse(R"({"accounts":"x","unsupported":"y"})"))).has_value());
  const std::optional<CodexAccountModelsStore> parsed = availability::parseCodexAccountModelsStore(rt::KvValue(json::parse(R"({
    "accounts": {
      "good": {"client_version": "0.160.0", "slugs": ["a", "b"], "updated_at_ms": 5},
      "bad": {"client_version": 7, "slugs": ["a"], "updated_at_ms": 5}
    },
    "unsupported": {"good": {"model": 10, "stale": "x"}, "broken": "nope"}
  })")));
  ASSERT(parsed.has_value());
  EXPECT_EQ(parsed->accounts.keys(), std::vector<std::string>{"good"});
  EXPECT_EQ(availability::toJson(*parsed).at("unsupported"), json::parse(R"({"good":{"model":10}})"));
  co_return;
}

ASYNC_TEST("codex account models: a fresh catalog clears a learned rejection and the unsupported map is bounded") {
  Scope scope;
  CodexModelUnsupportedOptions options;
  options.detail = UNSUPPORTED_DETAIL;
  options.nowMs = rt::dateNow();
  auto learning = availability::recordCodexModelUnsupported("store-a", MODEL, options);
  co_await std::move(learning);
  const auto learned = availability::codexModelUnavailableAccounts(MODEL, {"store-a"});
  ASSERT(detailOf(learned, "store-a") != nullptr);
  EXPECT_EQ(*detailOf(learned, "store-a"), std::optional<std::string>(UNSUPPORTED_DETAIL));
  std::vector<CodexAccountCatalogInput> entries;
  entries.push_back(catalogInput("store-a", "0.160.0", json::array{json::value(MODEL)}));
  auto recording = availability::recordCodexAccountCatalogs(std::move(entries));
  co_await std::move(recording);
  EXPECT_EQ(availability::codexModelUnavailableAccounts(MODEL, {"store-a"}).has("store-a"), false);

  CodexAccountModelsStore store = availability::emptyCodexAccountModelsStore();
  for (double index = 0; index < 40; index += 1)
    store = availability::withCodexModelUnsupported(store, "store-a", "model-" + rt::numberToString(index), 1'000 + index, 1'000 + index);
  const Record<double>* bounded = store.unsupported.get("store-a");
  ASSERT(bounded != nullptr);
  EXPECT_EQ(static_cast<double>(bounded->keys().size()), availability::CODEX_ACCOUNT_MODELS_UNSUPPORTED_MAX_COUNT);
  EXPECT(bounded->get("model-39") != nullptr);
  EXPECT(bounded->get("model-0") == nullptr);
  const CodexAccountModelsStore aged = availability::withCodexModelUnsupported(availability::emptyCodexAccountModelsStore(), "store-a", "model-old", 1'000,
                                                                               1'000 + availability::CODEX_ACCOUNT_MODELS_UNSUPPORTED_MAX_AGE_MS + 1);
  ASSERT(aged.unsupported.get("store-a") != nullptr);
  EXPECT_EQ(aged.unsupported.get("store-a")->size(), std::size_t{0});
}

ASYNC_TEST("codex account models: a stale cache keeps serving and refreshes in the background") {
  Scope scope;
  std::vector<CodexAccountCatalogInput> entries;
  entries.push_back(catalogInput("cache-a", "0.160.0", json::array{json::value("other")}));
  const CodexAccountModelsStore staleStore = availability::mergeCodexAccountCatalogs(availability::emptyCodexAccountModelsStore(), entries, 1);
  availability::setCodexAccountModelsStoreForTest(staleStore, rt::dateNow());
  EXPECT_EQ(availability::codexModelUnavailableAccounts(MODEL, {"cache-a"}).size(), std::size_t{0});
  EXPECT_EQ(availability::getCodexAccountModelsCacheForTest().refreshing, false);

  scope.state->writeKv(availability::CODEX_ACCOUNT_MODELS_KV_KEY, rt::KvValue(json::parse(R"({
    "accounts": {
      "cache-a": {"client_version": "0.160.0", "slugs": ["other"], "updated_at_ms": 1},
      "cache-b": {"client_version": "0.160.0", "slugs": [")" + MODEL + R"("], "updated_at_ms": 1}
    },
    "unsupported": {}
  })")));
  availability::setCodexAccountModelsStoreForTest(staleStore, rt::dateNow() - availability::CODEX_ACCOUNT_MODELS_CACHE_TTL_MS - 1);
  EXPECT_EQ(availability::codexModelUnavailableAccounts(MODEL, {"cache-a"}).size(), std::size_t{0});
  EXPECT_EQ(availability::getCodexAccountModelsCacheForTest().refreshing, true);
  auto refreshing = availability::awaitCodexAccountModelsRefreshForTest();
  co_await std::move(refreshing);
  EXPECT_EQ(availability::getCodexAccountModelsCacheForTest().refreshing, false);
  EXPECT_EQ(availability::codexModelUnavailableAccounts(MODEL, {"cache-a", "cache-b"}).has("cache-a"), true);
}

ASYNC_TEST("codex account models: catalog absence counts only against a sibling's same client version") {
  Scope scope;
  const CodexAccountModelsStore differentVersion = storeFrom(json::parse(R"({
    "accounts": {
      "version-a": {"client_version": "0.160.0", "slugs": ["other"], "updated_at_ms": 1},
      "version-b": {"client_version": "0.161.0", "slugs": [")" + MODEL + R"("], "updated_at_ms": 1}
    },
    "unsupported": {}
  })"));
  availability::setCodexAccountModelsStoreForTest(differentVersion, rt::dateNow());
  EXPECT_EQ(availability::codexModelUnavailableAccounts(MODEL, {"version-a", "version-b"}).size(), std::size_t{0});

  const CodexAccountModelsStore sameVersion = storeFrom(json::parse(R"({
    "accounts": {
      "version-a": {"client_version": "0.160.0", "slugs": ["other"], "updated_at_ms": 1},
      "version-b": {"client_version": "0.160.0", "slugs": [")" + MODEL + R"("], "updated_at_ms": 1}
    },
    "unsupported": {}
  })"));
  availability::setCodexAccountModelsStoreForTest(sameVersion, rt::dateNow());
  const auto first = availability::codexModelUnavailableAccounts(MODEL, {"version-a", "version-b"});
  ASSERT(detailOf(first, "version-a") != nullptr);
  EXPECT_EQ(*detailOf(first, "version-a"), std::optional<std::string>());
  EXPECT_EQ(availability::codexModelUnavailableAccounts(MODEL, {"version-a", "version-b"}).has("version-b"), false);
  co_return;
}

ASYNC_TEST("codex account models: a rejection older than the max age reads as unknown while a fresh one still skips") {
  Scope scope;
  const double nowMs = rt::dateNow();
  // The aged account's catalog omits the model and no sibling lists it, so only
  // the learned rejection could ever skip it: expiry must age it back to unknown.
  // The fresh observation sits one second inside the bound, far more than the
  // read latency this synchronous assertion can accumulate.
  json::object agedModels;
  agedModels[MODEL] = rt::makeNumber(nowMs - availability::CODEX_ACCOUNT_MODELS_UNSUPPORTED_MAX_AGE_MS - 1);
  json::object freshModels;
  freshModels[MODEL] = rt::makeNumber(nowMs - availability::CODEX_ACCOUNT_MODELS_UNSUPPORTED_MAX_AGE_MS + 1'000);
  json::object unsupported;
  unsupported["aged-a"] = std::move(agedModels);
  unsupported["fresh-b"] = std::move(freshModels);
  json::object literal = json::parse(R"({"accounts":{"aged-a":{"client_version":"0.160.0","slugs":["other-model"],"updated_at_ms":1}}})").as_object();
  literal["unsupported"] = std::move(unsupported);
  availability::setCodexAccountModelsStoreForTest(storeFrom(json::value(std::move(literal))), nowMs);
  const auto unavailable = availability::codexModelUnavailableAccounts(MODEL, {"aged-a", "fresh-b"});
  EXPECT_EQ(unavailable.has("aged-a"), false);
  ASSERT(detailOf(unavailable, "fresh-b") != nullptr);
  EXPECT_EQ(*detailOf(unavailable, "fresh-b"), std::optional<std::string>());
  co_return;
}

ASYNC_TEST("codex account models: only current pool accounts are returned and only their catalogs are sibling evidence") {
  Scope scope;
  std::vector<CodexAccountCatalogInput> entries;
  entries.push_back(catalogInput("pool-a", "0.160.0", json::array{json::value("other-model")}));
  entries.push_back(catalogInput("removed-b", "0.160.0", json::array{json::value(MODEL)}));
  auto recording = availability::recordCodexAccountCatalogs(std::move(entries));
  co_await std::move(recording);
  EXPECT_EQ(availability::codexModelUnavailableAccounts(MODEL, {"pool-a"}).has("pool-a"), false);
  const auto withSibling = availability::codexModelUnavailableAccounts(MODEL, {"pool-a", "removed-b"});
  ASSERT(detailOf(withSibling, "pool-a") != nullptr);
  EXPECT_EQ(*detailOf(withSibling, "pool-a"), std::optional<std::string>());

  CodexModelUnsupportedOptions options;
  options.detail = UNSUPPORTED_DETAIL;
  options.nowMs = rt::dateNow();
  auto learning = availability::recordCodexModelUnsupported("removed-b", MODEL, options);
  co_await std::move(learning);
  const auto currentPool = availability::codexModelUnavailableAccounts(MODEL, {"pool-a"});
  EXPECT_EQ(currentPool.size(), std::size_t{0});
  const auto both = availability::codexModelUnavailableAccounts(MODEL, {"pool-a", "removed-b"});
  ASSERT(detailOf(both, "removed-b") != nullptr);
  EXPECT_EQ(*detailOf(both, "removed-b"), std::optional<std::string>(UNSUPPORTED_DETAIL));
}

ASYNC_TEST("codex account models: a racing rejection write and catalog write both survive the versionstamp check") {
  Scope scope;
  const double nowMs = rt::dateNow();
  std::vector<CodexAccountCatalogInput> entries;
  entries.push_back(catalogInput("race-a", "0.160.0", json::array{json::value(MODEL)}));
  auto recording = availability::recordCodexAccountCatalogs(std::move(entries));
  co_await std::move(recording);
  // Another isolate commits its own merged catalog while this rejection write is
  // in flight: the read hands back the pre-race entry, so the first commit must
  // lose its versionstamp check before the writer retries against the fresh value.
  const auto reads = std::make_shared<int>(0);
  const std::shared_ptr<rt::Kv> raceKv = std::make_shared<RaceKv>(scope.state, reads);

  CodexModelUnsupportedOptions options;
  options.detail = UNSUPPORTED_DETAIL;
  options.nowMs = nowMs;
  options.kv = raceKv;
  auto learning = availability::recordCodexModelUnsupported("race-c", MODEL, options);
  co_await std::move(learning);
  EXPECT_EQ(*reads, 2);
  const rt::KvValue* stored = scope.state->readKv(availability::CODEX_ACCOUNT_MODELS_KV_KEY);
  ASSERT(stored != nullptr);
  std::vector<std::string> accountIds;
  for (const auto& [accountId, catalog] : (*stored)["accounts"].asObject()) accountIds.push_back(accountId);
  rt::sort(accountIds, [](const std::string& left, const std::string& right) { return static_cast<double>(rt::localeCompare(left, right)); });
  EXPECT_EQ(accountIds, (std::vector<std::string>{"race-a", "race-b"}));
  json::object expected;
  expected[MODEL] = rt::makeNumber(nowMs);
  EXPECT((*stored)["unsupported"]["race-c"] == rt::KvValue(expected));
  const std::optional<CodexAccountModelsStore> cached = availability::getCodexAccountModelsCacheForTest().store;
  ASSERT(cached.has_value());
  EXPECT(rt::KvValue(availability::toJson(*cached)) == *stored);

  std::vector<CodexAccountCatalogInput> fresh;
  fresh.push_back(catalogInput("race-c", "0.160.0", json::array{json::value(MODEL)}));
  auto clearing = availability::recordCodexAccountCatalogs(std::move(fresh), raceKv);
  co_await std::move(clearing);
  EXPECT_EQ(availability::codexModelUnavailableAccounts(MODEL, {"race-a", "race-b", "race-c"}).has("race-c"), false);
  const rt::KvValue* rewritten = scope.state->readKv(availability::CODEX_ACCOUNT_MODELS_KV_KEY);
  ASSERT(rewritten != nullptr);
  EXPECT((*rewritten)["unsupported"]["race-c"].isUndefined());
}

ASYNC_TEST("codex account models: a learned rejection merges on the fresh durable value, not a stale cache snapshot") {
  Scope scope;
  const double nowMs = rt::dateNow();
  std::vector<CodexAccountCatalogInput> entries;
  entries.push_back(catalogInput("stale-a", "0.160.0", json::array{json::value("other-model")}));
  availability::setCodexAccountModelsStoreForTest(availability::mergeCodexAccountCatalogs(availability::emptyCodexAccountModelsStore(), entries, 1), nowMs);
  scope.state->writeKv(availability::CODEX_ACCOUNT_MODELS_KV_KEY, rt::KvValue(json::parse(R"({
    "accounts": {"durable-b": {"client_version": "0.160.0", "slugs": ["other-model"], "updated_at_ms": 1}},
    "unsupported": {}
  })")));
  CodexModelUnsupportedOptions options;
  options.detail = UNSUPPORTED_DETAIL;
  options.nowMs = nowMs;
  auto learning = availability::recordCodexModelUnsupported("learn-c", MODEL, options);
  co_await std::move(learning);

  const rt::KvValue* stored = scope.state->readKv(availability::CODEX_ACCOUNT_MODELS_KV_KEY);
  ASSERT(stored != nullptr);
  std::vector<std::string> accountIds;
  for (const auto& [accountId, catalog] : (*stored)["accounts"].asObject()) accountIds.push_back(accountId);
  EXPECT_EQ(accountIds, std::vector<std::string>{"durable-b"});
  EXPECT((*stored)["accounts"]["stale-a"].isUndefined());
  json::object expected;
  expected[MODEL] = rt::makeNumber(nowMs);
  EXPECT((*stored)["unsupported"]["learn-c"] == rt::KvValue(expected));
  const std::optional<CodexAccountModelsStore> cached = availability::getCodexAccountModelsCacheForTest().store;
  EXPECT_EQ(cached ? cached->accounts.keys() : std::vector<std::string>(), std::vector<std::string>{"durable-b"});
}

ASYNC_TEST("codex account models: an unavailable or unwritable KV keeps both writers best effort") {
  Scope scope;
  const double nowMs = rt::dateNow();
  CodexModelUnsupportedOptions local;
  local.detail = UNSUPPORTED_DETAIL;
  local.nowMs = nowMs;
  local.kv = std::shared_ptr<rt::Kv>();
  auto learning = availability::recordCodexModelUnsupported("nolocal-a", MODEL, local);
  co_await std::move(learning);
  const auto unavailable = availability::codexModelUnavailableAccounts(MODEL, {"nolocal-a"});
  ASSERT(detailOf(unavailable, "nolocal-a") != nullptr);
  EXPECT_EQ(*detailOf(unavailable, "nolocal-a"), std::optional<std::string>(UNSUPPORTED_DETAIL));
  EXPECT(scope.state->readKv(availability::CODEX_ACCOUNT_MODELS_KV_KEY) == nullptr);

  const std::shared_ptr<rt::Kv> rejectingKv = std::make_shared<RejectingKv>(scope.state);
  std::vector<CodexAccountCatalogInput> entries;
  entries.push_back(catalogInput("fail-a", "0.160.0", json::array{json::value(MODEL)}));
  auto recording = availability::recordCodexAccountCatalogs(std::move(entries), rejectingKv);
  co_await std::move(recording);
  CodexModelUnsupportedOptions failing;
  failing.detail = UNSUPPORTED_DETAIL;
  failing.nowMs = nowMs;
  failing.kv = rejectingKv;
  auto rejected = availability::recordCodexModelUnsupported("fail-b", MODEL, failing);
  co_await std::move(rejected);
  EXPECT(scope.state->readKv(availability::CODEX_ACCOUNT_MODELS_KV_KEY) == nullptr);
}

// ── B3: eligibility-aware routing ────────────────────────────────────────────

ASYNC_TEST("codex routing: an ineligible active account yields to an entitled sibling with reason model_unavailable") {
  Scope scope;
  const std::vector<AuthState> accounts{codexAccount("route-a"), codexAccount("route-b")};
  const auto stamp = seedPool(scope, accounts);
  auto seeding = seedActiveRow(scope.state, accounts[0], stamp, 0, 1); co_await std::move(seeding);
  json::object catalogs;
  catalogs["route-a"] = json::object{{"client_version", "0.160.0"}, {"slugs", json::array{"other-model"}}, {"updated_at_ms", 1}};
  catalogs["route-b"] = json::object{{"client_version", "0.160.0"}, {"slugs", json::array{json::value(MODEL)}}, {"updated_at_ms", 1}};
  availability::setCodexAccountModelsStoreForTest(storeFrom(json::object{{"accounts", catalogs}, {"unsupported", json::object()}}), rt::dateNow());
  auto selecting = account_routing::selectCodexRoutingAccountsStrong(poolOf(accounts), accounts, rt::dateNow(), MODEL);
  const auto selection = co_await std::move(selecting);
  const auto* eligible = std::get_if<routing::RouteSelectionEligible>(&selection); ASSERT(eligible); ASSERT(!eligible->accounts.empty());
  EXPECT_EQ(eligible->accounts[0].auth.account_id, "route-b");
  const auto active = activeOf(scope); ASSERT(active);
  EXPECT_EQ(active->transition_reason, std::optional<std::string>("model_unavailable"));
  EXPECT_EQ(active->generation, 2);
  auto identifying = capacity_routing::routingAccountIdentity(accounts[1]); const auto identity = co_await std::move(identifying);
  EXPECT_EQ(active->account_id_hash, identity.accountIdHash);
}

ASYNC_TEST("codex routing: unknown availability never skips the active account") {
  Scope scope;
  const std::vector<AuthState> accounts{codexAccount("unknown-a"), codexAccount("unknown-b")};
  const auto stamp = seedPool(scope, accounts);
  auto seeding = seedActiveRow(scope.state, accounts[0], stamp, 0, 1); co_await std::move(seeding);
  auto selecting = account_routing::selectCodexRoutingAccountsStrong(poolOf(accounts), accounts, rt::dateNow(), MODEL);
  const auto selection = co_await std::move(selecting);
  const auto* eligible = std::get_if<routing::RouteSelectionEligible>(&selection); ASSERT(eligible); ASSERT(!eligible->accounts.empty());
  EXPECT_EQ(eligible->accounts[0].auth.account_id, "unknown-a");
  const auto active = activeOf(scope); ASSERT(active);
  EXPECT_EQ(active->transition_reason, std::nullopt); EXPECT_EQ(active->generation, 1);
}

ASYNC_TEST("codex routing: no entitled account answers a graceful model_not_found without any fence") {
  Scope scope;
  const std::vector<AuthState> accounts{codexAccount("blocked-a"), codexAccount("blocked-b")};
  const auto stamp = seedPool(scope, accounts);
  auto seeding = seedActiveRow(scope.state, accounts[0], stamp, 0, 1); co_await std::move(seeding);
  const double nowMs = rt::dateNow();
  auto store = availability::withCodexModelUnsupported(availability::emptyCodexAccountModelsStore(), "blocked-a", MODEL, nowMs);
  store = availability::withCodexModelUnsupported(store, "blocked-b", MODEL, nowMs);
  availability::setCodexAccountModelsStoreForTest(store, nowMs);
  auto selecting = account_routing::selectCodexRoutingAccountsStrong(poolOf(accounts), accounts, rt::dateNow(), MODEL);
  const auto selection = co_await std::move(selecting);
  const auto* unavailable = std::get_if<routing::RouteSelectionModelUnavailable>(&selection); ASSERT(unavailable); EXPECT_EQ(unavailable->model, MODEL);
  const auto response = dispatch::initialCodexSelectionResponse(selection); ASSERT(response); EXPECT_EQ(response->status(), 404);
  const auto payload = co_await response->json();
  EXPECT_EQ(rt::getString(payload.at("error"), "code"), std::optional<std::string>("model_not_found"));
  EXPECT_EQ(rt::getString(payload.at("error"), "type"), std::optional<std::string>("invalid_request_error"));
  EXPECT(rt::includes(rt::getString(payload.at("error"), "message").value_or(""), MODEL));
  if (const auto* state = scope.state->readKv(routing::CODEX_ACCOUNT_ROUTING_KV_KEY)) {
    const auto row = state->toJson();
    for (const auto& slot : row.at("slots").as_array()) {
      EXPECT(slot.at("quota_blocked_until_ms").is_null()); EXPECT(slot.at("quota_block_source").is_null());
      EXPECT(slot.at("invalid_credential_version").is_null()); EXPECT(slot.at("upstream_timeout_blocked_until_ms").is_null());
    }
  }
  const auto active = activeOf(scope); ASSERT(active); EXPECT_EQ(active->generation, 1);
}

// ── B4: learned fallback at dispatch ─────────────────────────────────────────

ASYNC_TEST("codex dispatch: only the exact model-not-supported detail is learned") {
  Scope scope;
  const dispatch::ParseCodexModelUnsupportedDetailResult expected{MODEL, UNSUPPORTED_DETAIL};
  EXPECT_EQ(dispatch::parseCodexModelUnsupportedDetail(json::object{{"detail", UNSUPPORTED_DETAIL}}), std::optional(expected));
  EXPECT(!dispatch::parseCodexModelUnsupportedDetail(rt::jsonParse(R"({"detail":"Some other failure."})")));
  EXPECT(!dispatch::parseCodexModelUnsupportedDetail(json::object{{"detail", UNSUPPORTED_DETAIL + " extra"}}));
  EXPECT(!dispatch::parseCodexModelUnsupportedDetail(rt::jsonParse(R"({"detail":7})")));
  EXPECT(!dispatch::parseCodexModelUnsupportedDetail(json::value("nope")));
  EXPECT(!dispatch::parseCodexModelUnsupportedDetail(json::object{{"error", json::object{{"detail", UNSUPPORTED_DETAIL}}}}));
  const auto matching = jsonResponse(json::object{{"detail", UNSUPPORTED_DETAIL}}, 400);
  auto parsing = dispatch::codexModelUnsupportedFromResponse(matching); const auto parsed = co_await std::move(parsing); EXPECT_EQ(parsed, std::optional(expected));
  const auto text = co_await matching.text(); EXPECT_EQ(text, rt::jsonStringify(json::object{{"detail", UNSUPPORTED_DETAIL}}));
  const auto other = jsonResponse(rt::jsonParse(R"({"detail":"Some other failure."})"), 400);
  auto otherParsing = dispatch::codexModelUnsupportedFromResponse(other); const auto otherParsed = co_await std::move(otherParsing); EXPECT(!otherParsed);
  const auto success = jsonResponse(json::object{{"detail", UNSUPPORTED_DETAIL}});
  auto successParsing = dispatch::codexModelUnsupportedFromResponse(success); const auto successParsed = co_await std::move(successParsing); EXPECT(!successParsed);
  const rt::Response malformed(rt::BodyInit(std::string("not json")), rt::ResponseInit{.status = 400});
  auto malformedParsing = dispatch::codexModelUnsupportedFromResponse(malformed); const auto malformedParsed = co_await std::move(malformedParsing); EXPECT(!malformedParsed);
}

ASYNC_TEST("codex dispatch: a learned rejection is attributed to the admitted account and writes no fence") {
  Scope scope;
  const std::vector<AuthState> accounts{codexAccount("learn-a"), codexAccount("learn-b")};
  const auto stamp = seedPool(scope, accounts);
  auto seeding = seedActiveRow(scope.state, accounts[0], stamp, 0, 1); co_await std::move(seeding);
  scope.state->rows.delete_(keyToString(routing::CODEX_ACCOUNT_ROUTING_KV_KEY));
  auto identifying = capacity_routing::routingAccountIdentity(accounts[0]); const auto identity = co_await std::move(identifying);
  const auto response = jsonResponse(json::object{{"detail", UNSUPPORTED_DETAIL}}, 400);
  auth::setCodexResponseAccountTelemetry(response, 1, accounts[0].account_id);
  routing::RoutingAccount admitted;
  admitted.auth = accounts[0]; admitted.slot = 0; admitted.accountIdHash = identity.accountIdHash;
  admitted.credentialVersion = identity.credentialVersion; admitted.activeGeneration = 1;
  auth::setCodexResponseActiveTelemetry(response, admitted);
  auto learning = dispatch::learnCodexModelUnavailable(response); const auto learned = co_await std::move(learning);
  const dispatch::CodexModelUnavailableAttempt expected{"learn-a", MODEL, UNSUPPORTED_DETAIL}; EXPECT_EQ(learned, std::optional(expected));
  const auto unavailable = availability::codexModelUnavailableAccounts(MODEL, {"learn-a", "learn-b"}); ASSERT(unavailable.get("learn-a"));
  EXPECT_EQ(*unavailable.get("learn-a"), std::optional<std::string>(UNSUPPORTED_DETAIL));
  const auto text = co_await response.text(); EXPECT_EQ(text, rt::jsonStringify(json::object{{"detail", UNSUPPORTED_DETAIL}}));
  EXPECT(!scope.state->readKv(routing::CODEX_ACCOUNT_ROUTING_KV_KEY));
  const auto active = activeOf(scope); ASSERT(active); EXPECT_EQ(active->generation, 1);
}

namespace {
  struct LoopState {
    rt::Response unsupported = jsonResponse(json::object{{"detail", UNSUPPORTED_DETAIL}}, 400);
    rt::Response sibling = jsonResponse(json::object{{"id", "served-by-sibling"}});
    rt::Response other = jsonResponse(json::object{{"detail", "A different upstream failure."}}, 400);
    int dispatches = 0;
    int advances = 0;
    int retries = 0;
    bool pendingRetry = false;
  };
  rt::Task<std::optional<dispatch::CodexModelUnavailableAttempt>> classifyModelUnavailable(rt::Response candidate) {
    rt::Promise<std::optional<dispatch::ParseCodexModelUnsupportedDetailResult>> parsing(dispatch::codexModelUnsupportedFromResponse(candidate));
    const auto parsed = co_await parsing.wait();
    if (!parsed) co_return std::nullopt;
    const dispatch::CodexModelUnavailableAttempt result{"loop-a", parsed->model, parsed->detail}; co_return result;
  }
  dispatch::CodexSerialAdmissionDrivers loopDrivers(std::shared_ptr<LoopState> state, int scenario) {
    dispatch::CodexSerialAdmissionDrivers drivers;
    drivers.runPendingShortRetry = [state]() {
      std::optional<rt::Response> response;
      if (state->pendingRetry) { state->pendingRetry = false; state->retries += 1; response = state->unsupported; }
      return rt::Promise<std::optional<rt::Response>>::resolve(response).wait();
    };
    drivers.dispatchActive = [state, scenario]() {
      state->dispatches += 1;
      std::optional<rt::Response> response;
      if (scenario == 1 && state->dispatches == 1) state->pendingRetry = true;
      else if (scenario == 3) response = state->other;
      else response = scenario == 2 || state->dispatches == 1 ? state->unsupported : state->sibling;
      return rt::Promise<std::optional<rt::Response>>::resolve(response).wait();
    };
    drivers.terminalTransportResponse = [] { return rt::Promise<std::optional<rt::Response>>::resolve(std::nullopt).wait(); };
    drivers.hasQueuedRetry = [state] { return state->pendingRetry; };
    drivers.reselectionRequested = [] { return false; };
    drivers.advanceReselection = [state] { state->advances += 1; return rt::Promise<std::optional<rt::Response>>::resolve(std::nullopt).wait(); };
    drivers.exhaustedResponse = [] {
      const rt::Response response(rt::BodyInit(std::string("exhausted")), rt::ResponseInit{.status = 503});
      return rt::Promise<rt::Response>::resolve(response).wait();
    };
    drivers.classifyModelUnavailable = classifyModelUnavailable;
    return drivers;
  }
}

ASYNC_TEST("codex dispatch: the learned 400 retries exactly one sibling and returns its response") {
  Scope scope; const auto state = std::make_shared<LoopState>();
  const auto drivers = loopDrivers(state, 0);
  rt::Promise<rt::Response> running(dispatch::runCodexSerialAdmissionLoop(drivers)); const auto response = co_await running.wait();
  EXPECT_EQ(response.status(), 200); const auto text = co_await response.text(); EXPECT_EQ(text, "{\"id\":\"served-by-sibling\"}");
  EXPECT_EQ(state->dispatches, 2); EXPECT_EQ(state->advances, 1);
}

ASYNC_TEST("codex dispatch: a generic 429 short retry classifies its unsupported 400 and retries one sibling") {
  Scope scope; const auto state = std::make_shared<LoopState>();
  const auto drivers = loopDrivers(state, 1);
  rt::Promise<rt::Response> running(dispatch::runCodexSerialAdmissionLoop(drivers)); const auto response = co_await running.wait();
  EXPECT_EQ(response.status(), 200); const auto text = co_await response.text(); EXPECT_EQ(text, "{\"id\":\"served-by-sibling\"}");
  EXPECT_EQ(state->retries, 1); EXPECT_EQ(state->dispatches, 2); EXPECT_EQ(state->advances, 1);
}

ASYNC_TEST("codex dispatch: a failing sibling returns the graceful 404 with a bounded attempt count") {
  Scope scope; const auto state = std::make_shared<LoopState>();
  const auto drivers = loopDrivers(state, 2);
  rt::Promise<rt::Response> running(dispatch::runCodexSerialAdmissionLoop(drivers)); const auto response = co_await running.wait();
  EXPECT_EQ(response.status(), 404); const auto payload = co_await response.json();
  EXPECT_EQ(rt::getString(payload.at("error"), "code"), std::optional<std::string>("model_not_found"));
  const std::string message = std::string(rt::getString(payload.at("error"), "message").value_or(""));
  EXPECT(rt::includes(message, MODEL)); EXPECT(rt::includes(message, UNSUPPORTED_DETAIL));
  EXPECT_EQ(state->dispatches, 2); EXPECT_EQ(state->advances, 1);
}

ASYNC_TEST("codex dispatch: every other 400 passes through byte-for-byte with no state change") {
  Scope scope; const auto state = std::make_shared<LoopState>();
  const auto drivers = loopDrivers(state, 3);
  rt::Promise<rt::Response> running(dispatch::runCodexSerialAdmissionLoop(drivers)); const auto response = co_await running.wait();
  EXPECT_EQ(response, state->other); EXPECT_EQ(response.status(), 400);
  const auto text = co_await response.text(); EXPECT_EQ(text, "{\"detail\":\"A different upstream failure.\"}");
  EXPECT_EQ(state->dispatches, 1); EXPECT_EQ(state->advances, 0);
}
