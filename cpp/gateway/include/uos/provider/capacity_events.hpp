#pragma once
// Port of src/provider/capacity-events.ts.
//
// `kvOverride?: Kv | null` is std::optional<std::shared_ptr<rt::Kv>> (std::nullopt is undefined,
// which reads getKv(); a null pointer is null) plus an overload without it; `options = {}`
// parameters are overloads. The event parsers validate stored values, so they take rt::KvValue
// (a json::value converts implicitly; a default-constructed KvValue is undefined).
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace uos::provider::capacity_events {
  /** Redacted, short-lived evidence used only to annotate the capacity chart. */
  inline const rt::KvKey PROVIDER_CAPACITY_RESET_EVENT_KV_PREFIX{"uos_ai", "provider_capacity", "v1", "reset_event"};
  inline constexpr double PROVIDER_CAPACITY_RESET_EVENT_RETENTION_MS = 7 * 24 * 60 * 60'000;
  inline const rt::KvKey PROVIDER_CAPACITY_RATE_LIMIT_RESET_EVENT_KV_PREFIX{"uos_ai", "provider_capacity", "v1", "rate_limit_reset_event"};
  inline const rt::KvKey PROVIDER_CAPACITY_DOWNTIME_EVENT_KV_PREFIX{"uos_ai", "provider_capacity", "v1", "downtime_event"};
  inline constexpr double PROVIDER_CAPACITY_DOWNTIME_EVENT_RETENTION_MS = 7 * 24 * 60 * 60'000;

  // "upstream_error" | "unreachable"
  using ProviderCapacityDowntimeFailureKind = std::string;

  struct ProviderCapacityResetEvent {
    double v = 1;  // `1`
    std::string event_id;
    double slot;  // `1 | 2`
    double observed_at_ms;
    friend bool operator==(const ProviderCapacityResetEvent&, const ProviderCapacityResetEvent&) = default;
  };

  struct ProviderCapacityRateLimitResetEvent {
    double v = 1;                      // `1`
    std::string event_id;
    std::string provider = "openai";  // `"openai"`
    double slot;                       // `1 | 2`
    std::string window;                // "primary" | "secondary"
    double observed_at_ms;
    double previous_sampled_at_ms;
    double previous_reset_at_ms;
    double reset_at_ms;
    double previous_used_percent;
    double current_used_percent;
    double capacity_gain_percentage_points;
    friend bool operator==(const ProviderCapacityRateLimitResetEvent&, const ProviderCapacityRateLimitResetEvent&) = default;
  };

  struct ProviderCapacityDowntimeEvent {
    double v = 1;                      // `1`
    std::string event_id;
    std::string provider = "openai";  // `"openai"`
    ProviderCapacityDowntimeFailureKind failure_kind;
    std::optional<double> status;  // number | null
    double observed_at_ms;
    friend bool operator==(const ProviderCapacityDowntimeEvent&, const ProviderCapacityDowntimeEvent&) = default;
  };

  rt::KvKey providerCapacityResetEventKey(const std::string& eventId);

  rt::KvKey providerCapacityRateLimitResetEventKey(const std::string& eventId);

  rt::KvKey providerCapacityDowntimeEventKey(const std::string& eventId);

  // `(value: unknown): ... | null`: std::nullopt is null.
  std::optional<ProviderCapacityResetEvent> parseProviderCapacityResetEvent(const rt::KvValue& value);

  std::optional<ProviderCapacityRateLimitResetEvent> parseProviderCapacityRateLimitResetEvent(const rt::KvValue& value);

  std::optional<ProviderCapacityDowntimeEvent> parseProviderCapacityDowntimeEvent(const rt::KvValue& value);

  rt::Task<bool> recordProviderCapacityResetEvent(ProviderCapacityResetEvent event);
  rt::Task<bool> recordProviderCapacityResetEvent(ProviderCapacityResetEvent event, std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  // `options: Readonly<{ kv?: Kv | null; now?: () => number }>` of listProviderCapacityRateLimitResetEvents,
  // listProviderCapacityDowntimeEvents and listProviderCapacityResetEvents.
  struct ListProviderCapacityEventsOptions {
    // std::nullopt is undefined (getKv()); a null pointer is null.
    std::optional<std::shared_ptr<rt::Kv>> kv{};
    // An empty function is undefined (Date.now).
    std::function<double()> now{};
  };

  rt::Task<std::vector<ProviderCapacityRateLimitResetEvent>> listProviderCapacityRateLimitResetEvents();
  rt::Task<std::vector<ProviderCapacityRateLimitResetEvent>> listProviderCapacityRateLimitResetEvents(ListProviderCapacityEventsOptions options);

  // `input: Readonly<{ failure_kind; status: number | null; observed_at_ms: number }>` of
  // recordProviderCapacityDowntimeEvent.
  struct ProviderCapacityDowntimeEventInput {
    ProviderCapacityDowntimeFailureKind failure_kind;
    std::optional<double> status;  // number | null
    double observed_at_ms;
  };

  /**
   * Record one redacted OpenAI incident per 15-minute chart segment. The event
   * contains no request, account, URL, or provider body data.
   */
  rt::Task<bool> recordProviderCapacityDowntimeEvent(ProviderCapacityDowntimeEventInput input);
  rt::Task<bool> recordProviderCapacityDowntimeEvent(ProviderCapacityDowntimeEventInput input, std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  rt::Task<std::vector<ProviderCapacityDowntimeEvent>> listProviderCapacityDowntimeEvents();
  rt::Task<std::vector<ProviderCapacityDowntimeEvent>> listProviderCapacityDowntimeEvents(ListProviderCapacityEventsOptions options);

  /**
   * Read recent reset markers and derive any missing markers from verified
   * redemption records. The derivation is intentionally best effort: an
   * unverified or unmapped redemption must never become a chart assertion.
   */
  rt::Task<std::vector<ProviderCapacityResetEvent>> listProviderCapacityResetEvents();
  rt::Task<std::vector<ProviderCapacityResetEvent>> listProviderCapacityResetEvents(ListProviderCapacityEventsOptions options);

  // `options: Readonly<{ now?: () => number }>` of the capacity sample trigger.
  struct ProviderCapacitySampleTriggerOptions {
    // An empty function is undefined (Date.now).
    std::function<double()> now{};
  };

  // `(options: Readonly<{ now?: () => number }>) => void`
  using ProviderCapacitySampleTrigger = std::function<void(const ProviderCapacitySampleTriggerOptions& options)>;

  /**
   * Tests replace the trigger so a background probe cannot race their fetch and KV
   * expectations; `null` restores the real one.
   */
  // An empty function is null.
  void setProviderCapacitySampleTriggerForTest(ProviderCapacitySampleTrigger trigger);

  void triggerProviderCapacitySample();
  void triggerProviderCapacitySample(const ProviderCapacitySampleTriggerOptions& options);

  // ---- JSON conversions of the stored events (members in declaration order) ----------------
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityResetEvent& value);
  ProviderCapacityResetEvent tag_invoke(const json::value_to_tag<ProviderCapacityResetEvent>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityRateLimitResetEvent& value);
  ProviderCapacityRateLimitResetEvent tag_invoke(const json::value_to_tag<ProviderCapacityRateLimitResetEvent>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderCapacityDowntimeEvent& value);
  ProviderCapacityDowntimeEvent tag_invoke(const json::value_to_tag<ProviderCapacityDowntimeEvent>&, const json::value& value);
}
