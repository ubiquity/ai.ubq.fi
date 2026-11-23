#include "uos/cache/kv_migration_projection.hpp"

#include "uos/cache/kv_migration_paid_fallback.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"
#include "uos/types.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uos::cache::kv_migration_projection {
  namespace {
    using ::uos::types::ApiKeyRequestLogRecord;
    using ::uos::types::PaidFallbackRequestV3;
    using kv_migration_paid_fallback::isApiKeyId;
    using kv_migration_paid_fallback::isFiniteNonNegativeNumber;
    using kv_migration_paid_fallback::isPaidFallbackRequestV3;
    using kv_migration_paid_fallback::isPendingPaidFallbackLedgerRecord;
    using kv_migration_paid_fallback::isPositiveSafeInteger;
    using kv_migration_paid_fallback::isSafeUsageCount;
    using kv_migration_paid_fallback::LEGACY_REQUEST_LOG_PREFIX;
    using kv_migration_paid_fallback::PAID_FALLBACK_LEDGER_PREFIX;
    using kv_migration_paid_fallback::PAID_FALLBACK_PENDING_V3_PREFIX;
    using kv_migration_paid_fallback::PAID_FALLBACK_REQUEST_V3_PREFIX;
    using kv_migration_paid_fallback::PAID_FALLBACK_V3_PREFIX;
    using kv_migration_paid_fallback::PAID_FALLBACK_WINDOW_V3_PREFIX;
    using kv_migration_paid_fallback::paidFallbackLedgerEntryMatchesIdentity;
    using kv_migration_paid_fallback::paidFallbackV3WindowReference;
    using kv_migration_paid_fallback::pendingPaidFallbackLedgerReferenceFromEntry;
    using kv_migration_usage::StrictApiKeyPair;

    // The helpers below that take references are awaited at once by their callers, whose locals
    // outlive them (the TS passes the same objects).
    //
    // The pairs' records are the stored ApiKeyRecord rows that hasStrictApiKeyCorePolicy accepted,
    // so their numeric members are numbers; where the TS does arithmetic on them a non-number is
    // coerced as JavaScript coerces it (the TS would also hand an uncoerced member on, which the
    // number-typed C++ rows cannot hold).

    const rt::KvGetOptions STRONG{.consistency = rt::KvConsistencyLevel::strong};

    // `key[index]` as the stored value JS holds for it; negative indexes count from the end
    // (`key.at(-1)`), and an index outside the key is undefined.
    rt::KvValue partAt(const rt::KvKey& key, std::ptrdiff_t index) {
      const std::ptrdiff_t size = static_cast<std::ptrdiff_t>(key.size());
      const std::ptrdiff_t resolved = index < 0 ? size + index : index;
      return resolved >= 0 && resolved < size ? rt::kvValueFromKeyPart(key[static_cast<std::size_t>(resolved)]) : rt::KvValue();
    }

    // `JSON.stringify(key)` (a bigint part throws TypeError, as in JS).
    std::string keyJson(const rt::KvKey& key) { return *rt::kvJsonStringify(rt::kvValueFromKey(key)); }

    bool isUndefinedValue(const rt::KvValue& value) noexcept { return value.isUndefined() || value.is<rt::KvHole>(); }
    bool isNumberValue(const rt::KvValue& value, double number) { return value.isNumber() && value.asNumber() == number; }
    bool isStringValue(const rt::KvValue& value, std::string_view text) { return value.isString() && value.asString() == text; }

    // `"key" in value` for a value isRecord accepted.
    bool hasOwnMember(const rt::KvValue& value, std::string_view key) { return value.isObject() && value.asObject().contains(key); }

    // ToNumber(ToPrimitive(value, number)); a bigint reads as its nearest number.
    double toNumber(const rt::KvValue& value) {
      if (isUndefinedValue(value)) return rt::Number::NaN;
      if (value.isNull()) return 0;
      if (value.isBool()) return value.asBool() ? 1 : 0;
      if (value.isNumber()) return value.asNumber();
      if (value.isString()) return rt::stringToNumber(value.asString());
      if (value.isBigInt()) return value.asBigInt().toNumber();
      if (const auto* date = std::get_if<rt::KvDate>(&value.variant())) return date->time;
      return rt::stringToNumber(rt::kvToString(value));
    }

    // A canonical array index ("0".."4294967294").
    std::optional<std::size_t> canonicalIndex(const std::string& key) {
      if (key.empty() || key.size() > 10 || (key.size() > 1 && key[0] == '0')) return std::nullopt;
      std::uint64_t index = 0;
      for (const char digit : key) {
        if (digit < '0' || digit > '9') return std::nullopt;
        index = index * 10 + static_cast<std::uint64_t>(digit - '0');
      }
      if (index >= 4'294'967'295ULL) return std::nullopt;
      return static_cast<std::size_t>(index);
    }

    // `value[key]` on a truthy stored value: an own member of an object, an array element or its
    // length, a string's character or length, a Uint8Array's element or length. Inherited members
    // (Object.prototype, Map.prototype.size, ...) read as absent, as everywhere in rt.
    rt::KvValue propertyRead(const rt::KvValue& value, const std::string& key) {
      if (value.isObject()) return value.asObject().at(key);
      if (value.isArray()) {
        const rt::KvArray& items = value.asArray();
        if (key == "length") return rt::KvValue(static_cast<double>(items.size()));
        const std::optional<std::size_t> index = canonicalIndex(key);
        if (index && *index < items.size() && !items[*index].is<rt::KvHole>()) return items[*index];
        return rt::KvValue();
      }
      if (value.isString()) {
        const std::string& text = value.asString();
        const auto length = static_cast<std::size_t>(rt::length(text));
        if (key == "length") return rt::KvValue(static_cast<double>(length));
        const std::optional<std::size_t> index = canonicalIndex(key);
        if (index && *index < length) return rt::KvValue(rt::charAt(text, static_cast<double>(*index)));
        return rt::KvValue();
      }
      if (value.isBytes()) {
        const rt::KvBytes& bytes = value.asBytes();
        if (key == "length") return rt::KvValue(static_cast<double>(bytes.size()));
        const std::optional<std::size_t> index = canonicalIndex(key);
        if (index && *index < bytes.size()) return rt::KvValue(static_cast<double>(bytes[*index]));
        return rt::KvValue();
      }
      return rt::KvValue();
    }

    // `LegacyPaidFallbackProjectionCandidate`
    struct LegacyPaidFallbackProjectionCandidate {
      ApiKeyRequestLogRecord value;
      rt::KvKey source_key;
    };

    const rt::OrderedSet<std::string>& LEGACY_PAID_FALLBACK_BILLING_STATES() {
      static const rt::OrderedSet<std::string> states{"pending", "reconciled", "not_billed", "unresolved"};
      return states;
    }

    /** True when an untrusted billing status is one of the legacy states. */
    bool isLegacyPaidFallbackBillingState(const rt::KvValue& value) { return value.isString() && LEGACY_PAID_FALLBACK_BILLING_STATES().has(value.asString()); }

    /** A non-empty trimmed string field, or the fallback. */
    std::string legacyText(const rt::KvValue& value, const std::string& fallback) {
      return value.isString() && !rt::trim(value.asString()).empty() ? value.asString() : fallback;
    }

    /** A non-empty trimmed string field, or null. */
    std::optional<std::string> legacyNullableText(const rt::KvValue& value) {
      return value.isString() && !rt::trim(value.asString()).empty() ? std::optional<std::string>(value.asString()) : std::nullopt;
    }

    /** A clamped HTTP status code, or 0 when the field is absent or not finite. */
    double legacyStatusCode(const rt::KvValue& value) {
      return value.isNumber() && rt::Number::isFinite(value.asNumber()) ? rt::Math::max(0, rt::Math::min(599, rt::Math::trunc(value.asNumber()))) : 0;
    }

    /** The legacy billing status a projected row carries. */
    std::string legacyBillingStatus(const rt::KvValue& billing) {
      if (isStringValue(billing, "reconciled") || isStringValue(billing, "not_billed") || isStringValue(billing, "unresolved")) return billing.asString();
      return "pending";
    }

    // `Readonly<{ keyId; requestId; createdAtMs; billing: unknown }>`
    struct LegacyIdentity {
      std::string keyId;
      std::string requestId;
      double createdAtMs;
      rt::KvValue billing;
    };

    // `(value: unknown): number | null` guards as an optional number.
    template <class Guard> std::optional<double> numberIf(const rt::KvValue& value, Guard guard) {
      return guard(value) ? std::optional<double>(value.asNumber()) : std::nullopt;
    }

    /** Projects an untrusted legacy request-log value onto the request shape. */
    ApiKeyRequestLogRecord legacyPaidFallbackValue(const rt::KvValue& raw, const LegacyIdentity& identity) {
      const rt::KvValue& fallbackReason = raw["fallback_reason"];
      return ApiKeyRequestLogRecord{
        .id = identity.requestId,
        .key_id = identity.keyId,
        .route = legacyText(raw["route"], "responses"),
        .path = legacyText(raw["path"], "/v1/responses"),
        .method = legacyText(raw["method"], "POST"),
        .status_code = legacyStatusCode(raw["status_code"]),
        .stream = raw["stream"].isBool() && raw["stream"].asBool(),
        .model = legacyText(raw["model"], "legacy-unknown"),
        .reasoning = legacyNullableText(raw["reasoning"]),
        .created_at_ms = identity.createdAtMs,
        .provider = "metered",
        .fallback_reason = fallbackReason.isString() ? fallbackReason.asString() : std::string("primary_429"),
        .provider_request_id = legacyNullableText(raw["provider_request_id"]),
        .completed_at_ms = numberIf(raw["completed_at_ms"], isPositiveSafeInteger),
        .latency_ms = numberIf(raw["latency_ms"], isSafeUsageCount),
        .input_tokens = numberIf(raw["input_tokens"], isSafeUsageCount),
        .output_tokens = numberIf(raw["output_tokens"], isSafeUsageCount),
        .provider_quota = numberIf(raw["provider_quota"], isFiniteNonNegativeNumber),
        .quota_per_credit = numberIf(raw["quota_per_credit"], isPositiveSafeInteger),
        .spend_microcredits = numberIf(raw["spend_microcredits"], isSafeUsageCount),
        .paid_fallback_window_reset_at_ms = numberIf(raw["paid_fallback_window_reset_at_ms"], isPositiveSafeInteger),
        .billing_status = legacyBillingStatus(identity.billing),
      };
    }

    std::string legacyPaidFallbackReference(const std::string& keyId, const std::string& requestId) {
      json::array reference;
      reference.emplace_back(json::string(keyId));
      reference.emplace_back(json::string(requestId));
      return rt::jsonStringify(reference);
    }

    // std::nullopt is null.
    std::optional<LegacyPaidFallbackProjectionCandidate> legacyPaidFallbackCandidate(const rt::KvEntry<>& entry) {
      if (!rt::kvIsObject(entry.value) || !isStringValue(entry.value["provider"], "metered")) return std::nullopt;
      const rt::KvValue& keyIdValue = entry.value["key_id"];
      const std::optional<std::string> keyId = isApiKeyId(keyIdValue) ? std::optional<std::string>(keyIdValue.asString()) : std::nullopt;
      const rt::KvValue keySuffix = partAt(entry.key, -2);
      const rt::KvValue requestSuffix = partAt(entry.key, -1);
      // `firstValid([entry.value.id, requestSuffix], isApiKeyId)` and
      // `firstValid([entry.value.created_at_ms, keySuffix], isPositiveSafeInteger)`
      const rt::KvValue& idValue = entry.value["id"];
      const rt::KvValue* requestId = isApiKeyId(idValue) ? &idValue : isApiKeyId(requestSuffix) ? &requestSuffix : nullptr;
      const rt::KvValue& createdValue = entry.value["created_at_ms"];
      const rt::KvValue* createdAtMs = isPositiveSafeInteger(createdValue) ? &createdValue : isPositiveSafeInteger(keySuffix) ? &keySuffix : nullptr;
      if (!keyId || requestId == nullptr || createdAtMs == nullptr) return std::nullopt;
      const rt::KvValue& billing = entry.value["billing_status"];
      if (!isUndefinedValue(billing) && !isLegacyPaidFallbackBillingState(billing)) return std::nullopt;
      const LegacyIdentity identity{.keyId = *keyId, .requestId = requestId->asString(), .createdAtMs = createdAtMs->asNumber(), .billing = billing};
      return LegacyPaidFallbackProjectionCandidate{.value = legacyPaidFallbackValue(entry.value, identity), .source_key = entry.key};
    }

    rt::Task<std::vector<LegacyPaidFallbackProjectionCandidate>> listLegacyPaidFallbackProjectionCandidates(std::shared_ptr<rt::Kv> kv) {
      rt::OrderedMap<std::string, LegacyPaidFallbackProjectionCandidate> byReference;
      const std::array<const rt::KvKey*, 2> prefixes{&LEGACY_REQUEST_LOG_PREFIX, &PAID_FALLBACK_LEDGER_PREFIX};
      for (std::size_t index = 0; index < prefixes.size(); ++index) {
        const rt::KvKey& prefix = *prefixes[index];
        const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = prefix});
        while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
          std::optional<LegacyPaidFallbackProjectionCandidate> candidate = legacyPaidFallbackCandidate(*entry);
          if (!candidate) continue;
          // The dedicated paid-fallback ledger is the newer copy of a request log.
          // Prefer it when both prefixes contain the same immutable request.
          const std::string reference = legacyPaidFallbackReference(candidate->value.key_id, candidate->value.id);
          if (prefixes[index] == &PAID_FALLBACK_LEDGER_PREFIX || !byReference.has(reference)) {
            byReference.set(reference, std::move(*candidate));
          }
        }
      }
      std::vector<LegacyPaidFallbackProjectionCandidate> candidates;
      for (const auto& [reference, candidate] : byReference) candidates.push_back(candidate);
      co_return candidates;
    }

    double legacyWindowResetAtMs(const rt::KvValue& record, const ApiKeyRequestLogRecord& request) {
      if (request.paid_fallback_window_reset_at_ms) return *request.paid_fallback_window_reset_at_ms;
      const double usageResetAtMs = toNumber(record["usage_reset_at_ms"]);
      if (request.created_at_ms < usageResetAtMs) return usageResetAtMs;
      const double windowMs = toNumber(record["window_ms"]);
      const double initialStart = usageResetAtMs - windowMs;
      const double elapsed = rt::Math::floor((request.created_at_ms - initialStart) / windowMs);
      return initialStart + (elapsed + 1) * windowMs;
    }

    std::string legacyPolicyVersion(const rt::KvValue& record, const ApiKeyRequestLogRecord& /*request*/, double windowResetAtMs) {
      const rt::KvValue& pricingCheckedAtMs = record["paid_fallback_pricing_checked_at_ms"];
      return "legacy:" + rt::numberToString(windowResetAtMs) + ":" + rt::kvToString(record["window_ms"]) + ":" +
             (pricingCheckedAtMs.isNullish() ? std::string("0") : rt::kvToString(pricingCheckedAtMs));
    }

    double legacyMaximumExposure(const rt::KvValue& record, const ApiKeyRequestLogRecord& request) {
      if (isNumberValue(record["paid_fallback_limit_microcredits"], -1)) return 0;
      if (isStringValue(record["paid_fallback_reservation_request_id"], request.id) && isSafeUsageCount(record["paid_fallback_reserved_microcredits"])) {
        // The legacy policy stores the one live reservation's exact exposure.
        // Prefer it over today's model policy, which may have changed since the
        // request was admitted.
        return record["paid_fallback_reserved_microcredits"].asNumber();
      }
      const rt::KvValue& exposures = record["paid_fallback_max_exposure_microcredits"];
      const rt::KvValue configured = exposures.truthy() ? propertyRead(exposures, request.model.value_or("")) : rt::KvValue(0.0);
      if (isPositiveSafeInteger(configured)) return configured.asNumber();
      return 0;
    }

    rt::KvKey paidFallbackV3WindowKey(const std::string& keyId, double windowResetAtMs) { return rt::KvKey{PAID_FALLBACK_WINDOW_V3_PREFIX, keyId, windowResetAtMs}; }

    bool isPaidFallbackPendingV3(const rt::KvValue& value) {
      return rt::kvIsObject(value) && isPositiveSafeInteger(value["created_at_ms"]) && isPositiveSafeInteger(value["next_reconciliation_at_ms"]) &&
             value["next_reconciliation_at_ms"].asNumber() >= value["created_at_ms"].asNumber();
    }

    bool paidFallbackPendingIdentityMatches(const rt::KvValue& value, const std::string& keyId, const std::string& requestId) {
      if (!isPaidFallbackPendingV3(value)) return false;
      const rt::KvValue& record = value;
      return (!hasOwnMember(record, "key_id") || isStringValue(record["key_id"], keyId)) && (!hasOwnMember(record, "request_id") || isStringValue(record["request_id"], requestId));
    }

    // `{ created_at_ms, next_reconciliation_at_ms }`
    rt::KvValue pendingMarker(double createdAtMs, double nextReconciliationAtMs) {
      rt::KvObject marker;
      marker.set("created_at_ms", createdAtMs);
      marker.set("next_reconciliation_at_ms", nextReconciliationAtMs);
      return rt::KvValue(std::move(marker));
    }

    bool isOutstanding(std::string_view billingState) { return billingState == "pending" || billingState == "unresolved"; }

    rt::Task<void> repairProjectedPaidFallbackPending(std::shared_ptr<rt::Kv> kv, double nowMs) {
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = PAID_FALLBACK_REQUEST_V3_PREFIX});
      while (std::optional<rt::KvEntry<>> requestEntry = co_await iterator->next()) {
        if (!isPaidFallbackRequestV3(requestEntry->value)) continue;
        const rt::KvValue& request = requestEntry->value;
        const std::string keyId = request["key_id"].asString();
        const std::string requestId = request["request_id"].asString();
        const double createdAtMs = request["created_at_ms"].asNumber();
        const rt::KvKey pendingKey{PAID_FALLBACK_PENDING_V3_PREFIX, keyId, requestId};
        const rt::KvEntryMaybe<> pendingEntry = co_await kv->get(pendingKey, STRONG);
        const bool outstanding = isOutstanding(request["billing_state"].asString());
        if (outstanding && !pendingEntry.value.isNull() && paidFallbackPendingIdentityMatches(pendingEntry.value, keyId, requestId) &&
            pendingEntry.value["created_at_ms"].asNumber() >= createdAtMs) {
          continue;
        }
        if (!outstanding && pendingEntry.value.isNull()) continue;
        const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
        atomic->check(*requestEntry).check(pendingEntry);
        if (outstanding) {
          atomic->set(pendingKey, pendingMarker(createdAtMs, rt::Math::max(nowMs, createdAtMs)));
        } else {
          atomic->delete_(pendingKey);
        }
        const rt::KvCommitResult committed = co_await atomic->commit();
        if (!committed.ok) {
          throw rt::JsError("Paid fallback V3 pending state changed concurrently: " + keyId + "/" + requestId);
        }
      }
    }

    // `PaidFallbackWindowReference = Readonly<{ key_id: string; window_reset_at_ms: number }>`
    struct PaidFallbackWindowReference {
      std::string key_id;
      double window_reset_at_ms;
    };

    using RecordsByKey = rt::OrderedMap<std::string, rt::KvValue>;

    // `new Map(pairs.map(({ record }) => [record.id, record]))`: every lookup is by a string id, so
    // a record whose id is not a string (never found) is left out.
    RecordsByKey recordsByKeyOf(const std::vector<StrictApiKeyPair>& pairs) {
      RecordsByKey records;
      for (const StrictApiKeyPair& pair : pairs) {
        const rt::KvValue& id = pair.record["id"];
        if (id.isString()) records.set(id.asString(), pair.record);
      }
      return records;
    }

    // `recordsByKey.get(keyId)?.paid_fallback_limit_microcredits === -1`
    bool hasUnlimitedRecord(const RecordsByKey& recordsByKey, const std::string& keyId) {
      const rt::KvValue* record = recordsByKey.get(keyId);
      return record != nullptr && isNumberValue((*record)["paid_fallback_limit_microcredits"], -1);
    }

    /** Every window reference that must agree with its projected request rows. */
    rt::Task<rt::OrderedMap<std::string, PaidFallbackWindowReference>> collectRepairablePaidFallbackWindows(std::shared_ptr<rt::Kv> kv, const RecordsByKey& recordsByKey,
                                                                                                          const rt::OrderedSet<std::string>& candidateWindowReferences) {
      rt::OrderedMap<std::string, PaidFallbackWindowReference> windows;
      for (const std::string& reference : candidateWindowReferences) {
        const json::value parsed = rt::jsonParse(reference);
        const json::array* items = parsed.if_array();
        if (items == nullptr || items->size() != 2 || !isApiKeyId(rt::KvValue((*items)[0])) || !isPositiveSafeInteger(rt::KvValue((*items)[1]))) {
          continue;
        }
        const std::string keyId((*items)[0].get_string());
        const rt::KvValue* record = recordsByKey.get(keyId);
        if (record != nullptr && !isNumberValue((*record)["paid_fallback_limit_microcredits"], -1)) {
          windows.set(reference, PaidFallbackWindowReference{.key_id = keyId, .window_reset_at_ms = *rt::getNumber((*items)[1])});
        }
      }
      {
        const std::size_t offset = PAID_FALLBACK_WINDOW_V3_PREFIX.size();
        const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = PAID_FALLBACK_WINDOW_V3_PREFIX});
        while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
          const rt::KvValue keyId = partAt(entry->key, static_cast<std::ptrdiff_t>(offset));
          const rt::KvValue windowResetAtMs = partAt(entry->key, static_cast<std::ptrdiff_t>(offset + 1));
          if (isApiKeyId(keyId) && isPositiveSafeInteger(windowResetAtMs) && !hasUnlimitedRecord(recordsByKey, keyId.asString())) {
            windows.set(paidFallbackV3WindowReference(keyId.asString(), windowResetAtMs.asNumber()),
                        PaidFallbackWindowReference{.key_id = keyId.asString(), .window_reset_at_ms = windowResetAtMs.asNumber()});
          }
        }
      }
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = PAID_FALLBACK_REQUEST_V3_PREFIX});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        if (!isPaidFallbackRequestV3(entry->value)) continue;
        const std::string keyId = entry->value["key_id"].asString();
        if (hasUnlimitedRecord(recordsByKey, keyId)) continue;
        const double windowResetAtMs = entry->value["window_reset_at_ms"].asNumber();
        windows.set(paidFallbackV3WindowReference(keyId, windowResetAtMs), PaidFallbackWindowReference{.key_id = keyId, .window_reset_at_ms = windowResetAtMs});
      }
      co_return windows;
    }

    // The members of a validated PaidFallbackRequestV3 row the window repair reads.
    struct WindowRequestView {
      std::string policy_version;
      std::string billing_state;
      double reserved_microcredits;
      std::optional<double> spend_microcredits;
    };

    /** Request rows currently stored for one window. */
    rt::Task<std::vector<WindowRequestView>> listPaidFallbackWindowRequests(std::shared_ptr<rt::Kv> kv, std::string keyId, double windowResetAtMs) {
      std::vector<WindowRequestView> requests;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = PAID_FALLBACK_REQUEST_V3_PREFIX});
      while (std::optional<rt::KvEntry<>> requestEntry = co_await iterator->next()) {
        const rt::KvValue& value = requestEntry->value;
        if (isPaidFallbackRequestV3(value) && value["key_id"].asString() == keyId && value["window_reset_at_ms"].asNumber() == windowResetAtMs) {
          const rt::KvValue& spend = value["spend_microcredits"];
          requests.push_back(WindowRequestView{.policy_version = value["policy_version"].asString(),
                                               .billing_state = value["billing_state"].asString(),
                                               .reserved_microcredits = value["reserved_microcredits"].asNumber(),
                                               .spend_microcredits = spend.isNull() ? std::nullopt : std::optional<double>(spend.asNumber())});
        }
      }
      co_return requests;
    }

    struct PaidFallbackRequestAggregate {
      double settledMicrocredits;
      double reservedMicrocredits;
      double pendingCount;
    };

    /** Aggregates the projected request rows of one window. */
    PaidFallbackRequestAggregate paidFallbackRequestAggregate(const std::vector<WindowRequestView>& requests) {
      double settledMicrocredits = 0;
      for (const WindowRequestView& request : requests) settledMicrocredits += request.billing_state == "settled" ? request.spend_microcredits.value_or(0) : 0;
      double reservedMicrocredits = 0;
      for (const WindowRequestView& request : requests) reservedMicrocredits += isOutstanding(request.billing_state) ? request.reserved_microcredits : 0;
      double pendingCount = 0;
      for (const WindowRequestView& request : requests) pendingCount += isOutstanding(request.billing_state) ? 1 : 0;
      return PaidFallbackRequestAggregate{.settledMicrocredits = settledMicrocredits, .reservedMicrocredits = reservedMicrocredits, .pendingCount = pendingCount};
    }

    // `context` of projectedPaidFallbackWindow: `record` nullptr is undefined.
    struct ProjectedWindowContext {
      std::string keyId;
      double windowResetAtMs;
      double nowMs;
      const rt::KvValue* record;
      const WindowRequestView* firstRequest;
    };

    /** The stored window row, or a freshly projected one when it is missing. */
    // `stored` is null or a row isPaidFallbackWindowV3 accepted.
    rt::KvObject projectedPaidFallbackWindow(const rt::KvValue& stored, const ProjectedWindowContext& context) {
      if (!stored.isNullish()) return stored.asObject();
      std::string policyVersion;
      if (context.firstRequest != nullptr) {
        policyVersion = context.firstRequest->policy_version;
      } else {
        const rt::KvValue windowMs = context.record != nullptr ? (*context.record)["window_ms"] : rt::KvValue();
        policyVersion = "legacy:" + rt::numberToString(context.windowResetAtMs) + ":" + (windowMs.isNullish() ? std::string("0") : rt::kvToString(windowMs)) + ":0";
      }
      const rt::KvValue limit = context.record != nullptr ? (*context.record)["paid_fallback_limit_microcredits"] : rt::KvValue();
      rt::KvObject window;
      window.set("v", 3.0);
      window.set("key_id", context.keyId);
      window.set("policy_version", policyVersion);
      window.set("window_reset_at_ms", context.windowResetAtMs);
      window.set("limit_microcredits", limit.isNullish() ? rt::KvValue(0.0) : limit);
      window.set("settled_microcredits", 0.0);
      window.set("reserved_microcredits", 0.0);
      window.set("pending_count", 0.0);
      window.set("updated_at_ms", context.nowMs);
      return window;
    }

    // `Readonly<{ keyId; windowResetAtMs; record: ApiKeyRecord | undefined; nowMs }>`
    struct RepairWindowContext {
      std::string keyId;
      double windowResetAtMs;
      std::optional<rt::KvValue> record;
      double nowMs;
    };

    /** Repairs one window aggregate with a bounded CAS retry loop. */
    rt::Task<void> repairOnePaidFallbackWindow(std::shared_ptr<rt::Kv> kv, const RepairWindowContext& context) {
      const rt::KvKey windowKey = paidFallbackV3WindowKey(context.keyId, context.windowResetAtMs);
      const std::string label = context.keyId + "/" + rt::numberToString(context.windowResetAtMs);
      for (int attempt = 0; attempt < 5; attempt += 1) {
        rt::Task<std::vector<WindowRequestView>> listing = listPaidFallbackWindowRequests(kv, context.keyId, context.windowResetAtMs);
        const std::vector<WindowRequestView> requests = co_await std::move(listing);
        const PaidFallbackRequestAggregate aggregate = paidFallbackRequestAggregate(requests);
        if (!rt::Number::isSafeInteger(aggregate.settledMicrocredits) || !rt::Number::isSafeInteger(aggregate.reservedMicrocredits) ||
            !rt::Number::isSafeInteger(aggregate.pendingCount)) {
          throw rt::JsError("Paid fallback V3 window aggregate overflow: " + label);
        }
        const rt::KvEntryMaybe<> entry = co_await kv->get(windowKey, STRONG);
        if (!entry.value.isNull() && !paid_fallback::ledger_state::isPaidFallbackWindowV3(rt::storedJsonView(entry.value))) {
          throw rt::JsError("Paid fallback V3 window is invalid: " + label);
        }
        const ProjectedWindowContext projected{.keyId = context.keyId,
                                               .windowResetAtMs = context.windowResetAtMs,
                                               .nowMs = context.nowMs,
                                               .record = context.record ? &*context.record : nullptr,
                                               .firstRequest = requests.empty() ? nullptr : &requests.front()};
        const rt::KvObject current = projectedPaidFallbackWindow(entry.value, projected);
        if (entry.value.truthy() && isNumberValue(current.at("settled_microcredits"), aggregate.settledMicrocredits) &&
            isNumberValue(current.at("reserved_microcredits"), aggregate.reservedMicrocredits) && isNumberValue(current.at("pending_count"), aggregate.pendingCount)) {
          break;
        }
        rt::KvObject next = current;
        next.set("settled_microcredits", aggregate.settledMicrocredits);
        next.set("reserved_microcredits", aggregate.reservedMicrocredits);
        next.set("pending_count", aggregate.pendingCount);
        next.set("updated_at_ms", rt::Math::max(context.nowMs, current.at("updated_at_ms").asNumber()));
        const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
        atomic->check(entry).set(windowKey, rt::KvValue(std::move(next)));
        const rt::KvCommitResult committed = co_await atomic->commit();
        if (committed.ok) break;
        if (attempt == 4) {
          throw rt::JsError("Paid fallback V3 window changed concurrently: " + label);
        }
      }
    }

    rt::Task<void> repairProjectedPaidFallbackWindows(std::shared_ptr<rt::Kv> kv, const std::vector<StrictApiKeyPair>& pairs,
                                                      const rt::OrderedSet<std::string>& candidateWindowReferences, double nowMs) {
      const RecordsByKey recordsByKey = recordsByKeyOf(pairs);
      rt::Task<rt::OrderedMap<std::string, PaidFallbackWindowReference>> collecting = collectRepairablePaidFallbackWindows(kv, recordsByKey, candidateWindowReferences);
      const rt::OrderedMap<std::string, PaidFallbackWindowReference> windows = co_await std::move(collecting);
      for (const auto& [reference, window] : windows) {
        const rt::KvValue* record = recordsByKey.get(window.key_id);
        const RepairWindowContext context{.keyId = window.key_id,
                                          .windowResetAtMs = window.window_reset_at_ms,
                                          .record = record != nullptr ? std::optional<rt::KvValue>(*record) : std::nullopt,
                                          .nowMs = nowMs};
        co_await repairOnePaidFallbackWindow(kv, context);
      }
    }

    /** Maps a legacy billing status onto the V3 billing state. */
    std::string legacyV3BillingState(const std::string& billingStatus) {
      if (billingStatus == "reconciled") return "settled";
      if (billingStatus == "not_billed") return "not_billed";
      if (billingStatus == "unresolved") return "unresolved";
      return "pending";
    }

    /** Maps a legacy billing state and status code onto the V3 terminal state. */
    std::string legacyV3TerminalState(const std::string& billingState, double statusCode) {
      if (billingState == "settled") return statusCode >= 200 && statusCode < 300 ? "completed" : "failed";
      if (billingState == "not_billed") return "cancelled";
      if (billingState == "unresolved") return "ambiguous";
      return "pending";
    }

    // `LegacyPaidFallbackProjectionStep = Readonly<{ projected: number; pending: number }>`
    struct LegacyPaidFallbackProjectionStep {
      double projected;
      double pending;
    };

    /** V3 request row projected from one legacy request-log row. */
    PaidFallbackRequestV3 legacyPaidFallbackRequestRow(const ApiKeyRequestLogRecord& legacy, const rt::KvValue& record, double windowResetAtMs,
                                                      const std::string& billingState, double quotaPerCredit, double nowMs) {
      const bool outstanding = isOutstanding(billingState);
      const std::string dispatchState = (legacy.provider_request_id && !legacy.provider_request_id->empty()) || legacy.status_code > 0 ? "dispatched" : "reserved";
      const std::string terminalState = legacyV3TerminalState(billingState, legacy.status_code);
      const double terminalAtMs = legacy.completed_at_ms.value_or(legacy.created_at_ms);
      return PaidFallbackRequestV3{
        .v = 3,
        .key_id = legacy.key_id,
        .request_id = legacy.id,
        .policy_version = legacyPolicyVersion(record, legacy, windowResetAtMs),
        .route = legacy.route,
        .path = legacy.path,
        .model = legacy.model.value_or("legacy-unknown"),
        .stream = legacy.stream,
        .reasoning = legacy.reasoning,
        .provider = legacy.provider == "surplus" ? std::optional<std::string>("surplus") : std::nullopt,
        .window_reset_at_ms = windowResetAtMs,
        .reserved_microcredits = outstanding ? legacyMaximumExposure(record, legacy) : 0,
        .quota_per_credit = quotaPerCredit,
        .provider_request_id = legacy.provider_request_id,
        .provider_quota = legacy.provider_quota,
        .input_tokens = legacy.input_tokens,
        .cached_input_tokens = rt::undefined,
        .output_tokens = legacy.output_tokens,
        .dispatch_state = dispatchState,
        .terminal_state = terminalState,
        .spend_microcredits = billingState == "settled" ? std::optional<double>(legacy.spend_microcredits.value_or(0)) : std::nullopt,
        .billing_state = billingState,
        .reconciliation_attempts = 0,
        .last_reconciliation_at_ms = std::nullopt,
        .dispatched_at_ms = dispatchState == "dispatched" ? std::optional<double>(legacy.created_at_ms) : std::nullopt,
        .terminal_at_ms = terminalState == "pending" ? std::nullopt : std::optional<double>(terminalAtMs),
        .settled_at_ms = billingState == "settled" ? std::optional<double>(terminalAtMs) : std::nullopt,
        .created_at_ms = legacy.created_at_ms,
        .updated_at_ms = rt::Math::max(legacy.created_at_ms, legacy.completed_at_ms.value_or(0), nowMs),
        .usage_rollup_at_ms = rt::undefined,
      };
    }

    // `keys: Readonly<{ requestKey: KvKey; pendingKey: KvKey }>`
    struct LegacyPaidFallbackKeys {
      rt::KvKey requestKey;
      rt::KvKey pendingKey;
    };

    /** Writes the projected V3 request and pending rows for one legacy candidate. */
    rt::Task<LegacyPaidFallbackProjectionStep> commitLegacyPaidFallbackStep(std::shared_ptr<rt::Kv> kv, const LegacyPaidFallbackKeys& keys,
                                                                            const PaidFallbackRequestV3& request, const rt::KvEntryMaybe<>& requestEntry,
                                                                            const rt::KvEntryMaybe<>& pendingEntry, double nowMs) {
      const rt::KvValue& existingRequest = requestEntry.value;
      // `effectiveRequest = existingRequest ?? request` (a stored row isPaidFallbackRequestV3 accepted).
      const bool storedRequest = !existingRequest.isNullish();
      const std::string effectiveBillingState = storedRequest ? existingRequest["billing_state"].asString() : request.billing_state;
      const double effectiveCreatedAtMs = storedRequest ? existingRequest["created_at_ms"].asNumber() : request.created_at_ms;
      const std::unique_ptr<rt::AtomicOperation> atomic = kv->atomic();
      atomic->check(requestEntry).check(pendingEntry);
      bool needsCommit = false;
      double projected = 0;
      double pending = 0;
      if (existingRequest.isNull()) {
        // Migrated rows inherit the one-year raw-row retention anchored to
        // their original creation time, so a re-run of this migration cannot
        // resurrect rows that have already aged out of the retention window.
        atomic->set(keys.requestKey, rt::KvValue(json::value_from(request)), rt::KvSetOptions{.expireIn = paid_fallback::ledger_state::requestRowExpireIn(request, nowMs)});
        projected += 1;
        needsCommit = true;
      }
      const bool effectiveOutstanding = isOutstanding(effectiveBillingState);
      if (effectiveOutstanding) {
        atomic->set(keys.pendingKey, pendingMarker(effectiveCreatedAtMs, rt::Math::max(nowMs, effectiveCreatedAtMs)));
        if (pendingEntry.value.isNull()) {
          pending += 1;
          needsCommit = true;
        } else if (!paidFallbackPendingIdentityMatches(pendingEntry.value, request.key_id, request.request_id)) {
          needsCommit = true;
        }
      } else if (!pendingEntry.value.isNull()) {
        atomic->delete_(keys.pendingKey);
        needsCommit = true;
      }
      if (needsCommit) {
        const rt::KvCommitResult committed = co_await atomic->commit();
        if (!committed.ok) {
          throw rt::JsError("Paid fallback V3 migration changed concurrently: " + request.key_id + "/" + request.request_id);
        }
      }
      co_return LegacyPaidFallbackProjectionStep{.projected = projected, .pending = pending};
    }

    /** Projects one legacy paid-fallback candidate into the V3 rows. */
    rt::Task<LegacyPaidFallbackProjectionStep> projectLegacyPaidFallbackCandidate(std::shared_ptr<rt::Kv> kv, const ApiKeyRequestLogRecord& legacy,
                                                                                  const RecordsByKey& recordsByKey, rt::OrderedSet<std::string>& candidateWindowReferences,
                                                                                  double nowMs) {
      const rt::KvValue* found = recordsByKey.get(legacy.key_id);
      if (found == nullptr) co_return LegacyPaidFallbackProjectionStep{.projected = 0, .pending = 0};
      const rt::KvValue record = *found;
      const LegacyPaidFallbackKeys keys{.requestKey = rt::KvKey{PAID_FALLBACK_V3_PREFIX, "request", legacy.key_id, legacy.id},
                                        .pendingKey = rt::KvKey{PAID_FALLBACK_V3_PREFIX, "pending", legacy.key_id, legacy.id}};
      const double windowResetAtMs = legacyWindowResetAtMs(record, legacy);
      if (!isNumberValue(record["paid_fallback_limit_microcredits"], -1)) {
        candidateWindowReferences.add(paidFallbackV3WindowReference(legacy.key_id, windowResetAtMs));
      }
      const std::string billingState = legacyV3BillingState(legacy.billing_status);
      const rt::KvValue quotaPerCredit = legacy.quota_per_credit ? rt::KvValue(*legacy.quota_per_credit) : record["paid_fallback_quota_per_credit"];
      if (!isPositiveSafeInteger(quotaPerCredit)) {
        throw rt::JsError("Legacy paid fallback request has no valid pricing: " + legacy.key_id + "/" + legacy.id);
      }
      const PaidFallbackRequestV3 request = legacyPaidFallbackRequestRow(legacy, record, windowResetAtMs, billingState, quotaPerCredit.asNumber(), nowMs);
      std::vector<rt::Promise<rt::KvEntryMaybe<>>> reads;
      reads.push_back(kv->get(keys.requestKey, STRONG));
      reads.push_back(kv->get(keys.pendingKey, STRONG));
      std::vector<rt::KvEntryMaybe<>> read = co_await rt::all(std::move(reads)).wait();
      const rt::KvEntryMaybe<> requestEntry = std::move(read[0]);
      const rt::KvEntryMaybe<> pendingEntry = std::move(read[1]);
      const rt::KvValue& existingRequest = requestEntry.value;
      if (!existingRequest.isNull() && (!isPaidFallbackRequestV3(existingRequest) || !isStringValue(existingRequest["key_id"], request.key_id) ||
                                        !isStringValue(existingRequest["request_id"], request.request_id))) {
        throw rt::JsError("V3 paid fallback request identity collision: " + legacy.key_id + "/" + legacy.id);
      }
      rt::Task<LegacyPaidFallbackProjectionStep> committing = commitLegacyPaidFallbackStep(kv, keys, request, requestEntry, pendingEntry, nowMs);
      const LegacyPaidFallbackProjectionStep step = co_await std::move(committing);
      co_return step;
    }

    rt::Task<LegacyPaidFallbackProjectionResult> projectLegacy(std::shared_ptr<rt::Kv> kv, std::vector<StrictApiKeyPair> pairs, double nowMs) {
      const RecordsByKey recordsByKey = recordsByKeyOf(pairs);
      const std::vector<LegacyPaidFallbackProjectionCandidate> candidates = co_await listLegacyPaidFallbackProjectionCandidates(kv);
      double projected = 0;
      double pending = 0;
      rt::OrderedSet<std::string> candidateWindowReferences;
      for (const LegacyPaidFallbackProjectionCandidate& candidate : candidates) {
        rt::Task<LegacyPaidFallbackProjectionStep> projecting = projectLegacyPaidFallbackCandidate(kv, candidate.value, recordsByKey, candidateWindowReferences, nowMs);
        const LegacyPaidFallbackProjectionStep step = co_await std::move(projecting);
        projected += step.projected;
        pending += step.pending;
      }
      co_await repairProjectedPaidFallbackPending(kv, nowMs);
      co_await repairProjectedPaidFallbackWindows(kv, pairs, candidateWindowReferences, nowMs);
      // Migration can create or repair pending markers after the runtime gate has
      // already been initialized. Recompute it so the next cron wake-up cannot
      // miss newly projected billable fallback work.
      const std::optional<double> gate = co_await paid_fallback::ledger_state::recomputePaidFallbackReconciliationGateV3(kv);
      (void)gate;
      co_return LegacyPaidFallbackProjectionResult{.projected = projected, .pending = pending};
    }

    /** True when an untrusted row is an outstanding paid-fallback ledger row. */
    bool isPendingPaidFallbackLedgerValue(const rt::KvValue& value) {
      return rt::kvIsObject(value) && isStringValue(value["provider"], "metered") &&
             (isStringValue(value["billing_status"], "pending") || isStringValue(value["billing_status"], "unresolved"));
    }

    /** References of every pending row already stored in the dedicated ledger. */
    rt::Task<rt::OrderedSet<std::string>> collectPendingPaidFallbackLedgerReferences(std::shared_ptr<rt::Kv> kv, std::vector<std::string>& errors) {
      rt::OrderedSet<std::string> availableReferences;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = PAID_FALLBACK_LEDGER_PREFIX});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        if (!isPendingPaidFallbackLedgerValue(entry->value)) continue;
        const std::optional<std::string> reference = pendingPaidFallbackLedgerReferenceFromEntry(*entry, PAID_FALLBACK_LEDGER_PREFIX);
        if (!reference) {
          errors.push_back("pending paid fallback ledger is malformed: " + keyJson(entry->key));
          continue;
        }
        availableReferences.add(*reference);
      }
      co_return availableReferences;
    }

    /** Pending legacy request-log rows and their dedicated-ledger destinations. */
    rt::Task<std::vector<LegacyPaidFallbackLedgerCandidate>> collectPendingLegacyPaidFallbackCandidates(std::shared_ptr<rt::Kv> kv,
                                                                                                       rt::OrderedSet<std::string>& availableReferences,
                                                                                                       std::vector<std::string>& errors) {
      std::vector<LegacyPaidFallbackLedgerCandidate> candidates;
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = LEGACY_REQUEST_LOG_PREFIX});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        const rt::KvValue& value = entry->value;
        if (!isPendingPaidFallbackLedgerValue(value)) continue;
        const std::optional<std::string> reference = pendingPaidFallbackLedgerReferenceFromEntry(*entry, LEGACY_REQUEST_LOG_PREFIX);
        if (!reference || !isPendingPaidFallbackLedgerRecord(value)) {
          errors.push_back("pending legacy paid fallback ledger is malformed: " + keyJson(entry->key));
          continue;
        }
        const rt::KvKey ledgerKey{PAID_FALLBACK_LEDGER_PREFIX, value["key_id"].asString(), value["created_at_ms"].asNumber(), value["id"].asString()};
        const rt::KvEntryMaybe<> existing = co_await kv->get(ledgerKey);
        candidates.push_back(LegacyPaidFallbackLedgerCandidate{.value = value, .ledgerKey = ledgerKey, .existing = existing});
        if (existing.value.isNull()) {
          availableReferences.add(*reference);
          continue;
        }
        if (!paidFallbackLedgerEntryMatchesIdentity(existing, value)) {
          errors.push_back("paid fallback ledger destination conflicts with legacy record: " + value["id"].asString());
          continue;
        }
        const std::optional<std::string> existingReference = pendingPaidFallbackLedgerReferenceFromEntry(existing, PAID_FALLBACK_LEDGER_PREFIX);
        if (existingReference == reference) availableReferences.add(*reference);
      }
      co_return candidates;
    }

    /** Reports reservations whose pending ledger row is missing. */
    void auditPaidFallbackReservationLedgers(const std::vector<StrictApiKeyPair>& pairs, const rt::OrderedSet<std::string>& availableReferences,
                                             std::vector<std::string>& errors) {
      for (const StrictApiKeyPair& pair : pairs) {
        const rt::KvValue& requestId = pair.record["paid_fallback_reservation_request_id"];
        if (requestId.isNull()) continue;
        // `paidFallbackLedgerReference(record.id, requestId)`: JSON.stringify([record.id, requestId]).
        const std::string reference = *rt::kvJsonStringify(rt::KvValue(rt::KvArray{pair.record["id"], requestId}));
        if (!availableReferences.has(reference)) {
          errors.push_back("paid fallback reservation has no pending ledger record: " + rt::kvToString(pair.record["id"]));
        }
      }
    }

    rt::Task<PendingPaidFallbackLedgerInventory> inspectPending(std::shared_ptr<rt::Kv> kv, std::vector<StrictApiKeyPair> pairs) {
      std::vector<std::string> errors;
      rt::Task<rt::OrderedSet<std::string>> collectingReferences = collectPendingPaidFallbackLedgerReferences(kv, errors);
      rt::OrderedSet<std::string> availableReferences = co_await std::move(collectingReferences);
      rt::Task<std::vector<LegacyPaidFallbackLedgerCandidate>> collectingCandidates = collectPendingLegacyPaidFallbackCandidates(kv, availableReferences, errors);
      std::vector<LegacyPaidFallbackLedgerCandidate> candidates = co_await std::move(collectingCandidates);
      auditPaidFallbackReservationLedgers(pairs, availableReferences, errors);
      co_return PendingPaidFallbackLedgerInventory{.candidates = std::move(candidates), .errors = std::move(errors)};
    }
  }

  rt::Task<LegacyPaidFallbackProjectionResult> projectLegacyPaidFallbackV3(std::shared_ptr<rt::Kv> kv, std::vector<StrictApiKeyPair> pairs, double nowMs) {
    return projectLegacy(std::move(kv), std::move(pairs), nowMs);
  }

  rt::Task<PendingPaidFallbackLedgerInventory> inspectPendingPaidFallbackLedgers(std::shared_ptr<rt::Kv> kv, std::vector<StrictApiKeyPair> pairs) {
    return inspectPending(std::move(kv), std::move(pairs));
  }
}
