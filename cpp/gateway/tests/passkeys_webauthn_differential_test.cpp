// Differential cases for src/auth/passkeys.ts end to end, including its port of
// @simplewebauthn/server 13.2.2 (verifyRegistrationResponse with the none, packed, fido-u2f, tpm,
// apple, android-key and android-safetynet formats; verifyAuthenticationResponse with ES256/ES512,
// RS1/RS256/RS384/RS512, PS256/PS384/PS512 and EdDSA keys; generateAuthenticationOptions) and the
// library's quirks and logged error texts.
//
// The vectors were recorded by Bun 1.4.0 running the TS handlers over tests/helpers/passkeys-harness.ts
// with Date.now fixed at 1790000000000, crypto.getRandomValues zero-filling and crypto.randomUUID
// returning "00000000-0000-4000-8000-000000000000" (the UUID rt derives from zero bytes): each case
// seeds the KV stub, posts one JSON body to a handler and records the status, the body text, the
// Set-Cookie and Retry-After headers, the console.warn calls and the whole store afterwards (or the
// error the handler rejects with), and the URLs the library fetched. Keys, certificates, CRLs and
// signatures were made with Bun's WebCrypto and @peculiar/x509. globalThis.fetch was a mock serving the
// case's `crls` (base64 DER by URL) and a 404 for anything else; here rt::GlobalFetchOverride stands in.
// The library's module-level CRL cache persists from case to case in both runs, as the order is the same.
// Links: tests/passkeys_webauthn_differential_test.cpp tests/helpers/passkeys_harness.cpp src/auth/passkeys.cpp src/auth/relay.cpp
//   src/http.cpp src/request.cpp src/utils.cpp src/kv.cpp src/storage/kv.cpp src/release.cpp src/storage/sqlite_readonly.cpp
//   src/storage/native_kv.cpp src/config.cpp src/provider/cerebras_rate_limits.cpp
#include "tests/helpers/passkeys_harness.hpp"
#include "uos/auth/passkeys.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/test_async.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using namespace uos::auth::passkeys;
namespace harness = uos::tests::helpers::passkeys_harness;

#include "tests/passkeys_webauthn_differential_vectors.inc"

namespace {
  constexpr double FIXED_NOW = 1'790'000'000'000;

  // A console argument as the oracle's JSON.stringify printed it.
  json::value consoleJson(const rt::console::Value& value) {
    switch (value.kind()) {
      case rt::console::Value::Kind::string: return json::value(value.string());
      case rt::console::Value::Kind::boolean: return json::value(value.boolean());
      case rt::console::Value::Kind::number: return rt::makeNumber(value.number());
      case rt::console::Value::Kind::null: return json::value(nullptr);
      case rt::console::Value::Kind::array: {
        json::array out;
        for (const rt::console::Value& element : value.elements()) out.push_back(consoleJson(element));
        return out;
      }
      case rt::console::Value::Kind::object: {
        json::object out;
        for (const auto& [key, member] : value.properties()) out[key] = consoleJson(member);
        return out;
      }
      case rt::console::Value::Kind::error: return json::object();
      case rt::console::Value::Kind::undefined: return json::value(nullptr);
    }
    return nullptr;
  }

  rt::Request requestFor(const json::object& vector) {
    rt::Headers headers{{"Content-Type", "application/json"}};
    if (const json::object* extra = rt::getObject(vector, "headers"))
      for (const json::key_value_pair& header : *extra) headers.set(header.key(), std::string(header.value().as_string()));
    rt::RequestInit init;
    init.method = "POST";
    init.headers = headers;
    if (const std::optional<std::string_view> body = rt::getString(vector, "body")) init.body = rt::BodyInit(std::string(*body));
    return rt::Request(std::string(rt::getString(vector, "url").value_or("")), init);
  }

  rt::Task<rt::Response> dispatch(std::string kind, rt::Request request, json::object options) {
    if (kind == "register-finish") {
      rt::Task<rt::Response> handling = handlePasskeyRegisterFinish(std::move(request));
      rt::Response response = co_await std::move(handling);
      co_return response;
    }
    if (kind == "login-finish") {
      rt::Task<rt::Response> handling = handlePasskeyLoginFinish(std::move(request));
      rt::Response response = co_await std::move(handling);
      co_return response;
    }
    if (kind == "register-start") {
      PasskeyRegisterStartOptions startOptions;
      if (const std::optional<bool> admin = rt::getBool(options, "defaultIsAdmin")) startOptions.defaultIsAdmin = *admin;
      if (const std::optional<std::string_view> token = rt::getString(options, "authenticatedPasskeyToken")) startOptions.authenticatedPasskeyToken = std::string(*token);
      rt::Task<rt::Response> handling = handlePasskeyRegisterStart(std::move(request), std::move(startOptions));
      rt::Response response = co_await std::move(handling);
      co_return response;
    }
    rt::Task<rt::Response> handling = handlePasskeyLoginStart(std::move(request));
    rt::Response response = co_await std::move(handling);
    co_return response;
  }

  // One replayed case: the observed outcome in the oracle's shape.
  rt::Task<json::object> replay(json::object vector) {
    harness::resetHarness();
    const auto realNow = rt::globalDateNow();
    rt::globalDateNow() = [] { return FIXED_NOW; };
    const rt::crypto::ScopedRandomSource zeros([](std::span<std::uint8_t> out) { std::fill(out.begin(), out.end(), std::uint8_t{0}); });
    for (const json::value& entry : vector.at("seed").as_array()) {
      const json::array& pair = entry.as_array();
      harness::kvStore.set(std::string(pair[0].as_string()), rt::KvValue(rt::jsonParse(pair[1].as_string())));
    }
    rt::console::Capture capture({rt::console::Method::warn});
    auto fetches = std::make_shared<std::vector<std::string>>();
    auto crls = std::make_shared<json::object>(rt::getObject(vector, "crls") ? *rt::getObject(vector, "crls") : json::object());
    const rt::GlobalFetchOverride fetchMock([fetches, crls](rt::RequestInfo input, rt::RequestInit) -> rt::Response {
      const std::string url = harness::requestUrl(input);
      fetches->push_back(url);
      if (const std::optional<std::string_view> body = rt::getString(*crls, url)) return rt::Response(rt::BodyInit(rt::fromBase64(*body)));
      rt::ResponseInit init;
      init.status = 404;
      return rt::Response(rt::BodyInit(std::string("missing")), init);
    });
    json::object observed;
    std::optional<std::string> thrown;
    std::optional<rt::Response> response;
    try {
      const json::object options = rt::getObject(vector, "options") ? *rt::getObject(vector, "options") : json::object();
      rt::Task<rt::Response> handling = dispatch(std::string(vector.at("kind").as_string()), requestFor(vector), options);
      response = co_await std::move(handling);
    } catch (const rt::JsError& error) {
      thrown = error.name + ": " + error.message;
    } catch (const std::exception& error) {
      thrown = std::string("Error: ") + error.what();
    }
    if (thrown) {
      observed["throws"] = *thrown;
    } else {
      observed["status"] = rt::makeNumber(static_cast<double>(response->status()));
      rt::Task<std::string> reading = response->text();
      const std::string text = co_await std::move(reading);
      observed["body"] = text;
      const std::optional<std::string> setCookie = response->headers().get("Set-Cookie");
      observed["setCookie"] = setCookie ? json::value(*setCookie) : json::value(nullptr);
      const std::optional<std::string> retryAfter = response->headers().get("Retry-After");
      observed["retryAfter"] = retryAfter ? json::value(*retryAfter) : json::value(nullptr);
    }
    json::array warnings;
    for (const rt::console::Call& call : capture.calls()) {
      json::array args;
      for (const rt::console::Value& arg : call.args) args.push_back(consoleJson(arg));
      warnings.push_back(std::move(args));
    }
    observed["warnings"] = std::move(warnings);
    json::array store;
    for (const auto& [key, value] : harness::kvStore) {
      json::array row;
      row.emplace_back(key);
      row.emplace_back(value.isUndefined() ? std::string("undefined") : rt::jsonStringify(value.toJson()));
      store.push_back(std::move(row));
    }
    observed["store"] = std::move(store);
    json::array fetched;
    for (const std::string& url : *fetches) fetched.emplace_back(url);
    observed["fetches"] = std::move(fetched);
    rt::globalDateNow() = realNow;
    harness::cleanupHarness();
    co_return observed;
  }

  // The recorded expectation with the members a replay observes, in the same order.
  json::object expectedOf(const json::object& vector) {
    const json::object& expected = vector.at("expected").as_object();
    json::object out;
    if (const json::value* thrown = rt::get(expected, "throws")) {
      out["throws"] = *thrown;
    } else {
      out["status"] = expected.at("status");
      out["body"] = expected.at("body");
      out["setCookie"] = expected.at("setCookie");
      out["retryAfter"] = expected.at("retryAfter");
    }
    out["warnings"] = expected.at("warnings");
    out["store"] = expected.at("store");
    out["fetches"] = expected.at("fetches");
    return out;
  }
}

ASYNC_TEST("passkey handlers replay the TS registration, assertion and start vectors") {
  std::size_t count = 0;
  for (const char* const text : PASSKEY_WEBAUTHN_VECTORS) {
    const json::object vector = rt::jsonParse(text).as_object();
    rt::Task<json::object> replaying = replay(vector);
    const json::object observed = co_await std::move(replaying);
    const json::object expected = expectedOf(vector);
    const bool same = rt::deepEqual(json::value(observed), json::value(expected));
    if (!same) {
      rt::test::report(__FILE__, __LINE__, std::string(vector.at("name").as_string()) + "\n  expected " + rt::jsonStringify(json::value(expected)) + "\n  observed " +
                         rt::jsonStringify(json::value(observed)),
                       false);
    }
    count += 1;
  }
  EXPECT_EQ(count, sizeof(PASSKEY_WEBAUTHN_VECTORS) / sizeof(PASSKEY_WEBAUTHN_VECTORS[0]));
}
