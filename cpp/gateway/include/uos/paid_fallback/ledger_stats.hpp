#pragma once
// Port of src/paid-fallback/ledger-stats.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::paid_fallback::ledger_stats {
  /**
   * Daily, best-effort growth instrumentation for the paid-fallback raw-row
   * store and its hourly rollups.
   *
   * The retention design (docs/log-retention-and-quota-runway-2026-08-25.md) was
   * sized on estimates: ~800 B per settled raw row, ~25 KB of rollup per model
   * per day, and window-bounded admin projection reads with unknown cost. These
   * counters record the measured values as one small row per UTC day so the
   * 365-day horizon, the rollup shape and the storage budget can be tuned with
   * evidence instead of guesses:
   *
   * - `settled_rows` / `settled_row_bytes`: settled raw rows written and their
   *   JSON-serialized size. Summed over the retention window this is the
   *   estimated retained raw-row store, compared against
   *   PAID_FALLBACK_RAW_STORE_BUDGET_BYTES to raise the storage alert.
   * - `rollup_writes` / `rollup_bytes`: merged hourly rollup records written and
   *   their serialized size. This is rollup write volume, not retained size.
   * - `projection_*`: admin quota-projection views and the KV read units they
   *   consumed, split by the requested 7/30/90-day window, so the real cost of an
   *   admin view is measurable.
   *
   * Writes are best-effort CAS merges: a lost sample must never fail a
   * settlement or an admin view. The alert is therefore a lower-bound estimate,
   * which the growth view documents.
   *
   * `estimatePaidFallbackRecordBytes` measures the JSON-serialized logical
   * record, which is the unit the raw-row store and rollups are counted in. It is
   * deliberately not the physical SQLite/WAL footprint of the shared KV database
   * and not total host storage: those include page overhead, indexes, WAL,
   * checkpoints and every other key family.
   */

  inline const rt::KvKey PAID_FALLBACK_LEDGER_STATS_PREFIX{"uos_ai", "paid_fallback", "v3", "ledger_stats"};
  inline constexpr double PAID_FALLBACK_LEDGER_STATS_DAY_MS = 24 * 60 * 60 * 1'000;
  /** Operational 5 GiB budget for the estimated retained raw-row store: its 80% mark is the storage-growth warning threshold. */
  inline constexpr double PAID_FALLBACK_RAW_STORE_BUDGET_BYTES = 5.0 * 1024 * 1024 * 1024;
  /** Alert once the estimated retained raw store reaches this share of the budget. */
  inline constexpr double PAID_FALLBACK_RAW_STORE_ALERT_RATIO = 0.8;
  /** Bounded daily leaderboard returned by the growth view. */
  inline constexpr double PAID_FALLBACK_LEDGER_STATS_LEADERBOARD_DAYS = 30;
  /** CAS attempts for one best-effort counter merge before dropping the sample. */
  inline constexpr double PAID_FALLBACK_LEDGER_STATS_MAX_WRITE_ATTEMPTS = 8;

  // `7 | 30 | 90`
  using PaidFallbackLedgerProjectionWindowDays = double;

  struct PaidFallbackLedgerProjectionCounters {
    double views;
    double read_units;
    double rollup_rows;
    friend bool operator==(const PaidFallbackLedgerProjectionCounters&, const PaidFallbackLedgerProjectionCounters&) = default;
  };

  struct PaidFallbackLedgerDailyStats {
    double v = 1;
    double day_start_at_ms;
    double settled_rows;
    double settled_row_bytes;
    double rollup_writes;
    double rollup_bytes;
    PaidFallbackLedgerProjectionCounters projection_7d;
    PaidFallbackLedgerProjectionCounters projection_30d;
    PaidFallbackLedgerProjectionCounters projection_90d;
    double updated_at_ms;
    friend bool operator==(const PaidFallbackLedgerDailyStats&, const PaidFallbackLedgerDailyStats&) = default;
  };

  // The stored row: value_from writes the members in declaration order; value_to is strict.
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackLedgerProjectionCounters& value);
  PaidFallbackLedgerProjectionCounters tag_invoke(const json::value_to_tag<PaidFallbackLedgerProjectionCounters>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackLedgerDailyStats& value);
  PaidFallbackLedgerDailyStats tag_invoke(const json::value_to_tag<PaidFallbackLedgerDailyStats>&, const json::value& value);

  PaidFallbackLedgerDailyStats emptyPaidFallbackLedgerDailyStats(double dayStartAtMs);

  /** UTC day bucket start, matching the hourly rollups' bucket-precision style. */
  double paidFallbackLedgerStatsDayStart(double ms);

  rt::KvKey paidFallbackLedgerStatsDayKey(double dayStartAtMs);

  // `value: unknown` (a stored value; a json::value converts implicitly).
  bool isPaidFallbackLedgerDailyStats(const rt::KvValue& value);

  /**
   * JSON-serialized byte size of one KV value. Deno KV's own encoding is not
   * JSON, so this is the same bounded estimate the admin payload size checks
   * use: it measures the row, not the physical page, and never throws.
   */
  // `value: unknown`: a JSON record (JSON.stringify cannot fail on one).
  double estimatePaidFallbackRecordBytes(const json::value& value);

  // `input: Readonly<{ settledRowBytes; rollupBytes: number | null; nowMs?: number }>`
  struct PaidFallbackLedgerSettlementStatsInput {
    double settledRowBytes;
    /** Serialized merged rollup record, or null when no rollup was written. */
    std::optional<double> rollupBytes;
    std::optional<double> nowMs{};
  };

  /**
   * Records one settled raw row (and the rollup it folded into, when any). The
   * caller measures the records; this function only merges the daily counters
   * and swallows every failure so measurement can never break settlement.
   */
  // `kv: Kv | null`: a null pointer is null.
  rt::Task<bool> recordPaidFallbackLedgerSettlementStats(std::shared_ptr<rt::Kv> kv, PaidFallbackLedgerSettlementStatsInput input);

  // `input: Readonly<{ windowDays; readUnits; rollupRows; nowMs?: number }>`
  struct PaidFallbackLedgerProjectionStatsInput {
    PaidFallbackLedgerProjectionWindowDays windowDays;
    double readUnits;
    double rollupRows;
    std::optional<double> nowMs{};
  };

  /**
   * Records one admin quota-projection view with the KV read units it consumed
   * (one per KV operation plus one per returned entry) for its requested window.
   */
  rt::Task<bool> recordPaidFallbackLedgerProjectionStats(std::shared_ptr<rt::Kv> kv, PaidFallbackLedgerProjectionStatsInput input);

  /** One bounded daily leaderboard entry, newest last. */
  struct PaidFallbackLedgerGrowthDay {
    double day_start_at_ms;
    double settled_rows;
    double settled_row_bytes;
    double rollup_writes;
    double rollup_bytes;
    double projection_views;
    double projection_read_units;
    double projection_rollup_rows;
    friend bool operator==(const PaidFallbackLedgerGrowthDay&, const PaidFallbackLedgerGrowthDay&) = default;
  };

  struct PaidFallbackLedgerProjectionSummary {
    PaidFallbackLedgerProjectionWindowDays window_days;
    double views;
    double read_units;
    double rollup_rows;
    std::optional<double> avg_read_units_per_view;
    std::optional<double> avg_rollup_rows_per_view;
    friend bool operator==(const PaidFallbackLedgerProjectionSummary&, const PaidFallbackLedgerProjectionSummary&) = default;
  };

  struct PaidFallbackLedgerGrowthView {
    double as_of_ms;
    std::string scan;  // "ok" | "unavailable"
    /** KV read units this scan consumed: one per list call plus one per returned daily key. */
    double read_units;
    double retention_days;
    double leaderboard_days;
    std::vector<PaidFallbackLedgerGrowthDay> leaderboard;
    double settled_rows;
    double settled_row_bytes;
    double rollup_writes;
    double rollup_bytes;
    double active_days;
    std::optional<double> avg_row_bytes;
    std::optional<double> avg_settled_rows_per_active_day;
    /** Sum of settled rows written inside the retention window (lower bound on retained rows). */
    double estimated_retained_rows;
    /** Sum of settled-row bytes written inside the retention window. */
    double estimated_retained_raw_bytes;
    double budget_bytes;
    double alert_ratio;
    double alert_threshold_bytes;
    bool alert;
    std::vector<PaidFallbackLedgerProjectionSummary> projections;
    friend bool operator==(const PaidFallbackLedgerGrowthView&, const PaidFallbackLedgerGrowthView&) = default;
  };

  // The view as the TS object JSON.stringify writes it (members in declaration order).
  json::object toJson(const PaidFallbackLedgerGrowthView& view);

  // `options: Readonly<{ nowMs: number; retentionMs: number; leaderboardDays?: number }>`
  struct PaidFallbackLedgerGrowthOptions {
    double nowMs;
    double retentionMs;
    std::optional<double> leaderboardDays{};
  };

  /**
   * Reads the daily counters inside the retention window and derives the
   * measured growth view. A failed scan reports `scan: "unavailable"` instead of
   * fabricating zeros, so an operator can tell "nothing settled" from "history
   * could not be read". The retained-byte estimate includes every row written in
   * the window and is therefore an upper bound, while dropped best-effort
   * samples make it a lower bound on actual writes.
   */
  rt::Task<PaidFallbackLedgerGrowthView> readPaidFallbackLedgerGrowth(std::shared_ptr<rt::Kv> kv, PaidFallbackLedgerGrowthOptions options);
}
