#pragma once
// Port of src/provider/lithos.ts.
// LithosAI API transport — a chat-completions-only upstream provider (see the module comment of
// src/provider/lithos.ts for the probed provider facts).
//
// `value: unknown` parameters have a `const json::value*` (nullptr is undefined) and a
// `const json::value&` overload. `Record<string, unknown>` is json::object.
#include "uos/api_key_policy.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace uos::provider::lithos {
  /** The eight ids `GET /v1/models` returned verbatim on 2026-09-23, case preserved. */
  inline constexpr std::array<std::string_view, 8> LITHOS_MODEL_IDS{
    "deepseek-ai/DeepSeek-V4.1-Flash",
    "deepseek-ai/DeepSeek-V4.1-Flash-fast",
    "deepseek-ai/DeepSeek-V4.1-Flash-ultra",
    "deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat",
    "moonshotai/Kimi-K3",
    "moonshotai/Kimi-K3-fast",
    "moonshotai/Kimi-K3-ultra",
    "moonshotai/Kimi-K3-ultra-chat",
  };

  /** Operator-facing names, kept beside the ids they describe. */
  // `Readonly<Record<string, string>>`
  inline const rt::OrderedMap<std::string, std::string> LITHOS_DISPLAY_NAMES{
    {"deepseek-ai/DeepSeek-V4.1-Flash", "LithosAI DeepSeek V4.1 Flash"},
    {"deepseek-ai/DeepSeek-V4.1-Flash-fast", "LithosAI DeepSeek V4.1 Flash (fast)"},
    {"deepseek-ai/DeepSeek-V4.1-Flash-ultra", "LithosAI DeepSeek V4.1 Flash (ultra)"},
    {"deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat", "LithosAI DeepSeek V4.1 Flash (ultra, chat)"},
    {"moonshotai/Kimi-K3", "LithosAI Kimi K3"},
    {"moonshotai/Kimi-K3-fast", "LithosAI Kimi K3 (fast)"},
    {"moonshotai/Kimi-K3-ultra", "LithosAI Kimi K3 (ultra)"},
    {"moonshotai/Kimi-K3-ultra-chat", "LithosAI Kimi K3 (ultra, chat)"},
  };

  inline constexpr std::string_view LITHOS_API_KEY_ENV = "LITHOSAI_API_KEY";
  inline constexpr std::string_view LITHOS_CHAT_COMPLETIONS_URL = "https://api.lithosai.cloud/v1/chat/completions";
  inline constexpr double LITHOS_CONTEXT_WINDOW_TOKENS = 1'048'576;
  /**
   * The full advertised window, not the 95 percent margin other providers keep:
   * this route is served at the size LithosAI publishes.
   */
  inline constexpr double LITHOS_EFFECTIVE_CONTEXT_WINDOW_PERCENT = 100;

  /**
   * The seven `reasoning_effort` values the API accepted on 2026-09-23, in the
   * order they were advertised. They are sent verbatim, so this is the whole
   * advertised tier list: no value was added, reordered into a ladder that was
   * not observed, or omitted.
   */
  inline constexpr std::array<std::string_view, 7> LITHOS_REASONING_LEVELS{"none", "minimal", "low", "medium", "high", "xhigh", "max"};

  using LithosReasoningLevel = std::string;  // (typeof LITHOS_REASONING_LEVELS)[number]

  /**
   * Gateway default when a request omits `reasoning_effort`. The provider's own
   * default was NOT probed (every probe passed an explicit tier), so this is a
   * local choice — the middle verified tier — and not a vendor-documented value.
   */
  inline constexpr std::string_view LITHOS_DEFAULT_REASONING_EFFORT = "medium";

  /**
   * The six rate-limit response headers every LithosAI response and refusal
   * carried on 2026-09-23. They are the same names the Sentinel upstream capture
   * allowlist already retains; there is deliberately no provider request-id
   * header in this list, because the provider sends none.
   */
  inline constexpr std::array<std::string_view, 6> LITHOS_RATE_LIMIT_HEADERS{
    "x-ratelimit-limit-requests", "x-ratelimit-remaining-requests", "x-ratelimit-reset-requests",
    "x-ratelimit-limit-tokens",   "x-ratelimit-remaining-tokens",   "x-ratelimit-reset-tokens",
  };

  // "lithos_api_key_missing" | "lithos_request_invalid" | "lithos_upstream_unreachable" |
  // "lithos_upstream_invalid_response" | "gateway_timeout"
  using LithosErrorCode = std::string;

  // `code` is also stored in rt::JsError::code, so code that reads `error.code` off any caught
  // error sees it as JS does.
  class LithosError : public rt::JsError {
  public:
    const LithosErrorCode code;
    const double status;

    LithosError(std::string message_, LithosErrorCode code_, double status_)
      : rt::JsError(std::move(message_), "LithosError"), code(std::move(code_)), status(status_) {
      rt::JsError::code = code;
    }
  };

  // `(input: RequestInfo | URL, init?: RequestInit) => Promise<Response>`
  using LithosFetch = rt::Fetch;

  struct LithosChatCompletionsOptions {
    // `apiKey?: string | null`: undefined reads LITHOSAI_API_KEY; null fails as unconfigured.
    rt::Nullable<std::string> apiKey{};
    // An empty Fetch is undefined (the global fetch).
    LithosFetch fetcher{};
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
    /** Request-owned passive recorder; best effort, never required. */
    std::optional<sentinel::upstream_capture::SentinelUpstreamRecorder> sentinelUpstreamRecorder{};
  };

  // `timeoutMs: number | null`: std::nullopt is null (restores STREAM_FIRST_EVENT_DEADLINE_MS).
  void setLithosFetchTimeoutMsForTest(std::optional<double> timeoutMs);

  // `type NormalizationResult<T> = Readonly<{ ok: true; value: T }> | Readonly<{ ok: false; message: string }>`
  // (module-private in the TS, like deepseek/index.ts's twin).
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
   * arbitrary metadata. This mirrors the DeepSeek normalizer even though the
   * LithosAI wire currently has nothing to normalize.
   */
  // std::nullopt is null.
  std::optional<std::string> normalizeLithosProviderRequestId(const json::value* value);
  std::optional<std::string> normalizeLithosProviderRequestId(const json::value& value);

  /**
   * The provider publishes NO request-id header at all: an admitted response and
   * a refusal were both inspected on 2026-09-23 and neither carried one, so this
   * seam has nothing documented to read. It deliberately reads no header name, so
   * the only honest answer for any response is null. Do not guess a header here;
   * re-probe the vendor first.
   */
  std::optional<std::string> getLithosProviderRequestId(const rt::Response& _response);

  std::optional<std::string> readLithosApiKey();

  /**
   * Resolves the key for one dispatch. A supplied empty or whitespace-only value
   * is treated as absent rather than as an override that switches the provider
   * off, exactly as the Cerebras and DeepSeek seams do.
   */
  // `supplied?: string | null`: the overload without it (and an undefined Nullable) reads the
  // environment. Throws LithosError "lithos_api_key_missing" (503).
  std::string requireLithosApiKey();
  std::string requireLithosApiKey(const rt::Nullable<std::string>& supplied);

  /**
   * Canonical upstream id for every client-facing id the provider publishes.
   *
   * This is an identity map: all eight ids are sent verbatim, and no alias,
   * casing or legacy spelling was observed to be rewritten by the API. It exists
   * so the dispatch seam has one table to read, exactly as the DeepSeek map does,
   * and so a future alias has one place to be declared.
   */
  // `Object.freeze(Object.fromEntries(LITHOS_MODEL_IDS.map((model) => [model, model])))`, written out.
  inline const rt::OrderedMap<std::string, std::string> LITHOS_UPSTREAM_MODEL_BY_ID{
    {"deepseek-ai/DeepSeek-V4.1-Flash", "deepseek-ai/DeepSeek-V4.1-Flash"},
    {"deepseek-ai/DeepSeek-V4.1-Flash-fast", "deepseek-ai/DeepSeek-V4.1-Flash-fast"},
    {"deepseek-ai/DeepSeek-V4.1-Flash-ultra", "deepseek-ai/DeepSeek-V4.1-Flash-ultra"},
    {"deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat", "deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat"},
    {"moonshotai/Kimi-K3", "moonshotai/Kimi-K3"},
    {"moonshotai/Kimi-K3-fast", "moonshotai/Kimi-K3-fast"},
    {"moonshotai/Kimi-K3-ultra", "moonshotai/Kimi-K3-ultra"},
    {"moonshotai/Kimi-K3-ultra-chat", "moonshotai/Kimi-K3-ultra-chat"},
  };

  /**
   * Resolves the canonical upstream model for a client-facing model id, or null
   * when the id is not a LithosAI model. Comparison is case-insensitive, matching
   * the gateway's model normalization elsewhere.
   */
  std::optional<std::string> lithosUpstreamModelFor(const std::string& model);

  /**
   * Projects the OpenAI Chat Completions body onto LithosAI's wire contract. One
   * provider necessity is applied — everything else is forwarded unchanged:
   *
   * 1. `model` becomes the canonical id from the same table the lookup reads.
   * 2. `max_completion_tokens` becomes this vendor's documented `max_tokens`. The
   *    gateway's Chat contract only accepts the OpenAI field name, and LithosAI
   *    documents no `max_completion_tokens`, so leaving it in place would both
   *    lose the cap and send an unrecognized parameter.
   *
   * `reasoning_effort` is deliberately NOT rewritten: this provider accepted
   * exactly none/minimal/low/medium/high/xhigh/max verbatim on 2026-09-23 and
   * refused anything else with a 400, so DeepSeek's `ultra`→`max` map must not
   * run here. An unknown id is a client error, not an upstream one.
   */
  // Throws LithosError "lithos_request_invalid" (400) for an id that is not LithosAI's.
  json::object projectLithosRequest(const json::object& body, const std::string& requestedModel);

  /**
   * This provider is chat-completions-only: `POST /v1/responses` answers 404
   * (probed 2026-09-23), so there is deliberately no Responses route here.
   *
   * The deadline covers response headers only. Once headers arrive the caller
   * owns the stream's inactivity deadline, so a long thinking turn is not killed
   * by the buffered-inference budget.
   */
  rt::Task<rt::Response> fetchLithosChatCompletions(json::object body, std::string requestedModel);
  rt::Task<rt::Response> fetchLithosChatCompletions(json::object body, std::string requestedModel, LithosChatCompletionsOptions options);

  /**
   * `prompt_tokens_details.cached_tokens`, or null when the provider measured no
   * cache hit (its ordinary wire shape is an explicit `null` details object) or
   * reported an impossible count. Exported so the provider adapter can reuse the
   * exact guards `normalizeUsage` applies instead of restating them.
   */
  std::optional<double> lithosCachedPromptTokens(const json::object& value, double promptTokens);

  /**
   * `completion_tokens_details.reasoning_tokens`, or null when the provider
   * reported no reasoning measurement or an impossible count. Exported beside
   * the cache guard so both detail counters have one definition.
   */
  std::optional<double> lithosReasoningTokens(const json::object& value, double completionTokens);

  /**
   * Reduces a LithosAI buffered success payload to the OpenAI Chat Completions
   * shape the Assistant consumes. Unknown provider fields, including diagnostics,
   * are not relayed or logged.
   */
  // `servedModel?: string`: std::nullopt is undefined.
  NormalizationResult<json::object> normalizeLithosChatCompletion(const json::value* value, const std::string& requestedModel,
                                                                  const std::optional<std::string>& servedModel = std::nullopt);
  NormalizationResult<json::object> normalizeLithosChatCompletion(const json::value& value, const std::string& requestedModel,
                                                                  const std::optional<std::string>& servedModel = std::nullopt);

  NormalizationResult<json::object> normalizeLithosChatCompletionChunk(const json::value* value, const std::string& requestedModel,
                                                                       const std::optional<std::string>& servedModel = std::nullopt);
  NormalizationResult<json::object> normalizeLithosChatCompletionChunk(const json::value& value, const std::string& requestedModel,
                                                                       const std::optional<std::string>& servedModel = std::nullopt);

  // The `options` of iterateLithosChatCompletionStream: `Readonly<{ signal?: AbortSignal; servedModel?: string }>`.
  struct IterateLithosChatCompletionStreamOptions {
    std::optional<rt::AbortSignal> signal{};
    std::optional<std::string> servedModel{};
  };

  /**
   * Reads the upstream Chat Completions SSE body and yields validated
   * `chat.completion.chunk` objects, so the caller relays them without
   * re-checking the wire shape.
   *
   * Usage is NOT gated here: this provider reports it unconditionally, including
   * on the trailing chunk whose `choices` is an empty array, which
   * `normalizeLithosChatCompletionChunk` accepts. There is deliberately no
   * `stream_options.include_usage` condition.
   */
  // `async function*`: AsyncGenerator<Record<string, unknown>, void, void>.
  rt::AsyncGenerator<json::object> iterateLithosChatCompletionStream(rt::Response response, std::string requestedModel);
  rt::AsyncGenerator<json::object> iterateLithosChatCompletionStream(rt::Response response, std::string requestedModel, IterateLithosChatCompletionStreamOptions options);
}
