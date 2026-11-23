#pragma once
// Port of src/codex/external-auth.ts.
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <string_view>

namespace uos::codex::external_auth {
  struct ExternalCodexIdentity {
    std::string userId;
    std::string accountId;
    friend bool operator==(const ExternalCodexIdentity&, const ExternalCodexIdentity&) = default;
  };

  /** Claims are identity preconditions; a passive upstream read separately proves authorization. */
  std::optional<ExternalCodexIdentity> externalCodexIdentity(std::string_view token);

  struct ExternalNativeCodexDocument {
    // Omit<CodexAuthState, "updated_at_ms" | "native_owner">
    struct Auth {
      std::string access_token;
      std::string refresh_token;
      std::string account_id;
      friend bool operator==(const Auth&, const Auth&) = default;
    };

    Auth auth;
    std::string userId;
    std::optional<std::string> refreshedAt;
    friend bool operator==(const ExternalNativeCodexDocument&, const ExternalNativeCodexDocument&) = default;
  };

  /** Exact native 0.162.1 external AuthDotJson, only for a trusted local native file. */
  // `raw: unknown`: nullptr is undefined.
  std::optional<ExternalNativeCodexDocument> parseExternalNativeCodexDocument(const json::value* raw);
  std::optional<ExternalNativeCodexDocument> parseExternalNativeCodexDocument(const json::value& raw);

  // JSON members in declaration order; reading is strict (rt::TypeError naming the member).
  void tag_invoke(const json::value_from_tag&, json::value& out, const ExternalNativeCodexDocument::Auth& value);
  ExternalNativeCodexDocument::Auth tag_invoke(const json::value_to_tag<ExternalNativeCodexDocument::Auth>&, const json::value& value);
}
