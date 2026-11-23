#pragma once
// Port of src/debug-routing.ts: the operator's temporary routing scenario, stored in KV and
// cached in process for five seconds.
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::debug_routing {
  inline const rt::KvKey DEBUG_ROUTING_KEY{"uos_ai", "debug_routing", "v1"};
  inline constexpr double DEBUG_ROUTING_MAX_DURATION_MS = 60 * 60'000;

  // "normal" | "metered_first" | "removed_provider_first" | "codex_401" | "codex_403" | "codex_429"
  // | "codex_503". A string: callers pass unchecked text, which setDebugRoutingConfig rejects.
  using DebugRoutingScenario = std::string;

  struct DebugRoutingConfig {
    DebugRoutingScenario scenario;
    std::optional<double> expires_at_ms;
    double updated_at_ms;
    friend bool operator==(const DebugRoutingConfig&, const DebugRoutingConfig&) = default;
  };

  void tag_invoke(const json::value_from_tag&, json::value& out, const DebugRoutingConfig& value);
  DebugRoutingConfig tag_invoke(const json::value_to_tag<DebugRoutingConfig>&, const json::value& value);

  // `loadDebugRoutingConfig(nowMs = Date.now())`
  rt::Task<DebugRoutingConfig> loadDebugRoutingConfig();
  rt::Task<DebugRoutingConfig> loadDebugRoutingConfig(double nowMs);

  // `setDebugRoutingConfig(scenario, durationMs, nowMs = Date.now())`. Rejects with rt::JsError
  // "Unknown debug routing scenario", "Debug routing duration must be between 0 and 3600000
  // milliseconds" or "Deno KV is unavailable".
  rt::Task<DebugRoutingConfig> setDebugRoutingConfig(DebugRoutingScenario scenario, double durationMs);
  rt::Task<DebugRoutingConfig> setDebugRoutingConfig(DebugRoutingScenario scenario, double durationMs, double nowMs);

  void resetDebugRoutingCacheForTest();
}
