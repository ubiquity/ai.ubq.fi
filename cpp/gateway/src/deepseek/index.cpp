// Port of src/deepseek/index.ts.
#include "uos/deepseek/index.hpp"

#include "uos/inference_deadline.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uos::deepseek::index {
  namespace {
    /**
     * Canonical upstream id for every client-facing id the provider publishes or
     * accepts. The legacy flash alias is the only id that is not sent verbatim: the
     * API serves it as `deepseek-flash`.
     */
    // `Readonly<Record<string, string>>`, read only through `Object.hasOwn`.
    const std::unordered_map<std::string, std::string>& DEEPSEEK_UPSTREAM_MODEL_BY_ID() {
      static const std::unordered_map<std::string, std::string> models{
        {std::string(DEEPSEEK_FLASH_MODEL), std::string(DEEPSEEK_FLASH_MODEL)},
        {std::string(DEEPSEEK_V4_FLASH_MODEL), std::string(DEEPSEEK_FLASH_MODEL)},
        {std::string(DEEPSEEK_PRO_MODEL), std::string(DEEPSEEK_PRO_MODEL)},
      };
      return models;
    }

    constexpr std::string_view DEEPSEEK_API_KEY_ENV = "DEEPSEEK_API_KEY";
    constexpr std::int64_t MAX_DEEPSEEK_PROVIDER_REQUEST_ID_LENGTH = 256;
    // `DEEPSEEK_OFFICIAL_MODEL_ID_SET.has(id)`
    bool isDeepSeekOfficialModelId(std::string_view id) {
      return std::find(DEEPSEEK_OFFICIAL_MODEL_IDS.begin(), DEEPSEEK_OFFICIAL_MODEL_IDS.end(), id) != DEEPSEEK_OFFICIAL_MODEL_IDS.end();
    }
    /**
     * Advanced Codex CLI presets are translated to the documented DeepSeek tier at
     * the provider boundary, exactly as the Codex Responses bridge does.
     */
    // `DEEPSEEK_WIRE_EFFORT_MAP[key]` on the plain object `{ ultra: "max" }`: its own member, else
    // what Object.prototype holds under that name. The callers lowercase the key, and only
    // `constructor` and `__proto__` among Object.prototype's names are all lowercase. Absent:
    // std::nullopt; the inherited `Object` function: a held std::nullopt.
    std::optional<std::optional<json::value>> DEEPSEEK_WIRE_EFFORT_MAP(std::string_view key) {
      if (key == "ultra") return std::optional<json::value>(json::value("max"));
      if (key == "constructor") return std::optional<json::value>();
      if (key == "__proto__") return std::optional<json::value>(json::value(json::object()));
      return std::nullopt;
    }

    double deepSeekFetchTimeoutMs = inference_deadline::STREAM_FIRST_EVENT_DEADLINE_MS;

    template <class T> NormalizationResult<T> ok(T value) { return NormalizationResultOk<T>{std::move(value)}; }
    NormalizationResultNotOk notOk(std::string message) { return NormalizationResultNotOk{std::move(message)}; }
    template <class T> bool failed(const NormalizationResult<T>& result) { return std::holds_alternative<NormalizationResultNotOk>(result); }
    template <class T> const NormalizationResultNotOk& failureOf(const NormalizationResult<T>& result) { return std::get<NormalizationResultNotOk>(result); }
    template <class T> T& valueOf(NormalizationResult<T>& result) { return std::get<NormalizationResultOk<T>>(result).value; }

    std::string numberText(std::size_t index) { return std::to_string(index); }

    std::optional<std::string> nonEmptyString(const json::value* value) {
      const std::optional<std::string> raw = utils::getString(value);
      const std::string text = raw ? rt::trim(*raw) : std::string();
      return text.empty() ? std::nullopt : std::optional<std::string>(text);
    }
    // The same for a value read from the environment (`string | undefined`).
    std::optional<std::string> nonEmptyString(const std::optional<std::string>& value) {
      if (!value) return std::nullopt;
      const json::value text(*value);
      return nonEmptyString(&text);
    }

    /** Tool IDs and names are opaque OpenAI wire values, not display strings. */
    std::optional<std::string> exactNonEmptyString(const json::value* value) {
      if (!value || !value->is_string()) return std::nullopt;
      std::string text(value->get_string());
      if (text.empty() || rt::trim(text) != text) return std::nullopt;
      return text;
    }

    std::optional<double> nonNegativeInteger(const json::value* value) {
      const std::optional<double> number = value ? rt::getNumber(*value) : std::nullopt;
      if (!number || !std::isfinite(*number)) return std::nullopt;
      const double normalized = rt::Math::trunc(*number);
      return normalized >= 0 ? std::optional<double>(normalized) : std::nullopt;
    }

    std::optional<std::string> getEnv(std::string_view key) {
      try {
        return rt::env::get(key);
      } catch (...) {
        return std::nullopt;
      }
    }

    rt::Reason timeoutError() { return rt::makeReason(DeepSeekError("Upstream request exceeded the gateway deadline.", "gateway_timeout", 504)); }

    rt::Reason abortError(const rt::AbortSignal& signal) {
      return rt::isError(signal.reason()) ? signal.reason() : rt::makeReason(rt::DOMException("The request was aborted.", "AbortError"));
    }

    // `error instanceof DeepSeekError` / `error instanceof ApiKeyQuotaDispatchError`.
    bool isDeepSeekError(const rt::Reason& error) {
      if (!error) return false;
      try {
        std::rethrow_exception(error);
      } catch (const DeepSeekError&) {
        return true;
      } catch (...) {
        return false;
      }
    }
    bool isApiKeyQuotaDispatchError(const rt::Reason& error) {
      if (!error) return false;
      try {
        std::rethrow_exception(error);
      } catch (const api_key_policy::ApiKeyQuotaDispatchError&) {
        return true;
      } catch (...) {
        return false;
      }
    }

    // `supplied: string | null | undefined`.
    std::string requireDeepSeekApiKey(const rt::Nullable<std::string>& supplied) {
      std::optional<std::string> apiKey;
      if (supplied.isUndefined()) apiKey = readDeepSeekApiKey();
      else if (supplied.hasValue()) apiKey = nonEmptyString(std::optional<std::string>(*supplied));
      if (apiKey) return *apiKey;
      throw DeepSeekError("The requested model is not configured.", "deepseek_api_key_missing", 503);
    }

    /**
     * DeepSeek's Chat contract defines no `developer` role, while the gateway's
     * Chat input accepts the OpenAI one. Forwarding it unchanged would send the
     * provider a role its contract does not define, so the role is translated
     * here, exactly as the Responses translator already does
     * (`src/deepseek/chat-projection.ts`). Message order and content, and every
     * other role, are forwarded unchanged; messages are copied rather than
     * mutated so the caller's own request record is never rewritten in place.
     */
    json::value projectDeepSeekMessages(const json::value& messages) {
      if (!messages.is_array()) return messages;
      json::array mapped;
      mapped.reserve(messages.get_array().size());
      for (const json::value& message : messages.get_array()) {
        const json::value* role = utils::isRecord(message) && !message.is_array() ? rt::get(message, "role") : nullptr;
        if (role && role->is_string() && role->get_string() == "developer") {
          json::object projected = rt::objectSpread(message.get_object());
          rt::objectDefine(projected, "role", json::value("system"));
          mapped.push_back(std::move(projected));
        } else {
          mapped.push_back(message);
        }
      }
      return mapped;
    }

    /**
     * Classifies one aborted dispatch, before any upstream response exists. The
     * request signal decides first because its reason carries the caller's own
     * abort/timeout identity.
     */
    rt::Reason abortedTransportError(const rt::AbortSignal& signal, const std::optional<rt::AbortSignal>& requestSignal, const rt::AbortSignal& deadlineSignal) {
      if (requestSignal && isTimeoutError(abortError(*requestSignal))) return timeoutError();
      if (deadlineSignal.aborted()) return timeoutError();
      return abortError(signal);
    }

    /**
     * Classifies a failed attempt: an aborted request signal keeps its own
     * identity, a deadline or timeout failure becomes the gateway timeout, and
     * everything else is an unreachable upstream.
     */
    rt::Reason transportFailureError(const rt::Reason& error, const std::optional<rt::AbortSignal>& requestSignal, const rt::AbortSignal& deadlineSignal) {
      if (requestSignal && requestSignal->aborted()) return isTimeoutError(abortError(*requestSignal)) ? timeoutError() : abortError(*requestSignal);
      if (deadlineSignal.aborted() || isTimeoutError(error)) return timeoutError();
      return rt::makeReason(DeepSeekError("Upstream request could not be completed.", "deepseek_upstream_unreachable", 502));
    }

    /** Upstream `null`, `undefined` and strings are the only accepted nullable text values. */
    bool isAbsentOrString(const json::value* value) { return !value || value->is_null() || value->is_string(); }

    bool acceptUpstreamModel(const json::value* value) {
      const std::optional<std::string> upstreamModel = nonEmptyString(value);
      return !upstreamModel || isDeepSeekOfficialModelId(rt::toLowerCase(*upstreamModel));
    }

    NormalizationResult<json::object> normalizeToolCall(const json::value& value, std::size_t index) {
      if (!utils::isRecord(value) || value.is_array()) {
        return notOk("Upstream tool call " + numberText(index) + " is not an object.");
      }
      const json::object& call = value.get_object();
      const std::optional<std::string> id = exactNonEmptyString(rt::get(call, "id"));
      if (!id) return notOk("Upstream tool call " + numberText(index) + " is missing an id.");
      const json::value* type = rt::get(call, "type");
      if (!(type && type->is_string() && type->get_string() == "function")) {
        return notOk("Upstream tool call " + numberText(index) + " has an unsupported type.");
      }
      const json::value* function = rt::get(call, "function");
      if (!utils::isRecord(function) || function->is_array()) {
        return notOk("Upstream tool call " + numberText(index) + " is missing its function.");
      }
      const std::optional<std::string> name = exactNonEmptyString(rt::get(function->get_object(), "name"));
      if (!name) return notOk("Upstream tool call " + numberText(index) + " is missing a function name.");
      const json::value* arguments = rt::get(function->get_object(), "arguments");
      if (!(arguments && arguments->is_string())) {
        return notOk("Upstream tool call " + numberText(index) + " has non-string arguments.");
      }
      // OpenAI's Chat contract represents function arguments as an opaque string.
      // Do not parse or reserialize it: the Assistant validates it in its shared
      // application pipeline after this provider adapter returns.
      json::object normalizedFunction;
      normalizedFunction["name"] = *name;
      normalizedFunction["arguments"] = *arguments;
      json::object normalized;
      normalized["id"] = *id;
      normalized["type"] = "function";
      normalized["function"] = std::move(normalizedFunction);
      return ok(std::move(normalized));
    }

    /** Retains an Optional OpenAI wire field only when the upstream frame actually carries it. */
    // `normalize` returns std::nullopt for the TS `null`; the result's std::nullopt is undefined.
    template <class T>
    NormalizationResult<std::optional<T>> optionalField(const json::value* value, std::optional<T> (*normalize)(const json::value& input), const std::string& message) {
      if (!value || value->is_null()) return ok(std::optional<T>());
      std::optional<T> normalized = normalize(*value);
      if (!normalized) return notOk(message);
      return ok(std::move(normalized));
    }

    std::optional<std::string> stringInput(const json::value& input) {
      return input.is_string() ? std::optional<std::string>(std::string(input.get_string())) : std::nullopt;
    }

    /** Normalizes the `function` object of one streaming tool-call delta. */
    NormalizationResult<json::object> normalizeToolCallDeltaFunction(const json::value& value, const std::string& label) {
      if (!utils::isRecord(value) || value.is_array()) return notOk(label + " has an invalid function.");
      const json::object& function = value.get_object();
      json::object fn;
      NormalizationResult<std::optional<std::string>> name = optionalField<std::string>(
        rt::get(function, "name"), [](const json::value& input) { return exactNonEmptyString(&input); }, label + " has an invalid function name.");
      if (failed(name)) return failureOf(name);
      if (valueOf(name)) fn["name"] = *valueOf(name);
      NormalizationResult<std::optional<std::string>> args = optionalField<std::string>(rt::get(function, "arguments"), stringInput, label + " has non-string arguments.");
      if (failed(args)) return failureOf(args);
      if (valueOf(args)) fn["arguments"] = *valueOf(args);
      return ok(std::move(fn));
    }

    /**
     * Normalizes a STREAMING tool-call delta. Unlike a buffered tool call, a delta
     * legitimately omits `id`, `type` and even `function.name` on continuation
     * frames, so only the fields this frame actually carries are retained.
     */
    NormalizationResult<json::object> normalizeToolCallDelta(const json::value& value, std::size_t index) {
      const std::string label = "Upstream tool call delta " + numberText(index);
      if (!utils::isRecord(value) || value.is_array()) return notOk(label + " is not an object.");
      const json::object& call = value.get_object();
      json::object delta;
      NormalizationResult<std::optional<double>> callIndex = optionalField<double>(
        rt::get(call, "index"), [](const json::value& input) { return nonNegativeInteger(&input); }, label + " has an invalid index.");
      if (failed(callIndex)) return failureOf(callIndex);
      if (valueOf(callIndex)) delta["index"] = rt::makeNumber(*valueOf(callIndex));
      NormalizationResult<std::optional<std::string>> id =
        optionalField<std::string>(rt::get(call, "id"), [](const json::value& input) { return exactNonEmptyString(&input); }, label + " has an invalid id.");
      if (failed(id)) return failureOf(id);
      if (valueOf(id)) delta["id"] = *valueOf(id);
      NormalizationResult<std::optional<std::string>> type = optionalField<std::string>(
        rt::get(call, "type"),
        [](const json::value& input) { return input.is_string() && input.get_string() == "function" ? std::optional<std::string>("function") : std::nullopt; },
        label + " has an unsupported type.");
      if (failed(type)) return failureOf(type);
      if (valueOf(type)) delta["type"] = *valueOf(type);
      const json::value* function = rt::get(call, "function");
      if (function && !function->is_null()) {
        NormalizationResult<json::object> fn = normalizeToolCallDeltaFunction(*function, label);
        if (failed(fn)) return failureOf(fn);
        if (!valueOf(fn).empty()) delta["function"] = std::move(valueOf(fn));
      }
      if (delta.empty()) return notOk(label + " carries no fields.");
      return ok(std::move(delta));
    }

    // The result's std::nullopt is undefined.
    NormalizationResult<std::optional<json::array>> normalizeToolCalls(const json::value* rawToolCalls, const std::string& label,
                                                                       NormalizationResult<json::object> (*normalizeOne)(const json::value& value, std::size_t index)) {
      if (!rawToolCalls) return ok(std::optional<json::array>());
      if (!rawToolCalls->is_array()) return notOk(label + " has invalid tool calls.");
      json::array toolCalls;
      std::size_t callIndex = 0;
      for (const json::value& call : rawToolCalls->get_array()) {
        NormalizationResult<json::object> normalized = normalizeOne(call, callIndex++);
        if (failed(normalized)) return failureOf(normalized);
        toolCalls.push_back(std::move(valueOf(normalized)));
      }
      return ok(std::optional<json::array>(std::move(toolCalls)));
    }

    json::object singleCounter(std::string_view key, double counter) {
      json::object out;
      out[key] = rt::makeNumber(counter);
      return out;
    }

    /**
     * Reduces a DeepSeek usage object to the OpenAI Chat Completions usage shape
     * the Assistant consumes. DeepSeek's provider-named cache counter is the only
     * cache signal the provider publishes, so it is relayed under the official
     * `prompt_tokens_details.cached_tokens` field rather than its provider-only
     * name; `prompt_cache_miss_tokens` needs no slot because the miss count is
     * already `prompt_tokens - cached_tokens`. Thinking mode's reasoning count is
     * relayed the same way under `completion_tokens_details.reasoning_tokens`. When
     * the upstream reports neither counter the matching detail object stays absent,
     * so no reader downstream can mistake a missing measurement for a measured zero.
     */
    // The result's std::nullopt is null.
    NormalizationResult<std::optional<json::object>> normalizeUsage(const json::value* value) {
      if (!value || value->is_null()) return ok(std::optional<json::object>());
      if (!utils::isRecord(value) || value->is_array()) return notOk("Upstream usage is not an object.");
      const json::object& usage = value->get_object();
      const std::optional<double> promptTokens = nonNegativeInteger(rt::get(usage, "prompt_tokens"));
      const std::optional<double> completionTokens = nonNegativeInteger(rt::get(usage, "completion_tokens"));
      const std::optional<double> totalTokens = nonNegativeInteger(rt::get(usage, "total_tokens"));
      if (!promptTokens || !completionTokens || !totalTokens) {
        return notOk("Upstream usage is incomplete.");
      }
      const std::optional<double> cachedTokens = deepSeekCachedPromptTokens(usage, *promptTokens);
      const std::optional<double> reasoningTokens = deepSeekReasoningTokens(usage, *completionTokens);
      json::object normalized;
      normalized["prompt_tokens"] = rt::makeNumber(*promptTokens);
      normalized["completion_tokens"] = rt::makeNumber(*completionTokens);
      normalized["total_tokens"] = rt::makeNumber(*totalTokens);
      if (cachedTokens) normalized["prompt_tokens_details"] = singleCounter("cached_tokens", *cachedTokens);
      if (reasoningTokens) normalized["completion_tokens_details"] = singleCounter("reasoning_tokens", *reasoningTokens);
      return ok(std::optional<json::object>(std::move(normalized)));
    }

    /**
     * The official Chat chunk/message schema types `refusal` as an optional string
     * or `null`. A string is payload this gateway must carry (a refusal is
     * answer-bearing output); `null` is absence; any other type is malformed input
     * and is rejected rather than silently dropped.
     */
    // The result's std::nullopt is null.
    NormalizationResult<std::optional<std::string>> normalizeRefusal(const json::value* value, const std::string& label) {
      if (!value || value->is_null()) return ok(std::optional<std::string>());
      if (value->is_string()) return ok(std::optional<std::string>(std::string(value->get_string())));
      return notOk(label + " has an invalid refusal.");
    }

    bool hasToolCalls(const std::optional<json::array>& toolCalls) { return toolCalls && !toolCalls->empty(); }

    // `content` and `reasoning` nullptr are undefined; `refusal` std::nullopt is null.
    bool choiceHasNoPayload(const json::value* content, const json::value* reasoning, const std::optional<std::string>& refusal,
                            const std::optional<json::array>& toolCalls) {
      return !content && !reasoning && !rt::truthy(refusal) && !hasToolCalls(toolCalls);
    }

    /** Mirrors the upstream `content` field: absent content becomes explicit JSON `null` only when another payload exists. */
    json::value choiceContent(const json::value* content, const std::optional<json::array>& toolCalls) {
      if (content && !content->is_null()) return *content;
      return hasToolCalls(toolCalls) ? json::value(nullptr) : json::value("");
    }

    // `Readonly<{ content: unknown; reasoning: string | null; refusal: string | null; toolCalls: Record<string, unknown>[] | undefined }>`
    struct ChoiceMessagePart {
      const json::value* content;  // nullptr is undefined; points into the message
      std::optional<std::string> reasoning;
      std::optional<std::string> refusal;
      std::optional<json::array> toolCalls;
    };

    NormalizationResult<ChoiceMessagePart> normalizeChoiceMessage(const json::object& message, std::size_t index) {
      const json::value* role = rt::get(message, "role");
      if (!(role && role->is_string() && role->get_string() == "assistant")) {
        return notOk("Upstream choice " + numberText(index) + " does not contain an assistant message.");
      }
      const json::value* content = rt::get(message, "content");
      if (!isAbsentOrString(content)) {
        return notOk("Upstream choice " + numberText(index) + " has unsupported message content.");
      }
      const json::value* reasoningContent = rt::get(message, "reasoning_content");
      if (!isAbsentOrString(reasoningContent)) {
        return notOk("Upstream choice " + numberText(index) + " has invalid reasoning content.");
      }
      // A malformed non-string refusal invalidates the message; an absent or null
      // one stays absent.
      NormalizationResult<std::optional<std::string>> refusal = normalizeRefusal(rt::get(message, "refusal"), "Upstream choice " + numberText(index));
      if (failed(refusal)) return failureOf(refusal);
      NormalizationResult<std::optional<json::array>> toolCallsResult = normalizeToolCalls(rt::get(message, "tool_calls"), "Upstream choice " + numberText(index), normalizeToolCall);
      if (failed(toolCallsResult)) return failureOf(toolCallsResult);
      std::optional<json::array>& toolCalls = valueOf(toolCallsResult);
      std::optional<std::string> reasoning =
        reasoningContent && reasoningContent->is_string() ? std::optional<std::string>(std::string(reasoningContent->get_string())) : std::nullopt;
      // A refusal-only message is an answer, not an empty one: the continuation
      // eligibility guard reads the same normalized value.
      if (choiceHasNoPayload(content, reasoningContent, valueOf(refusal), toolCalls)) {
        return notOk("Upstream choice " + numberText(index) + " has neither content nor a tool call.");
      }
      return ok(ChoiceMessagePart{content, std::move(reasoning), std::move(valueOf(refusal)), std::move(toolCalls)});
    }

    NormalizationResult<json::object> normalizeChoice(const json::value& value, std::size_t index) {
      if (!utils::isRecord(value) || value.is_array()) {
        return notOk("Upstream choice " + numberText(index) + " is not an object.");
      }
      const json::object& choice = value.get_object();
      const std::optional<double> choiceIndex = nonNegativeInteger(rt::get(choice, "index"));
      if (!choiceIndex) return notOk("Upstream choice " + numberText(index) + " has an invalid index.");
      const json::value* message = rt::get(choice, "message");
      if (!utils::isRecord(message) || message->is_array()) {
        return notOk("Upstream choice " + numberText(index) + " is missing an assistant message.");
      }
      NormalizationResult<ChoiceMessagePart> normalizedMessagePart = normalizeChoiceMessage(message->get_object(), index);
      if (failed(normalizedMessagePart)) return failureOf(normalizedMessagePart);
      ChoiceMessagePart& part = valueOf(normalizedMessagePart);

      const json::value* finishReason = rt::get(choice, "finish_reason");
      if (!isAbsentOrString(finishReason)) {
        return notOk("Upstream choice " + numberText(index) + " has an invalid finish reason.");
      }
      json::object normalizedMessage;
      normalizedMessage["role"] = "assistant";
      normalizedMessage["content"] = choiceContent(part.content, part.toolCalls);
      // DeepSeek thinking mode returns the chain of thought beside `content`.
      // Relay it 1:1 rather than dropping or logging it.
      if (part.reasoning) normalizedMessage["reasoning_content"] = *part.reasoning;
      // A refusal is answer-bearing output on this route's own Chat contract, so it
      // travels beside `content` exactly as the upstream sent it. Preserving it is
      // also what keeps the continuation decision from reading an explicit refusal
      // as an ordinary progress stop.
      if (part.refusal) normalizedMessage["refusal"] = *part.refusal;
      const bool toolCallsPresent = hasToolCalls(part.toolCalls);
      if (toolCallsPresent) normalizedMessage["tool_calls"] = std::move(*part.toolCalls);
      json::object normalized;
      normalized["index"] = rt::makeNumber(*choiceIndex);
      normalized["message"] = std::move(normalizedMessage);
      normalized["finish_reason"] = !rt::isNullish(finishReason) ? *finishReason : json::value(toolCallsPresent ? "tool_calls" : "stop");
      return ok(std::move(normalized));
    }

    /** Normalizes the `delta` object of one streaming chunk choice. */
    NormalizationResult<json::object> normalizeChunkDelta(const json::value& value, const std::string& label) {
      if (!utils::isRecord(value) || value.is_array()) return notOk(label + " has an invalid delta.");
      const json::object& delta = value.get_object();
      json::object normalizedDelta;
      NormalizationResult<std::optional<std::string>> role = optionalField<std::string>(rt::get(delta, "role"), stringInput, label + " has an invalid delta role.");
      if (failed(role)) return failureOf(role);
      if (valueOf(role)) normalizedDelta["role"] = *valueOf(role);
      for (const std::string_view field : {std::string_view("content"), std::string_view("reasoning_content"), std::string_view("refusal")}) {
        NormalizationResult<std::optional<std::string>> text = optionalField<std::string>(rt::get(delta, field), stringInput, label + " has invalid " + std::string(field) + ".");
        if (failed(text)) return failureOf(text);
        if (valueOf(text)) normalizedDelta[field] = *valueOf(text);
      }
      NormalizationResult<std::optional<std::string>> refusal = normalizeRefusal(rt::get(delta, "refusal"), label);
      if (failed(refusal)) return failureOf(refusal);
      if (valueOf(refusal)) normalizedDelta["refusal"] = *valueOf(refusal);
      NormalizationResult<std::optional<json::array>> toolCalls = normalizeToolCalls(rt::get(delta, "tool_calls"), label, normalizeToolCallDelta);
      if (failed(toolCalls)) return failureOf(toolCalls);
      if (hasToolCalls(valueOf(toolCalls))) normalizedDelta["tool_calls"] = std::move(*valueOf(toolCalls));
      return ok(std::move(normalizedDelta));
    }

    NormalizationResult<json::object> normalizeChoiceDelta(const json::value& value, std::size_t index) {
      const std::string label = "Upstream chunk choice " + numberText(index);
      if (!utils::isRecord(value) || value.is_array()) return notOk(label + " is not an object.");
      const json::object& choice = value.get_object();
      const std::optional<double> choiceIndex = nonNegativeInteger(rt::get(choice, "index"));
      if (!choiceIndex) return notOk(label + " has an invalid index.");
      const json::value* finishReason = rt::get(choice, "finish_reason");
      if (!isAbsentOrString(finishReason)) return notOk(label + " has an invalid finish reason.");
      json::object normalizedChoice;
      normalizedChoice["index"] = rt::makeNumber(*choiceIndex);
      NormalizationResult<std::optional<json::value>> delta = optionalField<json::value>(
        rt::get(choice, "delta"), [](const json::value& input) { return std::optional<json::value>(input); }, label + " has an invalid delta.");
      if (failed(delta)) return failureOf(delta);
      if (valueOf(delta)) {
        NormalizationResult<json::object> normalizedDelta = normalizeChunkDelta(*valueOf(delta), label);
        if (failed(normalizedDelta)) return failureOf(normalizedDelta);
        normalizedChoice["delta"] = std::move(valueOf(normalizedDelta));
      }
      if (finishReason) normalizedChoice["finish_reason"] = *finishReason;
      return ok(std::move(normalizedChoice));
    }
  }

  bool deepSeekThinkingModeActive(const json::value* reasoningEffort, const json::value* thinking) {
    const std::string effort = reasoningEffort && reasoningEffort->is_string() ? rt::toLowerCase(rt::trim(std::string(reasoningEffort->get_string()))) : std::string();
    if (!effort.empty()) return effort != "none";
    if (utils::isRecord(thinking) && !thinking->is_array()) {
      const std::optional<std::string> type = utils::getString(rt::get(*thinking, "type"));
      const std::optional<std::string> normalizedType = type ? std::optional<std::string>(rt::toLowerCase(rt::trim(*type))) : std::nullopt;
      if (normalizedType == "disabled") return false;
      if (normalizedType == "enabled") return true;
    }
    return true;
  }
  bool deepSeekThinkingModeActive(const json::value& reasoningEffort, const json::value& thinking) { return deepSeekThinkingModeActive(&reasoningEffort, &thinking); }

  std::optional<std::string> deepSeekThinkingModeForbiddenToolChoice(const json::value* toolChoice) {
    if (toolChoice && toolChoice->is_string() && toolChoice->get_string() == "required") return "required";
    if (utils::isRecord(toolChoice) && !toolChoice->is_array()) {
      std::optional<std::string> name = utils::getString(rt::get(*toolChoice, "name"));
      if (!name) {
        const json::value* function = rt::get(*toolChoice, "function");
        name = utils::isRecord(function) ? utils::getString(rt::get(*function, "name")) : std::nullopt;
      }
      if (rt::truthy(name)) return "function:" + *name;
    }
    return std::nullopt;
  }
  std::optional<std::string> deepSeekThinkingModeForbiddenToolChoice(const json::value& toolChoice) { return deepSeekThinkingModeForbiddenToolChoice(&toolChoice); }

  std::optional<std::string> deepSeekThinkingToolChoiceConflict(const json::value* reasoningEffort, const json::value* thinking, const json::value* toolChoice) {
    std::optional<std::string> forbidden = deepSeekThinkingModeForbiddenToolChoice(toolChoice);
    if (!rt::truthy(forbidden)) return std::nullopt;
    return deepSeekThinkingModeActive(reasoningEffort, thinking) ? forbidden : std::nullopt;
  }
  std::optional<std::string> deepSeekThinkingToolChoiceConflict(const json::value& reasoningEffort, const json::value& thinking, const json::value& toolChoice) {
    return deepSeekThinkingToolChoiceConflict(&reasoningEffort, &thinking, &toolChoice);
  }

  std::string deepSeekToolChoiceThinkingConflictMessage(const std::string& toolChoice, const std::string& effortField) {
    return "tool_choice '" + toolChoice + "' is not supported while " + effortField + " keeps DeepSeek thinking mode active; set " + effortField +
           " to 'none' or use tool_choice 'auto'";
  }

  DeepSeekFinishDisposition deepSeekFinishDisposition(const json::value* reason) {
    const std::string normalized = reason && reason->is_string() ? rt::trim(std::string(reason->get_string())) : std::string();
    // An absent reason means the frame carried none, not that generation was
    // cut off. The provider's own client defaults `pendingFinish ?? "stop"` at
    // its end sentinel, and this mapping only runs after that sentinel.
    if (normalized.empty() || normalized == "stop" || normalized == "tool_calls") return DeepSeekFinishDispositionCompleted{};
    if (normalized == "length") return DeepSeekFinishDispositionIncomplete{.reason = "max_output_tokens"};
    if (normalized == "content_filter") return DeepSeekFinishDispositionIncomplete{.reason = "content_filter"};
    if (normalized == "insufficient_system_resource") return DeepSeekFinishDispositionFailed{.code = "insufficient_system_resource"};
    if (normalized == "aborted") return DeepSeekFinishDispositionFailed{.code = "aborted"};
    return DeepSeekFinishDispositionUnknown{.value = reason && reason->is_string() ? std::string(reason->get_string()) : std::string()};
  }
  DeepSeekFinishDisposition deepSeekFinishDisposition(const json::value& reason) { return deepSeekFinishDisposition(&reason); }

  std::optional<std::string> normalizeDeepSeekProviderRequestId(const json::value* value) {
    const std::optional<std::string> requestId = exactNonEmptyString(value);
    if (!requestId || rt::length(*requestId) > MAX_DEEPSEEK_PROVIDER_REQUEST_ID_LENGTH) return std::nullopt;
    static const rt::Regex HEADER_SAFE("^[\\x21-\\x7e]+$");
    return HEADER_SAFE.test(*requestId) ? requestId : std::nullopt;
  }
  std::optional<std::string> normalizeDeepSeekProviderRequestId(const json::value& value) { return normalizeDeepSeekProviderRequestId(&value); }

  std::optional<std::string> getDeepSeekProviderRequestId(const rt::Response& response) {
    std::optional<std::string> header = response.headers().get("X-Request-Id");
    if (!header) header = response.headers().get("X-Ds-Request-Id");
    if (!header) header = response.headers().get("X-Deepseek-Request-Id");
    const json::value value = header ? json::value(*header) : json::value(nullptr);
    return normalizeDeepSeekProviderRequestId(&value);
  }

  bool isTimeoutError(const rt::Reason& error) { return rt::isError(error) && rt::errorName(error) == "TimeoutError"; }

  std::optional<std::string> readDeepSeekApiKey() { return nonEmptyString(getEnv(DEEPSEEK_API_KEY_ENV)); }

  void setDeepSeekFetchTimeoutMsForTest(std::optional<double> timeoutMs) {
    deepSeekFetchTimeoutMs = timeoutMs.value_or(inference_deadline::STREAM_FIRST_EVENT_DEADLINE_MS);
  }

  std::optional<std::string> deepSeekUpstreamModelFor(const std::string& model) {
    const std::string normalized = rt::toLowerCase(rt::trim(model));
    const std::unordered_map<std::string, std::string>& models = DEEPSEEK_UPSTREAM_MODEL_BY_ID();
    const auto found = models.find(normalized);
    return found != models.end() ? std::optional<std::string>(found->second) : std::nullopt;
  }

  std::optional<json::value> projectDeepSeekReasoningEffort(const std::string& effort) {
    std::optional<std::optional<json::value>> mapped = DEEPSEEK_WIRE_EFFORT_MAP(rt::toLowerCase(rt::trim(effort)));
    if (mapped) return std::move(*mapped);
    return json::value(effort);
  }

  std::optional<double> deepSeekDefaultOutputAllowance(const std::string& reasoningEffort) {
    const std::optional<json::value> wireEffort = projectDeepSeekReasoningEffort(reasoningEffort);
    return wireEffort && wireEffort->is_string() && wireEffort->get_string() == "none" ? std::optional<double>(8'192) : std::nullopt;
  }

  json::object projectDeepSeekRequest(const json::object& body, const std::string& requestedModel) {
    const std::optional<std::string> upstreamModel = deepSeekUpstreamModelFor(requestedModel);
    if (!upstreamModel) throw DeepSeekError("The requested model is not configured.", "deepseek_request_invalid", 400);
    json::object projected = rt::objectSpread(body);
    rt::objectDefine(projected, "model", json::value(*upstreamModel));
    if (const json::value* maxCompletionTokens = rt::get(projected, "max_completion_tokens"); maxCompletionTokens && !maxCompletionTokens->is_null()) {
      json::value cap = *maxCompletionTokens;
      rt::objectSet(projected, "max_tokens", std::move(cap));
    }
    rt::objectDelete(projected, "max_completion_tokens");
    if (const json::value* effort = rt::get(projected, "reasoning_effort"); effort && effort->is_string()) {
      std::optional<json::value> wireEffort = projectDeepSeekReasoningEffort(std::string(effort->get_string()));
      // The inherited `Object` function (std::nullopt) is a member JSON.stringify skips.
      if (wireEffort) rt::objectSet(projected, "reasoning_effort", std::move(*wireEffort));
      else rt::objectDelete(projected, "reasoning_effort");
    }
    if (const json::value* messages = rt::get(projected, "messages"); messages && messages->is_array()) {
      json::value mapped = projectDeepSeekMessages(*messages);
      rt::objectSet(projected, "messages", std::move(mapped));
    }
    return projected;
  }

  rt::Task<rt::Response> fetchDeepSeekChatCompletions(json::object body, std::string requestedModel) {
    return fetchDeepSeekChatCompletions(std::move(body), std::move(requestedModel), DeepSeekChatCompletionsOptions{});
  }

  rt::Task<rt::Response> fetchDeepSeekChatCompletions(json::object body, std::string requestedModel, DeepSeekChatCompletionsOptions options) {
    std::string encodedBody;
    json::value projectedBody;
    try {
      projectedBody = json::value(projectDeepSeekRequest(body, requestedModel));
      encodedBody = rt::jsonStringify(projectedBody);
    } catch (const DeepSeekError&) {
      throw;
    } catch (...) {
      throw DeepSeekError("Chat Completions requests must use a JSON-serializable body.", "deepseek_request_invalid", 400);
    }
    // The TS also refuses a `JSON.stringify` result that is not a string (a body whose
    // `toJSON` yields undefined); a json::object has no toJSON, so its text always exists.

    if (options.onProjectedBody) options.onProjectedBody(projectedBody.as_object());

    const std::string apiKey = requireDeepSeekApiKey(options.apiKey);
    const rt::Headers headers{
      {"Accept", "application/json"},
      {"Authorization", "Bearer " + apiKey},
      {"Content-Type", "application/json"},
    };
    const rt::AbortController deadline;
    const rt::Timeout timer = rt::setTimeout(
      [deadline] { deadline.abort(rt::makeReason(rt::DOMException("DeepSeek response headers timed out.", "TimeoutError"))); }, deepSeekFetchTimeoutMs);
    const auto clearTimer = rt::scopeExit([timer] { rt::clearTimeout(timer); });
    const rt::AbortSignal signal = options.signal ? rt::AbortSignal::any({*options.signal, deadline.signal()}) : deadline.signal();

    std::optional<sentinel::upstream_capture::SentinelUpstreamAttemptHandle> upstreamAttempt;
    std::optional<rt::Response> delivered;
    rt::Reason failure;
    try {
      // Keep this as the final awaited operation before provider transport so
      // API-key admission cannot be committed after a cancelled request.
      std::optional<api_key_policy::ApiKeyProviderDispatch> dispatch;
      if (options.beforeDispatch) {
        const rt::Promise<std::optional<api_key_policy::ApiKeyProviderDispatch>> admitted(options.beforeDispatch());
        dispatch = co_await admitted.wait();
      }
      if (signal.aborted()) {
        // `await dispatch?.cancelBeforeTransport()`: `await undefined` still takes a turn.
        if (dispatch) {
          const rt::Promise<void> cancelled(dispatch->cancelBeforeTransport());
          co_await cancelled.wait();
        } else {
          co_await rt::microtask();
        }
        rt::rethrow(abortedTransportError(signal, options.signal, deadline.signal()));
      }
      if (dispatch) dispatch->markTransportStarted();
      if (options.onDispatch) options.onDispatch();
      if (options.sentinelUpstreamRecorder) upstreamAttempt = options.sentinelUpstreamRecorder->startAttempt("deepseek");
      rt::RequestInit init;
      init.method = "POST";
      init.headers = headers;
      init.body = rt::BodyInit(std::move(encodedBody));
      init.redirect = "manual";
      init.signal = signal;
      const DeepSeekFetch fetcher = options.fetcher ? options.fetcher : rt::globalFetch();
      const rt::Promise<rt::Response> fetching(fetcher(std::string(DEEPSEEK_CHAT_COMPLETIONS_URL), std::move(init)));
      rt::Response response = co_await fetching.wait();
      if (options.onHeaders) options.onHeaders();
      delivered = upstreamAttempt ? upstreamAttempt->wrap(response) : response;
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      if (upstreamAttempt) upstreamAttempt->recordFetchError();
      if (isApiKeyQuotaDispatchError(failure) || isDeepSeekError(failure)) rt::rethrow(failure);
      rt::rethrow(transportFailureError(failure, options.signal, deadline.signal()));
    }
    co_return std::move(*delivered);
  }

  std::optional<double> deepSeekCachedPromptTokens(const json::object& value, double promptTokens) {
    const json::value* rawDetails = rt::get(value, "prompt_tokens_details");
    const json::value* details = utils::isRecord(rawDetails) && !rawDetails->is_array() ? rawDetails : nullptr;
    std::optional<double> cached = nonNegativeInteger(rt::get(value, "prompt_cache_hit_tokens"));
    if (!cached) cached = nonNegativeInteger(rt::get(details, "cached_tokens"));
    if (!cached || *cached > promptTokens) return std::nullopt;
    return cached;
  }

  std::optional<double> deepSeekReasoningTokens(const json::object& value, double completionTokens) {
    const json::value* rawDetails = rt::get(value, "completion_tokens_details");
    const json::value* details = utils::isRecord(rawDetails) && !rawDetails->is_array() ? rawDetails : nullptr;
    const std::optional<double> reasoning = nonNegativeInteger(rt::get(details, "reasoning_tokens"));
    if (!reasoning || *reasoning > completionTokens) return std::nullopt;
    return reasoning;
  }

  NormalizationResult<json::object> normalizeDeepSeekChatCompletion(const json::value* value, const std::string& requestedModel) {
    if (!utils::isRecord(value) || value->is_array()) {
      return notOk("Upstream did not return a Chat Completions object.");
    }
    const json::object& completion = value->get_object();
    const std::optional<std::string> id = nonEmptyString(rt::get(completion, "id"));
    if (!id) return notOk("Upstream Chat Completion is missing an id.");
    const std::optional<double> created = nonNegativeInteger(rt::get(completion, "created"));
    if (!created) return notOk("Upstream Chat Completion has an invalid creation time.");
    if (const json::value* object = rt::get(completion, "object"); object && !(object->is_string() && object->get_string() == "chat.completion")) {
      return notOk("Upstream did not return a Chat Completion.");
    }
    if (!acceptUpstreamModel(rt::get(completion, "model"))) {
      return notOk("Upstream returned a different model than requested.");
    }
    const json::value* rawChoices = rt::get(completion, "choices");
    if (!(rawChoices && rawChoices->is_array()) || rawChoices->get_array().empty()) {
      return notOk("Upstream Chat Completion has no choices.");
    }
    json::array choices;
    std::size_t index = 0;
    for (const json::value& choice : rawChoices->get_array()) {
      NormalizationResult<json::object> normalized = normalizeChoice(choice, index++);
      if (failed(normalized)) return failureOf(normalized);
      choices.push_back(std::move(valueOf(normalized)));
    }
    NormalizationResult<std::optional<json::object>> usage = normalizeUsage(rt::get(completion, "usage"));
    if (failed(usage)) return failureOf(usage);
    json::object normalized;
    normalized["id"] = *id;
    normalized["object"] = "chat.completion";
    normalized["created"] = rt::makeNumber(*created);
    // The client asked for one of the interchangeable official ids; echo that
    // id rather than the canonical upstream name.
    normalized["model"] = requestedModel;
    normalized["choices"] = std::move(choices);
    if (valueOf(usage)) normalized["usage"] = std::move(*valueOf(usage));
    return ok(std::move(normalized));
  }
  NormalizationResult<json::object> normalizeDeepSeekChatCompletion(const json::value& value, const std::string& requestedModel) {
    return normalizeDeepSeekChatCompletion(&value, requestedModel);
  }

  NormalizationResult<json::object> normalizeDeepSeekChatCompletionChunk(const json::value* value, const std::string& requestedModel) {
    if (!utils::isRecord(value) || value->is_array()) {
      return notOk("Upstream did not return a Chat Completions chunk.");
    }
    const json::object& chunk = value->get_object();
    const std::optional<std::string> id = nonEmptyString(rt::get(chunk, "id"));
    if (!id) return notOk("Upstream Chat Completion chunk is missing an id.");
    const std::optional<double> created = nonNegativeInteger(rt::get(chunk, "created"));
    if (!created) return notOk("Upstream Chat Completion chunk has an invalid creation time.");
    if (const json::value* object = rt::get(chunk, "object"); object && !(object->is_string() && object->get_string() == "chat.completion.chunk")) {
      return notOk("Upstream did not return a Chat Completion chunk.");
    }
    if (!acceptUpstreamModel(rt::get(chunk, "model"))) {
      return notOk("Upstream returned a different model than requested.");
    }
    const json::value* rawChoices = rt::get(chunk, "choices");
    if (!(rawChoices && rawChoices->is_array())) {
      return notOk("Upstream Chat Completion chunk has no choices array.");
    }
    json::array choices;
    std::size_t index = 0;
    for (const json::value& choice : rawChoices->get_array()) {
      NormalizationResult<json::object> normalized = normalizeChoiceDelta(choice, index++);
      if (failed(normalized)) return failureOf(normalized);
      choices.push_back(std::move(valueOf(normalized)));
    }
    NormalizationResult<std::optional<json::object>> usage = normalizeUsage(rt::get(chunk, "usage"));
    if (failed(usage)) return failureOf(usage);
    json::object normalized;
    normalized["id"] = *id;
    normalized["object"] = "chat.completion.chunk";
    normalized["created"] = rt::makeNumber(*created);
    normalized["model"] = requestedModel;
    normalized["choices"] = std::move(choices);
    if (valueOf(usage)) normalized["usage"] = std::move(*valueOf(usage));
    return ok(std::move(normalized));
  }
  NormalizationResult<json::object> normalizeDeepSeekChatCompletionChunk(const json::value& value, const std::string& requestedModel) {
    return normalizeDeepSeekChatCompletionChunk(&value, requestedModel);
  }
}
