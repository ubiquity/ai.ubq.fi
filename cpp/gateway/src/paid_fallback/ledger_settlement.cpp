#include "uos/paid_fallback/ledger_settlement.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/api_keys.hpp"
#include "uos/authority/native_paid.hpp"
#include "uos/kv.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/paid_fallback/ledger_stats.hpp"
#include "uos/paid_fallback/rollups.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"

#include <cstddef>
#include <type_traits>
#include <utility>
#include <variant>

namespace uos::paid_fallback::ledger_settlement {
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
    using rt::record::Reader;
    using rt::record::Writer;

    rt::Task<rt::KvEntryMaybe<>> readWithStore(const std::shared_ptr<rt::Kv>& kv, rt::KvKey key, rt::KvGetOptions options) {
      auto read = kv->get(std::move(key), options);
      return rt::detail::keepAlive(kv, std::move(read));
    }
    namespace contracts = authority::native_paid_contracts;
    namespace native_paid = authority::native_paid;
    using contracts::NativePaidProviderLog;
    using contracts::NativePaidRow;
    using contracts::PaidFallbackPendingV3;
    using contracts::PaidFallbackReconciliationGateV3;
    using contracts::PaidFallbackReconciliationLeaseV3;
    using contracts::PaidFallbackUsageRollup;
    using ledger_state::MAX_CAS_ATTEMPTS;
    using ledger_state::RECONCILIATION_LEASE_MS;
    using ::uos::types::PaidFallbackRequestV3;
    using ::uos::types::PaidFallbackWindowV3;

    // `{ consistency: "strong" }`
    rt::KvGetOptions strongRead() { return rt::KvGetOptions{.consistency = rt::KvConsistencyLevel::strong}; }

    // A stored KV value as the TS reads it through property access: what JSON can hold is kept, an
    // undefined member is absent, an undefined or hole element is null, and every other part (bigint,
    // KvU64, Date, Map, buffers, ...) becomes `{}`, an object no number/string/boolean/null check accepts.

    // `entry.value` of a read: null for an absent row.

    // A typed row (`kv.get<T>`): std::nullopt for null; the record's strict value_to otherwise.
    template <class T> std::optional<T> storedRow(const rt::KvValue& value) { return rt::kvValueAs<std::optional<T>>(value); }

    // `Readonly<{ value: PaidFallbackReconciliationGateV3 | null }>`: the gate helpers validate the
    // stored value themselves, so they read it raw (a malformed gate counts as absent).
    NativePaidRow<json::value> gateRow(const rt::KvEntryMaybe<>& entry) { return NativePaidRow<json::value>{.value = rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS), .versionstamp = entry.versionstamp}; }

    // The gate of a typed snapshot: the planner reads it only through paidFallbackReconciliationGateDueNow,
    // which treats a value isPaidFallbackReconciliationGate rejects as absent.
    NativePaidRow<PaidFallbackReconciliationGateV3> typedGateRow(const rt::KvEntryMaybe<>& entry) {
      const json::value view = rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS);
      std::optional<PaidFallbackReconciliationGateV3> gate;
      if (native_paid::isPaidFallbackReconciliationGate(view)) gate = json::value_to<PaidFallbackReconciliationGateV3>(view);
      return NativePaidRow<PaidFallbackReconciliationGateV3>{.value = std::move(gate), .versionstamp = entry.versionstamp};
    }

    template <class T> NativePaidRow<T> typedRow(const rt::KvEntryMaybe<>& entry, std::optional<T> value) {
      return NativePaidRow<T>{.value = std::move(value), .versionstamp = entry.versionstamp};
    }

    // `kv.get<PaidFallbackReconciliationLeaseV3>` read by paidFallbackLeaseActiveV3 (`!!lease &&
    // lease.expires_at_ms > now`) and paidFallbackLeaseOwnedV3 (`lease?.token === token`), which do
    // not validate it: a truthy value is a lease whose expiry compares as Number(expires_at_ms) and
    // whose token matches only when it is that string (a non-string token reads as "", which no
    // minted UUID equals).
    std::optional<PaidFallbackReconciliationLeaseV3> storedLease(const rt::KvValue& value) {
      if (!value.truthy()) return std::nullopt;
      const json::value view = rt::storedJsonView(value, STORED_JSON_VIEW_OPTIONS);
      const std::optional<std::string_view> token = rt::getString(rt::get(view, "token"));
      return PaidFallbackReconciliationLeaseV3{.token = token ? std::string(*token) : std::string(), .expires_at_ms = rt::toNumber(rt::get(view, "expires_at_ms"))};
    }

    // `isPaidFallbackUsageRollup(entry.value) ? entry.value : null`
    std::optional<PaidFallbackUsageRollup> storedRollup(const rt::KvValue& value) {
      const json::value view = rt::storedJsonView(value, STORED_JSON_VIEW_OPTIONS);
      if (!native_paid::isPaidFallbackUsageRollup(view)) return std::nullopt;
      return json::value_to<PaidFallbackUsageRollup>(view);
    }

    rt::KvValue kvValueOf(const json::value& value) { return rt::KvValue(value); }

    /** Clears a stale pending marker for a row that can no longer be billed. */
    rt::Task<bool> clearStalePendingMarkerV3(std::shared_ptr<rt::Kv> kv, rt::KvKey pendingKey, rt::KvEntryMaybe<> pendingEntry, bool pendingPresent, rt::KvKey gateKey,
                                             rt::KvEntryMaybe<> gateEntry, double now) {
      if (!pendingPresent) co_return true;
      const std::unique_ptr<rt::AtomicOperation> cleanup = kv->atomic();
      cleanup->check(pendingEntry).delete_(pendingKey);
      cleanup->check(gateEntry);
      cleanup->set(gateKey, kvValueOf(json::value_from(native_paid::paidFallbackReconciliationGateDueNow(gateRow(gateEntry), now))));
      const rt::KvCommitResult result = co_await cleanup->commit();
      co_return result.ok;
    }

    /** The usage rollup one settlement folds into. */
    struct SettlementRollupV3 {
      rt::KvKey rollupKey;
      rt::KvEntryMaybe<> rollupEntry;
      std::optional<PaidFallbackUsageRollup> existingRollup;
      PaidFallbackUsageRollup nextRollup;
      /** False when the model or provider label is empty, so no rollup is written. */
      bool foldIntoRollup;
    };

    /** Reads and merges the usage rollup this settlement belongs to. */
    rt::Task<SettlementRollupV3> prepareSettlementRollupV3(std::shared_ptr<rt::Kv> kv, PaidFallbackRequestV3 request, NativePaidProviderLog providerLog, double spend,
                                                           std::string correlation, double now) {
      const contracts::NativePaidRollupIdentity identity = native_paid::paidFallbackRequestRollupIdentity(request, correlation);
      rt::KvKey rollupKey = native_paid::paidFallbackUsageRollupKey(identity.bucketStartAtMs, identity.model, identity.provider, identity.shard);
      const rt::KvGetOptions strong = strongRead();
      rt::KvEntryMaybe<> rollupEntry = co_await kv->get(rollupKey, strong);
      std::optional<PaidFallbackUsageRollup> existingRollup = storedRollup(rollupEntry.value);
      native_paid::PaidFallbackSettlementRollupV3Result plan = native_paid::paidFallbackSettlementRollupV3(request, providerLog, spend, correlation, existingRollup, now);
      co_return SettlementRollupV3{
        .rollupKey = std::move(rollupKey),
        .rollupEntry = std::move(rollupEntry),
        .existingRollup = std::move(existingRollup),
        .nextRollup = std::move(plan.rollup),
        .foldIntoRollup = plan.foldIntoRollup,
      };
    }

    /**
     * Best-effort daily growth sample for one committed settlement. The recorder
     * swallows its own failures, so this only measures the two records: a lost
     * sample must never fail a settlement that already committed.
     */
    rt::Task<void> recordSettlementGrowthV3(std::shared_ptr<rt::Kv> kv, PaidFallbackRequestV3 settledRow, SettlementRollupV3 rollup, double now) {
      ledger_stats::PaidFallbackLedgerSettlementStatsInput input{
        .settledRowBytes = ledger_stats::estimatePaidFallbackRecordBytes(json::value_from(settledRow)),
        .rollupBytes = rollup.foldIntoRollup ? std::optional<double>(ledger_stats::estimatePaidFallbackRecordBytes(json::value_from(rollup.nextRollup))) : std::nullopt,
        .nowMs = now,
      };
      const rt::Promise<bool> recording(ledger_stats::recordPaidFallbackLedgerSettlementStats(kv, std::move(input)));
      co_await recording.wait();
    }

    struct SettlementCommitV3 {
      rt::KvKey requestKey;
      rt::KvKey pendingKey;
      rt::KvKey windowKey;
      rt::KvEntryMaybe<> requestEntry;
      rt::KvEntryMaybe<> pendingEntry;
      rt::KvEntryMaybe<> gateEntry;
      rt::KvEntryMaybe<> windowEntry;
      contracts::NativePaidSettlementPlan plan;
      SettlementRollupV3 rollup;
      double now;
    };

    rt::Task<bool> commitPaidFallbackSettlementV3(std::shared_ptr<rt::Kv> kv, std::shared_ptr<const SettlementCommitV3> input) {
      const rt::KvKey gateKey = native_paid::paidFallbackReconciliationGateV3Key();
      const PaidFallbackRequestV3& settledRow = input->plan.request;
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(input->requestEntry)
        .check(input->pendingEntry)
        .set(input->requestKey, kvValueOf(json::value_from(settledRow)),
             rt::KvSetOptions{
               .expireIn = input->plan.requestExpireIn,
             })
        .delete_(input->pendingKey);
      if (input->rollup.foldIntoRollup) {
        atomic->check(input->rollup.rollupEntry).set(input->rollup.rollupKey, kvValueOf(json::value_from(input->rollup.nextRollup)));
      }
      atomic->check(input->gateEntry);
      // Settlement removes billable work. Keep the gate due and version it so a
      // concurrent recompute cannot resurrect this marker's stale future time.
      atomic->set(gateKey, kvValueOf(json::value_from(input->plan.gate)));
      if (input->plan.window) {
        atomic->check(input->windowEntry)
          .set(input->windowKey, kvValueOf(json::value_from(*input->plan.window)), rt::KvSetOptions{.expireIn = native_paid::windowExpireIn(*input->plan.window, input->now)});
      }
      const rt::KvCommitResult result = co_await atomic->commit();
      co_return result.ok;
    }

    SettlePaidFallbackRequestV3Result unsettled(std::optional<double> retryDelayMs) {
      return SettlePaidFallbackRequestV3Result{.settled = false, .retry_delay_ms = retryDelayMs};
    }
  }

  rt::Task<std::optional<PaidFallbackReconciliationLeaseV3>> acquireReconciliationLease(std::shared_ptr<rt::Kv> kv, std::string keyId, double now) {
    const rt::KvKey key = native_paid::paidFallbackReconciliationLeaseV3Key(keyId);
    const rt::KvGetOptions strong = strongRead();
    for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      const rt::KvEntryMaybe<> entry = co_await kv->get(key, strong);
      if (native_paid::paidFallbackLeaseActiveV3(storedLease(entry.value), now)) co_return std::nullopt;
      PaidFallbackReconciliationLeaseV3 lease = native_paid::paidFallbackReconciliationLeaseV3(rt::crypto::randomUUID(), now);
      const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(entry).set(key, kvValueOf(json::value_from(lease)),
                                  rt::KvSetOptions{
                                    .expireIn = RECONCILIATION_LEASE_MS,
                                  });
      const rt::KvCommitResult commit = co_await operation->commit();
      if (commit.ok) co_return lease;
    }
    co_return std::nullopt;
  }

  rt::Task<void> releaseReconciliationLease(std::shared_ptr<rt::Kv> kv, std::string keyId, PaidFallbackReconciliationLeaseV3 lease) {
    const rt::KvKey key = native_paid::paidFallbackReconciliationLeaseV3Key(keyId);
    const rt::KvGetOptions strong = strongRead();
    const rt::KvEntryMaybe<> entry = co_await kv->get(key, strong);
    if (!native_paid::paidFallbackLeaseOwnedV3(storedLease(entry.value), lease.token)) co_return;
    const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
    operation->check(entry).delete_(key);
    co_await operation->commit();
  }

  rt::Task<void> _expeditePaidFallbackReconciliationV3(contracts::PaidFallbackAdmissionV3 reservation, double now) {
    const rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
    const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
    if (!kv) co_return;
    const rt::KvKey pendingKey = native_paid::paidFallbackPendingV3Key(reservation.key_id, reservation.request_id);
    const rt::KvKey gateKey = native_paid::paidFallbackReconciliationGateV3Key();
    const rt::KvGetOptions strong = strongRead();
    for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      const rt::Promise<rt::KvEntryMaybe<>> pendingRead(kv->get(pendingKey, strong));
      const rt::Promise<rt::KvEntryMaybe<>> gateRead(kv->get(gateKey, strong));
      auto reads = rt::all(pendingRead, gateRead);
      auto readEntries = co_await reads.wait();
      auto& [pending, gateEntry] = readEntries;
      const std::optional<PaidFallbackPendingV3> pendingValue = storedRow<PaidFallbackPendingV3>(pending.value);
      if (!pendingValue) co_return;
      const bool markerNeedsExpedite = pendingValue->next_reconciliation_at_ms > now;
      const bool gateNeedsArm = native_paid::paidFallbackReconciliationGateNeedsArm(gateRow(gateEntry), now);
      if (!markerNeedsExpedite && !gateNeedsArm) co_return;
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(pending).check(gateEntry);
      if (markerNeedsExpedite) {
        // `{ ...pending.value, next_reconciliation_at_ms: now }`, merged on the stored object.
        rt::KvValue expedited = pending.value;
        expedited.asObject().set("next_reconciliation_at_ms", rt::KvValue(now));
        atomic->set(pendingKey, std::move(expedited));
      }
      // A real expedite must version-bump the gate even when it is already due;
      // otherwise an overlapping recompute can overwrite the new marker with a
      // stale future timestamp.
      atomic->set(gateKey, kvValueOf(json::value_from(native_paid::paidFallbackReconciliationGateDueNow(gateRow(gateEntry), now))));
      const rt::KvCommitResult result = co_await atomic->commit();
      if (result.ok) {
        co_return;
      }
    }
    throw rt::JsError("Paid fallback reconciliation scheduling changed concurrently.");
  }

  rt::Task<std::optional<double>> deferPaidFallbackReconciliationV3(std::shared_ptr<rt::Kv> kv, std::string keyId, std::string requestId, double now) {
    const rt::KvKey requestKey = native_paid::paidFallbackRequestV3Key(keyId, requestId);
    const rt::KvKey pendingKey = native_paid::paidFallbackPendingV3Key(keyId, requestId);
    const rt::KvGetOptions strong = strongRead();
    for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      const rt::Promise<rt::KvEntryMaybe<>> requestRead(readWithStore(kv, requestKey, strong));
      const rt::Promise<rt::KvEntryMaybe<>> pendingRead(readWithStore(kv, pendingKey, strong));
      auto reads = rt::all(requestRead, pendingRead);
      auto readEntries = co_await reads.wait();
      auto& [requestEntry, pendingEntry] = readEntries;
      const contracts::NativePaidDeferralPlan plan =
        native_paid::planPaidFallbackDeferralV3(storedRow<PaidFallbackRequestV3>(requestEntry.value), storedRow<PaidFallbackPendingV3>(pendingEntry.value), now);
      if (std::holds_alternative<contracts::NativePaidDeferralPlanNoop>(plan)) co_return std::nullopt;
      if (std::holds_alternative<contracts::NativePaidDeferralPlanCleanup>(plan)) {
        const rt::KvKey gateKey = native_paid::paidFallbackReconciliationGateV3Key();
        const rt::KvEntryMaybe<> gateEntry = co_await kv->get(gateKey, strong);
        const std::unique_ptr<rt::AtomicOperation> cleanup = kv->atomic();
        cleanup->check(pendingEntry).delete_(pendingKey);
        cleanup->check(gateEntry);
        cleanup->set(gateKey, kvValueOf(json::value_from(native_paid::paidFallbackReconciliationGateDueNow(gateRow(gateEntry), now))));
        const rt::KvCommitResult cleanupResult = co_await cleanup->commit();
        if (cleanupResult.ok) co_return std::nullopt;
        continue;
      }
      const contracts::NativePaidDeferralPlanWrite& write = std::get<contracts::NativePaidDeferralPlanWrite>(plan);
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(requestEntry)
        .check(pendingEntry)
        .set(requestKey, kvValueOf(json::value_from(write.request)), rt::KvSetOptions{.expireIn = write.requestExpireIn})
        .set(pendingKey, kvValueOf(json::value_from(write.pending)));
      const rt::KvCommitResult result = co_await atomic->commit();
      if (result.ok) co_return write.delay;
    }
    throw rt::JsError("Paid fallback reconciliation deferral changed concurrently.");
  }

  rt::Task<SettlePaidFallbackRequestV3Result> settlePaidFallbackRequestV3(std::shared_ptr<rt::Kv> kv, std::string keyId, std::string requestId,
                                                                          NativePaidProviderLog providerLog, double now) {
    rt::Task<SettlePaidFallbackRequestV3Result> settling =
      settlePaidFallbackRequestV3(std::move(kv), std::move(keyId), std::move(requestId), std::move(providerLog), now, std::string("provider_request_id"), std::nullopt);
    SettlePaidFallbackRequestV3Result result = co_await std::move(settling);
    co_return result;
  }

  rt::Task<SettlePaidFallbackRequestV3Result> settlePaidFallbackRequestV3(std::shared_ptr<rt::Kv> kv, std::string keyId, std::string requestId,
                                                                          NativePaidProviderLog providerLog, double now, contracts::NativePaidCorrelation correlation) {
    rt::Task<SettlePaidFallbackRequestV3Result> settling =
      settlePaidFallbackRequestV3(std::move(kv), std::move(keyId), std::move(requestId), std::move(providerLog), now, std::move(correlation), std::nullopt);
    SettlePaidFallbackRequestV3Result result = co_await std::move(settling);
    co_return result;
  }

  rt::Task<SettlePaidFallbackRequestV3Result> settlePaidFallbackRequestV3(std::shared_ptr<rt::Kv> kv, std::string keyId, std::string requestId,
                                                                          NativePaidProviderLog providerLog, double now, contracts::NativePaidCorrelation correlation,
                                                                          std::optional<double> surplusBuyerCostMicro) {
    rt::KvKey requestKey = native_paid::paidFallbackRequestV3Key(keyId, requestId);
    rt::KvKey pendingKey = native_paid::paidFallbackPendingV3Key(keyId, requestId);
    const rt::KvKey gateKey = native_paid::paidFallbackReconciliationGateV3Key();
    const rt::KvGetOptions strong = strongRead();
    for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      const rt::Promise<rt::KvEntryMaybe<>> requestRead(readWithStore(kv, requestKey, strong));
      const rt::Promise<rt::KvEntryMaybe<>> pendingRead(readWithStore(kv, pendingKey, strong));
      const rt::Promise<rt::KvEntryMaybe<>> gateRead(readWithStore(kv, gateKey, strong));
      auto reads = rt::all(requestRead, pendingRead, gateRead);
      auto readEntries = co_await reads.wait();
      auto& [requestEntry, pendingEntry, gateEntry] = readEntries;
      const std::optional<PaidFallbackRequestV3> request = storedRow<PaidFallbackRequestV3>(requestEntry.value);
      const std::optional<PaidFallbackPendingV3> pending = storedRow<PaidFallbackPendingV3>(pendingEntry.value);
      contracts::NativePaidSettlementMetadata metadata{
        .correlation = correlation,
        .microcreditsPerCredit = api_keys::MICROCREDITS_PER_CREDIT,
        .surplusBuyerCostMicro = surplusBuyerCostMicro,
      };
      const contracts::NativePaidSettlementEvaluation evaluation = native_paid::evaluatePaidFallbackSettlementV3(request, pending, providerLog, metadata);
      if (std::holds_alternative<contracts::NativePaidSettlementEvaluationCleanup>(evaluation)) {
        const rt::Promise<bool> clearing(clearStalePendingMarkerV3(kv, pendingKey, pendingEntry, pending.has_value(), gateKey, gateEntry, now));
        const bool cleared = co_await clearing.wait();
        if (!cleared) continue;
        co_return unsettled(std::nullopt);
      }
      if (std::holds_alternative<contracts::NativePaidSettlementEvaluationNoop>(evaluation) || !request) co_return unsettled(std::nullopt);
      if (std::holds_alternative<contracts::NativePaidSettlementEvaluationDefer>(evaluation)) {
        const rt::Promise<std::optional<double>> deferring(deferPaidFallbackReconciliationV3(kv, keyId, requestId, now));
        const std::optional<double> retryDelayMs = co_await deferring.wait();
        co_return unsettled(retryDelayMs);
      }
      const double spend = std::get<contracts::NativePaidSettlementEvaluationSettle>(evaluation).spend;
      rt::KvKey windowKey = native_paid::paidFallbackWindowV3Key(keyId, request->window_reset_at_ms);
      rt::KvEntryMaybe<> windowEntry = co_await kv->get(windowKey, strong);
      const rt::Promise<SettlementRollupV3> preparing(prepareSettlementRollupV3(kv, *request, providerLog, spend, correlation, now));
      SettlementRollupV3 rollup = co_await preparing.wait();
      const contracts::NativePaidSettlementSnapshot snapshot{
        .request = typedRow(requestEntry, request),
        .pending = typedRow(pendingEntry, pending),
        .gate = typedGateRow(gateEntry),
        .window = typedRow(windowEntry, storedRow<PaidFallbackWindowV3>(windowEntry.value)),
        .rollup = typedRow(rollup.rollupEntry, rollup.existingRollup),
      };
      contracts::NativePaidSettlementPlan plan = native_paid::planPaidFallbackSettlementRowsV3(snapshot, providerLog, spend, rollup.foldIntoRollup, now);
      const PaidFallbackRequestV3 settledRow = plan.request;
      auto input = std::make_shared<const SettlementCommitV3>(SettlementCommitV3{
        .requestKey = requestKey,
        .pendingKey = pendingKey,
        .windowKey = std::move(windowKey),
        .requestEntry = requestEntry,
        .pendingEntry = pendingEntry,
        .gateEntry = gateEntry,
        .windowEntry = std::move(windowEntry),
        .plan = std::move(plan),
        .rollup = rollup,
        .now = now,
      });
      const rt::Promise<bool> committing(commitPaidFallbackSettlementV3(kv, std::move(input)));
      const bool committed = co_await committing.wait();
      if (committed) {
        // Best-effort growth instrumentation: the daily counters must never fail
        // a settlement that already committed.
        const rt::Promise<void> recording(recordSettlementGrowthV3(kv, settledRow, std::move(rollup), now));
        co_await recording.wait();
        co_return SettlePaidFallbackRequestV3Result{.settled = true, .retry_delay_ms = std::nullopt};
      }
    }
    throw rt::JsError("Paid fallback settlement changed concurrently.");
  }

  rt::Task<bool> settlePaidFallbackUsageV3(contracts::PaidFallbackAdmissionV3 reservation, PaidFallbackUsageSettlementV3 usage) {
    const rt::Promise<std::shared_ptr<rt::Kv>> opening = ::uos::kv::getKv();
    const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
    if (!kv) co_return false;
    NativePaidProviderLog providerLog{
      .request_id = usage.settlement_request_id,
      .quota = usage.provider_quota,
      .prompt_tokens = usage.input_tokens,
      .cached_prompt_tokens = usage.cached_input_tokens.toOptional(),
      .cache_write_prompt_tokens = std::nullopt,
      .completion_tokens = usage.output_tokens,
      .model = usage.model,
      .created_at = rt::Math::max(0.0, rt::Math::trunc(usage.created_at_ms / 1'000)),
    };
    const rt::Promise<SettlePaidFallbackRequestV3Result> settling(settlePaidFallbackRequestV3(kv, reservation.key_id, reservation.request_id, std::move(providerLog),
                                                                                              rt::dateNow(), std::string("surplus_reservation"), usage.buyer_cost_micro));
    const SettlePaidFallbackRequestV3Result result = co_await settling.wait();
    co_return result.settled;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SettlePaidFallbackRequestV3Result& value) {
    out = Writer().required("settled", value.settled).orNull("retry_delay_ms", value.retry_delay_ms).done();
  }

  SettlePaidFallbackRequestV3Result tag_invoke(const json::value_to_tag<SettlePaidFallbackRequestV3Result>&, const json::value& value) {
    const Reader in(value, "SettlePaidFallbackRequestV3Result");
    return {
      .settled = in.required<bool>("settled"),
      .retry_delay_ms = in.orNull<double>("retry_delay_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackUsageSettlementV3& value) {
    out = Writer()
            .required("settlement_request_id", value.settlement_request_id)
            .required("provider_quota", value.provider_quota)
            .required("input_tokens", value.input_tokens)
            .nullable("cached_input_tokens", value.cached_input_tokens)
            .required("output_tokens", value.output_tokens)
            .required("model", value.model)
            .required("created_at_ms", value.created_at_ms)
            .orUndefined("buyer_cost_micro", value.buyer_cost_micro)
            .done();
  }

  PaidFallbackUsageSettlementV3 tag_invoke(const json::value_to_tag<PaidFallbackUsageSettlementV3>&, const json::value& value) {
    const Reader in(value, "PaidFallbackUsageSettlementV3");
    return {
      .settlement_request_id = in.required<std::string>("settlement_request_id"),
      .provider_quota = in.required<double>("provider_quota"),
      .input_tokens = in.required<double>("input_tokens"),
      .cached_input_tokens = in.nullable<double>("cached_input_tokens"),
      .output_tokens = in.required<double>("output_tokens"),
      .model = in.required<std::string>("model"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .buyer_cost_micro = in.orUndefined<double>("buyer_cost_micro"),
    };
  }
}
