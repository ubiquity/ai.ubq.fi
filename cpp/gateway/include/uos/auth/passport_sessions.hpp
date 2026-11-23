#pragma once
// Port of src/auth/passport-sessions.ts.
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <vector>

namespace uos::auth::passport_sessions {
  /** Browser persistence is renewed on use; the authoritative store has no expiry. */
  inline constexpr double PASSPORT_COOKIE_MAX_AGE_SECONDS = 400 * 24 * 60 * 60;

  struct PassportSessionIdentity {
    std::string subject;
    std::string handle;
    bool isAdmin;
    std::vector<std::string> scopes;
    friend bool operator==(const PassportSessionIdentity&, const PassportSessionIdentity&) = default;
  };

  // `class PassportSessionError extends Error` (its name stays "Error"); `status` is 401 or 503.
  class PassportSessionError : public rt::JsError {
  public:
    double status;
    explicit PassportSessionError(double status_)
      : rt::JsError(status_ == 401 ? "Unauthorized" : "Passport session verification is unavailable."), status(status_) {}
  };

  bool isPersistentPassportSession(const std::string& token);

  // `grant: unknown`; the overload without `previousToken` is undefined.
  [[nodiscard]] rt::Task<std::string> createPassportSession(json::value grant, std::string subject);
  [[nodiscard]] rt::Task<std::string> createPassportSession(json::value grant, std::string subject, std::optional<std::string> previousToken);

  [[nodiscard]] rt::Task<PassportSessionIdentity> getPassportSession(std::string token);

  /** Local revocation completes before the independent upstream best-effort request. */
  // "revoked" | "unavailable" | "not_applicable"
  [[nodiscard]] rt::Task<std::string> revokePassportSession(std::string token);
}
