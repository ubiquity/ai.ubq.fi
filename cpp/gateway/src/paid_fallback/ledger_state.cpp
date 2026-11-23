#include "uos/paid_fallback/ledger_state.hpp"

#include "uos/kv.hpp"

#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"

#include <cstddef>
#include <limits>
#include <utility>

namespace uos::paid_fallback::ledger_state {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;
    namespace contracts = authority::native_paid_contracts;
    namespace native_paid = authority::native_paid;

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

    // `queueOptions = { delay: 0, backoffSchedule: [...] } as const`
    KvEnqueueOptions queueOptions() {
      KvEnqueueOptions options;
      options.delay = 0;
      options.backoffSchedule = std::vector<double>{5'000, 30'000, 120'000, 600'000, 3'600'000, 21'600'000};
      return options;
    }

    json::object emptyPaidFallbackProviderUsage() {
      return {{"request_count", 0}, {"input_tokens", 0}, {"output_tokens", 0}, {"total_tokens", 0}, {"spend_microcredits", 0}};
    }

    json::value addProviderUsage(const json::value& current, const rt::KvValue& value) {
      const auto type = rt::kvTypeof(value);
      // Stored objects have no callable own methods. Their default primitive
      // value is the existing runtime's String(value), including Date and arrays.
      if (current.is_string() || type == "string" || (type == "object" && !value.isNull()))
        return json::value(rt::concat(rt::toString(current), rt::kvToString(value)));
      if (type == "bigint") throw rt::TypeError("Invalid mix of BigInt and other type in addition.");
      const double number = value.isNumber() ? value.asNumber() : value.isBool() ? (value.asBool() ? 1 : 0)
        : value.isNull() ? 0 : std::numeric_limits<double>::quiet_NaN();
      return rt::makeNumber(rt::toNumber(current) + number);
    }

  }

  rt::Task<std::shared_ptr<rt::Kv>> resolveKv(std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    if (kvOverride) co_return *kvOverride;
    const rt::Promise<std::shared_ptr<rt::Kv>> opening = kv::getKv();
    std::shared_ptr<rt::Kv> kv = co_await opening.wait();
    co_return kv;
  }

  rt::Task<PendingMarkerScan> scanPaidFallbackPendingMarkers(std::shared_ptr<rt::Kv> kv) {
    PendingMarkerScan scan;
    const rt::KvListSelector selector = prefixSelector(paidFallbackPendingV3GlobalPrefix);
    const rt::KvListOptions options = strongList();
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector, options);
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      rt::KvEntry<contracts::PaidFallbackPendingV3> marker = entry->as<contracts::PaidFallbackPendingV3>();
      const double dueAtMs = marker.value.next_reconciliation_at_ms;
      scan.entries.push_back(std::move(marker));
      if (!rt::Number::isSafeInteger(dueAtMs) || dueAtMs < 0) continue;
      if (!scan.earliest_due_at_ms || dueAtMs < *scan.earliest_due_at_ms) scan.earliest_due_at_ms = dueAtMs;
    }
    co_return scan;
  }

  rt::Task<std::optional<double>> recomputePaidFallbackReconciliationGateV3(std::shared_ptr<rt::Kv> kv) {
    const rt::KvKey gateKey = paidFallbackReconciliationGateV3Key();
    const rt::KvGetOptions strong = strongRead();
    for (double attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
      const rt::KvEntryMaybe<> gateEntry = co_await kv->get(gateKey, strong);
      rt::Task<PendingMarkerScan> scanning = scanPaidFallbackPendingMarkers(kv);
      const PendingMarkerScan scan = co_await std::move(scanning);
      // The plan reads only the gate row's revision, never its stored value.
      const contracts::NativePaidRow<json::value> gateRow{.value = std::nullopt, .versionstamp = gateEntry.versionstamp};
      const native_paid::PaidFallbackGateAfterScanPlanV3 plan = native_paid::planPaidFallbackGateAfterScanV3(gateRow, scan.earliest_due_at_ms);
      const std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->check(gateEntry).set(gateKey, rt::KvValue(json::value_from(plan.gate)));
      const rt::KvCommitResult commit = co_await operation->commit();
      if (commit.ok) co_return scan.earliest_due_at_ms;
    }
    throw rt::JsError("Paid fallback reconciliation gate changed concurrently.");
  }

  rt::Task<bool> enqueuePaidFallbackReconciliationJob(std::shared_ptr<rt::Kv> kv, std::string keyId) {
    rt::Task<bool> enqueuing = enqueuePaidFallbackReconciliationJob(std::move(kv), std::move(keyId), 0);
    const bool enqueued = co_await std::move(enqueuing);
    co_return enqueued;
  }

  rt::Task<bool> enqueuePaidFallbackReconciliationJob(std::shared_ptr<rt::Kv> kv, std::string keyId, double delay) {
    KvEnqueue* const enqueue = dynamic_cast<KvEnqueue*>(kv.get());
    if (enqueue == nullptr) co_return false;
    bool enqueued = true;
    try {
      rt::KvObject message;
      message.set("key_id", rt::KvValue(keyId));
      KvEnqueueOptions options = queueOptions();
      options.delay = delay;
      rt::Task<void> sending = enqueue->enqueue(rt::KvValue(std::move(message)), std::move(options));
      co_await std::move(sending);
    } catch (...) {
      // The pending marker and its backoff are durable. Cron can enqueue the key
      // again when a transient queue outage has recovered.
      enqueued = false;
    }
    co_return enqueued;
  }

  rt::Task<std::vector<types::PaidFallbackRequestV3>> listPaidFallbackRequestsV3(std::string keyId) {
    rt::Task<std::vector<types::PaidFallbackRequestV3>> listing = listPaidFallbackRequestsV3(std::move(keyId), 100, std::nullopt);
    std::vector<types::PaidFallbackRequestV3> requests = co_await std::move(listing);
    co_return requests;
  }

  rt::Task<std::vector<types::PaidFallbackRequestV3>> listPaidFallbackRequestsV3(std::string keyId, double limit) {
    rt::Task<std::vector<types::PaidFallbackRequestV3>> listing = listPaidFallbackRequestsV3(std::move(keyId), limit, std::nullopt);
    std::vector<types::PaidFallbackRequestV3> requests = co_await std::move(listing);
    co_return requests;
  }

  rt::Task<std::vector<types::PaidFallbackRequestV3>> listPaidFallbackRequestsV3(std::string keyId, double limit, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    rt::Task<std::shared_ptr<rt::Kv>> resolving = resolveKv(std::move(kvOverride));
    const std::shared_ptr<rt::Kv> kv = co_await std::move(resolving);
    std::vector<types::PaidFallbackRequestV3> requests;
    if (!kv || !rt::Number::isFinite(limit) || limit <= 0) co_return requests;
    const rt::KvListSelector selector = prefixSelector(paidFallbackRequestV3Prefix(keyId));
    const rt::KvListOptions options = strongList();
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector, options);
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      requests.push_back(rt::kvValueAs<types::PaidFallbackRequestV3>(entry->value));
    }
    rt::sort(requests, [](const types::PaidFallbackRequestV3& left, const types::PaidFallbackRequestV3& right) -> double {
      const double byCreation = right.created_at_ms - left.created_at_ms;
      return rt::truthy(byCreation) ? byCreation : static_cast<double>(rt::localeCompare(right.request_id, left.request_id));
    });
    const double count = rt::Math::min(1'000.0, rt::Math::trunc(limit));
    if (static_cast<double>(requests.size()) > count) requests.erase(requests.begin() + static_cast<std::ptrdiff_t>(count), requests.end());
    co_return requests;
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackProviderUsageByProviderV3& value) {
    out = Writer().required("metered", value.metered).required("surplus", value.surplus).done();
  }

  PaidFallbackProviderUsageByProviderV3 tag_invoke(const json::value_to_tag<PaidFallbackProviderUsageByProviderV3>&, const json::value& value) {
    const Reader in(value, "PaidFallbackProviderUsageByProviderV3");
    return {
      .metered = in.required<json::object>("metered"),
      .surplus = in.required<json::object>("surplus"),
    };
  }

  rt::Task<PaidFallbackProviderUsageByProviderV3> getPaidFallbackProviderUsageV3(std::string keyId, double windowResetAtMs) {
    rt::Task<PaidFallbackProviderUsageByProviderV3> reading = getPaidFallbackProviderUsageV3(std::move(keyId), windowResetAtMs, std::nullopt);
    PaidFallbackProviderUsageByProviderV3 usage = co_await std::move(reading);
    co_return usage;
  }

  rt::Task<PaidFallbackProviderUsageByProviderV3> getPaidFallbackProviderUsageV3(std::string keyId, double windowResetAtMs,
                                                                                 std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    rt::Task<std::shared_ptr<rt::Kv>> resolving = resolveKv(std::move(kvOverride));
    const std::shared_ptr<rt::Kv> kv = co_await std::move(resolving);
    PaidFallbackProviderUsageByProviderV3 usage{
      .metered = emptyPaidFallbackProviderUsage(),
      .surplus = emptyPaidFallbackProviderUsage(),
    };
    if (!kv) co_return usage;
    // Request keys do not include the window reset, so scan the complete key
    // history and filter by the requested window instead of truncating at the
    // newest 1,000 rows.
    const rt::KvListSelector selector = prefixSelector(paidFallbackRequestV3Prefix(keyId));
    const rt::KvListOptions options = strongList();
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector, options);
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      const auto& request = entry->value;
      if (request.isNull()) throw rt::TypeError("null is not an object (evaluating 'request.window_reset_at_ms')");
      if (request.isUndefined()) throw rt::TypeError("undefined is not an object (evaluating 'request.window_reset_at_ms')");
      const auto& window = request["window_reset_at_ms"];
      if (!window.isNumber() || window.asNumber() != windowResetAtMs) continue;
      const auto& provider = request["provider"];
      json::object& current = provider.isString() && provider.asString() == "surplus" ? usage.surplus : usage.metered;
      const rt::KvValue zero(0);
      const auto& input = request["input_tokens"];
      const auto& output = request["output_tokens"];
      const auto& inputTokens = input.isNullish() ? zero : input;
      const auto& outputTokens = output.isNullish() ? zero : output;
      const auto requestCount = rt::makeNumber(rt::toNumber(current.at("request_count")) + 1);
      const auto inputCount = addProviderUsage(current.at("input_tokens"), inputTokens);
      const auto outputCount = addProviderUsage(current.at("output_tokens"), outputTokens);
      const auto totalWithInput = addProviderUsage(current.at("total_tokens"), inputTokens);
      const auto totalCount = addProviderUsage(totalWithInput, outputTokens);
      const auto& billingState = request["billing_state"];
      const auto& spend = billingState.isString() && billingState.asString() == "settled" ? request["spend_microcredits"] : zero;
      const auto spendCount = addProviderUsage(current.at("spend_microcredits"), spend.isNullish() ? zero : spend);
      current = {{"request_count", requestCount}, {"input_tokens", inputCount}, {"output_tokens", outputCount},
        {"total_tokens", totalCount}, {"spend_microcredits", spendCount}};
    }
    co_return usage;
  }

  rt::Task<std::optional<PaidFallbackWindowProjectionV3>> getPaidFallbackWindowProjectionV3(std::string keyId, double windowResetAtMs, double limitMicrocredits) {
    rt::Task<std::optional<PaidFallbackWindowProjectionV3>> reading =
      getPaidFallbackWindowProjectionV3(std::move(keyId), windowResetAtMs, limitMicrocredits, std::nullopt);
    std::optional<PaidFallbackWindowProjectionV3> projection = co_await std::move(reading);
    co_return projection;
  }

  rt::Task<std::optional<PaidFallbackWindowProjectionV3>> getPaidFallbackWindowProjectionV3(std::string keyId, double windowResetAtMs, double limitMicrocredits,
                                                                                            std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    rt::Task<std::shared_ptr<rt::Kv>> resolving = resolveKv(std::move(kvOverride));
    const std::shared_ptr<rt::Kv> kv = co_await std::move(resolving);
    if (!kv) co_return std::nullopt;
    const rt::KvKey windowKey = paidFallbackWindowV3Key(keyId, windowResetAtMs);
    const rt::KvGetOptions strong = strongRead();
    const rt::KvEntryMaybe<> window = co_await kv->get(windowKey, strong);
    if (window.value.truthy()) {
      const types::PaidFallbackWindowV3 stored = rt::kvValueAs<types::PaidFallbackWindowV3>(window.value);
      co_return PaidFallbackWindowProjectionV3{
        .key_id = keyId,
        .policy_version = stored.policy_version,
        .window_reset_at_ms = windowResetAtMs,
        .limit_microcredits = limitMicrocredits,
        .settled_microcredits = stored.settled_microcredits,
        .reserved_microcredits = stored.reserved_microcredits,
        .pending_count = stored.pending_count,
        .updated_at_ms = stored.updated_at_ms,
      };
    }

    std::optional<std::string> policyVersion;
    double settledMicrocredits = 0;
    double reservedMicrocredits = 0;
    double pendingCount = 0;
    std::optional<double> updatedAtMs;
    const rt::KvListSelector selector = prefixSelector(paidFallbackRequestV3Prefix(keyId));
    const rt::KvListOptions options = strongList();
    const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector, options);
    while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
      const types::PaidFallbackRequestV3 request = rt::kvValueAs<types::PaidFallbackRequestV3>(entry->value);
      if (request.window_reset_at_ms != windowResetAtMs) continue;
      if (request.billing_state == "settled") {
        settledMicrocredits += request.spend_microcredits.value_or(0);
      } else if (request.billing_state == "pending" || request.billing_state == "unresolved") {
        reservedMicrocredits += request.reserved_microcredits;
        pendingCount += 1;
      }
      if (!updatedAtMs || request.updated_at_ms > *updatedAtMs) {
        updatedAtMs = request.updated_at_ms;
        policyVersion = request.policy_version;
      }
    }
    co_return PaidFallbackWindowProjectionV3{
      .key_id = keyId,
      .policy_version = policyVersion,
      .window_reset_at_ms = windowResetAtMs,
      .limit_microcredits = limitMicrocredits,
      .settled_microcredits = settledMicrocredits,
      .reserved_microcredits = reservedMicrocredits,
      .pending_count = pendingCount,
      .updated_at_ms = updatedAtMs,
    };
  }

  rt::Task<std::optional<PaidFallbackOutstandingV3>> getPaidFallbackOutstandingV3(std::string keyId) {
    rt::Task<std::optional<PaidFallbackOutstandingV3>> reading = getPaidFallbackOutstandingV3(std::move(keyId), std::nullopt);
    std::optional<PaidFallbackOutstandingV3> outstanding = co_await std::move(reading);
    co_return outstanding;
  }

  rt::Task<std::optional<PaidFallbackOutstandingV3>> getPaidFallbackOutstandingV3(std::string keyId, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    rt::Task<std::shared_ptr<rt::Kv>> resolving = resolveKv(std::move(kvOverride));
    const std::shared_ptr<rt::Kv> kv = co_await std::move(resolving);
    if (!kv) co_return std::nullopt;
    double pendingRequests = 0;
    double unresolvedRequests = 0;
    const rt::KvListOptions options = strongList();
    const rt::KvListSelector requestSelector = prefixSelector(paidFallbackRequestV3Prefix(keyId));
    const std::unique_ptr<rt::KvListIterator> requests = kv->list(requestSelector, options);
    while (std::optional<rt::KvEntry<>> entry = co_await requests->next()) {
      // `entry.value.billing_state` on the stored value as it is.
      if (entry->value.isNull()) throw rt::TypeError("null is not an object (evaluating 'entry.value.billing_state')");
      if (entry->value.isUndefined()) throw rt::TypeError("undefined is not an object (evaluating 'entry.value.billing_state')");
      const rt::KvValue& billingState = entry->value["billing_state"];
      if (billingState.isString() && billingState.asString() == "pending") pendingRequests += 1;
      if (billingState.isString() && billingState.asString() == "unresolved") unresolvedRequests += 1;
    }
    std::vector<rt::KvKey> pendingMarkerKeys;
    const rt::KvListSelector pendingSelector = prefixSelector(paidFallbackPendingV3Prefix(keyId));
    const std::unique_ptr<rt::KvListIterator> pending = kv->list(pendingSelector, options);
    while (std::optional<rt::KvEntry<>> entry = co_await pending->next()) {
      pendingMarkerKeys.push_back(entry->key);
    }
    const double pendingMarkers = static_cast<double>(pendingMarkerKeys.size());
    co_return PaidFallbackOutstandingV3{
      .pending_requests = pendingRequests,
      .unresolved_requests = unresolvedRequests,
      .pending_markers = pendingMarkers,
      .has_outstanding = pendingRequests > 0 || unresolvedRequests > 0 || pendingMarkers > 0,
    };
  }
}
