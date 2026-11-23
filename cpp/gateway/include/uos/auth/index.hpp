#pragma once
// Port of src/auth/index.ts.
//
// The TS module names its result unions only through `Awaited<ReturnType<...>>`; they are named here
// after the TS type aliases (`ClientAuthMethod`, `AuthenticateClientResult`, `AdminAuthMethod`,
// `AdminAuthResult`). Every `AdminAuthMethod` alternative is also a `ClientAuthMethod` alternative
// (the same struct), so `AdminAuthMethod | ClientAuthMethod` is `ClientAuthMethod`.
#include "uos/authority/native_contracts.hpp"
#include "uos/auth/passport.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace uos::auth::index {
  using passport::PassportAuthMethod;

  struct ClientAuthMethodDisabled {
    static constexpr std::string_view kind = "disabled";
    friend bool operator==(const ClientAuthMethodDisabled&, const ClientAuthMethodDisabled&) = default;
  };
  struct ClientAuthMethodGithubToken {
    static constexpr std::string_view kind = "github_token";
    std::string owner;
    std::string repo;
    std::string state_id;
    std::string limit_scope;  // "org" | "repo"
    friend bool operator==(const ClientAuthMethodGithubToken&, const ClientAuthMethodGithubToken&) = default;
  };
  struct ClientAuthMethodAuthTokensAllowlist {
    static constexpr std::string_view kind = "auth_tokens_allowlist";
    friend bool operator==(const ClientAuthMethodAuthTokensAllowlist&, const ClientAuthMethodAuthTokensAllowlist&) = default;
  };
  struct ClientAuthMethodKvApiKey {
    static constexpr std::string_view kind = "kv_api_key";
    std::string key_id;
    authority::native_contracts::ApiKeyPolicy policy;
    friend bool operator==(const ClientAuthMethodKvApiKey&, const ClientAuthMethodKvApiKey&) = default;
  };
  struct ClientAuthMethodAdminAllowlist {
    static constexpr std::string_view kind = "admin_allowlist";
    friend bool operator==(const ClientAuthMethodAdminAllowlist&, const ClientAuthMethodAdminAllowlist&) = default;
  };
  struct ClientAuthMethodDenoDeployToken {
    static constexpr std::string_view kind = "deno_deploy_token";
    friend bool operator==(const ClientAuthMethodDenoDeployToken&, const ClientAuthMethodDenoDeployToken&) = default;
  };
  struct ClientAuthMethodPasskeySession {
    static constexpr std::string_view kind = "passkey_session";
    std::string user_id;
    std::string handle;
    bool is_admin;
    double credential_count;
    friend bool operator==(const ClientAuthMethodPasskeySession&, const ClientAuthMethodPasskeySession&) = default;
  };

  using ClientAuthMethod = std::variant<PassportAuthMethod, ClientAuthMethodDisabled, ClientAuthMethodGithubToken, ClientAuthMethodAuthTokensAllowlist,
                                        ClientAuthMethodKvApiKey, ClientAuthMethodAdminAllowlist, ClientAuthMethodDenoDeployToken,
                                        ClientAuthMethodPasskeySession>;

  // `{ ok: true; token: string | null; method: ClientAuthMethod } | { ok: false; response: Response }`
  struct AuthenticateClientResultOk {
    static constexpr bool ok = true;
    std::optional<std::string> token;
    ClientAuthMethod method;
  };
  struct AuthenticateClientResultNotOk {
    static constexpr bool ok = false;
    rt::Response response;
  };
  using AuthenticateClientResult = std::variant<AuthenticateClientResultOk, AuthenticateClientResultNotOk>;

  using AdminAuthMethodDisabled = ClientAuthMethodDisabled;
  using AdminAuthMethodAdminAllowlist = ClientAuthMethodAdminAllowlist;
  using AdminAuthMethodDenoDeployToken = ClientAuthMethodDenoDeployToken;
  using AdminAuthMethodPasskeySession = ClientAuthMethodPasskeySession;
  using AdminAuthMethod =
    std::variant<PassportAuthMethod, AdminAuthMethodDisabled, AdminAuthMethodAdminAllowlist, AdminAuthMethodDenoDeployToken, AdminAuthMethodPasskeySession>;

  // `{ ok: true; token: string; method: AdminAuthMethod; is_super_admin: boolean } | { ok: false; response: Response }`
  struct AdminAuthResultOk {
    static constexpr bool ok = true;
    std::string token;
    AdminAuthMethod method;
    bool is_super_admin;
  };
  struct AdminAuthResultNotOk {
    static constexpr bool ok = false;
    rt::Response response;
  };
  using AdminAuthResult = std::variant<AdminAuthResultOk, AdminAuthResultNotOk>;

  // `method.kind`
  std::string_view clientAuthMethodKind(const ClientAuthMethod& method);
  std::string_view adminAuthMethodKind(const AdminAuthMethod& method);

  [[nodiscard]] rt::Task<AuthenticateClientResult> authenticateClient(rt::Request req);

  [[nodiscard]] rt::Task<AdminAuthResult> authenticateAdmin(rt::Request req);

  // `Response | null`: std::nullopt is null.
  [[nodiscard]] rt::Task<std::optional<rt::Response>> requireAdminAuth(rt::Request req);

  [[nodiscard]] rt::Task<rt::Response> handleV1Auth(rt::Request req);
}
