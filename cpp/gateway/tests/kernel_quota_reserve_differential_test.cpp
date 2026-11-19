// Differential cases for src/kernel/quota-reserve.ts and the reservation context of
// src/kernel/quota-reservations.ts beyond tests/native-kernel-conformance.test.ts: each case runs
// the scenario the bun oracle ran (the TS modules on tests/helpers/counting-kv.ts with Date.now
// fixed at NOW) and compares the decision (status and error body), the settlement outcomes and
// every stored entry with the recorded output in kernel_quota_reserve_differential_vectors.inc.
// Links: tests/kernel_quota_reserve_differential_test.cpp src/kernel/quota_reserve.cpp src/kernel/quota_reservations.cpp
//        src/kernel/quota_v2.cpp src/authority/native_kernel.cpp src/authority/native_kernel_contracts.cpp src/http.cpp src/auth/relay.cpp
//        src/config.cpp src/provider/cerebras_rate_limits.cpp src/release.cpp src/utils.cpp src/kv.cpp src/storage/kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp tests/helpers/counting_kv.cpp
#include "uos/kernel/quota_reserve.hpp"
#include "uos/kernel/quota_v2.hpp"

#include "tests/helpers/counting_kv.hpp"
#include "uos/defaults.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace kvfacade = uos::kv;
  namespace quota_v2 = uos::kernel::quota_v2;
  namespace quota_reserve = uos::kernel::quota_reserve;
  using uos::tests::helpers::counting_kv::CountingKv;

  constexpr std::string_view ORACLE =
#include "kernel_quota_reserve_differential_vectors.inc"
    ;

  constexpr double NOW = 1'800'000'000'000;
  const std::string ROUTE = "/v1/responses";

  // `{ ...base, ...overrides }`
  json::object spread(json::object base, const json::object& overrides) {
    for (const auto& member : overrides) rt::objectDefine(base, member.key(), member.value());
    return base;
  }

  json::object members(std::initializer_list<std::pair<std::string_view, json::value>> entries) {
    json::object out;
    for (const auto& [key, value] : entries) rt::objectDefine(out, key, value);
    return out;
  }

  json::object identity(const std::string& scope, const std::string& owner, const std::optional<std::string>& repo) {
    json::object out;
    out["v"] = 2;
    out["scope"] = scope;
    out["owner"] = owner;
    if (repo) out["repo"] = *repo;
    return out;
  }

  rt::KvValue policy(const std::string& scope, const std::string& owner, const std::optional<std::string>& repo, const json::object& overrides = {}) {
    json::object row = identity(scope, owner, repo);
    row["usage_limit_requests"] = 2;
    row["window_ms"] = 60'000;
    row["expires_at_ms"] = -1;
    row["created_at_ms"] = 1'000;
    row["updated_at_ms"] = 2'000;
    return rt::KvValue(json::value(spread(std::move(row), overrides)));
  }

  rt::KvValue windowRow(const std::string& scope, const std::string& owner, const std::optional<std::string>& repo, const json::object& overrides = {}) {
    json::object row = identity(scope, owner, repo);
    row["usage_requests"] = 1;
    row["reserved_requests"] = 0;
    row["usage_reset_at_ms"] = rt::makeNumber(NOW + 30'000);
    row["applied_window_ms"] = 60'000;
    row["created_at_ms"] = rt::makeNumber(NOW - 30'000);
    row["updated_at_ms"] = rt::makeNumber(NOW - 1'000);
    return rt::KvValue(json::value(spread(std::move(row), overrides)));
  }

  rt::KvValue reservation(const std::string& scope, const std::string& owner, const std::optional<std::string>& repo, const json::object& overrides = {}) {
    json::object row = identity(scope, owner, repo);
    row["request_id"] = "req-old";
    row["route"] = "/v1/responses";
    row["window_created_at_ms"] = rt::makeNumber(NOW - 30'000);
    row["window_reset_at_ms"] = rt::makeNumber(NOW + 30'000);
    row["state"] = "reserved";
    row["terminal_intent"] = nullptr;
    row["reserved_at_ms"] = rt::makeNumber(NOW - 10'000);
    row["lease_expires_at_ms"] = rt::makeNumber(NOW + 290'000);
    row["committed_at_ms"] = nullptr;
    row["released_at_ms"] = nullptr;
    row["release_reason"] = nullptr;
    return rt::KvValue(json::value(spread(std::move(row), overrides)));
  }

  quota_reserve::ReserveKernelUsageLimitOptions on(const std::shared_ptr<CountingKv>& kv) {
    quota_reserve::ReserveKernelUsageLimitOptions options;
    options.kv = std::shared_ptr<rt::Kv>(kv);
    return options;
  }

  // `describe(decision)`
  rt::Task<json::object> describe(quota_v2::KernelQuotaReservationDecision decision) {
    json::object out;
    if (const auto* admitted = std::get_if<quota_v2::KernelQuotaReservationDecisionOk>(&decision)) {
      out["ok"] = true;
      out["aborted"] = admitted->reservation.signal.aborted();
      co_return out;
    }
    rt::Response response = std::get<quota_v2::KernelQuotaReservationDecisionNotOk>(decision).response;
    out["ok"] = false;
    out["status"] = response.status();
    rt::Task<json::value> reading = response.json();
    json::value body = co_await std::move(reading);
    out["body"] = std::move(body);
    co_return out;
  }

  // `rejectionOf(promise)`: null when it fulfills, else the rejection's message.
  rt::Task<json::value> rejectionOf(rt::Promise<void> promise) {
    std::optional<std::string> message;
    try {
      co_await promise.wait();
    } catch (const rt::JsError& error) {
      message = error.message;
    }
    co_return message ? json::value(json::string(*message)) : json::value(nullptr);
  }

  const quota_v2::KernelQuotaReservation* admittedReservation(const quota_v2::KernelQuotaReservationDecision& decision) {
    const auto* admitted = std::get_if<quota_v2::KernelQuotaReservationDecisionOk>(&decision);
    return admitted != nullptr ? &admitted->reservation : nullptr;
  }

  json::value entriesJson(const CountingKv& kv) {
    json::array entries;
    for (const auto& [encoded, entry] : kv.entries) {
      json::array key;
      for (const rt::KvKeyPart& part : entry.key) key.push_back(part.isString() ? json::value(json::string(part.string())) : rt::makeNumber(part.number()));
      json::object row;
      row["key"] = std::move(key);
      row["value"] = rt::canonicalNumbers(entry.value.toJson());
      row["versionstamp"] = entry.versionstamp;
      entries.push_back(std::move(row));
    }
    return json::value(std::move(entries));
  }

  // Runs one scenario on a fresh store (also installed as the facade KV) and compares it with the
  // oracle's entry of that name.
  template <class Scenario> rt::Task<void> expectOracle(std::string name, Scenario scenario) {
    rt::globalDateNow() = [] { return NOW; };
    const rt::console::Capture quiet({rt::console::Method::warn});
    const auto kv = std::make_shared<CountingKv>();
    kvfacade::setKvForTest(kv);
    rt::Task<json::object> running = scenario(kv);
    json::object result = co_await std::move(running);
    kvfacade::setKvForTest(nullptr);
    json::object actual;
    actual["name"] = name;
    actual["result"] = std::move(result);
    actual["entries"] = entriesJson(*kv);
    const json::value oracle = rt::jsonParse(ORACLE);
    const json::value* expected = nullptr;
    for (const json::value& entry : oracle.as_array()) {
      if (rt::getString(entry, "name") == std::optional<std::string_view>(name)) expected = &entry;
    }
    ASSERT(expected != nullptr);
    EXPECT_EQ(rt::jsonStringify(json::value(std::move(actual))), rt::jsonStringify(*expected));
  }
}

ASYNC_TEST("org reserve then commit") {
  auto scenario = [](std::shared_ptr<CountingKv> kv) -> rt::Task<json::object> {
    auto reserving = quota_reserve::reserveKernelOrgUsageLimit("acme", "req-1", ROUTE, on(kv));
    const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
    auto describing = describe(decision);
    json::object out = co_await std::move(describing);
    if (const quota_v2::KernelQuotaReservation* reservation = admittedReservation(decision)) {
      auto committing = rejectionOf(reservation->commit());
      out["commit"] = co_await std::move(committing);
    }
    co_return out;
  };
  auto checking = expectOracle("org reserve then commit", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("effective repo reserve then release") {
  auto scenario = [](std::shared_ptr<CountingKv> kv) -> rt::Task<json::object> {
    kv->seed(quota_v2::kernelRepoPolicyKey("acme", "demo"), policy("repo", "acme", "demo"));
    kv->seed(quota_v2::kernelRepoWindowKey("acme", "demo"), windowRow("repo", "acme", "demo"));
    auto reserving = quota_reserve::reserveEffectiveKernelUsageLimit("acme", "demo", "req-1", ROUTE, on(kv));
    const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
    auto describing = describe(decision);
    json::object out = co_await std::move(describing);
    if (const quota_v2::KernelQuotaReservation* reservation = admittedReservation(decision)) {
      auto releasing = rejectionOf(reservation->release(std::string("client_abort")));
      out["release"] = co_await std::move(releasing);
      auto lateCommitting = rejectionOf(reservation->commit());
      out["lateCommit"] = co_await std::move(lateCommitting);
      auto releasingAgain = rejectionOf(reservation->release(std::nullopt));
      out["again"] = co_await std::move(releasingAgain);
    }
    co_return out;
  };
  auto checking = expectOracle("effective repo reserve then release", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("effective org reserve guards the absent repo policy") {
  auto scenario = [](std::shared_ptr<CountingKv> kv) -> rt::Task<json::object> {
    kv->seed(quota_v2::kernelOrgPolicyKey("acme"), policy("org", "acme", std::nullopt, members({{"usage_limit_requests", 5}})));
    quota_reserve::ReserveKernelUsageLimitOptions options = on(kv);
    options.nowMs = NOW + 5;
    auto reserving = quota_reserve::reserveEffectiveKernelUsageLimit("acme", "demo", "req-1", ROUTE, options);
    const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
    auto describing = describe(decision);
    json::object out = co_await std::move(describing);
    if (const quota_v2::KernelQuotaReservation* reservation = admittedReservation(decision)) {
      auto committing = rejectionOf(reservation->commit());
      out["commit"] = co_await std::move(committing);
      auto lateReleasing = rejectionOf(reservation->release(std::string("late")));
      out["lateRelease"] = co_await std::move(lateReleasing);
    }
    co_return out;
  };
  auto checking = expectOracle("effective org reserve guards the absent repo policy", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("a full window refuses with the reset time") {
  auto scenario = [](std::shared_ptr<CountingKv> kv) -> rt::Task<json::object> {
    kv->seed(quota_v2::kernelRepoPolicyKey("acme", "demo"), policy("repo", "acme", "demo"));
    kv->seed(quota_v2::kernelRepoWindowKey("acme", "demo"), windowRow("repo", "acme", "demo", members({{"usage_requests", 1}, {"reserved_requests", 1}})));
    kv->seed(quota_v2::kernelRepoReservationKey("acme", "demo", NOW - 30'000, "req-old"), reservation("repo", "acme", "demo"));
    auto reserving = quota_reserve::reserveEffectiveKernelUsageLimit("acme", "demo", "req-1", ROUTE, on(kv));
    const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
    auto describing = describe(decision);
    json::object out = co_await std::move(describing);
    co_return out;
  };
  auto checking = expectOracle("a full window refuses with the reset time", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("a full window reclaims an expired lease and reserves") {
  auto scenario = [](std::shared_ptr<CountingKv> kv) -> rt::Task<json::object> {
    kv->seed(quota_v2::kernelRepoPolicyKey("acme", "demo"), policy("repo", "acme", "demo"));
    kv->seed(quota_v2::kernelRepoWindowKey("acme", "demo"), windowRow("repo", "acme", "demo", members({{"usage_requests", 1}, {"reserved_requests", 1}})));
    kv->seed(quota_v2::kernelRepoReservationKey("acme", "demo", NOW - 30'000, "req-old"),
             reservation("repo", "acme", "demo", members({{"lease_expires_at_ms", rt::makeNumber(NOW - 1)}})));
    auto reserving = quota_reserve::reserveEffectiveKernelUsageLimit("acme", "demo", "req-1", ROUTE, on(kv));
    const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
    auto describing = describe(decision);
    json::object out = co_await std::move(describing);
    if (const quota_v2::KernelQuotaReservation* reservation = admittedReservation(decision)) {
      auto committing = rejectionOf(reservation->commit());
      out["commit"] = co_await std::move(committing);
    }
    co_return out;
  };
  auto checking = expectOracle("a full window reclaims an expired lease and reserves", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("an expired policy refuses") {
  auto scenario = [](std::shared_ptr<CountingKv> kv) -> rt::Task<json::object> {
    kv->seed(quota_v2::kernelOrgPolicyKey("acme"), policy("org", "acme", std::nullopt, members({{"expires_at_ms", rt::makeNumber(NOW)}})));
    auto reserving = quota_reserve::reserveKernelOrgUsageLimit("acme", "req-1", ROUTE, on(kv));
    const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
    auto describing = describe(decision);
    json::object out = co_await std::move(describing);
    co_return out;
  };
  auto checking = expectOracle("an expired policy refuses", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("a zero limit refuses") {
  auto scenario = [](std::shared_ptr<CountingKv> kv) -> rt::Task<json::object> {
    kv->seed(uos::defaults::DEFAULT_KERNEL_POLICY_LIMIT_KEY, rt::KvValue(0));
    auto reserving = quota_reserve::reserveKernelOrgUsageLimit("acme", "req-1", ROUTE, on(kv));
    const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
    auto describing = describe(decision);
    json::object out = co_await std::move(describing);
    co_return out;
  };
  auto checking = expectOracle("a zero limit refuses", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("a duplicate request id refuses") {
  auto scenario = [](std::shared_ptr<CountingKv> kv) -> rt::Task<json::object> {
    kv->seed(quota_v2::kernelOrgPolicyKey("acme"), policy("org", "acme", std::nullopt, members({{"usage_limit_requests", 10}})));
    kv->seed(quota_v2::kernelOrgWindowKey("acme"), windowRow("org", "acme", std::nullopt, members({{"reserved_requests", 1}})));
    kv->seed(quota_v2::kernelOrgReservationKey("acme", NOW - 30'000, "req-old"), reservation("org", "acme", std::nullopt));
    auto reserving = quota_reserve::reserveKernelOrgUsageLimit("acme", "req-old", ROUTE, on(kv));
    const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
    auto describing = describe(decision);
    json::object out = co_await std::move(describing);
    co_return out;
  };
  auto checking = expectOracle("a duplicate request id refuses", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("a conflicting request identity refuses") {
  auto scenario = [](std::shared_ptr<CountingKv> kv) -> rt::Task<json::object> {
    kv->seed(quota_v2::kernelOrgPolicyKey("acme"), policy("org", "acme", std::nullopt, members({{"usage_limit_requests", 10}})));
    kv->seed(quota_v2::kernelOrgWindowKey("acme"), windowRow("org", "acme", std::nullopt, members({{"reserved_requests", 1}})));
    kv->seed(quota_v2::kernelOrgReservationKey("acme", NOW - 30'000, "req-old"), reservation("org", "acme", std::nullopt));
    auto reserving = quota_reserve::reserveKernelOrgUsageLimit("acme", "req-old", "/v1/chat/completions", on(kv));
    const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
    auto describing = describe(decision);
    json::object out = co_await std::move(describing);
    co_return out;
  };
  auto checking = expectOracle("a conflicting request identity refuses", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("a missing request id or route refuses") {
  auto scenario = [](std::shared_ptr<CountingKv> kv) -> rt::Task<json::object> {
    auto firstReserving = quota_reserve::reserveKernelOrgUsageLimit("acme", "", ROUTE, on(kv));
    const quota_v2::KernelQuotaReservationDecision first = co_await std::move(firstReserving);
    auto firstDescribing = describe(first);
    json::object firstOut = co_await std::move(firstDescribing);
    auto secondReserving = quota_reserve::reserveKernelOrgUsageLimit("acme", "req-1", "", on(kv));
    const quota_v2::KernelQuotaReservationDecision second = co_await std::move(secondReserving);
    auto secondDescribing = describe(second);
    json::object secondOut = co_await std::move(secondDescribing);
    json::object out;
    out["first"] = std::move(firstOut);
    out["second"] = std::move(secondOut);
    co_return out;
  };
  auto checking = expectOracle("a missing request id or route refuses", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("a malformed window refuses") {
  auto scenario = [](std::shared_ptr<CountingKv> kv) -> rt::Task<json::object> {
    kv->seed(quota_v2::kernelOrgWindowKey("acme"), rt::KvValue(json::value(members({{"v", 2}, {"scope", "org"}, {"owner", "acme"}}))));
    auto reserving = quota_reserve::reserveKernelOrgUsageLimit("acme", "req-1", ROUTE, on(kv));
    const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
    auto describing = describe(decision);
    json::object out = co_await std::move(describing);
    co_return out;
  };
  auto checking = expectOracle("a malformed window refuses", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("a malformed policy is unavailable") {
  auto scenario = [](std::shared_ptr<CountingKv> kv) -> rt::Task<json::object> {
    kv->seed(quota_v2::kernelOrgPolicyKey("acme"), rt::KvValue(json::value(members({{"v", 2}, {"scope", "org"}, {"owner", "acme"}}))));
    auto reserving = quota_reserve::reserveKernelOrgUsageLimit("acme", "req-1", ROUTE, on(kv));
    const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
    auto describing = describe(decision);
    json::object out = co_await std::move(describing);
    co_return out;
  };
  auto checking = expectOracle("a malformed policy is unavailable", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("an effective reserve with a malformed repo policy is unavailable") {
  auto scenario = [](std::shared_ptr<CountingKv> kv) -> rt::Task<json::object> {
    kv->seed(quota_v2::kernelRepoPolicyKey("acme", "demo"), rt::KvValue(json::value(members({{"v", 2}}))));
    auto reserving = quota_reserve::reserveEffectiveKernelUsageLimit("acme", "demo", "req-1", ROUTE, on(kv));
    const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
    auto describing = describe(decision);
    json::object out = co_await std::move(describing);
    co_return out;
  };
  auto checking = expectOracle("an effective reserve with a malformed repo policy is unavailable", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("a default cutover marker refuses a default-backed reserve") {
  auto scenario = [](std::shared_ptr<CountingKv> kv) -> rt::Task<json::object> {
    kv->seed(quota_v2::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY,
             rt::KvValue(json::value(members({{"v", 2}, {"id", "lease"}, {"created_at_ms", rt::makeNumber(NOW)}, {"expires_at_ms", rt::makeNumber(NOW + 300'000)}}))));
    auto reserving = quota_reserve::reserveKernelOrgUsageLimit("acme", "req-1", ROUTE, on(kv));
    const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
    auto describing = describe(decision);
    json::object out = co_await std::move(describing);
    co_return out;
  };
  auto checking = expectOracle("a default cutover marker refuses a default-backed reserve", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("no KV is unavailable") {
  auto scenario = [](std::shared_ptr<CountingKv>) -> rt::Task<json::object> {
    quota_reserve::ReserveKernelUsageLimitOptions nullKv;
    nullKv.kv = std::shared_ptr<rt::Kv>();
    auto firstReserving = quota_reserve::reserveKernelOrgUsageLimit("acme", "req-1", ROUTE, nullKv);
    const quota_v2::KernelQuotaReservationDecision first = co_await std::move(firstReserving);
    auto firstDescribing = describe(first);
    json::object firstOut = co_await std::move(firstDescribing);
    auto secondReserving = quota_reserve::reserveEffectiveKernelUsageLimit("acme", "demo", "req-1", ROUTE, nullKv);
    const quota_v2::KernelQuotaReservationDecision second = co_await std::move(secondReserving);
    auto secondDescribing = describe(second);
    json::object secondOut = co_await std::move(secondDescribing);
    json::object out;
    out["first"] = std::move(firstOut);
    out["second"] = std::move(secondOut);
    co_return out;
  };
  auto checking = expectOracle("no KV is unavailable", scenario);
  co_await std::move(checking);
}

ASYNC_TEST("the facade KV serves a reserve without options") {
  auto scenario = [](std::shared_ptr<CountingKv>) -> rt::Task<json::object> {
    auto reserving = quota_reserve::reserveEffectiveKernelUsageLimit("acme", "demo", "req-1", ROUTE);
    const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
    auto describing = describe(decision);
    json::object out = co_await std::move(describing);
    if (const quota_v2::KernelQuotaReservation* reservation = admittedReservation(decision)) {
      auto releasing = rejectionOf(reservation->release(std::nullopt));
      out["release"] = co_await std::move(releasing);
    }
    co_return out;
  };
  auto checking = expectOracle("the facade KV serves a reserve without options", scenario);
  co_await std::move(checking);
}
