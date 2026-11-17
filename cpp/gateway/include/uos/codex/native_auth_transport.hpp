#pragma once
// src/codex/native-auth-transport.ts: the native Codex daemon's auth capability
// (account verification and the token refresh request).
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>

namespace uos::codex::native_auth_transport {
  // Readonly<{ email: string; accountId: string }>
  struct NativeCodexExpectedAccount {
    std::string email;
    std::string accountId;
  };

  // { ok: true } | { ok: false; detail: string }: `detail` is present exactly when !ok.
  struct NativeCodexAccountVerification {
    bool ok = false;
    std::optional<std::string> detail{};
    friend bool operator==(const NativeCodexAccountVerification&, const NativeCodexAccountVerification&) = default;
  };

  /**
   * Verdict for the daemon's account reply. Managed-auth daemons whose active provider does
   * not require OpenAI auth (a gateway-routed custom provider reports `requiresOpenaiAuth:
   * false`) return no account even though they refresh the home's ChatGPT credentials. That
   * exact shape falls back to the credential-file identity the caller already bound; every
   * other shape must report a matching ChatGPT account.
   */
  // `account: unknown` (nullptr is undefined); `fileAccountId: string | null`.
  NativeCodexAccountVerification verifyNativeCodexAccount(const json::value& account, const NativeCodexExpectedAccount& expected, const std::optional<std::string>& fileAccountId);
  NativeCodexAccountVerification verifyNativeCodexAccount(const json::value* account, const NativeCodexExpectedAccount& expected, const std::optional<std::string>& fileAccountId);

  /** Fixed auth capability; the supervisor's read-only transport stays unchanged. */
  rt::Task<void> requestNativeCodexRefresh(std::string codexHome, std::string expectedEmail, std::string expectedAccountId);
}
