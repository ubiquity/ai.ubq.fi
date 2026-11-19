#pragma once
// Port of src/provider/health.ts.
//
// The record*ProviderHealth functions return the transition queue's promise, already running
// (rt::Promise<void>), as the TS reserves the transition synchronously. Their defaults
// (`status = null`, `now = Date.now`, `providerRequestId = null`) are default arguments: an
// empty `now` is Date.now. The get*ProviderHealth reads are rt::Task with overloads for the
// omitted trailing arguments. `binding?: ClaudeCodeAccountBinding` is std::optional (std::nullopt
// is undefined). parseProviderHealthObservation validates stored values, so it takes rt::KvValue
// (a json::value converts implicitly; a default-constructed KvValue is undefined).
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace uos::provider::health {
  inline const rt::KvKey PROVIDER_HEALTH_KEY_PREFIX{"uos_ai", "provider_health", "v1"};
  inline constexpr double PROVIDER_HEALTH_HEARTBEAT_WRITE_INTERVAL_MS = 60'000;
  inline constexpr double PROVIDER_HEALTH_STALE_AFTER_MS = 30 * 60'000;

  // "healthy" | "degraded" | "exhausted" | "invalid" | "unknown"
  using ProviderHealthState = std::string;
  // "success" | "reachable" | "auth_invalid" | "quota_exhausted" | "upstream_error" | "refresh_success" | "refresh_failed"
  using ProviderHealthEvent = std::string;

  struct ProviderHealthObservation {
    ProviderHealthEvent event;
    std::optional<double> status;  // number | null
    double observed_at_ms;
    /** Opaque upstream correlation only; absent on older observations. */
    rt::Nullable<std::string> provider_request_id{};
    friend bool operator==(const ProviderHealthObservation&, const ProviderHealthObservation&) = default;
  };

  struct ProviderHealthView {
    ProviderHealthState state;
    std::optional<bool> stale;                              // boolean | null
    std::optional<ProviderHealthEvent> last_event;          // ... | null
    std::optional<double> last_status;                      // number | null
    std::optional<double> last_observed_at_ms;              // number | null
    std::optional<std::string> last_provider_request_id;    // string | null
    std::optional<double> last_provider_request_id_at_ms;  // number | null
    std::optional<double> last_success_at_ms;               // number | null
    std::optional<double> last_401_at_ms;                   // number | null
    std::optional<double> last_429_at_ms;                   // number | null
    std::optional<double> last_error_at_ms;                 // number | null
    std::optional<double> last_refresh_at_ms;               // number | null
    std::optional<bool> last_refresh_succeeded;             // boolean | null
    friend bool operator==(const ProviderHealthView&, const ProviderHealthView&) = default;
  };

  /**
   * The providers the passive health view (`/health/providers`) publishes a
   * top-level key for, named after the route each one reports on: OpenLux
   * reports through `metered`.
   */
  inline constexpr std::array<std::string_view, 10> RECORD_PROVIDER_IDS = {"cerebras", "claude_code", "codex", "deepseek", "gemini",
                                                                           "lithos",   "metered",     "openrouter", "surplus", "ubiquity"};

  // `(typeof RECORD_PROVIDER_IDS)[number]`
  using RecordProvider = std::string;

  // `(value: unknown): ProviderHealthObservation | null`. The TS returns the validated value
  // itself; the typed record keeps the declared members (`provider_request_id` as stored, absent,
  // null or the raw string). std::nullopt is null.
  std::optional<ProviderHealthObservation> parseProviderHealthObservation(const rt::KvValue& value);

  rt::Promise<void> recordCodexProviderHealth(std::string accountId, ProviderHealthEvent event, std::optional<double> status = std::nullopt,
                                              std::function<double()> now = {}, std::optional<std::string> providerRequestId = std::nullopt);

  rt::Promise<void> recordOpenRouterProviderHealth(ProviderHealthEvent event, std::optional<double> status = std::nullopt, std::function<double()> now = {},
                                                   std::optional<std::string> providerRequestId = std::nullopt);

  rt::Promise<void> recordMeteredProviderHealth(ProviderHealthEvent event, std::optional<double> status = std::nullopt, std::function<double()> now = {},
                                                std::optional<std::string> providerRequestId = std::nullopt);

  rt::Promise<void> recordSurplusProviderHealth(ProviderHealthEvent event, std::optional<double> status = std::nullopt, std::function<double()> now = {},
                                                std::optional<std::string> providerRequestId = std::nullopt);

  rt::Promise<void> recordCerebrasProviderHealth(ProviderHealthEvent event, std::optional<double> status = std::nullopt, std::function<double()> now = {},
                                                 std::optional<std::string> providerRequestId = std::nullopt);

  rt::Promise<void> recordDeepSeekProviderHealth(ProviderHealthEvent event, std::optional<double> status = std::nullopt, std::function<double()> now = {},
                                                 std::optional<std::string> providerRequestId = std::nullopt);

  rt::Promise<void> recordGeminiProviderHealth(ProviderHealthEvent event, std::optional<double> status = std::nullopt, std::function<double()> now = {},
                                                std::optional<std::string> providerRequestId = std::nullopt);

  rt::Promise<void> recordLithosProviderHealth(ProviderHealthEvent event, std::optional<double> status = std::nullopt, std::function<double()> now = {},
                                               std::optional<std::string> providerRequestId = std::nullopt);

  rt::Promise<void> recordClaudeCodeProviderHealth(ProviderHealthEvent event, std::optional<double> status = std::nullopt, std::function<double()> now = {},
                                                   std::optional<std::string> providerRequestId = std::nullopt,
                                                   std::optional<claude_code_accounts::ClaudeCodeAccountBinding> binding = std::nullopt);

  // `now: () => number = Date.now` (an empty function is Date.now as well).
  rt::Task<ProviderHealthView> getCodexProviderHealth(std::string accountId);
  rt::Task<ProviderHealthView> getCodexProviderHealth(std::string accountId, std::function<double()> now);

  rt::Task<ProviderHealthView> getMeteredProviderHealth();
  rt::Task<ProviderHealthView> getMeteredProviderHealth(std::function<double()> now);
  rt::Task<ProviderHealthView> getOpenRouterProviderHealth();
  rt::Task<ProviderHealthView> getOpenRouterProviderHealth(std::function<double()> now);

  rt::Task<ProviderHealthView> getSurplusProviderHealth();
  rt::Task<ProviderHealthView> getSurplusProviderHealth(std::function<double()> now);

  rt::Task<ProviderHealthView> getCerebrasProviderHealth();
  rt::Task<ProviderHealthView> getCerebrasProviderHealth(std::function<double()> now);

  rt::Task<ProviderHealthView> getDeepSeekProviderHealth();
  rt::Task<ProviderHealthView> getDeepSeekProviderHealth(std::function<double()> now);

  rt::Task<ProviderHealthView> getGeminiProviderHealth();
  rt::Task<ProviderHealthView> getGeminiProviderHealth(std::function<double()> now);

  rt::Task<ProviderHealthView> getLithosProviderHealth();
  rt::Task<ProviderHealthView> getLithosProviderHealth(std::function<double()> now);

  rt::Task<ProviderHealthView> getClaudeCodeProviderHealth();
  rt::Task<ProviderHealthView> getClaudeCodeProviderHealth(std::function<double()> now);
  rt::Task<ProviderHealthView> getClaudeCodeProviderHealth(std::function<double()> now, std::optional<claude_code_accounts::ClaudeCodeAccountBinding> binding);

  void resetProviderHealthThrottleForTest();

  // ---- JSON conversions (members in declaration order) -------------------------------------
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderHealthObservation& value);
  ProviderHealthObservation tag_invoke(const json::value_to_tag<ProviderHealthObservation>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ProviderHealthView& value);
  ProviderHealthView tag_invoke(const json::value_to_tag<ProviderHealthView>&, const json::value& value);
}
