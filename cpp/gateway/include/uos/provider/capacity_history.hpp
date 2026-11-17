#pragma once
// Port of src/provider/capacity-history.ts.
// Provider capacity history, transition detection and rate-limit recovery, split out of src/provider_capacity.ts.
//
// Except providerCapacityHistoryKey, the module declares its helpers module-private and exports
// them in one `export { ... }` list; they are declared here in source order. This header includes
// uos/provider/capacity_contract.hpp and uos/provider/capacity_events.hpp for the record types
// those exported signatures use (the plan's header DAG lists no edge because it reads only
// `export const` declarations; neither header includes this one, so there is no cycle).
// `x: T | null` parameters are const std::optional<T>&; `kv: Kv` is a non-null
// std::shared_ptr<rt::Kv>. readStoredRateLimitObservation validates stored values, so it takes
// rt::KvValue (a json::value converts implicitly).
#include "uos/provider/capacity_contract.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace uos::provider::capacity_history {
  rt::KvKey providerCapacityHistoryKey(double bucketStartAtMs);

  // A live refresh can observe a banked reset inside the same 15-minute bucket
  // as the exhausted sample. Keep that earlier point under a sibling key so the
  // chart can show the zero-to-refill transition without turning every refresh
  // into an unbounded history stream.
  rt::KvKey providerCapacityHistoryTransitionKey(double bucketStartAtMs, double previousSampledAtMs);

  // Returns [] on a failed scan.
  rt::Task<std::vector<capacity_contract::ProviderCapacityHistoryPoint>> readCapacityHistory(std::shared_ptr<rt::Kv> kv, double nowMs);

  capacity_contract::ProviderCapacityHistoryPoint historyPointForSnapshot(const capacity_contract::ProviderCapacitySnapshot& snapshot);

  std::vector<capacity_contract::ProviderCapacityHistoryPoint> mergeHistoryPoints(const std::vector<capacity_contract::ProviderCapacityHistoryPoint>& points,
                                                                                  const capacity_contract::ProviderCapacityHistoryPoint& addition);

  bool sameCodexAccountCohort(const capacity_contract::ProviderCapacityCodexSource& left, const capacity_contract::ProviderCapacityCodexSource& right);

  // `previous: ProviderCapacityHistoryPoint | null`
  bool codexResetTransitionObserved(const std::optional<capacity_contract::ProviderCapacityHistoryPoint>& previous,
                                    const capacity_contract::ProviderCapacityHistoryPoint& current);

  bool codexAvailabilityTransitionObserved(const std::optional<capacity_contract::ProviderCapacityHistoryPoint>& previous,
                                           const capacity_contract::ProviderCapacityHistoryPoint& current);

  // `(value: unknown): StoredRateLimitObservation | null`; std::nullopt is null.
  std::optional<capacity_contract::StoredRateLimitObservation> readStoredRateLimitObservation(const rt::KvValue& value);

  // `previous: ProviderCapacitySnapshot | null`, `lastAvailableObservations = []`
  std::vector<capacity_events::ProviderCapacityRateLimitResetEvent> observedRateLimitResetEvents(
    const std::optional<capacity_contract::ProviderCapacitySnapshot>& previous, const capacity_contract::ProviderCapacitySnapshot& current);
  std::vector<capacity_events::ProviderCapacityRateLimitResetEvent> observedRateLimitResetEvents(
    const std::optional<capacity_contract::ProviderCapacitySnapshot>& previous, const capacity_contract::ProviderCapacitySnapshot& current,
    const std::vector<capacity_contract::StoredRateLimitObservation>& lastAvailableObservations);

  rt::Task<std::vector<capacity_events::ProviderCapacityRateLimitResetEvent>> mergeHistoricalRateLimitResetEvents(
    std::shared_ptr<rt::Kv> kv, std::vector<capacity_contract::ProviderCapacityHistoryPoint> history,
    std::vector<capacity_events::ProviderCapacityRateLimitResetEvent> events);

  // A codex source that comes back after an outage is a recovery candidate when the
  // account cohort matches and no last-available observation covers it yet.
  // Returns `(1 | 2)[]`.
  std::vector<double> recoverySlotsForSnapshot(const capacity_contract::ProviderCapacitySnapshot& snapshot,
                                               const std::optional<capacity_contract::ProviderCapacitySnapshot>& previousSnapshot,
                                               const std::vector<capacity_contract::StoredRateLimitObservation>& lastAvailableObservations);

  // `recoverySlots: readonly (1 | 2)[]`
  rt::Task<std::vector<capacity_contract::StoredRateLimitObservation>> recoverRateLimitObservations(std::shared_ptr<rt::Kv> kv,
                                                                                                     capacity_contract::ProviderCapacitySnapshot snapshot,
                                                                                                     std::vector<double> recoverySlots);

  // `Readonly<{ slot: 1 | 2; value: StoredRateLimitObservation }>`, one entry of lastAvailableCapacityEntries.
  struct LastAvailableCapacityEntry {
    double slot;  // `1 | 2`
    capacity_contract::StoredRateLimitObservation value;
    friend bool operator==(const LastAvailableCapacityEntry&, const LastAvailableCapacityEntry&) = default;
  };

  std::vector<LastAvailableCapacityEntry> lastAvailableCapacityEntries(const capacity_contract::ProviderCapacitySnapshot& snapshot);
}
