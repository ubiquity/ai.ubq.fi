#pragma once
// Port of src/codex/banked-reset-submission.ts.
// Codex banked reset submission, redemption and reconciliation, split out of src/codex_banked_reset.ts.
#include "uos/codex/banked_reset.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"

#include <memory>

namespace uos::codex::banked_reset_submission {
  /**
   * The production KV accessor. An unavailable accessor and an explicit null
   * override both fail closed, exactly as before.
   */
  // `Promise<Kv | null>`: a null pointer is null.
  [[nodiscard]] rt::Task<std::shared_ptr<rt::Kv>> openResetKv(banked_reset::CodexBankedResetDependencies dependencies);

  /**
   * Attempt or reconcile exactly one logical reset after normal account
   * failover. New external submissions require live policy and all durable
   * fences; an existing submitted/unknown record is reconciled even while the
   * kill switch is disabled.
   */
  [[nodiscard]] rt::Task<banked_reset::CodexBankedResetOutcome> attemptCodexBankedReset(banked_reset::CodexBankedResetCandidate candidate,
                                                                                       banked_reset::CodexBankedResetDependencies dependencies);

  /**
   * Recovery-only path for a durable submitted/unknown record. It never creates
   * a claim or calls `redeem`, so it remains safe during a rollback.
   */
  [[nodiscard]] rt::Task<banked_reset::CodexBankedResetOutcome> reconcileCodexBankedReset(banked_reset::CodexBankedResetCandidate candidate,
                                                                                         banked_reset::CodexBankedResetDependencies dependencies);
}
