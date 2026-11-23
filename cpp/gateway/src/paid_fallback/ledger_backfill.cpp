#include "uos/paid_fallback/ledger_backfill.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/authority/native_paid.hpp"
#include "uos/paid_fallback/ledger_admission.hpp"
#include "uos/paid_fallback/ledger_settlement.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/paid_fallback/rollups.hpp"
#include "uos/provider/metered.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/record.hpp"

#include <cstddef>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace uos::paid_fallback::ledger_backfill {
  // `let paidFallbackBackfillAttempted = false; let paidFallbackBackfillRetryPending = false;`
  bool paidFallbackBackfillAttempted = false;
  bool paidFallbackBackfillRetryPending = false;

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
    namespace contracts = authority::native_paid_contracts;
    namespace native_paid = authority::native_paid;
    using contracts::NativePaidRow;
    using contracts::PaidFallbackAdmissionV3;
    using contracts::PaidFallbackBackfillLeaseV3;
    using contracts::PaidFallbackPendingV3;
    using contracts::PaidFallbackReconciliationGateV3;
    using contracts::PaidFallbackUsageRollup;
    using ledger_state::MAX_CAS_ATTEMPTS;
    using ledger_state::PAID_FALLBACK_AUTOMATIC_BACKFILL_LIMIT;
    using ledger_state::PAID_FALLBACK_BACKFILL_LEASE_MS;
    using provider::metered::MeteredTokenLogEntry;
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

    // `{ prefix, start }` (the start key is inclusive).
    rt::KvListSelector prefixSelector(rt::KvKey prefix, std::optional<rt::KvKey> start) {
      rt::KvListSelector selector;
      selector.prefix = std::move(prefix);
      selector.start = std::move(start);
      return selector;
    }

    // A stored KV value as the TS reads it through property access: what JSON can hold is kept, an
    // undefined member is absent, an undefined or hole element is null, and every other part (bigint,
    // KvU64, Date, Map, buffers, ...) becomes `{}`, an object no number/string/boolean/null check accepts.

    // `entry.value` of a read: null for an absent row.

    // A typed row (`kv.get<T>`): std::nullopt for null; the record's strict value_to otherwise.
    template <class T> std::optional<T> storedRow(const rt::KvValue& value) { return rt::kvValueAs<std::optional<T>>(value); }

    // `Readonly<{ value: PaidFallbackReconciliationGateV3 | null }>` read raw: the gate helpers
    // validate the stored value themselves (a malformed gate counts as absent).
    NativePaidRow<json::value> gateRow(const rt::KvEntryMaybe<>& entry) { return NativePaidRow<json::value>{.value = rt::storedJsonView(entry.value, STORED_JSON_VIEW_OPTIONS), .versionstamp = entry.versionstamp}; }

    // `kv.get<PaidFallbackBackfillLeaseV3>` as paidFallbackLeaseActiveV3 (`!!lease &&
    // lease.expires_at_ms > now`) and paidFallbackLeaseOwnedV3 (`lease?.token === token`) read it
    // unvalidated: a truthy value is a lease whose expiry compares as Number(expires_at_ms) and whose
    // token matches only when it is that string (a non-string token reads as "", which no minted
    // UUID equals).
    std::optional<PaidFallbackBackfillLeaseV3> storedLease(const rt::KvValue& value) {
      if (!value.truthy()) return std::nullopt;
      const json::value view = rt::storedJsonView(value, STORED_JSON_VIEW_OPTIONS);
      const std::optional<std::string_view> token = rt::getString(rt::get(view, "token"));
      return PaidFallbackBackfillLeaseV3{.token = token ? std::string(*token) : std::string(), .expires_at_ms = rt::toNumber(rt::get(view, "expires_at_ms"))};
    }

    rt::KvValue kvValueOf(const json::value& value) { return rt::KvValue(value); }

    // A key as the array value `{ request_key: requestKey }` stores.

    // `requestKey as KvKey`: a stored array element that cannot be a key part has no C++ key; the TS
    // only fails later, when the list selector encodes it.

    // `part === other` on two key parts: bytes are objects (never the same one here), -0 equals 0 and
    // NaN equals nothing.

    /** True when both KV keys hold the same parts, in order. */

    // `await kv.atomic()...commit().catch(() => {})`: the commit's failure is swallowed (its
    // `.catch` takes one more turn).
    rt::Task<void> commitIgnoringFailure(std::unique_ptr<rt::AtomicOperation> atomic) {
      try {
        const rt::Promise<rt::KvCommitResult> committing(atomic->commit());
        co_await committing.wait();
      } catch (...) {
      }
      co_await rt::microtask();
    }

    /** Persists a resume cursor; a KV outage just leaves the sweep to restart. */
    rt::Task<void> persistBackfillCursorV3(std::shared_ptr<rt::Kv> kv, rt::KvKey cursorKey, rt::KvKey requestKey) {
      rt::KvObject cursor;
      cursor.set("request_key", rt::kvValueFromKey(requestKey));
      std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->set(cursorKey, rt::KvValue(std::move(cursor)));
      rt::Task<void> committing = commitIgnoringFailure(std::move(atomic));
      co_await std::move(committing);
    }

    /** Reads a persisted resume cursor; `null` when absent or malformed. */
    rt::Task<std::optional<rt::KvKey>> loadBackfillCursorV3(std::shared_ptr<rt::Kv> kv, rt::KvKey cursorKey) {
      std::optional<rt::KvEntryMaybe<>> cursorEntry;
      try {
        const rt::Promise<rt::KvEntryMaybe<>> reading(kv->get(cursorKey));
        cursorEntry = co_await reading.wait();
      } catch (...) {
        cursorEntry.reset();
      }
      // The turn of the derived `.catch` promise.
      co_await rt::microtask();
      if (!cursorEntry || !cursorEntry->value.isObject()) co_return std::nullopt;
      const rt::KvValue* requestKey = cursorEntry->value.asObject().find("request_key");
      if (requestKey == nullptr || !requestKey->isArray()) co_return std::nullopt;
      co_return rt::kvKeyFromValue(*requestKey);
    }

    /** Persists the last committed row so the next run resumes before the stalled one. */
    rt::Task<void> persistResumeBeforeRowV3(std::shared_ptr<rt::Kv> kv, rt::KvKey cursorKey, std::optional<rt::KvKey> lastVisitedKey, std::optional<rt::KvKey> resumeKey) {
      const std::optional<rt::KvKey> cursorKeyValue = lastVisitedKey ? lastVisitedKey : resumeKey;
      if (cursorKeyValue) {
        const rt::Promise<void> persisting(persistBackfillCursorV3(kv, std::move(cursorKey), *cursorKeyValue));
        co_await persisting.wait();
      }
    }

    /** Re-applies the anchored retention TTL to a row that was already folded. */
    rt::Task<bool> rewriteMarkedRequestRowTtlV3(std::shared_ptr<rt::Kv> kv, rt::KvKey requestKey, rt::KvEntryMaybe<> requestEntry, PaidFallbackRequestV3 request,
                                                double nowMs) {
      bool ok = false;
      try {
        const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
        // `.set(requestKey, request, ...)` writes the stored row back as it was read.
        atomic->check(requestEntry).set(requestKey, requestEntry.value, rt::KvSetOptions{.expireIn = native_paid::requestRowExpireIn(request, nowMs)});
        const rt::KvCommitResult committed = co_await atomic->commit();
        ok = committed.ok;
      } catch (...) {
        ok = false;
      }
      co_return ok;
    }

    /** Outcome of folding one request row into its usage rollup. */
    // "processed" | "rollup_written" | "failed"
    using RollupFoldOutcome = std::string;

    /**
     * Marks one request row as backfilled and, when it is a settled billable row,
     * folds its usage into the matching hourly rollup.
     */
    rt::Task<RollupFoldOutcome> foldRequestRowIntoRollupV3(std::shared_ptr<rt::Kv> kv, rt::KvKey requestKey, rt::KvEntryMaybe<> requestEntry,
                                                           PaidFallbackRequestV3 request, double nowMs) {
      const contracts::NativePaidRollupIdentity identity = native_paid::paidFallbackRequestRollupIdentity(request);
      const rt::KvKey rollupKey = native_paid::paidFallbackUsageRollupKey(identity.bucketStartAtMs, identity.model, identity.provider, identity.shard);
      std::optional<rt::KvEntryMaybe<>> rollupEntry;
      if (native_paid::paidFallbackBackfillRollupEligibleV3(request)) {
        const rt::KvGetOptions strong = strongRead();
        rollupEntry = co_await kv->get(rollupKey, strong);
      }
      std::optional<PaidFallbackUsageRollup> existingRollup;
      if (rollupEntry) {
        const json::value view = rt::storedJsonView(rollupEntry->value, STORED_JSON_VIEW_OPTIONS);
        if (native_paid::isPaidFallbackUsageRollup(view)) existingRollup = json::value_to<PaidFallbackUsageRollup>(view);
      }
      const native_paid::PaidFallbackBackfillFoldPlanV3 plan = native_paid::planPaidFallbackBackfillFoldV3(request, existingRollup, nowMs);
      const std::optional<PaidFallbackUsageRollup>& nextRollup = plan.rollup;
      // `{ ...request, updated_at_ms: now, usage_rollup_at_ms: now }`, merged on the stored row.
      rt::KvValue nextRow = requestEntry.value;
      nextRow.asObject().set("updated_at_ms", rt::KvValue(plan.request.updated_at_ms));
      nextRow.asObject().set("usage_rollup_at_ms", rt::KvValue(*plan.request.usage_rollup_at_ms));
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(requestEntry).set(requestKey, std::move(nextRow),
                                      rt::KvSetOptions{
                                        .expireIn = plan.requestExpireIn,
                                      });
      if (nextRollup && rollupEntry) {
        atomic->check(*rollupEntry).set(rollupKey, kvValueOf(json::value_from(*nextRollup)));
      }
      const rt::KvCommitResult result = co_await atomic->commit();
      if (!result.ok) {
        // A concurrent writer touched this row or its rollup shard. Stop the
        // sweep immediately so the persisted cursor stays before this row: a
        // later success must never let the cursor skip a missing rollup.
        co_return RollupFoldOutcome("failed");
      }
      co_return RollupFoldOutcome(nextRollup ? "rollup_written" : "processed");
    }

    /** Where the sweep stopped, and which cursor fallback that stop needs. */
    // "scan_budget" | "write_budget" | null
    using RollupBackfillTruncation = std::optional<std::string>;

    struct RollupBackfillSweep {
      double scanned;
      double processed;
      double rollupsWritten;
      double failed;
      std::optional<rt::KvKey> lastVisitedKey;
      RollupBackfillTruncation truncatedWith;
    };

    /** Mutable counters for one bounded sweep. */
    struct BackfillProgress {
      double scanned;
      double processed;
      double rollupsWritten;
      double failed;
      double budget;
      double scanRemaining;
      std::optional<rt::KvKey> lastVisitedKey;
    };

    /** Freezes the sweep counters into a result. */
    RollupBackfillSweep toRollupBackfillSweepV3(const BackfillProgress& progress, RollupBackfillTruncation truncatedWith) {
      return RollupBackfillSweep{
        .scanned = progress.scanned,
        .processed = progress.processed,
        .rollupsWritten = progress.rollupsWritten,
        .failed = progress.failed,
        .lastVisitedKey = progress.lastVisitedKey,
        .truncatedWith = std::move(truncatedWith),
      };
    }

    /**
     * Applies one row of the sweep to the progress counters. Returns where the
     * sweep must stop, or `"continue"` when the walk may go on.
     */
    // "continue" | "scan_budget" | "write_budget" (null is never returned here)
    rt::Task<std::string> advanceBackfillSweepV3(std::shared_ptr<rt::Kv> kv, rt::KvKey requestKey, std::shared_ptr<BackfillProgress> progress, double nowMs) {
      const rt::KvGetOptions strong = strongRead();
      const rt::KvEntryMaybe<> requestEntry = co_await kv->get(requestKey, strong);
      const std::optional<PaidFallbackRequestV3> request = storedRow<PaidFallbackRequestV3>(requestEntry.value);
      const std::string rowState = native_paid::paidFallbackRequestBackfillStateV3(request);
      if (rowState == "missing" || !request) co_return std::string("continue");
      if (rowState == "retention") {
        // A previously processed row can lose its TTL (for example after a KV
        // export/import restores values without expiry). Re-apply the anchored
        // retention without touching the rollup or the row value; marked rows
        // never consume the run budget.
        const rt::Promise<bool> rewriting(rewriteMarkedRequestRowTtlV3(kv, requestKey, requestEntry, *request, nowMs));
        const bool rewritten = co_await rewriting.wait();
        if (!rewritten) {
          progress->failed += 1;
          co_return std::string("write_budget");
        }
        progress->lastVisitedKey = requestKey;
        co_return std::string("continue");
      }
      if (progress->budget <= 0) co_return std::string("write_budget");
      progress->budget -= 1;
      const rt::Promise<RollupFoldOutcome> folding(foldRequestRowIntoRollupV3(kv, requestKey, requestEntry, *request, nowMs));
      const RollupFoldOutcome outcome = co_await folding.wait();
      if (outcome == "failed") {
        progress->failed += 1;
        co_return std::string("write_budget");
      }
      progress->lastVisitedKey = requestKey;
      progress->processed += 1;
      if (outcome == "rollup_written") progress->rollupsWritten += 1;
      co_return std::string("continue");
    }

    /** One bounded sweep of the request-row prefix. */
    rt::Task<RollupBackfillSweep> sweepPaidFallbackUsageRollupsV3(std::shared_ptr<rt::Kv> kv, std::optional<rt::KvKey> resumeKey, double limit, double scanBudget,
                                                                  double nowMs) {
      auto progress = std::make_shared<BackfillProgress>(BackfillProgress{
        .scanned = 0,
        .processed = 0,
        .rollupsWritten = 0,
        .failed = 0,
        .budget = limit,
        .scanRemaining = scanBudget,
        .lastVisitedKey = std::nullopt,
      });
      const rt::KvListSelector selector = resumeKey ? prefixSelector(native_paid::paidFallbackRequestV3GlobalPrefix, resumeKey)
                                                    : prefixSelector(native_paid::paidFallbackRequestV3GlobalPrefix);
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector);
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        progress->scanned += 1;
        if (progress->scanRemaining <= 0) {
          co_await iterator->return_();
          co_return toRollupBackfillSweepV3(*progress, std::string("scan_budget"));
        }
        progress->scanRemaining -= 1;
        const rt::Promise<std::string> advancing(advanceBackfillSweepV3(kv, entry->key, progress, nowMs));
        const std::string step = co_await advancing.wait();
        if (step == "continue") continue;
        co_await iterator->return_();
        co_return toRollupBackfillSweepV3(*progress, step);
      }
      co_return toRollupBackfillSweepV3(*progress, progress->failed > 0 ? RollupBackfillTruncation("write_budget") : std::nullopt);
    }

    /** Outcome of rewriting one window row's TTL. */
    // "skipped" | "rewritten" | "failed"
    using WindowTtlRewrite = std::string;

    /** Rewrites one window row with its anchored TTL. */
    rt::Task<WindowTtlRewrite> rewriteWindowTtlV3(std::shared_ptr<rt::Kv> kv, rt::KvKey key, double nowMs) {
      const rt::KvGetOptions strong = strongRead();
      const rt::KvEntryMaybe<> windowEntry = co_await kv->get(key, strong);
      const json::value view = rt::storedJsonView(windowEntry.value, STORED_JSON_VIEW_OPTIONS);
      if (!native_paid::isPaidFallbackWindowV3(view)) co_return WindowTtlRewrite("skipped");
      WindowTtlRewrite outcome = "failed";
      try {
        const PaidFallbackWindowV3 window = json::value_to<PaidFallbackWindowV3>(view);
        const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
        atomic->check(windowEntry).set(key, windowEntry.value, rt::KvSetOptions{.expireIn = native_paid::windowExpireIn(window, nowMs)});
        const rt::KvCommitResult committed = co_await atomic->commit();
        outcome = committed.ok ? "rewritten" : "failed";
      } catch (...) {
        outcome = "failed";
      }
      co_return outcome;
    }

    /** Reads at most one entry matching a selector; `null` when nothing matches. */
    rt::Task<std::optional<rt::KvEntry<>>> readFirstKvEntry(std::shared_ptr<rt::Kv> kv, rt::KvListSelector selector) {
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(std::move(selector), strongList());
      std::optional<rt::KvEntry<>> entry = co_await iterator->next();
      if (entry) {
        co_await iterator->return_();
        co_return entry;
      }
      co_return std::nullopt;
    }

    /** True when the window prefix still holds rows after `startKey` itself. */
    rt::Task<bool> hasWindowRowsAfterV3(std::shared_ptr<rt::Kv> kv, rt::KvKey startKey) {
      std::vector<rt::KvKey> seenKeys;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(prefixSelector(native_paid::paidFallbackWindowV3GlobalPrefix, std::move(startKey)));
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        seenKeys.push_back(entry->key);
        if (seenKeys.size() > 1) {
          co_await iterator->return_();
          co_return true;
        }
      }
      co_return false;
    }

    struct WindowTtlSweep {
      double scanned;
      double rewritten;
      std::optional<rt::KvKey> lastRewrittenKey;
      /** A row lost its CAS or threw; the caller keeps the cursor before it. */
      bool failed;
      /** The batch limit was reached and more rows remain; the cursor is persisted. */
      bool truncated;
    };

    /** One bounded window-TTL sweep. */
    rt::Task<WindowTtlSweep> sweepWindowTtlsV3(std::shared_ptr<rt::Kv> kv, rt::KvKey cursorKey, std::optional<rt::KvKey> resumeKey, double limit, double nowMs) {
      double scanned = 0;
      double rewritten = 0;
      std::optional<rt::KvKey> lastRewrittenKey;
      const rt::KvListSelector selector = resumeKey ? prefixSelector(native_paid::paidFallbackWindowV3GlobalPrefix, resumeKey)
                                                    : prefixSelector(native_paid::paidFallbackWindowV3GlobalPrefix);
      bool skippingResumeRow = resumeKey.has_value();
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector);
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        scanned += 1;
        // `start` is inclusive; skip the cursor row itself so a resumed run is
        // not forced to rewrite it and then trip the batch limit again.
        const std::optional<rt::KvKey>& resumeCursor = resumeKey;
        if (skippingResumeRow && resumeCursor && rt::kvKeyStrictEquals(entry->key, *resumeCursor)) {
          skippingResumeRow = false;
          continue;
        }
        const rt::Promise<WindowTtlRewrite> rewriting(rewriteWindowTtlV3(kv, entry->key, nowMs));
        const WindowTtlRewrite outcome = co_await rewriting.wait();
        if (outcome == "skipped") continue;
        if (outcome == "failed") {
          co_await iterator->return_();
          co_return WindowTtlSweep{.scanned = scanned, .rewritten = rewritten, .lastRewrittenKey = lastRewrittenKey, .failed = true, .truncated = false};
        }
        lastRewrittenKey = entry->key;
        rewritten += 1;
        if (rewritten < limit) continue;
        // Probe for rows beyond this batch before reporting truncation, so the
        // final batch is never reported as incomplete.
        const rt::Promise<bool> probing(hasWindowRowsAfterV3(kv, entry->key));
        const bool hasMore = co_await probing.wait();
        if (!hasMore) {
          co_await iterator->return_();
          break;
        }
        const rt::Promise<void> persisting(persistBackfillCursorV3(kv, cursorKey, entry->key));
        co_await persisting.wait();
        co_await iterator->return_();
        co_return WindowTtlSweep{.scanned = scanned, .rewritten = rewritten, .lastRewrittenKey = lastRewrittenKey, .failed = false, .truncated = true};
      }
      co_return WindowTtlSweep{.scanned = scanned, .rewritten = rewritten, .lastRewrittenKey = lastRewrittenKey, .failed = false, .truncated = false};
    }

    rt::Task<bool> hasPaidFallbackUsageRollups(std::shared_ptr<rt::Kv> kv) {
      const rt::Promise<std::optional<rt::KvEntry<>>> reading(readFirstKvEntry(kv, prefixSelector(native_paid::PAID_FALLBACK_USAGE_ROLLUP_PREFIX)));
      const std::optional<rt::KvEntry<>> firstRollup = co_await reading.wait();
      co_return firstRollup.has_value();
    }

    rt::Task<bool> hasBackfillableSettledPaidFallbackRequest(std::shared_ptr<rt::Kv> kv) {
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(prefixSelector(native_paid::paidFallbackRequestV3GlobalPrefix), strongList());
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        if (native_paid::isBackfillableSettledPaidFallbackRequest(rt::storedJsonView(entry->value, STORED_JSON_VIEW_OPTIONS))) {
          co_await iterator->return_();
          co_return true;
        }
      }
      co_return false;
    }

    rt::Task<bool> paidFallbackBackfillNeedsRun(std::shared_ptr<rt::Kv> kv) {
      const rt::KvGetOptions strong = strongRead();
      const auto read = [&kv, strong](rt::KvKey key) {
        auto task = kv->get(std::move(key), strong);
        return rt::detail::keepAlive(kv, std::move(task));
      };
      const rt::Promise<rt::KvEntryMaybe<>> stateRead(read(native_paid::paidFallbackBackfillStateV3Key()));
      const rt::Promise<rt::KvEntryMaybe<>> requestCursorRead(read(native_paid::paidFallbackBackfillCursorV3Key()));
      const rt::Promise<rt::KvEntryMaybe<>> windowCursorRead(read(native_paid::paidFallbackBackfillWindowCursorV3Key()));
      auto reads = rt::all(stateRead, requestCursorRead, windowCursorRead);
      auto readEntries = co_await reads.wait();
      auto& [stateEntry, requestCursorEntry, windowCursorEntry] = readEntries;
      if (native_paid::paidFallbackBackfillNeedsSweepV3(rt::storedJsonView(stateEntry.value, STORED_JSON_VIEW_OPTIONS), rt::storedJsonView(requestCursorEntry.value, STORED_JSON_VIEW_OPTIONS), rt::storedJsonView(windowCursorEntry.value, STORED_JSON_VIEW_OPTIONS))) {
        co_return true;
      }
      const rt::Promise<bool> probing(hasPaidFallbackUsageRollups(kv));
      const bool hasRollups = co_await probing.wait();
      if (hasRollups) co_return false;
      const rt::Promise<bool> scanning(hasBackfillableSettledPaidFallbackRequest(kv));
      const bool needed = co_await scanning.wait();
      co_return needed;
    }

    rt::Task<std::optional<PaidFallbackBackfillLeaseV3>> acquirePaidFallbackBackfillLease(std::shared_ptr<rt::Kv> kv, double nowMs) {
      const rt::KvKey leaseKey = native_paid::paidFallbackBackfillLeaseV3Key();
      const rt::KvGetOptions strong = strongRead();
      for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
        const rt::KvEntryMaybe<> entry = co_await kv->get(leaseKey, strong);
        if (native_paid::paidFallbackLeaseActiveV3(storedLease(entry.value), nowMs)) co_return std::nullopt;
        PaidFallbackBackfillLeaseV3 lease = native_paid::paidFallbackBackfillLeaseV3(rt::crypto::randomUUID(), nowMs);
        const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry).set(leaseKey, kvValueOf(json::value_from(lease)),
                                    rt::KvSetOptions{
                                      .expireIn = PAID_FALLBACK_BACKFILL_LEASE_MS,
                                    });
        const rt::KvCommitResult commit = co_await operation->commit();
        if (commit.ok) co_return lease;
      }
      co_return std::nullopt;
    }

    rt::Task<void> releasePaidFallbackBackfillLease(std::shared_ptr<rt::Kv> kv, PaidFallbackBackfillLeaseV3 lease) {
      try {
        const rt::KvGetOptions strong = strongRead();
        const rt::KvKey leaseKey = native_paid::paidFallbackBackfillLeaseV3Key();
        const rt::KvEntryMaybe<> entry = co_await kv->get(leaseKey, strong);
        if (!native_paid::paidFallbackLeaseOwnedV3(storedLease(entry.value), lease.token)) co_return;
        std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
        atomic->check(entry).delete_(native_paid::paidFallbackBackfillLeaseV3Key());
        rt::Task<void> committing = commitIgnoringFailure(std::move(atomic));
        co_await std::move(committing);
      } catch (...) {
        // Lease expiry is the recovery path when a release races a KV outage.
      }
    }

    /**
     * Surfaces a failed state write. A bare `set` has no CAS precondition, so
     * `KvCommitResult.ok` is typed as the literal `true`; the check is kept
     * because the completion marker must be durable before it is reported.
     */
    void throwIfCommitFailed(const rt::KvCommitResult& result, const std::string& message) {
      if (!result.ok) throw rt::JsError(message);
    }

    // `scheduledPaidFallbackBackfill: Promise<void> | null`
    std::optional<rt::Promise<void>>& scheduledPaidFallbackBackfill() {
      static std::optional<rt::Promise<void>> scheduled;
      return scheduled;
    }

    // The `.then(async (kv) => { ... })` body of schedulePaidFallbackBackfill.
    rt::Task<void> runScheduledPaidFallbackBackfill(std::shared_ptr<rt::Kv> kv) {
      if (!kv) {
        paidFallbackBackfillRetryPending = true;
        co_return;
      }
      const rt::Promise<PaidFallbackAutomaticBackfillResult> running(runPaidFallbackBackfillV3(kv));
      const PaidFallbackAutomaticBackfillResult result = co_await running.wait();
      paidFallbackBackfillRetryPending = result.kind == "in_progress" || result.kind == "busy";
    }

    /** Clears due pending markers whose request row is gone or no longer billable. */
    rt::Task<std::vector<rt::KvEntry<PaidFallbackRequestV3>>> collectReconcileCandidatesV3(std::shared_ptr<rt::Kv> kv, std::string keyId,
                                                                                          std::vector<rt::KvEntry<PaidFallbackPendingV3>> due, double now) {
      std::vector<rt::KvEntry<PaidFallbackRequestV3>> candidates;
      const rt::KvGetOptions strong = strongRead();
      for (const rt::KvEntry<PaidFallbackPendingV3>& pending : due) {
        const std::string requestId = pending.key.empty() ? std::string("undefined") : pending.key.back().toString();
        const rt::KvKey requestKey = native_paid::paidFallbackRequestV3Key(keyId, requestId);
        const rt::KvEntryMaybe<> requestEntry = co_await kv->get(requestKey, strong);
        const std::optional<PaidFallbackRequestV3> request = storedRow<PaidFallbackRequestV3>(requestEntry.value);
        if (!request || request->billing_state == "settled" || request->billing_state == "not_billed") {
          const rt::KvKey gateKey = native_paid::paidFallbackReconciliationGateV3Key();
          const rt::KvEntryMaybe<> gateEntry = co_await kv->get(gateKey, strong);
          const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
          atomic->check(pending).check(gateEntry).delete_(pending.key).set(gateKey, kvValueOf(json::value_from(native_paid::paidFallbackReconciliationGateDueNow(gateRow(gateEntry), now))));
          co_await atomic->commit();
          continue;
        }
        candidates.push_back(rt::KvEntry<PaidFallbackRequestV3>{requestEntry.key, *request, requestEntry.versionstamp.value_or(std::string())});
      }
      co_return candidates;
    }

    /** Fetches the Metered token log for these candidates; `null` when the fetch failed. */
    rt::Task<std::optional<std::vector<MeteredTokenLogEntry>>> loadMeteredLogsForCandidatesV3(std::vector<rt::KvEntry<PaidFallbackRequestV3>> candidates, double now) {
      std::vector<std::string> providerRequestIds;
      for (const rt::KvEntry<PaidFallbackRequestV3>& requestEntry : candidates) {
        if (requestEntry.value.provider_request_id) providerRequestIds.push_back(*requestEntry.value.provider_request_id);
      }
      if (providerRequestIds.empty()) co_return std::vector<MeteredTokenLogEntry>{};
      std::optional<std::vector<MeteredTokenLogEntry>> logs;
      try {
        std::vector<double> createdAtMs;
        for (const rt::KvEntry<PaidFallbackRequestV3>& requestEntry : candidates) createdAtMs.push_back(requestEntry.value.created_at_ms);
        provider::metered::MeteredTokenLogFetchOptions options;
        options.requestIds = std::move(providerRequestIds);
        options.startAtMs = rt::Math::min(std::span<const double>(createdAtMs)) - 60'000;
        options.endAtMs = now + 60'000;
        const rt::Promise<std::vector<MeteredTokenLogEntry>> fetching(provider::metered::fetchMeteredTokenLogs(std::move(options)));
        logs = co_await fetching.wait();
      } catch (...) {
        logs.reset();
      }
      co_return logs;
    }

    /** Settles every candidate that has an authoritative Metered log entry. */
    rt::Task<double> settleMeteredCandidatesV3(std::shared_ptr<rt::Kv> kv, std::string keyId, std::vector<rt::KvEntry<PaidFallbackRequestV3>> candidates,
                                               std::shared_ptr<const rt::OrderedMap<std::string, MeteredTokenLogEntry>> byId, double now) {
      double settled = 0;
      for (const rt::KvEntry<PaidFallbackRequestV3>& requestEntry : candidates) {
        const PaidFallbackRequestV3& request = requestEntry.value;
        const MeteredTokenLogEntry* providerLog =
          request.provider_request_id && !request.provider_request_id->empty() ? byId->get(*request.provider_request_id) : nullptr;
        if (providerLog == nullptr) {
          const rt::Promise<std::optional<double>> deferring(ledger_settlement::deferPaidFallbackReconciliationV3(kv, keyId, request.request_id, now));
          co_await deferring.wait();
          continue;
        }
        const rt::Promise<ledger_settlement::SettlePaidFallbackRequestV3Result> settling(
          ledger_settlement::settlePaidFallbackRequestV3(kv, keyId, request.request_id, *providerLog, now));
        const ledger_settlement::SettlePaidFallbackRequestV3Result result = co_await settling.wait();
        if (result.settled) settled += 1;
      }
      co_return settled;
    }

    // `await Promise.all(entries.map((requestEntry) => deferPaidFallbackReconciliationV3(...)))`
    rt::Task<void> deferAll(std::shared_ptr<rt::Kv> kv, std::string keyId, std::vector<rt::KvEntry<PaidFallbackRequestV3>> entries, double now) {
      std::vector<rt::Promise<std::optional<double>>> deferrals;
      for (const rt::KvEntry<PaidFallbackRequestV3>& requestEntry : entries) {
        deferrals.push_back(rt::Promise<std::optional<double>>(ledger_settlement::deferPaidFallbackReconciliationV3(kv, keyId, requestEntry.value.request_id, now)));
      }
      auto all = rt::all(std::move(deferrals));
      co_await all.wait();
    }

    // The `try` block of reconcilePaidFallbackV3.
    rt::Task<double> reconcileUnderLease(std::shared_ptr<rt::Kv> kv, std::string keyId, double now) {
      std::vector<rt::KvEntry<PaidFallbackPendingV3>> due;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(prefixSelector(native_paid::paidFallbackPendingV3Prefix(keyId)));
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        rt::KvEntry<PaidFallbackPendingV3> pending = entry->as<PaidFallbackPendingV3>();
        if (pending.value.next_reconciliation_at_ms <= now) due.push_back(std::move(pending));
      }
      if (due.empty()) co_return 0;

      const rt::Promise<std::vector<rt::KvEntry<PaidFallbackRequestV3>>> collecting(collectReconcileCandidatesV3(kv, keyId, std::move(due), now));
      const std::vector<rt::KvEntry<PaidFallbackRequestV3>> candidates = co_await collecting.wait();
      if (candidates.empty()) co_return 0;
      // Surplus has no Metered token-log endpoint. Its terminal usage is settled
      // synchronously by recordSurplusUsage; a missing or partial observation
      // must remain pending and fail closed rather than being looked up or
      // falsely settled through Metered logs.
      std::vector<rt::KvEntry<PaidFallbackRequestV3>> surplusCandidates;
      std::vector<rt::KvEntry<PaidFallbackRequestV3>> meteredCandidates;
      for (const rt::KvEntry<PaidFallbackRequestV3>& requestEntry : candidates) {
        if (requestEntry.value.provider == std::optional<std::string>("surplus")) surplusCandidates.push_back(requestEntry);
      }
      rt::Task<void> deferringSurplus = deferAll(kv, keyId, surplusCandidates, now);
      co_await std::move(deferringSurplus);
      for (const rt::KvEntry<PaidFallbackRequestV3>& requestEntry : candidates) {
        if (requestEntry.value.provider != std::optional<std::string>("surplus")) meteredCandidates.push_back(requestEntry);
      }
      if (meteredCandidates.empty()) co_return 0;
      const rt::Promise<std::optional<std::vector<MeteredTokenLogEntry>>> loading(loadMeteredLogsForCandidatesV3(meteredCandidates, now));
      const std::optional<std::vector<MeteredTokenLogEntry>> logs = co_await loading.wait();
      if (!logs) {
        rt::Task<void> deferringMetered = deferAll(kv, keyId, meteredCandidates, now);
        co_await std::move(deferringMetered);
        // The durable marker carries the retry timestamp, but it is not a
        // delivery schedule. The event-driven policy intentionally leaves this
        // work pending on an idle Mac until a later authorized event.
        co_return 0;
      }
      auto byId = std::make_shared<rt::OrderedMap<std::string, MeteredTokenLogEntry>>();
      for (const MeteredTokenLogEntry& log : *logs) byId->set(log.request_id, log);
      const rt::Promise<double> settling(settleMeteredCandidatesV3(kv, keyId, std::move(meteredCandidates), std::move(byId), now));
      const double settled = co_await settling.wait();
      co_return settled;
    }
  }

  rt::Task<PaidFallbackRollupBackfillResult> backfillPaidFallbackUsageRollups(std::shared_ptr<rt::Kv> kv) {
    rt::Task<PaidFallbackRollupBackfillResult> running = backfillPaidFallbackUsageRollups(std::move(kv), BackfillPaidFallbackUsageRollupsOptions{});
    const PaidFallbackRollupBackfillResult result = co_await std::move(running);
    co_return result;
  }

  rt::Task<PaidFallbackRollupBackfillResult> backfillPaidFallbackUsageRollups(std::shared_ptr<rt::Kv> kv, BackfillPaidFallbackUsageRollupsOptions options) {
    const double limit = rt::Math::max(1.0, rt::Math::min(10'000.0, rt::Math::trunc(options.limit.value_or(5'000))));
    const double nowMs = rt::Math::trunc(options.nowMs ? *options.nowMs : rt::dateNow());
    if (!rt::Number::isSafeInteger(nowMs) || nowMs < 0) throw rt::JsError("Paid fallback backfill clock is invalid");
    // The budget bounds mutating work; the scan budget bounds the walk itself
    // (marked rows skip the write budget but still cost a get and a TTL rewrite)
    // so a resumed run cannot exceed its deadline without advancing the cursor.
    const rt::KvKey cursorKey = native_paid::paidFallbackBackfillCursorV3Key();
    const rt::Promise<std::optional<rt::KvKey>> loading(loadBackfillCursorV3(kv, cursorKey));
    const std::optional<rt::KvKey> resumeKey = co_await loading.wait();
    const rt::Promise<RollupBackfillSweep> sweeping(sweepPaidFallbackUsageRollupsV3(kv, resumeKey, limit, limit * 10, nowMs));
    const RollupBackfillSweep sweep = co_await sweeping.wait();
    const PaidFallbackRollupBackfillResult result{
      .scanned = sweep.scanned,
      .processed = sweep.processed,
      .rollups_written = sweep.rollupsWritten,
      .failed = sweep.failed,
      .truncated = sweep.truncatedWith.has_value(),
    };
    if (!sweep.truncatedWith) {
      // Full sweep complete: drop the resume cursor.
      std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->delete_(cursorKey);
      rt::Task<void> dropping = commitIgnoringFailure(std::move(atomic));
      co_await std::move(dropping);
      co_return result;
    }
    if (*sweep.truncatedWith == "scan_budget") {
      // Persist the resume cursor: `start` is inclusive, so the next run
      // restarts at this row instead of re-walking the whole prefix.
      if (sweep.lastVisitedKey) {
        const rt::Promise<void> persisting(persistBackfillCursorV3(kv, cursorKey, *sweep.lastVisitedKey));
        co_await persisting.wait();
      }
      co_return result;
    }
    // Budget exhaustion or a failed row: keep the cursor at the last committed
    // row so the next run resumes before the row that still needs work.
    const rt::Promise<void> persisting(persistResumeBeforeRowV3(kv, cursorKey, sweep.lastVisitedKey, resumeKey));
    co_await persisting.wait();
    co_return result;
  }

  rt::Task<PaidFallbackWindowTtlBackfillResult> backfillPaidFallbackWindowTtls(std::shared_ptr<rt::Kv> kv) {
    rt::Task<PaidFallbackWindowTtlBackfillResult> running = backfillPaidFallbackWindowTtls(std::move(kv), BackfillPaidFallbackWindowTtlsOptions{});
    const PaidFallbackWindowTtlBackfillResult result = co_await std::move(running);
    co_return result;
  }

  rt::Task<PaidFallbackWindowTtlBackfillResult> backfillPaidFallbackWindowTtls(std::shared_ptr<rt::Kv> kv, BackfillPaidFallbackWindowTtlsOptions options) {
    const double limit = rt::Math::max(1.0, rt::Math::min(10'000.0, rt::Math::trunc(options.limit.value_or(5'000))));
    const double nowMs = rt::Math::trunc(options.nowMs ? *options.nowMs : rt::dateNow());
    if (!rt::Number::isSafeInteger(nowMs) || nowMs < 0) throw rt::JsError("Paid fallback window TTL clock is invalid");
    const rt::KvKey cursorKey = native_paid::paidFallbackBackfillWindowCursorV3Key();
    const rt::Promise<std::optional<rt::KvKey>> loading(loadBackfillCursorV3(kv, cursorKey));
    const std::optional<rt::KvKey> resumeKey = co_await loading.wait();
    const rt::Promise<WindowTtlSweep> sweeping(sweepWindowTtlsV3(kv, cursorKey, resumeKey, limit, nowMs));
    const WindowTtlSweep sweep = co_await sweeping.wait();
    if (sweep.failed) {
      // Keep the cursor at the last successful rewrite so the next run retries
      // the row that lost its CAS instead of advancing past its missing TTL.
      const std::optional<rt::KvKey> cursorKeyValue = sweep.lastRewrittenKey ? sweep.lastRewrittenKey : resumeKey;
      if (cursorKeyValue) {
        const rt::Promise<void> persisting(persistBackfillCursorV3(kv, cursorKey, *cursorKeyValue));
        co_await persisting.wait();
      }
      co_return PaidFallbackWindowTtlBackfillResult{.scanned = sweep.scanned, .rewritten = sweep.rewritten, .truncated = true};
    }
    if (sweep.truncated) {
      // The sweep already persisted the resume cursor for this batch.
      co_return PaidFallbackWindowTtlBackfillResult{.scanned = sweep.scanned, .rewritten = sweep.rewritten, .truncated = true};
    }
    std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
    atomic->delete_(cursorKey);
    rt::Task<void> dropping = commitIgnoringFailure(std::move(atomic));
    co_await std::move(dropping);
    co_return PaidFallbackWindowTtlBackfillResult{.scanned = sweep.scanned, .rewritten = sweep.rewritten, .truncated = false};
  }

  rt::Task<PaidFallbackAutomaticBackfillResult> runPaidFallbackBackfillV3(std::shared_ptr<rt::Kv> kv) {
    rt::Task<PaidFallbackAutomaticBackfillResult> running = runPaidFallbackBackfillV3(std::move(kv), RunPaidFallbackBackfillV3Options{});
    PaidFallbackAutomaticBackfillResult result = co_await std::move(running);
    co_return result;
  }

  rt::Task<PaidFallbackAutomaticBackfillResult> runPaidFallbackBackfillV3(std::shared_ptr<rt::Kv> kv, RunPaidFallbackBackfillV3Options options) {
    const double requestedLimit = rt::Math::trunc(options.limit.value_or(PAID_FALLBACK_AUTOMATIC_BACKFILL_LIMIT));
    const double limit =
      rt::Number::isFinite(requestedLimit) ? rt::Math::max(1.0, rt::Math::min(10'000.0, requestedLimit)) : PAID_FALLBACK_AUTOMATIC_BACKFILL_LIMIT;
    const double nowMs = rt::Math::trunc(options.nowMs ? *options.nowMs : rt::dateNow());
    if (!rt::Number::isSafeInteger(nowMs) || nowMs < 0) {
      throw rt::JsError("Paid fallback automatic backfill clock is invalid");
    }
    if (options.force != true) {
      const rt::Promise<bool> checking(paidFallbackBackfillNeedsRun(kv));
      const bool needed = co_await checking.wait();
      if (!needed) co_return PaidFallbackAutomaticBackfillResult{.kind = "skipped", .requests = std::nullopt, .windows = std::nullopt};
    }
    const rt::Promise<std::optional<PaidFallbackBackfillLeaseV3>> acquiring(acquirePaidFallbackBackfillLease(kv, nowMs));
    const std::optional<PaidFallbackBackfillLeaseV3> lease = co_await acquiring.wait();
    if (!lease) co_return PaidFallbackAutomaticBackfillResult{.kind = "busy", .requests = std::nullopt, .windows = std::nullopt};
    std::optional<PaidFallbackAutomaticBackfillResult> outcome;
    rt::Reason failure;
    try {
      BackfillPaidFallbackUsageRollupsOptions requestOptions;
      requestOptions.limit = limit;
      requestOptions.nowMs = nowMs;
      const rt::Promise<PaidFallbackRollupBackfillResult> requestSweep(backfillPaidFallbackUsageRollups(kv, requestOptions));
      const PaidFallbackRollupBackfillResult requests = co_await requestSweep.wait();
      BackfillPaidFallbackWindowTtlsOptions windowOptions;
      windowOptions.limit = limit;
      windowOptions.nowMs = nowMs;
      const rt::Promise<PaidFallbackWindowTtlBackfillResult> windowSweep(backfillPaidFallbackWindowTtls(kv, windowOptions));
      const PaidFallbackWindowTtlBackfillResult windows = co_await windowSweep.wait();
      const bool complete = !requests.truncated && !windows.truncated;
      if (complete) {
        const contracts::PaidFallbackBackfillStateV3 state{.v = 1, .completed_at_ms = nowMs};
        const rt::KvKey stateKey = native_paid::paidFallbackBackfillStateV3Key();
        rt::Task<rt::KvCommitResult> writing = kv->set(stateKey, kvValueOf(json::value_from(state)));
        const rt::KvCommitResult written = co_await std::move(writing);
        throwIfCommitFailed(written, "Paid fallback automatic backfill state changed concurrently.");
      }
      outcome = PaidFallbackAutomaticBackfillResult{
        .kind = complete ? "completed" : "in_progress",
        .requests = requests,
        .windows = windows,
      };
    } catch (...) {
      failure = std::current_exception();
    }
    const rt::Promise<void> releasing(releasePaidFallbackBackfillLease(kv, *lease));
    co_await releasing.wait();
    if (failure) std::rethrow_exception(failure);
    co_return std::move(*outcome);
  }

  void schedulePaidFallbackBackfill() { schedulePaidFallbackBackfill(nullptr); }

  void schedulePaidFallbackBackfill(std::shared_ptr<rt::Kv> kvOverride) {
    if (scheduledPaidFallbackBackfill()) return;
    paidFallbackBackfillAttempted = true;
    const rt::Promise<std::shared_ptr<rt::Kv>> source = kvOverride ? rt::Promise<std::shared_ptr<rt::Kv>>::resolve(kvOverride)
                                                                   : rt::Promise<std::shared_ptr<rt::Kv>>(ledger_state::resolveKv(std::nullopt));
    scheduledPaidFallbackBackfill() = source.then([](std::shared_ptr<rt::Kv> kv) { return runScheduledPaidFallbackBackfill(std::move(kv)); })
                                        .catch_([](const rt::Reason&) { paidFallbackBackfillRetryPending = true; })
                                        .finally_([] { scheduledPaidFallbackBackfill().reset(); });
  }

  std::optional<rt::Promise<void>> scheduledPaidFallbackBackfillForTest() { return scheduledPaidFallbackBackfill(); }

  rt::Task<double> reconcilePaidFallbackV3(std::string keyId) {
    rt::Task<double> reconciling = reconcilePaidFallbackV3(std::move(keyId), rt::dateNow(), std::nullopt, ReconcilePaidFallbackV3Options{});
    const double settled = co_await std::move(reconciling);
    co_return settled;
  }

  rt::Task<double> reconcilePaidFallbackV3(std::string keyId, double now) {
    rt::Task<double> reconciling = reconcilePaidFallbackV3(std::move(keyId), now, std::nullopt, ReconcilePaidFallbackV3Options{});
    const double settled = co_await std::move(reconciling);
    co_return settled;
  }

  rt::Task<double> reconcilePaidFallbackV3(std::string keyId, double now, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    rt::Task<double> reconciling = reconcilePaidFallbackV3(std::move(keyId), now, std::move(kvOverride), ReconcilePaidFallbackV3Options{});
    const double settled = co_await std::move(reconciling);
    co_return settled;
  }

  rt::Task<double> reconcilePaidFallbackV3(std::string keyId, double now, std::optional<std::shared_ptr<rt::Kv>> kvOverride, ReconcilePaidFallbackV3Options options) {
    const rt::Promise<std::shared_ptr<rt::Kv>> resolving(ledger_state::resolveKv(std::move(kvOverride)));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) co_return 0;
    const rt::Promise<std::optional<contracts::PaidFallbackReconciliationLeaseV3>> acquiring(ledger_settlement::acquireReconciliationLease(kv, keyId, now));
    const std::optional<contracts::PaidFallbackReconciliationLeaseV3> lease = co_await acquiring.wait();
    if (!lease) co_return 0;
    std::optional<double> settled;
    rt::Reason failure;
    try {
      rt::Task<double> body = reconcileUnderLease(kv, keyId, now);
      settled = co_await std::move(body);
    } catch (...) {
      failure = std::current_exception();
    }
    const rt::Promise<void> releasing(ledger_settlement::releaseReconciliationLease(kv, keyId, *lease));
    co_await releasing.wait();
    if (options.skipGateRecompute != true) {
      const rt::Promise<std::optional<double>> recomputing(ledger_state::recomputePaidFallbackReconciliationGateV3(kv));
      co_await recomputing.wait();
    }
    if (failure) std::rethrow_exception(failure);
    co_return *settled;
  }

  rt::Task<double> reconcileDuePaidFallbacksV3() {
    rt::Task<double> reconciling = reconcileDuePaidFallbacksV3(rt::dateNow(), std::nullopt);
    const double settled = co_await std::move(reconciling);
    co_return settled;
  }

  rt::Task<double> reconcileDuePaidFallbacksV3(double now) {
    rt::Task<double> reconciling = reconcileDuePaidFallbacksV3(now, std::nullopt);
    const double settled = co_await std::move(reconciling);
    co_return settled;
  }

  rt::Task<double> reconcileDuePaidFallbacksV3(double now, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    const rt::Promise<std::shared_ptr<rt::Kv>> resolving(ledger_state::resolveKv(std::move(kvOverride)));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) co_return 0;
    const rt::KvKey gateKey = native_paid::paidFallbackReconciliationGateV3Key();
    const rt::KvGetOptions strong = strongRead();
    // `gateEntry.value` as the reads below leave it (std::nullopt before the first read).
    std::optional<json::value> gateValue;
    std::optional<ledger_state::PendingMarkerScan> bootstrapScan;
    for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      const rt::KvEntryMaybe<> gateEntry = co_await kv->get(gateKey, strong);
      gateValue = rt::storedJsonView(gateEntry.value, STORED_JSON_VIEW_OPTIONS);
      if (native_paid::isPaidFallbackReconciliationGate(*gateValue)) break;
      const rt::Promise<ledger_state::PendingMarkerScan> scanning(ledger_state::scanPaidFallbackPendingMarkers(kv));
      bootstrapScan = co_await scanning.wait();
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(gateEntry).set(gateKey, kvValueOf(json::value_from(native_paid::paidFallbackGateAfterScanV3(bootstrapScan->earliest_due_at_ms))));
      const rt::KvCommitResult commit = co_await atomic->commit();
      if (commit.ok) {
        json::object bootstrapped;
        rt::objectDefine(bootstrapped, "next_due_at_ms",
                         bootstrapScan->earliest_due_at_ms ? rt::makeNumber(*bootstrapScan->earliest_due_at_ms) : json::value(nullptr));
        gateValue = json::value(std::move(bootstrapped));
        break;
      }
      bootstrapScan.reset();
    }
    if (!gateValue || !native_paid::isPaidFallbackReconciliationGate(*gateValue)) {
      throw rt::JsError("Paid fallback reconciliation gate changed concurrently.");
    }
    const std::optional<double> nextDueAtMs = rt::getNumber(rt::get(*gateValue, "next_due_at_ms"));
    if (!nextDueAtMs || *nextDueAtMs > now) co_return 0;

    std::optional<ledger_state::PendingMarkerScan> pendingScan = bootstrapScan;
    if (!pendingScan) {
      const rt::Promise<ledger_state::PendingMarkerScan> scanning(ledger_state::scanPaidFallbackPendingMarkers(kv));
      pendingScan = co_await scanning.wait();
    }
    rt::OrderedSet<std::string> keyIds;
    for (const rt::KvEntry<PaidFallbackPendingV3>& pending : pendingScan->entries) {
      if (pending.value.next_reconciliation_at_ms > now) continue;
      const rt::KvKeyPart* keyId = pending.key.size() >= 2 ? &pending.key[pending.key.size() - 2] : nullptr;
      if (keyId != nullptr && keyId->isString()) keyIds.add(keyId->string());
    }
    double settled = 0;
    rt::Reason failure;
    try {
      for (const std::string& keyId : keyIds) {
        ReconcilePaidFallbackV3Options reconcileOptions;
        reconcileOptions.skipGateRecompute = true;
        const rt::Promise<double> reconciling(reconcilePaidFallbackV3(keyId, now, kv, reconcileOptions));
        const double count = co_await reconciling.wait();
        settled += count;
      }
    } catch (...) {
      failure = std::current_exception();
    }
    const rt::Promise<std::optional<double>> recomputing(ledger_state::recomputePaidFallbackReconciliationGateV3(kv));
    co_await recomputing.wait();
    if (failure) std::rethrow_exception(failure);
    co_return settled;
  }

  rt::Task<double> markPaidFallbackTerminalV3(PaidFallbackAdmissionV3 reservation, std::string terminalState) {
    contracts::PaidFallbackRequestLifecyclePatchV3 patch;
    patch.terminal_state = std::move(terminalState);
    patch.increment_reconciliation_attempts = true;
    const rt::Promise<void> updating(ledger_admission::updatePaidFallbackRequestV3(reservation, std::move(patch)));
    co_await updating.wait();
    // A terminal event can arrive while the pending marker is still scheduled
    // for a later retry. Move that marker to "due" before the event's
    // best-effort sweep so it never burns this event on a no-op reconciliation.
    const rt::Promise<void> expediting(ledger_settlement::_expeditePaidFallbackReconciliationV3(reservation, rt::dateNow()));
    co_await expediting.wait();
    // The event owns the best-effort provider-log read; never fetch provider
    // logs from the inference request itself. A failed or late read remains
    // durably pending for the next authorized event.
    co_return 0;
  }

  rt::Task<double> markPaidFallbackTerminalV3(PaidFallbackAdmissionV3 reservation, std::string terminalState, ::uos::types::PaidFallbackProvider provider) {
    contracts::PaidFallbackRequestLifecyclePatchV3 patch;
    patch.provider = std::move(provider);
    patch.terminal_state = std::move(terminalState);
    patch.increment_reconciliation_attempts = true;
    const rt::Promise<void> updating(ledger_admission::updatePaidFallbackRequestV3(reservation, std::move(patch)));
    co_await updating.wait();
    const rt::Promise<void> expediting(ledger_settlement::_expeditePaidFallbackReconciliationV3(reservation, rt::dateNow()));
    co_await expediting.wait();
    co_return 0;
  }

  rt::Task<double> recordPaidFallbackTerminalV3(PaidFallbackAdmissionV3 reservation, std::string terminalState) {
    const rt::Promise<double> marking(markPaidFallbackTerminalV3(reservation, std::move(terminalState)));
    co_await marking.wait();
    // Legacy direct callers are retained only for deterministic local migration
    // tests; production inference calls markPaidFallbackTerminalV3 instead.
    const rt::Promise<double> reconciling(reconcilePaidFallbackV3(reservation.key_id, rt::dateNow()));
    const double settled = co_await reconciling.wait();
    co_return settled;
  }

  rt::Task<double> recordPaidFallbackTerminalV3(PaidFallbackAdmissionV3 reservation, std::string terminalState, ::uos::types::PaidFallbackProvider provider) {
    const rt::Promise<double> marking(markPaidFallbackTerminalV3(reservation, std::move(terminalState), std::move(provider)));
    co_await marking.wait();
    const rt::Promise<double> reconciling(reconcilePaidFallbackV3(reservation.key_id, rt::dateNow()));
    const double settled = co_await reconciling.wait();
    co_return settled;
  }

  rt::Task<double> handlePaidFallbackReconciliationJobV3(rt::KvValue message) {
    rt::Task<double> handling = handlePaidFallbackReconciliationJobV3(std::move(message), std::nullopt);
    const double settled = co_await std::move(handling);
    co_return settled;
  }

  rt::Task<double> handlePaidFallbackReconciliationJobV3(rt::KvValue message, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    // `!message || typeof message !== "object"`: null and the primitives are not messages.
    if (!message.truthy() || message.isString() || message.isNumber() || message.isBool() || message.isBigInt()) co_return 0;
    // `"key_id" in message && typeof message.key_id === "string" ? message.key_id : null`
    std::optional<std::string> keyId;
    if (message.isObject()) {
      if (const rt::KvValue* member = message.asObject().find("key_id"); member != nullptr && member->isString()) keyId = member->asString();
    }
    if (!keyId || keyId->empty()) co_return 0;
    const rt::Promise<std::shared_ptr<rt::Kv>> resolving(ledger_state::resolveKv(std::move(kvOverride)));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) co_return 0;
    const rt::Promise<double> reconciling(reconcilePaidFallbackV3(*keyId, rt::dateNow(), kv));
    const double settled = co_await reconciling.wait();
    co_return settled;
  }

  rt::Task<double> enqueueDuePaidFallbackReconciliationJobsV3() {
    rt::Task<double> enqueuing = enqueueDuePaidFallbackReconciliationJobsV3(rt::dateNow(), std::nullopt);
    const double queued = co_await std::move(enqueuing);
    co_return queued;
  }

  rt::Task<double> enqueueDuePaidFallbackReconciliationJobsV3(double now) {
    rt::Task<double> enqueuing = enqueueDuePaidFallbackReconciliationJobsV3(now, std::nullopt);
    const double queued = co_await std::move(enqueuing);
    co_return queued;
  }

  rt::Task<double> enqueueDuePaidFallbackReconciliationJobsV3(double now, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    const rt::Promise<std::shared_ptr<rt::Kv>> resolving(ledger_state::resolveKv(std::move(kvOverride)));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) co_return 0;
    rt::OrderedSet<std::string> keyIds;
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(prefixSelector(native_paid::paidFallbackPendingV3GlobalPrefix));
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      const rt::KvEntry<PaidFallbackPendingV3> pending = entry->as<PaidFallbackPendingV3>();
      if (pending.value.next_reconciliation_at_ms > now) continue;
      const rt::KvKeyPart* keyId = pending.key.size() >= 2 ? &pending.key[pending.key.size() - 2] : nullptr;
      if (keyId != nullptr && keyId->isString()) keyIds.add(keyId->string());
    }
    double queued = 0;
    for (const std::string& keyId : keyIds) {
      const rt::Promise<bool> enqueuing(ledger_state::enqueuePaidFallbackReconciliationJob(kv, keyId));
      const bool enqueued = co_await enqueuing.wait();
      if (enqueued) queued += 1;
    }
    co_return queued;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackRollupBackfillResult& value) {
    out = Writer()
            .required("scanned", value.scanned)
            .required("processed", value.processed)
            .required("rollups_written", value.rollups_written)
            .required("failed", value.failed)
            .required("truncated", value.truncated)
            .done();
  }

  PaidFallbackRollupBackfillResult tag_invoke(const json::value_to_tag<PaidFallbackRollupBackfillResult>&, const json::value& value) {
    const Reader in(value, "PaidFallbackRollupBackfillResult");
    return {
      .scanned = in.required<double>("scanned"),
      .processed = in.required<double>("processed"),
      .rollups_written = in.required<double>("rollups_written"),
      .failed = in.required<double>("failed"),
      .truncated = in.required<bool>("truncated"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackWindowTtlBackfillResult& value) {
    out = Writer().required("scanned", value.scanned).required("rewritten", value.rewritten).required("truncated", value.truncated).done();
  }

  PaidFallbackWindowTtlBackfillResult tag_invoke(const json::value_to_tag<PaidFallbackWindowTtlBackfillResult>&, const json::value& value) {
    const Reader in(value, "PaidFallbackWindowTtlBackfillResult");
    return {
      .scanned = in.required<double>("scanned"),
      .rewritten = in.required<double>("rewritten"),
      .truncated = in.required<bool>("truncated"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackAutomaticBackfillResult& value) {
    out = Writer().required("kind", value.kind).orNull("requests", value.requests).orNull("windows", value.windows).done();
  }

  PaidFallbackAutomaticBackfillResult tag_invoke(const json::value_to_tag<PaidFallbackAutomaticBackfillResult>&, const json::value& value) {
    const Reader in(value, "PaidFallbackAutomaticBackfillResult");
    return {
      .kind = in.required<std::string>("kind"),
      .requests = in.orNull<PaidFallbackRollupBackfillResult>("requests"),
      .windows = in.orNull<PaidFallbackWindowTtlBackfillResult>("windows"),
    };
  }
}
