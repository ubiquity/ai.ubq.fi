// Port of all twelve tests/passport-sessions.test.ts cases, including real auth/index calls.
// Links: tests/passport_sessions_test.cpp
// src/kv.cpp src/request.cpp src/utils.cpp src/storage/kv.cpp
// src/storage/sqlite_readonly.cpp src/http.cpp src/auth/index.cpp src/auth/passport.cpp
// src/authority/native_api_key_policy.cpp src/authority/native_api_key.cpp src/auth/passport_sessions.cpp
// src/kernel/attestation.cpp src/auth/local_development_key.cpp src/auth/passkeys.cpp src/api_keys.cpp
// src/types.cpp src/paid_fallback/ledger_state.cpp src/kernel/policy_queue.cpp src/api_key_policy.cpp
// src/auth/local_admin.cpp src/kernel/quota_reservations.cpp src/authority/native_paid_contracts.cpp src/config.cpp
// src/authority/native_paid_policy.cpp src/kernel/quota_v2.cpp src/authority/native_kernel.cpp src/provider/surplus.cpp
// src/authority/native_paid.cpp src/analytics.cpp src/responses_stream.cpp src/storage/native_kv.cpp
// src/provider/anthropic_chat_responses.cpp src/provider/surplus_claude_chat_request.cpp src/provider/anthropic_response_wire.cpp src/provider/surplus_claude_request.cpp
// src/authority/native_kernel_contracts.cpp src/provider/anthropic_replay.cpp src/release.cpp src/paid_fallback/index.cpp
// src/paid_fallback/ledger_backfill.cpp src/provider/cerebras_rate_limits.cpp src/paid_fallback/ledger_admission.cpp src/auth/relay.cpp
// src/paid_fallback/ledger_settlement.cpp src/provider/metered.cpp src/paid_fallback/ledger_stats.cpp src/provider/anthropic_responses.cpp
// src/provider/anthropic_chat_stream.cpp src/models/codex_models_fetch.cpp src/codex/auth.cpp src/bounded_response_body.cpp
// src/codex/auth_refresh.cpp src/codex/dispatch.cpp src/models/codex_models.cpp src/models/codex_models_availability.cpp
// src/runtime_config.cpp src/authority/native_codex_contracts.cpp src/codex/native_auth.cpp src/provider/health.cpp
// src/defaults.cpp src/codex/external_auth.cpp src/provider/capacity_events.cpp src/provider/claude_code_accounts.cpp
// src/codex/routing_mutations.cpp src/authority/native_codex_capacity.cpp src/provider/capacity_contract.cpp src/codex/capacity_routing.cpp
// src/provider/claude_code_credentials.cpp src/authority/native_codex_mutations.cpp src/codex/rate_limit_429.cpp src/authority/native_codex_429.cpp
// src/authority/native_codex_evaluation.cpp src/codex/routing_state.cpp src/codex/banked_reset.cpp src/codex/native_auth_transport.cpp
// src/provider/capacity.cpp src/provider/claude_capacity_rollups.cpp src/cache/prompt_analytics_core.cpp src/provider/capacity_history.cpp
// src/provider/capacity_parse.cpp src/metered_quota.cpp src/cache/prompt_analytics_read.cpp src/provider/claude_capacity_history.cpp
// src/provider/capacity_rollups.cpp src/provider/claude_code_quota.cpp src/codex/banked_reset_provider.cpp src/runtime/websocket.cpp
// src/codex/reset_settings.cpp src/codex/experiment.cpp src/provider/claude_code_usage_schema.cpp src/provider/capacity_analytics.cpp
// src/codex/account_routing.cpp src/codex/routing_serial.cpp src/provider/selection.cpp src/authority/native_codex.cpp
// src/codex/routing_evaluation.cpp src/codex/overage_settings.cpp tests/helpers/admin_auth_harness.cpp
// src/kv_conversion/profile_json.cpp
#include "tests/helpers/admin_auth_harness.hpp"
#include "uos/auth/passport.hpp"
#include "uos/auth/index.hpp"
#include "uos/config.hpp"
#include "uos/rt/env.hpp"
#include "uos/utils.hpp"
#include "uos/auth/passport_sessions.hpp"
#include "uos/kv.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <algorithm>
#include "uos/rt/regex.hpp"
#include <optional>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace harness = uos::tests::helpers::admin_auth_harness;
  using namespace uos::auth::passport_sessions;
  using uos::auth::passport::handlePassportRoute;

  const std::string ORIGIN = "https://ai.ubq.fi";
  const std::string COOKIE = "__Host-uos_ai_passport_session";

  json::object binding() {
    json::object value;
    value["clientId"] = "ai";
    value["audience"] = "ai";
    value["origin"] = ORIGIN;
    value["redirectUri"] = ORIGIN + "/auth/callback";
    return value;
  }

  const std::string GRANT_TOKEN = "uos_app_session_" + std::string(43, 'a');
  const std::string GRANT_SUBJECT = "fixture-admin";
  const std::string GRANT_CREATED_AT = "2000-01-01T00:00:00.000Z";

  // `{ token, subject, ...binding, createdAt }`
  json::object grant() {
    json::object value;
    value["token"] = GRANT_TOKEN;
    value["subject"] = GRANT_SUBJECT;
    for (const auto& member : binding()) value[member.key()] = member.value();
    value["createdAt"] = GRANT_CREATED_AT;
    return value;
  }

  // The test file's mutable state, shared with the fetch mock.
  struct State {
    double status = 200;
    bool isAdmin = true;
    std::vector<std::string> scopes{"admin"};
    double reads = 0;
    std::optional<json::value> bodyOverride;
  };

  json::value sessionBody(const State& state) {
    json::object session;
    session["subject"] = GRANT_SUBJECT;
    for (const auto& member : binding()) session[member.key()] = member.value();
    session["createdAt"] = GRANT_CREATED_AT;
    json::array scopes;
    for (const std::string& scope : state.scopes) scopes.emplace_back(scope);
    json::object user;
    user["id"] = GRANT_SUBJECT;
    user["handle"] = "administrator";
    user["isAdmin"] = state.isAdmin;
    user["scopes"] = std::move(scopes);
    json::object body;
    body["authenticated"] = true;
    body["session"] = std::move(session);
    body["user"] = std::move(user);
    return body;
  }

  rt::Request request(const std::string& token, std::string method = "GET", const std::string& path = "/admin/api-keys", rt::Headers extra = {}) {
    rt::Headers headers(extra);
    headers.set("Cookie", COOKIE + "=" + token);
    rt::RequestInit init;
    init.method = std::move(method);
    init.headers = std::move(headers);
    return rt::Request(ORIGIN + path, std::move(init));
  }

  bool isStatus(const rt::Reason& error, double status) {
    try {
      if (error) std::rethrow_exception(error);
    } catch (const PassportSessionError& sessionError) {
      return sessionError.status == status;
    } catch (...) {
    }
    return false;
  }

  // assert.rejects(task, unauthorized / unavailable)
  template <class T> rt::Task<bool> rejectsWithStatus(rt::Task<T> task, double status) {
    rt::Reason failure;
    try {
      (void)co_await std::move(task);
    } catch (...) {
      failure = std::current_exception();
    }
    co_return isStatus(failure, status);
  }

  // beforeEach / afterEach: the harness, the default state and the session fetch mock.
  struct Fixture {
    std::shared_ptr<State> state = std::make_shared<State>();
    std::unique_ptr<rt::GlobalFetchOverride> fetch{};

    Fixture() {
      harness::resetHarness();
      std::shared_ptr<State> shared = state;
      fetch = std::make_unique<rt::GlobalFetchOverride>(rt::Fetch([shared](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
        EXPECT_EQ(init.method, std::optional<std::string>("POST"));
        EXPECT_EQ(init.redirect, std::optional<std::string>("error"));
        const rt::Response bodyReader(init.body ? *init.body : rt::BodyInit(nullptr));
        auto reading = bodyReader.text();
        const std::string text = co_await std::move(reading);
        EXPECT(rt::deepEqual(rt::jsonParse(text), json::value(binding())));
        EXPECT_EQ(rt::Headers(init.headers.value_or(rt::Headers())).get("Authorization"), std::optional<std::string>("Bearer " + GRANT_TOKEN));
        const std::string url = harness::urlOf(input);
        if (rt::endsWith(url, "/revoke")) {
          rt::ResponseInit revoked;
          revoked.status = shared->status == 200 ? 204 : shared->status;
          co_return rt::Response(rt::BodyInit(nullptr), std::move(revoked));
        }
        EXPECT_EQ(url, "https://passport.ubq.fi/api/auth/sso/session");
        shared->reads += 1;
        rt::ResponseInit init_;
        init_.status = shared->status;
        const json::value data = shared->bodyOverride ? *shared->bodyOverride : sessionBody(*shared);
        rt::Response response = rt::Response::json(data, std::move(init_));
        co_return response;
      }));
    }
    ~Fixture() {
      fetch.reset();
      harness::cleanupHarness();
    }
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;

    void replaceFetch(rt::Fetch replacement) {
      fetch.reset();
      fetch = std::make_unique<rt::GlobalFetchOverride>(std::move(replacement));
    }
  };

  rt::Task<std::string> created(std::optional<std::string> previous = std::nullopt) {
    auto creating = previous ? createPassportSession(json::value(grant()), GRANT_SUBJECT, *previous) : createPassportSession(json::value(grant()), GRANT_SUBJECT);
    std::string token = co_await std::move(creating);
    co_return token;
  }
}

ASYNC_TEST("persistent authority survives elapsed time beyond the original access lifespan") {
  Fixture fixture;
  auto creating = created();
  const std::string token = co_await std::move(creating);
  const auto now = rt::globalDateNow();
  rt::globalDateNow() = [now] { return now() + 366.0 * 24 * 60 * 60 * 1000; };
  auto reading = getPassportSession(token);
  const PassportSessionIdentity identity = co_await std::move(reading);
  rt::globalDateNow() = now;
  EXPECT_EQ(identity.subject, GRANT_SUBJECT);
  EXPECT_EQ(fixture.state->reads, 1);
}

ASYNC_TEST("unavailable introspection keeps the local session and recovers without another login") {
  Fixture fixture;
  auto creating = created();
  const std::string token = co_await std::move(creating);
  fixture.state->status = 503;
  auto unavailableRead = rejectsWithStatus(getPassportSession(token), 503);
  const bool unavailableReadMatched = co_await std::move(unavailableRead);
  EXPECT(unavailableReadMatched);
  EXPECT_EQ(harness::kvStore.size(), 1u);
  fixture.replaceFetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response { throw rt::JsError("synthetic transport outage"); }));
  auto outageRead = rejectsWithStatus(getPassportSession(token), 503);
  const bool outageReadMatched = co_await std::move(outageRead);
  EXPECT(outageReadMatched);
  EXPECT_EQ(harness::kvStore.size(), 1u);
  const json::value recovered = sessionBody(State());
  fixture.replaceFetch(rt::Fetch([recovered](rt::RequestInfo, rt::RequestInit) -> rt::Response { return rt::Response::json(recovered); }));
  auto reading = getPassportSession(token);
  const PassportSessionIdentity identity = co_await std::move(reading);
  EXPECT_EQ(identity.subject, GRANT_SUBJECT);
}

ASYNC_TEST("creation rejects every wrong binding or subject and cannot replace a valid session on failure") {
  Fixture fixture;
  auto creating = created();
  const std::string token = co_await std::move(creating);
  const std::vector<std::pair<std::string, std::string>> patches = {
    {"clientId", "other"},
    {"audience", "other"},
    {"origin", "https://evil.example"},
    {"redirectUri", ORIGIN + "/other"},
    {"subject", "other"},
    {"token", "legacy.jwt.token"},
  };
  for (const auto& [name, value] : patches) {
    json::object patched = grant();
    patched[name] = value;
    auto rejected = rejectsWithStatus(createPassportSession(json::value(patched), GRANT_SUBJECT, token), 503);
    const bool rejectedMatched = co_await std::move(rejected);
    EXPECT(rejectedMatched);
    EXPECT_EQ(harness::kvStore.size(), 1u);
  }
  auto reading = getPassportSession(token);
  const PassportSessionIdentity identity = co_await std::move(reading);
  EXPECT_EQ(identity.subject, GRANT_SUBJECT);
}

ASYNC_TEST("introspection rejects substituted identities and bindings without cached-role fallback") {
  Fixture fixture;
  auto creating = created();
  const std::string token = co_await std::move(creating);
  const auto patched = [&fixture](std::string_view part, std::string_view name, json::value value) {
    json::value body = sessionBody(*fixture.state);
    body.as_object()[part].as_object()[name] = std::move(value);
    return body;
  };
  const std::vector<json::value> bodies = {
    patched("session", "audience", json::value("other")),
    patched("session", "origin", json::value("https://evil.example")),
    patched("user", "id", json::value("other")),
    patched("user", "isAdmin", json::value("true")),
  };
  for (const json::value& body : bodies) {
    fixture.state->bodyOverride = body;
    auto rejected = rejectsWithStatus(getPassportSession(token), 503);
    const bool rejectedMatched = co_await std::move(rejected);
    EXPECT(rejectedMatched);
    EXPECT_EQ(harness::kvStore.size(), 1u);
  }
}

ASYNC_TEST("local logout revokes first and truthfully reports unavailable upstream revocation") {
  Fixture fixture;
  auto creating = created();
  const std::string token = co_await std::move(creating);
  fixture.replaceFetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response {
    EXPECT_EQ(harness::kvStore.size(), 0u);
    rt::ResponseInit init;
    init.status = 503;
    return rt::Response(rt::BodyInit(nullptr), std::move(init));
  }));
  auto loggingOut = handlePassportRoute(request(token, "POST", "/auth/logout", rt::Headers{{"Origin", ORIGIN}}), "/auth/logout");
  const std::optional<rt::Response> logout = co_await std::move(loggingOut);
  ASSERT(logout.has_value());
  EXPECT_EQ(logout->status(), 204);
  EXPECT_EQ(logout->headers().get("X-UOS-Passport-Upstream-Revoke"), std::optional<std::string>("unavailable"));
  auto rejected = rejectsWithStatus(getPassportSession(token), 401);
  const bool rejectedMatched = co_await std::move(rejected);
    EXPECT(rejectedMatched);
}

ASYNC_TEST("cross-origin logout never revokes a session, and local store failure cannot claim logout") {
  Fixture fixture;
  auto creating = created();
  const std::string token = co_await std::move(creating);
  auto foreignLogout = handlePassportRoute(request(token, "POST", "/auth/logout", rt::Headers{{"Origin", "https://evil.example"}}), "/auth/logout");
  const std::optional<rt::Response> foreign = co_await std::move(foreignLogout);
  EXPECT(foreign.has_value() && foreign->status() == 403);
  EXPECT_EQ(harness::kvStore.size(), 1u);
  uos::kv::setKvForTest(nullptr);
  auto unavailableLogout = handlePassportRoute(request(token, "POST", "/auth/logout", rt::Headers{{"Origin", ORIGIN}}), "/auth/logout");
  const std::optional<rt::Response> unavailable = co_await std::move(unavailableLogout);
  ASSERT(unavailable.has_value());
  EXPECT_EQ(unavailable->status(), 503);
  EXPECT_EQ(unavailable->headers().getSetCookie().size(), 0u);
}

ASYNC_TEST("logout wins over an in-flight successful introspection without recreating authority") {
  Fixture fixture;
  auto creating = created();
  const std::string token = co_await std::move(creating);
  auto released = std::make_shared<rt::Resolvers<rt::Response>>(rt::withResolvers<rt::Response>());
  auto reached = std::make_shared<rt::Resolvers<void>>(rt::withResolvers<void>());
  fixture.replaceFetch(rt::Fetch([released, reached](rt::RequestInfo input, rt::RequestInit) -> rt::Task<rt::Response> {
    if (rt::endsWith(harness::urlOf(input), "/revoke")) {
      rt::ResponseInit init;
      init.status = 204;
      co_return rt::Response(rt::BodyInit(nullptr), std::move(init));
    }
    reached->resolve();
    rt::Response response = co_await released->promise.wait();
    co_return response;
  }));
  rt::Promise<PassportSessionIdentity> pending(getPassportSession(token));
  co_await reached->promise.wait();
  auto revoking = revokePassportSession(token);
  const std::string revoked = co_await std::move(revoking);
  EXPECT_EQ(revoked, "revoked");
  const json::value body = sessionBody(*fixture.state);
  released->resolve(rt::Response::json(body));
  auto rejected = rejectsWithStatus(pending.wait(), 401);
  const bool rejectedMatched = co_await std::move(rejected);
    EXPECT(rejectedMatched);
  EXPECT_EQ(harness::kvStore.size(), 0u);
}

ASYNC_TEST("a successfully stored replacement atomically removes the previous local session") {
  Fixture fixture;
  auto creatingPrevious = created();
  const std::string previous = co_await std::move(creatingPrevious);
  auto creatingReplacement = created(previous);
  const std::string replacement = co_await std::move(creatingReplacement);
  EXPECT(previous != replacement);
  EXPECT_EQ(harness::kvStore.size(), 1u);
  auto rejected = rejectsWithStatus(getPassportSession(previous), 401);
  const bool rejectedMatched = co_await std::move(rejected);
    EXPECT(rejectedMatched);
  auto reading = getPassportSession(replacement);
  const PassportSessionIdentity identity = co_await std::move(reading);
  EXPECT_EQ(identity.subject, GRANT_SUBJECT);
}

ASYNC_TEST("an authoritative revocation still removes authority when the rejected body cannot be cancelled") {
  Fixture fixture;
  auto creating = created();
  const std::string token = co_await std::move(creating);
  fixture.replaceFetch(rt::Fetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response {
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.cancel = [](rt::Reason) -> rt::Task<void> {
      throw rt::JsError("synthetic cancel failure");
      co_return;
    };
    rt::ResponseInit init;
    init.status = 401;
    return rt::Response(rt::BodyInit(rt::ByteStream(source)), std::move(init));
  }));
  auto rejected = rejectsWithStatus(getPassportSession(token), 401);
  const bool rejectedMatched = co_await std::move(rejected);
    EXPECT(rejectedMatched);
  EXPECT_EQ(harness::kvStore.size(), 0u);
}

ASYNC_TEST("permanent Passport sessions store a hashed local secret without an expiry or role snapshot") {
  rt::env::ScopedEnvironment environment({}); uos::config::config.codexAuthJsonB64.clear(); Fixture fixture;
  auto creating = created(); const auto token = co_await std::move(creating);
  static const rt::Regex pattern("^uos_ai_passport_session_[A-Za-z0-9_-]{43}$", ""); EXPECT(pattern.test(token));
  auto hashing = uos::utils::sha256Hex(token); const auto hash = co_await std::move(hashing);
  const rt::KvKey key{"uos_ai", "auth", "passport", "sessions", hash}; const auto encodedKey = harness::keyToString(key);
  const auto* record = harness::kvStore.get(encodedKey); ASSERT(record);
  auto expected = binding(); expected["version"] = 1; expected["subject"] = GRANT_SUBJECT; expected["createdAt"] = GRANT_CREATED_AT; expected["appSessionToken"] = GRANT_TOKEN;
  EXPECT(rt::deepEqual(record->toJson(), expected)); EXPECT(!rt::includes(encodedKey, token));
  auto authenticating = uos::auth::index::authenticateAdmin(request(token)); const auto auth = co_await std::move(authenticating);
  const auto* accepted = std::get_if<uos::auth::index::AdminAuthResultOk>(&auth); ASSERT(accepted);
  const auto* method = std::get_if<uos::auth::index::PassportAuthMethod>(&accepted->method); ASSERT(method); EXPECT(!method->user.exp); EXPECT(!method->user.jti); EXPECT_EQ(fixture.state->reads, 1);
}
ASYNC_TEST("every request reads current role and scopes, and a demotion immediately denies admin access") {
  rt::env::ScopedEnvironment environment({}); uos::config::config.codexAuthJsonB64.clear(); Fixture fixture;
  auto creating = created(); const auto token = co_await std::move(creating);
  auto authenticating = uos::auth::index::authenticateAdmin(request(token)); const auto accepted = co_await std::move(authenticating); EXPECT(std::holds_alternative<uos::auth::index::AdminAuthResultOk>(accepted));
  fixture.state->isAdmin = false; fixture.state->scopes.clear(); auto reading = getPassportSession(token); const auto identity = co_await std::move(reading); EXPECT_EQ(identity.subject, GRANT_SUBJECT); EXPECT(!identity.isAdmin); EXPECT(identity.scopes.empty());
  auto demoting = uos::auth::index::authenticateAdmin(request(token)); const auto demoted = co_await std::move(demoting); const auto* refusal = std::get_if<uos::auth::index::AdminAuthResultNotOk>(&demoted); ASSERT(refusal); EXPECT_EQ(refusal->response.status(), 403); EXPECT_EQ(fixture.state->reads, 3); EXPECT_EQ(harness::kvStore.size(), std::size_t(1));
}
ASYNC_TEST("authoritative revocation deletes local authority and clears the browser cookie") {
  rt::env::ScopedEnvironment environment({}); uos::config::config.codexAuthJsonB64.clear(); Fixture fixture;
  auto creating = created(); const auto token = co_await std::move(creating); fixture.state->status = 401;
  auto authenticating = uos::auth::index::authenticateAdmin(request(token)); const auto revoked = co_await std::move(authenticating); const auto* refusal = std::get_if<uos::auth::index::AdminAuthResultNotOk>(&revoked); ASSERT(refusal); EXPECT_EQ(refusal->response.status(), 401);
  const auto cookies = refusal->response.headers().getSetCookie(); EXPECT(std::any_of(cookies.begin(), cookies.end(), [](const auto& cookie) { return rt::startsWith(cookie, COOKIE + "=; Max-Age=0;"); })); EXPECT_EQ(harness::kvStore.size(), std::size_t(0)); fixture.state->status = 200;
  auto rejected = rejectsWithStatus(getPassportSession(token), 401); const auto matched = co_await std::move(rejected); EXPECT(matched); EXPECT_EQ(fixture.state->reads, 1);
}
