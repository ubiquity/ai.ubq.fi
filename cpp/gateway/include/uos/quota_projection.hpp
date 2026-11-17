#pragma once
// Port of src/quota-projection.ts.
#include "uos/metered_quota.hpp"
#include "uos/paid_fallback/rollups.hpp"
#include <array>
#include <optional>
#include <string>
#include <vector>

namespace uos::quota_projection {
  inline constexpr std::array<double, 3> QUOTA_PROJECTION_WINDOW_DAYS{7, 30, 90};
  using QuotaProjectionWindowDays = double;
  struct MeteredQuotaRunwayView {
    bool configured;
    bool available;
    std::optional<std::string> cache_state;
    std::optional<std::string> confidence;
    bool unlimited_quota;
    std::optional<double> balance_quota;
    std::optional<double> baseline_quota;
    std::optional<double> quota_per_credit;
    std::optional<double> balance_credits;
    std::optional<double> baseline_credits;
    std::optional<double> remaining_percent;
    std::optional<double> used_percent;
    std::optional<double> total_available;
    std::optional<double> total_granted;
    std::optional<double> total_used;
    std::optional<double> observed_at_ms;
    std::optional<double> cycle_started_at_ms;
    std::optional<double> last_credit_at_ms;
    std::optional<double> last_inferred_credit_quota;
    std::optional<double> latest_refill_amount_credits;
    std::optional<double> latest_refill_completed_at_ms;
    friend bool operator==(const MeteredQuotaRunwayView&, const MeteredQuotaRunwayView&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredQuotaRunwayView& value);
  MeteredQuotaRunwayView tag_invoke(const json::value_to_tag<MeteredQuotaRunwayView>&, const json::value& value);
  struct PaidFallbackModelSeries {
    std::string model;
    std::string provider;
    std::vector<paid_fallback::rollups::PaidFallbackUsageRollup> buckets;
    friend bool operator==(const PaidFallbackModelSeries&, const PaidFallbackModelSeries&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackModelSeries& value);
  PaidFallbackModelSeries tag_invoke(const json::value_to_tag<PaidFallbackModelSeries>&, const json::value& value);
  struct PaidFallbackModelUsageWindow {
    QuotaProjectionWindowDays window_days;
    double bucket_count;
    double request_count;
    double quota_sum;
    double spend_microcredits;
    double input_tokens;
    double cached_input_tokens;
    double output_tokens;
    std::optional<double> avg_quota_per_request;
    std::optional<double> quota_per_hour;
    std::optional<double> first_bucket_start_at_ms;
    std::optional<double> last_bucket_start_at_ms;
    friend bool operator==(const PaidFallbackModelUsageWindow&, const PaidFallbackModelUsageWindow&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackModelUsageWindow& value);
  PaidFallbackModelUsageWindow tag_invoke(const json::value_to_tag<PaidFallbackModelUsageWindow>&, const json::value& value);
  struct PaidFallbackModelUsage {
    std::string model;
    std::string provider;
    std::vector<PaidFallbackModelUsageWindow> windows;
    friend bool operator==(const PaidFallbackModelUsage&, const PaidFallbackModelUsage&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackModelUsage& value);
  PaidFallbackModelUsage tag_invoke(const json::value_to_tag<PaidFallbackModelUsage>&, const json::value& value);
  struct PaidFallbackRunwayEstimate {
    QuotaProjectionWindowDays window_days;
    bool unlimited;
    bool stale_balance;
    std::optional<double> requests_remaining;
    std::optional<double> time_remaining_ms;
    std::string refill_aware_status;
    std::optional<double> refill_aware_time_remaining_ms;
    std::optional<double> refill_aware_exhausted_at_ms;
    std::optional<double> refill_amount_credits;
    std::optional<double> refill_cadence_ms;
    std::optional<double> refill_rate_quota_per_hour;
    std::optional<double> next_refill_at_ms;
    std::optional<double> exhausted_at_ms;
    std::optional<double> percent_per_request_vs_balance;
    std::optional<double> percent_per_request_vs_baseline;
    friend bool operator==(const PaidFallbackRunwayEstimate&, const PaidFallbackRunwayEstimate&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackRunwayEstimate& value);
  PaidFallbackRunwayEstimate tag_invoke(const json::value_to_tag<PaidFallbackRunwayEstimate>&, const json::value& value);
  MeteredQuotaRunwayView meteredQuotaRunwayView(const std::optional<metered_quota::MeteredQuotaSnapshot>& snapshot);
  std::vector<PaidFallbackModelSeries> groupPaidFallbackUsageRollups(const std::vector<paid_fallback::rollups::PaidFallbackUsageRollup>& rollups);
  std::vector<PaidFallbackModelUsage> summarizePaidFallbackUsage(const std::vector<PaidFallbackModelSeries>& series, double nowMs);
  std::vector<PaidFallbackRunwayEstimate> projectPaidFallbackRunway(const PaidFallbackModelUsage& usage, const MeteredQuotaRunwayView& quota, double nowMs);
  std::vector<PaidFallbackRunwayEstimate> projectPaidFallbackRunway(const PaidFallbackModelUsage& usage, const MeteredQuotaRunwayView& quota, double nowMs, const std::optional<std::vector<metered_quota::MeteredQuotaBalanceSample>>& balanceHistory);
}
