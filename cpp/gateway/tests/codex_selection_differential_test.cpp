// Differential cases for src/codex/account-routing.ts, src/codex/routing-serial.ts, src/codex/routing-evaluation.ts,
// src/codex/native-auth.ts (through its test hooks) and the seeding path of src/codex/auth.ts; their direct bun suites
// need modules of later sub-chunks. The vectors were recorded by running the TypeScript modules under bun 1.4.0 over
// the same deterministic KV (tests/helpers/codex_diff_kv) with a fixed clock and HOME, CODEX_HOME and
// CODEX_AUTH_JSON_B64 unset; random tokens are masked. The scenario below mirrors the generator step for step.
// One vector differs by construction: the TS `electStale` account carries the `quotaResetAtMs` property of the
// CodexBlockedRoutingAccount it was given (structural typing keeps it through the spread); a C++ RoutingAccount has
// no such member, so that key is dropped from the expectation.
// Links: tests/codex_selection_differential_test.cpp tests/helpers/codex_diff_kv.cpp src/codex/account_routing.cpp src/codex/routing_serial.cpp
//        src/codex/routing_evaluation.cpp src/codex/capacity_routing.cpp src/codex/rate_limit_429.cpp src/codex/routing_mutations.cpp
//        src/codex/overage_settings.cpp src/codex/routing_state.cpp src/codex/auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp
//        src/codex/external_auth.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp
//        src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp
//        src/models/codex_models_availability.cpp src/provider/selection.cpp src/provider/presentation.cpp src/provider/health.cpp
//        src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/bounded_response_body.cpp src/config.cpp src/release.cpp
//        src/kv.cpp src/types.cpp src/utils.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
//        src/runtime/http_upgrade.cpp src/runtime/http.cpp src/runtime/websocket.cpp src/storage/kv.cpp src/storage/sqlite_readonly.cpp
//        src/storage/native_kv.cpp src/storage/value_compatibility.cpp src/storage/key_compatibility.cpp
#include "tests/helpers/codex_diff_kv.hpp"
#include "uos/codex/account_routing.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/native_auth.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/codex/routing_evaluation.hpp"

#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace account_routing = uos::codex::account_routing;
  namespace routing_evaluation = uos::codex::routing_evaluation;
  namespace routing_state = uos::codex::routing_state;
  namespace rate_limit_429 = uos::codex::rate_limit_429;
  namespace auth = uos::codex::auth;
  namespace native_auth = uos::codex::native_auth;
  using uos::tests::helpers::codex_diff_kv::DiffKv;
  using uos::tests::helpers::codex_diff_kv::plain;
  using uos::tests::helpers::codex_diff_kv::routingAccountJson;
  using uos::tests::helpers::codex_diff_kv::sameVector;
  using routing_state::RouteSelection;
  using routing_state::RoutingAccount;
  using uos::types::CodexAuthPoolState;
  using uos::types::CodexAuthState;

  const char* const VECTORS =
#include "codex_selection_differential_vectors.inc"
    ;

  constexpr double NOW = 1'790'000'000'000;

  std::string b64url(const json::value& value) {
    std::string encoded = rt::replaceAll(rt::replaceAll(rt::btoa(rt::jsonStringify(value)), "+", "-"), "/", "_");
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return encoded;
  }

  std::string jwt(const json::value& payload) {
    json::object header;
    header["alg"] = "none";
    return b64url(json::value(header)) + "." + b64url(payload) + ".sig";
  }

  json::value expiring(double secondsFromNow) {
    json::object payload;
    payload["exp"] = rt::makeNumber(NOW / 1000 + secondsFromNow);
    return json::value(std::move(payload));
  }

  rt::Response response429(double resetsAt) {
    rt::ResponseInit init;
    init.status = 429;
    init.headers = rt::Headers{{"content-type", "application/json"}};
    return rt::Response(rt::BodyInit(R"({"error":{"type":"usage_limit_reached","resets_at":)" + rt::numberToString(resetsAt) + "}}"), init);
  }

  json::value numberOrNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
  json::value stringOrNull(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }

  json::value numbers(const std::vector<double>& values) {
    json::array out;
    for (const double value : values) out.push_back(rt::makeNumber(value));
    return json::value(std::move(out));
  }

  json::value blockedAccountJson(const routing_state::CodexBlockedRoutingAccount& account) {
    json::value out = routingAccountJson(static_cast<const RoutingAccount&>(account));
    out.as_object()["quotaResetAtMs"] = rt::makeNumber(account.quotaResetAtMs);
    return out;
  }

  json::value blockedAccountsJson(const std::vector<routing_state::CodexBlockedRoutingAccount>& accounts) {
    json::array out;
    for (const auto& account : accounts) out.push_back(blockedAccountJson(account));
    return json::value(std::move(out));
  }

  // `JSON.parse(JSON.stringify(selection))` of a TS RouteSelection.
  json::value selectionJson(const RouteSelection& selection) {
    json::object out;
    if (const auto* eligible = std::get_if<routing_state::RouteSelectionEligible>(&selection)) {
      out["kind"] = "eligible";
      json::array accounts;
      for (const RoutingAccount& account : eligible->accounts) accounts.push_back(routingAccountJson(account));
      out["accounts"] = std::move(accounts);
      out["skippedSlots"] = numbers(eligible->skippedSlots);
      out["blockedAccounts"] = blockedAccountsJson(eligible->blockedAccounts);
    } else if (const auto* blocked = std::get_if<routing_state::RouteSelectionQuotaBlocked>(&selection)) {
      out["kind"] = "quota_blocked";
      out["skippedSlots"] = numbers(blocked->skippedSlots);
      out["retryAtMs"] = numberOrNull(blocked->retryAtMs);
      out["blockedAccounts"] = blockedAccountsJson(blocked->blockedAccounts);
      out["fullCohortExhausted"] = blocked->fullCohortExhausted;
      if (blocked->fullCohortUnavailable) out["fullCohortUnavailable"] = *blocked->fullCohortUnavailable;
      json::object snapshot;
      snapshot["selection"] = blocked->activeSnapshot.selection ? json::value(routing_state::toJson(*blocked->activeSnapshot.selection)) : json::value(nullptr);
      out["activeSnapshot"] = std::move(snapshot);
      out["poolSnapshotJson"] = stringOrNull(blocked->poolSnapshotJson);
      out["capacitySnapshotJson"] = stringOrNull(blocked->capacitySnapshotJson);
    } else if (const auto* upstream = std::get_if<routing_state::RouteSelectionUpstreamBlocked>(&selection)) {
      out["kind"] = "upstream_blocked";
      out["skippedSlots"] = numbers(upstream->skippedSlots);
      out["retryAtMs"] = numberOrNull(upstream->retryAtMs);
      out["blockedAccounts"] = json::array{};
    } else if (const auto* model = std::get_if<routing_state::RouteSelectionModelUnavailable>(&selection)) {
      out["kind"] = "model_unavailable";
      out["model"] = model->model;
      out["detail"] = stringOrNull(model->detail);
      out["skippedSlots"] = numbers(model->skippedSlots);
    } else if (const auto* credentials = std::get_if<routing_state::RouteSelectionCredentialsInvalid>(&selection)) {
      out["kind"] = "credentials_invalid";
      out["skippedSlots"] = numbers(credentials->skippedSlots);
      if (credentials->fullCohortUnavailable) out["fullCohortUnavailable"] = *credentials->fullCohortUnavailable;
    } else {
      out["kind"] = "routing_unavailable";
    }
    return plain(json::value(std::move(out)));
  }

  json::value poolJson(const std::optional<CodexAuthPoolState>& pool) { return pool ? plain(json::value_from(*pool)) : json::value(nullptr); }

  json::value errorJson(const rt::Reason& reason) {
    json::object out;
    out["name"] = stringOrNull(rt::errorName(reason));
    out["code"] = nullptr;
    out["status"] = nullptr;
    out["message"] = rt::errorMessage(reason).value_or(rt::reasonToString(reason));
    try {
      std::rethrow_exception(reason);
    } catch (const auth::CodexError& error) {
      out["code"] = error.code;
      out["status"] = rt::makeNumber(error.status);
    } catch (...) {
    }
    return json::value(std::move(out));
  }

  rt::Task<json::value> attemptPool(rt::Task<CodexAuthPoolState> task) {
    rt::Reason failure;
    json::value result;
    try {
      CodexAuthPoolState pool = co_await std::move(task);
      result = poolJson(pool);
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) co_return errorJson(failure);
    co_return result;
  }

  rt::Task<json::value> attemptEntry(rt::Task<auth::CodexAuthPoolEntry> task) {
    rt::Reason failure;
    json::value result;
    try {
      auth::CodexAuthPoolEntry entry = co_await std::move(task);
      result = poolJson(entry.pool);
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) co_return errorJson(failure);
    co_return result;
  }

  rt::Task<void> setRow(std::shared_ptr<DiffKv> kv, rt::KvKey key, json::value value) {
    const rt::KvValue stored(value);
    auto setting = kv->set(std::move(key), stored);
    co_await std::move(setting);
  }

  void resetAll() {
    auth::resetCodexAuthCacheForTest();
    uos::codex::overage_settings::resetOverageUsageCacheForTest();
    uos::provider::selection::resetProviderSelectionCacheForTest();
  }

  json::value object2(std::string_view firstName, json::value first, std::string_view secondName, json::value second) {
    json::object out;
    out[firstName] = std::move(first);
    out[secondName] = std::move(second);
    return json::value(std::move(out));
  }

  ASYNC_TEST("account routing, serial admission, evaluation, native auth and auth seeding match the bun oracle") {
    const rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    const std::string savedB64 = uos::config::config.codexAuthJsonB64;
    const bool savedDeploy = uos::config::config.isDeploy;
    uos::config::config.codexAuthJsonB64.clear();
    uos::config::config.isDeploy = false;
    json::value expectedValue = rt::jsonParse(VECTORS);
    json::object& expected = expectedValue.as_object();
    if (json::value* electStale = rt::get(expected, "electStale"); electStale != nullptr && electStale->is_object()) rt::objectDelete(electStale->as_object(), "quotaResetAtMs");
    json::object out;
    const auto realNow = rt::globalDateNow();
    rt::globalDateNow() = [] { return NOW; };
    const rt::KvKey& ROUTING = routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY;
    const rt::KvKey& ACTIVE = routing_state::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY;
    const rt::KvKey& POOL = auth::CODEX_AUTH_POOL_KV_KEY;

    /* ---------------------------------------------------------------- selection */
    auto kv = std::make_shared<DiffKv>();
    uos::kv::setKvForTest(kv);
    resetAll();
    const CodexAuthState authA{.access_token = jwt(expiring(3600)), .refresh_token = "refresh-a", .account_id = "acct-a", .updated_at_ms = NOW - 10'000};
    const CodexAuthState authB{.access_token = jwt(expiring(7200)), .refresh_token = "refresh-b", .account_id = "acct-b", .updated_at_ms = NOW - 10'000};
    const CodexAuthPoolState pool{.accounts = {authA, authB}, .updated_at_ms = NOW - 5'000};
    auto seedingPool = setRow(kv, POOL, json::value_from(pool));
    co_await std::move(seedingPool);

    auto selecting = account_routing::selectCodexRoutingAccounts(pool, pool.accounts, NOW, std::string("gpt-5"));
    const RouteSelection select = co_await std::move(selecting);
    out["select"] = selectionJson(select);
    auto selectingReordered = account_routing::selectCodexRoutingAccounts(pool, std::vector<CodexAuthState>{authB, authA}, NOW);
    const RouteSelection reordered = co_await std::move(selectingReordered);
    out["selectReordered"] = selectionJson(reordered);
    auto strongSelecting = account_routing::selectCodexRoutingAccountsStrong(pool, pool.accounts, NOW, std::string("gpt-5"));
    const RouteSelection strong = co_await std::move(strongSelecting);
    {
      json::object strongOut;
      strongOut["selection"] = selectionJson(strong);
      strongOut["active"] = plain(kv->row(ACTIVE));
      strongOut["routing"] = plain(kv->row(ROUTING));
      out["strong"] = json::value(std::move(strongOut));
    }
    ASSERT(std::holds_alternative<routing_state::RouteSelectionEligible>(strong));
    const RoutingAccount admitted = std::get<routing_state::RouteSelectionEligible>(strong).accounts.at(0);
    {
      auto refreshing = account_routing::refreshCodexActiveAccountAdmission(admitted);
      const bool current = co_await std::move(refreshing);
      RoutingAccount wrongGeneration = admitted;
      wrongGeneration.activeGeneration = admitted.activeGeneration.value_or(0) + 1;
      auto refreshingWrong = account_routing::refreshCodexActiveAccountAdmission(wrongGeneration);
      const bool wrong = co_await std::move(refreshingWrong);
      RoutingAccount noGeneration = admitted;
      noGeneration.activeGeneration = std::nullopt;
      auto refreshingNone = account_routing::refreshCodexActiveAccountAdmission(noGeneration);
      const bool none = co_await std::move(refreshingNone);
      routing_state::CodexActiveAccountSnapshot snapshot;
      snapshot.selection = routing_state::parseCodexActiveAccountSelection(kv->row(ACTIVE));
      json::object admission;
      admission["current"] = current;
      admission["wrongGeneration"] = wrong;
      admission["noGeneration"] = none;
      admission["snapshotCurrent"] = account_routing::isCodexActiveAccountSnapshotCurrent(kv->row(ACTIVE), snapshot);
      admission["snapshotAbsent"] = account_routing::isCodexActiveAccountSnapshotCurrent(json::value(nullptr), routing_state::CodexActiveAccountSnapshot{});
      out["refreshAdmission"] = json::value(std::move(admission));
    }

    const double resetsAt = rt::Math::floor(NOW / 1000) + 3600;
    auto blockingA = rate_limit_429::markCodexQuotaBlocked(admitted, response429(resetsAt), NOW);
    co_await std::move(blockingA);
    auto strongSelecting2 = account_routing::selectCodexRoutingAccountsStrong(pool, pool.accounts, NOW, std::string("gpt-5"));
    const RouteSelection strong2 = co_await std::move(strongSelecting2);
    out["strongAfterBlockA"] = object2("selection", selectionJson(strong2), "active", plain(kv->row(ACTIVE)));
    ASSERT(std::holds_alternative<routing_state::RouteSelectionEligible>(strong2));
    const RoutingAccount accountB = std::get<routing_state::RouteSelectionEligible>(strong2).accounts.at(0);
    auto blockingB = rate_limit_429::markCodexQuotaBlocked(accountB, response429(resetsAt + 60), NOW);
    co_await std::move(blockingB);
    auto strongSelecting3 = account_routing::selectCodexRoutingAccountsStrong(pool, pool.accounts, NOW, std::string("gpt-5"));
    const RouteSelection strong3 = co_await std::move(strongSelecting3);
    out["strongAllBlocked"] = object2("selection", selectionJson(strong3), "active", plain(kv->row(ACTIVE)));
    auto selectingBlocked = account_routing::selectCodexRoutingAccounts(pool, pool.accounts, NOW, std::string("gpt-5"));
    const RouteSelection selectBlocked = co_await std::move(selectingBlocked);
    out["selectAllBlocked"] = selectionJson(selectBlocked);

    ASSERT(std::holds_alternative<routing_state::RouteSelectionQuotaBlocked>(strong3));
    const routing_state::RouteSelectionQuotaBlocked& blocked = std::get<routing_state::RouteSelectionQuotaBlocked>(strong3);
    const routing_state::CodexBlockedRoutingAccount* candidatePointer = nullptr;
    for (const auto& account : blocked.blockedAccounts)
      if (account.slot == 0) candidatePointer = &account;
    ASSERT(candidatePointer != nullptr);
    const routing_state::CodexBlockedRoutingAccount candidate = *candidatePointer;
    const RoutingAccount candidateAccount = candidate;
    auto electingStale = account_routing::electCodexResetRecoveryAccount(candidateAccount, blocked.activeSnapshot, candidate.quotaResetAtMs, true);
    const std::optional<RoutingAccount> electStale = co_await std::move(electingStale);
    out["electStale"] = routingAccountJson(electStale);
    auto electing = account_routing::electCodexResetRecoveryAccount(candidateAccount, blocked.activeSnapshot, candidate.quotaResetAtMs);
    const std::optional<RoutingAccount> elect = co_await std::move(electing);
    out["elect"] = routingAccountJson(elect);
    out["electRows"] = object2("active", plain(kv->row(ACTIVE)), "routing", plain(kv->row(ROUTING)));
    auto electingAgain = account_routing::electCodexResetRecoveryAccount(candidateAccount, blocked.activeSnapshot, candidate.quotaResetAtMs);
    const std::optional<RoutingAccount> electAgain = co_await std::move(electingAgain);
    out["electAgain"] = routingAccountJson(electAgain);

    {
      json::object recheck;
      auto recheck1 = account_routing::recheckCodexRoutingSlot(1);
      const bool slot1 = co_await std::move(recheck1);
      recheck["slot1"] = slot1;
      recheck["row"] = plain(kv->row(ROUTING));
      auto recheck0 = account_routing::recheckCodexRoutingSlot(0);
      const bool zero = co_await std::move(recheck0);
      recheck["zero"] = zero;
      auto recheck5 = account_routing::recheckCodexRoutingSlot(5);
      const bool five = co_await std::move(recheck5);
      recheck["five"] = five;
      auto recheckFraction = account_routing::recheckCodexRoutingSlot(1.5);
      const bool fraction = co_await std::move(recheckFraction);
      recheck["fraction"] = fraction;
      auto recheck2 = account_routing::recheckCodexRoutingSlot(2);
      const bool slot2 = co_await std::move(recheck2);
      recheck["slot2"] = slot2;
      out["recheck"] = json::value(std::move(recheck));
    }
    out["recheckRow"] = plain(kv->row(ROUTING));

    uos::kv::setKvForTest(nullptr);
    {
      json::object noKv;
      auto strongWithoutKv = account_routing::selectCodexRoutingAccountsStrong(pool, pool.accounts, NOW);
      const RouteSelection unavailable = co_await std::move(strongWithoutKv);
      noKv["strong"] = selectionJson(unavailable);
      auto recheckLocal = account_routing::recheckCodexRoutingSlot(2);
      const bool rechecked = co_await std::move(recheckLocal);
      noKv["recheck"] = rechecked;
      const std::optional<routing_state::CodexAccountRoutingState> cached = routing_state::getCachedRoutingState();
      noKv["cached"] = cached ? plain(json::value(routing_state::toJson(*cached))) : json::value(nullptr);
      auto admittingWithoutKv = account_routing::refreshCodexActiveAccountAdmission(admitted);
      const bool admission = co_await std::move(admittingWithoutKv);
      noKv["admission"] = admission;
      auto electingWithoutKv = account_routing::electCodexResetRecoveryAccount(candidateAccount, blocked.activeSnapshot, candidate.quotaResetAtMs);
      const std::optional<RoutingAccount> electedWithoutKv = co_await std::move(electingWithoutKv);
      noKv["elect"] = routingAccountJson(electedWithoutKv);
      out["noKv"] = json::value(std::move(noKv));
    }
    uos::kv::setKvForTest(kv);

    /* --------------------------------------------------------------- evaluation */
    {
      const json::value poolValue = json::value_from(pool);
      const auto snapshot = [](const json::value& value) { return poolJson(routing_evaluation::parseCodexAuthPoolSnapshot(value)); };
      const auto poolWith = [](std::vector<json::value> accounts, json::value updatedAtMs) {
        json::object value;
        value["accounts"] = json::array(accounts.begin(), accounts.end());
        value["updated_at_ms"] = std::move(updatedAtMs);
        return json::value(std::move(value));
      };
      const json::value a = json::value_from(authA);
      json::value noAccess = a;
      noAccess.as_object()["access_token"] = "";
      json::value accountBadUpdated = a;
      accountBadUpdated.as_object()["updated_at_ms"] = -1;
      json::value third = a;
      third.as_object()["account_id"] = "acct-c";
      json::object snapshots;
      snapshots["valid"] = snapshot(poolValue);
      snapshots["duplicate"] = snapshot(poolWith({a, a}, rt::makeNumber(NOW)));
      snapshots["noAccessToken"] = snapshot(poolWith({noAccess}, rt::makeNumber(NOW)));
      snapshots["badUpdated"] = snapshot(poolWith({a}, json::value(1.5)));
      snapshots["accountBadUpdated"] = snapshot(poolWith({accountBadUpdated}, rt::makeNumber(NOW)));
      snapshots["empty"] = snapshot(poolWith({}, rt::makeNumber(NOW)));
      snapshots["three"] = snapshot(poolWith({a, json::value_from(authB), third}, rt::makeNumber(NOW)));
      snapshots["notRecord"] = snapshot(json::value("pool"));
      out["poolSnapshot"] = json::value(std::move(snapshots));
    }
    {
      routing_state::CodexRouteSelectionEligible eligible;
      routing_state::CodexRoutingAccount stray;
      stray.slot = 0;
      stray.accountIdHash = "x";
      stray.credentialVersion = "y";
      eligible.accounts.push_back(stray);
      try {
        (void)routing_evaluation::hydrateCodexRouteSelection(routing_state::CodexRouteSelection(eligible), {});
        out["hydrateError"] = nullptr;
      } catch (...) {
        out["hydrateError"] = errorJson(std::current_exception());
      }
    }

    /* -------------------------------------------------------------- native auth */
    auto kv2 = std::make_shared<DiffKv>();
    uos::kv::setKvForTest(kv2);
    resetAll();
    const std::string home = "/synthetic/native/.codex";
    const std::string managedToken = jwt(expiring(3600));
    json::object emailClaim;
    emailClaim["email"] = "native@example.com";
    const std::string idToken = jwt(json::value(emailClaim));
    const CodexAuthState authN{.access_token = managedToken, .refresh_token = "native-refresh", .account_id = "acct-n", .updated_at_ms = NOW - 1'000};
    const CodexAuthPoolState nativePool{.accounts = {authN}, .updated_at_ms = NOW - 1'000};
    const auto managedDocument = [](const std::string& accessToken, const std::string& refreshToken, const std::string& id, const std::string& refreshedAt) {
      json::object tokens;
      tokens["access_token"] = accessToken;
      tokens["refresh_token"] = refreshToken;
      tokens["account_id"] = "acct-n";
      tokens["id_token"] = id;
      json::object document;
      document["auth_mode"] = "chatgpt";
      document["tokens"] = std::move(tokens);
      document["last_refresh"] = refreshedAt;
      return json::value(std::move(document));
    };
    auto documentValue = std::make_shared<json::value>(managedDocument(managedToken, "native-refresh", idToken, "2026-11-16T00:00:00.123456789Z"));
    auto refreshCalls = std::make_shared<json::array>();
    const json::value renewedDocument = managedDocument(jwt(expiring(7200)), "native-refresh-2", idToken, "2026-11-16T00:10:00.5Z");
    native_auth::NativeAuthHooks hooks;
    hooks.codexHome = home;
    hooks.getEnv = [](std::string name) -> std::optional<std::string> {
      if (name == "HOME") return std::string("/synthetic/native");
      return std::nullopt;
    };
    hooks.readAuth = [documentValue]() { return rt::fulfilledTask(*documentValue); };
    hooks.refresh = [refreshCalls, documentValue, renewedDocument](std::string refreshHome, std::string email, std::optional<std::string> accountId) {
      json::object call;
      call["home"] = refreshHome;
      call["email"] = email;
      call["accountId"] = stringOrNull(accountId);
      refreshCalls->push_back(json::value(std::move(call)));
      *documentValue = renewedDocument;
      return rt::fulfilledTask();
    };
    native_auth::setNativeCodexAuthHooksForTest(hooks);
    {
      json::array homes;
      for (const std::string& entry : native_auth::nativeCodexConsumerHomes()) homes.push_back(json::value(entry));
      out["homes"] = std::move(homes);
    }
    auto generating = native_auth::nativeCodexCredentialGeneration(authN);
    const std::string generation = co_await std::move(generating);
    out["generation"] = generation;
    auto seedingNative = setRow(kv2, POOL, json::value_from(nativePool));
    co_await std::move(seedingNative);
    auth::CodexAuthPoolEntry nativeInput;
    nativeInput.kv = kv2;
    nativeInput.entry = kv2->entryOf(POOL).as<json::value>();
    nativeInput.pool = nativePool;
    auto binding = native_auth::bindNativeCodexAuthPool(nativeInput);
    const auth::CodexAuthPoolEntry bound = co_await std::move(binding);
    {
      json::object bind;
      bind["pool"] = poolJson(bound.pool);
      bind["versionstamp"] = bound.entry ? stringOrNull(bound.entry->versionstamp) : json::value(nullptr);
      bind["row"] = plain(kv2->row(POOL));
      out["bind"] = json::value(std::move(bind));
    }
    auth::CodexAuthPoolEntry detached;
    detached.kv = nullptr;
    detached.entry = std::nullopt;
    detached.pool = nativePool;
    auto bindingWithoutKv = native_auth::bindNativeCodexAuthPool(detached);
    const auth::CodexAuthPoolEntry unbound = co_await std::move(bindingWithoutKv);
    out["bindWithoutKv"] = poolJson(unbound.pool);
    auth::resetCodexAuthCacheForTest();
    auto readingEntry = auth::getAuthPoolEntry(true);
    const auth::CodexAuthPoolEntry authEntry = co_await std::move(readingEntry);
    out["authEntry"] = poolJson(authEntry.pool);
    const CodexAuthState owned = bound.pool.accounts.at(0);
    auto refreshingOwned = native_auth::refreshNativeCodexAuthIfOwned(owned);
    const std::optional<CodexAuthState> refreshedOwned = co_await std::move(refreshingOwned);
    {
      json::object refreshOut;
      refreshOut["auth"] = refreshedOwned ? plain(json::value_from(*refreshedOwned)) : json::value(nullptr);
      refreshOut["calls"] = *refreshCalls;
      refreshOut["row"] = plain(kv2->row(POOL));
      out["refreshOwned"] = json::value(std::move(refreshOut));
    }
    {
      CodexAuthState stale = owned;
      stale.access_token = "stale";
      stale.refresh_token = "stale";
      rt::Reason failure;
      try {
        auto refusing = native_auth::refreshNativeCodexAuthIfOwned(stale);
        co_await std::move(refusing);
      } catch (...) {
        failure = std::current_exception();
      }
      out["staleRefused"] = failure ? errorJson(failure) : json::value(nullptr);
    }
    const CodexAuthState unrelatedAuth{.access_token = "x", .refresh_token = "y", .account_id = "other", .updated_at_ms = NOW};
    auto refreshingUnrelated = native_auth::refreshNativeCodexAuthIfOwned(unrelatedAuth);
    const std::optional<CodexAuthState> unrelated = co_await std::move(refreshingUnrelated);
    out["unrelated"] = unrelated ? plain(json::value_from(*unrelated)) : json::value(nullptr);

    const auto externalToken = [](double secondsFromNow) {
      json::object claims;
      claims["chatgpt_user_id"] = "user-1";
      claims["chatgpt_account_id"] = "acct-x";
      json::object payload;
      payload["exp"] = rt::makeNumber(NOW / 1000 + secondsFromNow);
      payload["https://api.openai.com/auth"] = std::move(claims);
      return jwt(json::value(std::move(payload)));
    };
    const std::string extToken1 = externalToken(3600);
    const std::string extToken2 = externalToken(7200);
    const auto extDocument = [](const std::string& token, const std::string& refreshedAt) {
      json::object tokens;
      tokens["access_token"] = token;
      tokens["account_id"] = "acct-x";
      tokens["id_token"] = token;
      tokens["refresh_token"] = "";
      json::object document;
      document["auth_mode"] = "chatgptAuthTokens";
      document["OPENAI_API_KEY"] = nullptr;
      document["tokens"] = std::move(tokens);
      document["last_refresh"] = refreshedAt;
      return json::value(std::move(document));
    };
    const CodexAuthState previousExternal{.access_token = extToken1, .refresh_token = "", .account_id = "acct-x", .updated_at_ms = NOW - 1'000};
    auto generatingExternal = native_auth::nativeCodexCredentialGeneration(previousExternal);
    const std::string externalGeneration = co_await std::move(generatingExternal);
    CodexAuthState ownedExternal = previousExternal;
    ownedExternal.native_owner = CodexAuthState::NativeOwner{.codex_home = home,
                                                              .generation_hash = externalGeneration,
                                                              .native_refreshed_at = std::string("2026-11-16T00:00:00Z"),
                                                              .auth_mode = std::string("chatgptAuthTokens"),
                                                              .chatgpt_user_id = std::string("user-1")};
    const CodexAuthPoolState externalPool{.accounts = {ownedExternal}, .updated_at_ms = NOW - 1'000};
    auto planning = attemptPool(native_auth::planExternalNativeCodexAdoption(externalPool, extDocument(extToken2, "2026-11-16T00:30:00Z"), home, NOW, false));
    out["plan"] = co_await std::move(planning);
    auto planningSame = attemptPool(native_auth::planExternalNativeCodexAdoption(externalPool, extDocument(extToken1, "2026-11-16T00:00:00Z"), home, NOW, false));
    out["planSame"] = co_await std::move(planningSame);
    auto planningManaged = attemptPool(native_auth::planExternalNativeCodexAdoption(externalPool, *documentValue, home, NOW, false));
    out["planManagedDocument"] = co_await std::move(planningManaged);
    auto planningElsewhere = attemptPool(native_auth::planExternalNativeCodexAdoption(externalPool, extDocument(extToken2, "2026-11-16T00:30:00Z"), "/elsewhere", NOW, false));
    out["planWrongHome"] = co_await std::move(planningElsewhere);
    auto planningIntoManaged = attemptPool(native_auth::planExternalNativeCodexAdoption(bound.pool, extDocument(extToken2, "2026-11-16T00:30:00Z"), home, NOW, true));
    out["planIntoManaged"] = co_await std::move(planningIntoManaged);
    native_auth::setNativeCodexAuthHooksForTest(std::nullopt);

    /* ------------------------------------------------------------- auth seeding */
    auto kv3 = std::make_shared<DiffKv>();
    uos::kv::setKvForTest(kv3);
    resetAll();
    native_auth::NativeAuthHooks windows;
    windows.os = std::string("win32");
    native_auth::setNativeCodexAuthHooksForTest(windows);
    json::object seedTokens;
    seedTokens["access_token"] = "seed-access";
    seedTokens["refresh_token"] = "seed-refresh";
    seedTokens["account_id"] = "seed";
    json::object seedDocument;
    seedDocument["tokens"] = std::move(seedTokens);
    uos::config::config.codexAuthJsonB64 = rt::btoa(rt::jsonStringify(json::value(seedDocument)));
    auto seeding = auth::getAuthPoolEntry(true);
    const auth::CodexAuthPoolEntry seeded = co_await std::move(seeding);
    {
      json::object seedOut;
      seedOut["pool"] = poolJson(seeded.pool);
      seedOut["versionstamp"] = seeded.entry ? stringOrNull(seeded.entry->versionstamp) : json::value(nullptr);
      seedOut["row"] = plain(kv3->row(POOL));
      out["seed"] = json::value(std::move(seedOut));
    }
    auth::resetCodexAuthCacheForTest();
    auto deleting = kv3->delete_(POOL);
    co_await std::move(deleting);
    uos::config::config.codexAuthJsonB64 = "%%% not base64";
    auto seedingInvalid = attemptEntry(auth::getAuthPoolEntry(true));
    out["seedInvalid"] = co_await std::move(seedingInvalid);
    uos::config::config.codexAuthJsonB64 = rt::btoa("{not json");
    auth::resetCodexAuthCacheForTest();
    auto seedingNotJson = attemptEntry(auth::getAuthPoolEntry(true));
    out["seedNotJson"] = co_await std::move(seedingNotJson);
    native_auth::setNativeCodexAuthHooksForTest(std::nullopt);
    uos::kv::setKvForTest(nullptr);
    resetAll();

    for (const auto& member : expected) EXPECT(sameVector(expected, out, member.key()));
    EXPECT_EQ(out.size(), expected.size());

    uos::config::config.codexAuthJsonB64 = savedB64;
    uos::config::config.isDeploy = savedDeploy;
    rt::globalDateNow() = realNow;
  }
}
