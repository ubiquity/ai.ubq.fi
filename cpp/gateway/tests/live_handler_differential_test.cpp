// Links: tests/live_handler_differential_test.cpp
//   src/auth/relay.cpp src/authority/native_codex.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp
//   src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/authority/native_paid.cpp
//   src/authority/native_paid_contracts.cpp src/bounded_response_body.cpp src/cache/prompt_analytics.cpp src/cache/prompt_analytics_core.cpp
//   src/cache/prompt_analytics_queue.cpp src/cache/prompt_analytics_read.cpp src/cache/telemetry_gate.cpp src/codex/account_routing.cpp
//   src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/banked_reset.cpp src/codex/banked_reset_provider.cpp
//   src/codex/capacity_routing.cpp src/codex/dispatch.cpp src/codex/experiment.cpp src/codex/external_auth.cpp
//   src/codex/native_auth.cpp src/codex/native_auth_transport.cpp src/codex/overage_settings.cpp src/codex/rate_limit_429.cpp
//   src/codex/reset_settings.cpp src/codex/routing_evaluation.cpp src/codex/routing_mutations.cpp src/codex/routing_serial.cpp
//   src/codex/routing_state.cpp src/config.cpp src/http.cpp src/inference_deadline.cpp
//   src/kv.cpp src/metered_quota.cpp src/models/codex_models_availability.cpp src/optional_telemetry_queue.cpp
//   src/paid_fallback/ledger_state.cpp src/paid_fallback/ledger_stats.cpp src/provider/capacity.cpp src/provider/capacity_analytics.cpp
//   src/provider/capacity_contract.cpp src/provider/capacity_events.cpp src/provider/capacity_history.cpp src/provider/capacity_parse.cpp
//   src/provider/capacity_rollups.cpp src/provider/cerebras_rate_limits.cpp src/provider/claude_capacity_history.cpp src/provider/claude_capacity_rollups.cpp
//   src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/claude_code_quota.cpp src/provider/claude_code_usage_schema.cpp
//   src/provider/health.cpp src/provider/selection.cpp src/release.cpp src/runtime/http.cpp
//   src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp
//   src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
//   src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp src/types.cpp
//   src/utils.cpp src/live/upstream.cpp src/handler/http.cpp src/authority/native_contracts.cpp
//   src/auth/passport.cpp src/api_key_policy.cpp src/authority/native_api_key.cpp src/codex/quota.cpp
//   src/auth/passport_sessions.cpp src/authority/native_api_key_policy.cpp src/api_keys.cpp src/authority/native_paid_policy.cpp
//   src/live/handler.cpp src/live/routes.cpp
// Differential vectors recorded from the real src/live/handler.ts under Bun 1.4.0.
// Authentication and live call/sideband boundaries are mocked; routing, principal resolution and
// HTTP decorators are the real modules. The existing live-relay fixture isolates credentials,
// memory KV and provider fetch. No socket is opened and no real account is read.
#include "tests/helpers/live_relay_harness.hpp"
#include "uos/auth/index.hpp"
#include "uos/live/calls.hpp"
#include "uos/live/handler.hpp"
#include "uos/live/sideband.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/url.hpp"

#include <algorithm>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <variant>

namespace rt = uos::rt;
namespace json = uos::json;
namespace harness = uos::tests::helpers::live_relay_harness;
namespace auth = uos::auth::index;
namespace upgrade = uos::runtime::http_upgrade;

namespace {
  const json::value& vectors() {
    static const auto value = rt::jsonParse(R"oracle([{"input":{"name":"HTTP non-route bypass","path":"/other","method":"GET","auth":"reject"},"expected":{"kind":"null","trace":[["after-start"]],"randomCalls":0,"requestBodyUsed":false}},{"input":{"name":"HTTP wrong method bypass","path":"/v1/live","method":"GET","auth":"reject"},"expected":{"kind":"null","trace":[["after-start"]],"randomCalls":0,"requestBodyUsed":false}},{"input":{"name":"HTTP malformed escape bypass","path":"/v1/live/%zz","method":"GET","auth":"reject"},"expected":{"kind":"null","trace":[["after-start"]],"randomCalls":0,"requestBodyUsed":false}},{"input":{"name":"HTTP auth refusal decorated","path":"/v1/live","method":"POST","auth":"deny","existingId":true},"expected":{"kind":"response","status":409,"statusText":"Oracle Conflict","headers":{"x-uos-request-id":"request-fixed","access-control-allow-origin":"*","access-control-allow-credentials":null,"vary":"Accept, origin","location":null,"content-type":null,"x-fixture":"preserved","x-uos-git-sha":"unknown","x-uos-deployment-id":"unknown"},"exposesRequestId":true,"body":"dependency-body","sameDependency":false,"dependencyRequestId":"dependency-id","dependencyVary":"Accept, origin","trace":[["auth",true,"POST","/v1/live",false],["after-start"]],"randomCalls":0,"requestBodyUsed":false}},{"input":{"name":"HTTP call authenticates once","path":"/v1/live","method":"POST","response":"answer"},"expected":{"kind":"response","status":201,"statusText":"Oracle Accepted","headers":{"x-uos-request-id":"request-fixed","access-control-allow-origin":"*","access-control-allow-credentials":null,"vary":"Accept, origin","location":"/v1/live/rtc_live_calls_http","content-type":"application/sdp","x-fixture":"preserved","x-uos-git-sha":"unknown","x-uos-deployment-id":"unknown"},"exposesRequestId":true,"body":"v=0\r\no=- 2 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n","sameDependency":false,"dependencyRequestId":null,"dependencyVary":"Accept, origin","trace":[["auth",true,"POST","/v1/live",false],["after-start"],["call",true,"POST","/v1/live",false,"api-key:live-calls-http-key"]],"randomCalls":1,"requestBodyUsed":false}},{"input":{"name":"HTTP normalized call and trusted origin","path":"/v1/live///?ignored=1","method":"POST","response":"answer","origin":"https://ai.ubq.fi","existingId":true},"expected":{"kind":"response","status":201,"statusText":"Oracle Accepted","headers":{"x-uos-request-id":"request-fixed","access-control-allow-origin":"https://ai.ubq.fi","access-control-allow-credentials":"true","vary":"Accept, Origin","location":"/v1/live/rtc_live_calls_http","content-type":"application/sdp","x-fixture":"preserved","x-uos-git-sha":"unknown","x-uos-deployment-id":"unknown"},"exposesRequestId":true,"body":"v=0\r\no=- 2 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n","sameDependency":false,"dependencyRequestId":"dependency-id","dependencyVary":"Accept, origin","trace":[["auth",true,"POST","/v1/live///",false],["after-start"],["call",true,"POST","/v1/live///",false,"api-key:live-calls-http-key"]],"randomCalls":0,"requestBodyUsed":false}},{"input":{"name":"HTTP sideband decoded identifier","path":"/v1/live/a%2Fb///","method":"GET","origin":"https://untrusted.invalid"},"expected":{"kind":"response","status":409,"statusText":"Oracle Conflict","headers":{"x-uos-request-id":"request-fixed","access-control-allow-origin":"*","access-control-allow-credentials":null,"vary":"Accept, origin","location":null,"content-type":null,"x-fixture":"preserved","x-uos-git-sha":"unknown","x-uos-deployment-id":"unknown"},"exposesRequestId":true,"body":"dependency-body","sameDependency":false,"dependencyRequestId":null,"dependencyVary":"Accept, origin","trace":[["auth",true,"GET","/v1/live/a%2Fb///",false],["after-start"],["sideband",true,"GET","/v1/live/a%2Fb///",false,"a/b","api-key:live-calls-http-key"]],"randomCalls":1,"requestBodyUsed":false}},{"input":{"name":"HTTP GitHub principal","path":"/v1/live/id","method":"GET","auth":"github","origin":"https://ai.ubq.fi"},"expected":{"kind":"response","status":409,"statusText":"Oracle Conflict","headers":{"x-uos-request-id":"request-fixed","access-control-allow-origin":"https://ai.ubq.fi","access-control-allow-credentials":"true","vary":"Accept, Origin","location":null,"content-type":null,"x-fixture":"preserved","x-uos-git-sha":"unknown","x-uos-deployment-id":"unknown"},"exposesRequestId":true,"body":"dependency-body","sameDependency":false,"dependencyRequestId":null,"dependencyVary":"Accept, origin","trace":[["auth",true,"GET","/v1/live/id",false],["after-start"],["sideband",true,"GET","/v1/live/id",false,"id","github-repo:owner/repo"]],"randomCalls":1,"requestBodyUsed":false}},{"input":{"name":"HTTP disabled no-token principal","path":"/v1/live/id","method":"GET","auth":"disabled"},"expected":{"kind":"response","status":409,"statusText":"Oracle Conflict","headers":{"x-uos-request-id":"request-fixed","access-control-allow-origin":"*","access-control-allow-credentials":null,"vary":"Accept, origin","location":null,"content-type":null,"x-fixture":"preserved","x-uos-git-sha":"unknown","x-uos-deployment-id":"unknown"},"exposesRequestId":true,"body":"dependency-body","sameDependency":false,"dependencyRequestId":null,"dependencyVary":"Accept, origin","trace":[["auth",true,"GET","/v1/live/id",false],["after-start"],["sideband",true,"GET","/v1/live/id",false,"id","local-auth-disabled"]],"randomCalls":1,"requestBodyUsed":false}},{"input":{"name":"HTTP disabled bearer principal","path":"/v1/live/id","method":"GET","auth":"disabled-token"},"expected":{"kind":"response","status":409,"statusText":"Oracle Conflict","headers":{"x-uos-request-id":"request-fixed","access-control-allow-origin":"*","access-control-allow-credentials":null,"vary":"Accept, origin","location":null,"content-type":null,"x-fixture":"preserved","x-uos-git-sha":"unknown","x-uos-deployment-id":"unknown"},"exposesRequestId":true,"body":"dependency-body","sameDependency":false,"dependencyRequestId":null,"dependencyVary":"Accept, origin","trace":[["auth",true,"GET","/v1/live/id",false],["after-start"],["sideband",true,"GET","/v1/live/id",false,"id","bearer-sha256:a80d7ede50e6fef0c528ca495bbeac37ea9b70aeb4b49e0e5c439343d200c1c0"]],"randomCalls":1,"requestBodyUsed":false}},{"input":{"name":"HTTP allowlist principal","path":"/v1/live/id","method":"GET","auth":"allowlist"},"expected":{"kind":"response","status":409,"statusText":"Oracle Conflict","headers":{"x-uos-request-id":"request-fixed","access-control-allow-origin":"*","access-control-allow-credentials":null,"vary":"Accept, origin","location":null,"content-type":null,"x-fixture":"preserved","x-uos-git-sha":"unknown","x-uos-deployment-id":"unknown"},"exposesRequestId":true,"body":"dependency-body","sameDependency":false,"dependencyRequestId":null,"dependencyVary":"Accept, origin","trace":[["auth",true,"GET","/v1/live/id",false],["after-start"],["sideband",true,"GET","/v1/live/id",false,"id","auth-method:auth_tokens_allowlist"]],"randomCalls":1,"requestBodyUsed":false}},{"input":{"name":"HTTP authentication gate","path":"/v1/live","method":"POST","auth":"held","response":"answer"},"expected":{"kind":"response","status":201,"statusText":"Oracle Accepted","headers":{"x-uos-request-id":"request-fixed","access-control-allow-origin":"*","access-control-allow-credentials":null,"vary":"Accept, origin","location":"/v1/live/rtc_live_calls_http","content-type":"application/sdp","x-fixture":"preserved","x-uos-git-sha":"unknown","x-uos-deployment-id":"unknown"},"exposesRequestId":true,"body":"v=0\r\no=- 2 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\nm=audio 9 UDP/TLS/RTP/SAVPF 111\r\n","sameDependency":false,"dependencyRequestId":null,"dependencyVary":"Accept, origin","trace":[["auth",true,"POST","/v1/live",false],["after-start"],["auth-resumed"],["call",true,"POST","/v1/live",false,"api-key:live-calls-http-key"]],"randomCalls":1,"requestBodyUsed":false}},{"input":{"name":"HTTP auth rejection escapes","path":"/v1/live","method":"POST","auth":"reject"},"expected":{"kind":"rejected","name":"Error","message":"auth rejected","trace":[["auth",true,"POST","/v1/live",false],["after-start"]],"randomCalls":0,"requestBodyUsed":false}},{"input":{"name":"HTTP call rejection escapes","path":"/v1/live","method":"POST","dispatch":"reject"},"expected":{"kind":"rejected","name":"Error","message":"call rejected","trace":[["auth",true,"POST","/v1/live",false],["after-start"],["call",true,"POST","/v1/live",false,"api-key:live-calls-http-key"]],"randomCalls":0,"requestBodyUsed":false}},{"input":{"name":"HTTP sideband rejection escapes","path":"/v1/live/id","method":"GET","dispatch":"reject"},"expected":{"kind":"rejected","name":"Error","message":"sideband rejected","trace":[["auth",true,"GET","/v1/live/id",false],["after-start"],["sideband",true,"GET","/v1/live/id",false,"id","api-key:live-calls-http-key"]],"randomCalls":0,"requestBodyUsed":false}},{"input":{"name":"Upgrade non-route rejects before auth","path":"/other","method":"GET","upgrade":true,"auth":"reject"},"expected":{"kind":"response","status":404,"statusText":"","headers":{"x-uos-request-id":"00000000-0000-4000-8000-000000000000","access-control-allow-origin":"*","access-control-allow-credentials":null,"vary":null,"location":null,"content-type":"application/json","x-fixture":null,"x-uos-git-sha":"unknown","x-uos-deployment-id":"unknown"},"exposesRequestId":true,"body":"{\"error\":{\"message\":\"Unknown WebSocket endpoint.\",\"type\":\"invalid_request_error\",\"code\":\"not_found\"}}","sameDependency":false,"dependencyRequestId":null,"dependencyVary":null,"trace":[["after-start"]],"randomCalls":2,"requestBodyUsed":false}},{"input":{"name":"Upgrade call route rejects before auth","path":"/v1/live","method":"POST","upgrade":true,"auth":"reject"},"expected":{"kind":"response","status":404,"statusText":"","headers":{"x-uos-request-id":"00000000-0000-4000-8000-000000000000","access-control-allow-origin":"*","access-control-allow-credentials":null,"vary":null,"location":null,"content-type":"application/json","x-fixture":null,"x-uos-git-sha":"unknown","x-uos-deployment-id":"unknown"},"exposesRequestId":true,"body":"{\"error\":{\"message\":\"Unknown WebSocket endpoint.\",\"type\":\"invalid_request_error\",\"code\":\"not_found\"}}","sameDependency":false,"dependencyRequestId":null,"dependencyVary":null,"trace":[["after-start"]],"randomCalls":2,"requestBodyUsed":false}},{"input":{"name":"Upgrade malformed escape rejects before auth","path":"/v1/live/%zz","method":"GET","upgrade":true,"auth":"reject"},"expected":{"kind":"response","status":404,"statusText":"","headers":{"x-uos-request-id":"00000000-0000-4000-8000-000000000000","access-control-allow-origin":"*","access-control-allow-credentials":null,"vary":null,"location":null,"content-type":"application/json","x-fixture":null,"x-uos-git-sha":"unknown","x-uos-deployment-id":"unknown"},"exposesRequestId":true,"body":"{\"error\":{\"message\":\"Unknown WebSocket endpoint.\",\"type\":\"invalid_request_error\",\"code\":\"not_found\"}}","sameDependency":false,"dependencyRequestId":null,"dependencyVary":null,"trace":[["after-start"]],"randomCalls":2,"requestBodyUsed":false}},{"input":{"name":"Upgrade auth refusal decorated","path":"/v1/live/id","method":"GET","upgrade":true,"auth":"deny","existingId":true},"expected":{"kind":"response","status":409,"statusText":"Oracle Conflict","headers":{"x-uos-request-id":"00000000-0000-4000-8000-000000000000","access-control-allow-origin":"*","access-control-allow-credentials":null,"vary":"Accept, origin","location":null,"content-type":null,"x-fixture":"preserved","x-uos-git-sha":"unknown","x-uos-deployment-id":"unknown"},"exposesRequestId":true,"body":"dependency-body","sameDependency":false,"dependencyRequestId":"dependency-id","dependencyVary":"Accept, origin","trace":[["auth",true,"GET","/v1/live/id",false],["after-start"]],"randomCalls":1,"requestBodyUsed":false}},{"input":{"name":"Upgrade preparation response decorated","path":"/v1/live/a%2Fb///","method":"GET","upgrade":true,"origin":"https://ai.ubq.fi"},"expected":{"kind":"response","status":409,"statusText":"Oracle Conflict","headers":{"x-uos-request-id":"00000000-0000-4000-8000-000000000000","access-control-allow-origin":"https://ai.ubq.fi","access-control-allow-credentials":"true","vary":"Accept, Origin","location":null,"content-type":null,"x-fixture":"preserved","x-uos-git-sha":"unknown","x-uos-deployment-id":"unknown"},"exposesRequestId":true,"body":"dependency-body","sameDependency":false,"dependencyRequestId":null,"dependencyVary":"Accept, origin","trace":[["auth",true,"GET","/v1/live/a%2Fb///",false],["after-start"],["prepare",true,"GET","/v1/live/a%2Fb///",false,"a/b","api-key:live-calls-http-key"]],"randomCalls":2,"requestBodyUsed":false}},{"input":{"name":"Upgrade descriptor remains untouched","path":"/v1/live/id","method":"GET","upgrade":true,"dispatch":"upgrade"},"expected":{"kind":"upgrade","samePrepared":true,"maxPayloadBytes":1234,"headers":{"x-accepted":"retain","x-uos-request-id":"descriptor-id"},"payloadLimitClose":{"code":1011,"reason":"fixture limit"},"trace":[["auth",true,"GET","/v1/live/id",false],["after-start"],["prepare",true,"GET","/v1/live/id",false,"id","api-key:live-calls-http-key"],["accept","fixture"]],"randomCalls":1,"requestBodyUsed":false}},{"input":{"name":"Upgrade authentication gate","path":"/v1/live/id","method":"GET","upgrade":true,"auth":"held","dispatch":"upgrade"},"expected":{"kind":"upgrade","samePrepared":true,"maxPayloadBytes":1234,"headers":{"x-accepted":"retain","x-uos-request-id":"descriptor-id"},"payloadLimitClose":{"code":1011,"reason":"fixture limit"},"trace":[["auth",true,"GET","/v1/live/id",false],["after-start"],["auth-resumed"],["prepare",true,"GET","/v1/live/id",false,"id","api-key:live-calls-http-key"],["accept","fixture"]],"randomCalls":1,"requestBodyUsed":false}},{"input":{"name":"Upgrade auth rejection escapes","path":"/v1/live/id","method":"GET","upgrade":true,"auth":"reject"},"expected":{"kind":"rejected","name":"Error","message":"auth rejected","trace":[["auth",true,"GET","/v1/live/id",false],["after-start"]],"randomCalls":1,"requestBodyUsed":false}},{"input":{"name":"Upgrade preparation rejection escapes","path":"/v1/live/id","method":"GET","upgrade":true,"dispatch":"reject"},"expected":{"kind":"rejected","name":"Error","message":"prepare rejected","trace":[["auth",true,"GET","/v1/live/id",false],["after-start"],["prepare",true,"GET","/v1/live/id",false,"id","api-key:live-calls-http-key"]],"randomCalls":1,"requestBodyUsed":false}}])oracle");
    return value;
  }
  std::string text(const json::value& input, std::string_view key) {
    return std::string(rt::getString(input, key).value_or(""));
  }
  bool flag(const json::value& input, std::string_view key) {
    return rt::getBool(input, key).value_or(false);
  }
  struct Scenario {
    json::value input;
    std::optional<rt::Request> request;
    std::optional<rt::Response> dependency;
    std::optional<upgrade::WebSocketUpgrade> prepared;
    std::optional<rt::Resolvers<void>> authGate;
    std::shared_ptr<bool> accepted = std::make_shared<bool>(false);
    json::array trace;
    std::size_t randomCalls = 0;
  };
  Scenario* activeScenario = nullptr;
  Scenario& active() {
    if (!activeScenario) throw rt::JsError("live handler fixture is inactive");
    return *activeScenario;
  }
  void record(std::string kind, const rt::Request& req, const std::optional<std::string>& callId = std::nullopt,
              const std::optional<std::string>& principal = std::nullopt) {
    auto& scenario = active();
    json::array row{kind, req == *scenario.request, req.method(), rt::URL(req.url()).pathname(), req.bodyUsed()};
    if (callId) row.push_back(json::value(*callId));
    if (principal) row.push_back(json::value(*principal));
    scenario.trace.push_back(std::move(row));
  }
  rt::Response response() {
    auto& scenario = active();
    rt::Headers headers{{"x-fixture", "preserved"}, {"Vary", "Accept, origin"}};
    if (flag(scenario.input, "existingId")) headers.set("x-uos-request-id", "dependency-id");
    if (text(scenario.input, "response") == "answer") {
      auto answer = harness::acceptedAnswer();
      headers.set("Location", "/v1/live/" + std::string(harness::CALL_ID));
      headers.set("Content-Type", "application/sdp");
      rt::ResponseInit init;
      init.status = answer.status();
      init.statusText = "Oracle Accepted";
      init.headers = headers;
      scenario.dependency = rt::Response(rt::bodyInit(answer.body()), init);
    } else {
      scenario.dependency = rt::Response("dependency-body", rt::ResponseInit{.status = 409, .statusText = "Oracle Conflict", .headers = headers});
    }
    return *scenario.dependency;
  }
  auth::AuthenticateClientResult successAuth() {
    const auto mode = text(active().input, "auth");
    auth::ClientAuthMethod method = auth::ClientAuthMethodKvApiKey{.key_id = "live-calls-http-key", .policy = {}};
    std::optional<std::string> token = "fixture-client-bearer";
    if (mode == "github") method = auth::ClientAuthMethodGithubToken{"Owner", "Repo", "state", "repo"};
    else if (mode == "disabled" || mode == "disabled-token") {
      method = auth::ClientAuthMethodDisabled{};
      if (mode == "disabled") token.reset();
    } else if (mode == "allowlist") method = auth::ClientAuthMethodAuthTokensAllowlist{};
    return auth::AuthenticateClientResultOk{.token = token, .method = method};
  }
  json::value headerValue(const rt::Headers& headers, std::string_view name) {
    const auto value = headers.get(name);
    return value ? json::value(*value) : json::value(nullptr);
  }
  rt::Task<json::object> summarizeResponse(rt::Response result) {
    const auto& scenario = active();
    json::object headers;
    for (const std::string_view name : {"x-uos-request-id", "access-control-allow-origin", "access-control-allow-credentials", "vary", "location",
                                       "content-type", "x-fixture", "x-uos-git-sha", "x-uos-deployment-id"}) {
      headers.emplace(name, headerValue(result.headers(), name));
    }
    const auto exposed = rt::split(result.headers().get("access-control-expose-headers").value_or(""), ",");
    const bool exposesRequestId = std::find(exposed.begin(), exposed.end(), "x-uos-request-id") != exposed.end();
    auto reading = result.text();
    const auto body = co_await std::move(reading);
    json::object summary{{"kind", "response"}, {"status", result.status()}, {"statusText", result.statusText()}, {"headers", headers},
                         {"exposesRequestId", exposesRequestId}, {"body", body}, {"sameDependency", scenario.dependency && result == *scenario.dependency},
                         {"dependencyRequestId", scenario.dependency ? headerValue(scenario.dependency->headers(), "x-uos-request-id") : json::value(nullptr)},
                         {"dependencyVary", scenario.dependency ? headerValue(scenario.dependency->headers(), "vary") : json::value(nullptr)}};
    co_return summary;
  }
  json::object summarizeUpgrade(const upgrade::WebSocketUpgrade& result) {
    auto& scenario = active();
    result.accept(nullptr);
    const auto& original = *scenario.prepared;
    // TS object identity is checked through the copied descriptor's original callback and fields.
    const bool samePrepared = *scenario.accepted && result.accept.target_type() == original.accept.target_type() &&
      result.maxPayloadBytes == original.maxPayloadBytes && result.headers->entries() == original.headers->entries() &&
      result.payloadLimitClose->code == original.payloadLimitClose->code && result.payloadLimitClose->reason == original.payloadLimitClose->reason;
    json::object headers;
    for (const auto& [name, value] : result.headers->entries()) headers.emplace(name, value);
    return {{"kind", "upgrade"}, {"samePrepared", samePrepared}, {"maxPayloadBytes", result.maxPayloadBytes}, {"headers", headers},
            {"payloadLimitClose", json::object{{"code", result.payloadLimitClose->code}, {"reason", result.payloadLimitClose->reason}}}};
  }
  rt::Task<void> runScenario(std::size_t index) {
    harness::Fixture fixture;
    const auto savedOrigin = uos::config::config.allowOrigin;
    uos::config::config.allowOrigin = "*";
    auto restore = rt::scopeExit([savedOrigin] { uos::config::config.allowOrigin = savedOrigin; activeScenario = nullptr; });
    Scenario scenario;
    const auto& row = vectors().get_array().at(index);
    scenario.input = *rt::get(row, "input");
    activeScenario = &scenario;
    rt::crypto::ScopedRandomSource random([&scenario](std::span<std::uint8_t> bytes) {
      ++scenario.randomCalls;
      std::fill(bytes.begin(), bytes.end(), std::uint8_t{0});
    });
    rt::Headers headers{{"authorization", "Bearer fixture-client-bearer"}};
    if (const auto origin = rt::getString(scenario.input, "origin")) headers.set("origin", *origin);
    const auto url = "http://fixture.invalid" + text(scenario.input, "path");
    const auto method = text(scenario.input, "method");
    if (method == "POST") {
      const auto base = harness::callRequest("fixture-unread-body", headers);
      scenario.request = rt::Request(url, base);
    } else {
      rt::RequestInit init;
      init.method = method;
      init.headers = headers;
      scenario.request = rt::Request(url, init);
    }
    json::object observed;
    rt::Reason failure;
    try {
      if (flag(scenario.input, "upgrade")) {
        upgrade::UpgradeInfo info;
        rt::Promise<upgrade::UpgradeResult> pending(uos::live::handler::handleLiveUpgrade(*scenario.request, info));
        scenario.trace.push_back(json::array{"after-start"});
        if (scenario.authGate) scenario.authGate->resolve();
        const auto result = co_await pending.wait();
        if (const auto* httpResponse = std::get_if<rt::Response>(&result)) {
          auto reading = summarizeResponse(*httpResponse);
          observed = co_await std::move(reading);
        } else observed = summarizeUpgrade(std::get<upgrade::WebSocketUpgrade>(result));
      } else {
        const std::string requestId = "request-fixed";
        rt::Promise<std::optional<rt::Response>> pending(uos::live::handler::handleLiveRoute(*scenario.request, requestId));
        scenario.trace.push_back(json::array{"after-start"});
        if (scenario.authGate) scenario.authGate->resolve();
        const auto result = co_await pending.wait();
        if (result) {
          auto reading = summarizeResponse(*result);
          observed = co_await std::move(reading);
        } else observed = json::object{{"kind", "null"}};
      }
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) observed = json::object{{"kind", "rejected"}, {"name", rt::errorName(failure).value_or("")}, {"message", rt::errorMessage(failure).value_or("")}};
    observed.emplace("trace", scenario.trace);
    observed.emplace("randomCalls", scenario.randomCalls);
    observed.emplace("requestBodyUsed", scenario.request->bodyUsed());
    EXPECT_EQ(rt::jsonStringify(observed), rt::jsonStringify(*rt::get(row, "expected")));
    EXPECT(fixture.calls.empty());
    EXPECT(fixture.refreshTokens.empty());
    EXPECT(!rt::env::has("CODEX_HOME"));
    EXPECT(!rt::env::has("CODEX_AUTH_JSON_B64"));
    EXPECT(uos::config::config.codexAuthJsonB64.empty());
    auto settling = fixture.settle();
    co_await std::move(settling);
  }
}

namespace uos::auth::index {
  rt::Task<AuthenticateClientResult> authenticateClient(rt::Request req) {
    record("auth", req);
    auto& scenario = active();
    const auto mode = text(scenario.input, "auth");
    if (mode == "reject") throw rt::JsError("auth rejected");
    if (mode == "held") {
      scenario.authGate = rt::withResolvers<void>();
      co_await scenario.authGate->promise.wait();
      scenario.trace.push_back(json::array{"auth-resumed"});
    }
    if (mode == "deny") co_return AuthenticateClientResultNotOk{response()};
    co_return successAuth();
  }
}
namespace uos::live::calls {
  rt::Task<rt::Response> handleLiveCallCreate(rt::Request req, std::string principal, std::optional<FailedLiveCallCapture>) {
    record("call", req, std::nullopt, principal);
    if (text(active().input, "dispatch") == "reject") throw rt::JsError("call rejected");
    co_return response();
  }
}
namespace uos::live::sideband {
  rt::Task<rt::Response> handleLiveSideband(rt::Request req, std::string callId, std::string principal) {
    record("sideband", req, callId, principal);
    if (text(active().input, "dispatch") == "reject") throw rt::JsError("sideband rejected");
    co_return response();
  }
  rt::Task<upgrade::UpgradeResult> prepareLiveSidebandUpgrade(rt::Request req, std::string callId, std::string principal) {
    record("prepare", req, callId, principal);
    auto& scenario = active();
    const auto dispatch = text(scenario.input, "dispatch");
    if (dispatch == "reject") throw rt::JsError("prepare rejected");
    if (dispatch == "upgrade") {
      upgrade::WebSocketUpgrade result;
      result.accept = [accepted = scenario.accepted](std::shared_ptr<rt::ws::WebSocket>) {
        *accepted = true;
        active().trace.push_back(json::array{"accept", "fixture"});
      };
      result.maxPayloadBytes = 1234;
      result.headers = rt::Headers{{"x-accepted", "retain"}, {"x-uos-request-id", "descriptor-id"}};
      result.payloadLimitClose = upgrade::PayloadLimitClose{1011, "fixture limit"};
      scenario.prepared = result;
      co_return result;
    }
    co_return response();
  }
}

ASYNC_TEST("live handler HTTP non-route bypass") { auto task = runScenario(0); co_await std::move(task); }
ASYNC_TEST("live handler HTTP wrong method bypass") { auto task = runScenario(1); co_await std::move(task); }
ASYNC_TEST("live handler HTTP malformed escape bypass") { auto task = runScenario(2); co_await std::move(task); }
ASYNC_TEST("live handler HTTP auth refusal decorated") { auto task = runScenario(3); co_await std::move(task); }
ASYNC_TEST("live handler HTTP call authenticates once") { auto task = runScenario(4); co_await std::move(task); }
ASYNC_TEST("live handler HTTP normalized call and trusted origin") { auto task = runScenario(5); co_await std::move(task); }
ASYNC_TEST("live handler HTTP sideband decoded identifier") { auto task = runScenario(6); co_await std::move(task); }
ASYNC_TEST("live handler HTTP GitHub principal") { auto task = runScenario(7); co_await std::move(task); }
ASYNC_TEST("live handler HTTP disabled no-token principal") { auto task = runScenario(8); co_await std::move(task); }
ASYNC_TEST("live handler HTTP disabled bearer principal") { auto task = runScenario(9); co_await std::move(task); }
ASYNC_TEST("live handler HTTP allowlist principal") { auto task = runScenario(10); co_await std::move(task); }
ASYNC_TEST("live handler HTTP authentication gate") { auto task = runScenario(11); co_await std::move(task); }
ASYNC_TEST("live handler HTTP auth rejection escapes") { auto task = runScenario(12); co_await std::move(task); }
ASYNC_TEST("live handler HTTP call rejection escapes") { auto task = runScenario(13); co_await std::move(task); }
ASYNC_TEST("live handler HTTP sideband rejection escapes") { auto task = runScenario(14); co_await std::move(task); }
ASYNC_TEST("live handler Upgrade non-route rejects before auth") { auto task = runScenario(15); co_await std::move(task); }
ASYNC_TEST("live handler Upgrade call route rejects before auth") { auto task = runScenario(16); co_await std::move(task); }
ASYNC_TEST("live handler Upgrade malformed escape rejects before auth") { auto task = runScenario(17); co_await std::move(task); }
ASYNC_TEST("live handler Upgrade auth refusal decorated") { auto task = runScenario(18); co_await std::move(task); }
ASYNC_TEST("live handler Upgrade preparation response decorated") { auto task = runScenario(19); co_await std::move(task); }
ASYNC_TEST("live handler Upgrade descriptor remains untouched") { auto task = runScenario(20); co_await std::move(task); }
ASYNC_TEST("live handler Upgrade authentication gate") { auto task = runScenario(21); co_await std::move(task); }
ASYNC_TEST("live handler Upgrade auth rejection escapes") { auto task = runScenario(22); co_await std::move(task); }
ASYNC_TEST("live handler Upgrade preparation rejection escapes") { auto task = runScenario(23); co_await std::move(task); }

ASYNC_TEST("live WebSocket rejection logs one metadata-only request event") {
  const rt::console::Capture logs({rt::console::Method::info});
  auto unknown = runScenario(15);
  co_await std::move(unknown);
  const auto calls = logs.calls();
  ASSERT_EQ(calls.size(), 1U);
  ASSERT_EQ(calls.front().args.size(), 2U);
  EXPECT(calls.front().args[0] == std::string_view("[ai.ubq.fi] request_failure"));
  EXPECT(calls.front().text.find("fixture-client-bearer") == std::string::npos);
  EXPECT(calls.front().text.find("/other") == std::string::npos);
  const auto event = rt::jsonParse(calls.front().args[1].string());
  EXPECT_EQ(rt::getString(event, "route").value(), "live.upgrade");
  EXPECT_EQ(rt::getString(event, "request_id").value(), "00000000-0000-4000-8000-000000000000");
  EXPECT_EQ(rt::getBool(event, "body_present"), std::optional<bool>(false));
  EXPECT_EQ(rt::getBool(event, "authenticated"), std::optional<bool>(false));
  EXPECT_EQ(rt::getString(event, "body_evidence").value(), "no_body");
}
