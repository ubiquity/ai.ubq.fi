#pragma once
// Port of src/provider/claude-code-usage.ts.
//
// Direct Claude subscription usage reads. Anthropic exposes the same OAuth
// usage surface used by Claude Code's account panel; it is independent of an
// inference request and therefore never spends a subscription turn. The
// endpoint is undocumented, so this module owns its narrow wire contract,
// bounded failures, and conversion into the gateway's existing quota windows.
//
// ClaudeCodeUsageErrorKind is defined in uos/provider/claude_code_usage_fwd.hpp (included here).
// ClaudeCodeAuthHeaders and ClaudeCodeFetch come from uos/provider/claude_code_fwd.hpp, so this
// header does not include uos/provider/claude_code.hpp (the .cpp does, for the URL and header
// constants). `options = {}` and `observedAtMs = Date.now()` defaults are overloads.
#include "uos/provider/claude_code_fwd.hpp"
#include "uos/provider/claude_code_quota.hpp"
#include "uos/provider/claude_code_usage_fwd.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace uos::provider::claude_code_usage {
  // `${CLAUDE_CODE_BASE_URL}/api/oauth/usage`
  inline constexpr std::string_view CLAUDE_CODE_USAGE_URL = "https://api.anthropic.com/api/oauth/usage";
  /** The endpoint rate-limits unrecognized clients more aggressively. */
  inline constexpr std::string_view CLAUDE_CODE_USAGE_USER_AGENT = "claude-code/2.1.310";

  // `export type ClaudeCodeUsageErrorKind`: claude_code_usage_fwd.hpp.

  struct ClaudeCodeUsageSuccess {
    static constexpr bool ok = true;
    double status;
    double observed_at_ms;
    claude_code_quota::ClaudeCodeQuotaObservation observation;
    friend bool operator==(const ClaudeCodeUsageSuccess&, const ClaudeCodeUsageSuccess&) = default;
  };

  struct ClaudeCodeUsageFailure {
    static constexpr bool ok = false;
    ClaudeCodeUsageErrorKind kind;
    std::optional<double> status;  // number | null
    double observed_at_ms;
    std::optional<std::string> message;    // string | null
    std::optional<double> retry_after_ms;  // number | null
    friend bool operator==(const ClaudeCodeUsageFailure&, const ClaudeCodeUsageFailure&) = default;
  };

  // `ClaudeCodeUsageSuccess | ClaudeCodeUsageFailure` (discriminated by the static `ok`).
  using ClaudeCodeUsageResult = std::variant<ClaudeCodeUsageSuccess, ClaudeCodeUsageFailure>;

  /** Direct meters retain their own complete snapshot; canonical guards stay restricted to 5h/7d. */
  // `(payload: unknown, observedAtMs: number = Date.now())`: nullptr is undefined; std::nullopt is null.
  std::optional<claude_code_quota::ClaudeCodeQuotaObservation> parseClaudeCodeUsagePayload(const json::value& payload);
  std::optional<claude_code_quota::ClaudeCodeQuotaObservation> parseClaudeCodeUsagePayload(const json::value* payload);
  std::optional<claude_code_quota::ClaudeCodeQuotaObservation> parseClaudeCodeUsagePayload(const json::value& payload, double observedAtMs);
  std::optional<claude_code_quota::ClaudeCodeQuotaObservation> parseClaudeCodeUsagePayload(const json::value* payload, double observedAtMs);

  // `options: Readonly<{ fetcher?: ClaudeCodeFetch; signal?: AbortSignal; now?: () => number }>` of
  // fetchClaudeCodeUsage.
  struct FetchClaudeCodeUsageOptions {
    claude_code::ClaudeCodeFetch fetcher{};  // empty: the global fetch
    std::optional<rt::AbortSignal> signal{};
    std::function<double()> now{};  // empty: Date.now
  };

  /**
   * Read the subscription usage windows with the OAuth credential. Every failure
   * is returned as typed data so callers can distinguish an absent profile scope
   * from an upstream or transport failure without throwing or mutating auth.
   */
  rt::Task<ClaudeCodeUsageResult> fetchClaudeCodeUsage(claude_code::ClaudeCodeAuthHeaders credential);
  rt::Task<ClaudeCodeUsageResult> fetchClaudeCodeUsage(claude_code::ClaudeCodeAuthHeaders credential, FetchClaudeCodeUsageOptions options);
}
