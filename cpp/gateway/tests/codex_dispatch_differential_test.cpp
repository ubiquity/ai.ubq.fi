// Differential cases for src/codex/dispatch.ts (of its direct bun tests, codex-retry-after is ported in
// tests/codex_retry_after_test.cpp and codex-model-availability is a later-phase file). The vectors were recorded by
// running the TypeScript module under bun 1.4.0 with a fixed clock, a recording fetch and the deterministic KV of
// tests/helpers/codex_diff_kv, credential-isolated (env -i, bun --no-env-file; HOME, CODEX_HOME and CODEX_AUTH_JSON_B64
// unset; an empty HOME directory gives identical vectors). Each case below mirrors the generator step for step.
// KV writes are compared as a set: the fake KV here completes operations without the turns the bun fake takes.
// Links: tests/codex_dispatch_differential_test.cpp tests/helpers/codex_diff_kv.cpp src/auth/relay.cpp src/authority/native_codex.cpp
//        src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp
//        src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_paid.cpp src/authority/native_paid_contracts.cpp
//        src/bounded_response_body.cpp src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp src/cache/prompt_analytics_queue.cpp
//        src/cache/prompt_analytics_read.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp src/codex/auth.cpp src/codex/auth_refresh.cpp
//        src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp src/codex/capacity_routing.cpp src/codex/dispatch.cpp
//        src/codex/experiment.cpp src/codex/external_auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp
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
#include "uos/api_key_policy.hpp"
#include "uos/codex/auth.hpp"
#include "uos/codex/auth_refresh.hpp"
#include "uos/codex/capacity_routing.hpp"
#include "uos/codex/dispatch.hpp"
#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/models/codex_models_availability.hpp"
#include "uos/provider/capacity_events.hpp"
#include "uos/provider/health.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include "tests/codex_dispatch_differential_vectors.inc"

#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace dispatch = uos::codex::dispatch;
  namespace auth = uos::codex::auth;
  namespace auth_refresh = uos::codex::auth_refresh;
  namespace routing_state = uos::codex::routing_state;
  namespace capacity_routing = uos::codex::capacity_routing;
  namespace capacity_events = uos::provider::capacity_events;
  namespace health = uos::provider::health;
  namespace availability = uos::models::codex_models_availability;
  using uos::codex::auth::CodexError;
  using uos::tests::helpers::codex_diff_kv::DiffKv;
  using uos::types::CodexAuthState;

  constexpr double NOW = 1'790'000'000'000;

  const json::value& vectors() {
    static const json::value parsed = rt::jsonParse(CODEX_DISPATCH_VECTORS);
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

  std::optional<std::string> optionalString(const json::value* value) {
    if (value == nullptr || value->is_null()) return std::nullopt;
    if (const json::object* object = value->if_object(); object != nullptr && object->contains("$undefined")) return std::nullopt;
    return std::string(value->as_string());
  }

  // The generator's `tag`: undefined, NaN, infinities and -0 are tagged objects.
  json::value tagNumber(double number) {
    if (std::isnan(number)) return json::object{{"$num", "NaN"}};
    if (std::isinf(number)) return json::object{{"$num", number > 0 ? "Infinity" : "-Infinity"}};
    if (number == 0 && std::signbit(number)) return json::object{{"$num", "-0"}};
    return rt::makeNumber(number);
  }

  double untagNumber(const json::value& value) {
    if (const json::object* object = value.if_object()) {
      const std::string tagged = text(object->at("$num"));
      if (tagged == "NaN") return std::numeric_limits<double>::quiet_NaN();
      if (tagged == "Infinity") return std::numeric_limits<double>::infinity();
      if (tagged == "-Infinity") return -std::numeric_limits<double>::infinity();
      return -0.0;
    }
    return *rt::getNumber(value);
  }

  json::value undefinedTag() { return json::object{{"$undefined", true}}; }

  // The generator's `caught`.
  json::value caught(const rt::Reason& error) {
    if (!rt::isError(error)) {
      const std::optional<json::value> thrown = rt::thrownValue(error);
      json::object out;
      out["thrown"] = error && thrown.has_value() ? *thrown : undefinedTag();
      return out;
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
    } catch (const uos::api_key_policy::ApiKeyQuotaDispatchError& quotaError) {
      out["code"] = quotaError.code;
      out["status"] = rt::makeNumber(quotaError.status);
    } catch (const rt::DOMException&) {
    } catch (const rt::JsError& jsError) {
      if (jsError.code.has_value()) out["code"] = *jsError.code;
      if (const auto cause = rt::errorCause(error)) out["cause"] = caught(*cause);
    } catch (...) {
    }
    return out;
  }

  rt::Task<json::value> responseJson(rt::Response response) {
    json::array headers;
    for (const auto& [name, value] : response.headers().entries()) {
      json::array pair;
      pair.push_back(json::value(name));
      pair.push_back(json::value(value));
      headers.push_back(std::move(pair));
    }
    auto reading = response.text();
    const std::string body = co_await std::move(reading);
    json::object out;
    out["status"] = response.status();
    out["headers"] = std::move(headers);
    out["body"] = body;
    co_return out;
  }

  rt::Task<json::value> responseWithRouting(rt::Response response) {
    rt::Task<json::value> converting = responseJson(response);
    json::value out = co_await std::move(converting);
    const std::optional<std::string> routingError = auth::getCodexRoutingError(response);
    out.as_object()["routingError"] = routingError.has_value() ? json::value(*routingError) : json::value(nullptr);
    co_return out;
  }

  rt::Task<void> settle(int turns) {
    for (int index = 0; index < turns; ++index) co_await rt::microtask();
  }

  // Credential isolation (mandatory): nothing here may reach this host's ~/.codex/auth.json.
  struct CredentialIsolation {
    rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    std::string savedBaseUrl = uos::config::config.codexBaseUrl;
    CredentialIsolation() {
      uos::config::config.codexAuthJsonB64.clear();
      uos::config::config.codexBaseUrl = "https://chatgpt.com/backend-api/codex";
    }
    ~CredentialIsolation() {
      uos::config::config.codexAuthJsonB64 = savedSeed;
      uos::config::config.codexBaseUrl = savedBaseUrl;
    }
  };

  void fixClock() { rt::globalDateNow() = [] { return NOW; }; }

  CodexAuthState account(const std::string& id) { return CodexAuthState{.access_token = "t-" + id, .refresh_token = "r-" + id, .account_id = id, .updated_at_ms = 1}; }
}

ASYNC_TEST("dispatch differential: base urls, account order and retry-after seconds") {
  const CredentialIsolation isolation;
  fixClock();
  for (const json::value& row : section("baseUrls")) {
    uos::config::config.codexBaseUrl = text(row.at("base"));
    const std::optional<std::string> clientVersion = optionalString(&row.at("clientVersion"));
    json::array urls;
    for (const std::string& url : dispatch::codexModelsBaseUrls(clientVersion)) urls.push_back(json::value(url));
    EXPECT(same(row.at("urls"), urls, "baseUrls " + rt::jsonStringify(row.at("base"))));
  }
  uos::config::config.codexBaseUrl = "https://chatgpt.com/backend-api/codex";

  const std::vector<CodexAuthState> all{account("a"), account("b"), account("c")};
  for (const json::value& row : section("order")) {
    const auto size = static_cast<std::size_t>(*rt::getNumber(row.at("size")));
    const std::vector<CodexAuthState> accounts(all.begin(), all.begin() + static_cast<std::ptrdiff_t>(size));
    json::array ids;
    for (const CodexAuthState& item : dispatch::orderCodexAuthAccounts(accounts, *rt::getNumber(row.at("startIndex")))) ids.push_back(json::value(item.account_id));
    EXPECT(same(row.at("order"), ids, "order " + rt::jsonStringify(row)));
  }

  for (const json::value& row : section("retryAfter")) {
    const double delta = untagNumber(row.at("delta"));
    EXPECT(same(row.at("withNow"), tagNumber(dispatch::codexRetryAfterSeconds(NOW + delta, NOW)), "retryAfter withNow " + rt::jsonStringify(row)));
    EXPECT(same(row.at("defaultNow"), tagNumber(dispatch::codexRetryAfterSeconds(NOW + delta)), "retryAfter defaultNow " + rt::jsonStringify(row)));
  }
  EXPECT_EQ(dispatch::CODEX_CLIENT_RETRY_AFTER_MAX_SECONDS, *rt::getNumber(vectors().as_object().at("retryAfterMax")));

  for (const json::value& row : section("statusClass")) {
    EXPECT_EQ(dispatch::codexStatusClass(*rt::getNumber(row.at("status"))), text(row.at("cls")));
  }
  co_return;
}

ASYNC_TEST("dispatch differential: routing error responses and initial selections") {
  const CredentialIsolation isolation;
  fixClock();
  for (const json::value& row : section("routingErrors")) {
    const std::optional<double> retryAtMs = row.at("retryAtMs").is_null() ? std::nullopt : rt::getNumber(row.at("retryAtMs"));
    const rt::Response response = dispatch::routingErrorResponse(*rt::getNumber(row.at("status")), text(row.at("message")), text(row.at("code")), retryAtMs);
    rt::Task<json::value> converting = responseWithRouting(response);
    const json::value actual = co_await std::move(converting);
    EXPECT(same(row.at("response"), actual, "routingErrors " + text(row.at("code"))));
  }
  {
    const json::array& expected = section("upstreamTimeout");
    rt::Task<json::value> first = responseWithRouting(dispatch::upstreamTimeoutCircuitResponse(std::nullopt));
    const json::value firstJson = co_await std::move(first);
    EXPECT(same(expected.at(0), firstJson, "upstreamTimeout null"));
    rt::Task<json::value> second = responseWithRouting(dispatch::upstreamTimeoutCircuitResponse(NOW + 30'000));
    const json::value secondJson = co_await std::move(second);
    EXPECT(same(expected.at(1), secondJson, "upstreamTimeout retry"));
  }
  {
    const json::array& expected = section("modelUnavailable");
    const std::vector<std::pair<std::string, std::optional<std::string>>> cases{{"gpt-x", std::nullopt}, {"gpt-x", "Upstream said no."}, {"", ""}};
    for (std::size_t index = 0; index < cases.size(); ++index) {
      rt::Task<json::value> converting = responseWithRouting(dispatch::codexModelUnavailableResponse(cases[index].first, cases[index].second));
      const json::value actual = co_await std::move(converting);
      EXPECT(same(expected.at(index), actual, "modelUnavailable " + std::to_string(index)));
    }
  }
  for (const json::value& row : section("initialSelection")) {
    const std::string name = nameOf(row);
    routing_state::RouteSelection selection = routing_state::RouteSelectionEligible{};
    if (name == "routing_unavailable") selection = routing_state::RouteSelectionRoutingUnavailable{};
    if (name == "credentials_invalid") selection = routing_state::RouteSelectionCredentialsInvalid{.skippedSlots = {0}, .fullCohortUnavailable = true};
    if (name == "upstream_blocked") selection = routing_state::RouteSelectionUpstreamBlocked{.skippedSlots = {0}, .retryAtMs = NOW + 12'345, .blockedAccounts = {}};
    if (name == "upstream_blocked_null") selection = routing_state::RouteSelectionUpstreamBlocked{.skippedSlots = {}, .retryAtMs = std::nullopt, .blockedAccounts = {}};
    if (name == "model_unavailable") {
      selection = routing_state::RouteSelectionModelUnavailable{.model = "gpt-9", .detail = "The 'gpt-9' model is not supported.", .skippedSlots = {}};
    }
    if (name == "model_unavailable_null") selection = routing_state::RouteSelectionModelUnavailable{.model = "gpt-9", .detail = std::nullopt, .skippedSlots = {}};
    if (name == "quota_blocked") selection = routing_state::RouteSelectionQuotaBlocked{};
    const std::optional<rt::Response> response = dispatch::initialCodexSelectionResponse(selection);
    json::value actual = nullptr;
    if (response.has_value()) {
      rt::Task<json::value> converting = responseWithRouting(*response);
      actual = co_await std::move(converting);
    }
    EXPECT(same(row.at("response"), actual, "initialSelection " + name));
  }
}

ASYNC_TEST("dispatch differential: error classes, non-Error failures and routing logs") {
  const CredentialIsolation isolation;
  fixClock();
  const auto reasonFor = [](const std::string& name) -> rt::Reason {
    if (name == "gateway_timeout") return rt::makeReason(CodexError("m", "gateway_timeout", 504));
    if (name == "unreachable") return rt::makeReason(CodexError("m", "codex_upstream_unreachable", 502));
    if (name == "refresh_unreachable") return rt::makeReason(CodexError("m", "codex_auth_refresh_unreachable", 502));
    if (name == "auth_invalid") return rt::makeReason(CodexError("m", "codex_auth_invalid", 401));
    if (name == "reused") return rt::makeReason(CodexError("m", "refresh_token_reused", 401));
    if (name == "missing") return rt::makeReason(CodexError("m", "codex_auth_missing", 503));
    if (name == "status_429") return rt::makeReason(CodexError("m", "codex_auth_refresh_failed", 429));
    if (name == "status_403") return rt::makeReason(CodexError("m", "codex_auth_refresh_failed", 403));
    if (name == "status_200") return rt::makeReason(CodexError("m", "codex_auth_refresh_failed", 200));
    if (name == "status_600") return rt::makeReason(CodexError("m", "codex_auth_refresh_failed", 600));
    if (name == "timeout_dom") return rt::makeReason(rt::DOMException("t", "TimeoutError"));
    if (name == "abort_dom") return rt::makeReason(rt::DOMException("a", "AbortError"));
    if (name == "plain") return rt::makeReason(rt::JsError("x"));
    if (name == "timeout_named") return rt::makeReason(rt::JsError("named", "TimeoutError"));
    if (name == "string") return rt::thrown(json::value("string value"));
    if (name == "null") return rt::thrown(json::value(nullptr));
    if (name == "number") return rt::thrown(json::value(42));
    if (name == "object") return rt::thrown(json::value(json::object{{"a", 1}}));
    return nullptr;  // "undefined"
  };
  const std::function<rt::Reason()> fallback = [] { return rt::makeReason(CodexError("Codex auth pool is empty.", "codex_auth_missing", 503)); };
  for (const json::value& row : section("errors")) {
    const std::string name = nameOf(row);
    const rt::Reason error = reasonFor(name);
    const rt::Reason converted = dispatch::dispatchFailureAsError(error, fallback);
    json::object actual;
    actual["name"] = name;
    actual["cls"] = dispatch::codexErrorClass(error);
    actual["sibling"] = dispatch::isCodexSiblingTransportFailure(error);
    actual["asError"] = caught(converted);
    actual["same"] = converted == error;
    EXPECT(same(row, actual, "errors " + name));
  }

  rt::console::Capture capture({rt::console::Method::info});
  dispatch::logCodexRouting("codex_attempt", json::object{{"request_id", nullptr},
                                                          {"attempt", 2},
                                                          {"slot", 1},
                                                          {"phase", "initial"},
                                                          {"status", 200},
                                                          {"status_class", "2xx"},
                                                          {"active_generation", nullptr},
                                                          {"active_transition_reason", nullptr}});
  dispatch::logCodexRouting("codex_token_refresh", json::object{{"request_id", "req-1"},
                                                                {"slot", 2},
                                                                {"trigger", "401"},
                                                                {"outcome", "failed"},
                                                                {"status_class", "network_failure"},
                                                                {"active_generation", 7},
                                                                {"active_transition_reason", "quota_exhausted"}});
  dispatch::logCodexRouting("codex_two_second_retry", json::object{{"event", "overridden"}, {"delay_ms", 1500.5}, {"value", -0.0}});
  dispatch::logCodexRouting("codex_overage_served", json::object{});
  json::array logs;
  for (const rt::console::Call& call : capture.calls()) {
    json::array args;
    for (const rt::console::Value& arg : call.args) args.push_back(json::value(arg.string()));
    logs.push_back(std::move(args));
  }
  EXPECT(same(vectors().as_object().at("logs"), logs, "logs"));
  co_return;
}

ASYNC_TEST("dispatch differential: unsupported-model details, requested model, native session headers") {
  const CredentialIsolation isolation;
  for (const json::value& row : section("unsupportedDetail")) {
    const std::optional<dispatch::ParseCodexModelUnsupportedDetailResult> parsed = dispatch::parseCodexModelUnsupportedDetail(row.at("payload"));
    const json::value actual = parsed.has_value() ? json::value(json::object{{"model", parsed->model}, {"detail", parsed->detail}}) : json::value(nullptr);
    EXPECT(same(row.at("parsed"), actual, "unsupportedDetail " + nameOf(row)));
  }
  for (const json::value& row : section("requestedModel")) {
    const std::optional<std::string> model = dispatch::requestedCodexModel(row.at("body"));
    EXPECT(same(row.at("model"), model.has_value() ? json::value(*model) : json::value(nullptr), "requestedModel " + nameOf(row)));
  }
  for (const json::value& row : section("nativeHeaders")) {
    rt::Headers headers{{"originator", "codex_cli_rs"}, {"Session-Id", "old"}};
    dispatch::applyNativeSessionHeaders(headers, optionalString(&row.at("identity")));
    json::array entries;
    for (const auto& [name, value] : headers.entries()) entries.push_back(json::array{json::value(name), json::value(value)});
    EXPECT(same(row.at("headers"), entries, "nativeHeaders"));
  }
  co_return;
}

ASYNC_TEST("dispatch differential: prepareCodexSubscriptionRequest") {
  const CredentialIsolation isolation;
  static const rt::Regex UUID("^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$", "");
  for (const json::value& row : section("prepare")) {
    const json::value body = row.at("body");
    const std::optional<std::string> scope = optionalString(&row.at("scope"));
    rt::Promise<dispatch::PreparedCodexSubscriptionRequest> preparing(dispatch::prepareCodexSubscriptionRequest(body, scope));
    const dispatch::PreparedCodexSubscriptionRequest prepared = co_await preparing.wait();
    json::array warnings;
    for (const std::string& warning : prepared.warnings) warnings.push_back(json::value(warning));
    json::object actual;
    actual["body"] = prepared.body;
    actual["serializedBody"] = prepared.serializedBody;
    actual["conversationIdentity"] =
      !prepared.nativeSessionIdentity.has_value() && UUID.test(prepared.conversationIdentity) ? std::string("<uuid>") : prepared.conversationIdentity;
    actual["nativeSessionIdentity"] = prepared.nativeSessionIdentity.has_value() ? json::value(*prepared.nativeSessionIdentity) : json::value(nullptr);
    actual["warnings"] = std::move(warnings);
    EXPECT(same(row.at("prepared"), actual, "prepare " + nameOf(row)));
  }
  const std::optional<std::string> scope = "scope-a";
  const std::pair<std::string, std::string> variants[]{
    {"key-13", "912509f1-f9e6-86e6-898c-e3d583df9dbd"},
    {"key-4", "2efbdd44-1e90-8fb7-ad4c-78712eea5269"},
  };
  for (const auto& [key, identity] : variants) {
    const json::value body = json::object{{"prompt_cache_key", key}};
    auto preparing = dispatch::prepareCodexSubscriptionRequest(body, scope);
    const auto prepared = co_await std::move(preparing);
    EXPECT_EQ(prepared.nativeSessionIdentity, std::optional<std::string>(identity));
    EXPECT_EQ(prepared.conversationIdentity, identity);
  }
}

ASYNC_TEST("dispatch differential: codexModelUnsupportedFromResponse") {
  const CredentialIsolation isolation;
  const std::string detail = "The 'gpt-5' model is not supported when using Codex with a ChatGPT account.";
  const std::string matching = rt::jsonStringify(json::object{{"detail", detail}});
  for (const json::value& row : section("unsupportedFromResponse")) {
    const std::string name = nameOf(row);
    rt::ResponseInit init;
    init.status = name == "status_200" ? 200 : name == "status_404" ? 404 : 400;
    std::string payload = matching;
    if (name == "other_400") payload = rt::jsonStringify(json::object{{"detail", "Bad request"}});
    if (name == "invalid_json") payload = "{nope";
    if (name == "empty") payload = "";
    const rt::Response response(rt::BodyInit(payload), init);
    if (name == "used") {
      auto reading = response.text();
      co_await std::move(reading);
    }
    rt::Promise<std::optional<dispatch::ParseCodexModelUnsupportedDetailResult>> classifying(dispatch::codexModelUnsupportedFromResponse(response));
    const std::optional<dispatch::ParseCodexModelUnsupportedDetailResult> parsed = co_await classifying.wait();
    json::object actual;
    actual["name"] = name;
    actual["parsed"] = parsed.has_value() ? json::value(json::object{{"model", parsed->model}, {"detail", parsed->detail}}) : json::value(nullptr);
    actual["bodyUsed"] = response.bodyUsed();
    if (response.bodyUsed()) {
      actual["passthrough"] = nullptr;
    } else {
      auto reading = response.text();
      actual["passthrough"] = co_await std::move(reading);
    }
    EXPECT(same(row, actual, "unsupportedFromResponse " + name));
  }
}

namespace {
  struct CoordinatorLog {
    std::vector<std::string> entries;
    int turn = 0;
    bool running = true;
    int generation = 0;
  };

  rt::Task<void> tick(std::shared_ptr<CoordinatorLog> log) {
    while (log->running) {
      co_await rt::microtask();
      log->turn += 1;
    }
  }

  rt::Task<void> cancelDispatch(std::shared_ptr<CoordinatorLog> log, int id) {
    log->entries.push_back("cancel:" + std::to_string(id));
    co_await rt::microtask();
    log->entries.push_back("cancelled:" + std::to_string(id));
  }

  rt::Task<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>> promisedDispatch(std::shared_ptr<CoordinatorLog> log) {
    log->generation += 1;
    const int id = log->generation;
    log->entries.push_back("before:" + std::to_string(id));
    co_await rt::microtask();
    uos::api_key_policy::ApiKeyProviderDispatch handed;
    handed.markTransportStarted = [log, id] { log->entries.push_back("mark:" + std::to_string(id)); };
    handed.cancelBeforeTransport = [log, id] { return cancelDispatch(log, id); };
    co_return handed;
  }

  rt::Task<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>> undefinedDispatch(std::shared_ptr<CoordinatorLog> log) {
    log->entries.push_back("before:undefined");
    co_return std::nullopt;
  }

  rt::Task<void> claimLater(std::shared_ptr<CoordinatorLog> log, rt::Task<void> claiming) {
    co_await std::move(claiming);
    log->entries.push_back("claimed_later@" + std::to_string(log->turn));
  }
}

ASYNC_TEST("dispatch differential: provider dispatch coordinator turns") {
  const CredentialIsolation isolation;
  const std::vector<std::pair<std::string, std::vector<std::string>>> scripts{
    {"none", {"claim", "cancel", "mark", "cancel"}},
    {"promise_cancel_twice", {"claim", "claim", "cancel", "cancel", "mark", "claim"}},
    {"promise_mark_then_new_generation", {"claim", "mark", "mark", "claim", "cancel", "mark", "cancel"}},
    {"promise_cancel_before_claim_settles", {"claim_nowait", "cancel", "claim"}},
    {"promise_undefined", {"claim", "cancel", "mark"}},
    {"void", {"claim", "claim", "cancel", "mark", "claim"}},
  };
  const json::array& expected = section("coordinator");
  for (std::size_t index = 0; index < scripts.size(); ++index) {
    const auto& [name, steps] = scripts[index];
    const auto log = std::make_shared<CoordinatorLog>();
    dispatch::FetchCodexResponsesOptions::BeforeDispatch beforeDispatch;
    if (name.rfind("promise_undefined", 0) == 0) {
      beforeDispatch = std::function<rt::Task<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>>()>([log] { return undefinedDispatch(log); });
    } else if (name.rfind("promise", 0) == 0) {
      beforeDispatch = std::function<rt::Task<std::optional<uos::api_key_policy::ApiKeyProviderDispatch>>()>([log] { return promisedDispatch(log); });
    } else if (name == "void") {
      beforeDispatch = std::function<void()>([log] { log->entries.push_back("before:void"); });
    }
    const dispatch::CodexProviderDispatchCoordinator coordinator = dispatch::createCodexProviderDispatchCoordinator(beforeDispatch);
    rt::Promise<void> ticker(tick(log));
    for (const std::string& step : steps) {
      if (step == "claim") {
        rt::Task<void> claiming = coordinator.claim();
        co_await std::move(claiming);
        log->entries.push_back("claimed@" + std::to_string(log->turn));
      } else if (step == "claim_nowait") {
        rt::spawn(claimLater(log, coordinator.claim()));
      } else if (step == "mark") {
        coordinator.markTransportStarted();
        log->entries.push_back("marked@" + std::to_string(log->turn));
      } else if (step == "cancel") {
        rt::Task<void> cancelling = coordinator.cancelBeforeTransport();
        co_await std::move(cancelling);
        log->entries.push_back("cancel_done@" + std::to_string(log->turn));
      }
    }
    rt::Task<void> settling = settle(10);
    co_await std::move(settling);
    log->running = false;
    co_await ticker.wait();
    json::array entries;
    for (const std::string& entry : log->entries) entries.push_back(json::value(entry));
    EXPECT(same(expected.at(index).at("log"), entries, "coordinator " + name));
  }
}

namespace {
  struct AdmissionScript {
    json::object script;
    json::array queued;
    json::array reselect;
    json::array classify;
    std::map<std::string, std::size_t> cursor;
    std::size_t queuedIndex = 0;
    std::size_t reselectIndex = 0;
    std::size_t classifyIndex = 0;
    std::vector<std::string> log;

    // The scripted value of the next call of `driver`: a status number, null or "throw"; nullopt is undefined.
    std::optional<json::value> next(const std::string& driver) {
      const std::size_t index = cursor[driver]++;
      const json::array* values = rt::getArray(script, driver);
      if (values == nullptr || index >= values->size()) return std::nullopt;
      return (*values)[index];
    }
  };

  std::string scriptedText(const std::optional<json::value>& value) {
    if (!value.has_value()) return "none";
    if (value->is_null()) return "null";
    if (value->is_string()) return std::string(value->as_string());
    return rt::numberToString(*rt::getNumber(*value));
  }

  rt::Task<std::optional<rt::Response>> scriptedResponse(std::shared_ptr<AdmissionScript> state, std::string driver) {
    const std::optional<json::value> value = state->next(driver);
    state->log.push_back(driver + ":" + scriptedText(value));
    if (value.has_value() && value->is_string()) throw rt::JsError(driver + " failed");
    if (value.has_value() && rt::isNumber(*value)) {
      const double status = *rt::getNumber(*value);
      rt::ResponseInit init;
      init.status = status;
      co_return rt::Response(rt::BodyInit(driver + "-" + rt::numberToString(status)), init);
    }
    co_return std::nullopt;
  }

  rt::Task<rt::Response> scriptedExhausted(std::shared_ptr<AdmissionScript> state) {
    const std::optional<json::value> value = state->next("exhaustedResponse");
    state->log.push_back("exhaustedResponse:" + (value.has_value() ? scriptedText(value) : std::string("undefined")));
    rt::ResponseInit init;
    init.status = value.has_value() && rt::isNumber(*value) ? *rt::getNumber(*value) : 500;
    co_return rt::Response(rt::BodyInit("exhausted-" + (value.has_value() ? scriptedText(value) : std::string("undefined"))), init);
  }

  rt::Task<std::optional<dispatch::CodexModelUnavailableAttempt>> scriptedClassify(std::shared_ptr<AdmissionScript> state, rt::Response response) {
    const std::size_t index = state->classifyIndex++;
    const json::value* account = index < state->classify.size() ? &state->classify[index] : nullptr;
    const bool none = account == nullptr || account->is_null();
    state->log.push_back("classify:" + std::to_string(response.status()) + ":" + (none ? std::string("null") : std::string(account->as_string())));
    if (none) co_return std::nullopt;
    const std::string accountId(account->as_string());
    co_return dispatch::CodexModelUnavailableAttempt{.accountId = accountId, .model = "gpt-x", .detail = "detail for " + accountId};
  }
}

ASYNC_TEST("dispatch differential: serial admission loop") {
  const CredentialIsolation isolation;
  for (const json::value& row : section("admission")) {
    const std::string name = nameOf(row);
    const json::object& scenario = row.at("scenario").as_object();
    const auto state = std::make_shared<AdmissionScript>();
    state->script = scenario.at("script").as_object();
    if (const json::array* queued = rt::getArray(scenario, "queued")) state->queued = *queued;
    if (const json::array* reselect = rt::getArray(scenario, "reselect")) state->reselect = *reselect;
    if (const json::array* classify = rt::getArray(scenario, "classify")) state->classify = *classify;
    dispatch::CodexSerialAdmissionDrivers drivers;
    drivers.runPendingShortRetry = [state] { return scriptedResponse(state, "runPendingShortRetry"); };
    drivers.dispatchActive = [state] { return scriptedResponse(state, "dispatchActive"); };
    drivers.terminalTransportResponse = [state] { return scriptedResponse(state, "terminalTransportResponse"); };
    drivers.hasQueuedRetry = [state] {
      const std::size_t index = state->queuedIndex++;
      const bool value = index < state->queued.size() && state->queued[index].as_bool();
      state->log.push_back(std::string("hasQueuedRetry:") + (value ? "true" : "false"));
      return value;
    };
    drivers.reselectionRequested = [state] {
      const std::size_t index = state->reselectIndex++;
      const bool value = index < state->reselect.size() && state->reselect[index].as_bool();
      state->log.push_back(std::string("reselectionRequested:") + (value ? "true" : "false"));
      return value;
    };
    drivers.advanceReselection = [state] { return scriptedResponse(state, "advanceReselection"); };
    drivers.exhaustedResponse = [state] { return scriptedExhausted(state); };
    drivers.classifyModelUnavailable = [state](rt::Response response) { return scriptedClassify(state, std::move(response)); };
    json::object actual;
    actual["name"] = name;
    actual["scenario"] = row.at("scenario");
    rt::Reason failure;
    std::optional<rt::Response> response;
    try {
      rt::Promise<rt::Response> running(dispatch::runCodexSerialAdmissionLoop(drivers));
      response = co_await running.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    json::array log;
    for (const std::string& entry : state->log) log.push_back(json::value(entry));
    actual["log"] = std::move(log);
    if (response.has_value()) {
      rt::Task<json::value> converting = responseJson(*response);
      actual["response"] = co_await std::move(converting);
    } else {
      actual["error"] = caught(failure);
    }
    EXPECT(same(row, actual, "admission " + name));
  }
}

namespace {
  rt::Reason abortReasonOf(const std::optional<std::string>& kind) {
    if (kind == "timeout") return rt::makeReason(rt::DOMException("deadline", "TimeoutError"));
    if (kind == "abort") return rt::makeReason(rt::DOMException("stop", "AbortError"));
    if (kind == "string") return rt::thrown(json::value("client-gone"));
    return nullptr;
  }

  rt::Task<void> recordedSleep(std::shared_ptr<std::vector<double>> sleeps, std::string mode, double milliseconds) {
    sleeps->push_back(milliseconds);
    if (mode == "hang") {
      rt::Promise<void> never;
      co_await never.wait();
    }
    if (mode == "reject") throw rt::JsError("sleep failed");
  }
}

ASYNC_TEST("dispatch differential: waitForCodexRetry") {
  const CredentialIsolation isolation;
  for (const json::value& row : section("waitRetry")) {
    const std::string name = nameOf(row);
    const json::object& item = row.at("item").as_object();
    const double milliseconds = untagNumber(item.at("ms"));
    const std::string signalMode = text(item.at("signal"));
    const std::optional<std::string> reason = optionalString(rt::get(item, "reason"));
    const std::optional<std::string> abortDuring = optionalString(rt::get(item, "abortDuring"));
    const std::string sleepMode = optionalString(rt::get(item, "sleepMode")).value_or("resolve");
    const auto sleeps = std::make_shared<std::vector<double>>();
    const rt::AbortController controller;
    if (signalMode == "aborted") controller.abort(abortReasonOf(reason));
    std::function<rt::Task<void>(double)> sleep = [sleeps, sleepMode](double ms) { return recordedSleep(sleeps, sleepMode, ms); };
    const std::optional<rt::AbortSignal> signal = signalMode == "none" ? std::nullopt : std::optional<rt::AbortSignal>(controller.signal());
    rt::Promise<void> waiting(dispatch::waitForCodexRetry(milliseconds, signal, sleep));
    if (abortDuring.has_value()) {
      rt::Task<void> settling = settle(3);
      co_await std::move(settling);
      controller.abort(abortReasonOf(abortDuring));
    }
    json::object actual;
    actual["name"] = name;
    actual["item"] = row.at("item");
    rt::Reason failure;
    try {
      co_await waiting.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    json::array sleptFor;
    for (const double ms : *sleeps) sleptFor.push_back(tagNumber(ms));
    actual["sleeps"] = std::move(sleptFor);
    if (failure) actual["error"] = caught(failure);
    else actual["result"] = "ok";
    EXPECT(same(row, actual, "waitRetry " + name));
  }
}

TEST("abandoning a Codex retry wait releases the abort signal") {
  std::weak_ptr<rt::detail::AbortSignalState> signalState;
  {
    rt::FakeClock clock;
    rt::Loop loop(clock);
    const rt::LoopScope scope(loop);
    const rt::AbortController controller;
    const auto signal = controller.signal();
    signalState = signal.state();
    const rt::Promise<void> never;
    auto sleeping = [never](double) { return never.wait(); };
    auto waiting = dispatch::waitForCodexRetry(10, signal, std::move(sleeping));
    rt::spawn(std::move(waiting));
  }
  EXPECT(signalState.expired());
}

TEST("abandoning a shared Codex refresh wait releases the abort signal") {
  const rt::AbortController caller;
  std::weak_ptr<rt::detail::AbortSignalState> signalState;
  {
    rt::FakeClock clock;
    rt::Loop loop(clock);
    const rt::LoopScope scope(loop);
    const auto signal = rt::AbortSignal::any({caller.signal()});
    signalState = signal.state();
    const rt::Promise<CodexAuthState> refreshing;
    auto waiting = auth_refresh::awaitWithoutCancellingSharedWork(refreshing, signal);
    rt::spawn(std::move(waiting));
  }
  EXPECT(signalState.expired());
}

ASYNC_TEST("an aborted Codex retry lets the sleeper finish with its captures alive") {
  const rt::AbortController controller;
  const auto entered = rt::withResolvers();
  const auto gate = rt::withResolvers();
  const auto finished = rt::withResolvers<double>();
  auto sleep = [start = entered.resolve, resume = gate.promise, finish = finished.resolve](double milliseconds) -> rt::Task<void> {
    start();
    co_await resume.wait();
    finish(milliseconds);
  };
  rt::Promise<void> waiting(dispatch::waitForCodexRetry(37, controller.signal(), std::move(sleep)));
  co_await entered.promise.wait();
  const auto reason = rt::makeReason(rt::DOMException("client stopped waiting", "AbortError"));
  controller.abort(reason);
  auto completion = waiting.wait();
  EXPECT_REJECTS_REASON(std::move(completion), reason);
  EXPECT(finished.promise.pending());
  gate.resolve();
  const auto milliseconds = co_await finished.promise.wait();
  EXPECT_EQ(milliseconds, 37);
}

namespace {
  struct FetchRecorder {
    std::vector<std::string> log;
    json::array requests;
  };

  rt::Task<json::value> recordRequest(rt::RequestInfo input, rt::RequestInit init) {
    const bool hasBody = init.body.has_value() && !std::holds_alternative<std::nullptr_t>(*init.body);
    const std::optional<rt::AbortSignal> signal = init.signal;
    const rt::Request request(std::get<std::string>(input), init);
    json::array headers;
    for (const auto& [name, value] : request.headers().entries()) headers.push_back(json::array{json::value(name), json::value(value)});
    json::object out;
    out["url"] = request.url();
    out["method"] = request.method();
    out["headers"] = std::move(headers);
    if (hasBody) {
      auto reading = request.text();
      out["body"] = co_await std::move(reading);
    } else {
      out["body"] = nullptr;
    }
    out["redirect"] = request.redirect();
    out["signalAborted"] = signal.has_value() ? json::value(signal->aborted()) : json::value(nullptr);
    co_return out;
  }

  rt::Task<rt::Response> modelsFetchMock(std::shared_ptr<FetchRecorder> recorder, std::string mode, rt::RequestInfo input, rt::RequestInit init) {
    const std::optional<rt::AbortSignal> signal = init.signal;
    rt::Task<json::value> recording = recordRequest(std::move(input), std::move(init));
    json::value request = co_await std::move(recording);
    recorder->requests.push_back(std::move(request));
    if (mode == "throw") throw rt::TypeError("network down");
    if (signal.has_value() && signal->aborted()) rt::rethrow(signal->reason());
    rt::ResponseInit responseInit;
    responseInit.status = 200;
    responseInit.headers = rt::Headers{{"content-type", "application/json"}, {"etag", "W/\"x\""}};
    co_return rt::Response(rt::BodyInit(std::string("{\"models\":[]}")), responseInit);
  }
}

ASYNC_TEST("dispatch differential: fetchCodexModelsWithAuth") {
  const CredentialIsolation isolation;
  fixClock();
  const CodexAuthState authState{.access_token = "access-models", .refresh_token = "refresh-models", .account_id = "acct-models", .updated_at_ms = NOW - 1'000};
  for (const json::value& row : section("modelsFetch")) {
    const std::string name = nameOf(row);
    const json::object& item = row.at("item").as_object();
    const std::string mode = text(item.at("mode"));
    const auto recorder = std::make_shared<FetchRecorder>();
    const rt::GlobalFetchOverride override_(rt::Fetch([recorder, mode](rt::RequestInfo input, rt::RequestInit init) {
      return modelsFetchMock(recorder, mode, std::move(input), std::move(init));
    }));
    const rt::AbortController controller;
    if (mode == "abort") controller.abort(rt::makeReason(rt::DOMException("gone", "AbortError")));
    const std::optional<rt::AbortSignal> signal = mode == "abort" ? std::optional<rt::AbortSignal>(controller.signal()) : std::nullopt;
    const std::optional<std::string> clientVersion = optionalString(&item.at("clientVersion"));
    const std::optional<std::string> ifNoneMatch = optionalString(rt::get(item, "ifNoneMatch"));
    json::object actual;
    actual["name"] = name;
    actual["item"] = row.at("item");
    std::optional<rt::Response> response;
    rt::Reason failure;
    try {
      rt::Promise<rt::Response> fetching(dispatch::fetchCodexModelsWithAuth(authState, "https://chatgpt.com/backend-api/codex/models?client_version=0.100.0",
                                                                           clientVersion, ifNoneMatch, signal));
      response = co_await fetching.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    actual["requests"] = recorder->requests;
    if (response.has_value()) {
      rt::Task<json::value> converting = responseJson(*response);
      actual["response"] = co_await std::move(converting);
    } else {
      actual["error"] = caught(failure);
    }
    EXPECT(same(row, actual, "modelsFetch " + name));
  }
}

namespace {
  rt::Task<rt::Response> responsesFetchMock(std::shared_ptr<FetchRecorder> recorder, std::string mode, rt::AbortController controller, rt::RequestInfo input,
                                            rt::RequestInit init) {
    recorder->log.push_back("fetch");
    const std::optional<rt::AbortSignal> signal = init.signal;
    rt::Task<json::value> recording = recordRequest(std::move(input), std::move(init));
    json::value request = co_await std::move(recording);
    recorder->requests.push_back(std::move(request));
    if (mode == "throw") throw rt::TypeError("network down");
    if (mode == "throw_timeout") throw rt::DOMException("too slow", "TimeoutError");
    if (mode == "throw_reason") {
      controller.abort(rt::thrown(json::value("client-left")));
      rt::rethrow(signal->reason());
    }
    rt::ResponseInit responseInit;
    responseInit.status = 200;
    responseInit.headers = rt::Headers{{"content-type", "text/event-stream"}};
    co_return rt::Response(rt::BodyInit(std::string("data: {}\n\n")), responseInit);
  }

  rt::Task<void> beforeTransportHook(std::shared_ptr<FetchRecorder> recorder, std::string mode, rt::AbortController controller) {
    recorder->log.push_back("before");
    co_await rt::microtask();
    if (mode == "quota") {
      uos::api_key_policy::ApiKeyQuotaDispatchErrorOptions options;
      options.status = 429;
      options.code = "api_key_quota_exceeded";
      throw uos::api_key_policy::ApiKeyQuotaDispatchError("quota gone", options);
    }
    if (mode == "fence") throw dispatch::CodexActiveAccountFenceError();
    if (mode == "retry_fence") throw dispatch::CodexBankedResetRetryFenceError();
    if (mode == "plain") throw rt::JsError("before failed");
    if (mode == "string") rt::rethrow(rt::thrown(json::value("before-string")));
    if (mode == "abort_signal") controller.abort(rt::makeReason(rt::DOMException("late", "AbortError")));
  }
}

ASYNC_TEST("dispatch differential: fetchCodexResponseWithAuth") {
  const CredentialIsolation isolation;
  fixClock();
  const CodexAuthState authState{.access_token = "access-resp", .refresh_token = "refresh-resp", .account_id = "acct-resp", .updated_at_ms = NOW - 1'000};
  const rt::Headers base{{"originator", "codex_cli_rs"},
                         {"user-agent", "codex_cli_rs/0.100.0 (ai.ubq.fi)"},
                         {"Content-Type", "application/json"},
                         {"Accept", "text/event-stream"},
                         {"conversation_id", "conv-1"},
                         {"Authorization", "Bearer stale"}};
  for (const json::value& row : section("responsesFetch")) {
    const std::string name = nameOf(row);
    const json::object& item = row.at("item").as_object();
    const std::optional<std::string> signalMode = optionalString(rt::get(item, "signal"));
    const std::optional<std::string> before = optionalString(rt::get(item, "before"));
    const std::string fetchMode = text(item.at("fetchMode"));
    const auto recorder = std::make_shared<FetchRecorder>();
    const rt::AbortController controller;
    if (signalMode == "abort") controller.abort();
    if (signalMode == "timeout") controller.abort(rt::makeReason(rt::DOMException("deadline", "TimeoutError")));
    if (signalMode == "string") controller.abort(rt::thrown(json::value("client-gone")));
    if (signalMode == "null") controller.abort(rt::thrown(json::value(nullptr)));
    const rt::GlobalFetchOverride override_(rt::Fetch([recorder, fetchMode, controller](rt::RequestInfo input, rt::RequestInit init) {
      return responsesFetchMock(recorder, fetchMode, controller, std::move(input), std::move(init));
    }));
    std::function<rt::Task<void>()> beforeTransport;
    std::function<void()> onDispatch;
    if (before.has_value()) {
      const std::string beforeMode = *before;
      beforeTransport = [recorder, beforeMode, controller] { return beforeTransportHook(recorder, beforeMode, controller); };
      onDispatch = [recorder] { recorder->log.push_back("dispatch"); };
    }
    const std::optional<rt::AbortSignal> signal = signalMode.has_value() ? std::optional<rt::AbortSignal>(controller.signal()) : std::nullopt;
    json::object actual;
    actual["name"] = name;
    actual["item"] = row.at("item");
    std::optional<rt::Response> response;
    rt::Reason failure;
    try {
      rt::Promise<rt::Response> fetching(dispatch::fetchCodexResponseWithAuth(authState, "https://chatgpt.com/backend-api/codex/responses",
                                                                             "{\"model\":\"gpt-5\"}", base, signal, beforeTransport, onDispatch));
      response = co_await fetching.wait();
    } catch (...) {
      failure = std::current_exception();
    }
    json::array log;
    for (const std::string& entry : recorder->log) log.push_back(json::value(entry));
    actual["log"] = std::move(log);
    actual["requests"] = recorder->requests;
    if (response.has_value()) {
      rt::Task<json::value> converting = responseJson(*response);
      actual["response"] = co_await std::move(converting);
    } else {
      actual["error"] = caught(failure);
    }
    EXPECT(same(row, actual, "responsesFetch " + name));
  }
}

namespace {
  // The rows written after `before` (versionstamps are the commit counter), sorted by key.
  json::array writesAfter(const DiffKv& kv, double before) {
    std::map<std::string, json::value> sorted;
    for (const auto& [key, row] : kv.rows) {
      if (rt::stringToNumber(row.versionstamp) > before) sorted[key] = row.value.toJson();
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

  std::string base64NoPadding(const std::string& raw) {
    std::string encoded = rt::btoa(raw);
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return encoded;
  }
}

ASYNC_TEST("dispatch differential: provider health from responses and thrown failures") {
  const CredentialIsolation isolation;
  fixClock();
  const auto kv = std::make_shared<DiffKv>();
  uos::kv::setKvForTest(kv);
  const auto triggers = std::make_shared<int>(0);
  capacity_events::setProviderCapacitySampleTriggerForTest([triggers](const capacity_events::ProviderCapacitySampleTriggerOptions&) { *triggers += 1; });
  const std::string expiredJwt = rt::btoa(rt::jsonStringify(json::object{{"alg", "none"}})) + "." +
                                 base64NoPadding(rt::jsonStringify(json::object{{"exp", rt::Math::floor(NOW / 1000) - 60}})) + ".sig";
  const CodexAuthState authExpired{.access_token = expiredJwt, .refresh_token = "r", .account_id = "acct-h", .updated_at_ms = NOW};
  const CodexAuthState authFresh{.access_token = "opaque", .refresh_token = "r", .account_id = "acct-h", .updated_at_ms = NOW};
  const auto respond = [](double status, std::vector<std::pair<std::string, std::string>> headers, std::string body) {
    rt::ResponseInit init;
    init.status = status;
    rt::Headers built;
    for (const auto& [name, value] : headers) built.append(name, value);
    init.headers = built;
    return rt::Response(rt::BodyInit(body), init);
  };
  struct HealthCase {
    std::string name;
    rt::Response response;
    std::optional<CodexAuthState> auth;
    std::optional<std::string> event;
  };
  const std::vector<HealthCase> cases{
    {"401", respond(401, {{"X-Request-Id", "req-401"}}, ""), std::nullopt, std::nullopt},
    {"403_expired", respond(403, {{"X-Api-Request-Id", "req-403"}}, ""), authExpired, std::nullopt},
    {"403_fresh", respond(403, {}, ""), authFresh, std::nullopt},
    {"429", respond(429, {{"X-Oneapi-Request-Id", "req-429"}}, ""), std::nullopt, std::nullopt},
    {"500", respond(500, {{"x-request-id", "req-500"}, {"x-api-request-id", "ignored"}}, ""), std::nullopt, std::nullopt},
    {"200_success", respond(200, {}, "ok"), authFresh, "success"},
    {"200_reachable", respond(200, {}, "ok"), std::nullopt, "reachable"},
    {"200_none", respond(200, {}, "ok"), std::nullopt, std::nullopt},
    {"404", respond(404, {}, ""), std::nullopt, "success"},
  };
  const json::array& expected = section("health");
  std::size_t index = 0;
  for (const HealthCase& item : cases) {
    health::resetProviderHealthThrottleForTest();
    *triggers = 0;
    const double before = kv->version;
    rt::Promise<void> recording(dispatch::recordCodexResponseHealth("acct-" + item.name, item.response, item.auth, item.event));
    co_await recording.wait();
    rt::Task<void> settling = settle(40);
    co_await std::move(settling);
    const json::value& row = expected.at(index++);
    EXPECT_EQ(text(row.at("name")), item.name);
    EXPECT_EQ(static_cast<double>(*triggers), *rt::getNumber(row.at("triggers")));
    EXPECT(same(sortedWrites(row.at("writes")), writesAfter(*kv, before), "health " + item.name));
  }
  const std::vector<std::pair<std::string, rt::Reason>> thrown{
    {"refresh_failed", rt::makeReason(CodexError("m", "codex_auth_refresh_failed", 401))},
    {"reused", rt::makeReason(CodexError("m", "refresh_token_reused", 401))},
    {"owner_conflict", rt::makeReason(CodexError("m", "codex_auth_owner_conflict", 409))},
    {"gateway_timeout", rt::makeReason(CodexError("m", "gateway_timeout", 504))},
    {"unreachable", rt::makeReason(CodexError("m", "codex_upstream_unreachable", 502))},
    {"unreachable_400", rt::makeReason(CodexError("m", "codex_upstream_unreachable", 400))},
    {"plain", rt::makeReason(rt::JsError("x"))},
    {"string", rt::thrown(json::value("boom"))},
  };
  for (const auto& [name, error] : thrown) {
    health::resetProviderHealthThrottleForTest();
    *triggers = 0;
    const double before = kv->version;
    rt::Promise<void> recording(dispatch::recordCodexThrownHealth("acct-thrown-" + name, error));
    co_await recording.wait();
    rt::Task<void> settling = settle(40);
    co_await std::move(settling);
    const json::value& row = expected.at(index++);
    EXPECT_EQ(text(row.at("name")), "thrown_" + name);
    EXPECT_EQ(static_cast<double>(*triggers), *rt::getNumber(row.at("triggers")));
    EXPECT(same(sortedWrites(row.at("writes")), writesAfter(*kv, before), "health thrown " + name));
  }
  capacity_events::setProviderCapacitySampleTriggerForTest({});
  health::resetProviderHealthThrottleForTest();
  uos::kv::setKvForTest(nullptr);
}

ASYNC_TEST("dispatch differential: learnCodexModelUnavailable") {
  const CredentialIsolation isolation;
  fixClock();
  const auto kv = std::make_shared<DiffKv>();
  uos::kv::setKvForTest(kv);
  auth::resetCodexAuthCacheForTest();
  availability::resetCodexAccountModelsCacheForTest();
  const CodexAuthState accountA{.access_token = "access-a", .refresh_token = "refresh-a", .account_id = "acct-a", .updated_at_ms = NOW - 10'000};
  const CodexAuthState accountB{.access_token = "access-b", .refresh_token = "refresh-b", .account_id = "acct-b", .updated_at_ms = NOW - 10'000};
  const uos::types::CodexAuthPoolState pool{.accounts = {accountA, accountB}, .updated_at_ms = NOW - 5'000};
  const rt::KvValue poolValue(json::value_from(pool));
  rt::Task<rt::KvCommitResult> setting = kv->set(auth::CODEX_AUTH_POOL_KV_KEY, poolValue);
  const rt::KvCommitResult poolSet = co_await std::move(setting);
  rt::Promise<capacity_routing::CodexRoutingAccountIdentity> identifying(capacity_routing::routingAccountIdentity(accountB));
  const capacity_routing::CodexRoutingAccountIdentity identityB = co_await identifying.wait();
  const json::object active{{"v", 1},
                            {"account_id_hash", identityB.accountIdHash},
                            {"credential_version", identityB.credentialVersion},
                            {"pool_versionstamp", poolSet.versionstamp},
                            {"slot", 1},
                            {"routing_generation", 3},
                            {"generation", 7},
                            {"transition_reason", nullptr},
                            {"updated_at_ms", NOW - 1'000}};
  const rt::KvValue activeValue(active);
  rt::Task<rt::KvCommitResult> activating = kv->set(routing_state::CODEX_ACTIVE_ACCOUNT_SELECTION_KV_KEY, activeValue);
  co_await std::move(activating);
  const std::string detail = "The 'gpt-5' model is not supported when using Codex with a ChatGPT account.";
  const auto make = [](double status, const json::value& payload, std::optional<double> generation, std::optional<double> slot) {
    rt::ResponseInit init;
    init.status = status;
    const rt::Response response(rt::BodyInit(rt::jsonStringify(payload)), init);
    if (slot.has_value()) auth::setCodexResponseAccountTelemetry(response, *slot, "acct-b");
    if (generation.has_value()) {
      routing_state::RoutingAccount routing;
      routing.activeGeneration = *generation;
      routing.activeTransitionReason = nullptr;
      auth::setCodexResponseActiveTelemetry(response, routing);
    }
    return response;
  };
  const json::array& expected = section("learn");
  for (const json::value& row : expected) {
    const std::string name = nameOf(row);
    const double before = kv->version;
    std::optional<rt::Response> response;
    if (name == "match") response = make(400, json::object{{"detail", detail}}, 7.0, 2.0);
    if (name == "generation_mismatch") response = make(400, json::object{{"detail", detail}}, 8.0, 2.0);
    if (name == "no_active_telemetry") response = make(400, json::object{{"detail", detail}}, std::nullopt, 2.0);
    if (name == "no_slot") response = make(400, json::object{{"detail", detail}}, 7.0, std::nullopt);
    if (name == "status_200") response = make(200, json::object{{"detail", detail}}, 7.0, 2.0);
    if (name == "other_detail") response = make(400, json::object{{"detail", "nope"}}, 7.0, 2.0);
    if (name == "pool_rotated") {
      uos::types::CodexAuthPoolState rotated = pool;
      rotated.updated_at_ms = NOW - 4'000;
      const rt::KvValue rotatedValue(json::value_from(rotated));
      rt::Task<rt::KvCommitResult> rotating = kv->set(auth::CODEX_AUTH_POOL_KV_KEY, rotatedValue);
      co_await std::move(rotating);
      response = make(400, json::object{{"detail", "The 'gpt-6' model is not supported when using Codex with a ChatGPT account."}}, 7.0, 2.0);
    }
    rt::Promise<std::optional<dispatch::CodexModelUnavailableAttempt>> learning(dispatch::learnCodexModelUnavailable(*response));
    const std::optional<dispatch::CodexModelUnavailableAttempt> learned = co_await learning.wait();
    rt::Task<void> settling = settle(20);
    co_await std::move(settling);
    json::object actual;
    actual["name"] = name;
    actual["learned"] = learned.has_value() ? json::value(json::object{{"accountId", learned->accountId}, {"model", learned->model}, {"detail", learned->detail}})
                                            : json::value(nullptr);
    actual["writes"] = writesAfter(*kv, before);
    if (response->bodyUsed()) {
      actual["passthrough"] = nullptr;
    } else {
      auto reading = response->text();
      actual["passthrough"] = co_await std::move(reading);
    }
    json::object wanted = row.as_object();
    wanted["writes"] = sortedWrites(row.at("writes"));
    EXPECT(same(wanted, actual, "learn " + name));
  }
  uos::kv::setKvForTest(nullptr);
  auth::resetCodexAuthCacheForTest();
  availability::resetCodexAccountModelsCacheForTest();
}
