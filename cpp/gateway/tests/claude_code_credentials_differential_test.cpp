// Links: tests/claude_code_credentials_differential_test.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_accounts.cpp src/utils.cpp
// Differential cases for src/provider/claude-code-credentials.ts and src/provider/claude-code-accounts.ts (no direct
// bun test of theirs is runnable in this wave). The vectors (tests/claude_code_credentials_differential_vectors.inc) were
// recorded from the TS modules under bun 1.4.0 with Date.now fixed, crypto.randomUUID a counter, the provider
// credential variables unset and an in-memory Deno KV whose operations complete one macrotask later. Every KV scenario
// below repeats the generator's steps one for one and compares each step's result (or error text) in order.
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/test_async.hpp"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

using namespace uos;
namespace credentials = provider::claude_code_credentials;
namespace accounts = provider::claude_code_accounts;
using provider::claude_code::ClaudeCodeEnvLookup;

namespace {
  const char* const VECTORS =
#include "tests/claude_code_credentials_differential_vectors.inc"
    ;

  constexpr double NOW = 1'700'000'000'000;

  const json::object& vectors() {
    static const json::object parsed = rt::jsonParse(VECTORS).as_object();
    return parsed;
  }
  const json::array& pureCases(std::string_view fn) { return rt::getObject(vectors(), "pure")->at(fn).as_array(); }
  const json::array& scenarioSteps(std::string_view name) { return rt::getObject(vectors(), "scenarios")->at(name).as_array(); }

  void expectJson(const json::value& actual, const json::value& expected, const std::string& label, const char* file, int line) {
    if (!rt::deepEqual(actual, expected))
      rt::test::report(file, line, label + "\n      actual:   " + json::serialize(actual) + "\n      expected: " + json::serialize(expected), false);
  }
#define EXPECT_JSON(actual, expected, label) expectJson((actual), (expected), (label), __FILE__, __LINE__)

  template <class T> json::value plain(const std::optional<T>& value) { return value ? json::value_from(*value) : json::value(nullptr); }
  json::value plain(bool value) { return json::value(value); }
  json::value plain(double value) { return rt::canonicalNumbers(json::value(value)); }
  template <class T> json::value plain(const T& value) { return json::value_from(value); }
  json::value plain(const json::value& value) { return value; }

  json::value ok(json::value value) {
    json::object out;
    out["ok"] = std::move(value);
    return out;
  }
  json::value failure(const rt::Reason& reason) {
    json::object out;
    out["error"] = rt::reasonToString(reason);
    return out;
  }

  // `await capture(() => run())` of the generator.
  template <class T> rt::Task<json::value> capture(rt::Task<T> task) {
    rt::Reason error;
    json::value out;
    try {
      T value = co_await std::move(task);
      out = ok(plain(value));
    } catch (...) {
      error = std::current_exception();
    }
    if (error) out = failure(error);
    co_return out;
  }

  ClaudeCodeEnvLookup envOf(const json::object& map) {
    return [map](const std::string& name) -> std::optional<std::string> {
      const json::value* value = rt::get(map, name);
      if (value == nullptr || !value->is_string()) return std::nullopt;
      return std::string(value->get_string());
    };
  }

  // `crypto.randomUUID` as a counter: 00000000-0000-4000-8000-<counter as 12 hex digits>.
  rt::crypto::RandomSource uuidSource() {
    auto counter = std::make_shared<std::uint64_t>(0);
    return [counter](std::span<std::uint8_t> bytes) {
      *counter += 1;
      for (auto& byte : bytes) byte = 0;
      for (std::size_t index = 0; index < 6 && index < bytes.size(); ++index)
        bytes[bytes.size() - 1 - index] = static_cast<std::uint8_t>((*counter >> (8 * index)) & 0xff);
    };
  }

  // One generator scenario: a fresh store, the clock, the UUID counter and the credential cache reset.
  struct Scenario {
    explicit Scenario(std::string_view name_)
      : name(name_),
        expected(scenarioSteps(name_)),
        env({{"CLAUDE_API_KEY", std::nullopt}, {"CLAUDE_CODE_OAUTH_TOKEN", std::nullopt}, {"CLAUDE_CODE_WORKSPACE_ID", std::nullopt}}),
        random(uuidSource()),
        kv(std::make_shared<rt::MemoryKv>([] { return NOW; })) {
      rt::globalDateNow() = [] { return NOW; };
      credentials::resetClaudeCodeCredentialCacheForTest();
    }
    void step(const json::value& actual, const char* file, int line) {
      const std::string label = name + " step " + std::to_string(index);
      if (index >= expected.size()) rt::test::report(file, line, label + ": more steps than the generator recorded", false);
      else expectJson(actual, expected[index], label, file, line);
      ++index;
    }
    rt::Task<void> finish(const char* file, int line) {
      const rt::KvEntryMaybe<> entry = co_await kv->get(accounts::CLAUDE_CODE_ACCOUNTS_KEY);
      json::object out;
      out["roster"] = entry.value.toJson();
      step(out, file, line);
      if (index != expected.size()) rt::test::report(file, line, name + ": fewer steps than the generator recorded", false);
    }

    std::string name;
    const json::array& expected;
    std::size_t index = 0;
    rt::env::ScopedOverride env;
    rt::crypto::ScopedRandomSource random;
    std::shared_ptr<rt::Kv> kv;
  };
#define STEP(scenario, task)                     \
  do {                                           \
    auto uosStepTask = (task);                   \
    auto uosStepCapture = capture(std::move(uosStepTask)); \
    json::value uosStepResult = co_await std::move(uosStepCapture); \
    (scenario).step(uosStepResult, __FILE__, __LINE__); \
  } while (0)

  json::object parseObject(std::string_view text) { return rt::jsonParse(text).as_object(); }
  rt::KvValue storedValue(std::string_view text) { return rt::KvValue(rt::jsonParse(text)); }

  credentials::ClaudeCodeWorkspaceState workspaceOf(std::string_view text) { return json::value_to<credentials::ClaudeCodeWorkspaceState>(rt::jsonParse(text)); }
  credentials::ClaudeCodeResolvedCredential credentialOf(std::string_view text) {
    return json::value_to<credentials::ClaudeCodeResolvedCredential>(rt::jsonParse(text));
  }

  rt::Task<json::value> rawValue(std::shared_ptr<rt::Kv> kv, rt::KvKey key) {
    const rt::KvEntryMaybe<> entry = co_await kv->get(std::move(key));
    co_return entry.value.toJson();
  }

  rt::Task<json::value> currentEntryValue(std::shared_ptr<rt::Kv> kv, accounts::ClaudeCodeAccountBinding binding) {
    auto reading = accounts::currentClaudeCodeAccountEntry(std::move(kv), std::move(binding));
    const std::optional<rt::KvEntryMaybe<>> entry = co_await std::move(reading);
    if (!entry) co_return json::value(nullptr);
    json::object out;
    out["value"] = entry->value.toJson();
    co_return json::value(std::move(out));
  }

  rt::Task<double> generation(std::shared_ptr<rt::Kv> kv) { co_return credentials::claudeCodeStateGeneration(kv); }

  // `await kv.set(key, <literal>)` of the generator.
  rt::Task<rt::KvCommitResult> setRaw(const std::shared_ptr<rt::Kv>& kv, const rt::KvKey& key, std::string_view text) { return kv->set(key, storedValue(text)); }
#define SEED(kv, key, text)                    \
  do {                                         \
    auto uosSeed = setRaw((kv), (key), (text)); \
    co_await std::move(uosSeed);               \
  } while (0)
}

TEST("pure helpers match the TS") {
  for (const json::value& item : pureCases("classify")) {
    const std::string input(item.at("input").as_string());
    EXPECT_JSON(json::value(credentials::classifyClaudeCodeCredentialKind(input)), item.at("output"), "classify " + input);
  }
  for (const json::value& item : pureCases("hint")) {
    const std::string input(item.at("input").as_string());
    EXPECT_JSON(json::value(credentials::partialClaudeCodeCredentialHint(input)), item.at("output"), "hint " + input);
  }
  for (const json::value& item : pureCases("parseStored"))
    EXPECT_JSON(plain(credentials::parseClaudeCodeStoredCredential(rt::KvValue(item.at("input")))), item.at("output"), "parseStored " + json::serialize(item.at("input")));
  for (const json::value& item : pureCases("parseWorkspace"))
    EXPECT_JSON(plain(credentials::parseClaudeCodeWorkspaceState(rt::KvValue(item.at("input")))), item.at("output"), "parseWorkspace " + json::serialize(item.at("input")));
  for (const json::value& item : pureCases("parseWorkspaceId")) {
    const std::optional<std::string> id = credentials::parseClaudeCodeWorkspaceId(item.at("input"));
    EXPECT_JSON(id ? json::value(*id) : json::value(nullptr), item.at("output"), "parseWorkspaceId " + json::serialize(item.at("input")));
  }
  for (const json::value& item : pureCases("parseStore"))
    EXPECT_JSON(plain(accounts::parseClaudeCodeAccountStore(rt::KvValue(item.at("input")))), item.at("output"), "parseStore " + json::serialize(item.at("input")));
  for (const json::value& item : pureCases("merge")) {
    const json::value& previous = item.at("input").at("previous");
    const json::object& input = item.at("input").at("input").as_object();
    credentials::MergeClaudeCodeWorkspaceStateInput merge{.id = std::string(input.at("id").as_string()), .source = std::string(input.at("source").as_string())};
    if (const auto name = rt::getString(input, "name")) merge.name = *name;
    if (const auto nowMs = rt::getNumber(input, "nowMs")) merge.nowMs = *nowMs;
    if (const auto lastError = rt::getString(input, "last_error")) merge.last_error = *lastError;
    if (const json::value* verified = rt::get(input, "verified")) merge.verified = verified->as_bool();
    const std::optional<credentials::ClaudeCodeWorkspaceState> prior =
      previous.is_null() ? std::nullopt : std::optional(json::value_to<credentials::ClaudeCodeWorkspaceState>(previous));
    const auto realNow = rt::globalDateNow();
    rt::globalDateNow() = [] { return NOW; };
    const credentials::ClaudeCodeWorkspaceState merged = credentials::mergeClaudeCodeWorkspaceState(prior, merge);
    rt::globalDateNow() = realNow;
    EXPECT_JSON(plain(merged), item.at("output"), "merge " + json::serialize(item.at("input")));
  }
  for (const json::value& item : pureCases("envCredential"))
    EXPECT_JSON(plain(credentials::readClaudeCodeEnvCredential(envOf(item.at("input").as_object()))), item.at("output"), "envCredential " + json::serialize(item.at("input")));
  for (const json::value& item : pureCases("workspaceIdEnv")) {
    const std::optional<std::string> id = credentials::readClaudeCodeWorkspaceIdEnv(envOf(item.at("input").as_object()));
    EXPECT_JSON(id ? json::value(*id) : json::value(nullptr), item.at("output"), "workspaceIdEnv " + json::serialize(item.at("input")));
  }
}

ASYNC_TEST("legacy bootstrap") {
  Scenario s("legacy bootstrap");
  SEED(s.kv, credentials::CLAUDE_CODE_AUTH_KV_KEY,
                  R"({"secret":"sk-ant-oat-fixture-singleton","kind":"oauth","hint":"configured","label":"not verified identity","updated_at_ms":1234})");
  SEED(s.kv, credentials::CLAUDE_CODE_WORKSPACE_KV_KEY,
                  R"({"id":"wrkspc_singleton","name":null,"source":"kv","discovered_at_ms":null,"verified_at_ms":null,"last_error":null})");
  const ClaudeCodeEnvLookup env = envOf(parseObject(R"({"CLAUDE_API_KEY":"sk-ant-api-ignored"})"));
  STEP(s, accounts::loadClaudeCodeAccountStore(s.kv, env));
  STEP(s, accounts::loadClaudeCodeAccountStore(s.kv, env));
  STEP(s, credentials::resolveClaudeCodeCredential(s.kv, env));
  STEP(s, credentials::loadClaudeCodeStoredCredential(s.kv));
  STEP(s, credentials::resolveClaudeCodeWorkspace(s.kv, env));
  STEP(s, credentials::loadClaudeCodeWorkspaceState(s.kv));
  STEP(s, rawValue(s.kv, credentials::CLAUDE_CODE_AUTH_KV_KEY));
  co_await s.finish(__FILE__, __LINE__);
}

ASYNC_TEST("env bootstrap") {
  Scenario s("env bootstrap");
  const ClaudeCodeEnvLookup env = envOf(parseObject(R"({"CLAUDE_CODE_OAUTH_TOKEN":"sk-ant-oat-fixture-first-account","CLAUDE_CODE_WORKSPACE_ID":"wrkspc_env"})"));
  STEP(s, accounts::resolveClaudeCodeAccount(s.kv, env));
  accounts::ResolveClaudeCodeAccountOptions first{.accountId = "00000000-0000-4000-8000-000000000001"};
  STEP(s, accounts::resolveClaudeCodeAccount(s.kv, env, first));
  accounts::ResolveClaudeCodeAccountOptions nope{.accountId = "nope"};
  STEP(s, accounts::resolveClaudeCodeAccount(s.kv, env, nope));
  STEP(s, credentials::loadClaudeCodeStoredCredential(s.kv));
  STEP(s, credentials::resolveClaudeCodeWorkspace(s.kv));
  STEP(s, accounts::resolveClaudeCodeAccount(nullptr, env));
  STEP(s, credentials::resolveClaudeCodeCredential(nullptr, env));
  STEP(s, credentials::resolveClaudeCodeWorkspace(nullptr, env));
  co_await s.finish(__FILE__, __LINE__);
}

ASYNC_TEST("empty bootstrap") {
  Scenario s("empty bootstrap");
  const ClaudeCodeEnvLookup env = envOf(json::object());
  STEP(s, accounts::loadClaudeCodeAccountStore(s.kv, env));
  STEP(s, accounts::resolveClaudeCodeAccount(s.kv, env));
  STEP(s, credentials::resolveClaudeCodeCredential(s.kv, env));
  STEP(s, credentials::resolveClaudeCodeWorkspace(s.kv, envOf(parseObject(R"({"CLAUDE_CODE_WORKSPACE_ID":"wrkspc_pin"})"))));
  STEP(s, credentials::loadClaudeCodeStoredCredential(s.kv));
  STEP(s, credentials::loadClaudeCodeWorkspaceState(s.kv));
  STEP(s, accounts::removeClaudeCodeAccount(s.kv));
  STEP(s, accounts::loadClaudeCodeAccountStore(s.kv, envOf(parseObject(R"({"CLAUDE_API_KEY":"sk-ant-api-after-tombstone"})"))));
  co_await s.finish(__FILE__, __LINE__);
}

ASYNC_TEST("roster lifecycle") {
  Scenario s("roster lifecycle");
  credentials::StoreClaudeCodeStoredCredentialInput firstSecret{.secret = "  sk-ant-api03-first-account-secret  ", .label = "  first  "};
  STEP(s, credentials::storeClaudeCodeStoredCredential(s.kv, firstSecret));
  STEP(s, generation(s.kv));
  STEP(s, credentials::loadClaudeCodeStoredCredential(s.kv));
  accounts::AccountUpdate createSecond;
  createSecond.createNew = true;
  createSecond.credential = credentialOf(R"({"kind":"oauth","secret":"sk-ant-oat-second","hint":"second","label":null,"source":"kv","updated_at_ms":9})");
  createSecond.workspace = workspaceOf(R"({"id":"wrkspc_second","name":"Second","source":"kv","discovered_at_ms":null,"verified_at_ms":null,"last_error":null})");
  STEP(s, accounts::updateClaudeCodeAccount(s.kv, createSecond));
  accounts::AccountUpdate duplicate;
  duplicate.createNew = true;
  duplicate.credential = credentialOf(R"({"kind":"oauth","secret":"sk-ant-oat-second","hint":"dup","label":null,"source":"kv","updated_at_ms":10})");
  duplicate.workspace = workspaceOf(R"({"id":"wrkspc_second","name":null,"source":"env","discovered_at_ms":null,"verified_at_ms":null,"last_error":null})");
  STEP(s, accounts::updateClaudeCodeAccount(s.kv, duplicate));
  accounts::AccountUpdate unknown;
  unknown.accountId = "00000000-0000-4000-8000-0000000000ff";
  unknown.workspace = nullptr;
  STEP(s, accounts::updateClaudeCodeAccount(s.kv, unknown));
  STEP(s, accounts::selectClaudeCodeAccount(s.kv, "00000000-0000-4000-8000-000000000004"));
  STEP(s, accounts::selectClaudeCodeAccount(s.kv, "00000000-0000-4000-8000-000000000004", "operator"));
  STEP(s, accounts::selectClaudeCodeAccount(s.kv, "missing"));
  accounts::ResolveClaudeCodeAccountOptions secondId{.accountId = "00000000-0000-4000-8000-000000000004"};
  auto resolving = accounts::resolveClaudeCodeAccount(s.kv, ClaudeCodeEnvLookup(), secondId);
  const std::optional<accounts::ClaudeCodeAccountBinding> resolved = co_await std::move(resolving);
  s.step(ok(plain(resolved)), __FILE__, __LINE__);
  ASSERT(resolved.has_value());
  const accounts::ClaudeCodeAccountBinding second = *resolved;
  STEP(s, accounts::isClaudeCodeAccountCurrent(s.kv, second));
  STEP(s, currentEntryValue(s.kv, second));
  accounts::AccountUpdate clearWorkspace;
  clearWorkspace.accountId = second.account_id;
  clearWorkspace.workspace = nullptr;
  clearWorkspace.expected = second;
  STEP(s, accounts::updateClaudeCodeAccount(s.kv, clearWorkspace));
  STEP(s, accounts::isClaudeCodeAccountCurrent(s.kv, second));
  accounts::AccountUpdate stale;
  stale.accountId = second.account_id;
  stale.expected = second;
  STEP(s, accounts::updateClaudeCodeAccount(s.kv, stale));
  STEP(s, accounts::selectClaudeCodeAccount(s.kv, "00000000-0000-4000-8000-000000000001", "admin_selection", second));
  const credentials::ClaudeCodeWorkspaceState storedWorkspace =
    workspaceOf(R"({"id":"wrkspc_stored","name":"Stored","source":"kv","discovered_at_ms":1,"verified_at_ms":2,"last_error":null})");
  STEP(s, credentials::storeClaudeCodeWorkspaceState(s.kv, storedWorkspace));
  STEP(s, credentials::loadClaudeCodeWorkspaceState(s.kv));
  STEP(s, credentials::resolveClaudeCodeWorkspace(s.kv));
  accounts::AccountUpdate rotate;
  rotate.activate = true;
  rotate.credential = credentialOf(R"({"kind":"api_key","secret":"sk-ant-api-rotated","hint":"h","label":"l","source":"CLAUDE_API_KEY","updated_at_ms":null})");
  STEP(s, accounts::updateClaudeCodeAccount(s.kv, rotate));
  STEP(s, accounts::removeClaudeCodeAccount(s.kv, std::string("00000000-0000-4000-8000-000000000001")));
  STEP(s, accounts::removeClaudeCodeAccount(s.kv, std::string("00000000-0000-4000-8000-000000000001")));
  STEP(s, accounts::removeClaudeCodeAccount(s.kv, std::string()));
  STEP(s, credentials::clearClaudeCodeStoredCredential(s.kv));
  STEP(s, generation(s.kv));
  STEP(s, credentials::clearClaudeCodeStoredCredential(s.kv));
  STEP(s, credentials::loadClaudeCodeStoredCredential(s.kv));
  STEP(s, accounts::updateClaudeCodeAccount(s.kv, accounts::AccountUpdate()));
  co_await s.finish(__FILE__, __LINE__);
}

ASYNC_TEST("legacy cache") {
  Scenario s("legacy cache");
  SEED(s.kv, accounts::CLAUDE_CODE_ACCOUNTS_KEY, R"({"v":1,"accounts":[],"active_account_id":null,"active_reason":"x","updated_at_ms":1})");
  STEP(s, credentials::loadClaudeCodeStoredCredential(s.kv));
  co_await s.kv->delete_(accounts::CLAUDE_CODE_ACCOUNTS_KEY);
  SEED(s.kv, credentials::CLAUDE_CODE_AUTH_KV_KEY, R"({"secret":"sk-ant-api-legacy-one","updated_at_ms":3})");
  STEP(s, credentials::loadClaudeCodeStoredCredential(s.kv));
  SEED(s.kv, credentials::CLAUDE_CODE_AUTH_KV_KEY, R"({"secret":"sk-ant-api-legacy-two","updated_at_ms":4})");
  STEP(s, credentials::loadClaudeCodeStoredCredential(s.kv));
  credentials::resetClaudeCodeCredentialCacheForTest();
  STEP(s, credentials::loadClaudeCodeStoredCredential(s.kv));
  const credentials::ClaudeCodeWorkspaceState legacyWorkspace =
    workspaceOf(R"({"id":"wrkspc_legacy","name":null,"source":"env","discovered_at_ms":null,"verified_at_ms":null,"last_error":"e"})");
  STEP(s, credentials::storeClaudeCodeWorkspaceState(s.kv, legacyWorkspace));
  STEP(s, rawValue(s.kv, credentials::CLAUDE_CODE_WORKSPACE_KV_KEY));
  STEP(s, credentials::loadClaudeCodeWorkspaceState(s.kv));
  STEP(s, generation(s.kv));
  STEP(s, generation(nullptr));
  co_await s.finish(__FILE__, __LINE__);
}

ASYNC_TEST("invalid roster") {
  Scenario s("invalid roster");
  SEED(s.kv, accounts::CLAUDE_CODE_ACCOUNTS_KEY, R"({"v":1,"accounts":"bad"})");
  STEP(s, accounts::readClaudeCodeAccountStore(s.kv));
  STEP(s, accounts::loadClaudeCodeAccountStore(s.kv));
  STEP(s, credentials::loadClaudeCodeStoredCredential(s.kv));
  STEP(s, accounts::updateClaudeCodeAccount(s.kv, accounts::AccountUpdate()));
  STEP(s, accounts::removeClaudeCodeAccount(s.kv, std::string("x")));
  STEP(s, accounts::selectClaudeCodeAccount(s.kv, "x"));
  credentials::StoreClaudeCodeStoredCredentialInput plainSecret{.secret = "s"};
  STEP(s, credentials::storeClaudeCodeStoredCredential(s.kv, plainSecret));
  credentials::StoreClaudeCodeStoredCredentialInput blank{.secret = "   "};
  STEP(s, credentials::storeClaudeCodeStoredCredential(s.kv, blank));
  STEP(s, credentials::storeClaudeCodeStoredCredential(nullptr, plainSecret));
  co_await s.finish(__FILE__, __LINE__);
}

ASYNC_TEST("concurrent bootstrap") {
  Scenario s("concurrent bootstrap");
  SEED(s.kv, credentials::CLAUDE_CODE_AUTH_KV_KEY, R"({"secret":"sk-ant-oat-concurrent","updated_at_ms":8})");
  rt::Promise<accounts::ClaudeCodeAccountStore> first(accounts::loadClaudeCodeAccountStore(s.kv));
  rt::Promise<accounts::ClaudeCodeAccountStore> again(accounts::loadClaudeCodeAccountStore(s.kv));
  rt::Promise<std::optional<accounts::ClaudeCodeAccountBinding>> resolved(accounts::resolveClaudeCodeAccount(s.kv));
  const accounts::ClaudeCodeAccountStore firstStore = co_await first.wait();
  const accounts::ClaudeCodeAccountStore againStore = co_await again.wait();
  const std::optional<accounts::ClaudeCodeAccountBinding> account = co_await resolved.wait();
  json::array loads;
  loads.push_back(plain(firstStore));
  loads.push_back(plain(againStore));
  loads.push_back(plain(account));
  s.step(ok(json::value(std::move(loads))), __FILE__, __LINE__);
  accounts::AccountUpdate a;
  a.createNew = true;
  a.credential = credentialOf(R"({"kind":"api_key","secret":"sk-ant-api-a","hint":"a","label":null,"source":"kv","updated_at_ms":1})");
  accounts::AccountUpdate b;
  b.createNew = true;
  b.credential = credentialOf(R"({"kind":"api_key","secret":"sk-ant-api-b","hint":"b","label":null,"source":"kv","updated_at_ms":1})");
  rt::Promise<json::value> updateA(capture(accounts::updateClaudeCodeAccount(s.kv, a)));
  rt::Promise<json::value> updateB(capture(accounts::updateClaudeCodeAccount(s.kv, b)));
  const json::value resultA = co_await updateA.wait();
  const json::value resultB = co_await updateB.wait();
  json::array updates;
  for (const json::value* result : {&resultA, &resultB}) {
    if (const json::value* value = rt::get(*result, "ok")) updates.push_back(*value);
    else updates.push_back(*rt::get(*result, "error"));
  }
  s.step(ok(json::value(std::move(updates))), __FILE__, __LINE__);
  co_await s.finish(__FILE__, __LINE__);
}
