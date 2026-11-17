#pragma once
// Port of src/request-policy.ts.
// Model metadata, prompt-cache, reasoning and warning policy for upstream requests, extracted from src/openai.ts.
//
// `value: unknown` parameters have a `const json::value*` (nullptr is undefined) and a
// `const json::value&` overload. Every `{ ok: true; ... } | { ok: false; message: string }`
// result is a std::variant whose failure alternative is ParseFieldNotOk.
#include "uos/defaults.hpp"
#include "uos/models/codex_models.hpp"
#include "uos/models/metadata.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/types.hpp"

#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace uos::request_policy {
  /**
   * The hint for a Cerebras id. An unrecognized id falls back to the gpt-oss
   * contract, which is the route's original single-model declaration.
   */
  models::metadata::ModelMetadataHint cerebrasProviderHint(const std::string& model);

  /**
   * What the LithosAI route declares about its own models. All eight ids serve
   * one 1,048,576-token window (verified 2026-09-23), and `reasoning_effort` is
   * accepted verbatim for the seven tiers the route advertises, so the hint
   * carries the window, the effective percentage and the tiers together rather
   * than restating them at each call site.
   */
  // `export const LITHOS_PROVIDER_HINT`: built at module load from provider/lithos.ts's constants,
  // so it is constructed on first use.
  const models::metadata::ModelMetadataHint& LITHOS_PROVIDER_HINT();

  // Temporary hard cut while this exact gateway model has free Surplus inference.
  // Remove the cut when the free-inference window ends; do not generalize it to
  // other catalog models or paid-fallback routing.
  inline constexpr std::string_view TEMPORARY_FREE_SURPLUS_MODEL = "glm-5.2";

  bool isTemporaryFreeSurplusModel(const std::string& model);

  /**
   * Codex ids the owner authorized as servable before the upstream discovery
   * catalog advertises them. This is deliberately a closed list of exact ids, not
   * a recognition pattern, and it fabricates no catalog entry: the requested id is
   * forwarded to the Codex transport verbatim. `gpt-reserve` is the second,
   * separately metered id for luna, so it also owns its own quota class.
   */
  inline constexpr std::array<std::string_view, 1> ADDITIONAL_TRUSTED_CODEX_MODEL_IDS{"gpt-reserve"};

  bool isAdditionalTrustedCodexModel(const std::string& model);

  struct CodexModelReasoning {
    std::vector<defaults::ReasoningEffort> levels;
    std::optional<defaults::ReasoningEffort> defaultLevel;
    // ReadonlyMap<ReasoningEffort, ReasoningEffort>
    rt::OrderedMap<defaults::ReasoningEffort, defaults::ReasoningEffort> wireEfforts;
  };

  struct CodexModelMetadata {
    std::optional<models::codex_models::CodexModelsSnapshot> snapshot;
    std::optional<json::object> record;  // Record<string, unknown> | null
    CodexModelReasoning reasoning;
    std::optional<std::vector<std::string>> supportedEndpoints;
  };

  // `string | null`: std::nullopt is null.
  std::optional<std::string> modelIdFromSnapshotRecord(const json::object& model);

  // `record: Record<string, unknown> | null`: std::nullopt is null.
  CodexModelReasoning getCodexModelReasoning(const std::optional<json::object>& record);

  // `route: "chat.completions" | "responses"`.
  rt::Task<CodexModelMetadata> getCodexModelMetadata(std::string model, std::string route);

  // `route: "chat.completions" | "responses"`; `Response | null` (std::nullopt is null).
  std::optional<rt::Response> validateCodexModelAvailable(const std::string& model, const std::string& route, const CodexModelMetadata& metadata);

  // `inputParam: "input" | "messages"`; `Response | null` (std::nullopt is null).
  std::optional<rt::Response> validateKnownUnsupportedPromptCacheUse(const std::string& model, const CodexModelMetadata& metadata, const json::object& rawRecord,
                                                                     const std::vector<::uos::types::ResponseInputItem>& input, const std::string& inputParam);

  defaults::ReasoningEffort resolveDefaultReasoningLabel(const CodexModelReasoning& _modelReasoning, const defaults::ReasoningEffort& defaultEffort);

  // `effort: ReasoningEffort | undefined`: std::nullopt is undefined.
  defaults::ReasoningEffort resolveReasoningLabelFromEffort(const std::optional<defaults::ReasoningEffort>& effort, const defaults::ReasoningEffort& defaultLabel);

  // `reasoning: Record<string, unknown> | undefined`: std::nullopt is undefined.
  defaults::ReasoningEffort resolveReasoningLabelFromParam(const std::optional<json::object>& reasoning, const defaults::ReasoningEffort& defaultLabel);

  defaults::ReasoningEffort reasoningEffortForCodexRequest(const defaults::ReasoningEffort& effort, const CodexModelReasoning& modelReasoning);

  // `reasoning: Record<string, unknown> | undefined` and the result: std::nullopt is undefined.
  std::optional<json::object> normalizeReasoningParamForCodex(const std::optional<json::object>& reasoning, const CodexModelReasoning& modelReasoning);

  inline constexpr std::string_view UOS_WARNING_HEADER = "x-uos-warning";

  // `new Map<string, string>([...])` of the module's literal warning names.
  inline const rt::OrderedMap<std::string, std::string> WARNING_KEY_MAP{
    {"temperature", "temperature_ignored"},
    {"max_tokens", "max_output_tokens_ignored"},
    {"max_completion_tokens", "max_output_tokens_ignored"},
    {"max_output_tokens", "max_output_tokens_ignored"},
  };

  // `usedKeys: ReadonlySet<string>`.
  std::vector<std::string> buildIgnoredWarnings(const json::object& record, const rt::OrderedSet<std::string>& usedKeys);

  std::vector<std::string> responseWarnings(const rt::Response& response);

  // "tools" | "tool_choice" | "parallel_tool_calls" | "prompt_cache_key" | "prompt_cache_options" |
  // "prompt_cache_retention" | "text" | "include" | "context_management"
  using PassthroughToolSchemaKey = std::string;

  // Writes the passed-through members into `codexBody` (the TS mutates it).
  void applyPassthroughToCodexRequest(json::object& codexBody, const json::object& rawRecord, const std::vector<PassthroughToolSchemaKey>& keys);

  rt::Response withUosWarning(rt::Response response, const std::vector<std::string>& warnings);

  // `{ ok: false; message: string }`, the failure of every parser below.
  struct ParseFieldNotOk {
    static constexpr bool ok = false;
    std::string message;
    friend bool operator==(const ParseFieldNotOk&, const ParseFieldNotOk&) = default;
  };

  struct ParseReasoningEffortFieldOk {
    static constexpr bool ok = true;
    std::optional<defaults::ReasoningEffort> value;  // ReasoningEffort | undefined
    friend bool operator==(const ParseReasoningEffortFieldOk&, const ParseReasoningEffortFieldOk&) = default;
  };
  using ParseReasoningEffortFieldResult = std::variant<ParseReasoningEffortFieldOk, ParseFieldNotOk>;

  ParseReasoningEffortFieldResult parseReasoningEffortField(const json::value* value, const std::string& fieldName);
  ParseReasoningEffortFieldResult parseReasoningEffortField(const json::value& value, const std::string& fieldName);

  struct ParseReasoningParamOk {
    static constexpr bool ok = true;
    std::optional<json::object> value;  // Record<string, unknown> | undefined
    friend bool operator==(const ParseReasoningParamOk&, const ParseReasoningParamOk&) = default;
  };
  using ParseReasoningParamResult = std::variant<ParseReasoningParamOk, ParseFieldNotOk>;

  ParseReasoningParamResult parseReasoningParam(const json::value* value);
  ParseReasoningParamResult parseReasoningParam(const json::value& value);

  struct ParseStreamFieldOk {
    static constexpr bool ok = true;
    bool value;
    friend bool operator==(const ParseStreamFieldOk&, const ParseStreamFieldOk&) = default;
  };
  using ParseStreamFieldResult = std::variant<ParseStreamFieldOk, ParseFieldNotOk>;

  ParseStreamFieldResult parseStreamField(const json::value* value);
  ParseStreamFieldResult parseStreamField(const json::value& value);

  struct ParseChatStreamOptionsOk {
    static constexpr bool ok = true;
    bool includeUsage;
    friend bool operator==(const ParseChatStreamOptionsOk&, const ParseChatStreamOptionsOk&) = default;
  };
  using ParseChatStreamOptionsResult = std::variant<ParseChatStreamOptionsOk, ParseFieldNotOk>;

  ParseChatStreamOptionsResult parseChatStreamOptions(const json::value* value);
  ParseChatStreamOptionsResult parseChatStreamOptions(const json::value& value);

  struct ParseMaxCompletionTokensFieldOk {
    static constexpr bool ok = true;
    std::optional<double> value;  // number | undefined
    friend bool operator==(const ParseMaxCompletionTokensFieldOk&, const ParseMaxCompletionTokensFieldOk&) = default;
  };
  using ParseMaxCompletionTokensFieldResult = std::variant<ParseMaxCompletionTokensFieldOk, ParseFieldNotOk>;

  ParseMaxCompletionTokensFieldResult parseMaxCompletionTokensField(const json::value* value);
  ParseMaxCompletionTokensFieldResult parseMaxCompletionTokensField(const json::value& value);
}
