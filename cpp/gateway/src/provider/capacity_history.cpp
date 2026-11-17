#include "uos/provider/capacity_history.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/provider/capacity_parse.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"

#include <utility>

namespace uos::provider::capacity_history {
  using capacity_contract::ProviderCapacityCodexSource;
  using capacity_contract::ProviderCapacityHistoryPoint;
  using capacity_contract::ProviderCapacityMeteredSource;
  using capacity_contract::ProviderCapacitySnapshot;
  using capacity_contract::ProviderCapacitySource;
  using capacity_contract::ProviderCapacityWindow;
  using capacity_contract::StoredRateLimitObservation;
  using capacity_events::ProviderCapacityRateLimitResetEvent;

  namespace {
    // `isRecord` (src/utils.ts: `typeof value === "object" && value !== null`, arrays included) over a stored value.

    double historyBucketStartAtMs(double snapshotAtMs) {
      return rt::Math::floor(snapshotAtMs / capacity_contract::PROVIDER_CAPACITY_HISTORY_BUCKET_MS) * capacity_contract::PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
    }

    // `left.bucket_start_at_ms - right.bucket_start_at_ms || left.sampled_at_ms - right.sampled_at_ms`
    double byBucketThenSample(const ProviderCapacityHistoryPoint& left, const ProviderCapacityHistoryPoint& right) {
      const double bucket = left.bucket_start_at_ms - right.bucket_start_at_ms;
      return rt::truthy(bucket) ? bucket : left.sampled_at_ms - right.sampled_at_ms;
    }

    // `snapshot.sources.find((source) => source.source === "codex" && source.slot === slot)`; nullptr is undefined.
    const ProviderCapacityCodexSource* findCodexSource(const std::array<ProviderCapacitySource, 3>& sources, double slot) {
      for (const ProviderCapacitySource& source : sources) {
        const ProviderCapacityCodexSource* codex = std::get_if<ProviderCapacityCodexSource>(&source);
        if (codex && codex->slot == slot) return codex;
      }
      return nullptr;
    }

    // The same over a history point's `[codex, codex, metered]` tuple.
    const ProviderCapacityCodexSource* findCodexSource(const ProviderCapacityHistoryPoint& point, double slot) {
      if (std::get<0>(point.sources).slot == slot) return &std::get<0>(point.sources);
      if (std::get<1>(point.sources).slot == slot) return &std::get<1>(point.sources);
      return nullptr;
    }

    // `source.windows[windowKey]`
    const std::optional<ProviderCapacityWindow>& windowOf(const ProviderCapacityCodexSource& source, std::string_view windowKey) {
      return windowKey == "primary" ? source.windows.primary : source.windows.secondary;
    }

    std::optional<StoredRateLimitObservation> latestAvailableRateLimitObservation(const std::vector<ProviderCapacityHistoryPoint>& history, double slot,
                                                                                  const std::string& accountCohortId, double beforeSampledAtMs) {
      std::vector<StoredRateLimitObservation> observations;
      for (const ProviderCapacityHistoryPoint& point : history) {
        if (point.sampled_at_ms >= beforeSampledAtMs) continue;
        const ProviderCapacityCodexSource* source = nullptr;
        for (const ProviderCapacityCodexSource* candidate : {&std::get<0>(point.sources), &std::get<1>(point.sources)}) {
          if (candidate->slot == slot && candidate->state != "unavailable" && candidate->account_cohort_id == accountCohortId) {
            source = candidate;
            break;
          }
        }
        if (source) observations.push_back(StoredRateLimitObservation{.sampled_at_ms = point.sampled_at_ms, .source = *source});
      }
      rt::sort(observations, [](const StoredRateLimitObservation& left, const StoredRateLimitObservation& right) { return right.sampled_at_ms - left.sampled_at_ms; });
      if (observations.empty()) return std::nullopt;
      return observations[0];
    }

    struct RateLimitResetWindowInput {
      double slot;
      std::string_view window;
      double previousSampledAtMs;
      const std::optional<ProviderCapacityWindow>& previousWindow;
      double currentSampledAtMs;
      const std::optional<ProviderCapacityWindow>& currentWindow;
    };

    /**
     * Build one OpenAI rate-limit reset event for a single window. A reset only
     * counts when both samples carry the window fields and a later reset boundary
     * comes with a real capacity gain.
     */
    std::optional<ProviderCapacityRateLimitResetEvent> rateLimitResetEventForWindow(const RateLimitResetWindowInput& input) {
      const std::optional<ProviderCapacityWindow>& previousWindow = input.previousWindow;
      const std::optional<ProviderCapacityWindow>& currentWindow = input.currentWindow;
      if (!previousWindow || !previousWindow->used_percent || !currentWindow || !currentWindow->used_percent || !previousWindow->reset_at_ms ||
          !currentWindow->reset_at_ms || *currentWindow->reset_at_ms <= *previousWindow->reset_at_ms)
        return std::nullopt;
      const double capacityGain = *previousWindow->used_percent - *currentWindow->used_percent;
      if (capacityGain < capacity_contract::PROVIDER_CAPACITY_RATE_LIMIT_RESET_MIN_GAIN_PERCENTAGE_POINTS) return std::nullopt;
      return ProviderCapacityRateLimitResetEvent{
        .v = 1,
        .event_id = "openai-" + rt::numberToString(input.slot) + "-" + std::string(input.window) + "-" + rt::numberToString(input.previousSampledAtMs) + "-" +
                    rt::numberToString(input.currentSampledAtMs),
        .provider = "openai",
        .slot = input.slot,
        .window = std::string(input.window),
        .observed_at_ms = input.currentSampledAtMs,
        .previous_sampled_at_ms = input.previousSampledAtMs,
        .previous_reset_at_ms = *previousWindow->reset_at_ms,
        .reset_at_ms = *currentWindow->reset_at_ms,
        .previous_used_percent = *previousWindow->used_percent,
        .current_used_percent = *currentWindow->used_percent,
        .capacity_gain_percentage_points = capacityGain,
      };
    }

    struct HistoricalOutage {
      std::optional<StoredRateLimitObservation> previousAvailable;
      bool outageObserved;
    };

    // An unavailable sample either keeps tracking an outage for the same account
    // cohort, or clears the tracked window.
    HistoricalOutage trackHistoricalOutage(const std::optional<StoredRateLimitObservation>& previousAvailable, const ProviderCapacityCodexSource* source) {
      if (previousAvailable && source && sameCodexAccountCohort(previousAvailable->source, *source)) return HistoricalOutage{.previousAvailable = previousAvailable, .outageObserved = true};
      return HistoricalOutage{.previousAvailable = std::nullopt, .outageObserved = false};
    }

    void pushHistoricalRateLimitResetEvents(double slot, const ProviderCapacityHistoryPoint& point, const StoredRateLimitObservation& previousAvailable,
                                            const ProviderCapacityCodexSource& source, std::vector<ProviderCapacityRateLimitResetEvent>& events) {
      for (const std::string_view window : {std::string_view("primary"), std::string_view("secondary")}) {
        std::optional<ProviderCapacityRateLimitResetEvent> event = rateLimitResetEventForWindow(RateLimitResetWindowInput{
          .slot = slot,
          .window = window,
          .previousSampledAtMs = previousAvailable.sampled_at_ms,
          .previousWindow = windowOf(previousAvailable.source, window),
          .currentSampledAtMs = point.sampled_at_ms,
          .currentWindow = windowOf(source, window),
        });
        if (event) events.push_back(std::move(*event));
      }
    }

    void observeHistoricalSlotRateLimitResets(double slot, const std::vector<ProviderCapacityHistoryPoint>& sortedHistory,
                                              std::vector<ProviderCapacityRateLimitResetEvent>& events) {
      std::optional<StoredRateLimitObservation> previousAvailable;
      bool outageObserved = false;
      for (const ProviderCapacityHistoryPoint& point : sortedHistory) {
        const ProviderCapacityCodexSource* source = findCodexSource(point, slot);
        if (!source || source->state == "unavailable") {
          HistoricalOutage outage = trackHistoricalOutage(previousAvailable, source);
          previousAvailable = std::move(outage.previousAvailable);
          outageObserved = outage.outageObserved;
          continue;
        }
        if (previousAvailable && outageObserved && sameCodexAccountCohort(previousAvailable->source, *source)) {
          pushHistoricalRateLimitResetEvents(slot, point, *previousAvailable, *source, events);
        }
        if (source->state == "available") previousAvailable = StoredRateLimitObservation{.sampled_at_ms = point.sampled_at_ms, .source = *source};
        outageObserved = false;
      }
    }

    std::vector<ProviderCapacityRateLimitResetEvent> observedHistoricalRateLimitResetEvents(const std::vector<ProviderCapacityHistoryPoint>& history) {
      std::vector<ProviderCapacityRateLimitResetEvent> events;
      const std::vector<ProviderCapacityHistoryPoint> sortedHistory =
        rt::toSorted(history, [](const ProviderCapacityHistoryPoint& left, const ProviderCapacityHistoryPoint& right) { return left.sampled_at_ms - right.sampled_at_ms; });
      for (const double slot : {1.0, 2.0}) observeHistoricalSlotRateLimitResets(slot, sortedHistory, events);
      return events;
    }

    void sortRateLimitResetEvents(std::vector<ProviderCapacityRateLimitResetEvent>& events) {
      rt::sort(events, [](const ProviderCapacityRateLimitResetEvent& left, const ProviderCapacityRateLimitResetEvent& right) {
        const double order = left.observed_at_ms - right.observed_at_ms;
        return rt::truthy(order) ? order : static_cast<double>(rt::localeCompare(left.event_id, right.event_id));
      });
    }
  }

  rt::KvKey providerCapacityHistoryKey(double bucketStartAtMs) { return rt::KvKey{capacity_contract::PROVIDER_CAPACITY_HISTORY_KEY_PREFIX, bucketStartAtMs}; }

  rt::KvKey providerCapacityHistoryTransitionKey(double bucketStartAtMs, double previousSampledAtMs) {
    return rt::KvKey{capacity_contract::PROVIDER_CAPACITY_HISTORY_KEY_PREFIX, bucketStartAtMs, std::string("transition"), previousSampledAtMs};
  }

  rt::Task<std::vector<ProviderCapacityHistoryPoint>> readCapacityHistory(std::shared_ptr<rt::Kv> kv, double nowMs) {
    const double cutoffMs = rt::Math::max(0.0, nowMs - capacity_contract::PROVIDER_CAPACITY_HISTORY_RETENTION_MS);
    const double newestBucketMs = historyBucketStartAtMs(nowMs);
    std::vector<ProviderCapacityHistoryPoint> points;
    bool failed = false;
    try {
      const rt::KvListSelector selector{.prefix = capacity_contract::PROVIDER_CAPACITY_HISTORY_KEY_PREFIX};
      std::unique_ptr<rt::KvListIterator> rows = kv->list(selector);
      while (true) {
        std::optional<rt::KvEntry<>> entry = co_await rows->next();
        if (!entry) break;
        const rt::KvKeyPart* keyBucket = rt::element(entry->key, static_cast<double>(capacity_contract::PROVIDER_CAPACITY_HISTORY_KEY_PREFIX.size()));
        if (!keyBucket || !keyBucket->isNumber() || keyBucket->number() < cutoffMs || keyBucket->number() > newestBucketMs) continue;
        std::optional<ProviderCapacityHistoryPoint> point = capacity_parse::readStoredHistoryPoint(entry->value);
        if (!point || point->bucket_start_at_ms != keyBucket->number()) continue;
        points.push_back(std::move(*point));
      }
    } catch (...) {
      failed = true;
    }
    if (failed) co_return std::vector<ProviderCapacityHistoryPoint>{};
    rt::sort(points, byBucketThenSample);
    co_return points;
  }

  ProviderCapacityHistoryPoint historyPointForSnapshot(const ProviderCapacitySnapshot& snapshot) {
    const auto sourceForSlot = [&snapshot](double slot) -> ProviderCapacityCodexSource {
      const ProviderCapacityCodexSource* source = findCodexSource(snapshot.sources, slot);
      return source ? *source : capacity_parse::unavailableCodexSource(slot, snapshot.snapshot_at_ms);
    };
    const auto sourceForMetered = [&snapshot]() -> ProviderCapacityMeteredSource {
      for (const ProviderCapacitySource& candidate : snapshot.sources) {
        if (const auto* metered = std::get_if<ProviderCapacityMeteredSource>(&candidate)) return *metered;
      }
      return capacity_parse::unavailableMeteredSource(snapshot.snapshot_at_ms);
    };
    return ProviderCapacityHistoryPoint{
      .bucket_start_at_ms = historyBucketStartAtMs(snapshot.snapshot_at_ms),
      .sampled_at_ms = snapshot.snapshot_at_ms,
      .sources = {sourceForSlot(1), sourceForSlot(2), sourceForMetered()},
    };
  }

  std::vector<ProviderCapacityHistoryPoint> mergeHistoryPoints(const std::vector<ProviderCapacityHistoryPoint>& points, const ProviderCapacityHistoryPoint& addition) {
    rt::OrderedMap<std::string, ProviderCapacityHistoryPoint> bySample;
    for (const ProviderCapacityHistoryPoint& point : points) bySample.set(rt::numberToString(point.bucket_start_at_ms) + ":" + rt::numberToString(point.sampled_at_ms), point);
    bySample.set(rt::numberToString(addition.bucket_start_at_ms) + ":" + rt::numberToString(addition.sampled_at_ms), addition);
    std::vector<ProviderCapacityHistoryPoint> merged = rt::arrayFrom(bySample.values());
    rt::sort(merged, byBucketThenSample);
    return merged;
  }

  bool sameCodexAccountCohort(const ProviderCapacityCodexSource& left, const ProviderCapacityCodexSource& right) {
    return left.account_cohort_id.has_value() && left.account_cohort_id == right.account_cohort_id;
  }

  bool codexResetTransitionObserved(const std::optional<ProviderCapacityHistoryPoint>& previous, const ProviderCapacityHistoryPoint& current) {
    if (!previous || previous->sampled_at_ms >= current.sampled_at_ms) return false;
    for (const double slot : {1.0, 2.0}) {
      const ProviderCapacityCodexSource* previousSource = findCodexSource(*previous, slot);
      const ProviderCapacityCodexSource* currentSource = findCodexSource(current, slot);
      if (!previousSource || !currentSource || previousSource->state == "unavailable" || currentSource->state == "unavailable" ||
          !sameCodexAccountCohort(*previousSource, *currentSource)) {
        continue;
      }
      for (const std::string_view windowKey : {std::string_view("primary"), std::string_view("secondary")}) {
        const std::optional<ProviderCapacityWindow>& previousWindow = windowOf(*previousSource, windowKey);
        const std::optional<ProviderCapacityWindow>& currentWindow = windowOf(*currentSource, windowKey);
        if (previousWindow && previousWindow->used_percent && currentWindow && currentWindow->used_percent && *previousWindow->used_percent >= 90 &&
            *currentWindow->used_percent <= 20 && previousWindow->reset_at_ms == currentWindow->reset_at_ms)
          return true;
      }
    }
    return false;
  }

  bool codexAvailabilityTransitionObserved(const std::optional<ProviderCapacityHistoryPoint>& previous, const ProviderCapacityHistoryPoint& current) {
    if (!previous || previous->sampled_at_ms >= current.sampled_at_ms) return false;
    for (const double slot : {1.0, 2.0}) {
      const ProviderCapacityCodexSource* previousSource = findCodexSource(*previous, slot);
      const ProviderCapacityCodexSource* currentSource = findCodexSource(current, slot);
      if (previousSource && currentSource && sameCodexAccountCohort(*previousSource, *currentSource) &&
          (previousSource->state == "unavailable") != (currentSource->state == "unavailable"))
        return true;
    }
    return false;
  }

  std::optional<StoredRateLimitObservation> readStoredRateLimitObservation(const rt::KvValue& value) {
    if (!rt::kvIsObject(value) || !capacity_parse::isSafeTimestamp(value["sampled_at_ms"])) return std::nullopt;
    const double sampledAtMs = value["sampled_at_ms"].asNumber();
    std::optional<ProviderCapacityCodexSource> source = capacity_parse::readStoredCodexSource(value["source"], sampledAtMs);
    if (!source || source->state != "available") return std::nullopt;
    return StoredRateLimitObservation{.sampled_at_ms = sampledAtMs, .source = std::move(*source)};
  }

  std::vector<ProviderCapacityRateLimitResetEvent> observedRateLimitResetEvents(const std::optional<ProviderCapacitySnapshot>& previous,
                                                                               const ProviderCapacitySnapshot& current) {
    return observedRateLimitResetEvents(previous, current, {});
  }

  std::vector<ProviderCapacityRateLimitResetEvent> observedRateLimitResetEvents(const std::optional<ProviderCapacitySnapshot>& previous,
                                                                               const ProviderCapacitySnapshot& current,
                                                                               const std::vector<StoredRateLimitObservation>& lastAvailableObservations) {
    if (previous && previous->snapshot_at_ms >= current.snapshot_at_ms) return {};
    std::vector<ProviderCapacityRateLimitResetEvent> events;
    for (const double slot : {1.0, 2.0}) {
      const ProviderCapacityCodexSource* currentSource = findCodexSource(current.sources, slot);
      if (!currentSource || currentSource->state == "unavailable") continue;
      const ProviderCapacityCodexSource* previousSource = previous ? findCodexSource(previous->sources, slot) : nullptr;
      std::optional<StoredRateLimitObservation> previousObservation;
      if (previous && previousSource && previousSource->state != "unavailable" && sameCodexAccountCohort(*previousSource, *currentSource)) {
        previousObservation = StoredRateLimitObservation{.sampled_at_ms = previous->snapshot_at_ms, .source = *previousSource};
      } else {
        for (const StoredRateLimitObservation& observation : lastAvailableObservations) {
          if (observation.source.slot == slot && observation.sampled_at_ms < current.snapshot_at_ms && sameCodexAccountCohort(observation.source, *currentSource)) {
            previousObservation = observation;
            break;
          }
        }
      }
      if (!previousObservation) continue;
      for (const std::string_view window : {std::string_view("primary"), std::string_view("secondary")}) {
        std::optional<ProviderCapacityRateLimitResetEvent> event = rateLimitResetEventForWindow(RateLimitResetWindowInput{
          .slot = slot,
          .window = window,
          .previousSampledAtMs = previousObservation->sampled_at_ms,
          .previousWindow = windowOf(previousObservation->source, window),
          .currentSampledAtMs = current.snapshot_at_ms,
          .currentWindow = windowOf(*currentSource, window),
        });
        if (event) events.push_back(std::move(*event));
      }
    }
    return events;
  }

  rt::Task<std::vector<ProviderCapacityRateLimitResetEvent>> mergeHistoricalRateLimitResetEvents(std::shared_ptr<rt::Kv> kv,
                                                                                               std::vector<ProviderCapacityHistoryPoint> history,
                                                                                               std::vector<ProviderCapacityRateLimitResetEvent> events) {
    rt::OrderedMap<std::string, ProviderCapacityRateLimitResetEvent> merged;
    for (const ProviderCapacityRateLimitResetEvent& event : events) merged.set(event.event_id, event);
    const std::vector<ProviderCapacityRateLimitResetEvent> observed = observedHistoricalRateLimitResetEvents(history);
    for (const ProviderCapacityRateLimitResetEvent& event : observed) {
      if (merged.has(event.event_id)) continue;
      try {
        const rt::KvSetOptions options{.expireIn = capacity_events::PROVIDER_CAPACITY_RESET_EVENT_RETENTION_MS};
        auto writing = kv->set(capacity_events::providerCapacityRateLimitResetEventKey(event.event_id), rt::KvValue(json::value_from(event)), options);
        co_await std::move(writing);
      } catch (...) {
        // The view can still show a validated inference if a best-effort marker write fails.
      }
      merged.set(event.event_id, event);
    }
    std::vector<ProviderCapacityRateLimitResetEvent> sorted = rt::arrayFrom(merged.values());
    sortRateLimitResetEvents(sorted);
    co_return sorted;
  }

  std::vector<double> recoverySlotsForSnapshot(const ProviderCapacitySnapshot& snapshot, const std::optional<ProviderCapacitySnapshot>& previousSnapshot,
                                               const std::vector<StoredRateLimitObservation>& lastAvailableObservations) {
    std::vector<double> slots;
    for (const ProviderCapacitySource& candidate : snapshot.sources) {
      const ProviderCapacityCodexSource* source = std::get_if<ProviderCapacityCodexSource>(&candidate);
      if (!source || source->state == "unavailable") continue;
      const ProviderCapacityCodexSource* previousSource = previousSnapshot ? findCodexSource(previousSnapshot->sources, source->slot) : nullptr;
      const bool covered = std::any_of(lastAvailableObservations.begin(), lastAvailableObservations.end(), [source](const StoredRateLimitObservation& observation) {
        return observation.source.slot == source->slot && sameCodexAccountCohort(observation.source, *source);
      });
      if (previousSource && previousSource->state == "unavailable" && sameCodexAccountCohort(*previousSource, *source) && !covered) slots.push_back(source->slot);
    }
    return slots;
  }

  rt::Task<std::vector<StoredRateLimitObservation>> recoverRateLimitObservations(std::shared_ptr<rt::Kv> kv, ProviderCapacitySnapshot snapshot, std::vector<double> recoverySlots) {
    if (recoverySlots.empty()) co_return std::vector<StoredRateLimitObservation>{};
    std::vector<ProviderCapacityHistoryPoint> retainedHistory;
    try {
      rt::Promise<std::vector<ProviderCapacityHistoryPoint>> reading(readCapacityHistory(kv, snapshot.snapshot_at_ms));
      retainedHistory = co_await reading.wait();
    } catch (...) {
      retainedHistory.clear();
    }
    std::vector<StoredRateLimitObservation> recovered;
    for (const double slot : recoverySlots) {
      const ProviderCapacityCodexSource* source = findCodexSource(snapshot.sources, slot);
      if (!source || !source->account_cohort_id || source->account_cohort_id->empty()) continue;
      std::optional<StoredRateLimitObservation> observation = latestAvailableRateLimitObservation(retainedHistory, slot, *source->account_cohort_id, snapshot.snapshot_at_ms);
      if (observation) recovered.push_back(std::move(*observation));
    }
    co_return recovered;
  }

  std::vector<LastAvailableCapacityEntry> lastAvailableCapacityEntries(const ProviderCapacitySnapshot& snapshot) {
    std::vector<LastAvailableCapacityEntry> entries;
    for (const ProviderCapacitySource& candidate : snapshot.sources) {
      const ProviderCapacityCodexSource* source = std::get_if<ProviderCapacityCodexSource>(&candidate);
      if (source && source->state == "available") {
        entries.push_back(LastAvailableCapacityEntry{.slot = source->slot, .value = StoredRateLimitObservation{.sampled_at_ms = snapshot.snapshot_at_ms, .source = *source}});
      }
    }
    return entries;
  }
}
