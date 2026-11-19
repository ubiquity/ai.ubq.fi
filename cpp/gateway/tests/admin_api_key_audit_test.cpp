// Port of tests/admin-api-key-audit.test.ts.
// Real handler/index ingress covers the original change-history authentication gate.
// Links: tests/admin_api_key_audit_test.cpp
// src/provider/gemini.cpp src/admin/gemini_quota.cpp
// src/admin/api_key_audit.cpp src/admin/api_key_mutations.cpp src/admin/api_keys.cpp src/admin/capacity_observation_sync.cpp src/admin/chart_source_proxy.cpp src/admin/claude_code.cpp src/admin/codex.cpp src/admin/codex_reset.cpp src/admin/defaults.cpp src/admin/error_log.cpp
// src/admin/kernel.cpp src/admin/sentinel_incident.cpp src/agent_messages.cpp src/analytics.cpp src/api_key_policy.cpp src/api_keys.cpp src/auth/index.cpp src/auth/local_admin.cpp src/auth/local_development_key.cpp src/auth/passkeys.cpp
// src/auth/passport.cpp src/auth/passport_sessions.cpp src/auth/relay.cpp src/authority/native_api_key.cpp src/authority/native_api_key_audit.cpp src/authority/native_api_key_policy.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp
// src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_kernel.cpp src/authority/native_kernel_contracts.cpp src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/authority/native_paid_policy.cpp src/bounded_response_body.cpp src/cache/kv_migration_base.cpp src/cache/kv_migration_paid_fallback.cpp
// src/cache/kv_migration_usage.cpp src/cache/kv_migration_validation.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/scope_experiment.cpp src/cache/scope_experiment_model.cpp src/cache/scope_experiment_targets.cpp src/cache/scope_targets.cpp src/cache/telemetry_gate.cpp
// src/catalog/index.cpp src/catalog/models.cpp src/catalog/promotion.cpp src/catalog/store.cpp src/catalog/types.cpp src/chat/completions.cpp src/chat/envelope.cpp src/chat/stream_translation.cpp src/codex/account_routing.cpp src/codex/auth.cpp
// src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_claim.cpp src/codex/banked_reset_pool.cpp src/codex/banked_reset_provider.cpp src/codex/banked_reset_submission.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp
// src/codex/index.cpp src/codex/manual_reset.cpp src/codex/manual_reset_state.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/quota.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/responses_error.cpp
// src/codex/responses_operations.cpp src/codex/responses_runners.cpp src/codex/responses_state.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/codex/supervisor.cpp src/codex/supervisor_brief.cpp src/codex/supervisor_config.cpp
// src/codex/supervisor_follow.cpp src/codex/supervisor_inventory.cpp src/codex/supervisor_log.cpp src/codex/supervisor_secret.cpp src/codex/supervisor_transport.cpp src/config.cpp src/debug_routing.cpp src/decisions/handlers.cpp src/deepseek/chat_projection.cpp src/deepseek/context_admission_failure.cpp
// src/deepseek/flash_aliases.cpp src/deepseek/forwarded_payload_policy.cpp src/deepseek/handlers.cpp src/deepseek/index.cpp src/deepseek/responses.cpp src/deepseek/responses_payload.cpp src/deepseek/responses_stream.cpp src/deepseek/stream.cpp src/deepseek/waterfall.cpp src/deepseek/waterfall_handler.cpp
// src/defaults.cpp src/embeddings/handlers.cpp src/embeddings/jobs.cpp src/embeddings/ledger.cpp src/embeddings/voyage.cpp src/handler/admission.cpp src/handler/http.cpp src/handler/index.cpp src/handler/terminal_log.cpp src/handler/terminal_route.cpp
// src/health.cpp src/http.cpp src/images.cpp src/inference_admission.cpp src/inference_deadline.cpp src/input_normalization.cpp src/kernel/attestation.cpp src/kernel/policy_queue.cpp src/kernel/quota_reservations.cpp src/kernel/quota_reserve.cpp
// src/kernel/quota_v2.cpp src/kernel/quota_write.cpp src/kernel/usage.cpp src/kv.cpp src/kv_conversion/profile_json.cpp src/lib/jev_compaction/codex_items.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/request.cpp src/lib/jev_compaction/state.cpp src/lib/jev_compaction/types.cpp
// src/live/calls.cpp src/live/handler.cpp src/live/routes.cpp src/live/sideband.cpp src/live/upstream.cpp src/metered_quota.cpp src/models/catalog.cpp src/models/codex_models.cpp src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp
// src/models/codex_models_whitelist.cpp src/models/metadata.cpp src/models/openrouter_models.cpp src/openai.cpp src/openai_schema.cpp src/openai_telemetry.cpp src/optional_telemetry_queue.cpp src/paid_fallback/claude_code_route.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp
// src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/removed_provider.cpp src/paid_fallback/rollups.cpp src/paid_fallback/routing.cpp src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp
// src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp
// src/provider/cerebras.cpp src/provider/cerebras_handlers.cpp src/provider/cerebras_rate_limits.cpp src/provider/cerebras_responses.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code.cpp src/provider/claude_code_access.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
// src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/lithos.cpp src/provider/lithos_handlers.cpp src/provider/lithos_models.cpp src/provider/lithos_rate_limits.cpp src/provider/lithos_streams.cpp src/provider/metered.cpp
// src/provider/openrouter.cpp src/provider/openrouter_handlers.cpp src/provider/openrouter_request_controls.cpp src/provider/openrouter_streams.cpp src/provider/openrouter_web_tools.cpp src/provider/presentation.cpp src/provider/removed_provider_circuit.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/stream_relay.cpp
// src/provider/surplus.cpp src/provider/surplus_claude_chat_request.cpp src/provider/surplus_claude_request.cpp src/quota_projection.cpp src/recent_model_context.cpp src/release.cpp src/request.cpp src/request_policy.cpp src/responses_attempts.cpp src/responses_buffered.cpp
// src/responses_failover_stream.cpp src/responses_handler.cpp src/responses_native_headers.cpp src/responses_request.cpp src/responses_stream.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/websocket.cpp src/runtime_config.cpp src/sentinel/incident_outbox.cpp
// src/sentinel/replay_admin.cpp src/sentinel/replay_envelope.cpp src/sentinel/replay_limits.cpp src/sentinel/replay_model.cpp src/sentinel/replay_observation.cpp src/sentinel/replay_read.cpp src/sentinel/replay_retention.cpp src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_retention_metadata.cpp src/sentinel/replay_retention_schema.cpp
// src/sentinel/replay_store.cpp src/sentinel/upstream_capture.cpp src/static.cpp src/storage/kv.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/systemone/handlers.cpp src/types.cpp src/upstream_wire.cpp
// src/utils.cpp tests/helpers/admin_auth_harness.cpp
#include "uos/rt/crypto.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/process.hpp"
#include <array>
#include "uos/handler/index.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/test_async.hpp"

namespace {
  struct T2PublicAssets {
    std::vector<std::string> createdPaths;
    T2PublicAssets() {
      const auto temporary = uos::rt::env::get("TMPDIR");
      const auto directory = uos::rt::process::cwd();
      if (!temporary || !directory.starts_with(uos::rt::path::dirname(*temporary) + "/"))
        throw uos::rt::JsError("T2 public assets require the wrapper task workdir");
      const auto root = uos::rt::path::resolve(uos::rt::path::dirname(__FILE__), "../../..");
      const std::array<std::string, 3> names{"static", "favicon.png", "favicon-32.png"};
      for (const auto& name : names) {
        const auto target = uos::rt::path::join(root, name);
        const auto assetPath = uos::rt::path::join(directory, name);
        if (uos::rt::fs::existsSync(assetPath)) {
          if (uos::rt::fs::realpathSync(assetPath) != uos::rt::fs::realpathSync(target))
            throw uos::rt::JsError("T2 public asset fixture refuses an unexpected existing path");
        } else {
          uos::rt::fs::symlinkSync(target, assetPath);
          createdPaths.push_back(assetPath);
        }
      }
    }
    ~T2PublicAssets() {
      for (const auto& assetPath : createdPaths) uos::rt::fs::rmSync(assetPath);
    }
  };
  T2PublicAssets& t2PublicAssets() {
    static T2PublicAssets assets;
    return assets;
  }
  struct T2IngressScope {
    uos::config::Config savedConfig = uos::config::config;
    uos::rt::env::ScopedOverride credentials{{"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    uos::rt::GlobalFetchOverride blocked{uos::rt::Fetch([](uos::rt::RequestInfo, uos::rt::RequestInit) -> uos::rt::Response {
      throw uos::rt::JsError("T2 ingress fixture forbids an unstubbed upstream call");
    })};
    explicit T2IngressScope(std::shared_ptr<uos::rt::Kv> database = nullptr) {
      (void)t2PublicAssets();
      uos::config::config.codexAuthJsonB64.clear();
      uos::config::config.authTokens.clear();
      uos::config::config.adminTokens.clear();
      if (!database) database = std::make_shared<uos::rt::MemoryKv>(uos::rt::dateNow);
      uos::kv::setKvForTest(std::move(database));
    }
    ~T2IngressScope() {
      uos::kv::setKvForTest(nullptr);
      uos::config::config = savedConfig;
    }
  };
}
#include "tests/helpers/admin_auth_harness.hpp"
#include "uos/admin/api_keys.hpp"
#include "uos/admin/api_key_mutations.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/runtime_config.hpp"
#include "uos/provider/metered.hpp"
#include "uos/admin/api_key_audit.hpp"
#include "uos/api_keys.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/url.hpp"

#include <memory>
#include <string>
#include <vector>
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  namespace rt = uos::rt; namespace json = uos::json; namespace admin = uos::admin::api_keys; namespace audit = uos::admin::api_key_audit; namespace harness = uos::tests::helpers::admin_auth_harness; namespace auth = uos::auth::index;
  struct Fixture { rt::env::ScopedEnvironment environment{std::initializer_list<std::pair<std::string, std::string>>()}; Fixture() { uos::config::config.codexAuthJsonB64.clear(); harness::resetHarness(); } ~Fixture() { harness::cleanupHarness(); } };
  rt::Request request(const json::value& body) { return rt::Request("https://ai.ubq.fi/admin/api-keys", rt::RequestInit{.method = "POST", .headers = rt::Headers{{"Content-Type", "application/json"}}, .body = rt::BodyInit(rt::jsonStringify(body))}); }
  audit::ApiKeyAuditContext passkeyContext() { return {.auth = auth::AdminAuthResultOk{"synthetic-secret-never-persisted", auth::AdminAuthMethodPasskeySession{"user-1", "alice", true, 1}, false}, .request_id = "req-passkey-1"}; }
  rt::Task<rt::Response> list(const std::string& query = "") { return audit::handleAdminApiKeyChanges(rt::Request("https://ai.ubq.fi/admin/api-keys/changes" + query)); }
}
ASYNC_TEST("key creation writes one safe audit event in the key atomic commit") {
  Fixture fixture; const auto context = passkeyContext(); const json::object input{{"name", "Audit key"}, {"usage_limit_requests", -1}, {"expires_at_ms", -1}}; AWAIT(created, admin::handleAdminApiKeysCreate(request(input), context)); EXPECT_EQ(created.status(), 200); AWAIT(createdBody, created.json()); const auto id = std::string(*rt::getString(createdBody, "id")); const auto token = std::string(*rt::getString(createdBody, "token"));
  AWAIT(response, list()); AWAIT(body, response.json()); EXPECT_EQ(response.status(), 200); const auto* events = rt::getArray(body, "data"); ASSERT(events); ASSERT_EQ(events->size(), std::size_t(1)); const auto& event = events->front(); EXPECT_EQ(rt::getString(event, "action"), std::optional<std::string_view>("create")); EXPECT_EQ(rt::getString(event, "target_key_id"), std::optional<std::string_view>(id)); const auto* actor = rt::get(event, "actor"); ASSERT(actor); EXPECT_EQ(rt::getString(*actor, "kind"), std::optional<std::string_view>("passkey_user")); EXPECT_EQ(rt::getString(*actor, "principal_id"), std::optional<std::string_view>("user-1")); EXPECT(!rt::includes(rt::jsonStringify(event), token)); EXPECT(!rt::includes(rt::jsonStringify(event), "synthetic-secret"));
  bool ttl = false; for (const auto& call : harness::atomicSetCalls) if (call.key.size() >= 3 && call.key[0] == audit::API_KEY_CHANGE_PREFIX[0] && call.key[1] == audit::API_KEY_CHANGE_PREFIX[1] && call.key[2] == audit::API_KEY_CHANGE_PREFIX[2]) ttl = call.expireIn == std::optional(audit::API_KEY_CHANGE_RETENTION_MS); EXPECT(ttl);
}
ASYNC_TEST("Passport identity is retained in the durable audit actor without its JWT") {
  Fixture fixture; auth::PassportAuthMethod passport{"passport-user:passport-subject", {"passport-subject", "passport-handle", true, {}, "sso", 1900000000.0, std::string("passport-jti"), std::nullopt}}; const audit::ApiKeyAuditContext context{.auth = auth::AdminAuthResultOk{"passport-jwt-must-not-persist", passport, false}, .request_id = "req-passport-1"}; AWAIT(created, admin::handleAdminApiKeysCreate(request(json::object{{"name", "Passport key"}}), context)); EXPECT_EQ(created.status(), 200); AWAIT(response, list()); AWAIT(body, response.json()); const auto* events = rt::getArray(body, "data"); ASSERT(events); ASSERT_EQ(events->size(), std::size_t(1)); const auto* actor = rt::get(events->front(), "actor"); ASSERT(actor); EXPECT_EQ(rt::getString(*actor, "principal_id"), std::optional<std::string_view>("passport-subject")); EXPECT_EQ(rt::getString(*actor, "principal_label"), std::optional<std::string_view>("passport-handle")); EXPECT(!rt::includes(rt::jsonStringify(body), "passport-jwt"));
}
ASYNC_TEST("an injected audit commit failure leaves created key state and history unchanged") {
  Fixture fixture; harness::setAtomicCommitsToFail(1); const auto context = passkeyContext(); AWAIT(failed, admin::handleAdminApiKeysCreate(request(json::object{{"name", "Atomic create"}, {"usage_limit_requests", 5}}), context)); EXPECT_EQ(failed.status(), 500); EXPECT(!harness::kvStoreHasPrefix(uos::api_keys::API_KEY_ID_PREFIX)); AWAIT(response, list()); AWAIT(body, response.json()); EXPECT(rt::getArray(body, "data")->empty());
}
ASYNC_TEST("audit reads are bounded, paginate newest-first and validate the cursor") {
  Fixture fixture; std::vector<std::string> ids;
  for (int i = 0; i < 5; ++i) { AWAIT(created, admin::handleAdminApiKeysCreate(request(json::object{{"name", "Page " + std::to_string(i)}, {"usage_limit_requests", i + 1}}))); EXPECT_EQ(created.status(), 200); AWAIT(body, created.json()); ids.emplace_back(*rt::getString(body, "id")); co_await rt::sleep(2); }
  EXPECT_EQ(audit::API_KEY_CHANGE_RETENTION_DAYS, 90); EXPECT_EQ(audit::API_KEY_CHANGE_RETENTION_MS, 90.0 * 24 * 60 * 60 * 1000);
  const audit::ApiKeyChangeEvent expired{"expired-event", rt::dateNow() - audit::API_KEY_CHANGE_RETENTION_MS - 60000, "update", {"unknown", std::nullopt, std::nullopt, false}, "expired-key", std::string("Expired key"), {}, "expired-request", "test-release"}; harness::kvStore.set(harness::keyToString(audit::apiKeyChangeEventKey(expired)), rt::KvValue(json::value_from(expired)));
  AWAIT(first, list("?limit=2")); AWAIT(firstBody, first.json()); const auto* firstEvents = rt::getArray(firstBody, "data"); ASSERT(firstEvents); ASSERT_EQ(firstEvents->size(), std::size_t(2)); EXPECT_EQ(rt::getString(firstEvents->front(), "target_key_id"), std::optional<std::string_view>(ids[4])); const auto firstCursor = std::string(*rt::getString(firstBody, "next_cursor"));
  AWAIT(second, list("?limit=2&cursor=" + rt::encodeURIComponent(firstCursor))); AWAIT(secondBody, second.json()); const auto* secondEvents = rt::getArray(secondBody, "data"); ASSERT(secondEvents); ASSERT_EQ(secondEvents->size(), std::size_t(2)); EXPECT_EQ(rt::getString(secondEvents->front(), "target_key_id"), std::optional<std::string_view>(ids[2])); const auto secondCursor = std::string(*rt::getString(secondBody, "next_cursor"));
  AWAIT(third, list("?limit=2&cursor=" + rt::encodeURIComponent(secondCursor))); AWAIT(thirdBody, third.json()); const auto* thirdEvents = rt::getArray(thirdBody, "data"); ASSERT(thirdEvents); ASSERT_EQ(thirdEvents->size(), std::size_t(1)); EXPECT_EQ(rt::getString(thirdEvents->front(), "target_key_id"), std::optional<std::string_view>(ids[0])); EXPECT(rt::get(thirdBody, "next_cursor")->is_null());
  const std::vector<std::string> invalid{"?limit=0", "?limit=abc", "?limit=-1", "?cursor=not-a-real-cursor", "?cursor=", "?cursor=" + std::string(1025, 'x')}; for (const auto& query : invalid) { AWAIT(response, list(query)); EXPECT_EQ(response.status(), 400); }
  AWAIT(bounded, list("?limit=100000")); AWAIT(boundedBody, bounded.json()); EXPECT_EQ(rt::getArray(boundedBody, "data")->size(), std::size_t(5));
}
ASYNC_TEST("the change-history reader fails closed without KV and retains timestamp-only event views") {
  Fixture fixture; AWAIT(missing, audit::handleAdminApiKeyChanges(rt::Request("https://ai.ubq.fi/admin/api-keys/changes"), nullptr)); EXPECT_EQ(missing.status(), 500);
  const double now = rt::dateNow(); const rt::KvKey key{audit::API_KEY_CHANGE_PREFIX, 0.0, "timestamp-only"}; harness::kvStore.set(harness::keyToString(key), rt::KvValue(json::object{{"at_ms", rt::makeNumber(now)}, {"opaque", "preserved"}})); AWAIT(response, list()); AWAIT(body, response.json()); const auto* rows = rt::getArray(body, "data"); ASSERT(rows); ASSERT_EQ(rows->size(), std::size_t(1)); EXPECT_EQ(rt::getString(rows->front(), "opaque"), std::optional<std::string_view>("preserved"));
}

namespace {
  namespace mutations = uos::admin::api_key_mutations;
  audit::ApiKeyAuditContext sharedContext() { return {.auth = auth::AdminAuthResultOk{"synthetic-shared-secret", auth::AdminAuthMethodAdminAllowlist{}, true}, .request_id = "req-shared-1"}; }
  rt::KvValue storedRecord(const std::string& id) { const auto* stored = harness::kvStore.get(harness::keyToString(uos::api_keys::apiKeyIdKey(id))); return stored ? *stored : rt::KvValue(nullptr); }
  rt::Task<std::string> createKey(json::object body) {
    const auto context = passkeyContext(); auto task = admin::handleAdminApiKeysCreate(request(body), context); const auto output = co_await std::move(task);
    EXPECT_EQ(output.status(), 200); auto parsing = output.json(); const auto payload = co_await std::move(parsing); const auto id = rt::getString(payload, "id"); ASSERT(id); co_return std::string(*id);
  }
  rt::Task<json::array> events() { auto pending = list(); const auto output = co_await std::move(pending); EXPECT_EQ(output.status(), 200); auto parsing = output.json(); const auto body = co_await std::move(parsing); co_return *rt::getArray(body, "data"); }
  std::size_t actionCount(const json::array& rows, const std::string& action) { std::size_t count = 0; for (const auto& row : rows) if (rt::getString(row, "action") == action) ++count; return count; }
  void seedAuditPricing() {
    const json::object snapshot{{"source", "chatgpt_codex"}, {"updated_at_ms", rt::makeNumber(rt::dateNow())}, {"models", json::array{json::object{{"slug", "gpt-5.6-sol"}, {"context_window", 272000}}}}};
    const auto runtime = uos::runtime_config::buildRuntimeConfig(snapshot); const rt::KvKey snapshotKey{"ubq_ai", "codex_models"}; const rt::KvKey runtimeKey{"uos_ai", "runtime_config", "v2"}; harness::kvStore.set(harness::keyToString(snapshotKey), rt::KvValue(snapshot)); harness::kvStore.set(harness::keyToString(runtimeKey), rt::KvValue(json::value_from(runtime))); uos::runtime_config::cacheRuntimeConfig(runtime);
  }
  rt::Fetch pricingFetch() { return rt::Fetch([](rt::RequestInfo input, rt::RequestInit) -> rt::Response {
    const auto url = harness::urlOf(input);
    const std::string body = url == "https://api.openlux.ai/api/ratio_config" ? R"({"success":true,"data":{"model_ratio":{"gpt-5.6-sol":1,"not-in-codex-catalog":1},"model_price":{}}})" : url == "https://api.openlux.ai/api/status" ? R"({"success":true,"data":{"setup":true,"quota_per_unit":500000}})" : "";
    if (body.empty()) throw rt::JsError("Unexpected Metered metadata URL: " + url);
    return rt::Response(rt::BodyInit(body), rt::ResponseInit{.headers = rt::Headers{{"Content-Type", "application/json"}}});
  }); }
}
ASYNC_TEST("key lifecycle writes ordered audit events with safe fields and actor attribution") {
  Fixture fixture; AWAIT(id, createKey(json::object{{"name", "Audit key"}, {"usage_limit_requests", -1}, {"expires_at_ms", -1}})); const auto originalHash = storedRecord(id)["hash"].asString(); const auto shared = sharedContext();
  co_await rt::sleep(2); AWAIT(updated, mutations::handleAdminApiKeysUpdate(request(json::object{{"id", id}, {"name", "Audit key renamed"}, {"usage_limit_requests", 3}}), shared)); EXPECT_EQ(updated.status(), 200);
  co_await rt::sleep(2); AWAIT(revoked, mutations::handleAdminApiKeysRevoke(request(json::object{{"id", id}}), shared)); EXPECT_EQ(revoked.status(), 200);
  co_await rt::sleep(2); const auto passkey = passkeyContext(); AWAIT(unrevoked, mutations::handleAdminApiKeysUnrevoke(request(json::object{{"id", id}}), passkey)); EXPECT_EQ(unrevoked.status(), 200);
  co_await rt::sleep(2); AWAIT(revokedAgain, mutations::handleAdminApiKeysRevoke(request(json::object{{"id", id}}), shared)); EXPECT_EQ(revokedAgain.status(), 200);
  co_await rt::sleep(2); const audit::ApiKeyAuditContext bob{.auth = auth::AdminAuthResultOk{"synthetic-bob-secret", auth::AdminAuthMethodPasskeySession{"user-2", "bob", true, 1}, false}, .request_id = "req-passkey-1"}; AWAIT(deleted, mutations::handleAdminApiKeysDelete(request(json::object{{"id", id}}), bob)); EXPECT_EQ(deleted.status(), 200); EXPECT(storedRecord(id).isNull());
  AWAIT(rows, events()); ASSERT_EQ(rows.size(), std::size_t(6)); const std::vector<std::string> actions{"delete", "revoke", "unrevoke", "revoke", "update", "create"};
  for (std::size_t i = 0; i < rows.size(); ++i) { EXPECT_EQ(rt::getString(rows[i], "action"), std::optional<std::string_view>(actions[i])); if (i) EXPECT(*rt::getNumber(rows[i - 1], "at_ms") >= *rt::getNumber(rows[i], "at_ms")); }
  const auto& created = rows.back(); const auto* actor = rt::get(created, "actor"); EXPECT_EQ(rt::getString(*actor, "kind"), std::optional<std::string_view>("passkey_user")); EXPECT_EQ(rt::getString(*actor, "principal_id"), std::optional<std::string_view>("user-1")); EXPECT_EQ(rt::getString(*actor, "principal_label"), std::optional<std::string_view>("alice")); EXPECT_EQ(rt::getString(created, "target_key_name"), std::optional<std::string_view>("Audit key")); EXPECT_EQ(rt::getNumber(*rt::get(created, "changed"), "usage_limit_requests"), std::optional(-1.0)); EXPECT_EQ(rt::getString(created, "request_id"), std::optional<std::string_view>("req-passkey-1")); EXPECT(rt::getString(created, "release")); EXPECT(rt::getString(created, "id"));
  const auto& update = rows[4]; EXPECT_EQ(rt::getString(*rt::get(update, "actor"), "kind"), std::optional<std::string_view>("shared_admin_credential")); EXPECT(rt::get(*rt::get(update, "actor"), "principal_id")->is_null()); EXPECT_EQ(rt::getString(*rt::get(update, "changed"), "name"), std::optional<std::string_view>("Audit key renamed")); EXPECT_EQ(rt::getNumber(*rt::get(update, "changed"), "usage_limit_requests"), std::optional(3.0)); EXPECT_EQ(rt::getString(*rt::get(rows[0], "actor"), "principal_id"), std::optional<std::string_view>("user-2"));
  const auto serialized = rt::jsonStringify(rows); EXPECT(!rt::includes(serialized, originalHash)); static const rt::Regex unsafe("authorization|bearer|token_hash|prompt|\"hash\"|\"error\"", "i"); EXPECT(!unsafe.test(serialized));
}
ASYNC_TEST("finite and unlimited wire values round-trip through create, update and persisted readback") {
  Fixture fixture; uos::runtime_config::resetRuntimeConfigCacheForTest(); uos::provider::metered::resetMeteredModelsCacheForTest();
  AWAIT(finite, createKey(json::object{{"name", "Finite"}, {"usage_limit_requests", 7}, {"window_ms", 60000}, {"expires_at_ms", -1}})); AWAIT(unlimited, createKey(json::object{{"name", "Unlimited"}, {"usage_limit_requests", -1}, {"window_ms", 60000}, {"expires_at_ms", -1}})); EXPECT_EQ(storedRecord(finite)["usage_limit_requests"], rt::KvValue(7)); EXPECT_EQ(storedRecord(unlimited)["usage_limit_requests"], rt::KvValue(-1)); const auto shared = sharedContext();
  AWAIT(toUnlimited, mutations::handleAdminApiKeysUpdate(request(json::object{{"id", finite}, {"usage_limit_requests", -1}}), shared)); EXPECT_EQ(toUnlimited.status(), 200); AWAIT(unlimitedBody, toUnlimited.json()); EXPECT_EQ(rt::getNumber(unlimitedBody, "usage_limit_requests"), std::optional(-1.0)); EXPECT_EQ(storedRecord(finite)["usage_limit_requests"], rt::KvValue(-1));
  AWAIT(toFinite, mutations::handleAdminApiKeysUpdate(request(json::object{{"id", unlimited}, {"usage_limit_requests", 12}}), shared)); EXPECT_EQ(toFinite.status(), 200); EXPECT_EQ(storedRecord(unlimited)["usage_limit_requests"], rt::KvValue(12));
  const std::vector<json::object> malformed{json::object{{"usage_limit_requests", "5"}}, json::object{{"usage_limit_requests", -2}}, json::object{{"window_ms", 0}}, json::object{{"paid_fallback_limit_credits", -5}}, json::object{{"paid_fallback_enabled", true}, {"paid_fallback_limit_credits", 0}}};
  for (auto input : malformed) { input["id"] = finite; AWAIT(rejected, mutations::handleAdminApiKeysUpdate(request(input), shared)); EXPECT_EQ(rejected.status(), 400); EXPECT_EQ(storedRecord(finite)["usage_limit_requests"], rt::KvValue(-1)); }
  rt::env::set("METERED_API_KEY", "synthetic-metered-key"); rt::GlobalFetchOverride fetch(pricingFetch()); seedAuditPricing();
  AWAIT(paidId, createKey(json::object{{"name", "Paid unlimited"}, {"usage_limit_requests", 3}, {"window_ms", 60000}, {"expires_at_ms", -1}, {"paid_fallback_enabled", true}, {"paid_fallback_limit_credits", -1}})); EXPECT_EQ(storedRecord(paidId)["paid_fallback_enabled"], rt::KvValue(true)); EXPECT_EQ(storedRecord(paidId)["paid_fallback_limit_microcredits"], rt::KvValue(-1));
  AWAIT(disabledId, createKey(json::object{{"name", "Paid disabled"}, {"usage_limit_requests", 3}, {"expires_at_ms", -1}})); EXPECT_EQ(storedRecord(disabledId)["paid_fallback_enabled"], rt::KvValue(false)); EXPECT(storedRecord(disabledId)["paid_fallback_limit_microcredits"] != rt::KvValue(-1));
  AWAIT(enabled, mutations::handleAdminApiKeysUpdate(request(json::object{{"id", disabledId}, {"paid_fallback_enabled", true}, {"paid_fallback_limit_credits", 2}}), shared)); EXPECT_EQ(enabled.status(), 200); EXPECT_EQ(storedRecord(disabledId)["paid_fallback_model_ids"], rt::KvValue(json::array{"gpt-5.6-sol"})); EXPECT_EQ(storedRecord(disabledId)["paid_fallback_quota_per_credit"], rt::KvValue(500000));
  AWAIT(rows, events()); bool found = false; for (const auto& event : rows) if (rt::getString(event, "target_key_id") == finite && rt::getString(event, "action") == "update") { EXPECT_EQ(rt::getNumber(*rt::get(event, "changed"), "usage_limit_requests"), std::optional(-1.0)); found = true; } EXPECT(found);
}
ASYNC_TEST("an injected audit commit failure leaves key state and history unchanged") {
  Fixture fixture; harness::setAtomicCommitsToFail(1); const auto passkey = passkeyContext(); AWAIT(failedCreate, admin::handleAdminApiKeysCreate(request(json::object{{"name", "Atomic create"}, {"usage_limit_requests", 5}}), passkey)); EXPECT_EQ(failedCreate.status(), 500); EXPECT(!harness::kvStoreHasPrefix(uos::api_keys::API_KEY_ID_PREFIX)); AWAIT(empty, events()); EXPECT(empty.empty());
  AWAIT(id, createKey(json::object{{"name", "Atomic update"}, {"usage_limit_requests", 5}})); const auto before = storedRecord(id); harness::setAtomicCommitsToFail(1); const auto shared = sharedContext(); AWAIT(failed, mutations::handleAdminApiKeysUpdate(request(json::object{{"id", id}, {"name", "Should not persist"}}), shared)); EXPECT_EQ(failed.status(), 409); EXPECT_EQ(storedRecord(id), before); AWAIT(rows, events()); EXPECT_EQ(actionCount(rows, "update"), std::size_t(0));
}
ASYNC_TEST("no-op patch and already-active unrevoke keep success without fabricating an event") {
  Fixture fixture; AWAIT(id, createKey(json::object{{"name", "No-op key"}, {"usage_limit_requests", 5}})); AWAIT(before, events());
  AWAIT(noop, mutations::handleAdminApiKeysUpdate(request(json::object{{"id", id}}))); EXPECT_EQ(noop.status(), 200); AWAIT(noopBody, noop.json()); EXPECT_EQ(rt::getString(noopBody, "id"), std::optional<std::string_view>(id));
  AWAIT(active, mutations::handleAdminApiKeysUnrevoke(request(json::object{{"id", id}}))); EXPECT_EQ(active.status(), 200); AWAIT(activeBody, active.json()); EXPECT(rt::deepEqual(activeBody, json::object{{"id", id}, {"revoked_at_ms", nullptr}})); AWAIT(after, events()); EXPECT_EQ(after.size(), before.size()); EXPECT(storedRecord(id)["revoked_at_ms"].isNull());
}
ASYNC_TEST("revoke, unrevoke and final delete audit commits fail closed without key or history changes") {
  Fixture fixture; AWAIT(id, createKey(json::object{{"name", "Atomic lifecycle"}, {"usage_limit_requests", 5}, {"expires_at_ms", -1}})); const auto shared = sharedContext();
  harness::setAtomicCommitsToFail(1); AWAIT(failedRevoke, mutations::handleAdminApiKeysRevoke(request(json::object{{"id", id}}), shared)); EXPECT_EQ(failedRevoke.status(), 409); EXPECT(storedRecord(id)["revoked_at_ms"].isNull()); AWAIT(afterFailedRevoke, events()); EXPECT_EQ(actionCount(afterFailedRevoke, "revoke"), std::size_t(0));
  AWAIT(revoked, mutations::handleAdminApiKeysRevoke(request(json::object{{"id", id}}), shared)); EXPECT_EQ(revoked.status(), 200); AWAIT(afterRevoke, events()); EXPECT_EQ(actionCount(afterRevoke, "revoke"), std::size_t(1));
  harness::setAtomicCommitsToFail(1); AWAIT(failedUnrevoke, mutations::handleAdminApiKeysUnrevoke(request(json::object{{"id", id}}), shared)); EXPECT_EQ(failedUnrevoke.status(), 409); EXPECT(storedRecord(id)["revoked_at_ms"].isNumber()); AWAIT(afterFailedUnrevoke, events()); EXPECT_EQ(actionCount(afterFailedUnrevoke, "unrevoke"), std::size_t(0));
  AWAIT(unrevoked, mutations::handleAdminApiKeysUnrevoke(request(json::object{{"id", id}}), shared)); EXPECT_EQ(unrevoked.status(), 200); AWAIT(revokedAgain, mutations::handleAdminApiKeysRevoke(request(json::object{{"id", id}}), shared)); EXPECT_EQ(revokedAgain.status(), 200);
  harness::setAtomicCommitsBeforeFailure(1); AWAIT(failedDelete, mutations::handleAdminApiKeysDelete(request(json::object{{"id", id}}), shared)); EXPECT_EQ(failedDelete.status(), 409); EXPECT(storedRecord(id).truthy()); AWAIT(rows, events()); EXPECT_EQ(actionCount(rows, "delete"), std::size_t(0)); EXPECT_EQ(actionCount(rows, "revoke"), std::size_t(2));
}
ASYNC_TEST("blocked deletion and live reservations keep state and record no event") {
  Fixture fixture; AWAIT(deleteId, createKey(json::object{{"name", "Blocked delete"}, {"usage_limit_requests", 5}, {"window_ms", 60000}})); AWAIT(revoked, mutations::handleAdminApiKeysRevoke(request(json::object{{"id", deleteId}}))); EXPECT_EQ(revoked.status(), 200);
  const rt::KvKey pending{"uos_ai", "paid_fallback", "v3", "pending", deleteId, "pending-marker"}; harness::kvStore.set(harness::keyToString(pending), rt::KvValue(json::object{{"created_at_ms", rt::makeNumber(rt::dateNow() - 1000)}})); AWAIT(blocked, mutations::handleAdminApiKeysDelete(request(json::object{{"id", deleteId}}))); EXPECT_EQ(blocked.status(), 409); AWAIT(blockedBody, blocked.json()); EXPECT_EQ(rt::getString(*rt::get(blockedBody, "error"), "code"), std::optional<std::string_view>("paid_fallback_billing_outstanding")); EXPECT(storedRecord(deleteId).truthy()); AWAIT(afterDelete, events()); EXPECT_EQ(actionCount(afterDelete, "delete"), std::size_t(0));
  AWAIT(reservedId, createKey(json::object{{"name", "Reserved"}, {"usage_limit_requests", 5}, {"window_ms", 60000}})); const auto record = storedRecord(reservedId); const auto start = record["usage_reset_at_ms"].asNumber() - record["window_ms"].asNumber(); const rt::KvKey lease{"uos_ai", "api_key_usage", "v3", "request", reservedId, "v3:60000", start, "live-lease"}; const auto now = rt::dateNow();
  harness::kvStore.set(harness::keyToString(lease), rt::KvValue(json::object{{"v", 3}, {"key_id", reservedId}, {"request_id", "live-lease"}, {"route", "responses"}, {"state", "reserved"}, {"reserved_at_ms", rt::makeNumber(now - 1000)}, {"lease_expires_at_ms", rt::makeNumber(now + 300000)}, {"provider", nullptr}, {"dispatched_at_ms", nullptr}, {"released_at_ms", nullptr}, {"release_reason", nullptr}}));
  const auto shared = sharedContext(); AWAIT(conflict, mutations::handleAdminApiKeysUpdate(request(json::object{{"id", reservedId}, {"window_ms", 30000}}), shared)); EXPECT_EQ(conflict.status(), 409); AWAIT(conflictBody, conflict.json()); EXPECT(rt::includes(std::string(*rt::getString(*rt::get(conflictBody, "error"), "message")), "reserved")); EXPECT_EQ(storedRecord(reservedId)["window_ms"], rt::KvValue(60000)); AWAIT(rows, events()); EXPECT_EQ(actionCount(rows, "update"), std::size_t(0));
}

ASYNC_TEST("the change-history route is authenticated and fails closed without KV") {
  harness::resetHarness();
  const auto cleanup = rt::scopeExit([] { harness::cleanupHarness(); });
  T2IngressScope ingress(harness::kvStub());
  harness::kvStore.clear();
  const rt::Request anonymousRequest("https://ai.ubq.fi/admin/api-keys/changes");
  AWAIT(anonymous, uos::handler::index::handler(anonymousRequest));
  EXPECT_EQ(anonymous.status(), 401);
  if (auto body = anonymous.body()) co_await body->cancel();
  const rt::Request unavailableRequest("https://ai.ubq.fi/admin/api-keys/changes");
  const std::shared_ptr<rt::Kv> unavailableKv;
  AWAIT(unavailable, audit::handleAdminApiKeyChanges(unavailableRequest, unavailableKv));
  EXPECT_EQ(unavailable.status(), 500);
  AWAIT(payload, unavailable.json());
  EXPECT_EQ(rt::getString(payload.at("error"), "message"), "Deno KV is not available; cannot load key change history");
}

// Additional ingress evidence for the router-specific audit context, separate from the nine source originals.
ASYNC_TEST("the real router retains authenticated audit actor and request identity") {
  harness::resetHarness();
  const auto cleanup = rt::scopeExit([] { harness::cleanupHarness(); });
  T2IngressScope ingress(harness::kvStub());
  const std::string token = "audit-route-admin-token";
  uos::config::config.adminTokens.insert(token);
  std::uint8_t randomCall = 0;
  rt::crypto::ScopedRandomSource random([&randomCall](std::span<std::uint8_t> bytes) {
    ++randomCall;
    std::fill(bytes.begin(), bytes.end(), randomCall);
  });
  rt::RequestInit init;
  init.method = "POST";
  init.headers = rt::Headers{{"Authorization", "Bearer " + token}, {"Content-Type", "application/json"}};
  const json::object input{{"name", "Router audit key"}, {"usage_limit_requests", 1}, {"expires_at_ms", -1}};
  init.body = rt::jsonStringify(input);
  const rt::Request createRequest("https://ai.ubq.fi/admin/api-keys", init);
  AWAIT(created, uos::handler::index::handler(createRequest)); EXPECT_EQ(created.status(), 200);
  AWAIT(createdBody, created.json()); const auto id = std::string(*rt::getString(createdBody, "id"));
  rt::RequestInit readInit; readInit.headers = rt::Headers{{"Authorization", "Bearer " + token}};
  const rt::Request historyRequest("https://ai.ubq.fi/admin/api-keys/changes", readInit);
  AWAIT(history, uos::handler::index::handler(historyRequest)); EXPECT_EQ(history.status(), 200);
  AWAIT(historyBody, history.json()); const auto* events = rt::getArray(historyBody, "data"); ASSERT(events); ASSERT_EQ(events->size(), 1u);
  const auto& event = events->front();
  EXPECT_EQ(rt::getString(event, "target_key_id"), id);
  EXPECT_EQ(rt::getString(event, "request_id"), "01010101-0101-4101-8101-010101010101");
  EXPECT(rt::getString(event, "request_id") != rt::getString(event, "id"));
  EXPECT_EQ(rt::getString(event.at("actor"), "kind"), "shared_admin_credential");
  EXPECT_EQ(rt::getBool(event.at("actor"), "is_super_admin"), true);
  EXPECT(event.at("actor").at("principal_id").is_null());
  EXPECT(!rt::includes(rt::jsonStringify(event), token));
}
