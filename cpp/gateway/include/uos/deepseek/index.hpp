#pragma once
// Port of src/deepseek/index.ts.
//
// `value: unknown` parameters have a `const json::value*` (nullptr is undefined) and a
// `const json::value&` overload; the pointer overloads are the ones the ChatOnlyResponsesProfile
// function pointers (deepseek/responses.hpp) bind to. `Record<string, unknown>` is json::object.
#include "uos/api_key_policy.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/rt/regex.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace uos::deepseek::index {
  /**
   * Official DeepSeek API transport.
   *
   * This is a "special upstream provider" in the same sense as Cerebras: a
   * request addressed to one of its model ids is dispatched straight to the
   * provider's documented OpenAI-compatible Chat Completions endpoint and never
   * races or falls back to another provider.
   *
   * Provider facts (https://api-docs.deepseek.com, read 2026-09-16):
   * - base URL `https://api.deepseek.com`, Chat Completions at `/chat/completions`
   * - model ids `deepseek-flash` and `deepseek-v4-pro`; the provider's own
   *   `GET /models` lists exactly those two, so both are served here
   * - `deepseek-v4-flash` is an accepted interchangeable alias: the API answers it
   *   with `"model": "deepseek-flash"`. It is NOT `deepseek-v4.1-flash`, which the
   *   API rejects ("The supported API model names are deepseek-flash,
   *   deepseek-v4-pro"). That id belongs to the paid Surplus/Metered catalogue,
   *   not to this provider.
   * - `reasoning_effort` accepts none/low/high/max, plus the documented
   *   compatibility aliases minimal (→low), medium and xhigh (→high)
   * - thinking mode is ENABLED BY DEFAULT and its output arrives as
   *   `message.reasoning_content` / `delta.reasoning_content`
   * - the documented output cap is `max_tokens`, and `temperature` has no effect
   *   while thinking mode is active
   * - streaming is native SSE (`data: {...}` frames terminated by `data: [DONE]`),
   *   with `: keep-alive` comment frames while the model is thinking
   */
  inline constexpr std::string_view DEEPSEEK_FLASH_MODEL = "deepseek-flash";
  /**
   * Interchangeable legacy id for the same official model. It is still accepted
   * by the API and is the id the local Codex model catalog already advertises,
   * so both ids must route here.
   */
  inline constexpr std::string_view DEEPSEEK_V4_FLASH_MODEL = "deepseek-v4-flash";
  /** The provider's second published model, served under its own canonical id. */
  inline constexpr std::string_view DEEPSEEK_PRO_MODEL = "deepseek-v4-pro";
  /** Operator-facing names, kept beside the ids they describe. */
  // `Readonly<Record<string, string>>`, keyed by the three ids above.
  inline const rt::OrderedMap<std::string, std::string> DEEPSEEK_DISPLAY_NAMES{
    {"deepseek-flash", "DeepSeek Flash"},
    {"deepseek-v4-flash", "DeepSeek Flash (legacy id)"},
    {"deepseek-v4-pro", "DeepSeek Pro"},
  };
  inline constexpr std::array<std::string_view, 3> DEEPSEEK_OFFICIAL_MODEL_IDS{DEEPSEEK_FLASH_MODEL, DEEPSEEK_V4_FLASH_MODEL, DEEPSEEK_PRO_MODEL};

  inline constexpr std::string_view DEEPSEEK_CHAT_COMPLETIONS_URL = "https://api.deepseek.com/chat/completions";
  inline constexpr double DEEPSEEK_CONTEXT_WINDOW_TOKENS = 1'000'000;
  /** Documented reasoning tiers, in ascending order. */
  inline constexpr std::array<std::string_view, 4> DEEPSEEK_REASONING_LEVELS{"none", "low", "high", "max"};
  /** Documented default when a request omits `reasoning_effort` (thinking mode on, effort high). */
  inline constexpr std::string_view DEEPSEEK_DEFAULT_REASONING_EFFORT = "high";

  /**
   * Thinking-mode gate for the request parameters DeepSeek refuses while
   * thinking is active. `reasoning_effort` `none` disables thinking mode, and
   * `thinking: { type: "disabled" }` does too; every other documented tier
   * (`low`, `high`, `max`) enables it. An omitted effort leaves the documented
   * default (`high`) in force, so it counts as thinking. When both fields are
   * present, the explicit reasoning effort is the canonical mode.
   */
  bool deepSeekThinkingModeActive(const json::value* reasoningEffort, const json::value* thinking);
  bool deepSeekThinkingModeActive(const json::value& reasoningEffort, const json::value& thinking);

  /**
   * The `tool_choice` values DeepSeek's thinking mode refuses, or null when the
   * value is one the provider accepts in every mode. Only `required` and the
   * named-function form are restricted; `none` and `auto` are always valid.
   */
  // std::nullopt is null.
  std::optional<std::string> deepSeekThinkingModeForbiddenToolChoice(const json::value* toolChoice);
  std::optional<std::string> deepSeekThinkingModeForbiddenToolChoice(const json::value& toolChoice);

  /**
   * The terminal a DeepSeek `finish_reason` implies, before output validity
   * applies. The provider's documented vocabulary is `[ stop, length,
   * content_filter, tool_calls, insufficient_system_resource, aborted ]`, read
   * from the official Chat Completions reference
   * (https://api-docs.deepseek.com/api/create-chat-completion, 2026-09-21).
   */
  struct DeepSeekFinishDispositionCompleted {
    static constexpr std::string_view kind = "completed";
    friend bool operator==(const DeepSeekFinishDispositionCompleted&, const DeepSeekFinishDispositionCompleted&) = default;
  };
  struct DeepSeekFinishDispositionIncomplete {
    static constexpr std::string_view kind = "incomplete";
    std::string reason;  // "max_output_tokens" | "content_filter"
    friend bool operator==(const DeepSeekFinishDispositionIncomplete&, const DeepSeekFinishDispositionIncomplete&) = default;
  };
  struct DeepSeekFinishDispositionFailed {
    static constexpr std::string_view kind = "failed";
    std::string code;
    friend bool operator==(const DeepSeekFinishDispositionFailed&, const DeepSeekFinishDispositionFailed&) = default;
  };
  struct DeepSeekFinishDispositionUnknown {
    static constexpr std::string_view kind = "unknown";
    std::string value;
    friend bool operator==(const DeepSeekFinishDispositionUnknown&, const DeepSeekFinishDispositionUnknown&) = default;
  };
  using DeepSeekFinishDisposition =
    std::variant<DeepSeekFinishDispositionCompleted, DeepSeekFinishDispositionIncomplete, DeepSeekFinishDispositionFailed, DeepSeekFinishDispositionUnknown>;

  /**
   * The single expression of the thinking-mode `tool_choice` restriction: returns
   * the conflicting `tool_choice` value, or null when the request is servable.
   *
   * Both DeepSeek request seams call this so the Chat route and the Responses
   * translation cannot drift, and the returned value feeds one message builder so
   * a client sees the same contract on either route.
   */
  std::optional<std::string> deepSeekThinkingToolChoiceConflict(const json::value* reasoningEffort, const json::value* thinking, const json::value* toolChoice);
  std::optional<std::string> deepSeekThinkingToolChoiceConflict(const json::value& reasoningEffort, const json::value& thinking, const json::value& toolChoice);

  /**
   * The message a client sees when it asked for a `tool_choice` value that only
   * works with thinking mode disabled. It names both conflicting fields in the
   * vocabulary of the request it was sent on, so the caller can act on it without
   * reading provider documentation.
   */
  std::string deepSeekToolChoiceThinkingConflictMessage(const std::string& toolChoice, const std::string& effortField);

  /**
   * Maps a DeepSeek `finish_reason` onto the Responses terminal vocabulary.
   *
   * `length` covers output-budget exhaustion *and* context-window exhaustion, so
   * it maps to `max_output_tokens` — the only reason string the official OpenAI
   * response schema defines for that case. An unrecognized value is reported as
   * unknown rather than being silently treated as a normal stop, which is what
   * the provider's own client does (`mapFinishReason` fail-closes with the
   * uppercased value as its code).
   */
  DeepSeekFinishDisposition deepSeekFinishDisposition(const json::value* reason);
  DeepSeekFinishDisposition deepSeekFinishDisposition(const json::value& reason);

  /** One SSE frame is a single generated chunk; bound it so a peer cannot pin memory. */
  inline constexpr double MAX_DEEPSEEK_SSE_FRAME_BYTES = 4 * 1024 * 1024;
  // `/\r\n\r\n|\n\n|\r\r/` and `/\r\n|\r|\n/`
  inline const rt::Regex SSE_FRAME_BOUNDARY{"\\r\\n\\r\\n|\\n\\n|\\r\\r"};
  inline const rt::Regex SSE_LINE_BOUNDARY{"\\r\\n|\\r|\\n"};

  // `(input: RequestInfo | URL, init?: RequestInit) => Promise<Response>`
  using DeepSeekFetch = rt::Fetch;

  using DeepSeekErrorCode = std::string;  // "deepseek_api_key_missing" | "deepseek_request_invalid" | "deepseek_upstream_unreachable" | "gateway_timeout"

  // `code` is also stored in rt::JsError::code, so code that reads `error.code` off any caught
  // error sees it as JS does.
  class DeepSeekError : public rt::JsError {
  public:
    const DeepSeekErrorCode code;
    const double status;

    DeepSeekError(std::string message_, DeepSeekErrorCode code_, double status_)
      : rt::JsError(std::move(message_), "DeepSeekError"), code(std::move(code_)), status(status_) {
      rt::JsError::code = code;
    }
  };

  struct DeepSeekChatCompletionsOptions {
    // `apiKey?: string | null`: undefined reads DEEPSEEK_API_KEY; null fails as unconfigured.
    rt::Nullable<std::string> apiKey{};
    // An empty Fetch is undefined (the global fetch).
    DeepSeekFetch fetcher{};
    std::optional<rt::AbortSignal> signal{};
    /**
     * Resolves the API-key dispatch admission for this attempt. Both the
     * "claimed a dispatch" and the "nothing to claim" shapes are accepted, which
     * is why this is a union of function types rather than a union that puts
     * `void` inside `Promise<...>`.
     */
    // `(() => Promise<ApiKeyProviderDispatch>) | (() => void)`: the `() => void` form is a hook
    // whose Task yields std::nullopt. An empty function is undefined.
    std::function<rt::Task<std::optional<api_key_policy::ApiKeyProviderDispatch>>()> beforeDispatch{};
    std::function<void()> onDispatch{};
    std::function<void()> onHeaders{};
    /** Observes the exact projected body serialized for this dispatch, before any admission await. */
    std::function<void(const json::object& body)> onProjectedBody{};
    /** Request-owned passive recorder; best effort, never required. */
    std::optional<sentinel::upstream_capture::SentinelUpstreamRecorder> sentinelUpstreamRecorder{};
  };

  // `type NormalizationResult<T> = Readonly<{ ok: true; value: T }> | Readonly<{ ok: false; message: string }>`
  template <class T>
  struct NormalizationResultOk {
    static constexpr bool ok = true;
    T value;
    friend bool operator==(const NormalizationResultOk&, const NormalizationResultOk&) = default;
  };
  struct NormalizationResultNotOk {
    static constexpr bool ok = false;
    std::string message;
    friend bool operator==(const NormalizationResultNotOk&, const NormalizationResultNotOk&) = default;
  };
  template <class T>
  using NormalizationResult = std::variant<NormalizationResultOk<T>, NormalizationResultNotOk>;

  /**
   * Provider request IDs are opaque support-correlation values. Keep only a
   * bounded, header-safe value so an upstream cannot make the gateway reflect
   * arbitrary metadata.
   */
  std::optional<std::string> normalizeDeepSeekProviderRequestId(const json::value* value);
  std::optional<std::string> normalizeDeepSeekProviderRequestId(const json::value& value);

  std::optional<std::string> getDeepSeekProviderRequestId(const rt::Response& response);

  // `error: unknown` is the caught reason: `error instanceof Error && error.name === "TimeoutError"`.
  bool isTimeoutError(const rt::Reason& error);

  std::optional<std::string> readDeepSeekApiKey();

  // `timeoutMs: number | null`: std::nullopt is null (restores STREAM_FIRST_EVENT_DEADLINE_MS).
  void setDeepSeekFetchTimeoutMsForTest(std::optional<double> timeoutMs);

  /**
   * Resolves the canonical upstream model for a client-facing model id, or null
   * when the id is not an official DeepSeek model. Comparison is
   * case-insensitive, matching the gateway's model normalization elsewhere.
   */
  std::optional<std::string> deepSeekUpstreamModelFor(const std::string& model);

  /** Maps a requested reasoning tier onto the documented DeepSeek wire tier. */
  // `DEEPSEEK_WIRE_EFFORT_MAP[effort.trim().toLowerCase()] ?? effort` reads a plain object, so the
  // two all-lowercase Object.prototype names resolve through the prototype: `constructor` yields the
  // `Object` function (std::nullopt here: not nullish in JS, and JSON.stringify drops such a member
  // as it drops undefined) and `__proto__` yields Object.prototype (an empty json::object). Every
  // other effort yields a JSON string.
  std::optional<json::value> projectDeepSeekReasoningEffort(const std::string& effort);

  /**
   * The provider's own effective output allowance when the request omits
   * `max_tokens`, for the reasoning tier being requested.
   *
   * This is observability only: the value is never written into the upstream
   * body, so omitting a cap still sends no cap.
   */
  std::optional<double> deepSeekDefaultOutputAllowance(const std::string& reasoningEffort);

  /**
   * Projects the official Chat Completions body onto DeepSeek's documented wire
   * contract. Only three provider necessities are applied — everything else is
   * forwarded unchanged:
   *
   * 1. `model` becomes the canonical official id, so the interchangeable legacy
   *    alias reaches the API as the model it actually serves.
   * 2. `max_completion_tokens` becomes DeepSeek's documented `max_tokens`. The
   *    gateway's Chat contract only accepts the OpenAI field name, and DeepSeek
   *    documents no `max_completion_tokens`, so leaving it in place would both
   *    lose the cap and send an unrecognized parameter.
   * 3. `developer` messages become `system` messages, because DeepSeek's Chat
   *    contract defines no developer role.
   */
  // Throws DeepSeekError "deepseek_request_invalid" for a model that is not DeepSeek's.
  json::object projectDeepSeekRequest(const json::object& body, const std::string& requestedModel);

  /**
   * Sends only canonical OpenAI Chat Completions JSON to the official DeepSeek
   * API. The caller owns model selection and streaming policy; this transport
   * never chooses or falls back to another provider, and it never rewrites the
   * caller's `stream` flag.
   *
   * The deadline covers response headers only. Once headers arrive the caller
   * owns the stream's inactivity deadline, so a long thinking turn is not killed
   * by the buffered-inference budget.
   */
  rt::Task<rt::Response> fetchDeepSeekChatCompletions(json::object body, std::string requestedModel);
  rt::Task<rt::Response> fetchDeepSeekChatCompletions(json::object body, std::string requestedModel, DeepSeekChatCompletionsOptions options);

  // `number | null`: std::nullopt is null.
  std::optional<double> deepSeekCachedPromptTokens(const json::object& value, double promptTokens);

  std::optional<double> deepSeekReasoningTokens(const json::object& value, double completionTokens);

  /**
   * Reduces a DeepSeek buffered success payload to the OpenAI Chat Completions
   * shape the Assistant consumes. Unknown provider fields, including the
   * `system_fingerprint` and usage breakdowns, are not relayed or logged.
   */
  NormalizationResult<json::object> normalizeDeepSeekChatCompletion(const json::value* value, const std::string& requestedModel);
  NormalizationResult<json::object> normalizeDeepSeekChatCompletion(const json::value& value, const std::string& requestedModel);

  /**
   * Reduces one DeepSeek streaming chunk to the OpenAI `chat.completion.chunk`
   * shape. Chunks are relayed as they arrive, so this validates the frame rather
   * than buffering it.
   */
  NormalizationResult<json::object> normalizeDeepSeekChatCompletionChunk(const json::value* value, const std::string& requestedModel);
  NormalizationResult<json::object> normalizeDeepSeekChatCompletionChunk(const json::value& value, const std::string& requestedModel);
}
