// Port of src/provider/cerebras.ts.
#include "uos/provider/cerebras.hpp"

#include "uos/inference_deadline.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/env.hpp"
#include "uos/rt/headers.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace uos::provider::cerebras {
  namespace {
    constexpr std::string_view CEREBRAS_API_KEY_ENV = "CEREBRAS_API_KEY";
    constexpr std::int64_t MAX_CEREBRAS_PROVIDER_REQUEST_ID_LENGTH = 256;
    // `new Set([...])`: membership only.
    bool isCerebrasUnsupportedSchemaField(std::string_view key) {
      static const std::unordered_set<std::string_view> fields{
        "format", "maxItems", "maxLength", "maxProperties", "minItems", "minLength", "minProperties", "pattern", "uniqueItems",
      };
      return fields.contains(key);
    }
    double cerebrasFetchTimeoutMs = inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS;

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

    rt::Reason timeoutError() { return rt::makeReason(CerebrasError("Upstream request exceeded the gateway deadline.", "gateway_timeout", 504)); }

    rt::Reason abortError(const rt::AbortSignal& signal) {
      return rt::isError(signal.reason()) ? signal.reason() : rt::makeReason(rt::DOMException("The request was aborted.", "AbortError"));
    }

    bool isTimeoutError(const rt::Reason& error) { return rt::isError(error) && rt::errorName(error) == "TimeoutError"; }

    // `error instanceof CerebrasError` / `error instanceof ApiKeyQuotaDispatchError`.
    bool isCerebrasError(const rt::Reason& error) {
      if (!error) return false;
      try {
        std::rethrow_exception(error);
      } catch (const CerebrasError&) {
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
    std::string requireCerebrasApiKey(const rt::Nullable<std::string>& supplied) {
      std::optional<std::string> apiKey;
      if (supplied.isUndefined()) apiKey = readCerebrasApiKey();
      else if (supplied.hasValue()) apiKey = nonEmptyString(std::optional<std::string>(*supplied));
      if (apiKey) return *apiKey;
      throw CerebrasError("The requested model is not configured.", "cerebras_api_key_missing", 503);
    }

    /**
     * Cerebras accepts a documented subset of JSON Schema for strict native
     * tools. Keep the product's complete schema at the gateway boundary, then
     * project only the provider-bound copy: its server-side validation remains
     * the authoritative enforcement for omitted bounds.
     *
     * IMPORTANT: unsupported KEYWORDS are stripped only at schema positions.
     * Property NAMES inside a `properties` map are ordinary field identifiers —
     * a field called `pattern`, `format` or `minLength` must never be dropped
     * (this used to corrupt multi-field tool schemas: the property disappeared
     * while `required` still named it, and clients saw the model "misassign"
     * arguments into whatever field survived). "const"/"oneOf" are likewise
     * only schema-level constructs.
     */
    // The TS recursion is bounded here by rt::kJsonMaxDepth (JavaScriptCore throws the same
    // RangeError once its own stack is exhausted).
    json::value projectCerebrasSchemaValue(const json::value& value, bool inPropertiesMap, std::size_t depth) {
      if (depth > rt::kJsonMaxDepth) throw rt::RangeError("Maximum call stack size exceeded.");
      if (value.is_array()) {
        json::array mapped;
        mapped.reserve(value.get_array().size());
        for (const json::value& child : value.get_array()) mapped.push_back(projectCerebrasSchemaValue(child, false, depth + 1));
        return mapped;
      }
      if (!utils::isRecord(value) || value.is_array()) return value;
      json::object projected;
      for (const json::key_value_pair* entry : rt::objectEntries(value.get_object())) {
        const std::string_view key = entry->key();
        const json::value& child = entry->value();
        if (!inPropertiesMap && isCerebrasUnsupportedSchemaField(key)) continue;
        if (key == "const" && !inPropertiesMap) {
          json::array single;
          single.push_back(projectCerebrasSchemaValue(child, false, depth + 1));
          rt::objectSet(projected, "enum", std::move(single));
          continue;
        }
        json::value projectedChild = projectCerebrasSchemaValue(child, key == "properties", depth + 1);
        rt::objectSet(projected, key == "oneOf" && !inPropertiesMap ? std::string_view("anyOf") : key, std::move(projectedChild));
      }
      return projected;
    }

    std::vector<json::value> distinctSchemas(const std::vector<json::value>& values) {
      rt::OrderedMap<std::string, json::value> distinct;
      for (const json::value& value : values) distinct.set(rt::jsonStringify(value), value);
      std::vector<json::value> out;
      for (const auto& [text, value] : distinct) out.push_back(value);
      return out;
    }

    /**
     * Code-unit ascending string order. This is exactly what an argument-less
     * `Array.prototype.sort()` does for strings, so it keeps the projected
     * `required` array byte-identical on the upstream wire while still giving
     * `sort` an explicit comparator.
     */
    double compareStrings(const std::string& left, const std::string& right) {
      if (rt::lessThan(left, right)) return -1;
      if (rt::lessThan(right, left)) return 1;
      return 0;
    }

    bool isObjectVariant(const json::value& variant) {
      if (!utils::isRecord(variant) || variant.is_array()) return false;
      const json::value* type = rt::get(variant, "type");
      const json::value* properties = rt::get(variant, "properties");
      return type && type->is_string() && type->get_string() == "object" && utils::isRecord(properties) && !properties->is_array();
    }

    /**
     * Cerebras requires every function parameter schema to have an object root.
     * Our read tool's exact server-side schema is a root union by operation ID;
     * present it to the model as one object with a constrained ID and nested
     * argument union, then retain exact validation after the response returns.
     */
    json::value collapseCerebrasRootObjectUnion(const json::value& value) {
      if (!utils::isRecord(value) || value.is_array()) return value;
      const json::value* type = rt::get(value, "type");
      const json::value* anyOf = rt::get(value, "anyOf");
      if ((type && type->is_string() && type->get_string() == "object") || !(anyOf && anyOf->is_array())) return value;
      const json::array& variants = anyOf->get_array();
      if (variants.empty() || std::any_of(variants.begin(), variants.end(), [](const json::value& variant) { return !isObjectVariant(variant); })) return value;

      rt::OrderedMap<std::string, std::vector<json::value>> fields;
      std::optional<rt::OrderedSet<std::string>> requiredByEveryVariant;
      for (const json::value& variant : variants) {
        const json::object& properties = rt::get(variant, "properties")->get_object();
        for (const json::key_value_pair* entry : rt::objectEntries(properties)) {
          const std::string name(entry->key());
          // `fields.get(name) ?? []`, pushed and set back: the same array keeps its position.
          if (std::vector<json::value>* values = fields.get(name)) values->push_back(entry->value());
          else fields.set(name, std::vector<json::value>{entry->value()});
        }
        rt::OrderedSet<std::string> required;
        if (const json::value* names = rt::get(variant, "required"); names && names->is_array()) {
          for (const json::value& name : names->get_array())
            if (name.is_string()) required.add(std::string(name.get_string()));
        }
        if (!requiredByEveryVariant) {
          requiredByEveryVariant = std::move(required);
        } else {
          // `Set.prototype.intersection`: membership is all the sorted result below keeps.
          rt::OrderedSet<std::string> both;
          for (const std::string& name : *requiredByEveryVariant)
            if (required.has(name)) both.add(name);
          requiredByEveryVariant = std::move(both);
        }
      }

      json::object properties;
      for (const auto& [name, candidates] : fields) {
        std::vector<json::value> distinct = distinctSchemas(candidates);
        if (distinct.size() == 1) {
          rt::objectSet(properties, name, distinct[0]);
          continue;
        }
        if (name == "operationId" && std::all_of(distinct.begin(), distinct.end(), [](const json::value& candidate) {
              const json::value* values = rt::get(candidate, "enum");
              return utils::isRecord(candidate) && values && values->is_array();
            })) {
          std::vector<json::value> flattened;
          for (const json::value& candidate : distinct)
            for (const json::value& item : rt::get(candidate, "enum")->get_array()) flattened.push_back(item);
          json::array values;
          for (json::value& item : distinctSchemas(flattened)) values.push_back(std::move(item));
          json::object enumSchema;
          enumSchema["enum"] = std::move(values);
          rt::objectSet(properties, name, std::move(enumSchema));
          continue;
        }
        json::array union_;
        for (json::value& candidate : distinct) union_.push_back(std::move(candidate));
        json::object unionSchema;
        unionSchema["anyOf"] = std::move(union_);
        rt::objectSet(properties, name, std::move(unionSchema));
      }
      std::vector<std::string> required;
      if (requiredByEveryVariant) required = rt::arrayFrom(*requiredByEveryVariant);
      rt::sort(required, compareStrings);
      json::array requiredArray;
      for (std::string& name : required) requiredArray.push_back(json::value(std::move(name)));
      json::object collapsed;
      collapsed["type"] = "object";
      collapsed["properties"] = std::move(properties);
      collapsed["required"] = std::move(requiredArray);
      collapsed["additionalProperties"] = false;
      return collapsed;
    }

    /**
     * Projects one message onto the Cerebras wire.
     *
     * The `developer` role is mapped onto the `system` role Cerebras accepts: the
     * chat template rejects an unknown role instead of ignoring it (verified live
     * 2026-09-26: `system` 200, `developer` 400), and DeepSeek Harness sends
     * `developer` for system instructions, so an unprojected harness turn can
     * never succeed on this route.
     *
     * `reasoning_content` is removed from an assistant turn. Cerebras validates
     * every message field instead of ignoring the ones it does not implement, so
     * the reasoning this gateway's shared Responses translation replays on
     * assistant turns fails the whole request before the model is reached:
     *
     *   HTTP 400 wrong_api_format: messages.3.assistant.reasoning_content and
     *   messages.4.assistant.reasoning_content are unsupported
     *
     * (Codex session 01a0fea8-58df-7d93-af95-49f80cec6a1b, 2026-10-02, model
     * qwen-3.8-27b: the first tool-bearing turn dispatched, and the continuation
     * carrying the replayed reasoning item failed on both attempts, so the thread
     * could never continue.) The field is still required by DeepSeek's own wire in
     * thinking mode, so the removal belongs here, at the provider-bound copy
     * rather than in the shared translation: this transport is the one seam both
     * Cerebras wires (Chat Completions and the Responses adapter) dispatch
     * through, and DeepSeek/LithosAI bodies are untouched. Reasoning the provider
     * returns is unaffected.
     *
     * A message of any other role is returned as it stands; a `developer` or
     * assistant message is copied before it is projected, so the caller's own
     * request record and every other message object survive untouched.
     */
    json::value projectCerebrasMessage(const json::value& message) {
      if (!utils::isRecord(message) || message.is_array()) return message;
      const json::value* role = rt::get(message, "role");
      const bool developer = role && role->is_string() && role->get_string() == "developer";
      if (!developer && !(role && role->is_string() && role->get_string() == "assistant")) return message;
      json::object projected = rt::objectSpread(message.get_object());
      if (developer) rt::objectDefine(projected, "role", json::value("system"));
      rt::objectDelete(projected, "reasoning_content");
      return projected;
    }

    // `(messages: unknown): unknown`; nullptr is undefined.
    std::optional<json::value> projectCerebrasMessages(const json::value* messages) {
      if (!messages) return std::nullopt;
      if (!messages->is_array()) return *messages;
      json::array mapped;
      mapped.reserve(messages->get_array().size());
      for (const json::value& message : messages->get_array()) mapped.push_back(projectCerebrasMessage(message));
      return json::value(std::move(mapped));
    }

    bool isCerebrasSystemMessage(const json::value* message) {
      if (!utils::isRecord(message) || message->is_array()) return false;
      const json::value* role = rt::get(*message, "role");
      return role && role->is_string() && role->get_string() == "system";
    }

    /** Reject content Cerebras cannot represent instead of silently dropping it. */
    [[noreturn]] void rejectUnsupportedCerebrasSystemContent() {
      throw CerebrasError("Cerebras supports only text content in system and developer messages.", "cerebras_request_invalid", 400);
    }

    /** One part of a `system` message's content: a string part whole, or a record part's text. */
    std::string cerebrasSystemPartText(const json::value& part) {
      if (part.is_string()) return std::string(part.get_string());
      if (utils::isRecord(part) && !part.is_array()) {
        const json::value* text = rt::get(part, "text");
        if (text && text->is_string()) return std::string(text->get_string());
      }
      rejectUnsupportedCerebrasSystemContent();
    }

    /** One `system` message's own text: a string whole, or the concatenation of its text parts. */
    std::string cerebrasSystemContentText(const json::value* content) {
      if (content && content->is_string()) return std::string(content->get_string());
      if (!(content && content->is_array())) rejectUnsupportedCerebrasSystemContent();
      std::vector<std::string> parts;
      for (const json::value& part : content->get_array()) parts.push_back(cerebrasSystemPartText(part));
      return rt::join(parts, "");
    }

    /**
     * Normalizes the tool calls of one choice. `undefined` means the message
     * carried no `tool_calls` field at all.
     */
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

    /** Upstream `null`, `undefined` and strings are the only accepted nullable text values. */
    bool isAbsentOrString(const json::value* value) { return !value || value->is_null() || value->is_string(); }

    // The result's std::nullopt is undefined.
    NormalizationResult<std::optional<json::array>> normalizeToolCalls(const json::value* rawToolCalls, std::size_t choiceIndex) {
      if (!rawToolCalls) return ok(std::optional<json::array>());
      if (!rawToolCalls->is_array()) {
        return notOk("Upstream choice " + numberText(choiceIndex) + " has invalid tool calls.");
      }
      json::array toolCalls;
      std::size_t callIndex = 0;
      for (const json::value& call : rawToolCalls->get_array()) {
        NormalizationResult<json::object> normalized = normalizeToolCall(call, callIndex++);
        if (failed(normalized)) return failureOf(normalized);
        toolCalls.push_back(std::move(valueOf(normalized)));
      }
      return ok(std::optional<json::array>(std::move(toolCalls)));
    }

    bool hasToolCalls(const std::optional<json::array>& toolCalls) { return toolCalls && !toolCalls->empty(); }

    bool choiceHasNoPayload(const json::value* content, const std::optional<json::array>& toolCalls, const std::string& refusal) {
      return (!content || content->is_null()) && !hasToolCalls(toolCalls) && refusal.empty();
    }

    /** Mirrors the upstream `content` field: absent content becomes explicit JSON `null` only when a tool call or refusal exists. */
    json::value choiceContent(const json::value* content, const std::optional<json::array>& toolCalls, const std::string& refusal) {
      if (content && !content->is_null()) return *content;
      return hasToolCalls(toolCalls) || !refusal.empty() ? json::value(nullptr) : json::value("");
    }

    // `Readonly<{ content: unknown; refusal: string; toolCalls: Record<string, unknown>[] | undefined }>`
    struct ChoiceMessagePart {
      const json::value* content;  // nullptr is undefined; points into the message
      std::string refusal;
      std::optional<json::array> toolCalls;
    };

    /** Validates the assistant message of one choice and projects it for normalization. */
    NormalizationResult<ChoiceMessagePart> normalizeChoiceMessage(const json::object& message, std::size_t index) {
      const json::value* role = rt::get(message, "role");
      if (!(role && role->is_string() && role->get_string() == "assistant")) {
        return notOk("Upstream choice " + numberText(index) + " does not contain an assistant message.");
      }
      const json::value* content = rt::get(message, "content");
      if (!isAbsentOrString(content)) {
        return notOk("Upstream choice " + numberText(index) + " has unsupported message content.");
      }
      const json::value* rawRefusal = rt::get(message, "refusal");
      if (!isAbsentOrString(rawRefusal)) {
        return notOk("Upstream choice " + numberText(index) + " has an invalid refusal.");
      }
      std::string refusal = rawRefusal && rawRefusal->is_string() ? std::string(rawRefusal->get_string()) : std::string();
      NormalizationResult<std::optional<json::array>> toolCallsResult = normalizeToolCalls(rt::get(message, "tool_calls"), index);
      if (failed(toolCallsResult)) return failureOf(toolCallsResult);
      std::optional<json::array>& toolCalls = valueOf(toolCallsResult);
      if (choiceHasNoPayload(content, toolCalls, refusal)) {
        return notOk("Upstream choice " + numberText(index) + " has neither content nor a tool call.");
      }
      return ok(ChoiceMessagePart{content, std::move(refusal), std::move(toolCalls)});
    }

    NormalizationResult<json::object> normalizeChoice(const json::value& value, std::size_t index) {
      if (!utils::isRecord(value) || value.is_array()) {
        return notOk("Upstream choice " + numberText(index) + " is not an object.");
      }
      const json::object& choice = value.get_object();
      const std::optional<double> choiceIndex = nonNegativeInteger(rt::get(choice, "index"));
      if (!choiceIndex) return notOk("Upstream choice " + numberText(index) + " has an invalid index.");
      const json::value* rawMessage = rt::get(choice, "message");
      if (!utils::isRecord(rawMessage) || rawMessage->is_array()) {
        return notOk("Upstream choice " + numberText(index) + " is missing an assistant message.");
      }
      const json::object& message = rawMessage->get_object();
      NormalizationResult<ChoiceMessagePart> normalizedMessagePart = normalizeChoiceMessage(message, index);
      if (failed(normalizedMessagePart)) return failureOf(normalizedMessagePart);
      ChoiceMessagePart& part = valueOf(normalizedMessagePart);

      const json::value* finishReason = rt::get(choice, "finish_reason");
      if (!isAbsentOrString(finishReason)) {
        return notOk("Upstream choice " + numberText(index) + " has an invalid finish reason.");
      }
      json::object normalizedMessage;
      normalizedMessage["role"] = "assistant";
      normalizedMessage["content"] = choiceContent(part.content, part.toolCalls, part.refusal);
      // 1:1 with native Cerebras (compliance D1, measured 2026-08-29): the
      // upstream reasoning field must reach clients unchanged. The gateway
      // previously dropped it, so clients saw content+role only.
      if (const json::value* reasoning = rt::get(message, "reasoning"); reasoning && reasoning->is_string()) normalizedMessage["reasoning"] = *reasoning;
      if (!part.refusal.empty()) normalizedMessage["refusal"] = part.refusal;
      const bool toolCallsPresent = hasToolCalls(part.toolCalls);
      if (toolCallsPresent) normalizedMessage["tool_calls"] = std::move(*part.toolCalls);
      json::object normalized;
      normalized["index"] = rt::makeNumber(*choiceIndex);
      normalized["message"] = std::move(normalizedMessage);
      normalized["finish_reason"] = !rt::isNullish(finishReason) ? *finishReason : json::value(toolCallsPresent ? "tool_calls" : "stop");
      return ok(std::move(normalized));
    }

    /**
     * One documented nested usage counter, validated against the total it is
     * defined as a subset of. An absent or impossible value stays absent, so no
     * reader downstream can mistake a missing measurement for a measured zero.
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
     * Reduces a Cerebras usage object to the OpenAI Chat Completions usage shape
     * the Assistant consumes. The provider reports cache reads and reasoning tokens
     * as the documented nested details, so those counters are relayed under their
     * official names rather than dropped; an upstream that reports neither leaves
     * the corresponding detail object absent.
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
      const std::optional<double> cachedTokens = cerebrasCachedPromptTokens(usage, *promptTokens);
      const std::optional<double> reasoningTokens = cerebrasReasoningTokens(usage, *completionTokens);
      json::object normalized;
      normalized["prompt_tokens"] = rt::makeNumber(*promptTokens);
      normalized["completion_tokens"] = rt::makeNumber(*completionTokens);
      normalized["total_tokens"] = rt::makeNumber(*totalTokens);
      if (cachedTokens) normalized["prompt_tokens_details"] = singleCounter("cached_tokens", *cachedTokens);
      if (reasoningTokens) normalized["completion_tokens_details"] = singleCounter("reasoning_tokens", *reasoningTokens);
      return ok(std::optional<json::object>(std::move(normalized)));
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
      return rt::makeReason(CerebrasError("Upstream request could not be completed.", "cerebras_upstream_unreachable", 502));
    }
  }

  std::optional<std::string> cerebrasUpstreamModelFor(const std::string& model) {
    const std::string requested = rt::toLowerCase(rt::trim(model));
    const auto found = std::find(CEREBRAS_MODELS.begin(), CEREBRAS_MODELS.end(), requested);
    return found != CEREBRAS_MODELS.end() ? std::optional<std::string>(*found) : std::nullopt;
  }

  std::optional<std::string> normalizeCerebrasProviderRequestId(const json::value* value) {
    const std::optional<std::string> requestId = exactNonEmptyString(value);
    if (!requestId || rt::length(*requestId) > MAX_CEREBRAS_PROVIDER_REQUEST_ID_LENGTH) return std::nullopt;
    static const rt::Regex HEADER_SAFE("^[\\x21-\\x7e]+$");
    return HEADER_SAFE.test(*requestId) ? requestId : std::nullopt;
  }
  std::optional<std::string> normalizeCerebrasProviderRequestId(const json::value& value) { return normalizeCerebrasProviderRequestId(&value); }

  std::optional<std::string> getCerebrasProviderRequestId(const rt::Response& response) {
    std::optional<std::string> header = response.headers().get("X-Request-Id");
    if (!header) header = response.headers().get("X-Api-Request-Id");
    if (!header) header = response.headers().get("X-Cerebras-Request-Id");
    const json::value value = header ? json::value(*header) : json::value(nullptr);
    return normalizeCerebrasProviderRequestId(&value);
  }

  std::optional<std::string> readCerebrasApiKey() { return nonEmptyString(getEnv(CEREBRAS_API_KEY_ENV)); }

  void setCerebrasFetchTimeoutMsForTest(std::optional<double> timeoutMs) {
    cerebrasFetchTimeoutMs = timeoutMs.value_or(inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS);
  }

  std::optional<json::value> projectCerebrasToolSchema(const json::value* value) {
    if (!value) return std::nullopt;
    return projectCerebrasToolSchema(*value);
  }
  json::value projectCerebrasToolSchema(const json::value& value) { return collapseCerebrasRootObjectUnion(projectCerebrasSchemaValue(value, false, 0)); }

  std::optional<json::value> collapseCerebrasSystemMessages(const json::value* messages) {
    if (!messages) return std::nullopt;
    return collapseCerebrasSystemMessages(*messages);
  }
  json::value collapseCerebrasSystemMessages(const json::value& messages) {
    if (!messages.is_array()) return messages;
    const json::array& list = messages.get_array();
    std::vector<const json::value*> systems;
    for (const json::value& message : list)
      if (isCerebrasSystemMessage(&message)) systems.push_back(&message);
    if (systems.empty()) return messages;
    if (systems.size() == 1 && isCerebrasSystemMessage(rt::get(list, std::size_t{0}))) {
      (void)cerebrasSystemContentText(rt::get(*systems[0], "content"));
      return messages;
    }
    std::vector<std::string> texts;
    for (const json::value* message : systems) {
      std::string text = cerebrasSystemContentText(rt::get(*message, "content"));
      if (!text.empty()) texts.push_back(std::move(text));
    }
    json::object leading = rt::objectSpread(systems[0]->get_object());
    rt::objectDefine(leading, "content", json::value(rt::join(texts, "\n\n")));
    json::array collapsed;
    collapsed.push_back(std::move(leading));
    for (const json::value& message : list)
      if (!isCerebrasSystemMessage(&message)) collapsed.push_back(message);
    return collapsed;
  }

  json::object projectCerebrasRequest(const json::object& body) {
    // The Chat wire's `developer` mapping and the system-message collapse both
    // live here, so every dispatch through this transport carries exactly one
    // leading `system` message whichever wire built the body.
    const std::optional<json::value> projectedMessages = projectCerebrasMessages(rt::get(body, "messages"));
    const std::optional<json::value> messages = projectedMessages ? std::optional<json::value>(collapseCerebrasSystemMessages(*projectedMessages)) : std::nullopt;
    json::object projected = rt::objectSpread(body);
    // `messages: undefined` is a member JSON.stringify skips.
    if (messages) rt::objectDefine(projected, "messages", *messages);
    else rt::objectDelete(projected, "messages");
    const json::value* tools = rt::get(body, "tools");
    if (!(tools && tools->is_array())) return projected;
    json::array projectedTools;
    projectedTools.reserve(tools->get_array().size());
    for (const json::value& tool : tools->get_array()) {
      const json::value* function = utils::isRecord(tool) && !tool.is_array() ? rt::get(tool, "function") : nullptr;
      if (!utils::isRecord(tool) || tool.is_array() || !utils::isRecord(function) || function->is_array()) {
        projectedTools.push_back(tool);
        continue;
      }
      json::object projectedFunction = rt::objectSpread(function->get_object());
      if (const json::value* parameters = rt::get(function->get_object(), "parameters")) {
        rt::objectDefine(projectedFunction, "parameters", projectCerebrasToolSchema(*parameters));
      }
      json::object projectedTool = rt::objectSpread(tool.get_object());
      rt::objectDefine(projectedTool, "function", json::value(std::move(projectedFunction)));
      projectedTools.push_back(std::move(projectedTool));
    }
    json::object withTools = rt::objectSpread(projected);
    rt::objectDefine(withTools, "tools", json::value(std::move(projectedTools)));
    return withTools;
  }

  rt::Task<rt::Response> fetchCerebrasChatCompletions(json::object body) {
    return fetchCerebrasChatCompletions(std::move(body), CerebrasChatCompletionsOptions{});
  }

  rt::Task<rt::Response> fetchCerebrasChatCompletions(json::object body, CerebrasChatCompletionsOptions options) {
    std::string encodedBody;
    try {
      encodedBody = rt::jsonStringify(json::value(projectCerebrasRequest(body)));
    } catch (const CerebrasError&) {
      throw;
    } catch (...) {
      throw CerebrasError("Chat Completions requests must use a JSON-serializable body.", "cerebras_request_invalid", 400);
    }
    // The TS also refuses a `JSON.stringify` result that is not a string (a body whose
    // `toJSON` yields undefined); a json::object has no toJSON, so its text always exists.

    const std::string apiKey = requireCerebrasApiKey(options.apiKey);
    const rt::Headers headers{
      {"Accept", "application/json"},
      {"Authorization", "Bearer " + apiKey},
      {"Content-Type", "application/json"},
    };
    const rt::AbortController deadline;
    const rt::Timeout timer = rt::setTimeout(
      [deadline] { deadline.abort(rt::makeReason(rt::DOMException("Cerebras response headers timed out.", "TimeoutError"))); }, cerebrasFetchTimeoutMs);
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
      if (options.sentinelUpstreamRecorder) upstreamAttempt = options.sentinelUpstreamRecorder->startAttempt("cerebras");
      rt::RequestInit init;
      init.method = "POST";
      init.headers = headers;
      init.body = rt::BodyInit(encodedBody);
      init.redirect = "manual";
      init.signal = signal;
      const CerebrasFetch fetcher = options.fetcher ? options.fetcher : rt::globalFetch();
      const rt::Promise<rt::Response> fetching(fetcher(std::string(CEREBRAS_CHAT_COMPLETIONS_URL), std::move(init)));
      rt::Response response = co_await fetching.wait();
      if (options.onHeaders) options.onHeaders();
      delivered = upstreamAttempt ? upstreamAttempt->wrap(response) : response;
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) {
      if (upstreamAttempt) upstreamAttempt->recordFetchError();
      if (isApiKeyQuotaDispatchError(failure) || isCerebrasError(failure)) rt::rethrow(failure);
      rt::rethrow(transportFailureError(failure, options.signal, deadline.signal()));
    }
    co_return std::move(*delivered);
  }

  std::optional<double> cerebrasCachedPromptTokens(const json::object& value, double promptTokens) {
    return usageDetailCounter(rt::get(value, "prompt_tokens_details"), "cached_tokens", promptTokens);
  }

  std::optional<double> cerebrasReasoningTokens(const json::object& value, double completionTokens) {
    return usageDetailCounter(rt::get(value, "completion_tokens_details"), "reasoning_tokens", completionTokens);
  }

  NormalizationResult<json::object> normalizeCerebrasChatCompletion(const json::value* value, const std::string& requestedModel) {
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
    const std::optional<std::string> upstreamModel = nonEmptyString(rt::get(completion, "model"));
    if (upstreamModel && *upstreamModel != requestedModel) {
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
    normalized["model"] = requestedModel;
    normalized["choices"] = std::move(choices);
    if (valueOf(usage)) normalized["usage"] = std::move(*valueOf(usage));
    return ok(std::move(normalized));
  }
  NormalizationResult<json::object> normalizeCerebrasChatCompletion(const json::value& value, const std::string& requestedModel) {
    return normalizeCerebrasChatCompletion(&value, requestedModel);
  }
}
