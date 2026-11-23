// Links: tests/claude_usage_meters_test.cpp
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
//        src/types.cpp src/utils.cpp
// Port of tests/claude-usage-meters.test.ts, including the durable native-reopen case.
#include "uos/config.hpp"
#include "uos/provider/capacity.hpp"
#include "uos/provider/claude_capacity_history.hpp"
#include "uos/provider/claude_capacity_rollups.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_credentials.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/claude_code_usage.hpp"
#include "uos/provider/claude_code_usage_schema.hpp"

#include "uos/rt/env.hpp"
#include "uos/rt/fs.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/process.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/storage/kv.hpp"

#include <optional>
#include <stdexcept>
#include <string>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::provider::claude_code_usage_schema;

namespace {
  constexpr double AT = 1'800'000'000'000;

  json::object row(const json::object& patch = {}) {
    const json::object base = rt::jsonParse(R"({"kind":"future_weekly","group":"weekly","percent":121.5,"resets_at":"2027-01-20T00:00:00Z","severity":"vendor_future",)"
                                            R"("is_active":true,"scope":{"model":{"display_name":"Future model","vendor":7},"surface":{"display_name":"OAuth apps"}}})")
                                .get_object();
    return rt::objectSpread(base, patch);
  }

  json::object snapshot(const json::value& payload, double at = AT) {
    const std::optional<json::object> value = parseClaudeCodeUsageSnapshot(payload, at);
    if (!value) throw std::runtime_error("expected a snapshot");
    return *value;
  }

  const json::array& meters(const json::object& value) { return *rt::getArray(value, "meters"); }
  const json::value& meterField(const json::object& value, std::size_t index, std::string_view key) {
    return *rt::get(meters(value)[index].get_object(), key);
  }
}

TEST("independent nullable legacy usage fields retain their exact source shape") {
  for (const std::string_view key : CLAUDE_USAGE_LEGACY_WINDOWS) {
    json::object input;
    input[key] = json::object{{"utilization", nullptr}, {"resets_at", nullptr}};
    const json::object value = snapshot(input);
    EXPECT(rt::deepEqual(*rt::get(value, "payload"), input));
    EXPECT_EQ(meters(value).size(), std::size_t{1});
    EXPECT(meterField(value, 0, "used_percent").is_null());
    EXPECT(meterField(value, 0, "reset_at_ms").is_null());
    json::object nulled;
    nulled[key] = nullptr;
    EXPECT(meters(snapshot(nulled)).empty());
  }
  EXPECT_EQ(parseClaudeCodeUsageSnapshot(json::object{}, AT), std::nullopt);
  EXPECT_EQ(parseClaudeCodeUsageSnapshot(rt::jsonParse(R"({"seven_day":{"resets_at":null}})"), AT), std::nullopt);
  EXPECT_EQ(parseClaudeCodeUsageSnapshot(json::object{{"limits", json::array{row(json::object{{"percent", "1"}})}}}, AT), std::nullopt);
  EXPECT_EQ(parseClaudeCodeUsageSnapshot(json::object{{"limits", json::array{row(json::object{{"scope", json::object{{"model", json::object{}}}}})}}}, AT), std::nullopt);
  const json::value unparseable = rt::jsonParse(R"({"five_hour":{"utilization":125,"resets_at":"unparseable-provider-text"}})");
  EXPECT(rt::deepEqual(meterField(snapshot(unparseable), 0, "used_percent"), json::value(125)));
  EXPECT(meterField(snapshot(unparseable), 0, "reset_at_ms").is_null());
}

TEST("dynamic meters preserve server order, scope, severity, headline and finite over-100 values") {
  json::object payload;
  payload["limits"] = json::array{row(), row(json::object{{"kind", "future_session"}, {"group", "session"}, {"percent", 2}, {"is_active", false}}), row()};
  payload["seven_day"] = json::object{{"utilization", 10}, {"resets_at", nullptr}};
  payload["vendor"] = json::object{{"retained", true}};
  const json::object value = snapshot(payload);
  EXPECT(rt::deepEqual(*rt::get(value, "payload"), payload));
  std::vector<std::string> kinds;
  std::vector<std::string> ids;
  for (const json::value& meter : meters(value)) {
    kinds.emplace_back(*rt::getString(meter, "kind"));
    ids.emplace_back(*rt::getString(meter, "id"));
  }
  EXPECT_EQ(kinds, (std::vector<std::string>{"future_weekly", "future_session", "future_weekly"}));
  const rt::OrderedSet<std::string> distinctIds(ids.begin(), ids.end());
  EXPECT_EQ(distinctIds.size(), std::size_t{3});
  EXPECT(rt::deepEqual(meterField(value, 0, "used_percent"), rt::makeNumber(121.5)));
  EXPECT_EQ(meterField(value, 0, "severity"), json::value("vendor_future"));
  EXPECT_EQ(meterField(value, 0, "is_active"), json::value(true));
  EXPECT_EQ(meterField(value, 1, "is_active"), json::value(false));
  static const rt::Regex HEADLINE("Future model.*OAuth apps");
  EXPECT(HEADLINE.test(std::string(meterField(value, 0, "label").get_string())));
  const std::optional<json::object> reread = readClaudeCodeUsageSnapshot(json::value(value));
  EXPECT(reread && rt::deepEqual(*reread, value));
  json::object emptyLimits = rt::objectSpread(payload);
  rt::objectSet(emptyLimits, "limits", json::array{});
  EXPECT(meters(snapshot(emptyLimits)).empty());
  json::object nullLimits = rt::objectSpread(payload);
  rt::objectSet(nullLimits, "limits", nullptr);
  EXPECT_EQ(meterField(snapshot(nullLimits), 0, "kind"), json::value("seven_day"));
  EXPECT_EQ(meterField(snapshot(json::object{{"seven_day", *rt::get(payload, "seven_day")}}), 0, "kind"), json::value("seven_day"));
  EXPECT(meters(snapshot(rt::jsonParse(R"({"extra_usage":{"is_enabled":false,"monthly_limit":null,"used_credits":12,"utilization":null,"currency":"USD"}})"))).empty());
}

TEST("scoped usage at exhaustion cannot become an unscoped quota guard") {
  namespace quota = uos::provider::claude_code_quota;
  json::object limits;
  limits["limits"] = json::array{row(json::object{{"percent", 100}})};
  const std::optional<quota::ClaudeCodeQuotaObservation> observation = uos::provider::claude_code_usage::parseClaudeCodeUsagePayload(json::value(limits), AT);
  ASSERT(observation.has_value());
  EXPECT_EQ(observation->windows.size(), std::size_t{0});
  EXPECT(!quota::claudeCodeExhaustionFromResponse(429, observation).has_value());
  const quota::ClaudeCodeQuotaState state = quota::mergeClaudeCodeQuotaState(std::nullopt, *observation, std::nullopt, AT);
  EXPECT_EQ(state.blocked_until_ms, std::nullopt);
  EXPECT_EQ(state.block_window, std::nullopt);
  EXPECT_EQ(state.cooldown_until_ms, std::nullopt);
  EXPECT(!state.pool_proofs.has_value());
  const std::optional<quota::ClaudeCodeQuotaObservation> partial =
    uos::provider::claude_code_usage::parseClaudeCodeUsagePayload(rt::jsonParse(R"({"seven_day_sonnet":{"utilization":null,"resets_at":null}})"), AT + 1);
  ASSERT(partial.has_value() && partial->usage_snapshot.has_value());
  const quota::ClaudeCodeQuotaState replaced = quota::mergeClaudeCodeQuotaState(state, *partial, std::nullopt, AT + 1);
  ASSERT(replaced.usage_snapshot.has_value());
  EXPECT(rt::deepEqual(*rt::get(*replaced.usage_snapshot, "payload"), *rt::get(*partial->usage_snapshot, "payload")));
  EXPECT_EQ(meters(*replaced.usage_snapshot).size(), std::size_t{1});
  const quota::ClaudeCodeQuotaState again = quota::mergeClaudeCodeQuotaState(replaced, *observation, std::nullopt, AT + 2);
  EXPECT(again.usage_snapshot.has_value() && rt::deepEqual(json::value(*again.usage_snapshot), json::value(*replaced.usage_snapshot)));
}

ASYNC_TEST("direct snapshots, current readback, history and scoped rollups survive native reopen without header freshness") {
  namespace quota = uos::provider::claude_code_quota;
  namespace credentials = uos::provider::claude_code_credentials;
  namespace accounts = uos::provider::claude_code_accounts;
  namespace history = uos::provider::claude_capacity_history;
  namespace rollups = uos::provider::claude_capacity_rollups;
  rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt},
                                      {"CLAUDE_API_KEY", std::nullopt}, {"CLAUDE_CODE_OAUTH_TOKEN", std::nullopt}, {"CLAUDE_CODE_WORKSPACE_ID", std::nullopt}};
  const std::string savedSeed = uos::config::config.codexAuthJsonB64;
  uos::config::config.codexAuthJsonB64.clear();
  const auto restoreSeed = rt::scopeExit([&savedSeed] { uos::config::config.codexAuthJsonB64 = savedSeed; });
  const std::string directory = rt::fs::mkdtempSync(rt::path::join(rt::os::tmpdir(), "c0-claude-usage-"));
  const std::string path = rt::path::join(directory, "usage.fixture");
  std::shared_ptr<rt::Kv> store;
  const auto cleanup = rt::scopeExit([&] {
    if (store) store->close();
    credentials::resetClaudeCodeCredentialCacheForTest();
    rt::fs::rmSync(directory, {.recursive = true, .force = true});
  });
  auto opening = uos::storage::kv::openKv(path);
  store = co_await std::move(opening);
  const double at = rt::dateNow();
  credentials::StoreClaudeCodeStoredCredentialInput credential;
  credential.secret = std::string("sk-") + "ant-oat-fixture-usage-native";
  auto storing = credentials::storeClaudeCodeStoredCredential(store, credential);
  const auto storedCredential = co_await std::move(storing);
  ASSERT(storedCredential.has_value());
  const uos::provider::claude_code::ClaudeCodeEnvLookup noEnv = [](const std::string&) -> std::optional<std::string> { return std::nullopt; };
  auto resolving = accounts::resolveClaudeCodeAccount(store, noEnv);
  const auto binding = co_await std::move(resolving);
  ASSERT(binding.has_value());
  const json::object payload{{"limits", json::array{row(), row(json::object{{"kind", "surface_only"}, {"scope", json::object{{"surface", json::object{{"display_name", "Future surface"}}}}}, {"percent", 8}})}}};
  const auto observation = uos::provider::claude_code_usage::parseClaudeCodeUsagePayload(json::value(payload), at);
  ASSERT(observation.has_value());
  quota::QuotaOutcome outcome;
  outcome.binding = binding;
  outcome.observation = observation;
  outcome.response_status = 200;
  outcome.nowMs = at;
  outcome.usage = quota::ClaudeCodeUsageState{.source = "oauth_usage", .state = "ok", .status = 200, .message = std::nullopt, .attempted_at_ms = at, .observed_at_ms = at};
  auto recording = quota::recordClaudeCodeQuotaOutcome(store, outcome);
  const auto recorded = co_await std::move(recording);
  ASSERT(recorded.has_value());
  store->close();
  opening = uos::storage::kv::openKv(path);
  store = co_await std::move(opening);
  auto loading = quota::loadClaudeCodeQuotaState(store, binding);
  const auto loaded = co_await std::move(loading);
  ASSERT(loaded && loaded->usage_snapshot);
  EXPECT(rt::deepEqual(loaded->usage_snapshot->at("payload"), payload));
  auto readingHistory = history::readClaudeCapacityHistory(store, at + 1);
  const auto retained = co_await std::move(readingHistory);
  EXPECT_EQ(retained.scan, std::string("ok"));
  ASSERT(!retained.points.empty() && retained.points[0].usage_snapshot);
  EXPECT(rt::deepEqual(retained.points[0].usage_snapshot->at("payload"), payload));
  auto listing = rollups::listClaudeUsageRollups(store, at - 3'600'000, at + 1);
  const auto usageRollups = co_await std::move(listing);
  EXPECT_EQ(usageRollups.scan, std::string("ok"));
  ASSERT_EQ(usageRollups.points.size(), std::size_t{2});
  EXPECT_EQ(usageRollups.points[0].samples, 1);
  outcome = quota::QuotaOutcome{};
  outcome.binding = binding;
  outcome.observation = std::nullopt;
  outcome.nowMs = at + 10;
  outcome.usage = quota::ClaudeCodeUsageState{.source = "oauth_usage", .state = "unreachable", .status = std::nullopt, .message = "offline", .attempted_at_ms = at + 10, .observed_at_ms = std::nullopt};
  recording = quota::recordClaudeCodeQuotaOutcome(store, outcome);
  const auto failedRead = co_await std::move(recording);
  (void)failedRead;
  const rt::Headers headers{{"anthropic-ratelimit-unified-5h-utilization", "0.1"}};
  outcome = quota::QuotaOutcome{};
  outcome.binding = binding;
  outcome.observation = quota::observeClaudeCodeQuota(headers, at + 20);
  outcome.nowMs = at + 20;
  recording = quota::recordClaudeCodeQuotaOutcome(store, outcome);
  const auto headerRead = co_await std::move(recording);
  (void)headerRead;
  auto sourcing = uos::provider::capacity::claudeCapacitySource(store, at + 20, noEnv);
  const auto source = co_await std::move(sourcing);
  EXPECT(!source.usage_snapshot.has_value());
  EXPECT(!source.usage.has_value());
  EXPECT(!source.source_observed_at_ms.has_value());
  EXPECT(!source.windows.five_hour.has_value());
  EXPECT_EQ(source.state, std::string("available"));
  const json::value emptyPayload(json::object{{"limits", json::array{}}});
  const auto empty = uos::provider::claude_code_usage::parseClaudeCodeUsagePayload(emptyPayload, at + 30);
  ASSERT(empty.has_value());
  outcome = quota::QuotaOutcome{};
  outcome.binding = binding;
  outcome.observation = empty;
  outcome.nowMs = at + 30;
  recording = quota::recordClaudeCodeQuotaOutcome(store, outcome);
  const auto emptyRead = co_await std::move(recording);
  (void)emptyRead;
  loading = quota::loadClaudeCodeQuotaState(store, binding);
  const auto emptyState = co_await std::move(loading);
  ASSERT(emptyState && emptyState->usage_snapshot);
  EXPECT(meters(*emptyState->usage_snapshot).empty());
  readingHistory = history::readClaudeCapacityHistory(store, at + 31);
  const auto emptyHistory = co_await std::move(readingHistory);
  ASSERT(!emptyHistory.points.empty() && emptyHistory.points.back().usage_snapshot);
  EXPECT(meters(*emptyHistory.points.back().usage_snapshot).empty());
}
