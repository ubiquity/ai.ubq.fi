// Links: tests/claude_subscription_admin_test.cpp
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
// src/utils.cpp
// Real handler/index ingress covers the documented-path authentication matrix.
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
#include "uos/config.hpp"
#include "uos/provider/claude_code_access.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

ASYNC_TEST("explicit persisted principal controls are independent of credential import and deny by default: persistence") {
  namespace rt = uos::rt; namespace access = uos::provider::claude_code_access;
  const rt::env::ScopedEnvironment isolation{{{"HOME", "/nonexistent/w05c2"}}}; uos::config::config.codexAuthJsonB64.clear(); const auto kv = std::make_shared<rt::MemoryKv>(rt::dateNow);
  uos::provider::claude_code::ClaudeCodeEnvLookup empty = [](const std::string&) { return std::optional<std::string>(); };
  auto initial = access::loadClaudeCodeAccessPolicy(kv, empty); const auto absent = co_await std::move(initial); EXPECT(absent.allowed_key_ids.empty());
  const std::vector<std::string> allowed{"fixture-principal"}; auto allow = access::storeClaudeCodeAccessPolicy(*kv, allowed); const auto policy = co_await std::move(allow); EXPECT_EQ(policy.allowed_key_ids, allowed);
  uos::provider::claude_code_credentials::StoreClaudeCodeStoredCredentialInput credential;
  credential.secret = "fixture-credential";
  auto importing = uos::provider::claude_code_credentials::storeClaudeCodeStoredCredential(kv, credential); const auto stored = co_await std::move(importing); EXPECT(stored);
  auto loading = access::loadClaudeCodeAccessPolicy(kv, empty); const auto unchanged = co_await std::move(loading); EXPECT_EQ(unchanged.allowed_key_ids, allowed);
  const std::vector<std::string> nobody; auto denying = access::storeClaudeCodeAccessPolicy(*kv, nobody); const auto denied = co_await std::move(denying); EXPECT(denied.allowed_key_ids.empty()); EXPECT_EQ(denied.source, "kv");
  const std::vector<std::string> malformed{"a,b"}; auto invalid = access::storeClaudeCodeAccessPolicy(*kv, malformed); EXPECT_REJECTS_MESSAGE(std::move(invalid), rt::JsError, "Claude principal IDs are invalid");
  auto readback = access::loadClaudeCodeAccessPolicy(kv, empty); const auto result = co_await std::move(readback); EXPECT(result.allowed_key_ids.empty());
  uos::provider::claude_code_credentials::resetClaudeCodeCredentialCacheForTest();
}

#include "uos/admin/claude_code.hpp"
#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_models.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/url.hpp"

#include <functional>
#include <string_view>
#include <type_traits>
#include <utility>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace admin = uos::admin::claude_code;
  namespace accounts = uos::provider::claude_code_accounts;
  namespace credentials = uos::provider::claude_code_credentials;
  namespace models = uos::provider::claude_code_models;
  namespace quota = uos::provider::claude_code_quota;
  namespace access = uos::provider::claude_code_access;

  struct AdminScope {
    rt::env::ScopedEnvironment environment{std::vector<std::pair<std::string, std::string>>{{"HOME", "/nonexistent/w07c6"}}};
    std::shared_ptr<rt::MemoryKv> store = std::make_shared<rt::MemoryKv>(rt::dateNow);
    rt::GlobalFetchOverride blocked{rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("w07c6 fixture forbids unstubbed upstream calls"); })};
    AdminScope() {
      uos::config::config.codexAuthJsonB64.clear();
      uos::kv::setKvForTest(store);
      uos::codex::auth::resetCodexAuthCacheForTest();
      credentials::resetClaudeCodeCredentialCacheForTest();
      models::resetClaudeCodeModelsCacheForTest();
      uos::provider::selection::resetProviderSelectionCacheForTest();
      uos::provider::health::resetProviderHealthThrottleForTest();
    }
    ~AdminScope() {
      uos::kv::setKvForTest(nullptr);
      uos::codex::auth::resetCodexAuthCacheForTest();
      credentials::resetClaudeCodeCredentialCacheForTest();
      models::resetClaudeCodeModelsCacheForTest();
      uos::provider::selection::resetProviderSelectionCacheForTest();
      uos::provider::health::resetProviderHealthThrottleForTest();
    }
  };
  struct StubCall { std::string url; std::string method; rt::Headers headers; };
  std::string requestUrl(const rt::RequestInfo& input) {
    return std::visit([](const auto& value) -> std::string {
      using T = std::decay_t<decltype(value)>;
      if constexpr (std::is_same_v<T, std::string>) return value;
      else if constexpr (std::is_same_v<T, rt::URL>) return value.href();
      else return value.url();
    }, input);
  }
  rt::Response modelsOk(const StubCall&) {
    return rt::Response::json(json::object{{"data", json::array()}, {"has_more", false}});
  }
  struct StubbedAnthropic {
    std::vector<StubCall> calls;
    std::function<rt::Response(const StubCall&)> responder;
    rt::GlobalFetchOverride fetch;
    explicit StubbedAnthropic(std::function<rt::Response(const StubCall&)> reply = modelsOk)
      : responder(std::move(reply)), fetch(rt::Fetch([this](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
        calls.push_back({requestUrl(input), init.method.value_or("GET"), init.headers.value_or(rt::Headers())});
        return responder(calls.back());
      })) {}
  };
  rt::Request jsonRequest(json::value body, std::string method = "POST") {
    rt::RequestInit init;
    init.method = std::move(method);
    init.headers = rt::Headers{{"content-type", "application/json"}};
    init.body = rt::jsonStringify(body);
    return rt::Request("https://ai.ubq.fi/admin/providers/claude-code/auth", init);
  }
  std::string fixtureSecret(std::string suffix) { return std::string("sk-") + "ant-" + suffix; }
  const json::value& field(const json::value& value, std::initializer_list<std::string_view> path) {
    const json::value* current = &value;
    for (const auto key : path) {
      current = rt::get(*current, key);
      if (!current) throw rt::JsError("missing fixture field " + std::string(key));
    }
    return *current;
  }
  rt::Task<void> recordError(std::shared_ptr<rt::Kv> store, accounts::ClaudeCodeAccountBinding account, std::string kind, double status, std::string message) {
    quota::QuotaOutcome input;
    input.binding = std::move(account);
    input.observation = std::nullopt;
    input.config_error = quota::QuotaOutcome::ConfigError{std::move(kind), status, std::move(message), std::nullopt};
    auto recording = quota::recordClaudeCodeQuotaOutcome(store, input);
    co_await std::move(recording);
  }
}
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)

ASYNC_TEST("the admin status route reports the tier without any secret material") {
  AdminScope scope;
  {
    StubbedAnthropic upstream;
    const auto secret = fixtureSecret("usr-1A6ABCDEFGHIJKLMNOPBwAA");
    const auto req = jsonRequest(json::object{{"credential", secret}, {"label", "primary"}});
    AWAIT(stored, admin::handleAdminClaudeCodeAuthSet(req));
    EXPECT_EQ(stored.status(), 200);
    AWAIT(body, stored.json());
    EXPECT_EQ(field(body, {"data", "configured"}), json::value(true));
    EXPECT_EQ(field(body, {"data", "credential", "kind"}), json::value("api_key"));
    EXPECT_EQ(field(body, {"data", "credential", "source"}), json::value("kv"));
    EXPECT_EQ(field(body, {"data", "credential", "hint"}), json::value(fixtureSecret("usr-1A6...BwAA")));
    EXPECT_EQ(field(body, {"data", "credential", "label"}), json::value("primary"));
    EXPECT(!rt::includes(rt::jsonStringify(body), "ABCDEFGHIJ"));
    EXPECT_EQ(field(body, {"hint"}), json::value(fixtureSecret("usr-1A6...BwAA")));
    AWAIT(roster, scope.store->get(accounts::CLAUDE_CODE_ACCOUNTS_KEY));
    EXPECT(roster.versionstamp.has_value());
  }
  AWAIT(status, admin::handleAdminClaudeCodeAuthGet());
  AWAIT(payload, status.json());
  EXPECT_EQ(field(payload, {"data", "credential", "hint"}), json::value(fixtureSecret("usr-1A6...BwAA")));
  EXPECT(!rt::includes(rt::jsonStringify(payload), "ABCDEFGHIJ"));
  EXPECT_EQ(status.headers().get("cache-control"), std::optional<std::string>("no-store"));
}

ASYNC_TEST("a rejected credential is refused upstream and never stored") {
  AdminScope scope;
  StubbedAnthropic upstream([](const StubCall&) {
    const json::object error{{"type", "error"}, {"error", json::object{{"type", "authentication_error"}, {"message", "invalid x-api-key"}}}};
    return rt::Response::json(error, rt::ResponseInit{.status = 401});
  });
  const auto secret = fixtureSecret("usr-rejected-key-abcdefghijklmnop");
  const auto req = jsonRequest(json::object{{"credential", secret}});
  AWAIT(response, admin::handleAdminClaudeCodeAuthSet(req));
  EXPECT_EQ(response.status(), 400);
  ASSERT_EQ(upstream.calls.size(), 1u);
  EXPECT_EQ(upstream.calls[0].url, "https://api.anthropic.com/v1/models?limit=100");
  EXPECT_EQ(upstream.calls[0].headers.get("x-api-key"), std::optional<std::string>(secret));
  AWAIT(stored, scope.store->get(credentials::CLAUDE_CODE_AUTH_KV_KEY));
  EXPECT(!stored.versionstamp);
}

ASYNC_TEST("credential submissions are shape-checked before any upstream call") {
  AdminScope scope;
  StubbedAnthropic upstream;
  const auto blankRequest = jsonRequest(json::object{{"credential", "  "}});
  AWAIT(blank, admin::handleAdminClaudeCodeAuthSet(blankRequest)); EXPECT_EQ(blank.status(), 400);
  const auto prefixRequest = jsonRequest(json::object{{"credential", "openai-sk-not-anthropic"}});
  AWAIT(wrongPrefix, admin::handleAdminClaudeCodeAuthSet(prefixRequest)); EXPECT_EQ(wrongPrefix.status(), 400);
  const auto labelRequest = jsonRequest(json::object{{"credential", fixtureSecret("usr-abcdefghijklmnop")}, {"label", std::string(65, 'x')}});
  AWAIT(longLabel, admin::handleAdminClaudeCodeAuthSet(labelRequest)); EXPECT_EQ(longLabel.status(), 400);
  const auto workspaceRequest = jsonRequest(json::object{{"credential", fixtureSecret("usr-abcdefghijklmnop")}, {"workspace_id", "not-a-workspace"}});
  AWAIT(badWorkspace, admin::handleAdminClaudeCodeAuthSet(workspaceRequest)); EXPECT_EQ(badWorkspace.status(), 400);
  EXPECT(upstream.calls.empty());
}

ASYNC_TEST("a workspace pin is stored, validated, and reported with its source") {
  AdminScope scope;
  StubbedAnthropic upstream;
  const auto req = jsonRequest(json::object{{"credential", fixtureSecret("usr-1A6ABCDEFGHIJKLMNOPBwAA")}, {"workspace_id", "wrkspc_01ErorW3iFLRvQy55cpNWtEt"}});
  AWAIT(response, admin::handleAdminClaudeCodeAuthSet(req)); EXPECT_EQ(response.status(), 200);
  AWAIT(body, response.json());
  EXPECT_EQ(field(body, {"data", "workspace", "id"}), json::value("wrkspc_01ErorW3iFLRvQy55cpNWtEt"));
  EXPECT_EQ(field(body, {"data", "workspace", "source"}), json::value("kv"));
  EXPECT(rt::isNumber(field(body, {"data", "workspace", "verified_at_ms"})));
  ASSERT(!upstream.calls.empty());
  EXPECT_EQ(upstream.calls[0].headers.get("anthropic-workspace-id"), std::optional<std::string>("wrkspc_01ErorW3iFLRvQy55cpNWtEt"));
  AWAIT(resolved, credentials::resolveClaudeCodeWorkspace(scope.store)); ASSERT(resolved); EXPECT_EQ(resolved->source, "kv");
  AWAIT(storedBefore, credentials::loadClaudeCodeStoredCredential(scope.store));
  const auto repin = jsonRequest(json::object{{"workspace_id", "wrkspc_01FBEp5VyCHs7ghEnJqk6giF"}});
  AWAIT(repinned, admin::handleAdminClaudeCodeAuthSet(repin)); EXPECT_EQ(repinned.status(), 200);
  AWAIT(repinnedBody, repinned.json()); EXPECT_EQ(field(repinnedBody, {"data", "workspace", "id"}), json::value("wrkspc_01FBEp5VyCHs7ghEnJqk6giF"));
  AWAIT(storedAfter, credentials::loadClaudeCodeStoredCredential(scope.store));
  ASSERT(storedBefore && storedAfter); EXPECT_EQ(storedAfter->secret, storedBefore->secret);
}

ASYNC_TEST("clearing account guards preserves observations and account removal remains a tombstone") {
  AdminScope scope;
  { StubbedAnthropic upstream; const auto req = jsonRequest(json::object{{"credential", fixtureSecret("usr-1A6ABCDEFGHIJKLMNOPBwAA")}}); AWAIT(stored, admin::handleAdminClaudeCodeAuthSet(req)); EXPECT_EQ(stored.status(), 200); }
  AWAIT(account, accounts::resolveClaudeCodeAccount(scope.store)); ASSERT(account);
  auto recording = recordError(scope.store, *account, "billing_blocked", 400, "credit balance too low"); co_await std::move(recording);
  AWAIT(cooling, quota::loadClaudeCodeQuotaState(scope.store, account)); ASSERT(cooling && cooling->last_error); EXPECT_EQ(cooling->last_error->kind, "billing_blocked");
  AWAIT(clearedGuards, admin::handleAdminClaudeCodeQuotaClear());
  AWAIT(payload, clearedGuards.json());
  EXPECT_EQ(field(payload, {"guards_cleared"}), json::value(true));
  EXPECT_EQ(field(payload, {"data", "quota", "cooldown_until_ms"}), json::value(nullptr));
  EXPECT_EQ(field(payload, {"data", "quota", "last_error"}), json::value(nullptr));
  AWAIT(cleared, admin::handleAdminClaudeCodeAuthDelete()); EXPECT_EQ(cleared.status(), 200);
  AWAIT(credential, credentials::loadClaudeCodeStoredCredential(scope.store)); EXPECT(!credential);
}

ASYNC_TEST("the former Claude meter refresh is a passive read of accepted normal response headers") {
  AdminScope scope;
  { StubbedAnthropic upstream; const auto req = jsonRequest(json::object{{"credential", fixtureSecret("oat-fixture-header-read")}}); AWAIT(stored, admin::handleAdminClaudeCodeAuthSet(req)); EXPECT_EQ(stored.status(), 200); }
  AWAIT(account, accounts::resolveClaudeCodeAccount(scope.store)); ASSERT(account);
  const double now = rt::dateNow();
  quota::ClaudeCodeQuotaObservation observation;
  observation.windows.set("5h", quota::ClaudeCodeQuotaWindow{"allowed", 0.42, std::nullopt, now});
  observation.representative_claim = std::nullopt; observation.observed_at_ms = now;
  quota::QuotaOutcome input;
  input.binding = account; input.observation_source = "response_headers"; input.observation = observation; input.nowMs = now;
  AWAIT(recorded, quota::recordClaudeCodeQuotaOutcome(scope.store, input)); EXPECT(recorded);
  int calls = 0;
  admin::ClaudeUsageRefreshOptions options;
  options.now = [now] { return now + 1000; };
  options.fetcher = rt::Fetch([&calls](rt::RequestInfo, rt::RequestInit) -> rt::Response { ++calls; return rt::Response::json(json::object()); });
  const auto req = jsonRequest(json::object{{"account_id", account->account_id}, {"account_cohort_id", account->account_cohort_id}});
  AWAIT(response, admin::handleAdminClaudeCodeQuotaRefresh(req, options)); EXPECT_EQ(response.status(), 200);
  AWAIT(payload, response.json());
  EXPECT_EQ(field(payload, {"refreshed"}), json::value(false)); EXPECT_EQ(field(payload, {"reason"}), json::value("response_headers"));
  EXPECT_EQ(field(payload, {"observed_at_ms"}), rt::makeNumber(now)); EXPECT_EQ(field(payload, {"usage"}), json::value(nullptr)); EXPECT_EQ(calls, 0);
}

ASYNC_TEST("a passive meter read does not refresh or display historical OAuth errors") {
  AdminScope scope;
  { StubbedAnthropic upstream; const auto req = jsonRequest(json::object{{"credential", fixtureSecret("oat-fixture-old-usage")}}); AWAIT(stored, admin::handleAdminClaudeCodeAuthSet(req)); EXPECT_EQ(stored.status(), 200); }
  AWAIT(account, accounts::resolveClaudeCodeAccount(scope.store)); ASSERT(account);
  const double now = rt::dateNow();
  quota::QuotaOutcome input;
  input.binding = account; input.observation = std::nullopt; input.nowMs = now;
  input.usage = quota::ClaudeCodeUsageState{"oauth_usage", "scope_missing", 403, "historical profile error", now, std::nullopt};
  AWAIT(recorded, quota::recordClaudeCodeQuotaOutcome(scope.store, input)); EXPECT(recorded);
  AWAIT(before, quota::loadClaudeCodeQuotaState(scope.store, account));
  int calls = 0;
  admin::ClaudeUsageRefreshOptions options;
  options.now = [now] { return now + 31000; };
  options.fetcher = rt::Fetch([&calls](rt::RequestInfo, rt::RequestInit) -> rt::Response { ++calls; return rt::Response::json(json::object()); });
  const auto req = jsonRequest(json::object{{"account_id", account->account_id}, {"account_cohort_id", account->account_cohort_id}});
  AWAIT(response, admin::handleAdminClaudeCodeQuotaRefresh(req, options));
  AWAIT(payload, response.json());
  EXPECT_EQ(field(payload, {"refreshed"}), json::value(false)); EXPECT_EQ(field(payload, {"reason"}), json::value("response_headers"));
  EXPECT_EQ(field(payload, {"observed_at_ms"}), json::value(nullptr)); EXPECT_EQ(field(payload, {"usage"}), json::value(nullptr)); EXPECT_EQ(calls, 0);
  AWAIT(after, quota::loadClaudeCodeQuotaState(scope.store, account)); EXPECT_EQ(after, before);
}

ASYNC_TEST("the admin routes stay behind admin auth and expose the documented paths") {
  T2IngressScope ingress;
  const std::vector<std::pair<std::string, std::string>> paths{
    {"GET", "/admin/providers/claude-code/auth"},
    {"POST", "/admin/providers/claude-code/auth"},
    {"DELETE", "/admin/providers/claude-code/auth"},
    {"POST", "/admin/providers/claude-code/quota/clear"},
    {"POST", "/admin/providers/claude-code/quota/refresh"},
    {"GET", "/admin/providers/claude-code/access"},
    {"POST", "/admin/providers/claude-code/access"},
    {"POST", "/admin/providers/claude-code/accounts/active"},
  };
  for (const auto& [method, path] : paths) {
    rt::RequestInit init; init.method = method;
    const rt::Request request("http://localhost" + path, init);
    auto handling = uos::handler::index::handler(request);
    auto response = co_await std::move(handling);
    EXPECT_EQ(response.status(), 401);
    if (auto body = response.body()) co_await body->cancel();
  }
}

ASYNC_TEST("a workspace-only submission without any credential reports the missing credential") {
  AdminScope scope; StubbedAnthropic upstream;
  const auto req = jsonRequest(json::object{{"workspace_id", "wrkspc_01ErorW3iFLRvQy55cpNWtEt"}});
  admin::ClaudeCodeAuthOptions options;
  options.env = [](const std::string&) { return std::optional<std::string>(); };
  AWAIT(response, admin::handleAdminClaudeCodeAuthSet(req, options)); EXPECT_EQ(response.status(), 409); EXPECT(upstream.calls.empty());
}

ASYNC_TEST("configuration cooldown recorded by the tier is visible to the admin readback") {
  AdminScope scope;
  { StubbedAnthropic upstream; const auto req = jsonRequest(json::object{{"credential", fixtureSecret("oat-fixture-config")}}); AWAIT(stored, admin::handleAdminClaudeCodeAuthSet(req)); EXPECT_EQ(stored.status(), 200); }
  AWAIT(account, accounts::resolveClaudeCodeAccount(scope.store)); ASSERT(account);
  auto recording = recordError(scope.store, *account, "workspace_not_found", 404, "Workspace `72cde404` not found."); co_await std::move(recording);
  AWAIT(response, admin::handleAdminClaudeCodeAuthGet());
  AWAIT(payload, response.json()); EXPECT_EQ(field(payload, {"data", "quota", "last_error", "kind"}), json::value("workspace_not_found"));
  EXPECT(quota::CLAUDE_CODE_CONFIG_COOLDOWN_MS > 0);
  AWAIT(workspace, scope.store->get(credentials::CLAUDE_CODE_WORKSPACE_KV_KEY)); EXPECT(!workspace.versionstamp);
}

ASYNC_TEST("account roster preserves singleton requests and explicit active selection without granting principals") {
  AdminScope scope; StubbedAnthropic upstream;
  const auto firstRequest = jsonRequest(json::object{{"credential", fixtureSecret("oat-fixture-first-admin")}, {"label", "local label, not provider identity"}});
  AWAIT(firstResponse, admin::handleAdminClaudeCodeAuthSet(firstRequest)); AWAIT(first, firstResponse.json());
  const auto secondRequest = jsonRequest(json::object{{"credential", fixtureSecret("oat-fixture-second-admin")}, {"create_new", true}, {"label", "secondary"}});
  AWAIT(secondResponse, admin::handleAdminClaudeCodeAuthSet(secondRequest)); AWAIT(second, secondResponse.json()); EXPECT_EQ(secondResponse.status(), 200);
  EXPECT_EQ(field(second, {"data", "accounts"}).as_array().size(), 2u);
  EXPECT_EQ(field(second, {"data", "active_account_id"}), field(first, {"account_id"}));
  AWAIT(policy, access::loadClaudeCodeAccessPolicy(scope.store)); EXPECT(policy.allowed_key_ids.empty());
  const auto secondId = std::string(field(second, {"account_id"}).as_string());
  const auto selectRequest = jsonRequest(json::object{{"account_id", secondId}});
  AWAIT(selection, admin::handleAdminClaudeCodeAccountActive(selectRequest)); AWAIT(selected, selection.json());
  EXPECT_EQ(field(selected, {"data", "active_account_id"}), json::value(secondId));
  EXPECT_EQ(field(selected, {"data", "active_reason"}), json::value("admin_selection"));
  EXPECT_EQ(field(selected, {"data", "credential", "label"}), json::value("secondary"));
  EXPECT_EQ(field(selected, {"data", "credential", "expires_at_ms"}), json::value(nullptr));
  const auto& firstAccount = field(selected, {"data", "accounts"}).as_array()[0];
  EXPECT_EQ(field(firstAccount, {"capabilities", "identity_verified"}), json::value(false));
  EXPECT_EQ(field(firstAccount, {"capabilities", "can_refresh"}), json::value(false));
  EXPECT(!rt::includes(rt::jsonStringify(selected), "credential_fingerprint")); EXPECT(!rt::includes(rt::jsonStringify(selected), fixtureSecret("oat-fixture-second-admin")));
  const auto replacement = jsonRequest(json::object{{"account_id", field(first, {"account_id"})}, {"credential", fixtureSecret("oat-fixture-first-replacement")}});
  AWAIT(targeted, admin::handleAdminClaudeCodeAuthSet(replacement)); EXPECT_EQ(targeted.status(), 200);
  AWAIT(roster, accounts::loadClaudeCodeAccountStore(scope.store)); EXPECT_EQ(roster.active_account_id, std::optional<std::string>(secondId));
}

ASYNC_TEST("targeted guard clear refuses a replaced cohort and never clears another account") {
  AdminScope scope; StubbedAnthropic upstream;
  const auto firstRequest = jsonRequest(json::object{{"credential", fixtureSecret("oat-fixture-first-guards")}});
  AWAIT(firstResponse, admin::handleAdminClaudeCodeAuthSet(firstRequest)); AWAIT(first, firstResponse.json());
  const auto secondRequest = jsonRequest(json::object{{"credential", fixtureSecret("oat-fixture-second-guards")}, {"create_new", true}});
  AWAIT(secondResponse, admin::handleAdminClaudeCodeAuthSet(secondRequest)); AWAIT(second, secondResponse.json());
  const auto firstId = std::string(field(first, {"account_id"}).as_string()); const auto secondId = std::string(field(second, {"account_id"}).as_string());
  const accounts::ResolveClaudeCodeAccountOptions firstOptions{firstId}; const accounts::ResolveClaudeCodeAccountOptions secondOptions{secondId};
  AWAIT(firstBinding, accounts::resolveClaudeCodeAccount(scope.store, {}, firstOptions));
  AWAIT(secondBinding, accounts::resolveClaudeCodeAccount(scope.store, {}, secondOptions)); ASSERT(firstBinding && secondBinding);
  auto recordingFirst = recordError(scope.store, *firstBinding, "workspace_required", 400, "fixture workspace required"); co_await std::move(recordingFirst);
  auto recordingSecond = recordError(scope.store, *secondBinding, "workspace_required", 400, "fixture workspace required"); co_await std::move(recordingSecond);
  const auto missingRequest = jsonRequest(json::object{{"account_id", firstId}});
  AWAIT(missing, admin::handleAdminClaudeCodeQuotaClear(missingRequest)); EXPECT_EQ(missing.status(), 400);
  const auto wrongRequest = jsonRequest(json::object{{"account_id", firstId}, {"account_cohort_id", "retired"}});
  AWAIT(wrong, admin::handleAdminClaudeCodeQuotaClear(wrongRequest)); EXPECT_EQ(wrong.status(), 409);
  const auto clearRequest = jsonRequest(json::object{{"account_id", firstId}, {"account_cohort_id", firstBinding->account_cohort_id}});
  AWAIT(cleared, admin::handleAdminClaudeCodeQuotaClear(clearRequest)); EXPECT_EQ(cleared.status(), 200);
  AWAIT(secondQuota, quota::loadClaudeCodeQuotaState(scope.store, secondBinding)); ASSERT(secondQuota && secondQuota->last_error); EXPECT_EQ(secondQuota->last_error->kind, "workspace_required");
  rt::RequestInit deleteInit; deleteInit.method = "DELETE";
  const rt::Request deleteRequest("https://ai.ubq.fi/admin/providers/claude-code/auth?account_id=" + secondId, deleteInit);
  AWAIT(deleted, admin::handleAdminClaudeCodeAuthDelete(deleteRequest)); EXPECT_EQ(deleted.status(), 200);
  AWAIT(roster, accounts::loadClaudeCodeAccountStore(scope.store)); EXPECT_EQ(roster.active_account_id, std::optional<std::string>(firstId));
}

ASYNC_TEST("explicit persisted principal controls are independent of credential import and deny by default") {
  AdminScope scope;
  const auto allowRequest = jsonRequest(json::object{{"allowed_key_ids", json::array{"fixture-principal"}}});
  AWAIT(allowed, admin::handleAdminClaudeCodeAccess(allowRequest)); EXPECT_EQ(allowed.status(), 200);
  { StubbedAnthropic upstream; const auto req = jsonRequest(json::object{{"credential", fixtureSecret("oat-fixture-policy")}}); AWAIT(stored, admin::handleAdminClaudeCodeAuthSet(req)); EXPECT_EQ(stored.status(), 200); }
  AWAIT(policy, access::loadClaudeCodeAccessPolicy(scope.store)); const std::vector<std::string> ids{"fixture-principal"}; EXPECT_EQ(policy.allowed_key_ids, ids);
  const auto denyRequest = jsonRequest(json::object{{"allowed_key_ids", json::array()}});
  AWAIT(deny, admin::handleAdminClaudeCodeAccess(denyRequest)); EXPECT_EQ(deny.status(), 200);
  const rt::Request readRequest("https://ai.ubq.fi/admin/providers/claude-code/access");
  AWAIT(readbackResponse, admin::handleAdminClaudeCodeAccess(readRequest)); AWAIT(readback, readbackResponse.json());
  EXPECT(field(readback, {"data", "allowed_key_ids"}).as_array().empty()); EXPECT_EQ(field(readback, {"data", "source"}), json::value("kv"));
  const auto malformedRequest = jsonRequest(json::object{{"allowed_key_ids", json::array{"a,b"}}});
  AWAIT(malformed, admin::handleAdminClaudeCodeAccess(malformedRequest)); EXPECT_EQ(malformed.status(), 400);
  AWAIT(after, access::loadClaudeCodeAccessPolicy(scope.store)); EXPECT(after.allowed_key_ids.empty());
}

namespace {
  // Bun 1.4.0 oracle: full error envelopes and validation fields, with synthetic KV/fetch boundaries.
  rt::Task<void> expectError(rt::Response response, double status, std::string message, std::string code) {
    EXPECT_EQ(response.status(), status);
    const auto body = co_await response.json();
    const json::object expected{{"error", json::object{{"message", std::move(message)}, {"type", "invalid_request_error"}, {"code", std::move(code)}}}};
    EXPECT_EQ(rt::jsonStringify(body), rt::jsonStringify(expected));
  }
  class FaultOperation final : public rt::ForwardingAtomicOperation {
  public:
    FaultOperation(std::unique_ptr<rt::AtomicOperation> inner_, const int& fault) : rt::ForwardingAtomicOperation(std::move(inner_)), fault_(fault) {}
    rt::Task<rt::KvCommitResult> commit() override {
      if (fault_ == 1) throw rt::JsError("fixture persistence failure");
      if (fault_ == 2) std::rethrow_exception(rt::thrown(json::object{{"fixture", "nonerror"}}));
      if (fault_ == 3) { const rt::KvCommitResult conflict{false, ""}; return rt::fulfilledTask(conflict); }
      return rt::ForwardingAtomicOperation::commit();
    }
  private:
    const int& fault_;
  };
  class FaultKv final : public rt::ForwardingKv {
  public:
    int fault = 0;
    explicit FaultKv(rt::Kv& inner_) : rt::ForwardingKv(inner_) {}
    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<FaultOperation>(rt::ForwardingKv::atomic(), fault); }
  };
}

ASYNC_TEST("source oracle: unavailable storage preserves route-specific status and full error envelopes") {
  AdminScope scope; uos::kv::setKvForTest(nullptr);
  const auto req = jsonRequest(json::object());
  AWAIT(set, admin::handleAdminClaudeCodeAuthSet(req)); auto setCheck = expectError(set, 500, "KV is not available; cannot persist the Claude credential", "server_error"); co_await std::move(setCheck);
  AWAIT(remove, admin::handleAdminClaudeCodeAuthDelete()); auto removeCheck = expectError(remove, 500, "KV is not available; cannot remove the Claude account", "server_error"); co_await std::move(removeCheck);
  AWAIT(clear, admin::handleAdminClaudeCodeQuotaClear()); auto clearCheck = expectError(clear, 500, "KV is not available; cannot clear Claude guards", "server_error"); co_await std::move(clearCheck);
  AWAIT(refresh, admin::handleAdminClaudeCodeQuotaRefresh(req)); auto refreshCheck = expectError(refresh, 503, "Claude quota storage is unavailable", "server_error"); co_await std::move(refreshCheck);
  AWAIT(policy, admin::handleAdminClaudeCodeAccess(req)); auto policyCheck = expectError(policy, 503, "Claude principal policy storage is unavailable", "server_error"); co_await std::move(policyCheck);
  AWAIT(active, admin::handleAdminClaudeCodeAccountActive(req)); auto activeCheck = expectError(active, 503, "Claude account storage is unavailable", "server_error"); co_await std::move(activeCheck);
}

ASYNC_TEST("source oracle: malformed fields preserve JavaScript array and UTF-16 validation branches") {
  AdminScope scope; StubbedAnthropic upstream;
  struct Case { json::value body; std::string message; };
  const auto secret = fixtureSecret("usr-fixture-branch");
  const std::string id = "00000000-0000-4000-8000-000000000001";
  const std::vector<Case> cases{
    {nullptr, "Invalid JSON body"}, {json::array(), "credential or workspace_id is required"}, {json::object(), "credential or workspace_id is required"},
    {json::object{{"credential", fixtureSecret(std::string(506, 'x'))}}, "credential is too long"},
    {json::object{{"credential", secret}, {"label", rt::repeat("😀", 33)}}, "label is too long"},
    {json::object{{"credential", secret}, {"account_id", 7}}, "account_id is invalid"},
    {json::object{{"credential", secret}, {"account_id", "not-an-id"}}, "account_id is invalid"},
    {json::object{{"credential", secret}, {"create_new", nullptr}}, "create_new must be boolean"},
    {json::object{{"credential", secret}, {"activate", "true"}}, "activate must be boolean"},
    {json::object{{"credential", secret}, {"create_new", true}, {"account_id", id}}, "create_new cannot target an existing account"},
    {json::object{{"credential", secret}, {"account_id", id}}, "Claude account was not found"},
  };
  for (const auto& row : cases) {
    const auto req = jsonRequest(row.body);
    AWAIT(response, admin::handleAdminClaudeCodeAuthSet(req));
    auto checking = expectError(response, 400, row.message, "invalid_request_error"); co_await std::move(checking);
  }
  EXPECT(upstream.calls.empty());
}

ASYNC_TEST("source oracle: upstream authorization, quota, malformed pages and transport validation stay distinct") {
  struct Case { std::string mode; int upstream; int status; std::optional<double> validation; std::optional<std::string> detail; std::optional<std::string> error; };
  const std::string unvalidated = "Stored unvalidated: the models listing could not be read.";
  const std::vector<Case> cases{
    {"http", 401, 400, {}, {}, "The credential was rejected as unauthenticated."}, {"http", 403, 400, {}, {}, "The credential is not permitted for this endpoint."},
    {"http", 429, 200, 429, {}, {}}, {"http", 503, 200, 503, unvalidated, {}}, {"throw", 200, 200, {}, unvalidated, {}}, {"malformed", 200, 200, 200, unvalidated, {}},
  };
  for (const auto& row : cases) {
    AdminScope scope;
    StubbedAnthropic upstream([row](const StubCall&) -> rt::Response {
      if (row.mode == "throw") throw rt::JsError("fixture transport failure");
      const json::object body = row.mode == "malformed" ? json::object{{"data", nullptr}} : json::object{{"error", json::object{{"message", "fixture error"}}}};
      return rt::Response::json(body, rt::ResponseInit{.status = row.upstream});
    });
    const auto req = jsonRequest(json::object{{"credential", fixtureSecret("usr-fixture-branch")}});
    AWAIT(response, admin::handleAdminClaudeCodeAuthSet(req)); EXPECT_EQ(response.status(), row.status); EXPECT_EQ(upstream.calls.size(), 1u);
    if (row.error) {
      auto checking = expectError(response, row.status, *row.error, "invalid_api_key"); co_await std::move(checking);
      AWAIT(stored, scope.store->get(credentials::CLAUDE_CODE_AUTH_KV_KEY)); EXPECT(!stored.versionstamp);
    } else {
      AWAIT(body, response.json());
      const json::object expected{{"status", row.validation ? rt::makeNumber(*row.validation) : json::value(nullptr)}, {"detail", row.detail ? json::value(*row.detail) : json::value(nullptr)}};
      EXPECT_EQ(rt::jsonStringify(field(body, {"validation"})), rt::jsonStringify(expected));
      EXPECT_EQ(field(body, {"hint"}), json::value(fixtureSecret("usr-fix...anch")));
      EXPECT(!rt::includes(rt::jsonStringify(body), fixtureSecret("usr-fixture-branch")));
    }
  }
}

ASYNC_TEST("source oracle: refresh, clear, selection and policy refuse invalid targets without upstream calls") {
  AdminScope scope;
  struct Case { std::string method; json::value body; int status; std::string message; std::string code; };
  const std::vector<Case> cases{
    {"refresh", json::object(), 400, "account_id is required", "invalid_request_error"},
    {"refresh", json::object{{"account_id", "fixture"}, {"account_cohort_id", 7}}, 400, "account_cohort_id must be a string", "invalid_request_error"},
    {"refresh", json::object{{"account_id", "fixture"}}, 404, "Claude account was not found", "not_found"},
    {"clear", nullptr, 400, "Invalid JSON body", "invalid_request_error"},
    {"clear", json::object{{"account_id", 7}}, 400, "account_id must be a string", "invalid_request_error"},
    {"clear", json::object{{"account_id", "fixture"}}, 404, "Claude account was not found", "not_found"},
    {"active", json::object(), 400, "account_id is required", "invalid_request_error"},
    {"active", json::object{{"account_id", "fixture"}}, 404, "Claude account was not found", "not_found"},
    {"access", json::object{{"allowed_key_ids", json::array{"a,b"}}}, 400, "allowed_key_ids must be an explicit array of principal IDs", "invalid_request_error"},
  };
  for (const auto& row : cases) {
    const auto req = jsonRequest(row.body);
    auto pending = row.method == "refresh" ? admin::handleAdminClaudeCodeQuotaRefresh(req) : row.method == "clear" ? admin::handleAdminClaudeCodeQuotaClear(req) : row.method == "active" ? admin::handleAdminClaudeCodeAccountActive(req) : admin::handleAdminClaudeCodeAccess(req);
    const auto response = co_await std::move(pending);
    auto checking = expectError(response, row.status, row.message, row.code); co_await std::move(checking);
  }
  AWAIT(clear, admin::handleAdminClaudeCodeQuotaClear()); AWAIT(clearBody, clear.json()); EXPECT_EQ(clear.status(), 200); EXPECT_EQ(field(clearBody, {"guards_cleared"}), json::value(false));
  AWAIT(remove, admin::handleAdminClaudeCodeAuthDelete()); EXPECT_EQ(remove.status(), 200);
  const rt::Request req("https://ai.ubq.fi/admin/providers/claude-code/auth?account_id=fixture");
  AWAIT(missing, admin::handleAdminClaudeCodeAuthDelete(req)); auto checking = expectError(missing, 404, "Claude account was not found", "not_found"); co_await std::move(checking);
}

ASYNC_TEST("source oracle: persistence errors retain Error messages and use the non-Error fallback") {
  for (int fault = 1; fault <= 2; ++fault) {
    AdminScope scope; StubbedAnthropic upstream;
    const auto store = std::make_shared<FaultKv>(*scope.store); store->fault = fault; uos::kv::setKvForTest(store);
    const auto req = jsonRequest(json::object{{"credential", fixtureSecret("usr-fixture-branch")}});
    AWAIT(response, admin::handleAdminClaudeCodeAuthSet(req));
    const std::string message = fault == 1 ? "fixture persistence failure" : "Claude account changed before persistence";
    auto checking = expectError(response, 409, message, "claude_code_account_changed"); co_await std::move(checking);
  }
}

ASYNC_TEST("source oracle: api_key alias trims values and a conflicting guarded clear fails closed") {
  AdminScope scope; StubbedAnthropic upstream;
  const auto store = std::make_shared<FaultKv>(*scope.store); uos::kv::setKvForTest(store);
  const auto req = jsonRequest(json::object{{"api_key", " " + fixtureSecret("usr-fixture-branch") + " "}, {"label", "  label  "}, {"workspace_id", "wrkspc_fixture"}});
  AWAIT(storedResponse, admin::handleAdminClaudeCodeAuthSet(req)); EXPECT_EQ(storedResponse.status(), 200); AWAIT(stored, storedResponse.json());
  EXPECT_EQ(field(stored, {"data", "workspace", "id"}), json::value("wrkspc_fixture"));
  EXPECT_EQ(field(stored, {"data", "credential", "label"}), json::value("label"));
  ASSERT_EQ(upstream.calls.size(), 1u); EXPECT_EQ(upstream.calls[0].headers.get("x-api-key"), std::optional<std::string>(fixtureSecret("usr-fixture-branch")));
  const auto id = std::string(field(stored, {"account_id"}).as_string()); const auto cohort = std::string(field(stored, {"data", "account_cohort_id"}).as_string());
  const auto staleRequest = jsonRequest(json::object{{"account_id", id}, {"account_cohort_id", "retired"}});
  AWAIT(stale, admin::handleAdminClaudeCodeQuotaRefresh(staleRequest)); auto staleCheck = expectError(stale, 409, "Claude account generation changed", "claude_code_account_changed"); co_await std::move(staleCheck);
  const auto clearRequest = jsonRequest(json::object{{"account_id", id}, {"account_cohort_id", cohort}});
  store->fault = 3;
  AWAIT(conflict, admin::handleAdminClaudeCodeQuotaClear(clearRequest)); auto conflictCheck = expectError(conflict, 409, "Claude guards changed during clear", "claude_code_account_changed"); co_await std::move(conflictCheck);
  EXPECT_EQ(upstream.calls.size(), 1u);
}
#undef AWAIT
