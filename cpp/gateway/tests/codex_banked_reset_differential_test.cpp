// Differential cases for src/codex/{reset-settings,banked-reset,banked-reset-claim,banked-reset-submission,banked-reset-pool}.ts.
// Links: tests/codex_banked_reset_differential_test.cpp tests/helpers/codex_banked_reset_harness.cpp src/codex/reset_settings.cpp
//        src/codex/banked_reset.cpp src/codex/banked_reset_claim.cpp src/codex/banked_reset_submission.cpp src/codex/banked_reset_pool.cpp
//        src/codex/banked_reset_provider.cpp src/codex/auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp
//        src/codex/external_auth.cpp src/codex/capacity_routing.cpp src/codex/rate_limit_429.cpp src/codex/routing_mutations.cpp
//        src/codex/routing_state.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp
//        src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp
//        src/bounded_response_body.cpp src/config.cpp src/kv.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
//        src/provider/health.cpp src/release.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
//        src/runtime/http_upgrade.cpp src/runtime/http.cpp src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
//        src/utils.cpp src/types.cpp
//
// Each scenario runs, step for step, what a bun generator ran through the TS harness
// (tests/helpers/codex-banked-reset-harness.ts; the generator script was scratch and is not kept: to
// re-record, replay these cases in TS with the same names, the same `kv.beforeGet` read log and the same
// observation shape, and print them as one JSON array into codex_banked_reset_differential_vectors.inc).
// The C++ case runs the same steps through the harness port and compares everything the
// TS observed: the outcomes, every telemetry event and metric with its fields, the KV rows after the
// run, the provider calls, the order of KV reads (the harness's beforeGet numbering, which pins the
// interleaving of concurrent reads), the check keys of every atomic commit and the commit count. Pool
// outcomes are compared without their in-memory candidate and provider objects (slot and account id
// instead). Date.now is pinned to 1700000000000 (the migration marker) and the verification-latency
// metric value is masked. Credential isolation as in banked_reset_branch_coverage_test.cpp.
#include "tests/helpers/codex_banked_reset_harness.hpp"

#include "uos/codex/auth.hpp"
#include "uos/codex/banked_reset.hpp"
#include "uos/codex/banked_reset_pool.hpp"
#include "uos/codex/banked_reset_submission.hpp"
#include "uos/codex/reset_settings.hpp"
#include "uos/config.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace harness = uos::tests::helpers::codex_banked_reset_harness;
using uos::codex::banked_reset::CodexBankedResetCandidate;
using uos::codex::banked_reset::CodexBankedResetConfig;
using uos::codex::banked_reset::CodexBankedResetDependencies;
using uos::codex::banked_reset::CodexBankedResetOutcome;
using uos::codex::banked_reset::CodexBankedResetPoolOutcome;
using uos::codex::banked_reset::CodexBankedResetTelemetry;
using uos::codex::banked_reset_provider::ResetInventory;
using uos::codex::banked_reset_provider::ResetInventoryCredit;
using harness::FakeCodexUsageResetProvider;
using harness::MemoryKv;
using harness::TestClock;

#define AWAIT(name, expression)   \
  auto name##Task = (expression); \
  auto name = co_await std::move(name##Task)
#define AWAIT_DONE(name, expression) \
  auto name##Task = (expression);    \
  co_await std::move(name##Task)

namespace {
  const char* const VECTORS =
#include "codex_banked_reset_differential_vectors.inc"
    ;

  const json::value& scenarioVector(const std::string& name) {
    static const json::value parsed = rt::jsonParse(VECTORS);
    for (const json::value& scenario : parsed.as_array()) {
      if (rt::getString(scenario, "name") == name) return scenario;
    }
    throw rt::JsError("no vector named " + name);
  }

  struct IsolatedEnvironment {
    rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    std::string savedAuthJson = uos::config::config.codexAuthJsonB64;
    std::function<double()> savedDateNow = rt::globalDateNow();
    IsolatedEnvironment() {
      uos::config::config.codexAuthJsonB64.clear();
      rt::globalDateNow() = [] { return 1'700'000'000'000.0; };
    }
    ~IsolatedEnvironment() {
      uos::config::config.codexAuthJsonB64 = savedAuthJson;
      rt::globalDateNow() = savedDateNow;
    }
    IsolatedEnvironment(const IsolatedEnvironment&) = delete;
    IsolatedEnvironment& operator=(const IsolatedEnvironment&) = delete;
  };

  json::value textOrNull(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }

  json::value outcomeJson(const CodexBankedResetOutcome& outcome) {
    json::object result;
    result["kind"] = outcome.kind;
    result["reason"] = outcome.reason;
    result["accountIdHash"] = textOrNull(outcome.accountIdHash);
    result["quotaGeneration"] = textOrNull(outcome.quotaGeneration);
    result["idempotencyKeyHash"] = textOrNull(outcome.idempotencyKeyHash);
    result["record"] = outcome.record ? json::value_from(*outcome.record) : json::value(nullptr);
    return result;
  }

  json::value poolJson(const CodexBankedResetPoolOutcome& outcome) {
    json::object result;
    result["kind"] = outcome.kind;
    result["reason"] = outcome.reason;
    result["selectedSlot"] = outcome.selected ? rt::makeNumber(outcome.selected->slot) : json::value(nullptr);
    result["selectedAccount"] = outcome.selected ? json::value(outcome.selected->candidate.accountId) : json::value(nullptr);
    result["reset"] = outcome.reset ? outcomeJson(*outcome.reset) : json::value(nullptr);
    return result;
  }

  struct Env {
    std::shared_ptr<MemoryKv> kv = std::make_shared<MemoryKv>();
    std::shared_ptr<TestClock> clock = std::make_shared<TestClock>();
    std::shared_ptr<FakeCodexUsageResetProvider> provider;
    std::shared_ptr<json::array> log = std::make_shared<json::array>();
    std::shared_ptr<json::array> gets = std::make_shared<json::array>();
  };

  Env newEnv(json::value contract = harness::provenContract()) {
    Env env;
    env.provider = std::make_shared<FakeCodexUsageResetProvider>(std::move(contract));
    std::shared_ptr<json::array> gets = env.gets;
    env.kv->beforeGet = [gets](rt::KvKey key, double getNumber) -> std::optional<rt::Promise<void>> {
      json::array read;
      read.push_back(rt::makeNumber(getNumber));
      read.push_back(json::string(harness::encodeKey(key)));
      gets->push_back(std::move(read));
      return std::nullopt;
    };
    return env;
  }

  CodexBankedResetTelemetry telemetryOf(const std::shared_ptr<json::array>& log) {
    CodexBankedResetTelemetry telemetry;
    telemetry.event = [log](std::string event, json::object fields) {
      json::object entry;
      entry["event"] = event;
      entry["fields"] = std::move(fields);
      log->push_back(std::move(entry));
    };
    telemetry.metric = [log](std::string metricName, double value, json::object fields) {
      json::object entry;
      entry["metric"] = metricName;
      entry["value"] = metricName == "codex_reset_verification_latency_ms" ? json::value("<latency>") : rt::makeNumber(value);
      entry["fields"] = std::move(fields);
      log->push_back(std::move(entry));
    };
    return telemetry;
  }

  CodexBankedResetDependencies depsOf(const Env& env, CodexBankedResetConfig configured = harness::config(),
                                      std::function<CodexBankedResetConfig()> reloadConfig = {},
                                      std::shared_ptr<uos::codex::banked_reset_provider::CodexUsageResetProvider> provider = nullptr) {
    return harness::dependencies(env.kv, provider ? provider : env.provider, env.clock, std::move(configured), telemetryOf(env.log), std::move(reloadConfig));
  }

  json::value callsJson(const std::vector<harness::FakeProviderCall>& calls) {
    json::array result;
    for (const harness::FakeProviderCall& call : calls) {
      json::object entry;
      entry["method"] = call.method;
      entry["accountId"] = call.accountId;
      entry["quotaGeneration"] = call.quotaGeneration;
      entry["idempotencyKey"] = textOrNull(call.idempotencyKey);
      entry["commitCount"] = rt::makeNumber(call.commitCount);
      entry["providerReceiptId"] = textOrNull(call.providerReceiptId);
      entry["timeoutStage"] = textOrNull(call.timeoutStage);
      result.push_back(std::move(entry));
    }
    return result;
  }

  json::object observe(const std::string& name, const Env& env, json::array outcomes) {
    json::array rows;
    for (const auto& [encoded, entry] : env.kv->entries) {
      json::object row;
      row["key"] = harness::encodeKey(entry.key);
      row["value"] = entry.value.toJson();
      row["versionstamp"] = entry.versionstamp;
      rows.push_back(std::move(row));
    }
    json::array checks;
    for (const std::vector<rt::KvKey>& batch : env.kv->atomicCheckBatches) {
      json::array keys;
      for (const rt::KvKey& key : batch) keys.push_back(json::string(harness::encodeKey(key)));
      checks.push_back(std::move(keys));
    }
    json::object observation;
    observation["name"] = name;
    observation["outcomes"] = std::move(outcomes);
    observation["log"] = *env.log;
    observation["kv"] = std::move(rows);
    observation["calls"] = callsJson(env.provider->calls);
    observation["gets"] = *env.gets;
    observation["checks"] = std::move(checks);
    observation["commits"] = rt::makeNumber(env.kv->atomicCommitCount);
    return observation;
  }

  json::value contract(std::string_view key, json::value value) {
    json::object result = harness::provenContract();
    result[key] = std::move(value);
    return result;
  }

  json::value resultOf(std::string_view kind, std::string_view member, json::value value) {
    json::object result;
    result["kind"] = json::string(kind);
    result[member] = std::move(value);
    return result;
  }

  rt::KvValue enabledValue(bool enabled) {
    json::object value;
    value["enabled"] = enabled;
    return rt::KvValue(value);
  }

  // Compares one scenario's observation with the bun vector of the same name.
  void expectVector(const json::object& actual) {
    const std::string name(std::string_view(actual.at("name").as_string()));
    const json::value& expected = scenarioVector(name);
    if (rt::deepEqual(json::value(actual), expected)) return;
    for (const auto& member : expected.as_object()) {
      const json::value* mine = rt::get(actual, member.key());
      if (mine != nullptr && rt::deepEqual(*mine, member.value())) continue;
      rt::test::report(__FILE__, __LINE__,
                       name + "." + std::string(member.key()) + " differs\n      actual:   " + (mine ? rt::jsonStringify(*mine) : std::string("undefined")) +
                         "\n      expected: " + rt::jsonStringify(member.value()),
                       false);
    }
    if (actual.size() != expected.as_object().size()) rt::test::report(__FILE__, __LINE__, name + ": member count differs", false);
  }

  rt::Task<CodexBankedResetOutcome> attempt(CodexBankedResetCandidate input, CodexBankedResetDependencies deps) {
    rt::Promise<CodexBankedResetOutcome> attempting(harness::attemptCodexBankedReset(std::move(input), std::move(deps)));
    CodexBankedResetOutcome outcome = co_await attempting.wait();
    co_return outcome;
  }

  rt::Task<CodexBankedResetOutcome> reconcile(CodexBankedResetCandidate input, CodexBankedResetDependencies deps) {
    rt::Promise<CodexBankedResetOutcome> reconciling(harness::reconcileCodexBankedReset(std::move(input), std::move(deps)));
    CodexBankedResetOutcome outcome = co_await reconciling.wait();
    co_return outcome;
  }

  rt::Task<CodexBankedResetPoolOutcome> evaluate(std::vector<uos::codex::banked_reset::CodexBankedResetPoolCandidate> pool, CodexBankedResetDependencies deps) {
    rt::Promise<CodexBankedResetPoolOutcome> evaluating(harness::evaluateCodexBankedResetPool(std::move(pool), std::move(deps)));
    CodexBankedResetPoolOutcome outcome = co_await evaluating.wait();
    co_return outcome;
  }

  // One live attempt for the default candidate in a fresh environment.
  rt::Task<json::object> singleAttempt(std::string name, Env env, CodexBankedResetCandidate input, CodexBankedResetDependencies deps) {
    AWAIT_DONE(seeded, harness::seedFences(env.kv, input));
    AWAIT(outcome, attempt(input, deps));
    json::array outcomes;
    outcomes.push_back(outcomeJson(outcome));
    co_return observe(name, env, std::move(outcomes));
  }
}

ASYNC_TEST("live_happy_path_then_duplicate") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(env.kv, input));
  AWAIT(first, attempt(input, depsOf(env)));
  AWAIT(second, attempt(input, depsOf(env)));
  json::array outcomes;
  outcomes.push_back(outcomeJson(first));
  outcomes.push_back(outcomeJson(second));
  expectVector(observe("live_happy_path_then_duplicate", env, std::move(outcomes)));
}

ASYNC_TEST("terminal_completed") {
  IsolatedEnvironment isolation;
  const Env env = newEnv(contract("redeemOutcomeIsFinal", true));
  AWAIT(observed, singleAttempt("terminal_completed", env, harness::candidate(), depsOf(env)));
  expectVector(observed);
}

ASYNC_TEST("terminal_already_redeemed") {
  IsolatedEnvironment isolation;
  const Env env = newEnv(contract("redeemOutcomeIsFinal", true));
  env.provider->redeemResult = resultOf("already_redeemed", "providerReceiptId", json::string("fixture-replay"));
  AWAIT(observed, singleAttempt("terminal_already_redeemed", env, harness::candidate(), depsOf(env)));
  expectVector(observed);
}

ASYNC_TEST("accepted_unapplied_then_reconciled") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  env.provider->redeemResult = resultOf("accepted", "providerReceiptId", json::string("receipt-accepted"));
  env.provider->verifyResult = false;
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(env.kv, input));
  AWAIT(first, attempt(input, depsOf(env)));
  env.clock->advance(30'001);
  env.provider->verifyResult = true;
  AWAIT(second, reconcile(input, depsOf(env)));
  json::array outcomes;
  outcomes.push_back(outcomeJson(first));
  outcomes.push_back(outcomeJson(second));
  expectVector(observe("accepted_unapplied_then_reconciled", env, std::move(outcomes)));
}

ASYNC_TEST("provider_rejected") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  env.provider->redeemResult = resultOf("rejected", "reason", json::string("no_credit"));
  AWAIT(observed, singleAttempt("provider_rejected", env, harness::candidate(), depsOf(env)));
  expectVector(observed);
}

ASYNC_TEST("unknown_with_receipt") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  env.provider->redeemResult = resultOf("unknown", "providerReceiptId", json::string("receipt-unknown"));
  AWAIT(observed, singleAttempt("unknown_with_receipt", env, harness::candidate(), depsOf(env)));
  expectVector(observed);
}

ASYNC_TEST("unsafe_receipts") {
  IsolatedEnvironment isolation;
  const Env env = newEnv(contract("receiptIdsSafeToPersistAndLog", false));
  env.provider->redeemResult = resultOf("accepted", "providerReceiptId", json::string("unapproved-receipt"));
  AWAIT(observed, singleAttempt("unsafe_receipts", env, harness::candidate(), depsOf(env)));
  expectVector(observed);
}

ASYNC_TEST("inventory_empty") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  env.provider->inventory = ResetInventory{.availableCount = 0, .observedAtMs = 1'700'000'000'000, .credits = {}};
  AWAIT(observed, singleAttempt("inventory_empty", env, harness::candidate(), depsOf(env)));
  expectVector(observed);
}

ASYNC_TEST("inventory_stale") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  ResetInventory stale = harness::inventory("stale-credit", std::nullopt);
  stale.observedAtMs = 1'699'999'000'000;
  env.provider->inventory = stale;
  AWAIT(observed, singleAttempt("inventory_stale", env, harness::candidate(), depsOf(env)));
  expectVector(observed);
}

ASYNC_TEST("inventory_wrong_type") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  env.provider->inventory = ResetInventory{
    .availableCount = 1,
    .observedAtMs = 1'700'000'000'000,
    .credits = {ResetInventoryCredit{.id = "other-credit", .status = "available", .resetType = "other", .expiresAtMs = std::nullopt}},
  };
  AWAIT(observed, singleAttempt("inventory_wrong_type", env, harness::candidate(), depsOf(env)));
  expectVector(observed);
}

ASYNC_TEST("inventory_failure") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  env.provider->inventoryFailure = rt::makeReason(rt::JsError("inventory unavailable"));
  AWAIT(observed, singleAttempt("inventory_failure", env, harness::candidate(), depsOf(env)));
  expectVector(observed);
}

ASYNC_TEST("shadow_single") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  AWAIT(observed, singleAttempt("shadow_single", env, harness::candidate(), depsOf(env, harness::config({.mode = "shadow"}))));
  expectVector(observed);
}

ASYNC_TEST("usage_disabled") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(env.kv, input));
  AWAIT(disabled, env.kv->set(harness::codexResetUsageKey(harness::testHashText(input.accountId)), enabledValue(false)));
  (void)disabled;
  AWAIT(outcome, attempt(input, depsOf(env)));
  json::array outcomes;
  outcomes.push_back(outcomeJson(outcome));
  expectVector(observe("usage_disabled", env, std::move(outcomes)));
}

ASYNC_TEST("manual_admin_bypasses_usage") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  const std::string accountIdHash = harness::testHashText("test-account-a");
  CodexBankedResetCandidate input = harness::candidate();
  input.authorization = CodexBankedResetCandidate::Authorization{.accountIdHash = accountIdHash};
  AWAIT_DONE(seeded, harness::seedFences(env.kv, input));
  AWAIT(disabled, env.kv->set(harness::codexResetUsageKey(accountIdHash), enabledValue(false)));
  (void)disabled;
  AWAIT(outcome, attempt(input, depsOf(env)));
  json::array outcomes;
  outcomes.push_back(outcomeJson(outcome));
  expectVector(observe("manual_admin_bypasses_usage", env, std::move(outcomes)));
}

ASYNC_TEST("manual_admin_day_limit_two") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  CodexBankedResetCandidate input = harness::candidate();
  input.authorization = CodexBankedResetCandidate::Authorization{.accountIdHash = harness::testHashText("test-account-a")};
  AWAIT(observed, singleAttempt("manual_admin_day_limit_two", env, input, depsOf(env, harness::config({.maxPerAccountPerDay = 2}))));
  expectVector(observed);
}

ASYNC_TEST("day_cap_second_generation") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  const CodexBankedResetCandidate first = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(env.kv, first));
  AWAIT(one, attempt(first, depsOf(env)));
  const CodexBankedResetCandidate second = harness::candidate({.quotaResetAtMs = 1'700'000'090'000});
  AWAIT(two, attempt(second, depsOf(env)));
  json::array outcomes;
  outcomes.push_back(outcomeJson(one));
  outcomes.push_back(outcomeJson(two));
  expectVector(observe("day_cap_second_generation", env, std::move(outcomes)));
}

ASYNC_TEST("legacy_opt_out_migration") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(env.kv, input));
  AWAIT_DONE(legacy, env.kv->set(uos::codex::reset_settings::CODEX_BANKED_RESET_USAGE_LEGACY_KEY, rt::KvValue(false)));
  json::object account;
  account["access_token"] = "access-a";
  account["refresh_token"] = "refresh-a";
  account["account_id"] = input.accountId;
  account["updated_at_ms"] = rt::makeNumber(1'700'000'000'000.0);
  json::array accounts;
  accounts.push_back(std::move(account));
  json::object pool;
  pool["accounts"] = std::move(accounts);
  pool["updated_at_ms"] = rt::makeNumber(1'700'000'000'001.0);
  AWAIT_DONE(pooled, env.kv->set(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY, rt::KvValue(pool)));
  AWAIT(outcome, attempt(input, depsOf(env)));
  json::array outcomes;
  outcomes.push_back(outcomeJson(outcome));
  expectVector(observe("legacy_opt_out_migration", env, std::move(outcomes)));
}

ASYNC_TEST("legacy_opt_out_pending_seed") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(env.kv, input));
  AWAIT_DONE(legacy, env.kv->set(uos::codex::reset_settings::CODEX_BANKED_RESET_USAGE_LEGACY_KEY, rt::KvValue("malformed")));
  AWAIT(outcome, attempt(input, depsOf(env)));
  json::array outcomes;
  outcomes.push_back(outcomeJson(outcome));
  expectVector(observe("legacy_opt_out_pending_seed", env, std::move(outcomes)));
}

ASYNC_TEST("pool_shadow_twice") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  const CodexBankedResetCandidate first = harness::candidate();
  const CodexBankedResetCandidate second = harness::candidate({.accountId = "test-account-b"});
  AWAIT_DONE(seededFirst, harness::seedFences(env.kv, first));
  AWAIT_DONE(seededSecond, harness::seedFences(env.kv, second));
  auto secondProvider = std::make_shared<FakeCodexUsageResetProvider>();
  env.provider->inventory = harness::inventory("credit-a", 1'700'000'040'000);
  secondProvider->inventory = harness::inventory("credit-b", 1'700'000'020'000);
  const CodexBankedResetDependencies deps = depsOf(env, harness::config({.mode = "shadow"}));
  AWAIT(one, evaluate(harness::fullPool(first, env.provider, second, secondProvider), deps));
  AWAIT(two, evaluate(harness::fullPool(first, env.provider, second, secondProvider), deps));
  json::array outcomes;
  outcomes.push_back(poolJson(one));
  outcomes.push_back(poolJson(two));
  json::object observed = observe("pool_shadow_twice", env, std::move(outcomes));
  observed["secondCalls"] = callsJson(secondProvider->calls);
  expectVector(observed);
}

ASYNC_TEST("pool_live") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  const CodexBankedResetCandidate first = harness::candidate();
  const CodexBankedResetCandidate second = harness::candidate({.accountId = "test-account-b"});
  AWAIT_DONE(seededFirst, harness::seedFences(env.kv, first));
  AWAIT_DONE(seededSecond, harness::seedFences(env.kv, second));
  auto secondProvider = std::make_shared<FakeCodexUsageResetProvider>();
  env.provider->inventory = ResetInventory{.availableCount = 0, .observedAtMs = 1'700'000'000'000, .credits = {}};
  secondProvider->inventory = harness::inventory("credit-b", 1'700'000'050'000);
  AWAIT(outcome, evaluate(harness::fullPool(first, env.provider, second, secondProvider), depsOf(env)));
  json::array outcomes;
  outcomes.push_back(poolJson(outcome));
  json::object observed = observe("pool_live", env, std::move(outcomes));
  observed["secondCalls"] = callsJson(secondProvider->calls);
  expectVector(observed);
}

ASYNC_TEST("pool_live_day_limit_two") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  const CodexBankedResetCandidate first = harness::candidate();
  const CodexBankedResetCandidate second = harness::candidate({.accountId = "test-account-b"});
  AWAIT_DONE(seededFirst, harness::seedFences(env.kv, first));
  AWAIT_DONE(seededSecond, harness::seedFences(env.kv, second));
  AWAIT(outcome, evaluate(harness::fullPool(first, env.provider, second, env.provider), depsOf(env, harness::config({.maxPerAccountPerDay = 2}))));
  json::array outcomes;
  outcomes.push_back(poolJson(outcome));
  expectVector(observe("pool_live_day_limit_two", env, std::move(outcomes)));
}

ASYNC_TEST("pool_usage_disabled_one") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  const CodexBankedResetCandidate first = harness::candidate();
  const CodexBankedResetCandidate second = harness::candidate({.accountId = "test-account-b"});
  AWAIT_DONE(seededFirst, harness::seedFences(env.kv, first));
  AWAIT_DONE(seededSecond, harness::seedFences(env.kv, second));
  AWAIT_DONE(disabled, env.kv->set(harness::codexResetUsageKey(harness::testHashText(first.accountId)), enabledValue(false)));
  env.provider->inventory = harness::inventory("credit-b", 1'700'000'050'000);
  AWAIT(outcome, evaluate(harness::fullPool(first, env.provider, second, env.provider), depsOf(env, harness::config({.mode = "shadow"}))));
  json::array outcomes;
  outcomes.push_back(poolJson(outcome));
  expectVector(observe("pool_usage_disabled_one", env, std::move(outcomes)));
}

ASYNC_TEST("telemetry_sink_throws") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  CodexBankedResetDependencies deps = depsOf(env);
  CodexBankedResetTelemetry throwing;
  throwing.event = [](std::string, json::object) { throw rt::JsError("sink unavailable"); };
  throwing.metric = [](std::string, double, json::object) { throw rt::JsError("sink unavailable"); };
  deps.telemetry = throwing;
  AWAIT(observed, singleAttempt("telemetry_sink_throws", env, harness::candidate(), deps));
  expectVector(observed);
}

ASYNC_TEST("reconcile_negative_lookup_verified") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  env.provider->redeemResult = resultOf("unknown", "providerReceiptId", nullptr);
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(env.kv, input));
  AWAIT(first, attempt(input, depsOf(env)));
  env.clock->advance(30'001);
  env.provider->lookupResult = resultOf("rejected", "reason", json::string("not yet visible"));
  AWAIT(second, reconcile(input, depsOf(env)));
  json::array outcomes;
  outcomes.push_back(outcomeJson(first));
  outcomes.push_back(outcomeJson(second));
  expectVector(observe("reconcile_negative_lookup_verified", env, std::move(outcomes)));
}

ASYNC_TEST("concurrent_claims") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(env.kv, input));
  const CodexBankedResetDependencies deps = depsOf(env);
  rt::Promise<CodexBankedResetOutcome> firstAttempt(harness::attemptCodexBankedReset(input, deps));
  rt::Promise<CodexBankedResetOutcome> secondAttempt(harness::attemptCodexBankedReset(input, deps));
  auto both = rt::all(std::move(firstAttempt), std::move(secondAttempt));
  const std::tuple<CodexBankedResetOutcome, CodexBankedResetOutcome> outcomes = co_await both.wait();
  json::array observedOutcomes;
  observedOutcomes.push_back(outcomeJson(std::get<0>(outcomes)));
  observedOutcomes.push_back(outcomeJson(std::get<1>(outcomes)));
  expectVector(observe("concurrent_claims", env, std::move(observedOutcomes)));
}

ASYNC_TEST("kill_switch_during_submission") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  const CodexBankedResetCandidate input = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(env.kv, input));
  auto reads = std::make_shared<double>(0);
  const std::function<CodexBankedResetConfig()> reloadConfig = [reads] {
    *reads += 1;
    return *reads > 3 ? harness::config({.enabled = false}) : harness::config();
  };
  AWAIT(outcome, attempt(input, depsOf(env, harness::config(), reloadConfig)));
  json::array outcomes;
  outcomes.push_back(outcomeJson(outcome));
  json::object observed = observe("kill_switch_during_submission", env, std::move(outcomes));
  observed["reloads"] = rt::makeNumber(*reads);
  expectVector(observed);
}

ASYNC_TEST("selected_credit_given") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  CodexBankedResetCandidate input = harness::candidate();
  input.selectedCredit = ResetInventoryCredit{.id = "picked", .status = "available", .resetType = "codex_rate_limits", .expiresAtMs = 1'700'000'050'000};
  AWAIT(observed, singleAttempt("selected_credit_given", env, input, depsOf(env)));
  expectVector(observed);
}

ASYNC_TEST("gated_concurrent_duplicate") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  auto gate = std::make_shared<harness::Deferred<void>>();
  auto entered = std::make_shared<harness::Deferred<void>>();
  env.provider->redeemGate = gate->promise;
  env.provider->redeemEntered = entered;
  const CodexBankedResetDependencies deps = depsOf(env);
  const CodexBankedResetCandidate reset = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(env.kv, reset));
  rt::Promise<CodexBankedResetOutcome> firstPromise(harness::attemptCodexBankedReset(reset, deps));
  co_await entered->promise.wait();
  AWAIT(duplicate, attempt(harness::candidate({.requestId = "concurrent-request"}), deps));
  gate->resolve();
  const CodexBankedResetOutcome first = co_await firstPromise.wait();
  json::array outcomes;
  outcomes.push_back(outcomeJson(duplicate));
  outcomes.push_back(outcomeJson(first));
  expectVector(observe("gated_concurrent_duplicate", env, std::move(outcomes)));
}

namespace {
  rt::Task<json::object> subscriptionDisabledDuringInventory(std::string name, std::string mode) {
    const Env env = newEnv();
    const CodexBankedResetCandidate reset = harness::candidate();
    AWAIT_DONE(seeded, harness::seedFences(env.kv, reset));
    json::object key;
    key["id"] = "key-a";
    key["revoked_at_ms"] = nullptr;
    key["expires_at_ms"] = -1;
    key["enabled"] = true;
    const rt::KvKey usageKey = harness::codexResetUsageKey(harness::testHashText(reset.accountId));
    AWAIT_DONE(enabled, env.kv->set(usageKey, rt::KvValue(key)));
    auto gate = std::make_shared<harness::Deferred<void>>();
    auto entered = std::make_shared<harness::Deferred<void>>();
    env.provider->inventoryGate = gate->promise;
    env.provider->inventoryEntered = entered;
    std::vector<uos::codex::banked_reset::CodexBankedResetPoolCandidate> pool;
    pool.push_back(uos::codex::banked_reset::CodexBankedResetPoolCandidate{.slot = 0, .candidate = reset, .provider = env.provider});
    rt::Promise<CodexBankedResetPoolOutcome> pending(
      harness::evaluateCodexBankedResetPool(std::move(pool), depsOf(env, harness::config({.mode = mode, .maxPerAccountPerDay = 1}))));
    co_await entered->promise.wait();
    json::object disabledKey = key;
    disabledKey["enabled"] = false;
    AWAIT_DONE(disabled, env.kv->set(usageKey, rt::KvValue(disabledKey)));
    gate->resolve();
    const CodexBankedResetPoolOutcome outcome = co_await pending.wait();
    json::array outcomes;
    outcomes.push_back(poolJson(outcome));
    co_return observe(name, env, std::move(outcomes));
  }
}

ASYNC_TEST("subscription_disabled_during_inventory_shadow") {
  IsolatedEnvironment isolation;
  AWAIT(observed, subscriptionDisabledDuringInventory("subscription_disabled_during_inventory_shadow", "shadow"));
  expectVector(observed);
}

ASYNC_TEST("subscription_disabled_during_inventory_live") {
  IsolatedEnvironment isolation;
  AWAIT(observed, subscriptionDisabledDuringInventory("subscription_disabled_during_inventory_live", "live"));
  expectVector(observed);
}

ASYNC_TEST("post_submission_takeover") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  const CodexBankedResetCandidate reset = harness::candidate();
  AWAIT_DONE(seeded, harness::seedFences(env.kv, reset));
  auto renewalGate = std::make_shared<harness::Deferred<void>>();
  auto renewalEntered = std::make_shared<harness::Deferred<void>>();
  auto fired = std::make_shared<bool>(false);
  MemoryKv* kv = env.kv.get();
  std::shared_ptr<json::array> gets = env.gets;
  env.kv->beforeGet = [kv, gets, fired, renewalGate, renewalEntered](rt::KvKey key, double getNumber) -> std::optional<rt::Promise<void>> {
    json::array read;
    read.push_back(rt::makeNumber(getNumber));
    read.push_back(json::string(harness::encodeKey(key)));
    gets->push_back(std::move(read));
    if (kv->atomicCommitCount != 2 || *fired) return std::nullopt;
    *fired = true;
    renewalEntered->resolve();
    return renewalGate->promise;
  };
  const CodexBankedResetDependencies deps = depsOf(env);
  rt::Promise<CodexBankedResetOutcome> pausedOwner(harness::attemptCodexBankedReset(reset, deps));
  co_await renewalEntered->promise.wait();
  env.clock->advance(30'001);
  env.provider->lookupResult = resultOf("unknown", "providerReceiptId", nullptr);
  env.provider->verifyResult = false;
  AWAIT(takeover, reconcile(harness::candidate({.requestId = "post-submission-takeover"}), deps));
  renewalGate->resolve();
  const CodexBankedResetOutcome staleOutcome = co_await pausedOwner.wait();
  json::array outcomes;
  outcomes.push_back(outcomeJson(takeover));
  outcomes.push_back(outcomeJson(staleOutcome));
  expectVector(observe("post_submission_takeover", env, std::move(outcomes)));
}

ASYNC_TEST("cas_conflict_retry") {
  IsolatedEnvironment isolation;
  const Env env = newEnv();
  env.kv->failCommitNumbers.add(2);
  env.kv->failCommitNumbers.add(3);
  env.kv->failCommitNumbers.add(6);
  AWAIT(observed, singleAttempt("cas_conflict_retry", env, harness::candidate(), depsOf(env)));
  expectVector(observed);
}

TEST("the recorded vectors are complete and the comparison is strict") {
  const json::value parsed = rt::jsonParse(VECTORS);
  EXPECT_EQ(parsed.as_array().size(), std::size_t{32});
  for (const json::value& scenario : parsed.as_array()) EXPECT(rt::get(scenario, "error") == nullptr);
  json::value changed = scenarioVector("concurrent_claims");
  EXPECT(rt::deepEqual(changed, scenarioVector("concurrent_claims")));
  changed.as_object()["commits"] = rt::makeNumber(rt::toNumber(*rt::get(changed, "commits")) + 1);
  EXPECT(!rt::deepEqual(changed, scenarioVector("concurrent_claims")));
  EXPECT(!rt::deepEqual(scenarioVector("terminal_completed"), scenarioVector("terminal_already_redeemed")));
}

// The default sink (console.info) as bun prints it: `{ event, ...fields }` / `{ metric, value, ...fields }`
// stringified in JS property order (an index-like key first, a repeated key overriding in place).
TEST("the default telemetry sink prints one console.info line per event and metric") {
  IsolatedEnvironment isolation;
  rt::console::Capture capture({rt::console::Method::info});
  json::object eligible;
  eligible["request_id"] = nullptr;
  eligible["account_id_hash"] = "hash-a";
  eligible["routing_generation"] = 7;
  eligible["selected"] = true;
  uos::codex::banked_reset::reportCodexBankedResetEvent(std::nullopt, "codex_reset_eligible", eligible);
  json::object rejected;
  rejected["event"] = "overridden";
  rejected["reason"] = "inventory_empty";
  rejected["0"] = "index-key";
  uos::codex::banked_reset::reportCodexBankedResetEvent(std::nullopt, "codex_reset_rejected", rejected);
  json::object total;
  total["request_id"] = "request-1";
  total["quota_generation"] = "v1:q";
  uos::codex::banked_reset::reportCodexBankedResetMetric(std::nullopt, "codex_reset_eligible_total", 1, total);
  json::object latency;
  latency["metric"] = "overridden";
  latency["value"] = "also-overridden";
  latency["fence"] = rt::makeNumber(-0.0);
  uos::codex::banked_reset::reportCodexBankedResetMetric(std::nullopt, "codex_reset_verification_latency_ms", 0.5, latency);
  json::object verified;
  verified["state"] = "verified";
  uos::codex::banked_reset::reportCodexBankedResetEvent(CodexBankedResetTelemetry{}, "codex_reset_verified", verified);

  const std::vector<std::pair<std::string, std::string>> expected{
    {"[ai.ubq.fi] codex_banked_reset", R"({"event":"codex_reset_eligible","request_id":null,"account_id_hash":"hash-a","routing_generation":7,"selected":true})"},
    {"[ai.ubq.fi] codex_banked_reset", R"({"0":"index-key","event":"overridden","reason":"inventory_empty"})"},
    {"[ai.ubq.fi] codex_banked_reset_metric", R"({"metric":"codex_reset_eligible_total","value":1,"request_id":"request-1","quota_generation":"v1:q"})"},
    {"[ai.ubq.fi] codex_banked_reset_metric", R"({"metric":"overridden","value":"also-overridden","fence":0})"},
  };
  ASSERT_EQ(capture.calls().size(), expected.size());
  for (std::size_t index = 0; index < expected.size(); ++index) {
    const rt::console::Call& call = capture.calls()[index];
    ASSERT_EQ(call.args.size(), std::size_t{2});
    EXPECT_EQ(call.args[0].string(), expected[index].first);
    EXPECT_EQ(call.args[1].string(), expected[index].second);
  }
}
