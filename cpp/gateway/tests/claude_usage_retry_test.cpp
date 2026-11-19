// Links: tests/claude_usage_retry_test.cpp
//        src/auth/relay.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp
//        src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_paid.cpp
//        src/authority/native_paid_contracts.cpp src/bounded_response_body.cpp src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp
//        src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp
//        src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp
//        src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp
//        src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp
//        src/codex/reset_settings.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp
//        src/codex/routing_state.cpp src/config.cpp src/http.cpp src/inference_deadline.cpp
//        src/kv.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp src/optional_telemetry_queue.cpp
//        src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
//        src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
//        src/provider/capacity_rollups.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp
//        src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage.cpp
//        src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/selection.cpp src/release.cpp
//        src/runtime/http.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
//        src/runtime/http_upgrade.cpp src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
//        src/types.cpp src/utils.cpp src/provider/claude_code_usage_refresh.cpp
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/claude_code_usage_refresh.hpp"
#include "uos/provider/claude_capacity_history.hpp"
#include "uos/storage/kv.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv_js.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/test_async.hpp"
namespace rt = uos::rt;
namespace json = uos::json;
namespace accounts = uos::provider::claude_code_accounts;
namespace credentials = uos::provider::claude_code_credentials;
namespace quota = uos::provider::claude_code_quota;
namespace refresh = uos::provider::claude_code_usage_refresh;
namespace history = uos::provider::claude_capacity_history;
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  struct Isolated {
    rt::env::ScopedEnvironment environment{{{"HOME", "/nonexistent/w05c7-claude"}, {"TMPDIR", rt::env::get("TMPDIR").value_or("/tmp")}}};
    std::string saved = uos::config::config.codexAuthJsonB64;
    Isolated() { uos::config::config.codexAuthJsonB64.clear(); credentials::resetClaudeCodeCredentialCacheForTest(); }
    ~Isolated() { uos::kv::setKvForTest(nullptr); credentials::resetClaudeCodeCredentialCacheForTest(); uos::config::config.codexAuthJsonB64 = saved; }
  };
  const uos::provider::claude_code::ClaudeCodeEnvLookup nullEnv = [](const std::string&) -> std::optional<std::string> { return std::nullopt; };
  json::value usagePayload() { return rt::jsonParse(R"({"five_hour":{"utilization":42,"resets_at":null},"seven_day":{"utilization":12,"resets_at":null}})"); }
  rt::Task<accounts::ClaudeCodeAccountBinding> binding(std::shared_ptr<rt::Kv> kv) {
    const credentials::StoreClaudeCodeStoredCredentialInput input{.secret = std::string("sk-") + "ant-oat-fixture-w05c7"};
    rt::Promise storing(credentials::storeClaudeCodeStoredCredential(kv, input)); const auto credential = co_await storing.wait();
    if (!credential) throw rt::JsError("fixture credential missing");
    rt::Promise resolving(accounts::resolveClaudeCodeAccount(kv, nullEnv)); const auto account = co_await resolving.wait();
    if (!account) throw rt::JsError("fixture account missing");
    co_return *account;
  }
  std::string kind(const refresh::ClaudeCodeUsageRefresh& result) { return std::visit([](const auto& value) { return std::string(value.kind); }, result); }
  uos::provider::claude_code_usage::FetchClaudeCodeUsageOptions options(double now, rt::Fetch fetcher) { return {.fetcher = std::move(fetcher), .now = [now] { return now; }}; }
  rt::Fetch success() { return [](rt::RequestInfo, rt::RequestInit) { return rt::fulfilledTask(rt::Response::json(usagePayload())); }; }
}
// Port of all three cases in tests/claude-usage-retry.test.ts; real native reopen uses isolated TMPDIR.
ASYNC_TEST("legacy usage Retry-After survives native reopen without changing inference state") {
  Isolated scope; const std::string directory = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "w05c7-retry-")); const std::string path = rt::path::join(directory, "usage.fixture");
  std::shared_ptr<rt::Kv> kv; const auto cleanup = rt::scopeExit([&] { uos::kv::setKvForTest(nullptr); if (kv) kv->close(); rt::fs::rmSync(directory, {.recursive = true, .force = true}); });
  auto opening = uos::storage::kv::openKv(path); kv = co_await std::move(opening); uos::kv::setKvForTest(kv); AWAIT(account, binding(kv)); const double now = rt::dateNow();
  const auto initial = options(now - 31'000, success()); AWAIT(first, refresh::refreshClaudeCodeUsage(kv, account, initial)); EXPECT_EQ(kind(first), std::string("completed"));
  quota::QuotaOutcome guard{}; guard.binding = account; guard.observation = std::nullopt; guard.transient = quota::QuotaOutcome::Transient{300'000}; guard.nowMs = now - 31'000; AWAIT(guarded, quota::recordClaudeCodeQuotaOutcome(kv, guard)); ASSERT(guarded);
  AWAIT(previous, quota::loadClaudeCodeQuotaState(kv, account)); ASSERT(previous && previous->usage_snapshot);
  auto calls = std::make_shared<int>(0); const rt::Fetch limited = [calls](rt::RequestInfo, rt::RequestInit) { ++*calls; const rt::ResponseInit init{.status = 429, .headers = rt::Headers{{"Retry-After", "120"}}}; return rt::fulfilledTask(rt::Response("Too many usage reads", init)); };
  const auto limitOptions = options(now, limited); AWAIT(limitedResult, refresh::refreshClaudeCodeUsage(kv, account, limitOptions)); EXPECT_EQ(kind(limitedResult), std::string("completed")); kv->close(); opening = uos::storage::kv::openKv(path); kv = co_await std::move(opening); uos::kv::setKvForTest(kv);
  const auto freshOptions = options(now + 31'000, limited); AWAIT(fresh, refresh::refreshClaudeCodeUsage(kv, account, freshOptions)); EXPECT_EQ(kind(fresh), std::string("fresh")); EXPECT_EQ(*calls, 1);
  AWAIT(retained, quota::loadClaudeCodeQuotaState(kv, account)); ASSERT(retained); EXPECT_EQ(retained->usage_snapshot, previous->usage_snapshot); EXPECT_EQ(retained->windows, previous->windows); EXPECT_EQ(retained->cooldown_until_ms, previous->cooldown_until_ms); EXPECT_EQ(retained->blocked_until_ms, previous->blocked_until_ms); EXPECT_EQ(retained->pool_proofs, previous->pool_proofs);
  const rt::Fetch recoveredFetch = [calls](rt::RequestInfo, rt::RequestInit) { ++*calls; return rt::fulfilledTask(rt::Response::json(usagePayload())); }; const auto retryOptions = options(now + 120'000, recoveredFetch); AWAIT(retry, refresh::refreshClaudeCodeUsage(kv, account, retryOptions)); EXPECT_EQ(kind(retry), std::string("completed"));
  AWAIT(recovered, quota::loadClaudeCodeQuotaState(kv, account)); ASSERT(recovered && recovered->usage); EXPECT_EQ(recovered->usage->state, std::string("ok")); EXPECT(!recovered->usage->retry_at_ms); ASSERT(recovered->usage_snapshot); EXPECT_EQ(rt::getNumber(*recovered->usage_snapshot, "observed_at_ms"), std::optional<double>(now + 120'000)); EXPECT_EQ(recovered->cooldown_until_ms, previous->cooldown_until_ms);
}
ASYNC_TEST("a usage 429 without Retry-After retains the existing thirty-second attempt freshness") {
  Isolated scope; const auto kv = std::make_shared<rt::MemoryKv>(rt::dateNow); uos::kv::setKvForTest(kv); AWAIT(account, binding(kv)); const double now = rt::dateNow(); auto calls = std::make_shared<int>(0);
  const rt::Fetch fetcher = [calls](rt::RequestInfo, rt::RequestInit) { ++*calls; const rt::ResponseInit init{.status = 429}; return rt::fulfilledTask(rt::Response("Too many usage reads", init)); };
  const auto firstOptions = options(now, fetcher); AWAIT(first, refresh::refreshClaudeCodeUsage(kv, account, firstOptions)); EXPECT_EQ(kind(first), std::string("completed"));
  const auto freshOptions = options(now + 29'000, fetcher); AWAIT(fresh, refresh::refreshClaudeCodeUsage(kv, account, freshOptions)); EXPECT_EQ(kind(fresh), std::string("fresh")); EXPECT_EQ(*calls, 1);
  const auto staleOptions = options(now + 31'000, fetcher); AWAIT(stale, refresh::refreshClaudeCodeUsage(kv, account, staleOptions)); EXPECT_EQ(kind(stale), std::string("completed")); EXPECT_EQ(*calls, 2);
}
ASYNC_TEST("malformed and expired persisted usage retry deadlines cannot prevent collection") {
  Isolated scope; const auto kv = std::make_shared<rt::MemoryKv>(rt::dateNow); AWAIT(account, binding(kv)); const double now = rt::dateNow();
  const rt::Fetch limited = [](rt::RequestInfo, rt::RequestInit) { const rt::ResponseInit init{.status = 429}; return rt::fulfilledTask(rt::Response("Too many usage reads", init)); }; const auto initial = options(now, limited); AWAIT(first, refresh::refreshClaudeCodeUsage(kv, account, initial)); EXPECT_EQ(kind(first), std::string("completed"));
  AWAIT(previous, quota::loadClaudeCodeQuotaState(kv, account)); ASSERT(previous && previous->usage);
  const std::vector<rt::KvValue> deadlines{rt::KvUndefined{}, nullptr, "forever", rt::Number::NaN, rt::Number::POSITIVE_INFINITY, -1, now - 1, now + 1, rt::Number::MAX_SAFE_INTEGER + 1};
  for (const auto& deadline : deadlines) {
    rt::KvValue value(json::value_from(*previous)); value.as<rt::KvObject>().find("usage")->as<rt::KvObject>().set("retry_at_ms", deadline); co_await kv->set(quota::claudeCodeQuotaKey(account), value);
    auto calls = std::make_shared<int>(0); const rt::Fetch fetcher = [calls](rt::RequestInfo, rt::RequestInit) { ++*calls; return rt::fulfilledTask(rt::Response::json(usagePayload())); }; const auto stale = options(now + 31'000, fetcher); AWAIT(result, refresh::refreshClaudeCodeUsage(kv, account, stale)); EXPECT_EQ(kind(result), std::string("completed")); EXPECT_EQ(*calls, 1);
  }
}
