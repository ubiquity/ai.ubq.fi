// Port of tests/handler-terminal-usage-rollup.test.ts: all five source cases.
// Links: tests/handler_terminal_usage_rollup_test.cpp
// src/kv.cpp src/config.cpp src/release.cpp src/openai_telemetry.cpp
// src/cache/prompt_analytics_queue.cpp src/provider/removed_provider_telemetry.cpp src/cache/prompt_analytics_core.cpp src/storage/kv.cpp
// src/handler/terminal_log.cpp src/sentinel/replay_store.cpp src/sentinel/replay_envelope.cpp
// src/storage/sqlite_readonly.cpp src/sentinel/upstream_capture.cpp src/sentinel/replay_observation.cpp src/runtime/http_tasks.cpp
// src/sentinel/incident_outbox.cpp src/paid_fallback/rollups.cpp src/utils.cpp src/sentinel/replay_read.cpp
// src/optional_telemetry_queue.cpp src/sentinel/replay_model.cpp src/authority/native_paid.cpp src/sentinel/replay_retention_schema.cpp
// src/admin/error_log.cpp src/sentinel/replay_retention.cpp src/sentinel/replay_limits.cpp src/handler/http.cpp
// tests/helpers/counting_kv.cpp src/sentinel/replay_retention_bootstrap.cpp src/responses_failover_stream.cpp src/request.cpp
// src/authority/native_paid_contracts.cpp src/responses_stream.cpp src/codex/quota.cpp src/http.cpp
// src/api_key_policy.cpp src/auth/relay.cpp src/storage/native_kv.cpp src/authority/native_api_key.cpp
// src/codex/responses_error.cpp src/responses_native_headers.cpp src/sentinel/replay_retention_metadata.cpp src/cache/telemetry_gate.cpp
// src/provider/cerebras_rate_limits.cpp src/runtime/http_early_sse.cpp src/authority/native_api_key_policy.cpp src/deepseek/context_admission_failure.cpp
// src/api_keys.cpp src/provider/cerebras.cpp src/types.cpp src/authority/native_paid_policy.cpp
// src/bounded_response_body.cpp src/openai.cpp src/openai_schema.cpp src/lib/jev_compaction/codex_items.cpp
// src/lib/jev_compaction/compact.cpp src/models/codex_models.cpp src/defaults.cpp src/inference_deadline.cpp
// src/lib/jev_compaction/state.cpp src/runtime_config.cpp src/request_policy.cpp src/provider/selection.cpp
// src/lib/jev_compaction/request.cpp src/provider/claude_code_models.cpp src/provider/surplus.cpp src/lib/jev_compaction/types.cpp
// src/provider/surplus_claude_request.cpp src/provider/claude_code_credentials.cpp src/provider/metered.cpp src/provider/claude_code_accounts.cpp
// src/provider/anthropic_chat_responses.cpp src/provider/anthropic_responses.cpp src/provider/anthropic_replay.cpp src/provider/surplus_claude_chat_request.cpp
// src/provider/anthropic_response_wire.cpp src/models/codex_models_fetch.cpp src/provider/claude_code.cpp src/codex/auth_refresh.cpp
// src/provider/anthropic_chat_stream.cpp src/codex/dispatch.cpp src/codex/auth.cpp src/models/codex_models_availability.cpp
// src/codex/routing_mutations.cpp src/provider/health.cpp src/provider/claude_code_quota.cpp src/codex/capacity_routing.cpp
// src/codex/routing_state.cpp src/authority/native_codex_capacity.cpp src/provider/capacity_events.cpp src/authority/native_codex_contracts.cpp
// src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/codex/rate_limit_429.cpp src/codex/native_auth.cpp
// src/codex/banked_reset.cpp src/codex/external_auth.cpp src/codex/banked_reset_provider.cpp src/authority/native_codex_429.cpp
// src/codex/reset_settings.cpp src/provider/capacity_contract.cpp src/codex/native_auth_transport.cpp src/provider/capacity.cpp
// src/provider/claude_capacity_rollups.cpp src/provider/capacity_parse.cpp src/cache/prompt_analytics_read.cpp src/provider/capacity_history.cpp
// src/provider/claude_code_usage_schema.cpp src/provider/capacity_rollups.cpp src/metered_quota.cpp src/provider/claude_capacity_history.cpp
// src/runtime/websocket.cpp src/provider/capacity_analytics.cpp src/codex/experiment.cpp src/paid_fallback/ledger_stats.cpp
// src/codex/account_routing.cpp src/codex/routing_serial.cpp src/codex/routing_evaluation.cpp src/authority/native_codex.cpp
// src/codex/overage_settings.cpp
#include "tests/helpers/terminal_log_harness.hpp"
#include "tests/helpers/counting_kv.hpp"
#include "uos/paid_fallback/rollups.hpp"
#include "uos/rt/date.hpp"

using namespace uos;
namespace fixture = tests::helpers::terminal_log_harness;
namespace terminal = handler::terminal_log;
using authority::native_paid_contracts::TerminalUsageRollupInput;

ASYNC_TEST("terminal usage accounting records one observation with the observed request hour and usage") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  std::vector<TerminalUsageRollupInput> observations;
  const double requestStartedAtMs = rt::Date::parse("2026-09-22T16:00:00Z");
  auto options = fixture::terminalOptions("handler-usage-rollup-codex");
  options.requestStartedAtMs = requestStartedAtMs;
  options.recordUsageRollup = [&observations](TerminalUsageRollupInput input) {
    observations.push_back(std::move(input)); return rt::Promise<bool>::resolve(true);
  };
  auto wrapped = terminal::withTerminalRequestLog(fixture::codexResponseWithUsage(), options);
  auto response = co_await wrapped.wait();
  EXPECT_EQ(response.status(), 200);
  auto text = co_await response.text();
  EXPECT_EQ(text, std::string("complete"));
  ASSERT(observations.size() == 1);
  const TerminalUsageRollupInput expected{.model = "gpt-5.6-sol", .provider = "chatgpt_codex", .request_id = "handler-usage-rollup-codex",
    .request_created_at_ms = requestStartedAtMs, .input_tokens = 120, .cached_input_tokens = 40, .output_tokens = 30};
  EXPECT(observations[0] == expected);
}

ASYNC_TEST("terminal usage accounting falls back to the terminal clock when no request start is supplied") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  std::vector<TerminalUsageRollupInput> observations;
  const double before = rt::dateNow();
  auto options = fixture::terminalOptions("handler-usage-rollup-rejection");
  options.recordUsageRollup = [&observations](TerminalUsageRollupInput input) {
    observations.push_back(std::move(input)); return rt::Promise<bool>::resolve(true);
  };
  auto wrapped = terminal::withTerminalRequestLog(fixture::jsonResponse("complete", 200, "chatgpt_codex"), options);
  auto response = co_await wrapped.wait();
  const double after = rt::dateNow();
  EXPECT_EQ(response.status(), 200);
  auto text = co_await response.text();
  EXPECT_EQ(text, std::string("complete"));
  ASSERT(observations.size() == 1);
  ASSERT(observations[0].request_created_at_ms.has_value());
  EXPECT(*observations[0].request_created_at_ms >= before && *observations[0].request_created_at_ms <= after);
}

ASYNC_TEST("a failed terminal usage-rollup write never changes the terminal response") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  int attempts = 0;
  auto options = fixture::terminalOptions("handler-usage-rollup-failure");
  options.recordUsageRollup = [&attempts](TerminalUsageRollupInput) {
    ++attempts; return rt::Promise<bool>::reject(rt::makeReason(rt::JsError("terminal usage rollup KV unavailable")));
  };
  auto wrapped = terminal::withTerminalRequestLog(fixture::jsonResponse("complete", 200, "chatgpt_codex"), options);
  auto response = co_await wrapped.wait();
  EXPECT_EQ(attempts, 1);
  EXPECT_EQ(response.status(), 200);
  auto text = co_await response.text();
  EXPECT_EQ(text, std::string("complete"));
}

ASYNC_TEST("terminal usage accounting performs no KV work for settled, aggregate, route-less or unobserved terminals") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  auto store = std::make_shared<tests::helpers::counting_kv::CountingKv>();
  kv::setKvForTest(store);
  const std::vector<std::optional<std::string>> providers{"metered", "surplus", "mixed", std::nullopt, "chatgpt_codex"};
  for (const auto& provider : providers) {
    auto options = fixture::terminalOptions("handler-usage-rollup-skip-" + provider.value_or("gateway"));
    auto wrapped = terminal::withTerminalRequestLog(fixture::jsonResponse("complete", 200, provider), options);
    auto response = co_await wrapped.wait();
    auto text = co_await response.text();
    EXPECT_EQ(text, std::string("complete"));
  }
  EXPECT_EQ(store->commands.size(), std::size_t{0});
  EXPECT_EQ(store->entries.size(), std::size_t{0});
}

ASYNC_TEST("one observed terminal response writes exactly one rollup merge with actual tokens and no ledger charge") {
  fixture::Isolation isolation;
  rt::console::Capture logs({rt::console::Method::info});
  auto store = std::make_shared<tests::helpers::counting_kv::CountingKv>();
  kv::setKvForTest(store);
  const double requestStartedAtMs = rt::Date::parse("2026-09-22T16:00:00Z");
  auto options = fixture::terminalOptions("handler-usage-rollup-observed");
  options.requestStartedAtMs = requestStartedAtMs;
  auto wrapped = terminal::withTerminalRequestLog(fixture::codexResponseWithUsage(), options);
  auto response = co_await wrapped.wait();
  EXPECT_EQ(response.status(), 200);
  auto text = co_await response.text();
  EXPECT_EQ(text, std::string("complete"));
  int reads = 0;
  int commits = 0;
  for (const auto& command : store->commands) {
    if (command.command == "get") ++reads;
    if (command.command == "atomic.commit" && command.atomicResult == "committed") ++commits;
  }
  EXPECT_EQ(reads, 1);
  EXPECT_EQ(commits, 1);
  const paid_fallback::rollups::ListPaidFallbackUsageRollupsOptions range{.sinceMs = requestStartedAtMs - 1, .nowMs = requestStartedAtMs + 1};
  auto listing = paid_fallback::rollups::listPaidFallbackUsageRollups(store, range);
  const auto rows = co_await std::move(listing);
  ASSERT(rows.size() == 1);
  const auto& row = rows[0];
  EXPECT_EQ(row.provider, std::string("chatgpt_codex"));
  EXPECT_EQ(row.model, std::string("gpt-5.6-sol"));
  EXPECT_EQ(row.request_count, 1.0);
  EXPECT_EQ(row.input_tokens, 120.0);
  EXPECT_EQ(row.cached_input_tokens, 40.0);
  EXPECT_EQ(row.output_tokens, 30.0);
  EXPECT_EQ(row.quota_sum, 0.0);
  EXPECT_EQ(row.spend_microcredits, 0.0);
  EXPECT_EQ(row.bucket_start_at_ms, rt::Math::floor(requestStartedAtMs / 3'600'000) * 3'600'000);
}
