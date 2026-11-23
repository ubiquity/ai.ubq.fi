#pragma once
/**
 * Duplicate and semantic-loop detection (plan m05).
 *
 * The detector is a pure, deterministic state machine fed one tool call at a
 * time.  It answers two questions:
 *
 * 1. **Duplicate** — is this call identical to the immediately previous
 *    attempted call, and did that call succeed?  (Retries after *failures*
 *    are recovery, not duplication — m02 metric semantics — so the detector
 *    reports the raw adjacency fact and lets the retry policy decide whether
 *    re-execution is allowed.)
 * 2. **Semantic loop** — does the (tool, arguments, effect) signature recur
 *    within a bounded window, either as a repeating call pattern or as three
 *    repetitions of one effect (the same action producing the same result,
 *    i.e. no new information and no state change)?
 *
 * "Effect" is the deterministic result identity: `ok` plus the digest of the
 * output for successes, or the error code plus the digest of the error text
 * for failures.  Guard-rejected calls (duplicate_call / repeated_failure)
 * therefore carry a stable effect signature too, so repeated rejections are
 * themselves detected as a loop.
 *
 * This module performs no I/O.
 */
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uos::harmony::reliability::loops {
  /** Structural result view accepted by the rule engines (m04 envelope or recorded). */
  struct ResultLike {
    bool ok;
    rt::Nullable<std::string> error_code{};
    rt::Nullable<std::string> error{};
    rt::Nullable<std::string> output{};
    friend bool operator==(const ResultLike&, const ResultLike&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const ResultLike& value);
  ResultLike tag_invoke(const json::value_to_tag<ResultLike>&, const json::value& value);

  /** Sorts object keys recursively so argument canonical form is order-independent. */
  json::value canonicalize(const json::value& value);
  // `canonicalize(undefined)` is undefined (nullptr in, std::nullopt out).
  std::optional<json::value> canonicalize(const json::value* value);

  /** Deterministic canonical JSON form of tool arguments. */
  std::string canonicalArgs(const json::object& args);

  /** Stable identity of one (tool, arguments) pair. */
  std::string callIdentity(const std::string& tool, const json::object& args);

  /** Digest of the model-visible effect of one result (deterministic). */
  std::string effectDigest(const ResultLike& result);

  /** Full effect signature of a call + result. */
  std::string effectSignature(const std::string& tool, const json::object& args, const ResultLike& result);

  // "exact_adjacent" | "repeat_after_success"
  using DuplicateFlag = std::string;

  struct LoopFlags {
    /** Identity duplication detected (never qualifies a retry-after-failure). */
    std::optional<DuplicateFlag> duplicate;
    /** True when this call closes a semantic loop within the window. */
    bool semanticLoop;
    // "pattern_recurrence" | "effect_repeat" | null
    std::optional<std::string> loopKind;
    /** Consecutive calls flagged as semantic loops (after this call). */
    double streak;
    friend bool operator==(const LoopFlags&, const LoopFlags&) = default;
  };

  struct LoopDetectorOptions {
    /** Rolling window of remembered call signatures; default 8. */
    std::optional<double> window = std::nullopt;
    /** Pattern length compared for sequence recurrence; default 4. */
    std::optional<double> patternLength = std::nullopt;
    /** Repetitions of one effect signature that close a loop; default 3. */
    std::optional<double> effectRepeatThreshold = std::nullopt;
  };

  inline constexpr double DEFAULT_LOOP_WINDOW = 8;
  inline constexpr double DEFAULT_LOOP_PATTERN_LENGTH = 4;
  inline constexpr double DEFAULT_EFFECT_REPEAT_THRESHOLD = 3;

  /** Deterministic duplicate + semantic-loop detector over one call stream. */
  class LoopDetector {
  public:
    explicit LoopDetector(LoopDetectorOptions opts = {});

    /**
     * Duplicate check BEFORE execution.  Returns `exact_adjacent` when the call
     * is identical to the immediately previous attempted call, and
     * `repeat_after_success` when that previous call also succeeded.  Should be
     * called exactly once per call, before {@link observe}.
     */
    std::optional<DuplicateFlag> checkDuplicate(const std::string& tool, const json::object& args) const;

    /**
     * Records one executed/guarded call with its result and returns the updated
     * loop flags.  `identityFailed` marks a call that was NOT executed because
     * of a guard (duplicate/repeated-failure rejection): its effect signature
     * is still recorded so repeated rejections close a loop.
     */
    LoopFlags observe(const std::string& tool, const json::object& args, const ResultLike& result);

    /** Flags for the most recent observation of a call identity (or null). */
    std::optional<LoopFlags> flagsFor(const std::string& identity) const;

    /** Resets adjacency/streak state (does not clear remembered content). */
    void reset();

  private:
    struct Last {
      std::string identity{};
      bool ok = false;
    };
    double window_;
    double patternLength_;
    double effectThreshold_;
    std::vector<std::pair<std::string, std::string>> identities_{};  // [call identity, effect signature]
    std::unordered_map<std::string, double> effects_{};
    std::optional<Last> last_{};
    double streak_ = 0;
    std::unordered_map<std::string, LoopFlags> flags_{};
  };

  /** Renders a deterministic one-line loop rejection message. */
  std::string renderLoopFeedback(const LoopFlags& flags);
}
