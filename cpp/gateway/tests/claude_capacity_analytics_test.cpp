// Links: tests/claude_capacity_analytics_test.cpp
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
// The three usage-refresh cases from tests/claude-capacity-analytics.test.ts.
// Existing capacity/history module cases are independently covered in claude_capacity_differential_test.cpp.
ASYNC_TEST("legacy direct-usage storage retains its independent lease and cannot become header evidence") {
  Isolated scope; const auto kv = std::make_shared<rt::MemoryKv>(rt::dateNow); AWAIT(account, binding(kv)); const double now = rt::dateNow();
  auto fetched = rt::withResolvers<void>(); auto released = rt::withResolvers<rt::Response>(); auto calls = std::make_shared<int>(0);
  const rt::Fetch fetcher = [calls, fetched, released](rt::RequestInfo, rt::RequestInit) { ++*calls; fetched.resolve(); return released.promise.wait(); }; const auto firstOptions = options(now, fetcher);
  rt::Promise first(refresh::refreshClaudeCodeUsage(kv, account, firstOptions)); co_await fetched.promise.wait();
  const auto busyOptions = options(now, {}); AWAIT(busy, refresh::refreshClaudeCodeUsage(kv, account, busyOptions)); EXPECT_EQ(kind(busy), std::string("busy")); released.resolve(rt::Response::json(usagePayload())); const auto initial = co_await first.wait(); EXPECT_EQ(kind(initial), std::string("completed"));
  const auto freshOptions = options(now + 1, {}); AWAIT(fresh, refresh::refreshClaudeCodeUsage(kv, account, freshOptions)); EXPECT_EQ(kind(fresh), std::string("fresh")); EXPECT_EQ(*calls, 1);
  const rt::Fetch refused = [](rt::RequestInfo, rt::RequestInit) { const rt::ResponseInit init{.status = 403}; return rt::fulfilledTask(rt::Response::json(json::object{{"error", json::object{{"message", "user:profile scope required"}}}}, init)); }; const auto stale = options(now + refresh::CLAUDE_CODE_USAGE_FRESH_MS, refused); AWAIT(result, refresh::refreshClaudeCodeUsage(kv, account, stale)); EXPECT_EQ(kind(result), std::string("completed"));
  AWAIT(state, quota::loadClaudeCodeQuotaState(kv, account)); ASSERT(state && state->usage); EXPECT_EQ(state->usage->state, std::string("scope_missing")); ASSERT(state->windows.get("5h")); EXPECT_EQ(state->windows.get("5h")->utilization, std::optional<double>(0.42)); EXPECT(!state->blocked_until_ms); EXPECT(!state->cooldown_until_ms);
  AWAIT(retained, history::readClaudeCapacityHistory(kv, now + refresh::CLAUDE_CODE_USAGE_FRESH_MS)); EXPECT_EQ(retained.points.size(), 1u);
}
ASYNC_TEST("an account removed or replaced during direct collection cannot publish quota or history") {
  Isolated scope; const std::vector<std::string> changes{"remove", "replace"};
  for (const auto& change : changes) {
    const auto kv = std::make_shared<rt::MemoryKv>(rt::dateNow); AWAIT(account, binding(kv)); const double now = rt::dateNow();
    const rt::Fetch fetcher = [kv, account, change](rt::RequestInfo, rt::RequestInit) -> rt::Task<rt::Response> {
      if (change == "remove") { auto removing = accounts::removeClaudeCodeAccount(kv, account.account_id); co_await std::move(removing); }
      else { auto credential = account.credential; credential.secret = std::string("sk-") + "ant-oat-fixture-new-generation"; const accounts::AccountUpdate update{.accountId = account.account_id, .credential = credential}; auto updating = accounts::updateClaudeCodeAccount(kv, update, nullEnv); co_await std::move(updating); }
      co_return rt::Response::json(usagePayload());
    }; const auto configured = options(now, fetcher); AWAIT(result, refresh::refreshClaudeCodeUsage(kv, account, configured)); EXPECT_EQ(kind(result), std::string("changed")); AWAIT(state, quota::loadClaudeCodeQuotaState(kv, account)); EXPECT(!state); AWAIT(retained, history::readClaudeCapacityHistory(kv, now)); EXPECT(retained.points.empty());
  }
}
ASYNC_TEST("direct usage quota and history survive native close and reopen") {
  Isolated scope; const std::string directory = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "w05c7-direct-")); const std::string path = rt::path::join(directory, "usage.fixture"); std::shared_ptr<rt::Kv> kv;
  const auto cleanup = rt::scopeExit([&] { if (kv) kv->close(); rt::fs::rmSync(directory, {.recursive = true, .force = true}); }); auto opening = uos::storage::kv::openKv(path); kv = co_await std::move(opening); AWAIT(account, binding(kv)); const double now = rt::dateNow(); const auto configured = options(now, success()); AWAIT(first, refresh::refreshClaudeCodeUsage(kv, account, configured)); EXPECT_EQ(kind(first), std::string("completed")); kv->close(); opening = uos::storage::kv::openKv(path); kv = co_await std::move(opening);
  AWAIT(state, quota::loadClaudeCodeQuotaState(kv, account)); ASSERT(state && state->usage); EXPECT_EQ(state->usage->state, std::string("ok")); ASSERT(state->windows.get("5h")); EXPECT_EQ(state->windows.get("5h")->utilization, std::optional<double>(0.42)); AWAIT(retained, history::readClaudeCapacityHistory(kv, now + 1'000)); ASSERT(!retained.points.empty()); EXPECT_EQ(retained.points[0].windows.five_hour->used_percent, std::optional<double>(42));
}
