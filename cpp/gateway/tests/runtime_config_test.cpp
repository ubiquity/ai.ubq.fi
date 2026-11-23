// Links: tests/runtime_config_test.cpp src/auth/relay.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp
//        src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp
//        src/authority/native_codex_mutations.cpp src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp
//        src/bounded_response_body.cpp src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp
//        src/cache/prompt_analytics_read.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp src/codex/auth.cpp
//        src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp
//        src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/native_auth.cpp
//        src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp
//        src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp
//        src/config.cpp src/http.cpp src/inference_deadline.cpp src/kv.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp
//        src/optional_telemetry_queue.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp
//        src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp
//        src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
//        src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp
//        src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_quota.cpp
//        src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/selection.cpp src/release.cpp src/runtime/http.cpp
//        src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp
//        src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp
//        src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/types.cpp src/utils.cpp src/models/codex_models_fetch.cpp
//        src/runtime_config.cpp src/models/codex_models.cpp src/defaults.cpp
// Port of tests/runtime-config.test.ts: all four original cases and assertions.
#include "uos/runtime_config.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/test_async.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/text_codec.hpp"

#include <string>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::runtime_config;

namespace {
  double jsonBytes(const json::value& value) { return static_cast<double>(rt::utf8ByteLength(rt::jsonStringify(value))); }
}

TEST("runtime config compacts a near-limit full catalog into one 4 KiB read unit") {
  json::value fullSnapshot = rt::jsonParse(R"({
    "models": [
      {
        "slug": "gpt-compact-test",
        "display_name": "GPT Compact Test",
        "description": "",
        "visibility": "list",
        "supported_in_api": true,
        "context_window": 272000,
        "max_context_window": 1000000,
        "auto_compact_token_limit": 250000,
        "default_reasoning_level": "high",
        "supported_reasoning_levels": [{ "effort": null }, "low", "medium", "high", "xhigh", "ultra"],
        "reasoning_effort_wire_map": { "ultra": "max" }
      }
    ],
    "source": "chatgpt_codex",
    "client_version": "0.201.0",
    "updated_at_ms": 1000000
  })");
  fullSnapshot.as_object()["models"].as_array()[0].as_object()["description"] = rt::repeat("x", 57'000);
  EXPECT(jsonBytes(fullSnapshot) > 55'000);
  EXPECT(jsonBytes(fullSnapshot) < 60'000);

  const RuntimeConfigV2 runtime = buildRuntimeConfig(
    fullSnapshot.as_object(), BuildRuntimeConfigOptions{.defaultModel = "gpt-compact-test", .defaultReasoningEffort = "xhigh", .nowMs = 1'000'001});

  EXPECT(jsonBytes(toJson(runtime)) <= RUNTIME_CONFIG_MAX_BYTES);
  EXPECT(rt::deepEqual(json::value_from(runtime.codex_models.models), rt::jsonParse(R"([
    {
      "slug": "gpt-compact-test",
      "default_reasoning_level": "high",
      "supported_reasoning_levels": ["none", "low", "medium", "high", "xhigh", "ultra"],
      "reasoning_effort_wire_map": { "ultra": "max" }
    }
  ])")));
  EXPECT(normalizeRuntimeConfig(toJson(runtime)) == std::optional<RuntimeConfigV2>(runtime));
}

TEST("runtime config rejects an extreme model inventory instead of crossing one read unit") {
  json::array models;
  for (int index = 0; index < 200; ++index) {
    json::value model = rt::jsonParse(R"({
      "supported_reasoning_levels": ["none", "low", "medium", "high", "xhigh", "ultra"],
      "reasoning_effort_wire_map": { "ultra": "max" }
    })");
    rt::objectDefine(model.as_object(), "slug", json::string("gpt-runtime-model-" + rt::padStart(std::to_string(index), 3, "0")));
    models.push_back(std::move(model));
  }
  json::object fullSnapshot;
  fullSnapshot["models"] = std::move(models);
  fullSnapshot["source"] = "chatgpt_codex";
  fullSnapshot["client_version"] = "0.201.0";
  fullSnapshot["updated_at_ms"] = 1'000'000;
  EXPECT(jsonBytes(json::value(fullSnapshot)) < 60'000);
  bool rejected = false;
  try {
    (void)buildRuntimeConfig(fullSnapshot, BuildRuntimeConfigOptions{.nowMs = 1'000'001});
  } catch (const RuntimeConfigError& error) {
    rejected = error.message.find("too large") != std::string::npos;
  }
  EXPECT(rejected);
}

TEST("runtime config keeps prompt-cache controls compact and excludes probe scope evidence") {
  json::value promptCache = rt::jsonParse(R"({
    "version": 1,
    "providers": [
      {
        "id": "codex_chatgpt",
        "controls": {
          "key": true,
          "modes": ["implicit", "explicit"],
          "source": "catalog",
          "verified_at_ms": 1000000
        },
        "scope": {
          "probe_profile": "responses_implicit_input_text_keyed_cycle_isolated_v5",
          "account_slots": "shared",
          "token_refresh": "preserved",
          "conversation_id": "independent",
          "effective_model": "",
          "reproducible_cycles": 3,
          "source": "live_probe",
          "verified_at_ms": 1000001
        }
      }
    ]
  })");
  promptCache.as_object()["providers"].as_array()[0].as_object()["scope"].as_object()["effective_model"] = rt::repeat("gpt-cache-runtime-", 500);
  json::object model;
  model["slug"] = "gpt-cache-runtime";
  model["prompt_cache"] = promptCache;
  json::object snapshot;
  snapshot["models"] = json::array{model};
  snapshot["source"] = "chatgpt_codex";
  snapshot["updated_at_ms"] = 1'000'000;
  const RuntimeConfigV2 runtime = buildRuntimeConfig(snapshot, BuildRuntimeConfigOptions{.nowMs = 1'000'002});

  ASSERT(!runtime.codex_models.models.empty() && runtime.codex_models.models[0].prompt_cache.has_value());
  const json::value compacted = uos::models::codex_models::toJson(*runtime.codex_models.models[0].prompt_cache);
  json::object expectedProvider;
  expectedProvider["id"] = "codex_chatgpt";
  expectedProvider["controls"] = promptCache.as_object()["providers"].as_array()[0].as_object()["controls"];
  json::object expected;
  expected["version"] = 1;
  expected["providers"] = json::array{expectedProvider};
  EXPECT(rt::deepEqual(compacted, json::value(expected)));
  const json::value* providers = rt::get(compacted, "providers");
  const json::value* first = providers != nullptr && providers->is_array() ? rt::get(providers->get_array(), 0) : nullptr;
  EXPECT(first == nullptr || !rt::hasOwn(first->get_object(), "scope"));
  EXPECT(jsonBytes(toJson(runtime)) <= RUNTIME_CONFIG_MAX_BYTES);
  EXPECT(normalizeRuntimeConfig(toJson(runtime)) == std::optional<RuntimeConfigV2>(runtime));
}

ASYNC_TEST("full Codex model loader preserves catalog fields outside the inference runtime record") {
  const rt::env::ScopedEnvironment environment(std::vector<std::pair<std::string, std::string>>{});
  const std::string savedSeed = uos::config::config.codexAuthJsonB64;
  uos::config::config.codexAuthJsonB64.clear();
  const auto restore = rt::scopeExit([savedSeed] { uos::config::config.codexAuthJsonB64 = savedSeed; });
  const json::object fullSnapshot = rt::jsonParse(R"({"models":[{"slug":"gpt-full-loader","display_name":"GPT Full Loader","description":"Admin-only catalog detail","context_window":272000}],"source":"chatgpt_codex","client_version":"0.201.0","updated_at_ms":1000000})").as_object();
  const auto kv = std::make_shared<rt::MemoryKv>([] { return rt::dateNow(); });
  const rt::KvValue stored(fullSnapshot);
  auto writing = kv->set(uos::codex::auth::CODEX_MODELS_KV_KEY, stored);
  co_await std::move(writing);
  const std::optional<std::shared_ptr<rt::Kv>> kvOverride(kv);
  auto loading = uos::models::codex_models_fetch::loadFullCodexModelsSnapshot(kvOverride);
  const auto loaded = co_await std::move(loading);
  ASSERT(loaded);
  EXPECT(rt::deepEqual(json::value(*loaded), json::value(fullSnapshot)));
  const auto runtime = buildRuntimeConfig(fullSnapshot, BuildRuntimeConfigOptions{.nowMs = 1'000'001});
  const json::value compacted = json::value_from(runtime.codex_models.models[0]);
  EXPECT(!rt::hasOwn(compacted.as_object(), "description"));
  EXPECT(!rt::hasOwn(compacted.as_object(), "context_window"));
}
