#include "uos/paid_fallback/ledger_stats.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/text_codec.hpp"

#include <functional>
#include <string_view>
#include <utility>

namespace uos::paid_fallback::ledger_stats {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    PaidFallbackLedgerProjectionCounters zeroProjectionCounters() { return {.views = 0, .read_units = 0, .rollup_rows = 0}; }

    // `typeof value === "object" && value !== null` (utils.isRecord) over a stored value.

    bool isNonNegativeSafeInteger(const rt::KvValue& value) {
      return value.isNumber() && rt::Number::isSafeInteger(value.asNumber()) && value.asNumber() >= 0;
    }

    bool isProjectionCounters(const rt::KvValue& value) {
      return rt::kvIsObject(value) && isNonNegativeSafeInteger(value["views"]) && isNonNegativeSafeInteger(value["read_units"]) && isNonNegativeSafeInteger(value["rollup_rows"]);
    }

    rt::KvValue countersValue(const PaidFallbackLedgerProjectionCounters& counters) {
      rt::KvObject object;
      object.set("views", rt::KvValue(counters.views));
      object.set("read_units", rt::KvValue(counters.read_units));
      object.set("rollup_rows", rt::KvValue(counters.rollup_rows));
      return rt::KvValue(std::move(object));
    }

    // The stored row, members in the order the TS object literal writes them.
    rt::KvObject statsObject(const PaidFallbackLedgerDailyStats& stats) {
      rt::KvObject object;
      object.set("v", rt::KvValue(stats.v));
      object.set("day_start_at_ms", rt::KvValue(stats.day_start_at_ms));
      object.set("settled_rows", rt::KvValue(stats.settled_rows));
      object.set("settled_row_bytes", rt::KvValue(stats.settled_row_bytes));
      object.set("rollup_writes", rt::KvValue(stats.rollup_writes));
      object.set("rollup_bytes", rt::KvValue(stats.rollup_bytes));
      object.set("projection_7d", countersValue(stats.projection_7d));
      object.set("projection_30d", countersValue(stats.projection_30d));
      object.set("projection_90d", countersValue(stats.projection_90d));
      object.set("updated_at_ms", rt::KvValue(stats.updated_at_ms));
      return object;
    }

    PaidFallbackLedgerProjectionCounters countersFrom(const rt::KvValue& value) {
      return {.views = value["views"].asNumber(), .read_units = value["read_units"].asNumber(), .rollup_rows = value["rollup_rows"].asNumber()};
    }

    // A row isPaidFallbackLedgerDailyStats accepted.
    PaidFallbackLedgerDailyStats statsFrom(const rt::KvValue& value) {
      return {
        .v = 1,
        .day_start_at_ms = value["day_start_at_ms"].asNumber(),
        .settled_rows = value["settled_rows"].asNumber(),
        .settled_row_bytes = value["settled_row_bytes"].asNumber(),
        .rollup_writes = value["rollup_writes"].asNumber(),
        .rollup_bytes = value["rollup_bytes"].asNumber(),
        .projection_7d = countersFrom(value["projection_7d"]),
        .projection_30d = countersFrom(value["projection_30d"]),
        .projection_90d = countersFrom(value["projection_90d"]),
        .updated_at_ms = value["updated_at_ms"].asNumber(),
      };
    }

    std::string_view projectionCountersKey(PaidFallbackLedgerProjectionWindowDays windowDays) {
      if (windowDays == 7) return "projection_7d";
      if (windowDays == 30) return "projection_30d";
      return "projection_90d";
    }

    // `merge(existing)` over the stored object itself, so members the row carries beyond the
    // declared ones keep their place, as the TS spread keeps them.
    using Merge = std::function<rt::KvObject(const rt::KvObject& stats)>;

    /** One CAS retry loop; drops the sample instead of delaying settlement or the admin view. */
    rt::Task<void> mergePaidFallbackLedgerDailyStats(std::shared_ptr<rt::Kv> kv, double dayStartAtMs, double nowMs, Merge merge) {
      const rt::KvKey key = paidFallbackLedgerStatsDayKey(dayStartAtMs);
      rt::KvGetOptions strong;
      strong.consistency = rt::KvConsistencyLevel::strong;
      for (double attempt = 0; attempt < PAID_FALLBACK_LEDGER_STATS_MAX_WRITE_ATTEMPTS; attempt += 1) {
        const rt::KvEntryMaybe<> entry = co_await kv->get(key, strong);
        const rt::KvObject existing = isPaidFallbackLedgerDailyStats(entry.value) ? entry.value.asObject() : statsObject(emptyPaidFallbackLedgerDailyStats(dayStartAtMs));
        rt::KvObject next = merge(existing);
        next.set("v", rt::KvValue(1));
        next.set("day_start_at_ms", rt::KvValue(dayStartAtMs));
        next.set("updated_at_ms", rt::KvValue(nowMs));
        std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
        operation->check(entry).set(key, rt::KvValue(std::move(next)));
        const rt::KvCommitResult committed = co_await operation->commit();
        if (committed.ok) co_return;
      }
    }

    PaidFallbackLedgerGrowthView emptyGrowthView(double nowMs, double retentionDays, double leaderboardDays, std::string scan, double readUnits) {
      std::vector<PaidFallbackLedgerProjectionSummary> projections;
      for (const double windowDays : {7.0, 30.0, 90.0}) {
        projections.push_back(PaidFallbackLedgerProjectionSummary{
          .window_days = windowDays,
          .views = 0,
          .read_units = 0,
          .rollup_rows = 0,
          .avg_read_units_per_view = std::nullopt,
          .avg_rollup_rows_per_view = std::nullopt,
        });
      }
      return PaidFallbackLedgerGrowthView{
        .as_of_ms = nowMs,
        .scan = std::move(scan),
        .read_units = readUnits,
        .retention_days = retentionDays,
        .leaderboard_days = leaderboardDays,
        .leaderboard = {},
        .settled_rows = 0,
        .settled_row_bytes = 0,
        .rollup_writes = 0,
        .rollup_bytes = 0,
        .active_days = 0,
        .avg_row_bytes = std::nullopt,
        .avg_settled_rows_per_active_day = std::nullopt,
        .estimated_retained_rows = 0,
        .estimated_retained_raw_bytes = 0,
        .budget_bytes = PAID_FALLBACK_RAW_STORE_BUDGET_BYTES,
        .alert_ratio = PAID_FALLBACK_RAW_STORE_ALERT_RATIO,
        .alert_threshold_bytes = rt::Math::floor(PAID_FALLBACK_RAW_STORE_BUDGET_BYTES * PAID_FALLBACK_RAW_STORE_ALERT_RATIO),
        .alert = false,
        .projections = std::move(projections),
      };
    }

    double roundToTenths(double value) { return rt::Math::round(value * 10) / 10; }

    const PaidFallbackLedgerProjectionCounters& countersOf(const PaidFallbackLedgerDailyStats& row, std::string_view countersKey) {
      if (countersKey == "projection_7d") return row.projection_7d;
      if (countersKey == "projection_30d") return row.projection_30d;
      return row.projection_90d;
    }

    PaidFallbackLedgerProjectionSummary summarizeWindow(PaidFallbackLedgerProjectionWindowDays windowDays, std::string_view countersKey,
                                                        const std::vector<PaidFallbackLedgerDailyStats>& rows) {
      double views = 0;
      double readUnits = 0;
      double rollupRows = 0;
      for (const PaidFallbackLedgerDailyStats& row : rows) {
        const PaidFallbackLedgerProjectionCounters& counters = countersOf(row, countersKey);
        views += counters.views;
        readUnits += counters.read_units;
        rollupRows += counters.rollup_rows;
      }
      return PaidFallbackLedgerProjectionSummary{
        .window_days = windowDays,
        .views = views,
        .read_units = readUnits,
        .rollup_rows = rollupRows,
        .avg_read_units_per_view = views > 0 ? std::optional<double>(roundToTenths(readUnits / views)) : std::nullopt,
        .avg_rollup_rows_per_view = views > 0 ? std::optional<double>(roundToTenths(rollupRows / views)) : std::nullopt,
      };
    }

    json::value numberOrNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackLedgerProjectionCounters& value) {
    out = Writer().required("views", value.views).required("read_units", value.read_units).required("rollup_rows", value.rollup_rows).done();
  }
  PaidFallbackLedgerProjectionCounters tag_invoke(const json::value_to_tag<PaidFallbackLedgerProjectionCounters>&, const json::value& value) {
    const Reader in(value, "PaidFallbackLedgerProjectionCounters");
    return {.views = in.required<double>("views"), .read_units = in.required<double>("read_units"), .rollup_rows = in.required<double>("rollup_rows")};
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackLedgerDailyStats& value) {
    out = Writer()
            .required("v", value.v)
            .required("day_start_at_ms", value.day_start_at_ms)
            .required("settled_rows", value.settled_rows)
            .required("settled_row_bytes", value.settled_row_bytes)
            .required("rollup_writes", value.rollup_writes)
            .required("rollup_bytes", value.rollup_bytes)
            .required("projection_7d", value.projection_7d)
            .required("projection_30d", value.projection_30d)
            .required("projection_90d", value.projection_90d)
            .required("updated_at_ms", value.updated_at_ms)
            .done();
  }
  PaidFallbackLedgerDailyStats tag_invoke(const json::value_to_tag<PaidFallbackLedgerDailyStats>&, const json::value& value) {
    const Reader in(value, "PaidFallbackLedgerDailyStats");
    return {
      .v = in.required<double>("v"),
      .day_start_at_ms = in.required<double>("day_start_at_ms"),
      .settled_rows = in.required<double>("settled_rows"),
      .settled_row_bytes = in.required<double>("settled_row_bytes"),
      .rollup_writes = in.required<double>("rollup_writes"),
      .rollup_bytes = in.required<double>("rollup_bytes"),
      .projection_7d = in.required<PaidFallbackLedgerProjectionCounters>("projection_7d"),
      .projection_30d = in.required<PaidFallbackLedgerProjectionCounters>("projection_30d"),
      .projection_90d = in.required<PaidFallbackLedgerProjectionCounters>("projection_90d"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  PaidFallbackLedgerDailyStats emptyPaidFallbackLedgerDailyStats(double dayStartAtMs) {
    return PaidFallbackLedgerDailyStats{
      .v = 1,
      .day_start_at_ms = dayStartAtMs,
      .settled_rows = 0,
      .settled_row_bytes = 0,
      .rollup_writes = 0,
      .rollup_bytes = 0,
      .projection_7d = zeroProjectionCounters(),
      .projection_30d = zeroProjectionCounters(),
      .projection_90d = zeroProjectionCounters(),
      .updated_at_ms = 0,
    };
  }

  double paidFallbackLedgerStatsDayStart(double ms) {
    return rt::Math::floor(rt::Math::max(0.0, rt::Math::trunc(ms)) / PAID_FALLBACK_LEDGER_STATS_DAY_MS) * PAID_FALLBACK_LEDGER_STATS_DAY_MS;
  }

  rt::KvKey paidFallbackLedgerStatsDayKey(double dayStartAtMs) { return rt::KvKey{PAID_FALLBACK_LEDGER_STATS_PREFIX, dayStartAtMs}; }

  bool isPaidFallbackLedgerDailyStats(const rt::KvValue& value) {
    if (!rt::kvIsObject(value)) return false;
    return value["v"].isNumber() && value["v"].asNumber() == 1 && isNonNegativeSafeInteger(value["day_start_at_ms"]) && isNonNegativeSafeInteger(value["settled_rows"]) &&
           isNonNegativeSafeInteger(value["settled_row_bytes"]) && isNonNegativeSafeInteger(value["rollup_writes"]) && isNonNegativeSafeInteger(value["rollup_bytes"]) &&
           isProjectionCounters(value["projection_7d"]) && isProjectionCounters(value["projection_30d"]) && isProjectionCounters(value["projection_90d"]) &&
           isNonNegativeSafeInteger(value["updated_at_ms"]);
  }

  double estimatePaidFallbackRecordBytes(const json::value& value) {
    try {
      return static_cast<double>(rt::utf8ByteLength(rt::jsonStringify(value)));
    } catch (...) {
      return 0;
    }
  }

  rt::Task<bool> recordPaidFallbackLedgerSettlementStats(std::shared_ptr<rt::Kv> kv, PaidFallbackLedgerSettlementStatsInput input) {
    if (!kv) co_return false;
    const double nowMs = rt::Math::trunc(input.nowMs ? *input.nowMs : rt::dateNow());
    if (!rt::Number::isSafeInteger(nowMs) || nowMs < 0) co_return false;
    const double settledRowBytes = rt::Math::max(0.0, rt::Math::trunc(input.settledRowBytes));
    const std::optional<double> rollupBytes = input.rollupBytes ? std::optional<double>(rt::Math::max(0.0, rt::Math::trunc(*input.rollupBytes))) : std::nullopt;
    bool merged = true;
    try {
      Merge merge = [settledRowBytes, rollupBytes](const rt::KvObject& stats) {
        rt::KvObject next = stats;
        next.set("settled_rows", rt::KvValue(stats.at("settled_rows").asNumber() + 1));
        next.set("settled_row_bytes", rt::KvValue(stats.at("settled_row_bytes").asNumber() + settledRowBytes));
        next.set("rollup_writes", rt::KvValue(stats.at("rollup_writes").asNumber() + (rollupBytes ? 1 : 0)));
        next.set("rollup_bytes", rt::KvValue(stats.at("rollup_bytes").asNumber() + rollupBytes.value_or(0)));
        return next;
      };
      rt::Task<void> merging = mergePaidFallbackLedgerDailyStats(kv, paidFallbackLedgerStatsDayStart(nowMs), nowMs, std::move(merge));
      co_await std::move(merging);
    } catch (...) {
      merged = false;
    }
    co_return merged;
  }

  rt::Task<bool> recordPaidFallbackLedgerProjectionStats(std::shared_ptr<rt::Kv> kv, PaidFallbackLedgerProjectionStatsInput input) {
    if (!kv) co_return false;
    const double nowMs = rt::Math::trunc(input.nowMs ? *input.nowMs : rt::dateNow());
    if (!rt::Number::isSafeInteger(nowMs) || nowMs < 0) co_return false;
    const double readUnits = rt::Math::max(0.0, rt::Math::trunc(input.readUnits));
    const double rollupRows = rt::Math::max(0.0, rt::Math::trunc(input.rollupRows));
    const std::string countersKey(projectionCountersKey(input.windowDays));
    bool merged = true;
    try {
      Merge merge = [countersKey, readUnits, rollupRows](const rt::KvObject& stats) {
        const rt::KvValue& counters = stats.at(countersKey);
        rt::KvObject next = stats;
        next.set(countersKey, countersValue(PaidFallbackLedgerProjectionCounters{
                                .views = counters["views"].asNumber() + 1,
                                .read_units = counters["read_units"].asNumber() + readUnits,
                                .rollup_rows = counters["rollup_rows"].asNumber() + rollupRows,
                              }));
        return next;
      };
      rt::Task<void> merging = mergePaidFallbackLedgerDailyStats(kv, paidFallbackLedgerStatsDayStart(nowMs), nowMs, std::move(merge));
      co_await std::move(merging);
    } catch (...) {
      merged = false;
    }
    co_return merged;
  }

  json::object toJson(const PaidFallbackLedgerGrowthView& view) {
    json::array leaderboard;
    for (const PaidFallbackLedgerGrowthDay& day : view.leaderboard) {
      json::object entry;
      entry["day_start_at_ms"] = rt::makeNumber(day.day_start_at_ms);
      entry["settled_rows"] = rt::makeNumber(day.settled_rows);
      entry["settled_row_bytes"] = rt::makeNumber(day.settled_row_bytes);
      entry["rollup_writes"] = rt::makeNumber(day.rollup_writes);
      entry["rollup_bytes"] = rt::makeNumber(day.rollup_bytes);
      entry["projection_views"] = rt::makeNumber(day.projection_views);
      entry["projection_read_units"] = rt::makeNumber(day.projection_read_units);
      entry["projection_rollup_rows"] = rt::makeNumber(day.projection_rollup_rows);
      leaderboard.push_back(std::move(entry));
    }
    json::array projections;
    for (const PaidFallbackLedgerProjectionSummary& summary : view.projections) {
      json::object entry;
      entry["window_days"] = rt::makeNumber(summary.window_days);
      entry["views"] = rt::makeNumber(summary.views);
      entry["read_units"] = rt::makeNumber(summary.read_units);
      entry["rollup_rows"] = rt::makeNumber(summary.rollup_rows);
      entry["avg_read_units_per_view"] = numberOrNull(summary.avg_read_units_per_view);
      entry["avg_rollup_rows_per_view"] = numberOrNull(summary.avg_rollup_rows_per_view);
      projections.push_back(std::move(entry));
    }
    json::object out;
    out["as_of_ms"] = rt::makeNumber(view.as_of_ms);
    out["scan"] = view.scan;
    out["read_units"] = rt::makeNumber(view.read_units);
    out["retention_days"] = rt::makeNumber(view.retention_days);
    out["leaderboard_days"] = rt::makeNumber(view.leaderboard_days);
    out["leaderboard"] = std::move(leaderboard);
    out["settled_rows"] = rt::makeNumber(view.settled_rows);
    out["settled_row_bytes"] = rt::makeNumber(view.settled_row_bytes);
    out["rollup_writes"] = rt::makeNumber(view.rollup_writes);
    out["rollup_bytes"] = rt::makeNumber(view.rollup_bytes);
    out["active_days"] = rt::makeNumber(view.active_days);
    out["avg_row_bytes"] = numberOrNull(view.avg_row_bytes);
    out["avg_settled_rows_per_active_day"] = numberOrNull(view.avg_settled_rows_per_active_day);
    out["estimated_retained_rows"] = rt::makeNumber(view.estimated_retained_rows);
    out["estimated_retained_raw_bytes"] = rt::makeNumber(view.estimated_retained_raw_bytes);
    out["budget_bytes"] = rt::makeNumber(view.budget_bytes);
    out["alert_ratio"] = rt::makeNumber(view.alert_ratio);
    out["alert_threshold_bytes"] = rt::makeNumber(view.alert_threshold_bytes);
    out["alert"] = view.alert;
    out["projections"] = std::move(projections);
    return out;
  }

  rt::Task<PaidFallbackLedgerGrowthView> readPaidFallbackLedgerGrowth(std::shared_ptr<rt::Kv> kv, PaidFallbackLedgerGrowthOptions options) {
    const double nowMs = rt::Math::max(0.0, rt::Math::trunc(options.nowMs));
    const double retentionMs = rt::Math::max(PAID_FALLBACK_LEDGER_STATS_DAY_MS, rt::Math::trunc(options.retentionMs));
    const double retentionDays = rt::Math::max(1.0, rt::Math::round(retentionMs / PAID_FALLBACK_LEDGER_STATS_DAY_MS));
    const double leaderboardDays = rt::Math::max(1.0, rt::Math::trunc(options.leaderboardDays.value_or(PAID_FALLBACK_LEDGER_STATS_LEADERBOARD_DAYS)));
    if (!kv) co_return emptyGrowthView(nowMs, retentionDays, leaderboardDays, "unavailable", 0);
    rt::KvListSelector selector;
    selector.start = rt::KvKey{PAID_FALLBACK_LEDGER_STATS_PREFIX, paidFallbackLedgerStatsDayStart(nowMs - retentionMs)};
    selector.end = rt::KvKey{PAID_FALLBACK_LEDGER_STATS_PREFIX, nowMs + PAID_FALLBACK_LEDGER_STATS_DAY_MS};
    std::vector<PaidFallbackLedgerDailyStats> rows;
    bool scanned = true;
    try {
      const std::unique_ptr<rt::KvListIterator> iterator = kv->list(selector);
      while (std::optional<rt::KvEntry<>> entry = co_await iterator->next()) {
        if (isPaidFallbackLedgerDailyStats(entry->value)) rows.push_back(statsFrom(entry->value));
      }
    } catch (...) {
      scanned = false;
    }
    if (!scanned) co_return emptyGrowthView(nowMs, retentionDays, leaderboardDays, "unavailable", 0);

    const double leaderboardStartMs = paidFallbackLedgerStatsDayStart(nowMs - leaderboardDays * PAID_FALLBACK_LEDGER_STATS_DAY_MS);
    std::vector<PaidFallbackLedgerDailyStats> recent;
    for (const PaidFallbackLedgerDailyStats& row : rows)
      if (row.day_start_at_ms >= leaderboardStartMs) recent.push_back(row);
    // `.slice(-leaderboardDays)`: ToIntegerOrInfinity of the start (NaN is 0), then from the end.
    const double relativeStart = rt::Number::isNaN(-leaderboardDays) ? 0 : rt::Math::trunc(-leaderboardDays);
    const double length = static_cast<double>(recent.size());
    const double start = relativeStart < 0 ? rt::Math::max(length + relativeStart, 0.0) : rt::Math::min(relativeStart, length);
    std::vector<PaidFallbackLedgerGrowthDay> leaderboard;
    for (std::size_t index = static_cast<std::size_t>(start); index < recent.size(); ++index) {
      const PaidFallbackLedgerDailyStats& row = recent[index];
      leaderboard.push_back(PaidFallbackLedgerGrowthDay{
        .day_start_at_ms = row.day_start_at_ms,
        .settled_rows = row.settled_rows,
        .settled_row_bytes = row.settled_row_bytes,
        .rollup_writes = row.rollup_writes,
        .rollup_bytes = row.rollup_bytes,
        .projection_views = row.projection_7d.views + row.projection_30d.views + row.projection_90d.views,
        .projection_read_units = row.projection_7d.read_units + row.projection_30d.read_units + row.projection_90d.read_units,
        .projection_rollup_rows = row.projection_7d.rollup_rows + row.projection_30d.rollup_rows + row.projection_90d.rollup_rows,
      });
    }

    double settledRows = 0;
    double settledRowBytes = 0;
    double rollupWrites = 0;
    double rollupBytes = 0;
    double activeDays = 0;
    for (const PaidFallbackLedgerDailyStats& row : rows) {
      settledRows += row.settled_rows;
      settledRowBytes += row.settled_row_bytes;
      rollupWrites += row.rollup_writes;
      rollupBytes += row.rollup_bytes;
      if (row.settled_rows > 0) activeDays += 1;
    }
    const double alertThresholdBytes = rt::Math::floor(PAID_FALLBACK_RAW_STORE_BUDGET_BYTES * PAID_FALLBACK_RAW_STORE_ALERT_RATIO);
    std::vector<PaidFallbackLedgerProjectionSummary> projections;
    projections.push_back(summarizeWindow(7, "projection_7d", rows));
    projections.push_back(summarizeWindow(30, "projection_30d", rows));
    projections.push_back(summarizeWindow(90, "projection_90d", rows));
    PaidFallbackLedgerGrowthView view{
      .as_of_ms = nowMs,
      .scan = "ok",
      .read_units = static_cast<double>(rows.size()) + 1,
      .retention_days = retentionDays,
      .leaderboard_days = leaderboardDays,
      .leaderboard = std::move(leaderboard),
      .settled_rows = settledRows,
      .settled_row_bytes = settledRowBytes,
      .rollup_writes = rollupWrites,
      .rollup_bytes = rollupBytes,
      .active_days = activeDays,
      .avg_row_bytes = settledRows > 0 ? std::optional<double>(rt::Math::round(settledRowBytes / settledRows)) : std::nullopt,
      .avg_settled_rows_per_active_day = activeDays > 0 ? std::optional<double>(roundToTenths(settledRows / activeDays)) : std::nullopt,
      .estimated_retained_rows = settledRows,
      .estimated_retained_raw_bytes = settledRowBytes,
      .budget_bytes = PAID_FALLBACK_RAW_STORE_BUDGET_BYTES,
      .alert_ratio = PAID_FALLBACK_RAW_STORE_ALERT_RATIO,
      .alert_threshold_bytes = alertThresholdBytes,
      .alert = settledRowBytes >= alertThresholdBytes,
      .projections = std::move(projections),
    };
    co_return view;
  }
}
