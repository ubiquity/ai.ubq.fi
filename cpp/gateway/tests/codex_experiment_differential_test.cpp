// Differential cases for src/codex/experiment.ts (no direct bun suite). The vectors were recorded by running the
// TypeScript module under bun 1.4.0 with a fixed clock, a routed recording fetch and the deterministic KV of
// tests/helpers/codex_diff_kv, credential-isolated (HOME, CODEX_HOME and CODEX_AUTH_JSON_B64 unset). Each case mirrors
// the generator; KV writes are compared as a set.
// Links: tests/codex_experiment_differential_test.cpp tests/helpers/codex_diff_kv.cpp src/auth/relay.cpp src/authority/native_codex.cpp
//        src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp
//        src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_paid.cpp
//        src/authority/native_paid_contracts.cpp src/bounded_response_body.cpp src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp
//        src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp
//        src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp
//        src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp
//        src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp src/codex/reset_settings.cpp src/codex/routing_evaluation.cpp
//        src/codex/routing_mutations.cpp src/codex/routing_serial.cpp src/codex/routing_state.cpp src/config.cpp src/http.cpp
//        src/inference_deadline.cpp src/kv.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp src/optional_telemetry_queue.cpp
//        src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
//        src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
//        src/provider/capacity_rollups.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp
//        src/provider/claude_capacity_rollups.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
//        src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp src/provider/health.cpp src/provider/selection.cpp
//        src/release.cpp src/runtime/http.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
//        src/runtime/http_upgrade.cpp src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
//        src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/types.cpp src/utils.cpp
#include "tests/helpers/codex_diff_kv.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/experiment.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/health.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

#include "tests/codex_experiment_differential_vectors.inc"

#include <cstdio>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace auth = uos::codex::auth;
  namespace experiment = uos::codex::experiment;
  namespace capacity_events = uos::provider::capacity_events;
  namespace health = uos::provider::health;
  using uos::codex::auth::CodexError;
  using uos::tests::helpers::codex_diff_kv::DiffKv;
  using uos::tests::helpers::codex_diff_kv::plain;
  using uos::types::CodexAuthState;

  constexpr double NOW = 1'790'000'000'000;
  const std::string OAUTH = "https://auth.openai.com/oauth/token";

  const json::value& vectors() {
    static const json::value parsed = rt::jsonParse(CODEX_EXPERIMENT_VECTORS);
    return parsed;
  }

  const json::array& section(std::string_view name) { return vectors().as_object().at(name).as_array(); }

  bool same(const json::value& expected, const json::value& actual, const std::string& label) {
    if (rt::deepEqual(expected, actual)) return true;
    std::fprintf(stderr, "    %s differs\n      expected: %s\n      actual:   %s\n", label.c_str(), rt::jsonStringify(expected).c_str(), rt::jsonStringify(actual).c_str());
    return false;
  }

  std::string text(const json::value& value) { return std::string(value.as_string()); }
  std::string nameOf(const json::value& row) { return text(row.at("name")); }

  json::value caught(const rt::Reason& error) {
    if (!rt::isError(error)) {
      const std::optional<json::value> thrown = rt::thrownValue(error);
      return json::object{{"thrown", error && thrown.has_value() ? *thrown : json::value(json::object{{"$undefined", true}})}};
    }
    json::object out;
    out["name"] = rt::errorName(error).value_or("Error");
    out["message"] = rt::errorMessage(error).value_or("");
    try {
      std::rethrow_exception(error);
    } catch (const CodexError& codexError) {
      out["code"] = codexError.code;
      out["status"] = rt::makeNumber(codexError.status);
      if (codexError.causeValue) out["cause"] = caught(codexError.causeValue);
    } catch (...) {
    }
    return out;
  }

  rt::Task<json::value> responseJson(rt::Response response) {
    json::array headers;
    for (const auto& [name, value] : response.headers().entries()) headers.push_back(json::array{json::value(name), json::value(value)});
    auto reading = response.text();
    const std::string body = co_await std::move(reading);
    co_return json::object{{"status", response.status()}, {"headers", std::move(headers)}, {"body", body}};
  }

  rt::Task<void> settle(int turns) {
    for (int index = 0; index < turns; ++index) co_await rt::microtask();
  }

  json::array writesAfter(const DiffKv& kv, double before) {
    std::map<std::string, json::value> sorted;
    for (const auto& [key, row] : kv.rows) {
      if (rt::stringToNumber(row.versionstamp) > before) sorted[key] = plain(row.value.toJson());
    }
    json::array writes;
    for (auto& [key, value] : sorted) writes.push_back(json::object{{"key", key}, {"value", value}});
    return writes;
  }

  json::array sortedWrites(const json::value& writes) {
    std::map<std::string, json::value> sorted;
    for (const json::value& write : writes.as_array()) sorted[text(write.at("key"))] = write.at("value");
    json::array out;
    for (auto& [key, value] : sorted) out.push_back(json::object{{"key", key}, {"value", value}});
    return out;
  }

  // Credential isolation (mandatory): nothing here may reach this host's ~/.codex/auth.json.
  struct CredentialIsolation {
    rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    std::string savedBaseUrl = uos::config::config.codexBaseUrl;
    bool savedDeploy = uos::config::config.isDeploy;
    CredentialIsolation() {
      uos::config::config.codexAuthJsonB64.clear();
      uos::config::config.codexBaseUrl = "https://chatgpt.com/backend-api/codex";
      uos::config::config.isDeploy = false;
      rt::globalDateNow() = [] { return NOW; };
      capacity_events::setProviderCapacitySampleTriggerForTest([](const capacity_events::ProviderCapacitySampleTriggerOptions&) {});
    }
    ~CredentialIsolation() {
      capacity_events::setProviderCapacitySampleTriggerForTest({});
      uos::kv::setKvForTest(nullptr);
      auth::resetCodexAuthCacheForTest();
      uos::config::config.codexAuthJsonB64 = savedSeed;
      uos::config::config.codexBaseUrl = savedBaseUrl;
      uos::config::config.isDeploy = savedDeploy;
    }
  };

  CodexAuthState accountOf(const std::string& key) { return json::value_to<CodexAuthState>(vectors().as_object().at("accounts").at(key)); }

  void resetModules() {
    auth::resetCodexAuthCacheForTest();
    uos::codex::overage_settings::resetOverageUsageCacheForTest();
    health::resetProviderHealthThrottleForTest();
  }

  // The generator's freshKv: a new process KV holding `accounts` as the pool (none: no pool row).
  rt::Task<std::shared_ptr<DiffKv>> freshKv(std::optional<std::vector<CodexAuthState>> accounts) {
    const auto kv = std::make_shared<DiffKv>();
    uos::kv::setKvForTest(kv);
    resetModules();
    if (accounts.has_value()) {
      const uos::types::CodexAuthPoolState pool{.accounts = *accounts, .updated_at_ms = NOW - 5'000};
      const rt::KvValue poolValue(json::value_from(pool));
      rt::Task<rt::KvCommitResult> setting = kv->set(auth::CODEX_AUTH_POOL_KV_KEY, poolValue);
      co_await std::move(setting);
    }
    co_return kv;
  }

  json::value sessionJson(const experiment::CodexCacheScopeExperimentSession& session) {
    return json::object{{"expectedAccountIds", json::array{json::value(session.expectedAccountIds[0]), json::value(session.expectedAccountIds[1])}},
                        {"authPoolVersionstamp", session.authPoolVersionstamp}};
  }

  struct RouteState {
    std::string scenario;
    json::array requests;
  };

  rt::Response respond(const std::string& body, double status, std::vector<std::pair<std::string, std::string>> headerList = {}) {
    rt::ResponseInit init;
    init.status = status;
    if (!headerList.empty()) {
      rt::Headers headers;
      for (const auto& [name, value] : headerList) headers.append(name, value);
      init.headers = headers;
    }
    return rt::Response(rt::BodyInit(body), init);
  }

  rt::Response route(const std::string& scenario, const std::string& url) {
    if (scenario == "ok_slot_1") return respond("data: {}\n\n", 200, {{"content-type", "text/event-stream"}});
    if (scenario == "ok_slot_2") return respond("data: {}\n\n", 200);
    if (scenario == "quota_429") {
      return respond(rt::jsonStringify(json::object{{"error", json::object{{"type", "usage_limit_reached"}, {"message", "limit"}, {"resets_in_seconds", 3'600}}}}), 429,
                     {{"content-type", "application/json"}});
    }
    if (scenario == "server_500") return respond("boom", 500);
    if (scenario == "transport_error") throw rt::TypeError("network down");
    if (scenario == "refreshed") {
      return url == OAUTH ? respond(rt::jsonStringify(json::object{{"access_token", "access-b2"}, {"refresh_token", "refresh-b2"}}), 200) : respond("x", 404);
    }
    if (scenario == "same_tokens") return url == OAUTH ? respond(rt::jsonStringify(json::object{{"access_token", accountOf("a").access_token}}), 200) : respond("x", 404);
    if (scenario == "oauth_rejected") return url == OAUTH ? respond(rt::jsonStringify(json::object{{"error", "invalid_grant"}}), 400) : respond("x", 404);
    return respond("x", 200);
  }

  rt::Task<rt::Response> routedFetch(std::shared_ptr<RouteState> state, rt::RequestInfo input, rt::RequestInit init) {
    const bool hasBody = init.body.has_value();
    const rt::Request request(std::get<std::string>(input), init);
    json::array headers;
    for (const auto& [name, value] : request.headers().entries()) headers.push_back(json::array{json::value(name), json::value(value)});
    json::value body = nullptr;
    if (hasBody) {
      auto reading = request.text();
      body = co_await std::move(reading);
    }
    state->requests.push_back(json::object{{"url", request.url()}, {"method", request.method()}, {"headers", std::move(headers)}, {"body", body}});
    co_return route(state->scenario, request.url());
  }
}

ASYNC_TEST("experiment differential: capacity accounts and session start") {
  const CredentialIsolation isolation;
  const CodexAuthState a = accountOf("a");
  const CodexAuthState b = accountOf("b");
  const CodexAuthState c = accountOf("c");
  for (const json::value& row : section("capacityAccounts")) {
    const std::string name = nameOf(row);
    std::optional<std::vector<CodexAuthState>> accounts;
    if (name == "two") accounts = std::vector<CodexAuthState>{a, b};
    if (name == "opaque") accounts = std::vector<CodexAuthState>{c};
    if (name == "three_invalid") accounts = std::vector<CodexAuthState>{a, b, c};
    rt::Task<std::shared_ptr<DiffKv>> making = freshKv(accounts);
    co_await std::move(making);
    json::object actual{{"name", name}};
    try {
      rt::Promise<std::vector<experiment::CodexCapacityAccount>> loading(experiment::getCodexCapacityAccounts());
      const std::vector<experiment::CodexCapacityAccount> capacityAccounts = co_await loading.wait();
      json::array list;
      for (const experiment::CodexCapacityAccount& account : capacityAccounts) list.push_back(json::value_from(account));
      actual["accounts"] = std::move(list);
    } catch (...) {
      actual["error"] = caught(std::current_exception());
    }
    EXPECT(same(row, actual, "capacityAccounts " + name));
  }

  for (const json::value& row : section("begin")) {
    const std::string name = nameOf(row);
    if (name == "no_kv") {
      uos::kv::setKvForTest(nullptr);
      resetModules();
    } else {
      std::optional<std::vector<CodexAuthState>> accounts;
      if (name == "one_slot") accounts = std::vector<CodexAuthState>{a};
      if (name == "three_slots") accounts = std::vector<CodexAuthState>{a, b, c};
      if (name == "two_slots") accounts = std::vector<CodexAuthState>{a, b};
      rt::Task<std::shared_ptr<DiffKv>> making = freshKv(accounts);
      co_await std::move(making);
    }
    json::object actual{{"name", name}};
    try {
      rt::Promise<experiment::CodexCacheScopeExperimentSession> beginning(experiment::beginCodexCacheScopeExperiment());
      const experiment::CodexCacheScopeExperimentSession session = co_await beginning.wait();
      actual["session"] = sessionJson(session);
    } catch (...) {
      actual["error"] = caught(std::current_exception());
    }
    EXPECT(same(row, actual, "begin " + name));
  }
}

ASYNC_TEST("experiment differential: slot-pinned dispatch") {
  const CredentialIsolation isolation;
  const CodexAuthState a = accountOf("a");
  const CodexAuthState b = accountOf("b");
  for (const json::value& row : section("fetchExperiment")) {
    const std::string name = nameOf(row);
    rt::Task<std::shared_ptr<DiffKv>> making = freshKv(std::vector<CodexAuthState>{a, b});
    const std::shared_ptr<DiffKv> kv = co_await std::move(making);
    rt::Promise<experiment::CodexCacheScopeExperimentSession> beginning(experiment::beginCodexCacheScopeExperiment());
    const experiment::CodexCacheScopeExperimentSession session = co_await beginning.wait();
    EXPECT(same(row.at("session"), sessionJson(session), "fetchExperiment session " + name));
    if (row.at("stale").as_bool()) {
      const uos::types::CodexAuthPoolState pool{.accounts = {a, b}, .updated_at_ms = NOW - 4'000};
      const rt::KvValue poolValue(json::value_from(pool));
      rt::Task<rt::KvCommitResult> setting = kv->set(auth::CODEX_AUTH_POOL_KV_KEY, poolValue);
      co_await std::move(setting);
    }
    const double before = kv->version;
    const auto state = std::make_shared<RouteState>();
    state->scenario = name;
    const rt::GlobalFetchOverride override_(
      rt::Fetch([state](rt::RequestInfo input, rt::RequestInit init) { return routedFetch(state, std::move(input), std::move(init)); }));
    rt::console::Capture capture({rt::console::Method::info});
    experiment::FetchCodexResponsesForCacheScopeExperimentOptions options{
      .session = session, .slot = *rt::getNumber(row.at("slot")), .conversationId = text(row.at("conversationId"))};
    if (!row.at("clientVersion").is_null()) options.clientVersion = text(row.at("clientVersion"));
    const json::value body = row.at("body").is_null() ? json::value(json::object{{"model", "gpt-5"}}) : row.at("body");
    json::object actual{{"name", name},
                        {"slot", row.at("slot")},
                        {"conversationId", row.at("conversationId")},
                        {"clientVersion", row.at("clientVersion")},
                        {"stale", row.at("stale")},
                        {"body", row.at("body")},
                        {"session", sessionJson(session)}};
    std::optional<rt::Response> response;
    rt::Reason failure;
    try {
      rt::Promise<rt::Response> fetching(experiment::fetchCodexResponsesForCacheScopeExperiment(body, options));
      response = co_await fetching.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    if (response.has_value()) {
      rt::Task<json::value> converting = responseJson(*response);
      json::value converted = co_await std::move(converting);
      const std::optional<double> slot = auth::getCodexResponseSlot(*response);
      actual["result"] = json::object{{"response", converted}, {"slot", slot.has_value() ? json::value(*slot) : json::value(nullptr)}};
    } else {
      actual["result"] = json::object{{"error", caught(failure)}};
    }
    rt::Task<void> settling = settle(40);
    co_await std::move(settling);
    json::array logs;
    for (const rt::console::Call& call : capture.calls()) {
      json::array args;
      for (const rt::console::Value& arg : call.args) args.push_back(json::value(arg.string()));
      logs.push_back(std::move(args));
    }
    actual["requests"] = state->requests;
    actual["logs"] = std::move(logs);
    actual["writes"] = writesAfter(*kv, before);
    json::object wanted = row.as_object();
    wanted["writes"] = sortedWrites(row.at("writes"));
    EXPECT(same(wanted, actual, "fetchExperiment " + name));
  }
}

ASYNC_TEST("experiment differential: pinned slot refresh") {
  const CredentialIsolation isolation;
  const CodexAuthState a = accountOf("a");
  const CodexAuthState b = accountOf("b");
  for (const json::value& row : section("refreshExperiment")) {
    const std::string name = nameOf(row);
    rt::Task<std::shared_ptr<DiffKv>> making = freshKv(std::vector<CodexAuthState>{a, b});
    const std::shared_ptr<DiffKv> kv = co_await std::move(making);
    rt::Promise<experiment::CodexCacheScopeExperimentSession> beginning(experiment::beginCodexCacheScopeExperiment());
    const experiment::CodexCacheScopeExperimentSession session = co_await beginning.wait();
    const double before = kv->version;
    const auto state = std::make_shared<RouteState>();
    state->scenario = name;
    const rt::GlobalFetchOverride override_(
      rt::Fetch([state](rt::RequestInfo input, rt::RequestInit init) { return routedFetch(state, std::move(input), std::move(init)); }));
    json::object actual{{"name", name}, {"slot", row.at("slot")}, {"session", sessionJson(session)}};
    try {
      rt::Promise<experiment::CodexCacheScopeExperimentRefreshResult> refreshing(
        experiment::refreshCodexCacheScopeExperimentSlot(session, *rt::getNumber(row.at("slot"))));
      const experiment::CodexCacheScopeExperimentRefreshResult result = co_await refreshing.wait();
      if (const auto* refreshed = std::get_if<experiment::CodexCacheScopeExperimentRefreshResultRefreshed>(&result)) {
        actual["result"] = json::object{{"status", "refreshed"}, {"tokenChanged", refreshed->tokenChanged}, {"session", sessionJson(refreshed->session)}};
      } else {
        actual["result"] = json::object{{"status", "auth_pool_drift"}};
      }
    } catch (...) {
      actual["result"] = json::object{{"error", caught(std::current_exception())}};
    }
    rt::Task<void> settling = settle(40);
    co_await std::move(settling);
    actual["requests"] = state->requests;
    actual["writes"] = writesAfter(*kv, before);
    json::object wanted = row.as_object();
    wanted["writes"] = sortedWrites(row.at("writes"));
    EXPECT(same(wanted, actual, "refreshExperiment " + name));
  }
}
