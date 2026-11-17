#include "uos/cache/kv_migration_paid_fallback.hpp"

#include "uos/paid_fallback/index.hpp"
#include "uos/paid_fallback/ledger_state.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"

#include <array>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace uos::cache::kv_migration_paid_fallback {
  namespace {
    const rt::KvKey& PAID_FALLBACK_RECONCILIATION_LEASE_V3_PREFIX() {
      static const rt::KvKey prefix{PAID_FALLBACK_V3_PREFIX, "reconciliation_lease"};
      return prefix;
    }
    const rt::KvKey& PAID_FALLBACK_DELETION_GUARD_V3_PREFIX() {
      static const rt::KvKey prefix{PAID_FALLBACK_V3_PREFIX, "deletion_guard"};
      return prefix;
    }

    // `key[index]` as the stored value JS holds for it: undefined past the end.
    rt::KvValue partAt(const rt::KvKey& key, std::size_t index) { return index < key.size() ? rt::kvValueFromKeyPart(key[index]) : rt::KvValue(); }

    // `JSON.stringify(key)` (a bigint part throws TypeError, as in JS).
    std::string keyJson(const rt::KvKey& key) { return *rt::kvJsonStringify(rt::kvValueFromKey(key)); }

    bool isUndefinedValue(const rt::KvValue& value) noexcept { return value.isUndefined() || value.is<rt::KvHole>(); }

    // `left === right` on stored values: primitives by value (numbers by ===); any object is a
    // distinct JS object (every read deserializes its own), so it equals nothing here.
    bool strictEquals(const rt::KvValue& left, const rt::KvValue& right) {
      if (isUndefinedValue(left)) return isUndefinedValue(right);
      if (left.isNull()) return right.isNull();
      if (left.isBool()) return right.isBool() && left.asBool() == right.asBool();
      if (left.isNumber()) return right.isNumber() && left.asNumber() == right.asNumber();
      if (left.isString()) return right.isString() && left.asString() == right.asString();
      if (left.isBigInt()) return right.isBigInt() && left.asBigInt() == right.asBigInt();
      return false;
    }

    bool isNumberValue(const rt::KvValue& value, double number) { return value.isNumber() && value.asNumber() == number; }
    bool isStringValue(const rt::KvValue& value, std::string_view text) { return value.isString() && value.asString() == text; }

    // `"key" in value` for a value isRecord accepted.
    bool hasOwnMember(const rt::KvValue& value, std::string_view key) { return value.isObject() && value.asObject().contains(key); }

    bool isRoutableApiKeyPrefix(const rt::KvValue& value) {
      static const rt::Regex PATTERN("^u_[0-9a-f]{10}$");
      return value.isString() && PATTERN.test(value.asString());
    }

    // Larger than any supported API-key window preset (1m/1h/1d/1w), so exact
    // window reconstruction stops before its earliest rows can age out.
    constexpr double PAID_FALLBACK_VALIDATION_SLACK_MS = 32.0 * 24 * 60 * 60 * 1'000;

    bool isExpirationTimestamp(const rt::KvValue& value) { return isNumberValue(value, -1) || isSafeUsageCount(value); }
    bool isRevocationTimestamp(const rt::KvValue& value) { return value.isNull() || isSafeUsageCount(value); }
    bool isUsageLimit(const rt::KvValue& value) { return isNumberValue(value, -1) || isSafeUsageCount(value); }
    bool isNullablePositiveSafeInteger(const rt::KvValue& value) { return value.isNull() || isPositiveSafeInteger(value); }
    bool isNullableNonEmptyString(const rt::KvValue& value) { return value.isNull() || (value.isString() && rt::length(value.asString()) > 0); }
    bool isNonEmptyString(const rt::KvValue& value) { return value.isString() && rt::length(value.asString()) > 0; }

    bool hasPaidFallbackLedgerIdentity(const rt::KvValue& value) {
      return rt::kvIsObject(value) && isApiKeyId(value["key_id"]) && isApiKeyId(value["id"]) && isPositiveSafeInteger(value["created_at_ms"]) &&
             isStringValue(value["provider"], "metered");
    }

    // `left === right` between a key part and a stored value.
    bool partEquals(const rt::KvValue& part, const rt::KvValue& value) { return strictEquals(part, value); }

    // `[...states].includes(String(value))`
    template <std::size_t N> bool includesString(const std::array<std::string_view, N>& states, const rt::KvValue& value) {
      const std::string text = rt::kvToString(value);
      for (const std::string_view state : states)
        if (state == text) return true;
      return false;
    }

    std::string paidFallbackV3Reference(const std::string& keyId, const std::string& requestId) {
      json::array reference;
      reference.emplace_back(json::string(keyId));
      reference.emplace_back(json::string(requestId));
      return rt::jsonStringify(reference);
    }

    // The members of a validated PaidFallbackWindowV3 row this module reads.
    struct PaidFallbackWindowView {
      std::string key_id;
      double window_reset_at_ms;
      double reserved_microcredits;
      double settled_microcredits;
      double pending_count;
    };

    // The members of a validated PaidFallbackRequestV3 row this module reads.
    struct PaidFallbackRequestView {
      std::string key_id;
      std::string request_id;
      double window_reset_at_ms;
      double reserved_microcredits;
      std::optional<double> spend_microcredits;
      std::string billing_state;
    };

    PaidFallbackWindowView windowView(const rt::KvValue& value) {
      return PaidFallbackWindowView{.key_id = value["key_id"].asString(),
                                    .window_reset_at_ms = value["window_reset_at_ms"].asNumber(),
                                    .reserved_microcredits = value["reserved_microcredits"].asNumber(),
                                    .settled_microcredits = value["settled_microcredits"].asNumber(),
                                    .pending_count = value["pending_count"].asNumber()};
    }

    PaidFallbackRequestView requestView(const rt::KvValue& value) {
      const rt::KvValue& spend = value["spend_microcredits"];
      return PaidFallbackRequestView{.key_id = value["key_id"].asString(),
                                     .request_id = value["request_id"].asString(),
                                     .window_reset_at_ms = value["window_reset_at_ms"].asNumber(),
                                     .reserved_microcredits = value["reserved_microcredits"].asNumber(),
                                     .spend_microcredits = spend.isNull() ? std::nullopt : std::optional<double>(spend.asNumber()),
                                     .billing_state = value["billing_state"].asString()};
    }

    using PaidFallbackWindowMap = rt::OrderedMap<std::string, PaidFallbackWindowView>;
    using PaidFallbackRequestMap = rt::OrderedMap<std::string, PaidFallbackRequestView>;

    struct WindowInventory {
      double count;
      PaidFallbackWindowMap windows;
    };

    /** Validates every V3 window row and indexes it by its key reference. */
    rt::Task<WindowInventory> collectPaidFallbackWindows(std::shared_ptr<rt::Kv> kv, const rt::OrderedSet<std::string>& knownKeyIds, std::vector<std::string>& errors) {
      WindowInventory inventory{.count = 0, .windows = {}};
      const std::size_t offset = PAID_FALLBACK_WINDOW_V3_PREFIX.size();
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = PAID_FALLBACK_WINDOW_V3_PREFIX});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        inventory.count += 1;
        const rt::KvValue keyId = partAt(entry->key, offset);
        const rt::KvValue windowResetAtMs = partAt(entry->key, offset + 1);
        if (entry->key.size() != offset + 2 || !isApiKeyId(keyId) || !isPositiveSafeInteger(windowResetAtMs) ||
            !paid_fallback::ledger_state::isPaidFallbackWindowV3(rt::storedJsonView(entry->value)) || !strictEquals(entry->value["key_id"], keyId) ||
            !strictEquals(entry->value["window_reset_at_ms"], windowResetAtMs)) {
          errors.push_back("paid fallback V3 window is malformed: " + keyJson(entry->key));
          continue;
        }
        if (!knownKeyIds.has(keyId.asString())) errors.push_back("paid fallback V3 window is orphaned: " + keyId.asString());
        inventory.windows.set(paidFallbackV3WindowReference(keyId.asString(), windowResetAtMs.asNumber()), windowView(entry->value));
      }
      co_return inventory;
    }

    struct RequestInventory {
      double count;
      PaidFallbackRequestMap requests;
    };

    /** Validates every V3 request row and indexes it by its key reference. */
    rt::Task<RequestInventory> collectPaidFallbackRequests(std::shared_ptr<rt::Kv> kv, const rt::OrderedSet<std::string>& knownKeyIds, std::vector<std::string>& errors) {
      RequestInventory inventory{.count = 0, .requests = {}};
      const std::size_t offset = PAID_FALLBACK_REQUEST_V3_PREFIX.size();
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = PAID_FALLBACK_REQUEST_V3_PREFIX});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        inventory.count += 1;
        const rt::KvValue keyId = partAt(entry->key, offset);
        const rt::KvValue requestId = partAt(entry->key, offset + 1);
        if (entry->key.size() != offset + 2 || !isApiKeyId(keyId) || !isApiKeyId(requestId) || !isPaidFallbackRequestV3(entry->value) ||
            !strictEquals(entry->value["key_id"], keyId) || !strictEquals(entry->value["request_id"], requestId)) {
          errors.push_back("paid fallback V3 request is malformed: " + keyJson(entry->key));
          continue;
        }
        if (!knownKeyIds.has(keyId.asString())) errors.push_back("paid fallback V3 request is orphaned: " + keyId.asString() + "/" + requestId.asString());
        inventory.requests.set(paidFallbackV3Reference(keyId.asString(), requestId.asString()), requestView(entry->value));
      }
      co_return inventory;
    }

    struct PendingInventory {
      double count;
      rt::OrderedSet<std::string> pending;
    };

    /** Validates every outstanding-pending marker row. */
    rt::Task<PendingInventory> collectPaidFallbackPending(std::shared_ptr<rt::Kv> kv, std::vector<std::string>& errors) {
      PendingInventory inventory{.count = 0, .pending = {}};
      const std::size_t offset = PAID_FALLBACK_PENDING_V3_PREFIX.size();
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = PAID_FALLBACK_PENDING_V3_PREFIX});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        inventory.count += 1;
        const rt::KvValue keyId = partAt(entry->key, offset);
        const rt::KvValue requestId = partAt(entry->key, offset + 1);
        const rt::KvValue& value = entry->value;
        if (entry->key.size() != offset + 2 || !isApiKeyId(keyId) || !isApiKeyId(requestId) || !rt::kvIsObject(value) ||
            !isPositiveSafeInteger(value["created_at_ms"]) || !isPositiveSafeInteger(value["next_reconciliation_at_ms"]) ||
            value["next_reconciliation_at_ms"].asNumber() < value["created_at_ms"].asNumber() ||
            (hasOwnMember(value, "key_id") && !partEquals(keyId, value["key_id"])) || (hasOwnMember(value, "request_id") && !partEquals(requestId, value["request_id"]))) {
          errors.push_back("paid fallback V3 pending marker is malformed: " + keyJson(entry->key));
          continue;
        }
        inventory.pending.add(paidFallbackV3Reference(keyId.asString(), requestId.asString()));
      }
      co_return inventory;
    }

    /** Validates every reconciliation lease row and counts them. */
    rt::Task<double> countPaidFallbackLeases(std::shared_ptr<rt::Kv> kv, const rt::OrderedSet<std::string>& knownKeyIds, std::vector<std::string>& errors) {
      double count = 0;
      const rt::KvKey& prefix = PAID_FALLBACK_RECONCILIATION_LEASE_V3_PREFIX();
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = prefix});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        count += 1;
        const rt::KvValue keyId = partAt(entry->key, prefix.size());
        const rt::KvValue& value = entry->value;
        if (entry->key.size() != prefix.size() + 1 || !isApiKeyId(keyId) || !rt::kvIsObject(value) || !value["token"].isString() ||
            rt::length(value["token"].asString()) == 0 || !isPositiveSafeInteger(value["expires_at_ms"])) {
          errors.push_back("paid fallback V3 reconciliation lease is malformed: " + keyJson(entry->key));
          continue;
        }
        if (!knownKeyIds.has(keyId.asString())) errors.push_back("paid fallback V3 reconciliation lease is orphaned: " + keyId.asString());
      }
      co_return count;
    }

    /** Validates every deletion guard row and counts them. */
    rt::Task<double> countPaidFallbackDeletionGuards(std::shared_ptr<rt::Kv> kv, std::vector<std::string>& errors) {
      double count = 0;
      const rt::KvKey& prefix = PAID_FALLBACK_DELETION_GUARD_V3_PREFIX();
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(rt::KvListSelector{.prefix = prefix});
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        count += 1;
        const rt::KvValue keyId = partAt(entry->key, prefix.size());
        if (entry->key.size() != prefix.size() + 1 || !isApiKeyId(keyId) || !rt::kvIsObject(entry->value) || !isPositiveSafeInteger(entry->value["created_at_ms"])) {
          errors.push_back("paid fallback V3 deletion guard is malformed: " + keyJson(entry->key));
        }
      }
      co_return count;
    }

    bool isOutstanding(const PaidFallbackRequestView& request) { return request.billing_state == "pending" || request.billing_state == "unresolved"; }

    /** Reports every pending-marker inconsistency for one request row. */
    void pushRequestMarkerErrors(const std::string& reference, const PaidFallbackRequestView& request, const rt::OrderedSet<std::string>& pending,
                                 std::vector<std::string>& errors) {
      const bool outstanding = isOutstanding(request);
      if (outstanding && !pending.has(reference)) {
        errors.push_back("paid fallback V3 request is missing its pending marker: " + request.key_id + "/" + request.request_id);
      }
      if (!outstanding && pending.has(reference)) {
        errors.push_back("paid fallback V3 terminal request retains a pending marker: " + request.key_id + "/" + request.request_id);
      }
    }

    /** Reports every window/spend inconsistency for one request row. */
    void pushRequestWindowErrors(const PaidFallbackRequestView& request, const PaidFallbackWindowMap& windows, const rt::OrderedSet<std::string>& unlimitedKeyIds,
                                 std::vector<std::string>& errors) {
      const PaidFallbackWindowView* window = windows.get(paidFallbackV3WindowReference(request.key_id, request.window_reset_at_ms));
      const bool isBounded = !unlimitedKeyIds.has(request.key_id);
      if (isBounded && (request.reserved_microcredits > 0 || (request.billing_state == "settled" && request.spend_microcredits.value_or(0) > 0)) && window == nullptr) {
        errors.push_back("paid fallback V3 bounded request is missing its window: " + request.key_id + "/" + request.request_id);
      }
      if (request.billing_state == "settled" && !request.spend_microcredits) {
        errors.push_back("paid fallback V3 settled request is missing spend: " + request.key_id + "/" + request.request_id);
      }
      if (isOutstanding(request) && request.spend_microcredits) {
        errors.push_back("paid fallback V3 outstanding request has spend: " + request.key_id + "/" + request.request_id);
      }
    }

    /** Cross-checks every request row against its pending marker and window. */
    void auditPaidFallbackRequests(const PaidFallbackRequestMap& requests, const rt::OrderedSet<std::string>& pending, const PaidFallbackWindowMap& windows,
                                   const rt::OrderedSet<std::string>& unlimitedKeyIds, std::vector<std::string>& errors) {
      for (const auto& [reference, request] : requests) {
        pushRequestMarkerErrors(reference, request, pending, errors);
        pushRequestWindowErrors(request, windows, unlimitedKeyIds, errors);
        // Historical requests retain the policy version that admitted them. A
        // window can carry a newer version after an admin policy edit.
      }
      for (const std::string& reference : pending) {
        if (!requests.has(reference)) errors.push_back("paid fallback V3 pending marker is orphaned: " + reference);
      }
    }

    /** Recomputes every window aggregate that is still fully reconstructible. */
    void auditPaidFallbackWindows(const PaidFallbackWindowMap& windows, const PaidFallbackRequestMap& requests, double nowMs, std::vector<std::string>& errors) {
      for (const auto& [reference, window] : windows) {
        // Raw request rows expire one year after their individual creation times,
        // so a window's earliest rows can age out up to one window length before
        // `reset + retention`. Stop exact reconstruction with a conservative
        // slack (larger than any supported window preset) instead of reporting a
        // healthy window as inconsistent once its rows start disappearing.
        if (window.window_reset_at_ms + paid_fallback::ledger_state::PAID_FALLBACK_REQUEST_LOG_RETENTION_MS - PAID_FALLBACK_VALIDATION_SLACK_MS <= nowMs) continue;
        double outstandingCount = 0;
        double reservedMicrocredits = 0;
        for (const auto& [requestReference, request] : requests) {
          if (request.key_id == window.key_id && request.window_reset_at_ms == window.window_reset_at_ms && isOutstanding(request)) {
            outstandingCount += 1;
            reservedMicrocredits += request.reserved_microcredits;
          }
        }
        double settledMicrocredits = 0;
        for (const auto& [requestReference, request] : requests) {
          if (request.key_id == window.key_id && request.window_reset_at_ms == window.window_reset_at_ms && request.billing_state == "settled") {
            settledMicrocredits += request.spend_microcredits.value_or(0);
          }
        }
        if (!rt::Number::isSafeInteger(reservedMicrocredits) || !rt::Number::isSafeInteger(settledMicrocredits) || window.pending_count != outstandingCount ||
            window.reserved_microcredits != reservedMicrocredits || window.settled_microcredits != settledMicrocredits) {
          errors.push_back("paid fallback V3 window aggregate is inconsistent: " + reference);
        }
      }
    }
  }

  bool isSafeUsageCount(const rt::KvValue& value) {
    return value.isNumber() && rt::Number::isSafeInteger(value.asNumber()) && value.asNumber() >= 0;
  }

  bool isApiKeyId(const rt::KvValue& value) {
    if (!value.isString()) return false;
    const std::string& text = value.asString();
    const double length = static_cast<double>(rt::length(text));
    return text == rt::trim(text) && length > 0 && length <= 200;
  }

  bool isApiKeyHash(const rt::KvValue& value) {
    static const rt::Regex PATTERN("^[A-Za-z0-9_-]{43}$");
    return value.isString() && PATTERN.test(value.asString());
  }

  bool isPositiveSafeInteger(const rt::KvValue& value) { return isSafeUsageCount(value) && value.asNumber() > 0; }

  bool isFiniteNonNegativeNumber(const rt::KvValue& value) { return value.isNumber() && rt::Number::isFinite(value.asNumber()) && value.asNumber() >= 0; }

  bool isPendingPaidFallbackLedgerRecord(const rt::KvValue& value) {
    return hasPaidFallbackLedgerIdentity(value) && (isStringValue(value["billing_status"], "pending") || isStringValue(value["billing_status"], "unresolved"));
  }

  std::string paidFallbackLedgerReference(const std::string& keyId, const std::string& requestId) {
    json::array reference;
    reference.emplace_back(json::string(keyId));
    reference.emplace_back(json::string(requestId));
    return rt::jsonStringify(reference);
  }

  std::optional<std::string> pendingPaidFallbackLedgerReferenceFromEntry(const rt::KvEntryMaybe<>& entry, const rt::KvKey& prefix) {
    if (entry.key.size() != prefix.size() + 3 || !isPendingPaidFallbackLedgerRecord(entry.value)) return std::nullopt;
    const rt::KvValue keyId = partAt(entry.key, prefix.size());
    const rt::KvValue createdAtMs = partAt(entry.key, prefix.size() + 1);
    const rt::KvValue requestId = partAt(entry.key, prefix.size() + 2);
    if (!partEquals(keyId, entry.value["key_id"]) || !partEquals(createdAtMs, entry.value["created_at_ms"]) || !partEquals(requestId, entry.value["id"])) {
      return std::nullopt;
    }
    return paidFallbackLedgerReference(entry.value["key_id"].asString(), entry.value["id"].asString());
  }

  bool paidFallbackLedgerEntryMatchesIdentity(const rt::KvEntryMaybe<>& entry, const rt::KvValue& expected) {
    const std::size_t offset = PAID_FALLBACK_LEDGER_PREFIX.size();
    if (entry.key.size() != offset + 3 || !hasPaidFallbackLedgerIdentity(entry.value)) return false;
    const rt::KvValue keyId = partAt(entry.key, offset);
    const rt::KvValue createdAtMs = partAt(entry.key, offset + 1);
    const rt::KvValue requestId = partAt(entry.key, offset + 2);
    return strictEquals(keyId, expected["key_id"]) && strictEquals(createdAtMs, expected["created_at_ms"]) && strictEquals(requestId, expected["id"]) &&
           strictEquals(entry.value["key_id"], expected["key_id"]) && strictEquals(entry.value["created_at_ms"], expected["created_at_ms"]) &&
           strictEquals(entry.value["id"], expected["id"]);
  }

  bool hasStrictApiKeyHashCorePolicy(const rt::KvValue& value) {
    if (!paid_fallback::index::hasStrictPaidFallbackPolicy(rt::storedJsonView(value))) return false;
    const rt::KvValue& record = value;
    return isApiKeyId(record["id"]) && isExpirationTimestamp(record["expires_at_ms"]) && isRevocationTimestamp(record["revoked_at_ms"]) &&
           isUsageLimit(record["usage_limit_requests"]) && isSafeUsageCount(record["usage_requests"]) && isPositiveSafeInteger(record["usage_reset_at_ms"]) &&
           isPositiveSafeInteger(record["window_ms"]);
  }

  bool hasStrictApiKeyCorePolicy(const rt::KvValue& value) {
    if (!paid_fallback::index::hasStrictPaidFallbackKeyPolicy(rt::storedJsonView(value))) return false;
    const rt::KvValue& record = value;
    return isApiKeyId(record["id"]) && isApiKeyHash(record["hash"]) && isRoutableApiKeyPrefix(record["prefix"]) && isExpirationTimestamp(record["expires_at_ms"]) &&
           isRevocationTimestamp(record["revoked_at_ms"]) && isUsageLimit(record["usage_limit_requests"]) && isSafeUsageCount(record["usage_requests"]) &&
           isPositiveSafeInteger(record["usage_reset_at_ms"]) && isPositiveSafeInteger(record["window_ms"]);
  }

  bool apiKeyHashPolicyMatches(const rt::KvValue& record, const rt::KvValue& hashRecord) {
    static constexpr std::array<std::string_view, 13> FIELDS{"id",
                                                             "expires_at_ms",
                                                             "revoked_at_ms",
                                                             "usage_limit_requests",
                                                             "usage_requests",
                                                             "usage_reset_at_ms",
                                                             "window_ms",
                                                             "usage_quota_version",
                                                             "paid_fallback_enabled",
                                                             "paid_fallback_limit_microcredits",
                                                             "paid_fallback_spent_microcredits",
                                                             "paid_fallback_reserved_microcredits",
                                                             "paid_fallback_reservation_request_id"};
    for (const std::string_view field : FIELDS)
      if (!strictEquals(record[field], hashRecord[field])) return false;
    return true;
  }

  bool isPaidFallbackRequestV3(const rt::KvValue& value) {
    if (!rt::kvIsObject(value)) return false;
    static constexpr std::array<std::string_view, 3> DISPATCH_STATES{"reserved", "dispatched", "not_dispatched"};
    static constexpr std::array<std::string_view, 6> TERMINAL_STATES{"pending", "completed", "failed", "incomplete", "cancelled", "ambiguous"};
    static constexpr std::array<std::string_view, 4> BILLING_STATES{"pending", "settled", "not_billed", "unresolved"};
    const rt::KvValue& provider = value["provider"];
    return isNumberValue(value["v"], 3) && isApiKeyId(value["key_id"]) && isApiKeyId(value["request_id"]) && isNonEmptyString(value["policy_version"]) &&
           isNonEmptyString(value["route"]) && isNonEmptyString(value["path"]) && isNonEmptyString(value["model"]) && value["stream"].isBool() &&
           isNullableNonEmptyString(value["reasoning"]) && isPositiveSafeInteger(value["window_reset_at_ms"]) && isSafeUsageCount(value["reserved_microcredits"]) &&
           isPositiveSafeInteger(value["quota_per_credit"]) && (isUndefinedValue(provider) || isStringValue(provider, "metered") || isStringValue(provider, "surplus")) &&
           isNullableNonEmptyString(value["provider_request_id"]) && (value["provider_quota"].isNull() || isFiniteNonNegativeNumber(value["provider_quota"])) &&
           (value["input_tokens"].isNull() || isSafeUsageCount(value["input_tokens"])) && (value["output_tokens"].isNull() || isSafeUsageCount(value["output_tokens"])) &&
           includesString(DISPATCH_STATES, value["dispatch_state"]) && includesString(TERMINAL_STATES, value["terminal_state"]) &&
           (value["spend_microcredits"].isNull() || isSafeUsageCount(value["spend_microcredits"])) && includesString(BILLING_STATES, value["billing_state"]) &&
           isSafeUsageCount(value["reconciliation_attempts"]) && isNullablePositiveSafeInteger(value["last_reconciliation_at_ms"]) &&
           isNullablePositiveSafeInteger(value["dispatched_at_ms"]) && isNullablePositiveSafeInteger(value["terminal_at_ms"]) &&
           isNullablePositiveSafeInteger(value["settled_at_ms"]) && isPositiveSafeInteger(value["created_at_ms"]) && isPositiveSafeInteger(value["updated_at_ms"]);
  }

  std::string paidFallbackV3WindowReference(const std::string& keyId, double windowResetAtMs) {
    json::array reference;
    reference.emplace_back(json::string(keyId));
    reference.push_back(rt::makeNumber(windowResetAtMs));
    return rt::jsonStringify(reference);
  }

  rt::Task<PaidFallbackV3Inventory> inspectPaidFallbackV3(std::shared_ptr<rt::Kv> kv, rt::OrderedSet<std::string> knownKeyIds, rt::OrderedSet<std::string> unlimitedKeyIds) {
    return inspectPaidFallbackV3(std::move(kv), std::move(knownKeyIds), std::move(unlimitedKeyIds), rt::dateNow());
  }

  rt::Task<PaidFallbackV3Inventory> inspectPaidFallbackV3(std::shared_ptr<rt::Kv> kv, rt::OrderedSet<std::string> knownKeyIds, rt::OrderedSet<std::string> unlimitedKeyIds,
                                                          double nowMs) {
    std::vector<std::string> errors;
    rt::Task<WindowInventory> collectingWindows = collectPaidFallbackWindows(kv, knownKeyIds, errors);
    const WindowInventory windowInventory = co_await std::move(collectingWindows);
    rt::Task<RequestInventory> collectingRequests = collectPaidFallbackRequests(kv, knownKeyIds, errors);
    const RequestInventory requestInventory = co_await std::move(collectingRequests);
    rt::Task<PendingInventory> collectingPending = collectPaidFallbackPending(kv, errors);
    const PendingInventory pendingInventory = co_await std::move(collectingPending);
    const double reconciliationLeases = co_await countPaidFallbackLeases(kv, knownKeyIds, errors);
    const double deletionGuards = co_await countPaidFallbackDeletionGuards(kv, errors);

    auditPaidFallbackRequests(requestInventory.requests, pendingInventory.pending, windowInventory.windows, unlimitedKeyIds, errors);
    auditPaidFallbackWindows(windowInventory.windows, requestInventory.requests, nowMs, errors);

    co_return PaidFallbackV3Inventory{.windows = windowInventory.count,
                                      .requests = requestInventory.count,
                                      .pending = pendingInventory.count,
                                      .reconciliationLeases = reconciliationLeases,
                                      .deletionGuards = deletionGuards,
                                      .errors = std::move(errors)};
  }
}
