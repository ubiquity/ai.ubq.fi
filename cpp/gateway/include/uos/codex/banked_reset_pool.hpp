#pragma once
// Port of src/codex/banked-reset-pool.ts.
// Codex banked reset pool evaluation and shadow decisions, split out of src/codex_banked_reset.ts.
#include "uos/codex/banked_reset.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/kv.hpp"
#include "uos/types.hpp"

#include <memory>
#include <optional>
#include <vector>

namespace uos::codex::banked_reset_pool {
  /** Read-only, redacted administrative projection of recent shadow evidence. */
  // `kvOverride?: Kv | null`: the overload without it is undefined (getKv()); in the other, std::nullopt is
  // undefined and a null pointer is null. The result is null as std::nullopt.
  [[nodiscard]] rt::Task<std::optional<std::vector<::uos::types::CodexResetShadowDecisionRecord>>> listCodexResetShadowDecisions();
  [[nodiscard]] rt::Task<std::optional<std::vector<::uos::types::CodexResetShadowDecisionRecord>>> listCodexResetShadowDecisions(
    std::optional<std::shared_ptr<rt::Kv>> kvOverride);

  /**
   * Evaluate one complete currently fenced blocked cohort. Live reads each
   * blocked account's inventory and proceeds directly to the durable submission
   * for the selected credit. The former shadow-only branch remains for isolated
   * fixture coverage and is unreachable from production configuration.
   */
  [[nodiscard]] rt::Task<banked_reset::CodexBankedResetPoolOutcome> evaluateCodexBankedResetPool(
    std::vector<banked_reset::CodexBankedResetPoolCandidate> candidates, banked_reset::CodexBankedResetDependencies dependencies);
}
