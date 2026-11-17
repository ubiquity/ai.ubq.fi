#include "uos/deepseek/forwarded_payload_policy.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/provider/cerebras.hpp"
#include "uos/provider/lithos.hpp"
#include "uos/request_policy.hpp"
#include "uos/lib/jev_compaction/state.hpp"
#include "uos/utils.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/record.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

namespace uos::deepseek::forwarded_payload_policy {
/** Each Chat-only provider profile keeps its own measured context window. */
const PromptAdmissionProfileWindows& PROMPT_ADMISSION_PROFILE_WINDOWS() {
  static const PromptAdmissionProfileWindows windows{index::DEEPSEEK_CONTEXT_WINDOW_TOKENS, provider::lithos::LITHOS_CONTEXT_WINDOW_TOKENS,
    request_policy::cerebrasProviderHint(std::string(provider::cerebras::CEREBRAS_GPT_OSS_120B_MODEL)).context_window_tokens.valueOr(0)};
  return windows;
}
/** Reads one provider's admission window, or null when no verified window exists. */
std::optional<double> promptWindowTokensForProfile(std::string_view profile) {
  const auto& windows = PROMPT_ADMISSION_PROFILE_WINDOWS();
  double window = profile == "deepseek" ? windows.deepseek : profile == "lithos" ? windows.lithos : profile == "cerebras" ? windows.cerebras : 0;
  return rt::Number::isSafeInteger(window) && window > 0 ? std::optional(window) : std::nullopt;
}
/** Estimates one complete projected Chat prompt against one provider window. */
PromptAdmission estimatePromptAdmission(const json::object& body, double contextWindowTokens, const PromptAdmissionProjection& projectBody) {
  json::object projectedBody = body;
  if (projectBody) { try { projectedBody = projectBody(body); } catch (...) { projectedBody = body; } }
  const auto serializedBody = rt::jsonStringify(projectedBody);
  const auto* messages = rt::getArray(projectedBody, "messages");
  const auto* tools = rt::getArray(projectedBody, "tools");
  /**
   * The aggregate bounds are declared once and applied to every Chat provider.
   * The existing Jev estimator counts word pieces and was calibrated at 2-18%
   * above measured usage on real transcripts; its estimate is added to explicit
   * structural terms for chat framing that is absent from JSON serialization.
   * The reserve is the larger of this floor and the request's own `max_tokens`,
   * because a provider must fit both prompt and completion inside its window.
   */
  const auto structural = PROMPT_ADMISSION_POLICY.requestStructuralOverheadTokens +
    static_cast<double>(messages ? messages->size() : 0) * PROMPT_ADMISSION_POLICY.messageStructuralOverheadTokens +
    static_cast<double>(tools ? tools->size() : 0) * PROMPT_ADMISSION_POLICY.toolStructuralOverheadTokens;
  const auto estimated = lib::jev_compaction::state::estimateTokens(serializedBody) + structural;
  auto requested = rt::getNumber(projectedBody, "max_tokens");
  if (requested && (!rt::Number::isSafeInteger(*requested) || *requested <= 0)) requested.reset();
  const auto reserve = rt::Math::max(PROMPT_ADMISSION_POLICY.minimumOutputReserveTokens, requested.value_or(0));
  const auto budget = rt::Math::max(0.0, contextWindowTokens - reserve);
  return {std::string(PROMPT_ADMISSION_POLICY.version), estimated, static_cast<double>(rt::utf8ByteLength(serializedBody)), structural, requested, reserve, contextWindowTokens, budget, estimated <= budget};
}
/** The client-facing explanation for an aggregate admission refusal. */
std::string promptAdmissionFailureMessage(const PromptAdmission& admission, const std::string& providerLabel) {
  return "the complete projected " + providerLabel + " prompt is an estimated " + rt::numberToString(admission.estimatedPromptTokens) +
    " tokens (the calibrated estimator over " + rt::numberToString(admission.serializedBodyBytes) + " serialized UTF-8 bytes plus " + rt::numberToString(admission.structuralOverheadTokens) +
    " structural tokens), above the " + rt::numberToString(admission.promptBudgetTokens) + "-token budget from a " + rt::numberToString(admission.contextWindowTokens) +
    "-token window minus a " + rt::numberToString(admission.outputReserveTokens) + "-token output reserve under " + admission.policy + ". Reduce the prompt; no payload was dispatched.";
}
namespace {
  double forwardedBytes(const std::string& value) { return static_cast<double>(rt::utf8ByteLength(value)); }
  std::string malformedSummaryMessage(const std::string& detail) {
    return detail + "; " + std::string(COMPACTION_SUMMARY_POLICY.version) + " exempts at most " + rt::numberToString(COMPACTION_SUMMARY_POLICY.maxPerRequest) +
      " exact user input_text envelope beginning with the " + rt::numberToString(COMPACTION_SUMMARY_POLICY.prefixBytes) + "-byte Codex summary prefix and a newline. The summary was not reduced or forwarded.";
  }
  CompactionSummaryInspectionNotOk summaryFailure(const std::string& path, const std::string& detail) { return {path, malformedSummaryMessage(detail)}; }
  std::optional<CompactionSummaryInspection> stringSummaryFailure(const std::string& value, const std::string& path, const std::string& detail) {
    if (rt::startsWith(value, COMPACTION_SUMMARY_POLICY.prefix)) return summaryFailure(path, detail);
    return std::nullopt;
  }
  bool isExactSummaryEnvelope(const json::object& item, const json::value* content, const json::array* parts) {
    if (rt::getString(item, "type").value_or("message") != "message" || rt::getString(item, "role") != "user" || !content || !content->is_array() || !parts || parts->size() != 1) return false;
    const auto& part = (*parts)[0];
    const auto text = rt::getString(part, "text");
    return part.is_object() && rt::getString(part, "type") == "input_text" && text && rt::startsWith(*text, std::string(COMPACTION_SUMMARY_POLICY.prefix) + "\n") && rt::length(*text) > rt::length(COMPACTION_SUMMARY_POLICY.prefix) + 1;
  }
  std::optional<CompactionSummaryInspection> inspectSummaryItem(const json::value& item, double index) {
    const auto path = "input[" + rt::numberToString(index) + "]";
    if (item.is_string()) return stringSummaryFailure(rt::toString(item), path, "is a string item carrying a summary prefix");
    if (!item.is_object()) return std::nullopt;
    const auto* content = rt::get(item, "content");
    const auto* parts = rt::getArray(item, "content");
    bool prefix = content && content->is_string() && rt::startsWith(rt::toString(*content), COMPACTION_SUMMARY_POLICY.prefix);
    if (parts) for (const auto& part : *parts) if (auto text = rt::getString(part, "text"); text && rt::startsWith(*text, COMPACTION_SUMMARY_POLICY.prefix)) prefix = true;
    if (!prefix) return std::nullopt;
    if (!isExactSummaryEnvelope(item.as_object(), content, parts)) return summaryFailure(path, "carries a summary prefix outside the exact one-part user input_text envelope");
    return CompactionSummaryInspectionOk{index};
  }
}
/** Replaces oversized function-call arguments with a parseable JSON elision envelope. */
std::optional<ReducedToolArgumentsPayload> reduceToolArgumentsPayload(const std::string& value, const std::string& path, const std::string& callId, double limit) {
  const auto original = forwardedBytes(value);
  const auto message = "[gateway: " + std::string(FORWARDED_PAYLOAD_POLICY.version) + " elided " + rt::numberToString(original) +
    " bytes omitted; the model did not receive these tool-call arguments; this route forwards at most " + rt::numberToString(limit) + " bytes per message]";
  const json::object envelope{{"gateway_elision", json::object{{"policy", FORWARDED_PAYLOAD_POLICY.version}, {"kind", "tool_arguments"}, {"originalBytes", original}, {"omittedBytes", original}, {"message", message}}}};
  const auto content = rt::jsonStringify(envelope);
  const auto bytes = forwardedBytes(content);
  if (bytes > limit) return std::nullopt;
  return ReducedToolArgumentsPayload{content, {path, callId, "tool_arguments", original, bytes, original}};
}
/** Builds the operator record for one exact summary admitted above the generic bound. */
ForwardedPayloadExemption compactionSummaryExemption(const std::string& path, double bytes) {
  return {path, "compaction_summary", std::string(COMPACTION_SUMMARY_POLICY.version), std::string(COMPACTION_SUMMARY_POLICY.prefixSha256), bytes, 1, FORWARDED_PAYLOAD_POLICY.perMessageLimit};
}
/**
 * Finds the one readable local-compaction summary Codex can adopt. Codex
 * 0.163.0 strips `internal_chat_message_metadata_passthrough` for every
 * non-OpenAI provider, so the exact envelope itself is the only accepted
 * classifier. A request with two summary envelopes, or a prefix-bearing
 * message that is not exactly one user input_text part, fails closed.
 */
CompactionSummaryInspection inspectCompactionSummaryInput(const json::value* input) {
  if (input && input->is_string()) return stringSummaryFailure(rt::toString(*input), "input", "is a string carrying a summary prefix").value_or(CompactionSummaryInspectionOk{std::nullopt});
  if (!input || !input->is_array()) return CompactionSummaryInspectionOk{std::nullopt};
  std::optional<double> found;
  for (std::size_t i = 0; i < input->as_array().size(); ++i) {
    const auto inspection = inspectSummaryItem(input->as_array()[i], static_cast<double>(i));
    if (!inspection) continue;
    if (const auto* failure = std::get_if<CompactionSummaryInspectionNotOk>(&*inspection)) return *failure;
    if (found) return summaryFailure("input[" + rt::numberToString(static_cast<double>(i)) + "]", "adds a second summary envelope");
    found = static_cast<double>(i);
  }
  return CompactionSummaryInspectionOk{found};
}
CompactionSummaryInspection inspectCompactionSummaryInput(const json::value& input) { return inspectCompactionSummaryInput(&input); }
json::object toJson(const PromptAdmission& value) {
  return rt::record::Writer().required("policy", value.policy).required("estimatedPromptTokens", value.estimatedPromptTokens).required("serializedBodyBytes", value.serializedBodyBytes)
    .required("structuralOverheadTokens", value.structuralOverheadTokens).orNull("requestedOutputTokens", value.requestedOutputTokens).required("outputReserveTokens", value.outputReserveTokens)
    .required("contextWindowTokens", value.contextWindowTokens).required("promptBudgetTokens", value.promptBudgetTokens).required("admitted", value.admitted).done().as_object();
}
json::object toJson(const ForwardedPayloadElision& value) {
  return rt::record::Writer().required("path", value.path).orNull("callId", value.callId).required("kind", value.kind).required("originalBytes", value.originalBytes).required("forwardedBytes", value.forwardedBytes).required("omittedBytes", value.omittedBytes).done().as_object();
}
json::object toJson(const ForwardedPayloadExemption& value) {
  return rt::record::Writer().required("path", value.path).required("kind", value.kind).required("summaryPolicy", value.summaryPolicy).required("prefixSha256", value.prefixSha256).required("bytes", value.bytes).required("contentParts", value.contentParts).required("perMessageLimit", value.perMessageLimit).done().as_object();
}
/** Records each exact summary admitted whole above the generic per-message bound. */
void logForwardedPayloadExemptions(const std::vector<ForwardedPayloadExemption>& exemptions, const LogForwardedPayloadExemptionsContext& context) {
  for (const auto& exemption : exemptions) {
    auto material = rt::record::Writer().required("policy", std::string(FORWARDED_PAYLOAD_POLICY.version)).orNull("request_id", context.requestId).orNull("model", context.model).done().as_object();
    material["prompt_admission"] = context.promptAdmission ? json::value(toJson(*context.promptAdmission)) : json::value(nullptr);
    rt::objectAssign(material, toJson(exemption));
    material["prefixBytes"] = COMPACTION_SUMMARY_POLICY.prefixBytes;
    rt::console::info("[ai.ubq.fi] forwarding_exemption", rt::jsonStringify(material));
  }
}
}
