#include "uos/responses_failover_stream.hpp"

#include "uos/codex/responses_error.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/utils.hpp"

#include <cstddef>
#include <deque>
#include <exception>
#include <memory>
#include <string>
#include <utility>

namespace uos::responses_failover_stream {
  using responses_stream::MaybeAsyncCallback;
  using responses_stream::RESPONSES_TERMINAL_EVENT_TYPES;
  using responses_stream::ResponsesStreamError;
  using responses_stream::ResponsesStreamEvent;
  using responses_stream::ResponsesStreamFailureKind;
  using responses_stream::ResponsesStreamIterator;

  namespace {
    using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;
    // `ResponsesSemanticKind | null`, and `| undefined` (undecided) around it.
    using SemanticKind = std::optional<ResponsesSemanticKind>;
    using UndecidedSemanticKind = std::optional<SemanticKind>;

    const SemanticKind TEXT = ResponsesSemanticKind("text");
    const SemanticKind TOOL_CALL = ResponsesSemanticKind("tool_call");

    json::value text(std::string_view value) { return json::value(json::string(value)); }

    bool isOneOf(std::string_view value, std::initializer_list<std::string_view> options) {
      for (std::string_view option : options)
        if (value == option) return true;
      return false;
    }

    bool isTextType(std::string_view type) { return isOneOf(type, {"response.output_text.delta", "response.output_text.done"}); }
    bool isRefusalType(std::string_view type) { return isOneOf(type, {"response.refusal.delta", "response.refusal.done"}); }
    // reasoningTextProgressFields.get(type)
    std::optional<std::string_view> reasoningTextProgressField(std::string_view type) {
      if (type == "response.reasoning_summary_text.delta") return "delta";
      if (type == "response.reasoning_summary_text.done") return "text";
      if (type == "response.reasoning_text.delta") return "delta";
      if (type == "response.reasoning_text.done") return "text";
      return std::nullopt;
    }
    bool isReasoningSummaryPartProgressType(std::string_view type) {
      return isOneOf(type, {"response.reasoning_summary_part.added", "response.reasoning_summary_part.done"});
    }
    bool isExecutableToolType(std::string_view type) { return isOneOf(type, {"function_call", "custom_tool_call"}); }
    constexpr std::string_view hostedToolTypes[] = {"code_interpreter_call", "computer_call", "file_search_call", "image_generation_call", "mcp_call", "web_search_call"};
    bool isHostedToolType(std::string_view type) {
      for (std::string_view hosted : hostedToolTypes)
        if (type == hosted) return true;
      return false;
    }
    // hostedToolTerminalEventTypes: `response.<tool>.completed`, `.in_progress` and `.failed`.
    bool isHostedToolTerminalEventType(std::string_view type) {
      for (std::string_view hosted : hostedToolTypes) {
        for (std::string_view lifecycle : {"completed", "in_progress", "failed"}) {
          if (type == "response." + std::string(hosted) + "." + std::string(lifecycle)) return true;
        }
      }
      return false;
    }
    constexpr std::string_view imagePartialEventType = "response.image_generation_call.partial_image";

    bool isStringValue(const json::value* value) { return value && value->is_string(); }
    bool equalsString(const json::value* value, std::string_view expected) { return value && value->is_string() && value->get_string() == expected; }

    bool nonEmptyString(const json::value* value) { return value && value->is_string() && !value->get_string().empty(); }

    bool nonEmptyText(const json::object& value) { return nonEmptyString(rt::get(value, "delta")) || nonEmptyString(rt::get(value, "text")); }

    bool nonEmptyTrimmedString(const json::value* value) { return value && value->is_string() && !rt::trim(value->get_string()).empty(); }

    // `getString(value)?.trim()`.
    std::optional<std::string> trimmedString(const json::value* value) {
      const std::optional<std::string> raw = utils::getString(value);
      if (!raw) return std::nullopt;
      return rt::trim(*raw);
    }

    /** An executable tool call is only semantic once its identity and payload are both present. */
    SemanticKind executableToolCallKind(const json::value& item) {
      const json::value* itemType = rt::get(item, "type");
      if (!isStringValue(itemType) || !isExecutableToolType(itemType->get_string())) return std::nullopt;
      if (!nonEmptyTrimmedString(rt::get(item, "call_id")) || !nonEmptyTrimmedString(rt::get(item, "name"))) return std::nullopt;
      if (itemType->get_string() == "function_call") return isStringValue(rt::get(item, "arguments")) ? TOOL_CALL : std::nullopt;
      return isStringValue(rt::get(item, "input")) ? TOOL_CALL : std::nullopt;
    }

    /** A hosted tool call item is semantic once it carries one of the three lifecycle statuses. */
    SemanticKind hostedToolItemKind(const json::value& item) {
      const json::value* itemType = rt::get(item, "type");
      if (!isStringValue(itemType) || !isHostedToolType(itemType->get_string())) return std::nullopt;
      const json::value* status = rt::get(item, "status");
      if (!equalsString(status, "in_progress") && !equalsString(status, "completed") && !equalsString(status, "failed")) return std::nullopt;
      return TOOL_CALL;
    }

    bool outputContentPartHasText(const json::value& part) {
      const json::value* type = rt::get(part, "type");
      return utils::isRecord(part) && (((equalsString(type, "output_text") || equalsString(type, "text")) && nonEmptyString(rt::get(part, "text"))) ||
                                       (equalsString(type, "refusal") && nonEmptyString(rt::get(part, "refusal"))));
    }

    /** Classifies one `output` entry exactly as the upstream providers shape them. */
    SemanticKind semanticKindFromOutputItem(const json::value& item, const std::optional<std::string>& ignoredOutputItemId) {
      if (!utils::isRecord(item) || item.is_array()) return std::nullopt;
      if (ignoredOutputItemId && trimmedString(rt::get(item, "id")) == ignoredOutputItemId) return std::nullopt;
      const SemanticKind executableKind = executableToolCallKind(item);
      if (executableKind) return executableKind;
      const SemanticKind hostedKind = hostedToolItemKind(item);
      if (hostedKind) return hostedKind;
      if (equalsString(rt::get(item, "type"), "reasoning")) return std::nullopt;
      const json::value* content = rt::get(item, "content");
      if (!content || !content->is_array()) return std::nullopt;
      for (const json::value& part : content->get_array())
        if (outputContentPartHasText(part)) return TEXT;
      return std::nullopt;
    }

    SemanticKind semanticKindFromOutput(const json::value* output, const std::optional<std::string>& ignoredOutputItemId = std::nullopt) {
      if (!output || !output->is_array()) return std::nullopt;
      for (const json::value& item : output->get_array()) {
        const SemanticKind kind = semanticKindFromOutputItem(item, ignoredOutputItemId);
        if (kind) return kind;
      }
      return std::nullopt;
    }

    SemanticKind contentPartSemanticKind(const json::value& part) {
      const json::value* type = rt::get(part, "type");
      if ((equalsString(type, "output_text") || equalsString(type, "text")) && nonEmptyString(rt::get(part, "text"))) return TEXT;
      if (equalsString(type, "refusal") && nonEmptyString(rt::get(part, "refusal"))) return TEXT;
      return std::nullopt;
    }

    SemanticKind imagePartialSemanticKind(const json::object& value) {
      return nonEmptyString(rt::get(value, "partial_image_b64")) || nonEmptyString(rt::get(value, "partial_image")) || nonEmptyString(rt::get(value, "result"))
               ? TOOL_CALL
               : std::nullopt;
    }

    /** Text-bearing events decide their kind from their own fields; `undefined` means undecided. */
    UndecidedSemanticKind textBearingEventSemanticKind(const ResponsesStreamEvent& event) {
      if (isTextType(event.type)) return nonEmptyText(event.value) ? TEXT : SemanticKind();
      if (isRefusalType(event.type)) return nonEmptyText(event.value) || nonEmptyString(rt::get(event.value, "refusal")) ? TEXT : SemanticKind();
      if (event.type != "response.content_part.done") return std::nullopt;
      const json::value* part = rt::get(event.value, "part");
      if (!utils::isRecord(part)) return std::nullopt;
      const SemanticKind partKind = contentPartSemanticKind(*part);
      if (partKind) return partKind;
      return std::nullopt;
    }

    /** Decides a kind from the event itself, without inspecting any output payload. */
    UndecidedSemanticKind directResponsesEventSemanticKind(const ResponsesStreamEvent& event) {
      const UndecidedSemanticKind textKind = textBearingEventSemanticKind(event);
      if (textKind) return textKind;
      if (isHostedToolTerminalEventType(event.type)) return TOOL_CALL;
      if (event.type == imagePartialEventType) return imagePartialSemanticKind(event.value);
      return std::nullopt;
    }

    SemanticKind outputItemEventSemanticKind(const ResponsesStreamEvent& event, const json::value& item, const std::optional<std::string>& ignoredOutputItemId) {
      const std::string itemType = utils::getString(rt::get(item, "type")).value_or("");
      if (event.type == "response.output_item.done" && isExecutableToolType(itemType)) return executableToolCallKind(item);
      if (isHostedToolType(itemType)) return semanticKindFromOutputItem(item, ignoredOutputItemId);
      if (event.type == "response.output_item.added") return std::nullopt;
      return semanticKindFromOutputItem(item, ignoredOutputItemId);
    }

    /** Lifecycle output items decide their kind from the item alone; `undefined` means undecided. */
    UndecidedSemanticKind outputItemResponsesEventSemanticKind(const ResponsesStreamEvent& event, const std::optional<std::string>& ignoredOutputItemId) {
      const bool isOutputItemEvent = event.type == "response.output_item.added" || event.type == "response.output_item.done";
      const json::value* item = rt::get(event.value, "item");
      if (!isOutputItemEvent || !utils::isRecord(item)) return std::nullopt;
      return outputItemEventSemanticKind(event, *item, ignoredOutputItemId);
    }

    SemanticKind outputPayloadSemanticKind(const ResponsesStreamEvent& event, const std::optional<std::string>& ignoredOutputItemId) {
      const json::value* valueResponse = rt::get(event.value, "response");
      if (utils::isRecord(valueResponse) && !valueResponse->is_array()) return semanticKindFromOutput(rt::get(*valueResponse, "output"), ignoredOutputItemId);
      return semanticKindFromOutput(rt::get(event.value, "output"), ignoredOutputItemId);
    }

    SemanticKind responsesEventSemanticKindWithIgnoredOutputItem(const ResponsesStreamEvent& event, const std::optional<std::string>& ignoredOutputItemId) {
      const UndecidedSemanticKind directKind = directResponsesEventSemanticKind(event);
      if (directKind) return *directKind;
      const UndecidedSemanticKind itemKind = outputItemResponsesEventSemanticKind(event, ignoredOutputItemId);
      if (itemKind) return *itemKind;
      return outputPayloadSemanticKind(event, ignoredOutputItemId);
    }

    bool isNonNegativeSafeInteger(const json::value* value) {
      return value && rt::isNumber(*value) && rt::Number::isSafeInteger(*value) && *rt::getNumber(*value) >= 0;
    }

    const json::object* reasoningItemOf(const ResponsesStreamEvent& event) {
      if (event.type != "response.output_item.added" && event.type != "response.output_item.done") return nullptr;
      const json::value* item = rt::get(event.value, "item");
      if (!utils::isRecord(item) || item->is_array() || !equalsString(rt::get(*item, "type"), "reasoning")) return nullptr;
      return &item->get_object();
    }

    std::optional<std::string> reasoningLifecycleProgressKey(const ResponsesStreamEvent& event) {
      if (isReasoningSummaryPartProgressType(event.type)) {
        return event.type + ":" + utils::getString(rt::get(event.value, "item_id")).value_or("") + ":" + rt::toString(rt::get(event.value, "summary_index"));
      }
      if (const json::object* item = reasoningItemOf(event)) {
        const std::optional<std::string> itemId = trimmedString(rt::get(*item, "id"));
        return rt::truthy(itemId) ? std::optional<std::string>(event.type + ":" + *itemId) : std::nullopt;
      }
      return std::nullopt;
    }
  }

  std::optional<ResponsesSemanticKind> responsesEventSemanticKind(const ResponsesStreamEvent& event) {
    return responsesEventSemanticKindWithIgnoredOutputItem(event, std::nullopt);
  }

  bool responsesPayloadHasAnswerBearingOutput(const json::object& payload) { return semanticKindFromOutput(rt::get(payload, "output")).has_value(); }

  bool responsesEventReportsProgress(const ResponsesStreamEvent& event) {
    const std::optional<std::string_view> textField = reasoningTextProgressField(event.type);
    if (textField) {
      const json::value* value = rt::get(event.value, *textField);
      const json::value* index = rt::startsWith(event.type, "response.reasoning_summary_") ? rt::get(event.value, "summary_index") : rt::get(event.value, "content_index");
      return nonEmptyString(value) && isNonNegativeSafeInteger(index);
    }
    if (isReasoningSummaryPartProgressType(event.type)) return isNonNegativeSafeInteger(rt::get(event.value, "summary_index"));
    if (const json::object* item = reasoningItemOf(event)) {
      const std::optional<std::string> itemId = trimmedString(rt::get(*item, "id"));
      const json::value* summary = rt::get(*item, "summary");
      const json::value* content = rt::get(*item, "content");
      return rt::truthy(itemId) && ((summary && summary->is_array()) || (content && content->is_array()));
    }
    return false;
  }

  double appendResponsesPrecommitEvent(std::vector<ResponsesStreamEvent>& buffered, const ResponsesStreamEvent& event, double bufferedChars) {
    const double nextChars = bufferedChars + static_cast<double>(rt::length(event.raw));
    if (static_cast<double>(buffered.size()) >= MAX_RESPONSES_PRECOMMIT_EVENTS || nextChars > MAX_RESPONSES_PRECOMMIT_CHARS) {
      throw ResponsesStreamError("Upstream Responses precommit buffer exceeded its limit.", {.cause = nullptr, .kind = "event_too_large"});
    }
    buffered.push_back(event);
    return nextChars;
  }

  namespace {
    /** Reads the next upstream event, normalizing iterator exhaustion to `null`. */
    rt::Task<std::optional<ResponsesStreamEvent>> nextUpstreamEvent(ResponsesStreamIterator iterator) {
      rt::IteratorResult<ResponsesStreamEvent> next = co_await iterator.next();
      if (next.done) co_return std::nullopt;
      co_return std::move(next.value);
    }

    /**
     * Reports lifecycle progress at most once per key. Returns whether the caller must
     * release the stream now, which is only requested while `releaseOnProgress` is set.
     */
    bool reportProgressOnce(const ResponsesStreamEvent& event, rt::OrderedSet<std::string>& reportedLifecycleProgress, const PrepareResponsesStreamForCommitOptions& options) {
      if (!responsesEventReportsProgress(event)) return false;
      const std::optional<std::string> lifecycleKey = reasoningLifecycleProgressKey(event);
      if (lifecycleKey && reportedLifecycleProgress.has(*lifecycleKey)) return false;
      if (lifecycleKey) reportedLifecycleProgress.add(*lifecycleKey);
      if (options.onProgress) options.onProgress(event);
      return options.releaseOnProgress.value_or(false);
    }

    // `await iterator.return(reason).catch(() => {})`.
    rt::Task<void> returnQuietly(ResponsesStreamIterator iterator) {
      try {
        co_await iterator.return_();
      } catch (...) {
      }
      // The settled `.catch()` promise is awaited one turn after it.
      co_await rt::microtask();
    }
  }

  rt::Task<PreparedResponsesStream> prepareResponsesStreamForCommit(ResponsesStreamIterator iterator, PrepareResponsesStreamForCommitOptions options) {
    std::vector<ResponsesStreamEvent> buffered;
    rt::OrderedSet<std::string> reportedLifecycleProgress;
    double bufferedChars = 0;
    rt::Reason failure;
    try {
      for (;;) {
        std::optional<ResponsesStreamEvent> event = co_await nextUpstreamEvent(iterator);
        if (!event) throw ResponsesStreamError("Upstream Responses stream ended before semantic output.", {.cause = nullptr, .kind = "premature_eof"});
        bufferedChars = appendResponsesPrecommitEvent(buffered, *event, bufferedChars);
        if (options.onEvent) options.onEvent(*event);
        if (reportProgressOnce(*event, reportedLifecycleProgress, options)) {
          PreparedResponsesStream released{.iterator = iterator, .buffered = std::move(buffered), .bufferedChars = bufferedChars, .semantic = std::nullopt,
                                           .semanticKind = std::nullopt, .terminal = std::nullopt};
          co_return released;
        }
        const std::optional<ResponsesSemanticKind> semanticKind = responsesEventSemanticKind(*event);
        if (semanticKind) {
          std::optional<ResponsesStreamEvent> terminal = event->terminal ? event : std::nullopt;
          PreparedResponsesStream semantic{.iterator = iterator, .buffered = std::move(buffered), .bufferedChars = bufferedChars, .semantic = event,
                                           .semanticKind = semanticKind, .terminal = std::move(terminal)};
          co_return semantic;
        }
        if (event->terminal) {
          PreparedResponsesStream terminal{.iterator = iterator, .buffered = std::move(buffered), .bufferedChars = bufferedChars, .semantic = std::nullopt,
                                           .semanticKind = std::nullopt, .terminal = event};
          co_return terminal;
        }
      }
    } catch (...) {
      failure = std::current_exception();
    }
    co_await returnQuietly(iterator);
    std::rethrow_exception(failure);
  }

  rt::Task<PreparedResponsesStream> prepareResponsesStreamForCommit(ResponsesStreamIterator iterator) {
    PrepareResponsesStreamForCommitOptions options;
    return prepareResponsesStreamForCommit(std::move(iterator), std::move(options));
  }

  namespace {
    std::string sseRaw(const json::object& value) {
      return "event: " + utils::getString(rt::get(value, "type")).value_or("message") + "\ndata: " + rt::jsonStringify(json::value(value)) + "\n\n";
    }

    json::value incrementOutputIndex(const json::value& value) {
      return isNonNegativeSafeInteger(&value) ? rt::makeNumber(*rt::getNumber(value) + 1) : value;
    }

    json::object withResponseOutputPrefix(const json::object& response, const json::object& warningItem) {
      json::array output;
      output.push_back(warningItem);
      if (const json::array* existing = rt::getArray(response, "output"))
        for (const json::value& item : *existing) output.push_back(item);
      json::object prefixed = rt::objectSpread(response);
      rt::objectDefine(prefixed, "output", std::move(output));
      return prefixed;
    }
  }

  ResponsesStreamEvent responseEventFromValue(json::object value) {
    const std::optional<std::string> type = trimmedString(rt::get(value, "type"));
    if (!rt::truthy(type)) throw ResponsesStreamError("Responses event rewrite omitted type.", {.cause = nullptr, .kind = "malformed_event"});
    std::string raw = sseRaw(value);
    const bool terminal = RESPONSES_TERMINAL_EVENT_TYPES.has(*type);
    return ResponsesStreamEvent{.raw = std::move(raw), .value = std::move(value), .type = *type, .terminal = terminal};
  }

  ResponsesStreamEvent rewriteResponsesEventForWarning(const ResponsesStreamEvent& event, const json::object& warningItem, double sequenceNumber) {
    json::object value = rt::objectSpread(event.value);
    rt::objectDefine(value, "sequence_number", rt::makeNumber(sequenceNumber));
    if (rt::hasOwn(value, "output_index")) rt::objectSet(value, "output_index", incrementOutputIndex(*rt::get(value, "output_index")));
    const json::value* response = rt::get(value, "response");
    if ((event.type == "response.completed" || event.type == "response.failed" || event.type == "response.incomplete") && utils::isRecord(response) &&
        !response->is_array()) {
      json::object prefixed = withResponseOutputPrefix(response->get_object(), warningItem);
      rt::objectSet(value, "response", std::move(prefixed));
    }
    return responseEventFromValue(std::move(value));
  }

  ResponsesStreamEvent rewriteResponsesEventSequence(const ResponsesStreamEvent& event, double sequenceNumber) {
    json::object value = rt::objectSpread(event.value);
    rt::objectDefine(value, "sequence_number", rt::makeNumber(sequenceNumber));
    return responseEventFromValue(std::move(value));
  }

  std::string failoverWarningText(std::string_view actualModel) {
    return "\xE2\x9A\xA0 Failover active: this response is from `removed_provider:" + std::string(actualModel) + "` because the Codex upstream was unavailable.";
  }

  namespace {
    constexpr std::string_view FAILOVER_WARNING_ID_PREFIX = "msg_failover_";
    constexpr std::string_view FAILOVER_WARNING_PREFIX = "\xE2\x9A\xA0 Failover active: this response is from `removed_provider:";
    constexpr std::string_view FAILOVER_WARNING_SUFFIX = "` because the Codex upstream was unavailable.";

    std::string withoutDashes(std::string value) {
      std::string out;
      for (char character : value)
        if (character != '-') out += character;
      return out;
    }
  }

  bool isGatewayFailoverWarningItem(const json::value& value) {
    if (!utils::isRecord(value) || value.is_array() || !equalsString(rt::get(value, "type"), "message") || !equalsString(rt::get(value, "role"), "assistant")) {
      return false;
    }
    const std::optional<std::string> id = utils::getString(rt::get(value, "id"));
    if (!id || !rt::startsWith(*id, FAILOVER_WARNING_ID_PREFIX)) return false;
    const json::value* content = rt::get(value, "content");
    if (!content || !content->is_array() || content->get_array().size() != 1) return false;
    const json::value& first = content->get_array()[0];
    if (!utils::isRecord(first) || first.is_array() || !equalsString(rt::get(first, "type"), "output_text")) return false;
    const std::optional<std::string> textValue = utils::getString(rt::get(first, "text"));
    return rt::truthy(textValue) && rt::startsWith(*textValue, FAILOVER_WARNING_PREFIX) && rt::endsWith(*textValue, FAILOVER_WARNING_SUFFIX) &&
           rt::length(*textValue) > rt::length(FAILOVER_WARNING_PREFIX) + rt::length(FAILOVER_WARNING_SUFFIX);
  }

  bool isGatewayFailoverWarningItem(const json::value* value) { return value && isGatewayFailoverWarningItem(*value); }

  FailoverWarningEvents buildFailoverWarningEvents(std::string_view actualModel, std::string_view responseId, double startingSequenceNumber) {
    const std::string itemId = "msg_failover_" + withoutDashes(rt::crypto::randomUUID());
    const std::string warning = failoverWarningText(actualModel);
    json::object content{{"type", "output_text"}, {"text", warning}, {"annotations", json::array()}};
    json::object item{{"id", itemId}, {"type", "message"}, {"status", "completed"}, {"role", "assistant"}, {"content", json::array{content}}};
    json::object addedItem = rt::objectSpread(item);
    rt::objectDefine(addedItem, "status", text("in_progress"));
    rt::objectDefine(addedItem, "content", json::array());
    std::vector<json::object> values{
      json::object{{"type", "response.output_item.added"}, {"response_id", responseId}, {"output_index", 0}, {"item", addedItem}},
      json::object{{"type", "response.content_part.added"},
                   {"response_id", responseId},
                   {"item_id", itemId},
                   {"output_index", 0},
                   {"content_index", 0},
                   {"part", json::object{{"type", "output_text"}, {"text", ""}, {"annotations", json::array()}}}},
      json::object{{"type", "response.output_text.delta"},
                   {"response_id", responseId},
                   {"item_id", itemId},
                   {"output_index", 0},
                   {"content_index", 0},
                   {"delta", warning}},
      json::object{{"type", "response.output_text.done"},
                   {"response_id", responseId},
                   {"item_id", itemId},
                   {"output_index", 0},
                   {"content_index", 0},
                   {"text", warning}},
      json::object{{"type", "response.content_part.done"},
                   {"response_id", responseId},
                   {"item_id", itemId},
                   {"output_index", 0},
                   {"content_index", 0},
                   {"part", content}},
      json::object{{"type", "response.output_item.done"}, {"response_id", responseId}, {"output_index", 0}, {"item", item}},
    };
    FailoverWarningEvents warningEvents{.item = item, .events = {}};
    for (std::size_t index = 0; index < values.size(); ++index) {
      json::object value = rt::objectSpread(values[index]);
      rt::objectDefine(value, "sequence_number", rt::makeNumber(startingSequenceNumber + static_cast<double>(index)));
      warningEvents.events.push_back(responseEventFromValue(std::move(value)));
    }
    return warningEvents;
  }

  namespace {
    std::vector<std::string> idsFromEvent(const ResponsesStreamEvent& event) {
      std::vector<std::string> ids;
      const std::optional<std::string> direct = trimmedString(rt::get(event.value, "response_id"));
      if (rt::truthy(direct)) ids.push_back(*direct);
      const json::value* response = rt::get(event.value, "response");
      if (utils::isRecord(response) && !response->is_array()) {
        const std::optional<std::string> nested = trimmedString(rt::get(*response, "id"));
        if (rt::truthy(nested)) ids.push_back(*nested);
      }
      return ids;
    }
  }

  std::optional<std::string> responseIdFromEvents(const std::vector<ResponsesStreamEvent>& events) {
    std::optional<std::string> responseId;
    for (const ResponsesStreamEvent& event : events) {
      for (const std::string& candidate : idsFromEvent(event)) {
        if (rt::truthy(responseId) && *responseId != candidate) {
          throw ResponsesStreamError("Upstream Responses stream changed response identifiers.", {.cause = nullptr, .kind = "malformed_event"});
        }
        responseId = candidate;
      }
    }
    return responseId;
  }

  namespace {
    rt::WeakSet<ResponsesStreamEvent>& syntheticFailureEvents() {
      static rt::WeakSet<ResponsesStreamEvent> events;
      return events;
    }

    json::object upstreamEndedError() {
      return json::object{{"code", "server_error"}, {"message", "The upstream stream ended unexpectedly."}};
    }
  }

  ResponsesStreamEvent failureEventAfterCommit(std::string_view responseId, double sequenceNumber, const std::vector<json::object>& output,
                                               const json::object& responseTemplate) {
    json::object response = rt::objectSpread(responseTemplate);
    rt::objectDefine(response, "id", text(responseId));
    rt::objectDefine(response, "object", text(utils::getString(rt::get(responseTemplate, "object")).value_or("response")));
    rt::objectDefine(response, "status", text("failed"));
    rt::objectDefine(response, "error", upstreamEndedError());
    json::array items;
    for (const json::object& item : output) items.push_back(item);
    rt::objectDefine(response, "output", std::move(items));
    json::object value;
    rt::objectDefine(value, "type", text("response.failed"));
    rt::objectDefine(value, "sequence_number", rt::makeNumber(sequenceNumber));
    rt::objectDefine(value, "response", std::move(response));
    ResponsesStreamEvent event = responseEventFromValue(std::move(value));
    syntheticFailureEvents().add(event);
    return event;
  }

  namespace {
    ResponsesStreamEvent errorEventAfterCommit(double sequenceNumber) {
      json::object value;
      rt::objectDefine(value, "type", text("error"));
      rt::objectDefine(value, "sequence_number", rt::makeNumber(sequenceNumber));
      rt::objectDefine(value, "code", text("server_error"));
      rt::objectDefine(value, "message", text("The upstream stream ended unexpectedly."));
      rt::objectDefine(value, "param", nullptr);
      ResponsesStreamEvent event = responseEventFromValue(std::move(value));
      syntheticFailureEvents().add(event);
      return event;
    }

    constexpr std::string_view EMPTY_UPSTREAM_COMPLETION_MESSAGE = "Upstream response completed with no translated semantic output.";

    ResponsesStreamEvent emptyCompletionEventAfterCommit(double sequenceNumber) {
      json::object value;
      rt::objectDefine(value, "type", text("error"));
      rt::objectDefine(value, "sequence_number", rt::makeNumber(sequenceNumber));
      rt::objectDefine(value, "code", text("empty_upstream_completion"));
      rt::objectDefine(value, "message", text(EMPTY_UPSTREAM_COMPLETION_MESSAGE));
      rt::objectDefine(value, "param", nullptr);
      ResponsesStreamEvent event = responseEventFromValue(std::move(value));
      syntheticFailureEvents().add(event);
      return event;
    }
  }

  bool isSyntheticResponsesFailureEvent(const ResponsesStreamEvent& event) { return syntheticFailureEvents().has(event); }

  namespace {
    // `void Promise.resolve(callback()).catch(() => {})` inside a try. A function
    // object rather than a function, so argument-dependent lookup never prefers
    // std::invoke (rt::Reason is a std type).
    constexpr struct {
      template <class... A, class... P> void operator()(const MaybeAsyncCallback<A...>& callback, P&&... args) const {
        if (!callback) return;
        try {
          rt::Promise<void> result = callback(std::forward<P>(args)...);
          (void)result.catch_(rt::ignoreRejection);
        } catch (...) {
          // Observability callbacks never own stream delivery.
        }
      }
    } invoke{};

    /** Maps delta/done event types onto the field that carries their text payload. */
    std::optional<std::string_view> textFieldForEventType(std::string_view eventType) {
      if (eventType == "response.output_text.delta" || eventType == "response.refusal.delta") return "delta";
      if (eventType == "response.output_text.done") return "text";
      if (eventType == "response.refusal.done") return "refusal";
      return std::nullopt;
    }

    /** Setup events that may precede the failover warning instead of following it. */
    bool isLeadingSetupEvent(const ResponsesStreamEvent& event) { return event.type == "response.in_progress" || event.type == "response.queued"; }

    /** The trailing lifecycle segment of a hosted-tool event type (`response.<tool>.<lifecycle>`). */
    std::string hostedToolLifecycle(std::string_view eventType) { return rt::slice(eventType, static_cast<double>(rt::lastIndexOf(eventType, ".") + 1)); }

    /** Maps a hosted-tool lifecycle segment onto the recovered output item status. */
    std::string hostedToolItemStatus(std::string_view lifecycle) {
      if (lifecycle == "completed") return "completed";
      if (lifecycle == "failed") return "failed";
      return "in_progress";
    }

    /** The synthetic terminal type reported with a failure event, when it is one. */
    std::optional<std::string> syntheticTerminalTypeOf(const ResponsesStreamEvent& event) {
      return event.type == "response.failed" || event.type == "error" ? std::optional<std::string>(event.type) : std::nullopt;
    }

    /** Builds the error reported when the upstream stream ended without a terminal. */
    rt::Reason prematureEofError() {
      return rt::makeReason(ResponsesStreamError("Responses stream ended without a terminal.", {.cause = nullptr, .kind = "premature_eof"}));
    }

    // `{ ...value }` for a record that may be an array (its index keys become members).
    json::object spreadRecord(const json::value& value) {
      if (value.is_object()) return rt::objectSpread(value.get_object());
      json::object spread;
      if (value.is_array()) {
        const json::array& items = value.get_array();
        for (std::size_t index = 0; index < items.size(); ++index) rt::objectDefine(spread, rt::numberToString(static_cast<double>(index)), items[index]);
      }
      return spread;
    }

    struct WarningPreamble {
      json::object item;
      double sequenceNumber;
    };

    struct AccumulatedText {
      std::string id;
      std::string text;
      bool completed;
      bool refusal;
    };

    struct OwnedState {
      OwnedResponsesStreamOptions options;
      rt::AbortController localAbort;
      std::vector<ResponsesStreamEvent> initial;
      std::optional<json::object> warningItem;
      std::optional<std::string> responseId;
      double sequenceNumber = 0;
      bool closed = false;
      bool terminalEmitted = false;
      bool responseCreatedObserved = false;
      bool semanticCommitmentObserved = false;
      std::deque<ResponsesStreamEvent> queue;
      rt::WeakMap<ResponsesStreamEvent, ResponsesStreamEvent> originalUpstreamEvents;
      json::object responseTemplate;
      std::vector<json::object> completedOutputItems;
      rt::OrderedMap<std::string, std::size_t> completedOutputItemIds;
      rt::OrderedMap<std::string, AccumulatedText> accumulatedText;

      void validate(const ResponsesStreamEvent& event) const {
        if (options.validateEvent) options.validateEvent(event);
      }

      /**
       * Consumes the RemovedProvider preamble: `response.created` is re-sequenced first, the leading
       * setup events follow it, the failover warning is inserted, and everything else is rewritten.
       */
      WarningPreamble queueWarningPreamble(const std::string& preambleResponseId, const std::string& warningModel, double startingSequenceNumber) {
        double sequence = startingSequenceNumber;
        std::size_t createdIndex = initial.size();
        for (std::size_t index = 0; index < initial.size(); ++index) {
          if (initial[index].type == "response.created") {
            createdIndex = index;
            break;
          }
        }
        if (createdIndex == initial.size()) {
          throw ResponsesStreamError("RemovedProvider stream omitted response.created.", {.cause = nullptr, .kind = "malformed_event"});
        }
        const ResponsesStreamEvent created = initial[createdIndex];
        initial.erase(initial.begin() + static_cast<std::ptrdiff_t>(createdIndex));
        validate(created);
        const ResponsesStreamEvent rewrittenCreated = rewriteResponsesEventSequence(created, sequence++);
        originalUpstreamEvents.set(rewrittenCreated, created);
        queue.push_back(rewrittenCreated);

        std::size_t leadingCount = initial.size();
        for (std::size_t index = 0; index < initial.size(); ++index) {
          if (!isLeadingSetupEvent(initial[index])) {
            leadingCount = index;
            break;
          }
        }
        const std::vector<ResponsesStreamEvent> leadingSetup(initial.begin(), initial.begin() + static_cast<std::ptrdiff_t>(leadingCount));
        initial.erase(initial.begin(), initial.begin() + static_cast<std::ptrdiff_t>(leadingCount));
        for (const ResponsesStreamEvent& event : leadingSetup) validate(event);
        for (const ResponsesStreamEvent& event : leadingSetup) {
          const ResponsesStreamEvent rewritten = rewriteResponsesEventSequence(event, sequence++);
          originalUpstreamEvents.set(rewritten, event);
          queue.push_back(rewritten);
        }

        FailoverWarningEvents warning = buildFailoverWarningEvents(warningModel, preambleResponseId, sequence);
        for (const ResponsesStreamEvent& event : warning.events) queue.push_back(event);
        sequence += static_cast<double>(warning.events.size());
        for (const ResponsesStreamEvent& event : initial) validate(event);
        for (const ResponsesStreamEvent& event : initial) {
          const ResponsesStreamEvent rewritten = rewriteResponsesEventForWarning(event, warning.item, sequence++);
          originalUpstreamEvents.set(rewritten, event);
          queue.push_back(rewritten);
        }
        return WarningPreamble{.item = std::move(warning.item), .sequenceNumber = sequence};
      }

      std::optional<std::string> warningItemId() const {
        if (!warningItem) return std::nullopt;
        return trimmedString(rt::get(*warningItem, "id"));
      }

      std::optional<std::string> eventItemId(const ResponsesStreamEvent& event) const {
        const std::optional<std::string> direct = trimmedString(rt::get(event.value, "item_id"));
        if (rt::truthy(direct)) return direct;
        const json::value* item = rt::get(event.value, "item");
        if (utils::isRecord(item) && !item->is_array()) return trimmedString(rt::get(*item, "id"));
        return std::nullopt;
      }

      bool isWarningEvent(const ResponsesStreamEvent& event) const {
        const std::optional<std::string> warningId = warningItemId();
        return warningId.has_value() && eventItemId(event) == warningId;
      }

      static bool itemHasOutputText(const json::object& item) {
        const json::value* content = rt::get(item, "content");
        if (!content || !content->is_array()) return false;
        for (const json::value& part : content->get_array()) {
          const json::value* type = rt::get(part, "type");
          if (utils::isRecord(part) && !part.is_array() && (equalsString(type, "output_text") || equalsString(type, "text")) && nonEmptyString(rt::get(part, "text")))
            return true;
        }
        return false;
      }

      void rememberText(const ResponsesStreamEvent& event) {
        if (isWarningEvent(event)) return;
        const bool refusal = isRefusalType(event.type);
        const std::optional<std::string_view> textField = textFieldForEventType(event.type);
        const std::optional<std::string> eventText = textField ? utils::getString(rt::get(event.value, *textField)) : std::nullopt;
        if (!rt::truthy(eventText)) return;
        const std::optional<std::string> itemId = eventItemId(event);
        const std::string id = itemId ? *itemId
                                      : "msg_recovered_" + utils::getString(rt::get(event.value, "output_index"))
                                                               .value_or(rt::numberToString(static_cast<double>(accumulatedText.size())));
        const AccumulatedText* current = accumulatedText.get(id);
        if (event.type == "response.output_text.done" || event.type == "response.refusal.done") {
          if (!current || rt::startsWith(*eventText, current->text)) accumulatedText.set(id, AccumulatedText{id, *eventText, true, refusal});
          return;
        }
        AccumulatedText next{id, rt::concat(current ? current->text : std::string(), *eventText), current ? current->completed : false, refusal};
        accumulatedText.set(id, std::move(next));
      }

      void observeResponseIdentity(const ResponsesStreamEvent& event) {
        const std::optional<std::string> candidateResponseId = responseIdFromEvents({event});
        if (rt::truthy(candidateResponseId) && rt::truthy(responseId) && *candidateResponseId != *responseId) {
          throw ResponsesStreamError("Upstream Responses stream changed response identifiers.", {.cause = nullptr, .kind = "malformed_event"});
        }
        if (!responseId) responseId = candidateResponseId;
        const json::value* valueResponse = rt::get(event.value, "response");
        if (utils::isRecord(valueResponse) && !valueResponse->is_array()) responseTemplate = rt::objectSpread(responseTemplate, valueResponse->get_object());
      }

      static const json::value* compatibilityOutputOf(const ResponsesStreamEvent& event) {
        if (event.type != "response.output") return nullptr;
        const json::value* explicitOutput = rt::get(event.value, "output");
        if (explicitOutput && !explicitOutput->is_null()) return explicitOutput;
        const json::value* valueResponse = rt::get(event.value, "response");
        if (!utils::isRecord(valueResponse)) return nullptr;
        return rt::get(*valueResponse, "output");
      }

      void recordCompatibilityOutput(const json::value* output) {
        if (!output || !output->is_array()) return;
        for (const json::value& outputItem : output->get_array()) {
          if (!utils::isRecord(outputItem)) continue;
          const std::optional<std::string> id = trimmedString(rt::get(outputItem, "id"));
          const std::size_t* existingIndex = rt::truthy(id) ? completedOutputItemIds.get(*id) : nullptr;
          if (existingIndex) {
            completedOutputItems[*existingIndex] = spreadRecord(outputItem);
            continue;
          }
          if (rt::truthy(id)) completedOutputItemIds.set(*id, completedOutputItems.size());
          completedOutputItems.push_back(spreadRecord(outputItem));
        }
      }

      std::optional<json::object> visibleItemFromEvent(const ResponsesStreamEvent& event) const {
        const json::value* valueItem = rt::get(event.value, "item");
        if ((event.type == "response.output_item.added" || event.type == "response.output_item.done") && utils::isRecord(valueItem) && !valueItem->is_array()) {
          json::object item = rt::objectSpread(valueItem->get_object());
          if (event.type == "response.output_item.added") rt::objectSet(item, "status", text("incomplete"));
          return item;
        }
        if (!isHostedToolTerminalEventType(event.type)) return std::nullopt;
        const std::string lifecycle = hostedToolLifecycle(event.type);
        const std::string type = rt::slice(event.type, static_cast<double>(rt::length("response.")), -static_cast<double>(rt::length(lifecycle) + 1));
        const std::optional<std::string> existingId = eventItemId(event);
        const json::object* existing = nullptr;
        if (rt::truthy(existingId)) {
          if (const std::size_t* index = completedOutputItemIds.get(*existingId)) existing = &completedOutputItems[*index];
        }
        json::object item = existing ? rt::objectSpread(*existing) : json::object();
        const std::optional<std::string> itemId = eventItemId(event);
        rt::objectDefine(item, "id", text(itemId ? *itemId : "tool_recovered_" + rt::numberToString(static_cast<double>(completedOutputItems.size()))));
        rt::objectDefine(item, "type", text(type));
        rt::objectDefine(item, "status", text(hostedToolItemStatus(lifecycle)));
        return item;
      }

      void observeVisibleEvent(const ResponsesStreamEvent& event) {
        if (event.type == "response.created") responseCreatedObserved = true;
        if (!isWarningEvent(event) && responsesEventSemanticKindWithIgnoredOutputItem(event, warningItemId())) semanticCommitmentObserved = true;
        observeResponseIdentity(event);
        rememberText(event);
        if (isWarningEvent(event)) return;
        recordCompatibilityOutput(compatibilityOutputOf(event));
        std::optional<json::object> item = visibleItemFromEvent(event);
        if (!item) return;
        const std::optional<std::string> id = trimmedString(rt::get(*item, "id"));
        const std::size_t* existingIndex = rt::truthy(id) ? completedOutputItemIds.get(*id) : nullptr;
        if (existingIndex) {
          completedOutputItems[*existingIndex] = std::move(*item);
          return;
        }
        if (rt::truthy(id)) completedOutputItemIds.set(*id, completedOutputItems.size());
        completedOutputItems.push_back(std::move(*item));
      }

      void mergeCompletedOutputItems(std::vector<json::object>& output, rt::OrderedMap<std::string, std::size_t>& outputById) const {
        for (const json::object& item : completedOutputItems) {
          const std::optional<std::string> id = trimmedString(rt::get(item, "id"));
          if (rt::truthy(id) && outputById.has(*id)) continue;
          if (rt::truthy(id)) outputById.set(*id, output.size());
          output.push_back(rt::objectSpread(item));
        }
      }

      void mergeRecoveredText(std::vector<json::object>& output, rt::OrderedMap<std::string, std::size_t>& outputById) const {
        for (const AccumulatedText& recovered : accumulatedText.values()) {
          const std::size_t* existingIndex = outputById.get(recovered.id);
          const json::object* existingItem = existingIndex ? &output[*existingIndex] : nullptr;
          if (existingItem && itemHasOutputText(*existingItem)) continue;
          json::array content;
          if (recovered.refusal) content.push_back(json::object{{"type", "refusal"}, {"refusal", recovered.text}});
          else content.push_back(json::object{{"type", "output_text"}, {"text", recovered.text}, {"annotations", json::array()}});
          json::object message{{"id", recovered.id},
                               {"type", "message"},
                               {"status", recovered.completed ? "completed" : "incomplete"},
                               {"role", "assistant"},
                               {"content", std::move(content)}};
          if (!existingIndex) {
            outputById.set(recovered.id, output.size());
            output.push_back(std::move(message));
            continue;
          }
          output[*existingIndex] = rt::objectSpread(*existingItem, message);
        }
      }

      std::vector<json::object> failureOutput() const {
        std::vector<json::object> output;
        if (warningItem) output.push_back(rt::objectSpread(*warningItem));
        rt::OrderedMap<std::string, std::size_t> outputById;
        const std::optional<std::string> warningId = warningItemId();
        if (rt::truthy(warningId)) outputById.set(*warningId, 0);
        mergeCompletedOutputItems(output, outputById);
        mergeRecoveredText(output, outputById);
        return output;
      }

      ResponsesStreamEvent syntheticFailure() {
        if (semanticCommitmentObserved) {
          const std::string failedResponseId = responseId ? *responseId : "resp_" + withoutDashes(rt::crypto::randomUUID());
          const double sequence = sequenceNumber++;
          const std::vector<json::object> output = failureOutput();
          return failureEventAfterCommit(failedResponseId, sequence, output, responseTemplate);
        }
        return errorEventAfterCommit(sequenceNumber++);
      }

      static ResponsesStreamFailureKind failureKindFor(const rt::Reason& error) {
        const ResponsesStreamError* streamError = responses_stream::responsesStreamErrorOf(error);
        return streamError ? streamError->kind : ResponsesStreamFailureKind("read_error");
      }

      OwnedResponsesStreamFailureDetails failureDetails(const rt::Reason& error, std::optional<std::string> syntheticTerminalType,
                                                        std::optional<ResponsesStreamEvent> upstreamTerminal = std::nullopt) const {
        return OwnedResponsesStreamFailureDetails{.failureKind = failureKindFor(error),
                                                  .responseCreatedObserved = responseCreatedObserved,
                                                  .semanticCommitmentObserved = semanticCommitmentObserved,
                                                  .syntheticTerminalType = std::move(syntheticTerminalType),
                                                  .upstreamTerminal = std::move(upstreamTerminal)};
      }

      void advanceSequence(const ResponsesStreamEvent& event) {
        const json::value* value = rt::get(event.value, "sequence_number");
        if (value && rt::isNumber(*value) && rt::Number::isSafeInteger(*value) && *rt::getNumber(*value) >= sequenceNumber) sequenceNumber = *rt::getNumber(*value) + 1;
      }

      bool pullAborted() const {
        return (options.downstreamSignal && options.downstreamSignal->aborted()) || (options.signal && options.signal->aborted()) || localAbort.signal().aborted();
      }
    };

    rt::Task<std::optional<ResponsesStreamEvent>> nextVisible(std::shared_ptr<OwnedState> state) {
      if (!state->queue.empty()) {
        ResponsesStreamEvent queued = std::move(state->queue.front());
        state->queue.pop_front();
        co_return queued;
      }
      std::optional<ResponsesStreamEvent> event = co_await nextUpstreamEvent(state->options.iterator);
      if (!event) co_return std::nullopt;
      state->validate(*event);
      const std::optional<std::string> candidate = responseIdFromEvents({*event});
      if (rt::truthy(candidate) && rt::truthy(state->responseId) && *candidate != *state->responseId) {
        throw ResponsesStreamError("Upstream Responses stream changed response identifiers.", {.cause = nullptr, .kind = "malformed_event"});
      }
      if (!state->responseId) state->responseId = candidate;
      if (!state->warningItem) co_return event;
      ResponsesStreamEvent rewritten = rewriteResponsesEventForWarning(*event, *state->warningItem, state->sequenceNumber++);
      state->originalUpstreamEvents.set(rewritten, *event);
      co_return rewritten;
    }

    /** Closes the stream with a synthetic terminal, releases the iterator, then reports the failure. */
    rt::Task<void> emitFailureTerminal(std::shared_ptr<OwnedState> state, Controller controller, ResponsesStreamEvent failure, rt::Reason error,
                                       std::optional<ResponsesStreamEvent> upstreamTerminal) {
      OwnedResponsesStreamFailureDetails details = state->failureDetails(error, syntheticTerminalTypeOf(failure), std::move(upstreamTerminal));
      state->terminalEmitted = true;
      state->closed = true;
      controller.enqueue(rt::encodeUtf8(failure.raw));
      controller.close();
      co_await returnQuietly(state->options.iterator);
      invoke(state->options.onEvent, failure);
      invoke(state->options.onFailure, error, std::move(details));
    }

    /** Delivers one visible event, or the synthetic failure that replaces a missing one. */
    rt::Task<void> deliverNextEvent(std::shared_ptr<OwnedState> state, Controller controller, std::optional<ResponsesStreamEvent> next) {
      if (state->closed) co_return;
      if (!next) {
        const ResponsesStreamEvent failure = state->syntheticFailure();
        OwnedResponsesStreamFailureDetails details = state->failureDetails(prematureEofError(), syntheticTerminalTypeOf(failure));
        state->terminalEmitted = true;
        state->closed = true;
        controller.enqueue(rt::encodeUtf8(failure.raw));
        controller.close();
        invoke(state->options.onEvent, failure);
        invoke(state->options.onFailure, prematureEofError(), std::move(details));
        co_return;
      }
      const ResponsesStreamEvent& event = *next;
      if (state->terminalEmitted) co_return;
      state->observeVisibleEvent(event);
      state->advanceSequence(event);
      if (event.type == "response.completed" && !state->semanticCommitmentObserved) {
        rt::Reason error = rt::makeReason(ResponsesStreamError(std::string(EMPTY_UPSTREAM_COMPLETION_MESSAGE), {.cause = nullptr, .kind = "empty_upstream_completion"}));
        ResponsesStreamEvent failure = emptyCompletionEventAfterCommit(state->sequenceNumber++);
        const ResponsesStreamEvent* original = state->originalUpstreamEvents.find(event);
        std::optional<ResponsesStreamEvent> upstreamTerminal = original ? *original : event;
        auto emitting = emitFailureTerminal(state, controller, std::move(failure), std::move(error), std::move(upstreamTerminal));
        co_await std::move(emitting);
        co_return;
      }
      state->terminalEmitted = event.terminal;
      const ResponsesStreamEvent visibleEvent = state->options.codexContextErrorCompatibility.value_or(false) && event.type == "error"
                                                  ? codex::responses_error::codexContextFailureEvent(event, state->responseId, state->responseTemplate, state->failureOutput())
                                                  : event;
      controller.enqueue(rt::encodeUtf8(visibleEvent.raw));
      invoke(state->options.onEvent, event);
      if (event.terminal) {
        state->closed = true;
        controller.close();
        co_await returnQuietly(state->options.iterator);
      }
    }

    /** Handles a pull failure: a cancelled stream closes quietly, anything else reports a failure. */
    rt::Task<void> handlePullFailure(std::shared_ptr<OwnedState> state, Controller controller, rt::Reason error) {
      if (state->closed) co_return;
      if (state->pullAborted()) {
        state->closed = true;
        controller.close();
        co_await returnQuietly(state->options.iterator);
        invoke(state->options.onFailure, error, state->failureDetails(error, std::nullopt));
        co_return;
      }
      ResponsesStreamEvent failure = state->syntheticFailure();
      std::optional<ResponsesStreamEvent> noUpstreamTerminal;
      auto emitting = emitFailureTerminal(state, controller, std::move(failure), std::move(error), std::move(noUpstreamTerminal));
      co_await std::move(emitting);
    }

    rt::Task<void> ownedPull(std::shared_ptr<OwnedState> state, Controller controller) {
      if (state->closed) co_return;
      rt::Reason failure;
      try {
        std::optional<ResponsesStreamEvent> next = co_await nextVisible(state);
        co_await deliverNextEvent(state, controller, std::move(next));
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) co_await handlePullFailure(state, controller, failure);
    }

    void ownedCancel(const std::shared_ptr<OwnedState>& state, rt::Reason reason) {
      if (state->closed) return;
      state->closed = true;
      state->localAbort.abort(reason);
      if (state->options.abortUpstream) state->options.abortUpstream(reason);
      invoke(state->options.onCancel, reason);
      rt::spawn(state->options.iterator.return_(), rt::ignoreRejection);
    }
  }

  rt::ByteStream createOwnedResponsesStream(OwnedResponsesStreamOptions options) {
    std::vector<ResponsesStreamEvent> initial = options.initial;
    std::optional<std::string> responseId = options.responseId;
    double sequenceNumber = 0;
    if (!options.warning) {
      for (const ResponsesStreamEvent& event : initial) {
        const json::value* value = rt::get(event.value, "sequence_number");
        if (value && rt::isNumber(*value) && rt::Number::isSafeInteger(*value)) sequenceNumber = rt::Math::max(sequenceNumber, *rt::getNumber(*value) + 1);
      }
    }
    auto state = std::make_shared<OwnedState>(OwnedState{.options = std::move(options),
                                                         .localAbort = rt::AbortController(),
                                                         .initial = std::move(initial),
                                                         .warningItem = std::nullopt,
                                                         .responseId = std::move(responseId),
                                                         .sequenceNumber = sequenceNumber,
                                                         .closed = false,
                                                         .terminalEmitted = false,
                                                         .responseCreatedObserved = false,
                                                         .semanticCommitmentObserved = false,
                                                         .queue = {},
                                                         .originalUpstreamEvents = {},
                                                         .responseTemplate = {},
                                                         .completedOutputItems = {},
                                                         .completedOutputItemIds = {},
                                                         .accumulatedText = {}});

    if (state->options.warning) {
      if (!rt::truthy(state->responseId)) {
        throw ResponsesStreamError("RemovedProvider stream omitted a response identifier.", {.cause = nullptr, .kind = "malformed_event"});
      }
      WarningPreamble preamble = state->queueWarningPreamble(*state->responseId, state->options.warning->model, state->sequenceNumber);
      state->warningItem = std::move(preamble.item);
      state->sequenceNumber = preamble.sequenceNumber;
    } else {
      for (const ResponsesStreamEvent& event : state->initial) state->validate(event);
      for (const ResponsesStreamEvent& event : state->initial) state->queue.push_back(event);
    }

    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [state](Controller controller) { return ownedPull(state, std::move(controller)); };
    source.cancel = [state](rt::Reason reason) { ownedCancel(state, std::move(reason)); };
    return rt::ByteStream(std::move(source));
  }
}
