// Port of tests/codex-external-native-auth.test.ts. Not portable (they drive scripts/codex-auth-repair.ts and
// scripts/codex-external-auth-repair.ts, which are outside the C++ port):
//   "fixed external-only entrypoint refuses initial unregistered missing file without provider calls or writes"
//   "fixed external-only entrypoint refuses initial unregistered managed file without provider calls or writes"
//   "registered external repair refuses missing or managed files before peer collection"
//   "external repair rechecks leaf bytes and honors pool CAS without peers"
// "external native renewal ..." keeps the pool in an rt::MemoryKv instead of the unported AuthKv of
// tests/helpers/codex-auth-cache-harness.ts (AuthKv also asserts that every read is strongly consistent).
// Every case clears HOME, CODEX_HOME, CODEX_AUTH_JSON_B64 and config.codexAuthJsonB64 for its scope; native auth
// sees only the synthetic darwin HOME of the TS fixture.
// Links: tests/codex_external_native_auth_test.cpp src/codex/auth_repair.cpp src/codex/auth.cpp src/codex/auth_refresh.cpp
//        src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/external_auth.cpp src/codex/capacity_routing.cpp
//        src/codex/rate_limit_429.cpp src/codex/routing_mutations.cpp src/codex/routing_state.cpp src/codex/routing_evaluation.cpp
//        src/codex/account_routing.cpp src/codex/routing_serial.cpp src/codex/{overage_settings,reset_settings}.cpp src/authority/native_codex.cpp
//        src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp
//        src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/bounded_response_body.cpp src/config.cpp
//        src/kv.cpp src/models/codex_models_availability.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/health.cpp
//        src/provider/selection.cpp src/provider/presentation.cpp src/release.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp
//        src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/runtime/http.cpp src/runtime/websocket.cpp
//        src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/utils.cpp src/types.cpp
#include "uos/codex/external_auth.hpp"

#include "uos/codex/auth.hpp"
#include "uos/codex/auth_refresh.hpp"
#include "uos/codex/auth_repair.hpp"
#include "uos/codex/native_auth.hpp"
#include "uos/codex/routing_evaluation.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::codex::external_auth;

  std::string token(double seconds, const std::string& userId = "user-one", const std::string& accountId = "workspace-one", double generation = 1) {
    json::object payload;
    payload["exp"] = rt::makeNumber(rt::Math::floor(rt::dateNow() / 1000) + seconds);
    payload["generation"] = rt::makeNumber(generation);
    payload["https://api.openai.com/auth"] = json::object{{"chatgpt_user_id", userId}, {"chatgpt_account_id", accountId}};
    return "header." + rt::btoa(rt::jsonStringify(payload)) + ".signature";
  }

  json::object leaf(const std::string& accessToken, const std::optional<std::string>& refreshedAt = std::nullopt) {
    json::object document;
    document["OPENAI_API_KEY"] = nullptr;
    document["auth_mode"] = "chatgptAuthTokens";
    document["tokens"] = json::object{{"access_token", accessToken}, {"id_token", accessToken}, {"refresh_token", ""}, {"account_id", "workspace-one"}};
    if (refreshedAt && !refreshedAt->empty()) document["last_refresh"] = *refreshedAt;
    return document;
  }

  json::object leaf() { return leaf(token(3600)); }

  // `{ ...object, key: value }`
  json::object with(const json::object& object, std::string_view key, json::value value) {
    json::object copy = object;
    rt::objectSet(copy, key, std::move(value));
    return copy;
  }
}

TEST("external leaf requires one access JWT with exact user and workspace claims") {
  const json::object valid = leaf();
  const json::object& tokens = valid.at("tokens").as_object();
  const std::vector<json::object> changed{
    with(valid, "OPENAI_API_KEY", "key"),
    with(valid, "tokens", with(tokens, "id_token", json::string(token(7200)))),
    with(valid, "tokens", with(tokens, "refresh_token", "donor-secret")),
    with(valid, "tokens", with(tokens, "account_id", "other-workspace")),
    leaf(token(3600, "")),
  };
  for (const json::object& document : changed) EXPECT(parseExternalNativeCodexDocument(json::value(document)) == std::nullopt);
}

TEST("fixture: the unmodified leaf parses, so each rejection above is caused by its one change") {
  const std::optional<ExternalNativeCodexDocument> parsed = parseExternalNativeCodexDocument(json::value(leaf()));
  ASSERT(parsed.has_value());
  EXPECT_EQ(parsed->userId, "user-one");
  EXPECT_EQ(parsed->auth.account_id, "workspace-one");
}

/* --------------------------------------------- auth-repair, native adoption, renewal */

namespace {
  using uos::codex::auth_repair::CodexAuthCandidate;
  using uos::codex::auth_repair::codexAuthCandidateFromAuthJson;
  using uos::codex::auth_repair::codexAuthCandidatesFromPool;
  using uos::codex::native_auth::NativeAuthHooks;
  using uos::codex::native_auth::nativeCodexConsumerHomes;
  using uos::codex::native_auth::planExternalNativeCodexAdoption;
  using uos::codex::native_auth::setNativeCodexAuthHooksForTest;
  using uos::types::CodexAuthPoolState;
  using uos::types::CodexAuthState;

  const std::string HOME_DIR = "/synthetic/uos-external";
  const std::string PRIMARY = HOME_DIR + "/.codex";
  const std::string NAMED = HOME_DIR + "/.codex-ubiquity";

  CodexAuthState managed(std::optional<std::string> accessToken = std::nullopt, std::string accountId = "workspace-one") {
    return CodexAuthState{
      .access_token = accessToken ? *accessToken : token(-60),
      .refresh_token = "synthetic-managed-refresh",
      .account_id = std::move(accountId),
      .updated_at_ms = 1,
    };
  }

  CodexAuthPoolState initialPool() { return CodexAuthPoolState{.accounts = {managed(), managed(token(7200, "sibling-user", "sibling-workspace"), "sibling-workspace")}, .updated_at_ms = 1}; }

  // The TS `fixture(run)`: no deployment flag, a synthetic darwin HOME for native auth, and every module
  // seam restored afterwards. The credential home, the seeded auth.json and its config copy are cleared too.
  struct Fixture {
    rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    std::string savedAuthJson = uos::config::config.codexAuthJsonB64;
    bool previousDeploy = uos::config::config.isDeploy;
    Fixture() {
      uos::config::config.codexAuthJsonB64.clear();
      uos::config::config.isDeploy = false;
      setNativeCodexAuthHooksForTest(NativeAuthHooks{.os = "darwin", .getEnv = [](std::string name) { return name == "HOME" ? std::optional<std::string>(HOME_DIR) : std::nullopt; }});
    }
    ~Fixture() {
      setNativeCodexAuthHooksForTest(std::nullopt);
      uos::kv::setKvForTest(nullptr);
      uos::codex::auth::resetCodexAuthCacheForTest();
      uos::config::config.isDeploy = previousDeploy;
      uos::config::config.codexAuthJsonB64 = savedAuthJson;
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
  };

  // `assert.rejects(task, /pattern/)`: String(error) of the rejection, or nullopt when it resolved.
  template <class T> rt::Task<std::optional<std::string>> rejectionText(rt::Task<T> task) {
    rt::Reason failure;
    try {
      co_await std::move(task);
    } catch (...) {
      failure = std::current_exception();
    }
    if (!failure) co_return std::nullopt;
    co_return rt::reasonToString(failure);
  }

  bool rejectedWith(const std::optional<std::string>& text, const char* pattern) { return text && rt::Regex(pattern, "").test(*text); }

  rt::Task<void> rejectNativeRefresh(std::string message) {
    if (!message.empty()) throw rt::JsError(std::move(message));
    co_return;
  }

  rt::Task<rt::Response> rejectFetch(std::string message) {
    if (!message.empty()) throw rt::JsError(std::move(message));
    co_return rt::Response();
  }
}

ASYNC_TEST("official access-only leaf stays out of managed uploads and donor candidates") {
  const Fixture fixture;
  const json::value document = json::value(leaf());
  const std::optional<ExternalNativeCodexDocument> parsed = parseExternalNativeCodexDocument(document);
  ASSERT(parsed.has_value());
  EXPECT_EQ(parsed->auth.refresh_token, "");
  EXPECT(!uos::codex::auth::parseCodexAuthFromAuthJson(document).has_value());
  EXPECT(!codexAuthCandidateFromAuthJson(document, "local-auth-json", 1).has_value());
  auto adopting = planExternalNativeCodexAdoption(initialPool(), document, PRIMARY, rt::dateNow(), false);
  const CodexAuthPoolState adopted = co_await std::move(adopting);
  EXPECT(uos::codex::auth::parseCodexAuthPool(json::value_from(adopted)) == std::optional<CodexAuthPoolState>(adopted));
  EXPECT(uos::codex::routing_evaluation::parseCodexAuthPoolSnapshot(json::value_from(adopted)) == std::optional<CodexAuthPoolState>(adopted));
  std::vector<std::string> donors;
  for (const CodexAuthCandidate& candidate : codexAuthCandidatesFromPool(json::value_from(adopted), "vps-pool")) donors.push_back(candidate.account_id);
  EXPECT_EQ(donors, std::vector<std::string>{"sibling-workspace"});
  // `{ ...adopted, accounts: [{ ...adopted.accounts[0], native_owner: undefined }] }`
  json::object withoutOwner = json::value_from(adopted.accounts[0]).as_object();
  rt::objectDelete(withoutOwner, "native_owner");
  json::object ownerless = json::value_from(adopted).as_object();
  ownerless["accounts"] = json::array{withoutOwner};
  EXPECT(!uos::codex::auth::parseCodexAuthPool(json::value(ownerless)).has_value());
  json::object sentinel = json::value_from(adopted.accounts[0]).as_object();
  sentinel["refresh_token"] = "sentinel";
  json::object refreshed = json::value_from(adopted).as_object();
  refreshed["accounts"] = json::array{sentinel};
  EXPECT(!uos::codex::auth::parseCodexAuthPool(json::value(refreshed)).has_value());
}

namespace {
  rt::Task<void> expectSiblingPreserved(std::string home) {
    const Fixture fixture;
    EXPECT_EQ(nativeCodexConsumerHomes(), (std::vector<std::string>{PRIMARY, NAMED}));
    const CodexAuthPoolState pool = initialPool();
    auto adopting = planExternalNativeCodexAdoption(pool, json::value(leaf()), home, rt::dateNow(), false);
    const CodexAuthPoolState next = co_await std::move(adopting);
    ASSERT(next.accounts.size() == 2 && next.accounts[0].native_owner.has_value());
    EXPECT_EQ(next.accounts[0].native_owner->codex_home, home);
    EXPECT_EQ(next.accounts[0].native_owner->chatgpt_user_id, std::optional<std::string>("user-one"));
    EXPECT(next.accounts[1] == pool.accounts[1]);
    auto elsewhere = rejectionText(planExternalNativeCodexAdoption(pool, json::value(leaf()), HOME_DIR + "/other", rt::dateNow(), false));
    const std::optional<std::string> refused = co_await std::move(elsewhere);
    EXPECT(rejectedWith(refused, "fixed-home"));
  }
}

ASYNC_TEST("external adoption preserves the sibling at fixed home /synthetic/uos-external/.codex") {
  auto checking = expectSiblingPreserved(PRIMARY);
  co_await std::move(checking);
}

ASYNC_TEST("external adoption preserves the sibling at fixed home /synthetic/uos-external/.codex-ubiquity") {
  auto checking = expectSiblingPreserved(NAMED);
  co_await std::move(checking);
}

ASYNC_TEST("external adoption refuses valid managed accounts, user switches and generation rollback") {
  const Fixture fixture;
  const CodexAuthPoolState pool = initialPool();
  CodexAuthPoolState valid = pool;
  valid.accounts = {managed(token(1800)), pool.accounts[1]};
  auto validManaged = rejectionText(planExternalNativeCodexAdoption(valid, json::value(leaf()), PRIMARY, rt::dateNow(), false));
  const std::optional<std::string> validRefusal = co_await std::move(validManaged);
  EXPECT(rejectedWith(validRefusal, "valid managed"));
  auto switched = rejectionText(planExternalNativeCodexAdoption(pool, json::value(leaf(token(3600, "different-user"))), PRIMARY, rt::dateNow(), false));
  const std::optional<std::string> switchRefusal = co_await std::move(switched);
  EXPECT(rejectedWith(switchRefusal, "user or workspace"));
  auto adopting = planExternalNativeCodexAdoption(pool, json::value(leaf(token(7200))), PRIMARY, rt::dateNow(), false);
  const CodexAuthPoolState newer = co_await std::move(adopting);
  auto rollback = rejectionText(planExternalNativeCodexAdoption(newer, json::value(leaf(token(3600))), PRIMARY, rt::dateNow(), true));
  const std::optional<std::string> rollbackRefusal = co_await std::move(rollback);
  EXPECT(rejectedWith(rollbackRefusal, "did not advance"));
  ASSERT(!newer.accounts.empty() && newer.accounts[0].native_owner.has_value());
  CodexAuthPoolState corrupted = newer;
  corrupted.accounts[0].native_owner->generation_hash = std::string(64, 'a');
  auto changed = rejectionText(planExternalNativeCodexAdoption(corrupted, json::value(leaf(token(10800))), PRIMARY, rt::dateNow(), false));
  const std::optional<std::string> changedRefusal = co_await std::move(changed);
  EXPECT(rejectedWith(changedRefusal, "ownership changed"));
}

ASYNC_TEST("external native renewal adopts a new leaf with CAS and never calls refresh or OAuth") {
  const Fixture fixture;
  auto adopting = planExternalNativeCodexAdoption(initialPool(), json::value(leaf(token(3600))), NAMED, rt::dateNow(), false);
  const CodexAuthPoolState first = co_await std::move(adopting);
  // AuthKv (tests/helpers/codex-auth-cache-harness.ts) is not ported: the pool lives in an rt::MemoryKv at
  // the auth key, and `kv.auth` is read back from there.
  const auto kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  const rt::KvValue stored(json::value_from(first));
  auto seeding = kv->set(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY, stored);
  (void)co_await std::move(seeding);
  uos::kv::setKvForTest(kv);
  const auto document = std::make_shared<json::value>(leaf(token(7200)));
  const auto forbidden = std::make_shared<int>(0);
  const rt::GlobalFetchOverride oauth(rt::Fetch([forbidden](rt::RequestInfo, rt::RequestInit) {
    *forbidden += 1;
    return rejectFetch("OAuth forbidden");
  }));
  NativeAuthHooks hooks{.os = "darwin", .getEnv = [](std::string name) { return name == "HOME" ? std::optional<std::string>(HOME_DIR) : std::nullopt; }};
  hooks.readAuth = [document] { return rt::fulfilledTask(*document); };
  hooks.refresh = [forbidden](std::string, std::string, std::optional<std::string>) {
    *forbidden += 1;
    return rejectNativeRefresh("Native refresh forbidden");
  };
  setNativeCodexAuthHooksForTest(hooks);

  uos::codex::auth::CodexAuthAccountEntry entry{};
  entry.kv = kv;
  entry.entry = std::nullopt;
  entry.pool = first;
  entry.auth = first.accounts[0];
  auto refreshing = uos::codex::auth_refresh::refreshAuthCoordinated(entry);
  const CodexAuthState next = co_await std::move(refreshing);
  EXPECT_EQ(next.access_token, std::string(document->at("tokens").at("access_token").as_string()));
  EXPECT_EQ(next.refresh_token, "");
  auto reading = kv->get(uos::codex::auth::CODEX_AUTH_POOL_KV_KEY);
  const rt::KvEntryMaybe<> current = co_await std::move(reading);
  const std::optional<CodexAuthPoolState> storedPool = uos::codex::auth::parseCodexAuthPool(current.value.toJson());
  ASSERT(storedPool.has_value() && storedPool->accounts.size() == 2);
  EXPECT(storedPool->accounts[1] == first.accounts[1]);
  auto refused = rejectionText(uos::codex::auth_refresh::refreshAuthStateless(next));
  const std::optional<std::string> refusal = co_await std::move(refused);
  EXPECT(rejectedWith(refusal, "local refresh was refused"));
  *document = leaf(token(10800));
  auto renewing = uos::codex::auth_refresh::refreshAuthStateless(next);
  const CodexAuthState renewed = co_await std::move(renewing);
  EXPECT(renewed.access_token != next.access_token);
  EXPECT_EQ(*forbidden, 0);
}
