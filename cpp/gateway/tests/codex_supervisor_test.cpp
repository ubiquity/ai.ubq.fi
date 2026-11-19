// Port of tests/codex-supervisor.test.ts, retaining component assertions and real router authentication.
// Links: tests/codex_supervisor_test.cpp
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
#include "uos/codex/supervisor_config.hpp"
#include "uos/codex/supervisor_follow.hpp"
#include "uos/codex/supervisor_inventory.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/test.hpp"

namespace {
  using namespace uos::codex::supervisor_config;
  using uos::codex::supervisor_follow::extractFollowEntries;
  using uos::codex::supervisor_follow::FollowCursor;
  using uos::codex::supervisor_follow::parseFollowCursor;
  using uos::codex::supervisor_follow::SupervisorFollowEntry;
  using uos::codex::supervisor_inventory::classifySupervisorState;
  using uos::codex::supervisor_inventory::ListedThread;
  using uos::codex::supervisor_inventory::quotaFromRateLimits;
  using uos::codex::supervisor_inventory::sessionFromParts;
  using uos::codex::supervisor_inventory::SessionFromPartsInput;
  using uos::codex::supervisor_inventory::ThreadMetadata;
  using uos::codex::supervisor_inventory::ThreadProbe;
  namespace rt = uos::rt;
  namespace json = uos::json;

  std::vector<std::string> idsOf(const std::vector<SupervisorSource>& sources) {
    std::vector<std::string> ids;
    for (const SupervisorSource& source : sources) ids.push_back(source.id);
    return ids;
  }
}

TEST("parseSupervisorConfig accepts a valid source list") {
  const ParsedSupervisorConfig parsed = parseSupervisorConfig(rt::jsonParse(R"({
    "sources": [
      { "id": "local", "name": "This Mac", "socketPath": "/var/lib/uos-supervisor/a.sock", "codexHome": "/var/lib/uos-supervisor/.codex" },
      { "id": "vps", "name": "VPS", "socketPath": "/var/lib/uos-supervisor/b.sock" }
    ]
  })"));
  EXPECT_EQ(parsed.notes, std::vector<std::string>{});
  const std::vector<SupervisorSource> expected{
    {.id = "local", .name = "This Mac", .socketPath = "/var/lib/uos-supervisor/a.sock", .codexHome = "/var/lib/uos-supervisor/.codex"},
    {.id = "vps", .name = "VPS", .socketPath = "/var/lib/uos-supervisor/b.sock", .codexHome = std::nullopt},
  };
  EXPECT(parsed.sources == expected);
}

TEST("parseSupervisorConfig ignores unsafe or malformed sources") {
  const ParsedSupervisorConfig parsed = parseSupervisorConfig(rt::jsonParse(R"({
    "sources": [
      { "id": "Local", "name": "Uppercase id", "socketPath": "/var/lib/uos-supervisor/a.sock" },
      { "id": "relative", "name": "Relative socket", "socketPath": "app-server.sock" },
      { "id": "relative-home", "name": "Relative home", "socketPath": "/var/lib/uos-supervisor/c.sock", "codexHome": "relative/.codex" },
      { "id": "dupe", "name": "First", "socketPath": "/var/lib/uos-supervisor/d.sock" },
      { "id": "dupe", "name": "Second", "socketPath": "/var/lib/uos-supervisor/e.sock" },
      { "id": "ok", "name": "OK", "socketPath": "/var/lib/uos-supervisor/f.sock" }
    ]
  })"));
  EXPECT_EQ(idsOf(parsed.sources), (std::vector<std::string>{"dupe", "ok"}));
  EXPECT_EQ(parsed.notes.size(), 4u);
}

TEST("parseSupervisorConfig requires a sources array") {
  EXPECT(parseSupervisorConfig(json::value(nullptr)).sources.empty());
  EXPECT(parseSupervisorConfig(json::object{}).sources.empty());
  EXPECT_EQ(parseSupervisorConfig(rt::jsonParse(R"({ "sources": [] })")).notes.size(), 1u);
}

TEST("defaultSupervisorSource prefers CODEX_HOME and falls back to HOME") {
  std::vector<std::string> notes;
  const std::optional<SupervisorSource> fromCodexHome =
    defaultSupervisorSource([](std::string_view name) -> std::optional<std::string> { return name == "CODEX_HOME" ? "/custom/codex" : "/home/tester"; }, notes);
  EXPECT(fromCodexHome == (SupervisorSource{
                             .id = "local",
                             .name = "Local Codex",
                             .socketPath = "/custom/codex/app-server-control/app-server-control.sock",
                             .codexHome = "/custom/codex",
                           }));
  const std::optional<SupervisorSource> fromHome =
    defaultSupervisorSource([](std::string_view name) -> std::optional<std::string> { return name == "HOME" ? std::optional<std::string>("/home/tester/") : std::nullopt; }, notes);
  ASSERT(fromHome.has_value());
  EXPECT_EQ(fromHome->codexHome, std::optional<std::string>("/home/tester/.codex"));
  EXPECT_EQ(fromHome->socketPath, "/home/tester/.codex/app-server-control/app-server-control.sock");
  EXPECT(defaultSupervisorSource([](std::string_view) -> std::optional<std::string> { return std::nullopt; }, notes) == std::nullopt);
  EXPECT(notes.size() >= 1);
}

TEST("normalizeEpochMs accepts seconds and milliseconds") {
  EXPECT_EQ(normalizeEpochMs(rt::jsonParse("1730831111")), std::optional<double>(1'730'831'111'000));
  EXPECT_EQ(normalizeEpochMs(rt::jsonParse("1730831111000")), std::optional<double>(1'730'831'111'000));
  EXPECT_EQ(normalizeEpochMs(rt::jsonParse("0")), std::nullopt);
  EXPECT_EQ(normalizeEpochMs(rt::jsonParse("-5")), std::nullopt);
  EXPECT_EQ(normalizeEpochMs(json::value("1730831111")), std::nullopt);
  EXPECT_EQ(normalizeEpochMs(json::value(nullptr)), std::nullopt);
}

TEST("classifySupervisorState treats runtime status as authoritative") {
  EXPECT(classifySupervisorState({"active", "inProgress", true}) == SupervisorState::active);
  EXPECT(classifySupervisorState({"systemError", std::nullopt, true}) == SupervisorState::system_error);
  EXPECT(classifySupervisorState({"idle", std::nullopt, false}) == SupervisorState::idle);
  EXPECT(classifySupervisorState({"notLoaded", "completed", false}) == SupervisorState::idle);
}

TEST("classifySupervisorState never fabricates idle") {
  EXPECT(classifySupervisorState({"notLoaded", "inProgress", false}) == SupervisorState::stale);
  EXPECT(classifySupervisorState({"notLoaded", std::nullopt, false}) == SupervisorState::unknown);
  EXPECT(classifySupervisorState({std::nullopt, "completed", false}) == SupervisorState::unknown);
  EXPECT(classifySupervisorState({std::nullopt, "inProgress", true}) == SupervisorState::active);
  EXPECT(classifySupervisorState({std::nullopt, "inProgress", false}) == SupervisorState::stale);
  EXPECT(classifySupervisorState({std::nullopt, std::nullopt, true}) == SupervisorState::unknown);
}

TEST("extractFollowEntries keeps only assistant messages and command output") {
  const std::vector<SupervisorFollowEntry> entries = extractFollowEntries("turn_1", rt::jsonParse(R"([
    { "type": "reasoning", "id": "r1", "summary": ["private reasoning"], "content": ["private reasoning"] },
    { "type": "userMessage", "id": "u1", "content": [{ "type": "text", "text": "hello" }] },
    { "type": "agentMessage", "id": "a1", "text": "Working on it", "phase": "commentary" },
    { "type": "commandExecution", "id": "c1", "command": "deno task test", "status": "completed", "exitCode": 0, "aggregatedOutput": "ok\n" },
    { "type": "fileChange", "id": "f1", "changes": [] }
  ])"));
  std::vector<std::string> kinds;
  for (const SupervisorFollowEntry& entry : entries) kinds.push_back(entry.kind);
  EXPECT_EQ(kinds, (std::vector<std::string>{"message", "command"}));
  ASSERT(entries.size() == 2);
  EXPECT_EQ(entries[0].key, "turn_1:a1");
  EXPECT_EQ(entries[0].text, "Working on it");
  EXPECT_EQ(entries[0].status, std::optional<std::string>("commentary"));
  EXPECT_EQ(entries[0].index, 2.0);
  EXPECT_EQ(entries[1].key, "turn_1:c1");
  EXPECT_EQ(entries[1].command, std::optional<std::string>("deno task test"));
  EXPECT_EQ(entries[1].text, "ok\n");
  EXPECT_EQ(entries[1].exitCode, std::optional<double>(0));
  EXPECT_EQ(entries[1].index, 3.0);
}

TEST("extractFollowEntries tolerates unknown payloads") {
  EXPECT(extractFollowEntries("turn_1", json::value(nullptr)).empty());
  EXPECT(extractFollowEntries("turn_1", json::value("not-an-array")).empty());
  std::vector<std::string> texts;
  for (const SupervisorFollowEntry& entry : extractFollowEntries("turn_1", rt::jsonParse(R"([null, 7, { "type": "agentMessage" }])"))) texts.push_back(entry.text);
  EXPECT_EQ(texts, (std::vector<std::string>{""}));
}

TEST("parseFollowCursor round-trips turn ids that contain separators") {
  EXPECT(parseFollowCursor("turn_1:4") == (FollowCursor{"turn_1", 4}));
  EXPECT(parseFollowCursor("thr:a:b:0") == (FollowCursor{"thr:a:b", 0}));
  EXPECT(parseFollowCursor("turn_1:-1") == (FollowCursor{"turn_1", -1}));
  EXPECT(parseFollowCursor("") == std::nullopt);
  EXPECT(parseFollowCursor("turn_1") == std::nullopt);
  EXPECT(parseFollowCursor("turn_1:x") == std::nullopt);
}

ASYNC_TEST("the supervisor routes require admin auth in the real router") {
  T2IngressScope ingress;
  const rt::Request sessionsRequest("https://ai.ubq.fi/admin/codex/supervisor/sessions");
  auto listing = uos::handler::index::handler(sessionsRequest);
  const auto sessions = co_await std::move(listing);
  EXPECT_EQ(sessions.status(), 401);
  const rt::Request outputRequest("https://ai.ubq.fi/admin/codex/supervisor/output?source=local&id=thread-1");
  auto following = uos::handler::index::handler(outputRequest);
  const auto output = co_await std::move(following);
  EXPECT_EQ(output.status(), 401);
}

TEST("remote rows use verified live thread fields and keep token usage unavailable") {
  const SupervisorSession session = sessionFromParts(SessionFromPartsInput{
    .id = "01a0c7ae-5be8-7712-aaee-26638cf84a32",
    .source = {.id = "vps", .name = "vps.pavlovcik.com", .socketPath = "/var/lib/uos-supervisor/vps.sock", .codexHome = std::nullopt},
    .metadata = std::nullopt,
    .listed = ListedThread{
      .id = "01a0c7ae-5be8-7712-aaee-26638cf84a32",
      .name = std::nullopt,
      .preview = "Fix agent dispatch validation",
      .cwd = "/home/codex/repos/ubiquity/ai.ubq.fi",
      .model = "deepseek-flash",
      .effort = "high",
      .sourceKind = "cli",
      .parentThreadId = "01a0b67c-parent",
      .updatedAtMs = 1'789'852'742'000,
      .modelProvider = "uos",
    },
    .probe = ThreadProbe{.runtimeStatus = "active", .activeFlags = {}, .turnStatus = "inProgress", .thread = std::nullopt, .sampled = true},
    .loaded = true,
    .sampledAtMs = 1'790'056'921'788,
    .childIds = std::vector<std::string>{},
    .unavailable = {"token usage is not reported by the source"},
  });
  EXPECT_EQ(session.title, std::optional<std::string>("Fix agent dispatch validation"));
  EXPECT_EQ(session.titleSource, std::optional<std::string>("thread"));
  EXPECT_EQ(session.cwd, std::optional<std::string>("/home/codex/repos/ubiquity/ai.ubq.fi"));
  EXPECT_EQ(session.model, std::optional<std::string>("deepseek-flash"));
  EXPECT_EQ(session.effort, std::optional<std::string>("high"));
  EXPECT_EQ(session.sourceKind, std::optional<std::string>("cli"));
  EXPECT_EQ(session.parentThreadId, std::optional<std::string>("01a0b67c-parent"));
  EXPECT_EQ(session.provider, std::optional<std::string>("uos"));
  EXPECT(session.state == SupervisorState::active);
  EXPECT(!session.tokensUsed.has_value());
  EXPECT(!session.usageSource.has_value());
  EXPECT_EQ(session.unavailable, (std::vector<std::string>{"token usage is not reported by the source"}));
}

TEST("thread/read fields serve loaded threads and local metadata only enriches gaps") {
  const SupervisorSession session = sessionFromParts(SessionFromPartsInput{
    .id = "01a0bd67-loaded-only",
    .source = {.id = "local", .name = "m1.local", .socketPath = "/var/lib/uos-supervisor/local.sock", .codexHome = "/Users/example/.codex"},
    .metadata = ThreadMetadata{
      .title = std::nullopt,
      .preview = std::nullopt,
      .cwd = std::nullopt,
      .model = std::nullopt,
      .effort = std::nullopt,
      .tokensUsed = 10'424'391,
      .gitBranch = "development",
      .updatedAtMs = 1,
      .archived = false,
      .threadSource = "user",
      .parentThreadId = std::nullopt,
      .agentNickname = std::nullopt,
    },
    .listed = std::nullopt,
    .probe = ThreadProbe{
      .runtimeStatus = "idle",
      .activeFlags = {},
      .turnStatus = "completed",
      .thread = ListedThread{
        .id = "01a0bd67-loaded-only",
        .name = "Named thread",
        .preview = std::nullopt,
        .cwd = "/repo",
        .model = "gpt-6-astra",
        .effort = "max",
        .sourceKind = "vscode",
        .parentThreadId = std::nullopt,
        .updatedAtMs = 2,
        .modelProvider = "uos",
      },
      .sampled = true,
    },
    .loaded = true,
    .sampledAtMs = 3,
    .childIds = std::nullopt,
    .unavailable = {},
  });
  EXPECT_EQ(session.title, std::optional<std::string>("Named thread"));
  EXPECT_EQ(session.cwd, std::optional<std::string>("/repo"));
  EXPECT_EQ(session.model, std::optional<std::string>("gpt-6-astra"));
  EXPECT_EQ(session.effort, std::optional<std::string>("max"));
  EXPECT_EQ(session.sourceKind, std::optional<std::string>("vscode"));
  EXPECT(session.state == SupervisorState::idle);
  EXPECT_EQ(session.branch, std::optional<std::string>("development"));
  EXPECT_EQ(session.tokensUsed, std::optional<double>(10'424'391));
  EXPECT_EQ(session.usageSource, std::optional<std::string>("state_db"));
}

TEST("listed session gaps use probed fields before metadata and preserve nonnull listed values") {
  const ListedThread thread{
    .id = "01a0bd67-listed-and-probed",
    .name = "Probed title",
    .preview = "Probed preview",
    .cwd = "/probed-repo",
    .model = "gpt-6.1-sol",
    .effort = "ultra",
    .sourceKind = "subagent",
    .parentThreadId = "probed-parent",
    .updatedAtMs = 2,
    .modelProvider = "uos",
  };
  const ListedThread listed{
    .id = thread.id,
    .name = std::nullopt,
    .preview = std::nullopt,
    .cwd = std::nullopt,
    .model = std::nullopt,
    .effort = std::nullopt,
    .sourceKind = std::nullopt,
    .parentThreadId = std::nullopt,
    .updatedAtMs = std::nullopt,
    .modelProvider = std::nullopt,
  };
  const SessionFromPartsInput input{
    .id = thread.id,
    .source = {.id = "local", .name = "Test machine", .socketPath = "/var/lib/uos-supervisor/local.sock", .codexHome = "/Users/example/.codex"},
    .metadata = ThreadMetadata{
      .title = "Metadata title",
      .preview = "Metadata preview",
      .cwd = "/metadata-repo",
      .model = "metadata-model",
      .effort = "low",
      .tokensUsed = 0,
      .gitBranch = "development",
      .updatedAtMs = 1,
      .archived = false,
      .threadSource = "metadata-source",
      .parentThreadId = "metadata-parent",
      .agentNickname = std::nullopt,
    },
    .listed = std::nullopt,
    .probe = ThreadProbe{.runtimeStatus = "active", .activeFlags = {}, .turnStatus = "inProgress", .thread = thread, .sampled = true},
    .loaded = true,
    .sampledAtMs = 3,
    .childIds = std::nullopt,
    .unavailable = {},
  };
  SessionFromPartsInput filledInput = input;
  filledInput.listed = listed;
  const SupervisorSession filled = sessionFromParts(filledInput);
  EXPECT_EQ(filled.title, thread.name);
  EXPECT_EQ(filled.titleSource, std::optional<std::string>("thread"));
  EXPECT_EQ(filled.cwd, thread.cwd);
  EXPECT_EQ(filled.model, thread.model);
  EXPECT_EQ(filled.effort, thread.effort);
  EXPECT_EQ(filled.provider, thread.modelProvider);
  EXPECT_EQ(filled.sourceKind, thread.sourceKind);
  EXPECT_EQ(filled.parentThreadId, thread.parentThreadId);
  EXPECT_EQ(filled.lastActivityAtMs, thread.updatedAtMs);
  ListedThread conflicting = thread;
  conflicting.name = "Listed title";
  conflicting.preview = "Listed preview";
  conflicting.cwd = "/listed-repo";
  conflicting.model = "listed-model";
  conflicting.effort = "high";
  conflicting.sourceKind = "cli";
  conflicting.parentThreadId = "listed-parent";
  conflicting.updatedAtMs = 0;
  conflicting.modelProvider = "listed-provider";
  SessionFromPartsInput retainedInput = input;
  retainedInput.listed = conflicting;
  const SupervisorSession retained = sessionFromParts(retainedInput);
  EXPECT_EQ(retained.title, conflicting.name);
  EXPECT_EQ(retained.cwd, conflicting.cwd);
  EXPECT_EQ(retained.model, conflicting.model);
  EXPECT_EQ(retained.effort, conflicting.effort);
  EXPECT_EQ(retained.provider, conflicting.modelProvider);
  EXPECT_EQ(retained.sourceKind, conflicting.sourceKind);
  EXPECT_EQ(retained.parentThreadId, conflicting.parentThreadId);
  EXPECT_EQ(retained.lastActivityAtMs, std::optional<double>(0));
  ListedThread mixedListed = conflicting;
  mixedListed.model = std::nullopt;
  mixedListed.parentThreadId = std::nullopt;
  SessionFromPartsInput mixedInput = input;
  mixedInput.listed = mixedListed;
  const SupervisorSession mixed = sessionFromParts(mixedInput);
  EXPECT_EQ(mixed.title, conflicting.name);
  EXPECT_EQ(mixed.model, thread.model);
  EXPECT_EQ(mixed.parentThreadId, thread.parentThreadId);
  SessionFromPartsInput previewInput = input;
  previewInput.listed = listed;
  ListedThread unnamed = thread;
  unnamed.name = std::nullopt;
  previewInput.probe->thread = unnamed;
  const SupervisorSession preview = sessionFromParts(previewInput);
  EXPECT_EQ(preview.title, thread.preview);
}

TEST("shared quota projection keeps one account-wide bucket per limit") {
  const std::optional<SupervisorQuota> quota = quotaFromRateLimits(rt::jsonParse(R"({
    "rateLimits": { "limitId": "codex", "primary": { "usedPercent": 29, "windowDurationMins": 10080, "resetsAt": 1790478535 } },
    "rateLimitsByLimitId": {
      "codex": {
        "limitId": "codex",
        "primary": { "usedPercent": 29, "windowDurationMins": 10080, "resetsAt": 1790478535 },
        "planType": "pro",
        "rateLimitReachedType": null
      }
    },
    "rateLimitResetCredits": { "availableCount": 1 }
  })"));
  ASSERT(quota.has_value());
  EXPECT_EQ(quota->accountScope, "shared_codex_account");
  const std::vector<SupervisorQuotaBucket> expected{{
    .limitId = "codex",
    .limitName = std::nullopt,
    .usedPercent = 29,
    .windowDurationMins = 10080,
    .resetsAtMs = 1'790'478'535'000,
    .planType = "pro",
    .reachedType = std::nullopt,
  }};
  EXPECT(quota->buckets == expected);
  EXPECT_EQ(quota->resetCreditsAvailable, std::optional<double>(1));
  EXPECT(quotaFromRateLimits(json::object()) == std::nullopt);
  EXPECT(quotaFromRateLimits(json::value(nullptr)) == std::nullopt);
}
