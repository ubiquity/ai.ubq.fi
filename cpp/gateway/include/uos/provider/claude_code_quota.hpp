#pragma once
// Port of src/provider/claude-code-quota.ts.
//
// `kv: Kv | null` is std::shared_ptr<rt::Kv> (a null pointer is null); `kv: Kv` is non-null.
// `binding?: ClaudeCodeAccountBinding` and `nowMs = Date.now()` are overloads (a synchronous
// function's widest overload takes the binding as const std::optional<...>&). `x: T | null`
// parameters are const std::optional<T>&. `Partial<Record<K, V>>` and `Record<string, V>`
// members are authority::contracts::Record<V> (JS property order, `obj[k] = v` is set(),
// toEqual-style ==). The parsers of stored values take rt::KvValue (a json::value or a
// std::string converts implicitly; a default-constructed KvValue is undefined).
#include "uos/authority/contracts.hpp"
#include "uos/provider/claude_code_accounts.hpp"
#include "uos/provider/claude_code_usage_fwd.hpp"
#include "uos/provider/claude_code_usage_schema.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/kv.hpp"
#include "uos/rt/prelude.hpp"

#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

namespace uos::provider::claude_code_quota {
  inline const rt::KvKey CLAUDE_CODE_QUOTA_KV_KEY{"ubq_ai", "claude_code_quota"};
  /** Margin after an upstream reset before the tier is retried. */
  inline constexpr double CLAUDE_CODE_RESET_MARGIN_MS = 5 * 60'000;
  /** Floor for a durable block that arrived without a usable reset instant. */
  inline constexpr double CLAUDE_CODE_MIN_BLOCK_MS = 5 * 60'000;
  /** Cooldown for a generic rate limit with no durable window evidence. */
  inline constexpr double CLAUDE_CODE_TRANSIENT_COOLDOWN_MS = 60'000;
  /**
   * Cooldown after a configuration-level rejection (billing blocked, unknown
   * workspace). These do not clear on their own upstream, but the gateway must
   * not hammer the endpoint for every request while the operator fixes the
   * account; each new attempt is bounded by this window.
   */
  inline constexpr double CLAUDE_CODE_CONFIG_COOLDOWN_MS = 10 * 60'000;

  inline constexpr std::array<std::string_view, 2> CLAUDE_CODE_WINDOWS = {"5h", "7d"};
  // `(typeof CLAUDE_CODE_WINDOWS)[number]`
  using ClaudeCodeWindowId = std::string;

  /** Console API keys are metered with the standard per-family rate-limit headers. */
  inline constexpr std::array<std::string_view, 3> CLAUDE_CODE_STANDARD_FAMILIES = {"requests", "input-tokens", "output-tokens"};
  // `(typeof CLAUDE_CODE_STANDARD_FAMILIES)[number]`
  using ClaudeCodeStandardFamily = std::string;

  struct ClaudeCodeQuotaWindow {
    std::optional<std::string> status;     // string | null
    std::optional<double> utilization;     // number | null
    std::optional<double> reset_at_ms;     // number | null
    double observed_at_ms;
    friend bool operator==(const ClaudeCodeQuotaWindow&, const ClaudeCodeQuotaWindow&) = default;
  };

  struct ClaudeCodeStandardWindow {
    std::optional<double> limit;        // number | null
    std::optional<double> remaining;    // number | null
    std::optional<double> reset_at_ms;  // number | null
    double observed_at_ms;
    friend bool operator==(const ClaudeCodeStandardWindow&, const ClaudeCodeStandardWindow&) = default;
  };

  struct ClaudeCodeQuotaError {
    std::string kind;
    std::optional<double> status;        // number | null
    std::optional<std::string> message;  // string | null
    double at_ms;
    friend bool operator==(const ClaudeCodeQuotaError&, const ClaudeCodeQuotaError&) = default;
  };

  // `Readonly<Partial<Record<ClaudeCodeWindowId, ClaudeCodeQuotaWindow>>>`
  using ClaudeCodeQuotaWindows = authority::contracts::Record<ClaudeCodeQuotaWindow>;
  // `Readonly<Partial<Record<ClaudeCodeStandardFamily, ClaudeCodeStandardWindow>>>`
  using ClaudeCodeStandardWindows = authority::contracts::Record<ClaudeCodeStandardWindow>;

  // Every TS construction writes `usage_snapshot` (when present) after `observed_at_ms`, and
  // claude-capacity-history hashes `JSON.stringify(observation)` into a stored point id, so the
  // member is declared (and serialized) last.
  struct ClaudeCodeQuotaObservation {
    ClaudeCodeQuotaWindows windows;
    ClaudeCodeStandardWindows standard;
    std::optional<std::string> representative_claim;  // string | null
    double observed_at_ms;
    std::optional<claude_code_usage_schema::ClaudeCodeUsageSnapshot> usage_snapshot{};
    friend bool operator==(const ClaudeCodeQuotaObservation&, const ClaudeCodeQuotaObservation&) = default;
  };

  // `Omit<ClaudeCodeQuotaObservation, "usage_snapshot"> & Readonly<{ source: "response_headers" }>`.
  // Both TS constructions write `source` first, so it is declared (and serialized) first.
  struct ClaudeCodeHeaderObservation {
    std::string source = "response_headers";  // `"response_headers"`
    ClaudeCodeQuotaWindows windows;
    ClaudeCodeStandardWindows standard;
    std::optional<std::string> representative_claim;  // string | null
    double observed_at_ms;
    friend bool operator==(const ClaudeCodeHeaderObservation&, const ClaudeCodeHeaderObservation&) = default;
  };

  // `ClaudeCodeAccountIdentity & Readonly<{ v: 1; observation_id; source_observed_at_ms;
  // sampled_at_ms; response_status; state; observation; source?: "response_headers" }>`: the
  // identity members come first, as the TS spreads them first.
  struct ClaudeCodeAcceptedObservation : claude_code_accounts::ClaudeCodeAccountIdentity {
    double v = 1;  // `1`
    std::string observation_id;
    std::optional<double> source_observed_at_ms;  // number | null
    double sampled_at_ms;
    std::optional<double> response_status;  // number | null
    std::string state;                      // "available" | "unavailable"
    ClaudeCodeQuotaObservation observation;
    std::optional<std::string> source{};  // `"response_headers"`
    friend bool operator==(const ClaudeCodeAcceptedObservation&, const ClaudeCodeAcceptedObservation&) = default;
  };

  // `type ObservationRecorder = (kv: Kv, event: ClaudeCodeAcceptedObservation) => Promise<void>`
  using ObservationRecorder = std::function<rt::Task<void>(std::shared_ptr<rt::Kv> kv, ClaudeCodeAcceptedObservation event)>;

  /** Install at application module initialization, before its first inference, never on an admin read. */
  // Returns the disposer `() => void`. The TS compares recorders by identity (installing the
  // installed recorder again is allowed); here two recorders are the same when both target the
  // same plain function pointer, and any other two are different. Throws
  // Error("Claude observation recorder is already installed") as the TS does.
  std::function<void()> registerClaudeCodeObservationRecorder(ObservationRecorder recorder);

  // `(binding?: ClaudeCodeAccountBinding)`, return type inferred:
  // `["uos_ai", "claude_code_quota", "v1", binding.account_id, binding.account_cohort_id]` for a
  // binding, CLAUDE_CODE_QUOTA_KV_KEY without one (std::nullopt is undefined).
  rt::KvKey claudeCodeQuotaKey();
  rt::KvKey claudeCodeQuotaKey(const std::optional<claude_code_accounts::ClaudeCodeAccountBinding>& binding);

  // ClaudeCodeUsageState and ClaudeCodePoolProof follow ClaudeCodeQuotaState in the TS; C++ needs
  // them complete before the state's members.

  /** The most recent direct OAuth usage read. Never an inference-routing guard. */
  struct ClaudeCodeUsageState {
    std::string source = "oauth_usage";  // `"oauth_usage"`
    // `"ok" | ClaudeCodeUsageErrorKind`
    claude_code_usage::ClaudeCodeUsageErrorKind state;
    std::optional<double> status;        // number | null
    std::optional<std::string> message;  // string | null
    double attempted_at_ms;
    std::optional<double> observed_at_ms;  // number | null
    std::optional<double> retry_at_ms{};
    friend bool operator==(const ClaudeCodeUsageState&, const ClaudeCodeUsageState&) = default;
  };

  struct ClaudeCodePoolProof {
    std::string kind;                     // "auth_invalid" | "quota_exhausted"
    std::optional<std::string> model_id;  // string | null
    double observed_at_ms;
    double expires_at_ms;
    std::string evidence_generation;
    friend bool operator==(const ClaudeCodePoolProof&, const ClaudeCodePoolProof&) = default;
  };

  struct ClaudeCodeQuotaState {
    std::optional<ClaudeCodeHeaderObservation> response_headers{};
    std::optional<claude_code_usage_schema::ClaudeCodeUsageSnapshot> usage_snapshot{};
    // `pool_proofs?: Readonly<Record<string, ClaudeCodePoolProof>>`
    std::optional<authority::contracts::Record<ClaudeCodePoolProof>> pool_proofs{};
    ClaudeCodeQuotaWindows windows;
    ClaudeCodeStandardWindows standard;
    std::optional<ClaudeCodeUsageState> usage;          // ... | null
    std::optional<std::string> representative_claim;   // string | null
    std::optional<double> blocked_until_ms;             // number | null
    std::optional<ClaudeCodeWindowId> block_window;     // ... | null
    std::optional<double> cooldown_until_ms;            // number | null
    std::optional<ClaudeCodeQuotaError> last_error;     // ... | null
    double updated_at_ms;
    friend bool operator==(const ClaudeCodeQuotaState&, const ClaudeCodeQuotaState&) = default;
  };

  inline constexpr double CLAUDE_CODE_AUTH_INVALID_PROOF_TTL_MS = 5 * 60'000;

  // `(state: ClaudeCodeQuotaState | null, binding, model: string, nowMs = Date.now())`; std::nullopt
  // is null.
  std::optional<ClaudeCodePoolProof> claudeCodePoolProofForModel(const std::optional<ClaudeCodeQuotaState>& state,
                                                                 const claude_code_accounts::ClaudeCodeAccountBinding& binding, const std::string& model);
  std::optional<ClaudeCodePoolProof> claudeCodePoolProofForModel(const std::optional<ClaudeCodeQuotaState>& state,
                                                                 const claude_code_accounts::ClaudeCodeAccountBinding& binding, const std::string& model,
                                                                 double nowMs);

  /**
   * Read the rate-limit headers from one upstream response. Returns null when
   * the response carried no usable observation, so an absent header family never
   * overwrites a previous observation with emptiness. Subscription traffic
   * carries the unified 5h/7d windows; Console API keys carry the standard
   * per-family limits, so both are kept when present.
   */
  std::optional<ClaudeCodeQuotaObservation> observeClaudeCodeQuota(const rt::Headers& headers);
  std::optional<ClaudeCodeQuotaObservation> observeClaudeCodeQuota(const rt::Headers& headers, double nowMs);

  /** Sanitize an upstream error message for operator display; bounded, no headers. */
  // `(raw: unknown, maxLength = 240): string | null`; std::nullopt is null. The std::string and
  // `string | null` overloads cover the callers that pass known strings (a string literal argument
  // is ambiguous: pass a std::string).
  std::optional<std::string> sanitizeClaudeCodeErrorMessage(const rt::KvValue& raw, double maxLength = 240);
  std::optional<std::string> sanitizeClaudeCodeErrorMessage(const std::string& raw, double maxLength = 240);
  std::optional<std::string> sanitizeClaudeCodeErrorMessage(const std::optional<std::string>& raw, double maxLength = 240);

  // `(value: unknown): ClaudeCodeQuotaError | null`; std::nullopt is null.
  std::optional<ClaudeCodeQuotaError> parseClaudeCodeQuotaError(const rt::KvValue& value);

  // `Readonly<{ window: ClaudeCodeWindowId; reset_at_ms: number | null }>`: the durable-exhaustion
  // shape shared by claudeCodeExhaustionFromResponse, mergeClaudeCodeQuotaState's `block`,
  // QuotaOutcome::exhaustion and claude_code::ClaudeCodeResponseClassification::exhaustion.
  struct ClaudeCodeQuotaExhaustion {
    ClaudeCodeWindowId window;
    std::optional<double> reset_at_ms;  // number | null
    friend bool operator==(const ClaudeCodeQuotaExhaustion&, const ClaudeCodeQuotaExhaustion&) = default;
  };

  /**
   * Durable exhaustion requires a rejection plus a window whose reported state is
   * not `allowed` (or whose utilization is at 100 percent). A 429 without that
   * evidence is request-local and must not become a stored block.
   */
  // `observation: ClaudeCodeQuotaObservation | null`; std::nullopt is null.
  std::optional<ClaudeCodeQuotaExhaustion> claudeCodeExhaustionFromResponse(double status, const std::optional<ClaudeCodeQuotaObservation>& observation);

  // `(value: unknown): ClaudeCodeQuotaState | null`; std::nullopt is null.
  std::optional<ClaudeCodeQuotaState> parseClaudeCodeQuotaState(const rt::KvValue& value);

  rt::Task<std::optional<ClaudeCodeQuotaState>> loadClaudeCodeQuotaState(std::shared_ptr<rt::Kv> kv);
  rt::Task<std::optional<ClaudeCodeQuotaState>> loadClaudeCodeQuotaState(std::shared_ptr<rt::Kv> kv,
                                                                         std::optional<claude_code_accounts::ClaudeCodeAccountBinding> binding);

  /**
   * Fold one upstream observation into stored state. A window observation newer
   * than the stored one wins; an older one is ignored rather than resurrecting
   * stale allowance numbers. An active block is preserved until its reset time
   * has passed and a newer observation proves the window moved on.
   */
  // `previous: ClaudeCodeQuotaState | null`, `block: ... | null`, `nowMs = Date.now()`.
  ClaudeCodeQuotaState mergeClaudeCodeQuotaState(const std::optional<ClaudeCodeQuotaState>& previous, const ClaudeCodeQuotaObservation& observation,
                                                 const std::optional<ClaudeCodeQuotaExhaustion>& block);
  ClaudeCodeQuotaState mergeClaudeCodeQuotaState(const std::optional<ClaudeCodeQuotaState>& previous, const ClaudeCodeQuotaObservation& observation,
                                                 const std::optional<ClaudeCodeQuotaExhaustion>& block, double nowMs);

  /** True while the stored durable block still covers the present instant. */
  bool claudeCodeQuotaBlocked(const std::optional<ClaudeCodeQuotaState>& state);
  bool claudeCodeQuotaBlocked(const std::optional<ClaudeCodeQuotaState>& state, double nowMs);

  /** True during a short request-local cooldown recorded from a generic 429. */
  bool claudeCodeQuotaCoolingDown(const std::optional<ClaudeCodeQuotaState>& state);
  bool claudeCodeQuotaCoolingDown(const std::optional<ClaudeCodeQuotaState>& state, double nowMs);

  /**
   * Persist one upstream outcome's evidence. Success paths only refresh window
   * observations and clear a recorded configuration error; a durable block needs
   * the caller's exhausted-window evidence, a generic rate limit only sets a
   * short cooldown, and a configuration rejection (billing, workspace) sets the
   * longer bounded config cooldown plus its displayable detail, so a transient
   * failure can never be persisted as durable exhaustion.
   */
  // The TS folds null and undefined together for every `?: T | null` member here.
  struct QuotaOutcome {
    // `Readonly<{ retry_after_ms: number | null }>`
    struct Transient {
      std::optional<double> retry_after_ms;  // number | null
      friend bool operator==(const Transient&, const Transient&) = default;
    };
    // `Readonly<{ kind: string; status: number | null; message: string | null; retry_after_ms?: number | null }>`
    struct ConfigError {
      std::string kind;
      std::optional<double> status;        // number | null
      std::optional<std::string> message;  // string | null
      std::optional<double> retry_after_ms{};
      friend bool operator==(const ConfigError&, const ConfigError&) = default;
    };

    std::optional<std::string> model_id{};
    std::optional<bool> auth_invalid{};
    std::optional<double> response_observed_at_ms{};
    std::optional<ClaudeCodeQuotaObservation> observation;  // required: ClaudeCodeQuotaObservation | null
    std::optional<claude_code_accounts::ClaudeCodeAccountBinding> binding{};
    std::optional<double> response_status{};
    /** Durable exhaustion claimed by the caller's classification. */
    std::optional<ClaudeCodeQuotaExhaustion> exhaustion{};
    /** Generic rate limit with no durable window evidence. */
    std::optional<Transient> transient{};
    /** Configuration rejection: billing, workspace, or credential scoping. */
    std::optional<ConfigError> config_error{};
    /** The tier served this request; clears any recorded configuration error. */
    std::optional<bool> served{};
    std::optional<double> nowMs{};
    /** A direct OAuth usage read; metadata only and never part of routing guards. */
    std::optional<ClaudeCodeUsageState> usage{};
    /** Meter provenance is set only at the ordinary inference response-header seam. */
    std::optional<std::string> observation_source{};  // `"response_headers"`
    /** Internal CAS retry budget, never an upstream retry. */
    std::optional<double> attempts{};
  };

  // `kv: Kv | null`; std::nullopt is null.
  rt::Task<std::optional<ClaudeCodeQuotaState>> recordClaudeCodeQuotaOutcome(std::shared_ptr<rt::Kv> kv, QuotaOutcome input);

  /**
   * Operator switch: drop the durable block, the cooldown, and the recorded
   * configuration error so the next request retries the tier immediately. The
   * observed window utilization is preserved because it is upstream truth, not a
   * gateway guard.
   */
  rt::Task<std::optional<ClaudeCodeQuotaState>> clearClaudeCodeQuotaGuards(std::shared_ptr<rt::Kv> kv);
  rt::Task<std::optional<ClaudeCodeQuotaState>> clearClaudeCodeQuotaGuards(std::shared_ptr<rt::Kv> kv,
                                                                           std::optional<claude_code_accounts::ClaudeCodeAccountBinding> binding);

  // ---- JSON conversions of the snake_case records (members in declaration order; the
  // contracts::Record members as objects in their property order) ----------------------------
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeQuotaWindow& value);
  ClaudeCodeQuotaWindow tag_invoke(const json::value_to_tag<ClaudeCodeQuotaWindow>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeStandardWindow& value);
  ClaudeCodeStandardWindow tag_invoke(const json::value_to_tag<ClaudeCodeStandardWindow>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeQuotaError& value);
  ClaudeCodeQuotaError tag_invoke(const json::value_to_tag<ClaudeCodeQuotaError>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeQuotaObservation& value);
  ClaudeCodeQuotaObservation tag_invoke(const json::value_to_tag<ClaudeCodeQuotaObservation>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeHeaderObservation& value);
  ClaudeCodeHeaderObservation tag_invoke(const json::value_to_tag<ClaudeCodeHeaderObservation>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeAcceptedObservation& value);
  ClaudeCodeAcceptedObservation tag_invoke(const json::value_to_tag<ClaudeCodeAcceptedObservation>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeUsageState& value);
  ClaudeCodeUsageState tag_invoke(const json::value_to_tag<ClaudeCodeUsageState>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodePoolProof& value);
  ClaudeCodePoolProof tag_invoke(const json::value_to_tag<ClaudeCodePoolProof>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ClaudeCodeQuotaState& value);
  ClaudeCodeQuotaState tag_invoke(const json::value_to_tag<ClaudeCodeQuotaState>&, const json::value& value);
}
