// End-to-end differential cases for the Codex Responses transport: src/codex/index.ts fetchCodexResponses driving
// src/codex/responses-{state,operations,runners}.ts (which have no direct bun suite) over the deterministic KV of
// tests/helpers/codex_diff_kv and a routed upstream. The vectors were recorded under bun 1.4.0 with a fixed clock,
// credential-isolated (HOME, CODEX_HOME and CODEX_AUTH_JSON_B64 unset). Each case mirrors the generator; KV writes are
// compared as a set and random UUIDs (conversation ids, probe tokens) are masked.
// Links: tests/codex_responses_differential_test.cpp tests/helpers/codex_diff_kv.cpp src/auth/relay.cpp src/authority/native_codex.cpp
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
//        src/codex/index.cpp src/codex/responses_state.cpp src/codex/responses_operations.cpp src/codex/responses_runners.cpp
//        src/codex/banked_reset_pool.cpp src/codex/banked_reset_submission.cpp src/codex/banked_reset_claim.cpp
#include "tests/helpers/codex_diff_kv.hpp"
#include "uos/api_key_policy.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/dispatch.hpp"
#include "uos/codex/index.hpp"
#include "uos/codex/overage_settings.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/models/codex_models_availability.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/health.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/utils.hpp"

#include "tests/codex_responses_differential_vectors.inc"

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
  namespace dispatch = uos::codex::dispatch;
  namespace capacity_events = uos::provider::capacity_events;
  namespace health = uos::provider::health;
  using uos::codex::auth::CodexError;
  using uos::tests::helpers::codex_diff_kv::DiffKv;
  using uos::tests::helpers::codex_diff_kv::plain;
  using uos::types::CodexAuthState;

  constexpr double NOW = 1'790'000'000'000;
  const std::string OAUTH = "https://auth.openai.com/oauth/token";
  const std::string RESPONSES = "https://chatgpt.com/backend-api/codex/responses";

  const json::value& vectors() {
    static const json::value parsed = rt::jsonParse(CODEX_RESPONSES_VECTORS);
    return parsed;
  }

  bool same(const json::value& expected, const json::value& actual, const std::string& label) {
    if (rt::deepEqual(expected, actual)) return true;
    std::fprintf(stderr, "    %s differs\n      expected: %s\n      actual:   %s\n", label.c_str(), rt::jsonStringify(expected).c_str(), rt::jsonStringify(actual).c_str());
    return false;
  }

  std::string text(const json::value& value) { return std::string(value.as_string()); }

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

  rt::Response sse() { return respond("data: {\"type\":\"response.completed\"}\n\n", 200, {{"content-type", "text/event-stream"}}); }
  rt::Response jsonBody(const json::value& value, double status, std::vector<std::pair<std::string, std::string>> extra = {}) {
    std::vector<std::pair<std::string, std::string>> headers{{"content-type", "application/json"}};
    for (auto& header : extra) headers.push_back(std::move(header));
    return respond(rt::jsonStringify(value), status, headers);
  }
  rt::Response usageLimit() {
    return jsonBody(json::object{{"error", json::object{{"type", "usage_limit_reached"},
                                                        {"message", "The usage limit has been reached"},
                                                        {"resets_at", rt::Math::floor(NOW / 1000) + 3'600}}}},
                    429);
  }
  rt::Response unsupported() { return jsonBody(json::object{{"detail", "The 'gpt-5' model is not supported when using Codex with a ChatGPT account."}}, 400); }

  rt::Response route(const std::string& scenario, const std::string& url, const std::optional<std::string>& bearer, int attempt) {
    if (scenario == "refresh_401") {
      if (url == OAUTH) return jsonBody(json::object{{"access_token", "access-a2"}, {"refresh_token", "refresh-a2"}}, 200);
      return bearer == "Bearer access-a2" || bearer == "Bearer access-b" ? sse()
                                                                          : jsonBody(json::object{{"error", json::object{{"message", "expired"}, {"code", "token_expired"}}}}, 401);
    }
    if (scenario == "usage_limit_sibling") return bearer == "Bearer access-a" ? usageLimit() : sse();
    if (scenario == "server_500") return respond("upstream boom", 500);
    if (scenario == "transport_error") throw rt::TypeError("network down");
    if (scenario == "all_quota") return url == RESPONSES ? usageLimit() : respond("not found", 404);
    if (scenario == "model_unsupported") return bearer == "Bearer access-a" ? unsupported() : sse();
    if (scenario == "overage_allowed_all_quota") return url != RESPONSES ? respond("not found", 404) : attempt <= 2 ? usageLimit() : sse();
    if (scenario == "banked_reset_verified") {
      if (url == RESPONSES) return attempt <= 2 ? usageLimit() : sse();
      if (url.ends_with("/rate-limit-reset-credits")) {
        const std::string id = bearer == "Bearer access-a" ? "credit-a" : "credit-b";
        return jsonBody(json::object{{"credits", json::array{json::object{{"id", id}, {"status", "available"}, {"reset_type", "codex_rate_limits"}, {"expires_at", nullptr}}}},
                                     {"available_count", 1}},
                        200);
      }
      if (url.find("/rate-limit-reset-credits/") != std::string::npos) return jsonBody(json::object{{"code", "reset"}}, 200);
      return respond("not found", 404);
    }
    if (scenario == "generic_429_short_retry") {
      return attempt == 1 ? jsonBody(json::object{{"error", json::object{{"type", "rate_limit_exceeded"}, {"message", "slow down"}}}}, 429, {{"retry-after", "1"}})
                          : sse();
    }
    return sse();
  }

  struct Upstream {
    std::string scenario;
    int attempt = 0;
    json::array requests;
  };

  rt::Task<rt::Response> upstreamFetch(std::shared_ptr<Upstream> upstream, rt::RequestInfo input, rt::RequestInit init) {
    static const rt::Regex UUID("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$", "");
    const bool hasBody = init.body.has_value();
    const rt::Request request(std::get<std::string>(input), init);
    const std::optional<std::string> bearer = request.headers().get("authorization");
    if (request.url() == RESPONSES) upstream->attempt += 1;
    const auto pick = [&request](std::string_view name) -> json::value {
      const std::optional<std::string> value = request.headers().get(name);
      if (!value.has_value()) return nullptr;
      return UUID.test(*value) ? json::value("<uuid>") : json::value(*value);
    };
    const auto header = [&request](std::string_view name) -> json::value {
      const std::optional<std::string> value = request.headers().get(name);
      return value.has_value() ? json::value(*value) : json::value(nullptr);
    };
    json::object record{{"url", request.url()},
                        {"method", request.method()},
                        {"authorization", header("authorization")},
                        {"account", header("chatgpt-account-id")},
                        {"conversation", pick("conversation_id")},
                        {"session", pick("session-id")},
                        {"thread", pick("thread-id")},
                        {"clientRequest", pick("x-client-request-id")},
                        {"userAgent", header("user-agent")}};
    if (hasBody) {
      auto reading = request.text();
      record["body"] = co_await std::move(reading);
    } else {
      record["body"] = nullptr;
    }
    upstream->requests.push_back(std::move(record));
    co_return route(upstream->scenario, request.url(), bearer, upstream->attempt);
  }

  rt::Task<void> cancelHook(std::shared_ptr<std::vector<std::string>> log) {
    log->push_back("cancelBeforeTransport");
    co_return;
  }

  rt::Task<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>> beforeDispatchHook(std::shared_ptr<std::vector<std::string>> log) {
    log->push_back("beforeDispatch");
    uos::api_key_policy::ApiKeyProviderDispatch handed;
    handed.markTransportStarted = [log] { log->push_back("markTransportStarted"); };
    handed.cancelBeforeTransport = [log] { return cancelHook(log); };
    co_return handed;
  }

  rt::Task<void> recordSleep(std::shared_ptr<std::vector<std::string>> log, double milliseconds) {
    log->push_back("sleep:" + rt::numberToString(milliseconds));
    co_return;
  }
}

ASYNC_TEST("responses differential: fetchCodexResponses end to end") {
  const CredentialIsolation isolation;
  const CodexAuthState accountA{.access_token = "access-a", .refresh_token = "refresh-a", .account_id = "acct-a", .updated_at_ms = NOW - 10'000};
  const CodexAuthState accountB{.access_token = "access-b", .refresh_token = "refresh-b", .account_id = "acct-b", .updated_at_ms = NOW - 10'000};
  for (const json::value& row : vectors().as_object().at("responses").as_array()) {
    const std::string name = text(row.at("name"));
    const auto kv = std::make_shared<DiffKv>();
    uos::kv::setKvForTest(kv);
    auth::resetCodexAuthCacheForTest();
    uos::codex::overage_settings::resetOverageUsageCacheForTest();
    health::resetProviderHealthThrottleForTest();
    uos::models::codex_models_availability::resetCodexAccountModelsCacheForTest();
    const uos::types::CodexAuthPoolState pool{.accounts = {accountA, accountB}, .updated_at_ms = NOW - 5'000};
    const rt::KvValue poolValue(json::value_from(pool));
    rt::Task<rt::KvCommitResult> setting = kv->set(auth::CODEX_AUTH_POOL_KV_KEY, poolValue);
    co_await std::move(setting);
    if (row.at("overage").as_bool()) {
      for (const CodexAuthState& account : pool.accounts) {
        rt::Promise<std::string> hashing(uos::utils::sha256Hex(account.account_id));
        const std::string adminHash = co_await hashing.wait();
        rt::Promise<void> writing(uos::codex::overage_settings::writeOverageUsage(kv, adminHash, true));
        co_await writing.wait();
      }
    }
    const double before = kv->version;
    const auto upstream = std::make_shared<Upstream>();
    upstream->scenario = name;
    const rt::GlobalFetchOverride override_(
      rt::Fetch([upstream](rt::RequestInfo input, rt::RequestInit init) { return upstreamFetch(upstream, std::move(input), std::move(init)); }));
    const auto log = std::make_shared<std::vector<std::string>>();
    dispatch::FetchCodexResponsesOptions options;
    if (name == "native_session_warnings") {
      options.cacheScope = "scope-1";
      options.clientVersion = "0.101.0";
      options.requestId = "req-1";
    }
    if (name == "dispatch_hooks") {
      options.beforeDispatch = std::function<rt::Task<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>>()>([log] { return beforeDispatchHook(log); });
      options.timing = dispatch::CodexResponseTimingHooks{.onDispatch = [log] { log->push_back("onDispatch"); }, .onHeaders = [log] { log->push_back("onHeaders"); }};
    }
    if (name == "aborted_signal") options.signal = rt::AbortSignal::abort(rt::makeReason(rt::DOMException("client gone", "AbortError")));
    if (name == "generic_429_short_retry") options.retrySleep = [log](double milliseconds) { return recordSleep(log, milliseconds); };
    rt::console::Capture capture({rt::console::Method::info});
    json::object actual{{"name", name}, {"body", row.at("body")}, {"options", row.at("options")}, {"overage", row.at("overage")}};
    std::optional<rt::Response> response;
    rt::Reason failure;
    try {
      rt::Promise<rt::Response> fetching(uos::codex::index::fetchCodexResponses(row.at("body"), options));
      response = co_await fetching.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    if (response.has_value()) {
      json::array headers;
      for (const auto& [headerName, value] : response->headers().entries()) headers.push_back(json::array{json::value(headerName), json::value(value)});
      auto reading = response->text();
      const std::string responseBody = co_await std::move(reading);
      const std::optional<std::string> routingError = auth::getCodexRoutingError(*response);
      const std::optional<double> slot = auth::getCodexResponseSlot(*response);
      const auth::CodexResponseActiveTelemetry active = auth::getCodexResponseActiveTelemetry(*response);
      actual["result"] = json::object{
        {"response", json::object{{"status", response->status()}, {"headers", std::move(headers)}, {"body", responseBody}}},
        {"routingError", routingError.has_value() ? json::value(*routingError) : json::value(nullptr)},
        {"slot", slot.has_value() ? json::value(*slot) : json::value(nullptr)},
        {"active", json::object{{"activeGeneration", active.activeGeneration.has_value() ? json::value(*active.activeGeneration) : json::value(nullptr)},
                                {"activeTransitionReason", active.activeTransitionReason.has_value() ? json::value(*active.activeTransitionReason) : json::value(nullptr)}}}};
    } else {
      actual["result"] = json::object{{"error", caught(failure)}};
    }
    rt::Task<void> settling = settle(60);
    co_await std::move(settling);
    json::array entries;
    for (const std::string& entry : *log) entries.push_back(json::value(entry));
    json::array logs;
    for (const rt::console::Call& call : capture.calls()) {
      json::array args;
      for (const rt::console::Value& arg : call.args) args.push_back(json::value(arg.string()));
      logs.push_back(plain(json::value(std::move(args))));
    }
    actual["log"] = std::move(entries);
    actual["requests"] = upstream->requests;
    actual["logs"] = std::move(logs);
    actual["writes"] = writesAfter(*kv, before);
    json::object wanted = row.as_object();
    wanted["writes"] = sortedWrites(row.at("writes"));
    EXPECT(same(wanted, actual, "responses " + name));
    uos::kv::setKvForTest(nullptr);
  }
}
