#include "uos/sentinel/replay_retention_metadata.hpp"

#include "uos/sentinel/replay_limits.hpp"
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_observation.hpp"
#include "uos/sentinel/replay_retention_schema.hpp"

#include "uos/rt/error.hpp"

#include <array>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

namespace uos::sentinel::replay_retention_metadata {
  namespace {
    using replay_retention_schema::counter;
    using replay_retention_schema::LedgerState;
    using replay_retention_schema::LedgerStateKind;
    using replay_retention_schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY;
    using replay_retention_schema::SENTINEL_REPLAY_EVICTION_PREFIX;
    using replay_retention_schema::SENTINEL_REPLAY_EVICTION_REASON;
    using replay_retention_schema::SENTINEL_REPLAY_EXPIRED_REASON;
    using replay_retention_schema::SentinelReplayBudgetLedger;

    /**
     * When a maintenance recount last met a corrupt or mismatched metadata row. It
     * defers the next maintenance recount like the marker does, so a bad row costs
     * one bounded scan per interval instead of one per admin poll; it is written with
     * the interval as its TTL. It sits outside every counted prefix.
     */
    const rt::KvKey& SENTINEL_REPLAY_METADATA_RECOUNT_FAILED_KEY() {
      static const rt::KvKey key{"uos_ai", "sentinel_replay", "v1", "metadata_recount_failed"};
      return key;
    }

    // `type ReconcileMarker = Readonly<{ version: 1; reconciled_at_ms: number }>`
    rt::KvValue reconcileMarker(double reconciledAtMs) {
      rt::KvObject marker;
      marker.set("version", rt::KvValue(1));
      marker.set("reconciled_at_ms", rt::KvValue(reconciledAtMs));
      return rt::KvValue(std::move(marker));
    }

    // `type RecountFailure = Readonly<{ version: 1; failed_at_ms: number }>`
    rt::KvValue recountFailure(double failedAtMs) {
      rt::KvObject failure;
      failure.set("version", rt::KvValue(1));
      failure.set("failed_at_ms", rt::KvValue(failedAtMs));
      return rt::KvValue(std::move(failure));
    }

    // `type MetadataTotals = Readonly<{ records: number; bytes: number }>`
    struct MetadataTotals {
      double records;
      double bytes;
    };

    // `Readonly<{ kind: "complete"; totals: MetadataTotals }> | Readonly<{ kind: "over_limit" | "failed" }>`
    enum class MetadataScanKind { complete, over_limit, failed };
    struct MetadataScan {
      MetadataScanKind kind;
      MetadataTotals totals{};
    };

    // `typeof value === "object" && value !== null`
    bool isObjectLike(const rt::KvValue& value) {
      return !(value.isUndefined() || value.isNull() || value.isBool() || value.isNumber() || value.isString() || value.isBigInt() || value.is<rt::KvHole>());
    }
    bool isText(const rt::KvValue& value, std::string_view text) { return value.isString() && value.asString() == text; }

    bool isTombstone(const rt::KvValue& value) {
      if (!isObjectLike(value)) return false;
      const rt::KvValue& row = value;
      const rt::KvValue& fingerprint = row["fingerprint"];
      const rt::KvValue& requestId = row["request_id"];
      return row["version"].isNumber() && row["version"].asNumber() == 1 && fingerprint.isString() && replay_model::HEX_DIGEST.test(fingerprint.asString()) &&
             (isText(requestId, "") || replay_observation::isSentinelReplayRequestId(requestId)) && counter(row["evicted_at_ms"]) &&
             (isText(row["reason"], SENTINEL_REPLAY_EVICTION_REASON) || isText(row["reason"], SENTINEL_REPLAY_EXPIRED_REASON));
    }

    // `entry.key[index] === part` for a key part read from an entry and a prefix part: by value,
    // except Uint8Arrays, which are distinct objects.
    bool strictlyEqual(const rt::KvKeyPart& left, const rt::KvKeyPart& right) {
      if (left.isString() && right.isString()) return left.string() == right.string();
      if (left.isNumber() && right.isNumber()) return left.number() == right.number();
      if (left.isBigInt() && right.isBigInt()) return left.bigint() == right.bigint();
      if (left.isBool() && right.isBool()) return left.boolean() == right.boolean();
      return false;
    }

    // `entry.key[index] === value` where `value` is a validated string member.
    bool partIsText(const rt::KvKeyPart& part, const rt::KvValue& value) { return part.isString() && value.isString() && part.string() == value.asString(); }
  }

  bool metadataEntryMatches(const rt::KvEntry<>& entry, const rt::KvKey& prefix) {
    if (entry.key.size() != prefix.size() + 1) return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
      if (!strictlyEqual(entry.key[index], prefix[index])) return false;
    }
    if (&prefix == &replay_model::SENTINEL_REPLAY_REQUEST_PREFIX) {
      return replay_observation::isSentinelReplayCaptureStatusRow(entry.value) && partIsText(entry.key[prefix.size()], entry.value["request_id"]);
    }
    return isTombstone(entry.value) && partIsText(entry.key[prefix.size()], entry.value["fingerprint"]);
  }

  namespace {
    rt::Task<MetadataScan> scanMetadata(rt::Kv& kv) {
      double records = 0;
      double bytes = 0;
      const std::array<const rt::KvKey*, 2> prefixes{&replay_model::SENTINEL_REPLAY_REQUEST_PREFIX, &SENTINEL_REPLAY_EVICTION_PREFIX};
      for (const rt::KvKey* prefix : prefixes) {
        // One extra entry proves truncation. Both iterators must genuinely exhaust,
        // and the combined row budget remains bounded even for legacy over-cap data.
        const double limit = replay_limits::SENTINEL_REPLAY_MAX_STATUS_RECORDS + 1 - records;
        rt::KvListSelector selector;
        selector.prefix = *prefix;
        rt::KvListOptions options;
        options.limit = limit;
        options.consistency = rt::KvConsistencyLevel::strong;
        options.batchSize = 128;
        const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector), std::move(options));
        for (;;) {
          const std::optional<rt::KvEntry<>> entry = co_await iterator->next();
          if (!entry) break;
          if (!metadataEntryMatches(*entry, *prefix)) {
            co_await iterator->return_();
            co_return MetadataScan{.kind = MetadataScanKind::failed};
          }
          if (records >= replay_limits::SENTINEL_REPLAY_MAX_STATUS_RECORDS) {
            co_await iterator->return_();
            co_return MetadataScan{.kind = MetadataScanKind::over_limit};
          }
          records += 1;
          bytes += replay_retention_schema::sentinelReplayStatusMetadataBytes(entry->value);
        }
      }
      co_return MetadataScan{.kind = MetadataScanKind::complete, .totals = {.records = records, .bytes = bytes}};
    }

    rt::Task<void> recordRecountFailure(rt::Kv& kv, MaintenanceRecount maintenance);
  }

  rt::Task<std::string> reconcileSentinelReplayStatusMetadata(rt::Kv& kv, double budgetBytes) {
    auto reconciling = reconcileSentinelReplayStatusMetadata(kv, budgetBytes, std::nullopt);
    std::string result = co_await std::move(reconciling);
    co_return result;
  }

  rt::Task<std::string> reconcileSentinelReplayStatusMetadata(rt::Kv& kv, double budgetBytes, std::optional<MaintenanceRecount> maintenance) {
    try {
      const LedgerState state = co_await replay_retention_schema::readLedger(kv, budgetBytes);
      if (state.kind != LedgerStateKind::ok || !state.ledger->bootstrap_complete || state.ledger->accounting_error) co_return "failed";
      auto scanning = scanMetadata(kv);
      const MetadataScan scan = co_await std::move(scanning);
      if (scan.kind == MetadataScanKind::failed) {
        if (maintenance) {
          auto recording = recordRecountFailure(kv, *maintenance);
          co_await std::move(recording);
        }
        co_return "failed";
      }
      const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
      operation->check(rt::AtomicCheck(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, state.versionstamp));
      if (scan.kind == MetadataScanKind::complete) {
        const SentinelReplayBudgetLedger ledger = replay_retention_schema::withBudget(*state.ledger, budgetBytes);
        // `ledger !== state.ledger`: withBudget returns a new object exactly when the budget differs.
        const bool rebudgeted = !(state.ledger->budget_bytes == budgetBytes);
        const bool changed = rebudgeted || ledger.status_records != scan.totals.records || ledger.metadata_bytes != scan.totals.bytes;
        if (changed) {
          SentinelReplayBudgetLedger next = ledger;
          next.status_records = scan.totals.records;
          next.metadata_bytes = scan.totals.bytes;
          operation->set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, replay_retention_schema::sentinelReplayLedgerValue(next));
        }
      }
      if (maintenance) {
        const rt::KvValue marker = reconcileMarker(maintenance->at_ms);
        operation->check(rt::AtomicCheck(SENTINEL_REPLAY_METADATA_RECONCILED_KEY, maintenance->marker_versionstamp));
        operation->set(SENTINEL_REPLAY_METADATA_RECONCILED_KEY, marker);
      }
      const rt::KvCommitResult committed = co_await operation->commit();
      if (!committed.ok) co_return "failed";
      co_return scan.kind == MetadataScanKind::complete ? "reconciled" : "over_limit";
    } catch (...) {
    }
    co_return "failed";
  }

  namespace {
    /** Best effort: a lost write only means the next poll scans again, as before the backoff existed. */
    rt::Task<void> recordRecountFailure(rt::Kv& kv, MaintenanceRecount maintenance) {
      const rt::KvValue failure = recountFailure(maintenance.at_ms);
      rt::KvSetOptions options;
      options.expireIn = SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS;
      const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
      operation->check(rt::AtomicCheck(SENTINEL_REPLAY_METADATA_RECONCILED_KEY, maintenance.marker_versionstamp))
        .set(SENTINEL_REPLAY_METADATA_RECOUNT_FAILED_KEY(), failure, options);
      co_await operation->commit();
    }

    /** Whether a valid time recorded under `field` is no later than `nowMs` and less than one interval before it. */
    // `field`: "reconciled_at_ms" | "failed_at_ms"
    bool withinInterval(const rt::KvValue& value, std::string_view field, double nowMs) {
      if (!counter(nowMs) || !isObjectLike(value)) return false;
      const rt::KvValue& row = value;
      const rt::KvValue& at = row[field];
      if (!(row["version"].isNumber() && row["version"].asNumber() == 1) || !counter(at)) return false;
      return at.asNumber() <= nowMs && nowMs - at.asNumber() < SENTINEL_REPLAY_METADATA_RECONCILE_INTERVAL_MS;
    }
  }

  rt::Task<std::string> reconcileSentinelReplayStatusMetadataIfDue(rt::Kv& kv, double budgetBytes, double nowMs) {
    std::optional<std::string> markerVersionstamp;
    bool unreadable = false;
    try {
      std::vector<rt::KvKey> keys{SENTINEL_REPLAY_METADATA_RECONCILED_KEY, SENTINEL_REPLAY_METADATA_RECOUNT_FAILED_KEY()};
      const std::vector<rt::KvEntryMaybe<>> entries = co_await kv.getMany(std::move(keys));
      // `const [marker, failure] = ...`
      const rt::KvEntryMaybe<>& marker = entries.at(0);
      const rt::KvEntryMaybe<>& failure = entries.at(1);
      if (withinInterval(marker.value, "reconciled_at_ms", nowMs) || withinInterval(failure.value, "failed_at_ms", nowMs)) co_return "not_due";
      markerVersionstamp = marker.versionstamp;
    } catch (...) {
      unreadable = true;
    }
    if (unreadable) co_return "failed";
    std::optional<MaintenanceRecount> maintenance;
    if (counter(nowMs)) maintenance = MaintenanceRecount{.at_ms = nowMs, .marker_versionstamp = markerVersionstamp};
    auto reconciling = reconcileSentinelReplayStatusMetadata(kv, budgetBytes, std::move(maintenance));
    std::string result = co_await std::move(reconciling);
    co_return result;
  }
}
