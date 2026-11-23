#pragma once
// Port of src/admin/capacity-observation-sync.ts.
//
// Authenticated ingest for another gateway's capacity observations.
//
// The Mac service pushes its new local samples here; the VPS stores them in the
// analytics-only namespace and uses them only to fill chart buckets it did not
// observe itself (see capacity-analytics.ts). The operational history, leases,
// reset events, and routing keys are untouched.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <memory>
#include <optional>

namespace uos::admin::capacity_observation_sync {
  // `options: Readonly<{ kv?: Kv | null; now?: () => number }>`: `kv` std::nullopt is undefined
  // (getKv()), a null pointer is null; an empty `now` is undefined (Date.now).
  struct CapacityObservationIngestOptions {
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    std::function<double()> now{};
  };

  /**
   * POST /admin/providers/capacity/observations
   *
   * Body: { "host": "mac", "history": ProviderCapacityHistoryPoint[] }
   *
   * The route is inside the authenticated admin table. A malformed envelope, more
   * than MAX_REMOTE_CAPACITY_OBSERVATIONS rows, or any malformed, unaligned, or
   * out-of-bucket observation rejects the whole batch instead of storing partial
   * evidence. A failed write answers 503 so the pusher keeps the rows pending.
   */
  // `options = {}`
  rt::Task<rt::Response> handleCapacityObservationIngest(rt::Request request);
  rt::Task<rt::Response> handleCapacityObservationIngest(rt::Request request, const CapacityObservationIngestOptions& options);
}
