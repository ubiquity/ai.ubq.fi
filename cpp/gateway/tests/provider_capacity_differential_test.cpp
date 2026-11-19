// Links: tests/provider_capacity_differential_test.cpp src/auth/relay.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_codex.cpp src/authority/native_paid.cpp src/bounded_response_body.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/prompt_analytics.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp src/codex/auth_refresh.cpp src/codex/auth.cpp src/codex/banked_reset_provider.cpp src/codex/banked_reset.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/native_auth_transport.cpp src/codex/native_auth.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp src/http.cpp src/inference_deadline.cpp src/kv.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp src/optional_telemetry_queue.cpp src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity_analytics.cpp src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp src/provider/capacity_rollups.cpp src/provider/capacity.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/selection.cpp src/release.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/runtime/http.cpp src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/utils.cpp src/authority/native_paid_contracts.cpp src/types.cpp
// Differential cases for src/provider/capacity.ts: the sampler, the persisted / live / coalesced admin views, the
// event-driven sample and the two admin handlers, over an in-memory KV seeded like tests/helpers/provider-capacity-harness.ts
// (a two-account Codex pool and a Metered quota state) and a fetch double answering the Codex usage and OpenLux endpoints.
// The vectors (tests/provider_capacity_differential_vectors.inc) were recorded from the TS module under bun 1.4.0 with HOME
// an empty directory, no provider credentials (METERED_API_KEY set where the generator sets it), Date.now a controlled
// clock that the cold-wait timers advance, and an in-memory Deno KV whose operations complete one macrotask later. Each
// step compares the result, the fetch calls and every stored row, as the generator recorded them.
//
// The process environment is replaced for the whole test (rt::env::ScopedEnvironment): HOME names a directory that does
// not exist, so no ~/.codex/auth.json of the machine running the test is ever read, and no credential variable is visible.
#include "uos/codex/account_routing.hpp"
#include "uos/codex/auth.hpp"
#include "uos/kv.hpp"
#include "uos/metered_quota.hpp"
#include "uos/provider/capacity.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/health.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/test.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

using namespace uos;
namespace capacity = provider::capacity;
namespace contract = provider::capacity_contract;

namespace {
  const char* const VECTORS =
#include "tests/provider_capacity_differential_vectors.inc"
    ;

  const json::object& vectors() {
    static const json::object parsed = rt::jsonParse(VECTORS).as_object();
    return parsed;
  }

  constexpr double NOW = 1'800'000'000'000;

  void expectJson(const json::value& actual, const json::value& expected, const std::string& label, const char* file, int line) {
    if (!rt::deepEqual(actual, expected) || rt::jsonStringify(actual) != rt::jsonStringify(expected))
      rt::test::report(file, line, label + "\n      actual:   " + rt::jsonStringify(actual) + "\n      expected: " + rt::jsonStringify(expected), false);
  }
#define EXPECT_JSON(actual, expected, label) expectJson((actual), (expected), (label), __FILE__, __LINE__)

  json::value keyJson(const rt::KvKey& key) {
    json::array out;
    for (const rt::KvKeyPart& part : key) {
      if (part.isString()) out.emplace_back(json::string(part.string()));
      else out.push_back(rt::makeNumber(part.number()));
    }
    return out;
  }

  rt::KvKey keyOf(const json::value& value) {
    rt::KvKey key;
    for (const json::value& part : value.as_array()) {
      if (part.is_string()) key.emplace_back(std::string(part.get_string()));
      else key.emplace_back(rt::toNumber(part));
    }
    return key;
  }

  // `[...kv.rows.values()]` sorted by the key's JSON text.
  json::value rowsOf(const rt::MemoryKvDatabase& database) {
    std::vector<std::pair<std::string, json::value>> rows;
    for (const rt::MemoryKvDatabase::Row& row : database.rows()) {
      json::object item;
      item["key"] = keyJson(row.key);
      item["value"] = row.value.toJson();
      rows.emplace_back(rt::jsonStringify(item["key"]), std::move(item));
    }
    std::sort(rows.begin(), rows.end(), [](const auto& left, const auto& right) { return left.first < right.first; });
    json::array out;
    for (auto& [text, row] : rows) out.push_back(std::move(row));
    return out;
  }

  // The generator's fetch double and its switchable behaviour.
  struct Behaviour {
    std::optional<std::string> failAccount{};
    double failureStatus = 503;
    std::optional<std::string> invalidJson{};
    std::optional<std::string> badShape{};
    std::optional<std::string> throwAccount{};
    bool spark = false;
    std::vector<std::pair<std::string, std::pair<double, double>>> used{};
  };

  struct FetchState {
    Behaviour behaviour;
    json::array calls;
    double* clock = nullptr;
  };

  std::string requestUrl(const rt::RequestInfo& input) {
    if (const auto* text = std::get_if<std::string>(&input)) return *text;
    if (const auto* url = std::get_if<rt::URL>(&input)) return url->href();
    return std::get<rt::Request>(input).url();
  }

  json::value orNull(const std::optional<std::string>& value) { return value ? json::value(json::string(*value)) : json::value(nullptr); }

  rt::Response jsonResponse(const json::value& body, double status) {
    rt::Headers headers;
    headers.append("Content-Type", "application/json");
    return rt::Response(rt::BodyInit(rt::jsonStringify(body)), rt::ResponseInit{.status = status, .statusText = std::nullopt, .headers = headers});
  }

  json::value window(double seconds, double used, double resetAt) {
    json::object out;
    out["limit_window_seconds"] = rt::makeNumber(seconds);
    out["used_percent"] = rt::makeNumber(used);
    out["reset_at"] = rt::makeNumber(resetAt);
    return out;
  }

  rt::Fetch fetchDouble(std::shared_ptr<FetchState> state) {
    return rt::Fetch([state](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
      const rt::Headers headers(init.headers.value_or(rt::Headers()));
      const std::string url = requestUrl(input);
      const std::optional<std::string> account = headers.get("ChatGPT-Account-ID");
      json::object call;
      call["url"] = json::string(url);
      call["account"] = orNull(account);
      call["authorization"] = orNull(headers.get("Authorization"));
      call["accept"] = orNull(headers.get("Accept"));
      call["agent"] = orNull(headers.get("User-Agent"));
      call["method"] = orNull(init.method);
      call["redirect"] = orNull(init.redirect);
      state->calls.push_back(std::move(call));
      const Behaviour& behaviour = state->behaviour;
      if (url == "https://api.openlux.ai/api/usage/token/") {
        co_return jsonResponse(rt::jsonParse(R"({"success":true,"data":{"total_available":750,"total_granted":1000,"total_used":250,"unlimited_quota":false}})"), 200);
      }
      if (account && account == behaviour.throwAccount) throw rt::TypeError("network down");
      if (account && account == behaviour.failAccount) {
        co_return rt::Response(rt::BodyInit(std::string("upstream-secret-body")), rt::ResponseInit{.status = behaviour.failureStatus});
      }
      if (account && account == behaviour.invalidJson) co_return rt::Response(rt::BodyInit(std::string("{not json")), rt::ResponseInit{.status = 200});
      if (account && account == behaviour.badShape) co_return jsonResponse(rt::jsonParse(R"({"rate_limit":"nope"})"), 200);
      std::pair<double, double> used = account == std::optional<std::string>("account-one") ? std::pair<double, double>{12.5, 38} : std::pair<double, double>{67, 81.25};
      for (const auto& [name, values] : behaviour.used) {
        if (account == std::optional<std::string>(name)) used = values;
      }
      json::object body;
      json::object rateLimit;
      if (behaviour.spark) {
        rateLimit["primary_window"] = window(604'800, used.first, 1'800'010'000);
        rateLimit["secondary_window"] = nullptr;
        body["rate_limit"] = std::move(rateLimit);
        json::array additional;
        json::object spark;
        spark["limit_name"] = "GPT-5.3-Codex-Spark";
        spark["metered_feature"] = "codex_bengalfox";
        json::object sparkLimit;
        sparkLimit["primary_window"] = window(18'000, used.second, 1'800'011'000);
        sparkLimit["secondary_window"] = nullptr;
        spark["rate_limit"] = std::move(sparkLimit);
        additional.push_back(std::move(spark));
        json::object unanchored;
        unanchored["limit_name"] = "Unanchored";
        unanchored["metered_feature"] = nullptr;
        json::object unanchoredLimit;
        unanchoredLimit["primary_window"] = window(18'000, 0, *state->clock / 1000 + 18'000);
        unanchoredLimit["secondary_window"] = nullptr;
        unanchored["rate_limit"] = std::move(unanchoredLimit);
        additional.push_back(std::move(unanchored));
        body["additional_rate_limits"] = std::move(additional);
      } else {
        rateLimit["primary_window"] = window(10'800, used.first, 1'800'010'000);
        rateLimit["secondary_window"] = window(86'400, used.second, 1'800'020'000);
        body["rate_limit"] = std::move(rateLimit);
      }
      body["private_account_field"] = "must-not-escape";
      co_return jsonResponse(body, 200);
    });
  }

  rt::Task<json::value> responseJson(rt::Response response) {
    json::array headers;
    for (const auto& [name, value] : response.headers().entries()) {
      json::array pair;
      pair.emplace_back(json::string(name));
      pair.emplace_back(json::string(value));
      headers.push_back(std::move(pair));
    }
    auto reading = response.text();
    const std::string body = co_await std::move(reading);
    json::object out;
    out["status"] = rt::makeNumber(static_cast<double>(response.status()));
    out["headers"] = std::move(headers);
    out["body"] = json::string(body);
    co_return out;
  }

  // crypto.randomUUID() as the generator's counter: 00000000-0000-4000-8000-<n as 12 hex digits>.
  rt::crypto::RandomSource uuidSource() {
    auto counter = std::make_shared<std::uint64_t>(0);
    return [counter](std::span<std::uint8_t> bytes) {
      *counter += 1;
      for (auto& byte : bytes) byte = 0;
      for (std::size_t index = 0; index < 6 && index < bytes.size(); ++index)
        bytes[bytes.size() - 1 - index] = static_cast<std::uint8_t>((*counter >> (8 * index)) & 0xff);
    };
  }

  json::value ok(json::value value) {
    json::object out;
    out["ok"] = std::move(value);
    return out;
  }
  json::value undefinedOk() { return ok(json::object{{"$undefined", true}}); }
}

ASYNC_TEST("capacity: sampler, persisted, live and coalesced views, the event sample and the admin handlers") {
  rt::env::ScopedEnvironment environment({{"HOME", "/nonexistent/uos-b4-capacity-home"}, {"TZ", "UTC"}});
  double clock = NOW;
  const auto realNow = rt::globalDateNow();
  rt::globalDateNow() = [&clock] { return clock; };
  const json::object& seed = vectors().at("seed").as_object();
  const json::array& steps = vectors().at("steps").as_array();
  std::size_t index = 0;
  auto state = std::make_shared<FetchState>();
  state->clock = &clock;
  const rt::Fetch fetcher = fetchDouble(state);
  auto owners = std::make_shared<int>(0);
  const std::function<std::string()> createLeaseOwner = [owners] { return "owner-" + std::to_string(++*owners); };
  provider::capacity_events::setProviderCapacitySampleTriggerForTest([](const provider::capacity_events::ProviderCapacitySampleTriggerOptions&) {});

  std::shared_ptr<rt::MemoryKvDatabase> database;
  std::shared_ptr<rt::Kv> kv;
  const auto reset = [&](bool seedPool, bool seedMetered) -> rt::Task<void> {
    database = std::make_shared<rt::MemoryKvDatabase>([] { return rt::test::clock().dateNow(); });
    kv = std::make_shared<rt::MemoryKv>(database);
    codex::auth::resetCodexAuthCacheForTest();
    codex::account_routing::resetCodexAccountRoutingForTest();
    kv::setKvForTest(kv);
    if (seedPool) {
      const rt::KvValue pool(seed.at("pool"));
      auto writing = kv->set(keyOf(seed.at("codexAuthPoolKey")), pool);
      co_await std::move(writing);
    }
    if (seedMetered) {
      const rt::KvValue metered(seed.at("meteredState"));
      auto writing = kv->set(keyOf(seed.at("meteredStateKey")), metered);
      co_await std::move(writing);
    }
  };
  const auto check = [&](std::string_view label, const json::value& result) {
    ASSERT(index < steps.size());
    const json::value& step = steps[index++];
    EXPECT_EQ(std::string(step.at("label").as_string()), std::string(label));
    EXPECT_EQ(rt::toNumber(step.at("clock")), clock);
    EXPECT_JSON(result, step.at("result"), std::string(label) + " result");
    EXPECT_JSON(state->calls, step.at("fetches"), std::string(label) + " fetches");
    EXPECT_JSON(rowsOf(*database), step.at("rows"), std::string(label) + " rows");
    state->calls.clear();
  };
  const auto options = [&](std::optional<std::shared_ptr<rt::Kv>> store, double now) {
    contract::ProviderCapacitySnapshotOptions value;
    value.kv = std::move(store);
    value.fetcher = fetcher;
    value.now = [now] { return now; };
    value.createLeaseOwner = createLeaseOwner;
    return value;
  };
  const auto readOptions = [&](std::optional<std::shared_ptr<rt::Kv>> store, double now) {
    contract::ProviderCapacitySnapshotOptions value;
    value.kv = std::move(store);
    value.now = [now] { return now; };
    return value;
  };
  const std::optional<std::shared_ptr<rt::Kv>> noKv = std::shared_ptr<rt::Kv>();
  const auto viewStep = [&](std::string_view label, rt::Task<contract::ProviderCapacityView> task) -> rt::Task<void> {
    std::optional<json::value> result;
    std::string error;
    try {
      const contract::ProviderCapacityView view = co_await std::move(task);
      result = ok(json::value_from(view));
    } catch (const rt::JsError& failure) {
      error = failure.name + ": " + failure.message;
    }
    if (!result) {
      json::object failed;
      failed["error"] = json::string(error);
      result = failed;
    }
    check(label, *result);
  };
  const auto voidStep = [&](std::string_view label, rt::Task<void> task) -> rt::Task<void> {
    co_await std::move(task);
    check(label, undefinedOk());
  };
  const auto responseStep = [&](std::string_view label, rt::Task<rt::Response> task) -> rt::Task<void> {
    rt::Response response = co_await std::move(task);
    auto reading = responseJson(response);
    const json::value result = co_await std::move(reading);
    check(label, ok(result));
  };
#define VIEW_STEP(label, call) \
  { \
    auto task_ = (call); \
    auto step_ = viewStep(label, std::move(task_)); \
    co_await std::move(step_); \
  }
#define VOID_STEP(label, call) \
  { \
    auto task_ = (call); \
    auto step_ = voidStep(label, std::move(task_)); \
    co_await std::move(step_); \
  }
#define RESPONSE_STEP(label, call) \
  { \
    auto task_ = (call); \
    auto step_ = responseStep(label, std::move(task_)); \
    co_await std::move(step_); \
  }

  EXPECT_JSON(json::value_from(capacity::unavailableClaudeCapacitySource(NOW)), vectors().at("unavailableClaude"), "unavailableClaudeCapacitySource");

  // S1: no KV at all.
  {
    auto resetting = reset(false, false);
    co_await std::move(resetting);
  }
  kv::setKvForTest(nullptr);
  VIEW_STEP("refreshNoKv", capacity::refreshProviderCapacity(options(noKv, NOW + 0.5)));
  VIEW_STEP("persistedNoKv", capacity::getPersistedProviderCapacityView(readOptions(noKv, NOW)));
  VOID_STEP("sampleNoKv", capacity::sampleProviderCapacityOnEvent(options(noKv, NOW)));

  // S2: seeded store, metered configured.
  rt::env::set("METERED_API_KEY", "metered-api-key");
  {
    auto resetting = reset(true, true);
    co_await std::move(resetting);
  }
  VIEW_STEP("refreshLive", capacity::refreshProviderCapacity(options(kv, NOW)));
  VIEW_STEP("persisted", capacity::getPersistedProviderCapacityView(readOptions(kv, NOW + 1'000)));
  clock = NOW + 5 * 60'000;
  state->behaviour = Behaviour{.failAccount = "account-two", .failureStatus = 503};
  VIEW_STEP("refreshFailing", capacity::refreshProviderCapacity(options(kv, NOW + 5 * 60'000)));
  clock = NOW + 16 * 60'000;
  state->behaviour = Behaviour{.used = {{"account-one", {5, 10}}, {"account-two", {8, 9}}}};
  VIEW_STEP("refreshRecovered", capacity::refreshProviderCapacity(options(kv, NOW + 16 * 60'000)));
  clock = NOW + 31 * 60'000;
  state->behaviour = Behaviour{.invalidJson = "account-one", .badShape = "account-two"};
  VOID_STEP("sampleInvalid", capacity::sampleProviderCapacityOnEvent(options(kv, NOW + 31 * 60'000)));
  state->behaviour = Behaviour{.failAccount = "account-two", .failureStatus = 401, .throwAccount = "account-one", .spark = true};
  clock = NOW + 47 * 60'000;
  VIEW_STEP("refreshThrowSpark", capacity::refreshProviderCapacity(options(kv, NOW + 47 * 60'000)));
  state->behaviour = Behaviour{.spark = true};
  clock = NOW + 48 * 60'000;
  VIEW_STEP("persistedLater", capacity::getPersistedProviderCapacityView(readOptions(kv, NOW + 48 * 60'000)));
  VIEW_STEP("persistedStale", capacity::getPersistedProviderCapacityView(readOptions(kv, NOW + 47 * 60'000 + 30 * 60'000)));

  // Lease held by another owner: the refresh coalesces (waits out the cold wait) and serves the cached snapshot.
  {
    contract::CapacityLease other{.owner = "someone-else", .lease_until_ms = NOW + 49 * 60'000 + 5'000};
    const rt::KvValue lease(json::value_from(other));
    auto writing = kv->set(capacity::PROVIDER_CAPACITY_LEASE_KEY, lease);
    co_await std::move(writing);
  }
  const auto realSetTimeout = rt::globalSetTimeout();
  // The cold wait polls every 50 ms up to 2 s of Date.now(): advance the clock with each timer.
  rt::globalSetTimeout() = [realSetTimeout, &clock](std::function<void()> handler, double ms) {
    return realSetTimeout([handler, ms, &clock] {
      clock += ms;
      handler();
    }, 0);
  };
  clock = NOW + 49 * 60'000;
  VIEW_STEP("refreshCoalesced", capacity::refreshProviderCapacity(options(kv, NOW + 49 * 60'000)));
  VOID_STEP("sampleLeaseHeld", capacity::sampleProviderCapacityOnEvent(options(kv, NOW + 49 * 60'000)));
  rt::globalSetTimeout() = realSetTimeout;

  // Handlers.
  clock = NOW + 49 * 60'000 + 10'000;
  {
    auto deleting = kv->delete_(capacity::PROVIDER_CAPACITY_LEASE_KEY);
    co_await std::move(deleting);
  }
  const auto request = [](std::string url) { return rt::Request(url); };
  RESPONSE_STEP("rollupsDefault", capacity::handleProviderCapacityRollups(request("https://ai.ubq.fi/admin/providers/capacity/rollups"), readOptions(kv, clock)));
  RESPONSE_STEP("rollupsWindow", capacity::handleProviderCapacityRollups(request("https://ai.ubq.fi/admin/providers/capacity/rollups?window_days=2"), readOptions(kv, clock)));
  RESPONSE_STEP("rollupsBadWindow",
                capacity::handleProviderCapacityRollups(request("https://ai.ubq.fi/admin/providers/capacity/rollups?window_days=0x10"), readOptions(kv, clock)));
  RESPONSE_STEP("rollupsHugeWindow",
                capacity::handleProviderCapacityRollups(request("https://ai.ubq.fi/admin/providers/capacity/rollups?window_days=9999.9"), readOptions(kv, clock)));
  RESPONSE_STEP("rollupsNoKv", capacity::handleProviderCapacityRollups(request("https://ai.ubq.fi/admin/providers/capacity/rollups"), readOptions(noKv, clock)));
  // A remote analytics row merges into the admin view's chart.
  {
    auto reading = capacity::getPersistedProviderCapacityView(readOptions(kv, clock));
    const contract::ProviderCapacityView view = co_await std::move(reading);
    contract::ProviderCapacityHistoryPoint remote = view.history[0];
    remote.bucket_start_at_ms = view.history[0].bucket_start_at_ms + 2 * 15 * 60'000;
    remote.sampled_at_ms = remote.bucket_start_at_ms + 3;
    const rt::KvValue value(json::value_from(remote));
    auto writing = kv->set(rt::KvKey{"uos_ai", "provider_capacity", "analytics", "remote", "peer", remote.bucket_start_at_ms, remote.sampled_at_ms}, value);
    co_await std::move(writing);
  }
  RESPONSE_STEP("handlerFresh", capacity::handleProviderCapacity(request("https://ai.ubq.fi/admin/providers/capacity"), options(kv, NOW + 47 * 60'000 + 10'000)));
  RESPONSE_STEP("handlerStaleRefresh", capacity::handleProviderCapacity(request("https://ai.ubq.fi/admin/providers/capacity"), options(kv, clock)));
  RESPONSE_STEP("handlerLive", capacity::handleProviderCapacity(request("https://ai.ubq.fi/admin/providers/capacity?refresh=live"), options(kv, clock + 1)));
  RESPONSE_STEP("handlerNoKv", capacity::handleProviderCapacity(request("https://ai.ubq.fi/admin/providers/capacity"), options(noKv, clock)));
  {
    auto task = capacity::claudeCapacitySource(nullptr, NOW);
    const contract::ProviderCapacityClaudeSource source = co_await std::move(task);
    check("claudeSourceNoKv", ok(json::value_from(source)));
  }
  {
    auto task = capacity::claudeCapacitySource(kv, NOW);
    const contract::ProviderCapacityClaudeSource source = co_await std::move(task);
    check("claudeSource", ok(json::value_from(source)));
  }
  // S3: one configured Claude account (deterministic UUIDs), its provider health and the admin roster.
  {
    namespace accounts = provider::claude_code_accounts;
    namespace health = provider::health;
    health::resetProviderHealthThrottleForTest();
    provider::claude_code_credentials::ClaudeCodeResolvedCredential credential{
      .kind = "api_key", .secret = "fixture-capacity-credential", .hint = "fixt\xE2\x80\xA6" "tial", .label = "Test key", .source = "kv", .updated_at_ms = NOW};
    provider::claude_code_credentials::ClaudeCodeWorkspaceState workspace{
      .id = "wrkspc_test", .name = "Test workspace", .source = "kv", .discovered_at_ms = std::nullopt, .verified_at_ms = NOW, .last_error = std::nullopt};
    accounts::AccountUpdate update;
    update.createNew = true;
    update.credential = credential;
    update.workspace = workspace;
    update.activate = true;
    const provider::claude_code::ClaudeCodeEnvLookup noEnv = [](const std::string&) { return std::optional<std::string>(); };
    std::optional<accounts::ClaudeCodeAccountBinding> binding;
    {
      rt::crypto::ScopedRandomSource random(uuidSource());
      auto updating = accounts::updateClaudeCodeAccount(kv, update, noEnv);
      binding = co_await std::move(updating);
    }
    check("claudeAccount", ok(json::value_from(*binding)));
    {
      const rt::Promise<void> recording = health::recordClaudeCodeProviderHealth("upstream_error", 503, [&clock] { return clock; }, std::string("req-c"), binding);
      co_await recording.wait();
      check("claudeHealthRecord", undefinedOk());
    }
    const double later = clock + 1;
    {
      auto reading = health::getClaudeCodeProviderHealth([later] { return later; }, binding);
      const health::ProviderHealthView view = co_await std::move(reading);
      check("claudeHealthView", ok(json::value_from(view)));
    }
    {
      auto reading = health::getClaudeCodeProviderHealth([later] { return later; });
      const health::ProviderHealthView view = co_await std::move(reading);
      check("claudeHealthViewDefault", ok(json::value_from(view)));
    }
    {
      auto task = capacity::claudeCapacitySource(kv, clock);
      const contract::ProviderCapacityClaudeSource source = co_await std::move(task);
      check("claudeSourceConfigured", ok(json::value_from(source)));
    }
    RESPONSE_STEP("handlerWithClaude", capacity::handleProviderCapacity(request("https://ai.ubq.fi/admin/providers/capacity"), options(kv, clock + 2)));
    accounts::ClaudeCodeAccountBinding stale = *binding;
    stale.evidence_generation = "stale-generation";
    {
      auto reading = health::getClaudeCodeProviderHealth([later] { return later; }, stale);
      const health::ProviderHealthView view = co_await std::move(reading);
      check("claudeHealthStaleBinding", ok(json::value_from(view)));
    }
    {
      const rt::Promise<void> recording = health::recordClaudeCodeProviderHealth("success", 200, [&clock] { return clock; }, std::nullopt, stale);
      co_await recording.wait();
      check("claudeHealthStaleRecord", undefinedOk());
    }
    health::resetProviderHealthThrottleForTest();
  }
  EXPECT_EQ(index, steps.size());
  rt::env::remove("METERED_API_KEY");
  provider::capacity_events::setProviderCapacitySampleTriggerForTest({});
  kv::setKvForTest(nullptr);
  codex::auth::resetCodexAuthCacheForTest();
  codex::account_routing::resetCodexAccountRoutingForTest();
  rt::globalDateNow() = realNow;
}
