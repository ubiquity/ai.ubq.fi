// Links: tests/gemini_subscription_test.cpp src/admin/api_key_audit.cpp src/admin/api_key_mutations.cpp src/admin/api_keys.cpp src/admin/capacity_observation_sync.cpp src/admin/chart_source_proxy.cpp src/admin/claude_code.cpp
// src/admin/codex.cpp src/admin/codex_reset.cpp src/admin/defaults.cpp src/admin/error_log.cpp src/admin/gemini_quota.cpp src/admin/index.cpp src/admin/kernel.cpp src/admin/sentinel_incident.cpp src/agent_messages.cpp src/analytics.cpp
// src/api_key_policy.cpp src/api_keys.cpp src/auth/index.cpp src/auth/local_admin.cpp src/auth/local_development_key.cpp src/auth/passkeys.cpp src/auth/passport.cpp src/auth/passport_sessions.cpp src/auth/relay.cpp
// src/authority/contracts.cpp src/authority/native_api_key.cpp src/authority/native_api_key_audit.cpp src/authority/native_api_key_policy.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp
// src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_contracts.cpp src/authority/native_kernel.cpp
// src/authority/native_kernel_contracts.cpp src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp src/authority/native_paid_policy.cpp src/bounded_response_body.cpp src/cache/kv_migration_base.cpp
// src/cache/kv_migration_paid_fallback.cpp src/cache/kv_migration_usage.cpp src/cache/kv_migration_validation.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp
// src/cache/scope_experiment.cpp src/cache/scope_experiment_model.cpp src/cache/scope_experiment_targets.cpp src/cache/scope_targets.cpp src/cache/telemetry_gate.cpp src/catalog/index.cpp src/catalog/models.cpp src/catalog/promotion.cpp
// src/catalog/store.cpp src/catalog/types.cpp src/chat/completions.cpp src/chat/envelope.cpp src/chat/stream_translation.cpp src/codex/account_routing.cpp src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp
// src/codex/banked_reset_claim.cpp src/codex/banked_reset_pool.cpp src/codex/banked_reset_provider.cpp src/codex/banked_reset_submission.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp
// src/codex/external_auth.cpp src/codex/index.cpp src/codex/manual_reset.cpp src/codex/manual_reset_state.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/quota.cpp
// src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/responses_error.cpp src/codex/responses_operations.cpp src/codex/responses_runners.cpp src/codex/responses_state.cpp src/codex/routing_evaluation.cpp
// src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/codex/supervisor.cpp src/codex/supervisor_brief.cpp src/codex/supervisor_config.cpp src/codex/supervisor_follow.cpp
// src/codex/supervisor_inventory.cpp src/codex/supervisor_log.cpp src/codex/supervisor_secret.cpp src/codex/supervisor_transport.cpp src/config.cpp src/debug_routing.cpp src/decisions/handlers.cpp src/deepseek/chat_projection.cpp
// src/deepseek/context_admission_failure.cpp src/deepseek/flash_aliases.cpp src/deepseek/forwarded_payload_policy.cpp src/deepseek/handlers.cpp src/deepseek/index.cpp src/deepseek/responses.cpp src/deepseek/responses_payload.cpp
// src/deepseek/responses_stream.cpp src/deepseek/stream.cpp src/deepseek/waterfall.cpp src/deepseek/waterfall_handler.cpp src/defaults.cpp src/embeddings/handlers.cpp src/embeddings/jobs.cpp src/embeddings/ledger.cpp
// src/embeddings/voyage.cpp src/handler/admission.cpp src/handler/http.cpp src/handler/index.cpp src/handler/terminal_log.cpp src/handler/terminal_route.cpp src/health.cpp src/http.cpp src/images.cpp src/inference_admission.cpp
// src/inference_deadline.cpp src/input_normalization.cpp src/kernel/attestation.cpp src/kernel/policy_queue.cpp src/kernel/quota_reservations.cpp src/kernel/quota_reserve.cpp src/kernel/quota_v2.cpp src/kernel/quota_write.cpp
// src/kernel/usage.cpp src/kv.cpp src/kv_conversion/profile_json.cpp src/lib/jev_compaction/codex_items.cpp src/lib/jev_compaction/compact.cpp src/lib/jev_compaction/request.cpp src/lib/jev_compaction/state.cpp
// src/lib/jev_compaction/types.cpp src/live/calls.cpp src/live/handler.cpp src/live/routes.cpp src/live/sideband.cpp src/live/upstream.cpp src/metered_quota.cpp src/models/catalog.cpp src/models/codex_models.cpp
// src/models/codex_models_availability.cpp src/models/codex_models_fetch.cpp src/models/codex_models_whitelist.cpp src/models/metadata.cpp src/models/openrouter_models.cpp src/openai.cpp src/openai_schema.cpp src/openai_telemetry.cpp
// src/optional_telemetry_queue.cpp src/paid_fallback/claude_code_route.cpp src/paid_fallback/health.cpp src/paid_fallback/index.cpp src/paid_fallback/ledger_admission.cpp src/paid_fallback/ledger_backfill.cpp
// src/paid_fallback/ledger_settlement.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/paid_fallback/removed_provider.cpp src/paid_fallback/rollups.cpp src/paid_fallback/routing.cpp
// src/provider/anthropic_chat_responses.cpp src/provider/anthropic_chat_stream.cpp src/provider/anthropic_replay.cpp src/provider/anthropic_response_wire.cpp src/provider/anthropic_responses.cpp src/provider/capacity.cpp
// src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp src/provider/cerebras.cpp
// src/provider/cerebras_handlers.cpp src/provider/cerebras_rate_limits.cpp src/provider/cerebras_responses.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code.cpp
// src/provider/claude_code_access.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_models.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp
// src/provider/gemini.cpp src/provider/health.cpp src/provider/lithos.cpp src/provider/lithos_handlers.cpp src/provider/lithos_models.cpp src/provider/lithos_rate_limits.cpp src/provider/lithos_streams.cpp src/provider/metered.cpp
// src/provider/openrouter.cpp src/provider/openrouter_handlers.cpp src/provider/openrouter_request_controls.cpp src/provider/openrouter_streams.cpp src/provider/openrouter_web_tools.cpp src/provider/presentation.cpp
// src/provider/removed_provider_circuit.cpp src/provider/removed_provider_telemetry.cpp src/provider/selection.cpp src/provider/stream_relay.cpp src/provider/surplus.cpp src/provider/surplus_claude_chat_request.cpp
// src/provider/surplus_claude_request.cpp src/quota_projection.cpp src/recent_model_context.cpp src/release.cpp src/request.cpp src/request_policy.cpp src/responses_attempts.cpp src/responses_buffered.cpp src/responses_failover_stream.cpp
// src/responses_handler.cpp src/responses_native_headers.cpp src/responses_request.cpp src/responses_stream.cpp src/runtime/http.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
// src/runtime/http_upgrade.cpp src/runtime/websocket.cpp src/runtime_config.cpp src/sentinel/incident_outbox.cpp src/sentinel/replay_admin.cpp src/sentinel/replay_capture.cpp src/sentinel/replay_envelope.cpp src/sentinel/replay_limits.cpp
// src/sentinel/replay_model.cpp src/sentinel/replay_observation.cpp src/sentinel/replay_read.cpp src/sentinel/replay_retention.cpp src/sentinel/replay_retention_bootstrap.cpp src/sentinel/replay_retention_metadata.cpp
// src/sentinel/replay_retention_schema.cpp src/sentinel/replay_store.cpp src/sentinel/upstream_capture.cpp src/static.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp
// src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/systemone/handlers.cpp src/types.cpp src/upstream_wire.cpp src/utils.cpp
#include "uos/admin/api_keys.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/admin/gemini_quota.hpp"
#include "uos/config.hpp"
#include "uos/handler/index.hpp"
#include "uos/kv.hpp"
#include "uos/input_normalization.hpp"
#include "uos/provider/gemini.hpp"
#include "uos/provider/health.hpp"
#include "uos/provider/presentation.hpp"
#include "uos/provider/selection.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/limits.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/test_async.hpp"

#include <cerrno>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <sys/wait.h>
#include <unistd.h>
#include <variant>

namespace rt = uos::rt;
namespace json = uos::json;
namespace gemini = uos::provider::gemini;
namespace quota = uos::admin::gemini_quota;
namespace api_key_policy = uos::api_key_policy;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
#define REAL_GEMINI_TEST_IMPL(name, id) \
  static rt::Task<void> UOS_TEST_JOIN(geminiReal, id)(); \
  TEST(name) { rt::Loop loop; rt::blockOn(loop, UOS_TEST_JOIN(geminiReal, id)()); } \
  static rt::Task<void> UOS_TEST_JOIN(geminiReal, id)()
#define REAL_GEMINI_TEST(name) REAL_GEMINI_TEST_IMPL(name, __COUNTER__)

namespace {
  json::object usageResult() {
    return json::object{{"conversation_id", "80d7987f-f2e8-4ecf-a410-1d34184732a7"}, {"status", "SUCCESS"}, {"response", "GEMINI_OK"},
      {"usage", json::object{{"input_tokens", 8692}, {"output_tokens", 7}, {"thinking_tokens", 0}, {"cache_read_tokens", 0}, {"total_tokens", 8699}}}};
  }

  gemini::GeminiCliRunner turnRunner(bool includeUsage = true) {
    return [includeUsage](const gemini::GeminiCliOptions& command) {
      const bool usage = command.prompt == "/usage";
      EXPECT_EQ(command.mode, usage ? std::string() : std::string("plan"));
      EXPECT_EQ(command.disableSlashCommands, !usage);
      EXPECT_EQ(command.streamJson, !usage);
      gemini::GeminiCliTurn turn;
      turn.response = "GEMINI_OK";
      turn.conversationId = "80d7987f-f2e8-4ecf-a410-1d34184732a7";
      if (includeUsage) turn.usage = json::object{{"input_tokens", 8692}, {"output_tokens", 7}, {"cache_read_tokens", 0}, {"total_tokens", 8699}};
      turn.rawResult = usageResult();
      if (!usage) {
        if (command.onDispatch) command.onDispatch();
        if (command.onInit) command.onInit();
      }
      return rt::Promise<gemini::GeminiCliTurn>::resolve(std::move(turn)).wait();
    };
  }

  json::value usageCommand() {
    return rt::jsonParse(R"({"command":{"name":"usage","data":{"groups":[{"name":"Gemini Models","buckets":[{"id":"gemini-weekly","remaining_fraction":0.986355185508728,"reset_time":"2026-11-25T19:04:49Z"}]},{"name":"Claude and GPT models","buckets":[{"id":"3p-weekly","remaining_fraction":1,"reset_time":"2026-11-25T19:38:03Z"}]}]}}})");
  }

  json::object readTerminalLog(const rt::console::Capture& logs) {
    json::object record;
    std::size_t count = 0;
    for (const auto& call : logs.calls()) {
      if (call.args.size() != 2 || !(call.args.front() == "[ai.ubq.fi] request_terminal")) continue;
      record = rt::jsonParse(call.args.back().string()).as_object();
      ++count;
    }
    EXPECT_EQ(count, std::size_t(1));
    return record;
  }

  struct RealCliFixture {
    std::string originalDirectory = rt::process::cwd();
    std::string root = rt::fs::mkdtempSync("/tmp/gemini-process-");
    std::string home = root + "/home";
    std::string pidPath = home + "/child-pid";
    rt::env::ScopedEnvironment environment{{"HOME", home}, {"UOS_GATEWAY_ENABLE_ANTIGRAVITY", "1"}};

    explicit RealCliFixture(std::string script) {
      const std::string bin = home + "/.local/bin";
      const std::string state = home + "/.gemini/antigravity-cli";
      for (const auto& dir : {bin, state, root + "/.data"}) rt::fs::mkdirSync(dir, {.recursive = true, .mode = 0700});
      rt::fs::writeFileSync(state + "/antigravity-oauth-token", "", {.flag = "wx", .mode = 0600});
      rt::fs::writeFileSync(bin + "/agy", "#!/bin/sh\nset -eu\nprintf '%s\\n' \"$$\" > \"$HOME/child-pid\"\n" + script, {.flag = "wx", .mode = 0700});
      ASSERT(::chdir(root.c_str()) == 0);
    }

    ~RealCliFixture() {
      if (::chdir(originalDirectory.c_str()) != 0) std::abort();
      rt::fs::rmSync(root, {.recursive = true, .force = true});
    }

    void expectReaped() const {
      const int pid = std::stoi(rt::fs::readFileSync(pidPath, "utf8"));
      int status = 0;
      const int waited = ::waitpid(pid, &status, WNOHANG);
      const int error = errno;
      EXPECT_EQ(waited, -1);
      EXPECT_EQ(error, ECHILD);
    }
  };
}

TEST("Gemini model ids and subscription token accounting are explicit") {
  EXPECT_EQ(gemini::geminiUpstreamModelFor("gemini-3.8-flash-low").value_or(""), "gemini-3.8-flash-low");
  EXPECT_EQ(gemini::geminiUpstreamModelFor("google/gemini-3.8-flash-low").has_value(), false);
  const auto usage = gemini::geminiUsageTokens(json::object{{"input_tokens", 100}, {"output_tokens", 5}, {"cache_read_tokens", 80}, {"total_tokens", 105}});
  ASSERT(usage.has_value());
  EXPECT_EQ(usage->inputTokens, std::optional<double>(100));
  EXPECT_EQ(usage->cachedInputTokens, std::optional<double>(80));
  EXPECT_EQ(usage->outputTokens, std::optional<double>(5));
  EXPECT_EQ(usage->totalTokens, std::optional<double>(105));
  EXPECT_EQ(usage->status, "reported");
}

TEST("Gemini is an enabled subscription provider with both OpenAI text wires") {
  EXPECT(uos::provider::selection::isProviderEnabled("gemini", std::nullopt));
  EXPECT(uos::provider::selection::isProviderEnabled("gemini", uos::provider::selection::ProviderSelection{.provider_ids = {"gemini"}, .updated_at_ms = 1}));
  const auto& presentation = *uos::provider::presentation::PROVIDER_PRESENTATION.get("gemini");
  EXPECT_EQ(presentation.label, "Gemini");
  EXPECT_EQ(presentation.tier, "subscription");
  EXPECT_EQ(presentation.health_key, "gemini");
  EXPECT_EQ(presentation.endpoints, (std::vector<std::string>{"/v1/responses", "/v1/chat/completions"}));
}

TEST("Gemini CLI detection is disabled until the operator opts in") {
  const std::string root = rt::fs::mkdtempSync("/tmp/gemini-opt-in-");
  const std::string home = rt::path::join(root, "home");
  const std::string bin = rt::path::join(home, ".local/bin");
  const std::string state = rt::path::join(home, ".gemini/antigravity-cli");
  rt::fs::mkdirSync(bin, {.recursive = true, .mode = 0700});
  rt::fs::mkdirSync(state, {.recursive = true, .mode = 0700});
  auto cleanup = rt::scopeExit([root] { rt::fs::rmSync(root, {.recursive = true, .force = true}); });
  rt::fs::writeFileSync(rt::path::join(bin, "agy"), "fixture", {.flag = "wx", .mode = 0700});
  rt::fs::writeFileSync(rt::path::join(state, "antigravity-oauth-token"), "fixture", {.flag = "wx", .mode = 0600});
  rt::env::ScopedEnvironment environment{{"HOME", home}};
  EXPECT(!gemini::geminiCliConfigured());
  {
    rt::env::ScopedOverride optIn{{"UOS_GATEWAY_ENABLE_ANTIGRAVITY", "1"}};
    EXPECT(gemini::geminiCliConfigured());
  }
  EXPECT(!gemini::geminiCliConfigured());
}

ASYNC_TEST("Gemini CLI /usage projects the exact refreshable subscription buckets") {
  const double observed = 1795031902000;
  const auto snapshot = quota::projectUsageCommand(usageCommand(), observed);
  ASSERT(snapshot.has_value());
  EXPECT_EQ(snapshot->status, "observed");
  EXPECT_EQ(snapshot->observed_at_ms, observed);
  EXPECT_EQ(snapshot->plan_tier, std::optional<std::string>("Antigravity subscription"));
  ASSERT(snapshot->quota.get("gemini-weekly"));
  EXPECT_EQ(snapshot->quota.get("gemini-weekly")->remaining_fraction, std::optional<double>(0.986355185508728));
  EXPECT_EQ(snapshot->quota.get("gemini-weekly")->reset_time, std::optional<std::string>("2026-11-25T19:04:49.000Z"));
  ASSERT(snapshot->quota.get("3p-weekly"));
  EXPECT_EQ(snapshot->quota.get("3p-weekly")->remaining_fraction, std::optional<double>(1));
  co_return;
}

ASYNC_TEST("Gemini quota refresh writes a private snapshot through the CLI usage command") {
  const std::string root = rt::fs::mkdtempSync("/tmp/gemini-quota-");
  const std::string data = rt::path::join(root, ".data");
  EXPECT_EQ(rt::path::isAbsolute(data), true);
  EXPECT_EQ(std::filesystem::path(data).filename(), std::filesystem::path(".data"));
  rt::fs::mkdirSync(data, {.mode = 0700});
  auto cleanup = rt::scopeExit([root] { rt::fs::rmSync(root, {.recursive = true, .force = true}); });
  quota::GeminiQuotaOptions options;
  options.runner = [](const gemini::GeminiCliOptions& command) -> rt::Task<gemini::GeminiCliTurn> {
    EXPECT_EQ(command.prompt, "/usage");
    EXPECT_EQ(command.disableSlashCommands, false);
    EXPECT_EQ(command.mode, "");
    gemini::GeminiCliTurn turn;
    turn.rawResult = usageCommand().as_object();
    co_return turn;
  };
  const double now = 1795031902000;
  AWAIT(snapshot, quota::refreshSnapshot(data, options, now));
  EXPECT_EQ(snapshot.status, "observed");
  AWAIT(loaded, quota::readSnapshot(data, now + 1000));
  EXPECT_EQ(loaded.observed_at_ms, now);
  EXPECT_EQ(loaded.quota.size(), std::size_t(2));
  const auto info = rt::fs::lstatSync(rt::path::join(data, std::string(quota::GEMINI_QUOTA_FILE)));
  EXPECT_EQ(info.mode & 07777, 0600u);
  const auto stored = rt::jsonParse(rt::fs::readFileSync(rt::path::join(data, std::string(quota::GEMINI_QUOTA_FILE)), "utf8"));
  ASSERT(stored.is_object());
  EXPECT(rt::hasOwn(stored.as_object(), "plan_tier"));
  EXPECT(rt::hasOwn(stored.as_object(), "quota"));
  EXPECT(!rt::hasOwn(stored.as_object(), "refresh_supported"));
  EXPECT(!rt::hasOwn(stored.as_object(), "age_ms"));
  EXPECT(!rt::hasOwn(stored.as_object(), "stale"));
  co_return;
}

REAL_GEMINI_TEST("Gemini CLI receives only the minimal environment required for the owner CLI") {
  const std::string originalDirectory = rt::process::cwd();
  const std::string root = rt::fs::mkdtempSync("/tmp/gemini-cli-env-");
  const std::string home = rt::path::join(root, "home");
  const std::string bin = rt::path::join(home, ".local/bin");
  const std::string data = rt::path::join(root, ".data");
  rt::fs::mkdirSync(bin, {.recursive = true, .mode = 0700});
  rt::fs::mkdirSync(data, {.mode = 0700});
  const std::string executable = rt::path::join(bin, "agy");
  rt::fs::writeFileSync(executable,
    "#!/bin/sh\nset -eu\nenv > \"$HOME/captured-env\"\nprintf '%s\\n' '{\"conversation_id\":\"env-capture\",\"status\":\"SUCCESS\",\"response\":\"OK\","
    "\"usage\":{\"input_tokens\":1,\"output_tokens\":1,\"thinking_tokens\":0,\"cache_read_tokens\":0,\"total_tokens\":2}}'\n",
    {.flag = "wx", .mode = 0700});
  auto cleanup = rt::scopeExit([root] { rt::fs::rmSync(root, {.recursive = true, .force = true}); });
  auto restoreDirectory = rt::scopeExit([originalDirectory] { EXPECT_EQ(::chdir(originalDirectory.c_str()), 0); });
  ASSERT(::chdir(root.c_str()) == 0);
  gemini::setGeminiCliRunnerForTest({});
  auto restoreRunner = rt::scopeExit([] { gemini::setGeminiCliRunnerForTest({}); });
  rt::env::ScopedEnvironment environment{
    {"HOME", home}, {"PATH", "/usr/local/bin:/usr/bin:/bin"}, {"LANG", "C.UTF-8"}, {"UOS_GATEWAY_ENABLE_ANTIGRAVITY", "1"},
    {"UOS_AI_TOKEN", "must-not-leak"}, {"DEEPSEEK_API_KEY", "must-not-leak"}, {"METERED_API_KEY", "must-not-leak"}};
  const auto clearParentEnvironment = rt::scopeExit([] {
    ::unsetenv("UOS_AI_TOKEN");
    ::unsetenv("DEEPSEEK_API_KEY");
    ::unsetenv("METERED_API_KEY");
  });
  ASSERT_EQ(::setenv("UOS_AI_TOKEN", "synthetic-env-marker", 1), 0);
  ASSERT_EQ(::setenv("DEEPSEEK_API_KEY", "synthetic-env-marker", 1), 0);
  ASSERT_EQ(::setenv("METERED_API_KEY", "synthetic-env-marker", 1), 0);
  gemini::GeminiCliOptions options{.model = "gemini-3.8-flash-low", .prompt = "capture environment", .mode = "plan", .disableSlashCommands = true, .streamJson = false};
  AWAIT(turn, gemini::runGeminiCli(options));
  EXPECT_EQ(turn.response, "OK");
  const std::string captured = rt::fs::readFileSync(rt::path::join(home, "captured-env"), "utf8");
  EXPECT(captured.find("HOME=" + home + "\n") != std::string::npos);
  EXPECT(captured.find("PATH=/usr/local/bin:/usr/bin:/bin\n") != std::string::npos);
#if defined(__APPLE__)
  // Darwin's getcwd resolves the /tmp symlink before the CLI workspace is created.
  const auto expectedData = rt::fs::realpathSync(data);
#else
  const auto& expectedData = data;
#endif
  EXPECT(captured.find("TMPDIR=" + expectedData + "/antigravity-run-") != std::string::npos);
  EXPECT(captured.find("LANG=C.UTF-8\n") != std::string::npos);
  EXPECT(captured.find("UOS_AI_TOKEN") == std::string::npos);
  EXPECT(captured.find("DEEPSEEK_API_KEY") == std::string::npos);
  EXPECT(captured.find("METERED_API_KEY") == std::string::npos);
  EXPECT(captured.find("UOS_GATEWAY_ENABLE_ANTIGRAVITY") == std::string::npos);
  EXPECT(captured.find("synthetic-env-marker") == std::string::npos);
  co_return;
}

REAL_GEMINI_TEST("Gemini CLI abort kills the process and returns through the request signal") {
  const RealCliFixture fixture("trap '' TERM\nexec /bin/sleep 30\n");
  const rt::AbortController controller;
  const auto timer = rt::setTimeout([controller] { controller.abort(); }, 200);
  const auto clear = rt::scopeExit([&] { rt::clearTimeout(timer); });
  rt::RequestInit init{.method = "POST", .signal = controller.signal()};
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  const gemini::GeminiHandlerOptions options{.refreshQuota = false};
  const json::object raw{{"model", "gemini-3.8-flash-low"}, {"input", "wait"}};
  const double started = rt::performanceNow();
  AWAIT(response, gemini::handleGeminiResponses(request, raw, "gemini-3.8-flash-low", nullptr, options));
  EXPECT_EQ(response.status(), 499);
  EXPECT(rt::performanceNow() - started < 2'000);
  fixture.expectReaped();
  co_return;
}

REAL_GEMINI_TEST("Gemini CLI deadline kills and reaps a child that ignores SIGTERM") {
  const RealCliFixture fixture("trap '' TERM\nexec /bin/sleep 30\n");
  const gemini::GeminiCliOptions options{.model = "gemini-3.8-flash-low", .prompt = "deadline", .timeoutMs = 200};
  auto task = gemini::runGeminiCli(options);
  auto check = rt::test::expectRejects<rt::DOMException>(std::move(task), "Gemini CLI", "TimeoutError",
    "Antigravity CLI exceeded the gateway deadline.", __FILE__, __LINE__);
  co_await std::move(check);
  fixture.expectReaped();
}

REAL_GEMINI_TEST("Gemini drains both CLI pipes before finishing a stdin write") {
  const RealCliFixture fixture("head -c 98304 /dev/zero | tr '\\000' ' '\nhead -c 98304 /dev/zero >&2\ncat >/dev/null\n"
    "printf '\\n%s\\n' '{\"status\":\"SUCCESS\",\"response\":\"input-drained\"}'\n");
  const gemini::GeminiCliOptions options{.model = "gemini-3.8-flash-low", .prompt = std::string(1024 * 1024, 'x'), .timeoutMs = 3'000};
  AWAIT(turn, gemini::runGeminiCli(options));
  EXPECT_EQ(turn.response, "input-drained");
  fixture.expectReaped();
}

REAL_GEMINI_TEST("Gemini reaps the child on stderr, stdout event and stdout aggregate limits") {
  const std::vector<std::pair<std::string, std::string>> cases{
    {"head -c 131073 /dev/zero >&2", "Antigravity stderr exceeded its gateway limit."},
    {"head -c 4194305 /dev/zero", "Antigravity emitted an oversized stream-json event."},
    {"yes ' ' | head -c " + std::to_string(rt::limits::kJsonDocumentBytes + 1), "Antigravity stdout exceeded its gateway limit."},
  };
  for (const auto& fixtureCase : cases) {
    const RealCliFixture fixture("trap '' TERM\n" + fixtureCase.first + "\nexec /bin/sleep 30\n");
    const gemini::GeminiCliOptions options{.model = "gemini-3.8-flash-low", .prompt = std::string(1024 * 1024, 'x'), .timeoutMs = 6'000};
    auto task = gemini::runGeminiCli(options);
    auto check = rt::test::expectRejects<rt::JsError>(std::move(task), "Gemini CLI", "limit error", fixtureCase.second.c_str(), __FILE__, __LINE__);
    co_await std::move(check);
    fixture.expectReaped();
  }
}

REAL_GEMINI_TEST("Gemini CLI failures do not expose raw stderr to API clients") {
  const std::string originalDirectory = rt::process::cwd();
  const std::string root = rt::fs::mkdtempSync("/tmp/gemini-cli-redact-");
  const std::string home = rt::path::join(root, "home");
  const std::string bin = rt::path::join(home, ".local/bin");
  const std::string state = rt::path::join(home, ".gemini/antigravity-cli");
  rt::fs::mkdirSync(bin, {.recursive = true, .mode = 0700});
  rt::fs::mkdirSync(state, {.recursive = true, .mode = 0700});
  rt::fs::mkdirSync(rt::path::join(root, ".data"), {.mode = 0700});
  rt::fs::writeFileSync(rt::path::join(bin, "agy"), "#!/bin/sh\nprintf '%s\\n' TOP_SECRET_CLI_DETAIL >&2\nexit 7\n", {.flag = "wx", .mode = 0700});
  rt::fs::writeFileSync(rt::path::join(state, "antigravity-oauth-token"), "fixture", {.flag = "wx", .mode = 0600});
  auto cleanup = rt::scopeExit([root] { rt::fs::rmSync(root, {.recursive = true, .force = true}); });
  auto restoreDirectory = rt::scopeExit([originalDirectory] { EXPECT_EQ(::chdir(originalDirectory.c_str()), 0); });
  ASSERT(::chdir(root.c_str()) == 0);
  rt::env::ScopedEnvironment environment{{"HOME", home}, {"PATH", "/usr/bin:/bin"}, {"UOS_GATEWAY_ENABLE_ANTIGRAVITY", "1"}};
  gemini::setGeminiCliRunnerForTest({});
  auto restoreRunner = rt::scopeExit([] { gemini::setGeminiCliRunnerForTest({}); });
  rt::RequestInit init;
  init.method = "POST";
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  gemini::GeminiHandlerOptions options{.refreshQuota = false};
  const json::object raw{{"model", "gemini-3.8-flash-low"}, {"input", "Reply exactly GEMINI_OK."}, {"stream", false}};
  rt::console::Capture logs({rt::console::Method::error});
  AWAIT(response, gemini::handleGeminiResponses(request, raw, "gemini-3.8-flash-low", nullptr, options));
  EXPECT_EQ(response.status(), 502);
  AWAIT(body, response.text());
  EXPECT(body.find("TOP_SECRET_CLI_DETAIL") == std::string::npos);
  EXPECT(body.find("gemini_cli_failed") != std::string::npos);
  EXPECT(std::any_of(logs.calls().begin(), logs.calls().end(), [](const auto& call) {
    return call.text.find("Antigravity CLI exited without a result") != std::string::npos;
  }));
  for (const auto& call : logs.calls()) EXPECT(call.text.find("TOP_SECRET_CLI_DETAIL") == std::string::npos);
  co_return;
}

ASYNC_TEST("Gemini explicit quota refresh bypasses a fresh snapshot but normal reads remain coalesced") {
  const std::string root = rt::fs::mkdtempSync("/tmp/gemini-quota-force-");
  const std::string data = rt::path::join(root, ".data");
  rt::fs::mkdirSync(data, {.mode = 0700});
  auto cleanup = rt::scopeExit([root] { rt::fs::rmSync(root, {.recursive = true, .force = true}); });
  int calls = 0;
  quota::GeminiQuotaOptions options;
  options.runner = [&calls](const gemini::GeminiCliOptions&) {
    ++calls;
    gemini::GeminiCliTurn turn;
    turn.rawResult = usageCommand().as_object();
    return rt::Promise<gemini::GeminiCliTurn>::resolve(std::move(turn)).wait();
  };
  const double now = 1795031902000;
  AWAIT(first, quota::refreshSnapshot(data, options, now));
  AWAIT(cached, quota::refreshSnapshot(data, options, now + 1000));
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(cached.observed_at_ms, first.observed_at_ms);
  options.force_refresh = true;
  AWAIT(forced, quota::refreshSnapshot(data, options, now + 2000));
  EXPECT_EQ(calls, 2);
  EXPECT_EQ(forced.observed_at_ms, now + 2000);
  const auto publicSnapshot = quota::snapshotJson(forced, now + 2000);
  EXPECT_EQ(rt::getBool(publicSnapshot, "refresh_supported"), std::optional<bool>(true));
  co_return;
}

ASYNC_TEST("Gemini admin quota controls fail closed without the operator opt-in") {
  rt::env::ScopedOverride optIn{{"UOS_GATEWAY_ENABLE_ANTIGRAVITY", std::nullopt}};
  AWAIT(read, quota::handleAdminGeminiQuota());
  EXPECT_EQ(read.status(), 404);
  AWAIT(readBody, read.json());
  EXPECT_EQ(rt::getString(*rt::get(readBody, "error"), "code"), "gemini_disabled");
  rt::RequestInit init;
  init.method = "POST";
  AWAIT(refresh, quota::handleAdminGeminiQuotaRefresh(rt::Request("https://ai.ubq.fi/admin/providers/gemini/quota/refresh", init)));
  EXPECT_EQ(refresh.status(), 404);
  co_return;
}

ASYNC_TEST("Gemini admin quota controls expose the measured snapshot after operator opt-in") {
  const std::string originalDirectory = rt::process::cwd();
  const std::string root = rt::fs::mkdtempSync("/tmp/gemini-admin-");
  const std::string home = rt::path::join(root, "home");
  const std::string bin = rt::path::join(home, ".local/bin");
  const std::string state = rt::path::join(home, ".gemini/antigravity-cli");
  const std::string data = rt::path::join(root, ".data");
  rt::fs::mkdirSync(bin, {.recursive = true, .mode = 0700});
  rt::fs::mkdirSync(state, {.recursive = true, .mode = 0700});
  rt::fs::mkdirSync(data, {.mode = 0700});
  rt::fs::writeFileSync(rt::path::join(bin, "agy"), "fixture", {.flag = "wx", .mode = 0700});
  rt::fs::writeFileSync(rt::path::join(state, "antigravity-oauth-token"), "fixture", {.flag = "wx", .mode = 0600});
  auto cleanup = rt::scopeExit([root] { rt::fs::rmSync(root, {.recursive = true, .force = true}); });
  auto restoreDirectory = rt::scopeExit([originalDirectory] { EXPECT_EQ(::chdir(originalDirectory.c_str()), 0); });
  ASSERT(::chdir(root.c_str()) == 0);
  rt::env::ScopedEnvironment environment{{"HOME", home}, {"UOS_GATEWAY_ENABLE_ANTIGRAVITY", "1"}};
  quota::GeminiQuotaOptions options;
  options.runner = [](const gemini::GeminiCliOptions&) -> rt::Task<gemini::GeminiCliTurn> {
    gemini::GeminiCliTurn turn;
    turn.rawResult = usageCommand().as_object();
    co_return turn;
  };
  const double now = rt::dateNow();
  AWAIT(stored, quota::refreshSnapshot(data, options, now));
  EXPECT_EQ(stored.status, "observed");
  AWAIT(response, quota::handleAdminGeminiQuota());
  EXPECT_EQ(response.status(), 200);
  AWAIT(body, response.json());
  EXPECT_EQ(rt::getBool(body, "refresh_supported"), std::optional<bool>(true));
  EXPECT_EQ(rt::getNumber(body, "observed_at_ms"), std::optional<double>(now));
  co_return;
}

ASYNC_TEST("Gemini rejects caller tool declarations before dispatching the subscription CLI") {
  rt::RequestInit init;
  init.method = "POST";
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  bool dispatched = false;
  gemini::GeminiHandlerOptions options{.refreshQuota = false};
  options.runner = [&dispatched](const gemini::GeminiCliOptions&) {
    dispatched = true;
    return rt::Promise<gemini::GeminiCliTurn>::resolve(gemini::GeminiCliTurn{}).wait();
  };
  const json::object raw{{"model", "gemini-3.8-flash-low"}, {"input", "Use the supplied tool."},
    {"tools", json::array{json::object{{"type", "function"}, {"name", "fixture"}}}}};
  AWAIT(response, gemini::handleGeminiResponses(request, raw, "gemini-3.8-flash-low", nullptr, options));
  EXPECT_EQ(response.status(), 400);
  EXPECT_EQ(dispatched, false);
  AWAIT(body, response.json());
  const auto* error = rt::getObject(body, "error");
  ASSERT(error);
  EXPECT_EQ(rt::getString(*error, "code"), std::optional<std::string_view>("gemini_tools_unsupported"));
  co_return;
}

ASYNC_TEST("authenticated Gemini Responses route returns the provider answer and measured usage") {
  rt::RequestInit init;
  init.method = "POST";
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  std::optional<uos::openai_telemetry::UsageTokens> terminal;
  uos::openai_telemetry::UsageContext context;
  context.responseTelemetry = uos::openai_telemetry::createResponseTelemetryState();
  context.onTerminalUsage = [&terminal](std::optional<uos::openai_telemetry::UsageTokens> usage, bool) { terminal = std::move(usage); };
  gemini::GeminiHandlerOptions options{.runner = turnRunner(), .refreshQuota = false};
  const json::object raw{{"model", "gemini-3.8-flash-low"}, {"input", "Reply exactly GEMINI_OK."}};
  AWAIT(response, gemini::handleGeminiResponses(request, raw, "gemini-3.8-flash-low", &context, options));
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(response.headers().get("x-uos-upstream"), std::optional<std::string>("gemini"));
  AWAIT(body, response.json());
  EXPECT_EQ(rt::getString(body, "model").value_or(""), "gemini-3.8-flash-low");
  EXPECT_EQ(rt::getString(body, "status").value_or(""), "completed");
  ASSERT(terminal.has_value());
  EXPECT_EQ(terminal->inputTokens, std::optional<double>(8692));
  EXPECT_EQ(terminal->outputTokens, std::optional<double>(7));
  EXPECT_EQ(context.responseTelemetry->provider, std::optional<std::string>("gemini"));
  EXPECT_EQ(context.responseTelemetry->providerRequestId, std::optional<std::string>("80d7987f-f2e8-4ecf-a410-1d34184732a7"));
  co_return;
}

ASYNC_TEST("Gemini Responses streams when the CLI omits usage") {
  rt::RequestInit init;
  init.method = "POST";
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  gemini::GeminiHandlerOptions options{.runner = turnRunner(false), .refreshQuota = false};
  const json::object raw{{"model", "gemini-3.8-flash-low"}, {"input", "Reply exactly GEMINI_OK."}, {"stream", true}};
  AWAIT(response, gemini::handleGeminiResponses(request, raw, "gemini-3.8-flash-low", nullptr, options));
  EXPECT_EQ(response.status(), 200);
  AWAIT(body, response.text());
  EXPECT(body.find("GEMINI_OK") != std::string::npos);
  EXPECT(body.find("\"usage\":null") != std::string::npos);
  co_return;
}

ASYNC_TEST("Gemini maps API-key quota failures without collapsing them to 502") {
  rt::RequestInit init;
  init.method = "POST";
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  uos::openai_telemetry::UsageContext context;
  context.beforeProviderDispatch = [](api_key_policy::ApiKeyUsageProvider) -> rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>> {
    co_await rt::microtask();
    throw api_key_policy::ApiKeyQuotaDispatchError("API key quota changed concurrently", api_key_policy::ApiKeyQuotaDispatchErrorOptions{
      .status = 409, .code = "api_key_quota_conflict", .errorType = "invalid_request_error"});
    co_return std::nullopt;
  };
  gemini::GeminiHandlerOptions options{.runner = turnRunner(), .refreshQuota = false};
  const json::object raw{{"model", "gemini-3.8-flash-low"}, {"input", "Reply exactly GEMINI_OK."}, {"stream", false}};
  AWAIT(response, gemini::handleGeminiResponses(request, raw, "gemini-3.8-flash-low", &context, options));
  EXPECT_EQ(response.status(), 409);
  AWAIT(body, response.json());
  EXPECT_EQ(rt::getString(*rt::get(body, "error"), "code"), "api_key_quota_conflict");
  co_return;
}

ASYNC_TEST("Gemini cancels admission when the CLI fails before transport") {
  rt::RequestInit init;
  init.method = "POST";
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  int cancelled = 0;
  uos::openai_telemetry::UsageContext context;
  context.beforeProviderDispatch = [&cancelled](api_key_policy::ApiKeyUsageProvider) {
    api_key_policy::ApiKeyProviderDispatch dispatch;
    dispatch.markTransportStarted = [] {};
    dispatch.cancelBeforeTransport = [&cancelled] {
      ++cancelled;
      return rt::Promise<void>::resolve().wait();
    };
    return rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>>::resolve(std::move(dispatch)).wait();
  };
  gemini::GeminiHandlerOptions options{
    .runner = [](const gemini::GeminiCliOptions&) -> rt::Task<gemini::GeminiCliTurn> {
      co_await rt::microtask();
      throw rt::JsError("spawn failed");
      co_return gemini::GeminiCliTurn{};
    },
    .refreshQuota = false,
  };
  const json::object raw{{"model", "gemini-3.8-flash-low"}, {"input", "Reply exactly GEMINI_OK."}, {"stream", false}};
  AWAIT(response, gemini::handleGeminiResponses(request, raw, "gemini-3.8-flash-low", &context, options));
  EXPECT_EQ(response.status(), 502);
  EXPECT_EQ(cancelled, 1);
  co_return;
}

ASYNC_TEST("Gemini maps an already-cancelled request to 499") {
  rt::AbortController controller;
  controller.abort(rt::makeReason(rt::DOMException("client disconnected", "AbortError")));
  rt::RequestInit init;
  init.method = "POST";
  init.signal = controller.signal();
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  int dispatched = 0;
  uos::openai_telemetry::UsageContext context;
  context.beforeProviderDispatch = [&dispatched](api_key_policy::ApiKeyUsageProvider) {
    ++dispatched;
    return rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>>::resolve(api_key_policy::ApiKeyProviderDispatch{}).wait();
  };
  gemini::GeminiHandlerOptions options{.runner = turnRunner(), .refreshQuota = false};
  const json::object raw{{"model", "gemini-3.8-flash-low"}, {"input", "Reply exactly GEMINI_OK."}, {"stream", false}};
  AWAIT(response, gemini::handleGeminiResponses(request, raw, "gemini-3.8-flash-low", &context, options));
  EXPECT_EQ(response.status(), 499);
  EXPECT_EQ(dispatched, 0);
  co_return;
}

ASYNC_TEST("Gemini inference fails closed without the operator opt-in") {
  rt::env::ScopedOverride optIn{{"UOS_GATEWAY_ENABLE_ANTIGRAVITY", std::nullopt}};
  gemini::setGeminiCliRunnerForTest({});
  auto restoreRunner = rt::scopeExit([] { gemini::setGeminiCliRunnerForTest({}); });
  rt::RequestInit init;
  init.method = "POST";
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  gemini::GeminiHandlerOptions options{.refreshQuota = false};
  const json::object raw{{"model", "gemini-3.8-flash-low"}, {"input", "Reply exactly GEMINI_OK."}, {"stream", false}};
  AWAIT(response, gemini::handleGeminiResponses(request, raw, "gemini-3.8-flash-low", nullptr, options));
  EXPECT_EQ(response.status(), 503);
  AWAIT(body, response.json());
  EXPECT_EQ(rt::getString(*rt::get(body, "error"), "code"), "gemini_disabled");
  co_return;
}

ASYNC_TEST("Gemini Chat Completions streams the same provider answer and usage") {
  rt::RequestInit init;
  init.method = "POST";
  const rt::Request request("https://ai.ubq.fi/v1/chat/completions", init);
  gemini::GeminiHandlerOptions options{.runner = turnRunner(), .refreshQuota = false};
  const json::object raw{{"model", "gemini-3.8-flash-low"}, {"messages", json::array{json::object{{"role", "user"}, {"content", "Reply exactly GEMINI_OK."}}}},
    {"stream", true}, {"stream_options", json::object{{"include_usage", true}}}};
  AWAIT(response, gemini::handleGeminiChatCompletions(request, raw, "gemini-3.8-flash-low", nullptr, options));
  EXPECT_EQ(response.status(), 200);
  AWAIT(body, response.text());
  EXPECT(body.find("GEMINI_OK") != std::string::npos);
  EXPECT(body.find("\"usage\":{\"prompt_tokens\":8692") != std::string::npos);
  EXPECT(body.ends_with("data: [DONE]\n\n"));
  co_return;
}

ASYNC_TEST("Gemini Chat Completions can stream when the CLI supplies no token counts") {
  rt::RequestInit init{.method = "POST"};
  const rt::Request request("https://ai.ubq.fi/v1/chat/completions", init);
  const gemini::GeminiHandlerOptions options{.runner = turnRunner(false), .refreshQuota = false};
  const json::object raw{{"model", "gemini-3.8-flash-low"}, {"messages", json::array{json::object{{"role", "user"}, {"content", "Reply exactly GEMINI_OK."}}}},
    {"stream", true}, {"stream_options", json::object{{"include_usage", true}}}};
  AWAIT(response, gemini::handleGeminiChatCompletions(request, raw, "gemini-3.8-flash-low", nullptr, options));
  EXPECT_EQ(response.status(), 200);
  AWAIT(body, response.text());
  EXPECT(body.find("GEMINI_OK") != std::string::npos);
  EXPECT(body.find("\"usage\"") == std::string::npos);
  EXPECT(body.ends_with("data: [DONE]\n\n"));
}

ASYNC_TEST("Gemini prompt limits and missing executables do not record upstream health failures") {
  namespace health = uos::provider::health;
  const RealCliFixture fixture("exit 1\n");
  uos::kv::setKvForTest(std::make_shared<rt::MemoryKv>(rt::dateNow));
  auto restore = rt::scopeExit([] { uos::kv::setKvForTest(nullptr); health::resetProviderHealthThrottleForTest(); });
  health::resetProviderHealthThrottleForTest();
  const auto record = health::recordGeminiProviderHealth("success", 200);
  co_await record.wait();
  const rt::RequestInit init{.method = "POST"};
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  const gemini::GeminiHandlerOptions options{.refreshQuota = false};
  const json::object oversized{{"model", "gemini-3.8-flash-low"}, {"input", std::string(8 * 1024 * 1024, 'x')}};
  AWAIT(refused, gemini::handleGeminiResponses(request, oversized, "gemini-3.8-flash-low", nullptr, options));
  EXPECT_EQ(refused.status(), 413);
  ASSERT(!rt::fs::existsSync(fixture.pidPath));

  rt::fs::rmSync(fixture.home + "/.local/bin/agy");
  const json::object normal{{"model", "gemini-3.8-flash-low"}, {"input", "ordinary prompt"}};
  AWAIT(missing, gemini::handleGeminiResponses(request, normal, "gemini-3.8-flash-low", nullptr, options));
  EXPECT_EQ(missing.status(), 503);
  AWAIT(body, missing.json());
  EXPECT_EQ(rt::getString(*rt::get(body, "error"), "code"), "gemini_cli_unavailable");
  AWAIT(state, health::getGeminiProviderHealth());
  EXPECT_EQ(state.state, "healthy");
  EXPECT(!state.last_error_at_ms);
}

ASYNC_TEST("authenticated gateway request routes to Gemini and records the subscription spend") {
  const rt::env::ScopedOverride optIn{{"UOS_GATEWAY_ENABLE_ANTIGRAVITY", "1"}};
  const auto savedConfig = uos::config::config;
  uos::config::config.authTokens = {"fixture-gateway-key"};
  uos::config::config.adminTokens.clear();
  uos::config::config.codexAuthJsonB64.clear();
  uos::kv::setKvForTest(std::make_shared<rt::MemoryKv>(rt::dateNow));
  gemini::setGeminiCliRunnerForTest(turnRunner());
  auto restore = rt::scopeExit([savedConfig] {
    gemini::setGeminiCliRunnerForTest({});
    uos::kv::setKvForTest(nullptr);
    uos::config::config = savedConfig;
  });
  const json::object body{{"model", "gemini-3.8-flash-low"}, {"input", "Reply exactly GEMINI_OK."}, {"stream", false}};
  rt::RequestInit init;
  init.method = "POST";
  init.headers = rt::Headers{{"Authorization", "Bearer fixture-gateway-key"}, {"Content-Type", "application/json"}};
  init.body = rt::jsonStringify(body);
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  rt::console::Capture logs({rt::console::Method::info});
  AWAIT(response, uos::handler::index::handler(request));
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(response.headers().get("x-uos-upstream"), std::optional<std::string>("gemini"));
  const auto terminal = readTerminalLog(logs);
  EXPECT_EQ(rt::getString(terminal, "provider"), "gemini");
  EXPECT_EQ(rt::getNumber(terminal, "total_tokens"), std::optional<double>(8699));
  EXPECT_EQ(rt::getString(terminal, "stream_terminal_type"), "response.completed");
  co_return;
}

ASYNC_TEST("Gemini API-key request increments the admin usage meter exactly once") {
  const rt::env::ScopedOverride optIn{{"UOS_GATEWAY_ENABLE_ANTIGRAVITY", "1"}};
  const auto savedConfig = uos::config::config;
  uos::config::config.authTokens.clear();
  uos::config::config.adminTokens.clear();
  uos::config::config.codexAuthJsonB64.clear();
  uos::api_key_policy::resetApiKeyPolicyCacheForTest();
  uos::kv::setKvForTest(std::make_shared<rt::MemoryKv>(rt::dateNow));
  gemini::setGeminiCliRunnerForTest(turnRunner());
  auto restore = rt::scopeExit([savedConfig] {
    gemini::setGeminiCliRunnerForTest({});
    uos::kv::setKvForTest(nullptr);
    uos::config::config = savedConfig;
    uos::api_key_policy::resetApiKeyPolicyCacheForTest();
  });

  const std::string token = "u_" + std::string(64, 'a');
  rt::RequestInit createInit;
  createInit.method = "POST";
  createInit.headers = rt::Headers{{"Content-Type", "application/json"}};
  createInit.body = rt::jsonStringify(json::object{{"name", "Gemini usage acceptance"}, {"token", token}, {"usage_limit_requests", 10},
    {"window_ms", 3'600'000}, {"paid_fallback_enabled", false}});
  AWAIT(created, uos::admin::api_keys::handleAdminApiKeysCreate(rt::Request("https://ai.ubq.fi/admin/api-keys", createInit)));
  ASSERT_EQ(created.status(), 200);
  AWAIT(createdBody, created.json());
  const auto keyId = rt::getString(createdBody, "id");
  ASSERT(keyId.has_value());

  rt::RequestInit inferenceInit;
  inferenceInit.method = "POST";
  inferenceInit.headers = rt::Headers{{"Authorization", "Bearer " + token}, {"Content-Type", "application/json"}};
  inferenceInit.body = rt::jsonStringify(json::object{{"model", "gemini-3.8-flash-low"}, {"input", "Reply exactly GEMINI_OK."}, {"stream", false}});
  const rt::AbortController deliveryController;
  const uos::handler::http::RequestDeliveryInfo delivery{rt::Promise<void>::resolve(), deliveryController.signal()};
  AWAIT(inference, uos::handler::index::handler(rt::Request("https://ai.ubq.fi/v1/responses", inferenceInit), delivery));
  ASSERT_EQ(inference.status(), 200);
  AWAIT(inferenceBody, inference.text());
  EXPECT(inferenceBody.find("GEMINI_OK") != std::string::npos);

  AWAIT(listed, uos::admin::api_keys::handleAdminApiKeysList(rt::Request("https://ai.ubq.fi/admin/api-keys?include_usage=true")));
  ASSERT_EQ(listed.status(), 200);
  AWAIT(listedBody, listed.json());
  const auto* rows = rt::getArray(listedBody, "data");
  ASSERT(rows);
  std::optional<double> usageRequests;
  for (const auto& row : *rows) {
    if (rt::getString(row, "id") == keyId) usageRequests = rt::getNumber(rt::get(row, "usage_requests"));
  }
  ASSERT(usageRequests.has_value());
  EXPECT_EQ(*usageRequests, 1.0);
  co_return;
}

REAL_GEMINI_TEST("manual live Antigravity acceptance uses an authenticated gateway request and proves quota spend") {
  if (rt::env::get("GEMINI_LIVE_ACCEPTANCE") != std::optional<std::string>("1")) co_return;
  const std::string originalDirectory = rt::process::cwd();
  const std::string root = rt::fs::mkdtempSync("/tmp/gemini-live-");
  const std::string data = rt::path::join(root, ".data");
  rt::fs::mkdirSync(data, {.mode = 0700});
  const auto savedConfig = uos::config::config;
  uos::kv::setKvForTest(std::make_shared<rt::MemoryKv>(rt::dateNow));
  gemini::setGeminiCliRunnerForTest({});
  auto restore = rt::scopeExit([originalDirectory, root, savedConfig] {
    gemini::setGeminiCliRunnerForTest({});
    uos::kv::setKvForTest(nullptr);
    uos::config::config = savedConfig;
    if (::chdir(originalDirectory.c_str()) != 0) std::abort();
    rt::fs::rmSync(root, {.recursive = true, .force = true});
  });
  ASSERT(::chdir(root.c_str()) == 0);
  quota::GeminiQuotaOptions quotaOptions;
  AWAIT(before, quota::refreshSnapshot(data, quotaOptions, rt::dateNow()));
  ASSERT(before.quota.get("gemini-weekly"));
  const auto beforeFraction = *before.quota.get("gemini-weekly")->remaining_fraction;
  uos::config::config.authTokens = {"fixture-live-gateway-key"};
  uos::config::config.adminTokens.clear();
  uos::config::config.codexAuthJsonB64.clear();
  const json::object body{{"model", "gemini-3.8-flash-low"}, {"input", "Reply exactly GATEWAY_CANDIDATE_OK and do not use tools."}, {"stream", false}};
  rt::RequestInit init;
  init.method = "POST";
  init.headers = rt::Headers{{"Authorization", "Bearer fixture-live-gateway-key"}, {"Content-Type", "application/json"}};
  init.body = rt::jsonStringify(body);
  const rt::Request request("https://ai.ubq.fi/v1/responses", init);
  rt::console::Capture logs({rt::console::Method::info});
  AWAIT(response, uos::handler::index::handler(request));
  EXPECT_EQ(response.status(), 200);
  EXPECT_EQ(response.headers().get("x-uos-upstream"), std::optional<std::string>("gemini"));
  const auto terminal = readTerminalLog(logs);
  EXPECT_EQ(rt::getString(terminal, "provider"), "gemini");
  const auto totalTokens = rt::getNumber(terminal, "total_tokens");
  ASSERT(totalTokens.has_value());
  EXPECT(*totalTokens > 0);
  AWAIT(serialized, response.text());
  EXPECT(serialized.find("GATEWAY_CANDIDATE_OK") != std::string::npos);
  const rt::AbortSignal pause = rt::AbortSignal::timeout(11'000);
  try {
    auto sleeping = rt::sleep(11'000, pause);
    co_await std::move(sleeping);
  } catch (...) {
  }
  AWAIT(after, quota::refreshSnapshot(data, quotaOptions, rt::dateNow()));
  ASSERT(after.quota.get("gemini-weekly"));
  const auto afterFraction = *after.quota.get("gemini-weekly")->remaining_fraction;
  EXPECT(afterFraction < beforeFraction);
  const json::object receipt{{"status", "passed"}, {"request_status", 200}, {"provider", terminal.at("provider")},
    {"provider_request_id", terminal.at("provider_request_id")}, {"input_tokens", terminal.at("input_tokens")},
    {"output_tokens", terminal.at("output_tokens")}, {"total_tokens", terminal.at("total_tokens")},
    {"gemini_weekly_before", beforeFraction}, {"gemini_weekly_after", afterFraction}, {"observed_at_ms", rt::makeNumber(after.observed_at_ms)}};
  rt::fs::writeFileSync("/tmp/gemini-live-acceptance.json", rt::jsonStringify(receipt) + "\n", {.mode = 0600});
  co_return;
}
