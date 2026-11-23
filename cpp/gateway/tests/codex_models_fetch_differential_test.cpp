// Differential cases for src/models/codex-models-fetch.ts (its direct bun suites are later-phase files). The vectors
// were recorded by running the TypeScript module under bun 1.4.0 with a fixed clock, a routed recording fetch and the
// deterministic KV of tests/helpers/codex_diff_kv, credential-isolated (env -i, bun --no-env-file; HOME, CODEX_HOME and
// CODEX_AUTH_JSON_B64 unset; an empty HOME directory gives identical vectors). Each case mirrors the generator; KV writes
// are compared as a set (masked lease owners).
// Links: tests/codex_models_fetch_differential_test.cpp tests/helpers/codex_diff_kv.cpp src/auth/relay.cpp src/authority/native_codex.cpp
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
//        src/models/codex_models_fetch.cpp src/runtime_config.cpp src/models/codex_models.cpp src/defaults.cpp
#include "tests/helpers/codex_diff_kv.hpp"
#include "uos/codex/auth.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/models/codex_models_availability.hpp"
#include "uos/models/codex_models_fetch.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/health.hpp"
#include "uos/runtime_config.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include "tests/codex_models_fetch_differential_vectors.inc"

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
  namespace capacity_events = uos::provider::capacity_events;
  namespace health = uos::provider::health;
  namespace availability = uos::models::codex_models_availability;
  namespace fetcher = uos::models::codex_models_fetch;
  namespace runtime_config = uos::runtime_config;
  using uos::codex::auth::CodexError;
  using uos::tests::helpers::codex_diff_kv::DiffKv;
  using uos::tests::helpers::codex_diff_kv::plain;
  using uos::types::CodexAuthState;
  using Snapshot = uos::models::codex_models::CodexModelsSnapshot;

  constexpr double NOW = 1'790'000'000'000;
  const std::string OAUTH = "https://auth.openai.com/oauth/token";

  const json::value& vectors() {
    static const json::value parsed = rt::jsonParse(CODEX_MODELS_FETCH_VECTORS);
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

  json::value undefinedTag() { return json::object{{"$undefined", true}}; }
  json::value optionalJson(const std::optional<std::string>& value) { return value.has_value() ? json::value(*value) : undefinedTag(); }

  json::value caught(const rt::Reason& error) {
    if (!rt::isError(error)) {
      const std::optional<json::value> thrown = rt::thrownValue(error);
      return json::object{{"thrown", error && thrown.has_value() ? *thrown : undefinedTag()}};
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
    CredentialIsolation() {
      uos::config::config.codexAuthJsonB64.clear();
      uos::config::config.codexBaseUrl = "https://chatgpt.com/backend-api/codex";
      rt::globalDateNow() = [] { return NOW; };
    }
    ~CredentialIsolation() {
      uos::config::config.codexAuthJsonB64 = savedSeed;
      uos::config::config.codexBaseUrl = savedBaseUrl;
    }
  };

  std::vector<uos::types::ResponseInputItem> itemsOf(const json::array& items) {
    std::vector<uos::types::ResponseInputItem> converted;
    for (const json::value& item : items) converted.push_back(json::value_to<uos::types::ResponseInputItem>(item));
    return converted;
  }
}

ASYNC_TEST("codex-models-fetch differential: buildCodexRequest and preserveCodexDefaultModel") {
  const CredentialIsolation isolation;
  const std::vector<uos::types::ResponseInputItem> items = itemsOf(vectors().as_object().at("buildItems").as_array());
  for (const json::value& row : section("buildRequest")) {
    const std::string name = nameOf(row);
    json::object body;
    if (name == "none") {
      body = fetcher::buildCodexRequest("gpt-5", items);
    } else {
      fetcher::BuildCodexRequestOptions options;
      const json::object& wanted = row.at("options").as_object();
      if (const json::value* reasoning = rt::get(wanted, "reasoning")) {
        options.reasoning = reasoning->is_null() ? rt::Nullable<json::object>(nullptr) : rt::Nullable<json::object>(reasoning->as_object());
      }
      if (const json::value* instructions = rt::get(wanted, "instructions")) {
        options.instructions = instructions->is_null() ? rt::Nullable<std::string>(nullptr) : rt::Nullable<std::string>(text(*instructions));
      }
      body = fetcher::buildCodexRequest("gpt-5", items, options);
    }
    EXPECT(same(row.at("body"), body, "buildRequest " + name));
    EXPECT_EQ(rt::jsonStringify(body), rt::jsonStringify(row.at("body")));
  }

  const json::object snapshot = vectors().as_object().at("preserveSnapshot").as_object();
  for (const json::value& row : section("preserve")) {
    const json::value& candidate = row.at("candidate");
    const std::optional<std::string> wanted = candidate.is_string() ? std::optional<std::string>(text(candidate)) : std::nullopt;
    EXPECT(same(row.at("result"), optionalJson(fetcher::preserveCodexDefaultModel(snapshot, wanted)), "preserve " + rt::jsonStringify(candidate)));
  }
  for (const json::value& row : section("preserveBroken")) {
    const std::string name = nameOf(row);
    json::object broken;
    if (name == "string") broken["models"] = "nope";
    if (name == "null") broken["models"] = nullptr;
    json::object actual{{"name", name}};
    try {
      actual["result"] = optionalJson(fetcher::preserveCodexDefaultModel(broken, std::string("gpt-5")));
    } catch (...) {
      actual["error"] = caught(std::current_exception());
    }
    actual["blank"] = optionalJson(fetcher::preserveCodexDefaultModel(broken, std::string(" ")));
    EXPECT(same(row, actual, "preserveBroken " + name));
  }
  co_return;
}

ASYNC_TEST("codex-models-fetch differential: loadFullCodexModelsSnapshot") {
  const CredentialIsolation isolation;
  for (const json::value& row : section("loadFull")) {
    const std::string name = nameOf(row);
    std::optional<Snapshot> snapshot;
    if (name == "null_override") {
      rt::Promise<std::optional<Snapshot>> loading(fetcher::loadFullCodexModelsSnapshot(std::shared_ptr<rt::Kv>()));
      snapshot = co_await loading.wait();
    } else if (name == "process_kv") {
      const auto kv = std::make_shared<DiffKv>();
      rt::Task<rt::KvCommitResult> setting = kv->set(auth::CODEX_MODELS_KV_KEY, rt::KvValue(row.at("stored")));
      co_await std::move(setting);
      uos::kv::setKvForTest(kv);
      rt::Promise<std::optional<Snapshot>> loading(fetcher::loadFullCodexModelsSnapshot());
      snapshot = co_await loading.wait();
      uos::kv::setKvForTest(nullptr);
    } else {
      const auto kv = std::make_shared<DiffKv>();
      if (!row.at("stored").is_null()) {
        rt::Task<rt::KvCommitResult> setting = kv->set(auth::CODEX_MODELS_KV_KEY, rt::KvValue(row.at("stored")));
        co_await std::move(setting);
      }
      rt::Promise<std::optional<Snapshot>> loading(fetcher::loadFullCodexModelsSnapshot(std::shared_ptr<rt::Kv>(kv)));
      snapshot = co_await loading.wait();
    }
    EXPECT(same(row.at("snapshot"), snapshot.has_value() ? json::value(*snapshot) : json::value(nullptr), "loadFull " + name));
  }
}

ASYNC_TEST("codex-models-fetch differential: storeCodexModelsSnapshot and loadCodexModelsSnapshot") {
  const CredentialIsolation isolation;
  const auto kv = std::make_shared<DiffKv>();
  uos::kv::setKvForTest(kv);
  runtime_config::resetRuntimeConfigCacheForTest();
  for (const json::value& row : section("store")) {
    const std::string name = nameOf(row);
    const double before = kv->version;
    json::object actual{{"name", name}, {"snapshot", row.at("snapshot")}};
    rt::Reason failure;
    bool stored = false;
    try {
      rt::Promise<bool> storing(fetcher::storeCodexModelsSnapshot(row.at("snapshot").as_object()));
      stored = co_await storing.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    actual["result"] = failure ? json::value(json::object{{"error", caught(failure)}}) : json::value(stored);
    runtime_config::resetRuntimeConfigCacheForTest();
    actual["writes"] = writesAfter(*kv, before);
    rt::Promise<std::optional<Snapshot>> loading(fetcher::loadCodexModelsSnapshot());
    const std::optional<Snapshot> loaded = co_await loading.wait();
    actual["loaded"] = loaded.has_value() ? json::value(*loaded) : json::value(nullptr);
    runtime_config::resetRuntimeConfigCacheForTest();
    json::object wanted = row.as_object();
    wanted["writes"] = sortedWrites(row.at("writes"));
    EXPECT(same(wanted, actual, "store " + name));
  }
  uos::kv::setKvForTest(nullptr);
  runtime_config::resetRuntimeConfigCacheForTest();
}

namespace {
  struct RouteState {
    std::string scenario;
    json::array requests;
  };

  rt::Response jsonResponse(const json::value& value, double status = 200, std::vector<std::pair<std::string, std::string>> extra = {}) {
    rt::ResponseInit init;
    init.status = status;
    rt::Headers headers{{"content-type", "application/json"}};
    for (const auto& [name, value_] : extra) headers.set(name, value_);
    init.headers = headers;
    return rt::Response(rt::BodyInit(rt::jsonStringify(value)), init);
  }

  rt::Response textResponse(const std::string& body, double status, std::vector<std::pair<std::string, std::string>> headerList = {},
                            std::optional<std::string> statusText = std::nullopt) {
    rt::ResponseInit init;
    init.status = status;
    if (statusText.has_value()) init.statusText = *statusText;
    if (!headerList.empty()) {
      rt::Headers headers;
      for (const auto& [name, value] : headerList) headers.append(name, value);
      init.headers = headers;
    }
    return rt::Response(rt::BodyInit(body), init);
  }

  // The generator's per-scenario upstream (models URL by bearer; the OAuth token endpoint).
  rt::Response route(const std::string& scenario, const std::string& url, const std::optional<std::string>& bearer) {
    if (scenario == "single_ok") {
      return jsonResponse(json::object{{"models", json::array{json::object{{"slug", "gpt-5"}}, json::object{{"id", "gpt-4.1"}}, json::object{{"slug", " gpt-5 "}},
                                                               json::object{{"name", ""}}}},
                                       {"extra", 1}},
                          200, {{"etag", "W/\"cat\""}});
    }
    if (scenario == "single_if_none_match") {
      rt::ResponseInit init;
      init.status = 304;
      return rt::Response(nullptr, init);
    }
    if (scenario == "single_bad_version") return jsonResponse(json::object{{"models", json::array{}}});
    if (scenario == "single_401_refresh") {
      if (url == OAUTH) return jsonResponse(json::object{{"access_token", "access-a2"}, {"refresh_token", "refresh-a2"}});
      return bearer == "Bearer access-a2" ? jsonResponse(json::object{{"models", json::array{json::object{{"slug", "o3"}}}}}) : textResponse("expired", 401);
    }
    if (scenario == "single_transport" || scenario == "union_transport") throw rt::TypeError("network down");
    if (scenario == "union_ok") {
      if (bearer == "Bearer access-a") {
        return jsonResponse(json::object{{"models", json::array{json::object{{"slug", "gpt-5"}, {"from", "a"}}, json::object{{"slug", "gpt-4.1"}}}}, {"top", "a"}});
      }
      return jsonResponse(
        json::object{{"models", json::array{json::object{{"slug", "gpt-5"}, {"from", "b"}}, json::object{{"id", "o3"}}, json::value("junk")}}, {"top", "b"}, {"other", true}});
    }
    if (scenario == "union_one_429") {
      return bearer == "Bearer access-a" ? textResponse("slow down", 429) : jsonResponse(json::object{{"models", json::array{json::object{{"slug", "o3"}}}}});
    }
    if (scenario == "union_all_fail") return bearer == "Bearer access-a" ? textResponse("boom", 500) : textResponse("busy", 503);
    if (scenario == "union_non_catalog") {
      return bearer == "Bearer access-a" ? jsonResponse(json::object{{"data", json::array{}}}) : jsonResponse(json::object{{"models", json::array{json::object{{"slug", "o3"}}}}});
    }
    // validateCodexAuthJson cases
    if (scenario == "ok_json") {
      return jsonResponse(json::object{{"models", json::array{json::object{{"slug", "gpt-5"}}}}}, 200,
                          {{"content-type", "application/json; charset=utf-8"}, {"etag", "W/\"v\""}});
    }
    if (scenario == "ok_text") return textResponse("plain body", 200, {{"content-type", "text/plain"}});
    if (scenario == "ok_invalid_json") return textResponse("{bad", 200, {{"content-type", "application/json"}});
    if (scenario == "ok_empty_json") return textResponse("", 200, {{"content-type", "application/json"}});
    if (scenario == "fail_500") return textResponse("upstream exploded", 500);
    if (scenario == "fail_empty_body") return textResponse("", 502, {}, std::string("Bad Gateway"));
    if (scenario == "fail_long_body") return textResponse(std::string(9'000, 'x'), 500);
    if (scenario == "refresh_fails") {
      return url == OAUTH ? textResponse(rt::jsonStringify(json::object{{"error", "invalid_grant"}}), 400) : textResponse("expired", 401);
    }
    if (scenario == "refresh_ok") {
      if (url == OAUTH) return jsonResponse(json::object{{"access_token", "access-v2"}});
      return bearer == "Bearer access-v2" ? jsonResponse(json::object{{"models", json::array{}}}, 200, {{"etag", "e2"}}) : textResponse("expired", 401);
    }
    if (scenario == "refresh_ok_retry_throws") {
      if (url == OAUTH) return jsonResponse(json::object{{"access_token", "access-v3"}, {"refresh_token", "refresh-v3"}});
      if (bearer == "Bearer access-v3") throw rt::TypeError("network down");
      return textResponse("expired", 401);
    }
    if (scenario == "refresh_ok_still_401") return url == OAUTH ? jsonResponse(json::object{{"access_token", "access-v4"}}) : textResponse("still expired", 401);
    throw rt::JsError("unrouted scenario " + scenario);
  }

  rt::Task<rt::Response> routedFetch(std::shared_ptr<RouteState> state, rt::RequestInfo input, rt::RequestInit init) {
    const bool hasBody = init.body.has_value();
    const rt::Request request(std::get<std::string>(input), init);
    const std::optional<std::string> bearer = request.headers().get("authorization");
    const std::optional<std::string> ifNoneMatch = request.headers().get("if-none-match");
    const std::optional<std::string> userAgent = request.headers().get("user-agent");
    json::value body = nullptr;
    if (hasBody) {
      auto reading = request.text();
      body = co_await std::move(reading);
    }
    state->requests.push_back(json::object{{"url", request.url()},
                                           {"method", request.method()},
                                           {"authorization", bearer.has_value() ? json::value(*bearer) : json::value(nullptr)},
                                           {"ifNoneMatch", ifNoneMatch.has_value() ? json::value(*ifNoneMatch) : json::value(nullptr)},
                                           {"userAgent", userAgent.has_value() ? json::value(*userAgent) : json::value(nullptr)},
                                           {"body", body}});
    co_return route(state->scenario, request.url(), bearer);
  }
}

ASYNC_TEST("codex-models-fetch differential: fetchCodexModels single account and pool union") {
  const CredentialIsolation isolation;
  capacity_events::setProviderCapacitySampleTriggerForTest([](const capacity_events::ProviderCapacitySampleTriggerOptions&) {});
  for (const json::value& row : section("fetchModels")) {
    const std::string name = nameOf(row);
    const auto kv = std::make_shared<DiffKv>();
    uos::kv::setKvForTest(kv);
    auth::resetCodexAuthCacheForTest();
    availability::resetCodexAccountModelsCacheForTest();
    health::resetProviderHealthThrottleForTest();
    const rt::KvValue poolValue(json::value(json::object{{"accounts", row.at("accounts")}, {"updated_at_ms", NOW - 5'000}}));
    rt::Task<rt::KvCommitResult> setting = kv->set(auth::CODEX_AUTH_POOL_KV_KEY, poolValue);
    co_await std::move(setting);
    const double before = kv->version;
    const auto state = std::make_shared<RouteState>();
    state->scenario = name;
    const rt::GlobalFetchOverride override_(
      rt::Fetch([state](rt::RequestInfo input, rt::RequestInit init) { return routedFetch(state, std::move(input), std::move(init)); }));
    const auto callbacks = std::make_shared<int>(0);
    std::optional<fetcher::FetchCodexModelsOptions> options;
    if (row.at("options").is_object()) {
      const json::object& wanted = row.at("options").as_object();
      fetcher::FetchCodexModelsOptions built;
      if (const json::value* clientVersion = rt::get(wanted, "clientVersion")) built.clientVersion = text(*clientVersion);
      if (const json::value* ifNoneMatch = rt::get(wanted, "ifNoneMatch")) built.ifNoneMatch = text(*ifNoneMatch);
      if (rt::get(wanted, "onProviderTransportFailure") != nullptr) built.onProviderTransportFailure = [callbacks] { *callbacks += 1; };
      options = built;
    }
    json::object actual{{"name", name}, {"accounts", row.at("accounts")}, {"options", row.at("options")}};
    std::optional<rt::Response> response;
    rt::Reason failure;
    try {
      rt::Promise<rt::Response> fetching(options.has_value() ? fetcher::fetchCodexModels(*options) : fetcher::fetchCodexModels());
      response = co_await fetching.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    if (response.has_value()) {
      rt::Task<json::value> converting = responseJson(*response);
      json::value converted = co_await std::move(converting);
      actual["result"] = json::object{{"response", converted}};
    } else {
      actual["result"] = json::object{{"error", caught(failure)}};
    }
    rt::Task<void> settling = settle(40);
    co_await std::move(settling);
    actual["callbacks"] = *callbacks;
    actual["requests"] = state->requests;
    actual["writes"] = writesAfter(*kv, before);
    json::object wanted = row.as_object();
    wanted["writes"] = sortedWrites(row.at("writes"));
    EXPECT(same(wanted, actual, "fetchModels " + name));
    uos::kv::setKvForTest(nullptr);
  }
  auth::resetCodexAuthCacheForTest();
  availability::resetCodexAccountModelsCacheForTest();
  capacity_events::setProviderCapacitySampleTriggerForTest({});
}

ASYNC_TEST("codex-models-fetch differential: validateCodexAuthJson") {
  const CredentialIsolation isolation;
  capacity_events::setProviderCapacitySampleTriggerForTest([](const capacity_events::ProviderCapacitySampleTriggerOptions&) {});
  const CodexAuthState authState{.access_token = "access-v1", .refresh_token = "refresh-v1", .account_id = "acct-v", .updated_at_ms = NOW - 1'000};
  for (const json::value& row : section("validate")) {
    const std::string name = nameOf(row);
    const auto state = std::make_shared<RouteState>();
    state->scenario = name;
    const rt::GlobalFetchOverride override_(
      rt::Fetch([state](rt::RequestInfo input, rt::RequestInit init) { return routedFetch(state, std::move(input), std::move(init)); }));
    fetcher::ValidateCodexAuthJsonOptions options;
    if (row.at("options").is_object()) options.clientVersion = text(row.at("options").at("clientVersion"));
    json::object actual{{"name", name}, {"options", row.at("options")}};
    rt::Reason failure;
    std::optional<fetcher::ValidateCodexAuthJsonResult> result;
    try {
      rt::Promise<fetcher::ValidateCodexAuthJsonResult> validating(row.at("options").is_object() ? fetcher::validateCodexAuthJson(authState, options)
                                                                                                 : fetcher::validateCodexAuthJson(authState));
      result = co_await validating.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    if (!result.has_value()) {
      actual["result"] = json::object{{"error", caught(failure)}};
    } else if (const auto* ok = std::get_if<fetcher::ValidateCodexAuthJsonResultOk>(&*result)) {
      actual["result"] = json::object{{"ok", true},
                                      {"auth", json::value_from(ok->auth)},
                                      {"refreshed", ok->refreshed},
                                      {"status", ok->status},
                                      {"contentType", ok->contentType.has_value() ? json::value(*ok->contentType) : json::value(nullptr)},
                                      {"models", ok->models},
                                      {"modelsBody", ok->modelsBody},
                                      {"etag", ok->etag.has_value() ? json::value(*ok->etag) : json::value(nullptr)},
                                      {"clientVersion", ok->clientVersion}};
    } else {
      const auto& notOk = std::get<fetcher::ValidateCodexAuthJsonResultNotOk>(*result);
      actual["result"] = json::object{{"ok", false}, {"status", notOk.status}, {"body", notOk.body}};
    }
    actual["requests"] = state->requests;
    EXPECT(same(row, actual, "validate " + name));
  }
  capacity_events::setProviderCapacitySampleTriggerForTest({});
}
