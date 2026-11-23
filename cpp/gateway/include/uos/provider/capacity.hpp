#pragma once
// Port of src/provider/capacity.ts.
//
// At load the TS registers recordClaudeCapacityObservation as claude-code-quota's observation
// recorder (`registerClaudeCodeObservationRecorder(...)` at module top level): capacity.cpp does
// it from a namespace-scope initialiser, so every binary that links capacity.cpp has it installed.
//
// `options = {}` and `request = new Request(...)` defaults are overloads. `Pick<
// ProviderCapacitySnapshotOptions, "kv" | "now">` options take the whole options struct (the
// other members are unused). `kv: Kv | null` is std::shared_ptr<rt::Kv> (a null pointer is
// null); `env?: ClaudeCodeEnvLookup` is an overload without the argument (an empty
// ClaudeCodeEnvLookup is undefined as well).
#include "uos/provider/capacity_contract.hpp"
#include "uos/provider/capacity_history.hpp"
#include "uos/provider/capacity_parse.hpp"
#include "uos/provider/capacity_rollups.hpp"
#include "uos/provider/claude_code_fwd.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>

namespace uos::provider::capacity {
  inline constexpr double PROVIDER_CAPACITY_SNAPSHOT_RETENTION_MS = capacity_contract::PROVIDER_CAPACITY_HISTORY_RETENTION_MS;

  /**
   * How long a persisted capacity snapshot satisfies a normal admin read before
   * the read revalidates it. This matches the Analytics client's visible poll, and
   * is deliberately shorter than the fifteen-minute history bucket: refreshing a
   * value overwrites that bucket's point instead of adding one.
   */
  inline constexpr double PROVIDER_CAPACITY_READ_FRESH_MS = 30'000;

  // The admin chart keeps its seven-day raw history; long-run research reads
  // the separate hourly rollup store through a bounded query window instead.
  inline constexpr double PROVIDER_CAPACITY_RESEARCH_DEFAULT_WINDOW_DAYS = 90;
  inline constexpr double PROVIDER_CAPACITY_RESEARCH_MAX_WINDOW_DAYS = 365;

  // `export { ... } from "./capacity-contract.ts"`
  using capacity_contract::PROVIDER_CAPACITY_CODEX_TIMEOUT_MS;
  using capacity_contract::PROVIDER_CAPACITY_COLD_WAIT_MS;
  using capacity_contract::PROVIDER_CAPACITY_HISTORY_BUCKET_MS;
  using capacity_contract::PROVIDER_CAPACITY_HISTORY_KEY_PREFIX;
  using capacity_contract::PROVIDER_CAPACITY_HISTORY_RETENTION_MS;
  using capacity_contract::PROVIDER_CAPACITY_LEASE_KEY;
  using capacity_contract::PROVIDER_CAPACITY_LEASE_MS;
  using capacity_contract::PROVIDER_CAPACITY_RATE_LIMIT_RESET_MIN_GAIN_PERCENTAGE_POINTS;
  using capacity_contract::PROVIDER_CAPACITY_SNAPSHOT_KEY;
  using capacity_contract::PROVIDER_CAPACITY_SOURCE_STALE_MS;
  // `export { PROVIDER_CAPACITY_ROLLUP_BUCKET_MS } from "./capacity-rollups.ts"`
  using capacity_rollups::PROVIDER_CAPACITY_ROLLUP_BUCKET_MS;
  // `export type { ... } from "./capacity-contract.ts"`
  using capacity_contract::ProviderCapacityAdditionalRateLimit;
  using capacity_contract::ProviderCapacityCodexSource;
  using capacity_contract::ProviderCapacityFailureKind;
  using capacity_contract::ProviderCapacityFetch;
  using capacity_contract::ProviderCapacityHistoryPoint;
  using capacity_contract::ProviderCapacityMeteredSource;
  using capacity_contract::ProviderCapacitySnapshot;
  using capacity_contract::ProviderCapacitySnapshotOptions;
  using capacity_contract::ProviderCapacitySource;
  using capacity_contract::ProviderCapacityView;
  using capacity_contract::ProviderCapacityViewState;
  using capacity_contract::ProviderCapacityWindow;
  // `export { codexAccountLabel } from "./capacity-parse.ts"`
  using capacity_parse::codexAccountLabel;
  // `export { providerCapacityHistoryKey } from "./capacity-history.ts"`
  using capacity_history::providerCapacityHistoryKey;

  /** The Claude subscription row for a caller that has no tier state to read. */
  capacity_contract::ProviderCapacityClaudeSource unavailableClaudeCapacitySource(double snapshotAtMs);

  rt::Task<capacity_contract::ProviderCapacityClaudeSource> claudeCapacitySource(std::shared_ptr<rt::Kv> kv, double snapshotAtMs);
  rt::Task<capacity_contract::ProviderCapacityClaudeSource> claudeCapacitySource(std::shared_ptr<rt::Kv> kv, double snapshotAtMs,
                                                                                 claude_code::ClaudeCodeEnvLookup env);

  // `options: Pick<ProviderCapacitySnapshotOptions, "kv" | "now"> = {}`
  rt::Task<capacity_contract::ProviderCapacityView> getPersistedProviderCapacityView();
  rt::Task<capacity_contract::ProviderCapacityView> getPersistedProviderCapacityView(capacity_contract::ProviderCapacitySnapshotOptions options);

  rt::Task<capacity_contract::ProviderCapacityView> refreshProviderCapacity();
  rt::Task<capacity_contract::ProviderCapacityView> refreshProviderCapacity(capacity_contract::ProviderCapacitySnapshotOptions options);

  /**
   * Persist one capacity sample for an observed event, without building the admin
   * projection.
   *
   * Replaces the retired fifteen-minute deploy cron. The trigger is a capacity observation
   * - a Codex rate-limit reset, an upstream downtime - or an operator opening the
   * capacity view; `triggerProviderCapacitySample` is the fire-and-forget entry
   * point and debounces to one probe per history bucket. Nothing here runs because
   * time passed.
   *
   * The caller does not consume a ProviderCapacityView, so scanning the seven-day
   * history and reset-event ledgers before and after every sample only pays to
   * construct a discarded response. Keep the capture, routing observations, lease,
   * snapshot/history write, and same-bucket reset transition exactly on the
   * durable sampler path. Admin callers continue to use refreshProviderCapacity()
   * when they need the full projection.
   */
  rt::Task<void> sampleProviderCapacityOnEvent();
  rt::Task<void> sampleProviderCapacityOnEvent(capacity_contract::ProviderCapacitySnapshotOptions options);

  /**
   * Long-run research read over the forever-kept hourly capacity rollups. This is
   * deliberately separate from the operational capacity view: the chart keeps its
   * bounded seven-day raw history, while `window_days` (default 90, capped at
   * 365) bounds only this rollup query.
   */
  // `request = new Request("https://ai.ubq.fi/admin/providers/capacity/rollups")`,
  // `options: Pick<ProviderCapacitySnapshotOptions, "kv" | "now"> = {}`
  rt::Task<rt::Response> handleProviderCapacityRollups();
  rt::Task<rt::Response> handleProviderCapacityRollups(rt::Request request);
  rt::Task<rt::Response> handleProviderCapacityRollups(rt::Request request, capacity_contract::ProviderCapacitySnapshotOptions options);

  // `request = new Request("https://ai.ubq.fi/admin/providers/capacity")`, `options = {}`
  rt::Task<rt::Response> handleProviderCapacity();
  rt::Task<rt::Response> handleProviderCapacity(rt::Request request);
  rt::Task<rt::Response> handleProviderCapacity(rt::Request request, capacity_contract::ProviderCapacitySnapshotOptions options);
}
