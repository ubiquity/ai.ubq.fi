#include "uos/sentinel/replay_retention.hpp"

#include "uos/sentinel/replay_limits.hpp"
#include "uos/sentinel/replay_model.hpp"
#include "uos/sentinel/replay_observation.hpp"
#include "uos/sentinel/replay_read.hpp"
#include "uos/sentinel/replay_retention_bootstrap.hpp"
#include "uos/sentinel/replay_retention_metadata.hpp"

#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <functional>
#include <memory>
#include <tuple>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

namespace uos::sentinel::replay_retention {
  namespace {
    using replay_limits::SENTINEL_REPLAY_MAX_RECORDS;
    using replay_limits::SENTINEL_REPLAY_MAX_STATUS_RECORDS;
    using replay_model::SENTINEL_REPLAY_DEDUPE_PREFIX;
    using replay_model::SENTINEL_REPLAY_REQUEST_PREFIX;
    using replay_model::SENTINEL_REPLAY_STATUS_TTL_MS;
    using replay_model::SENTINEL_REPLAY_TTL_MS;
    using replay_model::SentinelReplayCaptureStatusRow;
    using replay_model::SentinelReplayManifest;
    using replay_retention_schema::AtomicRowApplication;
    using replay_retention_schema::accountingKeyMatches;
    using replay_retention_schema::CAS_ATTEMPTS;
    using replay_retention_schema::commitAccountingMutation;
    using replay_retention_schema::countChunks;
    using replay_retention_schema::deleteChunkBatch;
    using replay_retention_schema::DeleteChunkBatchResult;
    using replay_retention_schema::EVICTION_BATCH_RECORDS;
    using replay_retention_schema::EVICTION_MAX_CHUNK_DELETES;
    using replay_retention_schema::fault;
    using replay_retention_schema::isAccountingRow;
    using replay_retention_schema::isLegacyReservation;
    using replay_retention_schema::LedgerMutation;
    using replay_retention_schema::LedgerState;
    using replay_retention_schema::LedgerStateKind;
    using replay_retention_schema::manifestKeyMatches;
    using replay_retention_schema::readLedger;
    using replay_retention_schema::RESERVATION_REAP_LIMIT;
    using replay_retention_schema::SENTINEL_REPLAY_ACCOUNTING_PREFIX;
    using replay_retention_schema::SENTINEL_REPLAY_ACCOUNTING_REASON;
    using replay_retention_schema::SENTINEL_REPLAY_BUDGET_LEDGER_KEY;
    using replay_retention_schema::SENTINEL_REPLAY_CLEAN_TARGET_RATIO;
    using replay_retention_schema::SENTINEL_REPLAY_EVICTION_PREFIX;
    using replay_retention_schema::SENTINEL_REPLAY_EVICTION_REASON;
    using replay_retention_schema::SENTINEL_REPLAY_EXPIRED_REASON;
    using replay_retention_schema::SENTINEL_REPLAY_RESERVATION_PREFIX;
    using replay_retention_schema::SENTINEL_REPLAY_RESERVATION_TTL_MS;
    using replay_retention_schema::SENTINEL_REPLAY_STORAGE_FULL_REASON;
    using replay_retention_schema::sentinelReplayAccountingKey;
    using replay_retention_schema::sentinelReplayBudgetBytes;
    using replay_retention_schema::sentinelReplayEvictionKey;
    using replay_retention_schema::sentinelReplayManifestKey;
    using replay_retention_schema::sentinelReplayMetadataReserve;
    using replay_retention_schema::sentinelReplayPayloadBudgetBytes;
    using replay_retention_schema::sentinelReplayRequestStatusKey;
    using replay_retention_schema::sentinelReplayStatusMetadataBytes;
    using replay_retention_schema::SentinelReplayAccountingRow;
    using replay_retention_schema::SentinelReplayAdmission;
    using replay_retention_schema::SentinelReplayAdmissionNotOk;
    using replay_retention_schema::SentinelReplayAdmissionOk;
    using replay_retention_schema::SentinelReplayBudgetLedger;
    using replay_retention_schema::SentinelReplayClaimKind;
    using replay_retention_schema::SentinelReplayPublication;
    using replay_retention_schema::SentinelReplayRetentionStatus;
    using replay_retention_schema::STATUS_PRUNE_BATCH;
    using replay_retention_schema::storedStatusMetadataBytes;
    using replay_retention_schema::withBudget;

    // `Math.max(64 * 1_024, Math.trunc(input.budget_bytes ?? sentinelReplayBudgetBytes()))`
    double budgetOf(const std::optional<double>& budgetBytes) {
      return rt::Math::max(64 * 1'024, rt::Math::trunc(budgetBytes ? *budgetBytes : sentinelReplayBudgetBytes()));
    }

    // The typed view of a value isAccountingRow accepted (`kv.get<SentinelReplayAccountingRow>` is a cast in the TS).
    SentinelReplayAccountingRow accountingRowOf(const rt::KvValue& value) {
      return replay_retention_schema::toSentinelReplayAccountingRow(value);
    }

    rt::KvValue ledgerValue(const SentinelReplayBudgetLedger& ledger) { return replay_retention_schema::sentinelReplayLedgerValue(ledger); }
    rt::KvValue accountingValue(const SentinelReplayAccountingRow& row) { return replay_retention_schema::sentinelReplayAccountingValue(row); }

    // `String(element)` as Array.prototype.join applies it (null, undefined and holes join as "").
    std::string joinedText(const rt::KvValue& element);

    std::string joinedArray(const rt::KvArray& items, std::string_view separator) {
      std::string out;
      for (std::size_t index = 0; index < items.size(); ++index) {
        if (index > 0) out += separator;
        rt::append(out, joinedText(items[index]));
      }
      return out;
    }

    template <class T> std::string typedArrayText(const rt::KvArrayBufferView& view) {
      const std::uint64_t length = view.byteLength / sizeof(T);
      std::string out;
      for (std::uint64_t index = 0; index < length; ++index) {
        T element{};
        std::memcpy(&element, view.buffer.data() + view.byteOffset + index * sizeof(T), sizeof(T));
        if (index > 0) out += ",";
        if constexpr (std::is_same_v<T, std::int64_t>) out += rt::BigInt(element).toString();
        else if constexpr (std::is_same_v<T, std::uint64_t>) out += rt::BigInt::fromUint64(element).toString();
        else out += rt::numberToString(static_cast<double>(element));
      }
      return out;
    }

    std::string viewText(const rt::KvArrayBufferView& view) {
      switch (view.kind) {
        case rt::KvViewKind::DataView:
          return "[object DataView]";
        case rt::KvViewKind::Int8Array:
          return typedArrayText<std::int8_t>(view);
        case rt::KvViewKind::Uint8Array:
        case rt::KvViewKind::Uint8ClampedArray:
          return typedArrayText<std::uint8_t>(view);
        case rt::KvViewKind::Int16Array:
          return typedArrayText<std::int16_t>(view);
        case rt::KvViewKind::Uint16Array:
          return typedArrayText<std::uint16_t>(view);
        case rt::KvViewKind::Int32Array:
          return typedArrayText<std::int32_t>(view);
        case rt::KvViewKind::Uint32Array:
          return typedArrayText<std::uint32_t>(view);
        case rt::KvViewKind::Float32Array:
          return typedArrayText<float>(view);
        case rt::KvViewKind::Float64Array:
          return typedArrayText<double>(view);
        case rt::KvViewKind::BigInt64Array:
          return typedArrayText<std::int64_t>(view);
        case rt::KvViewKind::BigUint64Array:
          return typedArrayText<std::uint64_t>(view);
      }
      return "";
    }

    std::string joinedText(const rt::KvValue& element) {
      return std::visit(
        [](const auto& item) -> std::string {
          using T = std::decay_t<decltype(item)>;
          if constexpr (std::is_same_v<T, rt::KvUndefined> || std::is_same_v<T, std::nullptr_t> || std::is_same_v<T, rt::KvHole>) return "";
          else if constexpr (std::is_same_v<T, bool>) return item ? "true" : "false";
          else if constexpr (std::is_same_v<T, double>) return rt::numberToString(item);
          else if constexpr (std::is_same_v<T, std::string>) return item;
          else if constexpr (std::is_same_v<T, rt::BigInt>) return item.toString();
          else if constexpr (std::is_same_v<T, rt::KvBytes>) {
            std::string out;
            for (std::size_t index = 0; index < item.size(); ++index) {
              if (index > 0) out += ",";
              out += rt::numberToString(item[index]);
            }
            return out;
          } else if constexpr (std::is_same_v<T, rt::KvArrayBufferView>) return viewText(item);
          else if constexpr (std::is_same_v<T, rt::KvArrayBuffer>) return "[object ArrayBuffer]";
          else if constexpr (std::is_same_v<T, rt::KvDate>) return rt::Date(item.time).toString();
          else if constexpr (std::is_same_v<T, rt::KvRegExp>) return "/" + item.source + "/" + item.flags;
          else if constexpr (std::is_same_v<T, rt::KvError>) {
            // Error.prototype.toString
            const std::string message = item.message.value_or("");
            if (item.name.empty()) return message;
            if (message.empty()) return item.name;
            return item.name + ": " + message;
          } else if constexpr (std::is_same_v<T, rt::KvArray>) return joinedArray(item, ",");
          else if constexpr (std::is_same_v<T, rt::KvMap>) return "[object Map]";
          else if constexpr (std::is_same_v<T, rt::KvSet>) return "[object Set]";
          else return "[object Object]";  // a plain object, or a nested KvU64 (read back as `{ value: n }`)
        },
        element.variant());
    }

    // `key.join("\u0000")`
    std::string joinedKey(const rt::KvKey& key) {
      std::string out;
      for (std::size_t index = 0; index < key.size(); ++index) {
        if (index > 0) out += std::string(1, '\0');
        rt::append(out, key[index].toString());
      }
      return out;
    }

    // `type StatusCandidate = Readonly<{ key: KvKey; versionstamp: string; bytes: number; captured_at_ms: number }>`
    struct StatusCandidate {
      rt::KvKey key;
      std::string versionstamp;
      double bytes;
      double captured_at_ms;
    };

    rt::Task<std::optional<std::vector<StatusCandidate>>> statusCandidates(rt::Kv& kv) {
      std::vector<StatusCandidate> candidates;
      {
        rt::KvListSelector selector;
        selector.prefix = SENTINEL_REPLAY_REQUEST_PREFIX;
        const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector));
        for (;;) {
          const std::optional<rt::KvEntry<>> entry = co_await iterator->next();
          if (!entry) break;
          if (!replay_observation::isSentinelReplayCaptureStatusRow(entry->value) ||
              !replay_retention_metadata::metadataEntryMatches(*entry, SENTINEL_REPLAY_REQUEST_PREFIX)) {
            co_await iterator->return_();
            co_return std::nullopt;
          }
          candidates.push_back(StatusCandidate{
            .key = entry->key,
            .versionstamp = entry->versionstamp,
            .bytes = sentinelReplayStatusMetadataBytes(entry->value),
            .captured_at_ms = entry->value["captured_at_ms"].asNumber(),
          });
        }
      }
      {
        rt::KvListSelector selector;
        selector.prefix = SENTINEL_REPLAY_EVICTION_PREFIX;
        const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector));
        for (;;) {
          const std::optional<rt::KvEntry<>> entry = co_await iterator->next();
          if (!entry) break;
          if (!replay_retention_metadata::metadataEntryMatches(*entry, SENTINEL_REPLAY_EVICTION_PREFIX)) {
            co_await iterator->return_();
            co_return std::nullopt;
          }
          // `entry.value as { evicted_at_ms: number }`: the tombstone contract made it a counter.
          const rt::KvValue& value = entry->value;
          candidates.push_back(StatusCandidate{
            .key = entry->key,
            .versionstamp = entry->versionstamp,
            .bytes = sentinelReplayStatusMetadataBytes(value),
            .captured_at_ms = value["evicted_at_ms"].asNumber(),
          });
        }
      }
      rt::sort(candidates, [](const StatusCandidate& left, const StatusCandidate& right) { return left.captured_at_ms - right.captured_at_ms; });
      co_return candidates;
    }

    rt::Task<bool> admitSentinelReplayStatusMetadataTask(rt::Kv& kv, SentinelReplayStatusMetadataInput input);
    rt::Task<SentinelReplayStagingFence> advanceSentinelReplayStagingFenceTask(rt::Kv& kv, SentinelReplayStagingFenceInput input);
  }

  rt::Task<bool> admitSentinelReplayStatusMetadata(rt::Kv& kv, const SentinelReplayStatusMetadataInput& input) {
    return admitSentinelReplayStatusMetadataTask(kv, input);
  }

  rt::Task<double> pruneCaptureOwnedStatusMetadata(rt::Kv& kv, double nowMs, double extraRecords, double extraBytes, double maxDeletes) {
    auto pruning = pruneCaptureOwnedStatusMetadata(kv, nowMs, extraRecords, extraBytes, maxDeletes, std::nullopt);
    const double deleted = co_await std::move(pruning);
    co_return deleted;
  }

  rt::Task<double> pruneCaptureOwnedStatusMetadata(rt::Kv& kv, double nowMs, double extraRecords, double extraBytes, double maxDeletes,
                                                   std::optional<double> budgetBytesOverride) {
    const double budgetBytes = budgetOf(budgetBytesOverride);
    const double reserve = sentinelReplayMetadataReserve(budgetBytes);
    const LedgerState initial = co_await readLedger(kv, budgetBytes);
    if (initial.kind == LedgerStateKind::corrupt) co_return 0;
    const SentinelReplayBudgetLedger ledger = withBudget(*initial.ledger, budgetBytes);
    if (ledger.status_records + extraRecords <= SENTINEL_REPLAY_MAX_STATUS_RECORDS && ledger.metadata_bytes + extraBytes <= reserve) co_return 0;
    auto reconciling = replay_retention_metadata::reconcileSentinelReplayStatusMetadata(kv, budgetBytes);
    const std::string reconciled = co_await std::move(reconciling);
    if (reconciled == "failed") co_return 0;
    double deleted = 0;
    auto listing = statusCandidates(kv);
    const std::optional<std::vector<StatusCandidate>> listed = co_await std::move(listing);
    const std::vector<StatusCandidate> candidates = listed ? *listed : std::vector<StatusCandidate>();
    for (const StatusCandidate& candidate : candidates) {
      if (deleted >= maxDeletes) break;
      const LedgerState state = co_await readLedger(kv, budgetBytes);
      if (state.kind == LedgerStateKind::corrupt) break;
      const SentinelReplayBudgetLedger current = withBudget(*state.ledger, budgetBytes);
      if (current.status_records + extraRecords <= SENTINEL_REPLAY_MAX_STATUS_RECORDS && current.metadata_bytes + extraBytes <= reserve) break;
      SentinelReplayBudgetLedger next = current;
      next.status_records = rt::Math::max(0, current.status_records - 1);
      next.metadata_bytes = rt::Math::max(0, current.metadata_bytes - candidate.bytes);
      next.status_pruned_records = current.status_pruned_records + 1;
      next.last_warning_at_ms = nowMs;
      const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
      operation->check(rt::AtomicCheck(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, state.versionstamp))
        .check(rt::AtomicCheck(candidate.key, candidate.versionstamp))
        .set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, ledgerValue(next))
        .delete_(candidate.key);
      const rt::KvCommitResult committed = co_await operation->commit();
      if (committed.ok) deleted += 1;
    }
    co_return deleted;
  }

  namespace {
    rt::Task<bool> admitSentinelReplayStatusMetadataTask(rt::Kv& kv, SentinelReplayStatusMetadataInput input) {
      const double budgetBytes = budgetOf(input.budget_bytes);
      const double reserve = sentinelReplayMetadataReserve(budgetBytes);
      const double bytes = sentinelReplayStatusMetadataBytes(input.row);
      for (double attempt = 0; attempt < CAS_ATTEMPTS; attempt += 1) {
        // The sweep counts every in-scope row, so until it has finished no row may
        // also be counted by its own admission, or the same row is charged twice.
        // Never wait for the background owner fill on this client-visible failure path.
        auto bootstrapping = replay_retention_bootstrap::bootstrapBeforeMutation(kv, budgetBytes, input.now_ms, 0);
        const std::optional<SentinelReplayBudgetLedger> bootstrapped = co_await std::move(bootstrapping);
        if (!bootstrapped) co_return false;
        auto pruning = pruneCaptureOwnedStatusMetadata(kv, input.now_ms, 1, bytes, 1, budgetBytes);
        co_await std::move(pruning);
        // The bound tracks the LIVE row set, so replacing the row already stored at
        // this key adds one record and the byte difference, never a second full
        // charge. Checking the exact versionstamp of the replaced row keeps that
        // delta true while another writer touches the same key.
        rt::Promise<LedgerState> ledgerRead(readLedger(kv, budgetBytes));
        rt::Promise<rt::KvEntryMaybe<>> existingRead(kv.get(input.key));
        auto reads = rt::all(ledgerRead, existingRead);
        const std::tuple<LedgerState, rt::KvEntryMaybe<>> results = co_await reads.wait();
        const LedgerState& current = std::get<0>(results);
        const rt::KvEntryMaybe<>& existing = std::get<1>(results);
        if (current.kind == LedgerStateKind::corrupt) co_return false;
        const SentinelReplayBudgetLedger ledger = withBudget(*current.ledger, budgetBytes);
        const double addedRecords = existing.value.isNull() ? 1 : 0;
        const double addedBytes = bytes - storedStatusMetadataBytes(existing.value);
        if (ledger.status_records + addedRecords > SENTINEL_REPLAY_MAX_STATUS_RECORDS || ledger.metadata_bytes + addedBytes > reserve) co_return false;
        SentinelReplayBudgetLedger next = ledger;
        next.status_records = ledger.status_records + addedRecords;
        next.metadata_bytes = rt::Math::max(0, ledger.metadata_bytes + addedBytes);
        rt::KvSetOptions rowOptions;
        rowOptions.expireIn = input.ttl_ms;
        const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
        operation->check(rt::AtomicCheck(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, current.versionstamp))
          .check(rt::AtomicCheck(input.key, existing.versionstamp))
          .set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, ledgerValue(next))
          .set(input.key, input.row, rowOptions);
        const rt::KvCommitResult committed = co_await operation->commit();
        if (committed.ok) co_return true;
      }
      co_return false;
    }

    rt::Task<SentinelReplayStagingFence> advanceSentinelReplayStagingFenceTask(rt::Kv& kv, SentinelReplayStagingFenceInput input) {
      const double budgetBytes = budgetOf(input.budget_bytes);
      for (double attempt = 0; attempt < CAS_ATTEMPTS; attempt += 1) {
        const rt::KvEntryMaybe<> entry = co_await kv.get(input.accounting_key);
        if (!isAccountingRow(entry.value) || !accountingKeyMatches(input.accounting_key, accountingRowOf(entry.value))) {
          co_return SentinelReplayStagingFenceNotOk{.reason = "missing"};
        }
        const SentinelReplayAccountingRow row = accountingRowOf(entry.value);
        if (row.state == "revoked") co_return SentinelReplayStagingFenceNotOk{.reason = "revoked"};
        if (row.state != "reserved") co_return SentinelReplayStagingFenceNotOk{.reason = "missing"};
        if (row.fence != input.fence) co_return SentinelReplayStagingFenceNotOk{.reason = "revoked"};
        if (row.expires_at_ms <= input.now_ms) co_return SentinelReplayStagingFenceNotOk{.reason = "expired"};
        const LedgerState state = co_await readLedger(kv, budgetBytes);
        if (state.kind == LedgerStateKind::corrupt) co_return SentinelReplayStagingFenceNotOk{.reason = "missing"};
        SentinelReplayAccountingRow next = row;
        next.stage = row.stage + 1;
        const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
        operation->check(rt::AtomicCheck(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, state.versionstamp))
          .check(rt::AtomicCheck(input.accounting_key, entry.versionstamp))
          .set(input.accounting_key, accountingValue(next));
        const rt::KvCommitResult committed = co_await operation->commit();
        if (committed.ok) co_return SentinelReplayStagingFenceOk{.stage = next.stage, .versionstamp = committed.versionstamp};
      }
      co_return SentinelReplayStagingFenceNotOk{.reason = "missing"};
    }

    /** Everything one publish attempt must re-verify before it may write anything. */
    struct PublishAttemptContext {
      rt::KvKey accountingKey;
      SentinelReplayAccountingRow accounting;
      double actualCharge;
      double payloadBudget;
      double reserve;
      /** Net record and byte movement of the status row this attempt replaces. */
      double statusRecordsDelta;
      double statusBytesDelta;
      double leaseNowMs;
    };

    /**
     * Read the metadata bounds and, when the record cap or the byte reserve would be
     * exceeded, prune the oldest capture-owned status rows first. Returns the metadata
     * reserve, or null when the ledger is corrupt and cannot be trusted.
     */
    rt::Task<std::optional<double>> prepareStatusMetadataRoom(rt::Kv& kv, double budgetBytes, double statusBytes, double nowMs) {
      const LedgerState state = co_await readLedger(kv, budgetBytes);
      if (state.kind == LedgerStateKind::corrupt) co_return std::nullopt;
      const SentinelReplayBudgetLedger ledger = withBudget(*state.ledger, budgetBytes);
      const double reserve = sentinelReplayMetadataReserve(budgetBytes);
      const bool tooManyRecords = ledger.status_records + 1 > SENTINEL_REPLAY_MAX_STATUS_RECORDS;
      const bool tooManyBytes = ledger.metadata_bytes + statusBytes > reserve;
      if (tooManyRecords || tooManyBytes) {
        auto pruning = pruneCaptureOwnedStatusMetadata(kv, nowMs, 1, statusBytes, STATUS_PRUNE_BATCH, budgetBytes);
        co_await std::move(pruning);
      }
      co_return reserve;
    }

    /**
     * Re-verify the reservation against the live row and ledger for one publish
     * attempt. The row must still be this writer's unexpired `reserved` row with its
     * original identity, the actual charge must fit both the reservation that was
     * already held back and the payload budget, and the status row must fit the
     * metadata reserve. Returns null when the reservation may no longer publish.
     */
    std::optional<SentinelReplayAccountingRow> publishAttemptRow(const rt::KvEntryMaybe<>& entry, const SentinelReplayBudgetLedger& ledger,
                                                                 const PublishAttemptContext& context) {
      if (!isAccountingRow(entry.value)) return std::nullopt;
      const SentinelReplayAccountingRow row = accountingRowOf(entry.value);
      if (!accountingKeyMatches(context.accountingKey, row)) return std::nullopt;
      if (row.state != "reserved") return std::nullopt;
      if (row.fence != context.accounting.fence) return std::nullopt;
      if (row.expires_at_ms <= context.leaseNowMs) return std::nullopt;
      if (row.created_at_ms != context.accounting.created_at_ms) return std::nullopt;
      if (!rt::Number::isSafeInteger(context.actualCharge) || context.actualCharge <= 0) return std::nullopt;
      if (context.actualCharge > row.bytes) return std::nullopt;
      if (ledger.reserved_bytes < row.bytes) return std::nullopt;
      if (ledger.records + 1 > SENTINEL_REPLAY_MAX_RECORDS) return std::nullopt;
      // The published charge must fit what the reservation already held back: the
      // reserved charge is released in the same commit that stores the actual one.
      if (ledger.stored_bytes + ledger.reserved_bytes - row.bytes + context.actualCharge > context.payloadBudget) return std::nullopt;
      if (ledger.status_records + context.statusRecordsDelta > SENTINEL_REPLAY_MAX_STATUS_RECORDS) return std::nullopt;
      if (ledger.metadata_bytes + context.statusBytesDelta > context.reserve) return std::nullopt;
      return row;
    }

    rt::Task<std::optional<SentinelReplayPublication>> prepareSentinelReplayPublicationTask(rt::Kv& kv, SentinelReplayAccountingRow accounting,
                                                                                           rt::KvKey accountingKey, double actualCharge,
                                                                                           SentinelReplayPublicationOptions options) {
      const double budgetBytes = budgetOf(options.budget_bytes);
      const double payloadBudget = sentinelReplayPayloadBudgetBytes(budgetBytes);
      const double statusBytes = rt::Math::max(0, rt::Math::trunc(options.status_bytes ? *options.status_bytes : 0));
      auto preparing = prepareStatusMetadataRoom(kv, budgetBytes, statusBytes, options.now_ms);
      const std::optional<double> reserve = co_await std::move(preparing);
      if (!reserve) co_return std::nullopt;
      for (double attempt = 0; attempt < CAS_ATTEMPTS; attempt += 1) {
        rt::Promise<rt::KvEntryMaybe<>> entryRead(kv.get(accountingKey));
        rt::Promise<LedgerState> ledgerRead(readLedger(kv, budgetBytes));
        rt::Promise<rt::KvEntryMaybe<>> statusRead(kv.get(options.status_key));
        auto reads = rt::all(entryRead, ledgerRead, statusRead);
        const std::tuple<rt::KvEntryMaybe<>, LedgerState, rt::KvEntryMaybe<>> results = co_await reads.wait();
        const rt::KvEntryMaybe<>& entry = std::get<0>(results);
        const LedgerState& ledgerState = std::get<1>(results);
        const rt::KvEntryMaybe<>& statusEntry = std::get<2>(results);
        if (ledgerState.kind == LedgerStateKind::corrupt) co_return std::nullopt;
        const SentinelReplayBudgetLedger ledger = withBudget(*ledgerState.ledger, budgetBytes);
        // Publishing replaces the request own status row, so the metadata bound moves
        // by the difference instead of charging a second full row for it.
        const double statusRecordsDelta = statusEntry.value.isNull() ? 1 : 0;
        const double statusBytesDelta = statusBytes - storedStatusMetadataBytes(statusEntry.value);
        const PublishAttemptContext context{
          .accountingKey = accountingKey,
          .accounting = accounting,
          .actualCharge = actualCharge,
          .payloadBudget = payloadBudget,
          .reserve = *reserve,
          .statusRecordsDelta = statusRecordsDelta,
          .statusBytesDelta = statusBytesDelta,
          .leaseNowMs = options.lease_now_ms,
        };
        const std::optional<SentinelReplayAccountingRow> row = publishAttemptRow(entry, ledger, context);
        if (!row) co_return std::nullopt;
        const double stored = ledger.stored_bytes + actualCharge;
        SentinelReplayBudgetLedger nextLedger = ledger;
        nextLedger.reserved_bytes = rt::Math::max(0, ledger.reserved_bytes - row->bytes);
        nextLedger.stored_bytes = stored;
        nextLedger.records = ledger.records + 1;
        nextLedger.status_records = rt::Math::max(0, ledger.status_records + statusRecordsDelta);
        nextLedger.metadata_bytes = rt::Math::max(0, ledger.metadata_bytes + statusBytesDelta);
        nextLedger.over_budget = stored > payloadBudget;
        nextLedger.last_warning_at_ms = stored > payloadBudget ? std::optional<double>(options.now_ms) : ledger.last_warning_at_ms;
        // The short reservation lease becomes the payload TTL only here, at publish.
        SentinelReplayAccountingRow nextRow = *row;
        nextRow.state = "stored";
        nextRow.bytes = actualCharge;
        nextRow.expires_at_ms = options.now_ms + SENTINEL_REPLAY_TTL_MS;
        nextRow.status_expires_at_ms = options.now_ms + SENTINEL_REPLAY_STATUS_TTL_MS;
        co_return SentinelReplayPublication{
          .ledger_key = SENTINEL_REPLAY_BUDGET_LEDGER_KEY,
          .ledger_versionstamp = ledgerState.versionstamp,
          .ledger = std::move(nextLedger),
          .accounting_key = accountingKey,
          .accounting_versionstamp = entry.versionstamp,
          .accounting = std::move(nextRow),
          .status_key = options.status_key,
          .status_versionstamp = statusEntry.versionstamp,
        };
      }
      co_return std::nullopt;
    }
  }

  rt::Task<SentinelReplayStagingFence> advanceSentinelReplayStagingFence(rt::Kv& kv, const SentinelReplayStagingFenceInput& input) {
    return advanceSentinelReplayStagingFenceTask(kv, input);
  }

  rt::Task<std::optional<SentinelReplayPublication>> prepareSentinelReplayPublication(rt::Kv& kv, const SentinelReplayAccountingRow& accounting,
                                                                                     const rt::KvKey& accountingKey, double actualCharge,
                                                                                     const SentinelReplayPublicationOptions& options) {
    return prepareSentinelReplayPublicationTask(kv, accounting, accountingKey, actualCharge, options);
  }

  namespace {
    // `Readonly<{ row: SentinelReplayAccountingRow; revoked: boolean }>`
    struct RevokedAccounting {
      SentinelReplayAccountingRow row;
      bool revoked;
    };

    /**
     * Fence a `reserved` row so its writer can never stage or publish again, and
     * report the row cleanup should act on. A row that already published (`stored`)
     * or was already claimed (`evicting`) is NOT revoked here: releasing its charge is
     * the ordinary eviction/TTL path's job, never this cleanup's.
     */
    rt::Task<RevokedAccounting> revokeReservedAccounting(rt::Kv& kv, SentinelReplayAccountingRow accounting, rt::KvKey accountingKey) {
      if (accounting.state == "revoked") co_return RevokedAccounting{.row = accounting, .revoked = true};
      if (accounting.state != "reserved") co_return RevokedAccounting{.row = accounting, .revoked = false};
      for (double attempt = 0; attempt < CAS_ATTEMPTS; attempt += 1) {
        const rt::KvEntryMaybe<> entry = co_await kv.get(accountingKey);
        if (!isAccountingRow(entry.value) || entry.value["fence"].asNumber() != accounting.fence) break;
        const SentinelReplayAccountingRow current = accountingRowOf(entry.value);
        if (current.state == "revoked") co_return RevokedAccounting{.row = current, .revoked = true};
        if (current.state == "stored") co_return RevokedAccounting{.row = current, .revoked = false};
        if (current.state != "reserved") break;
        SentinelReplayAccountingRow next = current;
        next.state = "revoked";
        next.fence = current.fence + 1;
        const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
        operation->check(rt::AtomicCheck(accountingKey, entry.versionstamp)).set(accountingKey, accountingValue(next));
        const rt::KvCommitResult committed = co_await operation->commit();
        if (committed.ok) co_return RevokedAccounting{.row = std::move(next), .revoked = true};
      }
      co_return RevokedAccounting{.row = accounting, .revoked = false};
    }

    rt::Task<SentinelReplayAbandonment> abandonSentinelReplayAccountingTask(rt::Kv& kv, SentinelReplayAccountingRow accounting, rt::KvKey accountingKey,
                                                                            SentinelReplayAbandonOptions options) {
      const double budgetBytes = budgetOf(options.budget_bytes);
      const double maxDeletes = rt::Math::max(1, options.max_chunk_deletes ? *options.max_chunk_deletes : EVICTION_MAX_CHUNK_DELETES);
      auto revoking = revokeReservedAccounting(kv, accounting, accountingKey);
      const RevokedAccounting revocation = co_await std::move(revoking);
      const SentinelReplayAccountingRow& current = revocation.row;
      if (!revocation.revoked) co_return SentinelReplayAbandonment{.revoked = false, .deleted_chunks = 0, .released = false, .remaining = 0};
      auto deleting = deleteChunkBatch(kv, current.capture_id, maxDeletes);
      const DeleteChunkBatchResult cleanup = co_await std::move(deleting);
      double remaining = cleanup.remaining;
      if (!(cleanup.remaining > 0)) {
        auto counting = countChunks(kv, current.capture_id);
        remaining = co_await std::move(counting);
      }
      if (remaining > 0) co_return SentinelReplayAbandonment{.revoked = true, .deleted_chunks = cleanup.deleted, .released = false, .remaining = remaining};
      // Release the charge only in the same commit that deletes the row, and only
      // when a fresh listing proves no chunk survives.
      bool released = false;
      for (double attempt = 0; attempt < CAS_ATTEMPTS; attempt += 1) {
        const rt::KvEntryMaybe<> entry = co_await kv.get(accountingKey);
        if (!isAccountingRow(entry.value) || entry.value["fence"].asNumber() != current.fence) break;
        // Bind the narrowed row: TypeScript does not carry a property narrowing into
        // the mutation closure, and no assertion should be needed for that.
        const SentinelReplayAccountingRow releasedRow = accountingRowOf(entry.value);
        const double releasedBytes = releasedRow.bytes;
        LedgerMutation mutate = [releasedBytes](const SentinelReplayBudgetLedger& ledger) {
          SentinelReplayBudgetLedger next = ledger;
          next.reserved_bytes = rt::Math::max(0, ledger.reserved_bytes - releasedBytes);
          return next;
        };
        AtomicRowApplication applyRow = [accountingKey](rt::AtomicOperation& operation) -> rt::AtomicOperation& { return operation.delete_(accountingKey); };
        auto committing = commitAccountingMutation(kv, accountingKey, entry.versionstamp, std::move(mutate), std::move(applyRow), budgetBytes);
        released = co_await std::move(committing);
        if (released) break;
      }
      co_return SentinelReplayAbandonment{.revoked = true, .deleted_chunks = cleanup.deleted, .released = released, .remaining = 0};
    }
  }

  rt::Task<SentinelReplayAbandonment> abandonSentinelReplayAccounting(rt::Kv& kv, const SentinelReplayAccountingRow& accounting, const rt::KvKey& accountingKey,
                                                                      const SentinelReplayAbandonOptions& options) {
    return abandonSentinelReplayAccountingTask(kv, accounting, accountingKey, options);
  }

  namespace {
    // `Readonly<{ key: KvKey; row: SentinelReplayAccountingRow }>`
    struct AccountingEntry {
      rt::KvKey key;
      SentinelReplayAccountingRow row;
    };

    /** Find fenced/abandoned rows with bounded work and a durable prefix cursor. */
    // Keep the cursor outside the accounting prefix so discovery never counts it as a row.
    const rt::KvKey& SENTINEL_REPLAY_REAP_CURSOR_KEY() {
      static const rt::KvKey key{"uos_ai", "sentinel_replay", "v1", "reap_cursor"};
      return key;
    }

    rt::Task<std::vector<AccountingEntry>> reapCandidates(rt::Kv& kv, double nowMs, double limit) {
      const rt::KvEntryMaybe<> cursorEntry = co_await kv.get(SENTINEL_REPLAY_REAP_CURSOR_KEY());
      std::optional<std::string> cursor;
      if (cursorEntry.value.isString() && rt::length(cursorEntry.value.asString()) > 0) cursor = cursorEntry.value.asString();
      rt::KvListSelector selector;
      selector.prefix = SENTINEL_REPLAY_ACCOUNTING_PREFIX;
      rt::KvListOptions options;
      options.limit = limit * 4;
      options.cursor = cursor;
      const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector), std::move(options));
      std::vector<AccountingEntry> found;
      const auto isCandidate = [nowMs](const SentinelReplayAccountingRow& row) {
        return row.state == "revoked" || (row.state == "reserved" && row.expires_at_ms <= nowMs);
      };
      for (;;) {
        const std::optional<rt::KvEntry<>> entry = co_await iterator->next();
        if (!entry) break;
        if (isAccountingRow(entry->value)) {
          const SentinelReplayAccountingRow row = accountingRowOf(entry->value);
          if (accountingKeyMatches(entry->key, row) && isCandidate(row)) found.push_back(AccountingEntry{.key = entry->key, .row = row});
        }
        if (static_cast<double>(found.size()) >= limit) {
          co_await iterator->return_();
          break;
        }
      }
      const std::string listed = iterator->cursor();
      const std::optional<std::string> nextCursor = listed.empty() ? std::nullopt : std::optional<std::string>(listed);
      const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
      operation->check(rt::AtomicCheck(SENTINEL_REPLAY_REAP_CURSOR_KEY(), cursorEntry.versionstamp));
      if (!nextCursor) operation->delete_(SENTINEL_REPLAY_REAP_CURSOR_KEY());
      else operation->set(SENTINEL_REPLAY_REAP_CURSOR_KEY(), rt::KvValue(*nextCursor));
      co_await operation->commit();
      co_return found;
    }

    rt::Task<double> resumeEvictingRows(rt::Kv& kv, double nowMs, double budgetBytes, double limit);

    rt::Task<double> reapAccountingRows(rt::Kv& kv, double nowMs, double budgetBytes) {
      auto finding = reapCandidates(kv, nowMs, RESERVATION_REAP_LIMIT);
      const std::vector<AccountingEntry> candidates = co_await std::move(finding);
      double reaped = 0;
      for (const AccountingEntry& candidate : candidates) {
        const SentinelReplayAbandonOptions options{.now_ms = nowMs, .budget_bytes = budgetBytes};
        auto abandoning = abandonSentinelReplayAccounting(kv, candidate.row, candidate.key, options);
        const SentinelReplayAbandonment result = co_await std::move(abandoning);
        if (result.revoked) reaped += 1;
      }
      // A victim whose final commit failed keeps its charge and is resumed here.
      auto resuming = resumeEvictingRows(kv, nowMs, budgetBytes, RESERVATION_REAP_LIMIT);
      const double resumed = co_await std::move(resuming);
      reaped += resumed;
      co_return reaped;
    }

    rt::Task<double> reapLegacyReservations(rt::Kv& kv, double budgetBytes) {
      double reaped = 0;
      rt::KvListSelector selector;
      selector.prefix = SENTINEL_REPLAY_RESERVATION_PREFIX;
      rt::KvListOptions options;
      options.limit = RESERVATION_REAP_LIMIT;
      const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector), std::move(options));
      for (;;) {
        std::optional<rt::KvEntry<>> entry = co_await iterator->next();
        if (!entry) break;
        if (!isLegacyReservation(entry->value)) continue;
        // A throw inside the loop body closes the iterator (for await calls `return()`).
        rt::Reason failure;
        try {
          auto releasing = replay_retention_bootstrap::releaseLegacyReservation(
            kv, entry->key, replay_retention_bootstrap::toSentinelReplayLegacyReservation(entry->value), budgetBytes);
          co_await std::move(releasing);
        } catch (...) {
          failure = std::current_exception();
        }
        if (failure) {
          co_await iterator->return_();
          std::rethrow_exception(failure);
        }
        reaped += 1;
      }
      co_return reaped;
    }

    rt::Task<void> recordSkipReason(rt::Kv& kv, std::optional<std::string> reason, double nowMs, double budgetBytes) {
      for (double attempt = 0; attempt < CAS_ATTEMPTS; attempt += 1) {
        const LedgerState state = co_await readLedger(kv, budgetBytes);
        if (state.kind == LedgerStateKind::corrupt) co_return;
        const SentinelReplayBudgetLedger ledger = withBudget(*state.ledger, budgetBytes);
        SentinelReplayBudgetLedger next = ledger;
        next.last_skip_reason = reason;
        next.last_full_at_ms = reason && *reason == SENTINEL_REPLAY_STORAGE_FULL_REASON ? std::optional<double>(nowMs) : ledger.last_full_at_ms;
        next.last_warning_at_ms = !reason ? ledger.last_warning_at_ms : std::optional<double>(nowMs);
        const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
        operation->check(rt::AtomicCheck(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, state.versionstamp)).set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, ledgerValue(next));
        const rt::KvCommitResult committed = co_await operation->commit();
        if (committed.ok) co_return;
      }
    }

    /**
     * Delete a claimed victim's manifest and, only when the dedupe row still
     * references that exact manifest key, its dedupe row. The CAS is what stops an
     * old capture's eviction from deleting a newer capture that shares the
     * fingerprint, and a manifest that no longer matches the claim is left alone.
     */
    rt::Task<void> deleteClaimedVictimPayload(rt::Kv& kv, SentinelReplayAccountingRow row, rt::KvKey manifestKey) {
      const rt::KvEntryMaybe<> manifestEntry = co_await kv.get(manifestKey);
      if (!manifestEntry.value.isNull() || manifestEntry.versionstamp) {
        if (!replay_read::isSentinelReplayManifest(manifestEntry.value)) co_return;
        const SentinelReplayManifest manifest = replay_read::toSentinelReplayManifest(manifestEntry.value);
        if (!manifestKeyMatches(manifestKey, manifest) || manifest.fingerprint != row.fingerprint) co_return;
        fault("delete");
        co_await kv.delete_(manifestKey);
      }
      const rt::KvKey dedupeKey{SENTINEL_REPLAY_DEDUPE_PREFIX, row.fingerprint};
      const rt::KvEntryMaybe<> dedupeEntry = co_await kv.get(dedupeKey);
      // `dedupeEntry.value?.manifest_key`
      const rt::KvValue& recorded = dedupeEntry.value["manifest_key"];
      if (!recorded.isArray()) co_return;
      if (recorded.asArray().size() != 7) co_return;
      if (joinedArray(recorded.asArray(), std::string(1, '\0')) != joinedKey(manifestKey)) co_return;
      const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
      operation->check(rt::AtomicCheck(dedupeKey, dedupeEntry.versionstamp)).delete_(dedupeKey);
      co_await operation->commit();
    }

    /**
     * The ledger transition for one finalized victim: release exactly its charge and
     * count it as either evicted or expired. Status/tombstone counters move only when
     * `statusFits`, because the metadata reserve is a hard bound even during cleanup.
     */
    // `kind`: "evicted" | "expired"
    SentinelReplayBudgetLedger finalizedLedger(const SentinelReplayBudgetLedger& ledger, const SentinelReplayAccountingRow& row, const std::string& kind,
                                               double nowMs, double budgetBytes, double statusRecordsDelta, double metadataBytesDelta) {
      const double stored = rt::Math::max(0, ledger.stored_bytes - row.bytes);
      SentinelReplayBudgetLedger next = ledger;
      next.stored_bytes = stored;
      next.records = rt::Math::max(0, ledger.records - 1);
      next.status_records = rt::Math::max(0, ledger.status_records + statusRecordsDelta);
      next.metadata_bytes = rt::Math::max(0, ledger.metadata_bytes + metadataBytesDelta);
      next.evicted_bytes = kind == "evicted" ? ledger.evicted_bytes + row.bytes : ledger.evicted_bytes;
      next.evicted_records = kind == "evicted" ? ledger.evicted_records + 1 : ledger.evicted_records;
      next.expired_bytes = kind == "expired" ? ledger.expired_bytes + row.bytes : ledger.expired_bytes;
      next.expired_records = kind == "expired" ? ledger.expired_records + 1 : ledger.expired_records;
      next.last_eviction_at_ms = kind == "evicted" ? std::optional<double>(nowMs) : ledger.last_eviction_at_ms;
      next.over_budget = stored > sentinelReplayPayloadBudgetBytes(budgetBytes);
      return next;
    }

    /** Ownerless legacy victims retain a tombstone without inventing a request-status row. */
    // `existingStatus: KvEntryMaybe<unknown> | null` (std::nullopt is null).
    rt::AtomicOperation& applyClaimedVictimMetadata(rt::AtomicOperation& operation, const std::optional<rt::KvEntryMaybe<>>& existingStatus,
                                                    const rt::KvValue& statusRow, const rt::KvKey& tombstoneKey, const rt::KvValue& tombstone, bool statusFits) {
      rt::AtomicOperation* applied = &operation;
      if (existingStatus) {
        applied = &applied->check(rt::AtomicCheck(existingStatus->key, existingStatus->versionstamp)).delete_(existingStatus->key);
      }
      if (!statusFits) return *applied;
      rt::KvSetOptions options;
      options.expireIn = SENTINEL_REPLAY_STATUS_TTL_MS;
      applied = &applied->set(tombstoneKey, tombstone, options);
      return !existingStatus ? *applied : applied->set(existingStatus->key, statusRow, options);
    }

    // `Readonly<{ finalized: boolean; chunks: number }>`
    struct FinalizedVictim {
      bool finalized;
      double chunks;
    };

    // `hasOwner ? kv.get(statusKey) : Promise.resolve(null)`: the read, as the promise Promise.all takes.
    rt::Task<std::optional<rt::KvEntryMaybe<>>> readOptionalEntry(rt::Kv& kv, rt::KvKey key) {
      rt::KvEntryMaybe<> entry = co_await kv.get(std::move(key));
      co_return entry;
    }

    /**
     * Delete one already-claimed victim's payload and release its charge in a single
     * atomic commit. The charge is released only after a fresh listing proves the
     * chunk prefix is empty, so a partial deletion never frees capacity.
     */
    rt::Task<FinalizedVictim> finalizeClaimedVictim(rt::Kv& kv, rt::KvKey key, SentinelReplayAccountingRow row, double nowMs, double budgetBytes,
                                                    double maxChunkDeletes) {
      if (!row.claim_kind) co_return FinalizedVictim{.finalized = false, .chunks = 0};
      const std::string kind = *row.claim_kind;
      const std::string reason(kind == "evicted" ? SENTINEL_REPLAY_EVICTION_REASON : SENTINEL_REPLAY_EXPIRED_REASON);
      auto deleting = deleteChunkBatch(kv, row.capture_id, maxChunkDeletes);
      const DeleteChunkBatchResult cleanup = co_await std::move(deleting);
      double remaining = cleanup.remaining;
      if (!(cleanup.remaining > 0)) {
        auto counting = countChunks(kv, row.capture_id);
        remaining = co_await std::move(counting);
      }
      if (remaining > 0) co_return FinalizedVictim{.finalized = false, .chunks = cleanup.deleted};
      const SentinelReplayManifest keyedManifest{
        .version = 1,
        .capture_id = row.capture_id,
        .fingerprint = row.fingerprint,
        .case_group_digest = "",
        .captured_at_ms = row.created_at_ms,
        .expires_at_ms = row.expires_at_ms,
        .algorithm = "AES-256-GCM",
        .compression = "gzip",
        .iv = "",
        .chunk_count = 0,
        .ciphertext_bytes = 0,
      };
      const rt::KvKey manifestKey = sentinelReplayManifestKey(keyedManifest);
      auto deletingPayload = deleteClaimedVictimPayload(kv, row, manifestKey);
      co_await std::move(deletingPayload);
      const SentinelReplayCaptureStatusRow statusRecord{
        .version = 1,
        .request_id = row.request_id,
        .status = kind,
        .reason = reason,
        .captured_at_ms = row.created_at_ms,
        .manifest_key = std::nullopt,
        .fingerprint = std::nullopt,
        .expires_at_ms = std::nullopt,
      };
      const rt::KvValue statusRow = replay_model::toKvValue(statusRecord);
      rt::KvObject tombstoneObject;
      tombstoneObject.set("version", rt::KvValue(1));
      tombstoneObject.set("fingerprint", rt::KvValue(row.fingerprint));
      tombstoneObject.set("request_id", rt::KvValue(row.request_id));
      tombstoneObject.set("evicted_at_ms", rt::KvValue(nowMs));
      tombstoneObject.set("reason", rt::KvValue(reason));
      const rt::KvValue tombstone(std::move(tombstoneObject));
      const rt::KvKey statusKey = sentinelReplayRequestStatusKey(row.request_id);
      const bool hasOwner = row.request_id != "";
      const double statusRows = hasOwner ? 2 : 1;
      const rt::KvKey tombstoneKey = sentinelReplayEvictionKey(row.fingerprint);
      const double statusBytes = (hasOwner ? sentinelReplayStatusMetadataBytes(statusRow) : 0) + sentinelReplayStatusMetadataBytes(tombstone);
      auto pruning = pruneCaptureOwnedStatusMetadata(kv, nowMs, statusRows, statusBytes, STATUS_PRUNE_BATCH, budgetBytes);
      co_await std::move(pruning);
      // Read the rows this commit really replaces or deletes. The bound tracks the
      // live row set, so re-creating the victim status row and adding its tombstone
      // may move it only by the difference; the versionstamps checked below keep that
      // delta true while another writer touches those keys.
      rt::Promise<LedgerState> ledgerRead(readLedger(kv, budgetBytes));
      rt::Promise<std::optional<rt::KvEntryMaybe<>>> statusRead = hasOwner
                                                                    ? rt::Promise<std::optional<rt::KvEntryMaybe<>>>(readOptionalEntry(kv, statusKey))
                                                                    : rt::Promise<std::optional<rt::KvEntryMaybe<>>>::resolve(std::nullopt);
      rt::Promise<rt::KvEntryMaybe<>> tombstoneRead(kv.get(tombstoneKey));
      auto reads = rt::all(ledgerRead, statusRead, tombstoneRead);
      const std::tuple<LedgerState, std::optional<rt::KvEntryMaybe<>>, rt::KvEntryMaybe<>> results = co_await reads.wait();
      const LedgerState& ledgerState = std::get<0>(results);
      const std::optional<rt::KvEntryMaybe<>>& existingStatus = std::get<1>(results);
      const rt::KvEntryMaybe<>& existingTombstone = std::get<2>(results);
      if (ledgerState.kind == LedgerStateKind::corrupt) co_return FinalizedVictim{.finalized = false, .chunks = cleanup.deleted};
      const SentinelReplayBudgetLedger ledger = withBudget(*ledgerState.ledger, budgetBytes);
      // `storedStatusMetadataBytes(existingStatus?.value)`: no status read contributes nothing.
      const double existingStatusBytes = existingStatus ? storedStatusMetadataBytes(existingStatus->value) : 0;
      const double existingTombstoneBytes = storedStatusMetadataBytes(existingTombstone.value);
      const double statusPresent = !existingStatus || existingStatus->value.isNull() ? 0 : 1;
      const double tombstonePresent = existingTombstone.value.isNull() ? 0 : 1;
      // The metadata bound is hard even during cleanup: when the reserve cannot take
      // the tombstone, the victim's bytes are still released below, but the bounded
      // status/tombstone rows are dropped instead of exceeding the bound.
      const double metadataReserve = sentinelReplayMetadataReserve(budgetBytes);
      // Room for both rows means both are written; otherwise the status row is
      // deleted and any existing tombstone survives exactly as it was.
      const bool statusFits =
        ledger.status_records + statusRows - statusPresent - tombstonePresent <= SENTINEL_REPLAY_MAX_STATUS_RECORDS &&
        ledger.metadata_bytes + statusBytes - existingStatusBytes - existingTombstoneBytes <= metadataReserve;
      const double afterRecords = statusFits ? statusRows : tombstonePresent;
      const double afterBytes = statusFits ? statusBytes : existingTombstoneBytes;
      const double statusRecordsDelta = afterRecords - statusPresent - tombstonePresent;
      const double metadataBytesDelta = afterBytes - existingStatusBytes - existingTombstoneBytes;
      const SentinelReplayBudgetLedger nextLedger = finalizedLedger(ledger, row, kind, nowMs, budgetBytes, statusRecordsDelta, metadataBytesDelta);
      const rt::KvEntryMaybe<> entry = co_await kv.get(key);
      if (!isAccountingRow(entry.value) || entry.value["state"].asString() != "evicting") {
        co_return FinalizedVictim{.finalized = false, .chunks = cleanup.deleted};
      }
      fault("commit");
      const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
      operation->check(rt::AtomicCheck(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, ledgerState.versionstamp))
        .check(rt::AtomicCheck(key, entry.versionstamp))
        .check(rt::AtomicCheck(tombstoneKey, existingTombstone.versionstamp))
        .set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, ledgerValue(nextLedger))
        .delete_(key);
      rt::AtomicOperation& applied = applyClaimedVictimMetadata(*operation, existingStatus, statusRow, tombstoneKey, tombstone, statusFits);
      const rt::KvCommitResult committed = co_await applied.commit();
      // A commit failure leaves the victim in `evicting` with its charge held for a
      // later pass; its bytes were fully removed, so nothing is double-charged.
      co_return FinalizedVictim{.finalized = committed.ok, .chunks = cleanup.deleted};
    }

    /**
     * Claim one stored victim for exactly-once deletion, then finalize it. The claim
     * commit is what makes eviction exactly-once: exactly one caller can move
     * stored -> evicting at this row versionstamp, and a loser must move on without
     * touching the victim.
     */
    rt::Task<std::optional<SentinelReplayAccountingRow>> claimVictim(rt::Kv& kv, rt::KvKey key, SentinelReplayAccountingRow row, SentinelReplayClaimKind kind,
                                                                   double budgetBytes) {
      // Re-read before claiming: a stale victim whose row was already deleted or
      // already claimed must never be resurrected by a `versionstamp: null` check.
      const rt::KvEntryMaybe<> current = co_await kv.get(key);
      if (!isAccountingRow(current.value)) co_return std::nullopt;
      // Bind the narrowed row: TypeScript does not carry a property narrowing into
      // the mutation closure, and no assertion should be needed for that.
      const SentinelReplayAccountingRow claimed = accountingRowOf(current.value);
      if (!accountingKeyMatches(key, claimed) || claimed.state != "stored" || claimed.fence != row.fence || claimed.bytes != row.bytes) co_return std::nullopt;
      SentinelReplayAccountingRow claimedRow = claimed;
      claimedRow.state = "evicting";
      claimedRow.fence = row.fence + 1;
      claimedRow.claim_kind = kind;
      LedgerMutation mutate = [](const SentinelReplayBudgetLedger& ledger) { return ledger; };
      const rt::KvValue claimedValue = accountingValue(claimedRow);
      AtomicRowApplication applyRow = [key, claimedValue](rt::AtomicOperation& operation) -> rt::AtomicOperation& { return operation.set(key, claimedValue); };
      auto committing = commitAccountingMutation(kv, key, current.versionstamp, std::move(mutate), std::move(applyRow), budgetBytes);
      const bool committed = co_await std::move(committing);
      co_return committed ? std::optional<SentinelReplayAccountingRow>(std::move(claimedRow)) : std::nullopt;
    }

    // `(row: SentinelReplayAccountingRow, nowMs: number) => boolean`
    using VictimPredicate = std::function<bool(const SentinelReplayAccountingRow& row, double nowMs)>;

    rt::Task<std::vector<AccountingEntry>> selectVictims(rt::Kv& kv, double limit, VictimPredicate predicate, double nowMs) {
      std::vector<AccountingEntry> victims;
      // Accounting keys are timestamp-first, so listing this prefix IS the
      // oldest-first index; no second index namespace is needed.
      rt::KvListSelector selector;
      selector.prefix = SENTINEL_REPLAY_ACCOUNTING_PREFIX;
      rt::KvListOptions options;
      options.limit = limit * 8;
      const std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector), std::move(options));
      for (;;) {
        const std::optional<rt::KvEntry<>> entry = co_await iterator->next();
        if (!entry) break;
        if (!isAccountingRow(entry->value)) continue;
        const SentinelReplayAccountingRow row = accountingRowOf(entry->value);
        if (!accountingKeyMatches(entry->key, row)) continue;
        if (!predicate(row, nowMs)) continue;
        victims.push_back(AccountingEntry{.key = entry->key, .row = row});
        if (static_cast<double>(victims.size()) >= limit) {
          co_await iterator->return_();
          break;
        }
      }
      co_return victims;
    }

    /**
     * Resume an `evicting` victim whose final commit failed after its bytes were
     * removed. The claim kind is persisted on the row because the current clock
     * may have passed the payload TTL before this continuation runs.
     */
    rt::Task<double> resumeEvictingRows(rt::Kv& kv, double nowMs, double budgetBytes, double limit) {
      VictimPredicate evicting = [](const SentinelReplayAccountingRow& row, double) { return row.state == "evicting"; };
      auto selecting = selectVictims(kv, limit, std::move(evicting), nowMs);
      const std::vector<AccountingEntry> victims = co_await std::move(selecting);
      double resumed = 0;
      for (const AccountingEntry& victim : victims) {
        auto finalizing = finalizeClaimedVictim(kv, victim.key, victim.row, nowMs, budgetBytes, EVICTION_MAX_CHUNK_DELETES);
        const FinalizedVictim result = co_await std::move(finalizing);
        if (result.finalized) resumed += 1;
      }
      co_return resumed;
    }

    rt::Task<SentinelReplayEviction> evictSentinelReplaysTask(rt::Kv& kv, SentinelReplayEvictionOptions options) {
      const double budgetBytes = budgetOf(options.budget_bytes);
      const double maxRecords =
        rt::Math::max(1, rt::Math::min(EVICTION_BATCH_RECORDS, options.max_records ? *options.max_records : EVICTION_BATCH_RECORDS));
      const std::optional<double> targetRecords =
        !options.target_records ? std::nullopt : std::optional<double>(rt::Math::max(0, rt::Math::trunc(*options.target_records)));
      // Expired payloads belong to the TTL reclamation pass, which reports them as
      // `expired`; each claim records its cause so a resumed row keeps that cause
      // even when its payload TTL has since passed.
      VictimPredicate unexpired = [](const SentinelReplayAccountingRow& row, double nowMs) { return row.state == "stored" && row.expires_at_ms > nowMs; };
      auto selecting = selectVictims(kv, maxRecords, std::move(unexpired), options.now_ms);
      const std::vector<AccountingEntry> victims = co_await std::move(selecting);
      double records = 0;
      double bytes = 0;
      double chunks = 0;
      for (const AccountingEntry& victim : victims) {
        const double remainingDeletes =
          rt::Math::max(0, (options.max_chunk_deletes ? *options.max_chunk_deletes : EVICTION_MAX_CHUNK_DELETES) - chunks);
        if (remainingDeletes <= 0) break;
        const LedgerState state = co_await readLedger(kv, budgetBytes);
        if (state.kind == LedgerStateKind::corrupt) break;
        const SentinelReplayBudgetLedger ledger = withBudget(*state.ledger, budgetBytes);
        if (ledger.stored_bytes <= options.target_bytes && (!targetRecords || ledger.records <= *targetRecords)) break;
        auto claiming = claimVictim(kv, victim.key, victim.row, "evicted", budgetBytes);
        const std::optional<SentinelReplayAccountingRow> claimed = co_await std::move(claiming);
        if (!claimed) continue;
        auto finalizing = finalizeClaimedVictim(kv, victim.key, *claimed, options.now_ms, budgetBytes, remainingDeletes);
        const FinalizedVictim result = co_await std::move(finalizing);
        chunks += result.chunks;
        if (!result.finalized) continue;
        records += 1;
        bytes += victim.row.bytes;
      }
      co_return SentinelReplayEviction{.records = records, .bytes = bytes, .chunks = chunks};
    }

    rt::Task<SentinelReplayReclamation> reclaimExpiredSentinelReplaysTask(rt::Kv& kv, SentinelReplayReclaimOptions options) {
      const double budgetBytes = budgetOf(options.budget_bytes);
      const double maxRecords =
        rt::Math::max(1, rt::Math::min(EVICTION_BATCH_RECORDS, options.max_records ? *options.max_records : EVICTION_BATCH_RECORDS));
      VictimPredicate expired = [](const SentinelReplayAccountingRow& row, double nowMs) { return row.state == "stored" && row.expires_at_ms <= nowMs; };
      auto selecting = selectVictims(kv, maxRecords, std::move(expired), options.now_ms);
      const std::vector<AccountingEntry> victims = co_await std::move(selecting);
      double records = 0;
      double bytes = 0;
      for (const AccountingEntry& victim : victims) {
        auto claiming = claimVictim(kv, victim.key, victim.row, "expired", budgetBytes);
        const std::optional<SentinelReplayAccountingRow> claimed = co_await std::move(claiming);
        if (!claimed) continue;
        auto finalizing = finalizeClaimedVictim(kv, victim.key, *claimed, options.now_ms, budgetBytes, EVICTION_MAX_CHUNK_DELETES);
        const FinalizedVictim result = co_await std::move(finalizing);
        if (!result.finalized) continue;
        records += 1;
        bytes += victim.row.bytes;
      }
      co_return SentinelReplayReclamation{.records = records, .bytes = bytes};
    }
  }

  rt::Task<SentinelReplayEviction> evictSentinelReplays(rt::Kv& kv, const SentinelReplayEvictionOptions& options) {
    return evictSentinelReplaysTask(kv, options);
  }

  rt::Task<SentinelReplayReclamation> reclaimExpiredSentinelReplays(rt::Kv& kv, const SentinelReplayReclaimOptions& options) {
    return reclaimExpiredSentinelReplaysTask(kv, options);
  }

  namespace {
    /**
     * Bounded cleanup for an over-budget or over-cap admission: reclaim payload-TTL
     * expiry first, then evict oldest-first toward both clean targets. Returns whether
     * any capacity was actually reclaimed, so a refusal is only reported when nothing moved.
     */
    rt::Task<bool> reclaimAdmissionCapacity(rt::Kv& kv, double budgetBytes, double payloadBudget, double charge, double nowMs) {
      const double target = rt::Math::floor(payloadBudget * SENTINEL_REPLAY_CLEAN_TARGET_RATIO) - charge;
      const SentinelReplayReclaimOptions reclaimOptions{.now_ms = nowMs, .budget_bytes = budgetBytes};
      auto reclaiming = reclaimExpiredSentinelReplays(kv, reclaimOptions);
      const SentinelReplayReclamation expired = co_await std::move(reclaiming);
      const SentinelReplayEvictionOptions evictionOptions{
        .target_bytes = rt::Math::max(0, target),
        .target_records = SENTINEL_REPLAY_MAX_RECORDS - 1,
        .now_ms = nowMs,
        .budget_bytes = budgetBytes,
      };
      auto evicting = evictSentinelReplays(kv, evictionOptions);
      const SentinelReplayEviction evicted = co_await std::move(evicting);
      co_return evicted.records > 0 || expired.records > 0;
    }

    rt::Task<SentinelReplayAdmission> reserveSentinelReplayCapacityTask(rt::Kv& kv, SentinelReplayCapacityInput input) {
      const double budgetBytes = budgetOf(input.budget_bytes);
      const double payloadBudget = sentinelReplayPayloadBudgetBytes(budgetBytes);
      const double charge = replay_limits::sentinelReplayReservationBytes(input.plaintext_bytes, input.metadata_bytes);
      const std::string accountingReason(SENTINEL_REPLAY_ACCOUNTING_REASON);
      const std::string storageFullReason(SENTINEL_REPLAY_STORAGE_FULL_REASON);
      for (double round = 0; round < 4; round += 1) {
        auto reaping = reapAccountingRows(kv, input.now_ms, budgetBytes);
        co_await std::move(reaping);
        const LedgerState state = co_await readLedger(kv, budgetBytes);
        if (state.kind == LedgerStateKind::corrupt) {
          auto recording = recordSkipReason(kv, accountingReason, input.now_ms, budgetBytes);
          co_await std::move(recording);
          co_return SentinelReplayAdmissionNotOk{.reason = accountingReason};
        }
        const SentinelReplayBudgetLedger ledger = withBudget(*state.ledger, budgetBytes);
        if (!ledger.bootstrap_complete) {
          // VPS failure capture may be awaited before responding: start the owner fill without waiting.
          auto running = replay_retention_bootstrap::runBootstrapBatch(kv, ledger, state.versionstamp, budgetBytes, input.now_ms, 0);
          const SentinelReplayBudgetLedger bootstrapped = co_await std::move(running);
          if (!bootstrapped.bootstrap_complete) {
            auto recording = recordSkipReason(kv, accountingReason, input.now_ms, budgetBytes);
            co_await std::move(recording);
            co_return SentinelReplayAdmissionNotOk{.reason = accountingReason};
          }
          continue;
        }
        // Admissions may create owners from here on, so no earlier chunk owner index can be trusted again.
        replay_retention_bootstrap::forgetChunkOwnerIndex(kv);
        const bool overBudget =
          ledger.stored_bytes + ledger.reserved_bytes + charge > payloadBudget || ledger.records + 1 > SENTINEL_REPLAY_MAX_RECORDS;
        if (overBudget) {
          auto reclaiming = reclaimAdmissionCapacity(kv, budgetBytes, payloadBudget, charge, input.now_ms);
          const bool reclaimed = co_await std::move(reclaiming);
          if (!reclaimed) {
            auto recording = recordSkipReason(kv, storageFullReason, input.now_ms, budgetBytes);
            co_await std::move(recording);
            co_return SentinelReplayAdmissionNotOk{.reason = storageFullReason};
          }
          continue;
        }
        const SentinelReplayAccountingRow row{
          .version = 1,
          .capture_id = input.capture_id,
          .request_id = input.request_id,
          .fingerprint = input.fingerprint,
          .bytes = charge,
          .state = "reserved",
          .fence = 1,
          .created_at_ms = input.now_ms,
          // A short staging lease: a crashed writer's charge is reaped promptly. The
          // payload TTL is written only when publish turns this row into `stored`.
          .expires_at_ms = input.now_ms + SENTINEL_REPLAY_RESERVATION_TTL_MS,
          .status_expires_at_ms = input.now_ms + SENTINEL_REPLAY_STATUS_TTL_MS,
          .stage = 0,
        };
        const rt::KvKey key = sentinelReplayAccountingKey(row.created_at_ms, row.capture_id);
        SentinelReplayBudgetLedger nextLedger = ledger;
        nextLedger.reserved_bytes = ledger.reserved_bytes + charge;
        nextLedger.last_skip_reason = std::nullopt;
        fault("commit");
        const std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
        operation->check(rt::AtomicCheck(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, state.versionstamp))
          .check(rt::AtomicCheck(key, std::nullopt))
          .set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, ledgerValue(nextLedger))
          .set(key, accountingValue(row));
        const rt::KvCommitResult committed = co_await operation->commit();
        if (committed.ok) co_return SentinelReplayAdmissionOk{.accounting_key = key, .accounting = row, .charge = charge};
      }
      auto recording = recordSkipReason(kv, storageFullReason, input.now_ms, budgetBytes);
      co_await std::move(recording);
      co_return SentinelReplayAdmissionNotOk{.reason = storageFullReason};
    }

    rt::Task<SentinelReplayRetentionStatus> runSentinelReplayRetentionMaintenanceTask(rt::Kv& kv, SentinelReplayMaintenanceOptions options) {
      const double budgetBytes = budgetOf(options.budget_bytes);
      const double payloadBudget = sentinelReplayPayloadBudgetBytes(budgetBytes);
      // The resumable sweep runs FIRST and gates every counter mutation below. It
      // counts each in-scope row itself, so a release or reclamation that moved a
      // counter while the sweep was still pending would let that row be counted a
      // second time once the sweep reached it.
      auto bootstrapping = replay_retention_bootstrap::bootstrapBeforeMutation(kv, budgetBytes, options.now_ms);
      const std::optional<SentinelReplayBudgetLedger> bootstrapped = co_await std::move(bootstrapping);
      if (!bootstrapped) {
        auto reading = readSentinelReplayRetentionStatus(kv, budgetBytes);
        SentinelReplayRetentionStatus status = co_await std::move(reading);
        co_return status;
      }
      auto reaping = reapAccountingRows(kv, options.now_ms, budgetBytes);
      co_await std::move(reaping);
      auto reapingLegacy = reapLegacyReservations(kv, budgetBytes);
      co_await std::move(reapingLegacy);
      const SentinelReplayReclaimOptions reclaimOptions{.now_ms = options.now_ms, .budget_bytes = budgetBytes};
      auto reclaiming = reclaimExpiredSentinelReplays(kv, reclaimOptions);
      co_await std::move(reclaiming);
      const LedgerState current = co_await readLedger(kv, budgetBytes);
      if (current.kind == LedgerStateKind::ok && current.ledger->stored_bytes + current.ledger->reserved_bytes > payloadBudget) {
        const SentinelReplayEvictionOptions evictionOptions{
          .target_bytes = rt::Math::floor(payloadBudget * SENTINEL_REPLAY_CLEAN_TARGET_RATIO),
          .now_ms = options.now_ms,
          .budget_bytes = budgetBytes,
        };
        auto evicting = evictSentinelReplays(kv, evictionOptions);
        co_await std::move(evicting);
      }
      // Throttled: a healthy admin poll inside the interval neither rescans metadata nor writes the ledger.
      auto reconciling = replay_retention_metadata::reconcileSentinelReplayStatusMetadataIfDue(kv, budgetBytes, options.now_ms);
      co_await std::move(reconciling);
      auto reading = readSentinelReplayRetentionStatus(kv, budgetBytes);
      SentinelReplayRetentionStatus status = co_await std::move(reading);
      co_return status;
    }
  }

  rt::Task<SentinelReplayAdmission> reserveSentinelReplayCapacity(rt::Kv& kv, const SentinelReplayCapacityInput& input) {
    return reserveSentinelReplayCapacityTask(kv, input);
  }

  rt::Task<SentinelReplayRetentionStatus> runSentinelReplayRetentionMaintenance(rt::Kv& kv, const SentinelReplayMaintenanceOptions& options) {
    return runSentinelReplayRetentionMaintenanceTask(kv, options);
  }
}
