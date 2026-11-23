#pragma once
/**
 * Verification requirements and false-completion prevention (plan m05).
 *
 * The reliability layer treats two claims as unproven until evidence exists:
 *
 * - **Edits**: every successful `editor.apply_patch` creates a *pending
 *   verification* for its path.  The requirement is satisfied by a
 *   subsequent `filesystem.read` of the same path whose content includes the
 *   written marker, or by a successful `shell.exec` that names the path, or
 *   by the task's declared verification command (exact match, because the
 *   benchmark runner re-runs the same command as the authoritative gate).
 * - **Recovery**: a failed `shell.exec` or failed `editor.apply_patch` must
 *   be followed by evidence before the run may finish — a later successful
 *   command (any), a successful write of the same path, or an exec that
 *   mentions the path.  A final answer while such evidence is missing is a
 *   *false completion* and is rejected deterministically.
 *
 * {@link guardFinal} is the single deterministic point of truth for final
 * acceptance; the harness (and the benchmark derivation layer) both use it.
 * This module performs no I/O and never executes tools.
 */
#include "uos/harmony/reliability/loops.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace uos::harmony::reliability::verify {
  // "read" | "shell"
  using VerificationKind = std::string;

  /**
   * Closed set of guard error codes the reliability harness uses in tool result
   * envelopes for calls it refuses to execute.  These are m05-owned (m04's
   * {@link ToolResult.error_code} set covers execution failures); metrics and
   * state derivation treat them as guards, never as real tool errors.
   */
  inline constexpr std::array<std::string_view, 2> GUARD_ERROR_CODES = {"duplicate_call", "repeated_failure"};

  struct VerificationPolicy {
    /** Reject finals while any write is unverified. Default true. */
    bool requireVerificationBeforeFinal;
    /** Reject finals while command/edit failures are unresolved. Default true. */
    bool requireRecoveryBeforeFinal;
    /** Reject finals while a semantic loop is active. Default true. */
    bool rejectFinalDuringLoop;
    /** Reject finals before any task.update_plan when writes exist. Default false. */
    bool requirePlanBeforeWrites;
    /** Hard cap of final attempts before the harness aborts. Default 3. */
    double maxFinalAttempts;
    /** Identical finals with no intervening action allowed before abort. Default 2. */
    double maxRepeatedFinals;
    /** Task-declared verification command (exact match satisfies all writes). */
    std::optional<std::string> verificationCommand;
    friend bool operator==(const VerificationPolicy&, const VerificationPolicy&) = default;
  };

  inline const VerificationPolicy DEFAULT_VERIFICATION_POLICY{
    .requireVerificationBeforeFinal = true,
    .requireRecoveryBeforeFinal = true,
    .rejectFinalDuringLoop = true,
    .requirePlanBeforeWrites = false,
    .maxFinalAttempts = 3,
    .maxRepeatedFinals = 2,
    .verificationCommand = std::nullopt,
  };

  struct PendingVerification {
    std::string path;
    /** Written marker (the patch `new` value) the read must contain. */
    std::string marker;
    bool add;
    double since;
    friend bool operator==(const PendingVerification&, const PendingVerification&) = default;
  };

  struct UnresolvedCommand {
    std::string command;
    std::string code;
    double since;
    friend bool operator==(const UnresolvedCommand&, const UnresolvedCommand&) = default;
  };

  struct UnresolvedEdit {
    std::string path;
    double since;
    friend bool operator==(const UnresolvedEdit&, const UnresolvedEdit&) = default;
  };

  struct VerificationResolution {
    VerificationKind kind;
    /** Paths whose pending verification this call satisfied. */
    std::vector<std::string> paths;
    friend bool operator==(const VerificationResolution&, const VerificationResolution&) = default;
  };

  class VerificationTracker {
  public:
    const VerificationPolicy policy{};

    explicit VerificationTracker(VerificationPolicy policy_ = DEFAULT_VERIFICATION_POLICY);

    /**
     * Observes one executed call and updates requirements.  Returns the
     * resolution when this call satisfied a pending verification.
     */
    std::optional<VerificationResolution> observe(const std::string& tool, const json::object& args, const loops::ResultLike& result);

    std::vector<PendingVerification> pending() const;
    std::vector<UnresolvedCommand> unresolvedCommands() const;
    std::vector<UnresolvedEdit> unresolvedEdits() const;
    bool allVerified() const;

    struct Totals {
      double required;
      double satisfied;
      friend bool operator==(const Totals&, const Totals&) = default;
    };
    Totals totals() const;

    /** Deterministic JSON snapshot for comparisons. */
    json::object snapshot() const;

  private:
    /** Forgets every unsolved command and edit failure of the previous approach. */
    void abandonUnresolved_();
    /**
     * A successful patch becomes a pending verification; a failed one becomes
     * an unresolved edit that must be recovered before a final answer.
     */
    std::optional<VerificationResolution> observeEdit_(const std::optional<std::string>& path, const json::object& args, const loops::ResultLike& result);
    /** A read of the written marker satisfies that path's pending verification. */
    std::optional<VerificationResolution> observeRead_(const std::string& path, const loops::ResultLike& result);
    /** A successful command recovers failures; a failed one is recorded. */
    std::optional<VerificationResolution> observeExec_(const json::object& args, const loops::ResultLike& result);

    rt::OrderedMap<std::string, PendingVerification> pending_;
    rt::OrderedMap<std::string, UnresolvedCommand> unresolvedCommands_;
    rt::OrderedMap<std::string, UnresolvedEdit> unresolvedEdits_;
    double required_ = 0;
    double satisfied_ = 0;
  };

  // "unverified_write" | "unresolved_command" | "unresolved_edit" | "active_loop" | "false_completion" | "plan_required"
  using FinalRequirementKind = std::string;

  struct FinalRequirement {
    FinalRequirementKind kind;
    std::string message;
    std::optional<std::string> path = std::nullopt;
    std::optional<std::string> command = std::nullopt;
    friend bool operator==(const FinalRequirement&, const FinalRequirement&) = default;
  };

  struct FinalAttempt {
    std::string content;
    bool rejected;
    /** Sequence of the last successful tool call before this final (0 = none). */
    double lastActionSeq;
    /** How many times this exact content was attempted without intervening action. */
    double repetitions;
    friend bool operator==(const FinalAttempt&, const FinalAttempt&) = default;
  };

  struct FinalGuardInput {
    std::string finalContent;
    /** Sequence of the last successful tool call before this final (0 = none). */
    double lastActionSeq;
    std::vector<FinalAttempt> previousFinals;
    double semanticLoopStreak;
    bool planUpdated;
    double writes;
    const VerificationTracker& tracker;
    VerificationPolicy policy;
  };

  struct FinalGuardDecision {
    bool allowed;
    std::vector<FinalRequirement> requirements;
    /** True when the same final content was already rejected with no action between. */
    bool falseCompletion;
    /** Updated attempt bookkeeping for the harness. */
    FinalAttempt attempt;
  };

  /**
   * Deterministic final-answer gate.  Returns `allowed: false` with every
   * blocking requirement when the claim is not yet supported by evidence.
   */
  FinalGuardDecision guardFinal(const FinalGuardInput& input);

  /** One deterministic line listing every unmet requirement. */
  std::string renderGuardRequirements(const std::vector<FinalRequirement>& requirements);

  /** Stable prefix of every harness guard message handed back to the model. */
  inline constexpr std::string_view GUARD_PREFIX = "[guard] final answer rejected";
}
