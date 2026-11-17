#include "uos/sentinel/replay_retention_schema.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/date.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <cstring>
#include <initializer_list>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

namespace uos::sentinel::replay_retention_schema {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    // `/^[0-9a-f]{64}$/`
    const rt::Regex& HEX_DIGEST() {
      static const rt::Regex pattern("^[0-9a-f]{64}$", "");
      return pattern;
    }

    std::optional<double> budgetOverrideForTest;
    SentinelReplayRetentionFaultInjector faultInjectorForTest;

    // `typeof value === "object" && value !== null`
    bool isNumber(const rt::KvValue& value, double number) { return value.isNumber() && value.asNumber() == number; }
    bool isText(const rt::KvValue& value, std::string_view text) { return value.isString() && value.asString() == text; }
    // `key[index] === text` / `key[index] === number`
    bool partIs(const rt::KvKeyPart& part, std::string_view text) { return part.isString() && part.string() == text; }
    bool partIs(const rt::KvKeyPart& part, double number) { return part.isNumber() && part.number() == number; }

    SentinelReplayBudgetLedger emptyLedger(double budgetBytes) {
      return {
        .version = 1,
        .budget_bytes = budgetBytes,
        .stored_bytes = 0,
        .reserved_bytes = 0,
        .records = 0,
        .status_records = 0,
        .metadata_bytes = 0,
        .evicted_bytes = 0,
        .evicted_records = 0,
        .expired_bytes = 0,
        .expired_records = 0,
        .status_pruned_records = 0,
        .last_eviction_at_ms = std::nullopt,
        .last_warning_at_ms = std::nullopt,
        .last_full_at_ms = std::nullopt,
        .last_skip_reason = std::nullopt,
        .bootstrap_complete = false,
        .bootstrap_cursor = std::nullopt,
        .accounting_error = std::nullopt,
        .over_budget = false,
      };
    }

    bool isAbsentOrString(const rt::KvValue& value) { return value.isUndefined() || value.isNull() || value.isString(); }

    /**
     * A present ledger is only accepted when every counter is a non-negative safe
     * integer and every marker has its declared shape. A malformed ledger is
     * reported as corrupt, never silently replaced with an empty ledger.
     */
    bool isLedger(const rt::KvValue& value) {
      if (!rt::kvIsObject(value)) return false;
      const rt::KvValue& row = value;
      if (!isNumber(row["version"], 1)) return false;
      for (const std::string_view field : {"budget_bytes", "stored_bytes", "reserved_bytes", "records", "status_records", "metadata_bytes", "evicted_bytes",
                                           "evicted_records", "expired_bytes", "expired_records", "status_pruned_records"}) {
        if (!counter(row[field])) return false;
      }
      if (!row["bootstrap_complete"].isBool() || !row["over_budget"].isBool()) return false;
      for (const std::string_view field : {"last_eviction_at_ms", "last_warning_at_ms", "last_full_at_ms"}) {
        if (!row[field].isNull() && !counter(row[field])) return false;
      }
      for (const std::string_view field : {"last_skip_reason", "bootstrap_cursor", "accounting_error"}) {
        if (!row[field].isNull() && !row[field].isString()) return false;
      }
      // Optional: ledgers written before sweeps had ids omit it.
      return isAbsentOrString(row["bootstrap_sweep"]);
    }

    rt::KvValue ledgerValue(const SentinelReplayBudgetLedger& ledger) { return sentinelReplayLedgerValue(ledger); }


  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelReplayAccountingRow& value) {
    out = Writer()
            .required("version", value.version)
            .required("capture_id", value.capture_id)
            .required("request_id", value.request_id)
            .required("fingerprint", value.fingerprint)
            .required("bytes", value.bytes)
            .required("state", value.state)
            .required("fence", value.fence)
            .required("created_at_ms", value.created_at_ms)
            .required("expires_at_ms", value.expires_at_ms)
            .required("status_expires_at_ms", value.status_expires_at_ms)
            .required("stage", value.stage)
            .orUndefined("claim_kind", value.claim_kind)
            .done();
  }

  SentinelReplayAccountingRow tag_invoke(const json::value_to_tag<SentinelReplayAccountingRow>&, const json::value& value) {
    const Reader in(value, "SentinelReplayAccountingRow");
    return {
      .version = in.required<double>("version"),
      .capture_id = in.required<std::string>("capture_id"),
      .request_id = in.required<std::string>("request_id"),
      .fingerprint = in.required<std::string>("fingerprint"),
      .bytes = in.required<double>("bytes"),
      .state = in.required<std::string>("state"),
      .fence = in.required<double>("fence"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
      .status_expires_at_ms = in.required<double>("status_expires_at_ms"),
      .stage = in.required<double>("stage"),
      .claim_kind = in.orUndefined<std::string>("claim_kind"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelReplayLegacyReservation& value) {
    out = Writer()
            .required("version", value.version)
            .required("capture_id", value.capture_id)
            .required("request_id", value.request_id)
            .required("bytes", value.bytes)
            .required("fence", value.fence)
            .required("state", value.state)
            .required("created_at_ms", value.created_at_ms)
            .required("expires_at_ms", value.expires_at_ms)
            .done();
  }

  SentinelReplayLegacyReservation tag_invoke(const json::value_to_tag<SentinelReplayLegacyReservation>&, const json::value& value) {
    const Reader in(value, "SentinelReplayLegacyReservation");
    return {
      .version = in.required<double>("version"),
      .capture_id = in.required<std::string>("capture_id"),
      .request_id = in.required<std::string>("request_id"),
      .bytes = in.required<double>("bytes"),
      .fence = in.required<double>("fence"),
      .state = in.required<std::string>("state"),
      .created_at_ms = in.required<double>("created_at_ms"),
      .expires_at_ms = in.required<double>("expires_at_ms"),
    };
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelReplayBudgetLedger& value) {
    out = Writer()
            .required("version", value.version)
            .required("budget_bytes", value.budget_bytes)
            .required("stored_bytes", value.stored_bytes)
            .required("reserved_bytes", value.reserved_bytes)
            .required("records", value.records)
            .required("status_records", value.status_records)
            .required("metadata_bytes", value.metadata_bytes)
            .required("evicted_bytes", value.evicted_bytes)
            .required("evicted_records", value.evicted_records)
            .required("expired_bytes", value.expired_bytes)
            .required("expired_records", value.expired_records)
            .required("status_pruned_records", value.status_pruned_records)
            .orNull("last_eviction_at_ms", value.last_eviction_at_ms)
            .orNull("last_warning_at_ms", value.last_warning_at_ms)
            .orNull("last_full_at_ms", value.last_full_at_ms)
            .orNull("last_skip_reason", value.last_skip_reason)
            .required("bootstrap_complete", value.bootstrap_complete)
            .orNull("bootstrap_cursor", value.bootstrap_cursor)
            .nullable("bootstrap_sweep", value.bootstrap_sweep)
            .orNull("accounting_error", value.accounting_error)
            .required("over_budget", value.over_budget)
            .done();
  }

  SentinelReplayBudgetLedger tag_invoke(const json::value_to_tag<SentinelReplayBudgetLedger>&, const json::value& value) {
    const Reader in(value, "SentinelReplayBudgetLedger");
    return {
      .version = in.required<double>("version"),
      .budget_bytes = in.required<double>("budget_bytes"),
      .stored_bytes = in.required<double>("stored_bytes"),
      .reserved_bytes = in.required<double>("reserved_bytes"),
      .records = in.required<double>("records"),
      .status_records = in.required<double>("status_records"),
      .metadata_bytes = in.required<double>("metadata_bytes"),
      .evicted_bytes = in.required<double>("evicted_bytes"),
      .evicted_records = in.required<double>("evicted_records"),
      .expired_bytes = in.required<double>("expired_bytes"),
      .expired_records = in.required<double>("expired_records"),
      .status_pruned_records = in.required<double>("status_pruned_records"),
      .last_eviction_at_ms = in.orNull<double>("last_eviction_at_ms"),
      .last_warning_at_ms = in.orNull<double>("last_warning_at_ms"),
      .last_full_at_ms = in.orNull<double>("last_full_at_ms"),
      .last_skip_reason = in.orNull<std::string>("last_skip_reason"),
      .bootstrap_complete = in.required<bool>("bootstrap_complete"),
      .bootstrap_cursor = in.orNull<std::string>("bootstrap_cursor"),
      .bootstrap_sweep = in.nullable<std::string>("bootstrap_sweep"),
      .accounting_error = in.orNull<std::string>("accounting_error"),
      .over_budget = in.required<bool>("over_budget"),
    };
  }

  bool operator==(const SentinelReplayAccountingRow& left, const SentinelReplayAccountingRow& right) {
    return std::tie(left.version, left.capture_id, left.request_id, left.fingerprint, left.bytes, left.state, left.fence,
                    left.created_at_ms, left.expires_at_ms, left.status_expires_at_ms, left.stage, left.claim_kind) ==
           std::tie(right.version, right.capture_id, right.request_id, right.fingerprint, right.bytes, right.state, right.fence,
                    right.created_at_ms, right.expires_at_ms, right.status_expires_at_ms, right.stage, right.claim_kind);
  }

  bool operator==(const SentinelReplayBudgetLedger& left, const SentinelReplayBudgetLedger& right) {
    return std::tie(left.version, left.budget_bytes, left.stored_bytes, left.reserved_bytes, left.records, left.status_records,
                    left.metadata_bytes, left.evicted_bytes, left.evicted_records, left.expired_bytes, left.expired_records,
                    left.status_pruned_records, left.last_eviction_at_ms, left.last_warning_at_ms, left.last_full_at_ms,
                    left.last_skip_reason, left.bootstrap_complete, left.bootstrap_cursor, left.bootstrap_sweep,
                    left.accounting_error, left.over_budget) ==
           std::tie(right.version, right.budget_bytes, right.stored_bytes, right.reserved_bytes, right.records, right.status_records,
                    right.metadata_bytes, right.evicted_bytes, right.evicted_records, right.expired_bytes, right.expired_records,
                    right.status_pruned_records, right.last_eviction_at_ms, right.last_warning_at_ms, right.last_full_at_ms,
                    right.last_skip_reason, right.bootstrap_complete, right.bootstrap_cursor, right.bootstrap_sweep,
                    right.accounting_error, right.over_budget);
  }

  namespace {
    template <class T> T storedRowOf(const rt::KvValue& value, std::initializer_list<std::string_view> fields) {
      json::object declared;
      for (const std::string_view field : fields) {
        const rt::KvValue& member = value[field];
        if (!member.isUndefined()) declared[std::string(field)] = member.toJson();
      }
      T row = json::value_to<T>(json::value(std::move(declared)));
      row.storedObject = value.asObject();
      return row;
    }

    rt::KvValue storedRowValue(const std::optional<rt::KvObject>& original, const json::object& declared, std::string_view optionalField) {
      rt::KvObject stored = original.value_or(rt::KvObject());
      for (const auto& member : declared) stored.set(std::string(member.key()), rt::KvValue(member.value()));
      // A cleared optional property is undefined in a JS spread, rather than an old value or a deleted key.
      if (original && original->contains(optionalField) && !declared.contains(optionalField)) stored.set(std::string(optionalField), rt::KvValue());
      return rt::KvValue(std::move(stored));
    }
  }

  SentinelReplayAccountingRow toSentinelReplayAccountingRow(const rt::KvValue& value) {
    return storedRowOf<SentinelReplayAccountingRow>(value, {"version", "capture_id", "request_id", "fingerprint", "bytes", "state", "fence",
      "created_at_ms", "expires_at_ms", "status_expires_at_ms", "stage", "claim_kind"});
  }

  SentinelReplayBudgetLedger toSentinelReplayBudgetLedger(const rt::KvValue& value) {
    return storedRowOf<SentinelReplayBudgetLedger>(value, {"version", "budget_bytes", "stored_bytes", "reserved_bytes", "records", "status_records",
      "metadata_bytes", "evicted_bytes", "evicted_records", "expired_bytes", "expired_records", "status_pruned_records", "last_eviction_at_ms",
      "last_warning_at_ms", "last_full_at_ms", "last_skip_reason", "bootstrap_complete", "bootstrap_cursor", "bootstrap_sweep", "accounting_error", "over_budget"});
  }

  rt::KvValue sentinelReplayAccountingValue(const SentinelReplayAccountingRow& value) {
    const json::value declared = json::value_from(value);
    return storedRowValue(value.storedObject, declared.as_object(), "claim_kind");
  }

  rt::KvValue sentinelReplayLedgerValue(const SentinelReplayBudgetLedger& value) {
    const json::value declared = json::value_from(value);
    return storedRowValue(value.storedObject, declared.as_object(), "bootstrap_sweep");
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const SentinelReplayRetentionStatus& value) {
    out = Writer()
            .required("state", value.state)
            .required("scope", value.scope)
            .orNull("budget_bytes", value.budget_bytes)
            .orNull("stored_bytes", value.stored_bytes)
            .orNull("reserved_bytes", value.reserved_bytes)
            .orNull("records", value.records)
            .orNull("metadata_bytes", value.metadata_bytes)
            .orNull("status_records", value.status_records)
            .orNull("evicted_records", value.evicted_records)
            .orNull("evicted_bytes", value.evicted_bytes)
            .orNull("expired_records", value.expired_records)
            .orNull("last_eviction_at_ms", value.last_eviction_at_ms)
            .orNull("last_warning_at_ms", value.last_warning_at_ms)
            .required("over_budget", value.over_budget)
            .required("near_capacity", value.near_capacity)
            .required("accounting_complete", value.accounting_complete)
            .orNull("accounting_error", value.accounting_error)
            .orNull("skipped_reason", value.skipped_reason)
            .done();
  }

  SentinelReplayRetentionStatus tag_invoke(const json::value_to_tag<SentinelReplayRetentionStatus>&, const json::value& value) {
    const Reader in(value, "SentinelReplayRetentionStatus");
    return {
      .state = in.required<std::string>("state"),
      .scope = in.required<std::string>("scope"),
      .budget_bytes = in.orNull<double>("budget_bytes"),
      .stored_bytes = in.orNull<double>("stored_bytes"),
      .reserved_bytes = in.orNull<double>("reserved_bytes"),
      .records = in.orNull<double>("records"),
      .metadata_bytes = in.orNull<double>("metadata_bytes"),
      .status_records = in.orNull<double>("status_records"),
      .evicted_records = in.orNull<double>("evicted_records"),
      .evicted_bytes = in.orNull<double>("evicted_bytes"),
      .expired_records = in.orNull<double>("expired_records"),
      .last_eviction_at_ms = in.orNull<double>("last_eviction_at_ms"),
      .last_warning_at_ms = in.orNull<double>("last_warning_at_ms"),
      .over_budget = in.required<bool>("over_budget"),
      .near_capacity = in.required<bool>("near_capacity"),
      .accounting_complete = in.required<bool>("accounting_complete"),
      .accounting_error = in.orNull<std::string>("accounting_error"),
      .skipped_reason = in.orNull<std::string>("skipped_reason"),
    };
  }

  void setSentinelReplayBudgetForTest(std::optional<double> bytes) {
    budgetOverrideForTest = !bytes ? std::nullopt : std::optional<double>(rt::Math::max(64 * 1'024, rt::Math::trunc(*bytes)));
  }

  void setSentinelReplayRetentionFaultsForTest(SentinelReplayRetentionFaultInjector injector) { faultInjectorForTest = std::move(injector); }

  void fault(const SentinelReplayRetentionFaultKind& kind) {
    if (faultInjectorForTest && faultInjectorForTest(kind)) throw rt::JsError("injected_" + kind + "_failure");
  }

  double sentinelReplayBudgetBytes() { return budgetOverrideForTest.value_or(replay_limits::SENTINEL_REPLAY_BUDGET_BYTES); }

  double sentinelReplayMetadataReserve(double budgetBytes) {
    return rt::Math::max(0, rt::Math::min(replay_limits::SENTINEL_REPLAY_METADATA_RESERVE_BYTES, rt::Math::floor(rt::Math::max(0, budgetBytes) / 16)));
  }

  double sentinelReplayPayloadBudgetBytes(double budgetBytes) { return rt::Math::max(0, budgetBytes - sentinelReplayMetadataReserve(budgetBytes)); }

  rt::KvKey sentinelReplayAccountingKey(double capturedAtMs, const std::string& captureId) {
    return rt::KvKey{SENTINEL_REPLAY_ACCOUNTING_PREFIX, capturedAtMs, captureId};
  }

  rt::KvKey sentinelReplayManifestKey(const replay_model::SentinelReplayManifest& manifest) {
    return rt::KvKey{replay_model::SENTINEL_REPLAY_MANIFEST_PREFIX, manifest.captured_at_ms, manifest.fingerprint, manifest.capture_id};
  }

  rt::KvKey sentinelReplayEvictionKey(const std::string& fingerprint) { return rt::KvKey{SENTINEL_REPLAY_EVICTION_PREFIX, fingerprint}; }

  rt::KvKey sentinelReplayRequestStatusKey(const std::string& requestId) { return rt::KvKey{replay_model::SENTINEL_REPLAY_REQUEST_PREFIX, requestId}; }

  double sentinelReplayManifestCharge(const replay_model::SentinelReplayManifest& manifest) {
    if (manifest.stored_bytes && rt::Number::isSafeInteger(*manifest.stored_bytes) && *manifest.stored_bytes > 0) return *manifest.stored_bytes;
    return replay_limits::sentinelReplayStoredCharge(manifest.ciphertext_bytes, replay_limits::SENTINEL_REPLAY_LEGACY_METADATA_BYTES) +
           replay_limits::SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES;
  }

  bool counter(const rt::KvValue& value) { return value.isNumber() && counter(value.asNumber()); }
  bool counter(const rt::KvKeyPart& value) { return value.isNumber() && counter(value.number()); }
  bool counter(const json::value& value) {
    const std::optional<double> number = rt::getNumber(value);
    return number && counter(*number);
  }
  bool counter(double value) { return rt::Number::isSafeInteger(value) && value >= 0; }

  bool isAccountingRow(const rt::KvValue& value) {
    if (!rt::kvIsObject(value)) return false;
    const rt::KvValue& row = value;
    const rt::KvValue& state = row["state"];
    const rt::KvValue& claimKind = row["claim_kind"];
    return isNumber(row["version"], 1) && row["capture_id"].isString() && rt::length(row["capture_id"].asString()) > 0 &&
           rt::length(row["capture_id"].asString()) <= 128 && row["request_id"].isString() && row["fingerprint"].isString() &&
           HEX_DIGEST().test(row["fingerprint"].asString()) && counter(row["bytes"]) && row["bytes"].asNumber() > 0 &&
           (isText(state, "reserved") || isText(state, "stored") || isText(state, "evicting") || isText(state, "revoked")) && counter(row["fence"]) &&
           row["fence"].asNumber() >= 1 && counter(row["created_at_ms"]) && counter(row["expires_at_ms"]) && counter(row["status_expires_at_ms"]) &&
           counter(row["stage"]) && (claimKind.isUndefined() || isText(claimKind, "evicted") || isText(claimKind, "expired"));
  }

  bool accountingKeyMatches(const rt::KvKey& key, const SentinelReplayAccountingRow& row) {
    return key.size() == SENTINEL_REPLAY_ACCOUNTING_PREFIX.size() + 2 && partIs(key[3], "accounting") && partIs(key[4], row.created_at_ms) &&
           partIs(key[5], row.capture_id);
  }

  bool manifestKeyMatches(const rt::KvKey& key, const replay_model::SentinelReplayManifest& manifest) {
    return key.size() == replay_model::SENTINEL_REPLAY_MANIFEST_PREFIX.size() + 3 && partIs(key[3], "manifest") && partIs(key[4], manifest.captured_at_ms) &&
           partIs(key[5], manifest.fingerprint) && partIs(key[6], manifest.capture_id);
  }

  bool isLegacyReservation(const rt::KvValue& value) {
    if (!rt::kvIsObject(value)) return false;
    const rt::KvValue& row = value;
    const rt::KvValue& state = row["state"];
    return isNumber(row["version"], 1) && row["capture_id"].isString() && counter(row["bytes"]) && counter(row["fence"]) &&
           (isText(state, "reserved") || isText(state, "revoked") || isText(state, "published"));
  }

  double sentinelReplayStatusMetadataBytes(const rt::KvValue& value) {
    try {
      const std::optional<std::string> text = rt::kvJsonStringify(value);
      return static_cast<double>(text ? rt::utf8ByteLength(*text) : 0) + replay_limits::SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES;
    } catch (...) {
      return replay_limits::SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES;
    }
  }

  double sentinelReplayStatusMetadataBytes(const json::value& value) {
    try {
      return static_cast<double>(rt::utf8ByteLength(rt::jsonStringify(value))) + replay_limits::SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES;
    } catch (...) {
      return replay_limits::SENTINEL_REPLAY_RECORD_OVERHEAD_BYTES;
    }
  }

  double storedStatusMetadataBytes(const rt::KvValue& value) { return value.isNullish() ? 0 : sentinelReplayStatusMetadataBytes(value); }
  double storedStatusMetadataBytes(const json::value& value) { return value.is_null() ? 0 : sentinelReplayStatusMetadataBytes(value); }

  rt::Task<LedgerState> readLedger(rt::Kv& kv, double budgetBytes) {
    const rt::KvEntryMaybe<> entry = co_await kv.get(SENTINEL_REPLAY_BUDGET_LEDGER_KEY);
    if (entry.value.isNull()) co_return LedgerState{.kind = LedgerStateKind::absent, .ledger = emptyLedger(budgetBytes), .versionstamp = std::nullopt};
    if (!isLedger(entry.value)) co_return LedgerState{.kind = LedgerStateKind::corrupt, .ledger = std::nullopt, .versionstamp = entry.versionstamp};
    co_return LedgerState{
      .kind = LedgerStateKind::ok, .ledger = toSentinelReplayBudgetLedger(entry.value), .versionstamp = entry.versionstamp};
  }

  SentinelReplayBudgetLedger withBudget(const SentinelReplayBudgetLedger& ledger, double budgetBytes) {
    if (ledger.budget_bytes == budgetBytes) return ledger;
    SentinelReplayBudgetLedger next = ledger;
    next.budget_bytes = budgetBytes;
    return next;
  }

  rt::Task<std::optional<SentinelReplayLedgerSnapshot>> readSentinelReplayLedgerSnapshot(rt::Kv& kv) {
    const double budget = rt::Math::max(64 * 1'024, rt::Math::trunc(sentinelReplayBudgetBytes()));
    auto read = readSentinelReplayLedgerSnapshot(kv, budget);
    std::optional<SentinelReplayLedgerSnapshot> snapshot = co_await std::move(read);
    co_return snapshot;
  }

  rt::Task<std::optional<SentinelReplayLedgerSnapshot>> readSentinelReplayLedgerSnapshot(rt::Kv& kv, double budgetBytes) {
    const double budget = rt::Math::max(64 * 1'024, rt::Math::trunc(budgetBytes));
    const LedgerState state = co_await readLedger(kv, budget);
    if (state.kind == LedgerStateKind::corrupt) co_return std::nullopt;
    co_return SentinelReplayLedgerSnapshot{.ledger = withBudget(*state.ledger, budget), .versionstamp = state.versionstamp};
  }

  rt::Task<DeleteChunkBatchResult> deleteChunkBatch(rt::Kv& kv, std::string captureId, double limit) {
    if (limit <= 0) co_return DeleteChunkBatchResult{.deleted = 0, .remaining = 1};
    std::vector<rt::KvKey> keys;
    rt::KvListSelector selector;
    selector.prefix = rt::KvKey{replay_model::SENTINEL_REPLAY_CHUNK_PREFIX, captureId};
    rt::KvListOptions options;
    options.limit = limit + 1;
    std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector), std::move(options));
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) keys.push_back(entry->key);
    // keys.slice(0, limit)
    const double end = rt::Math::trunc(limit);
    const std::size_t count = end >= static_cast<double>(keys.size()) ? keys.size() : static_cast<std::size_t>(end);
    for (std::size_t index = 0; index < count; ++index) {
      fault("delete");
      co_await kv.delete_(keys[index]);
    }
    co_return DeleteChunkBatchResult{.deleted = static_cast<double>(count), .remaining = static_cast<double>(keys.size() - count)};
  }

  rt::Task<double> countChunks(rt::Kv& kv, std::string captureId) {
    const rt::KvKey prefix{replay_model::SENTINEL_REPLAY_CHUNK_PREFIX, captureId};
    rt::KvListSelector selector;
    selector.prefix = prefix;
    rt::KvListOptions options;
    options.limit = 1;
    std::unique_ptr<rt::KvListIterator> iterator = kv.list(std::move(selector), std::move(options));
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      // Only a row that really sits under this capture's chunk prefix proves the
      // payload survives; an unrelated stray key must not hold the charge.
      if (entry->key.size() == prefix.size() + 1) {
        co_await iterator->return_();
        co_return 1;
      }
    }
    co_return 0;
  }

  rt::Task<bool> commitAccountingMutation(rt::Kv& kv, rt::KvKey key, std::optional<std::string> rowVersionstamp, LedgerMutation mutate,
                                          AtomicRowApplication applyRow, double budgetBytes) {
    const LedgerState state = co_await readLedger(kv, budgetBytes);
    if (state.kind == LedgerStateKind::corrupt) co_return false;
    const SentinelReplayBudgetLedger ledger = withBudget(*state.ledger, budgetBytes);
    const SentinelReplayBudgetLedger next = mutate(ledger);
    fault("commit");
    std::unique_ptr<rt::AtomicOperation> operation = kv.atomic();
    rt::AtomicOperation& checked = operation->check(rt::AtomicCheck(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, state.versionstamp))
                                     .check(rt::AtomicCheck(key, rowVersionstamp))
                                     .set(SENTINEL_REPLAY_BUDGET_LEDGER_KEY, ledgerValue(next));
    rt::AtomicOperation& applied = applyRow(checked);
    const rt::KvCommitResult committed = co_await applied.commit();
    co_return committed.ok;
  }

  rt::Task<SentinelReplayRetentionStatus> readSentinelReplayRetentionStatus(rt::Kv& kv) {
    auto read = readSentinelReplayRetentionStatus(kv, sentinelReplayBudgetBytes());
    SentinelReplayRetentionStatus status = co_await std::move(read);
    co_return status;
  }

  rt::Task<SentinelReplayRetentionStatus> readSentinelReplayRetentionStatus(rt::Kv& kv, double budgetBytes) {
    const double budget = rt::Math::max(64 * 1'024, rt::Math::trunc(budgetBytes));
    std::optional<LedgerState> state;
    try {
      state = co_await readLedger(kv, budget);
    } catch (...) {
      state = std::nullopt;
    }
    if (!state) {
      co_return SentinelReplayRetentionStatus{
        .state = "unavailable",
        .scope = "capture_owned_kv_payload",
        .budget_bytes = std::nullopt,
        .stored_bytes = std::nullopt,
        .reserved_bytes = std::nullopt,
        .records = std::nullopt,
        .metadata_bytes = std::nullopt,
        .status_records = std::nullopt,
        .evicted_records = std::nullopt,
        .evicted_bytes = std::nullopt,
        .expired_records = std::nullopt,
        .last_eviction_at_ms = std::nullopt,
        .last_warning_at_ms = std::nullopt,
        .over_budget = false,
        .near_capacity = false,
        .accounting_complete = false,
        .accounting_error = "unavailable",
        .skipped_reason = std::nullopt,
      };
    }
    if (state->kind == LedgerStateKind::corrupt) {
      co_return SentinelReplayRetentionStatus{
        .state = "ok",
        .scope = "capture_owned_kv_payload",
        .budget_bytes = budget,
        .stored_bytes = std::nullopt,
        .reserved_bytes = std::nullopt,
        .records = std::nullopt,
        .metadata_bytes = std::nullopt,
        .status_records = std::nullopt,
        .evicted_records = std::nullopt,
        .evicted_bytes = std::nullopt,
        .expired_records = std::nullopt,
        .last_eviction_at_ms = std::nullopt,
        .last_warning_at_ms = std::nullopt,
        .over_budget = false,
        .near_capacity = false,
        .accounting_complete = false,
        .accounting_error = "ledger_corrupt",
        .skipped_reason = std::string(SENTINEL_REPLAY_ACCOUNTING_REASON),
      };
    }
    const SentinelReplayBudgetLedger ledger = withBudget(*state->ledger, budget);
    const double payloadBudget = sentinelReplayPayloadBudgetBytes(budget);
    const double used = ledger.stored_bytes + ledger.reserved_bytes;
    co_return SentinelReplayRetentionStatus{
      .state = "ok",
      .scope = "capture_owned_kv_payload",
      .budget_bytes = budget,
      .stored_bytes = ledger.stored_bytes,
      .reserved_bytes = ledger.reserved_bytes,
      .records = ledger.records,
      .metadata_bytes = ledger.metadata_bytes,
      .status_records = ledger.status_records,
      .evicted_records = ledger.evicted_records,
      .evicted_bytes = ledger.evicted_bytes,
      .expired_records = ledger.expired_records,
      .last_eviction_at_ms = ledger.last_eviction_at_ms,
      .last_warning_at_ms = ledger.last_warning_at_ms,
      .over_budget = used > payloadBudget,
      .near_capacity = used >= rt::Math::floor(payloadBudget * SENTINEL_REPLAY_CLEAN_TARGET_RATIO),
      .accounting_complete = ledger.bootstrap_complete && !ledger.accounting_error,
      .accounting_error = ledger.accounting_error,
      .skipped_reason = ledger.last_skip_reason,
    };
  }
}
