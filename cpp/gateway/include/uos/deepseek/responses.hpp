#pragma once
// Port of src/deepseek/responses.ts.
//
// Responses <-> DeepSeek Chat Completions adapter: the shared types, the per-provider
// ChatOnlyResponsesProfile objects and the tool-name helpers (see the module comment of
// src/deepseek/responses.ts).
#include "uos/deepseek/index.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace uos::deepseek::responses {
  // `Readonly<{ ok: false; message: string; param: string; code?: string; kind?: "aggregate_context_overflow" }>`
  struct DeepSeekResponsesFailure {
    static constexpr bool ok = false;
    std::string message;
    std::string param;
    std::optional<std::string> code{};
    std::optional<std::string> kind{};  // "aggregate_context_overflow"
    friend bool operator==(const DeepSeekResponsesFailure&, const DeepSeekResponsesFailure&) = default;
  };

  // `Readonly<{ ok: true; value: T }> | DeepSeekResponsesFailure`
  template <class T>
  struct DeepSeekResponsesResultOk {
    static constexpr bool ok = true;
    T value;
    friend bool operator==(const DeepSeekResponsesResultOk&, const DeepSeekResponsesResultOk&) = default;
  };
  template <class T>
  using DeepSeekResponsesResult = std::variant<DeepSeekResponsesResultOk<T>, DeepSeekResponsesFailure>;

  // `code?: string`: std::nullopt is undefined (no `code` member).
  DeepSeekResponsesFailure failure(const std::string& param, const std::string& message, const std::optional<std::string>& code = std::nullopt);

  /** A trusted internal distinction; it is never accepted from request metadata. */
  DeepSeekResponsesFailure aggregateContextOverflow(const std::string& message);

  /**
   * One provider's answers to the questions this translation asks. A provider
   * difference belongs in its profile; the translation itself stays shared.
   *
   * `projectReasoningEffort` returns null when the provider does not accept the
   * requested tier, and the request then fails closed with an
   * `invalid_request_error` rather than sending a value the provider refuses.
   */
  // Plain function pointers and string views, so each profile is constant-initialized (the .cpp
  // defines the three profiles `constinit`; a TS arrow function becomes a captureless lambda).
  // `unknown` parameters are pointers (nullptr is undefined).
  struct ChatOnlyResponsesProfile {
    std::string_view id;  // "deepseek" | "lithos" | "cerebras"
    /** Human name used in client-facing error text. */
    std::string_view label;
    /** Canonical upstream model for a client-facing id, or null when the id is not this provider's. */
    std::optional<std::string> (*upstreamModelFor)(const std::string& model);
    // `(effort: string) => string | null`: null (rt::Nullable null) is a tier the provider does not
    // accept. The DeepSeek projection reads a plain object, so two lowercase Object.prototype names
    // yield inherited values (see deepseek::index::projectDeepSeekReasoningEffort): the `Object`
    // function is rt::undefined here (not nullish in JS; JSON.stringify drops the member as it drops
    // undefined) and Object.prototype an empty json::object. Every other result is a JSON string.
    rt::Nullable<json::value> (*projectReasoningEffort)(const std::string& effort);
    /** The existing finish vocabulary, reused: both providers report the same reasons this adapter maps. */
    deepseek::index::DeepSeekFinishDisposition (*finishDisposition)(const json::value* reason);
    std::optional<std::string> (*thinkingToolChoiceConflict)(const json::value* reasoningEffort, const json::value* thinking, const json::value* toolChoice);
    std::string (*toolChoiceThinkingConflictMessage)(const std::string& conflict, const std::string& field);
    std::optional<double> (*cachedPromptTokens)(const json::object& value, double promptTokens);
    std::optional<double> (*reasoningTokens)(const json::object& value, double completionTokens);
    /** Throws the provider's own not-configured error when its API key is absent. */
    void (*requireApiKey)();
    /** True when the provider reports stream usage only behind `stream_options.include_usage`. */
    bool requiresStreamUsageOption;
  };

  // The three profiles, defined in the .cpp (they reference the DeepSeek, LithosAI and Cerebras
  // transports, and the Cerebras tier set request-policy derives).
  extern const ChatOnlyResponsesProfile DEEPSEEK_RESPONSES_PROFILE;

  /**
   * The LithosAI profile.
   *
   * The usage counters are the transport's own exported guards
   * (`lithosCachedPromptTokens`, `lithosReasoningTokens`), so the transport and
   * this adapter cannot drift on what a readable measurement is. Only the tier
   * predicate stays local, because it is this adapter's request-side decision:
   * `src/lithos.ts` forwards whatever tier it is given and never judges one.
   */
  extern const ChatOnlyResponsesProfile LITHOS_RESPONSES_PROFILE;

  /**
   * The Cerebras profile.
   *
   * The usage counters are the transport's own exported guards
   * (`cerebrasCachedPromptTokens`, `cerebrasReasoningTokens`), so the transport
   * and this adapter cannot drift on what a readable measurement is. Only the
   * tier predicate stays local, because it is this adapter's request-side
   * decision and the provider's per-id enforcement stays in its own transport.
   */
  extern const ChatOnlyResponsesProfile CEREBRAS_RESPONSES_PROFILE;

  /** The Responses identity a flattened Chat tool name was derived from. */
  struct OriginalToolName {
    std::string name;
    std::optional<std::string> namespace_;  // `namespace: string | null`
    friend bool operator==(const OriginalToolName&, const OriginalToolName&) = default;
  };

  /** Maps a flattened Chat tool name back to the identity the client asked for. */
  // `toolNames: ReadonlyMap<string, OriginalToolName>`.
  OriginalToolName originalTool(const std::string& name, const rt::OrderedMap<std::string, OriginalToolName>& toolNames);

  /** Maps a flattened Chat tool name back to the name the client asked for. */
  std::string originalToolName(const std::string& name, const rt::OrderedMap<std::string, OriginalToolName>& toolNames);
}
