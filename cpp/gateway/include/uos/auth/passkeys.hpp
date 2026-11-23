#pragma once
// Port of src/auth/passkeys.ts.
//
// @simplewebauthn/server 13.2.2 (generateAuthenticationOptions, verifyRegistrationResponse,
// verifyAuthenticationResponse; scope in docs/rt/platform.md) has no rt equivalent: its port is a
// module-private section of src/auth/passkeys.cpp, with the library's MetadataService left in its
// default DISABLED state and SettingsService holding its default root certificates, as the TS
// gateway never configures either.
//
// Stored records are read with the strict record codecs (a malformed stored record throws TypeError
// where the TS would read through it); where the TS spreads a stored record into the record it
// writes back, the C++ merges on the stored rt::KvObject so undeclared members survive.
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::auth::passkeys {
  struct PasskeyUserRecord {
    std::string id;
    std::string handle;
    bool is_admin;
    std::vector<std::string> credential_ids;
    double created_at_ms;
    double updated_at_ms;
    friend bool operator==(const PasskeyUserRecord&, const PasskeyUserRecord&) = default;
  };

  struct PasskeyCredentialRecord {
    std::string credential_id;
    std::string user_id;
    std::string public_key;
    double sign_count;
    // `transports: string[]`: the registration response's `response.transports` as the client sent
    // it (`?? []`); the TS stores the wire value unchecked, so any JSON value can appear here.
    json::value transports;
    double created_at_ms;
    friend bool operator==(const PasskeyCredentialRecord&, const PasskeyCredentialRecord&) = default;
  };

  struct PasskeySessionRecord {
    std::string token;
    std::string user_id;
    double created_at_ms;
    double expires_at_ms;
    std::optional<std::string> audience_origin{};
    friend bool operator==(const PasskeySessionRecord&, const PasskeySessionRecord&) = default;
  };

  struct PasskeySession {
    std::string token;
    PasskeyUserRecord user;
    PasskeySessionRecord session;
    friend bool operator==(const PasskeySession&, const PasskeySession&) = default;
  };

  void tag_invoke(const json::value_from_tag&, json::value& out, const PasskeyUserRecord& value);
  PasskeyUserRecord tag_invoke(const json::value_to_tag<PasskeyUserRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PasskeyCredentialRecord& value);
  PasskeyCredentialRecord tag_invoke(const json::value_to_tag<PasskeyCredentialRecord>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const PasskeySessionRecord& value);
  PasskeySessionRecord tag_invoke(const json::value_to_tag<PasskeySessionRecord>&, const json::value& value);

  inline constexpr double PASSKEY_CHALLENGE_TTL_MS = 5 * 60 * 1000;
  inline constexpr double PASSKEY_SESSION_TTL_MS = 7.0 * 24 * 60 * 60 * 1000;
  inline constexpr double PASSKEY_MAX_REQUEST_BODY_BYTES = 64 * 1024;
  inline constexpr std::string_view PASSKEY_RELAY_COOKIE_NAME = "__Host-uos_ai_relay_session";
  // Anonymous login-start challenge issuance is bounded by a fixed, clock-aligned
  // window so the reset instant is deterministic and needs no new secret,
  // environment variable, or per-client identifier.
  inline constexpr double PASSKEY_LOGIN_START_WINDOW_MS = 60'000;
  inline constexpr double PASSKEY_LOGIN_START_LIMIT = 10;

  rt::KvKey passkeyUserKey(const std::string& userId);
  rt::KvKey passkeyHandleKey(const std::string& handle);
  rt::KvKey passkeyCredentialKey(const std::string& credentialId);
  rt::KvKey passkeyChallengeKey(const std::string& challenge);
  rt::KvKey passkeySessionKey(const std::string& token);
  rt::KvKey passkeyLoginStartThrottleKey(double windowStartMs);

  // `value: unknown`; nullptr is undefined.
  std::string normalizePasskeyHandle(const json::value& value);
  std::string normalizePasskeyHandle(const json::value* value);

  rt::Task<std::string> buildPasskeyHandle(std::string seed);

  // `Pick<PasskeyUserRecord, "is_admin"> | null | undefined`; nullptr is null or undefined.
  bool isPasskeyUserAdmin(const PasskeyUserRecord* user);
  bool isPasskeyUserAdmin(const PasskeyUserRecord& user);

  // The `{ origin, rpId }` result of getPasskeyRequestMeta.
  struct PasskeyRequestMeta {
    std::string origin;
    std::string rpId;
    friend bool operator==(const PasskeyRequestMeta&, const PasskeyRequestMeta&) = default;
  };
  // `clientOrigin?: unknown`; the one-argument overload and nullptr are undefined.
  PasskeyRequestMeta getPasskeyRequestMeta(const rt::Request& req);
  PasskeyRequestMeta getPasskeyRequestMeta(const rt::Request& req, const json::value* clientOrigin);

  rt::Task<bool> hasPasskeyUsers();

  // `Readonly<{ key: KvKey; value: PasskeyCredentialRecord; versionstamp: string }>`
  struct PasskeyCredentialEntry {
    rt::KvKey key;
    PasskeyCredentialRecord value;
    std::string versionstamp;
  };
  rt::Task<bool> updatePasskeyCredentialSignCount(std::shared_ptr<rt::Kv> kv, PasskeyCredentialEntry entry, double signCount);

  // `SaveVerifiedPasskeyRegistrationInput` (module-private in the TS; the parameter type of an export).
  struct SaveVerifiedPasskeyRegistrationInput {
    std::string userId;
    std::string handle;
    bool isAdmin;
    std::string credentialId;
    std::string publicKey;
    double signCount;
    // `transports: string[]` as received; see PasskeyCredentialRecord::transports.
    json::value transports;
  };
  // `{ ok: true; user: PasskeyUserRecord } | { ok: false; response: Response }`
  struct SavedPasskeyRegistrationOk {
    static constexpr bool ok = true;
    PasskeyUserRecord user;
  };
  struct SavedPasskeyRegistrationRejected {
    static constexpr bool ok = false;
    rt::Response response;
  };
  using SavedPasskeyRegistration = std::variant<SavedPasskeyRegistrationOk, SavedPasskeyRegistrationRejected>;
  rt::Task<SavedPasskeyRegistration> saveVerifiedPasskeyRegistration(std::shared_ptr<rt::Kv> kv, SaveVerifiedPasskeyRegistrationInput input);

  // std::nullopt is null.
  rt::Task<std::optional<PasskeySession>> getPasskeySession(std::string token);
  rt::Task<std::optional<PasskeySession>> getPasskeySessionFromRequest(rt::Request req);
  // `authenticatedPasskeyToken?: string`: the one-argument overload and std::nullopt are undefined.
  rt::Task<std::optional<PasskeySession>> getPasskeySessionForRequest(rt::Request req);
  rt::Task<std::optional<PasskeySession>> getPasskeySessionForRequest(rt::Request req, std::optional<std::string> authenticatedPasskeyToken);

  // `options: { defaultIsAdmin?: boolean; authenticatedPasskeyToken?: string } = {}`
  struct PasskeyRegisterStartOptions {
    std::optional<bool> defaultIsAdmin{};
    std::optional<std::string> authenticatedPasskeyToken{};
  };
  rt::Task<rt::Response> handlePasskeyRegisterStart(rt::Request req);
  rt::Task<rt::Response> handlePasskeyRegisterStart(rt::Request req, PasskeyRegisterStartOptions options);

  rt::Task<rt::Response> handlePasskeyRegisterFinish(rt::Request req);

  rt::Task<rt::Response> handlePasskeyLoginStart(rt::Request req);

  rt::Task<rt::Response> handlePasskeyLoginFinish(rt::Request req);

  // `options: { authenticatedPasskeyToken?: string } = {}` of handlePasskeySession and handlePasskeyLogout.
  struct PasskeySessionOptions {
    std::optional<std::string> authenticatedPasskeyToken{};
  };
  rt::Task<rt::Response> handlePasskeySession(rt::Request req);
  rt::Task<rt::Response> handlePasskeySession(rt::Request req, PasskeySessionOptions options);

  rt::Task<rt::Response> handlePasskeyLogout(rt::Request req);
  rt::Task<rt::Response> handlePasskeyLogout(rt::Request req, PasskeySessionOptions options);

  rt::Task<rt::Response> handlePasskeyUsersList();

  rt::Task<rt::Response> handlePasskeyUsersUpdate(rt::Request req);
}
