#pragma once
// Port of src/codex/routing-mutations.ts.
// Codex routing account mutations and probe claiming, split out of src/codex_account_routing.ts.
#include "uos/authority/native_codex_429.hpp"
#include "uos/codex/routing_state.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <functional>
#include <optional>
#include <vector>

namespace uos::codex::routing_mutations {
  // `now = Date.now()` is the overload without it.
  [[nodiscard]] rt::Task<authority::native_codex_429::Codex429Classification> markCodexRecoveryProbeQuotaBlocked(routing_state::RoutingAccount account,
                                                                                                              rt::Response response);
  [[nodiscard]] rt::Task<authority::native_codex_429::Codex429Classification> markCodexRecoveryProbeQuotaBlocked(routing_state::RoutingAccount account,
                                                                                                              rt::Response response, double now);

  /**
   * Legacy transition retained for state-migration tests. Live inference no
   * longer calls it; normalized durable reads discard the resulting fence.
   */
  // `now = Date.now()` is the overload without it.
  [[nodiscard]] rt::Task<void> markCodexUpstreamTimeout(routing_state::RoutingAccount account);
  [[nodiscard]] rt::Task<void> markCodexUpstreamTimeout(routing_state::RoutingAccount account, double now);

  [[nodiscard]] rt::Task<void> markCodexCredentialInvalid(routing_state::RoutingAccount account);

  /**
   * OAuth refresh can replace a token while an inference attempt is in flight.
   * Treat the token as a new credential version while retaining the account's
   * stable reset observation, so a refresh cannot manufacture a second key.
   */
  [[nodiscard]] rt::Task<routing_state::RoutingAccount> reconcileCodexRoutingAccount(routing_state::RoutingAccount account, ::uos::types::CodexAuthState auth);

  [[nodiscard]] rt::Task<void> releaseCodexRoutingProbe(routing_state::RoutingAccount account);

  /** A successful recovery probe is the only trusted way to clear reset ambiguity. */
  [[nodiscard]] rt::Task<void> markCodexSuccess(routing_state::RoutingAccount account);

  /**
   * Pure exact-fence predicate used by both strong-read helpers and the banked
   * reset's atomic KV checks. Equality (rather than `>=`) prevents a shorter
   * later Retry-After from authorizing a reset against a longer existing block;
   * an unresolved legacy account association denies every new claim.
   */
  // `value: unknown`: nullptr is undefined.
  bool isCodexQuotaBlockFenceCurrent(const json::value& value, const routing_state::RoutingAccount& account, double quotaResetAtMs, double routingGeneration);
  bool isCodexQuotaBlockFenceCurrent(const json::value* value, const routing_state::RoutingAccount& account, double quotaResetAtMs, double routingGeneration);

  /**
   * Returns a durable routing fence for a fully persisted quota observation.
   * Account routing otherwise fails open when KV is down; banked redemption is
   * stricter and must refuse to start without this strong-read proof.
   */
  // `Promise<number | null>`
  [[nodiscard]] rt::Task<std::optional<double>> getCodexQuotaBlockFence(routing_state::RoutingAccount account, double quotaResetAtMs);

  /**
   * An additional immutable record that must remain current while a verified
   * reset clears its routing circuit. The gateway uses this for the auth-pool
   * slot, so a credential rotation cannot turn a verified reset into a retry
   * with a removed credential.
   */
  // `isCurrent: (value: unknown) => boolean` is called with the strongly read row as JSON (null when
  // the key is absent). The callers call it inside the try the TS wraps around it, so a row that
  // cannot be read as JSON counts as not current. banked_reset::CodexBankedResetFence and
  // responses_state::BankedResetFence (structurally identical in the TS) are this type.
  struct CodexQuotaResetReconciliationFence {
    rt::KvKey key;
    std::function<bool(const json::value& value)> isCurrent;
  };

  // `Readonly<{ quotaResetAtMs: number; routingGeneration: number; fences?: readonly CodexQuotaResetReconciliationFence[] }>`,
  // the input of both reconcileCodexQuotaAfter*Reset functions.
  struct CodexQuotaResetReconciliationInput {
    double quotaResetAtMs;
    double routingGeneration;
    std::optional<std::vector<CodexQuotaResetReconciliationFence>> fences{};
  };

  /**
   * A verified reset may release only the exact circuit observation that caused
   * it. Credential and generation checks prevent an old provider transaction
   * from admitting a newly rotated credential or a later quota window.
   */
  [[nodiscard]] rt::Task<std::optional<routing_state::RoutingAccount>> reconcileCodexQuotaAfterVerifiedReset(routing_state::RoutingAccount account,
                                                                                                           CodexQuotaResetReconciliationInput input);

  /**
   * Recover an account whose verified reset record predates later routing
   * transitions. The ledger proves the reset was already spent, so this path
   * only fences a new inference probe; it never submits another reset.
   */
  [[nodiscard]] rt::Task<std::optional<routing_state::RoutingAccount>> reconcileCodexQuotaAfterStaleVerifiedReset(routing_state::RoutingAccount account,
                                                                                                                CodexQuotaResetReconciliationInput input);

  // `now = Date.now()` is the overload without it.
  [[nodiscard]] rt::Task<std::optional<routing_state::RoutingAccount>> claimCodexRoutingProbe(::uos::types::CodexAuthPoolState pool,
                                                                                            routing_state::RoutingAccount account);
  [[nodiscard]] rt::Task<std::optional<routing_state::RoutingAccount>> claimCodexRoutingProbe(::uos::types::CodexAuthPoolState pool,
                                                                                            routing_state::RoutingAccount account, double now);
}
