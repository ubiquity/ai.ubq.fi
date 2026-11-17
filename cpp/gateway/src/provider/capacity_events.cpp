#include "uos/provider/capacity_events.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/codex/banked_reset.hpp"
#include "uos/kv.hpp"
#include "uos/provider/capacity.hpp"
#include "uos/provider/capacity_contract.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"

#include <utility>

// The stored events' tag_invoke pairs (declared in the header) are defined in capacity_contract.cpp, which this module
// links in any case: that keeps the contract module a link leaf although ProviderCapacityView embeds the events.

namespace uos::provider::capacity_events {
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
    // `isRecord` (src/utils.ts: `typeof value === "object" && value !== null`, arrays included) over a stored value;
    // members of a record that is not a plain object read as undefined.

    bool isNumberLiteral(const rt::KvValue& value, double literal) { return value.isNumber() && value.asNumber() == literal; }
    bool isStringLiteral(const rt::KvValue& value, std::string_view literal) { return value.isString() && value.asString() == literal; }

    bool isSafeTimestamp(double value) { return rt::Number::isSafeInteger(value) && value >= 0; }
    bool isSafeTimestamp(const rt::KvValue& value) { return value.isNumber() && isSafeTimestamp(value.asNumber()); }

    bool isNonEmptyText(const rt::KvValue& value) {
      if (!value.isString()) return false;
      const std::int64_t length = rt::length(value.asString());
      return length > 0 && length <= 512;
    }

    bool isPercent(const rt::KvValue& value) {
      return value.isNumber() && rt::Number::isFinite(value.asNumber()) && value.asNumber() >= 0 && value.asNumber() <= 100;
    }

    // A stored value as the JSON the banked-reset parsers read: an undefined member is absent (as `=== undefined` and
    // `typeof` see it), an undefined element or hole is null, and any other non-JSON part (Date, Map, KvU64, bytes,
    // bigint, ...) an empty object, which every one of their guards rejects as it rejects the original.

    // `kvOverride: Kv | null | undefined`
    rt::Task<std::shared_ptr<rt::Kv>> resolveKv(std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
      if (kvOverride) co_return *kvOverride;
      std::shared_ptr<rt::Kv> kv;
      try {
        auto opening = kv::getKv();
        kv = co_await opening.wait();
      } catch (...) {
        kv = nullptr;
      }
      co_return kv;
    }

    // `Math.trunc(options.now?.() ?? Date.now())`, kept only when it is a safe non-negative integer.
    double listingNow(const std::function<double()>& now) {
      const double rawNow = rt::Math::trunc(now ? now() : rt::dateNow());
      return rt::Number::isSafeInteger(rawNow) && rawNow >= 0 ? rawNow : rt::dateNow();
    }

    template <class Event> void sortByObservation(std::vector<Event>& events) {
      rt::sort(events, [](const Event& left, const Event& right) {
        const double order = left.observed_at_ms - right.observed_at_ms;
        return rt::truthy(order) ? order : static_cast<double>(rt::localeCompare(left.event_id, right.event_id));
      });
    }

    rt::Task<bool> writeResetEvent(std::shared_ptr<rt::Kv> kv, ProviderCapacityResetEvent event) {
      const rt::KvKey key = providerCapacityResetEventKey(event.event_id);
      for (int attempt = 0; attempt < 4; attempt += 1) {
        rt::KvEntryMaybe<> entry;
        bool readFailed = false;
        try {
          const rt::KvGetOptions options{.consistency = rt::KvConsistencyLevel::strong};
          auto reading = kv->get(key, options);
          entry = co_await std::move(reading);
        } catch (...) {
          readFailed = true;
        }
        if (readFailed) co_return false;
        const std::optional<ProviderCapacityResetEvent> existing = entry.value.isNull() ? std::nullopt : parseProviderCapacityResetEvent(entry.value);
        if (existing) co_return existing->slot == event.slot && existing->observed_at_ms == event.observed_at_ms;
        bool commitFailed = false;
        bool committedOk = false;
        try {
          std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
          const rt::KvSetOptions options{.expireIn = PROVIDER_CAPACITY_RESET_EVENT_RETENTION_MS};
          operation->check(entry).set(key, rt::KvValue(json::value_from(event)), options);
          auto committing = operation->commit();
          const rt::KvCommitResult committed = co_await std::move(committing);
          committedOk = committed.ok;
        } catch (...) {
          commitFailed = true;
        }
        if (commitFailed) co_return false;
        if (committedOk) co_return true;
      }
      co_return false;
    }

    rt::Task<bool> writeDowntimeEvent(std::shared_ptr<rt::Kv> kv, ProviderCapacityDowntimeEvent event) {
      const rt::KvKey key = providerCapacityDowntimeEventKey(event.event_id);
      for (int attempt = 0; attempt < 4; attempt += 1) {
        rt::KvEntryMaybe<> entry;
        bool readFailed = false;
        try {
          const rt::KvGetOptions options{.consistency = rt::KvConsistencyLevel::strong};
          auto reading = kv->get(key, options);
          entry = co_await std::move(reading);
        } catch (...) {
          readFailed = true;
        }
        if (readFailed) co_return false;
        if (!entry.value.isNull()) co_return parseProviderCapacityDowntimeEvent(entry.value).has_value();
        bool commitFailed = false;
        bool committedOk = false;
        try {
          std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
          const rt::KvSetOptions options{.expireIn = PROVIDER_CAPACITY_DOWNTIME_EVENT_RETENTION_MS};
          operation->check(entry).set(key, rt::KvValue(json::value_from(event)), options);
          auto committing = operation->commit();
          const rt::KvCommitResult committed = co_await std::move(committing);
          committedOk = committed.ok;
        } catch (...) {
          commitFailed = true;
        }
        if (commitFailed) co_return false;
        if (committedOk) co_return true;
      }
      co_return false;
    }

    double downtimeBucketStartAtMs(double observedAtMs) { return rt::Math::floor(observedAtMs / (15 * 60'000)) * 15 * 60'000; }

    struct ResetShadowFence {
      double slot;
      std::string account_id_hash;
      std::string quota_generation;
    };

    rt::Task<std::vector<ProviderCapacityResetEvent>> readBackfillableEvents(std::shared_ptr<rt::Kv> kv, double cutoffMs, double nowMs) {
      std::vector<ResetShadowFence> decisions;
      std::vector<ProviderCapacityResetEvent> redemptions;
      bool failed = false;
      try {
        const rt::KvListSelector decisionSelector{.prefix = codex::banked_reset::CODEX_RESET_SHADOW_DECISION_KV_PREFIX};
        std::unique_ptr<rt::KvListIterator> decisionRows = kv->list(decisionSelector);
        while (true) {
          std::optional<rt::KvEntry<>> entry = co_await decisionRows->next();
          if (!entry) break;
          const std::optional<::uos::types::CodexResetShadowDecisionRecord> decision =
            codex::banked_reset::parseCodexResetShadowDecisionRecord(rt::storedJsonView(entry->value, STORED_JSON_VIEW_OPTIONS));
          if (!decision) continue;
          for (const auto& fence : decision->fences) {
            if (fence.slot == 1 || fence.slot == 2) {
              decisions.push_back(ResetShadowFence{.slot = fence.slot, .account_id_hash = fence.account_id_hash, .quota_generation = fence.quota_generation});
            }
          }
        }
        const rt::KvListSelector redemptionSelector{.prefix = codex::banked_reset::CODEX_RESET_REDEMPTION_KV_PREFIX};
        std::unique_ptr<rt::KvListIterator> redemptionRows = kv->list(redemptionSelector);
        while (true) {
          std::optional<rt::KvEntry<>> entry = co_await redemptionRows->next();
          if (!entry) break;
          const std::optional<::uos::types::CodexResetRedemptionRecord> record = codex::banked_reset::parseCodexResetRedemptionRecord(rt::storedJsonView(entry->value, STORED_JSON_VIEW_OPTIONS));
          if (!record || record->state != ::uos::types::CodexResetRedemptionState::verified || !record->verified_at_ms || *record->verified_at_ms < cutoffMs ||
              *record->verified_at_ms > nowMs) {
            continue;
          }
          const ResetShadowFence* fence = nullptr;
          for (const ResetShadowFence& candidate : decisions) {
            if (candidate.account_id_hash == record->account_id_hash && candidate.quota_generation == record->quota_generation) {
              fence = &candidate;
              break;
            }
          }
          if (!fence || (fence->slot != 1 && fence->slot != 2)) continue;
          redemptions.push_back(ProviderCapacityResetEvent{
            .v = 1,
            .event_id = record->idempotency_key_hash,
            .slot = fence->slot,
            .observed_at_ms = *record->verified_at_ms,
          });
        }
      } catch (...) {
        failed = true;
      }
      if (failed) co_return std::vector<ProviderCapacityResetEvent>{};
      co_return redemptions;
    }

    // Debounce state of the default trigger (one probe per history bucket).
    double lastSampledBucketStartAtMs = 0;

    void defaultProviderCapacitySampleTrigger(const ProviderCapacitySampleTriggerOptions& options) {
      const double nowMs = options.now ? options.now() : rt::dateNow();
      const double bucketStartAtMs =
        rt::Math::floor(nowMs / capacity_contract::PROVIDER_CAPACITY_HISTORY_BUCKET_MS) * capacity_contract::PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
      if (bucketStartAtMs <= lastSampledBucketStartAtMs) return;
      lastSampledBucketStartAtMs = bucketStartAtMs;
      // `void import("./capacity.ts").then((sampler) => sampler.sampleProviderCapacityOnEvent({ now: () => nowMs })).catch(() => {})`:
      // the sampler is linked in, so the import is a promise already resolved.
      const rt::Promise<void> imported = rt::Promise<void>::resolve();
      (void)imported
        .then([nowMs]() {
          capacity_contract::ProviderCapacitySnapshotOptions sampleOptions;
          sampleOptions.now = [nowMs] { return nowMs; };
          return capacity::sampleProviderCapacityOnEvent(std::move(sampleOptions));
        })
        .catch_([](const rt::Reason&) {});
    }

    ProviderCapacitySampleTrigger& providerCapacitySampleTrigger() {
      static ProviderCapacitySampleTrigger trigger = defaultProviderCapacitySampleTrigger;
      return trigger;
    }
  }

  rt::KvKey providerCapacityResetEventKey(const std::string& eventId) { return rt::KvKey{PROVIDER_CAPACITY_RESET_EVENT_KV_PREFIX, eventId}; }

  rt::KvKey providerCapacityRateLimitResetEventKey(const std::string& eventId) { return rt::KvKey{PROVIDER_CAPACITY_RATE_LIMIT_RESET_EVENT_KV_PREFIX, eventId}; }

  rt::KvKey providerCapacityDowntimeEventKey(const std::string& eventId) { return rt::KvKey{PROVIDER_CAPACITY_DOWNTIME_EVENT_KV_PREFIX, eventId}; }

  std::optional<ProviderCapacityResetEvent> parseProviderCapacityResetEvent(const rt::KvValue& value) {
    if (!rt::kvIsObject(value) || !isNumberLiteral(value["v"], 1) || !isNonEmptyText(value["event_id"])) return std::nullopt;
    if ((!isNumberLiteral(value["slot"], 1) && !isNumberLiteral(value["slot"], 2)) || !isSafeTimestamp(value["observed_at_ms"])) return std::nullopt;
    return ProviderCapacityResetEvent{
      .v = 1,
      .event_id = value["event_id"].asString(),
      .slot = value["slot"].asNumber(),
      .observed_at_ms = value["observed_at_ms"].asNumber(),
    };
  }

  std::optional<ProviderCapacityRateLimitResetEvent> parseProviderCapacityRateLimitResetEvent(const rt::KvValue& value) {
    if (!rt::kvIsObject(value) || !isNumberLiteral(value["v"], 1) || !isNonEmptyText(value["event_id"]) || !isStringLiteral(value["provider"], "openai") ||
        (!isNumberLiteral(value["slot"], 1) && !isNumberLiteral(value["slot"], 2)) ||
        (!isStringLiteral(value["window"], "primary") && !isStringLiteral(value["window"], "secondary")) || !isSafeTimestamp(value["observed_at_ms"]) ||
        !isSafeTimestamp(value["previous_sampled_at_ms"]) || !isSafeTimestamp(value["previous_reset_at_ms"]) || !isSafeTimestamp(value["reset_at_ms"]) ||
        !isPercent(value["previous_used_percent"]) || !isPercent(value["current_used_percent"]) || !isPercent(value["capacity_gain_percentage_points"]))
      return std::nullopt;
    const double observedAtMs = value["observed_at_ms"].asNumber();
    const double previousSampledAtMs = value["previous_sampled_at_ms"].asNumber();
    const double previousResetAtMs = value["previous_reset_at_ms"].asNumber();
    const double resetAtMs = value["reset_at_ms"].asNumber();
    const double previousUsedPercent = value["previous_used_percent"].asNumber();
    const double currentUsedPercent = value["current_used_percent"].asNumber();
    const double capacityGain = value["capacity_gain_percentage_points"].asNumber();
    if (previousSampledAtMs >= observedAtMs || previousResetAtMs >= resetAtMs || rt::Math::abs(previousUsedPercent - currentUsedPercent - capacityGain) > 0.001)
      return std::nullopt;
    return ProviderCapacityRateLimitResetEvent{
      .v = 1,
      .event_id = value["event_id"].asString(),
      .provider = "openai",
      .slot = value["slot"].asNumber(),
      .window = value["window"].asString(),
      .observed_at_ms = observedAtMs,
      .previous_sampled_at_ms = previousSampledAtMs,
      .previous_reset_at_ms = previousResetAtMs,
      .reset_at_ms = resetAtMs,
      .previous_used_percent = previousUsedPercent,
      .current_used_percent = currentUsedPercent,
      .capacity_gain_percentage_points = capacityGain,
    };
  }

  std::optional<ProviderCapacityDowntimeEvent> parseProviderCapacityDowntimeEvent(const rt::KvValue& value) {
    if (!rt::kvIsObject(value) || !isNumberLiteral(value["v"], 1) || !isNonEmptyText(value["event_id"]) || !isStringLiteral(value["provider"], "openai")) return std::nullopt;
    if (!isStringLiteral(value["failure_kind"], "upstream_error") && !isStringLiteral(value["failure_kind"], "unreachable")) return std::nullopt;
    const rt::KvValue& status = value["status"];
    if (!(status.isNull() || (status.isNumber() && rt::Number::isSafeInteger(status.asNumber()) && status.asNumber() >= 500 && status.asNumber() <= 599)))
      return std::nullopt;
    if (!isSafeTimestamp(value["observed_at_ms"])) return std::nullopt;
    return ProviderCapacityDowntimeEvent{
      .v = 1,
      .event_id = value["event_id"].asString(),
      .provider = "openai",
      .failure_kind = value["failure_kind"].asString(),
      .status = status.isNull() ? std::nullopt : std::optional<double>(status.asNumber()),
      .observed_at_ms = value["observed_at_ms"].asNumber(),
    };
  }

  rt::Task<bool> recordProviderCapacityResetEvent(ProviderCapacityResetEvent event) {
    std::optional<std::shared_ptr<rt::Kv>> kvOverride;
    auto recording = recordProviderCapacityResetEvent(std::move(event), std::move(kvOverride));
    const bool recorded = co_await std::move(recording);
    co_return recorded;
  }

  rt::Task<bool> recordProviderCapacityResetEvent(ProviderCapacityResetEvent event, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    rt::Promise<std::shared_ptr<rt::Kv>> resolving(resolveKv(std::move(kvOverride)));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) co_return false;
    rt::Promise<bool> writing(writeResetEvent(kv, std::move(event)));
    const bool written = co_await writing.wait();
    co_return written;
  }

  rt::Task<std::vector<ProviderCapacityRateLimitResetEvent>> listProviderCapacityRateLimitResetEvents() {
    ListProviderCapacityEventsOptions options;
    auto listing = listProviderCapacityRateLimitResetEvents(std::move(options));
    std::vector<ProviderCapacityRateLimitResetEvent> events = co_await std::move(listing);
    co_return events;
  }

  rt::Task<std::vector<ProviderCapacityRateLimitResetEvent>> listProviderCapacityRateLimitResetEvents(ListProviderCapacityEventsOptions options) {
    rt::Promise<std::shared_ptr<rt::Kv>> resolving(resolveKv(options.kv));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) co_return std::vector<ProviderCapacityRateLimitResetEvent>{};
    const double nowMs = listingNow(options.now);
    const double cutoffMs = rt::Math::max(0.0, nowMs - PROVIDER_CAPACITY_RESET_EVENT_RETENTION_MS);
    std::vector<ProviderCapacityRateLimitResetEvent> events;
    bool failed = false;
    try {
      const rt::KvListSelector selector{.prefix = PROVIDER_CAPACITY_RATE_LIMIT_RESET_EVENT_KV_PREFIX};
      std::unique_ptr<rt::KvListIterator> rows = kv->list(selector);
      while (true) {
        std::optional<rt::KvEntry<>> entry = co_await rows->next();
        if (!entry) break;
        std::optional<ProviderCapacityRateLimitResetEvent> event = parseProviderCapacityRateLimitResetEvent(entry->value);
        if (event && event->observed_at_ms >= cutoffMs && event->observed_at_ms <= nowMs) events.push_back(std::move(*event));
      }
    } catch (...) {
      failed = true;
    }
    if (failed) co_return std::vector<ProviderCapacityRateLimitResetEvent>{};
    sortByObservation(events);
    co_return events;
  }

  rt::Task<bool> recordProviderCapacityDowntimeEvent(ProviderCapacityDowntimeEventInput input) {
    std::optional<std::shared_ptr<rt::Kv>> kvOverride;
    auto recording = recordProviderCapacityDowntimeEvent(std::move(input), std::move(kvOverride));
    const bool recorded = co_await std::move(recording);
    co_return recorded;
  }

  rt::Task<bool> recordProviderCapacityDowntimeEvent(ProviderCapacityDowntimeEventInput input, std::optional<std::shared_ptr<rt::Kv>> kvOverride) {
    if (!isSafeTimestamp(input.observed_at_ms)) co_return false;
    if (input.status && (!rt::Number::isSafeInteger(*input.status) || *input.status < 500 || *input.status > 599)) co_return false;
    ProviderCapacityDowntimeEvent event{
      .v = 1,
      .event_id = "openai-" + rt::numberToString(downtimeBucketStartAtMs(input.observed_at_ms)),
      .provider = "openai",
      .failure_kind = input.failure_kind,
      .status = input.status,
      .observed_at_ms = input.observed_at_ms,
    };
    rt::Promise<std::shared_ptr<rt::Kv>> resolving(resolveKv(std::move(kvOverride)));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) co_return false;
    rt::Promise<bool> writing(writeDowntimeEvent(kv, std::move(event)));
    const bool written = co_await writing.wait();
    co_return written;
  }

  rt::Task<std::vector<ProviderCapacityDowntimeEvent>> listProviderCapacityDowntimeEvents() {
    ListProviderCapacityEventsOptions options;
    auto listing = listProviderCapacityDowntimeEvents(std::move(options));
    std::vector<ProviderCapacityDowntimeEvent> events = co_await std::move(listing);
    co_return events;
  }

  rt::Task<std::vector<ProviderCapacityDowntimeEvent>> listProviderCapacityDowntimeEvents(ListProviderCapacityEventsOptions options) {
    rt::Promise<std::shared_ptr<rt::Kv>> resolving(resolveKv(options.kv));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) co_return std::vector<ProviderCapacityDowntimeEvent>{};
    const double nowMs = listingNow(options.now);
    const double cutoffMs = rt::Math::max(0.0, nowMs - PROVIDER_CAPACITY_DOWNTIME_EVENT_RETENTION_MS);
    std::vector<ProviderCapacityDowntimeEvent> events;
    bool failed = false;
    try {
      const rt::KvListSelector selector{.prefix = PROVIDER_CAPACITY_DOWNTIME_EVENT_KV_PREFIX};
      std::unique_ptr<rt::KvListIterator> rows = kv->list(selector);
      while (true) {
        std::optional<rt::KvEntry<>> entry = co_await rows->next();
        if (!entry) break;
        std::optional<ProviderCapacityDowntimeEvent> event = parseProviderCapacityDowntimeEvent(entry->value);
        if (event && event->observed_at_ms >= cutoffMs && event->observed_at_ms <= nowMs) events.push_back(std::move(*event));
      }
    } catch (...) {
      failed = true;
    }
    if (failed) co_return std::vector<ProviderCapacityDowntimeEvent>{};
    sortByObservation(events);
    co_return events;
  }

  rt::Task<std::vector<ProviderCapacityResetEvent>> listProviderCapacityResetEvents() {
    ListProviderCapacityEventsOptions options;
    auto listing = listProviderCapacityResetEvents(std::move(options));
    std::vector<ProviderCapacityResetEvent> events = co_await std::move(listing);
    co_return events;
  }

  rt::Task<std::vector<ProviderCapacityResetEvent>> listProviderCapacityResetEvents(ListProviderCapacityEventsOptions options) {
    rt::Promise<std::shared_ptr<rt::Kv>> resolving(resolveKv(options.kv));
    const std::shared_ptr<rt::Kv> kv = co_await resolving.wait();
    if (!kv) co_return std::vector<ProviderCapacityResetEvent>{};
    const double nowMs = listingNow(options.now);
    const double cutoffMs = rt::Math::max(0.0, nowMs - PROVIDER_CAPACITY_RESET_EVENT_RETENTION_MS);
    rt::OrderedMap<std::string, ProviderCapacityResetEvent> events;
    bool failed = false;
    try {
      const rt::KvListSelector selector{.prefix = PROVIDER_CAPACITY_RESET_EVENT_KV_PREFIX};
      std::unique_ptr<rt::KvListIterator> rows = kv->list(selector);
      while (true) {
        std::optional<rt::KvEntry<>> entry = co_await rows->next();
        if (!entry) break;
        std::optional<ProviderCapacityResetEvent> event = parseProviderCapacityResetEvent(entry->value);
        if (event && event->observed_at_ms >= cutoffMs && event->observed_at_ms <= nowMs) {
          const std::string eventId = event->event_id;
          events.set(eventId, std::move(*event));
        }
      }
    } catch (...) {
      failed = true;
    }
    if (failed) co_return std::vector<ProviderCapacityResetEvent>{};

    rt::Promise<std::vector<ProviderCapacityResetEvent>> backfilling(readBackfillableEvents(kv, cutoffMs, nowMs));
    const std::vector<ProviderCapacityResetEvent> backfillable = co_await backfilling.wait();
    for (const ProviderCapacityResetEvent& event : backfillable) {
      if (!events.has(event.event_id)) {
        rt::Promise<bool> writing(writeResetEvent(kv, event));
        co_await writing.wait();
        events.set(event.event_id, event);
      }
    }

    std::vector<ProviderCapacityResetEvent> sorted = rt::arrayFrom(events.values());
    sortByObservation(sorted);
    co_return sorted;
  }

  void setProviderCapacitySampleTriggerForTest(ProviderCapacitySampleTrigger trigger) {
    providerCapacitySampleTrigger() = trigger ? std::move(trigger) : ProviderCapacitySampleTrigger(defaultProviderCapacitySampleTrigger);
  }

  void triggerProviderCapacitySample() { triggerProviderCapacitySample(ProviderCapacitySampleTriggerOptions{}); }

  void triggerProviderCapacitySample(const ProviderCapacitySampleTriggerOptions& options) {
    // Copy the slot first: a trigger may replace itself.
    const ProviderCapacitySampleTrigger trigger = providerCapacitySampleTrigger();
    trigger(options);
  }
}
