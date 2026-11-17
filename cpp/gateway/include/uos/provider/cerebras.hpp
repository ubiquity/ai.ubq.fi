#pragma once
// Port of src/provider/cerebras.ts.
//
// `value: unknown` parameters have a `const json::value*` (nullptr is undefined) and a
// `const json::value&` overload; an `unknown` result of an undefined input is std::nullopt.
// `Record<string, unknown>` is json::object.
#include "uos/api_key_policy.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/prelude.hpp"
#include "uos/sentinel/upstream_capture.hpp"

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace uos::provider::cerebras {
  inline constexpr std::string_view CEREBRAS_GPT_OSS_120B_MODEL = "gpt-oss-120b";
  inline constexpr std::string_view CEREBRAS_QWEN_3_8_27B_MODEL = "qwen-3.8-27b";
  inline constexpr std::string_view CEREBRAS_CHAT_COMPLETIONS_URL = "https://api.cerebras.ai/v1/chat/completions";

  /**
   * Every Cerebras id this route serves, in advertised order. The provider used
   * to expose exactly one model, so the routing predicates compared against a
   * single constant; keep the ids in one list so a catalog row and its dispatch
   * rule cannot drift apart again.
   *
   * Per-model capability facts (context window, reasoning tiers and defaults)
   * live with the hints in `src/request-policy.ts`, which is their only consumer.
   */
  inline constexpr std::array<std::string_view, 2> CEREBRAS_MODELS{CEREBRAS_GPT_OSS_120B_MODEL, CEREBRAS_QWEN_3_8_27B_MODEL};

  /**
   * Resolves a client-supplied id to the Cerebras wire id it names, or null when
   * this route does not own it. Matching is case-insensitive because model ids
   * are opaque and clients normalize case inconsistently; the returned value is
   * always the provider's exact spelling so the upstream sees a known id.
   */
  std::optional<std::string> cerebrasUpstreamModelFor(const std::string& model);

  // `(input: RequestInfo | URL, init?: RequestInit) => Promise<Response>`
  using CerebrasFetch = rt::Fetch;

  using CerebrasErrorCode = std::string;  // "cerebras_api_key_missing" | "cerebras_request_invalid" | "cerebras_upstream_unreachable" | "gateway_timeout"

  // `code` is also stored in rt::JsError::code, so code that reads `error.code` off any caught
  // error sees it as JS does.
  class CerebrasError : public rt::JsError {
  public:
    const CerebrasErrorCode code;
    const double status;

    CerebrasError(std::string message_, CerebrasErrorCode code_, double status_)
      : rt::JsError(std::move(message_), "CerebrasError"), code(std::move(code_)), status(status_) {
      rt::JsError::code = code;
    }
  };

  struct CerebrasChatCompletionsOptions {
    // `apiKey?: string | null`: undefined reads CEREBRAS_API_KEY; null fails as unconfigured.
    rt::Nullable<std::string> apiKey{};
    // An empty Fetch is undefined (the global fetch).
    CerebrasFetch fetcher{};
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
   * arbitrary metadata.
   */
  // std::nullopt is null.
  std::optional<std::string> normalizeCerebrasProviderRequestId(const json::value* value);
  std::optional<std::string> normalizeCerebrasProviderRequestId(const json::value& value);

  std::optional<std::string> getCerebrasProviderRequestId(const rt::Response& response);

  std::optional<std::string> readCerebrasApiKey();

  // `timeoutMs: number | null`: std::nullopt is null (restores BUFFERED_INFERENCE_DEADLINE_MS).
  void setCerebrasFetchTimeoutMsForTest(std::optional<double> timeoutMs);

  // `(value: unknown): unknown`.
  std::optional<json::value> projectCerebrasToolSchema(const json::value* value);
  json::value projectCerebrasToolSchema(const json::value& value);

  /**
   * Cerebras applies the model's chat template strictly, and the Qwen template
   * accepts exactly one `system` message, at index 0: a second `system` message,
   * or one that follows another message, fails the whole turn before inference
   * with "Failed to apply chat template to messages due to error: System message
   * must be at the beginning." (`wrong_api_format`, verified live 2026-09-28
   * against qwen-3.8-27b). The shared Responses translation emits one `system`
   * message per `instructions` plus one for each `developer` input item, and a
   * Codex client sends both, so the Cerebras wires have to collapse them into the
   * single leading message the template accepts.
   *
   * Every `system` message's text is kept, in original order, joined with a blank
   * line; unsupported content is rejected before dispatch rather than dropped.
   * All other messages keep their relative order. A body that already holds at
   * most one text-only `system` message, at index 0, is returned unchanged, so
   * an ordinary request is never rewritten. The collapse runs for both ids on
   * this route: `gpt-oss-120b`'s harmony template tolerates the extra message,
   * but one wire shape must not drift by model.
   */
  // `(messages: unknown): unknown`; throws CerebrasError for unsupported system content.
  std::optional<json::value> collapseCerebrasSystemMessages(const json::value* messages);
  json::value collapseCerebrasSystemMessages(const json::value& messages);

  // Throws CerebrasError "cerebras_request_invalid" for a body the wire cannot carry.
  json::object projectCerebrasRequest(const json::object& body);

  rt::Task<rt::Response> fetchCerebrasChatCompletions(json::object body);
  rt::Task<rt::Response> fetchCerebrasChatCompletions(json::object body, CerebrasChatCompletionsOptions options);

  /**
   * `prompt_tokens_details.cached_tokens`, or null when the provider reported no
   * cache measurement or an impossible count. Exported so the provider adapter
   * can reuse the exact guard `normalizeUsage` applies instead of restating it.
   */
  std::optional<double> cerebrasCachedPromptTokens(const json::object& value, double promptTokens);

  /**
   * `completion_tokens_details.reasoning_tokens`, or null when the provider
   * reported no reasoning measurement or an impossible count. Exported beside
   * the cache guard so both detail counters have one definition.
   */
  std::optional<double> cerebrasReasoningTokens(const json::object& value, double completionTokens);

  /**
   * Reduces a Cerebras success payload to the OpenAI Chat Completions shape the
   * Assistant consumes. Unknown provider fields, including diagnostics, are not
   * relayed or logged.
   */
  NormalizationResult<json::object> normalizeCerebrasChatCompletion(const json::value* value, const std::string& requestedModel);
  NormalizationResult<json::object> normalizeCerebrasChatCompletion(const json::value& value, const std::string& requestedModel);
}
