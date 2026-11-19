// Differential cases for src/auth/passport.ts (tests/passport.test.ts drives the module through src/handler/index.ts
// and src/auth/index.ts, which are not ported, so none of its cases can run yet: "Bun Passport button destination
// completes exchange, sets host cookies, returns to admin and authenticates protected reads", "Passport callback
// rejects absent, mismatched and duplicated state before code exchange", "failed Passport callback clears only
// transient cookies and never creates a session from an agent", "Passport callback only returns to fixed local
// destinations", "Passport admin rejects ordinary users, agent cookies, JWT bearers and cross-origin mutations",
// "Passport verifies signature, expiry, issuer, audience, client, algorithm and key source", "Passport unavailable
// keys fail closed, proxy origin stays fixed and logout clears only its cookie").
//
// The vectors were recorded from Bun 1.4.0: a script set Date.now to a fixed instant, made getRandomValues
// deterministic (byte n of a case is (n * 37 + 11) & 0xff), generated an RSA key and JWTs, mocked the Passport
// issuer (JWKS, exchange, session introspection, revocation) and a harness-style KV stub, then recorded every
// handlePassportRoute / authenticatePassportSession / authenticatePassportAdmin / renewPassportSessionCookie
// outcome: status, headers (Bun's iteration order), body, the issuer calls made and the KV keys left behind.
// Links: tests/passport_differential_test.cpp tests/helpers/admin_auth_harness.cpp src/auth/passport.cpp
//        src/auth/passport_sessions.cpp src/http.cpp src/kv.cpp src/utils.cpp src/config.cpp src/release.cpp src/auth/relay.cpp
//        src/provider/cerebras_rate_limits.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
//        src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
#include "tests/helpers/admin_auth_harness.hpp"
#include "uos/auth/passport.hpp"
#include "uos/kv.hpp"
#include "uos/utils.hpp"

#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "tests/passport_differential_vectors.inc"

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace harness = uos::tests::helpers::admin_auth_harness;
  using namespace uos::auth::passport;

  const json::value& vectors() {
    static const json::value parsed = [] {
      std::string text;
      for (const char* part : PASSPORT_VECTOR_PARTS) text += part;
      return rt::jsonParse(text);
    }();
    return parsed;
  }

  std::string text(const json::value& value, std::string_view key) { return std::string(*rt::getString(value, key)); }

  struct Plan {
    double jwksStatus = 200;
    double exchangeStatus = 200;
    std::optional<std::string> exchangeBody;
    double sessionStatus = 200;
    bool sessionIsAdmin = true;
  };

  Plan planOf(const json::value& testCase) {
    Plan plan;
    const json::value* spec = rt::get(testCase, "plan");
    if (!spec) return plan;
    if (const std::optional<double> value = rt::getNumber(*spec, "jwksStatus")) plan.jwksStatus = *value;
    if (const std::optional<double> value = rt::getNumber(*spec, "exchangeStatus")) plan.exchangeStatus = *value;
    if (const std::optional<std::string_view> value = rt::getString(*spec, "exchangeBody")) plan.exchangeBody = std::string(*value);
    if (const std::optional<double> value = rt::getNumber(*spec, "sessionStatus")) plan.sessionStatus = *value;
    if (const std::optional<bool> value = rt::getBool(*spec, "sessionIsAdmin")) plan.sessionIsAdmin = *value;
    return plan;
  }

  rt::Response statusResponse(std::string body, double status, std::optional<std::string> contentType) {
    rt::ResponseInit init;
    init.status = status;
    if (contentType) init.headers = rt::Headers{{"content-type", *contentType}};
    return rt::Response(rt::BodyInit(std::move(body)), std::move(init));
  }

  // The issuer mock of the recording script: it logs `<method> <url> <authorization|-> <body|-> <redirect|->`.
  rt::Fetch issuerMock(Plan plan, std::shared_ptr<std::vector<std::string>> log) {
    return rt::Fetch([plan, log](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
      const std::string url = harness::urlOf(input);
      std::string bodyText = "-";
      if (init.body) {
        const rt::Response reader(*init.body);
        auto reading = reader.text();
        bodyText = co_await std::move(reading);
      }
      const rt::Headers headers(init.headers.value_or(rt::Headers()));
      log->push_back(init.method.value_or("GET") + " " + url + " " + headers.get("authorization").value_or("-") + " " + bodyText + " " +
                     init.redirect.value_or("-"));
      const json::value& data = vectors();
      if (url == "https://passport.ubq.fi/api/auth/jwks") {
        rt::Response response = statusResponse(text(data, "jwksBody"), plan.jwksStatus, std::string("application/json;charset=utf-8"));
        co_return response;
      }
      if (url == "https://passport.ubq.fi/api/auth/sso/exchange") {
        rt::Response response = statusResponse(plan.exchangeBody.value_or(text(data, "defaultExchangeBody")), plan.exchangeStatus, std::string("application/json"));
        co_return response;
      }
      if (url == "https://passport.ubq.fi/api/auth/sso/session") {
        rt::Response response = statusResponse(text(data, plan.sessionIsAdmin ? "sessionBodyAdmin" : "sessionBodyNonAdmin"), plan.sessionStatus,
                                               std::string("application/json;charset=utf-8"));
        co_return response;
      }
      if (url == "https://passport.ubq.fi/api/auth/sso/session/revoke") {
        rt::ResponseInit revoked;
        revoked.status = 204;
        co_return rt::Response(rt::BodyInit(nullptr), std::move(revoked));
      }
      throw rt::JsError("Unexpected fetch: " + url);
    });
  }

  rt::Request requestOf(const json::value& spec) {
    rt::Headers headers;
    if (const json::array* entries = rt::getArray(spec, "headers")) {
      for (const json::value& entry : *entries) headers.append(std::string(entry.as_array().at(0).as_string()), std::string(entry.as_array().at(1).as_string()));
    }
    rt::RequestInit init;
    init.method = rt::getString(spec, "method") ? std::string(*rt::getString(spec, "method")) : std::string("GET");
    init.headers = std::move(headers);
    return rt::Request(text(spec, "url"), std::move(init));
  }

  rt::Task<json::value> responseRecord(rt::Response response) {
    json::array headers;
    for (const auto& [name, value] : response.headers().entries()) headers.push_back(json::array{json::value(name), json::value(value)});
    auto reading = response.text();
    const std::string body = co_await std::move(reading);
    json::object record;
    record["status"] = response.status();
    record["headers"] = std::move(headers);
    record["body"] = body;
    co_return json::value(std::move(record));
  }

  template <class Result> rt::Task<json::value> authRecord(std::optional<Result> auth) {
    if (!auth) co_return json::value(nullptr);
    json::object record;
    if (std::holds_alternative<PassportResultNotOk>(*auth)) {
      record["ok"] = false;
      auto recording = responseRecord(std::get<PassportResultNotOk>(*auth).response);
      record["response"] = co_await std::move(recording);
      co_return json::value(std::move(record));
    }
    record["ok"] = true;
    if constexpr (std::is_same_v<Result, PassportResult>) {
      const PassportResultOk& ok = std::get<PassportResultOk>(*auth);
      record["token"] = nullptr;
      record["method"] = json::value_from(ok.method);
    } else {
      const PassportAdminResultOk& ok = std::get<PassportAdminResultOk>(*auth);
      record["token"] = ok.token;
      record["method"] = json::value_from(ok.method);
      record["is_super_admin"] = PassportAdminResultOk::is_super_admin;
    }
    co_return rt::canonicalNumbers(json::value(std::move(record)));
  }

  rt::Task<void> storeSession(const json::value& data) {
    auto hashing = uos::utils::sha256Hex(text(data, "persistentToken"));
    const std::string digest = co_await std::move(hashing);
    const json::value& grant = *rt::get(data, "appGrant");
    rt::KvObject record;
    record.set("version", rt::KvValue(1));
    record.set("subject", rt::KvValue(text(grant, "subject")));
    record.set("clientId", rt::KvValue("ai"));
    record.set("audience", rt::KvValue("ai"));
    record.set("origin", rt::KvValue("https://ai.ubq.fi"));
    record.set("redirectUri", rt::KvValue(text(grant, "redirectUri")));
    record.set("createdAt", rt::KvValue(text(grant, "createdAt")));
    record.set("appSessionToken", rt::KvValue(text(grant, "token")));
    harness::kvStore.set(harness::keyToString(rt::KvKey{"uos_ai", "auth", "passport", "sessions", digest}), rt::KvValue(record));
  }

  rt::Task<json::value> runCase(const json::value& testCase) {
    const std::string fn = text(testCase, "fn");
    const rt::Request req = requestOf(*rt::get(testCase, "request"));
    json::object outcome;
    rt::Reason failure;
    json::value value;
    try {
      if (fn == "route") {
        auto handling = handlePassportRoute(req, text(testCase, "path"));
        const std::optional<rt::Response> response = co_await std::move(handling);
        if (response) {
          auto recording = responseRecord(*response);
          value = co_await std::move(recording);
        }
      } else if (fn == "session") {
        auto authenticating = authenticatePassportSession(req);
        std::optional<PassportResult> auth = co_await std::move(authenticating);
        auto recording = authRecord(std::move(auth));
        value = co_await std::move(recording);
      } else if (fn == "admin") {
        auto authenticating = authenticatePassportAdmin(req);
        std::optional<PassportAdminResult> auth = co_await std::move(authenticating);
        auto recording = authRecord(std::move(auth));
        value = co_await std::move(recording);
      } else {
        const rt::Response renewed = renewPassportSessionCookie(req, statusResponse("x", *rt::getNumber(testCase, "responseStatus"), std::nullopt));
        auto recording = responseRecord(renewed);
        value = co_await std::move(recording);
      }
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) outcome["error"] = rt::reasonToString(failure);
    else outcome["value"] = value;
    co_return json::value(std::move(outcome));
  }
}

ASYNC_TEST("passport routes, session and admin authentication and cookie renewal match Bun") {
  const json::value& data = vectors();
  const double now = *rt::getNumber(data, "now");
  const auto realNow = rt::globalDateNow();
  rt::globalDateNow() = [now] { return now; };
  EXPECT_EQ(rt::get(data, "cases")->as_array().size(), 68u);
  std::size_t issuerCalls = 0;
  for (const json::value& testCase : rt::get(data, "cases")->as_array()) {
    const std::string name = text(testCase, "name");
    resetPassportCacheForTest();
    harness::resetHarness();
    auto counter = std::make_shared<std::uint64_t>(0);
    const rt::crypto::ScopedRandomSource random([counter](std::span<std::uint8_t> bytes) {
      for (std::uint8_t& byte : bytes) byte = static_cast<std::uint8_t>(((*counter)++ * 37 + 11) & 0xff);
    });
    auto log = std::make_shared<std::vector<std::string>>();
    const rt::GlobalFetchOverride fetch(issuerMock(planOf(testCase), log));
    const std::optional<std::string_view> kvMode = rt::getString(testCase, "kv");
    if (kvMode == "none") uos::kv::setKvForTest(nullptr);
    if (kvMode == "session") {
      auto storing = storeSession(data);
      co_await std::move(storing);
    }
    auto running = runCase(testCase);
    const json::value result = co_await std::move(running);
    if (!rt::deepEqual(result, *rt::get(testCase, "result"))) {
      rt::test::report(__FILE__, __LINE__, name + ": result " + rt::jsonStringify(result) + " != " + rt::jsonStringify(*rt::get(testCase, "result")), false);
    }
    json::array fetches;
    for (const std::string& line : *log) fetches.emplace_back(line);
    issuerCalls += log->size();
    if (!rt::deepEqual(json::value(fetches), *rt::get(testCase, "fetches"))) {
      rt::test::report(__FILE__, __LINE__, name + ": fetches " + rt::jsonStringify(json::value(fetches)) + " != " + rt::jsonStringify(*rt::get(testCase, "fetches")), false);
    }
    std::vector<std::string> keys;
    for (const auto& [key, value] : harness::kvStore) keys.push_back(key);
    rt::sort(keys);
    json::array storeKeys;
    for (const std::string& key : keys) storeKeys.emplace_back(key);
    if (!rt::deepEqual(json::value(storeKeys), *rt::get(testCase, "store"))) {
      rt::test::report(__FILE__, __LINE__, name + ": store " + rt::jsonStringify(json::value(storeKeys)) + " != " + rt::jsonStringify(*rt::get(testCase, "store")), false);
    }
    harness::cleanupHarness();
  }
  rt::globalDateNow() = realNow;
  resetPassportCacheForTest();
  // Every recorded issuer call was replayed (the cases above compare them one by one).
  EXPECT_EQ(issuerCalls, 37u);
}
