#pragma once
// Port of src/authority/native-codex-429.ts.
// Source-owned Codex 429 classification and quota decisions. Native owners retain I/O, CAS, clocks and credentials.
#include "uos/authority/native_codex_contracts.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/http.hpp"

#include <optional>

namespace uos::authority::native_codex_429 {
  // `RetryAfterDeadline` is module-private in the TS; it is the return type of futureRetryAfterDeadline.
  struct RetryAfterDeadline {
    double deadlineMs;
    /** A delta timeout is useful for routing, but not a durable reset identity. */
    bool isStable;
    friend bool operator==(const RetryAfterDeadline&, const RetryAfterDeadline&) = default;
  };

  // `RetryAfterDeadline | null`
  std::optional<RetryAfterDeadline> futureRetryAfterDeadline(const rt::Headers& headers, double now);

  struct Codex429Classification {
    rt::Response response;
    bool usageLimitReached;
    std::optional<double> retryAtMs;
    std::optional<native_codex_contracts::CodexQuotaBlockSource> quotaBlockSource;
    /** Whether `retryAtMs` is a canonical absolute deadline (not a provider generation by itself). */
    bool resetDeadlineIsStable;
    /** Conflicting deadline signals permanently fence this observation from redemption. */
    bool resetDeadlineConflict;
  };

  /**
   * Read a bounded error body and replace the response so callers retain the
   * OpenAI-compatible upstream payload after routing has classified it.
   */
  [[nodiscard]] rt::Task<Codex429Classification> classifyCodex429(rt::Response response, double now);

  /** A probe lease minted by this isolate: unlike a stored legacy record, every field is present. */
  // `Readonly<{ token; expires_at_ms; generation; circuit; quota_class: CodexQuotaClass | null }>`: the same
  // shape as native-codex-contracts' CodexProbeLease, whose C++ struct it names (quota_class is never undefined
  // on a known lease).
  using CodexKnownProbeLease = native_codex_contracts::CodexProbeLease;

  /**
   * An expired circuit is represented by a fenced half-open probe. A
   * non-blocking 429 must release that old circuit, while the generation and
   * token checks prevent a stale probe from clearing a newer claim.
   */
  std::optional<native_codex_contracts::CodexRoutingSlot> nonBlockingQuotaProbeSlot(const native_codex_contracts::CodexAccountRoutingState& state,
                                                                                   const native_codex_contracts::CodexRoutingAccount& account);

  /**
   * Persist the class-scoped quota block for a 429 that proves exhaustion. The
   * account stays fenced, and an ordinary request that predates a foreign
   * half-open claim must not replace that lease or admit a parallel probe.
   */
  std::optional<native_codex_contracts::CodexRoutingSlot> quotaBlockedSlot(const native_codex_contracts::CodexAccountRoutingState& state,
                                                                          const native_codex_contracts::CodexRoutingAccount& account,
                                                                          const Codex429Classification& parsed, double now, bool recoveryProbe);

  /** Apply the shared slot decision with the host's explicit durable-row timestamp. */
  std::optional<native_codex_contracts::CodexAccountRoutingState> quotaBlockTransition(const native_codex_contracts::CodexAccountRoutingState& state,
                                                                                      const native_codex_contracts::CodexRoutingAccount& account,
                                                                                      const Codex429Classification& parsed, double now, bool recoveryProbe,
                                                                                      double updatedAtMs);

  /** Release a matching non-blocking probe without changing another class's claim. */
  std::optional<native_codex_contracts::CodexAccountRoutingState> releaseNonBlockingQuotaProbe(const native_codex_contracts::CodexAccountRoutingState& state,
                                                                                              const native_codex_contracts::CodexRoutingAccount& account,
                                                                                              double updatedAtMs);
}
