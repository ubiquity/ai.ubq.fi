// Port of tests/native-kernel-conformance.test.ts (every case): the shared vectors
// (runKernelConformanceVectors over src/authority/native-kernel.ts), the three pure test cases, and
// the two wrapper cases over src/kernel/quota-reserve.ts. CountingKv is final in C++, so the test's
// KernelConflictKv subclass is a ForwardingKv over its own CountingKv.
// Links: tests/native_kernel_conformance_test.cpp src/kernel/quota_reserve.cpp src/kernel/quota_reservations.cpp src/kernel/quota_v2.cpp
//        src/authority/native_kernel.cpp src/authority/native_kernel_contracts.cpp src/http.cpp src/auth/relay.cpp src/config.cpp
//        src/provider/cerebras_rate_limits.cpp src/release.cpp src/utils.cpp src/kv.cpp src/storage/kv.cpp src/storage/sqlite_readonly.cpp
//        src/storage/native_kv.cpp tests/helpers/counting_kv.cpp
#include "uos/api_keys.hpp"
#include "uos/authority/native_kernel.hpp"
#include "uos/authority/native_kernel_contracts.hpp"
#include "uos/kernel/quota_reserve.hpp"
#include "uos/kernel/quota_v2.hpp"

#include "tests/helpers/counting_kv.hpp"
#include "uos/defaults.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace kernelV2 = uos::authority::native_kernel;
using namespace uos::authority::native_kernel_contracts;

namespace {
  // BEGIN SHARED_KERNEL_VECTORS
  /** Run this exact function against the source reducer in both pinned Bun and actual workerd. */
  struct KernelConformanceResults {
    std::vector<rt::KvKey> keys;
    std::vector<kernelV2::KernelQuotaScopeDecision> scope;
    std::vector<std::optional<std::string>> malformedPolicy;
    std::optional<KernelQuotaPolicyV2> normalizedPolicy;
    std::optional<KernelQuotaWindowV2> legacyWindow;
    std::optional<KernelQuotaWindowV2> malformedWindow;
    std::vector<KernelQuotaEffectiveWindow> replacement;
    std::vector<KernelQuotaEligibilityPlan> eligibility;
    KernelQuotaReservePlanReserve reserved;
    KernelQuotaReservePlan duplicate;
    KernelQuotaReservePlan identityConflict;
    KernelQuotaReservePlan full;
    KernelQuotaReservePlan unlimited;
    KernelQuotaTerminalPlanIntent pending;
    KernelQuotaSettlePlanSettle settle;
    KernelQuotaTerminalPlan replay;
    KernelQuotaTerminalPlan terminalMismatch;
    KernelQuotaTerminalPlan missing;
    KernelQuotaTerminalPlan malformed;
    KernelQuotaSettlePlan oldWindow;
    KernelQuotaSettlePlan lostWindowReservation;
    KernelQuotaSettlePlan invalidStoredWindow;
    KernelQuotaSettlePlan released;
    std::optional<std::string> longReleaseReason;
    kernelV2::KernelQuotaRenewal renewal;
    std::vector<std::optional<std::string>> reclaim;
    std::vector<KernelQuotaCutoverPlan> cutover;
    std::vector<bool> acquired;
    std::vector<kernelV2::KernelPolicyWindowGuard> policyGuard;
    std::vector<std::string> deletePreconditions;
    std::vector<bool> resetRequested;
    kernelV2::KernelPolicyTiming policyTiming;
    KernelQuotaPolicyWritePlan policyWrite;
    std::vector<KernelQuotaWindowV2> policyDelete;
    std::vector<double> retention;
    bool snapshotUnchanged;
  };

  KernelQuotaRead<> row(json::value value, std::optional<std::string> revision = std::string("opaque.rev:0x1")) {
    return KernelQuotaRead<>{.value = std::move(value), .revision = std::move(revision), .expiresAtMs = nullptr};
  }
  // `{ ...object, [key]: value }` on a row's JSON.
  json::value with(const json::value& object, std::string_view key, json::value value) {
    json::object out = object.as_object();
    rt::objectDefine(out, key, std::move(value));
    return out;
  }
  json::value without(const json::value& object, std::string_view key) {
    json::object out = object.as_object();
    rt::objectDelete(out, key);
    return out;
  }
  template <class Plan, class Alternative> const Alternative& expect(const Plan& plan, const char* message) {
    const Alternative* alternative = std::get_if<Alternative>(&plan);
    if (alternative == nullptr) throw std::runtime_error(message);
    return *alternative;
  }

  KernelConformanceResults runKernelConformanceVectors(const KernelQuotaConstants& constants) {
    const double now = 10'000'000;
    const KernelQuotaIdentity identity{.scope = "repo", .owner = "synthetic", .repo = "repo"};
    const KernelQuotaRead<> empty = row(nullptr, std::nullopt);
    const KernelQuotaDefaults defaults{.limit = 1, .windowMs = 60'000};
    const KernelQuotaPolicyV2 policy{.v = 2, .scope = "repo", .owner = "synthetic", .repo = "repo", .usage_limit_requests = 1, .window_ms = 60'000,
                                     .expires_at_ms = constants.noExpirationMs, .created_at_ms = 0, .updated_at_ms = now};
    const json::value policyJson = json::value_from(policy);
    const json::value orgPolicy = without(with(policyJson, "scope", "org"), "repo");
    const KernelQuotaEffectivePolicy effective = kernelV2::policyFor(row(policyJson), "repo", "synthetic", "repo", defaults, constants);
    const KernelQuotaWindowV2 window = kernelV2::newWindow("repo", "synthetic", "repo", 60'000, now);
    const KernelQuotaReservePlanReserve reserved = expect<KernelQuotaReservePlan, KernelQuotaReservePlanReserve>(
      kernelV2::decideKernelQuotaReserve(identity, effective, window, empty, "request", "responses", constants.noUsageLimit, now), "Synthetic kernel reservation was not admitted");
    KernelQuotaReservationIdentity request;
    request.scope = identity.scope;
    request.owner = identity.owner;
    request.repo = identity.repo;
    request.windowCreatedAtMs = now;
    request.requestId = "request";
    request.route = "responses";
    const auto snapshotJson = [&] {
      json::object snapshot;
      snapshot["policy"] = policyJson;
      snapshot["window"] = json::value_from(window);
      snapshot["reservation"] = json::value_from(reserved.reservation);
      return rt::jsonStringify(json::value(std::move(snapshot)));
    };
    const std::string before = snapshotJson();
    const KernelQuotaTerminalPlanIntent pending = expect<KernelQuotaTerminalPlan, KernelQuotaTerminalPlanIntent>(
      kernelV2::decideKernelQuotaTerminalIntent(request, row(json::value_from(reserved.reservation)), "committed", now + 1), "Synthetic kernel terminal intent was not persisted");
    const KernelQuotaSettlePlanSettle settled = expect<KernelQuotaSettlePlan, KernelQuotaSettlePlanSettle>(
      kernelV2::decideKernelQuotaSettlement(pending.reservation, {.window = reserved.window, .windowPresent = true}, "committed", "completed", now + 2),
      "Synthetic kernel settlement was not writable");
    const auto failure = [](const auto& operation) -> std::optional<std::string> {
      try {
        operation();
        return std::nullopt;
      } catch (const rt::JsError& error) {
        return error.message;
      }
    };
    const auto eligibility = [&](json::value value, const std::function<void(KernelQuotaEligibilitySnapshot&)>& extra = {}) {
      KernelQuotaEligibilitySnapshot snapshot{.policy = row(std::move(value)), .repoGuard = std::nullopt, .defaultCutover = empty};
      if (extra) extra(snapshot);
      return kernelV2::decideKernelQuotaEligibility(identity, snapshot, defaults, constants, now);
    };
    const KernelQuotaPolicyV2 resetPolicy = kernelV2::makeKernelQuotaPolicy(identity, effective, 7, {.windowMs = 120'000, .expiresAtMs = constants.noExpirationMs},
                                                                           constants.noUsageLimit, now + 4);
    KernelQuotaEffectivePolicy unlimitedPolicy = effective;
    unlimitedPolicy.limit = constants.noUsageLimit;
    KernelQuotaWindowV2 busyWindow = reserved.window;
    busyWindow.usage_requests = 100;
    KernelQuotaWindowV2 oldWindow = window;
    oldWindow.created_at_ms = now + 1;
    oldWindow.usage_requests = 8;
    KernelQuotaDefaults longerDefaults = defaults;
    longerDefaults.windowMs = 120'000;
    KernelQuotaPolicySetOptions resetUsage;
    resetUsage.resetUsage = true;
    KernelQuotaPolicySetOptions timing;
    timing.windowMs = 0;
    timing.expiresAtMs = -99;
    const json::value cutoverRow = rt::jsonParse(R"({"v":2,"id":"lease","created_at_ms":10000000,"expires_at_ms":10300000})");
    return KernelConformanceResults{
      .keys =
        {
          kernelV2::kernelRepoPolicyKey("synthetic", "repo"),
          kernelV2::kernelOrgPolicyKey("synthetic"),
          kernelV2::kernelRepoWindowKey("synthetic", "repo"),
          kernelV2::kernelOrgWindowKey("synthetic"),
          kernelV2::kernelRepoReservationKey("synthetic", "repo", now, "request"),
          kernelV2::kernelOrgReservationKey("synthetic", now, "request"),
          kernelV2::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY,
        },
      .scope =
        {
          kernelV2::decideKernelQuotaScope({.repoPolicy = empty, .orgPolicy = empty}, "synthetic", "repo", constants),
          kernelV2::decideKernelQuotaScope({.repoPolicy = empty, .orgPolicy = row(orgPolicy)}, "synthetic", "repo", constants),
          kernelV2::decideKernelQuotaScope({.repoPolicy = row(policyJson), .orgPolicy = row(orgPolicy)}, "synthetic", "repo", constants),
        },
      .malformedPolicy =
        {
          failure([&] { kernelV2::policyFor(row(json::object()), "repo", "synthetic", "repo", defaults, constants); }),
          failure([&] { kernelV2::decideKernelQuotaScope({.repoPolicy = row(policyJson), .orgPolicy = row(json::object())}, "synthetic", "repo", constants); }),
        },
      .normalizedPolicy = kernelV2::normalizeKernelQuotaPolicyV2(
        with(with(with(policyJson, "window_ms", "60000.9"), "usage_limit_requests", "1.9"), "expires_at_ms", "bad"), "repo", "synthetic", "repo", constants),
      .legacyWindow = kernelV2::normalizeKernelQuotaWindowV2(without(json::value_from(window), "reserved_requests"), "repo", "synthetic", "repo"),
      .malformedWindow = kernelV2::normalizeKernelQuotaWindowV2(with(json::value_from(window), "reserved_requests", -1), "repo", "synthetic", "repo"),
      .replacement =
        {
          kernelV2::windowForEffectivePolicy(reserved.window, "repo", "synthetic", "repo", 60'000, now + 60'000),
          kernelV2::windowForEffectivePolicy(reserved.window, "repo", "synthetic", "repo", 120'000, now + 1),
        },
      .eligibility =
        {
          eligibility(nullptr),
          eligibility(nullptr, [](KernelQuotaEligibilitySnapshot& snapshot) { snapshot.expectedRepoPolicy = "present"; }),
          eligibility(nullptr,
                      [&](KernelQuotaEligibilitySnapshot& snapshot) {
                        snapshot.expectedRepoPolicy = "absent";
                        snapshot.repoGuard = row(policyJson);
                      }),
          eligibility(nullptr, [&](KernelQuotaEligibilitySnapshot& snapshot) { snapshot.defaultCutover = row(json::object()); }),
          eligibility(policyJson, [&](KernelQuotaEligibilitySnapshot& snapshot) { snapshot.defaultCutover = row(json::object()); }),
          eligibility(with(policyJson, "expires_at_ms", now)),
          eligibility(with(policyJson, "usage_limit_requests", 0)),
        },
      .reserved = reserved,
      .duplicate = kernelV2::decideKernelQuotaReserve(identity, effective, reserved.window, row(json::value_from(reserved.reservation)), "request", "responses",
                                                      constants.noUsageLimit, now),
      .identityConflict = kernelV2::decideKernelQuotaReserve(identity, effective, reserved.window, row(json::value_from(reserved.reservation)), "request",
                                                             "chat.completions", constants.noUsageLimit, now),
      .full = kernelV2::decideKernelQuotaReserve(identity, effective, reserved.window, empty, "other", "responses", constants.noUsageLimit, now),
      .unlimited = kernelV2::decideKernelQuotaReserve(identity, unlimitedPolicy, busyWindow, empty, "other", "responses", constants.noUsageLimit, now),
      .pending = pending,
      .settle = settled,
      .replay = kernelV2::decideKernelQuotaTerminalIntent(request, row(json::value_from(settled.reservation)), "committed", now + 3),
      .terminalMismatch = kernelV2::decideKernelQuotaTerminalIntent(request, row(json::value_from(pending.reservation)), "released", now + 3),
      .missing = kernelV2::decideKernelQuotaTerminalIntent(request, empty, "committed", now),
      .malformed = kernelV2::decideKernelQuotaTerminalIntent(request, row(json::object()), "committed", now),
      .oldWindow = kernelV2::decideKernelQuotaSettlement(pending.reservation, {.window = oldWindow, .windowPresent = true}, "committed", "completed", now + 4),
      .lostWindowReservation = kernelV2::decideKernelQuotaSettlement(pending.reservation, {.window = window, .windowPresent = true}, "committed", "completed", now + 4),
      .invalidStoredWindow = kernelV2::decideKernelQuotaSettlement(pending.reservation, {.window = std::nullopt, .windowPresent = true}, "committed", "completed", now + 4),
      .released = kernelV2::decideKernelQuotaSettlement(reserved.reservation, {.window = reserved.window, .windowPresent = true}, "released", "", now + 4),
      .longReleaseReason = kernelV2::settledKernelReservationRow(reserved.reservation, "released", std::string(130, 'x'), now + 4).release_reason,
      .renewal = kernelV2::decideKernelQuotaRenewal(request, row(json::value_from(pending.reservation)), now + 300'001),
      .reclaim =
        {
          kernelV2::decideKernelQuotaReclaim(reserved.reservation, now + 299'999),
          kernelV2::decideKernelQuotaReclaim(reserved.reservation, now + 300'000),
          kernelV2::decideKernelQuotaReclaim(pending.reservation, pending.reservation.lease_expires_at_ms),
          kernelV2::decideKernelQuotaReclaim(settled.reservation, now + 900'000),
        },
      .cutover =
        {
          kernelV2::decideKernelDefaultWindowCutover(empty, "lease", now),
          kernelV2::decideKernelDefaultWindowCutover(row(json::object()), "lease", now),
          kernelV2::decideKernelDefaultWindowCutover(row(rt::jsonParse(R"({"v":2,"id":"expired","created_at_ms":1,"expires_at_ms":2})")), "lease", now),
        },
      .acquired =
        {
          kernelV2::kernelDefaultWindowCutoverAcquired(row(cutoverRow), "lease"),
          kernelV2::kernelDefaultWindowCutoverAcquired(row(cutoverRow, std::nullopt), "lease"),
        },
      .policyGuard =
        {
          kernelV2::decideKernelPolicyWindowGuard(reserved.window, true, true, now),
          kernelV2::decideKernelPolicyWindowGuard(reserved.window, true, false, now),
          kernelV2::decideKernelPolicyWindowGuard(std::nullopt, true, true, now),
          kernelV2::decideKernelPolicyWindowGuard(reserved.window, true, true, now + 60'000),
        },
      .deletePreconditions =
        {
          kernelV2::decideKernelQuotaPolicyDeletePrecondition({.policy = empty, .defaultCutover = row(json::object())}),
          kernelV2::decideKernelQuotaPolicyDeletePrecondition({.policy = row(policyJson), .defaultCutover = row(json::object())}),
          kernelV2::decideKernelQuotaPolicyDeletePrecondition({.policy = row(policyJson), .defaultCutover = empty}),
        },
      .resetRequested =
        {
          kernelV2::kernelQuotaPolicyResetRequested({}, 60'000, 60'000),
          kernelV2::kernelQuotaPolicyResetRequested(resetUsage, 60'000, 60'000),
          kernelV2::kernelQuotaPolicyResetRequested({}, 60'000, 120'000),
        },
      .policyTiming = kernelV2::resolveKernelPolicyTiming(timing, effective, constants.noExpirationMs),
      .policyWrite = kernelV2::decideKernelQuotaPolicyWrite(identity, resetPolicy, reserved.window, true, now + 4),
      .policyDelete =
        {
          kernelV2::decideKernelQuotaPolicyDeleteWindow(identity, effective, defaults, reserved.window, now + 4),
          kernelV2::decideKernelQuotaPolicyDeleteWindow(identity, effective, longerDefaults, reserved.window, now + 4),
        },
      .retention =
        {
          kernelV2::kernelReservationRetentionMs(window.usage_reset_at_ms, now),
          kernelV2::kernelReservationRetentionMs(window.usage_reset_at_ms, now + 1),
          kernelV2::kernelReservationRetentionMs(window.usage_reset_at_ms, now + 700'000'000),
        },
      .snapshotUnchanged = before == snapshotJson(),
    };
  }
  // END SHARED_KERNEL_VECTORS

  const KernelQuotaConstants constants{.noUsageLimit = uos::api_keys::API_KEY_NO_USAGE_LIMIT, .noExpirationMs = uos::api_keys::API_KEY_NO_EXPIRATION_MS};

  template <class Variant> std::string kindOf(const Variant& value) {
    return std::visit([](const auto& alternative) { return std::string(std::decay_t<decltype(alternative)>::kind); }, value);
  }
  // `value.kind === "refused" ? value.status : value.kind`
  json::value statusOrKind(const KernelQuotaEligibilityPlan& value) {
    if (const auto* refused = std::get_if<KernelQuotaRefusal>(&value)) return rt::makeNumber(refused->status);
    return json::value(kindOf(value));
  }
  KernelQuotaTerminalPlanUnchanged unchanged(const std::string& outcome) { return KernelQuotaTerminalPlanUnchanged{.outcome = outcome}; }
}

TEST("native kernel pure scope and eligibility preserve explicit refusal, defaults and exact key identity") {
  const KernelConformanceResults result = runKernelConformanceVectors(constants);
  const std::vector<rt::KvKey> keys = {
    rt::KvKey{"uos_ai", "kernel_quota", "v2", "repo_policy", "synthetic", "repo"},
    rt::KvKey{"uos_ai", "kernel_quota", "v2", "org_policy", "synthetic"},
    rt::KvKey{"uos_ai", "kernel_quota", "v2", "repo_window", "synthetic", "repo"},
    rt::KvKey{"uos_ai", "kernel_quota", "v2", "org_window", "synthetic"},
    rt::KvKey{"uos_ai", "kernel_quota", "v2", "repo_reservation", "synthetic", "repo", 10'000'000, "request"},
    rt::KvKey{"uos_ai", "kernel_quota", "v2", "org_reservation", "synthetic", 10'000'000, "request"},
    rt::KvKey{"uos_ai", "kernel_quota", "v2", "default_window_cutover"},
  };
  EXPECT(result.keys == keys);
  const std::vector<kernelV2::KernelQuotaScopeDecision> scope = {
    {.limit_scope = "org", .has_policy = false},
    {.limit_scope = "org", .has_policy = true},
    {.limit_scope = "repo", .has_policy = true},
  };
  EXPECT(result.scope == scope);
  const std::vector<std::optional<std::string>> malformedPolicy = {std::string("kernel quota policy is malformed"), std::string("Kernel org quota policy is malformed")};
  EXPECT(result.malformedPolicy == malformedPolicy);
  ASSERT(result.normalizedPolicy.has_value());
  EXPECT_EQ(result.normalizedPolicy->window_ms, 60'000.0);
  EXPECT_EQ(result.normalizedPolicy->usage_limit_requests, 1.0);
  EXPECT_EQ(result.normalizedPolicy->expires_at_ms, -1.0);
  EXPECT(result.legacyWindow.has_value() && result.legacyWindow->reserved_requests == 0);
  EXPECT(!result.malformedWindow.has_value());
  json::array eligibility;
  for (const KernelQuotaEligibilityPlan& value : result.eligibility) eligibility.push_back(statusOrKind(value));
  EXPECT(rt::deepEqual(eligibility, rt::jsonParse(R"(["eligible",503,503,503,"eligible",429,429])")));
}

TEST("native kernel pure lifecycle preserves duplicate refusal, committed intent, replay and original-window retention") {
  const KernelConformanceResults result = runKernelConformanceVectors(constants);
  EXPECT_EQ(kindOf(result.duplicate), std::string("refused"));
  EXPECT_EQ(std::get<KernelQuotaRefusal>(result.duplicate).message, std::string("Kernel quota request already has a reservation"));
  EXPECT_EQ(kindOf(result.identityConflict), std::string("refused"));
  EXPECT_EQ(kindOf(result.full), std::string("full"));
  EXPECT_EQ(kindOf(result.unlimited), std::string("reserve"));
  EXPECT_EQ(result.reserved.reservation.lease_expires_at_ms, 10'300'000.0);
  EXPECT_EQ(result.pending.reservation.terminal_intent, std::optional<std::string>("committed"));
  EXPECT_EQ(result.pending.leaseExpiresAtMs, 10'300'001.0);
  ASSERT(result.settle.window.has_value());
  EXPECT_EQ(result.settle.window->usage_requests, 1.0);
  EXPECT_EQ(result.settle.window->reserved_requests, 0.0);
  EXPECT(result.replay == KernelQuotaTerminalPlan(unchanged("settled")));
  EXPECT(result.terminalMismatch == KernelQuotaTerminalPlan(unchanged("terminal_mismatch")));
  EXPECT(result.missing == KernelQuotaTerminalPlan(unchanged("missing")));
  EXPECT(result.malformed == KernelQuotaTerminalPlan(unchanged("invalid")));
  EXPECT_EQ(kindOf(result.oldWindow), std::string("settle"));
  EXPECT(!std::get<KernelQuotaSettlePlanSettle>(result.oldWindow).window.has_value());
  EXPECT(result.lostWindowReservation == KernelQuotaSettlePlan(unchanged("invalid")));
  EXPECT(result.invalidStoredWindow == KernelQuotaSettlePlan(unchanged("invalid")));
  if (const auto* released = std::get_if<KernelQuotaSettlePlanSettle>(&result.released)) {
    EXPECT(released->window.has_value() && released->window->usage_requests == 0);
    EXPECT_EQ(released->reservation.release_reason, std::optional<std::string>("request_incomplete"));
  } else {
    EXPECT(false && "Release must write its terminal row");
  }
  EXPECT(result.longReleaseReason.has_value() && rt::length(*result.longReleaseReason) == 120);
  const std::vector<std::optional<std::string>> reclaim = {std::nullopt, std::string("released"), std::string("committed"), std::nullopt};
  EXPECT(result.reclaim == reclaim);
  EXPECT_EQ(result.renewal.reservation.terminal_intent, std::optional<std::string>("committed"));
  EXPECT_EQ(result.renewal.leaseExpiresAtMs, 10'600'001.0);
  EXPECT_EQ(result.renewal.expireIn + 10'300'001, 10'060'000.0 + 604'800'000.0);
  EXPECT(result.retention == (std::vector<double>{604'860'000, 604'859'999, 1}));
  EXPECT(result.snapshotUnchanged);
}

TEST("native kernel pure cutover and policy writes keep live-work guards separate from natural replacement") {
  const KernelConformanceResults result = runKernelConformanceVectors(constants);
  json::array cutover;
  for (const KernelQuotaCutoverPlan& value : result.cutover) {
    if (const auto* refused = std::get_if<KernelQuotaCutoverPlanRefused>(&value)) cutover.push_back(json::value(refused->reason));
    else cutover.push_back(rt::makeNumber(std::get<KernelQuotaCutoverPlanAcquire>(value).expireIn));
  }
  EXPECT(rt::deepEqual(cutover, rt::jsonParse(R"([300000,"unavailable","concurrent_change"])")));
  EXPECT(result.acquired == (std::vector<bool>{true, false}));
  std::vector<std::string> guards;
  for (const kernelV2::KernelPolicyWindowGuard& value : result.policyGuard) guards.push_back(kindOf(value));
  EXPECT(guards == (std::vector<std::string>{"reclaim", "proceed", "abort", "proceed"}));
  EXPECT(result.deletePreconditions == (std::vector<std::string>{"absent", "conflict", "proceed"}));
  EXPECT(result.resetRequested == (std::vector<bool>{false, true, true}));
  EXPECT(result.policyTiming == (kernelV2::KernelPolicyTiming{.windowMs = 60'000, .expiresAtMs = -1}));
  EXPECT_EQ(result.policyWrite.policy.created_at_ms, 0.0);
  EXPECT_EQ(result.policyWrite.window.reserved_requests, 0.0);
  EXPECT_EQ(result.policyWrite.window.applied_window_ms, 120'000.0);
  EXPECT_EQ(result.policyDelete[0].created_at_ms, 10'000'000.0);
  EXPECT_EQ(result.policyDelete[0].reserved_requests, 1.0);
  EXPECT_EQ(result.policyDelete[1].created_at_ms, 10'000'004.0);
  EXPECT_EQ(result.policyDelete[1].reserved_requests, 0.0);
  bool everyReplacement = true;
  for (const KernelQuotaEffectiveWindow& value : result.replacement) everyReplacement = everyReplacement && value.needsWrite && value.window.reserved_requests == 0;
  EXPECT(everyReplacement);
}

namespace {
  using uos::tests::helpers::counting_kv::CountingKv;
  namespace quota_v2 = uos::kernel::quota_v2;
  namespace quota_reserve = uos::kernel::quota_reserve;

  class KernelConflictKv;

  class KernelConflictAtomicOperation final : public rt::ForwardingAtomicOperation {
  public:
    KernelConflictAtomicOperation(KernelConflictKv& owner, std::unique_ptr<rt::AtomicOperation> operation)
        : rt::ForwardingAtomicOperation(std::move(operation)), owner_(owner) {}
    using rt::ForwardingAtomicOperation::check;
    // `operation.check = (entry) => { checks.push(entry.key); return check(entry); }`
    rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
      if (!items.empty()) checks_.push_back(items.front().key);
      return rt::ForwardingAtomicOperation::check(std::move(items));
    }
    rt::Task<rt::KvCommitResult> commit() override;

  private:
    KernelConflictKv& owner_;
    std::vector<rt::KvKey> checks_;
  };

  /** Forces real opaque-revision CAS loss without changing the existing fixture or harness. */
  class KernelConflictKv final : public rt::ForwardingKv {
  public:
    KernelConflictKv() : KernelConflictKv(std::make_shared<CountingKv>()) {}
    double conflicts = 0;
    bool changeDefault = false;
    std::vector<std::vector<rt::KvKey>> checkedKeys;
    void seed(const rt::KvKey& key, const rt::KvValue& value) { counting_->seed(key, value); }
    const CountingKv& counting() const { return *counting_; }
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<KernelConflictAtomicOperation>(*this, rt::ForwardingKv::atomic()); }

  private:
    explicit KernelConflictKv(std::shared_ptr<CountingKv> counting) : rt::ForwardingKv(*counting), counting_(std::move(counting)) {}
    std::shared_ptr<CountingKv> counting_;
  };

  // `operation.commit = async () => { ... }`
  rt::Task<rt::KvCommitResult> KernelConflictAtomicOperation::commit() {
    owner_.checkedKeys.push_back(checks_);
    if (owner_.changeDefault) {
      owner_.changeDefault = false;
      owner_.seed(uos::defaults::DEFAULT_KERNEL_POLICY_LIMIT_KEY, rt::KvValue(0));
    }
    if (owner_.conflicts > 0) {
      owner_.conflicts -= 1;
      co_return rt::KvCommitResult{};
    }
    rt::Task<rt::KvCommitResult> committing = rt::ForwardingAtomicOperation::commit();
    rt::KvCommitResult result = co_await std::move(committing);
    co_return result;
  }
}

ASYNC_TEST("native kernel wrapper re-reads a racing default and checks the absent repo plus all default guards") {
  const auto kv = std::make_shared<KernelConflictKv>();
  kv->seed(uos::defaults::DEFAULT_KERNEL_POLICY_LIMIT_KEY, rt::KvValue(1));
  kv->seed(uos::defaults::DEFAULT_KERNEL_POLICY_WINDOW_KEY, rt::KvValue(60'000));
  kv->changeDefault = true;
  quota_reserve::ReserveKernelUsageLimitOptions options;
  options.kv = std::shared_ptr<rt::Kv>(kv);
  auto reserving = quota_reserve::reserveEffectiveKernelUsageLimit("synthetic", "repo", "request", "responses", options);
  const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
  if (const auto* admitted = std::get_if<quota_v2::KernelQuotaReservationDecisionOk>(&decision)) {
    rt::Promise<void> cleanup = admitted->reservation.release(std::string("test_cleanup"));
    co_await cleanup.wait();
    ASSERT(false && "Racing zero default must refuse");
  }
  EXPECT_EQ(std::get<quota_v2::KernelQuotaReservationDecisionNotOk>(decision).response.status(), 429);
  ASSERT(!kv->checkedKeys.empty());
  // `key.slice(3, 4)[0] ?? key[1]`
  std::vector<std::string> checked;
  for (const rt::KvKey& key : kv->checkedKeys[0]) checked.push_back(key.size() > 3 ? key[3].toString() : key[1].toString());
  const std::vector<std::string> expected{"org_policy",  "org_window", "org_reservation", "repo_policy", "kernel_policy_limit_requests", "kernel_policy_window_ms",
                                          "default_window_cutover"};
  EXPECT(checked == expected);
  EXPECT_EQ(kv->checkedKeys.size(), std::size_t{1});
  bool reservationStored = false;
  for (const auto& [encoded, entry] : kv->counting().entries) {
    for (const rt::KvKeyPart& part : entry.key) reservationStored = reservationStored || (part.isString() && part.string() == "org_reservation");
  }
  EXPECT_EQ(reservationStored, false);
}

ASYNC_TEST("native kernel wrapper retains three CAS attempts and leaves all rows absent after conflict exhaustion") {
  const auto kv = std::make_shared<KernelConflictKv>();
  kv->seed(uos::defaults::DEFAULT_KERNEL_POLICY_LIMIT_KEY, rt::KvValue(1));
  kv->seed(uos::defaults::DEFAULT_KERNEL_POLICY_WINDOW_KEY, rt::KvValue(60'000));
  kv->conflicts = 3;
  quota_reserve::ReserveKernelUsageLimitOptions options;
  options.kv = std::shared_ptr<rt::Kv>(kv);
  auto reserving = quota_reserve::reserveKernelOrgUsageLimit("synthetic", "request", "responses", options);
  const quota_v2::KernelQuotaReservationDecision decision = co_await std::move(reserving);
  if (const auto* admitted = std::get_if<quota_v2::KernelQuotaReservationDecisionOk>(&decision)) {
    rt::Promise<void> cleanup = admitted->reservation.release(std::string("test_cleanup"));
    co_await cleanup.wait();
    ASSERT(false && "Exhausted CAS must refuse");
  }
  EXPECT_EQ(std::get<quota_v2::KernelQuotaReservationDecisionNotOk>(decision).response.status(), 503);
  EXPECT_EQ(kv->checkedKeys.size(), std::size_t{3});
  EXPECT_EQ(kv->counting().entries.size(), std::size_t{2});
}
