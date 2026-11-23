#pragma once
// Port of src/harmony/classifier.ts.
/**
 * Bootstrap classifier contract (plan m01 protocol probe).
 *
 * The ambiguous-progress classifier is a single bounded, data-only Harmony
 * request to the exact model `gpt-oss-120b` with:
 * - no tools,
 * - no structured-output wrapper (`response_format`),
 * - a small completion limit,
 * - reasoning effort starting at `low` and compared with `medium`.
 *
 * The assistant's complete trimmed final content must be the literal `true`
 * or `false`, matched full-string with the anchored case-insensitive
 * expression `/^(true|false)$/i`.  Transport failures, refusals, tool calls,
 * surrounding prose, JSON wrappers, or a model mismatch all become `unknown`.
 * Nothing in this module changes bootstrap control authority: it only
 * produces evidence for the deterministic rules in m06.
 */
#include "uos/harmony/types.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/regex.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace uos::harmony::classifier {
  inline const rt::Regex BOOTSTRAP_CLASSIFIER_BOOLEAN_PATTERN{"^(true|false)$", "i"};

  /** Small output bound for the classifier request. */
  inline constexpr double BOOTSTRAP_CLASSIFIER_MAX_OUTPUT_TOKENS = 128;

  struct BootstrapObservation {
    std::string runId;
    std::optional<double> generation;
    std::optional<std::string> phase;
    std::optional<std::string> milestone;
    std::optional<std::string> failureFingerprint;
    std::optional<std::string> gitSha;
    std::optional<double> ledgerVersion;
    std::optional<std::string> retryState;
    std::optional<std::string> verificationEvidence;
    friend bool operator==(const BootstrapObservation&, const BootstrapObservation&) = default;
  };

  // `{ verdict: "true"; raw: string }`
  struct BootstrapClassifierVerdictTrue {
    static constexpr std::string_view verdict = "true";
    std::string raw;
    friend bool operator==(const BootstrapClassifierVerdictTrue&, const BootstrapClassifierVerdictTrue&) = default;
  };
  // `{ verdict: "false"; raw: string }`
  struct BootstrapClassifierVerdictFalse {
    static constexpr std::string_view verdict = "false";
    std::string raw;
    friend bool operator==(const BootstrapClassifierVerdictFalse&, const BootstrapClassifierVerdictFalse&) = default;
  };
  // `{ verdict: "unknown"; raw: string | null; reason: string }`
  struct BootstrapClassifierVerdictUnknown {
    static constexpr std::string_view verdict = "unknown";
    std::optional<std::string> raw;
    std::string reason;
    friend bool operator==(const BootstrapClassifierVerdictUnknown&, const BootstrapClassifierVerdictUnknown&) = default;
  };
  using BootstrapClassifierVerdict = std::variant<BootstrapClassifierVerdictTrue, BootstrapClassifierVerdictFalse, BootstrapClassifierVerdictUnknown>;

  /**
   * Parses the trimmed final content as a full literal boolean.  Any wrapping,
   * prose, JSON, or other value is `unknown`; only the anchored expression
   * `/^(true|false)$/i` over the trimmed string is accepted.
   */
  // `value: unknown`: nullptr is undefined.
  BootstrapClassifierVerdict parseBootstrapClassifierVerdict(const json::value& value);
  BootstrapClassifierVerdict parseBootstrapClassifierVerdict(const json::value* value);

  struct BootstrapClassifierRequestOptions {
    BootstrapObservation observation;
    std::string decisionDefinition;
    // "low" | "medium"
    std::string reasoningEffort;
    std::optional<double> maxCompletionTokens{};
    std::optional<std::string> currentDate{};
  };

  /**
   * Builds the one bounded, data-only request: no tools, no response_format, a
   * small completion limit, and the observation carried as inert data in the
   * user message while the instruction and protocol live in system/developer.
   */
  json::object buildBootstrapClassifierRequest(const BootstrapClassifierRequestOptions& options);

  /**
   * Converts a normalized response into a classifier verdict.  A refusal,
   * any tool call, absent final content, or a model mismatch (already enforced
   * by the normalizer) is `unknown`; a mismatch surfaces as `unknown` with the
   * reason attached.
   */
  BootstrapClassifierVerdict verdictFromBootstrapResponse(const types::NormalizedAssistantResponse& response);
}
