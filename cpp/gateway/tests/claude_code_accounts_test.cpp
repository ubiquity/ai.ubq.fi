// Links: tests/claude_code_accounts_test.cpp
// src/api_keys.cpp src/auth/relay.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp
// src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp
// src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/authority/native_paid_policy.cpp src/bounded_response_body.cpp
// src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_read.cpp src/codex/account_routing.cpp src/codex/auth.cpp
// src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp
// src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/native_auth.cpp
// src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp
// src/codex/responses_error.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp
// src/codex/routing_state.cpp src/config.cpp src/deepseek/flash_aliases.cpp src/deepseek/index.cpp
// src/defaults.cpp src/http.cpp src/kv.cpp src/metered_quota.cpp
// src/models/codex_models.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp src/models/openrouter_models.cpp
// src/openai_telemetry.cpp src/paid_fallback/claude_code_route.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp
// src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp
// src/paid_fallback/ledger_stats.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp
// src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
// src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
// src/provider/capacity_rollups.cpp src/provider/cerebras.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp
// src/provider/claude_capacity_rollups.cpp src/provider/claude_code.cpp src/provider/claude_code_access.cpp src/provider/claude_code_accounts.cpp
// src/provider/claude_code_credentials.cpp src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp
// src/provider/health.cpp src/provider/lithos.cpp src/provider/lithos_models.cpp src/provider/lithos_rate_limits.cpp
// src/provider/metered.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/surplus.cpp
// src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/release.cpp src/request_policy.cpp
// src/responses_failover_stream.cpp src/responses_stream.cpp src/runtime/websocket.cpp src/runtime_config.cpp
// src/storage/kv.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp
// src/types.cpp src/upstream_wire.cpp src/utils.cpp tests/helpers/counting_kv.cpp
//
// Original account, quota, health and route cases from tests/claude-code-accounts.test.ts.
// Additional direct access-policy parser and CAS cases exercise every branch of claude-code-access.ts.
#include "uos/provider/claude_code_access.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/claude_code_route.hpp"
#include "uos/provider/anthropic_replay.hpp"
#include "uos/provider/claude_capacity_history.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/storage/kv.hpp"
#include "helpers/counting_kv.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace access = uos::provider::claude_code_access;
  namespace accounts = uos::provider::claude_code_accounts;
  namespace credentials = uos::provider::claude_code_credentials;
  namespace quota = uos::provider::claude_code_quota;
  namespace providerHealth = uos::provider::health;
  namespace route = uos::paid_fallback::claude_code_route;
  uos::provider::claude_code::ClaudeCodeEnvLookup noEnvironment() { return [](const std::string&) { return std::optional<std::string>(); }; }
  struct StoreOwner { rt::MemoryKv store{rt::dateNow}; };
  class ConflictingKv : private StoreOwner, public rt::ForwardingKv {
  public:
    int commits = 0;
    int conflicts;
    explicit ConflictingKv(int count) : rt::ForwardingKv(store), conflicts(count) {}
    std::unique_ptr<rt::AtomicOperation> atomic() override {
      class Operation : public rt::ForwardingAtomicOperation {
      public:
        ConflictingKv& owner;
        Operation(ConflictingKv& target, std::unique_ptr<rt::AtomicOperation> operation) : rt::ForwardingAtomicOperation(std::move(operation)), owner(target) {}
        rt::Task<rt::KvCommitResult> commit() override {
          ++owner.commits;
          if (owner.commits <= owner.conflicts) return rt::fulfilledTask(rt::KvCommitResult{false, ""});
          return rt::ForwardingAtomicOperation::commit();
        }
      };
      return std::make_unique<Operation>(*this, rt::ForwardingKv::atomic());
    }
  };
}

ASYNC_TEST("principal policy seeds exactly the explicit environment and empty stored deny survives restart and account import") {
  const rt::env::ScopedEnvironment isolation{{{"HOME", "/nonexistent/w05c2"}}}; const auto kv = std::make_shared<rt::MemoryKv>(rt::dateNow);
  uos::provider::claude_code::ClaudeCodeEnvLookup initial = [](const std::string& name) { return name == "CLAUDE_CODE_ALLOWED_KEY_IDS" ? std::optional<std::string>("owner-a, owner-b") : std::nullopt; };
  auto seeding = access::loadClaudeCodeAccessPolicy(kv, initial); const auto seeded = co_await std::move(seeding);
  const std::vector<std::string> expected{"owner-a", "owner-b"}; EXPECT_EQ(seeded.allowed_key_ids, expected); EXPECT_EQ(seeded.source, "env");
  const std::vector<std::string> deny; auto storing = access::storeClaudeCodeAccessPolicy(*kv, deny); const auto stored = co_await std::move(storing); EXPECT(stored.allowed_key_ids.empty());
  uos::provider::claude_code::ClaudeCodeEnvLookup credential = [](const std::string& name) { return name == "CLAUDE_CODE_OAUTH_TOKEN" ? std::optional<std::string>("fixture-first-account") : std::nullopt; };
  auto importing = accounts::resolveClaudeCodeAccount(kv, credential); const auto binding = co_await std::move(importing); EXPECT(binding);
  credentials::resetClaudeCodeCredentialCacheForTest();
  uos::provider::claude_code::ClaudeCodeEnvLookup changed = [](const std::string&) { return std::optional<std::string>("new-unapproved-principal"); };
  auto loading = access::loadClaudeCodeAccessPolicy(kv, changed); const auto explicitPolicy = co_await std::move(loading);
  EXPECT_EQ(explicitPolicy.source, "kv"); EXPECT(explicitPolicy.allowed_key_ids.empty());
  credentials::resetClaudeCodeCredentialCacheForTest();
}

TEST("Claude principal ID parsing validates shape size UTF-16 whitespace controls and stable locale order") {
  for (const auto& value : std::vector<json::value>{nullptr, "id", json::object{}, json::array{1}, json::array{""}, json::array{" "}, json::array{"a,b"},
    json::array{"a\tb"}, json::array{"a\nb"}, json::array{std::string("a\x7f")}, json::array{std::string("a\x01")}, json::array{rt::repeat("a", 257)},
    json::array{rt::repeat("😀", 129)}, json::array{"a b"}}) EXPECT(!access::parseClaudeCodeAccessIds(value));
  json::array tooMany; for (int i = 0; i < 1'001; ++i) tooMany.push_back("id"); EXPECT(!access::parseClaudeCodeAccessIds(json::value(tooMany)));
  const json::value values = json::array{"b", "A", "a", "b", "a-b", "a_b"}; const auto parsed = access::parseClaudeCodeAccessIds(values); EXPECT(parsed);
  const std::vector<std::string> expected{"a", "A", "a_b", "a-b", "b"}; EXPECT_EQ(*parsed, expected);
  const auto boundary = access::parseClaudeCodeAccessIds(json::value(json::array{rt::repeat("😀", 128)})); EXPECT(boundary); EXPECT_EQ(boundary->size(), 1U);
  const auto empty = access::parseClaudeCodeAccessIds(json::value(json::array{})); EXPECT(empty); EXPECT(empty->empty());
}

ASYNC_TEST("Claude principal policy refuses absent storage and invalid stored rows without environment fallback") {
  const rt::env::ScopedEnvironment isolation{{{"HOME", "/nonexistent/w05c2"}}};
  auto absent = access::loadClaudeCodeAccessPolicy(nullptr); EXPECT_REJECTS_MESSAGE(std::move(absent), rt::JsError, "Claude principal policy storage is unavailable");
  const auto kv = std::make_shared<rt::MemoryKv>(rt::dateNow); const json::value valid = json::object{{"v", 1}, {"source", "env"}, {"allowed_key_ids", json::array{"owner"}}, {"updated_at_ms", 1}};
  std::vector<json::value> rows{false, "policy", json::array{}, json::object{}};
  for (const auto& [field, invalid] : std::vector<std::pair<std::string, json::value>>{{"v", 2}, {"source", "other"}, {"allowed_key_ids", json::array{"a,b"}}, {"updated_at_ms", 0}, {"updated_at_ms", 1.5}}) {
    auto row = valid; rt::objectSet(row.as_object(), field, invalid); rows.push_back(std::move(row));
  }
  int environmentReads = 0; uos::provider::claude_code::ClaudeCodeEnvLookup fallback = [&](const std::string&) { ++environmentReads; return std::optional<std::string>("owner"); };
  for (const auto& row : rows) {
    const rt::KvValue raw(row); auto writing = kv->set(access::CLAUDE_CODE_ACCESS_KEY, raw); const auto ignored = co_await std::move(writing); (void)ignored;
    auto loading = access::loadClaudeCodeAccessPolicy(kv, fallback); EXPECT_REJECTS_MESSAGE(std::move(loading), rt::JsError, "Claude principal policy is invalid; refusing environment fallback");
  }
  EXPECT_EQ(environmentReads, 0);
}

ASYNC_TEST("Claude principal policy retries exactly eight CAS attempts and preserves a successful retry") {
  const rt::env::ScopedEnvironment isolation{{{"HOME", "/nonexistent/w05c2"}}};
  const auto retrying = std::make_shared<ConflictingKv>(2); auto first = access::loadClaudeCodeAccessPolicy(retrying, noEnvironment()); const auto seeded = co_await std::move(first);
  EXPECT_EQ(retrying->commits, 3); EXPECT_EQ(seeded.source, "env"); EXPECT(seeded.allowed_key_ids.empty());
  const auto exhausted = std::make_shared<ConflictingKv>(8); auto failed = access::loadClaudeCodeAccessPolicy(exhausted, noEnvironment());
  EXPECT_REJECTS_MESSAGE(std::move(failed), rt::JsError, "Claude principal policy changed repeatedly"); EXPECT_EQ(exhausted->commits, 8);
}

ASYNC_TEST("Claude principal writer rejects malformed IDs and never overwrites the prior explicit policy") {
  const rt::env::ScopedEnvironment isolation{{{"HOME", "/nonexistent/w05c2"}}}; const auto kv = std::make_shared<rt::MemoryKv>(rt::dateNow);
  const std::vector<std::string> allowed{"owner-b", "owner-a", "owner-b"}; auto writing = access::storeClaudeCodeAccessPolicy(*kv, allowed); const auto written = co_await std::move(writing);
  const std::vector<std::string> expected{"owner-a", "owner-b"}; EXPECT_EQ(written.source, "kv"); EXPECT_EQ(written.allowed_key_ids, expected); EXPECT_EQ(written.updated_at_ms, rt::dateNow());
  const json::value invalid = json::array{"a,b"}; auto refused = access::storeClaudeCodeAccessPolicy(*kv, invalid); EXPECT_REJECTS_MESSAGE(std::move(refused), rt::JsError, "Claude principal IDs are invalid");
  auto reading = access::loadClaudeCodeAccessPolicy(kv, noEnvironment()); const auto actual = co_await std::move(reading); EXPECT_EQ(actual, written);
}

namespace {
  const std::string routeModel = "claude-fixture-model";
  auto accountEnv(bool principal = false) {
    return [principal](const std::string& name) -> std::optional<std::string> {
      if (name == "CLAUDE_CODE_OAUTH_TOKEN") return "fixture-first-account";
      if (principal && name == "CLAUDE_CODE_ALLOWED_KEY_IDS") return "fixture-owner";
      return std::nullopt;
    };
  }
  std::string accountFixtureTaskScratch() {
    const auto scratch = rt::env::get("TMPDIR");
    if (!scratch || scratch->empty()) throw rt::JsError("Account fixture requires the wrapper TMPDIR");
    return *scratch;
  }
  struct AccountFixture {
    const std::string taskScratch = accountFixtureTaskScratch();
    rt::env::ScopedEnvironment isolation{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c1-accounts"}, {"TMPDIR", taskScratch}}};
    std::string savedConfig = uos::config::config.codexAuthJsonB64;
    std::shared_ptr<rt::MemoryKv> store = std::make_shared<rt::MemoryKv>(rt::dateNow);
    uos::openai_telemetry::UsageContext usage;
    AccountFixture() {
      uos::config::config.codexAuthJsonB64.clear(); usage.keyId = "fixture-owner"; uos::kv::setKvForTest(store);
      credentials::resetClaudeCodeCredentialCacheForTest(); providerHealth::resetProviderHealthThrottleForTest();
      uos::provider::selection::resetProviderSelectionCacheForTest(); uos::provider::claude_code_models::resetClaudeCodeModelsCacheForTest();
    }
    ~AccountFixture() { uos::kv::setKvForTest(nullptr); uos::config::config.codexAuthJsonB64 = savedConfig; }
    rt::Task<std::optional<accounts::ClaudeCodeAccountBinding>> binding() { return accounts::resolveClaudeCodeAccount(store, accountEnv()); }
    rt::Task<accounts::ClaudeCodeAccountBinding> second(const accounts::ClaudeCodeAccountBinding& first, bool activate = false) {
      accounts::AccountUpdate update; update.createNew = true; update.activate = activate; update.credential = first.credential; update.credential->secret = "fixture-second";
      return accounts::updateClaudeCodeAccount(store, update);
    }
    rt::Task<std::optional<uos::openai_telemetry::RoutedResponsesUpstream>> call(rt::Fetch fetcher, std::string model = routeModel) {
      json::object body{{"model", model}, {"input", "fixture only"}, {"stream", false}};
      route::SubscriptionOptions options; options.model = model; options.env = accountEnv(true); options.usageContext = &usage; options.fetcher = fetcher;
      return route::attemptClaudeCodeSubscription(std::move(body), options);
    }
  };
  std::optional<quota::ClaudeCodeQuotaObservation> observation(std::string utilization = "0.25", std::optional<double> now = std::nullopt) {
    return quota::observeClaudeCodeQuota(rt::Headers{{"anthropic-ratelimit-unified-5h-status", "allowed"}, {"anthropic-ratelimit-unified-5h-utilization", utilization}}, now.value_or(rt::dateNow()));
  }
  quota::QuotaOutcome outcome(const accounts::ClaudeCodeAccountBinding& account, std::optional<quota::ClaudeCodeQuotaObservation> observed = observation()) {
    quota::QuotaOutcome input; input.binding = account; input.observation = observed; return input;
  }
  rt::Response accountSuccess(std::string model = routeModel) {
    return rt::Response::json(json::object{{"id", "msg_fixture"}, {"type", "message"}, {"role", "assistant"}, {"model", model},
      {"content", json::array{json::object{{"type", "text"}, {"text", "FIXTURE_OK"}}}}, {"stop_reason", "end_turn"}, {"usage", json::object{{"input_tokens", 2}, {"output_tokens", 1}}}});
  }
  rt::Response accountExhausted() {
    return rt::Response::json(json::object{{"type", "error"}, {"error", json::object{{"type", "rate_limit_error"}, {"message", "fixture quota exhausted"}}}},
      {.status = 429, .headers = rt::Headers{{"anthropic-ratelimit-unified-5h-status", "limited"}, {"anthropic-ratelimit-unified-5h-utilization", "1"},
        {"anthropic-ratelimit-unified-5h-reset", rt::numberToString(rt::Math::floor(rt::dateNow() / 1000) + 3600)}}});
  }
  quota::ObservationRecorder& scopedObservationRecorder() { static quota::ObservationRecorder callback; return callback; }
  rt::Task<void> forwardObservation(std::shared_ptr<rt::Kv> store, quota::ClaudeCodeAcceptedObservation event) {
    return scopedObservationRecorder()(std::move(store), std::move(event));
  }
  struct ObservationRecorderScope {
    std::function<void()> dispose;
    explicit ObservationRecorderScope(quota::ObservationRecorder recorder) {
      // This binary links capacity through the route. Remove only its known import-time recorder
      // to reproduce the original accounts test's module graph, then restore it on scope exit.
      bool occupied = false;
      const quota::ObservationRecorder probe = [](std::shared_ptr<rt::Kv>, quota::ClaudeCodeAcceptedObservation) { return rt::fulfilledTask(); };
      try { quota::registerClaudeCodeObservationRecorder(probe)(); }
      catch (const rt::JsError& error) { occupied = error.message == "Claude observation recorder is already installed"; }
      EXPECT(occupied);
      quota::registerClaudeCodeObservationRecorder(&uos::provider::claude_capacity_history::recordClaudeCapacityObservation)();
      EXPECT(!scopedObservationRecorder());
      scopedObservationRecorder() = std::move(recorder);
      dispose = quota::registerClaudeCodeObservationRecorder(&forwardObservation);
    }
    ~ObservationRecorderScope() { dispose(); scopedObservationRecorder() = {}; quota::registerClaudeCodeObservationRecorder(&uos::provider::claude_capacity_history::recordClaudeCapacityObservation); }
  };
}
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)

ASYNC_TEST("singleton bootstrap preserves original keys and never attributes the legacy global quota") {
  AccountFixture f;
  const json::value credential = json::object{{"secret", "fixture-singleton"}, {"kind", "oauth"}, {"hint", "configured"}, {"label", "not verified identity"}, {"updated_at_ms", 1234}};
  const json::value workspace = json::object{{"id", "wrkspc_singleton"}, {"name", nullptr}, {"source", "kv"}, {"discovered_at_ms", nullptr}, {"verified_at_ms", nullptr}, {"last_error", nullptr}};
  AWAIT(authWrite, f.store->set(credentials::CLAUDE_CODE_AUTH_KV_KEY, credential)); (void)authWrite;
  AWAIT(workspaceWrite, f.store->set(credentials::CLAUDE_CODE_WORKSPACE_KV_KEY, workspace)); (void)workspaceWrite;
  const rt::KvValue legacy(json::object{{"windows", json::object{}}, {"updated_at_ms", 1234}, {"blocked_until_ms", rt::dateNow() + 60000}});
  AWAIT(quotaWrite, f.store->set(quota::CLAUDE_CODE_QUOTA_KV_KEY, legacy)); (void)quotaWrite;
  AWAIT(account, f.binding()); EXPECT(account); EXPECT_EQ(account->credential.secret, "fixture-singleton"); EXPECT(account->workspace); EXPECT_EQ(account->workspace->id, "wrkspc_singleton");
  AWAIT(state, quota::loadClaudeCodeQuotaState(f.store, account)); EXPECT(!state);
  AWAIT(savedCredential, f.store->get(credentials::CLAUDE_CODE_AUTH_KV_KEY)); EXPECT(rt::deepEqual(savedCredential.value.toJson(), credential));
  AWAIT(savedWorkspace, f.store->get(credentials::CLAUDE_CODE_WORKSPACE_KV_KEY)); EXPECT(rt::deepEqual(savedWorkspace.value.toJson(), workspace));
  AWAIT(roster, accounts::loadClaudeCodeAccountStore(f.store)); EXPECT_EQ(roster.active_account_id, std::optional<std::string>(account->account_id));
}
ASYNC_TEST("a pristine passive read stays unpersisted and allows later intentional first environment bootstrap") {
  AccountFixture f; AWAIT(passive, accounts::resolveClaudeCodeAccount(f.store, noEnvironment())); EXPECT(!passive);
  AWAIT(empty, accounts::loadClaudeCodeAccountStore(f.store, noEnvironment())); EXPECT(empty.accounts.empty());
  AWAIT(absent, f.store->get(accounts::CLAUDE_CODE_ACCOUNTS_KEY)); EXPECT(absent.value.isNull());
  AWAIT(first, f.binding()); EXPECT(first); EXPECT_EQ(first->credential.secret, "fixture-first-account");
  AWAIT(store, accounts::loadClaudeCodeAccountStore(f.store)); EXPECT_EQ(store.active_account_id, std::optional<std::string>(first->account_id));
}
ASYNC_TEST("targeted missing-account deletion leaves pristine state untouched while explicit clear persists a tombstone") {
  AccountFixture f; const std::optional<std::string> missing("nonexistent-account");
  AWAIT(removed, accounts::removeClaudeCodeAccount(f.store, missing)); EXPECT(!removed);
  AWAIT(absent, f.store->get(accounts::CLAUDE_CODE_ACCOUNTS_KEY)); EXPECT(absent.value.isNull());
  AWAIT(cleared, accounts::removeClaudeCodeAccount(f.store)); EXPECT(!cleared);
  AWAIT(store, accounts::loadClaudeCodeAccountStore(f.store, accountEnv())); EXPECT(store.accounts.empty());
  AWAIT(tombstone, f.store->get(accounts::CLAUDE_CODE_ACCOUNTS_KEY)); EXPECT(!tombstone.value.isNull()); credentials::resetClaudeCodeCredentialCacheForTest();
  AWAIT(reloaded, f.binding()); EXPECT(!reloaded);
}
ASYNC_TEST("an explicit first credential import creates the absent roster through native CAS") {
  AccountFixture f; accounts::AccountUpdate update; update.credential = credentials::ClaudeCodeResolvedCredential{"oauth", "fixture-first-import", "fixture import", "unverified fixture label", "kv", rt::dateNow()};
  AWAIT(imported, accounts::updateClaudeCodeAccount(f.store, update)); AWAIT(store, accounts::loadClaudeCodeAccountStore(f.store));
  EXPECT_EQ(store.accounts.size(), 1u); EXPECT_EQ(store.active_account_id, std::optional<std::string>(imported.account_id)); EXPECT_EQ(store.active_reason, "first_configured");
}
ASYNC_TEST("concurrent native first bootstrap and explicit deletion always settle to the durable tombstone") {
  AccountFixture f; rt::Promise<std::optional<accounts::ClaudeCodeAccountBinding>> bootstrap(f.binding()); rt::Promise<bool> deletion(accounts::removeClaudeCodeAccount(f.store));
  const auto both = rt::all(bootstrap, deletion); const auto completed = co_await both.wait(); const auto bootstrapped = std::get<0>(completed);
  AWAIT(store, accounts::loadClaudeCodeAccountStore(f.store, accountEnv())); EXPECT(store.accounts.empty()); EXPECT(!store.active_account_id);
  AWAIT(reloaded, f.binding()); EXPECT(!reloaded); if (bootstrapped) { AWAIT(current, accounts::isClaudeCodeAccountCurrent(f.store, *bootstrapped)); EXPECT(!current); }
}
ASYNC_TEST("durable identity/cohort and global active survive cache resets and same-binding updates") {
  AccountFixture f; AWAIT(first, f.binding()); EXPECT(first); AWAIT(second, f.second(*first));
  accounts::AccountUpdate edit; edit.accountId = first->account_id; edit.credential = first->credential; edit.credential->label = "renamed locally";
  AWAIT(edited, accounts::updateClaudeCodeAccount(f.store, edit)); credentials::resetClaudeCodeCredentialCacheForTest(); AWAIT(active, f.binding()); EXPECT(active); EXPECT_EQ(active->account_id, first->account_id);
  EXPECT_EQ(edited.account_cohort_id, first->account_cohort_id); EXPECT_EQ(edited.evidence_generation, first->evidence_generation); EXPECT(second.account_cohort_id != first->account_cohort_id);
  AWAIT(selected, accounts::selectClaudeCodeAccount(f.store, second.account_id)); EXPECT(selected); AWAIT(next, f.binding()); EXPECT(next); EXPECT_EQ(next->account_id, second.account_id);
  AWAIT(store, accounts::loadClaudeCodeAccountStore(f.store)); EXPECT_EQ(store.active_reason, "admin_selection");
}
ASYNC_TEST("native persistent close/reopen retains account cohort, global active and observed quota") {
  AccountFixture f; EXPECT_EQ(rt::env::get("TMPDIR"), std::optional<std::string>(f.taskScratch));
  const auto scratchRoot = rt::path::resolve(f.taskScratch); EXPECT_EQ(rt::path::resolve(rt::os::tmpdir()), scratchRoot);
  const auto directory = rt::fs::mkdtempSync(rt::os::tmpdir() + "/claude-account-restart-");
  EXPECT_EQ(rt::path::dirname(directory), scratchRoot);
  const auto cleanup = rt::scopeExit([directory] { rt::fs::rmSync(directory, {.recursive = true, .force = true}); });
  const auto path = directory + "/fixture.sqlite3"; AWAIT(database, uos::storage::kv::openKv(path));
  AWAIT(first, accounts::resolveClaudeCodeAccount(database, accountEnv())); EXPECT(first); auto input = outcome(*first); input.served = true;
  AWAIT(recorded, quota::recordClaudeCodeQuotaOutcome(database, input)); EXPECT(recorded); database->close(); database.reset(); credentials::resetClaudeCodeCredentialCacheForTest();
  AWAIT(reopened, uos::storage::kv::openKv(path)); AWAIT(restarted, accounts::resolveClaudeCodeAccount(reopened, noEnvironment())); EXPECT(restarted);
  EXPECT_EQ(restarted->account_id, first->account_id); EXPECT_EQ(restarted->account_cohort_id, first->account_cohort_id);
  AWAIT(state, quota::loadClaudeCodeQuotaState(reopened, restarted)); EXPECT(state); EXPECT_EQ(state->windows.get("5h")->utilization, std::optional<double>(0.25)); reopened->close();
}
ASYNC_TEST("removal leaves a durable tombstone instead of resurrecting singleton or environment credentials") {
  AccountFixture f; const rt::KvValue legacy(json::object{{"secret", "fixture-retained"}, {"kind", "oauth"}, {"updated_at_ms", 1234}});
  AWAIT(seed, f.store->set(credentials::CLAUDE_CODE_AUTH_KV_KEY, legacy)); (void)seed; AWAIT(first, f.binding()); EXPECT(first);
  AWAIT(removed, accounts::removeClaudeCodeAccount(f.store, first->account_id)); EXPECT(removed);
  AWAIT(credential, credentials::resolveClaudeCodeCredential(f.store, accountEnv())); EXPECT(!credential); AWAIT(store, accounts::loadClaudeCodeAccountStore(f.store, accountEnv())); EXPECT(store.accounts.empty());
  AWAIT(retained, f.store->get(credentials::CLAUDE_CODE_AUTH_KV_KEY)); EXPECT(retained.value.truthy());
}
ASYNC_TEST("malformed existing roster fails closed rather than importing a credential again") {
  AccountFixture f; const rt::KvValue corrupt(json::object{{"v", 1}, {"accounts", "corrupt"}}); AWAIT(seed, f.store->set(accounts::CLAUDE_CODE_ACCOUNTS_KEY, corrupt)); (void)seed;
  bool rejected = false; try { AWAIT(result, f.binding()); (void)result; } catch (const rt::JsError& error) { rejected = error.message.find("invalid") != std::string::npos; } EXPECT(rejected);
}
ASYNC_TEST("credential and workspace replacement rotate the persisted cohort and reject late old evidence") {
  AccountFixture f; AWAIT(first, f.binding()); EXPECT(first); auto input = outcome(*first); input.served = true; AWAIT(recorded, quota::recordClaudeCodeQuotaOutcome(f.store, input)); EXPECT(recorded);
  accounts::AccountUpdate update; update.credential = first->credential; update.credential->secret = "fixture-rotated"; AWAIT(second, accounts::updateClaudeCodeAccount(f.store, update));
  EXPECT_EQ(second.account_id, first->account_id); EXPECT(second.account_cohort_id != first->account_cohort_id); AWAIT(current, accounts::isClaudeCodeAccountCurrent(f.store, *first)); EXPECT(!current);
  input = outcome(*first, observation("1")); input.exhaustion = quota::ClaudeCodeQuotaExhaustion{"5h", rt::dateNow() + 60000}; AWAIT(stale, quota::recordClaudeCodeQuotaOutcome(f.store, input)); EXPECT(!stale);
  AWAIT(secondState, quota::loadClaudeCodeQuotaState(f.store, second)); EXPECT(!secondState);
  accounts::AccountUpdate workspace; workspace.workspace = credentials::ClaudeCodeWorkspaceState{"wrkspc_new", std::nullopt, "kv", std::nullopt, std::nullopt, std::nullopt};
  AWAIT(third, accounts::updateClaudeCodeAccount(f.store, workspace)); EXPECT(third.account_cohort_id != second.account_cohort_id); AWAIT(thirdState, quota::loadClaudeCodeQuotaState(f.store, third)); EXPECT(!thirdState);
}
ASYNC_TEST("a still-valid non-active account may record only its own cohort") {
  AccountFixture f; AWAIT(first, f.binding()); EXPECT(first); AWAIT(second, f.second(*first, true)); AWAIT(current, accounts::isClaudeCodeAccountCurrent(f.store, *first)); EXPECT(current);
  auto input = outcome(*first); input.served = true; AWAIT(recorded, quota::recordClaudeCodeQuotaOutcome(f.store, input)); EXPECT(recorded);
  AWAIT(firstState, quota::loadClaudeCodeQuotaState(f.store, first)); EXPECT(firstState); EXPECT_EQ(firstState->windows.get("5h")->utilization, std::optional<double>(0.25));
  AWAIT(secondState, quota::loadClaudeCodeQuotaState(f.store, second)); EXPECT(!secondState);
}
ASYNC_TEST("guard clear preserves observed windows but invalidates older in-flight publication") {
  AccountFixture f; AWAIT(first, f.binding()); EXPECT(first); auto input = outcome(*first); input.config_error = quota::QuotaOutcome::ConfigError{"workspace_required", 400, "workspace required"};
  AWAIT(recorded, quota::recordClaudeCodeQuotaOutcome(f.store, input)); EXPECT(recorded); AWAIT(cleared, quota::clearClaudeCodeQuotaGuards(f.store, first)); EXPECT(cleared); EXPECT(!cleared->cooldown_until_ms);
  AWAIT(current, f.binding()); EXPECT(current); EXPECT_EQ(current->account_cohort_id, first->account_cohort_id); EXPECT(current->evidence_generation != first->evidence_generation);
  input = outcome(*first, observation("1")); input.transient = quota::QuotaOutcome::Transient{5000}; AWAIT(stale, quota::recordClaudeCodeQuotaOutcome(f.store, input)); EXPECT(!stale);
  AWAIT(state, quota::loadClaudeCodeQuotaState(f.store, current)); EXPECT(state); EXPECT_EQ(state->windows.get("5h")->utilization, std::optional<double>(0.25));
}
ASYNC_TEST("accepted recorder gets raw missing-window semantics and is never called for empty or rejected evidence") {
  AccountFixture f; std::vector<quota::ClaudeCodeAcceptedObservation> events; ObservationRecorderScope recording([&](std::shared_ptr<rt::Kv>, quota::ClaudeCodeAcceptedObservation event) { events.push_back(event); return rt::fulfilledTask(); });
  AWAIT(first, f.binding()); EXPECT(first); const auto raw = observation("0.15", 1000); auto input = outcome(*first, raw); input.response_status = 200; input.served = true; input.nowMs = 2000;
  AWAIT(recorded, quota::recordClaudeCodeQuotaOutcome(f.store, input)); EXPECT(recorded); EXPECT_EQ(events.size(), 1u); EXPECT_EQ(events.front().observation, *raw); EXPECT(!events.front().observation.windows.get("7d"));
  EXPECT_EQ(events.front().source_observed_at_ms, std::optional<double>(1000)); EXPECT_EQ(events.front().sampled_at_ms, 2000); EXPECT_EQ(events.front().credential_kind, "oauth"); EXPECT_EQ(events.front().state, "available");
  input = outcome(*first, std::nullopt); input.served = true; AWAIT(empty, quota::recordClaudeCodeQuotaOutcome(f.store, input)); (void)empty;
  input.observation = quota::ClaudeCodeQuotaObservation{{}, {}, std::nullopt, 3000}; AWAIT(noWindows, quota::recordClaudeCodeQuotaOutcome(f.store, input)); (void)noWindows;
  accounts::AccountUpdate update; update.credential = first->credential; update.credential->secret = "fixture-replaced"; AWAIT(replaced, accounts::updateClaudeCodeAccount(f.store, update)); (void)replaced;
  input = outcome(*first); AWAIT(stale, quota::recordClaudeCodeQuotaOutcome(f.store, input)); EXPECT(!stale); EXPECT_EQ(events.size(), 1u);
}
ASYNC_TEST("actual credential invalidity keeps nonempty header history unavailable without inventing missing windows") {
  AccountFixture f; std::vector<quota::ClaudeCodeAcceptedObservation> events; ObservationRecorderScope recording([&](std::shared_ptr<rt::Kv>, quota::ClaudeCodeAcceptedObservation event) { events.push_back(event); return rt::fulfilledTask(); });
  AWAIT(account, f.binding()); EXPECT(account); auto input = outcome(*account); input.model_id = routeModel; input.response_status = 401; input.auth_invalid = true;
  AWAIT(recorded, quota::recordClaudeCodeQuotaOutcome(f.store, input)); EXPECT(recorded); EXPECT_EQ(events.size(), 1u); EXPECT_EQ(events.front().state, "unavailable"); EXPECT_EQ(events.front().response_status, std::optional<double>(401)); EXPECT(!events.front().observation.windows.get("7d"));
}
ASYNC_TEST("concurrent accepted observations preserve independent window families under real KV CAS") {
  AccountFixture f; AWAIT(account, f.binding()); EXPECT(account); auto daily = outcome(*account); daily.served = true;
  auto weekly = outcome(*account, quota::observeClaudeCodeQuota(rt::Headers{{"anthropic-ratelimit-unified-7d-utilization", "0.1"}})); weekly.served = true;
  rt::Promise<std::optional<quota::ClaudeCodeQuotaState>> a(quota::recordClaudeCodeQuotaOutcome(f.store, daily)), b(quota::recordClaudeCodeQuotaOutcome(f.store, weekly));
  const auto both = rt::all(a, b); const auto completed = co_await both.wait(); (void)completed;
  AWAIT(state, quota::loadClaudeCodeQuotaState(f.store, account)); EXPECT(state); EXPECT_EQ(state->windows.get("5h")->utilization, std::optional<double>(0.25)); EXPECT_EQ(state->windows.get("7d")->utilization, std::optional<double>(0.1));
}
ASYNC_TEST("health cannot cross credential replacement or a guarded clear generation") {
  AccountFixture f; AWAIT(first, f.binding()); EXPECT(first); auto recording = providerHealth::recordClaudeCodeProviderHealth("success", 200, rt::dateNow, "req_first", first); co_await recording.wait();
  AWAIT(initial, providerHealth::getClaudeCodeProviderHealth(rt::dateNow, first)); EXPECT_EQ(initial.state, "healthy");
  accounts::AccountUpdate update; update.credential = first->credential; update.credential->secret = "fixture-health-replaced"; AWAIT(second, accounts::updateClaudeCodeAccount(f.store, update));
  recording = providerHealth::recordClaudeCodeProviderHealth("auth_invalid", 401, rt::dateNow, "req_late", first); co_await recording.wait(); AWAIT(replaced, providerHealth::getClaudeCodeProviderHealth(rt::dateNow, second)); EXPECT_EQ(replaced.state, "unknown");
  AWAIT(cleared, quota::clearClaudeCodeQuotaGuards(f.store, second)); (void)cleared;
  recording = providerHealth::recordClaudeCodeProviderHealth("upstream_error", 503, rt::dateNow, "req_late_clear", second); co_await recording.wait(); AWAIT(current, f.binding()); EXPECT(current);
  AWAIT(after, providerHealth::getClaudeCodeProviderHealth(rt::dateNow, current)); EXPECT_EQ(after.state, "unknown");
}
ASYNC_TEST("a rejected history writer cannot turn a committed quota outcome into failure") {
  AccountFixture f; AWAIT(account, f.binding()); EXPECT(account); const rt::console::Capture warnings({rt::console::Method::warn});
  ObservationRecorderScope recording([](std::shared_ptr<rt::Kv>, quota::ClaudeCodeAcceptedObservation) -> rt::Task<void> { throw rt::JsError("private failure details must not be logged"); co_return; });
  auto input = outcome(*account); input.served = true; AWAIT(recorded, quota::recordClaudeCodeQuotaOutcome(f.store, input)); EXPECT(recorded);
  AWAIT(state, quota::loadClaudeCodeQuotaState(f.store, account)); EXPECT(state); EXPECT_EQ(state->windows.get("5h")->utilization, std::optional<double>(0.25)); EXPECT_EQ(warnings.calls().size(), 1u); EXPECT(warnings.calls().front().text.find("private failure details") == std::string::npos);
}
ASYNC_TEST("new duplicate credential/workspace entries cannot impersonate separate capacity") {
  AccountFixture f; AWAIT(account, f.binding()); EXPECT(account); accounts::AccountUpdate update; update.createNew = true; update.credential = account->credential;
  bool rejected = false; try { AWAIT(result, accounts::updateClaudeCodeAccount(f.store, update)); (void)result; } catch (const rt::JsError& error) { rejected = error.message.find("already configured") != std::string::npos; } EXPECT(rejected);
  AWAIT(store, accounts::loadClaudeCodeAccountStore(f.store)); EXPECT_EQ(store.accounts.size(), 1u);
}
ASYNC_TEST("authoritative exhaustion switches the global active once and shares one admission lifetime") {
  AccountFixture f; AWAIT(first, f.binding()); EXPECT(first); AWAIT(second, f.second(*first)); std::vector<std::string> seen; int claims = 0, starts = 0;
  f.usage.beforeProviderDispatch = [&](std::string) { ++claims; uos::api_key_policy::ApiKeyProviderDispatch dispatch; dispatch.markTransportStarted = [&] { ++starts; }; dispatch.cancelBeforeTransport = [] { return rt::fulfilledTask(); }; return rt::fulfilledTask(std::optional(dispatch)); };
  const rt::Fetch fetcher([&](rt::RequestInfo, rt::RequestInit init) { const auto bearer = init.headers->get("authorization").value_or("missing"); seen.push_back(bearer); return bearer == "Bearer " + first->credential.secret ? accountExhausted() : accountSuccess(); });
  AWAIT(result, f.call(fetcher)); EXPECT(result); EXPECT_EQ(result->response.status(), 200); EXPECT_EQ(claims, 1); EXPECT_EQ(starts, 1); EXPECT_EQ(seen.size(), 2u);
  AWAIT(active, f.binding()); EXPECT(active); EXPECT_EQ(active->account_id, second.account_id); AWAIT(store, accounts::loadClaudeCodeAccountStore(f.store)); EXPECT_EQ(store.active_reason, "authoritative_unavailability");
  credentials::resetClaudeCodeCredentialCacheForTest(); AWAIT(reloaded, f.binding()); EXPECT(reloaded); EXPECT_EQ(reloaded->account_id, second.account_id);
}
ASYNC_TEST("only fresh strong proof for every selected account permits paid continuation") {
  AccountFixture f; AWAIT(first, f.binding()); EXPECT(first); AWAIT(second, f.second(*first)); (void)second; int calls = 0;
  const rt::Fetch exhausted([&](rt::RequestInfo, rt::RequestInit) { ++calls; return accountExhausted(); }); AWAIT(result, f.call(exhausted)); EXPECT(!result); EXPECT_EQ(calls, 2);
  const rt::Fetch success([&](rt::RequestInfo, rt::RequestInit) { ++calls; return accountSuccess(); }); AWAIT(repeated, f.call(success)); EXPECT(!repeated); EXPECT_EQ(calls, 2);
}
ASYNC_TEST("an exhausted plus transient pool retains the original failure and never claims exhaustion") {
  AccountFixture f; AWAIT(first, f.binding()); EXPECT(first); AWAIT(second, f.second(*first));
  const rt::Fetch fetcher([&](rt::RequestInfo, rt::RequestInit init) { return init.headers->get("authorization") == "Bearer " + first->credential.secret ? accountExhausted()
    : rt::Response::json(json::object{{"type", "error"}, {"error", json::object{{"type", "overloaded_error"}, {"message", "fixture transient, not capacity"}}}}, {.status = 503}); });
  AWAIT(result, f.call(fetcher)); EXPECT(result); EXPECT_EQ(result->response.status(), 503); AWAIT(payload, result->response.json());
  const json::value expected = json::object{{"error", json::object{{"type", "overloaded_error"}, {"message", "fixture transient, not capacity"}, {"code", "overloaded_error"}}}}; EXPECT(rt::deepEqual(payload, expected));
  AWAIT(active, f.binding()); EXPECT(active); EXPECT_EQ(active->account_id, second.account_id); AWAIT(state, quota::loadClaudeCodeQuotaState(f.store, second)); EXPECT(!quota::claudeCodePoolProofForModel(state, second, routeModel));
}
ASYNC_TEST("unknown/transient active state cannot switch to an otherwise available second account") {
  AccountFixture f; AWAIT(first, f.binding()); EXPECT(first); AWAIT(second, f.second(*first)); (void)second; int calls = 0;
  const rt::Fetch busy([&](rt::RequestInfo, rt::RequestInit) { ++calls; return rt::Response::json(json::object{{"error", json::object{{"type", "overloaded_error"}, {"message", "fixture busy"}}}}, {.status = 503}); });
  AWAIT(result, f.call(busy)); EXPECT(result); EXPECT_EQ(result->response.status(), 503); EXPECT_EQ(calls, 1); AWAIT(active, f.binding()); EXPECT(active); EXPECT_EQ(active->account_id, first->account_id);
}
ASYNC_TEST("a transport failure never switches active or authorizes paid continuation") {
  AccountFixture f; AWAIT(first, f.binding()); EXPECT(first); AWAIT(second, f.second(*first)); (void)second;
  const rt::Fetch failed([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::TypeError("fixture disconnected"); });
  AWAIT(result, f.call(failed)); EXPECT(result); EXPECT_EQ(result->response.status(), 502); AWAIT(payload, result->response.json()); EXPECT_EQ(rt::getString(*rt::get(payload, "error"), "code"), std::optional<std::string_view>("claude_code_upstream_unreachable"));
  AWAIT(active, f.binding()); EXPECT(active); EXPECT_EQ(active->account_id, first->account_id);
}
ASYNC_TEST("quota proof is exact-model and expires at its recorded reset boundary") {
  AccountFixture f; AWAIT(first, f.binding()); EXPECT(first); AWAIT(second, f.second(*first)); (void)second; const auto now = rt::dateNow();
  auto input = outcome(*first, observation("1", now)); input.model_id = routeModel; input.exhaustion = quota::ClaudeCodeQuotaExhaustion{"5h", now + 60000}; input.response_status = 429; input.nowMs = now;
  AWAIT(recorded, quota::recordClaudeCodeQuotaOutcome(f.store, input)); EXPECT(recorded); AWAIT(state, quota::loadClaudeCodeQuotaState(f.store, first));
  const auto proof = quota::claudeCodePoolProofForModel(state, *first, routeModel, now); EXPECT(proof); EXPECT_EQ(proof->observed_at_ms, now);
  EXPECT(!quota::claudeCodePoolProofForModel(state, *first, routeModel, now - 1)); EXPECT(!quota::claudeCodePoolProofForModel(state, *first, "claude-different", now));
  EXPECT_EQ(quota::claudeCodePoolProofForModel(state, *first, routeModel, proof->expires_at_ms - 1), proof); EXPECT(!quota::claudeCodePoolProofForModel(state, *first, routeModel, proof->expires_at_ms));
  const rt::Fetch fetcher([](rt::RequestInfo, rt::RequestInit) { return accountSuccess("claude-different"); }); AWAIT(result, f.call(fetcher, "claude-different")); EXPECT(result); EXPECT_EQ(result->response.status(), 200);
  AWAIT(active, f.binding()); EXPECT(active); EXPECT_EQ(active->account_id, first->account_id);
}
ASYNC_TEST("success invalidates current exact-model/credential proof but not a newer rejection") {
  AccountFixture f; AWAIT(account, f.binding()); EXPECT(account); const auto now = rt::dateNow(); auto input = outcome(*account, observation("1")); input.model_id = routeModel; input.auth_invalid = true; input.response_observed_at_ms = now;
  AWAIT(recorded, quota::recordClaudeCodeQuotaOutcome(f.store, input)); EXPECT(recorded); AWAIT(state, quota::loadClaudeCodeQuotaState(f.store, account)); EXPECT(quota::claudeCodePoolProofForModel(state, *account, routeModel));
  input = outcome(*account, std::nullopt); input.model_id = routeModel; input.served = true; input.response_observed_at_ms = now - 1000;
  AWAIT(older, quota::recordClaudeCodeQuotaOutcome(f.store, input)); (void)older; AWAIT(olderState, quota::loadClaudeCodeQuotaState(f.store, account)); EXPECT(quota::claudeCodePoolProofForModel(olderState, *account, routeModel));
  input.response_observed_at_ms = now + 1000; AWAIT(newer, quota::recordClaudeCodeQuotaOutcome(f.store, input)); (void)newer; AWAIT(newerState, quota::loadClaudeCodeQuotaState(f.store, account)); EXPECT(!quota::claudeCodePoolProofForModel(newerState, *account, routeModel));
}
ASYNC_TEST("stored principal deny remains authoritative across an otherwise successful configured pool") {
  AccountFixture f; AWAIT(account, f.binding()); EXPECT(account); const std::vector<std::string> deny; AWAIT(stored, access::storeClaudeCodeAccessPolicy(*f.store, deny)); (void)stored; int calls = 0;
  const rt::Fetch fetcher([&](rt::RequestInfo, rt::RequestInit) { ++calls; return accountSuccess(); }); AWAIT(result, f.call(fetcher)); EXPECT(!result); EXPECT_EQ(calls, 0);
  AWAIT(policy, access::loadClaudeCodeAccessPolicy(f.store, accountEnv(true))); EXPECT(policy.allowed_key_ids.empty());
}
ASYNC_TEST("principal revocation during admission cancels before transport and cannot become provider exhaustion") {
  AccountFixture f; AWAIT(account, f.binding()); EXPECT(account); auto transports = std::make_shared<int>(0), cancellations = std::make_shared<int>(0);
  f.usage.beforeProviderDispatch = [store = f.store, transports, cancellations](std::string) {
    return rt::async([store, transports, cancellations]() -> rt::Task<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>> {
      const std::vector<std::string> deny; auto pending = access::storeClaudeCodeAccessPolicy(*store, deny); const auto stored = co_await std::move(pending); (void)stored;
      uos::api_key_policy::ApiKeyProviderDispatch dispatch; dispatch.markTransportStarted = [transports] { ++*transports; }; dispatch.cancelBeforeTransport = [cancellations] { ++*cancellations; return rt::fulfilledTask(); }; co_return dispatch;
    });
  };
  const rt::Fetch fetcher([transports](rt::RequestInfo, rt::RequestInit) { ++*transports; return accountSuccess(); }); AWAIT(result, f.call(fetcher)); EXPECT(result); EXPECT_EQ(result->response.status(), 403);
  AWAIT(payload, result->response.json()); EXPECT_EQ(rt::getString(*rt::get(payload, "error"), "code"), std::optional<std::string_view>("claude_code_request_invalid")); EXPECT_EQ(*transports, 0); EXPECT_EQ(*cancellations, 1);
  AWAIT(state, quota::loadClaudeCodeQuotaState(f.store, account)); EXPECT(!quota::claudeCodePoolProofForModel(state, *account, routeModel)); AWAIT(health, providerHealth::getClaudeCodeProviderHealth(rt::dateNow, account)); EXPECT_EQ(health.state, "unknown");
}
ASYNC_TEST("native reasoning replay cannot cross account credential/workspace cohorts") {
  AccountFixture f; AWAIT(account, f.binding()); EXPECT(account); const auto id = rt::crypto::randomUUID();
  const json::array output{json::object{{"type", "message"}, {"id", "msg_resp_" + id + "_0"}, {"status", "completed"}, {"role", "assistant"}, {"content", json::array{json::object{{"type", "output_text"}, {"text", "fixture reply"}, {"annotations", json::array{}}}}}}};
  uos::provider::anthropic_replay::ReplayPublication publication; publication.scope = "fixture-owner:" + account->account_id + ":" + account->account_cohort_id; publication.provider = "claude_code"; publication.model = routeModel; publication.output = output;
  publication.nativeBlocks = json::array{json::object{{"type", "thinking"}, {"thinking", "fixture private reasoning"}, {"signature", "fixture signature"}}, json::object{{"type", "text"}, {"text", "fixture reply"}}}; EXPECT(uos::provider::anthropic_replay::rememberAnthropicReplay(publication));
  json::array input{json::object{{"role", "user"}, {"content", "fixture first"}}, output.front(), json::object{{"role", "user"}, {"content", "fixture next"}}};
  json::object body{{"model", routeModel}, {"stream", false}, {"input", input}}; int transports = 0;
  route::SubscriptionOptions options; options.model = routeModel; options.env = accountEnv(true); options.usageContext = &f.usage; options.fetcher = rt::Fetch([&](rt::RequestInfo, rt::RequestInit) { ++transports; return accountSuccess(); });
  AWAIT(same, route::attemptClaudeCodeSubscription(body, options)); EXPECT(same); EXPECT_EQ(same->response.status(), 200); EXPECT_EQ(transports, 1);
  accounts::AccountUpdate update; update.credential = account->credential; update.credential->secret = "fixture-replay-replacement"; AWAIT(replaced, accounts::updateClaudeCodeAccount(f.store, update)); (void)replaced;
  AWAIT(changed, route::attemptClaudeCodeSubscription(body, options)); EXPECT(changed); EXPECT_EQ(changed->response.status(), 400); AWAIT(payload, changed->response.json()); EXPECT_EQ(rt::getString(*rt::get(payload, "error"), "code"), std::optional<std::string_view>("claude_code_request_invalid")); EXPECT_EQ(transports, 1);
}
ASYNC_TEST("account discovery reads only the state needed for pristine, configured and tombstoned rosters") {
  AccountFixture f; const auto counting = std::make_shared<uos::tests::helpers::counting_kv::CountingKv>();
  AWAIT(empty, accounts::loadClaudeCodeAccountStore(counting, noEnvironment())); EXPECT(empty.accounts.empty()); EXPECT_EQ(counting->commands.size(), 2u);
  EXPECT_EQ(counting->commands[0].command, "get"); EXPECT_EQ(counting->commands[0].keys, std::vector<rt::KvKey>{accounts::CLAUDE_CODE_ACCOUNTS_KEY});
  EXPECT_EQ(counting->commands[1].command, "get"); EXPECT_EQ(counting->commands[1].keys, std::vector<rt::KvKey>{credentials::CLAUDE_CODE_AUTH_KV_KEY});
  counting->clearMeasurements(); AWAIT(imported, accounts::loadClaudeCodeAccountStore(counting, accountEnv())); EXPECT_EQ(imported.accounts.size(), 1u); EXPECT_EQ(counting->commands.size(), 4u);
  EXPECT_EQ(counting->commands[0].keys, std::vector<rt::KvKey>{accounts::CLAUDE_CODE_ACCOUNTS_KEY}); EXPECT_EQ(counting->commands[1].keys, std::vector<rt::KvKey>{credentials::CLAUDE_CODE_AUTH_KV_KEY}); EXPECT_EQ(counting->commands[2].keys, std::vector<rt::KvKey>{credentials::CLAUDE_CODE_WORKSPACE_KV_KEY});
  EXPECT_EQ(counting->commands[3].command, "atomic.commit"); const std::vector<rt::KvKey> commitKeys{accounts::CLAUDE_CODE_ACCOUNTS_KEY, accounts::CLAUDE_CODE_ACCOUNTS_KEY}; EXPECT_EQ(counting->commands[3].keys, commitKeys);
  counting->clearMeasurements(); AWAIT(configured, accounts::loadClaudeCodeAccountStore(counting, accountEnv())); EXPECT_EQ(configured.accounts.size(), 1u); EXPECT_EQ(counting->commands.size(), 1u); EXPECT_EQ(counting->commands[0].keys, std::vector<rt::KvKey>{accounts::CLAUDE_CODE_ACCOUNTS_KEY});
  AWAIT(removed, accounts::removeClaudeCodeAccount(counting, imported.accounts.front().account_id)); EXPECT(removed); counting->clearMeasurements(); AWAIT(tombstone, accounts::loadClaudeCodeAccountStore(counting, accountEnv())); EXPECT(tombstone.accounts.empty()); EXPECT_EQ(counting->commands.size(), 1u); EXPECT_EQ(counting->commands[0].keys, std::vector<rt::KvKey>{accounts::CLAUDE_CODE_ACCOUNTS_KEY});
}
