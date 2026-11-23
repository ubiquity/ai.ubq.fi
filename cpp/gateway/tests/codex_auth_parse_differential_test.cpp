// Differential cases for the pure parsers and helpers of src/codex/auth.ts beyond tests/codex-auth-coverage.test.ts:
// pool owners (managed, external, malformed), auth.json shapes, JWT expiry edge values, email claims, the client
// version pick, native-owner upserts and the refresh windows. The vectors were recorded by running the module under
// bun 1.4.0 with a fixed clock; the inputs below rebuild the generator's inputs exactly.
// Links: tests/codex_auth_parse_differential_test.cpp src/codex/auth.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp
//        src/codex/external_auth.cpp src/codex/capacity_routing.cpp src/codex/rate_limit_429.cpp src/codex/routing_mutations.cpp
//        src/codex/routing_state.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp
//        src/authority/native_codex_contracts.cpp src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp
//        src/bounded_response_body.cpp src/config.cpp src/kv.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
//        src/provider/health.cpp src/release.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
//        src/runtime/http_upgrade.cpp src/runtime/http.cpp src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
//        src/utils.cpp src/types.cpp
#include "uos/codex/auth.hpp"

#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

#include <cstdio>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace auth = uos::codex::auth;
  using uos::types::CodexAuthPoolState;
  using uos::types::CodexAuthState;

  const char* const VECTORS =
#include "codex_auth_parse_differential_vectors.inc"
    ;

  constexpr double NOW = 1'790'000'000'000;

  std::string b64url(const json::value& value) {
    std::string encoded = rt::replaceAll(rt::replaceAll(rt::btoa(rt::jsonStringify(value)), "+", "-"), "/", "_");
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return encoded;
  }

  std::string jwt(const json::value& payload) {
    json::object header;
    header["alg"] = "none";
    return b64url(json::value(header)) + "." + b64url(payload) + ".sig";
  }

  json::value object(std::initializer_list<std::pair<std::string_view, json::value>> members) {
    json::object out;
    for (const auto& [key, value] : members) out[key] = value;
    return json::value(std::move(out));
  }

  json::value errorJson(const rt::Reason& reason) {
    json::object error;
    error["name"] = rt::errorName(reason) ? json::value(*rt::errorName(reason)) : json::value(nullptr);
    error["code"] = nullptr;
    error["status"] = nullptr;
    error["message"] = rt::errorMessage(reason).value_or(rt::reasonToString(reason));
    try {
      std::rethrow_exception(reason);
    } catch (const auth::CodexError& codexError) {
      error["code"] = codexError.code;
      error["status"] = rt::makeNumber(codexError.status);
    } catch (...) {
    }
    return object({{"error", json::value(std::move(error))}});
  }

  // The generator's `outcome(run)`: `{ value }` (JSON of the result) or `{ error }`.
  json::value outcome(const std::function<json::value()>& run) {
    try {
      return object({{"value", run()}});
    } catch (...) {
      return errorJson(std::current_exception());
    }
  }

  json::value poolJson(const std::optional<CodexAuthPoolState>& pool) { return pool ? json::value_from(*pool) : json::value(nullptr); }

  json::value numberOrNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
  json::value stringOrNull(const std::optional<std::string>& value) { return value ? json::value(*value) : json::value(nullptr); }

  bool same(const json::object& expected, const json::object& actual, std::string_view name) {
    const json::value* want = rt::get(expected, name);
    const json::value* got = rt::get(actual, name);
    if (want != nullptr && got != nullptr && rt::deepEqual(*want, *got)) return true;
    std::fprintf(stderr, "    vector %.*s differs\n      expected: %s\n      actual:   %s\n", static_cast<int>(name.size()), name.data(),
                 want != nullptr ? rt::jsonStringify(*want).c_str() : "<missing>", got != nullptr ? rt::jsonStringify(*got).c_str() : "<missing>");
    return false;
  }

  TEST("auth.ts parsers and helpers match the bun oracle on edge inputs") {
    const json::value expectedValue = rt::jsonParse(VECTORS);
    const json::object& expected = expectedValue.as_object();
    const auto realNow = rt::globalDateNow();
    rt::globalDateNow() = [] { return NOW; };
    json::object out;

    const std::string hash(64, 'a');
    json::object externalClaims;
    externalClaims["chatgpt_user_id"] = "user-1";
    externalClaims["chatgpt_account_id"] = "acct-x";
    const std::string extToken = jwt(object({{"exp", rt::makeNumber(NOW / 1000 + 3600)}, {"https://api.openai.com/auth", json::value(externalClaims)}}));
    const auto account = [](std::initializer_list<std::pair<std::string_view, json::value>> extra) {
      json::object value;
      value["access_token"] = "access";
      value["refresh_token"] = "refresh";
      value["account_id"] = "acct";
      value["updated_at_ms"] = 5;
      for (const auto& [key, member] : extra) value[key] = member;
      return json::value(std::move(value));
    };
    const auto poolOf = [](json::value accountValue, json::value updatedAtMs) {
      return object({{"accounts", json::value(json::array{std::move(accountValue)})}, {"updated_at_ms", std::move(updatedAtMs)}});
    };
    const auto externalAccount = [&extToken, &hash](std::string refreshToken, std::string accountId, std::string userId) {
      return object({{"access_token", json::value(extToken)},
                     {"refresh_token", json::value(refreshToken)},
                     {"account_id", json::value(accountId)},
                     {"updated_at_ms", json::value(5)},
                     {"native_owner", object({{"codex_home", json::value("/h")},
                                               {"generation_hash", json::value(hash)},
                                               {"auth_mode", json::value("chatgptAuthTokens")},
                                               {"chatgpt_user_id", json::value(userId)}})}});
    };
    const std::vector<std::pair<std::string, json::value>> poolCases{
      {"plain", poolOf(account({}), json::value(1))},
      {"ownerManaged",
       poolOf(account({{"native_owner", object({{"codex_home", json::value("/home/x/.codex")},
                                                 {"generation_hash", json::value(hash)},
                                                 {"native_refreshed_at", json::value("2026-01-01T00:00:00Z")}})}}),
              json::value(1))},
      {"ownerEmptyRefreshed",
       poolOf(account({{"native_owner", object({{"codex_home", json::value("/home/x/.codex")}, {"generation_hash", json::value(hash)}, {"native_refreshed_at", json::value("")}})}}),
              json::value(1))},
      {"ownerNull", poolOf(account({{"native_owner", json::value(nullptr)}}), json::value(1))},
      {"ownerRelativeHome", poolOf(account({{"native_owner", object({{"codex_home", json::value("home/.codex")}, {"generation_hash", json::value(hash)}})}}), json::value(1))},
      {"ownerShortHash", poolOf(account({{"native_owner", object({{"codex_home", json::value("/h")}, {"generation_hash", json::value("abc")}})}}), json::value(1))},
      {"ownerUpperHash", poolOf(account({{"native_owner", object({{"codex_home", json::value("/h")}, {"generation_hash", json::value(std::string(64, 'A'))}})}}), json::value(1))},
      {"ownerBadMode",
       poolOf(account({{"native_owner", object({{"codex_home", json::value("/h")},
                                                 {"generation_hash", json::value(hash)},
                                                 {"auth_mode", json::value("apikey")},
                                                 {"chatgpt_user_id", json::value("u")}})}}),
              json::value(1))},
      {"ownerModeNoUser",
       poolOf(account({{"native_owner", object({{"codex_home", json::value("/h")}, {"generation_hash", json::value(hash)}, {"auth_mode", json::value("chatgptAuthTokens")}})}}),
              json::value(1))},
      {"externalValid", poolOf(externalAccount("", "acct-x", "user-1"), json::value(1))},
      {"externalWrongUser", poolOf(externalAccount("", "acct-x", "user-2"), json::value(1))},
      {"externalWithRefresh", poolOf(externalAccount("r", "acct-x", "user-1"), json::value(1))},
      {"externalWrongAccount", poolOf(externalAccount("", "acct-y", "user-1"), json::value(1))},
      {"emptyRefresh", poolOf(account({{"refresh_token", json::value("")}}), json::value(1))},
      {"numberRefresh", poolOf(account({{"refresh_token", json::value(5)}}), json::value(1))},
      {"infiniteUpdated", poolOf(account({}), json::value(rt::Number::POSITIVE_INFINITY))},
      {"fractionUpdated", poolOf(account({{"updated_at_ms", json::value(1.5)}}), json::value(-2.5))},
      {"stringUpdated", poolOf(account({}), json::value("1"))},
      {"arrayPool", json::value(json::array{account({})})},
      {"accountsNotArray", object({{"accounts", object({{"0", account({})}})}, {"updated_at_ms", json::value(1)}})},
    };
    {
      json::object pools;
      for (const auto& [name, value] : poolCases) pools[name] = outcome([&value] { return poolJson(auth::parseCodexAuthPool(value)); });
      out["pools"] = std::move(pools);
    }

    {
      const auto tokens = [](std::initializer_list<std::pair<std::string_view, json::value>> members) { return object(members); };
      const std::vector<std::pair<std::string, json::value>> authJsonCases{
        {"external", object({{"auth_mode", json::value("chatgptAuthTokens")},
                             {"tokens", tokens({{"access_token", json::value("a")}, {"refresh_token", json::value("r")}, {"account_id", json::value("i")}})}})},
        {"apikeyMode", object({{"auth_mode", json::value("apikey")},
                               {"tokens", tokens({{"access_token", json::value("a")}, {"refresh_token", json::value("r")}, {"account_id", json::value("i")}})}})},
        {"emptyAccess", object({{"tokens", tokens({{"access_token", json::value("")}, {"refresh_token", json::value("r")}, {"account_id", json::value("i")}})}})},
        {"arrayTokens", object({{"tokens", json::value(json::array{json::value("a")})}})},
        {"extraFields", object({{"tokens", tokens({{"access_token", json::value("a")},
                                                   {"refresh_token", json::value("r")},
                                                   {"account_id", json::value("i")},
                                                   {"id_token", json::value("x")}})},
                                {"OPENAI_API_KEY", json::value(nullptr)}})},
        {"arrayValue", json::value(json::array{json::value(1)})},
      };
      json::object authJson;
      for (const auto& [name, value] : authJsonCases)
        authJson[name] = outcome([&value] {
          const std::optional<auth::ParseCodexAuthFromAuthJsonResult> parsed = auth::parseCodexAuthFromAuthJson(value);
          return parsed ? json::value_from(*parsed) : json::value(nullptr);
        });
      out["authJson"] = std::move(authJson);
    }

    {
      const auto pad = [](const std::string& payload) { return "h." + payload + ".s"; };
      const auto expMs = [](const std::string& token) { return numberOrNull(auth::getJwtExpMs(std::string_view(token))); };
      json::object jwtOut;
      jwtOut["spaced"] = expMs("  " + jwt(object({{"exp", json::value(1'700'000'000)}})) + "  ");
      jwtOut["zero"] = expMs(jwt(object({{"exp", json::value(0)}})));
      jwtOut["negative"] = expMs(jwt(object({{"exp", json::value(-5)}})));
      jwtOut["fraction"] = expMs(jwt(object({{"exp", json::value(1.5)}})));
      jwtOut["huge"] = expMs(jwt(object({{"exp", json::value(1e300)}})));
      jwtOut["fourParts"] = expMs(jwt(object({{"exp", json::value(1)}})) + ".extra");
      jwtOut["standardBase64"] = expMs(pad(rt::btoa(rt::jsonStringify(object({{"exp", json::value(1'700'000'001)}, {"pad", json::value("?\?>>")}})))));
      jwtOut["urlUnpadded"] = expMs(pad(b64url(object({{"exp", json::value(1'700'000'002)}, {"x", json::value("ab")}}))));
      jwtOut["invalidChars"] = expMs(pad("%%%"));
      jwtOut["arrayPayload"] = expMs(jwt(json::value(json::array{object({{"exp", json::value(5)}})})));
      out["jwt"] = std::move(jwtOut);
    }

    {
      const auto emailOf = [](const json::value& payload) { return stringOrNull(auth::getCodexAccountEmail(jwt(payload))); };
      const auto withEmail = [](json::value email) { return object({{"email", std::move(email)}}); };
      json::object email;
      email["profileNotRecord"] = emailOf(object({{"https://api.openai.com/profile", json::value("x")}, {"email", json::value("top@example.com")}}));
      email["profileEmailInvalid"] =
        emailOf(object({{"https://api.openai.com/profile", object({{"email", json::value("nope")}})}, {"email", json::value("top@example.com")}}));
      email["exactly320"] = emailOf(withEmail(json::value(std::string(308, 'a') + "@example.com")));
      email["over320"] = emailOf(withEmail(json::value(std::string(309, 'a') + "@example.com")));
      email["del"] = emailOf(withEmail(json::value("a\x7f@example.com")));
      email["tab"] = emailOf(withEmail(json::value("\ta@example.com\t")));
      email["innerTab"] = emailOf(withEmail(json::value("a\tb@example.com")));
      email["unicode"] = emailOf(withEmail(json::value("\xc3\xa9l\xc3\xa8ve@example.com")));
      email["twoSegments"] = stringOrNull(auth::getCodexAccountEmail("a." + b64url(withEmail(json::value("two@example.com")))));
      email["numberEmail"] = emailOf(withEmail(json::value(5)));
      out["email"] = std::move(email);
    }

    {
      json::object userAgent;
      for (const std::string value :
           {"1.2.3.4", "1..2", "0.100.0-beta", "0.100.0", "0.100", "0.99.99", "10.0.0", "0x10.0", " 1.0 ", "1e2.0", "Infinity.1", "-0.200", "0.100.0.", "abc"})
        userAgent[value] = auth::codexUserAgent(value);
      out["userAgent"] = std::move(userAgent);
    }

    {
      const CodexAuthState::NativeOwner owner{.codex_home = "/home/x/.codex", .generation_hash = hash};
      const std::string later = jwt(object({{"exp", json::value(2'000'000'000)}}));
      const std::string earlier = jwt(object({{"exp", json::value(1'000'000'000)}}));
      const CodexAuthState prior{.access_token = later, .refresh_token = "r1", .account_id = "acct", .updated_at_ms = 1, .native_owner = owner};
      const CodexAuthPoolState priorPool{.accounts = {prior}, .updated_at_ms = 1};
      const auto upsert = [](const std::optional<CodexAuthPoolState>& pool, const CodexAuthState& next) {
        return outcome([&pool, &next] { return poolJson(auth::upsertCodexAuthAccount(pool, next)); });
      };
      CodexAuthState withoutOwner = prior;
      withoutOwner.native_owner = std::nullopt;
      CodexAuthState otherHome = prior;
      otherHome.access_token = later + "x";
      otherHome.native_owner->codex_home = "/other";
      const CodexAuthState noOwner{.access_token = later, .refresh_token = "r2", .account_id = "acct", .updated_at_ms = 2};
      const CodexAuthState older{.access_token = earlier, .refresh_token = "r2", .account_id = "acct", .updated_at_ms = 2, .native_owner = owner};
      const CodexAuthState newer{
        .access_token = jwt(object({{"exp", json::value(2'100'000'000)}})), .refresh_token = "r2", .account_id = "acct", .updated_at_ms = 2, .native_owner = owner};
      const CodexAuthState accessOnly{.access_token = "a", .refresh_token = "", .account_id = "acct", .updated_at_ms = 2};
      CodexAuthState accessOnlyExternal = accessOnly;
      accessOnlyExternal.native_owner = owner;
      accessOnlyExternal.native_owner->auth_mode = std::string("chatgptAuthTokens");
      accessOnlyExternal.native_owner->chatgpt_user_id = std::string("u");
      json::object upserts;
      upserts["sameTokensKeepsOwner"] = upsert(priorPool, withoutOwner);
      upserts["otherHome"] = upsert(priorPool, otherHome);
      upserts["noOwnerReplacement"] = upsert(priorPool, noOwner);
      upserts["olderToken"] = upsert(priorPool, older);
      upserts["newerToken"] = upsert(priorPool, newer);
      upserts["accessOnly"] = upsert(std::nullopt, accessOnly);
      upserts["accessOnlyExternal"] = upsert(std::nullopt, accessOnlyExternal);
      out["upsert"] = std::move(upserts);
    }

    {
      const auto stateWith = [](std::string accessToken, double updatedAtMs) {
        return CodexAuthState{.access_token = std::move(accessToken), .refresh_token = "r", .account_id = "a", .updated_at_ms = updatedAtMs};
      };
      json::object refresh;
      refresh["zeroExp"] = auth::needsRefresh(stateWith(jwt(object({{"exp", json::value(0)}})), NOW));
      refresh["boundary"] = auth::needsRefresh(stateWith(jwt(object({{"exp", rt::makeNumber((NOW + 120'000) / 1000)}})), NOW));
      refresh["justOutside"] = auth::needsRefresh(stateWith(jwt(object({{"exp", rt::makeNumber((NOW + 121'000) / 1000)}})), NOW));
      refresh["ageBoundary"] = auth::needsRefresh(stateWith("opaque", NOW - 420'000));
      refresh["ageOver"] = auth::needsRefresh(stateWith("opaque", NOW - 420'001));
      out["needsRefresh"] = std::move(refresh);
      json::object expired;
      expired["exactlyNow"] = auth::accessTokenExpired(stateWith(jwt(object({{"exp", rt::makeNumber(NOW / 1000)}})), NOW));
      expired["zeroExp"] = auth::accessTokenExpired(stateWith(jwt(object({{"exp", json::value(0)}})), NOW));
      out["expired"] = std::move(expired);
    }

    for (const auto& member : expected) EXPECT(same(expected, out, member.key()));
    EXPECT_EQ(out.size(), expected.size());
    rt::globalDateNow() = realNow;
  }
}
