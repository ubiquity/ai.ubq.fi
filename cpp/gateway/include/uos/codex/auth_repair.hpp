#pragma once
// Port of src/codex/auth-repair.ts.
/**
 * Cross-machine Codex credential repair.
 *
 * The Mac and the VPS keep independent KV auth pools that are seeded from the
 * same synchronized `~/.codex/auth.json`. Because each host refreshes its own
 * access token, one host can end up holding a token the other has already
 * rotated away; the host left behind starts answering 401 until a human
 * re-uploads a credential. This module plans a narrow repair: keep every
 * account that still authenticates, and for an account that does not, adopt a
 * credential read from another host only after that credential has itself been
 * proven to authenticate. A probe that cannot decide an account does not mark
 * it broken: only an explicit rejection or a verified access-token expiry
 * establishes that a replacement is warranted.
 *
 * Validation is deliberately read-only. It is an account-bound `GET` against
 * the Codex usage endpoint, so it never runs inference and never reaches the
 * OAuth token endpoint, which means a check can never rotate a shared refresh
 * token and can never invalidate the other host's copy.
 */
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::codex::auth_repair {
  /** Where a candidate credential was read from, in the order sources are consulted. */
  // "local-auth-json" | "vps-pool" | "vps-auth-json"
  using CodexAuthCandidateSource = std::string;

  /** A credential offered as a replacement for one account. */
  struct CodexAuthCandidate {
    CodexAuthCandidateSource source;
    std::string account_id;
    std::string access_token;
    std::string refresh_token;
    /** Freshness reported by the source, or null when the source has no usable timestamp. */
    std::optional<double> updated_at_ms;
    friend bool operator==(const CodexAuthCandidate&, const CodexAuthCandidate&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthCandidate& value);
  CodexAuthCandidate tag_invoke(const json::value_to_tag<CodexAuthCandidate>&, const json::value& value);

  /** The read-only outcome of authenticating one access token upstream. */
  // `{ outcome: "valid"; status: number }`
  struct CodexAuthProbeResultValid {
    static constexpr std::string_view outcome = "valid";
    double status;
    friend bool operator==(const CodexAuthProbeResultValid&, const CodexAuthProbeResultValid&) = default;
  };
  // `{ outcome: "invalid"; status: number }`
  struct CodexAuthProbeResultInvalid {
    static constexpr std::string_view outcome = "invalid";
    double status;
    friend bool operator==(const CodexAuthProbeResultInvalid&, const CodexAuthProbeResultInvalid&) = default;
  };
  // `{ outcome: "inconclusive"; status: number | null }`
  struct CodexAuthProbeResultInconclusive {
    static constexpr std::string_view outcome = "inconclusive";
    std::optional<double> status;
    friend bool operator==(const CodexAuthProbeResultInconclusive&, const CodexAuthProbeResultInconclusive&) = default;
  };
  using CodexAuthProbeResult = std::variant<CodexAuthProbeResultValid, CodexAuthProbeResultInvalid, CodexAuthProbeResultInconclusive>;
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthProbeResult& value);
  CodexAuthProbeResult tag_invoke(const json::value_to_tag<CodexAuthProbeResult>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthProbeResultInconclusive& value);
  CodexAuthProbeResultInconclusive tag_invoke(const json::value_to_tag<CodexAuthProbeResultInconclusive>&, const json::value& value);

  /** A candidate paired with the evidence used to rank and accept it. */
  struct AssessedCodexAuthCandidate {
    CodexAuthCandidate candidate;
    std::optional<double> access_exp_ms;
    CodexAuthProbeResult probe;
    friend bool operator==(const AssessedCodexAuthCandidate&, const AssessedCodexAuthCandidate&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const AssessedCodexAuthCandidate& value);
  AssessedCodexAuthCandidate tag_invoke(const json::value_to_tag<AssessedCodexAuthCandidate>&, const json::value& value);

  /** One local pool account with its own probe result. */
  struct CodexAuthRepairAccount {
    double slot;
    ::uos::types::CodexAuthState account;
    CodexAuthProbeResult probe;
    /**
     * True when no candidate offers the same refresh token this account already
     * holds, false when one does, and null when no candidate source was read, so
     * the steady-state path never claims a comparison it did not make.
     */
    std::optional<bool> refresh_diverged;
    friend bool operator==(const CodexAuthRepairAccount&, const CodexAuthRepairAccount&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthRepairAccount& value);
  CodexAuthRepairAccount tag_invoke(const json::value_to_tag<CodexAuthRepairAccount>&, const json::value& value);

  // One entry of `rejected`: `{ source: CodexAuthCandidateSource; outcome: CodexAuthProbeResult["outcome"]; status: number | null }`.
  struct CodexAuthRepairRejection {
    CodexAuthCandidateSource source;
    // "valid" | "invalid" | "inconclusive"
    std::string outcome;
    std::optional<double> status;
    friend bool operator==(const CodexAuthRepairRejection&, const CodexAuthRepairRejection&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthRepairRejection& value);
  CodexAuthRepairRejection tag_invoke(const json::value_to_tag<CodexAuthRepairRejection>&, const json::value& value);

  /** A repairable account and the credential chosen for it. */
  struct CodexAuthRepairSelection {
    double slot;
    std::string account_id;
    CodexAuthCandidate candidate;
    std::optional<double> access_exp_ms;
    /** Every candidate for this account that was not chosen, with its probe outcome. */
    std::vector<CodexAuthRepairRejection> rejected;
    friend bool operator==(const CodexAuthRepairSelection&, const CodexAuthRepairSelection&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthRepairSelection& value);
  CodexAuthRepairSelection tag_invoke(const json::value_to_tag<CodexAuthRepairSelection>&, const json::value& value);

  struct CodexAuthRepairPlan {
    struct Unrepairable {
      double slot;
      std::string account_id;
      // "no_candidates" | "no_valid_candidate"
      std::string reason;
      std::vector<CodexAuthRepairRejection> rejected;
      friend bool operator==(const Unrepairable&, const Unrepairable&) = default;
    };
    struct Healthy {
      double slot;
      std::string account_id;
      std::optional<bool> refresh_diverged;
      friend bool operator==(const Healthy&, const Healthy&) = default;
    };
    struct Inconclusive {
      double slot;
      std::string account_id;
      CodexAuthProbeResultInconclusive probe;
      std::optional<bool> refresh_diverged;
      friend bool operator==(const Inconclusive&, const Inconclusive&) = default;
    };

    std::vector<CodexAuthRepairSelection> selections;
    /** Accounts that need a replacement but have no candidate that authenticates. */
    std::vector<Unrepairable> unrepairable;
    /** Accounts whose own credential still authenticates. */
    std::vector<Healthy> healthy;
    /**
     * Accounts kept as they are because their own probe could not decide them
     * and no verified expiry exists: an inconclusive probe is not evidence that
     * the credential is unusable, so nothing may replace it.
     */
    std::vector<Inconclusive> inconclusive;
    friend bool operator==(const CodexAuthRepairPlan&, const CodexAuthRepairPlan&) = default;
  };
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthRepairPlan::Unrepairable& value);
  CodexAuthRepairPlan::Unrepairable tag_invoke(const json::value_to_tag<CodexAuthRepairPlan::Unrepairable>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthRepairPlan::Healthy& value);
  CodexAuthRepairPlan::Healthy tag_invoke(const json::value_to_tag<CodexAuthRepairPlan::Healthy>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthRepairPlan::Inconclusive& value);
  CodexAuthRepairPlan::Inconclusive tag_invoke(const json::value_to_tag<CodexAuthRepairPlan::Inconclusive>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const CodexAuthRepairPlan& value);
  CodexAuthRepairPlan tag_invoke(const json::value_to_tag<CodexAuthRepairPlan>&, const json::value& value);

  /**
   * A candidate is acceptable only when the upstream accepted its access token
   * *and* that token still has time left on it. A token can pass a probe and
   * still be useless moments later, so the expiry read from the token itself is
   * checked independently of the network result.
   */
  bool isCodexAuthCandidateUsable(const AssessedCodexAuthCandidate& assessed, double nowMs);

  /**
   * True only when the evidence establishes that the local credential is
   * unusable: the upstream rejected it outright, or the access token's own
   * expiry has already passed. A probe that answered neither way (a timeout, a
   * 403, a 5xx) decides nothing, so it never justifies a replacement by itself;
   * the same predicate gates the CLI's paid candidate reads so an inconclusive
   * slot cannot trigger them.
   */
  bool needsCodexAuthReplacement(const CodexAuthProbeResult& probe, std::optional<double> accessExpMs, double nowMs);

  /**
   * Orders usable candidates so the caller can take the first one: most
   * remaining access-token life first, then the freshest source timestamp, then
   * the source order the caller supplied. A credential with no readable expiry
   * sorts last because it carries the least evidence.
   */
  std::vector<AssessedCodexAuthCandidate> rankCodexAuthCandidates(const std::vector<AssessedCodexAuthCandidate>& candidates, double nowMs);

  // The `input` of planCodexAuthRepair.
  struct PlanCodexAuthRepairInput {
    std::vector<CodexAuthRepairAccount> accounts;
    std::vector<AssessedCodexAuthCandidate> candidates;
    double nowMs;
  };

  /**
   * Decides, per account, whether to keep the local credential, adopt another
   * host's, or report that the account cannot be repaired. Accounts that still
   * authenticate are never rewritten, and a slot whose probe was inconclusive is
   * preserved rather than repaired: a healthy slot cannot be disturbed by a
   * repair run, and neither can a slot whose health is simply unknown.
   */
  // Every element of `input.candidates` is its own object: the chosen candidate is excluded from
  // `rejected` by its position, which is what the TS identity test (`assessed !== chosen`) does
  // for an array of distinct objects.
  CodexAuthRepairPlan planCodexAuthRepair(const PlanCodexAuthRepairInput& input);

  /**
   * Applies a plan to the pool. Only selected accounts are replaced, matched by
   * account id, so slot order, the other account, and every unrelated KV entry
   * are left untouched. The expiry is checked again here because the plan may
   * have spent time in candidate assessment. Returns null when a selected
   * candidate is no longer usable or the pool could not represent the result;
   * callers must treat either case as "write nothing".
   *
   * Each replaced account carries the caller's `nowMs` so a repair is
   * reproducible in tests; the shared `upsertCodexAuthAccount` helper owns the
   * pool-level timestamp, exactly as it does for a gateway refresh.
   */
  std::optional<::uos::types::CodexAuthPoolState> applyCodexAuthPlan(const ::uos::types::CodexAuthPoolState& pool, const CodexAuthRepairPlan& plan, double nowMs);

  /** Reads a candidate from an `auth.json` document. */
  // `value: unknown`: nullptr is undefined.
  std::optional<CodexAuthCandidate> codexAuthCandidateFromAuthJson(const json::value& value, CodexAuthCandidateSource source, std::optional<double> updatedAtMs);
  std::optional<CodexAuthCandidate> codexAuthCandidateFromAuthJson(const json::value* value, CodexAuthCandidateSource source, std::optional<double> updatedAtMs);

  /** Reads every candidate from a stored auth pool. */
  // `value: unknown`: nullptr is undefined. Throws CodexError for a malformed native owner (parseCodexAuthPool).
  std::vector<CodexAuthCandidate> codexAuthCandidatesFromPool(const json::value& value, CodexAuthCandidateSource source);
  std::vector<CodexAuthCandidate> codexAuthCandidatesFromPool(const json::value* value, CodexAuthCandidateSource source);

  /** A stable identity for de-duplicating the same credential seen from several sources. */
  std::string codexAuthCandidateKey(const CodexAuthCandidate& candidate);
}
