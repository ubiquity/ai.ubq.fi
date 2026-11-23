// Port of tests/passkeys.test.ts: bounds on anonymous passkey login challenge issuance, over the
// ported passkeys harness KV stub (tests/helpers/passkeys_harness). Each case resets the harness first
// and removes the stub afterwards, as the TS beforeEach/afterEach do.
// Links: tests/passkeys_test.cpp tests/helpers/passkeys_harness.cpp src/auth/passkeys.cpp src/auth/relay.cpp src/http.cpp src/request.cpp
//   src/utils.cpp src/kv.cpp src/storage/kv.cpp src/release.cpp src/storage/sqlite_readonly.cpp src/storage/native_kv.cpp
// src/config.cpp src/provider/cerebras_rate_limits.cpp
//
// These tests pin the acceptance contract for issue #115: a burst of
// unauthenticated login-start requests is bounded by a deterministic,
// clock-aligned window; a rejected request writes nothing; and named attempts
// cannot distinguish an unknown handle from a handle without passkeys.
#include "tests/helpers/passkeys_harness.hpp"
#include "uos/auth/passkeys.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::auth::passkeys;
namespace harness = uos::tests::helpers::passkeys_harness;

namespace {
  struct HarnessScope {
    HarnessScope() { harness::resetHarness(); }
    ~HarnessScope() { harness::cleanupHarness(); }
    HarnessScope(const HarnessScope&) = delete;
    HarnessScope& operator=(const HarnessScope&) = delete;
  };

  const std::string loginStartUrl = "https://ai.ubq.fi/api/auth/login/start";

  rt::Request loginStartRequest(const json::object& body = {}) {
    rt::RequestInit init;
    init.method = "POST";
    init.headers = rt::Headers{{"Content-Type", "application/json"}};
    init.body = rt::BodyInit(rt::jsonStringify(body));
    return rt::Request(loginStartUrl, init);
  }

  double challengeKeyCount() {
    double count = 0;
    for (const auto& [key, value] : harness::kvStore)
      if (rt::includes(key, "\"challenges\"")) count += 1;
    return count;
  }

  double windowStartMs(double atMs) { return rt::Math::floor(atMs / PASSKEY_LOGIN_START_WINDOW_MS) * PASSKEY_LOGIN_START_WINDOW_MS; }

  std::vector<std::pair<std::string, std::string>> storeSnapshot() {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& [key, value] : harness::kvStore) out.emplace_back(key, rt::jsonStringify(value.toJson()));
    return out;
  }

  rt::Task<rt::Response> loginStart(json::object body) {
    rt::Request request = loginStartRequest(body);
    rt::Task<rt::Response> handling = handlePasskeyLoginStart(std::move(request));
    rt::Response response = co_await std::move(handling);
    co_return response;
  }

  rt::Task<json::value> bodyOf(rt::Response response) {
    rt::Task<json::value> reading = response.json();
    json::value body = co_await std::move(reading);
    co_return body;
  }
}

ASYNC_TEST("passkey login start bounds anonymous challenge creation and writes nothing after the limit") {
  HarnessScope scope;
  harness::kvStore.clear();

  for (double index = 0; index < PASSKEY_LOGIN_START_LIMIT; index += 1) {
    const rt::Response response = co_await loginStart(json::object());
    EXPECT_EQ(response.status(), 200.0);
    EXPECT_EQ(response.headers().get("Cache-Control"), std::optional<std::string>("no-store"));
  }
  EXPECT_EQ(challengeKeyCount(), PASSKEY_LOGIN_START_LIMIT);

  const rt::Response limited = co_await loginStart(json::object());
  EXPECT_EQ(limited.status(), 429.0);
  const json::value payload = co_await bodyOf(limited);
  const json::value* error = rt::get(payload, "error");
  ASSERT(error != nullptr);
  EXPECT_EQ(rt::getString(*error, "code"), std::optional<std::string_view>("rate_limit_exceeded"));
  EXPECT_EQ(rt::getString(*error, "type"), std::optional<std::string_view>("rate_limit_error"));
  EXPECT(rt::Regex("too many", "i").test(std::string(rt::getString(*error, "message").value_or(""))));
  const double retryAfterSeconds = rt::stringToNumber(limited.headers().get("Retry-After").value_or(""));
  EXPECT(rt::Number::isInteger(retryAfterSeconds));
  EXPECT(retryAfterSeconds > 0);
  EXPECT(retryAfterSeconds <= PASSKEY_LOGIN_START_WINDOW_MS / 1000);
  EXPECT_EQ(challengeKeyCount(), PASSKEY_LOGIN_START_LIMIT);

  // Every rejected request must leave the KV store untouched, not just skip the
  // challenge write.
  const auto before = storeSnapshot();
  const rt::Response repeated = co_await loginStart(json::object());
  EXPECT_EQ(repeated.status(), 429.0);
  const auto after = storeSnapshot();
  EXPECT(after == before);
}

ASYNC_TEST("passkey login start resets deterministically at the next clock-aligned window") {
  HarnessScope scope;
  harness::kvStore.clear();
  const auto realNow = rt::globalDateNow();
  double clock = realNow();
  rt::globalDateNow() = [&clock] { return clock; };
  for (double index = 0; index < PASSKEY_LOGIN_START_LIMIT; index += 1) {
    const rt::Response response = co_await loginStart(json::object());
    EXPECT_EQ(response.status(), 200.0);
  }
  const rt::Response limited = co_await loginStart(json::object());
  EXPECT_EQ(limited.status(), 429.0);
  EXPECT(harness::kvStore.has(harness::keyToString(passkeyLoginStartThrottleKey(windowStartMs(clock)))));

  // Advancing exactly to the next window boundary admits requests again.
  clock = windowStartMs(clock) + PASSKEY_LOGIN_START_WINDOW_MS;
  const rt::Response allowed = co_await loginStart(json::object());
  EXPECT_EQ(allowed.status(), 200.0);
  const json::value payload = co_await bodyOf(allowed);
  EXPECT(rt::getString(rt::get(rt::get(payload, "publicKey"), "challenge")).has_value());
  EXPECT_EQ(challengeKeyCount(), PASSKEY_LOGIN_START_LIMIT + 1);
  rt::globalDateNow() = realNow;
}

ASYNC_TEST("named passkey login failures are indistinguishable from discoverable login") {
  HarnessScope scope;
  harness::kvStore.clear();
  const double now = rt::dateNow();
  const PasskeyUserRecord credentialless{
    .id = "user-credentialless", .handle = "credentialless", .is_admin = true, .credential_ids = {}, .created_at_ms = now, .updated_at_ms = now};
  harness::kvStore.set(harness::keyToString(passkeyUserKey(credentialless.id)), rt::KvValue(json::value_from(credentialless)));
  harness::kvStore.set(harness::keyToString(passkeyHandleKey(credentialless.handle)), rt::KvValue(credentialless.id));

  std::vector<json::object> bodies;
  bodies.push_back(json::object{{"handle", "unknown-handle"}});
  bodies.push_back(json::object{{"handle", credentialless.handle}});
  bodies.push_back(json::object());
  std::vector<json::object> shapes;
  for (const json::object& body : bodies) {
    const rt::Response response = co_await loginStart(body);
    EXPECT_EQ(response.status(), 200.0);
    const json::value payload = co_await bodyOf(response);
    const json::value* publicKey = rt::get(payload, "publicKey");
    EXPECT(rt::getString(rt::get(publicKey, "challenge")).has_value());
    EXPECT(rt::get(publicKey, "allowCredentials") == nullptr);
    EXPECT_EQ(rt::getString(rt::get(publicKey, "userVerification")), std::optional<std::string_view>("required"));
    json::object shape;
    if (const json::object* members = rt::getObject(publicKey)) {
      for (const json::key_value_pair& member : *members)
        if (member.key() != "challenge") shape[member.key()] = member.value();
    }
    shapes.push_back(std::move(shape));
  }
  EXPECT(rt::deepEqual(json::value(shapes[0]), json::value(shapes[1])));
  EXPECT(rt::deepEqual(json::value(shapes[1]), json::value(shapes[2])));
}

ASYNC_TEST("named failures share the anonymous login-start budget") {
  HarnessScope scope;
  harness::kvStore.clear();
  const double now = rt::dateNow();
  const PasskeyUserRecord credentialless{
    .id = "user-budget", .handle = "budget-credentialless", .is_admin = false, .credential_ids = {}, .created_at_ms = now, .updated_at_ms = now};
  harness::kvStore.set(harness::keyToString(passkeyUserKey(credentialless.id)), rt::KvValue(json::value_from(credentialless)));
  harness::kvStore.set(harness::keyToString(passkeyHandleKey(credentialless.handle)), rt::KvValue(credentialless.id));

  for (double index = 0; index < PASSKEY_LOGIN_START_LIMIT; index += 1) {
    json::object body;
    body["handle"] = rt::mod(index, 2) == 0 ? std::string("unknown-budget-handle") : credentialless.handle;
    const rt::Response response = co_await loginStart(body);
    EXPECT_EQ(response.status(), 200.0);
  }
  const rt::Response limited = co_await loginStart(json::object());
  EXPECT_EQ(limited.status(), 429.0);
  EXPECT_EQ(challengeKeyCount(), PASSKEY_LOGIN_START_LIMIT);
}

ASYNC_TEST("concurrent passkey login start bursts never exceed the anonymous window budget") {
  HarnessScope scope;
  harness::kvStore.clear();
  std::vector<rt::Promise<rt::Response>> pending;
  for (double index = 0; index < PASSKEY_LOGIN_START_LIMIT + 5; index += 1) pending.push_back(rt::Promise<rt::Response>(loginStart(json::object())));
  const std::vector<rt::Response> responses = co_await rt::all(std::move(pending)).wait();
  double allowed = 0;
  double limited = 0;
  for (const rt::Response& response : responses) {
    if (response.status() == 200) allowed += 1;
    if (response.status() == 429) limited += 1;
  }
  EXPECT_EQ(allowed, PASSKEY_LOGIN_START_LIMIT);
  EXPECT_EQ(limited, 5.0);
  EXPECT_EQ(challengeKeyCount(), PASSKEY_LOGIN_START_LIMIT);
}
