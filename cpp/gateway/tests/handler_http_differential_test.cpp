// Differential cases for src/handler/http.ts, recorded from Bun 1.4.0 with no credentials.
// Full decisions/embeddings/systemone/Passport/provider handler integration cases require handler/index.ts.
// Links: tests/handler_http_differential_test.cpp
// src/handler/http.cpp src/authority/native_contracts.cpp src/utils.cpp src/auth/passport.cpp
// src/api_key_policy.cpp src/http.cpp src/types.cpp src/authority/native_api_key.cpp
// src/codex/quota.cpp src/auth/passport_sessions.cpp src/provider/cerebras_rate_limits.cpp src/authority/native_api_key_policy.cpp
// src/api_keys.cpp src/auth/relay.cpp src/authority/native_paid_policy.cpp src/kv.cpp
// src/config.cpp src/storage/kv.cpp src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp
// src/release.cpp
#include "uos/handler/http.hpp"
#include "uos/authority/native_contracts.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <optional>
#include <string>
#include <vector>
#define AWAIT(name, expression) auto name##Task = (expression); auto name = co_await std::move(name##Task)
namespace {
  namespace rt = uos::rt; namespace json = uos::json; namespace http = uos::handler::http; namespace auth = uos::auth::index;
  const json::value& vectors() { static const auto value = rt::jsonParse(
#include "tests/handler_http_vectors.inc"
  ); return value; }
  json::array headersJson(const rt::Headers& headers) { json::array result; for (const auto& [key, value] : headers.entries()) result.push_back(json::array{key, value}); return result; }
  std::optional<std::string> text(const json::value& value, const std::string& key) { const auto item = rt::getString(value, key); return item ? std::optional(std::string(*item)) : std::nullopt; }
}
TEST("handler HTTP route and path normalization match 48 TypeScript vectors") {
  for (const auto& row : *rt::getArray(vectors(), "routes")) {
    const auto method = *text(row, "method"), path = *text(row, "path"); EXPECT_EQ(http::normalizePath(path), *text(row, "normalized")); EXPECT_EQ(http::terminalRouteForRequest(method, path), text(row, "terminal")); EXPECT_EQ(http::kernelQuotaRouteForRequest(method, path), text(row, "kernel"));
  }
}
ASYNC_TEST("handler HTTP idempotency identities match all authentication families") {
  for (const auto& row : *rt::getArray(vectors(), "principals")) {
    const auto& input = *rt::get(row, "input"); const auto& method = *rt::get(input, "method"); const auto kind = *rt::getString(method, "kind");
    http::PrincipalAuthMethod principal = auth::ClientAuthMethodDisabled{};
    if (kind == "kv_api_key") principal = http::PrincipalAuthMethodKvApiKey{*text(method, "key_id")};
    else if (kind == "github_token") principal = auth::ClientAuthMethodGithubToken{*text(method, "owner"), *text(method, "repo"), *text(method, "state_id"), *text(method, "limit_scope")};
    else if (kind == "passkey_session") principal = auth::ClientAuthMethodPasskeySession{*text(method, "user_id"), *text(method, "handle"), *rt::getBool(method, "is_admin"), *rt::getNumber(method, "credential_count")};
    else if (kind == "passport_session") principal = json::value_to<auth::PassportAuthMethod>(method);
    else if (kind == "auth_tokens_allowlist") principal = auth::ClientAuthMethodAuthTokensAllowlist{};
    else if (kind == "admin_allowlist") principal = auth::ClientAuthMethodAdminAllowlist{};
    else if (kind == "deno_deploy_token") principal = auth::ClientAuthMethodDenoDeployToken{};
    const http::PrincipalAuthResult authResult{text(input, "token"), principal}; AWAIT(actual, http::resolveIdempotencyPrincipal(authResult)); EXPECT_EQ(actual, *text(row, "expected"));
  }
}
ASYNC_TEST("handler HTTP provider correlation scrubbing preserves source response data") {
  for (const auto& row : *rt::getArray(vectors(), "correlations")) {
    rt::Headers headers{{"X-Other", "keep"}}; for (const auto name : http::PROVIDER_NATIVE_CORRELATION_HEADERS) headers.set(name, "provider-value");
    const auto original = rt::Response("payload", rt::ResponseInit{.status = 201, .statusText = "Created", .headers = headers}); const auto response = http::withProviderRequestId(original, text(row, "id")); EXPECT_EQ(response.status(), static_cast<int>(*rt::getNumber(row, "status"))); EXPECT_EQ(response.statusText(), *text(row, "statusText")); EXPECT(rt::deepEqual(headersJson(response.headers()), *rt::get(row, "headers"))); AWAIT(body, response.text()); EXPECT_EQ(body, *text(row, "body"));
  }
}
TEST("handler HTTP degradation signals preserve the status completed and non-null gates") {
  for (const auto& row : *rt::getArray(vectors(), "degradation")) { const auto& raw = *rt::get(row, "input"); const http::SentinelProviderDegradationInput input{*rt::getNumber(raw, "status"), *rt::getBool(raw, "completed"), text(raw, "removedProviderTriggerClass")}; EXPECT_EQ(http::shouldSignalSentinelProviderDegradation(input), *rt::getBool(row, "expected")); }
}
ASYNC_TEST("handler HTTP inference quota headers preserve explicit null zero and bounded usage") {
  for (const auto& row : *rt::getArray(vectors(), "quota")) {
    const auto* rawPolicy = rt::get(row, "policy"); std::optional<uos::authority::native_contracts::ApiKeyPolicy> policy;
    if (rawPolicy && !rawPolicy->is_null()) policy = json::value_to<uos::authority::native_contracts::ApiKeyPolicy>(*rawPolicy);
    const auto* rawTelemetry = rt::get(row, "telemetry"); std::optional<uos::openai_telemetry::ResponseTelemetry> telemetry;
    if (rawTelemetry && !rawTelemetry->is_null()) { telemetry.emplace(); const auto* quota = rt::get(*rawTelemetry, "quotaUsedPercent"); if (quota) telemetry->quotaUsedPercent = quota->is_null() ? rt::Nullable<double>(nullptr) : rt::Nullable<double>(*rt::getNumber(*quota)); }
    const auto original = rt::Response("payload", rt::ResponseInit{.headers = rt::Headers{{"x-other", "keep"}}}); const auto response = http::decorateInferenceQuota(original, policy ? &*policy : nullptr, telemetry ? &*telemetry : nullptr); EXPECT(rt::deepEqual(headersJson(response.headers()), *rt::get(row, "headers"))); AWAIT(body, response.text()); EXPECT_EQ(body, *text(row, "body"));
  }
}
ASYNC_TEST("handler HTTP background registration uses explicit and runtime registrars and fails closed") {
  auto cleanup = rt::scopeExit([] { http::globalEdgeRuntime().reset(); }); http::globalEdgeRuntime().reset(); const auto task = rt::Promise<void>::resolve(); EXPECT(!http::scheduleSentinelBackgroundTask(task, {})); int calls = 0; const http::SentinelBackgroundTaskRegistrar registrar = [&](rt::Promise<void> received) { EXPECT(received == task); ++calls; }; EXPECT(http::scheduleSentinelBackgroundTask(task, registrar)); EXPECT_EQ(calls, 1); http::globalEdgeRuntime() = http::SentinelBackgroundRuntime{registrar}; EXPECT(http::scheduleSentinelBackgroundTask(task, {})); EXPECT_EQ(calls, 2); const http::SentinelBackgroundTaskRegistrar broken = [](rt::Promise<void>) { throw rt::JsError("refused"); }; EXPECT(!http::scheduleSentinelBackgroundTask(task, broken)); const auto original = rt::Response("body", rt::ResponseInit{.status = 202}); const auto response = http::withRequestId(original, "request"); EXPECT_EQ(response.headers().get("x-uos-request-id"), std::optional<std::string>("request")); EXPECT_EQ(response.status(), 202); AWAIT(body, response.text()); EXPECT_EQ(body, "body");
}
