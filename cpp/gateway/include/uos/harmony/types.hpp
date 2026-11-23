#pragma once
// Port of src/harmony/types.ts.
/**
 * Shared types for the isolated Harmony/Cerebras protocol adapter (plan m01).
 *
 * The GPT-OSS models were trained on the Harmony response format
 * (https://github.com/openai/harmony).  This module defines the normalized
 * conversation and turn model used by the adapter, the probe manifest, and
 * their deterministic fake-transport tests.  It must not be imported by the
 * gateway routes: this is probe and harness infrastructure owned by the
 * Harmony agent-foundation work.
 */
#include "uos/provider/cerebras.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace uos::harmony::types {
  /** Exact upstream model used by every Harmony protocol probe. */
  inline constexpr std::string_view HARMONY_CEREBRAS_MODEL = provider::cerebras::CEREBRAS_GPT_OSS_120B_MODEL;

  /** Reasoning efforts the plan is allowed to experiment with. */
  // "low" | "medium" | "high"
  using HarmonyReasoningEffort = std::string;

  /** Harmony assistant channels. */
  // "analysis" | "commentary" | "final"
  using HarmonyChannel = std::string;

  /**
   * Call styles the adapter can produce.
   *
   * - `generic` forwards the official Chat Completions `tools` (and top-level
   *   `reasoning_effort` / `response_format`) and lets the provider translate
   *   between OpenAI tool calls and Harmony semantics.  This is the gateway's
   *   current GPT-OSS behavior.
   * - `native` renders tools and the reasoning level *in* the system/developer
   *   messages exactly like the Harmony renderer, sends no `tools` field, and
   *   relies on Harmony-native output (auto-parsed by the provider or parsed by
   *   this adapter).
   */
  // "generic" | "native"
  using HarmonyCallStyle = std::string;

  /**
   * How exposed tools are normalized for the provider.
   *
   * Cerebras requires every tool in one request to carry the same `strict`
   * value ("Tools with mixed values for 'strict' are not allowed"), so the
   * adapter normalizes by default.  `preserve` exists only for protocol probes
   * that must record the upstream rejection.
   */
  // "normalize-false" | "normalize-true" | "preserve"
  using ToolStrictnessMode = std::string;

  /** A model-facing tool definition (official OpenAI Chat Completions shape). */
  struct ToolDefinition {
    std::string name;
    std::string description;
    json::object parameters;
    std::optional<bool> strict{};
    friend bool operator==(const ToolDefinition&, const ToolDefinition&) = default;
  };

  /** Normalized tool call inside an assistant turn. */
  struct ToolCall {
    std::string id;
    /** Tool name without the `functions.` Harmony namespace prefix. */
    std::string name;
    /** Opaque JSON string, matched by the shared application pipeline. */
    std::string arguments;
    friend bool operator==(const ToolCall&, const ToolCall&) = default;
  };

  /**
   * One normalized assistant turn.  `analysis` is the private chain of thought
   * (Harmony `analysis` channel) and never leaves the adapter; `content` is the
   * user-visible final/commentary text.
   */
  struct AssistantTurn {
    static constexpr std::string_view role = "assistant";
    std::optional<std::string> content;
    std::vector<std::string> analysis;
    std::vector<ToolCall> toolCalls;
    std::optional<std::string> finishReason;
    friend bool operator==(const AssistantTurn&, const AssistantTurn&) = default;
  };

  /** Normalized tool result turn (OpenAI `tool` role). */
  struct ToolResultTurn {
    static constexpr std::string_view role = "tool";
    std::string toolCallId;
    std::string name;
    std::string content;
    friend bool operator==(const ToolResultTurn&, const ToolResultTurn&) = default;
  };

  // `{ role: "system"; content: string }`
  struct ConversationTurnSystem {
    static constexpr std::string_view role = "system";
    std::string content;
    friend bool operator==(const ConversationTurnSystem&, const ConversationTurnSystem&) = default;
  };

  // `{ role: "developer"; content: string }`
  struct ConversationTurnDeveloper {
    static constexpr std::string_view role = "developer";
    std::string content;
    friend bool operator==(const ConversationTurnDeveloper&, const ConversationTurnDeveloper&) = default;
  };

  // `{ role: "user"; content: string }`
  struct ConversationTurnUser {
    static constexpr std::string_view role = "user";
    std::string content;
    friend bool operator==(const ConversationTurnUser&, const ConversationTurnUser&) = default;
  };

  /** A conversation turn the adapter is allowed to hold in state. */
  // Discriminated by `role` (each alternative's static member).
  using ConversationTurn = std::variant<ConversationTurnSystem, ConversationTurnDeveloper, ConversationTurnUser, AssistantTurn, ToolResultTurn>;

  /**
   * Parsed Harmony assistant output segments, in emission order.
   *
   * - `reasoning`  : `analysis` channel text (private chain of thought).
   * - `commentary` : `commentary` channel text without a recipient (preambles).
   * - `tool_call`  : `commentary`/`analysis` channel with a function recipient.
   * - `final`      : `final` channel text (the answer to show the user).
   */
  struct HarmonyTurnReasoning {
    static constexpr std::string_view kind = "reasoning";
    std::string text;
    friend bool operator==(const HarmonyTurnReasoning&, const HarmonyTurnReasoning&) = default;
  };
  struct HarmonyTurnCommentary {
    static constexpr std::string_view kind = "commentary";
    std::string text;
    friend bool operator==(const HarmonyTurnCommentary&, const HarmonyTurnCommentary&) = default;
  };
  struct HarmonyTurnToolCall {
    static constexpr std::string_view kind = "tool_call";
    std::string recipient;
    std::string name;
    std::string arguments;
    friend bool operator==(const HarmonyTurnToolCall&, const HarmonyTurnToolCall&) = default;
  };
  struct HarmonyTurnFinal {
    static constexpr std::string_view kind = "final";
    std::string text;
    friend bool operator==(const HarmonyTurnFinal&, const HarmonyTurnFinal&) = default;
  };
  using HarmonyTurn = std::variant<HarmonyTurnReasoning, HarmonyTurnCommentary, HarmonyTurnToolCall, HarmonyTurnFinal>;

  /** Shape of one raw assistant message parsed from Harmony text. */
  struct HarmonyRawMessage {
    std::string role;
    std::optional<HarmonyChannel> channel;
    std::optional<std::string> recipient;
    std::optional<std::string> constrain;
    std::string content;
    // "<|end|>" | "<|call|>" | "<|return|>" | "truncated"
    std::string stoppedBy;
    friend bool operator==(const HarmonyRawMessage&, const HarmonyRawMessage&) = default;
  };

  /** Result of parsing raw assistant output text as Harmony. */
  struct HarmonyParseResult {
    std::vector<HarmonyRawMessage> messages;
    std::vector<HarmonyTurn> turns;
    bool truncated;
    friend bool operator==(const HarmonyParseResult&, const HarmonyParseResult&) = default;
  };

  /** What a normalized assistant response looked like on the wire. */
  struct AssistantResponseShape {
    bool contentPresent;
    double contentChars;
    // "reasoning_content" | "reasoning" | "none"
    std::string reasoningField;
    double reasoningChars;
    bool toolCallsField;
    double toolCallCount;
    std::optional<std::string> finishReason;
    bool refusal;
    friend bool operator==(const AssistantResponseShape&, const AssistantResponseShape&) = default;
  };

  /** Normalized assistant completion from a provider Chat Completions payload. */
  struct NormalizedAssistantResponse {
    std::vector<HarmonyTurn> turns;
    std::vector<std::string> analysis;
    std::optional<std::string> content;
    std::vector<ToolCall> toolCalls;
    std::optional<std::string> finishReason;
    std::optional<std::string> refusal;
    AssistantResponseShape shape;
    std::string id;
    std::string model;
    double created;
    friend bool operator==(const NormalizedAssistantResponse&, const NormalizedAssistantResponse&) = default;
  };

  /** Official OpenAI Chat Completions response format parameter. */
  // `{ type: "json_object" }`
  struct ResponseFormatParamJsonObject {
    static constexpr std::string_view type = "json_object";
    friend bool operator==(const ResponseFormatParamJsonObject&, const ResponseFormatParamJsonObject&) = default;
  };
  // `{ type: "json_schema"; json_schema: {...} }`
  struct ResponseFormatParamJsonSchema {
    // `{ name: string; strict?: boolean; schema: Record<string, unknown>; description?: string }`
    struct JsonSchema {
      std::string name;
      std::optional<bool> strict{};
      json::object schema;
      std::optional<std::string> description{};
      friend bool operator==(const JsonSchema&, const JsonSchema&) = default;
    };
    static constexpr std::string_view type = "json_schema";
    JsonSchema json_schema;
    friend bool operator==(const ResponseFormatParamJsonSchema&, const ResponseFormatParamJsonSchema&) = default;
  };
  using ResponseFormatParam = std::variant<ResponseFormatParamJsonObject, ResponseFormatParamJsonSchema>;
  void tag_invoke(const json::value_from_tag&, json::value& out, const ResponseFormatParamJsonSchema::JsonSchema& value);
  ResponseFormatParamJsonSchema::JsonSchema tag_invoke(const json::value_to_tag<ResponseFormatParamJsonSchema::JsonSchema>&, const json::value& value);
  void tag_invoke(const json::value_from_tag&, json::value& out, const ResponseFormatParam& value);
  ResponseFormatParam tag_invoke(const json::value_to_tag<ResponseFormatParam>&, const json::value& value);

  /** Structured-output description for the native (Harmony-rendered) style. */
  struct NativeResponseFormat {
    std::string formatName;
    std::optional<std::string> description{};
    json::object schema;
    friend bool operator==(const NativeResponseFormat&, const NativeResponseFormat&) = default;
  };

  /** Policy for combining tools with a structured `response_format`. */
  // "error" | "probe"
  using CombinationPolicy = std::string;

  /**
   * Adapter-domain error.  `code` lets tests and the probe runner distinguish
   * local policy rejections from upstream failures.
   */
  // "unproven-combination" | "mixed-strictness-requested" | "invalid-request" | "invalid-upstream-response" | "no-model-output"
  using HarmonyAdapterErrorCode = std::string;

  // `code` is also stored in rt::JsError::code, so code that reads `error.code` off any caught
  // error sees it as JS does.
  class HarmonyAdapterError : public rt::JsError {
  public:
    const HarmonyAdapterErrorCode code;

    HarmonyAdapterError(std::string message_, HarmonyAdapterErrorCode code_) : rt::JsError(std::move(message_), "HarmonyAdapterError"), code(std::move(code_)) {
      rt::JsError::code = code;
    }
  };
}
