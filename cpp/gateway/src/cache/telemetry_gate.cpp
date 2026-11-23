#include "uos/cache/telemetry_gate.hpp"

#include "uos/kv.hpp"
#include "uos/release.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <cstdint>
#include <utility>

namespace uos::cache::telemetry_gate {
  namespace {
    struct CounterPair {
      rt::BigInt completed;
      rt::BigInt reported;
      rt::BigInt cacheWriteReported;
      rt::BigInt invalid;
    };

    const rt::Regex& RELEASE_SHA() {
      static const rt::Regex pattern("^[a-f0-9]{7,64}$", "i");
      return pattern;
    }
    constexpr double MAX_MODEL_CHARS = 256;
    const rt::BigInt& COVERAGE_NUMERATOR() {
      static const rt::BigInt value(199);
      return value;
    }
    const rt::BigInt& COVERAGE_DENOMINATOR() {
      static const rt::BigInt value(200);
      return value;
    }

    // `value: unknown`: only a string can be a release.
    std::optional<std::string> normalizedRelease(const std::string& value) {
      const std::string release = rt::trim(value);
      if (release.empty() || rt::toLowerCase(release) == "unknown" || !RELEASE_SHA().test(release)) return std::nullopt;
      return rt::toLowerCase(release);
    }

    bool hasAsciiControlCharacter(const std::string& value) {
      for (const std::uint32_t code : rt::codePoints(value)) {
        if (code < 0x20 || code == 0x7f) return true;
      }
      return false;
    }

    // `value: unknown` (a `string | null` member).
    std::optional<std::string> normalizedModel(const std::optional<std::string>& value) {
      if (!value) return std::nullopt;
      const std::string model = rt::trim(*value);
      if (model.empty() || static_cast<double>(rt::length(model)) > MAX_MODEL_CHARS || hasAsciiControlCharacter(model)) return std::nullopt;
      return model;
    }

    std::optional<PromptCacheTelemetryProvider> asProvider(const std::optional<std::string>& value) {
      if (!value || std::find(PROMPT_CACHE_TELEMETRY_PROVIDERS.begin(), PROMPT_CACHE_TELEMETRY_PROVIDERS.end(), *value) == PROMPT_CACHE_TELEMETRY_PROVIDERS.end())
        return std::nullopt;
      return value;
    }

    std::optional<PromptCacheTelemetryRoute> asRoute(const std::string& value) {
      if (std::find(PROMPT_CACHE_TELEMETRY_ROUTES.begin(), PROMPT_CACHE_TELEMETRY_ROUTES.end(), value) == PROMPT_CACHE_TELEMETRY_ROUTES.end()) return std::nullopt;
      return value;
    }

    bool isCompleted2xx(const PromptCacheTelemetryEvent& event) {
      return event.completed && rt::Number::isInteger(event.status) && event.status >= 200 && event.status < 300;
    }

    std::optional<std::string> resolveRelease(const PromptCacheTelemetryGateOptions& options) {
      return normalizedRelease(options.release ? *options.release : std::string(release::RELEASE_GIT_SHA));
    }

    rt::Task<std::shared_ptr<rt::Kv>> resolveKv(PromptCacheTelemetryGateOptions options) {
      std::shared_ptr<rt::Kv> resolved;
      try {
        if (!options.kv) {
          rt::Promise<std::shared_ptr<rt::Kv>> pending = kv::getKv();
          resolved = co_await pending.wait();
        } else {
          resolved = *options.kv;
        }
      } catch (...) {
        resolved = nullptr;
      }
      co_return resolved;
    }

    rt::Task<std::string> modelHash(std::string model) {
      std::string text = "uos-prompt-cache-telemetry-model-v1";
      text.push_back('\0');
      text += model;
      rt::Task<std::string> hashing = utils::sha256Hex(std::move(text));
      std::string hashed = co_await std::move(hashing);
      co_return hashed;
    }

    rt::KvKey counterKey(const std::string& release, const PromptCacheTelemetryProvider& provider, const std::string& modelHashValue, const PromptCacheTelemetryRoute& route,
                         std::string_view counter) {
      return rt::KvKey{PROMPT_CACHE_TELEMETRY_GATE_KV_PREFIX, release, provider, modelHashValue, route, counter};
    }

    // `Readonly<{ release?: string | null; provider?: ... | null; route?: ... | null; modelHash?: string | null }>`
    struct ResultInput {
      std::optional<std::string> release{};
      std::optional<PromptCacheTelemetryProvider> provider{};
      std::optional<PromptCacheTelemetryRoute> route{};
      std::optional<std::string> modelHash{};
    };

    PromptCacheTelemetryRecordResult recordResult(std::string status, std::string reason, ResultInput input = {}) {
      return PromptCacheTelemetryRecordResult{
        .status = std::move(status),
        .reason = std::move(reason),
        .release = std::move(input.release),
        .provider = std::move(input.provider),
        .route = std::move(input.route),
        .model_hash = std::move(input.modelHash),
      };
    }

    bool isKvU64(const rt::KvValue& value) { return value.isU64(); }

    /** A missing sibling counter counts as zero; a present but malformed one is invalid. */
    std::optional<rt::BigInt> siblingCounter(bool missing, const rt::KvValue& value) {
      if (missing) return rt::BigInt(0);
      if (isKvU64(value)) return value.asU64().toBigInt();
      return std::nullopt;
    }

    std::optional<CounterPair> counterPair(const rt::KvValue& completedValue, const rt::KvValue& reportedValue, const rt::KvValue& cacheWriteReportedValue,
                                           const rt::KvValue& invalidValue) {
      const bool completedMissing = completedValue.isNull();
      const bool reportedMissing = reportedValue.isNull();
      const bool cacheWriteReportedMissing = cacheWriteReportedValue.isNull();
      const bool invalidMissing = invalidValue.isNull();
      if (completedMissing && reportedMissing && cacheWriteReportedMissing && invalidMissing) {
        return CounterPair{rt::BigInt(0), rt::BigInt(0), rt::BigInt(0), rt::BigInt(0)};
      }
      if (completedMissing || !isKvU64(completedValue)) return std::nullopt;
      const rt::BigInt completed = completedValue.asU64().toBigInt();
      const std::optional<rt::BigInt> reported = siblingCounter(reportedMissing, reportedValue);
      const std::optional<rt::BigInt> cacheWriteReported = siblingCounter(cacheWriteReportedMissing, cacheWriteReportedValue);
      const std::optional<rt::BigInt> invalid = siblingCounter(invalidMissing, invalidValue);
      if (!reported || !cacheWriteReported || !invalid) return std::nullopt;
      if (*reported > completed || *cacheWriteReported > *reported || *invalid > completed) return std::nullopt;
      if (*reported + *invalid > completed) return std::nullopt;
      return CounterPair{completed, *reported, *cacheWriteReported, *invalid};
    }

    std::optional<double> coverage(const rt::BigInt& numerator, const CounterPair& counters) {
      if (counters.completed == rt::BigInt(0)) return std::nullopt;
      return ((numerator * rt::BigInt(1'000'000)) / counters.completed).toNumber() / 1'000'000;
    }

    bool passesCoverage(const rt::BigInt& numerator, const CounterPair& counters) {
      return counters.completed > rt::BigInt(0) && numerator * COVERAGE_DENOMINATOR() >= counters.completed * COVERAGE_NUMERATOR();
    }

    PromptCacheTelemetryCounterSummary counterSummary(const CounterPair& counters) {
      return PromptCacheTelemetryCounterSummary{
        .completed = counters.completed.toString(),
        .reported = counters.reported.toString(),
        .cache_write_reported = counters.cacheWriteReported.toString(),
        .invalid = counters.invalid.toString(),
        .reported_coverage = coverage(counters.reported, counters),
        .reported_coverage_passed = passesCoverage(counters.reported, counters),
        .cache_write_reported_coverage = coverage(counters.cacheWriteReported, counters),
        .cache_write_reported_coverage_passed = passesCoverage(counters.cacheWriteReported, counters),
      };
    }

    // `reason: "unknown_release" | "unsupported_target" | "kv_unavailable"`
    PromptCacheTelemetryBaselineResult unavailableBaseline(const std::string& reason, ResultInput input = {}) {
      return PromptCacheTelemetryBaselineResult{
        .status = reason == "kv_unavailable" ? "unavailable" : "not_ready",
        .reason = reason,
        .release = std::move(input.release),
        .provider = std::move(input.provider),
        .model_hash = std::move(input.modelHash),
        .aggregate = std::nullopt,
        .routes = {},
      };
    }

    constexpr std::array<std::string_view, 4> COUNTERS{"completed", "reported", "cache_write_reported", "invalid"};

    struct RouteEntries {
      PromptCacheTelemetryRoute route;
      rt::KvEntryMaybe<> completed;
      rt::KvEntryMaybe<> reported;
      rt::KvEntryMaybe<> cacheWriteReported;
      rt::KvEntryMaybe<> invalid;
    };

    // `async (route) => { const [...] = await Promise.all([kv.get(...) x4]); return { route, ... }; }`
    rt::Task<RouteEntries> readRouteEntries(std::shared_ptr<rt::Kv> kv, std::string release, std::string provider, std::string hashedModel, std::string route) {
      rt::KvGetOptions strong;
      strong.consistency = rt::KvConsistencyLevel::strong;
      std::vector<rt::Promise<rt::KvEntryMaybe<>>> gets;
      for (const std::string_view counter : COUNTERS) gets.push_back(rt::Promise(kv->get(counterKey(release, provider, hashedModel, route, counter), strong)));
      auto all = rt::all(std::move(gets));
      std::vector<rt::KvEntryMaybe<>> entries = co_await all.wait();
      RouteEntries read{.route = route, .completed = entries[0], .reported = entries[1], .cacheWriteReported = entries[2], .invalid = entries[3]};
      co_return read;
    }

    json::value textOrNull(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }
    json::value numberOrNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
    void writeSummary(json::object& out, const PromptCacheTelemetryCounterSummary& summary) {
      out["completed"] = summary.completed;
      out["reported"] = summary.reported;
      out["cache_write_reported"] = summary.cache_write_reported;
      out["invalid"] = summary.invalid;
      out["reported_coverage"] = numberOrNull(summary.reported_coverage);
      out["reported_coverage_passed"] = summary.reported_coverage_passed;
      out["cache_write_reported_coverage"] = numberOrNull(summary.cache_write_reported_coverage);
      out["cache_write_reported_coverage_passed"] = summary.cache_write_reported_coverage_passed;
    }
  }

  json::object toJson(const PromptCacheTelemetryRecordResult& result) {
    json::object out;
    out["status"] = result.status;
    out["reason"] = result.reason;
    out["release"] = textOrNull(result.release);
    out["provider"] = textOrNull(result.provider);
    out["route"] = textOrNull(result.route);
    out["model_hash"] = textOrNull(result.model_hash);
    return out;
  }

  json::object toJson(const PromptCacheTelemetryCounterSummary& summary) {
    json::object out;
    writeSummary(out, summary);
    return out;
  }

  json::object toJson(const PromptCacheTelemetryRouteBaseline& route) {
    json::object out;
    out["route"] = route.route;
    out["observed"] = route.observed;
    writeSummary(out, route);
    out["completed_minimum_passed"] = route.completed_minimum_passed;
    return out;
  }

  json::object toJson(const PromptCacheTelemetryBaselineResult& result) {
    json::array routes;
    for (const PromptCacheTelemetryRouteBaseline& route : result.routes) routes.push_back(toJson(route));
    json::object out;
    out["status"] = result.status;
    out["reason"] = result.reason;
    out["release"] = textOrNull(result.release);
    out["provider"] = textOrNull(result.provider);
    out["model_hash"] = textOrNull(result.model_hash);
    out["aggregate"] = result.aggregate ? json::value(toJson(*result.aggregate)) : json::value(nullptr);
    out["routes"] = std::move(routes);
    return out;
  }

  rt::Task<std::optional<PromptCacheTelemetryCounterKeys>> resolvePromptCacheTelemetryCounterKeys(PromptCacheTelemetryBaselineTarget target) {
    return resolvePromptCacheTelemetryCounterKeys(std::move(target), PromptCacheTelemetryGateOptions());
  }

  rt::Task<std::optional<PromptCacheTelemetryCounterKeys>> resolvePromptCacheTelemetryCounterKeys(PromptCacheTelemetryBaselineTarget target,
                                                                                                  PromptCacheTelemetryGateOptions options) {
    const std::optional<std::string> release = resolveRelease(options);
    const std::optional<PromptCacheTelemetryProvider> provider = asProvider(target.provider);
    const std::optional<std::string> model = normalizedModel(target.model);
    if (!release || !provider || !model) co_return std::nullopt;
    std::optional<std::string> hashedModel;
    try {
      rt::Task<std::string> hashing = modelHash(*model);
      hashedModel = co_await std::move(hashing);
    } catch (...) {
      hashedModel.reset();
    }
    if (!hashedModel) co_return std::nullopt;
    PromptCacheTelemetryCounterKeys keys{.release = *release, .provider = *provider, .model_hash = *hashedModel, .routes = {}};
    for (const std::string_view route : PROMPT_CACHE_TELEMETRY_ROUTES) {
      const std::string name(route);
      keys.routes.push_back(PromptCacheTelemetryCounterKeys::Route{
        .route = name,
        .completed = counterKey(*release, *provider, *hashedModel, name, "completed"),
        .reported = counterKey(*release, *provider, *hashedModel, name, "reported"),
        .cache_write_reported = counterKey(*release, *provider, *hashedModel, name, "cache_write_reported"),
        .invalid = counterKey(*release, *provider, *hashedModel, name, "invalid"),
      });
    }
    std::optional<PromptCacheTelemetryCounterKeys> resolved(std::move(keys));
    co_return resolved;
  }

  rt::Task<PromptCacheTelemetryRecordResult> recordPromptCacheTelemetry(PromptCacheTelemetryEvent event) {
    return recordPromptCacheTelemetry(std::move(event), PromptCacheTelemetryGateOptions());
  }

  rt::Task<PromptCacheTelemetryRecordResult> recordPromptCacheTelemetry(PromptCacheTelemetryEvent event, PromptCacheTelemetryGateOptions options) {
    const std::optional<std::string> release = resolveRelease(options);
    if (!release) co_return recordResult("ignored", "unknown_release");
    if (!isCompleted2xx(event)) co_return recordResult("ignored", "not_completed_2xx", ResultInput{.release = release});

    const std::optional<PromptCacheTelemetryProvider> provider = asProvider(event.provider);
    if (!provider) co_return recordResult("ignored", "unsupported_provider", ResultInput{.release = release});
    const std::optional<PromptCacheTelemetryRoute> route = asRoute(event.route);
    if (!route) co_return recordResult("ignored", "unsupported_route", ResultInput{.release = release, .provider = provider});
    const std::optional<std::string> model = normalizedModel(event.model);
    if (!model) co_return recordResult("ignored", "invalid_model", ResultInput{.release = release, .provider = provider, .route = route});

    std::optional<std::string> hashedModel;
    try {
      rt::Task<std::string> hashing = modelHash(*model);
      hashedModel = co_await std::move(hashing);
    } catch (...) {
      hashedModel.reset();
    }
    if (!hashedModel) co_return recordResult("unavailable", "kv_unavailable", ResultInput{.release = release, .provider = provider, .route = route});

    rt::Task<std::shared_ptr<rt::Kv>> resolving = resolveKv(options);
    const std::shared_ptr<rt::Kv> kv = co_await std::move(resolving);
    if (!kv) {
      co_return recordResult("unavailable", "kv_unavailable", ResultInput{.release = release, .provider = provider, .route = route, .modelHash = hashedModel});
    }

    bool committedOk = false;
    try {
      std::unique_ptr<rt::AtomicOperation> operation = kv->atomic();
      operation->sum(counterKey(*release, *provider, *hashedModel, *route, "completed"), 1);
      if (event.usageTelemetryStatus == "reported") {
        operation->sum(counterKey(*release, *provider, *hashedModel, *route, "reported"), 1);
        if (event.cacheWriteTokensPresent) {
          operation->sum(counterKey(*release, *provider, *hashedModel, *route, "cache_write_reported"), 1);
        }
      }
      if (event.usageTelemetryStatus == "invalid") {
        operation->sum(counterKey(*release, *provider, *hashedModel, *route, "invalid"), 1);
      }
      const rt::KvCommitResult committed = co_await operation->commit();
      committedOk = committed.ok;
    } catch (...) {
      committedOk = false;
    }
    if (!committedOk) {
      co_return recordResult("unavailable", "kv_unavailable", ResultInput{.release = release, .provider = provider, .route = route, .modelHash = hashedModel});
    }

    co_return recordResult("recorded", "recorded", ResultInput{.release = release, .provider = provider, .route = route, .modelHash = hashedModel});
  }

  rt::Task<PromptCacheTelemetryBaselineResult> readPromptCacheTelemetryBaseline(PromptCacheTelemetryBaselineTarget target) {
    return readPromptCacheTelemetryBaseline(std::move(target), PromptCacheTelemetryGateOptions());
  }

  rt::Task<PromptCacheTelemetryBaselineResult> readPromptCacheTelemetryBaseline(PromptCacheTelemetryBaselineTarget target, PromptCacheTelemetryGateOptions options) {
    const std::optional<std::string> release = resolveRelease(options);
    if (!release) co_return unavailableBaseline("unknown_release");

    const std::optional<PromptCacheTelemetryProvider> provider = asProvider(target.provider);
    const std::optional<std::string> model = normalizedModel(target.model);
    if (!provider || !model) co_return unavailableBaseline("unsupported_target", ResultInput{.release = release, .provider = provider});

    std::optional<std::string> hashedModel;
    try {
      rt::Task<std::string> hashing = modelHash(*model);
      hashedModel = co_await std::move(hashing);
    } catch (...) {
      hashedModel.reset();
    }
    if (!hashedModel) co_return unavailableBaseline("kv_unavailable", ResultInput{.release = release, .provider = provider});

    rt::Task<std::shared_ptr<rt::Kv>> resolving = resolveKv(options);
    const std::shared_ptr<rt::Kv> kv = co_await std::move(resolving);
    if (!kv) co_return unavailableBaseline("kv_unavailable", ResultInput{.release = release, .provider = provider, .modelHash = hashedModel});

    std::optional<std::vector<RouteEntries>> entriesByRoute;
    try {
      std::vector<rt::Promise<RouteEntries>> routes;
      for (const std::string_view route : PROMPT_CACHE_TELEMETRY_ROUTES)
        routes.push_back(rt::Promise(readRouteEntries(kv, *release, *provider, *hashedModel, std::string(route))));
      auto all = rt::all(std::move(routes));
      entriesByRoute = co_await all.wait();
    } catch (...) {
      entriesByRoute.reset();
    }
    if (!entriesByRoute) co_return unavailableBaseline("kv_unavailable", ResultInput{.release = release, .provider = provider, .modelHash = hashedModel});

    struct RouteCounters {
      PromptCacheTelemetryRoute route;
      CounterPair counters;
    };
    std::vector<RouteCounters> countersByRoute;
    for (const RouteEntries& entries : *entriesByRoute) {
      const std::optional<CounterPair> counters = counterPair(entries.completed.value, entries.reported.value, entries.cacheWriteReported.value, entries.invalid.value);
      if (!counters) {
        PromptCacheTelemetryBaselineResult invalid =
          unavailableBaseline("kv_unavailable", ResultInput{.release = release, .provider = provider, .modelHash = hashedModel});
        invalid.status = "not_ready";
        invalid.reason = "invalid_counter";
        co_return invalid;
      }
      countersByRoute.push_back(RouteCounters{entries.route, *counters});
    }

    CounterPair aggregate{rt::BigInt(0), rt::BigInt(0), rt::BigInt(0), rt::BigInt(0)};
    for (const RouteCounters& entry : countersByRoute) {
      aggregate = CounterPair{
        aggregate.completed + entry.counters.completed,
        aggregate.reported + entry.counters.reported,
        aggregate.cacheWriteReported + entry.counters.cacheWriteReported,
        aggregate.invalid + entry.counters.invalid,
      };
    }
    std::vector<PromptCacheTelemetryRouteBaseline> routes;
    for (const RouteCounters& entry : countersByRoute) {
      PromptCacheTelemetryRouteBaseline baseline;
      static_cast<PromptCacheTelemetryCounterSummary&>(baseline) = counterSummary(entry.counters);
      baseline.route = entry.route;
      baseline.observed = entry.counters.completed > rt::BigInt(0);
      baseline.completed_minimum_passed = entry.counters.completed >= rt::BigInt::fromNumber(PROMPT_CACHE_TELEMETRY_MIN_COMPLETED_PER_ROUTE);
      routes.push_back(std::move(baseline));
    }

    const PromptCacheTelemetryCounterSummary aggregateSummary = counterSummary(aggregate);
    const bool aggregateCompletedPassed = aggregate.completed >= rt::BigInt::fromNumber(PROMPT_CACHE_TELEMETRY_MIN_COMPLETED);
    bool everyObservedRouteCompleted = true;
    bool everyObservedRouteReported = true;
    bool everyObservedRouteCacheWriteReported = true;
    for (const PromptCacheTelemetryRouteBaseline& route : routes) {
      if (!route.observed) continue;
      everyObservedRouteCompleted = everyObservedRouteCompleted && route.completed_minimum_passed;
      everyObservedRouteReported = everyObservedRouteReported && route.reported_coverage_passed;
      everyObservedRouteCacheWriteReported = everyObservedRouteCacheWriteReported && route.cache_write_reported_coverage_passed;
    }

    // Ordered fail-closed gate list: the first unmet threshold names the reason.
    const std::array<std::pair<bool, std::string_view>, 6> eligibilityGates{{
      {aggregateCompletedPassed, "aggregate_completed_below_minimum"},
      {everyObservedRouteCompleted, "route_completed_below_minimum"},
      {aggregateSummary.reported_coverage_passed, "aggregate_reported_coverage_below_minimum"},
      {everyObservedRouteReported, "route_reported_coverage_below_minimum"},
      {aggregateSummary.cache_write_reported_coverage_passed, "aggregate_cache_write_reported_coverage_below_minimum"},
      {everyObservedRouteCacheWriteReported, "route_cache_write_reported_coverage_below_minimum"},
    }};
    std::string reason = "eligible";
    for (const auto& [passed, gateReason] : eligibilityGates) {
      if (!passed) {
        reason = std::string(gateReason);
        break;
      }
    }

    PromptCacheTelemetryBaselineResult result{
      .status = reason == "eligible" ? "eligible" : "not_ready",
      .reason = reason,
      .release = release,
      .provider = provider,
      .model_hash = hashedModel,
      .aggregate = aggregateSummary,
      .routes = std::move(routes),
    };
    co_return result;
  }
}
