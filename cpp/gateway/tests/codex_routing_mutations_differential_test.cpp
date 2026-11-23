// Differential cases for src/codex/rate-limit-429.ts, src/codex/routing-mutations.ts,
// src/codex/overage-settings.ts and the capacity-routing transitions tests/codex-capacity-routing-coverage.test.ts
// leaves out (their direct bun suites need modules of later sub-chunks). The vectors were recorded by running the
// TypeScript modules under bun 1.4.0 over the same deterministic KV (tests/helpers/codex_diff_kv) with a fixed
// clock; random probe tokens are masked. The scenario below mirrors the generator step for step.
// Links: tests/codex_routing_mutations_differential_test.cpp tests/helpers/codex_diff_kv.cpp src/codex/capacity_routing.cpp
//        src/codex/rate_limit_429.cpp src/codex/routing_mutations.cpp src/codex/overage_settings.cpp src/codex/routing_state.cpp
//        src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp
//        src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/bounded_response_body.cpp src/kv.cpp
//        src/storage/kv.cpp src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp src/storage/value_compatibility.cpp
// src/storage/key_compatibility.cpp src/utils.cpp src/types.cpp
#include "tests/helpers/codex_diff_kv.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/codex/rate_limit_429.hpp"
#include "uos/codex/routing_mutations.hpp"

#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/utils.hpp"

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace capacity_routing = uos::codex::capacity_routing;
  namespace rate_limit_429 = uos::codex::rate_limit_429;
  namespace routing_mutations = uos::codex::routing_mutations;
  namespace overage_settings = uos::codex::overage_settings;
  namespace routing_state = uos::codex::routing_state;
  using uos::tests::helpers::codex_diff_kv::DiffKv;
  using uos::tests::helpers::codex_diff_kv::plain;
  using uos::tests::helpers::codex_diff_kv::routingAccountJson;
  using uos::tests::helpers::codex_diff_kv::sameVector;
  using routing_state::RoutingAccount;
  using uos::types::CodexAuthPoolState;
  using uos::types::CodexAuthState;

  const char* const VECTORS =
#include "codex_routing_mutations_differential_vectors.inc"
    ;

  constexpr double NOW = 1'790'000'000'000;

  rt::Task<RoutingAccount> routingFor(CodexAuthState auth, double slot) {
    auto identifying = capacity_routing::routingAccountIdentity(auth);
    const capacity_routing::CodexRoutingAccountIdentity identity = co_await std::move(identifying);
    RoutingAccount account;
    account.auth = auth;
    account.slot = slot;
    account.accountIdHash = identity.accountIdHash;
    account.credentialVersion = identity.credentialVersion;
    account.quotaHeadroom = std::nullopt;
    account.probeRequired = false;
    account.probeGeneration = std::nullopt;
    account.probeToken = std::nullopt;
    co_return account;
  }

  rt::Response response429(const std::string& body, std::optional<std::string> retryAfter = std::nullopt) {
    rt::Headers headers{{"content-type", "application/json"}};
    if (retryAfter) headers.set("retry-after", *retryAfter);
    rt::ResponseInit init;
    init.status = 429;
    init.headers = headers;
    return rt::Response(rt::BodyInit(body), init);
  }

  rt::Task<json::value> classified(rate_limit_429::Codex429Classification parsed) {
    auto reading = parsed.response.text();
    const std::string body = co_await std::move(reading);
    json::object out;
    out["usageLimitReached"] = parsed.usageLimitReached;
    out["retryAtMs"] = parsed.retryAtMs ? rt::makeNumber(*parsed.retryAtMs) : json::value(nullptr);
    out["quotaBlockSource"] = parsed.quotaBlockSource ? json::value(*parsed.quotaBlockSource) : json::value(nullptr);
    out["resetDeadlineIsStable"] = parsed.resetDeadlineIsStable;
    out["resetDeadlineConflict"] = parsed.resetDeadlineConflict;
    out["status"] = parsed.response.status();
    out["body"] = body;
    co_return json::value(std::move(out));
  }

  json::value pair(std::string_view firstName, json::value first, std::string_view secondName, json::value second) {
    json::object out;
    out[firstName] = std::move(first);
    out[secondName] = std::move(second);
    return json::value(std::move(out));
  }

  json::value numberOrNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }

  routing_mutations::CodexQuotaResetReconciliationFence okFence() {
    return routing_mutations::CodexQuotaResetReconciliationFence{rt::KvKey{"uos_ai", "fence", "one"}, [](const json::value& value) {
                                                                   const json::value* ok = rt::get(value, "ok");
                                                                   return ok != nullptr && ok->is_bool() && ok->as_bool();
                                                                 }};
  }

  rt::Task<void> setRow(std::shared_ptr<DiffKv> kv, rt::KvKey key, json::value value) {
    const rt::KvValue stored(value);
    auto setting = kv->set(std::move(key), stored);
    co_await std::move(setting);
  }

  ASYNC_TEST("rate-limit-429, routing-mutations and overage-settings match the bun oracle") {
    const json::value expectedValue = rt::jsonParse(VECTORS);
    const json::object& expected = expectedValue.as_object();
    json::object out;
    double clock = NOW;
    const auto realNow = rt::globalDateNow();
    rt::globalDateNow() = [&clock] { return clock; };
    auto kv = std::make_shared<DiffKv>();
    uos::kv::setKvForTest(kv);
    capacity_routing::resetCodexAccountRoutingForTest();
    overage_settings::resetOverageUsageCacheForTest();
    const rt::KvKey& ROUTING = routing_state::CODEX_ACCOUNT_ROUTING_KV_KEY;

    const CodexAuthState authA{.access_token = "access-a", .refresh_token = "refresh-a", .account_id = "acct-a", .updated_at_ms = NOW - 10'000};
    const CodexAuthState authB{.access_token = "access-b", .refresh_token = "refresh-b", .account_id = "acct-b", .updated_at_ms = NOW - 10'000};
    const CodexAuthPoolState pool{.accounts = {authA, authB}, .updated_at_ms = NOW - 5'000};

    auto loading = capacity_routing::loadCodexAccountRouting(pool);
    const routing_state::CodexAccountRoutingState loaded = co_await std::move(loading);
    out["load"] = plain(json::value(routing_state::toJson(loaded)));
    auto routingA = routingFor(authA, 0);
    RoutingAccount a = co_await std::move(routingA);
    a.requestedModel = std::string("gpt-5");
    auto routingB = routingFor(authB, 1);
    const RoutingAccount b = co_await std::move(routingB);

    const double resetsAt = rt::Math::floor(NOW / 1000) + 3600;
    auto blocking = rate_limit_429::markCodexQuotaBlocked(
      a, response429(R"({"error":{"type":"usage_limit_reached","resets_at":)" + rt::numberToString(resetsAt) + "}}"), NOW);
    const rate_limit_429::Codex429Classification blocked = co_await std::move(blocking);
    auto classifyingBlocked = classified(blocked);
    json::value blockedJson = co_await std::move(classifyingBlocked);
    out["block"] = pair("classification", std::move(blockedJson), "row", plain(kv->row(ROUTING)));

    auto fencing = routing_mutations::getCodexQuotaBlockFence(a, *blocked.retryAtMs);
    const std::optional<double> fence = co_await std::move(fencing);
    auto fencingWrong = routing_mutations::getCodexQuotaBlockFence(a, *blocked.retryAtMs + 1);
    const std::optional<double> wrong = co_await std::move(fencingWrong);
    auto fencingUnsafe = routing_mutations::getCodexQuotaBlockFence(a, -1);
    const std::optional<double> unsafe = co_await std::move(fencingUnsafe);
    {
      json::object fenceOut;
      fenceOut["fence"] = numberOrNull(fence);
      fenceOut["wrong"] = numberOrNull(wrong);
      fenceOut["unsafe"] = numberOrNull(unsafe);
      fenceOut["current"] = routing_mutations::isCodexQuotaBlockFenceCurrent(kv->row(ROUTING), a, *blocked.retryAtMs, fence.value_or(-1));
      fenceOut["stale"] = routing_mutations::isCodexQuotaBlockFenceCurrent(kv->row(ROUTING), a, *blocked.retryAtMs, fence.value_or(-1) + 1);
      out["fence"] = json::value(std::move(fenceOut));
    }

    auto recovering = routing_mutations::markCodexRecoveryProbeQuotaBlocked(b, response429(R"({"error":{"type":"usage_limit_reached"}})", "120"), NOW);
    const rate_limit_429::Codex429Classification recovery = co_await std::move(recovering);
    auto classifyingRecovery = classified(recovery);
    json::value recoveryJson = co_await std::move(classifyingRecovery);
    out["recovery"] = pair("classification", std::move(recoveryJson), "row", plain(kv->row(ROUTING)));

    auto nonBlocking = rate_limit_429::markCodexQuotaBlockedWithMode(a, response429(R"({"error":{"type":"rate_limit_exceeded"}})"), NOW, false);
    const rate_limit_429::Codex429Classification passed = co_await std::move(nonBlocking);
    auto classifyingPassed = classified(passed);
    json::value passedJson = co_await std::move(classifyingPassed);
    out["nonBlocking"] = pair("classification", std::move(passedJson), "row", plain(kv->row(ROUTING)));

    json::object notOk;
    notOk["ok"] = false;
    auto refusing = setRow(kv, rt::KvKey{"uos_ai", "fence", "one"}, json::value(notOk));
    co_await std::move(refusing);
    routing_mutations::CodexQuotaResetReconciliationInput resetInput{*blocked.retryAtMs, fence.value_or(-1)};
    resetInput.fences = std::vector<routing_mutations::CodexQuotaResetReconciliationFence>{okFence()};
    auto refusedReconciling = routing_mutations::reconcileCodexQuotaAfterVerifiedReset(a, resetInput);
    const std::optional<RoutingAccount> refused = co_await std::move(refusedReconciling);
    json::object isOk;
    isOk["ok"] = true;
    auto accepting = setRow(kv, rt::KvKey{"uos_ai", "fence", "one"}, json::value(isOk));
    co_await std::move(accepting);
    auto verifying = routing_mutations::reconcileCodexQuotaAfterVerifiedReset(a, resetInput);
    const std::optional<RoutingAccount> verified = co_await std::move(verifying);
    {
      json::object verifiedOut;
      verifiedOut["refused"] = routingAccountJson(refused);
      verifiedOut["account"] = routingAccountJson(verified);
      verifiedOut["row"] = plain(kv->row(ROUTING));
      out["verified"] = json::value(std::move(verifiedOut));
    }

    const double generationAfter = *rt::getNumber(kv->row(ROUTING).at("slots").at(0), "generation");
    clock = NOW + 40'000;
    routing_mutations::CodexQuotaResetReconciliationInput staleInput{*blocked.retryAtMs, generationAfter};
    auto staleReconciling = routing_mutations::reconcileCodexQuotaAfterStaleVerifiedReset(a, staleInput);
    const std::optional<RoutingAccount> stale = co_await std::move(staleReconciling);
    routing_mutations::CodexQuotaResetReconciliationInput staleWrongInput{*blocked.retryAtMs + 5, generationAfter};
    auto staleWrongReconciling = routing_mutations::reconcileCodexQuotaAfterStaleVerifiedReset(a, staleWrongInput);
    const std::optional<RoutingAccount> staleWrong = co_await std::move(staleWrongReconciling);
    {
      json::object staleOut;
      staleOut["account"] = routingAccountJson(stale);
      staleOut["wrong"] = routingAccountJson(staleWrong);
      staleOut["row"] = plain(kv->row(ROUTING));
      out["staleVerified"] = json::value(std::move(staleOut));
    }

    if (verified) {
      auto succeeding = routing_mutations::markCodexSuccess(*verified);
      co_await std::move(succeeding);
    }
    out["verifiedSuccess"] = plain(kv->row(ROUTING));

    clock = NOW + 200'000;
    RoutingAccount probeB = b;
    probeB.probeRequired = true;
    auto claiming = routing_mutations::claimCodexRoutingProbe(pool, probeB, clock);
    const std::optional<RoutingAccount> claimed = co_await std::move(claiming);
    out["claim"] = pair("account", routingAccountJson(claimed), "row", plain(kv->row(ROUTING)));
    auto claimingNotRequired = routing_mutations::claimCodexRoutingProbe(pool, b, clock);
    const std::optional<RoutingAccount> notRequired = co_await std::move(claimingNotRequired);
    out["claimNotRequired"] = routingAccountJson(notRequired);

    ASSERT(claimed.has_value());
    auto releasingByResponse = rate_limit_429::markCodexQuotaBlocked(*claimed, response429(R"({"error":{"type":"other"}})"), clock);
    const rate_limit_429::Codex429Classification released = co_await std::move(releasingByResponse);
    auto classifyingReleased = classified(released);
    json::value releasedJson = co_await std::move(classifyingReleased);
    out["releaseByNonBlocking"] = pair("classification", std::move(releasedJson), "row", plain(kv->row(ROUTING)));

    auto claimingAgain = routing_mutations::claimCodexRoutingProbe(pool, probeB, clock);
    const std::optional<RoutingAccount> claimedAgain = co_await std::move(claimingAgain);
    out["claimAgain"] = routingAccountJson(claimedAgain);
    if (claimedAgain) {
      auto releasing = routing_mutations::releaseCodexRoutingProbe(*claimedAgain);
      co_await std::move(releasing);
    }
    out["releaseProbe"] = plain(kv->row(ROUTING));

    auto invalidating = routing_mutations::markCodexCredentialInvalid(b);
    co_await std::move(invalidating);
    out["invalid"] = plain(kv->row(ROUTING));

    auto timingOut = routing_mutations::markCodexUpstreamTimeout(b, clock);
    co_await std::move(timingOut);
    out["timeout"] = plain(kv->row(ROUTING));
    capacity_routing::resetCodexAccountRoutingForTest();
    auto reloading = capacity_routing::loadCodexAccountRouting(pool);
    const routing_state::CodexAccountRoutingState reloaded = co_await std::move(reloading);
    out["timeoutLoad"] = plain(json::value(routing_state::toJson(reloaded)));

    CodexAuthState rotatedA = authA;
    rotatedA.access_token = "access-a2";
    rotatedA.refresh_token = "refresh-a2";
    auto unchanging = routing_mutations::reconcileCodexRoutingAccount(a, authA);
    const RoutingAccount unchanged = co_await std::move(unchanging);
    out["reconcileUnchanged"] = routingAccountJson(unchanged);
    auto rotating = routing_mutations::reconcileCodexRoutingAccount(a, rotatedA);
    const RoutingAccount reconciled = co_await std::move(rotating);
    out["reconcileRotated"] = pair("account", routingAccountJson(reconciled), "row", plain(kv->row(ROUTING)));
    const CodexAuthState replacement{.access_token = "access-c", .refresh_token = "refresh-c", .account_id = "acct-c", .updated_at_ms = NOW};
    auto replacing = routing_mutations::reconcileCodexRoutingAccount(b, replacement);
    const RoutingAccount replaced = co_await std::move(replacing);
    out["reconcileReplaced"] = pair("account", routingAccountJson(replaced), "row", plain(kv->row(ROUTING)));

    routing_state::CodexCapacityRoutingObservation observation;
    observation.slot = 0;
    observation.account_id_hash = reconciled.accountIdHash;
    observation.state = "available";
    observation.source_observed_at_ms = std::nullopt;
    observation.snapshot_at_ms = clock - 1'000;
    routing_state::CodexCapacityRoutingWindow primary;
    primary.limit_window_seconds = 18'000;
    primary.used_percent = 100;
    primary.reset_at_ms = clock + 600'000;
    observation.windows.primary = primary;
    auto blockingCapacity = capacity_routing::reconcileCapacityQuotaBlock(reconciled, observation, clock);
    const bool changed = co_await std::move(blockingCapacity);
    out["capacityBlock"] = pair("changed", json::value(changed), "row", plain(kv->row(ROUTING)));
    auto blockingAgain = capacity_routing::reconcileCapacityQuotaBlock(reconciled, observation, clock);
    const bool changedAgain = co_await std::move(blockingAgain);
    out["capacityBlockAgain"] = changedAgain;

    auto hashingA = uos::utils::sha256Hex("acct-a");
    const std::string adminHashA = co_await std::move(hashingA);
    auto hashingB = uos::utils::sha256Hex("acct-b");
    const std::string adminHashB = co_await std::move(hashingB);
    auto allowingA = overage_settings::writeOverageUsage(kv, adminHashA, true);
    co_await std::move(allowingA);
    auto denyingB = overage_settings::writeOverageUsage(kv, adminHashB, false);
    co_await std::move(denyingB);
    auto identifyingA = capacity_routing::routingAccountIdentity(authA);
    const capacity_routing::CodexRoutingAccountIdentity identityA = co_await std::move(identifyingA);
    auto identifyingB = capacity_routing::routingAccountIdentity(authB);
    const capacity_routing::CodexRoutingAccountIdentity identityB = co_await std::move(identifyingB);
    const bool beforeLoad = overage_settings::overageUsageAllowedSync(identityA.accountIdHash);
    auto loadingOverage = overage_settings::loadOverageUsageSettings(pool, true);
    co_await std::move(loadingOverage);
    {
      json::object overageOut;
      overageOut["beforeLoad"] = beforeLoad;
      overageOut["a"] = overage_settings::overageUsageAllowedSync(identityA.accountIdHash);
      overageOut["b"] = overage_settings::overageUsageAllowedSync(identityB.accountIdHash);
      auto readingA = overage_settings::readOverageUsage(kv, adminHashA);
      const bool readA = co_await std::move(readingA);
      auto readingMissing = overage_settings::readOverageUsage(kv, "missing");
      const bool readMissing = co_await std::move(readingMissing);
      overageOut["readA"] = readA;
      overageOut["readMissing"] = readMissing;
      json::array key;
      for (const rt::KvKeyPart& part : overage_settings::codexOverageUsageKey("hash")) key.push_back(json::value(part.string()));
      overageOut["key"] = std::move(key);
      out["overage"] = json::value(std::move(overageOut));
    }
    auto disallowingA = overage_settings::writeOverageUsage(kv, adminHashA, false);
    co_await std::move(disallowingA);
    auto loadingCached = overage_settings::loadOverageUsageSettings(pool);
    co_await std::move(loadingCached);
    out["overageCached"] = overage_settings::overageUsageAllowedSync(identityA.accountIdHash);
    auto reloadingOverage = overage_settings::loadOverageUsageSettings(pool, true);
    co_await std::move(reloadingOverage);
    out["overageReloaded"] = overage_settings::overageUsageAllowedSync(identityA.accountIdHash);

    for (const auto& member : expected) EXPECT(sameVector(expected, out, member.key()));
    EXPECT_EQ(out.size(), expected.size());

    uos::kv::setKvForTest(nullptr);
    capacity_routing::resetCodexAccountRoutingForTest();
    overage_settings::resetOverageUsageCacheForTest();
    rt::globalDateNow() = realNow;
  }
}
