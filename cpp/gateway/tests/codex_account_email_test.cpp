// Port of tests/codex-account-email.test.ts (through the src/codex/index.ts re-export of getCodexAccountEmail).
// Links: tests/codex_account_email_test.cpp src/codex/auth.cpp src/codex/capacity_routing.cpp src/codex/native_auth.cpp
//        src/codex/rate_limit_429.cpp src/codex/routing_mutations.cpp src/codex/routing_state.cpp src/codex/external_auth.cpp
//        src/codex/native_auth_transport.cpp src/provider/claude_code_accounts.cpp src/provider/claude_code_credentials.cpp
//        src/provider/health.cpp src/authority/native_codex_429.cpp src/authority/native_codex_capacity.cpp
//        src/authority/native_codex_contracts.cpp src/authority/native_codex_mutations.cpp src/bounded_response_body.cpp
//        src/config.cpp src/kv.cpp src/release.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
//        src/runtime/http_upgrade.cpp src/runtime/http.cpp src/runtime/websocket.cpp src/storage/key_compatibility.cpp src/storage/kv.cpp
// src/storage/native_kv.cpp src/storage/sqlite_readonly.cpp src/storage/value_compatibility.cpp
//        src/utils.cpp src/types.cpp src/authority/native_codex_evaluation.cpp
#include "uos/codex/index.hpp"
#include "uos/config.hpp"
#include "uos/rt/base64.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using uos::codex::index::getCodexAccountEmail;

  // Base64url drops the `=` padding. Trimming it with an unanchored `/=+$/` regex
  // is super-linear (measured ~1.6s per 20k-character run of `=`), so the
  // trailing run is removed with an explicit linear scan instead.
  std::string stripBase64Padding(const std::string& base64) {
    std::size_t end = base64.size();
    while (end > 0 && base64[end - 1] == '=') end -= 1;
    return base64.substr(0, end);
  }

  std::string encodeBase64Url(const json::value& value) {
    return rt::replaceAll(rt::replaceAll(stripBase64Padding(rt::btoa(rt::jsonStringify(value))), "+", "-"), "/", "_");
  }

  // Credential isolation: nothing here may reach this host's ~/.codex/auth.json.
  struct CredentialIsolation {
    rt::env::ScopedOverride environment{{"HOME", std::nullopt}, {"CODEX_HOME", std::nullopt}, {"CODEX_AUTH_JSON_B64", std::nullopt}};
    std::string savedSeed = uos::config::config.codexAuthJsonB64;
    CredentialIsolation() { uos::config::config.codexAuthJsonB64.clear(); }
    ~CredentialIsolation() { uos::config::config.codexAuthJsonB64 = savedSeed; }
  };

  std::string tokenWithPayload(const json::value& payload) {
    const json::value header = json::object{{"alg", "none"}};
    return encodeBase64Url(header) + "." + encodeBase64Url(payload) + ".signature";
  }
}

TEST("Codex account email is read from the provider profile claim") {
  const CredentialIsolation isolation;
  const json::value payload = json::object{{"https://api.openai.com/profile", json::object{{"email", "first@example.com"}}}};
  const std::string token = tokenWithPayload(payload);
  EXPECT_EQ(getCodexAccountEmail(token), std::optional<std::string>("first@example.com"));
}

TEST("Codex account email falls back to the top-level claim and rejects invalid tokens") {
  const CredentialIsolation isolation;
  EXPECT_EQ(getCodexAccountEmail(tokenWithPayload(json::object{{"email", "second@example.com"}})), std::optional<std::string>("second@example.com"));
  EXPECT_EQ(getCodexAccountEmail("not-a-jwt"), std::optional<std::string>());
  EXPECT_EQ(getCodexAccountEmail(tokenWithPayload(json::object{{"email", "not-an-email"}})), std::optional<std::string>());
}
