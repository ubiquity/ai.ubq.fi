#pragma once
// Port of src/auth/passport.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::auth::passport {
  struct PassportAuthMethod {
    static constexpr std::string_view kind = "passport_session";
    // `Readonly<{ sub; handle; is_admin; scopes; auth; exp: number | null; jti: string | null; agent_token_id: string | null }>`
    struct User {
      std::string sub;
      std::string handle;
      bool is_admin;
      std::vector<std::string> scopes;
      std::string auth;
      std::optional<double> exp;
      std::optional<std::string> jti;
      std::optional<std::string> agent_token_id;
      friend bool operator==(const User&, const User&) = default;
    };
    std::string principal;
    User user;
    friend bool operator==(const PassportAuthMethod&, const PassportAuthMethod&) = default;
  };

  // value_from writes the members in declaration order (`kind` first); value_to is strict.
  void tag_invoke(const json::value_from_tag&, json::value& out, const PassportAuthMethod::User& value);
  PassportAuthMethod::User tag_invoke(const json::value_to_tag<PassportAuthMethod::User>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PassportAuthMethod& value);
  PassportAuthMethod tag_invoke(const json::value_to_tag<PassportAuthMethod>&, const json::value& value);

  // `PassportResult`: `{ ok: true; token: null; method } | { ok: false; response }`.
  struct PassportResultOk {
    static constexpr bool ok = true;
    std::nullptr_t token = nullptr;
    PassportAuthMethod method;
  };
  struct PassportResultNotOk {
    static constexpr bool ok = false;
    rt::Response response;
  };
  using PassportResult = std::variant<PassportResultOk, PassportResultNotOk>;

  // `PassportAdminResult`: `{ ok: true; token: string; method; is_super_admin: false } | { ok: false; response }`.
  struct PassportAdminResultOk {
    static constexpr bool ok = true;
    std::string token;
    PassportAuthMethod method;
    static constexpr bool is_super_admin = false;
  };
  using PassportAdminResultNotOk = PassportResultNotOk;
  using PassportAdminResult = std::variant<PassportAdminResultOk, PassportAdminResultNotOk>;

  void resetPassportCacheForTest();

  /** Renew browser storage only after the existing auth-report route succeeds. */
  rt::Response renewPassportSessionCookie(const rt::Request& req, rt::Response response);

  // std::nullopt is null.
  [[nodiscard]] rt::Task<std::optional<PassportResult>> authenticatePassportSession(rt::Request req);

  // std::nullopt is null.
  [[nodiscard]] rt::Task<std::optional<PassportAdminResult>> authenticatePassportAdmin(rt::Request req);

  /** Browser Passport routes share the exact registered production client. */
  // std::nullopt is null.
  [[nodiscard]] rt::Task<std::optional<rt::Response>> handlePassportRoute(rt::Request req, std::string path);
}
