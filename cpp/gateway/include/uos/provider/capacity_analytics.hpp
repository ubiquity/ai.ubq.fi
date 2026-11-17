#pragma once
// Port of src/provider/capacity-analytics.ts.
//
// Analytics-only capacity observations pushed by another gateway.
//
// These rows augment the admin chart history; routing and reset fences continue
// to read only the operational local history in capacity-history.ts. Keeping
// the namespace separate makes the chart additive and never changes a local
// probe, lease, reset event, or routing decision.
//
// The companion sends closed fifteen-minute bucket points plus immutable
// same-bucket transitions: at most 672 ordinary points per host in the seven-day
// read window, not every live refresh. Rows expire seven days after ingestion;
// reads also exclude buckets older than seven days. Pre-existing intermediate
// samples age out under that same TTL. There is no additional hard per-host cap,
// which would discard genuine transition evidence during a busy week.
//
// The receiving gateway's own history stays canonical. A pushed sample only
// fills a fifteen-minute bucket the receiver did not observe, and only when the
// sample shows every configured Codex account available under the same slot
// accounts as a neighbouring local sample. The pusher's own 401 or unreachable
// samples, and samples taken against other slot accounts, never enter the chart.
//
// `kv: Kv` is a non-null std::shared_ptr<rt::Kv>. `value: unknown` inputs (a pushed request
// body) take json::value plus a pointer overload (nullptr is undefined); isRemoteCapacityHostId
// also takes a known string (a string literal argument is ambiguous: pass a std::string_view).
#include "uos/provider/capacity_contract.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::provider::capacity_analytics {
  inline const rt::KvKey REMOTE_CAPACITY_OBSERVATION_PREFIX{"uos_ai", "provider_capacity", "analytics", "remote"};

  /**
   * Rows in one ingest request. This is below a full week of fifteen-minute
   * buckets (672): the pushing gateway sends only samples newer than the last
   * acknowledged one, oldest first, one batch per poll.
   */
  inline constexpr double MAX_REMOTE_CAPACITY_OBSERVATIONS = 512;

  struct RemoteCapacityObservation {
    std::string host;
    capacity_contract::ProviderCapacityHistoryPoint point;
    friend bool operator==(const RemoteCapacityObservation&, const RemoteCapacityObservation&) = default;
  };

  /** Host ids are bounded lowercase labels so a pushed row cannot widen the key space. */
  // `(value: unknown): value is string`
  bool isRemoteCapacityHostId(const json::value& value);
  bool isRemoteCapacityHostId(const json::value* value);
  bool isRemoteCapacityHostId(std::string_view value);

  /** Parses one pushed history batch or returns null when the batch or any row is out of bounds or malformed. */
  // std::nullopt is null.
  std::optional<std::vector<capacity_contract::ProviderCapacityHistoryPoint>> parseRemoteCapacityObservations(const json::value& value);
  std::optional<std::vector<capacity_contract::ProviderCapacityHistoryPoint>> parseRemoteCapacityObservations(const json::value* value);

  /**
   * Writes the retention-eligible points in bounded atomic batches and returns
   * the accepted count. A batch that fails to commit throws, so the caller never
   * acknowledges rows that were not stored.
   */
  rt::Task<double> storeRemoteCapacityObservations(std::shared_ptr<rt::Kv> kv, std::string host,
                                                   std::vector<capacity_contract::ProviderCapacityHistoryPoint> points, double nowMs);

  /** Reads every retained pushed point with its host. A failed scan reports no remote rows. */
  rt::Task<std::vector<RemoteCapacityObservation>> readRemoteCapacityObservations(std::shared_ptr<rt::Kv> kv, double nowMs);

  /**
   * Returns the local history plus the pushed samples that fill buckets the
   * local gateway never observed. A pushed sample qualifies only when it is
   * available and reports the same slot accounts as the closest local sample on
   * either side; a gap bucket is filled from one pushing host only. A gateway
   * with no local samples takes none, because there is nothing to match against.
   */
  std::vector<capacity_contract::ProviderCapacityHistoryPoint> mergeCapacityHistory(const std::vector<capacity_contract::ProviderCapacityHistoryPoint>& local,
                                                                                    const std::vector<RemoteCapacityObservation>& remote);
}
