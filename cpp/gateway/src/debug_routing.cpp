#include "uos/debug_routing.hpp"
#include "uos/rt/kv_js.hpp"

#include "uos/kv.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"

#include <array>
#include <memory>
#include <string_view>
#include <utility>

namespace uos::debug_routing {
  namespace {
    using rt::record::Reader;
    using rt::record::Writer;

    constexpr std::array<std::string_view, 7> SCENARIOS{"normal", "metered_first", "removed_provider_first", "codex_401", "codex_403", "codex_429", "codex_503"};

    bool hasScenario(std::string_view scenario) {
      for (std::string_view known : SCENARIOS) {
        if (known == scenario) return true;
      }
      return false;
    }

    // `typeof value === "object" && value !== null` (utils.isRecord) over a stored value.

    // `typeof v === "number" && Number.isSafeInteger(v) && v > 0`
    bool isPositiveSafeInteger(const rt::KvValue& value) { return value.isNumber() && rt::Number::isSafeInteger(value.asNumber()) && value.asNumber() > 0; }

    std::optional<DebugRoutingConfig> normalize(const rt::KvValue& value) {
      if (!rt::kvIsObject(value)) return std::nullopt;
      const rt::KvValue& rawScenario = value["scenario"];
      const std::optional<std::string> scenario = rawScenario.isString() ? std::optional<std::string>(rt::trim(rawScenario.asString())) : std::nullopt;
      if (!rt::truthy(scenario) || !hasScenario(*scenario)) return std::nullopt;
      const rt::KvValue& expires = value["expires_at_ms"];
      const std::optional<double> expiresAtMs = isPositiveSafeInteger(expires) ? std::optional<double>(expires.asNumber()) : std::nullopt;
      const rt::KvValue& updated = value["updated_at_ms"];
      const double updatedAtMs = isPositiveSafeInteger(updated) ? updated.asNumber() : 0;
      if (!rt::truthy(updatedAtMs)) return std::nullopt;
      return DebugRoutingConfig{.scenario = *scenario, .expires_at_ms = expiresAtMs, .updated_at_ms = updatedAtMs};
    }

    // `let cached: Readonly<{ value: DebugRoutingConfig; expiresAtMs: number }> | null`
    struct CachedDebugRouting {
      DebugRoutingConfig value;
      double expiresAtMs;
    };

    std::optional<CachedDebugRouting>& cached() {
      static std::optional<CachedDebugRouting> value;
      return value;
    }

    constexpr double CACHE_TTL_MS = 5'000;

    DebugRoutingConfig normalConfig(double nowMs) {
      return DebugRoutingConfig{
        .scenario = "normal",
        .expires_at_ms = std::nullopt,
        .updated_at_ms = nowMs,
      };
    }

    rt::KvValue storedConfig(const DebugRoutingConfig& value) { return rt::KvValue(json::value_from(value)); }
  }

  void tag_invoke(const json::value_from_tag&, json::value& out, const DebugRoutingConfig& value) {
    out = Writer().required("scenario", value.scenario).orNull("expires_at_ms", value.expires_at_ms).required("updated_at_ms", value.updated_at_ms).done();
  }

  DebugRoutingConfig tag_invoke(const json::value_to_tag<DebugRoutingConfig>&, const json::value& value) {
    const Reader in(value, "DebugRoutingConfig");
    return {
      .scenario = in.required<std::string>("scenario"),
      .expires_at_ms = in.orNull<double>("expires_at_ms"),
      .updated_at_ms = in.required<double>("updated_at_ms"),
    };
  }

  rt::Task<DebugRoutingConfig> loadDebugRoutingConfig() {
    rt::Task<DebugRoutingConfig> loading = loadDebugRoutingConfig(rt::dateNow());
    DebugRoutingConfig config = co_await std::move(loading);
    co_return config;
  }

  rt::Task<DebugRoutingConfig> loadDebugRoutingConfig(double nowMs) {
    if (cached() && cached()->expiresAtMs > nowMs) {
      if (cached()->value.expires_at_ms && *cached()->value.expires_at_ms <= nowMs) co_return normalConfig(nowMs);
      co_return cached()->value;
    }
    std::optional<DebugRoutingConfig> loaded;
    bool failed = false;
    try {
      const rt::Promise<std::shared_ptr<rt::Kv>> opening = kv::getKv();
      const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
      if (kv) {
        const rt::KvGetOptions strong{.consistency = rt::KvConsistencyLevel::strong};
        const rt::KvEntryMaybe<> entry = co_await kv->get(DEBUG_ROUTING_KEY, strong);
        loaded = normalize(entry.value);
        if (loaded && !(loaded->expires_at_ms && *loaded->expires_at_ms <= nowMs)) {
          cached() = CachedDebugRouting{.value = *loaded, .expiresAtMs = nowMs + CACHE_TTL_MS};
        } else {
          loaded.reset();
        }
      }
    } catch (...) {
      failed = true;
    }
    if (failed || !loaded) co_return normalConfig(nowMs);
    co_return *loaded;
  }

  rt::Task<DebugRoutingConfig> setDebugRoutingConfig(DebugRoutingScenario scenario, double durationMs) {
    rt::Task<DebugRoutingConfig> setting = setDebugRoutingConfig(std::move(scenario), durationMs, rt::dateNow());
    DebugRoutingConfig config = co_await std::move(setting);
    co_return config;
  }

  rt::Task<DebugRoutingConfig> setDebugRoutingConfig(DebugRoutingScenario scenario, double durationMs, double nowMs) {
    if (!hasScenario(scenario)) throw rt::JsError("Unknown debug routing scenario");
    if (!rt::Number::isSafeInteger(durationMs) || durationMs < 0 || durationMs > DEBUG_ROUTING_MAX_DURATION_MS) {
      throw rt::JsError("Debug routing duration must be between 0 and 3600000 milliseconds");
    }
    const DebugRoutingConfig value{
      .scenario = scenario,
      .expires_at_ms = scenario == "normal" || durationMs == 0 ? std::nullopt : std::optional<double>(nowMs + durationMs),
      .updated_at_ms = nowMs,
    };
    const rt::Promise<std::shared_ptr<rt::Kv>> opening = kv::getKv();
    const std::shared_ptr<rt::Kv> kv = co_await opening.wait();
    if (!kv) throw rt::JsError("Deno KV is unavailable");
    const rt::KvValue stored = storedConfig(value);
    const rt::KvSetOptions options{.expireIn = DEBUG_ROUTING_MAX_DURATION_MS + 5 * 60'000};
    co_await kv->set(DEBUG_ROUTING_KEY, stored, options);
    cached() = CachedDebugRouting{.value = value, .expiresAtMs = nowMs + CACHE_TTL_MS};
    co_return value;
  }

  void resetDebugRoutingCacheForTest() { cached().reset(); }
}
