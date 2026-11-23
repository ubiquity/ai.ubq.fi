// Differential cases for src/codex/routing-state.ts. Every bun test that imports the module drives it
// through unported modules (manual reset, capacity routing, routing mutations, auth, the handler), so
// the expected values here were recorded from the module itself under Bun 1.4.0 (scratch oracle script).
#include "uos/codex/routing_state.hpp"

#include "uos/authority/native_codex_contracts.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test.hpp"
#include "uos/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace routing_state = uos::codex::routing_state;
namespace contracts = uos::authority::native_codex_contracts;
using routing_state::CodexAccountRoutingState;
using routing_state::CodexRoutingAccount;
using routing_state::RoutingAccount;

namespace {
  const double now = 1'700'000'000'000;
  const std::string A(64, 'a');
  const std::string B(64, 'b');
  const std::string C(64, 'c');
  const std::string D(64, 'd');

  // `JSON.stringify(routingStateCache)`
  std::string cacheJson() {
    json::object object;
    rt::objectDefine(object, "state", routing_state::routingStateCache.state ? json::value(routing_state::toJson(*routing_state::routingStateCache.state)) : json::value(nullptr));
    rt::objectDefine(object, "versionstamp",
                     routing_state::routingStateCache.versionstamp ? json::value(json::string(*routing_state::routingStateCache.versionstamp)) : json::value(nullptr));
    rt::objectDefine(object, "loadedAtMs", rt::makeNumber(routing_state::routingStateCache.loadedAtMs));
    return rt::jsonStringify(object);
  }

  // `JSON.stringify(account)` for a CodexRoutingAccount: `T | null` members print null, absent `?:` members are omitted.
  std::string accountJson(const CodexRoutingAccount& account) {
    json::object object;
    rt::objectDefine(object, "slot", rt::makeNumber(account.slot));
    rt::objectDefine(object, "accountIdHash", json::value(json::string(account.accountIdHash)));
    rt::objectDefine(object, "credentialVersion", json::value(json::string(account.credentialVersion)));
    rt::objectDefine(object, "quotaHeadroom", account.quotaHeadroom ? rt::makeNumber(*account.quotaHeadroom) : json::value(nullptr));
    rt::objectDefine(object, "probeRequired", account.probeRequired);
    rt::objectDefine(object, "probeGeneration", account.probeGeneration ? rt::makeNumber(*account.probeGeneration) : json::value(nullptr));
    rt::objectDefine(object, "probeToken", account.probeToken ? json::value(json::string(*account.probeToken)) : json::value(nullptr));
    rt::objectDefine(object, "probeCircuit", account.probeCircuit);
    if (account.overageFallback) rt::objectDefine(object, "overageFallback", *account.overageFallback);
    if (account.routingGeneration) rt::objectDefine(object, "routingGeneration", rt::makeNumber(*account.routingGeneration));
    if (account.activeGeneration) rt::objectDefine(object, "activeGeneration", rt::makeNumber(*account.activeGeneration));
    if (account.activePoolVersionstamp) rt::objectDefine(object, "activePoolVersionstamp", json::value(json::string(*account.activePoolVersionstamp)));
    rt::objectDefine(object, "activeTransitionReason", account.activeTransitionReason);
    rt::objectDefine(object, "requestedModel", account.requestedModel);
    return rt::jsonStringify(object);
  }

  uos::types::CodexAuthState authState() {
    uos::types::CodexAuthState auth{
      .access_token = "access-secret",
      .refresh_token = "refresh-secret",
      .account_id = "acct-1",
      .updated_at_ms = now,
      .native_owner = uos::types::CodexAuthState::NativeOwner{.codex_home = "/h", .generation_hash = "g"},
    };
    return auth;
  }

  RoutingAccount fullAccount() {
    CodexRoutingAccount metadata;
    metadata.slot = 0;
    metadata.accountIdHash = A;
    metadata.credentialVersion = B;
    metadata.quotaHeadroom = 12.5;
    metadata.probeRequired = true;
    metadata.probeGeneration = 3;
    metadata.probeToken = "tok";
    metadata.probeCircuit = std::string("quota");
    metadata.overageFallback = true;
    metadata.routingGeneration = 4;
    metadata.activeGeneration = 2;
    metadata.activePoolVersionstamp = "P/opaque:11";
    metadata.activeTransitionReason = std::string("manual_reset");
    metadata.requestedModel = std::string("gpt-test");
    return RoutingAccount{metadata, authState()};
  }
  RoutingAccount minimalAccount() {
    CodexRoutingAccount metadata;
    metadata.slot = 1;
    metadata.accountIdHash = C;
    metadata.credentialVersion = D;
    metadata.quotaHeadroom = std::nullopt;
    metadata.probeRequired = false;
    metadata.probeGeneration = std::nullopt;
    metadata.probeToken = std::nullopt;
    return RoutingAccount{metadata, authState()};
  }
  RoutingAccount nullsAccount() {
    CodexRoutingAccount metadata;
    metadata.slot = 0;
    metadata.accountIdHash = A;
    metadata.credentialVersion = B;
    metadata.probeRequired = false;
    metadata.probeCircuit = nullptr;
    metadata.activeTransitionReason = nullptr;
    metadata.requestedModel = nullptr;
    return RoutingAccount{metadata, authState()};
  }
}

// Registered first: the cache is module state and starts as `{ state: null, versionstamp: null, loadedAtMs: 0 }`.
TEST("routing-state cache starts empty, stamps Date.now(), maps an absent versionstamp to null and stays directly writable") {
  EXPECT_EQ(cacheJson(), R"({"state":null,"versionstamp":null,"loadedAtMs":0})");
  EXPECT(!routing_state::getCachedRoutingState().has_value());
  const auto realNow = rt::globalDateNow();
  double clock = 1234;
  rt::globalDateNow() = [&clock] { return clock; };
  const std::optional<CodexAccountRoutingState> state =
    routing_state::parseCodexAccountRoutingState(rt::jsonParse(R"({"v":2,"updated_at_ms":1700000000000,"banked_reset_legacy_identity_unresolved":false,"slots":[]})"));
  ASSERT(state.has_value());
  routing_state::setRoutingStateCache(state, "vs-1");
  EXPECT_EQ(cacheJson(), R"({"state":{"v":2,"updated_at_ms":1700000000000,"banked_reset_legacy_identity_unresolved":false,"slots":[]},"versionstamp":"vs-1","loadedAtMs":1234})");
  // `getCachedRoutingState() === state`: the C++ cache holds a copy, so the check is value equality.
  const std::optional<CodexAccountRoutingState> cached = routing_state::getCachedRoutingState();
  ASSERT(cached.has_value());
  EXPECT_EQ(rt::jsonStringify(routing_state::toJson(*cached)), rt::jsonStringify(routing_state::toJson(*state)));
  clock = 5678;
  routing_state::setRoutingStateCache(state);
  EXPECT_EQ(cacheJson(), R"({"state":{"v":2,"updated_at_ms":1700000000000,"banked_reset_legacy_identity_unresolved":false,"slots":[]},"versionstamp":null,"loadedAtMs":5678})");
  clock = 9999;
  routing_state::setRoutingStateCache(std::nullopt, std::nullopt);
  EXPECT_EQ(cacheJson(), R"({"state":null,"versionstamp":null,"loadedAtMs":9999})");
  EXPECT(!routing_state::getCachedRoutingState().has_value());
  routing_state::setRoutingStateCache(state, std::nullopt);
  EXPECT_EQ(cacheJson(), R"({"state":{"v":2,"updated_at_ms":1700000000000,"banked_reset_legacy_identity_unresolved":false,"slots":[]},"versionstamp":null,"loadedAtMs":9999})");
  routing_state::routingStateCache.versionstamp = "direct";
  routing_state::routingStateCache.loadedAtMs = 7;
  EXPECT_EQ(cacheJson(), R"({"state":{"v":2,"updated_at_ms":1700000000000,"banked_reset_legacy_identity_unresolved":false,"slots":[]},"versionstamp":"direct","loadedAtMs":7})");
  rt::globalDateNow() = realNow;
  routing_state::setRoutingStateCache(std::nullopt);
  routing_state::routingStateCache.loadedAtMs = 0;
}

TEST("nativeCodexAccountMetadata keeps every routing member and drops only the credential") {
  EXPECT_EQ(accountJson(routing_state::nativeCodexAccountMetadata(fullAccount())),
            R"({"slot":0,"accountIdHash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credentialVersion":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quotaHeadroom":12.5,"probeRequired":true,"probeGeneration":3,"probeToken":"tok","probeCircuit":"quota","overageFallback":true,"routingGeneration":4,"activeGeneration":2,"activePoolVersionstamp":"P/opaque:11","activeTransitionReason":"manual_reset","requestedModel":"gpt-test"})");
  EXPECT_EQ(accountJson(routing_state::nativeCodexAccountMetadata(minimalAccount())),
            R"({"slot":1,"accountIdHash":"cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc","credentialVersion":"dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd","quotaHeadroom":null,"probeRequired":false,"probeGeneration":null,"probeToken":null})");
  EXPECT_EQ(accountJson(routing_state::nativeCodexAccountMetadata(nullsAccount())),
            R"({"slot":0,"accountIdHash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credentialVersion":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quotaHeadroom":null,"probeRequired":false,"probeGeneration":null,"probeToken":null,"probeCircuit":null,"activeTransitionReason":null,"requestedModel":null})");
  // A CodexBlockedRoutingAccount projects to its CodexRoutingAccount members. The TS copy also keeps the
  // extra `quotaResetAtMs` member, which every caller sets again on the projection it builds.
  routing_state::CodexBlockedRoutingAccount blocked{fullAccount(), now + 60'000};
  blocked.routingGeneration = 3;
  EXPECT_EQ(accountJson(routing_state::nativeCodexAccountMetadata(blocked)),
            R"({"slot":0,"accountIdHash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credentialVersion":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","quotaHeadroom":12.5,"probeRequired":true,"probeGeneration":3,"probeToken":"tok","probeCircuit":"quota","overageFallback":true,"routingGeneration":3,"activeGeneration":2,"activePoolVersionstamp":"P/opaque:11","activeTransitionReason":"manual_reset","requestedModel":"gpt-test"})");
}

TEST("isCodexActiveAccountSelectionCurrent binds the exact active row to the projected account") {
  const json::value active = rt::jsonParse(
    R"({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","pool_versionstamp":"P/opaque:11","slot":0,"routing_generation":4,"generation":2,"transition_reason":null,"updated_at_ms":1700000000000})");
  const auto variant = [&active](std::string_view key, json::value value) {
    json::value row = active;
    rt::objectSet(row.as_object(), key, std::move(value));
    return row;
  };
  const std::vector<json::value> raws = {
    active,
    variant("generation", rt::makeNumber(3.0)),
    variant("pool_versionstamp", json::value("P/other")),
    variant("routing_generation", rt::makeNumber(3.0)),
    variant("slot", rt::makeNumber(1.0)),
    variant("credential_version", json::value(json::string(D))),
    variant("account_id_hash", json::value(json::string(C))),
    variant("generation", json::value("x")),
    json::value(nullptr),
  };
  RoutingAccount withoutPool = fullAccount();
  withoutPool.activePoolVersionstamp = std::nullopt;
  RoutingAccount withoutRouting = fullAccount();
  withoutRouting.routingGeneration = std::nullopt;
  const std::vector<RoutingAccount> accounts = {fullAccount(), minimalAccount(), nullsAccount(), withoutPool, withoutRouting};
  const auto grid = [&accounts](const json::value* raw) {
    json::array rows;
    for (const RoutingAccount& account : accounts) {
      json::array row;
      for (const double generation : {1.0, 2.0, 3.0}) row.push_back(routing_state::isCodexActiveAccountSelectionCurrent(raw, account, generation));
      rows.push_back(std::move(row));
    }
    return rt::jsonStringify(rows);
  };
  const std::string none = "[[false,false,false],[false,false,false],[false,false,false],[false,false,false],[false,false,false]]";
  const std::vector<std::string> expected = {
    "[[false,true,false],[false,false,false],[false,false,false],[false,false,false],[false,false,false]]",
    "[[false,false,true],[false,false,false],[false,false,false],[false,false,false],[false,false,false]]",
    none, none, none, none, none, none, none,
  };
  for (std::size_t r = 0; r < raws.size(); ++r) EXPECT_EQ(grid(&raws[r]), expected[r]);
  EXPECT_EQ(grid(nullptr), none);
  EXPECT_EQ(routing_state::isCodexActiveAccountSelectionCurrent(raws[0], fullAccount(), 2), true);
}

TEST("routing-state re-exports the native codex contracts") {
  json::array keys;
  for (const rt::KvKey* key : {&routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY, &routing_state::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, &routing_state::CODEX_AUTH_POOL_KV_KEY,
                               &routing_state::CODEX_CAPACITY_ROUTING_OBSERVATION_KV_KEY}) {
    json::array parts;
    for (const rt::KvKeyPart& part : *key) parts.push_back(json::value(json::string(part.string())));
    keys.push_back(std::move(parts));
  }
  keys.push_back(rt::makeNumber(routing_state::CODEX_HALF_OPEN_LEASE_MS));
  keys.push_back(rt::makeNumber(routing_state::CODEX_UPSTREAM_TIMEOUT_CIRCUIT_MS));
  keys.push_back(rt::makeNumber(routing_state::CODEX_CAPACITY_ROUTING_MAX_AGE_MS));
  keys.push_back(rt::makeNumber(routing_state::ROUTING_CACHE_REVALIDATE_MS));
  EXPECT_EQ(rt::jsonStringify(keys),
            R"([["uos_ai","codex_account_routing","v2"],["uos_ai","codex_active_account","v1"],["ubq_ai","codex_auth"],["uos_ai","codex_capacity_routing","v1","observations"],30000,60000,1800000,5000])");
  EXPECT(&routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY == &contracts::CODEX_ACCOUNT_ROUTING_KV_KEY);
  const json::value row = rt::jsonParse(
    R"({"v":1,"account_id_hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","credential_version":"bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb","pool_versionstamp":"P/opaque:11","slot":0,"routing_generation":4,"generation":2,"transition_reason":null,"updated_at_ms":1700000000000})");
  EXPECT(routing_state::parseCodexActiveAccountSelection(row) == contracts::parseCodexActiveAccountSelection(row));
  EXPECT_EQ(routing_state::isSafeMs(rt::makeNumber(5.0)), true);
  EXPECT_EQ(routing_state::isVersionstamp(&row), false);
  EXPECT_EQ(routing_state::hasUnresolvedLegacyResetIdentity(nullptr), false);
  EXPECT(!routing_state::parseCodexAccountRoutingState(row).has_value());
  EXPECT(!routing_state::parseOptionalSafeMs(&row).has_value());
  const RoutingAccount account = fullAccount();
  EXPECT_EQ(routing_state::codexActiveAccountSelectionCurrent(row, routing_state::nativeCodexAccountMetadata(account), 2), true);
  routing_state::CodexRoutingSlot slot;
  slot.observed_reset_at_ms = 5;
  slot.observed_reset_at_is_stable = true;
  EXPECT_EQ(routing_state::isLegacyStableResetIdentity(slot), true);
}

TEST("RouteSelection has the six TS members in order, the account-carrying ones with credentials") {
  routing_state::RouteSelection selection = routing_state::RouteSelectionEligible{{fullAccount()}, {1}, {}};
  const auto kind = [](const routing_state::RouteSelection& value) { return std::visit([](const auto& member) { return std::string(member.kind); }, value); };
  EXPECT_EQ(kind(selection), "eligible");
  EXPECT_EQ(std::get<routing_state::RouteSelectionEligible>(selection).accounts[0].auth.account_id, "acct-1");
  selection = routing_state::RouteSelectionQuotaBlocked{
    .skippedSlots = {0, 1},
    .retryAtMs = now,
    .blockedAccounts = {routing_state::CodexBlockedRoutingAccount{fullAccount(), now + 60'000}},
    .fullCohortExhausted = true,
    .fullCohortUnavailable = true,
    .activeSnapshot = {},
    .poolSnapshotJson = std::nullopt,
    .capacitySnapshotJson = std::nullopt,
  };
  EXPECT_EQ(kind(selection), "quota_blocked");
  EXPECT_EQ(std::get<routing_state::RouteSelectionQuotaBlocked>(selection).blockedAccounts[0].quotaResetAtMs, now + 60'000);
  selection = routing_state::RouteSelectionUpstreamBlocked{};
  EXPECT_EQ(kind(selection), "upstream_blocked");
  selection = routing_state::RouteSelectionModelUnavailable{};
  EXPECT_EQ(kind(selection), "model_unavailable");
  selection = routing_state::RouteSelectionCredentialsInvalid{};
  EXPECT_EQ(kind(selection), "credentials_invalid");
  selection = routing_state::RouteSelectionRoutingUnavailable{};
  EXPECT_EQ(kind(selection), "routing_unavailable");
  EXPECT_EQ(selection.index(), std::size_t{5});
}
