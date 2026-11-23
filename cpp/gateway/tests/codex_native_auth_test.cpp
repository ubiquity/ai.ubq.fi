// Links: tests/codex_native_auth_test.cpp tests/helpers/codex_auth_cache_harness.cpp src/auth/relay.cpp src/authority/native_codex.cpp
//        src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp
//        src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_paid.cpp
//        src/authority/native_paid_contracts.cpp src/bounded_response_body.cpp src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp
//        src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp
//        src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp
//        src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp
//        src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/routing_evaluation.cpp
//        src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp src/http.cpp
//        src/inference_deadline.cpp src/kv.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp src/optional_telemetry_queue.cpp
//        src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
//        src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
//        src/provider/capacity_rollups.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp
//        src/provider/claude_capacity_rollups.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
//        src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/selection.cpp
//        src/release.cpp src/runtime/http.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
//        src/runtime/http_upgrade.cpp src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
//        src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/types.cpp src/utils.cpp
//        src/codex/index.cpp src/codex/responses_state.cpp src/codex/responses_operations.cpp src/codex/responses_runners.cpp
//        src/codex/banked_reset_pool.cpp src/codex/banked_reset_submission.cpp src/codex/banked_reset_claim.cpp
// Port of tests/codex-native-auth.test.ts: all 31 original cases and assertions.
#include "tests/helpers/codex_auth_cache_harness.hpp"
#include "uos/codex/auth_refresh.hpp"
#include "uos/codex/native_auth.hpp"
#include "uos/codex/routing_evaluation.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace h = uos::tests::helpers::codex_auth_cache_harness;
  namespace ca = uos::codex::auth;
  namespace ar = uos::codex::auth_refresh;
  namespace na = uos::codex::native_auth;
  namespace cr = uos::codex::account_routing;
  using Auth = uos::types::CodexAuthState;
  using Pool = uos::types::CodexAuthPoolState;
  using Environment = rt::OrderedMap<std::string, std::string>;
  const std::string NATIVE_HOME = "/synthetic/uos268";
  const std::string DEFAULT_HOME = "/synthetic/uos792";
  Environment nativeEnvironment() { return {{"CODEX_HOME", NATIVE_HOME}, {"HOME", DEFAULT_HOME}}; }
  std::string jwt(const json::value& claims) { return "header." + h::encodeBase64Url(claims) + ".signature"; }
  Auth nativeAuth(const std::string& account, double generation) {
    Auth result; result.account_id = account;
    result.access_token = jwt(json::object{{"exp", rt::Math::floor(rt::dateNow() / 1000) + generation * 3600}, {"generation", generation}});
    result.refresh_token = "synthetic-refresh-" + account + "-" + rt::numberToString(generation);
    result.updated_at_ms = rt::dateNow(); return result;
  }
  json::object document(const Auth& value, std::optional<std::string> refreshedAt = std::nullopt) {
    auto tokens = json::value_from(value).as_object(); tokens["id_token"] = jwt(json::object{{"email", "synthetic@example.test"}});
    json::object result{{"auth_mode", "chatgpt"}, {"tokens", tokens}};
    if (refreshedAt) result["last_refresh"] = *refreshedAt;
    return result;
  }
  na::NativeAuthHooks nativeHooks() {
    na::NativeAuthHooks hooks; hooks.os = "darwin";
    hooks.getEnv = [](std::string name) -> std::optional<std::string> { return name == "CODEX_HOME" ? std::optional(NATIVE_HOME) : std::nullopt; };
    return hooks;
  }
  ca::CodexAuthAccountEntry accountEntry(const ca::CodexAuthPoolEntry& entry, const Auth& auth) {
    ca::CodexAuthAccountEntry result; static_cast<ca::CodexAuthPoolEntry&>(result) = entry; result.auth = auth; return result;
  }
  struct Fixture {
    h::HarnessScope harness;
    Auth first;
    Auth sibling;
    std::shared_ptr<h::AuthKv> kv;
    json::value source;
    rt::Reason sourceError;
    int refreshes = 0;
    int directOauthCalls = 0;
    std::vector<std::string> refreshHomes;
    std::string os = "darwin";
    Environment environment;
    rt::GlobalFetchOverride fetch;
    explicit Fixture(Environment environment_ = nativeEnvironment()) : environment(std::move(environment_)), fetch(rt::Fetch([this](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
      ++directOauthCalls;
      return rt::Promise<rt::Response>::reject(rt::makeReason(rt::JsError("Direct OAuth was forbidden by this fixture"))).wait();
    })) {
      rt::globalDateNow() = [] { return 1'800'000'000'000.0; };
      first = nativeAuth("shared", 1); sibling = nativeAuth("uploaded", 2);
      kv = std::make_shared<h::AuthKv>(Pool{{first, sibling}, rt::dateNow()}); source = document(first);
      uos::config::config.isDeploy = false; uos::kv::setKvForTest(kv); ca::resetCodexAuthCacheForTest(); installHooks();
    }
    void installHooks() {
      auto hooks = nativeHooks(); hooks.os = os;
      hooks.getEnv = [this](std::string name) -> std::optional<std::string> { const auto* value = environment.get(name); return value ? std::optional(*value) : std::nullopt; };
      hooks.readAuth = [this]() -> rt::Task<json::value> {
        if (sourceError) return rt::Promise<json::value>::reject(sourceError).wait();
        return rt::Promise<json::value>::resolve(source).wait();
      };
      hooks.refresh = [this](std::string home, std::string, std::optional<std::string>) {
        ++refreshes; refreshHomes.push_back(std::move(home)); source = document(nativeAuth("shared", 3)); sourceError = nullptr;
        return rt::Promise<void>::resolve().wait();
      };
      na::setNativeCodexAuthHooksForTest(hooks);
    }
    void setDocument(json::value value) { source = std::move(value); sourceError = nullptr; }
    void setOs(std::string value) { os = std::move(value); installHooks(); }
    ~Fixture() { na::setNativeCodexAuthHooksForTest(std::nullopt); uos::kv::setKvForTest(nullptr); ca::resetCodexAuthCacheForTest(); }
  };
  template<class T> rt::Task<rt::Reason> failureOf(rt::Task<T> task) {
    rt::Reason failure; rt::Promise<T> pending(std::move(task));
    try { (void) co_await pending.wait(); } catch (...) { failure = std::current_exception(); }
    co_return failure;
  }
}
#define AWAIT(name, expression) rt::Promise name##Pending((expression)); auto name = co_await name##Pending.wait()
#define WAIT(expression) do { rt::Promise pending((expression)); co_await pending.wait(); } while (false)
#define REJECTS_CONTAINING(expression, text) do { AWAIT(failure, failureOf(expression)); EXPECT(failure); EXPECT(rt::includes(rt::errorMessage(failure).value_or(""), text)); } while (false)

namespace {
  rt::Task<void> resolverCase(Environment environment, std::string expectedHome, bool linux) {
    Fixture state(std::move(environment)); if (linux) state.setOs("linux");
    AWAIT(entry, ca::getAuthPoolEntry(true)); const auto owner = entry.pool.accounts[0].native_owner; ASSERT(owner);
    EXPECT_EQ(owner->codex_home, expectedHome); AWAIT(generation, na::nativeCodexCredentialGeneration(state.first)); EXPECT_EQ(owner->generation_hash, generation);
    AWAIT(next, ar::refreshAuthCoordinated(accountEntry(entry, entry.pool.accounts[0]))); ASSERT(next.native_owner);
    EXPECT_EQ(next.native_owner->codex_home, expectedHome);
    if (!linux) { AWAIT(nextGeneration, na::nativeCodexCredentialGeneration(next)); EXPECT_EQ(next.native_owner->generation_hash, nextGeneration); }
    EXPECT(next.refresh_token != state.first.refresh_token); EXPECT_EQ(state.refreshHomes, (std::vector<std::string>{expectedHome}));
    EXPECT_EQ(state.kv->auth.accounts[1], state.sibling); EXPECT_EQ(state.directOauthCalls, 0);
  }
  rt::Task<void> guardCase(bool platform) {
    Fixture state; if (platform) state.setOs("win32"); else uos::config::config.isDeploy = true;
    AWAIT(entry, ca::getAuthPoolEntry(true)); EXPECT_EQ(entry.pool.accounts[0].native_owner, std::nullopt);
    auto owner = state.first; AWAIT(generation, na::nativeCodexCredentialGeneration(state.first));
    owner.native_owner = Auth::NativeOwner{.codex_home = NATIVE_HOME, .generation_hash = generation}; const auto snapshot = state.kv->auth;
    AWAIT(failure, failureOf(ar::refreshAuthStateless(owner))); const auto* error = ca::codexErrorOf(failure); ASSERT(error);
    EXPECT_EQ(error->code, "codex_auth_owner_unavailable"); EXPECT_EQ(error->status, 503);
    EXPECT_EQ(state.refreshes, 0); EXPECT_EQ(state.directOauthCalls, 0); EXPECT_EQ(state.kv->auth, snapshot);
  }
  rt::Task<void> changedEnvironmentCase(Environment environment) {
    Fixture state; AWAIT(entry, ca::getAuthPoolEntry(true)); const auto snapshot = state.kv->auth; state.environment = std::move(environment);
    AWAIT(failure, failureOf(ar::refreshAuthCoordinated(accountEntry(entry, entry.pool.accounts[0])))); const auto* error = ca::codexErrorOf(failure); ASSERT(error);
    EXPECT_EQ(error->code, "codex_auth_owner_unavailable"); EXPECT_EQ(error->status, 503);
    EXPECT_EQ(state.refreshes, 0); EXPECT_EQ(state.directOauthCalls, 0); EXPECT_EQ(state.kv->auth, snapshot);
  }
  rt::Task<void> unknownExpiryOrderCase(bool older) {
    Fixture state; auto previous = state.first; previous.access_token = "synthetic-opaque-access-token";
    state.kv->auth.accounts = {previous, state.sibling};
    state.setDocument(document(previous, rt::Date(rt::dateNow() - 3'600'000).toISOString()));
    AWAIT(entry, ca::getAuthPoolEntry(true)); const auto snapshot = state.kv->auth;
    auto expired = previous; expired.access_token = jwt(json::object{{"exp", rt::Math::floor(rt::dateNow() / 1000) - 60}, {"generation", 2}});
    expired.refresh_token = "synthetic-older-native-refresh";
    state.setDocument(document(expired, older ? std::optional(rt::Date(rt::dateNow() - 7'200'000).toISOString()) : std::nullopt));
    REJECTS_CONTAINING(ar::refreshAuthCoordinated(accountEntry(entry, entry.pool.accounts[0])), "did not advance");
    EXPECT_EQ(state.refreshes, 0); EXPECT_EQ(state.directOauthCalls, 0); EXPECT_EQ(state.kv->auth, snapshot);
  }
  rt::Task<void> boundFailureCase(std::string failure) {
    Fixture state; AWAIT(entry, ca::getAuthPoolEntry(true)); const auto snapshot = state.kv->auth;
    if (failure == "missing") state.sourceError = rt::makeReason(rt::JsError("synthetic missing file"));
    if (failure == "account-mismatch") state.setDocument(document(nativeAuth("foreign", 4)));
    if (failure == "rollback") state.setDocument(document(nativeAuth("shared", 0)));
    AWAIT(reason, failureOf(ar::refreshAuthCoordinated(accountEntry(entry, entry.pool.accounts[0])))); EXPECT(ca::codexErrorOf(reason));
    EXPECT_EQ(state.refreshes, 0); EXPECT_EQ(state.kv->auth, snapshot);
  }
}

ASYNC_TEST("native ownership binds only the equal CLI account and survives cache restart") {
  Fixture state; AWAIT(entry, ca::getAuthPoolEntry(true)); (void) entry;
  const auto stored = state.kv->auth.accounts[0]; ASSERT(stored.native_owner);
  AWAIT(generation, na::nativeCodexCredentialGeneration(state.first)); EXPECT_EQ(stored.native_owner->generation_hash, generation);
  EXPECT_EQ(state.kv->auth.accounts[1], state.sibling); ca::resetCodexAuthCacheForTest(); AWAIT(restarted, ca::getAuthPoolEntry(true));
  EXPECT_EQ(std::optional(restarted.pool), ca::parseCodexAuthPool(json::value_from(state.kv->auth)));
}
ASYNC_TEST("native resolver CODEX_HOME override binds and refreshes the same credential owner") {
  WAIT(resolverCase(nativeEnvironment(), NATIVE_HOME, false));
}
ASYNC_TEST("native resolver CODEX_HOME without HOME binds and refreshes the same credential owner") {
  const Environment environment{{"CODEX_HOME", NATIVE_HOME}}; WAIT(resolverCase(environment, NATIVE_HOME, false));
}
ASYNC_TEST("native resolver HOME default binds and refreshes the same credential owner") {
  const Environment environment{{"HOME", DEFAULT_HOME}}; WAIT(resolverCase(environment, DEFAULT_HOME + "/.codex", false));
}
ASYNC_TEST("native resolver empty CODEX_HOME binds and refreshes the same credential owner") {
  const Environment environment{{"CODEX_HOME", ""}, {"HOME", DEFAULT_HOME}}; WAIT(resolverCase(environment, DEFAULT_HOME + "/.codex", false));
}
ASYNC_TEST("native resolver on Linux CODEX_HOME override binds and refreshes the same credential owner") {
  WAIT(resolverCase(nativeEnvironment(), NATIVE_HOME, true));
}
ASYNC_TEST("native resolver on Linux HOME default binds and refreshes the same credential owner") {
  const Environment environment{{"HOME", DEFAULT_HOME}}; WAIT(resolverCase(environment, DEFAULT_HOME + "/.codex", true));
}
ASYNC_TEST("native resolver preserves its platform guard despite CODEX_HOME") { WAIT(guardCase(true)); }
ASYNC_TEST("native resolver preserves its deployment guard despite CODEX_HOME") { WAIT(guardCase(false)); }
ASYNC_TEST("an established custom native owner refuses changed custom home without direct OAuth") {
  const Environment environment{{"CODEX_HOME", "/synthetic/foreign"}, {"HOME", DEFAULT_HOME}}; WAIT(changedEnvironmentCase(environment));
}
ASYNC_TEST("an established custom native owner refuses removed custom home without direct OAuth") {
  const Environment environment{{"HOME", DEFAULT_HOME}}; WAIT(changedEnvironmentCase(environment));
}
ASYNC_TEST("an established custom native owner refuses missing environment without direct OAuth") {
  const Environment environment; WAIT(changedEnvironmentCase(environment));
}
ASYNC_TEST("native refresh adopts its persisted generation while preserving an uploaded sibling") {
  Fixture state; AWAIT(entry, ca::getAuthPoolEntry(true)); AWAIT(next, ar::refreshAuthCoordinated(accountEntry(entry, entry.pool.accounts[0])));
  EXPECT(next.refresh_token != state.first.refresh_token); EXPECT_EQ(state.refreshes, 1); EXPECT_EQ(state.kv->auth.accounts[1], state.sibling);
  ASSERT(next.native_owner); AWAIT(generation, na::nativeCodexCredentialGeneration(next)); EXPECT_EQ(next.native_owner->generation_hash, generation);
}
ASYNC_TEST("routed native auth adopts an intervening persisted rotation without another refresh") {
  Fixture state; const auto nativeRefreshedAt = rt::Date(rt::dateNow()).toISOString(); state.setDocument(document(state.first, nativeRefreshedAt));
  AWAIT(entry, ca::getAuthPoolEntry(true)); AWAIT(selection, cr::selectCodexRoutingAccountsStrong(entry.pool, entry.pool.accounts));
  const auto* eligible = std::get_if<uos::codex::routing_state::RouteSelectionEligible>(&selection); ASSERT(eligible); ASSERT(!eligible->accounts.empty());
  const auto routed = eligible->accounts[0]; EXPECT_EQ(routed.auth.account_id, state.first.account_id);
  EXPECT_EQ(routed.auth.native_owner, entry.pool.accounts[0].native_owner); ASSERT(routed.auth.native_owner);
  EXPECT_EQ(routed.auth.native_owner->native_refreshed_at, std::optional(nativeRefreshedAt));
  AWAIT(rotated, ar::refreshAuthCoordinated(accountEntry(entry, entry.pool.accounts[0])));
  EXPECT(rotated.access_token != routed.auth.access_token); EXPECT(rotated.refresh_token != routed.auth.refresh_token); const auto persisted = state.kv->auth;
  AWAIT(adopted, ar::refreshAuthCoordinated(accountEntry(entry, routed.auth))); EXPECT_EQ(adopted, rotated); ASSERT(adopted.native_owner);
  AWAIT(generation, na::nativeCodexCredentialGeneration(rotated)); EXPECT_EQ(adopted.native_owner->generation_hash, generation);
  EXPECT_EQ(state.refreshes, 1); EXPECT_EQ(state.directOauthCalls, 0); EXPECT_EQ(state.kv->auth, persisted); EXPECT_EQ(state.kv->auth.accounts[1], state.sibling);
}
TEST("routed snapshots preserve unbound accounts and refuse malformed native ownership") {
  h::HarnessScope scope; const auto unbound = nativeAuth("uploaded", 1); Pool pool{{unbound}, unbound.updated_at_ms};
  EXPECT_EQ(uos::codex::routing_evaluation::parseCodexAuthPoolSnapshot(json::value_from(pool)), std::optional(pool));
  auto maximumPool = pool; maximumPool.accounts.clear();
  for (int index = 0; index < ca::CODEX_AUTH_POOL_MAX_ACCOUNTS; ++index) maximumPool.accounts.push_back(nativeAuth("uploaded-" + std::to_string(index), 1));
  EXPECT_EQ(uos::codex::routing_evaluation::parseCodexAuthPoolSnapshot(json::value_from(maximumPool)), std::optional(maximumPool));
  auto overflow = maximumPool; overflow.accounts.push_back(nativeAuth("overflow", 1));
  EXPECT_EQ(uos::codex::routing_evaluation::parseCodexAuthPoolSnapshot(json::value_from(overflow)), std::nullopt);
  auto malformed = unbound; malformed.native_owner = Auth::NativeOwner{.codex_home = NATIVE_HOME, .generation_hash = "invalid"};
  auto invalid = pool; invalid.accounts = {malformed}; const auto invalidValue = json::value_from(invalid);
  EXPECT_THROWS_MATCHING(uos::codex::routing_evaluation::parseCodexAuthPoolSnapshot(invalidValue), "ownership is malformed");
  auto negative = pool; negative.updated_at_ms = -1; EXPECT_EQ(uos::codex::routing_evaluation::parseCodexAuthPoolSnapshot(json::value_from(negative)), std::nullopt);
  auto fractional = pool; fractional.accounts[0].updated_at_ms = 1.5; EXPECT_EQ(uos::codex::routing_evaluation::parseCodexAuthPoolSnapshot(json::value_from(fractional)), std::nullopt);
  auto duplicate = pool; duplicate.accounts = {unbound, unbound}; EXPECT_EQ(uos::codex::routing_evaluation::parseCodexAuthPoolSnapshot(json::value_from(duplicate)), std::nullopt);
}
ASYNC_TEST("an established native owner adopts a newer synced generation without another rotation") {
  Fixture state; AWAIT(entry, ca::getAuthPoolEntry(true)); const auto updated = nativeAuth("shared", 4); state.setDocument(document(updated));
  AWAIT(next, ar::refreshAuthCoordinated(accountEntry(entry, entry.pool.accounts[0]))); EXPECT_EQ(next.refresh_token, updated.refresh_token); EXPECT_EQ(state.refreshes, 0);
}
ASYNC_TEST("an expired newer native generation is refreshed by its owner before adoption") {
  Fixture state; const auto now = rt::Math::floor(rt::dateNow() / 1000); auto previous = state.first;
  previous.access_token = jwt(json::object{{"exp", now - 60}, {"generation", 1}}); state.kv->auth.accounts = {previous, state.sibling};
  state.setDocument(document(previous, rt::Date(rt::dateNow() - 7'200'000).toISOString())); AWAIT(entry, ca::getAuthPoolEntry(true));
  auto expired = previous; expired.access_token = jwt(json::object{{"exp", now - 60}, {"generation", 2}}); expired.refresh_token = "synthetic-expired-newer-generation";
  state.setDocument(document(expired, rt::Date(rt::dateNow() - 3'600'000).toISOString()));
  AWAIT(next, ar::refreshAuthCoordinated(accountEntry(entry, entry.pool.accounts[0])));
  EXPECT_EQ(next.refresh_token, nativeAuth("shared", 3).refresh_token); EXPECT_EQ(state.refreshes, 1); EXPECT_EQ(state.directOauthCalls, 0);
  EXPECT_EQ(state.kv->auth.accounts[1], state.sibling); EXPECT_EQ(state.kv->auth.accounts[0].refresh_token, next.refresh_token);
}
ASYNC_TEST("an unusable native owner result is not adopted or retried") {
  Fixture state; const auto now = rt::Math::floor(rt::dateNow() / 1000); auto previous = state.first;
  previous.access_token = jwt(json::object{{"exp", now - 60}, {"generation", 1}}); state.kv->auth.accounts = {previous, state.sibling};
  state.setDocument(document(previous, rt::Date(rt::dateNow() - 7'200'000).toISOString())); AWAIT(entry, ca::getAuthPoolEntry(true)); const auto snapshot = state.kv->auth;
  auto expired = previous; expired.access_token = jwt(json::object{{"exp", now - 60}, {"generation", 2}}); expired.refresh_token = "synthetic-expired-newer-generation";
  const auto expiredDocument = document(expired, rt::Date(rt::dateNow() - 3'600'000).toISOString()); int ownerRequests = 0;
  auto hooks = nativeHooks(); hooks.readAuth = [expiredDocument] { return rt::Promise<json::value>::resolve(expiredDocument).wait(); };
  hooks.refresh = [&](std::string, std::string, std::optional<std::string>) { ++ownerRequests; EXPECT_EQ(state.kv->auth, snapshot); return rt::Promise<void>::resolve().wait(); };
  na::setNativeCodexAuthHooksForTest(hooks);
  REJECTS_CONTAINING(ar::refreshAuthCoordinated(accountEntry(entry, entry.pool.accounts[0])), "did not advance");
  EXPECT_EQ(ownerRequests, 1); EXPECT_EQ(state.directOauthCalls, 0); EXPECT_EQ(state.kv->auth, snapshot);
}
ASYNC_TEST("native repair requires order proof when stored access expiry is unknown: older") { WAIT(unknownExpiryOrderCase(true)); }
ASYNC_TEST("native repair requires order proof when stored access expiry is unknown: missing") { WAIT(unknownExpiryOrderCase(false)); }
ASYNC_TEST("native repair can use a newer owner timestamp when stored access expiry is unknown") {
  Fixture state; auto previous = state.first; previous.access_token = "synthetic-opaque-access-token"; state.kv->auth.accounts = {previous, state.sibling};
  state.setDocument(document(previous, rt::Date(rt::dateNow() - 7'200'000).toISOString())); AWAIT(entry, ca::getAuthPoolEntry(true));
  auto expired = previous; expired.access_token = jwt(json::object{{"exp", rt::Math::floor(rt::dateNow() / 1000) - 60}, {"generation", 2}});
  expired.refresh_token = "synthetic-newer-native-refresh"; state.setDocument(document(expired, rt::Date(rt::dateNow() - 3'600'000).toISOString()));
  AWAIT(next, ar::refreshAuthCoordinated(accountEntry(entry, entry.pool.accounts[0])));
  EXPECT_EQ(next.refresh_token, nativeAuth("shared", 3).refresh_token); EXPECT_EQ(state.refreshes, 1); EXPECT_EQ(state.kv->auth.accounts[1], state.sibling);
}
ASYNC_TEST("a usable native generation retains adoption when stored access expiry is unknown") {
  Fixture state; auto previous = state.first; previous.access_token = "synthetic-opaque-access-token"; state.kv->auth.accounts = {previous, state.sibling};
  state.setDocument(document(previous)); AWAIT(entry, ca::getAuthPoolEntry(true)); const auto usable = nativeAuth("shared", 4); state.setDocument(document(usable));
  AWAIT(next, ar::refreshAuthCoordinated(accountEntry(entry, entry.pool.accounts[0])));
  EXPECT_EQ(next.refresh_token, usable.refresh_token); EXPECT_EQ(state.refreshes, 0); EXPECT_EQ(state.kv->auth.accounts[1], state.sibling);
}
ASYNC_TEST("same-expiry native rotations use owner generation metadata and reject a rollback") {
  Fixture state; const auto observed = rt::Date(rt::dateNow()).toISOString(); const auto prior = observed.substr(0, observed.size() - 1) + "001Z";
  const auto later = observed.substr(0, observed.size() - 1) + "002Z"; state.setDocument(document(state.first, prior)); AWAIT(entry, ca::getAuthPoolEntry(true));
  auto next = state.first; next.refresh_token = "synthetic-same-expiry-rotation"; state.setDocument(document(next, later));
  AWAIT(advanced, ar::refreshAuthCoordinated(accountEntry(entry, entry.pool.accounts[0]))); EXPECT_EQ(advanced.refresh_token, next.refresh_token); EXPECT_EQ(state.refreshes, 0);
  AWAIT(current, ca::getAuthPoolEntry(true)); state.setDocument(document(state.first, prior));
  REJECTS_CONTAINING(ar::refreshAuthCoordinated(accountEntry(current, current.pool.accounts[0])), "did not advance");
}
ASYNC_TEST("bound native missing refuses direct OAuth and credential replacement") { WAIT(boundFailureCase("missing")); }
ASYNC_TEST("bound native account-mismatch refuses direct OAuth and credential replacement") { WAIT(boundFailureCase("account-mismatch")); }
ASYNC_TEST("bound native rollback refuses direct OAuth and credential replacement") { WAIT(boundFailureCase("rollback")); }
ASYNC_TEST("successful native RPC without persisted progress remains a failed refresh") {
  Fixture state; AWAIT(entry, ca::getAuthPoolEntry(true)); auto hooks = nativeHooks();
  hooks.readAuth = [&] { return rt::Promise<json::value>::resolve(document(state.first)).wait(); };
  hooks.refresh = [](std::string, std::string, std::optional<std::string>) { return rt::Promise<void>::resolve().wait(); }; na::setNativeCodexAuthHooksForTest(hooks);
  REJECTS_CONTAINING(ar::refreshAuthCoordinated(accountEntry(entry, entry.pool.accounts[0])), "did not persist");
}
ASYNC_TEST("native daemon failure never activates a direct refresh fallback") {
  Fixture state; AWAIT(entry, ca::getAuthPoolEntry(true)); auto hooks = nativeHooks();
  hooks.readAuth = [&] { return rt::Promise<json::value>::resolve(document(state.first)).wait(); };
  hooks.refresh = [](std::string, std::string, std::optional<std::string>) { return rt::Promise<void>::reject(rt::makeReason(rt::JsError("synthetic absent daemon"))).wait(); };
  na::setNativeCodexAuthHooksForTest(hooks); REJECTS_CONTAINING(ar::refreshAuthStateless(entry.pool.accounts[0]), "direct refresh was refused");
}
ASYNC_TEST("stateless upload refresh rejects an obsolete bound credential before dispatch") {
  Fixture state; AWAIT(entry, ca::getAuthPoolEntry(true)); state.setDocument(document(nativeAuth("shared", 4)));
  AWAIT(adopted, ar::refreshAuthCoordinated(accountEntry(entry, entry.pool.accounts[0]))); (void) adopted;
  REJECTS_CONTAINING(ar::refreshAuthStateless(state.first), "stale native"); EXPECT_EQ(state.refreshes, 0);
}
ASYNC_TEST("same-account upload and repair preserve native binding and reject a stale replacement") {
  Fixture state; AWAIT(entry, ca::getAuthPoolEntry(true)); const auto unchanged = ca::upsertCodexAuthAccount(entry.pool, state.first); ASSERT(unchanged);
  EXPECT_EQ(unchanged->accounts[0].native_owner, entry.pool.accounts[0].native_owner);
  EXPECT_THROWS_MATCHING(ca::upsertCodexAuthAccount(entry.pool, nativeAuth("shared", 4)), "native Codex owner");
}
ASYNC_TEST("native generation CAS retries preserve a concurrent unrelated-account upload") {
  Fixture state; AWAIT(entry, ca::getAuthPoolEntry(true)); const auto replacement = nativeAuth("uploaded", 5); auto hooks = nativeHooks();
  hooks.readAuth = [&] { return rt::Promise<json::value>::resolve(document(state.first)).wait(); };
  hooks.refresh = [&](std::string, std::string, std::optional<std::string>) {
    state.kv->auth = Pool{{state.kv->auth.accounts[0], replacement}, rt::dateNow()}; ++state.kv->authVersion;
    auto updatedHooks = nativeHooks(); updatedHooks.readAuth = [] { return rt::Promise<json::value>::resolve(document(nativeAuth("shared", 3))).wait(); };
    na::setNativeCodexAuthHooksForTest(updatedHooks); return rt::Promise<void>::resolve().wait();
  };
  na::setNativeCodexAuthHooksForTest(hooks); AWAIT(adopted, ar::refreshAuthCoordinated(accountEntry(entry, entry.pool.accounts[0]))); (void) adopted;
  EXPECT_EQ(state.kv->auth.accounts[1], replacement);
}
