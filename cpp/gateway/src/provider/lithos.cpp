// Port of src/provider/lithos.ts.
#include "uos/provider/lithos.hpp"

#include "uos/inference_deadline.hpp"
#include "uos/provider/lithos_rate_limits.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/utils.hpp"

#include <cmath>
#include <cstdint>
#include <exception>
#include <memory>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uos::provider::lithos {
  namespace {
    double lithosFetchTimeoutMs = inference_deadline::STREAM_FIRST_EVENT_DEADLINE_MS;

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

    /**
     * Provider request IDs are opaque support-correlation values. Keep only a
     * bounded, header-safe value so an upstream cannot make the gateway reflect
     * arbitrary metadata. This mirrors the DeepSeek normalizer even though the
     * LithosAI wire currently has nothing to normalize.
     */
    constexpr std::int64_t MAX_LITHOS_PROVIDER_REQUEST_ID_LENGTH = 256;

    std::optional<double> nonNegativeInteger(const json::value* value) {
      const std::optional<double> number = value ? rt::getNumber(*value) : std::nullopt;
      if (!number || !std::isfinite(*number)) return std::nullopt;
      const double normalized = rt::Math::trunc(*number);
      return normalized >= 0 ? std::optional<double>(normalized) : std::nullopt;
    }

    /** Upstream `null`, `undefined` and strings are the only accepted nullable text values. */
    bool isAbsentOrString(const json::value* value) { return !value || value->is_null() || value->is_string(); }

    /** Retains an Optional OpenAI wire field only when the upstream frame actually carries it. */
    // `normalize` returns std::nullopt for the TS `null`; the result's std::nullopt is undefined.
    template <class T>
    NormalizationResult<std::optional<T>> optionalField(const json::value* value, std::optional<T> (*normalize)(const json::value& input), const std::string& message) {
      if (!value || value->is_null()) return ok(std::optional<T>());
      std::optional<T> normalized = normalize(*value);
      if (!normalized) return notOk(message);
      return ok(std::move(normalized));
    }

    std::optional<std::string> getEnv(std::string_view key) {
      try {
        return rt::env::get(key);
      } catch (...) {
        return std::nullopt;
      }
    }

    /**
     * Lowercase lookup index over the same identity map, so the table a later
     * dispatch seam reads and the lookup it calls can never drift apart. The
     * provider's own ids are mixed-case, so a client that spells one differently
     * must still resolve to the canonical id that goes upstream. A Map is used
     * rather than an object so an inherited property name (`constructor`) can never
     * resolve as a model.
     */
    // Derived from LITHOS_UPSTREAM_MODEL_BY_ID on first use; lookups only.
    const std::unordered_map<std::string, std::string>& LITHOS_UPSTREAM_MODEL_BY_LOWERCASE_ID() {
      static const std::unordered_map<std::string, std::string> index = [] {
        std::unordered_map<std::string, std::string> out;
        for (const auto& [id, canonical] : LITHOS_UPSTREAM_MODEL_BY_ID) out.insert_or_assign(rt::toLowerCase(id), canonical);
        return out;
      }();
      return index;
    }

    rt::Reason timeoutError() { return rt::makeReason(LithosError("Upstream request exceeded the gateway deadline.", "gateway_timeout", 504)); }

    rt::Reason abortError(const rt::AbortSignal& signal) {
      return rt::isError(signal.reason()) ? signal.reason() : rt::makeReason(rt::DOMException("The request was aborted.", "AbortError"));
    }

    bool isTimeoutError(const rt::Reason& error) { return rt::isError(error) && rt::errorName(error) == "TimeoutError"; }

    // `error instanceof LithosError` / `error instanceof ApiKeyQuotaDispatchError`.
    bool isLithosError(const rt::Reason& error) {
      if (!error) return false;
      try {
        std::rethrow_exception(error);
      } catch (const LithosError&) {
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
      return rt::makeReason(LithosError("Upstream request could not be completed.", "lithos_upstream_unreachable", 502));
    }

    /**
     * The upstream echoes the model it served. "Normalizing" an echo means trimming
     * it and stripping a leading `/models/` route prefix, because this vendor's
     * route shape is `/models/{author}/{slug}`. Three things are accepted, and
     * anything else describes a different model than the one the client asked for:
     *
     * 1. No echo at all (`undefined`/`null`, or nothing but whitespace). The
     *    provider said nothing about the model, which is not a mismatch.
     * 2. A normalized echo that equals the canonical id for the requested model,
     *    case-insensitively. This is the ordinary case: seven of the eight ids echo
     *    themselves verbatim.
     * 3. A normalized echo that equals the requested id's trailing path segment
     *    (slug), case-insensitively — or a `-`-delimited leading part of that slug.
     *    This is NOT a special-case table: the rule is derived from the requested id
     *    and holds for any id. Live probes on 2026-09-23 answered EVERY completion
     *    for `deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat` with
     *    `model: "/models/DeepSeek-V4.1-Flash"` (five sequential probes, identical;
     *    a longer prompt changed nothing), which is the engine folding that tier
     *    onto the base `DeepSeek-V4.1-Flash` slug while
     *    `GET /v1/models/deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat` still reports
     *    the full id. That echo is a stable property of the wire, so an
     *    exact-equality check would reject every real response for one of the eight
     *    advertised models. Do not tighten this back into equality; re-probe the
     *    `-ultra-chat` tier before narrowing it.
     */
    /** True when an echo names this canonical model under the derived rules 2 and 3. */
    bool echoNamesModel(const std::string& echo, const std::string& canonicalModel) {
      const std::string requested = rt::toLowerCase(rt::trim(canonicalModel));
      if (echo == requested) return true;
      const std::string slug = rt::slice(requested, rt::lastIndexOf(requested, "/") + 1);
      return echo == slug || rt::startsWith(slug, echo + "-");
    }

    bool acceptUpstreamModel(const json::value* value, const std::string& requestedModel, const std::optional<std::string>& servedModel) {
      // Rule 1: a missing or text-free echo carries no model claim to compare.
      if (!value || value->is_null()) return true;
      static const rt::Regex MODELS_ROUTE_PREFIX("^\\/models\\/");
      const std::string echo =
        value->is_string() ? rt::toLowerCase(rt::trim(rt::replace(rt::trim(std::string(value->get_string())), MODELS_ROUTE_PREFIX, ""))) : std::string();
      if (echo.empty()) return true;
      // The id the client asked for, in this provider's canonical spelling.
      const std::string requested = rt::trim(lithosUpstreamModelFor(requestedModel).value_or(requestedModel));
      if (echoNamesModel(echo, requested)) return true;  // rules 2 and 3
      if (!servedModel) return false;
      // A request the requested tier refused may be served further down its
      // configured failover ladder (ultra -> fast -> normal), and the vendor then
      // names that tier. Any ladder tier is accepted, and only when this attempt
      // actually addressed it: nothing else may report a model other than the one
      // requested.
      const std::string served = rt::trim(lithosUpstreamModelFor(*servedModel).value_or(*servedModel));
      if (rt::toLowerCase(served) == rt::toLowerCase(requested)) return false;
      return lithos_rate_limits::lithosIsLadderTarget(requested, served) && echoNamesModel(echo, served);
    }

    NormalizationResult<json::object> normalizeLithosToolCall(const json::value& value, std::size_t index) {
      if (!utils::isRecord(value) || value.is_array()) return notOk("Upstream tool call " + numberText(index) + " is not an object.");
      const json::object& call = value.get_object();
      const std::optional<std::string> id = exactNonEmptyString(rt::get(call, "id"));
      if (!id) return notOk("Upstream tool call " + numberText(index) + " is missing an id.");
      const json::value* type = rt::get(call, "type");
      if (!(type && type->is_string() && type->get_string() == "function")) return notOk("Upstream tool call " + numberText(index) + " has an unsupported type.");
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
      // Tool-call ids arrive as `name:index` on this provider. They are opaque
      // OpenAI wire values, so they are relayed unchanged: nothing here parses or
      // repairs the provider-specific id shape, and OpenAI's Chat contract
      // represents function arguments as an opaque string that is not reparsed.
      json::object normalizedFunction;
      normalizedFunction["name"] = *name;
      normalizedFunction["arguments"] = *arguments;
      json::object normalized;
      normalized["id"] = *id;
      normalized["type"] = "function";
      normalized["function"] = std::move(normalizedFunction);
      return ok(std::move(normalized));
    }

    /** Normalizes the `function` object of one streaming tool-call delta. */
    NormalizationResult<json::object> normalizeLithosToolCallDeltaFunction(const json::value& value, const std::string& label) {
      if (!utils::isRecord(value) || value.is_array()) return notOk(label + " has an invalid function.");
      const json::object& function = value.get_object();
      json::object fn;
      NormalizationResult<std::optional<std::string>> name = optionalField<std::string>(
        rt::get(function, "name"), [](const json::value& input) { return exactNonEmptyString(&input); }, label + " has an invalid function name.");
      if (failed(name)) return failureOf(name);
      if (valueOf(name)) fn["name"] = *valueOf(name);
      NormalizationResult<std::optional<std::string>> args = optionalField<std::string>(
        rt::get(function, "arguments"),
        [](const json::value& input) { return input.is_string() ? std::optional<std::string>(std::string(input.get_string())) : std::nullopt; },
        label + " has non-string arguments.");
      if (failed(args)) return failureOf(args);
      if (valueOf(args)) fn["arguments"] = *valueOf(args);
      return ok(std::move(fn));
    }

    /**
     * Normalizes a STREAMING tool-call delta. Unlike a buffered tool call, a delta
     * legitimately omits `id`, `type` and even `function.name` on continuation
     * frames, so only the fields this frame actually carries are retained.
     */
    NormalizationResult<json::object> normalizeLithosToolCallDelta(const json::value& value, std::size_t index) {
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
        NormalizationResult<json::object> fn = normalizeLithosToolCallDeltaFunction(*function, label);
        if (failed(fn)) return failureOf(fn);
        if (!valueOf(fn).empty()) delta["function"] = std::move(valueOf(fn));
      }
      if (delta.empty()) return notOk(label + " carries no fields.");
      return ok(std::move(delta));
    }

    /**
     * Normalizes the tool calls of one message or delta. `undefined` means the
     * frame carried no tool calls at all; callers pass `?? undefined` because this
     * provider reports "none" as an explicit `null` rather than an absent field.
     */
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

    // `value ?? undefined`: an explicit null reads as absent.
    const json::value* nullAsUndefined(const json::value* value) { return value && value->is_null() ? nullptr : value; }

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

    /**
     * One nested usage counter, validated against the total it is defined as a
     * subset of. An absent or impossible value stays absent, so no reader
     * downstream can mistake a missing measurement for a measured zero.
     */
    std::optional<double> usageDetailCounter(const json::value* container, std::string_view key, double ceiling) {
      if (!utils::isRecord(container) || container->is_array()) return std::nullopt;
      const std::optional<double> counter = nonNegativeInteger(rt::get(container->get_object(), key));
      if (!counter || *counter > ceiling) return std::nullopt;
      return counter;
    }

    json::object singleCounter(std::string_view key, double counter) {
      json::object out;
      out[key] = rt::makeNumber(counter);
      return out;
    }

    /**
     * Reduces a LithosAI usage object to the OpenAI Chat Completions usage shape
     * the Assistant consumes.
     *
     * The provider reports `prompt_tokens_details` as an explicit `null` when it
     * measured nothing, and an explicit `null` is NOT a measured zero: a null (or
     * absent) details object leaves the matching detail object out of the
     * projection entirely. `completion_tokens_details.reasoning_tokens` is a real
     * integer on this wire and is relayed under its official OpenAI name.
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
      const std::optional<double> cachedTokens = lithosCachedPromptTokens(usage, *promptTokens);
      const std::optional<double> reasoningTokens = lithosReasoningTokens(usage, *completionTokens);
      json::object normalized;
      normalized["prompt_tokens"] = rt::makeNumber(*promptTokens);
      normalized["completion_tokens"] = rt::makeNumber(*completionTokens);
      normalized["total_tokens"] = rt::makeNumber(*totalTokens);
      if (cachedTokens) normalized["prompt_tokens_details"] = singleCounter("cached_tokens", *cachedTokens);
      if (reasoningTokens) normalized["completion_tokens_details"] = singleCounter("reasoning_tokens", *reasoningTokens);
      return ok(std::optional<json::object>(std::move(normalized)));
    }

    bool hasToolCalls(const std::optional<json::array>& toolCalls) { return toolCalls && !toolCalls->empty(); }

    // `content` nullptr is undefined; `reasoning` and `refusal` std::nullopt are null.
    bool choiceHasNoPayload(const json::value* content, const std::optional<std::string>& reasoning, const std::optional<std::string>& refusal,
                            const std::optional<json::array>& toolCalls) {
      return !content && !reasoning && !refusal && !hasToolCalls(toolCalls);
    }

    /** Mirrors the upstream `content` field: absent content becomes explicit JSON `null` only when a tool call exists. */
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
      // An explicit upstream `null` means "no reasoning produced", so it stays
      // absent from the projection rather than becoming an empty string.
      std::optional<std::string> reasoning =
        reasoningContent && reasoningContent->is_string() ? std::optional<std::string>(std::string(reasoningContent->get_string())) : std::nullopt;
      NormalizationResult<std::optional<std::string>> refusal = normalizeRefusal(rt::get(message, "refusal"), "Upstream choice " + numberText(index));
      if (failed(refusal)) return failureOf(refusal);
      // `tool_calls` is present and `null` on an ordinary response from this
      // provider, so a null is absence, not malformed input.
      NormalizationResult<std::optional<json::array>> toolCallsResult =
        normalizeToolCalls(nullAsUndefined(rt::get(message, "tool_calls")), "Upstream choice " + numberText(index), normalizeLithosToolCall);
      if (failed(toolCallsResult)) return failureOf(toolCallsResult);
      std::optional<json::array>& toolCalls = valueOf(toolCallsResult);
      if (choiceHasNoPayload(content, reasoning, valueOf(refusal), toolCalls)) {
        return notOk("Upstream choice " + numberText(index) + " has neither content nor a tool call.");
      }
      return ok(ChoiceMessagePart{content, std::move(reasoning), std::move(valueOf(refusal)), std::move(toolCalls)});
    }

    NormalizationResult<json::object> normalizeChoice(const json::value& value, std::size_t index) {
      if (!utils::isRecord(value) || value.is_array()) return notOk("Upstream choice " + numberText(index) + " is not an object.");
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
      if (!isAbsentOrString(finishReason)) return notOk("Upstream choice " + numberText(index) + " has an invalid finish reason.");
      json::object normalizedMessage;
      normalizedMessage["role"] = "assistant";
      normalizedMessage["content"] = choiceContent(part.content, part.toolCalls);
      if (part.reasoning) normalizedMessage["reasoning_content"] = *part.reasoning;
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
      NormalizationResult<std::optional<std::string>> role = optionalField<std::string>(
        rt::get(delta, "role"),
        [](const json::value& input) { return input.is_string() ? std::optional<std::string>(std::string(input.get_string())) : std::nullopt; },
        label + " has an invalid delta role.");
      if (failed(role)) return failureOf(role);
      if (valueOf(role)) normalizedDelta["role"] = *valueOf(role);
      for (const std::string_view field : {std::string_view("content"), std::string_view("reasoning_content")}) {
        NormalizationResult<std::optional<std::string>> text = optionalField<std::string>(
          rt::get(delta, field),
          [](const json::value& input) { return input.is_string() ? std::optional<std::string>(std::string(input.get_string())) : std::nullopt; },
          label + " has invalid " + std::string(field) + ".");
        if (failed(text)) return failureOf(text);
        if (valueOf(text)) normalizedDelta[field] = *valueOf(text);
      }
      NormalizationResult<std::optional<std::string>> refusal = normalizeRefusal(rt::get(delta, "refusal"), label);
      if (failed(refusal)) return failureOf(refusal);
      if (valueOf(refusal)) normalizedDelta["refusal"] = *valueOf(refusal);
      NormalizationResult<std::optional<json::array>> toolCalls = normalizeToolCalls(nullAsUndefined(rt::get(delta, "tool_calls")), label, normalizeLithosToolCallDelta);
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

    /** One SSE frame is a single generated chunk; bound it so a peer cannot pin memory. */
    constexpr double MAX_LITHOS_SSE_FRAME_BYTES = 4 * 1024 * 1024;
    // `/\r\n\r\n|\n\n|\r\r/` and `/\r\n|\r|\n/`, compiled on first use.
    const rt::Regex& LITHOS_SSE_FRAME_BOUNDARY() {
      static const rt::Regex regex("\\r\\n\\r\\n|\\n\\n|\\r\\r");
      return regex;
    }
    const rt::Regex& LITHOS_SSE_LINE_BOUNDARY() {
      static const rt::Regex regex("\\r\\n|\\r|\\n");
      return regex;
    }

    LithosError invalidResponseError(const std::string& message) { return LithosError(message, "lithos_upstream_invalid_response", 502); }

    // `Readonly<{ kind: "chunk"; value: Record<string, unknown> }> | Readonly<{ kind: "done" }>`
    struct LithosStreamFrameChunk {
      [[maybe_unused]] static constexpr std::string_view kind = "chunk";
      json::object value;
    };
    struct LithosStreamFrameDone {
      [[maybe_unused]] static constexpr std::string_view kind = "done";
    };
    using LithosStreamFrame = std::variant<LithosStreamFrameChunk, LithosStreamFrameDone>;

    /**
     * Parses one complete SSE event block. `data:` payloads are the OpenAI Chat
     * Completions stream contract and are normalized before they leave this module.
     *
     * Comments (this provider's keep-alives) and any other SSE field (`event:`,
     * `id:`, `retry:`) carry no Chat Completions payload, so a block with no
     * `data:` line is skipped rather than relayed. A malformed payload is a
     * fail-closed upstream fault: it is never skipped, because silently dropping a
     * frame would truncate the answer the client is assembling.
     */
    std::optional<LithosStreamFrame> parseLithosSseEventBlock(std::string_view raw, const std::string& requestedModel, const std::optional<std::string>& servedModel) {
      static const rt::Regex LEADING_SPACE("^ ");
      std::vector<std::string> data;
      for (const std::string& line : rt::split(raw, LITHOS_SSE_LINE_BOUNDARY())) {
        if (line.empty()) continue;
        if (line == "data") data.push_back("");
        else if (line.starts_with("data:")) data.push_back(rt::replace(rt::slice(line, 5), LEADING_SPACE, ""));
      }
      if (data.empty()) return std::nullopt;
      const std::string payload = rt::join(data, "\n");
      if (rt::trim(payload) == "[DONE]") return LithosStreamFrameDone{};
      json::value parsed;
      try {
        parsed = rt::jsonParse(payload);
      } catch (...) {
        throw invalidResponseError("Upstream emitted malformed Chat Completions SSE JSON.");
      }
      NormalizationResult<json::object> normalized = normalizeLithosChatCompletionChunk(&parsed, requestedModel, servedModel);
      if (failed(normalized)) {
        throw invalidResponseError("Upstream emitted an invalid Chat Completions chunk: " + failureOf(normalized).message);
      }
      return LithosStreamFrameChunk{std::move(valueOf(normalized))};
    }

    /**
     * Splits an incremental SSE byte stream into relayable frames, retaining the
     * unterminated tail between reads. Frames that carry no `data:` payload are
     * skipped, and the byte bound is enforced on the retained tail.
     */
    // The TS keeps `buffered.text` and re-slices it; here the retained tail is `text` from
    // `start` on (consumed frames are dropped on the next push) and its JS `.length` is kept
    // in `length`, which is what the bound compares.
    class LithosFrameReader {
    public:
      LithosFrameReader(std::string requestedModel_, std::optional<std::string> servedModel_)
        : requestedModel(std::move(requestedModel_)), servedModel(std::move(servedModel_)) {}

      // `push(incoming)`: nullptr is the end of the stream.
      void push(const rt::ByteChunk* incoming) {
        const std::string decoded = incoming ? decoder.decode(*incoming, {.stream = true}) : decoder.decode();
        if (start) {
          text.erase(0, start);
          start = 0;
        }
        // TextDecoder output is well formed, so plain appending is JS concatenation.
        text += decoded;
        length += rt::length(decoded);
        if (static_cast<double>(length) > MAX_LITHOS_SSE_FRAME_BYTES) {
          throw invalidResponseError("Upstream SSE frame exceeded the gateway bound.");
        }
      }

      std::optional<LithosStreamFrame> shift() {
        for (;;) {
          const std::string_view buffered = std::string_view(text).substr(start);
          const std::optional<rt::RegexMatch> match = LITHOS_SSE_FRAME_BOUNDARY().exec(buffered);
          if (!match) return std::nullopt;
          const std::string raw(buffered.substr(0, match->byteIndex));
          length -= static_cast<std::int64_t>(match->endIndex());
          start += match->byteEndIndex();
          std::optional<LithosStreamFrame> frame = parseLithosSseEventBlock(raw, requestedModel, servedModel);
          if (frame) return frame;
        }
      }

    private:
      std::string requestedModel;
      std::optional<std::string> servedModel;
      rt::TextDecoder decoder;
      std::string text;
      std::size_t start = 0;
      std::int64_t length = 0;
    };

    using ReadResult = rt::ReadableStreamReadResult<rt::ByteChunk>;

    rt::Reason streamAbortReason(const rt::AbortSignal& signal) {
      return rt::isError(signal.reason()) ? signal.reason() : rt::makeReason(rt::DOMException("The stream was aborted.", "AbortError"));
    }

    /** Races one upstream read against caller cancellation. */
    rt::Task<ReadResult> raceLithosReaderRead(rt::ReadableStreamDefaultReader<rt::ByteChunk> reader, std::optional<rt::AbortSignal> signal) {
      if (!signal) {
        ReadResult read = co_await reader.read();
        co_return read;
      }
      if (signal->aborted()) rt::rethrow(abortError(*signal));
      const auto aborted = rt::withResolvers<ReadResult>();
      // The listener lives in the signal, so it reaches the signal weakly.
      const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = signal->state();
      const rt::EventListener onAbort([reject = aborted.reject, weakSignal] {
        if (const auto state = weakSignal.lock()) reject(streamAbortReason(rt::AbortSignal(state)));
      });
      const auto cleanup = rt::scopeExit([&] {
        signal->removeEventListener("abort", onAbort);
        // The disarmed abort promise can no longer win. Release its reaction.
        aborted.resolve(ReadResult{.done = true, .value = std::nullopt});
      });
      signal->addEventListener("abort", onAbort, {.once = true});
      if (signal->aborted()) onAbort();
      const auto race = rt::race(reader.readPromise(), aborted.promise);
      auto result = co_await race.wait();
      co_return result;
    }

    /**
     * Owns one upstream SSE read loop: the reader lock, the decoded frame queue,
     * and the terminal release. Kept outside the generator so each concern is
     * independently readable.
     */
    class LithosStreamSession : public std::enable_shared_from_this<LithosStreamSession> {
    public:
      LithosStreamSession(rt::ReadableStreamDefaultReader<rt::ByteChunk> reader_, std::string requestedModel, std::optional<rt::AbortSignal> signal_,
                          std::optional<std::string> servedModel)
        : reader(std::move(reader_)), signal(std::move(signal_)), frames(std::move(requestedModel), std::move(servedModel)) {}

      void finish() {
        if (released) return;
        released = true;
        try {
          rt::Task<void> cancellation = reader.cancel(rt::thrown(json::value("LithosAI Chat Completions stream finished")));
          releaseReaderLock();
          rt::spawn(std::move(cancellation), rt::ignoreRejection);
        } catch (...) {
          releaseReaderLock();
        }
      }

      rt::Task<std::optional<LithosStreamFrame>> next() { return nextFrame(shared_from_this()); }

    private:
      void releaseReaderLock() {
        try {
          reader.releaseLock();
        } catch (...) {
          // The reader may already be released by a failing upstream stream.
        }
      }

      static rt::Task<std::optional<LithosStreamFrame>> nextFrame(std::shared_ptr<LithosStreamSession> session) {
        for (;;) {
          std::optional<LithosStreamFrame> frame = session->frames.shift();
          if (frame) co_return frame;
          if (session->eof) co_return std::nullopt;
          const rt::Promise<ReadResult> reading(raceLithosReaderRead(session->reader, session->signal));
          ReadResult result = co_await reading.wait();
          session->eof = result.done;
          session->frames.push(result.done ? nullptr : &*result.value);
        }
      }

      rt::ReadableStreamDefaultReader<rt::ByteChunk> reader;
      std::optional<rt::AbortSignal> signal;
      LithosFrameReader frames;
      bool eof = false;
      bool released = false;
    };

    rt::Task<void> iterateLithosChatCompletionStreamBody(rt::Response response, std::string requestedModel, IterateLithosChatCompletionStreamOptions options,
                                                         rt::Yield<json::object> yield) {
      const std::optional<rt::ByteStream> body = response.body();
      if (!body) throw invalidResponseError("Upstream returned no Chat Completions stream body.");
      const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = body->getReader();

      const auto session = std::make_shared<LithosStreamSession>(reader, requestedModel, options.signal, options.servedModel);
      // `try { ... } finally { session.finish(); }`: a return() at the yield unwinds through
      // here as rt::GeneratorReturn, which is rethrown after the release.
      rt::Reason failure;
      try {
        for (;;) {
          std::optional<LithosStreamFrame> frame;
          rt::Reason readFailure;
          try {
            const rt::Promise<std::optional<LithosStreamFrame>> pending(session->next());
            frame = co_await pending.wait();
          } catch (...) {
            readFailure = std::current_exception();
          }
          if (readFailure) {
            if (isLithosError(readFailure)) rt::rethrow(readFailure);
            if (options.signal && options.signal->aborted()) rt::rethrow(abortError(*options.signal));
            throw invalidResponseError("LithosAI Chat Completions stream failed.");
          }
          if (!frame) throw invalidResponseError("Upstream Chat Completions stream ended before [DONE].");
          if (std::holds_alternative<LithosStreamFrameDone>(*frame)) break;
          json::object value = std::move(std::get<LithosStreamFrameChunk>(*frame).value);
          co_await yield(std::move(value));
        }
      } catch (...) {
        failure = std::current_exception();
      }
      session->finish();
      if (failure) std::rethrow_exception(failure);
    }
  }

  void setLithosFetchTimeoutMsForTest(std::optional<double> timeoutMs) {
    lithosFetchTimeoutMs = timeoutMs.value_or(inference_deadline::STREAM_FIRST_EVENT_DEADLINE_MS);
  }

  std::optional<std::string> normalizeLithosProviderRequestId(const json::value* value) {
    const std::optional<std::string> requestId = exactNonEmptyString(value);
    if (!requestId || rt::length(*requestId) > MAX_LITHOS_PROVIDER_REQUEST_ID_LENGTH) return std::nullopt;
    static const rt::Regex HEADER_SAFE("^[\\x21-\\x7e]+$");
    return HEADER_SAFE.test(*requestId) ? requestId : std::nullopt;
  }
  std::optional<std::string> normalizeLithosProviderRequestId(const json::value& value) { return normalizeLithosProviderRequestId(&value); }

  std::optional<std::string> getLithosProviderRequestId(const rt::Response&) { return std::nullopt; }

  std::optional<std::string> readLithosApiKey() { return nonEmptyString(getEnv(LITHOS_API_KEY_ENV)); }

  std::string requireLithosApiKey() { return requireLithosApiKey(rt::Nullable<std::string>()); }

  std::string requireLithosApiKey(const rt::Nullable<std::string>& supplied) {
    std::optional<std::string> apiKey;
    if (supplied.isUndefined()) apiKey = readLithosApiKey();
    else if (supplied.hasValue()) apiKey = nonEmptyString(std::optional<std::string>(*supplied));
    if (apiKey) return *apiKey;
    throw LithosError("The requested model is not configured.", "lithos_api_key_missing", 503);
  }

  std::optional<std::string> lithosUpstreamModelFor(const std::string& model) {
    const std::unordered_map<std::string, std::string>& index = LITHOS_UPSTREAM_MODEL_BY_LOWERCASE_ID();
    const auto found = index.find(rt::toLowerCase(rt::trim(model)));
    return found != index.end() ? std::optional<std::string>(found->second) : std::nullopt;
  }

  json::object projectLithosRequest(const json::object& body, const std::string& requestedModel) {
    const std::optional<std::string> upstreamModel = lithosUpstreamModelFor(requestedModel);
    if (!upstreamModel) throw LithosError("The requested model is not configured.", "lithos_request_invalid", 400);
    json::object projected = rt::objectSpread(body);
    rt::objectDefine(projected, "model", json::value(*upstreamModel));
    if (const json::value* maxCompletionTokens = rt::get(projected, "max_completion_tokens"); maxCompletionTokens && !maxCompletionTokens->is_null()) {
      json::value cap = *maxCompletionTokens;
      rt::objectSet(projected, "max_tokens", std::move(cap));
    }
    rt::objectDelete(projected, "max_completion_tokens");
    return projected;
  }

  rt::Task<rt::Response> fetchLithosChatCompletions(json::object body, std::string requestedModel) {
    return fetchLithosChatCompletions(std::move(body), std::move(requestedModel), LithosChatCompletionsOptions{});
  }

  rt::Task<rt::Response> fetchLithosChatCompletions(json::object body, std::string requestedModel, LithosChatCompletionsOptions options) {
    std::string encodedBody;
    try {
      encodedBody = rt::jsonStringify(json::value(projectLithosRequest(body, requestedModel)));
    } catch (const LithosError&) {
      throw;
    } catch (...) {
      throw LithosError("Chat Completions requests must use a JSON-serializable body.", "lithos_request_invalid", 400);
    }
    // The TS also refuses a `JSON.stringify` result that is not a string (a body whose
    // `toJSON` yields undefined); a json::object has no toJSON, so its text always exists.

    const std::string apiKey = requireLithosApiKey(options.apiKey);
    const rt::Headers headers{
      {"Accept", "application/json"},
      {"Authorization", "Bearer " + apiKey},
      {"Content-Type", "application/json"},
    };
    const rt::AbortController deadline;
    const rt::Timeout timer = rt::setTimeout(
      [deadline] { deadline.abort(rt::makeReason(rt::DOMException("LithosAI response headers timed out.", "TimeoutError"))); }, lithosFetchTimeoutMs);
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
      if (options.sentinelUpstreamRecorder) upstreamAttempt = options.sentinelUpstreamRecorder->startAttempt("lithos");
      rt::RequestInit init;
      init.method = "POST";
      init.headers = headers;
      init.body = rt::BodyInit(encodedBody);
      init.redirect = "manual";
      init.signal = signal;
      const LithosFetch fetcher = options.fetcher ? options.fetcher : rt::globalFetch();
      const rt::Promise<rt::Response> fetching(fetcher(std::string(LITHOS_CHAT_COMPLETIONS_URL), std::move(init)));
      rt::Response response = co_await fetching.wait();
      if (options.onHeaders) options.onHeaders();
      delivered = upstreamAttempt ? upstreamAttempt->wrap(response) : response;
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      if (upstreamAttempt) upstreamAttempt->recordFetchError();
      if (isApiKeyQuotaDispatchError(failure) || isLithosError(failure)) rt::rethrow(failure);
      rt::rethrow(transportFailureError(failure, options.signal, deadline.signal()));
    }
    co_return std::move(*delivered);
  }

  std::optional<double> lithosCachedPromptTokens(const json::object& value, double promptTokens) {
    return usageDetailCounter(rt::get(value, "prompt_tokens_details"), "cached_tokens", promptTokens);
  }

  std::optional<double> lithosReasoningTokens(const json::object& value, double completionTokens) {
    return usageDetailCounter(rt::get(value, "completion_tokens_details"), "reasoning_tokens", completionTokens);
  }

  NormalizationResult<json::object> normalizeLithosChatCompletion(const json::value* value, const std::string& requestedModel,
                                                                  const std::optional<std::string>& servedModel) {
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
    if (!acceptUpstreamModel(rt::get(completion, "model"), requestedModel, servedModel)) {
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
    // The client asked for one of the provider's own ids; echo that id rather
    // than relaying the upstream spelling back at it.
    normalized["model"] = requestedModel;
    normalized["choices"] = std::move(choices);
    if (valueOf(usage)) normalized["usage"] = std::move(*valueOf(usage));
    return ok(std::move(normalized));
  }
  NormalizationResult<json::object> normalizeLithosChatCompletion(const json::value& value, const std::string& requestedModel,
                                                                  const std::optional<std::string>& servedModel) {
    return normalizeLithosChatCompletion(&value, requestedModel, servedModel);
  }

  NormalizationResult<json::object> normalizeLithosChatCompletionChunk(const json::value* value, const std::string& requestedModel,
                                                                       const std::optional<std::string>& servedModel) {
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
    if (!acceptUpstreamModel(rt::get(chunk, "model"), requestedModel, servedModel)) {
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
  NormalizationResult<json::object> normalizeLithosChatCompletionChunk(const json::value& value, const std::string& requestedModel,
                                                                       const std::optional<std::string>& servedModel) {
    return normalizeLithosChatCompletionChunk(&value, requestedModel, servedModel);
  }

  rt::AsyncGenerator<json::object> iterateLithosChatCompletionStream(rt::Response response, std::string requestedModel) {
    return iterateLithosChatCompletionStream(std::move(response), std::move(requestedModel), IterateLithosChatCompletionStreamOptions{});
  }

  rt::AsyncGenerator<json::object> iterateLithosChatCompletionStream(rt::Response response, std::string requestedModel, IterateLithosChatCompletionStreamOptions options) {
    return rt::AsyncGenerator<json::object>([response, requestedModel, options](rt::Yield<json::object> yield) {
      return iterateLithosChatCompletionStreamBody(response, requestedModel, options, std::move(yield));
    });
  }
}
