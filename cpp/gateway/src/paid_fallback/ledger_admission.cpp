#include "uos/paid_fallback/ledger_admission.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/api_keys.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/ledger_backfill.hpp"
#include "uos/paid_fallback/ledger_settlement.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"

#include <cstddef>
#include <type_traits>
#include <utility>
#include <vector>

namespace uos::paid_fallback::ledger_admission {
  namespace {
    constexpr rt::StoredJsonViewOptions STORED_JSON_VIEW_OPTIONS{
      .undefinedMember = rt::StoredJsonViewOptions::UndefinedView::omit,
      .undefinedElement = rt::StoredJsonViewOptions::UndefinedView::null,
      .hole = rt::StoredJsonViewOptions::UndefinedView::null,
      .topLevelUndefined = rt::StoredJsonViewOptions::UndefinedView::null,
      .bigint = rt::StoredJsonViewOptions::BigIntView::emptyObject,
      .kvU64Members = false,
      .typedArrayMembers = false,
    };
    namespace contracts = authority::native_paid_contracts;
    namespace native_paid = authority::native_paid;
    using contracts::NativePaidRow;
    using contracts::PaidFallbackAdmissionV3;
    using contracts::PaidFallbackDeletionGuardV3;
    using contracts::PaidFallbackReconciliationGateV3;
    using ledger_state::AdmissionInput;
    using ledger_state::MAX_CAS_ATTEMPTS;
    using ::uos::types::PaidFallbackRequestV3;
    using ::uos::types::PaidFallbackWindowV3;

    // `{ consistency: "strong" }`
    rt::KvGetOptions strongRead() { return rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong}; }

    rt::KvListOptions strongList() {
      rt::KvListOptions options;
      options.consistency = rt::KvConsistencyLevel::strong;
      return options;
    }

    rt::KvListSelector prefixSelector(rt::KvKey prefix) {
      rt::KvListSelector selector;
      selector.prefix = std::move(prefix);
      return selector;
    }

    // A stored KV value as the TS reads it through property access: what JSON can hold is kept, an
    // undefined member is absent, an undefined or hole element is null, and every other part (bigint,
    // KvU64, Date, Map, buffers, ...) becomes `{}`, an object no number/string/boolean/null check accepts.

    // `entry.value` of a read: null for an absent row.

    // A typed row (`kv.get<T>`): std::nullopt for null; the record's strict value_to otherwise.
    template <class T> std::optional<T> storedRow(const rt::KvValue& value) { return rt::kvValueAs<std::optional<T>>(value); }

    template <class T> NativePaidRow<T> typedRow(const rt::KvEntryMaybe<>& entry, std::optional<T> value) {
      return NativePaidRow<T>{.value = std::move(value), .versionstamp = entry.versionstamp};
    }

    // `Readonly<{ value: PaidFallbackReconciliationGateV3 | null }>` read raw: the gate helpers
    // validate the stored value themselves (a malformed gate counts as absent).
    NativePaidRow<json::value> gateRow(const rt::KvEntryMaybe<>& entry) { return NativePaidRow<json::value>{.value = rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS), .versionstamp = entry.versionstamp}; }

    // The gate of a typed snapshot: the planner reads it only through paidFallbackReconciliationGateDueNow,
    // which treats a value isPaidFallbackReconciliationGate rejects as absent.
    NativePaidRow<PaidFallbackReconciliationGateV3> typedGateRow(const rt::KvEntryMaybe<>& entry) {
      const json::value view = rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS);
      std::optional<PaidFallbackReconciliationGateV3> gate;
      if (native_paid::isPaidFallbackReconciliationGate(view)) gate = json::value_to<PaidFallbackReconciliationGateV3>(view);
      return NativePaidRow<PaidFallbackReconciliationGateV3>{.value = std::move(gate), .versionstamp = entry.versionstamp};
    }

    // `kv.get<PaidFallbackDeletionGuardV3>`: every reader here only tests the stored value's
    // truthiness (`guard ? ... : ...`, `if (snapshot.deletionGuard.value)`), so any truthy value is a
    // guard (the local-development guard extends the record).
    std::optional<PaidFallbackDeletionGuardV3> storedDeletionGuard(const rt::KvValue& value) {
      if (!value.truthy()) return std::nullopt;
      return PaidFallbackDeletionGuardV3{.created_at_ms = rt::toNumber(rt::get(rt::storedJsonView(value, STORED_JSON_VIEW_OPTIONS), "created_at_ms"))};
    }

    rt::KvValue kvValueOf(const json::value& value) { return rt::KvValue(value); }

    rt::Task<void> deleteKvKeys(std::shared_ptr<rt::Kv> kv, std::vector<rt::KvKey> keys) {
      for (std::size_t offset = 0; offset < keys.size(); offset += 100) {
        const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
        for (std::size_t index = offset; index < keys.size() && index < offset + 100; ++index) atomic->delete_(keys[index]);
        const rt::KvCommitResult commit = co_await atomic->commit();
        if (!commit.ok) throw rt::JsError("Paid fallback state deletion changed concurrently.");
      }
    }

    // `rt::KvEntry<>` keys of a strongly consistent prefix scan.
    rt::Task<std::vector<rt::KvKey>> listKeys(std::shared_ptr<rt::Kv> kv, rt::KvKey prefix) {
      std::vector<rt::KvKey> keys;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(prefixSelector(std::move(prefix)), strongList());
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) keys.push_back(entry->key);
      co_return keys;
    }

    struct AdmissionWriteContext {
      rt::KvKey requestKey;
      rt::KvKey pendingKey;
      rt::KvKey windowKey;
      rt::KvKey gateKey;
      rt::KvEntryMaybe<> requestEntry;
      rt::KvEntryMaybe<> windowEntry;
      rt::KvEntryMaybe<> deletionGuardEntry;
      contracts::NativePaidAdmissionPlanWrite plan;
      double now;
    };

    // `Readonly<{ kind: "reserved"; reservation }> | Readonly<{ kind: "blocked"; reason }> |
    // Readonly<{ kind: "write"; context: AdmissionWriteContext }>`
    struct AdmissionDecisionWrite {
      std::shared_ptr<const AdmissionWriteContext> context;
    };
    using AdmissionDecision = std::variant<AdmitPaidFallbackV3ResultReserved, AdmitPaidFallbackV3ResultBlocked, AdmissionDecisionWrite>;

    /**
     * Commits the policy transition that unblocks a limit-exceeded window so the
     * next attempt re-reads the new policy. Returns `true` when the caller should
     * report `limit_exceeded`, `false` when the CAS lost and the attempt retries.
     */
    rt::Task<bool> commitExhaustedPolicyTransitionV3(std::shared_ptr<rt::Kv> kv, std::shared_ptr<const AdmissionInput> input, rt::KvKey windowKey,
                                                     rt::KvEntryMaybe<> windowEntry, rt::KvEntryMaybe<> deletionGuardEntry, PaidFallbackWindowV3 transitioned,
                                                     double now) {
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(windowEntry).check(deletionGuardEntry);
      if (input->policyCheck) atomic->check(*input->policyCheck);
      atomic->set(windowKey, kvValueOf(json::value_from(transitioned)), rt::KvSetOptions{.expireIn = native_paid::windowExpireIn(transitioned, now)});
      const rt::KvCommitResult transition = co_await atomic->commit();
      co_return transition.ok;
    }

    /** Reads the current rows and decides what this admission attempt should do. */
    rt::Task<std::optional<AdmissionDecision>> decidePaidFallbackAdmissionV3(std::shared_ptr<rt::Kv> kv, std::shared_ptr<const AdmissionInput> input, bool unlimited,
                                                                             std::optional<double> maximumExposure) {
      rt::KvKey requestKey = native_paid::paidFallbackRequestV3Key(input->keyId, input->requestId);
      rt::KvKey pendingKey = native_paid::paidFallbackPendingV3Key(input->keyId, input->requestId);
      rt::KvKey windowKey = native_paid::paidFallbackWindowV3Key(input->keyId, input->windowResetAtMs);
      rt::KvKey gateKey = native_paid::paidFallbackReconciliationGateV3Key();

      const rt::KvGetOptions strong = strongRead();
      const rt::Promise<rt::KvEntryMaybe<>> requestRead(kv->get(requestKey, strong));
      rt::KvEntryMaybe<> absentWindow;
      absentWindow.key = windowKey;
      absentWindow.value = rt::KvValue(nullptr);
      const rt::Promise<rt::KvEntryMaybe<>> windowRead =
        unlimited ? rt::Promise<rt::KvEntryMaybe<>>::resolve(std::move(absentWindow)) : rt::Promise<rt::KvEntryMaybe<>>(kv->get(windowKey, strong));
      const rt::KvKey deletionGuardKey = native_paid::paidFallbackDeletionGuardV3Key(input->keyId);
      const rt::Promise<rt::KvEntryMaybe<>> deletionGuardRead(kv->get(deletionGuardKey, strong));
      auto reads = rt::all(requestRead, windowRead, deletionGuardRead);
      auto readEntries = co_await reads.wait();
      auto& [requestEntry, windowEntry, deletionGuardEntry] = readEntries;
      const double now = rt::dateNow();
      contracts::NativePaidAdmissionSnapshot snapshot{
        .request = typedRow(requestEntry, storedRow<PaidFallbackRequestV3>(requestEntry.value)),
        .window = typedRow(windowEntry, storedRow<PaidFallbackWindowV3>(windowEntry.value)),
        .deletionGuard = typedRow(deletionGuardEntry, storedDeletionGuard(deletionGuardEntry.value)),
        .policyFence = std::nullopt,
      };
      if (input->policyCheck) snapshot.policyFence = contracts::NativePaidAdmissionSnapshot::PolicyFence{.versionstamp = input->policyCheck->versionstamp};
      const contracts::NativePaidAdmissionPolicy policy{.unlimited = unlimited, .maximumExposure = maximumExposure};
      contracts::NativePaidAdmissionPlan plan = native_paid::planPaidFallbackAdmissionV3(*input, snapshot, policy, now);
      if (const auto* exhausted = std::get_if<contracts::NativePaidAdmissionPlanExhaustedPolicy>(&plan)) {
        const rt::Promise<bool> transitioning(
          commitExhaustedPolicyTransitionV3(kv, input, std::move(windowKey), windowEntry, deletionGuardEntry, exhausted->window, now));
        const bool committed = co_await transitioning.wait();
        if (!committed) co_return std::nullopt;
        co_return AdmissionDecision(AdmitPaidFallbackV3ResultBlocked{.reason = "limit_exceeded"});
      }
      if (const auto* blocked = std::get_if<contracts::NativePaidAdmissionPlanBlocked>(&plan)) {
        co_return AdmissionDecision(AdmitPaidFallbackV3ResultBlocked{.reason = blocked->reason});
      }
      if (const auto* reserved = std::get_if<contracts::NativePaidAdmissionPlanReserved>(&plan)) {
        co_return AdmissionDecision(AdmitPaidFallbackV3ResultReserved{.reservation = reserved->reservation});
      }
      auto context = std::make_shared<const AdmissionWriteContext>(AdmissionWriteContext{
        .requestKey = std::move(requestKey),
        .pendingKey = std::move(pendingKey),
        .windowKey = std::move(windowKey),
        .gateKey = std::move(gateKey),
        .requestEntry = requestEntry,
        .windowEntry = windowEntry,
        .deletionGuardEntry = deletionGuardEntry,
        .plan = std::get<contracts::NativePaidAdmissionPlanWrite>(std::move(plan)),
        .now = now,
      });
      co_return AdmissionDecision(AdmissionDecisionWrite{.context = std::move(context)});
    }

    /** Writes one reservation; returns the admission, or `null` when the CAS lost. */
    rt::Task<std::optional<PaidFallbackAdmissionV3>> writePaidFallbackAdmissionV3(std::shared_ptr<rt::Kv> kv, std::shared_ptr<const AdmissionInput> input,
                                                                                  std::shared_ptr<const AdmissionWriteContext> context) {
      const AdmissionWriteContext& c = *context;
      std::optional<rt::KvEntryMaybe<>> gateEntry;
      if (c.plan.checkGate) {
        const rt::KvGetOptions strong = strongRead();
        gateEntry = co_await kv->get(c.gateKey, strong);
      }
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(c.requestEntry).check(c.deletionGuardEntry);
      if (input->policyCheck) atomic->check(*input->policyCheck);
      atomic->set(c.requestKey, kvValueOf(json::value_from(c.plan.request)), rt::KvSetOptions{.expireIn = c.plan.requestExpireIn})
        .set(c.pendingKey, kvValueOf(json::value_from(c.plan.pending)));
      if (gateEntry) {
        atomic->check(*gateEntry).set(c.gateKey, kvValueOf(json::value_from(native_paid::paidFallbackReconciliationGateDueNow(gateRow(*gateEntry), c.now))));
      }
      if (c.plan.window) {
        atomic->check(c.windowEntry).set(c.windowKey, kvValueOf(json::value_from(*c.plan.window)), rt::KvSetOptions{.expireIn = native_paid::windowExpireIn(*c.plan.window, c.now)});
      }
      const rt::KvCommitResult result = co_await atomic->commit();
      if (!result.ok) co_return std::nullopt;
      co_return c.plan.reservation;
    }

    /** Reads the reconciliation gate when a lifecycle transition must arm it. */
    rt::Task<std::optional<rt::KvEntryMaybe<>>> loadLifecycleGateEntryV3(std::shared_ptr<rt::Kv> kv, rt::KvKey gateKey, bool shouldArm) {
      if (!shouldArm) co_return std::nullopt;
      const rt::KvGetOptions strong = strongRead();
      rt::KvEntryMaybe<> entry = co_await kv->get(gateKey, strong);
      co_return entry;
    }

    rt::Task<void> releasePaidFallbackBeforeDispatchV3(PaidFallbackAdmissionV3 reservation, bool allowDispatchIntent) {
      const rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (!kv) co_return;
      const rt::KvKey requestKey = native_paid::paidFallbackRequestV3Key(reservation.key_id, reservation.request_id);
      const rt::KvKey pendingKey = native_paid::paidFallbackPendingV3Key(reservation.key_id, reservation.request_id);
      const rt::KvKey windowKey = native_paid::paidFallbackWindowV3Key(reservation.key_id, reservation.window_reset_at_ms);
      const rt::KvKey gateKey = native_paid::paidFallbackReconciliationGateV3Key();
      const rt::KvGetOptions strong = strongRead();
      for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
        const rt::Promise<rt::KvEntryMaybe<>> requestRead(kv->get(requestKey, strong));
        const rt::Promise<rt::KvEntryMaybe<>> windowRead(kv->get(windowKey, strong));
        const rt::Promise<rt::KvEntryMaybe<>> gateRead(kv->get(gateKey, strong));
        auto reads = rt::all(requestRead, windowRead, gateRead);
        auto readEntries = co_await reads.wait();
        auto& [requestEntry, windowEntry, gateEntry] = readEntries;
        const double now = rt::dateNow();
        const contracts::NativePaidReleaseSnapshot snapshot{
          .request = typedRow(requestEntry, storedRow<PaidFallbackRequestV3>(requestEntry.value)),
          .window = typedRow(windowEntry, storedRow<PaidFallbackWindowV3>(windowEntry.value)),
          .gate = typedGateRow(gateEntry),
        };
        const std::optional<contracts::NativePaidReleasePlan> plan = native_paid::planPaidFallbackReleaseV3(snapshot, allowDispatchIntent, now);
        if (!plan) co_return;
        const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
        atomic->check(requestEntry).set(requestKey, kvValueOf(json::value_from(plan->request)), rt::KvSetOptions{.expireIn = plan->requestExpireIn}).delete_(pendingKey);
        // Removing a marker always versions G, including when G is already due.
        atomic->check(gateEntry).set(gateKey, kvValueOf(json::value_from(plan->gate)));
        if (plan->window) {
          atomic->check(windowEntry).set(windowKey, kvValueOf(json::value_from(*plan->window)), rt::KvSetOptions{.expireIn = native_paid::windowExpireIn(*plan->window, now)});
        }
        const rt::KvCommitResult result = co_await atomic->commit();
        if (result.ok) co_return;
      }
      throw rt::JsError("Paid fallback release changed concurrently.");
    }
  }

  rt::Task<PaidFallbackDeletionV3> deletePaidFallbackStateV3(std::string keyId) {
    rt::Task<PaidFallbackDeletionV3> deleting = deletePaidFallbackStateV3(std::move(keyId), std::nullopt);
    PaidFallbackDeletionV3 result = co_await std::move(deleting);
    co_return result;
  }

  rt::Task<PaidFallbackDeletionV3> deletePaidFallbackStateV3(std::string keyId, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    const rt::Promise<std::shared_ptr<rt::Kv>> resolving(ledger_state::resolveKv(std::move(kvOverride)));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) co_return PaidFallbackDeletionV3Unavailable{};
    const rt::KvKey guardKey = native_paid::paidFallbackDeletionGuardV3Key(keyId);
    const rt::KvGetOptions strong = strongRead();
    for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      const rt::KvEntryMaybe<> guard = co_await kv->get(guardKey, strong);
      const std::optional<PaidFallbackDeletionGuardV3> nextGuard = native_paid::planPaidFallbackDeletionGuardV3(storedDeletionGuard(guard.value), rt::dateNow());
      if (!nextGuard) break;
      const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(guard).set(guardKey, kvValueOf(json::value_from(*nextGuard)));
      const rt::KvCommitResult commit = co_await operation->commit();
      if (commit.ok) break;
      if (attempt == MAX_CAS_ATTEMPTS - 1) {
        throw rt::JsError("Paid fallback deletion guard changed concurrently.");
      }
    }
    const rt::Promise<std::optional<contracts::PaidFallbackOutstandingV3>> reading(ledger_state::getPaidFallbackOutstandingV3(keyId, kv));
    const std::optional<contracts::PaidFallbackOutstandingV3> outstanding = co_await reading.wait();
    const native_paid::PaidFallbackDeletionPlanV3 decision = native_paid::planPaidFallbackDeletionV3(outstanding);
    if (const auto* unavailable = std::get_if<native_paid::PaidFallbackDeletionPlanV3Unavailable>(&decision)) co_return *unavailable;
    if (const auto* blocked = std::get_if<native_paid::PaidFallbackDeletionPlanV3Blocked>(&decision)) co_return *blocked;

    rt::Task<std::vector<rt::KvKey>> listingRequests = listKeys(kv, native_paid::paidFallbackRequestV3Prefix(keyId));
    const std::vector<rt::KvKey> requestKeys = co_await std::move(listingRequests);
    rt::Task<std::vector<rt::KvKey>> listingWindows = listKeys(kv, native_paid::paidFallbackWindowV3Prefix(keyId));
    const std::vector<rt::KvKey> windowKeys = co_await std::move(listingWindows);
    rt::Task<std::vector<rt::KvKey>> listingPending = listKeys(kv, native_paid::paidFallbackPendingV3Prefix(keyId));
    const std::vector<rt::KvKey> pendingKeys = co_await std::move(listingPending);
    const rt::KvKey leaseKey = native_paid::paidFallbackReconciliationLeaseV3Key(keyId);
    const rt::KvEntryMaybe<> lease = co_await kv->get(leaseKey, strong);
    std::vector<rt::KvKey> keys;
    keys.insert(keys.end(), requestKeys.begin(), requestKeys.end());
    keys.insert(keys.end(), windowKeys.begin(), windowKeys.end());
    keys.insert(keys.end(), pendingKeys.begin(), pendingKeys.end());
    if (lease.value.truthy()) keys.push_back(leaseKey);
    const rt::Promise<void> deleting(deleteKvKeys(kv, std::move(keys)));
    co_await deleting.wait();
    co_return PaidFallbackDeletionV3Deleted{
      .deleted_requests = static_cast<double>(requestKeys.size()),
      .deleted_windows = static_cast<double>(windowKeys.size()),
      .deleted_pending = static_cast<double>(pendingKeys.size()),
      .deleted_leases = lease.value.truthy() ? 1.0 : 0.0,
    };
  }

  rt::Task<AdmitPaidFallbackV3Result> admitPaidFallbackV3(AdmissionInput input) {
    const rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
    const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
    if (!kv) co_return AdmitPaidFallbackV3ResultBlocked{.reason = "invalid_policy"};
    if (!ledger_backfill::paidFallbackBackfillAttempted || ledger_backfill::paidFallbackBackfillRetryPending) ledger_backfill::schedulePaidFallbackBackfill(kv);
    const bool unlimited = input.limitMicrocredits == api_keys::PAID_FALLBACK_NO_LIMIT;
    if (native_paid::isInvalidAdmissionPolicy(input, unlimited, input.maximumExposureMicrocredits)) {
      co_return AdmitPaidFallbackV3ResultBlocked{.reason = "invalid_policy"};
    }
    const auto shared = std::make_shared<const AdmissionInput>(std::move(input));
    for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      const rt::Promise<std::optional<AdmissionDecision>> deciding(decidePaidFallbackAdmissionV3(kv, shared, unlimited, shared->maximumExposureMicrocredits));
      std::optional<AdmissionDecision> decision = co_await deciding.wait();
      if (!decision) continue;
      if (auto* reserved = std::get_if<AdmitPaidFallbackV3ResultReserved>(&*decision)) co_return std::move(*reserved);
      if (auto* blocked = std::get_if<AdmitPaidFallbackV3ResultBlocked>(&*decision)) co_return std::move(*blocked);
      const rt::Promise<std::optional<PaidFallbackAdmissionV3>> writing(
        writePaidFallbackAdmissionV3(kv, shared, std::get<AdmissionDecisionWrite>(*decision).context));
      std::optional<PaidFallbackAdmissionV3> reservation = co_await writing.wait();
      if (reservation) co_return AdmitPaidFallbackV3ResultReserved{.reservation = std::move(*reservation)};
    }
    co_return AdmitPaidFallbackV3ResultBlocked{.reason = "concurrent_update"};
  }

  rt::Task<void> updatePaidFallbackRequestV3(PaidFallbackAdmissionV3 reservation, contracts::PaidFallbackRequestLifecyclePatchV3 patch) {
    const rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
    const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
    if (!kv) co_return;
    const rt::KvKey key = native_paid::paidFallbackRequestV3Key(reservation.key_id, reservation.request_id);
    const rt::KvGetOptions strong = strongRead();
    for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      const rt::KvEntryMaybe<> entry = co_await kv->get(key, strong);
      const std::optional<PaidFallbackRequestV3> current = storedRow<PaidFallbackRequestV3>(entry.value);
      if (!current) co_return;
      const double now = rt::dateNow();
      const std::optional<contracts::NativePaidLifecyclePlan> plan = native_paid::planPaidFallbackLifecycleV3(*current, patch, now);
      if (!plan) co_return;
      const rt::KvKey gateKey = native_paid::paidFallbackReconciliationGateV3Key();
      const rt::Promise<std::optional<rt::KvEntryMaybe<>>> loading(loadLifecycleGateEntryV3(kv, gateKey, plan->readGate));
      const std::optional<rt::KvEntryMaybe<>> gateEntry = co_await loading.wait();
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(entry).set(key, kvValueOf(json::value_from(plan->request)), rt::KvSetOptions{.expireIn = plan->requestExpireIn});
      if (gateEntry) {
        atomic->check(*gateEntry);
        if (plan->dispatchBoundary || native_paid::paidFallbackReconciliationGateNeedsArm(gateRow(*gateEntry), now)) {
          atomic->set(gateKey, kvValueOf(json::value_from(native_paid::paidFallbackReconciliationGateDueNow(gateRow(*gateEntry), now))));
        }
      }
      const rt::KvCommitResult result = co_await atomic->commit();
      if (result.ok) co_return;
    }
    throw rt::JsError("Paid fallback request changed concurrently.");
  }

  rt::Task<void> releaseUndispatchedPaidFallbackV3(PaidFallbackAdmissionV3 reservation) {
    const rt::Promise<void> releasing(releasePaidFallbackBeforeDispatchV3(std::move(reservation), false));
    co_await releasing.wait();
  }

  rt::Task<void> releasePaidFallbackBeforeProviderFetchV3(PaidFallbackAdmissionV3 reservation) {
    const rt::Promise<void> releasing(releasePaidFallbackBeforeDispatchV3(std::move(reservation), true));
    co_await releasing.wait();
  }
}
