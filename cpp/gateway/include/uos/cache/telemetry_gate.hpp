#pragma once
// Port of src/cache/telemetry-gate.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::cache::telemetry_gate {
  /**
   * Durable Stage 0 evidence is deliberately scoped to an immutable artifact.
   * A local checkout has `unknown` baked into release.ts, so it can never create
   * evidence that a deployed cache-scope experiment could consume.
   */
  inline const rt::KvKey PROMPT_CACHE_TELEMETRY_GATE_KV_PREFIX{"uos_ai", "prompt_cache_telemetry_gate", "v1"};
  inline constexpr double PROMPT_CACHE_TELEMETRY_MIN_COMPLETED = 10'000;
  inline constexpr double PROMPT_CACHE_TELEMETRY_MIN_COMPLETED_PER_ROUTE = 1'000;

  inline constexpr std::array<std::string_view, 3> PROMPT_CACHE_TELEMETRY_PROVIDERS{"chatgpt_codex", "metered", "surplus"};
  inline constexpr std::array<std::string_view, 2> PROMPT_CACHE_TELEMETRY_ROUTES{"responses", "chat.completions"};

  using PromptCacheTelemetryProvider = std::string;
  using PromptCacheTelemetryRoute = std::string;

  struct PromptCacheTelemetryEvent {
    std::optional<std::string> provider;
    std::optional<std::string> model;
    std::string route;
    double status;
    bool completed;
    std::string usageTelemetryStatus;
    /** True only when a valid upstream cache_write_tokens field was observed; zero is present. */
    bool cacheWriteTokensPresent;
  };

  struct PromptCacheTelemetryGateOptions {
    /** Supplying a KV instance makes focused tests and explicit control-plane reads deterministic. */
    // `kv?: Kv | null`: std::nullopt is undefined (read the process KV), a null pointer is null.
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    /** Defaults to the immutable source identity baked into this artifact. */
    std::optional<std::string> release{};
  };

  struct PromptCacheTelemetryRecordResult {
    std::string status;  // "recorded" | "ignored" | "unavailable"
    // "recorded" | "unknown_release" | "not_completed_2xx" | "unsupported_provider" | "unsupported_route" | "invalid_model" | "kv_unavailable"
    std::string reason;
    std::optional<std::string> release;
    std::optional<PromptCacheTelemetryProvider> provider;
    std::optional<PromptCacheTelemetryRoute> route;
    /** A domain-separated SHA-256 digest; raw model text is never persisted or returned. */
    std::optional<std::string> model_hash;
    friend bool operator==(const PromptCacheTelemetryRecordResult&, const PromptCacheTelemetryRecordResult&) = default;
  };

  struct PromptCacheTelemetryBaselineTarget {
    std::string provider;
    std::string model;
  };

  /**
   * Opaque keys are useful for administrative migration/attestation tooling that
   * must seed or inspect durable counters without ever learning a raw model name.
   */
  struct PromptCacheTelemetryCounterKeys {
    struct Route {
      PromptCacheTelemetryRoute route;
      rt::KvKey completed;
      rt::KvKey reported;
      rt::KvKey cache_write_reported;
      rt::KvKey invalid;
    };
    std::string release;
    PromptCacheTelemetryProvider provider;
    std::string model_hash;
    std::vector<Route> routes;
  };

  struct PromptCacheTelemetryCounterSummary {
    /** Decimal U64 strings keep this result safe to JSON serialize without losing precision. */
    std::string completed;
    std::string reported;
    /** Completed responses with a valid cache_write_tokens field, including a valid zero. */
    std::string cache_write_reported;
    /** Completed responses whose upstream usage object was present but invalid. */
    std::string invalid;
    std::optional<double> reported_coverage;
    bool reported_coverage_passed;
    std::optional<double> cache_write_reported_coverage;
    bool cache_write_reported_coverage_passed;
  };

  // `PromptCacheTelemetryCounterSummary & Readonly<{ route; observed; completed_minimum_passed }>`
  struct PromptCacheTelemetryRouteBaseline : PromptCacheTelemetryCounterSummary {
    PromptCacheTelemetryRoute route;
    bool observed = false;
    bool completed_minimum_passed = false;
  };

  struct PromptCacheTelemetryBaselineResult {
    std::string status;  // "eligible" | "not_ready" | "unavailable"
    // "eligible" | "unknown_release" | "unsupported_target" | "kv_unavailable" | "invalid_counter" | "aggregate_completed_below_minimum" |
    // "route_completed_below_minimum" | "aggregate_reported_coverage_below_minimum" | "route_reported_coverage_below_minimum" |
    // "aggregate_cache_write_reported_coverage_below_minimum" | "route_cache_write_reported_coverage_below_minimum"
    std::string reason;
    std::optional<std::string> release;
    std::optional<PromptCacheTelemetryProvider> provider;
    std::optional<std::string> model_hash;
    std::optional<PromptCacheTelemetryCounterSummary> aggregate;
    std::vector<PromptCacheTelemetryRouteBaseline> routes;
  };

  // The results as the TS objects JSON.stringify (members in the order the TS builds them).
  json::object toJson(const PromptCacheTelemetryRecordResult& result);
  json::object toJson(const PromptCacheTelemetryCounterSummary& summary);
  json::object toJson(const PromptCacheTelemetryRouteBaseline& route);
  json::object toJson(const PromptCacheTelemetryBaselineResult& result);

  /**
   * Resolves only opaque Deno KV counter keys for a valid target. It performs no
   * writes and returns null for an unknown release or unsupported target.
   */
  // The overloads without `options` are `{}`.
  rt::Task<std::optional<PromptCacheTelemetryCounterKeys>> resolvePromptCacheTelemetryCounterKeys(PromptCacheTelemetryBaselineTarget target);
  rt::Task<std::optional<PromptCacheTelemetryCounterKeys>> resolvePromptCacheTelemetryCounterKeys(PromptCacheTelemetryBaselineTarget target,
                                                                                                  PromptCacheTelemetryGateOptions options);

  /**
   * Counts only terminal, successful inference completions. Completed and
   * telemetry-status counters are summed in one atomic Deno KV commit, so a
   * read can never observe a reported or invalid event without its denominator.
   */
  rt::Task<PromptCacheTelemetryRecordResult> recordPromptCacheTelemetry(PromptCacheTelemetryEvent event);
  rt::Task<PromptCacheTelemetryRecordResult> recordPromptCacheTelemetry(PromptCacheTelemetryEvent event, PromptCacheTelemetryGateOptions options);

  /**
   * Reads only the target provider/model cohort for the immutable current
   * release. It is intentionally fail-closed: malformed counters, an unknown
   * release, missing KV, or any threshold miss are never eligible.
   */
  rt::Task<PromptCacheTelemetryBaselineResult> readPromptCacheTelemetryBaseline(PromptCacheTelemetryBaselineTarget target);
  rt::Task<PromptCacheTelemetryBaselineResult> readPromptCacheTelemetryBaseline(PromptCacheTelemetryBaselineTarget target, PromptCacheTelemetryGateOptions options);
}
