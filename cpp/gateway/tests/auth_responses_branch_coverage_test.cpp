// Port of tests/auth-responses-branch-coverage.test.ts: passkey and stream-translation cases.
// Deferred: eight api-key-policy cases listed in the TS file; their owner supplies the remaining cases.
// Links: tests/auth_responses_branch_coverage_test.cpp tests/helpers/passkeys_harness.cpp src/auth/passkeys.cpp src/auth/relay.cpp
//   src/http.cpp src/request.cpp src/utils.cpp src/kv.cpp src/storage/kv.cpp src/release.cpp src/storage/sqlite_readonly.cpp
//   src/storage/native_kv.cpp src/config.cpp src/provider/cerebras_rate_limits.cpp
//   src/chat/stream_translation.cpp src/openai_telemetry.cpp src/codex/responses_error.cpp src/inference_deadline.cpp
//   src/provider/removed_provider_telemetry.cpp src/responses_failover_stream.cpp src/responses_stream.cpp src/types.cpp
#include "tests/helpers/passkeys_harness.hpp"
#include "uos/auth/passkeys.hpp"
#include "uos/chat/stream_translation.hpp"
#include "uos/kv.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <functional>
#include <limits>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::auth::passkeys;
namespace harness = uos::tests::helpers::passkeys_harness;

// GCC 12.2 coroutine rule (PORTING.md): every awaited call is bound to a named Task first and its
// result to a named local before an assertion reads it.
#define AWAIT(name, expression)        \
  auto name##Task = (expression);      \
  auto name = co_await std::move(name##Task)

namespace {
  struct HarnessScope {
    HarnessScope() { harness::resetHarness(); }
    ~HarnessScope() { harness::cleanupHarness(); }
    HarnessScope(const HarnessScope&) = delete;
    HarnessScope& operator=(const HarnessScope&) = delete;
  };

  /* --------------------------------------------------------------- passkeys */

  using HeaderList = std::vector<std::pair<std::string, std::string>>;

  rt::Request jsonRequest(const std::string& url, const json::value& body, const HeaderList& headers = {}) {
    rt::Headers all{{"Content-Type", "application/json"}};
    for (const auto& [name, value] : headers) all.set(name, value);
    rt::RequestInit init;
    init.method = "POST";
    init.body = rt::BodyInit(rt::jsonStringify(body));
    init.headers = all;
    return rt::Request(url, init);
  }

  rt::Request plainRequest(const std::string& url, const HeaderList& headers = {}) {
    rt::Headers all;
    for (const auto& [name, value] : headers) all.set(name, value);
    rt::RequestInit init;
    init.headers = all;
    return rt::Request(url, init);
  }

  rt::Request postText(const std::string& url, std::string body) {
    rt::RequestInit init;
    init.method = "POST";
    init.body = rt::BodyInit(std::move(body));
    return rt::Request(url, init);
  }

  rt::Request postEmpty(const std::string& url) {
    rt::RequestInit init;
    init.method = "POST";
    return rt::Request(url, init);
  }

  std::string clientDataJson(const std::string& challenge) {
    json::object data;
    data["type"] = "webauthn.create";
    data["challenge"] = challenge;
    data["origin"] = "https://ai.ubq.fi";
    return rt::btoa(rt::jsonStringify(data));
  }

  void seedChallenge(const std::string& challenge, const std::function<void(rt::KvObject&)>& overrides = {}) {
    rt::KvObject record;
    record.set("challenge", rt::KvValue(challenge));
    record.set("type", rt::KvValue("registration"));
    record.set("user_id", rt::KvValue("user-test"));
    record.set("handle", rt::KvValue("uos-passkey-test"));
    record.set("rp_id", rt::KvValue("ai.ubq.fi"));
    record.set("origin", rt::KvValue("https://ai.ubq.fi"));
    record.set("is_admin", rt::KvValue(false));
    record.set("created_at_ms", rt::KvValue(rt::dateNow()));
    record.set("expires_at_ms", rt::KvValue(rt::dateNow() + 60'000));
    if (overrides) overrides(record);
    harness::kvStore.set(harness::keyToString(passkeyChallengeKey(challenge)), rt::KvValue(std::move(record)));
  }

  // `{ response: { id?, response: { clientDataJSON, attestationObject? } } }`
  json::value webauthnBody(std::optional<std::string> id, std::string clientData, std::optional<std::string> attestationObject = std::nullopt) {
    json::object inner;
    inner["clientDataJSON"] = clientData;
    if (attestationObject) inner["attestationObject"] = *attestationObject;
    json::object response;
    if (id) response["id"] = *id;
    response["response"] = std::move(inner);
    json::object body;
    body["response"] = std::move(response);
    return body;
  }

  json::value object(std::initializer_list<std::pair<std::string_view, json::value>> members) {
    json::object out;
    for (const auto& [key, value] : members) out[key] = value;
    return out;
  }

  rt::Task<double> statusOf(rt::Task<rt::Response> task) {
    const rt::Response response = co_await std::move(task);
    co_return response.status();
  }

  rt::Task<json::value> bodyOf(rt::Response response) {
    rt::Task<json::value> reading = response.json();
    json::value body = co_await std::move(reading);
    co_return body;
  }

  rt::Task<bool> sessionPresent(rt::Task<std::optional<PasskeySession>> task) {
    const std::optional<PasskeySession> session = co_await std::move(task);
    co_return session.has_value();
  }

  rt::Task<rt::Response> registerStart(json::value body, PasskeyRegisterStartOptions options) {
    rt::Request request = jsonRequest("https://ai.ubq.fi/auth/passkey/register/start", body);
    rt::Task<rt::Response> handling = handlePasskeyRegisterStart(std::move(request), std::move(options));
    rt::Response response = co_await std::move(handling);
    co_return response;
  }

  rt::Task<double> registerFinishStatus(json::value body) {
    rt::Task<double> status = statusOf(handlePasskeyRegisterFinish(jsonRequest("https://ai.ubq.fi/auth/passkey/register/finish", body)));
    const double value = co_await std::move(status);
    co_return value;
  }

  rt::Task<double> loginFinishStatus(json::value body) {
    rt::Task<double> status = statusOf(handlePasskeyLoginFinish(jsonRequest("https://ai.ubq.fi/auth/passkey/login/finish", body)));
    const double value = co_await std::move(status);
    co_return value;
  }

  rt::Task<rt::Response> usersUpdate(json::value body) {
    rt::Task<rt::Response> handling = handlePasskeyUsersUpdate(jsonRequest("https://ai.ubq.fi/admin/passkeys", body));
    rt::Response response = co_await std::move(handling);
    co_return response;
  }

  // The `withoutKv(async () => { ... })` body of "passkey handlers fail closed without a KV binding".
  rt::Task<void> failClosedWithoutKv() {
    AWAIT(users, hasPasskeyUsers());
    EXPECT(!users);
    const json::value empty = json::object();
    AWAIT(registerStartStatus, statusOf(handlePasskeyRegisterStart(jsonRequest("https://ai.ubq.fi/auth/passkey/register/start", empty), PasskeyRegisterStartOptions{})));
    EXPECT_EQ(registerStartStatus, 503.0);
    AWAIT(registerFinish, statusOf(handlePasskeyRegisterFinish(jsonRequest("https://ai.ubq.fi/auth/passkey/register/finish", empty))));
    EXPECT_EQ(registerFinish, 503.0);
    AWAIT(loginStart, statusOf(handlePasskeyLoginStart(jsonRequest("https://ai.ubq.fi/auth/passkey/login/start", empty))));
    EXPECT_EQ(loginStart, 503.0);
    AWAIT(loginFinish, statusOf(handlePasskeyLoginFinish(jsonRequest("https://ai.ubq.fi/auth/passkey/login/finish", empty))));
    EXPECT_EQ(loginFinish, 503.0);
    AWAIT(usersList, statusOf(handlePasskeyUsersList()));
    EXPECT_EQ(usersList, 503.0);
    AWAIT(usersUpdateStatus, statusOf(handlePasskeyUsersUpdate(jsonRequest("https://ai.ubq.fi/admin/passkeys", empty))));
    EXPECT_EQ(usersUpdateStatus, 503.0);
    AWAIT(anySession, sessionPresent(getPasskeySession(std::string("uos_ai_session_any"))));
    EXPECT(!anySession);
  }
}

ASYNC_TEST("passkey handles are normalized and an empty seed produces no handle") {
  HarnessScope scope;
  AWAIT(blank, buildPasskeyHandle(std::string("   ")));
  EXPECT_EQ(blank, std::string(""));
  AWAIT(built, buildPasskeyHandle(std::string("  token-value  ")));
  EXPECT(rt::Regex("^uos-passkey-[0-9a-f]{16}$", "").test(built));
  EXPECT_EQ(normalizePasskeyHandle(json::value("---")), std::string(""));
  EXPECT_EQ(normalizePasskeyHandle(json::value("---Mixed   Name---")), std::string("mixed-name"));
  EXPECT_EQ(normalizePasskeyHandle(json::value(nullptr)), std::string(""));
  EXPECT_EQ(rt::length(normalizePasskeyHandle(json::value(rt::repeat("a", 200)))), std::int64_t{96});
}

ASYNC_TEST("passkey request metadata prefers a trusted origin and tolerates malformed headers") {
  HarnessScope scope;
  const PasskeyRequestMeta canonical = getPasskeyRequestMeta(plainRequest("https://ai.ubq.fi/auth/passkey/register/start"));
  EXPECT_EQ(canonical.origin, std::string("https://ai.ubq.fi"));
  EXPECT_EQ(canonical.rpId, std::string("ai.ubq.fi"));

  const PasskeyRequestMeta forwarded =
    getPasskeyRequestMeta(plainRequest("https://internal.example/auth/passkey/register/start", {{"x-forwarded-proto", "https"}, {"x-forwarded-host", "ai.ubq.fi"}}));
  EXPECT_EQ(forwarded.origin, std::string("https://ai.ubq.fi"));

  const PasskeyRequestMeta malformedHost = getPasskeyRequestMeta(plainRequest("https://ai.ubq.fi/x", {{"x-forwarded-host", "not a host /"}}));
  EXPECT_EQ(malformedHost.origin, std::string("https://ai.ubq.fi"));

  const PasskeyRequestMeta loopback = getPasskeyRequestMeta(plainRequest("http://localhost:8080/x", {{"x-forwarded-proto", "http"}}));
  EXPECT_EQ(loopback.origin, std::string("http://localhost:8080"));

  const json::value evil("https://evil.example.com");
  const PasskeyRequestMeta untrusted = getPasskeyRequestMeta(plainRequest("https://ai.ubq.fi/x"), &evil);
  EXPECT_EQ(untrusted.origin, std::string("https://ai.ubq.fi"));
  const json::value garbage("not a url");
  const PasskeyRequestMeta garbageOrigin = getPasskeyRequestMeta(plainRequest("https://ai.ubq.fi/x"), &garbage);
  EXPECT_EQ(garbageOrigin.origin, std::string("https://ai.ubq.fi"));
  co_return;
}

ASYNC_TEST("passkey sessions reject unknown, expired and orphaned records") {
  HarnessScope scope;
  harness::kvStore.clear();
  AWAIT(missing, sessionPresent(getPasskeySession(std::string("uos_ai_session_missing"))));
  EXPECT(!missing);
  AWAIT(noUsers, hasPasskeyUsers());
  EXPECT(!noUsers);

  harness::seedPasskeySession("uos_ai_session_live_here");
  const std::string liveKey = harness::keyToString(passkeySessionKey("uos_ai_session_live_here"));
  rt::KvObject expiredRecord = harness::kvStore.get(liveKey)->asObject();
  expiredRecord.set("expires_at_ms", rt::KvValue(rt::dateNow() - 1));
  harness::kvStore.set(liveKey, rt::KvValue(std::move(expiredRecord)));
  AWAIT(expired, sessionPresent(getPasskeySession(std::string("uos_ai_session_live_here"))));
  EXPECT(!expired);  // an expired session is refused and removed
  EXPECT(!harness::kvStore.has(liveKey));

  const std::string orphanToken = "uos_ai_session_orphan";
  const PasskeySessionRecord orphan{
    .token = orphanToken, .user_id = "user-gone", .created_at_ms = rt::dateNow(), .expires_at_ms = rt::dateNow() + 60'000, .audience_origin = std::nullopt};
  harness::kvStore.set(harness::keyToString(passkeySessionKey(orphanToken)), rt::KvValue(json::value_from(orphan)));
  AWAIT(orphaned, sessionPresent(getPasskeySession(orphanToken)));
  EXPECT(!orphaned);  // a session whose user disappeared is refused
  AWAIT(someUsers, hasPasskeyUsers());
  EXPECT(someUsers);
  const harness::SeededPasskeySession usable = harness::seedPasskeySession("uos_ai_session_usable");
  AWAIT(live, sessionPresent(getPasskeySession(usable.token)));
  EXPECT(live);
}

ASYNC_TEST("a passkey session is read from a bearer token or the relay cookie, and a corrupt cookie is refused") {
  HarnessScope scope;
  harness::kvStore.clear();
  const harness::SeededPasskeySession seeded = harness::seedPasskeySession("uos_ai_session_cookie");
  const std::string token = seeded.token;
  const std::string cookieName(PASSKEY_RELAY_COOKIE_NAME);

  AWAIT(bearer, getPasskeySessionFromRequest(plainRequest("https://ai.ubq.fi/x", {{"authorization", "Bearer " + token}})));
  EXPECT(bearer && bearer->token == token);

  AWAIT(cookie, getPasskeySessionFromRequest(plainRequest("https://ai.ubq.fi/x", {{"cookie", "other=1; " + cookieName + "=" + token}})));
  EXPECT(cookie && cookie->token == token);

  AWAIT(emptyCookie, getPasskeySessionFromRequest(plainRequest("https://ai.ubq.fi/x", {{"cookie", cookieName + "="}})));
  EXPECT(!emptyCookie.has_value());  // an empty cookie value carries no token
  AWAIT(corruptCookie, getPasskeySessionFromRequest(plainRequest("https://ai.ubq.fi/x", {{"cookie", cookieName + "=%E0%A4%A"}})));
  EXPECT(!corruptCookie.has_value());  // a cookie that cannot be decoded is refused instead of throwing
  AWAIT(none, getPasskeySessionFromRequest(plainRequest("https://ai.ubq.fi/x")));
  EXPECT(!none.has_value());
}

ASYNC_TEST("passkey handlers fail closed without a KV binding") {
  HarnessScope scope;
  uos::kv::setKvForTest(nullptr);
  rt::Reason failure;
  try {
    rt::Task<void> checking = failClosedWithoutKv();
    co_await std::move(checking);
  } catch (...) {
    failure = std::current_exception();
  }
  uos::kv::setKvForTest(harness::kvStub());
  if (failure) std::rethrow_exception(failure);
}

ASYNC_TEST("passkey register start validates the body, refuses a taken username and issues a challenge") {
  HarnessScope scope;
  harness::kvStore.clear();
  const std::string url = "https://ai.ubq.fi/auth/passkey/register/start";
  AWAIT(notJson, statusOf(handlePasskeyRegisterStart(postText(url, "not json"))));
  EXPECT_EQ(notJson, 400.0);
  AWAIT(notObject, statusOf(registerStart(json::value("a string, not an object"), PasskeyRegisterStartOptions{})));
  EXPECT_EQ(notObject, 400.0);  // a non-object body is refused

  const harness::SeededPasskeySession owner = harness::seedPasskeySession("uos_ai_session_owner");
  AWAIT(taken, statusOf(registerStart(object({{"handle", json::value(owner.user.handle)}}), PasskeyRegisterStartOptions{})));
  EXPECT_EQ(taken, 409.0);  // an existing username owned by another account cannot be registered again

  PasskeyRegisterStartOptions admin;
  admin.defaultIsAdmin = true;
  AWAIT(created, registerStart(object({{"handle", json::value("Brand New Name")}}), admin));
  EXPECT_EQ(created.status(), 200.0);
  AWAIT(createdBody, bodyOf(created));
  EXPECT_EQ(rt::getString(createdBody, "handle"), std::optional<std::string_view>("brand-new-name"));
  EXPECT(rt::truthy(rt::get(rt::get(createdBody, "publicKey"), "challenge")));  // a registration challenge is issued

  const harness::SeededPasskeySession existing = harness::seedPasskeySession("uos_ai_session_existing");
  PasskeyRegisterStartOptions authenticated;
  authenticated.authenticatedPasskeyToken = existing.token;
  AWAIT(rekeyed, registerStart(object({{"handle", json::value(existing.user.handle)}}), authenticated));
  EXPECT_EQ(rekeyed.status(), 200.0);
  AWAIT(rekeyedBody, bodyOf(rekeyed));
  EXPECT_EQ(rt::getString(rekeyedBody, "handle"), std::optional<std::string_view>(existing.user.handle));

  PasskeyRegisterStartOptions absent;
  absent.authenticatedPasskeyToken = "uos_ai_session_absent";
  AWAIT(unauthorized, statusOf(registerStart(object({{"handle", json::value("whatever")}}), absent)));
  EXPECT_EQ(unauthorized, 401.0);  // an unknown relay token is refused
}

ASYNC_TEST("passkey register finish rejects every unusable challenge before attestation") {
  HarnessScope scope;
  AWAIT(notJson, statusOf(handlePasskeyRegisterFinish(postText("https://ai.ubq.fi/x", "not json"))));
  EXPECT_EQ(notJson, 400.0);
  AWAIT(emptyBody, registerFinishStatus(json::object()));
  EXPECT_EQ(emptyBody, 400.0);  // a body without a webauthn response is refused
  AWAIT(noClientData, registerFinishStatus(object({{"response", object({{"id", json::value("credential")}})}})));
  EXPECT_EQ(noClientData, 400.0);  // a response without client data is refused
  AWAIT(notBase64, registerFinishStatus(webauthnBody(std::string("credential"), "not base64!!")));
  EXPECT_EQ(notBase64, 400.0);

  AWAIT(unknownChallenge, registerFinishStatus(webauthnBody(std::string("credential"), clientDataJson("unknown-challenge"))));
  EXPECT_EQ(unknownChallenge, 400.0);  // an unknown challenge is refused

  seedChallenge("login-shaped", [](rt::KvObject& record) { record.set("type", rt::KvValue("authentication")); });
  AWAIT(wrongType, registerFinishStatus(webauthnBody(std::string("credential"), clientDataJson("login-shaped"))));
  EXPECT_EQ(wrongType, 400.0);  // an authentication challenge is not a registration challenge

  seedChallenge("attestation-fails");
  AWAIT(failedAttestation, registerFinishStatus(webauthnBody(std::string("credential"), clientDataJson("attestation-fails"), std::string("AA"))));
  EXPECT_EQ(failedAttestation, 400.0);  // an invalid attestation is refused
}

ASYNC_TEST("passkey login start validates the relay origin and tolerates an empty body") {
  HarnessScope scope;
  harness::kvStore.clear();
  const std::string url = "https://ai.ubq.fi/auth/passkey/login/start";
  AWAIT(emptyBody, statusOf(handlePasskeyLoginStart(postEmpty(url))));
  EXPECT_EQ(emptyBody, 200.0);  // an empty login-start body is allowed so the browser can discover the accountless flow

  AWAIT(badOrigin, statusOf(handlePasskeyLoginStart(jsonRequest(url, object({{"relay_origin", json::value("not a url")}})))));
  EXPECT_EQ(badOrigin, 400.0);  // an unusable relay origin is refused
  AWAIT(untrustedOrigin, statusOf(handlePasskeyLoginStart(jsonRequest(url, object({{"relay_origin", json::value("https://evil.example.com")}})))));
  EXPECT_EQ(untrustedOrigin, 400.0);  // an untrusted relay origin is refused

  AWAIT(relayed, statusOf(handlePasskeyLoginStart(jsonRequest(url, object({{"relay_origin", json::value("https://ai.ubq.fi")}})))));
  EXPECT_EQ(relayed, 200.0);  // the canonical relay origin is trusted
}

ASYNC_TEST("passkey login finish rejects an unusable response, challenge and credential") {
  HarnessScope scope;
  AWAIT(notJson, statusOf(handlePasskeyLoginFinish(postText("https://ai.ubq.fi/x", "not json"))));
  EXPECT_EQ(notJson, 400.0);
  AWAIT(emptyBody, loginFinishStatus(json::object()));
  EXPECT_EQ(emptyBody, 400.0);
  AWAIT(noId, loginFinishStatus(webauthnBody(std::nullopt, clientDataJson("c"))));
  EXPECT_EQ(noId, 400.0);  // a response without a credential id is refused
  AWAIT(badClientData, loginFinishStatus(webauthnBody(std::string("credential"), "!!!")));
  EXPECT_EQ(badClientData, 400.0);
  AWAIT(absent, loginFinishStatus(webauthnBody(std::string("credential"), clientDataJson("absent"))));
  EXPECT_EQ(absent, 400.0);

  seedChallenge("registration-shaped", [](rt::KvObject& record) { record.set("type", rt::KvValue("registration")); });
  AWAIT(wrongType, loginFinishStatus(webauthnBody(std::string("credential"), clientDataJson("registration-shaped"))));
  EXPECT_EQ(wrongType, 400.0);  // a registration challenge is not an authentication challenge

  // `{ type: "authentication", user_id: undefined }`
  seedChallenge("unknown-credential", [](rt::KvObject& record) {
    record.set("type", rt::KvValue("authentication"));
    record.set("user_id", rt::KvValue());
  });
  AWAIT(unknownCredential, loginFinishStatus(webauthnBody(std::string("absent-credential"), clientDataJson("unknown-credential"))));
  EXPECT_EQ(unknownCredential, 400.0);  // an unknown credential is refused
}

ASYNC_TEST("an expired passkey challenge is consumed once and refused") {
  HarnessScope scope;
  harness::kvStore.clear();
  seedChallenge("expired-challenge", [](rt::KvObject& record) { record.set("expires_at_ms", rt::KvValue(rt::dateNow() - 1)); });
  AWAIT(response, registerFinishStatus(webauthnBody(std::string("credential"), clientDataJson("expired-challenge"))));
  EXPECT_EQ(response, 400.0);
  EXPECT(!harness::kvStore.has(harness::keyToString(passkeyChallengeKey("expired-challenge"))));  // an expired challenge is deleted when it is read
}

ASYNC_TEST("passkey user administration lists stored users and validates every update field") {
  HarnessScope scope;
  harness::kvStore.clear();
  const double now = rt::dateNow();
  const PasskeyUserRecord oldUser{.id = "user-old", .handle = "first", .is_admin = false, .credential_ids = {}, .created_at_ms = now - 500, .updated_at_ms = now - 500};
  harness::kvStore.set(harness::keyToString(passkeyUserKey("user-old")), rt::KvValue(json::value_from(oldUser)));
  const PasskeyUserRecord newUser{.id = "user-new", .handle = "second", .is_admin = true, .credential_ids = {"c1"}, .created_at_ms = now, .updated_at_ms = now};
  harness::kvStore.set(harness::keyToString(passkeyUserKey("user-new")), rt::KvValue(json::value_from(newUser)));
  AWAIT(listed, handlePasskeyUsersList());
  EXPECT_EQ(listed.status(), 200.0);
  AWAIT(listBody, bodyOf(listed));
  EXPECT_EQ(rt::getString(listBody, "object"), std::optional<std::string_view>("list"));
  std::vector<std::string> ids;
  if (const json::array* data = rt::getArray(listBody, "data"))
    for (const json::value& user : *data) ids.emplace_back(rt::getString(user, "id").value_or(""));
  EXPECT_EQ(ids, (std::vector<std::string>{"user-new", "user-old"}));  // users are listed newest first

  AWAIT(notJson, statusOf(handlePasskeyUsersUpdate(postText("https://ai.ubq.fi/x", "not json"))));
  EXPECT_EQ(notJson, 400.0);
  AWAIT(array, statusOf(usersUpdate(json::array())));
  EXPECT_EQ(array, 400.0);
  AWAIT(noId, statusOf(usersUpdate(object({{"is_admin", json::value(true)}}))));
  EXPECT_EQ(noId, 400.0);  // an update without an id is refused
  AWAIT(notBoolean, statusOf(usersUpdate(object({{"id", json::value("user-new")}, {"is_admin", json::value("yes")}}))));
  EXPECT_EQ(notBoolean, 400.0);  // a non-boolean admin flag is refused
  AWAIT(absentUser, statusOf(usersUpdate(object({{"id", json::value("user-absent")}, {"is_admin", json::value(true)}}))));
  EXPECT_EQ(absentUser, 404.0);

  AWAIT(updated, usersUpdate(object({{"id", json::value("user-new")}, {"is_admin", json::value(false)}})));
  EXPECT_EQ(updated.status(), 200.0);
  AWAIT(updatedBody, bodyOf(updated));
  EXPECT_EQ(rt::getBool(rt::get(rt::get(updatedBody, "user"), "is_admin")), std::optional<bool>(false));

  harness::setBeforeAtomicCommit([] {
    const std::string key = harness::keyToString(passkeyUserKey("user-new"));
    rt::KvObject stored = harness::kvStore.get(key)->asObject();
    stored.set("updated_at_ms", rt::KvValue(rt::dateNow() + 5));
    harness::kvStore.set(key, rt::KvValue(std::move(stored)));
  });
  AWAIT(conflicted, statusOf(usersUpdate(object({{"id", json::value("user-new")}, {"is_admin", json::value(true)}}))));
  EXPECT_EQ(conflicted, 409.0);  // a concurrently modified user is reported instead of overwritten
}

ASYNC_TEST("passkey session and logout handlers require a live session") {
  HarnessScope scope;
  harness::kvStore.clear();
  AWAIT(anonymousSession, statusOf(handlePasskeySession(plainRequest("https://ai.ubq.fi/auth/session"))));
  EXPECT_EQ(anonymousSession, 401.0);
  AWAIT(anonymousLogout, statusOf(handlePasskeyLogout(plainRequest("https://ai.ubq.fi/auth/logout"))));
  EXPECT_EQ(anonymousLogout, 204.0);  // logout is idempotent for an anonymous caller

  const harness::SeededPasskeySession seeded = harness::seedPasskeySession("uos_ai_session_live");
  AWAIT(session, handlePasskeySession(plainRequest("https://ai.ubq.fi/auth/session", {{"authorization", "Bearer " + seeded.token}})));
  EXPECT_EQ(session.status(), 200.0);
  AWAIT(sessionBody, bodyOf(session));
  EXPECT_EQ(rt::getString(rt::get(rt::get(sessionBody, "user"), "id")), std::optional<std::string_view>(seeded.user.id));

  AWAIT(logout, handlePasskeyLogout(plainRequest("https://ai.ubq.fi/auth/logout", {{"authorization", "Bearer " + seeded.token}})));
  EXPECT_EQ(logout.status(), 204.0);
  AWAIT(afterLogout, sessionPresent(getPasskeySession(seeded.token)));
  EXPECT(!afterLogout);  // logout removes the session

  PasskeySessionOptions relayed;
  relayed.authenticatedPasskeyToken = "uos_ai_session_absent";
  AWAIT(relayedStatus, statusOf(handlePasskeySession(plainRequest("https://ai.ubq.fi/auth/session"), relayed)));
  EXPECT_EQ(relayedStatus, 401.0);  // an expired relay token is refused
}

#define EXPECT_TRUE(condition) EXPECT(condition)
#define EXPECT_FALSE(condition) EXPECT(!(condition))
namespace st = uos::chat::stream_translation;
namespace {
  using ChatParts = rt::OrderedMap<std::string, std::string>;
  void expectChatContent(const st::ReconciledChatContent& result, const std::string& text, const std::string& refusal) {
    EXPECT_EQ(result.outputText, text); EXPECT_EQ(result.refusal, refusal);
  }
}
TEST("terminal telemetry events and malformed streams are classified") {
  st::recordResponsesTerminal({"event: response.output_text.delta", {}, "response.output_text.delta", false});
  st::recordResponsesTerminal({"event: response.completed", {{"response", json::object{{"usage", nullptr}}}}, "response.completed", true});
  st::recordResponsesTerminal({"event: response.failed", {}, "response.failed", true});
  EXPECT_TRUE(rt::deepEqual(st::emptyUpstreamCompletionError(), json::object{{"error", json::object{{"message", std::string(st::EMPTY_UPSTREAM_COMPLETION_MESSAGE)}, {"type", "server_error"}, {"code", "empty_upstream_completion"}, {"param", nullptr}}}}));
  EXPECT_THROWS_MATCHING(st::malformedFunctionCallStream("upstream drift"), "upstream drift");
}
TEST("completed text and refusal reconciliation accepts a prefix and rejects a conflict") {
  EXPECT_EQ(st::reconcileCompletedOutputText("hello", "hello world"), " world");
  EXPECT_EQ(st::reconcileCompletedOutputText("hello world", "hello world"), "");
  EXPECT_EQ(st::reconcileCompletedOutputText("hello", "hello"), "");
  EXPECT_EQ(st::reconcileCompletedOutputText("hello", ""), "");
  EXPECT_EQ(st::reconcileCompletedOutputText("hello world", "hello"), "");
  EXPECT_THROWS_MATCHING(st::reconcileCompletedOutputText("hello", "goodbye"), "conflicts with prior text deltas");
  EXPECT_EQ(st::reconcileCompletedRefusal("no", "no thanks"), " thanks");
  EXPECT_EQ(st::reconcileCompletedRefusal("no thanks", "no thanks"), "");
  EXPECT_EQ(st::reconcileCompletedRefusal("", ""), "");
  EXPECT_EQ(st::reconcileCompletedRefusal("no thanks", "no"), "");
  EXPECT_THROWS_MATCHING(st::reconcileCompletedRefusal("no", "yes"), "conflicts with prior refusal deltas");
}
TEST("a chat content part key prefers the item id, then the output index, then the part index") {
  EXPECT_EQ(st::chatOutputTextPartKey({{"item_id", "item-1"}}), "item:item-1:0");
  EXPECT_EQ(st::chatOutputTextPartKey({{"item_id", "item-1"}, {"content_index", 2}}), "item:item-1:2");
  EXPECT_EQ(st::chatOutputTextPartKey({{"item_id", "   "}, {"output_index", 4}}), "output:4:0");
  EXPECT_EQ(st::chatOutputTextPartKey({{"output_index", 2}, {"content_index", "1"}}), "output:2:1");
  EXPECT_EQ(st::chatOutputTextPartKey({{"output_index", "two"}}), "output:two:0");
  EXPECT_EQ(st::chatOutputTextPartKey({{"output_index", true}}), "output:0:0");
  EXPECT_EQ(st::chatOutputTextPartKey({{"content_index", json::object()}}), "output:0:0");
}
TEST("completed chat content parts reject unusable shapes and reconcile repeats") {
  ChatParts text, refusal; const json::object identified{{"item_id", "item-1"}};
  EXPECT_THROWS_MATCHING(st::reconcileChatContentPart(text, refusal, identified, json::value(nullptr)), "missing its part object");
  EXPECT_THROWS_MATCHING(st::reconcileChatContentPart(text, refusal, identified, json::array{"not", "a", "record"}), "missing its part object");
  EXPECT_THROWS_MATCHING(st::reconcileChatContentPart(text, refusal, identified, json::object{{"type", "output_text"}}), "missing string output text");
  EXPECT_THROWS_MATCHING(st::reconcileChatContentPart(text, refusal, identified, json::object{{"type", "refusal"}}), "missing string refusal text");
  expectChatContent(st::reconcileChatContentPart(text, refusal, identified, json::object{{"type", "image"}}), "", "");
  expectChatContent(st::reconcileChatContentPart(text, refusal, identified, json::object{{"type", "output_text"}, {"text", "hello"}}), "hello", "");
  expectChatContent(st::reconcileChatContentPart(text, refusal, identified, json::object{{"type", "output_text"}, {"text", "hello there"}}), " there", "");
  expectChatContent(st::reconcileChatContentPart(text, refusal, {}, json::object{{"type", "output_text"}, {"text", "hello there"}}), "", "");
  ChatParts freshText, freshRefusal;
  expectChatContent(st::reconcileChatContentPart(freshText, freshRefusal, {}, json::object{{"type", "output_text"}, {"text", "fresh text"}}), "fresh text", "");
  expectChatContent(st::reconcileChatContentPart(refusal, refusal, identified, json::object{{"type", "refusal"}, {"refusal", "no"}}), "", "no");
}
TEST("id-less completed chat parts are reconciled by output position") {
  ChatParts text, refusal;
  const json::value output = json::array{json::object{{"content", json::array{json::object{{"type", "output_text"}, {"text", "ha"}}}}}, json::object{{"content", json::array{json::object{{"type", "output_text"}, {"text", "ha"}}}}}};
  expectChatContent(st::reconcileChatResponseOutputContent(text, refusal, {}, output), "haha", "");
}
TEST("id-less aggregate terminal items reconcile all identified positions") {
  ChatParts text, refusal; const json::value part = json::object{{"type", "output_text"}, {"text", "ha"}};
  const json::object first{{"item_id", "item-0"}, {"output_index", 0}}, second{{"item_id", "item-1"}, {"output_index", 1}};
  const json::value aggregate = json::object{{"content", json::array{json::object{{"type", "output_text"}, {"text", "haha"}}}}};
  expectChatContent(st::reconcileChatContentPart(text, refusal, first, part), "ha", "");
  expectChatContent(st::reconcileChatContentPart(text, refusal, second, part), "ha", "");
  expectChatContent(st::reconcileChatOutputItemContent(text, refusal, {}, aggregate), "", "");
  ChatParts responseText, responseRefusal;
  st::reconcileChatContentPart(responseText, responseRefusal, first, part); st::reconcileChatContentPart(responseText, responseRefusal, second, part);
  expectChatContent(st::reconcileChatResponseOutputContent(responseText, responseRefusal, {}, json::array{aggregate}), "", "");
}
TEST("identified completed parts reuse the item-id alias when output_index is omitted") {
  ChatParts text, refusal; const json::object positioned{{"item_id", "item-1"}, {"output_index", 0}}, unpositioned{{"item_id", "item-1"}};
  const json::value part = json::object{{"type", "output_text"}, {"text", "hello"}};
  expectChatContent(st::reconcileChatContentPart(text, refusal, positioned, part), "hello", "");
  EXPECT_EQ(*text.get("output:0:0"), "hello"); EXPECT_EQ(*text.get("item:item-1:0"), "hello");
  expectChatContent(st::reconcileChatContentPart(text, refusal, unpositioned, part), "", "");
  const json::value refused = json::object{{"type", "refusal"}, {"refusal", "no"}};
  expectChatContent(st::reconcileChatContentPart(text, refusal, positioned, refused), "", "no");
  expectChatContent(st::reconcileChatContentPart(text, refusal, unpositioned, refused), "", "");
}
TEST("completed output items and response outputs accumulate their content") {
  ChatParts text, refusal;
  expectChatContent(st::reconcileChatOutputItemContent(text, refusal, {}, json::value(nullptr)), "", "");
  expectChatContent(st::reconcileChatOutputItemContent(text, refusal, {}, json::object{{"content", "not an array"}}), "", "");
  expectChatContent(st::reconcileChatOutputItemContent(text, refusal, {{"item_id", "item-1"}}, json::object{{"content", json::array{json::object{{"type", "output_text"}, {"text", "hello"}}, json::object{{"type", "nonsense"}}}}}), "hello", "");
  expectChatContent(st::reconcileChatResponseOutputContent(text, refusal, {}, json::value("not an array")), "", "");
  const json::value output = json::array{json::object{{"content", json::array{json::object{{"type", "output_text"}, {"text", "hello"}}}}}, json::object{{"content", json::array{json::object{{"type", "refusal"}, {"refusal", "no"}}}}}};
  expectChatContent(st::reconcileChatResponseOutputContent(text, refusal, {}, output), "", "no");
}
TEST("accumulated text, refusal and items are appended only when the response lacks them") {
  const json::object empty;
  EXPECT_TRUE(rt::deepEqual(st::withAccumulatedResponseText(empty, ""), empty));
  auto appended = st::withAccumulatedResponseText(empty, "hello");
  const auto* output = rt::getArray(appended, "output"); EXPECT_EQ(output->size(), 1U);
  EXPECT_EQ(rt::getString((*rt::getArray((*output)[0], "content"))[0], "text").value(), "hello");
  const json::object already{{"output", json::array{json::object{{"content", json::array{json::object{{"type", "output_text"}, {"text", "existing"}}}}}}}};
  EXPECT_TRUE(rt::deepEqual(st::withAccumulatedResponseText(already, "hello"), already));
  EXPECT_TRUE(rt::deepEqual(st::withAccumulatedResponseText(already, "hello", std::numeric_limits<double>::quiet_NaN()), already));
  auto prefixed = already; prefixed["output"].get_array().push_back(json::object());
  EXPECT_EQ(rt::getArray(st::withAccumulatedResponseText(prefixed, "hello", 1), "output")->size(), 3U);
  EXPECT_TRUE(rt::deepEqual(st::withAccumulatedResponseRefusal(empty, ""), empty));
  auto refused = st::withAccumulatedResponseRefusal(empty, "no");
  EXPECT_EQ(rt::getString((*rt::getArray((*rt::getArray(refused, "output"))[0], "content"))[0], "refusal").value(), "no");
  const json::object refusalPresent{{"output", json::array{json::object{{"content", json::array{json::object{{"type", "refusal"}, {"refusal", "existing"}}}}}}}};
  EXPECT_TRUE(rt::deepEqual(st::withAccumulatedResponseRefusal(refusalPresent, "no"), refusalPresent));
  EXPECT_TRUE(rt::deepEqual(st::withAccumulatedResponseItems(empty, {}), empty));
  EXPECT_TRUE(rt::deepEqual(st::withAccumulatedResponseItems(empty, {{{"id", "call-1"}}}), json::object{{"output", json::array{json::object{{"id", "call-1"}}}}}));
}
TEST("a function-call accumulator creates, streams, finalizes and rejects drift") {
  st::ChatFunctionCallAccumulator calls; EXPECT_FALSE(calls.hasCalls());
  EXPECT_FALSE(calls.add({{"item_id", "call-1"}}, json::value("not an item")));
  EXPECT_FALSE(calls.add({{"item_id", "call-1"}}, json::object{{"type", "message"}}));
  EXPECT_THROWS_MATCHING(calls.add({{"item_id", "call-1"}}, json::object{{"type", "function_call"}, {"name", "tool"}}), "missing call_id, name, or string arguments");
  EXPECT_THROWS_MATCHING(calls.add({}, json::object{{"type", "function_call"}, {"call_id", "call-1"}, {"name", "tool"}}), "omitted item_id and output_index");
  const json::object event{{"item_id", "call-1"}}; const auto item = [](std::string arguments) { return json::value(json::object{{"type", "function_call"}, {"call_id", "call-1"}, {"name", "tool"}, {"arguments", arguments}}); };
  const auto created = calls.add(event, item("{")); EXPECT_TRUE(created); EXPECT_TRUE(created->includeIdentity); EXPECT_EQ(created->suffix, "{"); EXPECT_TRUE(calls.hasCalls()); EXPECT_EQ(calls.calls().size(), 1U);
  const auto repeated = calls.add(event, item("{\"a\":1}")); EXPECT_FALSE(repeated->includeIdentity); EXPECT_EQ(repeated->suffix, "\"a\":1}");
  EXPECT_THROWS_MATCHING(calls.add(event, json::object{{"type", "function_call"}, {"call_id", "other"}, {"name", "tool"}, {"arguments", ""}}), "changed its call_id or name");
  EXPECT_THROWS_MATCHING(calls.delta({{"item_id", "absent"}}), "no matching item");
  EXPECT_THROWS_MATCHING(calls.delta({{"item_id", "call-1"}, {"delta", 5}}), "not a string");
  EXPECT_EQ(calls.delta({{"item_id", "call-1"}, {"delta", "!"}}).delta, "!");
  EXPECT_THROWS_MATCHING(calls.done({{"item_id", "absent"}}), "completion has no matching item"); EXPECT_THROWS_MATCHING(calls.done(event), "missing string arguments");
  EXPECT_EQ(calls.done({{"item_id", "call-1"}, {"arguments", "{\"a\":1}!"}}).suffix, "");
  EXPECT_THROWS_MATCHING(calls.delta({{"item_id", "call-1"}, {"delta", "x"}}), "after its completion event");
  EXPECT_THROWS_MATCHING(calls.done({{"item_id", "call-1"}, {"arguments", "different"}}), "changed finalized arguments");
  EXPECT_EQ(calls.done({{"item_id", "call-1"}, {"arguments", "{\"a\":1}!"}}).suffix, ""); calls.assertFinalized();
  EXPECT_THROWS_MATCHING(calls.reconcileItem(event, json::object{{"type", "function_call"}}), "missing string arguments");
  EXPECT_FALSE(calls.reconcileItem(event, json::object{{"type", "message"}}));
  EXPECT_EQ(calls.reconcileOutput(event, json::value("not an array")).size(), 0U); EXPECT_EQ(calls.reconcileOutput(event, json::array{nullptr, json::object{{"type", "message"}}}).size(), 0U);
  st::ChatFunctionCallAccumulator fresh; const auto reconciled = fresh.reconcileItem(event, item("{}")); EXPECT_TRUE(reconciled); EXPECT_EQ(reconciled->suffix, "{}");
  EXPECT_THROWS_MATCHING(fresh.reconcileItem(event, item("{\"b\":2}")), "changed finalized arguments");
  st::ChatFunctionCallAccumulator unfinished; unfinished.add(event, item("")); EXPECT_THROWS_MATCHING(unfinished.assertFinalized(), "ended before finalized arguments");
}
TEST("a chat tool-call delta carries its identity and arguments only when asked") {
  const st::ChatFunctionCall call{"item:call-1", 1, "call-1", "tool", "{}", true};
  EXPECT_TRUE(rt::deepEqual(st::chatToolCallDelta(call), json::object{{"index", 1}, {"function", json::object()}}));
  EXPECT_TRUE(rt::deepEqual(st::chatToolCallDelta(call, {.includeIdentity = true}), json::object{{"index", 1}, {"function", json::object{{"name", "tool"}}}, {"id", "call-1"}, {"type", "function"}}));
  EXPECT_TRUE(rt::deepEqual(st::chatToolCallDelta(call, {.includeIdentity = false, .argumentsDelta = "more"}), json::object{{"index", 1}, {"function", json::object{{"arguments", "more"}}}}));
  st::ChatFunctionCallAccumulator empty; EXPECT_FALSE(st::translatedChatOutputObserved("", "", empty)); EXPECT_TRUE(st::translatedChatOutputObserved("text", "", empty)); EXPECT_TRUE(st::translatedChatOutputObserved("", "no", empty));
  empty.add({{"item_id", "call-1"}}, json::object{{"type", "function_call"}, {"call_id", "call-1"}, {"name", "tool"}, {"arguments", ""}}); EXPECT_TRUE(st::translatedChatOutputObserved("", "", empty));
}
