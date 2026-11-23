#pragma once
// Port of src/deepseek/forwarded-payload-policy.ts.
#include "uos/rt/prelude.hpp"
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::deepseek::forwarded_payload_policy {
  struct CompactionSummaryPolicy {
    std::string_view version;
    std::string_view prefix;
    double prefixBytes;
    std::string_view prefixSha256;
    double maxPerRequest;
  };
  /**
   * The declared, versioned policy that bounds how much payload this gateway
   * forwards inside one Chat Completions message.
   *
   * Why this exists: both providers on this translation count forwarded bytes -
   * base64 image data URLs and raw tool results alike - as text tokens. On
   * 2026-09-24 a single 744,586-byte `view_image` result took one session from
   * 736,213 requested tokens to 1,250,713 against a 1,048,576-token window; every
   * later request replayed the same blob and was rejected, so the thread could
   * not be resumed.
   *
   * The first stopgap cut each payload at an undeclared 64 KiB constant. This
   * policy replaces it: the limit is declared here, advertised to Codex clients
   * through the model catalog's `forwarding_policy` extension, carried in every
   * elision marker and operator log line, and versioned so a change is visible
   * rather than silent.
   *
   * - `mode: "bytes"` counts the UTF-8 bytes of one forwarded message payload.
   * - `perMessageLimit` bounds the forwarded payload including its elision marker.
   * - Reduction is deterministic (a byte prefix plus a marker naming the counts)
   *   and applies only when the request did not forbid it: an explicit
   *   `truncation: "disabled"` fails closed with HTTP 400
   *   `context_length_exceeded` instead of mutating the input.
   * - One exact stock Codex local-compaction summary envelope is exempt from the
   *   per-message cut. The envelope is a user message with one `input_text` part
   *   whose text begins with the exact 399-byte Codex `SUMMARY_PREFIX` and a
   *   newline. The prefix is request text, not proof of authority: it only selects
   *   the versioned exception, and the complete projected prompt still has to fit
   *   the selected provider's aggregate admission.
   * - Aggregate admission is a separate, versioned control over the serialized
   *   Chat body after every field is projected. UTF-8 bytes plus explicit
   *   structural overhead bound the prompt against the provider window minus an
   *   output reserve, so tools, instructions, arguments, reasoning, images, and
   *   transport structure cannot hide outside the per-message checks.
   */
  inline constexpr CompactionSummaryPolicy COMPACTION_SUMMARY_POLICY{
    "codex-compaction-summary/v1",
    "Another language model started to solve this problem and produced a summary of its thinking process. You also have access to the state of the tools that were used by that language model. Use this to build on the work that has already been done and avoid duplicating work. Here is the summary produced by the other language model, use the information in this summary to assist with your own analysis:",
    399, "e9b088e794a6bb9082ac053fcc760bd818d7e720ee4bcdc72c6e480de7b7cb0e", 1};
  struct ForwardedPayloadPolicy { std::string_view version; std::string_view mode; double perMessageLimit; CompactionSummaryPolicy summaryExemption; };
  inline constexpr ForwardedPayloadPolicy FORWARDED_PAYLOAD_POLICY{"deepseek-forwarded-payload/v2", "bytes", 65'536, COMPACTION_SUMMARY_POLICY};
  struct PromptAdmissionPolicy { std::string_view version; double minimumOutputReserveTokens; double requestStructuralOverheadTokens; double messageStructuralOverheadTokens; double toolStructuralOverheadTokens; };
  inline constexpr PromptAdmissionPolicy PROMPT_ADMISSION_POLICY{"chat-prompt-admission/v3", 16'384, 256, 16, 32};
  struct PromptAdmissionProfileWindows { double deepseek; double lithos; double cerebras; };
  const PromptAdmissionProfileWindows& PROMPT_ADMISSION_PROFILE_WINDOWS();
  std::optional<double> promptWindowTokensForProfile(std::string_view profile);
  /** The aggregate budget and observed cost for one complete projected Chat body. */
  struct PromptAdmission {
    std::string policy;
    double estimatedPromptTokens;
    double serializedBodyBytes;
    double structuralOverheadTokens;
    std::optional<double> requestedOutputTokens;
    double outputReserveTokens;
    double contextWindowTokens;
    double promptBudgetTokens;
    bool admitted;
  };
  using PromptAdmissionProjection = std::function<json::object(const json::object&)>;
  PromptAdmission estimatePromptAdmission(const json::object& body, double contextWindowTokens, const PromptAdmissionProjection& projectBody = {});
  std::string promptAdmissionFailureMessage(const PromptAdmission& admission, const std::string& providerLabel);
  using ForwardedPayloadReduction = std::string;
  /** One payload the declared policy reduced, as recorded in the operator log. */
  struct ForwardedPayloadElision {
    std::string path;
    std::optional<std::string> callId;
    std::string kind;
    double originalBytes;
    double forwardedBytes;
    double omittedBytes;
  };
  struct ReducedToolArgumentsPayload { std::string content; ForwardedPayloadElision elision; };
  std::optional<ReducedToolArgumentsPayload> reduceToolArgumentsPayload(const std::string& value, const std::string& path, const std::string& callId, double limit);
  /** One exact stock local-compaction summary forwarded above the generic bound. */
  struct ForwardedPayloadExemption {
    std::string path;
    std::string kind;
    std::string summaryPolicy;
    std::string prefixSha256;
    double bytes;
    double contentParts;
    double perMessageLimit;
  };
  ForwardedPayloadExemption compactionSummaryExemption(const std::string& path, double bytes);
  struct CompactionSummaryInspectionOk { static constexpr bool ok = true; std::optional<double> index; };
  struct CompactionSummaryInspectionNotOk { static constexpr bool ok = false; std::string path; std::string message; };
  using CompactionSummaryInspection = std::variant<CompactionSummaryInspectionOk, CompactionSummaryInspectionNotOk>;
  CompactionSummaryInspection inspectCompactionSummaryInput(const json::value* input);
  CompactionSummaryInspection inspectCompactionSummaryInput(const json::value& input);
  struct LogForwardedPayloadExemptionsContext {
    std::optional<std::string> requestId{};
    std::optional<std::string> model{};
    std::optional<PromptAdmission> promptAdmission{};
  };
  void logForwardedPayloadExemptions(const std::vector<ForwardedPayloadExemption>& exemptions, const LogForwardedPayloadExemptionsContext& context = {});
  json::object toJson(const PromptAdmission& value);
  json::object toJson(const ForwardedPayloadElision& value);
  json::object toJson(const ForwardedPayloadExemption& value);
}
