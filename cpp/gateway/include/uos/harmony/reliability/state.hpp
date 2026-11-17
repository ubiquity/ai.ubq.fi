#pragma once
/**
 * Structured task state and the state contract (plan m05).
 *
 * The benchmark tracks two views of a run:
 *
 * - the **full transcript** (every message, every tool result) — maximal
 *   fidelity, maximum context cost;
 * - the **structured task state** — the compact, decision-relevant projection
 *   of what the agent has done and what is still unproven.
 *
 * `StructuredTaskState` is derived deterministically from tool observations
 * by REPLAYING the same rule engines the live harness uses — the loop
 * detector (`loops.ts`) and the verification tracker (`verify.ts`) — so a
 * recorded trajectory and a live run always produce the same state.
 * {@link stateContract} renders the decision-relevant projection as canonical
 * JSON; the comparison evidence (`benchmarks/compare.ts`) proves that
 * transcript compaction preserves this contract at short/medium/large budget
 * tiers, which is exactly the m05 claim that a structured context can replace
 * a full replay without losing state.
 *
 * This module performs no I/O.
 */
#include "uos/harmony/reliability/loops.hpp"
#include "uos/harmony/reliability/verify.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <vector>

namespace uos::harmony::reliability::state {
  // "planning" | "exploring" | "acting" | "recovering" | "verifying" | "completing" | "stalled" | "done"
  using TaskPhase = std::string;

  struct ToolObservation {
    /** Monotonic attempt sequence (includes invalid and guarded calls). */
    double seq;
    std::string tool;
    json::object args;
    bool valid;
    /** Result when the call was executed or resolved by a guard. */
    rt::Nullable<loops::ResultLike> result{};
  };

  struct FinalObservation {
    std::string content;
    bool accepted;
    double seq;
    friend bool operator==(const FinalObservation&, const FinalObservation&) = default;
  };

  struct ReliabilityRun {
    std::vector<ToolObservation> observations;
    std::vector<FinalObservation> finals;
    double modelCalls;
  };

  struct ReadRecord {
    std::string path;
    /** Digest of the last read content of this path. */
    std::string digest;
    /** Last observation sequence touching this path. */
    double seq;
    friend bool operator==(const ReadRecord&, const ReadRecord&) = default;
  };

  struct WriteRecord {
    std::string path;
    std::string marker;
    bool add;
    bool verified;
    // "read" | "shell" | null
    std::optional<std::string> verifiedBy;
    double seq;
    friend bool operator==(const WriteRecord&, const WriteRecord&) = default;
  };

  struct StateErrorRecord {
    std::string code;
    std::string tool;
    double seq;
    /** Short deterministic fingerprint of the error text. */
    std::string digest;
    friend bool operator==(const StateErrorRecord&, const StateErrorRecord&) = default;
  };

  struct StructuredTaskState {
    // `plan: { items: readonly string[]; seq: number | null }`
    struct Plan {
      std::vector<std::string> items;
      std::optional<double> seq;
      friend bool operator==(const Plan&, const Plan&) = default;
    };
    // `{ path: string; seq: number }`
    struct PathEntry {
      std::string path;
      double seq;
      friend bool operator==(const PathEntry&, const PathEntry&) = default;
    };
    // `{ command: string; seq: number }`
    struct CommandEntry {
      std::string command;
      double seq;
      friend bool operator==(const CommandEntry&, const CommandEntry&) = default;
    };

    TaskPhase phase;
    double modelCalls;
    double toolCalls;
    double invalidCalls;
    double invalidCallStreak;
    double duplicateCalls;
    double semanticLoops;
    double semanticLoopStreak;
    Plan plan;
    std::vector<ReadRecord> reads;
    std::vector<WriteRecord> writes;
    std::vector<StateErrorRecord> errors;
    std::vector<PathEntry> pendingVerification;
    std::vector<CommandEntry> unresolvedCommands;
    std::vector<PathEntry> unresolvedEdits;
    /** Sequence of the last successful tool call (0 = none yet). */
    double lastActionSeq;
    std::vector<FinalObservation> finals;
    double finalAttempts;
    friend bool operator==(const StructuredTaskState&, const StructuredTaskState&) = default;
  };

  StructuredTaskState emptyTaskState();

  struct ObservationMeta {
    std::optional<std::string> duplicate;
    bool semanticLoop;
    std::optional<verify::VerificationResolution> verification;
    friend bool operator==(const ObservationMeta&, const ObservationMeta&) = default;
  };

  /** Converts a recorded observation result into the canonical envelope shape. */
  std::optional<loops::ResultLike> toToolResult(const rt::Nullable<loops::ResultLike>& result);

  /** Replays a deterministic rule stream over observations. */
  std::vector<ObservationMeta> replayMeta(const std::vector<ToolObservation>& observations, const verify::VerificationPolicy& policy = verify::DEFAULT_VERIFICATION_POLICY);

  /** Applies one tool observation with its replayed meta to the state. */
  StructuredTaskState reduceToolObservation(const StructuredTaskState& state, const ToolObservation& obs, const ObservationMeta& meta);

  StructuredTaskState reduceFinalAttempt(const StructuredTaskState& state, const FinalObservation& final);

  /** Derives structured state from a deterministic replay of the run. */
  StructuredTaskState stateFromRun(const ReliabilityRun& run);

  /** Derives structured state from observations plus externally supplied meta. */
  StructuredTaskState deriveStateWithMeta(const ReliabilityRun& run, const std::vector<ObservationMeta>& meta);

  /**
   * Canonical JSON of the decision-relevant projection.  Compaction may remove
   * intermediate read outputs, analysis and old transcript pairs — this
   * projection is defined to ignore exactly those, so two views of the same run
   * have equal contracts iff they encode the same durable state.
   */
  std::string stateContract(const StructuredTaskState& state);

  /** Deterministic multi-line summary of the structured state (model-facing). */
  std::string renderStateSummary(const StructuredTaskState& state);
}
