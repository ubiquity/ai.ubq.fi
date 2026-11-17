#include "uos/quota_projection.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include <algorithm>
#include <set>

namespace uos::quota_projection {
  void tag_invoke(const json::value_from_tag&, json::value& out, const MeteredQuotaRunwayView& value) {
    out = rt::record::Writer()
      .required("configured", value.configured)
      .required("available", value.available)
      .orNull("cache_state", value.cache_state)
      .orNull("confidence", value.confidence)
      .required("unlimited_quota", value.unlimited_quota)
      .orNull("balance_quota", value.balance_quota)
      .orNull("baseline_quota", value.baseline_quota)
      .orNull("quota_per_credit", value.quota_per_credit)
      .orNull("balance_credits", value.balance_credits)
      .orNull("baseline_credits", value.baseline_credits)
      .orNull("remaining_percent", value.remaining_percent)
      .orNull("used_percent", value.used_percent)
      .orNull("total_available", value.total_available)
      .orNull("total_granted", value.total_granted)
      .orNull("total_used", value.total_used)
      .orNull("observed_at_ms", value.observed_at_ms)
      .orNull("cycle_started_at_ms", value.cycle_started_at_ms)
      .orNull("last_credit_at_ms", value.last_credit_at_ms)
      .orNull("last_inferred_credit_quota", value.last_inferred_credit_quota)
      .orNull("latest_refill_amount_credits", value.latest_refill_amount_credits)
      .orNull("latest_refill_completed_at_ms", value.latest_refill_completed_at_ms)
      .done();
  }
  MeteredQuotaRunwayView tag_invoke(const json::value_to_tag<MeteredQuotaRunwayView>&, const json::value& value) {
    const rt::record::Reader in(value, "MeteredQuotaRunwayView");
    return {
      .configured = in.required<bool>("configured"),
      .available = in.required<bool>("available"),
      .cache_state = in.orNull<std::string>("cache_state"),
      .confidence = in.orNull<std::string>("confidence"),
      .unlimited_quota = in.required<bool>("unlimited_quota"),
      .balance_quota = in.orNull<double>("balance_quota"),
      .baseline_quota = in.orNull<double>("baseline_quota"),
      .quota_per_credit = in.orNull<double>("quota_per_credit"),
      .balance_credits = in.orNull<double>("balance_credits"),
      .baseline_credits = in.orNull<double>("baseline_credits"),
      .remaining_percent = in.orNull<double>("remaining_percent"),
      .used_percent = in.orNull<double>("used_percent"),
      .total_available = in.orNull<double>("total_available"),
      .total_granted = in.orNull<double>("total_granted"),
      .total_used = in.orNull<double>("total_used"),
      .observed_at_ms = in.orNull<double>("observed_at_ms"),
      .cycle_started_at_ms = in.orNull<double>("cycle_started_at_ms"),
      .last_credit_at_ms = in.orNull<double>("last_credit_at_ms"),
      .last_inferred_credit_quota = in.orNull<double>("last_inferred_credit_quota"),
      .latest_refill_amount_credits = in.orNull<double>("latest_refill_amount_credits"),
      .latest_refill_completed_at_ms = in.orNull<double>("latest_refill_completed_at_ms"),
    };
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackModelSeries& value) {
    out = rt::record::Writer()
      .required("model", value.model)
      .required("provider", value.provider)
      .required("buckets", value.buckets)
      .done();
  }
  PaidFallbackModelSeries tag_invoke(const json::value_to_tag<PaidFallbackModelSeries>&, const json::value& value) {
    const rt::record::Reader in(value, "PaidFallbackModelSeries");
    return {
      .model = in.required<std::string>("model"),
      .provider = in.required<std::string>("provider"),
      .buckets = in.required<std::vector<paid_fallback::rollups::PaidFallbackUsageRollup>>("buckets"),
    };
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackModelUsageWindow& value) {
    out = rt::record::Writer()
      .required("window_days", value.window_days)
      .required("bucket_count", value.bucket_count)
      .required("request_count", value.request_count)
      .required("quota_sum", value.quota_sum)
      .required("spend_microcredits", value.spend_microcredits)
      .required("input_tokens", value.input_tokens)
      .required("cached_input_tokens", value.cached_input_tokens)
      .required("output_tokens", value.output_tokens)
      .orNull("avg_quota_per_request", value.avg_quota_per_request)
      .orNull("quota_per_hour", value.quota_per_hour)
      .orNull("first_bucket_start_at_ms", value.first_bucket_start_at_ms)
      .orNull("last_bucket_start_at_ms", value.last_bucket_start_at_ms)
      .done();
  }
  PaidFallbackModelUsageWindow tag_invoke(const json::value_to_tag<PaidFallbackModelUsageWindow>&, const json::value& value) {
    const rt::record::Reader in(value, "PaidFallbackModelUsageWindow");
    return {
      .window_days = in.required<QuotaProjectionWindowDays>("window_days"),
      .bucket_count = in.required<double>("bucket_count"),
      .request_count = in.required<double>("request_count"),
      .quota_sum = in.required<double>("quota_sum"),
      .spend_microcredits = in.required<double>("spend_microcredits"),
      .input_tokens = in.required<double>("input_tokens"),
      .cached_input_tokens = in.required<double>("cached_input_tokens"),
      .output_tokens = in.required<double>("output_tokens"),
      .avg_quota_per_request = in.orNull<double>("avg_quota_per_request"),
      .quota_per_hour = in.orNull<double>("quota_per_hour"),
      .first_bucket_start_at_ms = in.orNull<double>("first_bucket_start_at_ms"),
      .last_bucket_start_at_ms = in.orNull<double>("last_bucket_start_at_ms"),
    };
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackModelUsage& value) {
    out = rt::record::Writer()
      .required("model", value.model)
      .required("provider", value.provider)
      .required("windows", value.windows)
      .done();
  }
  PaidFallbackModelUsage tag_invoke(const json::value_to_tag<PaidFallbackModelUsage>&, const json::value& value) {
    const rt::record::Reader in(value, "PaidFallbackModelUsage");
    return {
      .model = in.required<std::string>("model"),
      .provider = in.required<std::string>("provider"),
      .windows = in.required<std::vector<PaidFallbackModelUsageWindow>>("windows"),
    };
  }
  void tag_invoke(const json::value_from_tag&, json::value& out, const PaidFallbackRunwayEstimate& value) {
    out = rt::record::Writer()
      .required("window_days", value.window_days)
      .required("unlimited", value.unlimited)
      .required("stale_balance", value.stale_balance)
      .orNull("requests_remaining", value.requests_remaining)
      .orNull("time_remaining_ms", value.time_remaining_ms)
      .required("refill_aware_status", value.refill_aware_status)
      .orNull("refill_aware_time_remaining_ms", value.refill_aware_time_remaining_ms)
      .orNull("refill_aware_exhausted_at_ms", value.refill_aware_exhausted_at_ms)
      .orNull("refill_amount_credits", value.refill_amount_credits)
      .orNull("refill_cadence_ms", value.refill_cadence_ms)
      .orNull("refill_rate_quota_per_hour", value.refill_rate_quota_per_hour)
      .orNull("next_refill_at_ms", value.next_refill_at_ms)
      .orNull("exhausted_at_ms", value.exhausted_at_ms)
      .orNull("percent_per_request_vs_balance", value.percent_per_request_vs_balance)
      .orNull("percent_per_request_vs_baseline", value.percent_per_request_vs_baseline)
      .done();
  }
  PaidFallbackRunwayEstimate tag_invoke(const json::value_to_tag<PaidFallbackRunwayEstimate>&, const json::value& value) {
    const rt::record::Reader in(value, "PaidFallbackRunwayEstimate");
    return {
      .window_days = in.required<QuotaProjectionWindowDays>("window_days"),
      .unlimited = in.required<bool>("unlimited"),
      .stale_balance = in.required<bool>("stale_balance"),
      .requests_remaining = in.orNull<double>("requests_remaining"),
      .time_remaining_ms = in.orNull<double>("time_remaining_ms"),
      .refill_aware_status = in.required<std::string>("refill_aware_status"),
      .refill_aware_time_remaining_ms = in.orNull<double>("refill_aware_time_remaining_ms"),
      .refill_aware_exhausted_at_ms = in.orNull<double>("refill_aware_exhausted_at_ms"),
      .refill_amount_credits = in.orNull<double>("refill_amount_credits"),
      .refill_cadence_ms = in.orNull<double>("refill_cadence_ms"),
      .refill_rate_quota_per_hour = in.orNull<double>("refill_rate_quota_per_hour"),
      .next_refill_at_ms = in.orNull<double>("next_refill_at_ms"),
      .exhausted_at_ms = in.orNull<double>("exhausted_at_ms"),
      .percent_per_request_vs_balance = in.orNull<double>("percent_per_request_vs_balance"),
      .percent_per_request_vs_baseline = in.orNull<double>("percent_per_request_vs_baseline"),
    };
  }
  namespace {
    constexpr double DAY_MS = 24.0 * 60 * 60 * 1'000;
    constexpr double HOUR_MS = 60.0 * 60 * 1'000;
    bool positiveFinite(std::optional<double> value) { return value && rt::Number::isFinite(*value) && *value > 0; }
    bool nonNegativeFinite(std::optional<double> value) { return value && rt::Number::isFinite(*value) && *value >= 0; }
    // The rate covers the entire selected window, idle hours included.
    PaidFallbackModelUsageWindow summarizeWindow(const std::vector<paid_fallback::rollups::PaidFallbackUsageRollup>& buckets, double windowDays, double nowMs) {
      const double windowStartMs = rt::Math::floor((nowMs - windowDays * DAY_MS) / HOUR_MS) * HOUR_MS;
      PaidFallbackModelUsageWindow result{};
      result.window_days = windowDays;
      std::set<double> hours;
      for (const auto& bucket : buckets) {
        if (!(bucket.bucket_start_at_ms >= windowStartMs)) continue;
        hours.insert(bucket.bucket_start_at_ms);
        result.request_count += bucket.request_count;
        result.quota_sum += bucket.quota_sum;
        result.spend_microcredits += bucket.spend_microcredits;
        result.input_tokens += bucket.input_tokens;
        result.cached_input_tokens += bucket.cached_input_tokens;
        result.output_tokens += bucket.output_tokens;
        if (!result.first_bucket_start_at_ms) result.first_bucket_start_at_ms = bucket.bucket_start_at_ms;
        result.last_bucket_start_at_ms = bucket.bucket_start_at_ms;
      }
      result.bucket_count = static_cast<double>(hours.size());
      if (result.request_count > 0) result.avg_quota_per_request = result.quota_sum / result.request_count;
      if (windowDays * 24 > 0) result.quota_per_hour = result.quota_sum / (windowDays * 24);
      return result;
    }
    std::optional<double> runwayBalance(const MeteredQuotaRunwayView& quota) {
      if (quota.unlimited_quota) return std::nullopt;
      if (quota.balance_quota) return rt::Math::max(0, *quota.balance_quota);
      if (quota.total_available) return rt::Math::max(0, *quota.total_available);
      return std::nullopt;
    }
    std::optional<double> runwayBaseline(const MeteredQuotaRunwayView& quota) {
      if (quota.unlimited_quota) return std::nullopt;
      if (quota.baseline_quota) return rt::Math::max(0, *quota.baseline_quota);
      return quota.total_granted && *quota.total_granted > 0 ? quota.total_granted : std::nullopt;
    }
    struct RefillProjection {
      std::string status = "unknown";
      std::optional<double> timeRemainingMs{};
      std::optional<double> exhaustedAtMs{};
      std::optional<double> amountCredits{};
      std::optional<double> cadenceMs{};
      std::optional<double> rateQuotaPerHour{};
      std::optional<double> nextRefillAtMs{};
    };
    struct RefillEvent { double atMs; };
    std::optional<double> median(std::vector<double> values) {
      if (values.empty()) return std::nullopt;
      rt::sort(values, [](double left, double right) { return left - right; });
      const std::size_t middle = values.size() / 2;
      return values.size() % 2 == 1 ? values[middle] : (values[middle - 1] + values[middle]) / 2;
    }
    // Positive balance changes locate possible refill events, but their net amounts omit intervening debits.
    // Only current credit observations supply amounts; retained history supplies event times for cadence.
    std::vector<RefillEvent> refillEventsFromHistory(const std::vector<metered_quota::MeteredQuotaBalanceSample>& samples, double nowMs) {
      std::vector<metered_quota::MeteredQuotaBalanceSample> ordered;
      for (const auto& sample : samples)
        if (nonNegativeFinite(sample.observed_at_ms) && sample.observed_at_ms <= nowMs && nonNegativeFinite(sample.balance_quota)) ordered.push_back(sample);
      rt::sort(ordered, [](const auto& left, const auto& right) {
        const double difference = left.observed_at_ms - right.observed_at_ms;
        return rt::truthy(difference) ? difference : left.bucket_start_at_ms - right.bucket_start_at_ms;
      });
      std::vector<RefillEvent> events;
      for (std::size_t i = 1; i < ordered.size(); ++i) {
        const auto& sample = ordered[i]; const auto& previous = ordered[i - 1];
        const double balanceIncrease = sample.balance_quota - previous.balance_quota;
        const double baselineIncrease = nonNegativeFinite(sample.baseline_quota) && nonNegativeFinite(previous.baseline_quota) ? sample.baseline_quota - previous.baseline_quota : 0;
        if (rt::Math::max(balanceIncrease, baselineIncrease) > 0) events.push_back({sample.observed_at_ms});
      }
      return events;
    }
    void mergeObservedRefillEvent(std::vector<RefillEvent>& events, double observedRefillAtMs) {
      std::erase_if(events, [observedRefillAtMs](const auto& event) { return rt::Math::abs(event.atMs - observedRefillAtMs) <= HOUR_MS; });
      events.push_back({observedRefillAtMs});
    }
    std::vector<RefillEvent> refillEvents(const MeteredQuotaRunwayView& quota, const std::optional<std::vector<metered_quota::MeteredQuotaBalanceSample>>& history, double nowMs) {
      auto events = history ? refillEventsFromHistory(*history, nowMs) : std::vector<RefillEvent>{};
      const auto inferredCreditAtMs = quota.confidence == "inferred_adjustment" ? quota.last_credit_at_ms : std::nullopt;
      const bool cycleStartIsRefill = quota.latest_refill_completed_at_ms || positiveFinite(quota.latest_refill_amount_credits) || quota.confidence == "refill_observed" || quota.confidence == "inferred_adjustment";
      const std::array<std::optional<double>, 3> observed{cycleStartIsRefill ? quota.cycle_started_at_ms : std::nullopt, inferredCreditAtMs, quota.latest_refill_completed_at_ms};
      for (const auto atMs : observed) if (nonNegativeFinite(atMs) && *atMs <= nowMs) mergeObservedRefillEvent(events, *atMs);
      rt::sort(events, [](const auto& left, const auto& right) { return left.atMs - right.atMs; });
      return events;
    }
    struct RefillAmount { std::optional<double> amountQuota; std::optional<double> amountCredits; };
    RefillAmount refillAmount(const MeteredQuotaRunwayView& quota) {
      const bool inferredIsLatest = quota.confidence == "inferred_adjustment" && (!quota.latest_refill_completed_at_ms || (quota.last_credit_at_ms && *quota.last_credit_at_ms > *quota.latest_refill_completed_at_ms));
      const auto observedAmountQuota = positiveFinite(quota.latest_refill_amount_credits) && positiveFinite(quota.quota_per_credit) ? std::optional<double>(*quota.latest_refill_amount_credits * *quota.quota_per_credit) : std::nullopt;
      const auto amountFromObservation = positiveFinite(observedAmountQuota) ? observedAmountQuota : std::nullopt;
      const auto inferredAmountQuota = positiveFinite(quota.last_inferred_credit_quota) ? quota.last_inferred_credit_quota : std::nullopt;
      const auto amountQuota = inferredIsLatest ? inferredAmountQuota : amountFromObservation;
      return {amountQuota, amountQuota && positiveFinite(quota.quota_per_credit) ? std::optional<double>(*amountQuota / *quota.quota_per_credit) : std::nullopt};
    }
    struct RefillCadence { std::optional<double> cadenceMs; std::optional<double> rateQuotaPerHour; };
    RefillCadence refillCadence(const std::vector<RefillEvent>& events, std::optional<double> amountQuota) {
      std::vector<double> intervals;
      for (std::size_t i = 1; i < events.size(); ++i) { const double interval = events[i].atMs - events[i - 1].atMs; if (positiveFinite(interval)) intervals.push_back(interval); }
      const auto cadenceMs = median(intervals);
      const auto calculated = amountQuota && cadenceMs ? std::optional<double>(*amountQuota / (*cadenceMs / HOUR_MS)) : std::nullopt;
      return {cadenceMs, positiveFinite(calculated) ? calculated : std::nullopt};
    }
    std::optional<double> nextRefillAt(const std::vector<RefillEvent>& events, double cadenceMs, double nowMs) {
      if (events.empty()) return std::nullopt;
      const double latest = events.back().atMs;
      double periods = 0;
      if (latest < nowMs) periods = rt::Math::ceil((nowMs - latest) / cadenceMs); else if (latest == nowMs) periods = 1;
      return latest + periods * cadenceMs;
    }
    RefillProjection refillProjection(const MeteredQuotaRunwayView& quota, std::optional<double> balanceQuota, std::optional<double> burnQuotaPerHour, const std::optional<std::vector<metered_quota::MeteredQuotaBalanceSample>>& history, double nowMs) {
      RefillProjection empty;
      // Finite token inventory has no wallet refill cycle, even when old wallet samples remain.
      if (!quota.balance_quota) return empty;
      const auto events = refillEvents(quota, history, nowMs);
      const auto [amountQuota, amountCredits] = refillAmount(quota);
      const auto [cadenceMs, rateQuotaPerHour] = refillCadence(events, amountQuota);
      RefillProjection unknown = empty; unknown.amountCredits = amountCredits; unknown.cadenceMs = cadenceMs; unknown.rateQuotaPerHour = rateQuotaPerHour;
      if (!balanceQuota || !positiveFinite(burnQuotaPerHour)) return unknown;
      const double noRefillTime = rt::Math::trunc((*balanceQuota / *burnQuotaPerHour) * HOUR_MS);
      RefillProjection noRefill = unknown; noRefill.status = "exhausts_without_refill"; noRefill.timeRemainingMs = noRefillTime; noRefill.exhaustedAtMs = rt::Math::trunc(nowMs + noRefillTime);
      if (!events.empty() && !amountQuota) return unknown;
      if (!cadenceMs || !amountQuota || !positiveFinite(rateQuotaPerHour)) return history ? noRefill : unknown;
      const auto next = nextRefillAt(events, *cadenceMs, nowMs);
      if (!next) return history ? noRefill : unknown;
      const double hoursUntilNext = rt::Math::max(0, (*next - nowMs) / HOUR_MS);
      const double balanceAtNext = *balanceQuota - *burnQuotaPerHour * hoursUntilNext;
      if (balanceAtNext < 0) { noRefill.nextRefillAtMs = next; return noRefill; }
      const double cycleBurn = *burnQuotaPerHour * (*cadenceMs / HOUR_MS);
      if (*rateQuotaPerHour >= *burnQuotaPerHour) return {"sustainable", std::nullopt, std::nullopt, amountCredits, cadenceMs, rateQuotaPerHour, next};
      const double balanceAfterFirst = balanceAtNext + *amountQuota;
      const double cycleLoss = cycleBurn - *amountQuota;
      const double cycles = balanceAfterFirst < cycleBurn ? 0 : rt::Math::floor((balanceAfterFirst - cycleBurn) / cycleLoss) + 1;
      const double balanceBefore = balanceAfterFirst - cycles * cycleLoss;
      const double exhausted = rt::Math::trunc(*next + cycles * *cadenceMs + (balanceBefore / *burnQuotaPerHour) * HOUR_MS);
      return {"exhausts_with_refill", rt::Math::max(0, exhausted - nowMs), exhausted, amountCredits, cadenceMs, rateQuotaPerHour, next};
    }
  }
  MeteredQuotaRunwayView meteredQuotaRunwayView(const std::optional<metered_quota::MeteredQuotaSnapshot>& snapshot) {
    MeteredQuotaRunwayView result{};
    if (!snapshot) return result;
    const auto& state = snapshot->state;
    const bool tokenUsage = snapshot->unlimited_quota || snapshot->total_available || snapshot->total_used;
    result.configured = true; result.available = true; result.cache_state = snapshot->cache_state;
    result.confidence = tokenUsage ? std::nullopt : std::optional<std::string>(state.confidence);
    result.unlimited_quota = tokenUsage ? snapshot->unlimited_quota : false;
    result.balance_quota = tokenUsage ? std::nullopt : std::optional<double>(state.current_balance_quota);
    result.baseline_quota = tokenUsage ? std::nullopt : std::optional<double>(state.post_refill_baseline_quota);
    result.quota_per_credit = state.quota_per_credit;
    result.balance_credits = snapshot->balance_credits; result.baseline_credits = snapshot->baseline_credits;
    result.remaining_percent = snapshot->remaining_percent; result.used_percent = snapshot->used_percent;
    result.total_available = tokenUsage ? snapshot->total_available : std::nullopt; result.total_granted = tokenUsage ? snapshot->total_granted : std::nullopt; result.total_used = tokenUsage ? snapshot->total_used : std::nullopt;
    result.observed_at_ms = state.observed_at_ms; result.cycle_started_at_ms = tokenUsage ? std::nullopt : std::optional<double>(state.cycle_started_at_ms);
    result.last_credit_at_ms = tokenUsage ? std::nullopt : state.last_credit_at_ms;
    result.last_inferred_credit_quota = tokenUsage ? std::nullopt : std::optional<double>(state.last_inferred_credit_quota);
    result.latest_refill_amount_credits = state.latest_refill_amount_credits; result.latest_refill_completed_at_ms = state.latest_refill_completed_at_ms;
    return result;
  }
  std::vector<PaidFallbackModelSeries> groupPaidFallbackUsageRollups(const std::vector<paid_fallback::rollups::PaidFallbackUsageRollup>& rollups) {
    rt::OrderedMap<std::string, std::vector<paid_fallback::rollups::PaidFallbackUsageRollup>> byModelProvider;
    for (const auto& rollup : rollups) {
      const std::string identity = rollup.model + std::string(1, '\0') + rollup.provider;
      if (auto* existing = byModelProvider.get(identity)) existing->push_back(rollup); else byModelProvider.set(identity, {rollup});
    }
    std::vector<PaidFallbackModelSeries> result;
    for (auto& [identity, buckets] : byModelProvider) {
      const auto parts = rt::split(identity, std::string(1, '\0'));
      rt::sort(buckets, [](const auto& left, const auto& right) { return left.bucket_start_at_ms - right.bucket_start_at_ms; });
      result.push_back({parts.empty() ? "" : parts[0], parts.size() < 2 ? "" : parts[1], buckets});
    }
    rt::sort(result, [](const auto& left, const auto& right) { const double order = rt::localeCompare(left.model, right.model); return order != 0 ? order : rt::localeCompare(left.provider, right.provider); });
    return result;
  }
  std::vector<PaidFallbackModelUsage> summarizePaidFallbackUsage(const std::vector<PaidFallbackModelSeries>& series, double nowMs) {
    std::vector<PaidFallbackModelUsage> result;
    for (const auto& entry : series) { PaidFallbackModelUsage value{entry.model, entry.provider, {}}; for (const double days : QUOTA_PROJECTION_WINDOW_DAYS) value.windows.push_back(summarizeWindow(entry.buckets, days, nowMs)); result.push_back(std::move(value)); }
    return result;
  }
  std::vector<PaidFallbackRunwayEstimate> projectPaidFallbackRunway(const PaidFallbackModelUsage& usage, const MeteredQuotaRunwayView& quota, double nowMs) {
    return projectPaidFallbackRunway(usage, quota, nowMs, std::vector<metered_quota::MeteredQuotaBalanceSample>{});
  }
  std::vector<PaidFallbackRunwayEstimate> projectPaidFallbackRunway(const PaidFallbackModelUsage& usage, const MeteredQuotaRunwayView& quota, double nowMs, const std::optional<std::vector<metered_quota::MeteredQuotaBalanceSample>>& balanceHistory) {
    if (usage.provider != "metered") return {};
    const auto balance = runwayBalance(quota); const auto baseline = runwayBaseline(quota);
    std::vector<PaidFallbackRunwayEstimate> result;
    for (const auto& window : usage.windows) {
      PaidFallbackRunwayEstimate estimate{}; estimate.window_days = window.window_days; estimate.unlimited = quota.unlimited_quota; estimate.stale_balance = quota.cache_state == "stale"; estimate.refill_aware_status = "unknown";
      if (quota.unlimited_quota) { result.push_back(estimate); continue; }
      if (balance && positiveFinite(window.avg_quota_per_request)) estimate.requests_remaining = rt::Math::floor(*balance / *window.avg_quota_per_request);
      if (balance && positiveFinite(window.quota_per_hour)) estimate.time_remaining_ms = rt::Math::trunc((*balance / *window.quota_per_hour) * HOUR_MS);
      const auto refill = refillProjection(quota, balance, window.quota_per_hour, balanceHistory, nowMs);
      estimate.refill_aware_status = refill.status; estimate.refill_aware_time_remaining_ms = refill.timeRemainingMs; estimate.refill_aware_exhausted_at_ms = refill.exhaustedAtMs;
      estimate.refill_amount_credits = refill.amountCredits; estimate.refill_cadence_ms = refill.cadenceMs; estimate.refill_rate_quota_per_hour = refill.rateQuotaPerHour; estimate.next_refill_at_ms = refill.nextRefillAtMs;
      if (estimate.time_remaining_ms) estimate.exhausted_at_ms = nowMs + *estimate.time_remaining_ms;
      if (balance && positiveFinite(window.avg_quota_per_request) && *balance > 0) estimate.percent_per_request_vs_balance = (*window.avg_quota_per_request / *balance) * 100;
      if (baseline && positiveFinite(window.avg_quota_per_request) && *baseline > 0) estimate.percent_per_request_vs_baseline = (*window.avg_quota_per_request / *baseline) * 100;
      result.push_back(std::move(estimate));
    }
    return result;
  }
}
