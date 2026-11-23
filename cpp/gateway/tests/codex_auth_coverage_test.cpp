// Port of tests/codex-auth-coverage.test.ts.
// Links: tests/codex_auth_coverage_test.cpp src/codex/auth.cpp src/codex/auth_refresh.cpp src/codex/native_auth.cpp src/codex/native_auth_transport.cpp
//        src/codex/external_auth.cpp src/codex/capacity_routing.cpp src/codex/rate_limit_429.cpp src/codex/routing_mutations.cpp src/codex/routing_state.cpp
//        src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp src/authority/native_codex_contracts.cpp
//        src/authority/native_codex_evaluation.cpp src/authority/native_codex_mutations.cpp src/bounded_response_body.cpp src/config.cpp src/kv.cpp
//        src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp src/provider/health.cpp src/release.cpp src/runtime/http_body.cpp
//        src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp src/runtime/http_upgrade.cpp src/runtime/http.cpp src/runtime/websocket.cpp
//        src/storage/key_compatibility.cpp src/storage/kv.cpp src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp
//        src/storage/value_compatibility.cpp src/utils.cpp src/types.cpp
//
// The bun suite runs with HOME, CODEX_HOME and CODEX_AUTH_JSON_B64 unset (one case asserts it), so no
// native credential home is consulted; every case here clears them (and config.codexAuthJsonB64) for its
// scope. The suite's ScriptedKv is ported below on rt::Kv and its fetch stub as an rt::GlobalFetchOverride.
// TS identity checks on plain objects (`warm.pool === first.pool`, `concurrentB.pool ===
// concurrentA.pool`) compare values; `entry.kv === kv` compares the shared KV handle. The
// awaitWithoutCancellingSharedWork cases carry CodexAuthState payloads instead of strings (the template
// is instantiated for CodexAuthState only); the shapes of the assertions are unchanged.
#include "uos/codex/auth.hpp"
#include "uos/codex/auth_refresh.hpp"

#include "uos/config.hpp"
#include "uos/kv.hpp"
#include "uos/provider/health.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/utils.hpp"

#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  namespace auth = uos::codex::auth;
  namespace auth_refresh = uos::codex::auth_refresh;
  using auth::CodexError;
  using uos::codex::routing_state::RoutingAccount;
  using uos::types::CodexAuthPoolState;
  using uos::types::CodexAuthState;

  /* ------------------------------------------------------------------ isolation */

  // The bun suite's environment: no credential home and no seeded auth.json.
  struct IsolatedEnvironment {
    rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    std::string savedAuthJson = uos::config::config.codexAuthJsonB64;
    bool savedDeploy = uos::config::config.isDeploy;
    IsolatedEnvironment() {
      uos::config::config.codexAuthJsonB64.clear();
      uos::config::config.isDeploy = false;
    }
    ~IsolatedEnvironment() {
      uos::config::config.codexAuthJsonB64 = savedAuthJson;
      uos::config::config.isDeploy = savedDeploy;
    }
    IsolatedEnvironment(const IsolatedEnvironment&) = delete;
    IsolatedEnvironment& operator=(const IsolatedEnvironment&) = delete;
  };

  /* ------------------------------------------------------------------ fixtures */

  std::string encodeBase64Url(const json::value& value) {
    std::string encoded = rt::replaceAll(rt::replaceAll(rt::btoa(rt::jsonStringify(value)), "+", "-"), "/", "_");
    while (!encoded.empty() && encoded.back() == '=') encoded.pop_back();
    return encoded;
  }

  /** A decodable JWT-shaped token; only the payload matters to the gateway. */
  std::string jwtLike(const json::value& payload) {
    json::object header;
    header["alg"] = "none";
    return encodeBase64Url(json::value(header)) + "." + encodeBase64Url(payload) + ".signature";
  }

  std::string accessTokenExpiringIn(double milliseconds) {
    json::object payload;
    payload["exp"] = rt::makeNumber(rt::Math::floor((rt::dateNow() + milliseconds) / 1'000));
    return jwtLike(json::value(payload));
  }

  CodexAuthState accountAuth(const std::string& accountId, const std::string& accessToken, std::optional<std::string> refreshToken = std::nullopt) {
    return CodexAuthState{.access_token = accessToken,
                          .refresh_token = refreshToken.value_or("refresh-" + accountId),
                          .account_id = accountId,
                          .updated_at_ms = rt::dateNow()};
  }

  CodexAuthPoolState poolOf(std::vector<CodexAuthState> accounts) { return CodexAuthPoolState{.accounts = std::move(accounts), .updated_at_ms = rt::dateNow()}; }

  RoutingAccount routingAccount(const CodexAuthState& authState) {
    RoutingAccount account;
    account.auth = authState;
    account.slot = 0;
    account.accountIdHash = "account-one-hash";
    account.credentialVersion = "credential-one";
    account.quotaHeadroom = std::nullopt;
    account.probeRequired = true;
    account.probeGeneration = 1;
    account.probeToken = "probe-token";
    return account;
  }

  json::value poolJson(const CodexAuthPoolState& pool) { return json::value_from(pool); }

  /* ------------------------------------------------------------------ fake KV */

  struct KvWrite {
    std::string type;  // "set" | "delete"
    rt::KvKey key;
  };

  std::string keyOf(const rt::KvKey& key) {
    std::string out = "[";
    for (std::size_t index = 0; index < key.size(); ++index) {
      if (index > 0) out += ",";
      const rt::KvKeyPart& part = key[index];
      out += part.isString() ? rt::jsonQuote(part.string()) : part.toString();
    }
    return out + "]";
  }

  // `write.key[1]`
  std::optional<std::string> keySegment(const rt::KvKey& key, std::size_t index) {
    if (index >= key.size() || !key[index].isString()) return std::nullopt;
    return key[index].string();
  }

  /**
   * In-memory KV with observable reads and failure injection. `openKv` is
   * not used by this fixture, so every KV-dependent path receives
   * this stub through `setKvForTest`.
   */
  class ScriptedKv final : public rt::Kv {
  public:
    rt::OrderedMap<std::string, rt::KvValue> values;
    rt::OrderedMap<std::string, double> versions;
    std::vector<std::string> gets;
    std::vector<KvWrite> writes;
    std::vector<KvWrite> commitAttempts;
    int commitFailures = 0;
    int commitThrows = 0;
    bool failLeaseDeleteCommit = false;
    bool rejectNextAuthGet = false;
    std::function<void(const rt::KvKey& key)> onGet;

    using rt::Kv::get;
    using rt::Kv::getMany;

    rt::Task<rt::KvEntryMaybe<>> get(rt::KvKey key, rt::KvGetOptions options = {}) override {
      EXPECT(options.consistency == rt::KvConsistencyLevel::strong);  // credential reads must be strong
      const std::string encoded = keyOf(key);
      gets.push_back(encoded);
      if (onGet) {
        const std::function<void(const rt::KvKey&)> observer = onGet;
        observer(key);
      }
      if (rejectNextAuthGet && encoded == keyOf(auth::CODEX_AUTH_POOL_KV_KEY)) {
        rejectNextAuthGet = false;
        throw rt::JsError("kv unavailable");
      }
      const rt::KvValue* value = values.get(encoded);
      rt::KvEntryMaybe<> entry;
      entry.key = key;
      entry.value = value != nullptr ? *value : rt::KvValue(nullptr);
      entry.versionstamp = versionstampOf(encoded);
      co_return entry;
    }

    rt::Task<std::vector<rt::KvEntryMaybe<>>> getMany(std::vector<rt::KvKey>, rt::KvGetOptions = {}) override {
      throw rt::TypeError("kv.getMany is not a function");
    }

    rt::Task<rt::KvCommitResult> set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions = {}) override {
      writes.push_back(KvWrite{"set", key});
      apply("set", key, value);
      rt::KvCommitResult result;
      result.ok = true;
      result.versionstamp = rt::padStart(rt::numberToString(versionOf(keyOf(key)).value_or(0)), 20, "0");
      co_return result;
    }

    rt::Task<void> delete_(rt::KvKey) override { throw rt::TypeError("kv.delete is not a function"); }
    std::unique_ptr<rt::KvListIterator> list(rt::KvListSelector, rt::KvListOptions = {}) override { throw rt::TypeError("kv.list is not a function"); }
    void close() override {}

    std::unique_ptr<rt::AtomicOperation> atomic() override { return std::make_unique<Chain>(*this); }

    std::optional<double> versionOf(const std::string& encoded) const {
      const double* version = versions.get(encoded);
      return version != nullptr ? std::optional<double>(*version) : std::nullopt;
    }

    std::optional<std::string> versionstampOf(const std::string& encoded) const {
      const std::optional<double> version = versionOf(encoded);
      if (!version) return std::nullopt;
      return rt::padStart(rt::numberToString(*version), 20, "0");
    }

    void apply(const std::string& type, const rt::KvKey& key, const rt::KvValue& value) {
      const std::string encoded = keyOf(key);
      if (type == "delete") {
        values.delete_(encoded);
        writes.push_back(KvWrite{"delete", key});
        versions.set(encoded, versionOf(encoded).value_or(0) + 1);
        return;
      }
      writes.push_back(KvWrite{"set", key});
      values.set(encoded, value);
      versions.set(encoded, versionOf(encoded).value_or(0) + 1);
    }

    // `kv.values.get(keyOf(key))` as JSON (null when absent).
    json::value stored(const rt::KvKey& key) const {
      const rt::KvValue* value = values.get(keyOf(key));
      return value != nullptr ? value->toJson() : json::value(nullptr);
    }

  private:
    class Chain final : public rt::AtomicOperation {
    public:
      explicit Chain(ScriptedKv& owner_) : owner(owner_) {}
      using rt::AtomicOperation::check;
      using rt::AtomicOperation::max;
      using rt::AtomicOperation::min;
      using rt::AtomicOperation::sum;
      rt::AtomicOperation& check(std::vector<rt::AtomicCheck> items) override {
        for (rt::AtomicCheck& item : items) checks.push_back(std::move(item));
        return *this;
      }
      rt::AtomicOperation& mutate(std::vector<rt::KvMutation>) override { throw rt::TypeError("chain.mutate is not a function"); }
      rt::AtomicOperation& sum(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("chain.sum is not a function"); }
      rt::AtomicOperation& min(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("chain.min is not a function"); }
      rt::AtomicOperation& max(rt::KvKey, const rt::BigInt&) override { throw rt::TypeError("chain.max is not a function"); }
      rt::AtomicOperation& set(rt::KvKey key, rt::KvValue value, rt::KvSetOptions = {}) override {
        pending.push_back(Write{"set", std::move(key), std::move(value)});
        return *this;
      }
      rt::AtomicOperation& delete_(rt::KvKey key) override {
        pending.push_back(Write{"delete", std::move(key), rt::KvValue()});
        return *this;
      }
      rt::Task<rt::KvCommitResult> commit() override {
        for (const Write& write : pending) owner.commitAttempts.push_back(KvWrite{write.type, write.key});
        rt::KvCommitResult result;
        if (owner.failLeaseDeleteCommit && pending.size() == 1 && pending[0].type == "delete" &&
            keySegment(pending[0].key, 1) == std::optional<std::string>("codex_auth_refresh")) {
          owner.failLeaseDeleteCommit = false;
          throw rt::JsError("release unavailable");
        }
        if (owner.commitThrows > 0) {
          owner.commitThrows -= 1;
          throw rt::JsError("commit unavailable");
        }
        if (owner.commitFailures > 0) {
          owner.commitFailures -= 1;
          co_return result;
        }
        for (const rt::AtomicCheck& check : checks) {
          if (owner.versionstampOf(keyOf(check.key)) != check.versionstamp) co_return result;
        }
        for (const Write& write : pending) owner.apply(write.type, write.key, write.value);
        result.ok = true;
        result.versionstamp = "00000000000000000009";
        co_return result;
      }

    private:
      struct Write {
        std::string type;
        rt::KvKey key;
        rt::KvValue value;
      };
      ScriptedKv& owner;
      std::vector<rt::AtomicCheck> checks;
      std::vector<Write> pending;
    };
  };

  void resetSingletons() {
    auth::resetCodexAuthCacheForTest();
    uos::provider::health::resetProviderHealthThrottleForTest();
  }

  /** Runs one isolated test with this stub installed and every codex singleton reset. */
  rt::Task<void> runScripted(rt::Task<void> (*run)(std::shared_ptr<ScriptedKv> kv)) {
    const IsolatedEnvironment isolation;
    auto kv = std::make_shared<ScriptedKv>();
    uos::kv::setKvForTest(kv);
    resetSingletons();
    rt::Reason failure;
    try {
      auto running = run(kv);
      co_await std::move(running);
    } catch (...) {
      failure = std::current_exception();
    }
    uos::kv::setKvForTest(nullptr);
    resetSingletons();
    if (failure) std::rethrow_exception(failure);
  }

  rt::Task<void> seed(std::shared_ptr<ScriptedKv> kv, rt::KvKey key, json::value value) {
    const rt::KvValue stored(value);
    auto setting = kv->set(std::move(key), stored);
    co_await std::move(setting);
  }

  /** Waits for one fire-and-forget health write without depending on wall-clock time. */
  rt::Task<bool> settleAsyncWork(std::function<bool()> predicate, int attempts = 50) {
    for (int attempt = 0; attempt < attempts; attempt += 1) {
      if (predicate()) co_return true;
      co_await rt::sleep(0);
    }
    co_return predicate();
  }

  std::size_t healthWrites(const ScriptedKv& kv) {
    std::size_t count = 0;
    for (const KvWrite& write : kv.writes)
      if (keySegment(write.key, 1) == std::optional<std::string>("provider_health")) count += 1;
    return count;
  }

  std::size_t poolReads(const ScriptedKv& kv) {
    std::size_t count = 0;
    for (const std::string& key : kv.gets)
      if (key == keyOf(auth::CODEX_AUTH_POOL_KV_KEY)) count += 1;
    return count;
  }

  /* --------------------------------------------------------------------- tests */

  TEST("codexUserAgent keeps the higher of the requested and pinned client versions") {
    const std::string pinned = "codex_cli_rs/" + std::string(auth::CODEX_CLIENT_VERSION) + " (ai.ubq.fi)";
    EXPECT_EQ(auth::codexUserAgent(std::string("0.99.0")), pinned);  // an older patch line keeps the pinned version
    EXPECT_EQ(auth::codexUserAgent(std::string("0.100.1")), std::string("codex_cli_rs/0.100.1 (ai.ubq.fi)"));
    EXPECT_EQ(auth::codexUserAgent(std::string("2.0")), std::string("codex_cli_rs/2.0 (ai.ubq.fi)"));  // a two-part version is accepted verbatim
    EXPECT_EQ(auth::codexUserAgent(std::string("  0.200.0  ")), std::string("codex_cli_rs/0.200.0 (ai.ubq.fi)"));  // surrounding whitespace is trimmed
    EXPECT_EQ(auth::codexUserAgent(std::nullopt), pinned);
    EXPECT_EQ(auth::codexUserAgent(std::string("")), pinned);  // an empty version never replaces the pinned one
    EXPECT_EQ(auth::codexUserAgent(std::string("1")), std::string("codex_cli_rs/1 (ai.ubq.fi)"));  // an unparsable version is reported verbatim
    EXPECT_EQ(auth::codexUserAgent(std::string("1.-2.3")), std::string("codex_cli_rs/1.-2.3 (ai.ubq.fi)"));  // a negative component is unparsable
  }

  TEST("parseCodexAuthFromAuthJson accepts only a complete token triple") {
    const json::value complete = rt::jsonParse(R"({"tokens":{"access_token":"a","refresh_token":"r","account_id":"id"}})");
    const std::optional<auth::ParseCodexAuthFromAuthJsonResult> parsed = auth::parseCodexAuthFromAuthJson(complete);
    EXPECT(parsed == std::optional<auth::ParseCodexAuthFromAuthJsonResult>(auth::ParseCodexAuthFromAuthJsonResult{.access_token = "a", .refresh_token = "r", .account_id = "id"}));
    EXPECT(!auth::parseCodexAuthFromAuthJson(rt::jsonParse(R"({"tokens":null})")));
    EXPECT(!auth::parseCodexAuthFromAuthJson(rt::jsonParse("{}")));
    EXPECT(!auth::parseCodexAuthFromAuthJson(rt::jsonParse(R"({"tokens":{"access_token":"a","refresh_token":"r"}})")));
    EXPECT(!auth::parseCodexAuthFromAuthJson(json::value(nullptr)));
  }

  TEST("parseCodexAuthPool rejects a pool that is not a bounded, unique, complete account list") {
    const CodexAuthPoolState valid = poolOf({accountAuth("one", "access-one")});
    EXPECT(auth::parseCodexAuthPool(poolJson(valid)) == std::optional<CodexAuthPoolState>(valid));
    json::object missingTimestamp;
    missingTimestamp["accounts"] = json::array{json::value_from(accountAuth("one", "a"))};
    EXPECT(!auth::parseCodexAuthPool(json::value(missingTimestamp)));  // a missing pool timestamp is rejected
    EXPECT(!auth::parseCodexAuthPool(rt::jsonParse(R"({"accounts":[7],"updated_at_ms":1})")));  // a non-object account is rejected
    EXPECT(!auth::parseCodexAuthPool(rt::jsonParse(R"({"accounts":[],"updated_at_ms":1})")));
    const auto poolValue = [](std::vector<CodexAuthState> accounts) {
      json::array items;
      for (const CodexAuthState& account : accounts) items.push_back(json::value_from(account));
      json::object pool;
      pool["accounts"] = std::move(items);
      pool["updated_at_ms"] = 1;
      return json::value(std::move(pool));
    };
    EXPECT(!auth::parseCodexAuthPool(poolValue({accountAuth("a", "x"), accountAuth("b", "y"), accountAuth("c", "z")})));
    EXPECT(!auth::parseCodexAuthPool(poolValue({accountAuth("same", "x"), accountAuth("same", "y")})));  // a duplicated account id is rejected
    EXPECT(!auth::parseCodexAuthPool(rt::jsonParse(R"({"accounts":[{"access_token":"a","refresh_token":"r","account_id":"one"}],"updated_at_ms":1})")));
  }

  TEST("upsertCodexAuthAccount replaces, appends and refuses to grow the pool") {
    const IsolatedEnvironment isolation;
    const CodexAuthState first = accountAuth("one", "access-one");
    const CodexAuthState second = accountAuth("two", "access-two");
    const std::optional<CodexAuthPoolState> replaced = auth::upsertCodexAuthAccount(poolOf({first, second}), accountAuth("one", "access-one-new"));
    ASSERT(replaced.has_value());
    EXPECT_EQ(replaced->accounts.size(), std::size_t{2});
    EXPECT_EQ(replaced->accounts[0].access_token, std::string("access-one-new"));
    EXPECT_EQ(replaced->accounts[1].account_id, std::string("two"));

    const std::optional<CodexAuthPoolState> appended = auth::upsertCodexAuthAccount(poolOf({first}), second);
    ASSERT(appended.has_value());
    ASSERT_EQ(appended->accounts.size(), std::size_t{2});
    EXPECT_EQ(appended->accounts[0].account_id, std::string("one"));
    EXPECT_EQ(appended->accounts[1].account_id, std::string("two"));

    EXPECT(!auth::upsertCodexAuthAccount(poolOf({first, second}), accountAuth("three", "access-three")));  // a full pool refuses a new account
    const std::optional<CodexAuthPoolState> seeded = auth::upsertCodexAuthAccount(std::nullopt, first);
    ASSERT(seeded.has_value());
    ASSERT_EQ(seeded->accounts.size(), std::size_t{1});
    EXPECT_EQ(seeded->accounts[0].account_id, std::string("one"));
  }

  TEST("JWT expiry parsing, refresh need and expiry checks agree on the same token") {
    const std::string expiring = accessTokenExpiringIn(60'000);
    const std::string healthy = accessTokenExpiringIn(10 * 60'000);
    EXPECT_EQ(auth::getJwtExpMs(std::string_view(expiring)), std::optional<double>(rt::Math::floor((rt::dateNow() + 60'000) / 1'000) * 1'000));
    EXPECT_EQ(auth::getJwtExpMs("a.b"), std::optional<double>());
    EXPECT_EQ(auth::getJwtExpMs("a..c"), std::optional<double>());  // an empty payload segment carries no claims
    json::object header;
    header["alg"] = "none";
    const std::string prefix = encodeBase64Url(json::value(header)) + ".";
    EXPECT_EQ(auth::getJwtExpMs(std::string_view(prefix + rt::btoa("not json") + ".sig")), std::optional<double>());  // an undecodable payload
    EXPECT_EQ(auth::getJwtExpMs(std::string_view(prefix + encodeBase64Url(json::array{1, 2}) + ".sig")), std::optional<double>());  // a non-object payload
    json::object soon;
    soon["exp"] = "soon";
    EXPECT_EQ(auth::getJwtExpMs(std::string_view(prefix + encodeBase64Url(json::value(soon)) + ".sig")), std::optional<double>());  // a non-numeric expiry
    EXPECT_EQ(auth::getJwtExpMs(json::value(7)), std::optional<double>());

    CodexAuthState expiringAuth = accountAuth("one", expiring);
    expiringAuth.updated_at_ms = rt::dateNow();
    EXPECT_EQ(auth::needsRefresh(expiringAuth), true);  // a token inside the refresh window is refreshed
    CodexAuthState healthyAuth = accountAuth("one", healthy);
    healthyAuth.updated_at_ms = rt::dateNow();
    EXPECT_EQ(auth::needsRefresh(healthyAuth), false);
    CodexAuthState opaque = accountAuth("one", "opaque-token");
    CodexAuthState aged = opaque;
    aged.updated_at_ms = rt::dateNow() - 8 * 60'000;
    EXPECT_EQ(auth::needsRefresh(aged), true);  // an opaque token falls back to its age
    CodexAuthState fresh = opaque;
    fresh.updated_at_ms = rt::dateNow();
    EXPECT_EQ(auth::needsRefresh(fresh), false);

    CodexAuthState expired = accountAuth("one", accessTokenExpiringIn(-1'000));
    expired.updated_at_ms = rt::dateNow();
    EXPECT_EQ(auth::accessTokenExpired(expired), true);
    EXPECT_EQ(auth::accessTokenExpired(healthyAuth), false);
    EXPECT_EQ(auth::accessTokenExpired(opaque), false);  // an opaque token has no provable expiry
  }

  TEST("getCodexAccountEmail reads only a usable provider email claim") {
    const auto email = [](const std::string& value) {
      json::object payload;
      payload["email"] = value;
      return jwtLike(json::value(payload));
    };
    json::object profile;
    profile["email"] = "dev@example.com";
    json::object profilePayload;
    profilePayload["https://api.openai.com/profile"] = profile;
    EXPECT_EQ(auth::getCodexAccountEmail(jwtLike(json::value(profilePayload))), std::optional<std::string>("dev@example.com"));
    EXPECT_EQ(auth::getCodexAccountEmail(email("  fallback@example.com  ")), std::optional<std::string>("fallback@example.com"));
    EXPECT_EQ(auth::getCodexAccountEmail(email("no-at-sign")), std::optional<std::string>());
    EXPECT_EQ(auth::getCodexAccountEmail(email(std::string("control") + '\x01' + "@example.com")), std::optional<std::string>());  // a control character
    EXPECT_EQ(auth::getCodexAccountEmail(email(std::string(320, 'a') + "@example.com")), std::optional<std::string>());  // an over-long address
    EXPECT_EQ(auth::getCodexAccountEmail(email("")), std::optional<std::string>());
    EXPECT_EQ(auth::getCodexAccountEmail(jwtLike(json::array{1, 2})), std::optional<std::string>());  // a non-object payload has no email claim
    EXPECT_EQ(auth::getCodexAccountEmail("a"), std::optional<std::string>());  // a token without a payload segment has no claims
  }

  TEST("codexAuthWarningForError maps only re-authentication failures to the dashboard warning") {
    const std::optional<std::string> warning = std::string(auth::CODEX_AUTH_REAUTH_WARNING);
    EXPECT_EQ(auth::codexAuthWarningForError(rt::makeReason(CodexError("invalid", "codex_auth_invalid", 503))), warning);
    EXPECT_EQ(auth::codexAuthWarningForError(rt::makeReason(CodexError("refresh failed", "codex_auth_refresh_failed", 503))), warning);
    EXPECT_EQ(auth::codexAuthWarningForError(rt::makeReason(CodexError("reused", "refresh_token_reused", 401))), warning);
    EXPECT_EQ(auth::codexAuthWarningForError(rt::makeReason(CodexError("missing", "codex_auth_missing", 503))), std::optional<std::string>());
    EXPECT_EQ(auth::codexAuthWarningForError(rt::makeReason(rt::JsError("plain"))), std::optional<std::string>());
    EXPECT_EQ(auth::codexAuthWarningForError(rt::thrown(json::value("not an error"))), std::optional<std::string>());
  }

  rt::Response textResponse(const std::string& body, double status, std::optional<rt::Headers> headers = std::nullopt) {
    rt::ResponseInit init;
    init.status = status;
    init.headers = std::move(headers);
    return rt::Response(rt::BodyInit(body), init);
  }

  ASYNC_TEST("withCodexWarnings merges, dedupes and only decorates a response that needs it") {
    const rt::Response plain = textResponse("body", 200);
    EXPECT(auth::withCodexWarnings(plain, {}) == plain);  // a warning-free response is returned untouched
    EXPECT_EQ(plain.headers().get("x-uos-warning"), std::optional<std::string>());

    const rt::Response alreadyWarned = textResponse("body", 200, rt::Headers{{"x-uos-warning", "first"}});
    const rt::Response mergeInput = alreadyWarned.clone();
    const rt::Response undecorated = auth::withCodexWarnings(alreadyWarned, {});
    EXPECT(!(undecorated == alreadyWarned));
    EXPECT_EQ(undecorated.headers().get("x-uos-warning"), std::optional<std::string>("first"));
    EXPECT_EQ(undecorated.status(), 200);
    auto reading = undecorated.text();
    const std::string text = co_await std::move(reading);
    EXPECT_EQ(text, std::string("body"));

    const rt::Response merged = auth::withCodexWarnings(mergeInput, {"second", "first"});
    EXPECT_EQ(merged.headers().get("x-uos-warning"), std::optional<std::string>("first, second"));  // an existing warning is never duplicated
    EXPECT_EQ(alreadyWarned.headers().get("x-uos-warning"), std::optional<std::string>("first"));  // the original response keeps its own headers

    const rt::Response authWarned = auth::withCodexAuthWarning(textResponse("body", 502), std::string(auth::CODEX_AUTH_REAUTH_WARNING));
    EXPECT_EQ(authWarned.headers().get("x-uos-warning"), std::optional<std::string>(std::string(auth::CODEX_AUTH_REAUTH_WARNING)));
    EXPECT_EQ(authWarned.status(), 502);
  }

  const auth::CodexResponseActiveTelemetry NO_TELEMETRY{std::nullopt, std::nullopt};

  ASYNC_TEST("response-local routing telemetry follows a decorated response and never explains a response that was not admitted") {
    const CodexAuthState authState = accountAuth("account-one", "access-one");
    const RoutingAccount probe = routingAccount(authState);
    const rt::Response response = textResponse("body", 200);
    EXPECT(auth::getCodexResponseActiveTelemetry(response) == NO_TELEMETRY);  // an unadmitted response has no telemetry

    const rt::Response undecided = textResponse("body", 200);
    RoutingAccount undecidedAccount = routingAccount(authState);
    undecidedAccount.activeGeneration = std::nullopt;
    auth::setCodexResponseActiveTelemetry(undecided, undecidedAccount);
    EXPECT(auth::getCodexResponseActiveTelemetry(undecided) == NO_TELEMETRY);

    RoutingAccount admitted = routingAccount(authState);
    admitted.activeGeneration = 7;
    admitted.activeTransitionReason = std::string("quota_exhausted");
    const auth::CodexResponseActiveTelemetry admittedTelemetry{7, std::string("quota_exhausted")};
    auth::setCodexResponseActiveTelemetry(response, admitted);
    auth::setCodexResponseAccountTelemetry(response, 1, "account-one");
    const rt::Response decorated = auth::withCodexWarnings(response, {"codex_quota_blocked"});
    EXPECT(!(decorated == response));
    EXPECT(auth::getCodexResponseActiveTelemetry(decorated) == admittedTelemetry);
    EXPECT_EQ(auth::getCodexResponseSlot(decorated), std::optional<double>(1));  // the admitted slot survives decoration
    auto cohorting = auth::getCodexResponseAccountCohortId(decorated);
    const std::optional<std::string> cohort = co_await std::move(cohorting);
    static const rt::Regex DIGEST("^[0-9a-f]{64}$", "");
    EXPECT(DIGEST.test(cohort.value_or("")));  // the account cohort is an opaque digest

    const rt::Response sameAccount = textResponse("other", 200);
    auth::setCodexResponseAccountTelemetry(sameAccount, 0, "account-one");
    auto sameCohorting = auth::getCodexResponseAccountCohortId(sameAccount);
    const std::optional<std::string> sameCohort = co_await std::move(sameCohorting);
    EXPECT_EQ(sameCohort, cohort);  // the same account yields the same cohort digest
    const rt::Response otherAccount = textResponse("other", 200);
    auth::setCodexResponseAccountTelemetry(otherAccount, 0, "account-two");
    auto otherCohorting = auth::getCodexResponseAccountCohortId(otherAccount);
    const std::optional<std::string> otherCohort = co_await std::move(otherCohorting);
    EXPECT(otherCohort != cohort);
    const rt::Response anonymous = textResponse("other", 200);
    auto anonymousCohorting = auth::getCodexResponseAccountCohortId(anonymous);
    const std::optional<std::string> anonymousCohort = co_await std::move(anonymousCohorting);
    EXPECT_EQ(anonymousCohort, std::optional<std::string>());

    const rt::Response target = textResponse("target", 200);
    auth::inheritCodexResponseActiveTelemetry(target, decorated);
    EXPECT(auth::getCodexResponseActiveTelemetry(target) == admittedTelemetry);
    const rt::Response untouched = textResponse("untouched", 200);
    auth::inheritCodexResponseActiveTelemetry(untouched, anonymous);
    EXPECT(auth::getCodexResponseActiveTelemetry(untouched) == NO_TELEMETRY);
    auth::inheritCodexResponseActiveTelemetry(untouched, decorated);
    EXPECT(auth::getCodexResponseActiveTelemetry(untouched) == admittedTelemetry);

    const rt::Response probeCarrier = textResponse("probe", 200);
    auth::setCodexResponseAccountTelemetry(probeCarrier, 1, "account-one");
    auth::codexProbeByResponse.set(probeCarrier, probe);
    const rt::Response probeDecorated = auth::withCodexWarnings(probeCarrier, {"codex_upstream_degraded"});
    const std::optional<RoutingAccount> carried = auth::getCodexRoutingProbe(probeDecorated);
    EXPECT(carried.has_value() && carried->probeToken == std::optional<std::string>("probe-token"));  // the recovery probe survives decoration
    EXPECT(auth::getCodexResponseActiveTelemetry(probeDecorated) == NO_TELEMETRY);
  }

  rt::Task<void> terminalOutcomeScenario(std::shared_ptr<ScriptedKv> kv) {
    const rt::Response completed = textResponse("ok", 200);
    auth::setCodexResponseAccountTelemetry(completed, 0, "account-one");
    auto completing = auth::markCodexResponseCompleted(completed);
    co_await std::move(completing);
    auto settling = settleAsyncWork([kv] { return healthWrites(*kv) > 0; });
    const bool recorded = co_await std::move(settling);
    EXPECT(recorded);  // a completed response records a provider-health success
    const std::size_t afterFirst = healthWrites(*kv);
    auto completingAgain = auth::markCodexResponseCompleted(completed);
    co_await std::move(completingAgain);
    auto releasing = auth::releaseCodexResponseProbe(completed);
    co_await std::move(releasing);
    EXPECT_EQ(healthWrites(*kv), afterFirst);  // a second terminal call on the same response records nothing new

    const rt::Response decorated = textResponse("ok", 200);
    auth::setCodexResponseAccountTelemetry(decorated, 0, "account-two");
    auto releasingDecorated = auth::releaseCodexResponseProbe(decorated);
    co_await std::move(releasingDecorated);
    const rt::Response afterDecoration = auth::withCodexWarnings(decorated, {"codex_some_warning"});
    auto completingDecorated = auth::markCodexResponseCompleted(afterDecoration);
    co_await std::move(completingDecorated);
    auto waiting = settleAsyncWork([] { return false; }, 5);
    co_await std::move(waiting);
    EXPECT_EQ(healthWrites(*kv), afterFirst);  // the terminal outcome survives decoration instead of being recorded twice

    const rt::Response failed = textResponse("upstream broke", 502);
    auth::setCodexResponseAccountTelemetry(failed, 0, "account-three");
    auto failing = auth::markCodexResponseUpstreamError(failed);
    co_await std::move(failing);
    auto failingAgain = auth::markCodexResponseUpstreamError(failed);
    co_await std::move(failingAgain);
    auto waitingAgain = settleAsyncWork([] { return false; }, 5);
    co_await std::move(waitingAgain);
    EXPECT_EQ(healthWrites(*kv), afterFirst);  // a non-2xx response records no upstream-error health transition

    const rt::Response degraded = textResponse("degraded", 200);
    auth::setCodexResponseAccountTelemetry(degraded, 0, "account-four");
    auto degrading = auth::markCodexResponseUpstreamError(degraded);
    co_await std::move(degrading);
    auto settlingDegraded = settleAsyncWork([kv, afterFirst] { return healthWrites(*kv) > afterFirst; });
    const bool degradedRecorded = co_await std::move(settlingDegraded);
    EXPECT(degradedRecorded);  // a trustworthy 2xx failure degrades provider health
  }

  ASYNC_TEST("a terminal response records its outcome exactly once, even after decoration") {
    auto running = runScripted(&terminalOutcomeScenario);
    co_await std::move(running);
  }

  rt::Task<auth::CodexAuthPoolEntry> poolEntry(bool forceKv = false, bool bypassInFlight = false) {
    auto reading = auth::getAuthPoolEntry(forceKv, bypassInFlight);
    auth::CodexAuthPoolEntry entry = co_await std::move(reading);
    co_return entry;
  }

  rt::Task<void> warmCacheScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthPoolState pool = poolOf({accountAuth("one", accessTokenExpiringIn(10 * 60'000))});
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(pool));
    co_await std::move(seeding);
    auto firstReading = poolEntry();
    const auth::CodexAuthPoolEntry first = co_await std::move(firstReading);
    EXPECT(first.pool == pool);
    EXPECT(first.kv.get() == kv.get());  // a stored pool keeps its KV binding so a refresh can persist
    const std::size_t readsAfterFirst = poolReads(*kv);

    auto warmReading = poolEntry();
    const auth::CodexAuthPoolEntry warm = co_await std::move(warmReading);
    EXPECT(warm.pool == first.pool);                // the warm cache serves the same pool object
    EXPECT_EQ(poolReads(*kv), readsAfterFirst);    // a warm read never touches KV

    auth::resetCodexAuthCacheForTest();
    uos::kv::setKvForTest(kv);
    std::vector<rt::Promise<auth::CodexAuthPoolEntry>> concurrent;
    concurrent.push_back(rt::Promise<auth::CodexAuthPoolEntry>(auth::getAuthPoolEntry(true)));
    concurrent.push_back(rt::Promise<auth::CodexAuthPoolEntry>(auth::getAuthPoolEntry(true)));
    rt::Promise<std::vector<auth::CodexAuthPoolEntry>> both = rt::all(std::move(concurrent));
    const std::vector<auth::CodexAuthPoolEntry> entries = co_await both.wait();
    EXPECT(entries[0].pool == pool);
    EXPECT(entries[1].pool == entries[0].pool);  // overlapping strong reads share one in-flight pool read
    EXPECT_EQ(poolReads(*kv), readsAfterFirst + 1);  // single-flight collapses overlapping reads into one KV read
  }

  ASYNC_TEST("getAuthPoolEntry serves the warm cache, coalesces concurrent reads and works without any KV binding") {
    auto running = runScripted(&warmCacheScenario);
    co_await std::move(running);
  }

  ASYNC_TEST("getAuthPoolEntry fails closed without a KV binding and still serves a cached pool") {
    {
      const IsolatedEnvironment isolation;
      uos::kv::setKvForTest(nullptr);
      auth::resetCodexAuthCacheForTest();
      const CodexAuthPoolState cached = poolOf({accountAuth("one", accessTokenExpiringIn(10 * 60'000))});
      auth::cacheCodexAuthPool(cached);
      auto reading = poolEntry(true);
      const auth::CodexAuthPoolEntry withoutKv = co_await std::move(reading);
      EXPECT(withoutKv.pool == cached);  // without a KV binding the cached pool is still served
      EXPECT(withoutKv.kv == nullptr);
      EXPECT(!withoutKv.entry.has_value());
      uos::kv::setKvForTest(nullptr);
      auth::resetCodexAuthCacheForTest();
    }
    {
      const IsolatedEnvironment isolation;
      uos::kv::setKvForTest(nullptr);
      auth::resetCodexAuthCacheForTest();
      // Without KV or a credential/home input there is no auth seed to fall back to:
      // the read fails closed instead of inventing credentials.
      EXPECT(!rt::env::get("CODEX_AUTH_JSON_B64").has_value());
      EXPECT(!rt::env::get("HOME").has_value());
      std::optional<CodexError> failure;
      try {
        auto reading = poolEntry(true);
        co_await std::move(reading);
      } catch (const CodexError& error) {
        failure = error;
      }
      EXPECT(failure.has_value() && failure->code == "codex_auth_invalid" && failure->status == 503);
      uos::kv::setKvForTest(nullptr);
      auth::resetCodexAuthCacheForTest();
    }
  }

  rt::Task<void> releaseProbeScenario(std::shared_ptr<ScriptedKv>) {
    const rt::Response response = textResponse("body", 200);
    auth::setCodexResponseAccountTelemetry(response, 0, "account-one");
    auth::codexProbeByResponse.set(response, routingAccount(accountAuth("account-one", "access-one")));
    auto releasing = auth::releaseCodexResponseProbe(response);
    co_await std::move(releasing);
    EXPECT(!auth::getCodexRoutingProbe(response).has_value());   // the released probe is detached from the response
    EXPECT_EQ(auth::codexProbeTransitionsInFlight.size(), std::size_t{0});  // no probe transition is left in flight
    auto releasingAgain = auth::releaseCodexResponseProbe(response);
    co_await std::move(releasingAgain);
    EXPECT(!auth::getCodexRoutingProbe(response).has_value());
  }

  ASYNC_TEST("releaseCodexResponseProbe detaches a probe once and leaves the transition set empty") {
    auto running = runScripted(&releaseProbeScenario);
    co_await std::move(running);
  }

  TEST("a codex error carries its code, status and cause for the fallback adapter") {
    const rt::Reason cause = rt::makeReason(rt::JsError("socket closed"));
    const CodexError error("refresh unreachable", "codex_auth_refresh_unreachable", 502, cause);
    EXPECT_EQ(error.name, std::string("CodexError"));
    EXPECT_EQ(error.code, std::string("codex_auth_refresh_unreachable"));
    EXPECT_EQ(error.status, 502.0);
    EXPECT(error.causeValue == cause);
    EXPECT(CodexError("plain", "codex_auth_missing", 503).causeValue == nullptr);
  }

  /* ----------------------------------------------------------- auth refresh */

  struct FetchCall {
    std::string url;
    std::optional<std::string> method;
    json::value body;
    rt::Headers headers;
  };

  // `Response | Error`
  using FetchResult = std::variant<rt::Response, rt::Reason>;
  using FetchHandler = std::function<FetchResult(const FetchCall& call)>;

  rt::Task<rt::Response> settleFetch(FetchResult result) {
    if (const rt::Reason* reason = std::get_if<rt::Reason>(&result)) std::rethrow_exception(*reason);
    co_return std::get<rt::Response>(result);
  }

  /** Replaces `fetch` for one test; the OAuth endpoint is never reached for real. */
  struct StubbedFetch {
    std::shared_ptr<std::vector<FetchCall>> calls = std::make_shared<std::vector<FetchCall>>();
    rt::GlobalFetchOverride scope;
    explicit StubbedFetch(FetchHandler handler) : scope(mock(calls, std::move(handler))) {}

    static rt::Fetch mock(std::shared_ptr<std::vector<FetchCall>> calls, FetchHandler handler) {
      return rt::Fetch([calls, handler](rt::RequestInfo input, rt::RequestInit init) -> rt::Task<rt::Response> {
        std::string url;
        if (const std::string* text = std::get_if<std::string>(&input)) url = *text;
        else if (const rt::URL* parsed = std::get_if<rt::URL>(&input)) url = parsed->href();
        else url = std::get<rt::Request>(input).url();
        json::value body = json::object{};
        if (init.body) {
          if (const std::string* text = std::get_if<std::string>(&*init.body)) body = rt::jsonParse(*text);
        }
        FetchCall call{url, init.method, body, rt::Headers(init.headers.value_or(rt::Headers()))};
        calls->push_back(call);
        return settleFetch(handler(call));
      });
    }
  };

  rt::Response jsonResponse(double status, const json::value& payload) {
    rt::ResponseInit init;
    init.status = status;
    init.headers = rt::Headers{{"Content-Type", "application/json"}};
    return rt::Response(rt::BodyInit(rt::jsonStringify(payload)), init);
  }

  FetchHandler respond(rt::Response response) {
    return [response](const FetchCall&) -> FetchResult { return response; };
  }

  FetchHandler respondJson(double status, const std::string& payload) {
    return [status, payload](const FetchCall&) -> FetchResult { return jsonResponse(status, rt::jsonParse(payload)); };
  }

  FetchHandler failWith(const std::string& message) {
    return [message](const FetchCall&) -> FetchResult { return rt::makeReason(rt::JsError(message)); };
  }

  template <class T> rt::Task<std::optional<CodexError>> failureOf(rt::Task<T> task) {
    std::optional<CodexError> failure;
    bool threw = false;
    try {
      co_await std::move(task);
    } catch (const CodexError& error) {
      failure = error;
      threw = true;
    } catch (...) {
      threw = true;
      EXPECT(false);  // expected a CodexError
    }
    if (!threw) EXPECT(false);  // expected the call to fail
    co_return failure;
  }

  rt::Task<auth::CodexAuthAccountEntry> currentEntry(std::string accountId) {
    auto reading = auth_refresh::getCurrentAccountEntry(std::move(accountId), true);
    auth::CodexAuthAccountEntry entry = co_await std::move(reading);
    co_return entry;
  }

  rt::Task<CodexAuthState> coordinated(auth::CodexAuthAccountEntry entry) {
    auto refreshing = auth_refresh::refreshAuthCoordinated(std::move(entry));
    CodexAuthState refreshed = co_await std::move(refreshing);
    co_return refreshed;
  }

  rt::Task<void> currentEntryScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthPoolState pool = poolOf({accountAuth("one", accessTokenExpiringIn(10 * 60'000))});
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(pool));
    co_await std::move(seeding);
    auto reading = currentEntry("one");
    const auth::CodexAuthAccountEntry entry = co_await std::move(reading);
    EXPECT_EQ(entry.auth.account_id, std::string("one"));
    EXPECT(entry.pool == pool);
    EXPECT(entry.kv.get() == kv.get());

    auto failing = failureOf(auth_refresh::getCurrentAccountEntry("gone", true));
    const std::optional<CodexError> missing = co_await std::move(failing);
    ASSERT(missing.has_value());
    EXPECT_EQ(missing->code, std::string("codex_auth_missing"));
    EXPECT_EQ(missing->status, 503.0);
  }

  ASYNC_TEST("getCurrentAccountEntry reports an account that is no longer configured") {
    auto running = runScripted(&currentEntryScenario);
    co_await std::move(running);
  }

  ASYNC_TEST("refreshAuthStateless sends one official refresh request and reports each provider rejection with its own code") {
    const IsolatedEnvironment isolation;
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    struct Case {
      std::string name;
      double status;
      std::string payload;
      std::string code;
      double expectedStatus;
    };
    const std::vector<Case> cases{
      {"a reused refresh token", 429, R"({"error":{"code":"refresh_token_reused"}})", "refresh_token_reused", 503},
      {"an exact invalid_grant code", 400, R"({"error":"invalid_grant"})", "codex_auth_refresh_failed", 401},
      {"a described expired grant", 403, R"({"error_description":"refresh token expired"})", "codex_auth_refresh_failed", 401},
      {"an undescribed 500", 500, R"({"error":{"message":"internal"}})", "codex_auth_refresh_failed", 503},
      {"an unrecognized status", 418, R"({"detail":"teapot"})", "codex_auth_refresh_failed", 503},
    };
    for (const Case& testCase : cases) {
      const StubbedFetch stub(respondJson(testCase.status, testCase.payload));
      auto failing = failureOf(auth_refresh::refreshAuthStateless(stale));
      const std::optional<CodexError> error = co_await std::move(failing);
      if (!error) continue;
      if (error->code != testCase.code || error->status != testCase.expectedStatus) {
        std::fprintf(stderr, "    case: %s\n", testCase.name.c_str());
        EXPECT_EQ(error->code, testCase.code);
        EXPECT_EQ(error->status, testCase.expectedStatus);
      }
    }

    {
      const StubbedFetch stub(respond(textResponse("<html>gateway error</html>", 502)));
      auto failing = failureOf(auth_refresh::refreshAuthStateless(stale));
      const std::optional<CodexError> error = co_await std::move(failing);
      ASSERT(error.has_value());
      EXPECT_EQ(error->code, std::string("codex_auth_refresh_failed"));
      EXPECT_EQ(error->status, 503.0);
      EXPECT_EQ(error->message, std::string("Codex auth refresh failed (status 502)."));  // an advisory body is ignored
    }

    {
      const StubbedFetch stub(respond(textResponse("not json", 200)));
      auto failing = failureOf(auth_refresh::refreshAuthStateless(stale));
      const std::optional<CodexError> error = co_await std::move(failing);
      ASSERT(error.has_value());
      EXPECT_EQ(error->code, std::string("codex_auth_refresh_failed"));
      EXPECT_EQ(error->status, 503.0);
      EXPECT(rt::includes(error->message, "missing access_token"));
    }

    {
      const StubbedFetch stub(failWith("network down"));
      auto failing = failureOf(auth_refresh::refreshAuthStateless(stale));
      const std::optional<CodexError> error = co_await std::move(failing);
      ASSERT(error.has_value());
      EXPECT_EQ(error->code, std::string("codex_auth_refresh_unreachable"));
      EXPECT_EQ(error->status, 502.0);
    }

    {
      const StubbedFetch stub(respondJson(200, R"({"access_token":"fresh-access","refresh_token":"fresh-refresh"})"));
      auto refreshing = auth_refresh::refreshAuthStateless(stale);
      const CodexAuthState refreshed = co_await std::move(refreshing);
      EXPECT_EQ(refreshed.access_token, std::string("fresh-access"));
      EXPECT_EQ(refreshed.refresh_token, std::string("fresh-refresh"));
      EXPECT_EQ(refreshed.account_id, std::string("one"));  // the account identity is never taken from the response
      EXPECT(refreshed.updated_at_ms > 0);
      ASSERT_EQ(stub.calls->size(), std::size_t{1});
      const FetchCall& call = stub.calls->at(0);
      EXPECT_EQ(call.url, std::string(auth::CODEX_REFRESH_TOKEN_URL));
      EXPECT_EQ(call.method, std::optional<std::string>("POST"));
      json::object expectedBody;
      expectedBody["client_id"] = auth::CODEX_REFRESH_CLIENT_ID;
      expectedBody["grant_type"] = "refresh_token";
      expectedBody["refresh_token"] = stale.refresh_token;
      expectedBody["scope"] = "openid profile email";
      EXPECT(rt::deepEqual(call.body, json::value(expectedBody)));
    }

    {
      const StubbedFetch stub(respondJson(200, R"({"access_token":"only-access"})"));
      auto refreshing = auth_refresh::refreshAuthStateless(stale);
      const CodexAuthState refreshed = co_await std::move(refreshing);
      EXPECT_EQ(refreshed.refresh_token, stale.refresh_token);  // a response without a rotated refresh token keeps the stored one
    }
  }

  rt::Task<void> concurrentRotationScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    const CodexAuthPoolState pool = poolOf({stale});
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(pool));
    co_await std::move(seeding);
    auto reading = currentEntry("one");
    const auth::CodexAuthAccountEntry entry = co_await std::move(reading);
    const CodexAuthState rotated = accountAuth("one", "rotated-access", "rotated-refresh");

    const StubbedFetch stub([rotated](const FetchCall&) -> FetchResult {
      // Another writer persists its rotation while this refresh is in flight.
      auth::cacheCodexAuthPool(poolOf({rotated}));
      return jsonResponse(200, rt::jsonParse(R"({"access_token":"fetch-access","refresh_token":"fetch-refresh"})"));
    });
    auto refreshing = coordinated(entry);
    const CodexAuthState refreshed = co_await std::move(refreshing);
    EXPECT_EQ(stub.calls->size(), std::size_t{1});
    EXPECT_EQ(refreshed.access_token, std::string("rotated-access"));  // a concurrent rotation wins over this request's own response
    EXPECT_EQ(refreshed.refresh_token, std::string("rotated-refresh"));
    const json::value stored = kv->stored(auth::CODEX_AUTH_POOL_KV_KEY);
    EXPECT_EQ(rt::getString(stored.at("accounts").at(0), "access_token"), std::optional<std::string_view>(stale.access_token));  // never overwritten
  }

  ASYNC_TEST("refreshAuthCoordinated adopts credentials another writer rotated during the refresh") {
    auto running = runScripted(&concurrentRotationScenario);
    co_await std::move(running);
  }

  rt::Task<void> leaseScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(poolOf({stale})));
    co_await std::move(seeding);
    auto reading = currentEntry("one");
    const auth::CodexAuthAccountEntry entry = co_await std::move(reading);

    const StubbedFetch stub(respondJson(200, R"({"access_token":"persisted-access","refresh_token":"persisted-refresh"})"));
    auto refreshing = coordinated(entry);
    const CodexAuthState refreshed = co_await std::move(refreshing);
    EXPECT_EQ(stub.calls->size(), std::size_t{1});
    EXPECT_EQ(refreshed.access_token, std::string("persisted-access"));
    const json::value stored = kv->stored(auth::CODEX_AUTH_POOL_KV_KEY);
    EXPECT_EQ(rt::getString(stored.at("accounts").at(0), "access_token"), std::optional<std::string_view>("persisted-access"));  // persisted with CAS
    EXPECT_EQ(rt::getString(stored.at("accounts").at(0), "refresh_token"), std::optional<std::string_view>("persisted-refresh"));

    bool leaseSet = false;
    bool leaseDeleted = false;
    for (const KvWrite& write : kv->writes) {
      if (keySegment(write.key, 1) != std::optional<std::string>("codex_auth_refresh")) continue;
      leaseSet = leaseSet || write.type == "set";
      leaseDeleted = leaseDeleted || write.type == "delete";
    }
    EXPECT(leaseSet);      // the refresh runs under a claimed lease
    EXPECT(leaseDeleted);  // the lease is released after the refresh
    EXPECT_EQ(auth::refreshesInFlight.size(), std::size_t{0});  // no refresh attempt is left in flight
  }

  ASYNC_TEST("refreshAuthCoordinated claims the cross-isolate lease, persists the rotated token and releases the lease") {
    auto running = runScripted(&leaseScenario);
    co_await std::move(running);
  }

  rt::Task<rt::KvKey> leaseKeyOf(std::string accountId) {
    auto hashing = uos::utils::sha256Hex(std::move(accountId));
    const std::string hash = co_await std::move(hashing);
    co_return rt::KvKey{auth::CODEX_AUTH_REFRESH_LEASE_PREFIX, hash};
  }

  json::value leaseRow(const std::string& owner, double leaseUntilMs) {
    json::object lease;
    lease["owner"] = owner;
    lease["lease_until_ms"] = rt::makeNumber(leaseUntilMs);
    return json::value(std::move(lease));
  }

  rt::Task<void> waiterAdoptsScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    const CodexAuthState rotated = accountAuth("one", "owner-rotated-access");
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(poolOf({stale})));
    co_await std::move(seeding);
    auto reading = currentEntry("one");
    const auth::CodexAuthAccountEntry entry = co_await std::move(reading);
    auto keying = leaseKeyOf("one");
    const rt::KvKey leaseKey = co_await std::move(keying);
    auto leasing = seed(kv, leaseKey, leaseRow("other-isolate", rt::dateNow() + 120));
    co_await std::move(leasing);
    // The owner persists its rotation while this isolate waits on the lease.
    ScriptedKv* raw = kv.get();
    kv->onGet = [raw, leaseKey, rotated](const rt::KvKey& key) {
      if (keyOf(key) == keyOf(leaseKey)) raw->values.set(keyOf(auth::CODEX_AUTH_POOL_KV_KEY), rt::KvValue(poolJson(poolOf({rotated}))));
    };

    const StubbedFetch stub(failWith("a waiter must never reach the auth server"));
    auto refreshing = coordinated(entry);
    const CodexAuthState refreshed = co_await std::move(refreshing);
    EXPECT_EQ(stub.calls->size(), std::size_t{0});  // the waiter adopts the owner's token instead of issuing OAuth
    EXPECT_EQ(refreshed.access_token, std::string("owner-rotated-access"));
    EXPECT_EQ(rt::getString(kv->stored(leaseKey), "owner"), std::optional<std::string_view>("other-isolate"));  // the waiter never steals an observed lease
    kv->onGet = nullptr;
  }

  ASYNC_TEST("a waiter adopts the lease owner's rotated credentials instead of refreshing again") {
    auto running = runScripted(&waiterAdoptsScenario);
    co_await std::move(running);
  }

  rt::Task<void> unclaimableLeaseScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(poolOf({stale})));
    co_await std::move(seeding);
    auto reading = currentEntry("one");
    const auth::CodexAuthAccountEntry entry = co_await std::move(reading);
    kv->commitFailures = 3;

    const StubbedFetch stub(respondJson(200, R"({"access_token":"unclaimed-access","refresh_token":"unclaimed-refresh"})"));
    auto refreshing = coordinated(entry);
    const CodexAuthState refreshed = co_await std::move(refreshing);
    EXPECT_EQ(stub.calls->size(), std::size_t{1});
    EXPECT_EQ(refreshed.access_token, std::string("unclaimed-access"));  // an unclaimable lease still refreshes, just without coordination
  }

  rt::Task<void> failedReleaseScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(poolOf({stale})));
    co_await std::move(seeding);
    auto reading = currentEntry("one");
    const auth::CodexAuthAccountEntry entry = co_await std::move(reading);
    kv->failLeaseDeleteCommit = true;

    const StubbedFetch stub(respondJson(200, R"({"access_token":"release-failed-access","refresh_token":"release-failed-refresh"})"));
    auto refreshing = coordinated(entry);
    const CodexAuthState refreshed = co_await std::move(refreshing);
    EXPECT_EQ(refreshed.access_token, std::string("release-failed-access"));  // a lease release failure never masks completed refresh work
    std::size_t releaseAttempts = 0;
    for (const KvWrite& write : kv->commitAttempts)
      if (keySegment(write.key, 1) == std::optional<std::string>("codex_auth_refresh") && write.type == "delete") releaseAttempts += 1;
    EXPECT_EQ(releaseAttempts, std::size_t{1});  // the release was attempted and its failure was swallowed
  }

  ASYNC_TEST("a lease that cannot be claimed falls open to a bounded direct refresh and a failed release stays harmless") {
    auto first = runScripted(&unclaimableLeaseScenario);
    co_await std::move(first);
    auto second = runScripted(&failedReleaseScenario);
    co_await std::move(second);
  }

  rt::Task<void> failOpenScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(poolOf({stale})));
    co_await std::move(seeding);
    auto reading = currentEntry("one");
    auth::CodexAuthAccountEntry entry = co_await std::move(reading);
    kv->rejectNextAuthGet = true;

    const StubbedFetch stub(respondJson(200, R"({"access_token":"fail-open-access","refresh_token":"fail-open-refresh"})"));
    entry.auth = stale;
    auto refreshing = coordinated(entry);
    const CodexAuthState refreshed = co_await std::move(refreshing);
    EXPECT_EQ(refreshed.access_token, std::string("fail-open-access"));  // an unavailable coordination read falls back to the supplied account
  }

  ASYNC_TEST("the coordinated refresh fails open when its own strong pool re-read is unavailable") {
    auto running = runScripted(&failOpenScenario);
    co_await std::move(running);
  }

  auth::CodexAuthAccountEntry detachedEntry(std::shared_ptr<rt::Kv> kv, const CodexAuthState& stale) {
    auth::CodexAuthAccountEntry entry;
    entry.kv = std::move(kv);
    entry.entry = std::nullopt;
    entry.pool = poolOf({stale});
    entry.auth = stale;
    return entry;
  }

  ASYNC_TEST("the coordinated refresh skips coordination entirely without a KV binding") {
    const IsolatedEnvironment isolation;
    uos::kv::setKvForTest(nullptr);
    auth::resetCodexAuthCacheForTest();
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    {
      const StubbedFetch stub(respondJson(200, R"({"access_token":"no-kv-access","refresh_token":"no-kv-refresh"})"));
      auto refreshing = coordinated(detachedEntry(nullptr, stale));
      const CodexAuthState refreshed = co_await std::move(refreshing);
      EXPECT_EQ(refreshed.access_token, std::string("no-kv-access"));
    }
    uos::kv::setKvForTest(nullptr);
    auth::resetCodexAuthCacheForTest();
  }

  rt::Task<void> lostCasScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(poolOf({stale})));
    co_await std::move(seeding);
    auto reading = currentEntry("one");
    const auth::CodexAuthAccountEntry entry = co_await std::move(reading);
    kv->commitFailures = 10;

    const StubbedFetch stub(respondJson(200, R"({"access_token":"never-persisted","refresh_token":"never-persisted-refresh"})"));
    auto failing = failureOf(auth_refresh::refreshAuthCoordinated(entry));
    const std::optional<CodexError> error = co_await std::move(failing);
    ASSERT(error.has_value());
    EXPECT_EQ(error->code, std::string("codex_auth_refresh_failed"));
    EXPECT_EQ(error->status, 503.0);
    EXPECT(rt::includes(error->message, "could not persist after concurrent updates"));
  }

  ASYNC_TEST("a refresh that loses every compare-and-set reports the conflict instead of claiming success") {
    auto running = runScripted(&lostCasScenario);
    co_await std::move(running);
  }

  rt::Task<void> disappearedPoolScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(poolOf({stale})));
    co_await std::move(seeding);

    const StubbedFetch stub(respondJson(200, R"({"access_token":"orphaned-access","refresh_token":"orphaned-refresh"})"));
    kv->values.delete_(keyOf(auth::CODEX_AUTH_POOL_KV_KEY));
    auto failing = failureOf(auth_refresh::refreshAuthCoordinated(detachedEntry(kv, stale)));
    const std::optional<CodexError> error = co_await std::move(failing);
    ASSERT(error.has_value());
    EXPECT_EQ(error->code, std::string("codex_auth_missing"));
    EXPECT_EQ(error->status, 503.0);
    EXPECT(rt::includes(error->message, "disappeared during refresh"));
  }

  ASYNC_TEST("a pool that disappears during a refresh is reported instead of silently replaced") {
    auto running = runScripted(&disappearedPoolScenario);
    co_await std::move(running);
  }

  rt::Task<void> kvRotationScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(poolOf({stale})));
    co_await std::move(seeding);
    const CodexAuthState rotated = accountAuth("one", "kv-rotated-access", "kv-rotated-refresh");

    const StubbedFetch stub(respondJson(200, R"({"access_token":"fetch-access","refresh_token":"fetch-refresh"})"));
    kv->values.set(keyOf(auth::CODEX_AUTH_POOL_KV_KEY), rt::KvValue(poolJson(poolOf({rotated}))));
    auto refreshing = coordinated(detachedEntry(kv, stale));
    const CodexAuthState refreshed = co_await std::move(refreshing);
    EXPECT_EQ(refreshed.access_token, std::string("kv-rotated-access"));  // the durable rotation is the authority
    const json::value stored = kv->stored(auth::CODEX_AUTH_POOL_KV_KEY);
    EXPECT_EQ(rt::getString(stored.at("accounts").at(0), "access_token"), std::optional<std::string_view>("kv-rotated-access"));
  }

  ASYNC_TEST("a pool rotated in KV during the refresh is adopted instead of the response's own token") {
    auto running = runScripted(&kvRotationScenario);
    co_await std::move(running);
  }

  rt::Task<void> healthyTokenScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState healthy = accountAuth("one", accessTokenExpiringIn(10 * 60'000));
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(poolOf({healthy})));
    co_await std::move(seeding);
    auto reading = currentEntry("one");
    const auth::CodexAuthAccountEntry entry = co_await std::move(reading);

    const StubbedFetch stub(failWith("no refresh should be needed"));
    auto validating = auth_refresh::getValidAuth(entry);
    const CodexAuthState unchanged = co_await std::move(validating);
    EXPECT_EQ(stub.calls->size(), std::size_t{0});  // a token outside the refresh window is used as is
    EXPECT_EQ(unchanged.access_token, healthy.access_token);
  }

  ASYNC_TEST("getValidAuth reuses a healthy token without touching the auth server") {
    auto running = runScripted(&healthyTokenScenario);
    co_await std::move(running);
  }

  rt::Task<void> coalescedRefreshScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(poolOf({stale})));
    co_await std::move(seeding);
    auto reading = currentEntry("one");
    const auth::CodexAuthAccountEntry entry = co_await std::move(reading);

    const StubbedFetch stub(respondJson(200, R"({"access_token":"shared-access","refresh_token":"shared-refresh"})"));
    std::vector<rt::Promise<CodexAuthState>> pending;
    pending.push_back(rt::Promise<CodexAuthState>(auth_refresh::getValidAuth(entry)));
    pending.push_back(rt::Promise<CodexAuthState>(auth_refresh::getValidAuth(entry)));
    rt::Promise<std::vector<CodexAuthState>> both = rt::all(std::move(pending));
    const std::vector<CodexAuthState> results = co_await both.wait();
    EXPECT_EQ(stub.calls->size(), std::size_t{1});  // two concurrent callers share one refresh
    EXPECT_EQ(results[0].access_token, std::string("shared-access"));
    EXPECT_EQ(results[1].access_token, std::string("shared-access"));
    EXPECT_EQ(auth::refreshesInFlight.size(), std::size_t{0});  // the shared refresh is removed once it settles
  }

  ASYNC_TEST("getValidAuth coalesces concurrent refreshes of one stale account into a single request") {
    auto running = runScripted(&coalescedRefreshScenario);
    co_await std::move(running);
  }

  ASYNC_TEST("version, telemetry and warning decoration keep the exact edge values") {
    const std::string pinned = "codex_cli_rs/" + std::string(auth::CODEX_CLIENT_VERSION) + " (ai.ubq.fi)";
    EXPECT_EQ(auth::codexUserAgent(std::string(auth::CODEX_CLIENT_VERSION)), pinned);  // an equal version keeps the requested spelling

    json::object header;
    header["alg"] = "none";
    const std::string invalidPayload = encodeBase64Url(json::value(header)) + "." + rt::btoa("not json") + ".signature";
    EXPECT_EQ(auth::getCodexAccountEmail(invalidPayload), std::optional<std::string>());  // an undecodable payload yields no email claim

    const rt::Response explicitNullReason = textResponse("body", 200);
    RoutingAccount generationOnly = routingAccount(accountAuth("one", "a"));
    generationOnly.activeGeneration = 7;
    auth::setCodexResponseActiveTelemetry(explicitNullReason, generationOnly);
    EXPECT(auth::getCodexResponseActiveTelemetry(explicitNullReason) == (auth::CodexResponseActiveTelemetry{7, std::nullopt}));

    const rt::Response routed = textResponse("body", 429);
    auth::codexRoutingErrors.set(routed, "codex_quota_blocked");
    const rt::Response decorated = auth::withCodexWarnings(routed, {"codex_quota_blocked"});
    EXPECT_EQ(auth::getCodexRoutingError(decorated), std::optional<std::string>("codex_quota_blocked"));  // survives decoration
    EXPECT_EQ(auth::getCodexRoutingError(routed), std::optional<std::string>("codex_quota_blocked"));

    const rt::Response authWarned = auth::withCodexAuthWarning(textResponse("body", 502), std::string(auth::CODEX_AUTH_REAUTH_WARNING));
    const rt::Response bothWarnings = auth::withCodexWarnings(authWarned, {"codex_upstream_degraded"});
    EXPECT_EQ(bothWarnings.headers().get("x-uos-warning"), std::optional<std::string>(std::string(auth::CODEX_AUTH_REAUTH_WARNING) + ", codex_upstream_degraded"));
    EXPECT_EQ(auth::getCodexRoutingError(bothWarnings), std::optional<std::string>());  // never invents a routing error
    const rt::Response cohortless = textResponse("body", 502);
    auto cohorting = auth::getCodexResponseAccountCohortId(cohortless);
    const std::optional<std::string> cohort = co_await std::move(cohorting);
    EXPECT_EQ(cohort, std::optional<std::string>());
  }

  rt::Task<void> registeredProbeScenario(std::shared_ptr<ScriptedKv>) {
    const rt::Response completed = textResponse("body", 200);
    auth::setCodexResponseAccountTelemetry(completed, 0, "account-one");
    auth::codexProbeByResponse.set(completed, routingAccount(accountAuth("account-one", "access-one")));
    auto completing = auth::markCodexResponseCompleted(completed);
    co_await std::move(completing);
    EXPECT(!auth::getCodexRoutingProbe(completed).has_value());  // a completed response consumes its recovery probe
    EXPECT_EQ(auth::codexProbeTransitionsInFlight.size(), std::size_t{0});

    const rt::Response degraded = textResponse("body", 200);
    auth::setCodexResponseAccountTelemetry(degraded, 1, "account-two");
    RoutingAccount secondSlot = routingAccount(accountAuth("account-two", "access-two"));
    secondSlot.slot = 1;
    auth::codexProbeByResponse.set(degraded, secondSlot);
    auto degrading = auth::markCodexResponseUpstreamError(degraded);
    co_await std::move(degrading);
    EXPECT(!auth::getCodexRoutingProbe(degraded).has_value());  // a degraded response also consumes its recovery probe
    EXPECT_EQ(auth::codexProbeTransitionsInFlight.size(), std::size_t{0});
  }

  ASYNC_TEST("a registered recovery probe is transitioned on both a completed and a degraded response") {
    auto running = runScripted(&registeredProbeScenario);
    co_await std::move(running);
  }

  rt::Task<void> generationRaceScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthPoolState stored = poolOf({accountAuth("one", accessTokenExpiringIn(10 * 60'000))});
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(stored));
    co_await std::move(seeding);
    const CodexAuthPoolState winner = poolOf({accountAuth("one", "winner-access")});
    auto rotated = std::make_shared<bool>(false);
    kv->onGet = [rotated, winner](const rt::KvKey& key) {
      if (!*rotated && keyOf(key) == keyOf(auth::CODEX_AUTH_POOL_KV_KEY)) {
        *rotated = true;
        auth::cacheCodexAuthPool(winner);
      }
    };

    auto reading = poolEntry(true);
    const auth::CodexAuthPoolEntry entry = co_await std::move(reading);
    EXPECT(entry.pool == winner);  // the concurrent writer's pool wins over the row this read started from
    EXPECT(entry.kv == nullptr);
    EXPECT(!entry.entry.has_value());

    // A banked-reset retry bypasses the warm cache and performs its own strong read.
    auth::resetCodexAuthCacheForTest();
    uos::kv::setKvForTest(kv);
    auto bypassing = poolEntry(false, true);
    const auth::CodexAuthPoolEntry bypassed = co_await std::move(bypassing);
    EXPECT(bypassed.pool == stored);  // a banked-reset retry reads the durable row instead of inheriting the cache
    EXPECT(bypassed.kv.get() == kv.get());
    kv->onGet = nullptr;
  }

  ASYNC_TEST("an auth-pool read that loses its generation race serves the concurrent cache instead of the stale row") {
    auto running = runScripted(&generationRaceScenario);
    co_await std::move(running);
  }

  rt::Task<void> unreachableScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(poolOf({stale})));
    co_await std::move(seeding);
    auto reading = currentEntry("one");
    const auth::CodexAuthAccountEntry entry = co_await std::move(reading);
    const StubbedFetch stub(failWith("auth server unreachable"));
    auto failing = failureOf(auth_refresh::refreshAuthCoordinated(entry));
    const std::optional<CodexError> unreachable = co_await std::move(failing);
    ASSERT(unreachable.has_value());
    EXPECT_EQ(unreachable->code, std::string("codex_auth_refresh_unreachable"));
    EXPECT_EQ(unreachable->status, 502.0);
  }

  rt::Task<void> rejectedScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(poolOf({stale})));
    co_await std::move(seeding);
    auto reading = currentEntry("one");
    const auth::CodexAuthAccountEntry entry = co_await std::move(reading);
    const StubbedFetch stub(respondJson(401, R"({"error":{"code":"invalid_grant"}})"));
    auto failing = failureOf(auth_refresh::refreshAuthCoordinated(entry));
    const std::optional<CodexError> rejected = co_await std::move(failing);
    ASSERT(rejected.has_value());
    EXPECT_EQ(rejected->code, std::string("codex_auth_refresh_failed"));
    EXPECT_EQ(rejected->status, 401.0);
  }

  rt::Task<void> emptyResponseScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(poolOf({stale})));
    co_await std::move(seeding);
    auto reading = currentEntry("one");
    const auth::CodexAuthAccountEntry entry = co_await std::move(reading);
    const StubbedFetch stub(respondJson(200, R"({"refresh_token":"only-refresh"})"));
    auto failing = failureOf(auth_refresh::refreshAuthCoordinated(entry));
    const std::optional<CodexError> empty = co_await std::move(failing);
    ASSERT(empty.has_value());
    EXPECT_EQ(empty->code, std::string("codex_auth_refresh_failed"));
    EXPECT(rt::includes(empty->message, "missing access_token"));
  }

  ASYNC_TEST("a coordinated refresh reports an unreachable auth server and a rejected refresh token") {
    auto first = runScripted(&unreachableScenario);
    co_await std::move(first);
    auto second = runScripted(&rejectedScenario);
    co_await std::move(second);
    auto third = runScripted(&emptyResponseScenario);
    co_await std::move(third);
  }

  rt::Task<void> stolenLeaseScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(poolOf({stale})));
    co_await std::move(seeding);
    auto reading = currentEntry("one");
    const auth::CodexAuthAccountEntry entry = co_await std::move(reading);
    auto keying = leaseKeyOf("one");
    const rt::KvKey leaseKey = co_await std::move(keying);
    auto leaseReads = std::make_shared<int>(0);
    ScriptedKv* raw = kv.get();
    kv->onGet = [raw, leaseKey, leaseReads](const rt::KvKey& key) {
      if (keyOf(key) != keyOf(leaseKey)) return;
      *leaseReads += 1;
      // Another isolate replaces the lease between the claim and the release.
      if (*leaseReads == 2) raw->values.set(keyOf(leaseKey), rt::KvValue(leaseRow("other-isolate", rt::dateNow() + 30'000)));
    };

    const StubbedFetch stub(respondJson(200, R"({"access_token":"released-access","refresh_token":"released-refresh"})"));
    auto refreshing = coordinated(entry);
    const CodexAuthState refreshed = co_await std::move(refreshing);
    EXPECT_EQ(refreshed.access_token, std::string("released-access"));
    EXPECT_EQ(rt::getString(kv->stored(leaseKey), "owner"), std::optional<std::string_view>("other-isolate"));  // a lease no longer owned is never deleted
    std::size_t deletes = 0;
    for (const KvWrite& write : kv->commitAttempts)
      if (write.type == "delete") deletes += 1;
    EXPECT_EQ(deletes, std::size_t{0});
    kv->onGet = nullptr;
  }

  ASYNC_TEST("a lease stolen while the refresh ran keeps its new owner and the local release stays silent") {
    auto running = runScripted(&stolenLeaseScenario);
    co_await std::move(running);
  }

  rt::Task<void> handoffScenario(std::shared_ptr<ScriptedKv> kv) {
    const CodexAuthState stale = accountAuth("one", accessTokenExpiringIn(-60'000));
    const CodexAuthState rotated = accountAuth("one", "handoff-access", "handoff-refresh");
    auto seeding = seed(kv, auth::CODEX_AUTH_POOL_KV_KEY, poolJson(poolOf({stale})));
    co_await std::move(seeding);
    auto reading = currentEntry("one");
    const auth::CodexAuthAccountEntry entry = co_await std::move(reading);
    auto keying = leaseKeyOf("one");
    const rt::KvKey leaseKey = co_await std::move(keying);
    auto leasing = seed(kv, leaseKey, leaseRow("other-isolate", rt::dateNow() + 30));
    co_await std::move(leasing);

    auto leaseReads = std::make_shared<int>(0);
    auto authReads = std::make_shared<int>(0);
    ScriptedKv* raw = kv.get();
    kv->onGet = [raw, leaseKey, rotated, leaseReads, authReads](const rt::KvKey& key) {
      if (keyOf(key) == keyOf(leaseKey)) {
        *leaseReads += 1;
        // The observation window itself is unavailable once.
        if (*leaseReads == 1) raw->rejectNextAuthGet = true;
        return;
      }
      if (keyOf(key) == keyOf(auth::CODEX_AUTH_POOL_KV_KEY)) {
        *authReads += 1;
        // The owner publishes its rotation exactly at the lease handoff.
        if (*authReads == 2) raw->values.set(keyOf(auth::CODEX_AUTH_POOL_KV_KEY), rt::KvValue(poolJson(poolOf({rotated}))));
      }
    };

    const StubbedFetch stub(failWith("a waiter must not reach the auth server"));
    auto refreshing = coordinated(entry);
    const CodexAuthState refreshed = co_await std::move(refreshing);
    EXPECT_EQ(stub.calls->size(), std::size_t{0});  // the handed-off credentials are adopted without a second OAuth call
    EXPECT_EQ(refreshed.access_token, std::string("handoff-access"));
    EXPECT_EQ(refreshed.refresh_token, std::string("handoff-refresh"));
    kv->onGet = nullptr;
  }

  ASYNC_TEST("a waiter survives an unavailable coordination read and adopts the credentials published at the handoff") {
    auto running = runScripted(&handoffScenario);
    co_await std::move(running);
  }

  CodexAuthState payload(const std::string& value) { return CodexAuthState{.access_token = value, .refresh_token = "", .account_id = "payload", .updated_at_ms = 0}; }

  ASYNC_TEST("abort reasons are preserved as Errors for both promise rejections and thrown values") {
    const rt::Reason original = rt::makeReason(rt::JsError("client went away"));
    EXPECT(auth_refresh::abortReasonAsError(original) == original);
    const rt::Reason undefinedReason = auth_refresh::abortReasonAsError(nullptr);
    EXPECT_EQ(rt::errorName(undefinedReason), std::optional<std::string>("AbortError"));
    EXPECT_EQ(rt::errorMessage(undefinedReason), std::optional<std::string>("The request was aborted."));
    json::object causeObject;
    causeObject["code"] = "transport";
    const rt::Reason cause = rt::thrown(json::value(causeObject));
    const rt::Reason wrapped = auth_refresh::abortReasonAsError(cause);
    EXPECT_EQ(rt::errorMessage(wrapped), std::optional<std::string>("The request was aborted."));
    bool carriesCause = false;
    bool isDomException = false;
    try {
      std::rethrow_exception(wrapped);
    } catch (const rt::DOMException&) {
      isDomException = true;
    } catch (const rt::JsError&) {
      carriesCause = rt::errorCause(wrapped) == std::optional<rt::Reason>(cause);
    } catch (...) {
    }
    EXPECT(carriesCause);
    EXPECT(!isDomException);  // a non-Error reason becomes a plain Error carrying its cause

    auto waitingDone = auth_refresh::awaitWithoutCancellingSharedWork(rt::Promise<CodexAuthState>::resolve(payload("done")), std::nullopt);
    const CodexAuthState completed = co_await std::move(waitingDone);
    EXPECT_EQ(completed.access_token, std::string("done"));  // a wait without a client signal never rewrites the result

    const rt::Reason reason = rt::makeReason(rt::JsError("stopped"));
    const rt::AbortController controller;
    controller.abort(reason);
    std::optional<std::string> stopped;
    try {
      auto waitingLate = auth_refresh::awaitWithoutCancellingSharedWork(rt::Promise<CodexAuthState>::resolve(payload("late")), controller.signal());
      co_await std::move(waitingLate);
    } catch (...) {
      stopped = rt::reasonToString(std::current_exception());
    }
    EXPECT(stopped.has_value() && rt::includes(*stopped, "stopped"));

    const rt::AbortController pending;
    rt::Promise<CodexAuthState> sharedWork = rt::newPromise<CodexAuthState>([](auto resolve, auto) {
      rt::setTimeout([resolve] { resolve(payload("shared result")); }, 20);
    });
    rt::Promise<CodexAuthState> waiting(auth_refresh::awaitWithoutCancellingSharedWork(sharedWork, pending.signal()));
    pending.abort(rt::makeReason(rt::JsError("client closed")));
    std::optional<std::string> closed;
    try {
      co_await waiting.wait();
    } catch (...) {
      closed = rt::reasonToString(std::current_exception());
    }
    EXPECT(closed.has_value() && rt::includes(*closed, "client closed"));
    const CodexAuthState shared = co_await sharedWork.wait();
    EXPECT_EQ(shared.access_token, std::string("shared result"));  // the aborted waiter never cancels the shared work it was observing
  }
}
