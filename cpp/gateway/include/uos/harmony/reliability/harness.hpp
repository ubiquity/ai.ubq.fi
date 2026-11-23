#pragma once
// Port of src/harmony/reliability/harness.ts.
/**
 * Canonical reliability harness (plan m05): detailed validation, duplicate and
 * semantic-loop guards, bounded transient retries, verification requirements,
 * structured task state and model-facing transcript compaction. Invalid calls
 * are never executed; guarded duplicates are recorded without counting tool
 * errors. Unverified writes and unresolved command/edit failures block finals.
 *
 * The transport and tool backends are injected: no environment variable, CLI
 * flag, secret or external call is used here. The authoritative conversation
 * stays full; compaction only changes the next model-facing request.
 */
#include "uos/harmony/adapter.hpp"
#include "uos/harmony/conversation.hpp"
#include "uos/harmony/reliability/context.hpp"
#include "uos/harmony/reliability/failure.hpp"
#include "uos/harmony/reliability/retry.hpp"
#include "uos/harmony/reliability/state.hpp"
#include "uos/harmony/reliability/verify.hpp"
#include "uos/harmony/tools/backend.hpp"
#include "uos/harmony/tools/result.hpp"
#include "uos/harmony/types.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::harmony::reliability::harness {
  struct HarnessEventModelRequest {
    static constexpr std::string_view type = "model_request";
    double id;
    std::string mode;
    adapter::BuiltHarmonyRequest built;
    double estimatedTokens;
  };
  struct HarnessEventModelResponse {
    static constexpr std::string_view type = "model_response";
    double requestId;
    types::NormalizedAssistantResponse normalized;
    double estimatedTokens;
  };
  struct HarnessEventToolCall {
    static constexpr std::string_view type = "tool_call";
    std::string id;
    std::string tool;
    json::object arguments;
    bool valid;
    std::optional<std::string> invalidReason{};
    rt::Nullable<std::string> repeated{};
  };
  struct HarnessEventToolResult {
    static constexpr std::string_view type = "tool_result";
    std::string id;
    tools::result::ToolResult result;
    std::optional<double> durationMs{};
  };
  struct HarnessEventGuard {
    static constexpr std::string_view type = "guard";
    verify::FinalRequirementKind kind;
    std::string message;
    double attempt;
    state::TaskPhase phase;
  };
  struct HarnessEventFinal {
    static constexpr std::string_view type = "final";
    std::string content;
    bool accepted;
    double attempt;
  };
  using HarnessEvent = std::variant<HarnessEventModelRequest, HarnessEventModelResponse, HarnessEventToolCall, HarnessEventToolResult, HarnessEventGuard, HarnessEventFinal>;

  struct HarnessOptions {
    std::string systemPrompt;
    std::string userPrompt;
    adapter::HarmonyTransport transport;
    tools::backend::ToolBackends backends;
    /** Model-facing tool surface; defaults to the canonical compact surface. */
    std::optional<std::vector<types::ToolDefinition>> tools{};
    std::optional<types::HarmonyReasoningEffort> reasoningEffort{};
    std::optional<double> maxCompletionTokens{};
    std::optional<double> maxTurns{};
    std::optional<double> maxToolCalls{};
    std::optional<double> invalidCallStreakLimit{};
    std::optional<double> loopThreshold{};
    std::optional<double> maxGuardRejections{};
    std::optional<context::ContextBudgetKind> transcriptBudget{};
    std::optional<std::string> contextMode{};
    std::optional<retry::RetryPolicy> retryPolicy{};
    std::optional<verify::VerificationPolicy> verificationPolicy{};
    /** Task-declared verification command (exact-match exec satisfies writes). */
    std::optional<std::string> verificationCommand{};
    std::function<void(const HarnessEvent&)> emit{};
    std::optional<rt::AbortSignal> signal{};
  };

  struct HarnessOutcome {
    std::string phase;
    std::optional<std::string> finalContent;
    conversation::Conversation conversation;
    state::StructuredTaskState state;
    failure::ReliabilityClassification classification;
    std::vector<HarnessEvent> events;
    double modelCalls;
    std::optional<std::string> abortedReason;
  };

  struct CanonicalPolicyOptions {
    std::vector<std::string> tools;
    context::ContextBudgetKind budget;
  };
  /** Deterministic model-facing policy preamble (fixed text, no secrets). */
  std::string renderCanonicalPolicy(const CanonicalPolicyOptions& opts);

  /** Runs the deterministic reliability loop, retaining the full authoritative conversation. */
  [[nodiscard]] rt::Task<HarnessOutcome> runReliabilityHarness(HarnessOptions opts);
}
