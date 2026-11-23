// Port of all 27 original tests/admin-auth-part1.test.ts registrations, in source order.
// Links: tests/admin_auth_part1_test.cpp tests/helpers/admin_auth_harness.cpp src/agent_messages.cpp src/authority/native_kernel.cpp src/admin/defaults.cpp src/defaults.cpp src/kernel/quota_v2.cpp src/admin/api_keys.cpp src/models/codex_models_fetch.cpp src/runtime_config.cpp src/admin/kernel.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
// src/authority/native_api_key_policy.cpp src/api_keys.cpp src/codex/banked_reset_provider.cpp src/types.cpp src/kernel/usage.cpp src/metered_quota.cpp src/paid_fallback/ledger_stats.cpp src/api_key_policy.cpp src/config.cpp src/paid_fallback/rollups.cpp src/cache/kv_migration_base.cpp src/models/catalog.cpp
// src/authority/native_paid_policy.cpp src/release.cpp src/deepseek/waterfall.cpp src/provider/capacity_parse.cpp src/input_normalization.cpp src/codex/auth_refresh.cpp src/quota_projection.cpp src/authority/native_paid.cpp src/codex/dispatch.cpp src/codex/auth.cpp src/paid_fallback/ledger_state.cpp
// src/kv_conversion/profile_json.cpp src/authority/native_paid_contracts.cpp src/codex/reset_settings.cpp src/models/metadata.cpp src/paid_fallback/ledger_backfill.cpp src/models/codex_models.cpp src/admin/api_key_mutations.cpp src/kernel/quota_reservations.cpp src/paid_fallback/ledger_settlement.cpp
// src/models/codex_models_whitelist.cpp src/kernel/quota_write.cpp src/admin/api_key_audit.cpp src/provider/cerebras.cpp src/http.cpp src/models/codex_models_availability.cpp src/authority/native_api_key.cpp src/provider/surplus.cpp src/analytics.cpp src/codex/overage_settings.cpp
// src/provider/capacity_events.cpp src/bounded_response_body.cpp src/codex/capacity_routing.cpp src/utils.cpp src/provider/lithos.cpp src/request_policy.cpp src/provider/capacity.cpp src/provider/capacity_contract.cpp src/authority/native_codex_capacity.cpp src/admin/codex.cpp
// src/provider/anthropic_chat_responses.cpp src/codex/native_auth.cpp src/codex/external_auth.cpp src/authority/native_codex_contracts.cpp src/provider/claude_capacity_rollups.cpp src/provider/health.cpp src/provider/surplus_claude_request.cpp src/paid_fallback/ledger_admission.cpp src/provider/metered.cpp
// src/provider/claude_capacity_history.cpp src/provider/selection.cpp src/codex/experiment.cpp src/codex/banked_reset.cpp src/provider/openrouter.cpp src/authority/native_codex_evaluation.cpp src/catalog/types.cpp src/models/openrouter_models.cpp src/codex/routing_mutations.cpp src/paid_fallback/index.cpp
// src/codex/rate_limit_429.cpp src/handler/index.cpp src/provider/capacity_history.cpp src/provider/anthropic_chat_stream.cpp src/cache/prompt_analytics_read.cpp src/kernel/attestation.cpp src/debug_routing.cpp src/cache/scope_experiment_targets.cpp src/authority/native_codex_429.cpp
// src/cache/prompt_analytics_core.cpp src/health.cpp src/provider/claude_code_accounts.cpp src/cache/scope_experiment_model.cpp src/cache/telemetry_gate.cpp src/admin/claude_code.cpp src/authority/native_api_key_audit.cpp src/provider/capacity_rollups.cpp src/provider/cerebras_rate_limits.cpp
// src/auth/passkeys.cpp src/admin/chart_source_proxy.cpp src/provider/claude_code_models.cpp src/provider/claude_code_access.cpp src/auth/relay.cpp src/provider/claude_code_credentials.cpp src/kv.cpp src/provider/anthropic_responses.cpp src/provider/capacity_analytics.cpp
// src/authority/native_kernel_contracts.cpp src/auth/index.cpp src/provider/anthropic_response_wire.cpp src/authority/native_codex_mutations.cpp src/handler/terminal_route.cpp src/request.cpp src/admin/capacity_observation_sync.cpp src/provider/claude_code.cpp src/codex/supervisor_brief.cpp
// src/codex/supervisor.cpp src/catalog/index.cpp src/systemone/handlers.cpp src/kernel/policy_queue.cpp src/handler/http.cpp src/openai_telemetry.cpp src/provider/lithos_rate_limits.cpp src/provider/lithos_models.cpp src/cache/kv_migration_validation.cpp src/responses_failover_stream.cpp
// src/catalog/models.cpp src/sentinel/replay_observation.cpp src/catalog/store.cpp src/sentinel/replay_admin.cpp src/codex/account_routing.cpp src/handler/terminal_log.cpp src/recent_model_context.cpp src/auth/local_admin.cpp src/provider/claude_code_quota.cpp src/provider/surplus_claude_chat_request.cpp
// src/deepseek/index.cpp src/sentinel/replay_model.cpp src/codex/supervisor_config.cpp src/responses_stream.cpp src/admin/error_log.cpp src/cache/kv_migration_paid_fallback.cpp src/sentinel/replay_read.cpp src/admin/sentinel_incident.cpp src/codex/routing_state.cpp src/chat/envelope.cpp
// src/auth/local_development_key.cpp src/cache/scope_experiment.cpp src/embeddings/jobs.cpp src/auth/passport.cpp src/cache/scope_targets.cpp src/sentinel/replay_store.cpp src/openai.cpp src/codex/supervisor_secret.cpp src/provider/openrouter_handlers.cpp src/sentinel/replay_retention.cpp
// src/paid_fallback/health.cpp src/provider/claude_code_usage_schema.cpp src/static.cpp src/cache/kv_migration_usage.cpp src/sentinel/replay_retention_schema.cpp src/codex/supervisor_follow.cpp src/embeddings/ledger.cpp src/sentinel/upstream_capture.cpp src/codex/manual_reset_state.cpp
// src/admin/codex_reset.cpp src/live/handler.cpp src/sentinel/incident_outbox.cpp src/images.cpp src/paid_fallback/routing.cpp src/provider/presentation.cpp src/embeddings/voyage.cpp src/sentinel/replay_envelope.cpp src/codex/routing_serial.cpp src/codex/routing_evaluation.cpp src/codex/supervisor_log.cpp
// src/codex/native_auth_transport.cpp src/storage/kv.cpp src/upstream_wire.cpp src/responses_handler.cpp src/embeddings/handlers.cpp src/provider/openrouter_streams.cpp src/codex/manual_reset.cpp src/live/routes.cpp src/codex/supervisor_transport.cpp src/decisions/handlers.cpp src/live/sideband.cpp
// src/auth/passport_sessions.cpp src/responses_attempts.cpp src/handler/admission.cpp src/provider/anthropic_replay.cpp src/codex/supervisor_inventory.cpp src/codex/banked_reset_pool.cpp src/authority/native_codex.cpp src/provider/openrouter_web_tools.cpp src/sentinel/replay_retention_bootstrap.cpp
// src/responses_native_headers.cpp src/paid_fallback/removed_provider.cpp src/codex/banked_reset_claim.cpp src/openai_schema.cpp src/provider/cerebras_handlers.cpp src/deepseek/flash_aliases.cpp src/paid_fallback/claude_code_route.cpp src/sentinel/replay_limits.cpp src/live/upstream.cpp
// src/chat/stream_translation.cpp src/provider/removed_provider_circuit.cpp src/inference_deadline.cpp src/runtime/http_tasks.cpp src/sentinel/replay_retention_metadata.cpp src/provider/cerebras_responses.cpp src/responses_request.cpp src/deepseek/responses_payload.cpp
// src/provider/openrouter_request_controls.cpp src/provider/removed_provider_telemetry.cpp src/kernel/quota_reserve.cpp src/deepseek/responses.cpp src/catalog/promotion.cpp src/codex/quota.cpp src/live/calls.cpp src/inference_admission.cpp src/provider/stream_relay.cpp src/codex/banked_reset_submission.cpp
// src/chat/completions.cpp src/codex/responses_error.cpp src/runtime/http_early_sse.cpp src/deepseek/responses_stream.cpp src/provider/lithos_handlers.cpp src/cache/prompt_analytics_queue.cpp src/storage/native_kv.cpp src/deepseek/handlers.cpp src/codex/index.cpp
// src/optional_telemetry_queue.cpp src/provider/lithos_streams.cpp src/deepseek/forwarded_payload_policy.cpp src/runtime/websocket.cpp src/deepseek/context_admission_failure.cpp src/storage/sqlite_readonly.cpp src/codex/responses_operations.cpp src/lib/jev_compaction/state.cpp
// src/lib/jev_compaction/codex_items.cpp src/responses_buffered.cpp src/deepseek/waterfall_handler.cpp src/deepseek/stream.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/types.cpp src/codex/responses_state.cpp src/deepseek/chat_projection.cpp src/codex/responses_runners.cpp
// src/lib/jev_compaction/request.cpp
#include "tests/helpers/admin_auth_harness.hpp"
#include "tests/helpers/responses_conformance_client.hpp"
#include "uos/admin/defaults.hpp"
#include "uos/authority/native_kernel.hpp"
#include "uos/config.hpp"
#include "uos/handler/index.hpp"
#include "uos/kv.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <string>
#include <vector>

#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  namespace rt = uos::rt; namespace json = uos::json; namespace h = uos::tests::helpers::admin_auth_harness;
  using namespace uos::tests::helpers::admin_auth_harness;
  std::vector<std::pair<std::string, std::string>> isolatedEnvironment() {
    std::vector<std::pair<std::string, std::string>> values;
    for (const auto* key : {"HOME", "TMPDIR"}) if (const auto value = rt::env::get(key)) values.emplace_back(key, *value);
    return values;
  }
  struct Fixture {
    uos::config::Config previous = uos::config::config;
    rt::env::ScopedEnvironment environment{isolatedEnvironment()};
    Fixture() { uos::config::config.codexAuthJsonB64.clear(); resetHarness(); uos::tests::helpers::responses_conformance_client::stageGatewayAssets(); }
    ~Fixture() { cleanupHarness(); uos::config::config = previous; }
  };
  rt::Request request(const json::value& body, const std::string& method = "POST", const std::string& path = "/admin/api-keys") {
    return rt::Request("https://ai.ubq.fi" + path, {.method = method, .headers = rt::Headers{{"Content-Type", "application/json"}}, .body = rt::BodyInit(rt::jsonStringify(body))});
  }
  const json::value& member(const json::value& value, std::string_view key) { const auto* found = rt::get(value, key); ASSERT(found); return *found; }
  bool hasOwn(const json::value& value, std::string_view key) { return value.is_object() && rt::hasOwn(value.as_object(), key); }
  std::string str(const json::value& value, std::string_view key) { const auto found = rt::getString(value, key); ASSERT(found); return std::string(*found); }
  double num(const json::value& value, std::string_view key) { const auto found = rt::getNumber(value, key); ASSERT(found); return *found; }
  json::value stored(const rt::KvKey& key) { const auto* value = kvStore.get(keyToString(key)); ASSERT(value); return value->toJson(); }
  void put(const rt::KvKey& key, const json::value& value) { kvStore.set(keyToString(key), rt::KvValue(value)); }
  const json::value& first(const json::value& body) { const auto& rows = member(body, "data").as_array(); ASSERT(!rows.empty()); return rows.front(); }
  json::object keyRecord(const std::string& id, double now, double reset) {
    return {{"id", id}, {"name", "V3 usage list"}, {"prefix", "u_1234567890"}, {"hash", "hash-" + id}, {"created_at_ms", now}, {"expires_at_ms", -1}, {"revoked_at_ms", nullptr}, {"usage_limit_requests", 10}, {"usage_requests", 17}, {"usage_reset_at_ms", reset}, {"window_ms", 60000}, {"usage_quota_version", 3}, {"paid_fallback_enabled", false}, {"paid_fallback_limit_microcredits", 0}, {"paid_fallback_spent_microcredits", 0}, {"paid_fallback_reserved_microcredits", 0}, {"paid_fallback_reservation_request_id", nullptr}, {"paid_fallback_model_ids", json::array()}, {"paid_fallback_quota_per_credit", 0}, {"paid_fallback_pricing_checked_at_ms", nullptr}};
  }
  std::vector<std::pair<std::string, std::string>> storeSnapshot() { std::vector<std::pair<std::string, std::string>> rows; for (const auto& [key, value] : kvStore) rows.emplace_back(key, rt::jsonStringify(value.toJson())); return rows; }
  rt::Fetch modelsMock(json::array models, int status = 200) { return rt::Fetch([models = std::move(models), status](rt::RequestInfo, rt::RequestInit) { return rt::Response::json(json::object{{"models", models}}, {.status = status}); }); }
  json::array slugs(const json::value& catalog) { json::array values; for (const auto& model : member(catalog, "models").as_array()) values.push_back(member(model, "slug")); return values; }
  rt::Task<void> defaultTier(std::string slug, json::array tiers, json::value effort, int expectedStatus) {
    Fixture fixture;
    seedCodexSnapshot(json::object{{"source", "codex_cli"}, {"updated_at_ms", 123}, {"models", json::array{json::object{{"slug", slug}, {"display_name", slug == "gpt-5.6-sol" ? "GPT-5.6 Sol" : "GPT-5.5"}, {"default_reasoning_level", "medium"}, {"supported_reasoning_levels", tiers}}}}});
    const json::object body{{"model", slug}, {"reasoning_effort", effort}};
    AWAIT(response, handleAdminDefaults(request(body, "POST", "/admin/defaults"))); EXPECT_EQ(response.status(), expectedStatus); AWAIT(payload, response.json());
    if (expectedStatus == 200) {
      EXPECT(rt::deepEqual(member(member(payload, "defaults"), "reasoning_effort"), effort));
      EXPECT(rt::deepEqual(member(stored(rt::KvKey{"uos_ai", "runtime_config", "v2"}), "default_reasoning_effort"), effort));
    } else {
      EXPECT(rt::Regex("reasoning_effort must be a non-empty string").test(str(member(payload, "error"), "message")));
      EXPECT(!kvStore.has(keyToString(rt::KvKey{"default", "reasoning_effort"})));
    }
  }
}

ASYNC_TEST("subscription reset settings require admin authentication") {
  Fixture fixture; const std::vector<std::string> methods{"GET", "PATCH"};
  for (const auto& method : methods) {
    AWAIT(response, uos::handler::index::handler(rt::Request("http://localhost/admin/providers/codex/banked-resets", {.method = method})));
    EXPECT_EQ(response.status(), 401); if (auto body = response.body()) { auto cancelling = body->cancel(); co_await std::move(cancelling); }
  }
}
ASYNC_TEST("API key list reports committed V3 usage and ignores legacy counters") {
  Fixture fixture; const std::string id = "v2-usage-list"; const double windowMs = 60000; const auto start = rt::dateNow(); const auto reset = start + windowMs;
  put(rt::KvKey{"ubq_ai", "api_keys", "id", id}, keyRecord(id, start, reset));
  kvStore.set(keyToString(rt::KvKey{"uos_ai", "api_key_usage", "v2", id, "60000", start}), rt::KvValue::object({{"value", rt::KvValue(rt::BigInt(3))}}));
  put(rt::KvKey{"uos_ai", "api_key_usage", "v3", "window", id, "v3:60000", start}, json::object{{"v", 3}, {"key_id", id}, {"policy_version", "v3:60000"}, {"window_start_ms", start}, {"window_reset_at_ms", reset}, {"committed_requests", 4}, {"reserved_requests", 1}, {"updated_at_ms", start}});
  AWAIT(withoutUsage, handleAdminApiKeysList(rt::Request("https://ai.ubq.fi/admin/api-keys"))); EXPECT_EQ(withoutUsage.status(), 200); AWAIT(withoutPayload, withoutUsage.json()); EXPECT(!hasOwn(first(withoutPayload), "usage_requests"));
  AWAIT(withUsage, handleAdminApiKeysList(rt::Request("https://ai.ubq.fi/admin/api-keys?include_usage=1"))); EXPECT_EQ(withUsage.status(), 200); AWAIT(withPayload, withUsage.json()); EXPECT_EQ(num(first(withPayload), "usage_requests"), 4); EXPECT_EQ(num(member(first(withPayload), "usage"), "request_count"), 4);
}
ASYNC_TEST("API key list projects current paid fallback totals from V3 only") {
  Fixture fixture; const std::string id = "paid-fallback-v3-projection"; const auto now = rt::dateNow(); const auto reset = now + 60000;
  auto record = keyRecord(id, now - 1000, reset); record["name"] = "V3 projection"; record["usage_limit_requests"] = -1; record["usage_requests"] = 0; rt::objectDelete(record, "usage_quota_version"); record["paid_fallback_enabled"] = true; record["paid_fallback_limit_microcredits"] = 2000000; record["paid_fallback_spent_microcredits"] = 1000000; record["paid_fallback_reserved_microcredits"] = 500000; record["paid_fallback_reservation_request_id"] = "legacy-stale-reservation"; record["paid_fallback_model_ids"] = json::array{"gpt-5.6-sol"}; record["paid_fallback_quota_per_credit"] = 500000; record["paid_fallback_pricing_checked_at_ms"] = now;
  put(rt::KvKey{"ubq_ai", "api_keys", "id", id}, record);
  put(rt::KvKey{"uos_ai", "paid_fallback", "v3", "window", id, reset}, json::object{{"v", 3}, {"key_id", id}, {"policy_version", "60000"}, {"window_reset_at_ms", reset}, {"limit_microcredits", 2000000}, {"settled_microcredits", 28992}, {"reserved_microcredits", 125000}, {"pending_count", 1}, {"updated_at_ms", now}});
  put(rt::KvKey{"uos_ai", "paid_fallback", "v3", "request", id, "metered-request"}, json::object{{"request_id", "metered-request"}, {"provider", "metered"}, {"window_reset_at_ms", reset}, {"input_tokens", 3}, {"output_tokens", 4}, {"billing_state", "settled"}, {"spend_microcredits", 500}, {"created_at_ms", now - 2}});
  put(rt::KvKey{"uos_ai", "paid_fallback", "v3", "request", id, "surplus-request"}, json::object{{"request_id", "surplus-request"}, {"provider", "surplus"}, {"window_reset_at_ms", reset}, {"input_tokens", 11}, {"output_tokens", 7}, {"billing_state", "settled"}, {"spend_microcredits", 700}, {"created_at_ms", now - 1}});
  AWAIT(response, handleAdminApiKeysList(rt::Request("https://ai.ubq.fi/admin/api-keys"))); EXPECT_EQ(response.status(), 200); AWAIT(payload, response.json()); const auto& row = first(payload);
  EXPECT_EQ(num(row, "paid_fallback_limit_credits"), 2); EXPECT_EQ(num(row, "paid_fallback_spent_credits"), 0.028992); EXPECT_EQ(num(row, "paid_fallback_reserved_credits"), 0.125); EXPECT_EQ(num(row, "paid_fallback_pending_count"), 1);
  const auto& usage = member(row, "paid_fallback_provider_usage"); const auto& metered = member(usage, "metered"); const auto& surplus = member(usage, "surplus");
  EXPECT_EQ(num(metered, "request_count"), 1); EXPECT_EQ(num(metered, "total_tokens"), 7); EXPECT_EQ(num(metered, "spend_microcredits"), 500); EXPECT_EQ(num(surplus, "request_count"), 1); EXPECT_EQ(num(surplus, "total_tokens"), 18); EXPECT_EQ(num(surplus, "spend_microcredits"), 700);
}
ASYNC_TEST("API key limit edits retain V3 usage while resets create a fresh aggregate and reject live leases") {
  Fixture fixture; const auto token = "u_" + std::string(64, 'd');
  AWAIT(createdResponse, handleAdminApiKeysCreate(request(json::object{{"name", "V3 reset"}, {"token", token}, {"usage_limit_requests", 8}, {"window_ms", 60000}}))); EXPECT_EQ(createdResponse.status(), 200); AWAIT(created, createdResponse.json()); const auto id = str(created, "id");
  const auto record = stored(rt::KvKey{"ubq_ai", "api_keys", "id", id}); const auto oldStart = num(record, "usage_reset_at_ms") - num(record, "window_ms"); const rt::KvKey oldWindowKey{"uos_ai", "api_key_usage", "v3", "window", id, "v3:60000", oldStart}; auto oldWindow = stored(oldWindowKey).as_object(); oldWindow["committed_requests"] = 2; put(oldWindowKey, oldWindow);
  AWAIT(limitOnly, handleAdminApiKeysUpdate(request(json::object{{"id", id}, {"usage_limit_requests", 5}}, "PATCH"))); EXPECT_EQ(limitOnly.status(), 200); AWAIT(limitPayload, limitOnly.json()); EXPECT_EQ(num(limitPayload, "usage_requests"), 2); EXPECT_EQ(num(stored(oldWindowKey), "committed_requests"), 2);
  AWAIT(reset, handleAdminApiKeysUpdate(request(json::object{{"id", id}, {"reset_usage", true}}, "PATCH"))); EXPECT_EQ(reset.status(), 200); AWAIT(resetPayload, reset.json()); EXPECT_EQ(num(resetPayload, "usage_requests"), 0); EXPECT(num(resetPayload, "usage_reset_at_ms") != num(record, "usage_reset_at_ms"));
  const auto resetRecord = stored(rt::KvKey{"ubq_ai", "api_keys", "id", id}); const auto resetStart = num(resetRecord, "usage_reset_at_ms") - num(resetRecord, "window_ms"); const rt::KvKey resetWindowKey{"uos_ai", "api_key_usage", "v3", "window", id, "v3:60000", resetStart}; auto resetWindow = stored(resetWindowKey).as_object(); resetWindow["reserved_requests"] = 1; put(resetWindowKey, resetWindow);
  put(rt::KvKey{"uos_ai", "api_key_usage", "v3", "request", id, "v3:60000", resetStart, "live-reset-lease"}, json::object{{"v", 3}, {"key_id", id}, {"request_id", "live-reset-lease"}, {"route", "responses"}, {"state", "reserved"}, {"reserved_at_ms", rt::dateNow()}, {"lease_expires_at_ms", rt::dateNow() + 60000}, {"provider", nullptr}, {"dispatched_at_ms", nullptr}, {"released_at_ms", nullptr}, {"release_reason", nullptr}});
  AWAIT(blocked, handleAdminApiKeysUpdate(request(json::object{{"id", id}, {"reset_usage", true}}, "PATCH"))); EXPECT_EQ(blocked.status(), 409);
}
ASYNC_TEST("admin Codex model GET returns the full catalog rather than the compact runtime record") {
  Fixture fixture; seedCodexSnapshot(json::object{{"models", json::array{json::object{{"slug", "gpt-admin-full-catalog"}, {"display_name", "GPT Admin Full Catalog"}, {"description", "full catalog description"}, {"context_window", 272000}, {"supported_reasoning_levels", json::array{"none", "high"}}}}}, {"source", "chatgpt_codex"}, {"client_version", "0.201.0"}, {"updated_at_ms", rt::dateNow()}});
  AWAIT(response, handleAdminCodexModelsGet()); EXPECT_EQ(response.status(), 200); AWAIT(payload, response.json()); const auto& model = member(member(payload, "data"), "models").as_array().front(); EXPECT_EQ(str(model, "description"), "full catalog description"); EXPECT_EQ(num(model, "context_window"), 272000);
}
ASYNC_TEST("admin Codex model update rejects a catalog whose compact runtime record exceeds 4 KiB") {
  Fixture fixture; json::array models; for (int index = 0; index < 200; ++index) models.push_back(json::object{{"slug", "gpt-admin-runtime-" + rt::padStart(std::to_string(index), 3, "0")}, {"supported_reasoning_levels", json::array{"none", "low", "medium", "high", "xhigh", "ultra"}}});
  AWAIT(response, handleAdminCodexModelsSet(makeRequest(json::object{{"source", "chatgpt_codex"}, {"client_version", "0.201.0"}, {"models", models}}))); EXPECT_EQ(response.status(), 413); AWAIT(payload, response.json()); const auto& error = member(payload, "error"); EXPECT_EQ(str(error, "code"), "runtime_config_invalid"); EXPECT(rt::Regex("runtime config is too large").test(str(error, "message"))); EXPECT(!kvStore.has(keyToString(rt::KvKey{"ubq_ai", "codex_models"}))); EXPECT(!kvStore.has(keyToString(rt::KvKey{"uos_ai", "runtime_config", "v2"})));
}
ASYNC_TEST("admin defaults includes serializable Metered quota diagnostics without credentials") {
  Fixture fixture; uos::admin::defaults::AdminDefaultsDependencies deps;
  deps.getMeteredQuotaDiagnostics = []() -> rt::Task<uos::metered_quota::MeteredQuotaDiagnostics> { co_return uos::metered_quota::MeteredQuotaDiagnostics{.configured = true, .available = true, .cache_state = "fresh", .confidence = "refill_observed", .balance_credits = 75, .baseline_credits = 100, .remaining_percent = 75, .used_percent = 25, .observed_at_ms = 2000000, .cycle_started_at_ms = 1000000, .last_known_debits_credits = 1, .last_inferred_credit_credits = 50, .last_credit_at_ms = 1500000, .latest_refill_id = "refill-2", .latest_refill_amount_credits = 50, .latest_refill_completed_at_ms = 1400000}; };
  AWAIT(response, handleAdminDefaults(rt::Request("https://ai.ubq.fi/admin/defaults"), deps)); EXPECT_EQ(response.status(), 200); AWAIT(payload, response.json()); EXPECT(!hasOwn(payload, "ok")); const auto& quota = member(payload, "metered_quota"); EXPECT_EQ(num(quota, "remaining_percent"), 75); EXPECT_EQ(str(quota, "latest_refill_id"), "refill-2"); EXPECT(!hasOwn(quota, "system_token")); EXPECT(!hasOwn(quota, "user_id"));
}
ASYNC_TEST("admin defaults permits kernel-only updates without runtime configuration") {
  Fixture fixture;
  // The source fake omits versionstamp, outside the real SDK's string|null contract.
  // Only its acquired marker uses revision !== null, falsiness and the fake's no-op
  // atomic.check/delete. Empty string preserves those observations of undefined;
  // it is not a production revision or CAS proof. Null values and other keys stay
  // nullopt, and this local adapter cannot wrap any store except this exact fake.
  class OmittedMarkerRevisionKv final : public rt::ForwardingKv {
  public:
    OmittedMarkerRevisionKv() : rt::ForwardingKv(*h::kvStub()) { ASSERT(&inner == h::kvStub().get()); }
    using rt::ForwardingKv::get;
    std::size_t adaptedReads = 0;
    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override {
      ASSERT(&inner == h::kvStub().get());
      const bool marker = key == uos::authority::native_kernel::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY;
      auto reading = rt::ForwardingKv::get(std::move(key), options);
      auto entry = co_await std::move(reading);
      if (marker && !entry.versionstamp && entry.value.isObject()) {
        const auto value = entry.value.toJson();
        const auto id = rt::getString(value, "id");
        if (rt::getNumber(value, "v") == std::optional(2.0) && id && !id->empty()) {
          entry.versionstamp = std::string();
          ++adaptedReads;
        }
      }
      co_return entry;
    }
  };
  const auto adapter = std::make_shared<OmittedMarkerRevisionKv>();
  AWAIT(absentMarker, adapter->get(uos::authority::native_kernel::KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY));
  EXPECT(absentMarker.value.isNull()); EXPECT(!absentMarker.versionstamp); EXPECT_EQ(adapter->adaptedReads, std::size_t(0));
  uos::kv::setKvForTest(adapter);
  AWAIT(response, handleAdminDefaults(request(json::object{{"kernel_policy_limit_requests", 17}, {"kernel_policy_window_ms", 60000}}, "POST", "/admin/defaults"))); EXPECT_EQ(response.status(), 200); AWAIT(payload, response.json()); const json::object expected{{"model", ""}, {"reasoning_effort", "medium"}, {"kernel_policy_limit_requests", 17}, {"kernel_policy_window_ms", 60000}}; EXPECT(rt::deepEqual(member(payload, "defaults"), expected)); EXPECT(rt::deepEqual(stored(rt::KvKey{"default", "kernel_policy_limit_requests"}), json::value(17))); EXPECT(rt::deepEqual(stored(rt::KvKey{"default", "kernel_policy_window_ms"}), json::value(60000))); EXPECT(!kvStore.has(keyToString(rt::KvKey{"uos_ai", "runtime_config", "v2"})));
  EXPECT(adapter->adaptedReads > 0);
  AWAIT(nonMarker, adapter->get(rt::KvKey{"default", "kernel_policy_limit_requests"})); EXPECT(!nonMarker.versionstamp);
  const std::vector<json::object> bodies{{{"model", "gpt-5.5"}}, {{"reasoning_effort", "none"}}}; for (const auto& body : bodies) { AWAIT(guarded, handleAdminDefaults(request(body, "POST", "/admin/defaults"))); EXPECT_EQ(guarded.status(), 503); }
}
ASYNC_TEST("admin defaults rejects late invalid fields without partial writes") {
  Fixture fixture; seedCodexSnapshot(json::object{{"source", "codex_cli"}, {"updated_at_ms", 123}, {"models", json::array{json::object{{"slug", "gpt-5.5"}, {"display_name", "GPT-5.5"}, {"default_reasoning_level", "medium"}, {"supported_reasoning_levels", json::array{"none", "medium"}}}}}}); const auto before = storeSnapshot();
  AWAIT(response, handleAdminDefaults(request(json::object{{"model", "gpt-5.5"}, {"kernel_policy_window_ms", 0}}, "POST", "/admin/defaults"))); EXPECT_EQ(response.status(), 400); EXPECT_EQ(storeSnapshot(), before);
}
ASYNC_TEST("admin kernel quota policies preserve usage until an explicit reset and return to default source") {
  Fixture fixture; const std::string owner = "kernel-owner", repo = "kernel-repo"; const auto policyKey = kernelRepoPolicyKey(owner, repo), windowKey = kernelRepoWindowKey(owner, repo); put(rt::KvKey{"default", "kernel_policy_limit_requests"}, 9); put(rt::KvKey{"default", "kernel_policy_window_ms"}, 60000);
  AWAIT(set, handleAdminKernelUsageSet(request(json::object{{"owner", owner}, {"repo", repo}, {"usage_limit_requests", 5}, {"window_ms", 60000}}, "PUT", "/admin/kernel-usage"))); EXPECT_EQ(set.status(), 200); auto initial = stored(windowKey).as_object(); initial["usage_requests"] = 4; put(windowKey, initial);
  AWAIT(limitOnly, handleAdminKernelUsageSet(request(json::object{{"owner", owner}, {"repo", repo}, {"usage_limit_requests", 3}, {"reset_usage", false}}, "PUT", "/admin/kernel-usage"))); EXPECT_EQ(limitOnly.status(), 200); EXPECT_EQ(num(stored(policyKey), "usage_limit_requests"), 3); EXPECT_EQ(num(stored(windowKey), "usage_requests"), 4);
  AWAIT(invalidReset, handleAdminKernelUsageSet(request(json::object{{"owner", owner}, {"repo", repo}, {"usage_limit_requests", 3}, {"reset_usage", "false"}}, "PUT", "/admin/kernel-usage"))); EXPECT_EQ(invalidReset.status(), 400); EXPECT_EQ(num(stored(windowKey), "usage_requests"), 4);
  AWAIT(reset, handleAdminKernelUsageSet(request(json::object{{"owner", owner}, {"repo", repo}, {"usage_limit_requests", 3}, {"reset_usage", true}}, "PUT", "/admin/kernel-usage"))); EXPECT_EQ(reset.status(), 200); EXPECT_EQ(num(stored(windowKey), "usage_requests"), 0); auto preserved = stored(windowKey).as_object(); preserved["usage_requests"] = 2; put(windowKey, preserved);
  AWAIT(deleted, handleAdminKernelUsageDelete(request(json::object{{"owner", owner}, {"repo", repo}}, "DELETE", "/admin/kernel-usage"))); EXPECT_EQ(deleted.status(), 200); EXPECT(!kvStore.has(keyToString(policyKey))); EXPECT_EQ(num(stored(windowKey), "usage_requests"), 2); AWAIT(snapshot, getKernelUsageLimitSnapshot(owner, repo)); ASSERT(snapshot); EXPECT_EQ(snapshot->source, "default"); EXPECT_EQ(snapshot->record.usage_requests, 2); EXPECT_EQ(snapshot->record.usage_limit_requests, 9);
}
ASYNC_TEST("admin codex auth stores live upstream model catalog as source of truth") {
  Fixture fixture; std::vector<std::string> urls;
  const json::array upstream{json::object{{"slug", "gpt-5.3-codex-spark"}, {"display_name", "GPT-5.3-Codex-Spark"}, {"visibility", "list"}, {"supported_in_api", false}, {"default_reasoning_level", "high"}, {"supported_reasoning_levels", json::array{json::object{{"effort", "low"}, {"description", "Fast responses"}}, json::object{{"effort", "medium"}, {"description", "Balanced reasoning"}}, json::object{{"effort", "high"}, {"description", "Greater reasoning depth"}}, json::object{{"effort", "xhigh"}, {"description", "Extra high reasoning depth"}}, json::object{{"effort", "max"}, {"description", "Maximum reasoning depth"}}, json::object{{"effort", "ultra"}, {"description", "Maximum reasoning with automatic task delegation"}}}}}, json::object{{"slug", "codex-auto-review"}, {"display_name", "Codex Auto Review"}, {"visibility", "hide"}, {"supported_in_api", true}}, json::object{{"slug", "codex-internal-evals"}, {"visibility", "hide"}, {"supported_in_api", false}}};
  const rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo input, rt::RequestInit) { urls.push_back(urlOf(input)); return rt::Response::json(json::object{{"models", upstream}}); }));
  const json::object models{{"source", "codex_cli"}, {"client_version", "0.126.0"}, {"updated_at_ms", 123}, {"models", json::array{json::object{{"slug", "stale-local-model"}, {"display_name", "Stale Local Model"}, {"context_window", 272000}, {"max_context_window", 1000000}, {"auto_compact_token_limit", nullptr}, {"supported_reasoning_levels", json::array{json::object{{"effort", nullptr}}, "low", "medium", "high", "xhigh"}}}, json::object{{"slug", "codex-auto-review"}, {"display_name", "Codex Auto Review"}, {"visibility", "hide"}, {"supported_in_api", true}}}}};
  AWAIT(response, handleAdminCodexAuth(makeRequest(json::object{{"auth", authPayload}, {"models", models}}))); EXPECT_EQ(response.status(), 200); AWAIT(payload, response.json()); EXPECT_EQ(num(member(payload, "models"), "count"), 2); EXPECT_EQ(str(member(payload, "models"), "source"), "chatgpt_codex"); EXPECT_EQ(urls.size(), std::size_t(1));
  const auto catalog = stored(rt::KvKey{"ubq_ai", "codex_models"}); EXPECT_EQ(str(catalog, "source"), "chatgpt_codex"); EXPECT_EQ(str(catalog, "client_version"), "0.126.0"); const json::array expectedSlugs{"gpt-5.3-codex-spark", "codex-auto-review"}; EXPECT(rt::deepEqual(slugs(catalog), expectedSlugs)); const auto& entries = member(catalog, "models").as_array(); ASSERT_EQ(entries.size(), std::size_t(2)); EXPECT_EQ(rt::getBool(entries[0], "supported_in_api"), std::optional(false)); EXPECT_EQ(str(entries[0], "default_reasoning_level"), "high"); const json::array tiers{"low", "medium", "high", "xhigh", "max", "ultra"}; EXPECT(rt::deepEqual(member(entries[0], "supported_reasoning_levels"), tiers)); const json::object wireMap{{"ultra", "max"}}; EXPECT(rt::deepEqual(member(entries[0], "reasoning_effort_wire_map"), wireMap)); EXPECT_EQ(str(entries[1], "visibility"), "hide"); EXPECT_EQ(rt::getBool(entries[1], "supported_in_api"), std::optional(true));
}
ASYNC_TEST("admin codex auth stores live model catalog without caller model snapshot") {
  Fixture fixture; const rt::GlobalFetchOverride fetch(modelsMock(json::array{json::object{{"slug", "gpt-5.3-codex-spark"}, {"visibility", "list"}}})); AWAIT(response, handleAdminCodexAuth(makeRequest(json::object{{"auth", authPayload}}))); EXPECT_EQ(response.status(), 200); const json::array expected{"gpt-5.3-codex-spark"}; EXPECT(rt::deepEqual(slugs(stored(rt::KvKey{"ubq_ai", "codex_models"})), expected));
}
ASYNC_TEST("admin Codex auth accepts authenticated 429 and preserves the last known-good catalog") {
  Fixture fixture; const json::object snapshot{{"source", "chatgpt_codex"}, {"client_version", "0.200.0"}, {"updated_at_ms", 123}, {"models", json::array{json::object{{"slug", "gpt-5.6"}, {"display_name", "GPT-5.6"}}}}}; seedCodexSnapshot(snapshot); put(rt::KvKey{"ubq_ai", "codex_catalog_auth_generation"}, "existing-generation"); put(rt::KvKey{"ubq_ai", "codex_auth"}, json::object{{"accounts", json::array{json::object{{"access_token", "limited-access"}, {"refresh_token", "limited-refresh"}, {"account_id", "limited-account"}, {"updated_at_ms", 100}}}}, {"updated_at_ms", 100}});
  const rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) { return rt::Response(std::string("{\"statusCode\":429,\"description\":\"Too Many Requests\"}"), {.status = 429, .headers = rt::Headers{{"Content-Type", "application/json"}}}); }));
  AWAIT(response, handleAdminCodexAuth(makeRequest(json::object{{"auth", authPayload}}))); EXPECT_EQ(response.status(), 200); AWAIT(payload, response.json()); EXPECT_EQ(num(payload, "account_count"), 2); EXPECT_EQ(num(payload, "upstream_status"), 429); EXPECT_EQ(rt::getBool(payload, "catalog_seeded"), std::optional(false)); EXPECT_EQ(rt::getBool(payload, "normalized_snapshot_updated"), std::optional(false)); EXPECT(rt::deepEqual(stored(rt::KvKey{"ubq_ai", "codex_models"}), snapshot)); EXPECT(rt::deepEqual(stored(rt::KvKey{"ubq_ai", "codex_catalog_auth_generation"}), json::value("existing-generation")));
}
ASYNC_TEST("admin Codex auth adds, rotates, and caps the two-account pool") {
  Fixture fixture; const rt::GlobalFetchOverride fetch(modelsMock(json::array{json::object{{"slug", "gpt-5.5"}}}));
  const auto upload = [](const std::string& accountId, const std::string& accessToken) { return handleAdminCodexAuth(makeRequest(json::object{{"auth", json::object{{"tokens", json::object{{"access_token", accessToken}, {"refresh_token", "refresh-" + accountId}, {"account_id", accountId}}}}}})); };
  AWAIT(firstResponse, upload("account-one", "access-one")); EXPECT_EQ(firstResponse.status(), 200); AWAIT(second, upload("account-two", "access-two")); EXPECT_EQ(second.status(), 200); AWAIT(secondPayload, second.json()); EXPECT_EQ(num(secondPayload, "account_count"), 2); const json::array ids{"account-one", "account-two"}; EXPECT(rt::deepEqual(member(secondPayload, "account_ids"), ids)); AWAIT(rotation, upload("account-one", "access-one-rotated")); EXPECT_EQ(rotation.status(), 200);
  const auto rotated = stored(rt::KvKey{"ubq_ai", "codex_auth"}); const auto& accounts = member(rotated, "accounts").as_array(); json::array actualIds; for (const auto& account : accounts) actualIds.push_back(member(account, "account_id")); EXPECT(rt::deepEqual(actualIds, ids)); EXPECT_EQ(str(accounts[0], "access_token"), "access-one-rotated"); EXPECT_EQ(str(accounts[1], "access_token"), "access-two");
  AWAIT(third, upload("account-three", "access-three")); EXPECT_EQ(third.status(), 409); AWAIT(thirdPayload, third.json()); EXPECT_EQ(str(member(thirdPayload, "error"), "code"), "codex_auth_pool_full"); json::array remaining; const auto retained = stored(rt::KvKey{"ubq_ai", "codex_auth"}); for (const auto& account : member(retained, "accounts").as_array()) remaining.push_back(member(account, "account_id")); EXPECT(rt::deepEqual(remaining, ids));
}
ASYNC_TEST("admin codex auth rotation replaces a prior account snapshot even at an older version") {
  Fixture fixture; const rt::GlobalFetchOverride fetch(rt::Fetch([](rt::RequestInfo input, rt::RequestInit) { const auto version = rt::URL(urlOf(input)).searchParams().get("client_version").value_or("missing"); const json::object body{{"models", json::array{json::object{{"slug", "gpt-" + version}, {"rich_field", json::object{{"preserved", true}}}}}}}; return rt::Response::json(body, {.headers = rt::Headers{{"Content-Type", "application/json"}, {"ETag", "\"" + version + "\""}}}); }));
  AWAIT(newer, handleAdminCodexAuth(makeRequest(json::object{{"auth", authPayload}, {"models", json::object{{"client_version", "0.201.0"}}}}))); EXPECT_EQ(newer.status(), 200); const auto firstGeneration = stored(rt::KvKey{"ubq_ai", "codex_catalog_auth_generation"}); EXPECT(firstGeneration.is_string()); AWAIT(older, handleAdminCodexAuth(makeRequest(json::object{{"auth", authPayload}, {"models", json::object{{"client_version", "0.200.0"}}}}))); EXPECT_EQ(older.status(), 200); AWAIT(payload, older.json()); EXPECT_EQ(rt::getBool(payload, "normalized_snapshot_updated"), std::optional(true)); EXPECT(!hasOwn(payload, "ok")); const auto generation = stored(rt::KvKey{"ubq_ai", "codex_catalog_auth_generation"}); EXPECT(generation.is_string()); EXPECT(!rt::deepEqual(generation, firstGeneration)); const auto catalog = stored(rt::KvKey{"ubq_ai", "codex_models"}); EXPECT_EQ(str(catalog, "client_version"), "0.200.0"); EXPECT_EQ(str(member(catalog, "models").as_array().front(), "slug"), "gpt-0.200.0"); const auto metadata = stored(rt::KvKey{"ubq_ai", "codex_catalog", "0.200.0"}); EXPECT(rt::deepEqual(member(metadata, "auth_generation"), generation)); EXPECT_EQ(str(metadata, "etag"), "\"0.200.0\"");
}
ASYNC_TEST("admin codex auth retries transient snapshot contention") {
  Fixture fixture; setAtomicCommitsToFail(1); const rt::GlobalFetchOverride fetch(modelsMock(json::array{json::object{{"slug", "gpt-5.5"}}}));
  const json::object modelSnapshot{{"source", "codex_cli"}, {"client_version", "0.126.0"}, {"updated_at_ms", 123}, {"models", json::array{json::object{{"slug", "gpt-5.5"}, {"display_name", "GPT-5.5"}}}}};
  AWAIT(response, handleAdminCodexAuth(makeRequest(json::object{{"auth", authPayload}, {"models", modelSnapshot}}))); EXPECT_EQ(response.status(), 200); EXPECT(kvStore.has(keyToString(rt::KvKey{"ubq_ai", "codex_auth"}))); EXPECT(kvStore.has(keyToString(rt::KvKey{"ubq_ai", "codex_models"}))); setAtomicCommitsToFail(0);
}
ASYNC_TEST("admin codex auth fails atomically after snapshot contention retries are exhausted") {
  Fixture fixture; setAtomicCommitsToFail(3); const rt::GlobalFetchOverride fetch(modelsMock(json::array{json::object{{"slug", "gpt-5.5"}}})); AWAIT(response, handleAdminCodexAuth(makeRequest(json::object{{"auth", authPayload}, {"models", json::object{{"client_version", "0.126.0"}}}}))); EXPECT_EQ(response.status(), 500); EXPECT(!kvStore.has(keyToString(rt::KvKey{"ubq_ai", "codex_auth"}))); EXPECT(!kvStore.has(keyToString(rt::KvKey{"ubq_ai", "codex_models"}))); setAtomicCommitsToFail(0);
}
ASYNC_TEST("admin KV migration import stays dry-run unless write is explicit") {
  Fixture fixture; const auto body = stringEntryLine(rt::KvKey{"default", "model"}, "gpt-5.5"); AWAIT(response, handleAdminKvMigrationImport(rt::Request("https://ai.ubq.fi/admin/kv-migration/import?profile=prod&dry_run=false&overwrite=true", {.method = "POST", .body = rt::BodyInit(body)}))); EXPECT_EQ(response.status(), 200); AWAIT(payload, response.json()); EXPECT_EQ(rt::getBool(payload, "dry_run"), std::optional(true)); EXPECT_EQ(num(payload, "imported"), 1); EXPECT(!kvStore.has(keyToString(rt::KvKey{"default", "model"})));
}
ASYNC_TEST("admin KV migration import reports partial writes with HTTP 422") {
  Fixture fixture; const auto body = stringEntryLine(rt::KvKey{"default", "model"}, "gpt-5.5") + "\n{not-json}"; AWAIT(response, handleAdminKvMigrationImport(rt::Request("https://ai.ubq.fi/admin/kv-migration/import?profile=prod&write=1&overwrite=true", {.method = "POST", .body = rt::BodyInit(body)}))); EXPECT_EQ(response.status(), 422); AWAIT(payload, response.json()); EXPECT_EQ(num(payload, "imported"), 1); EXPECT_EQ(num(payload, "errors"), 1); EXPECT(rt::deepEqual(stored(rt::KvKey{"default", "model"}), json::value("gpt-5.5")));
}
ASYNC_TEST("admin defaults accepts none when the model supports none") { auto testing = defaultTier("gpt-5.5", json::array{"none", "low", "medium", "high", "xhigh"}, "none", 200); co_await std::move(testing); }
ASYNC_TEST("admin defaults accepts a tier advertised by the Codex CLI catalog") { auto testing = defaultTier("gpt-5.6-sol", json::array{"low", "medium", "high", "xhigh", "max", "ultra"}, "ultra", 200); co_await std::move(testing); }
ASYNC_TEST("admin defaults does not reject an unlisted reasoning tier") { auto testing = defaultTier("gpt-5.5", json::array{"low", "medium", "high", "xhigh"}, "future-tier", 200); co_await std::move(testing); }
ASYNC_TEST("admin defaults rejects null reasoning effort") { auto testing = defaultTier("gpt-5.5", json::array{"none", "low", "medium", "high", "xhigh"}, nullptr, 400); co_await std::move(testing); }
ASYNC_TEST("paid fallback pricing initializes only when a key becomes enabled") {
  Fixture fixture; seedCodexSnapshot(json::object{{"source", "chatgpt_codex"}, {"updated_at_ms", rt::dateNow()}, {"models", json::array{json::object{{"slug", "gpt-5.6-sol"}, {"context_window", 272000}}, json::object{{"slug", "codex-only-model"}}}}}); const rt::env::ScopedOverride environment({{"METERED_API_KEY", "metered-test-key"}}); std::vector<std::string> urls; const rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo input, rt::RequestInit) { const auto url = urlOf(input); urls.push_back(url); return meteredMetadataResponse(url); }));
  AWAIT(createdResponse, handleAdminApiKeysCreate(request(json::object{{"name", "Overflow disabled"}, {"token", DISABLED_FALLBACK_TOKEN}, {"paid_fallback_enabled", false}, {"paid_fallback_limit_credits", 2}}))); EXPECT_EQ(createdResponse.status(), 200); EXPECT_EQ(urls.size(), std::size_t(0)); AWAIT(created, createdResponse.json()); EXPECT_EQ(rt::getBool(created, "paid_fallback_enabled"), std::optional(false)); EXPECT_EQ(num(created, "paid_fallback_limit_credits"), 2); EXPECT(member(created, "paid_fallback_pricing_checked_at_ms").is_null()); const auto id = str(created, "id");
  AWAIT(enabledResponse, handleAdminApiKeysUpdate(request(json::object{{"id", id}, {"paid_fallback_enabled", true}}, "PATCH"))); EXPECT_EQ(enabledResponse.status(), 200); auto sorted = urls; rt::sort(sorted); const std::vector<std::string> expected{"https://api.openlux.ai/api/ratio_config", "https://api.openlux.ai/api/status"}; EXPECT_EQ(sorted, expected); AWAIT(enabled, enabledResponse.json()); EXPECT_EQ(rt::getBool(enabled, "paid_fallback_enabled"), std::optional(true)); const json::array models{"gpt-5.6-sol"}; EXPECT(rt::deepEqual(member(enabled, "paid_fallback_model_ids"), models)); EXPECT(rt::isNumber(member(enabled, "paid_fallback_pricing_checked_at_ms")));
  urls.clear(); AWAIT(capResponse, handleAdminApiKeysUpdate(request(json::object{{"id", id}, {"paid_fallback_limit_credits", -1}}, "PATCH"))); EXPECT_EQ(capResponse.status(), 200); EXPECT_EQ(urls.size(), std::size_t(0)); AWAIT(unlimited, capResponse.json()); EXPECT_EQ(num(unlimited, "paid_fallback_limit_credits"), -1); const auto storedId = stored(rt::KvKey{"ubq_ai", "api_keys", "id", id}); const auto storedHash = stored(rt::KvKey{"ubq_ai", "api_keys", "hash", str(storedId, "hash")}); EXPECT_EQ(num(storedId, "paid_fallback_limit_microcredits"), -1); EXPECT_EQ(num(storedHash, "paid_fallback_limit_microcredits"), -1);
  AWAIT(disable, handleAdminApiKeysUpdate(request(json::object{{"id", id}, {"paid_fallback_enabled", false}}, "PATCH"))); EXPECT_EQ(disable.status(), 200); EXPECT_EQ(urls.size(), std::size_t(0)); AWAIT(reenable, handleAdminApiKeysUpdate(request(json::object{{"id", id}, {"paid_fallback_enabled", true}}, "PATCH"))); EXPECT_EQ(reenable.status(), 200); rt::sort(urls); EXPECT_EQ(urls, expected);
}
ASYNC_TEST("API key creation rejects custom tokens outside the v2 routable shape") {
  Fixture fixture; AWAIT(response, handleAdminApiKeysCreate(request(json::object{{"name", "Legacy custom token"}, {"token", "legacy_custom_token_that_was_previously_accepted"}}))); EXPECT_EQ(response.status(), 400); AWAIT(payload, response.json()); EXPECT(rt::Regex("u_ prefix followed by 64 lowercase hexadecimal").test(str(member(payload, "error"), "message"))); bool any = false; for (const auto& [key, value] : kvStore) { (void)value; any = any || rt::includes(key, "\"api_keys\""); } EXPECT(!any);
}
ASYNC_TEST("API key creation generates a v2 token when token is null") {
  Fixture fixture; AWAIT(response, handleAdminApiKeysCreate(request(json::object{{"name", "Generated token"}, {"token", nullptr}}))); EXPECT_EQ(response.status(), 200); AWAIT(payload, response.json()); const auto token = str(payload, "token"); EXPECT(rt::Regex("^u_[0-9a-f]{64}$").test(token)); EXPECT_EQ(str(payload, "prefix"), token.substr(0, 12));
}
ASYNC_TEST("enabled key creation initializes once and failed enable leaves the key disabled") {
  Fixture fixture; seedCodexSnapshot(json::object{{"source", "chatgpt_codex"}, {"updated_at_ms", rt::dateNow()}, {"models", json::array{json::object{{"slug", "gpt-5.6-sol"}, {"context_window", 272000}}}}}); const rt::env::ScopedOverride environment({{"METERED_API_KEY", "metered-test-key"}}); int calls = 0; const rt::GlobalFetchOverride fetch(rt::Fetch([&](rt::RequestInfo input, rt::RequestInit) { ++calls; return meteredMetadataResponse(urlOf(input)); }));
  AWAIT(enabledCreate, handleAdminApiKeysCreate(request(json::object{{"name", "Overflow enabled"}, {"token", ENABLED_FALLBACK_TOKEN}, {"paid_fallback_enabled", true}, {"paid_fallback_limit_credits", 1}}))); EXPECT_EQ(enabledCreate.status(), 200); EXPECT_EQ(calls, 2); AWAIT(enabledPayload, enabledCreate.json()); EXPECT(!hasOwn(enabledPayload, "ok")); const json::array models{"gpt-5.6-sol"}; EXPECT(rt::deepEqual(member(enabledPayload, "paid_fallback_model_ids"), models));
  calls = 0; AWAIT(duplicate, handleAdminApiKeysCreate(request(json::object{{"name", "Duplicate overflow key"}, {"token", ENABLED_FALLBACK_TOKEN}, {"paid_fallback_enabled", true}, {"paid_fallback_limit_credits", 1}}))); EXPECT_EQ(duplicate.status(), 409); EXPECT_EQ(calls, 0);
  AWAIT(disabled, handleAdminApiKeysCreate(request(json::object{{"name", "Enable failure"}, {"token", FAILED_FALLBACK_TOKEN}, {"paid_fallback_enabled", false}, {"paid_fallback_limit_credits", 1}}))); AWAIT(disabledPayload, disabled.json()); const auto id = str(disabledPayload, "id");
  const rt::GlobalFetchOverride failure(rt::Fetch([](rt::RequestInfo input, rt::RequestInit) { const auto url = urlOf(input); if (rt::endsWith(url, "/api/status")) return rt::Response(std::string("upstream unavailable"), {.status = 503}); return meteredMetadataResponse(url); })); AWAIT(failedEnable, handleAdminApiKeysUpdate(request(json::object{{"id", id}, {"paid_fallback_enabled", true}}, "PATCH"))); EXPECT_EQ(failedEnable.status(), 502); const auto record = stored(rt::KvKey{"ubq_ai", "api_keys", "id", id}); EXPECT_EQ(rt::getBool(record, "paid_fallback_enabled"), std::optional(false)); EXPECT(member(record, "paid_fallback_pricing_checked_at_ms").is_null());
}
