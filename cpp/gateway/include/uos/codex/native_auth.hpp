#pragma once
// Port of src/codex/native-auth.ts.
// Local native Codex credential ownership: pool binding, external adoption and owned refresh.
#include "uos/codex/auth.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace uos::codex::native_auth {
  // `type NativeAuthHooks` (module-private in the TS). An absent member (std::nullopt, empty
  // std::function) uses the real environment.
  struct NativeAuthHooks {
    std::optional<std::string> codexHome{};
    // `typeof process.platform`
    std::optional<std::string> os{};
    // `(name: string) => string | undefined`
    std::function<std::optional<std::string>(std::string name)> getEnv{};
    // `() => Promise<unknown>`: the raw auth.json document.
    std::function<rt::Task<json::value>()> readAuth{};
    // `(home: string, email: string, accountId?: string) => Promise<void>`
    std::function<rt::Task<void>(std::string home, std::string email, std::optional<std::string> accountId)> refresh{};
  };

  /** Isolated fixtures never touch the owner's credential home or daemon. */
  // `value: NativeAuthHooks | null`: std::nullopt is null.
  void setNativeCodexAuthHooksForTest(std::optional<NativeAuthHooks> value);

  /** The recovery contract permits the primary and the one existing named Ubiquity home. */
  std::vector<std::string> nativeCodexConsumerHomes();

  // `auth: Pick<CodexAuthState, "account_id" | "access_token" | "refresh_token">`: every caller passes a
  // whole CodexAuthState; only those three members are read.
  [[nodiscard]] rt::Task<std::string> nativeCodexCredentialGeneration(::uos::types::CodexAuthState auth);

  /** Equality is the only bootstrap proof; a file never overlays uploaded siblings. */
  [[nodiscard]] rt::Task<auth::CodexAuthPoolEntry> bindNativeCodexAuthPool(auth::CodexAuthPoolEntry input);

  /** The CLI proves the current fixed-home file and passive authorization before this plan. */
  // `raw: unknown`
  [[nodiscard]] rt::Task<::uos::types::CodexAuthPoolState> planExternalNativeCodexAdoption(::uos::types::CodexAuthPoolState pool, json::value raw,
                                                                                          std::string home, double nowMs, bool previousRejected);

  /** Returns null only for an unrelated, unbound account. Never falls open for an owner. */
  [[nodiscard]] rt::Task<std::optional<::uos::types::CodexAuthState>> refreshNativeCodexAuthIfOwned(::uos::types::CodexAuthState input,
                                                                                                  bool rejectStaleReplacement = false);
}
