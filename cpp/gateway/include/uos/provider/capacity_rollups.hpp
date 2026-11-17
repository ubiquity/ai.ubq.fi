#pragma once
// Port of src/provider/capacity-rollups.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::provider::capacity_rollups {
  /**
   * Compact per-hour capacity rollups for the two Codex subscription slots.
   *
   * The admin chart reads fifteen-minute ProviderCapacityHistoryPoints that are
   * retained for seven days because that read feeds the admin view payload
   * directly. Long-run research reads these hourly aggregates instead: per hour,
   * and per slot inside the record, the min/max/last window usage observed. Every
   * persisted capacity sample folds its slot summaries into the same atomic that
   * writes the fifteen-minute history point, so a retained sample cannot miss its
   * rollup, and rollups are written without a TTL.
   */
  inline const rt::KvKey PROVIDER_CAPACITY_ROLLUP_PREFIX{"uos_ai", "provider_capacity", "v1", "rollup"};

  /** One hour, matching the Metered balance history and paid-fallback rollups. */
  inline constexpr double PROVIDER_CAPACITY_ROLLUP_BUCKET_MS = 60 * 60 * 1'000;

  /** Bounds one research scan: 365 days is at most 8,760 hourly points. */
  inline constexpr double PROVIDER_CAPACITY_ROLLUP_READ_LIMIT = 20'000;

  // "available" | "stale" | "unavailable"
  using ProviderCapacityRollupState = std::string;

  struct ProviderCapacityRollupWindow {
    double samples;
    std::optional<double> limit_window_seconds;
    std::optional<double> min_used_percent;
    std::optional<double> max_used_percent;
    std::optional<double> last_used_percent;
    std::optional<double> last_reset_at_ms;
    friend bool operator==(const ProviderCapacityRollupWindow&, const ProviderCapacityRollupWindow&) = default;
  };

  struct ProviderCapacityRollupSlot {
    // `1 | 2`
    double slot;
    double available_sample_count;
    ProviderCapacityRollupState last_state;
    ProviderCapacityRollupWindow primary;
    ProviderCapacityRollupWindow secondary;
    friend bool operator==(const ProviderCapacityRollupSlot&, const ProviderCapacityRollupSlot&) = default;
  };

  struct ProviderCapacityRollupPoint {
    // `1`
    double v = 1;
    double bucket_start_at_ms;
    double sample_count;
    double first_sampled_at_ms;
    double last_sampled_at_ms;
    // `readonly [ProviderCapacityRollupSlot, ProviderCapacityRollupSlot]`
    std::array<ProviderCapacityRollupSlot, 2> slots;
    friend bool operator==(const ProviderCapacityRollupPoint&, const ProviderCapacityRollupPoint&) = default;
  };

  struct ProviderCapacityRollupWindowInput {
    std::optional<double> limit_window_seconds;
    std::optional<double> used_percent;
    std::optional<double> reset_at_ms;
    friend bool operator==(const ProviderCapacityRollupWindowInput&, const ProviderCapacityRollupWindowInput&) = default;
  };

  struct ProviderCapacityRollupSlotInput {
    // `1 | 2`
    double slot;
    ProviderCapacityRollupState state;
    std::optional<ProviderCapacityRollupWindowInput> primary;
    std::optional<ProviderCapacityRollupWindowInput> secondary;
    friend bool operator==(const ProviderCapacityRollupSlotInput&, const ProviderCapacityRollupSlotInput&) = default;
  };

  struct ProviderCapacityRollupInput {
    double bucket_start_at_ms;
    double sampled_at_ms;
    // `readonly [ProviderCapacityRollupSlotInput, ProviderCapacityRollupSlotInput]`
    std::array<ProviderCapacityRollupSlotInput, 2> slots;
    friend bool operator==(const ProviderCapacityRollupInput&, const ProviderCapacityRollupInput&) = default;
  };

  // The stored / wire records: value_from writes the members in declaration order (a stored
  // point is `rt::KvValue(json::value_from(point))`); value_to is strict (rt::TypeError) and
  // a `slots` tuple must hold exactly two objects.
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityRollupWindow& value);
  ProviderCapacityRollupWindow tag_invoke(const json::value_to_tag<ProviderCapacityRollupWindow>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityRollupSlot& value);
  ProviderCapacityRollupSlot tag_invoke(const json::value_to_tag<ProviderCapacityRollupSlot>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityRollupPoint& value);
  ProviderCapacityRollupPoint tag_invoke(const json::value_to_tag<ProviderCapacityRollupPoint>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityRollupWindowInput& value);
  ProviderCapacityRollupWindowInput tag_invoke(const json::value_to_tag<ProviderCapacityRollupWindowInput>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityRollupSlotInput& value);
  ProviderCapacityRollupSlotInput tag_invoke(const json::value_to_tag<ProviderCapacityRollupSlotInput>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityRollupInput& value);
  ProviderCapacityRollupInput tag_invoke(const json::value_to_tag<ProviderCapacityRollupInput>&, const json::value& value);

  // `(value: unknown): value is ProviderCapacityRollupPoint` over a stored value (a json::value
  // converts implicitly).
  bool isProviderCapacityRollupPoint(const rt::KvValue& value);

  // `isProviderCapacityRollupPoint(value) ? value : null` with the narrowed value as the typed
  // record. It reads only the declared members, so a value the guard accepts always converts
  // (members beyond the declared ones, which this module never writes, are not kept).
  std::optional<ProviderCapacityRollupPoint> providerCapacityRollupPointOf(const rt::KvValue& value);

  double providerCapacityRollupBucketStartAtMs(double observedAtMs);

  rt::KvKey providerCapacityRollupKey(double bucketStartAtMs);

  /** Pure merge of one capacity observation into its hour bucket. */
  // `existing: ProviderCapacityRollupPoint | null`
  ProviderCapacityRollupPoint mergeProviderCapacityRollup(const std::optional<ProviderCapacityRollupPoint>& existing, const ProviderCapacityRollupInput& input);

  // `options: Readonly<{ sinceMs: number; nowMs: number; limit?: number }>`
  struct ListProviderCapacityRollupsOptions {
    double sinceMs;
    double nowMs;
    std::optional<double> limit{};
  };

  /**
   * Reads the hourly rollups intersecting `[sinceMs, nowMs]`. The complete
   * requested range is returned in bucket order; `limit` bounds each range scan
   * so a research query can never enumerate the whole retained history.
   */
  // `kv: Kv` is non-null.
  rt::Task<std::vector<ProviderCapacityRollupPoint>> listProviderCapacityRollups(std::shared_ptr<rt::Kv> kv, ListProviderCapacityRollupsOptions options);
}
