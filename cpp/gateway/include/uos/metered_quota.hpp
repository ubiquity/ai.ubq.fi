#pragma once
// Port of src/metered-quota.ts.
//
// `kv: Kv` is a non-null std::shared_ptr<rt::Kv>; `kv: Kv | null` a std::shared_ptr<rt::Kv> whose
// null pointer is null; the `kv?: Kv | null` option is std::optional<std::shared_ptr<rt::Kv>>
// (std::nullopt is undefined, which reads getKv()). `fetcher?: MeteredFetch` is rt::Fetch (empty is
// undefined); `now?: () => number` an std::function (empty is Date.now). `options = {}` parameters
// are overloads.
#include "uos/provider/metered.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::metered_quota {
  inline constexpr double METERED_QUOTA_FRESH_MS = 5 * 60'000;
  inline constexpr double METERED_QUOTA_RETENTION_MS = 24.0 * 60 * 60'000;
  inline constexpr double METERED_QUOTA_REFRESH_LEASE_MS = 15'000;
  inline constexpr double METERED_QUOTA_COLD_WAIT_MS = 2'000;
  inline constexpr double METERED_QUOTA_FETCH_TIMEOUT_MS = 10'000;

  inline constexpr std::string_view METERED_API_KEY_ENV = "METERED_API_KEY";

  inline const rt::KvKey METERED_QUOTA_STATE_KEY{"uos_ai", "metered_quota", "v1", "state"};
  inline const rt::KvKey METERED_QUOTA_REFRESH_LEASE_KEY{"uos_ai", "metered_quota", "v1", "refresh_lease"};
  inline const rt::KvKey METERED_QUOTA_INVALIDATION_KEY{"uos_ai", "metered_quota", "v1", "invalidation"};
  inline const rt::KvKey METERED_QUOTA_BALANCE_HISTORY_PREFIX{"uos_ai", "metered_quota", "v1", "balance_history"};
  inline constexpr double METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS = 60.0 * 60 * 1'000;
  inline constexpr double METERED_QUOTA_BALANCE_HISTORY_DAILY_BUCKET_MS = 24 * METERED_QUOTA_BALANCE_HISTORY_BUCKET_MS;
  inline constexpr std::array<double, 4> METERED_QUOTA_BALANCE_WINDOW_DAYS{7, 30, 90, 365};
  using MeteredQuotaBalanceWindowDays = double;  // (typeof METERED_QUOTA_BALANCE_WINDOW_DAYS)[number]: 7 | 30 | 90 | 365

  // `value: string | null`: std::nullopt is null.
  MeteredQuotaBalanceWindowDays normalizeMeteredQuotaBalanceWindowDays(const std::optional<std::string>& value);

  struct MeteredAccountCredentials {
    std::string apiKey;
    friend bool operator==(const MeteredAccountCredentials&, const MeteredAccountCredentials&) = default;
  };

  struct MeteredRefillObservation {
    std::string id;
    double amount_credits;
    double completed_at_ms;
    friend bool operator==(const MeteredRefillObservation&, const MeteredRefillObservation&) = default;
  };

  struct MeteredQuotaObservation {
    std::optional<double> balance_quota;
    std::optional<double> used_quota;
    std::optional<double> quota_per_credit;
    double observed_at_ms;
    std::optional<MeteredRefillObservation> latest_refill;
    std::optional<bool> unlimited_quota{};
    std::optional<double> total_available{};
    std::optional<double> total_granted{};
    std::optional<double> total_used{};
    friend bool operator==(const MeteredQuotaObservation&, const MeteredQuotaObservation&) = default;
  };

  using MeteredQuotaConfidence = std::string;  // "provisional" | "refill_observed" | "inferred_adjustment"

  struct MeteredQuotaState {
    double current_balance_quota;
    double post_refill_baseline_quota;
    double last_observed_used_quota;
    double quota_per_credit;
    double observed_at_ms;
    double cycle_started_at_ms;
    MeteredQuotaConfidence confidence;
    double last_known_debits_quota;
    double last_inferred_credit_quota;
    std::optional<double> last_credit_at_ms;
    std::optional<std::string> latest_refill_id;
    std::optional<double> latest_refill_amount_credits;
    std::optional<double> latest_refill_completed_at_ms;
    std::optional<bool> unlimited_quota{};
    rt::Nullable<double> total_available{};
    rt::Nullable<double> total_granted{};
    rt::Nullable<double> total_used{};
    friend bool operator==(const MeteredQuotaState&, const MeteredQuotaState&) = default;
  };

  using MeteredQuotaCacheState = std::string;  // "fresh" | "refreshed" | "stale" | "wait"

  struct MeteredQuotaBalanceSample {
    double v = 1;
    double bucket_start_at_ms;
    double observed_at_ms;
    double balance_quota;
    double baseline_quota;
    double quota_per_credit;
    std::optional<double> remaining_percent;
    std::optional<bool> unlimited_quota{};
    rt::Nullable<double> total_available{};
    rt::Nullable<double> total_granted{};
    rt::Nullable<double> total_used{};
    friend bool operator==(const MeteredQuotaBalanceSample&, const MeteredQuotaBalanceSample&) = default;
  };

  struct MeteredQuotaSnapshot {
    MeteredQuotaState state;
    MeteredQuotaCacheState cache_state;
    std::optional<double> balance_credits;
    std::optional<double> baseline_credits;
    std::optional<double> last_inferred_credit_credits;
    std::optional<double> remaining_percent;
    std::optional<double> used_percent;
    bool unlimited_quota;
    std::optional<double> total_available;
    std::optional<double> total_granted;
    std::optional<double> total_used;
    friend bool operator==(const MeteredQuotaSnapshot&, const MeteredQuotaSnapshot&) = default;
  };

  struct MeteredQuotaDiagnostics {
    bool configured;
    bool available;
    std::optional<MeteredQuotaCacheState> cache_state;
    std::optional<MeteredQuotaConfidence> confidence;
    std::optional<double> balance_credits;
    std::optional<double> baseline_credits;
    std::optional<double> remaining_percent;
    std::optional<double> used_percent;
    std::optional<double> observed_at_ms;
    std::optional<double> cycle_started_at_ms;
    std::optional<double> last_known_debits_credits;
    std::optional<double> last_inferred_credit_credits;
    std::optional<double> last_credit_at_ms;
    std::optional<std::string> latest_refill_id;
    std::optional<double> latest_refill_amount_credits;
    std::optional<double> latest_refill_completed_at_ms;
    rt::Nullable<bool> unlimited_quota{};
    rt::Nullable<double> total_available{};
    rt::Nullable<double> total_granted{};
    rt::Nullable<double> total_used{};
    friend bool operator==(const MeteredQuotaDiagnostics&, const MeteredQuotaDiagnostics&) = default;
  };

  struct GetMeteredQuotaSnapshotOptions {
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    provider::metered::MeteredFetch fetcher{};
    std::function<double()> now{};
    std::optional<rt::AbortSignal> signal{};
    std::optional<bool> forceRefresh{};
    // `createLeaseOwner?: () => string`: an empty function is undefined (crypto.randomUUID()).
    std::function<std::string()> createLeaseOwner{};
  };

  // `Pick<GetMeteredQuotaSnapshotOptions, "kv" | "now">`, the options of getCachedMeteredQuotaSnapshot,
  // invalidateMeteredQuotaSnapshot and getCachedConfiguredMeteredQuotaSnapshot.
  struct GetCachedMeteredQuotaSnapshotOptions {
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    std::function<double()> now{};
  };

  // std::nullopt is null.
  std::optional<MeteredAccountCredentials> readMeteredAccountCredentials();

  /**
   * Non-secret fingerprint of the configured OpenLux account. The balance
   * history is namespaced by it so switching METERED_API_KEY starts a fresh
   * curve per account instead of corrupting the retained run-down history.
   */
  // `credentials: MeteredAccountCredentials | null`; std::nullopt is null.
  rt::Task<std::optional<std::string>> meterQuotaAccountFingerprint(std::optional<MeteredAccountCredentials> credentials);

  // The `options` of fetchMeteredQuotaObservation.
  struct FetchMeteredQuotaObservationOptions {
    provider::metered::MeteredFetch fetcher{};
    std::function<double()> now{};
    std::optional<rt::AbortSignal> signal{};
  };

  rt::Task<MeteredQuotaObservation> fetchMeteredQuotaObservation(MeteredAccountCredentials credentialsInput);
  rt::Task<MeteredQuotaObservation> fetchMeteredQuotaObservation(MeteredAccountCredentials credentialsInput, FetchMeteredQuotaObservationOptions options);

  // `previous: MeteredQuotaState | null`: std::nullopt is null.
  MeteredQuotaState updateMeteredQuotaState(const std::optional<MeteredQuotaState>& previous, const MeteredQuotaObservation& observation);

  // `(value: unknown): value is MeteredQuotaState`: nullptr is undefined; the rt::KvValue overload
  // checks a stored value as read. A json::object argument is ambiguous between the json::value
  // and rt::KvValue overloads: pass it as json::value.
  bool isMeteredQuotaState(const json::value* value);
  bool isMeteredQuotaState(const json::value& value);
  bool isMeteredQuotaState(const rt::KvValue& value);

  // std::nullopt is null.
  rt::Task<std::optional<MeteredQuotaSnapshot>> getMeteredQuotaSnapshot(MeteredAccountCredentials credentials);
  rt::Task<std::optional<MeteredQuotaSnapshot>> getMeteredQuotaSnapshot(MeteredAccountCredentials credentials, GetMeteredQuotaSnapshotOptions options);

  rt::Task<std::optional<MeteredQuotaSnapshot>> getCachedMeteredQuotaSnapshot();
  rt::Task<std::optional<MeteredQuotaSnapshot>> getCachedMeteredQuotaSnapshot(GetCachedMeteredQuotaSnapshotOptions options);

  rt::Task<void> invalidateMeteredQuotaSnapshot();
  rt::Task<void> invalidateMeteredQuotaSnapshot(GetCachedMeteredQuotaSnapshotOptions options);

  /**
   * Upserts one hourly balance sample. Best-effort: a failure must never fail
   * a quota refresh. Keeps the newest observation in each hour bucket so a
   * later refresh never leaves the hour's oldest balance on the run-down curve.
   * Reader is readMeteredQuotaBalanceHistory.
   */
  // `accountFingerprint: string | null`: std::nullopt is null.
  rt::Task<void> writeMeteredQuotaBalanceSample(std::shared_ptr<rt::Kv> kv, MeteredQuotaState state, double observedAtMs,
                                                std::optional<std::string> accountFingerprint);

  // The `options` of readMeteredQuotaBalanceHistory.
  struct ReadMeteredQuotaBalanceHistoryOptions {
    double sinceMs;
    double nowMs;
    std::optional<std::string> accountFingerprint;
    std::optional<double> limit{};
  };

  rt::Task<std::vector<MeteredQuotaBalanceSample>> readMeteredQuotaBalanceHistory(std::shared_ptr<rt::Kv> kv, ReadMeteredQuotaBalanceHistoryOptions options);

  /**
   * Deterministically keeps the newest observation in each UTC bucket. The
   * returned bucket timestamp describes the resampled series rather than the
   * source hour, while observed_at_ms retains the exact observation time.
   */
  std::vector<MeteredQuotaBalanceSample> resampleMeteredQuotaBalanceHistory(const std::vector<MeteredQuotaBalanceSample>& samples, double bucketMs, double limit);

  rt::Task<std::optional<MeteredQuotaSnapshot>> getConfiguredMeteredQuotaSnapshot();
  rt::Task<std::optional<MeteredQuotaSnapshot>> getConfiguredMeteredQuotaSnapshot(GetMeteredQuotaSnapshotOptions options);

  rt::Task<std::optional<MeteredQuotaSnapshot>> getCachedConfiguredMeteredQuotaSnapshot();
  rt::Task<std::optional<MeteredQuotaSnapshot>> getCachedConfiguredMeteredQuotaSnapshot(GetCachedMeteredQuotaSnapshotOptions options);

  rt::Task<MeteredQuotaDiagnostics> getMeteredQuotaDiagnostics();
  rt::Task<MeteredQuotaDiagnostics> getMeteredQuotaDiagnostics(GetMeteredQuotaSnapshotOptions options);

  // ---- JSON conversions of the snake_case records (members in declaration order) ----------
  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredRefillObservation& value);
  MeteredRefillObservation tag_invoke(const json::value_to_tag<MeteredRefillObservation>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredQuotaObservation& value);
  MeteredQuotaObservation tag_invoke(const json::value_to_tag<MeteredQuotaObservation>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredQuotaState& value);
  MeteredQuotaState tag_invoke(const json::value_to_tag<MeteredQuotaState>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredQuotaBalanceSample& value);
  MeteredQuotaBalanceSample tag_invoke(const json::value_to_tag<MeteredQuotaBalanceSample>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredQuotaSnapshot& value);
  MeteredQuotaSnapshot tag_invoke(const json::value_to_tag<MeteredQuotaSnapshot>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredQuotaDiagnostics& value);
  MeteredQuotaDiagnostics tag_invoke(const json::value_to_tag<MeteredQuotaDiagnostics>&, const json::value& value);
}
