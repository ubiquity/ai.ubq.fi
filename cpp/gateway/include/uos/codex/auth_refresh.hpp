#pragma once
// Port of src/codex/auth-refresh.ts.
// Codex auth refresh coordination, split out of src/codex.ts.
//
// awaitWithoutCancellingSharedWork is generic in the TS. The template is defined in
// src/codex/auth_refresh.cpp and explicitly instantiated there for every T the port uses
// (the `extern template` lines below); a new T needs a new instantiation there.
#include "uos/codex/auth.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/types.hpp"

#include <optional>
#include <string>
#include <utility>

namespace uos::codex::auth_refresh {
  [[nodiscard]] rt::Task<auth::CodexAuthAccountEntry> getCurrentAccountEntry(std::string accountId, bool forceKv);

  bool sameCodexCredentials(const ::uos::types::CodexAuthState& left, const ::uos::types::CodexAuthState& right);

  // `new Promise((resolve) => setTimeout(resolve, milliseconds))`
  [[nodiscard]] rt::Task<void> delay(double milliseconds);

  /**
   * Coalesce refreshes across isolates. A waiter repeatedly force-reads the
   * pool and adopts a newer token instead of issuing OAuth itself. The lease
   * contains only a hash-derived slot key and an opaque owner token.
   */
  [[nodiscard]] rt::Task<::uos::types::CodexAuthState> refreshAuthCoordinated(auth::CodexAuthAccountEntry input);


  /**
   * Abort reasons become promise rejection reasons and thrown values, both of
   * which must be Errors. A non-Error abort reason is preserved as the cause.
   */
  // `(reason: unknown): Error`: the result always holds an Error (with its cause for a non-Error reason).
  rt::Reason abortReasonAsError(const rt::Reason& reason);

  // `<T>(promise: Promise<T>, signal?: AbortSignal): Promise<T>`; std::nullopt is an absent signal (every
  // caller passes its possibly undefined signal).
  template <class T>
  [[nodiscard]] rt::Task<T> awaitWithoutCancellingSharedWork(rt::Promise<T> promise, std::optional<rt::AbortSignal> signal);
  extern template rt::Task<::uos::types::CodexAuthState> awaitWithoutCancellingSharedWork<::uos::types::CodexAuthState>(
    rt::Promise<::uos::types::CodexAuthState> promise, std::optional<rt::AbortSignal> signal);

  [[nodiscard]] rt::Task<::uos::types::CodexAuthState> refreshAuthStateless(::uos::types::CodexAuthState auth);

  [[nodiscard]] rt::Task<::uos::types::CodexAuthState> getValidAuth(auth::CodexAuthAccountEntry current);
}
