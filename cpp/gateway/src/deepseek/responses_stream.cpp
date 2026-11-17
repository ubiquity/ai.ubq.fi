#include "uos/deepseek/responses_stream.hpp"
#include "uos/utils.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include <memory>

namespace uos::deepseek::responses_stream {
namespace {
  using Events = std::vector<json::object>;
  using namespace responses_payload;
  template<class T> std::optional<std::string> stringField(const T& value, std::string_view key) { return utils::getString(rt::get(value, key)); }
  void append(Events& events, const Events& added) { events.insert(events.end(), added.begin(), added.end()); }
  struct StreamToolCall { double key; std::string id; std::string callId; std::string name; std::string arguments; bool announced; double outputIndex; };
  struct StreamState {
    bool started = false;
    bool completed = false;
    std::string text, reasoning, refusal;
    double messageIndex = -1;
    bool messageOpen = false;
    bool messageDone = false;
    double textPartIndex = -1;
    double refusalPartIndex = -1;
    /**
       * The output slot the most recently announced reasoning item owns. The
       * provider's normal order opens it at the first reasoning delta, before any
       * later item advances the index; the item id is derived from that slot
       * (`rs_${responseId}_${reasoningIndex}`), matching the buffered transport's
       * first-choice name.
       */
    double reasoningIndex = -1;
    /**
       * True while an announced reasoning item is still awaiting its done events.
       * The pinned Codex consumer holds a single active item
       * (`lib/codex/codex-rs/core/src/session/turn.rs`), so this item is closed
       * before the next output item is announced.
       */
    bool reasoningOpen = false;
    std::string reasoningSegment;
    /**
       * True when the current reasoning segment arrived after another output item
       * was already announced. Announcing it there would overlap that item for the
       * single-active-item consumer, so it is held and delivered as one closed
       * lifecycle at the terminal instead.
       */
    bool reasoningPending = false;
    rt::OrderedMap<double, std::shared_ptr<StreamToolCall>> toolCalls;
    double nextOutputIndex = 0;
    json::array output;
    std::optional<json::object> usage;
    std::optional<json::value> finishReason;
  };
  void outputSet(StreamState& state, double index, const json::object& item) {
    const auto slot = static_cast<std::size_t>(index);
    while (state.output.size() <= slot) state.output.push_back(nullptr);
    state.output[slot] = item;
  }
  /**
   * Merges one tool-call delta into the accumulated call for its index. The item
   * id is assigned when the call is announced, the point at which its official
   * kind (`fc_` or `ctc_`) is known, so every event for the call names one id.
   */
  std::shared_ptr<StreamToolCall> mergeToolCallDelta(StreamState& state, const std::string& responseId, const json::object& raw, double position) {
    const double key = rt::getNumber(raw, "index").value_or(position);
    auto* current = state.toolCalls.get(key);
    auto existing = current ? *current : std::make_shared<StreamToolCall>(StreamToolCall{key, "", stringField(raw, "id").value_or(responseId + "_call_" + rt::numberToString(key)), "", "", false, -1});
    if (auto id = stringField(raw, "id"); id && !id->empty()) existing->callId = *id;
    if (const auto* fn = rt::getObject(raw, "function")) {
      if (auto name = stringField(*fn, "name"); name && !name->empty()) existing->name = *name;
      if (auto args = stringField(*fn, "arguments")) rt::append(existing->arguments, *args);
    }
    state.toolCalls.set(key, existing);
    return existing;
  }
  struct Translator {
    std::string requestedModel, responseId;
    DeepSeekResponsesEcho echo;
    double createdAtSeconds;
    rt::OrderedMap<std::string, responses::OriginalToolName> toolNames;
    rt::OrderedSet<std::string> customToolNames;
    responses::ChatOnlyResponsesProfile profile;
    StreamState state;
    std::string messageId() const { return "msg_" + responseId + "_0"; }
    std::string reasoningItemId() const { return "rs_" + responseId + "_" + rt::numberToString(state.reasoningIndex); }
    /**
       * Closes the open reasoning item at the index it was announced at, using the
       * text accumulated for that item alone. It is called before the next output
       * item is announced and again at the terminal, so the pinned Codex consumer's
       * single `active_item` is always the item a done event closes. The terminal
       * item is stored at its own `output_index` and carries the same single
       * `summary_text` part the buffered transport publishes.
       */
    Events closeReasoning() {
      if (!state.reasoningOpen) return {};
      state.reasoningOpen = false;
      /**
         * The text accumulated for the reasoning item that is currently open or
         * waiting for its terminal announcement. `reasoning` keeps the whole stream's
         * text for the first-leg draft.
         */
      const auto text = state.reasoningSegment;
      state.reasoningSegment.clear();
      const auto itemId = reasoningItemId();
      const auto item = reasoningItem(itemId, text);
      outputSet(state, state.reasoningIndex, item);
      return {{{"type", "response.reasoning_summary_text.done"}, {"item_id", itemId}, {"output_index", state.reasoningIndex}, {"summary_index", 0}, {"text", text}},
        {{"type", "response.reasoning_summary_part.done"}, {"item_id", itemId}, {"output_index", state.reasoningIndex}, {"summary_index", 0}, {"part", json::object{{"type", "summary_text"}, {"text", text}}}},
        {{"type", "response.output_item.done"}, {"output_index", state.reasoningIndex}, {"item", item}}};
    }
    Events startEvents() {
      if (state.started) return {};
      state.started = true;
      const auto created = responsesEnvelope(responseId, requestedModel, createdAtSeconds, "in_progress", echo);
      return {{{"type", "response.created"}, {"response", created}}, {{"type", "response.in_progress"}, {"response", created}}};
    }
    Events ensureMessageItem() {
      if (state.messageOpen) return {};
      auto events = closeReasoning();
      state.messageOpen = true;
      state.messageIndex = state.nextOutputIndex++;
      events.push_back({{"type", "response.output_item.added"}, {"output_index", state.messageIndex},
        {"item", json::object{{"id", messageId()}, {"type", "message"}, {"status", "in_progress"}, {"role", "assistant"}, {"content", json::array{}}}}});
      return events;
    }
    /**
       * Announces the reasoning item and its single summary part at the index the
       * item owns, before any later item can advance `nextOutputIndex`.
       *
       * The announcement is the contract the official client accumulates on:
       * `response.output_item.added` appends its item to the client's ordered output
       * and every later event reads that output at the `output_index` it names. A
       * slot reserved without this event (the previous behavior) made the next item
       * the client's first accumulated item while its content events still named a
       * later index, so the client failed the whole stream with
       * `missing output at index <n>`. The item carries the official reasoning shape
       * the buffered transport already publishes: one `summary_text` part whose text
       * is the provider's own `reasoning_content`.
       */
    Events ensureReasoningItem() {
      if (state.reasoningOpen) return {};
      state.reasoningOpen = true;
      state.reasoningPending = false;
      state.reasoningIndex = state.nextOutputIndex++;
      return {{{"type", "response.output_item.added"}, {"output_index", state.reasoningIndex},
        {"item", json::object{{"id", reasoningItemId()}, {"type", "reasoning"}, {"status", "in_progress"}, {"summary", json::array{}}}}},
        {{"type", "response.reasoning_summary_part.added"}, {"item_id", reasoningItemId()}, {"output_index", state.reasoningIndex}, {"summary_index", 0}, {"part", json::object{{"type", "summary_text"}, {"text", ""}}}}};
    }
    /** One summary-text delta for the reasoning item that is currently open. */
    json::object reasoningDeltaEvent(const std::string& text) const {
      return {{"type", "response.reasoning_summary_text.delta"}, {"item_id", reasoningItemId()}, {"output_index", state.reasoningIndex}, {"summary_index", 0}, {"delta", text}};
    }
    /**
       * Accumulates one provider reasoning delta.
       *
       * The provider's normal order is reasoning before any answer item, and that
       * first item is streamed as it arrives. Once another output item has been
       * announced, a new reasoning segment cannot be announced without overlapping
       * that item for the single-active-item Codex consumer, so it is held and
       * delivered as one closed lifecycle at the terminal. No event ever targets a
       * reasoning item after it closes; a later segment becomes its own item.
       */
    Events applyReasoningDelta(const std::string& text) {
      rt::append(state.reasoning, text);
      if (state.reasoningOpen) { rt::append(state.reasoningSegment, text); return {reasoningDeltaEvent(text)}; }
      if (state.nextOutputIndex == 0) { auto events = ensureReasoningItem(); rt::append(state.reasoningSegment, text); events.push_back(reasoningDeltaEvent(text)); return events; }
      state.reasoningPending = true; rt::append(state.reasoningSegment, text); return {};
    }
    /** Content parts are appended in arrival order, so their index is their position. */
    double nextContentIndex() const { return (state.textPartIndex >= 0 ? 1 : 0) + (state.refusalPartIndex >= 0 ? 1 : 0); }
    Events addContentPart(const json::object& part, const std::string& kind) {
      const auto index = nextContentIndex();
      if (kind == "text") state.textPartIndex = index; else state.refusalPartIndex = index;
      return {{{"type", "response.content_part.added"}, {"item_id", messageId()}, {"output_index", state.messageIndex}, {"content_index", index}, {"part", part}}};
    }
    Events announceTextPart() {
      auto events = ensureMessageItem();
      if (state.textPartIndex < 0) append(events, addContentPart(json::object{{"type", "output_text"}, {"text", ""}, {"annotations", json::array{}}}, "text"));
      return events;
    }
    /**
       * The terminal object for this stream, plus the terminal kind telemetry and
       * the caller report. Both come from the one shared DeepSeek reason mapping,
       * so the emitted event and its recorded classification cannot disagree.
       *
       * Scope note: this maps the provider's own reason vocabulary only. Whether an
       * answer-less completion is a valid completion is the provider-agnostic
       * completion-validity question, answered once by
       * `isAnswerBearingCompletion` and applied by the route caller.
       */
    DeepSeekTerminalEnvelope terminalEnvelope() const { return deepSeekTerminalEnvelope(responseId, requestedModel, createdAtSeconds, "completed", echo, state.finishReason ? &*state.finishReason : nullptr, profile); }
    bool isCustomCall(const StreamToolCall& call) const { return customToolNames.has(call.name); }
    Events announceToolCall(StreamToolCall& call) {
      auto events = closeReasoning();
      call.announced = true; call.outputIndex = state.nextOutputIndex++;
      const bool custom = isCustomCall(call);
      const auto original = responses::originalTool(call.name, toolNames);
      call.id = std::string(custom ? "ctc_" : "fc_") + responseId + "_" + rt::numberToString(call.key);
      json::object item{{"id", call.id}, {"type", custom ? "custom_tool_call" : "function_call"}, {"status", "in_progress"}, {"call_id", call.callId}, {"name", original.name}};
      if (original.namespace_ && !original.namespace_->empty()) item["namespace"] = *original.namespace_;
      item[custom ? "input" : "arguments"] = "";
      events.push_back({{"type", "response.output_item.added"}, {"output_index", call.outputIndex}, {"item", std::move(item)}});
      return events;
    }
    Events applyTextDelta(const json::object& delta) {
      Events events;
      if (auto text = stringField(delta, "reasoning_content"); text && !text->empty()) append(events, applyReasoningDelta(*text));
      const auto text = stringField(delta, "content");
      if (!text || text->empty()) return events;
      rt::append(state.text, *text);
      append(events, announceTextPart());
      events.push_back({{"type", "response.output_text.delta"}, {"item_id", messageId()}, {"output_index", state.messageIndex}, {"content_index", state.textPartIndex}, {"delta", *text}, {"logprobs", json::array{}}});
      return events;
    }
    /**
       * A refusal delta is answer-bearing payload, so it gets its own content part.
       * The event carries the fields this translator emits on every other streamed
       * event; `sequence_number` is a route-level concern (see the module handback),
       * not something only this event is missing.
       */
    Events applyRefusalDelta(const json::object& delta) {
      const auto refusal = stringField(delta, "refusal");
      if (!refusal || refusal->empty()) return {};
      rt::append(state.refusal, *refusal);
      auto events = ensureMessageItem();
      if (state.refusalPartIndex < 0) append(events, addContentPart(json::object{{"type", "refusal"}, {"refusal", ""}}, "refusal"));
      events.push_back({{"type", "response.refusal.delta"}, {"item_id", messageId()}, {"output_index", state.messageIndex}, {"content_index", state.refusalPartIndex}, {"delta", *refusal}});
      return events;
    }
    Events applyToolCallDeltas(const json::object& delta) {
      Events events;
      const auto* raw = rt::getArray(delta, "tool_calls"); if (!raw) return events;
      for (std::size_t position = 0; position < raw->size(); ++position) {
        const auto& entry = (*raw)[position]; if (!entry.is_object()) continue;
        auto call = mergeToolCallDelta(state, responseId, entry.as_object(), static_cast<double>(position));
        if (!call->announced && !call->name.empty() && !state.reasoningPending) append(events, announceToolCall(*call));
        const auto* fn = rt::getObject(entry, "function");
        if (call->announced && !isCustomCall(*call) && fn) if (auto args = stringField(*fn, "arguments"); args && !args->empty()) events.push_back({{"type", "response.function_call_arguments.delta"}, {"item_id", call->id}, {"output_index", call->outputIndex}, {"delta", *args}});
      }
      return events;
    }
    Events closeMessage() {
      if (!state.messageOpen || state.messageDone) return {};
      state.messageDone = true;
      Events events;
      if (state.textPartIndex >= 0) {
        events.push_back({{"type", "response.output_text.done"}, {"item_id", messageId()}, {"output_index", state.messageIndex}, {"content_index", state.textPartIndex}, {"text", state.text}, {"logprobs", json::array{}}});
        events.push_back({{"type", "response.content_part.done"}, {"item_id", messageId()}, {"output_index", state.messageIndex}, {"content_index", state.textPartIndex}, {"part", json::object{{"type", "output_text"}, {"text", state.text}, {"annotations", json::array{}}}}});
      }
      if (state.refusalPartIndex >= 0) {
        events.push_back({{"type", "response.refusal.done"}, {"item_id", messageId()}, {"output_index", state.messageIndex}, {"content_index", state.refusalPartIndex}, {"refusal", state.refusal}});
        events.push_back({{"type", "response.content_part.done"}, {"item_id", messageId()}, {"output_index", state.messageIndex}, {"content_index", state.refusalPartIndex}, {"part", json::object{{"type", "refusal"}, {"refusal", state.refusal}}}});
      }
      json::array content;
      content.resize(static_cast<std::size_t>(nextContentIndex()));
      if (state.textPartIndex >= 0) content[static_cast<std::size_t>(state.textPartIndex)] = json::object{{"type", "output_text"}, {"text", state.text}, {"annotations", json::array{}}};
      if (state.refusalPartIndex >= 0) content[static_cast<std::size_t>(state.refusalPartIndex)] = json::object{{"type", "refusal"}, {"refusal", state.refusal}};
      const json::object item{{"id", messageId()}, {"type", "message"}, {"status", "completed"}, {"role", "assistant"}, {"content", std::move(content)}};
      outputSet(state, state.messageIndex, item);
      events.push_back({{"type", "response.output_item.done"}, {"output_index", state.messageIndex}, {"item", item}});
      return events;
    }
    /**
       * Delivers a reasoning segment that could not be announced when it arrived
       * (it followed another output item). The item is opened, filled with one
       * summary-text delta, and closed in one batch after the answer items, so the
       * single-active-item consumer never sees it overlap another item and no event
       * targets an item that already completed.
       */
    Events flushPendingReasoning() {
      if (state.reasoningOpen || !state.reasoningPending) return {};
      auto events = ensureReasoningItem();
      events.push_back(reasoningDeltaEvent(state.reasoningSegment)); append(events, closeReasoning()); return events;
    }
    /**
       * Closes every accumulated tool call with the status its decided terminal
       * allows. Only a normal completion may advertise a call as `completed`; a
       * truncation or interruption closes it with the official `incomplete` status,
       * so a status-aware consumer never receives a partial call as executable.
       */
    Events closeToolCalls(const std::string& status) {
      Events events;
      std::vector<std::pair<double, std::shared_ptr<StreamToolCall>>> ordered;
      for (const auto& entry : state.toolCalls) ordered.push_back(entry);
      rt::sort(ordered, [](const auto& a, const auto& b) { return a.first - b.first; });
      for (const auto& [key, call] : ordered) {
        static_cast<void>(key);
        if (!call->announced) { if (call->name.empty()) continue; append(events, announceToolCall(*call)); }
        if (isCustomCall(*call)) {
          const auto input = freeformInputFromArguments(call->arguments);
          auto item = customToolCallItem(call->id, call->callId, responses::originalToolName(call->name, toolNames), input); item["status"] = status;
          if (!input.empty()) events.push_back({{"type", "response.custom_tool_call_input.delta"}, {"item_id", call->id}, {"output_index", call->outputIndex}, {"call_id", call->callId}, {"delta", input}});
          outputSet(state, call->outputIndex, item);
          events.push_back({{"type", "response.output_item.done"}, {"output_index", call->outputIndex}, {"item", item}}); continue;
        }
        events.push_back({{"type", "response.function_call_arguments.done"}, {"item_id", call->id}, {"output_index", call->outputIndex}, {"arguments", call->arguments}});
        const auto original = responses::originalTool(call->name, toolNames);
        auto item = functionCallItem(call->id, call->callId, original.name, call->arguments, original.namespace_); item["status"] = status;
        outputSet(state, call->outputIndex, item);
        events.push_back({{"type", "response.output_item.done"}, {"output_index", call->outputIndex}, {"item", item}});
      }
      return events;
    }
    Events push(const json::object& chunk) {
      auto events = startEvents();
      if (auto usage = toResponsesUsage(rt::get(chunk, "usage"), profile)) state.usage = usage;
      if (const auto* choices = rt::getArray(chunk, "choices")) for (const auto& choice : *choices) {
        if (!choice.is_object()) continue;
        if (auto finish = stringField(choice, "finish_reason")) state.finishReason = json::value(*finish);
        const auto* delta = rt::getObject(choice, "delta"); if (!delta) continue;
        append(events, applyTextDelta(*delta)); append(events, applyRefusalDelta(*delta)); append(events, applyToolCallDeltas(*delta));
      }
      return events;
    }
    DeepSeekResponsesAnswerBearingOutput answerBearingOutput() const {
      double count = 0; for (const auto& [key, call] : state.toolCalls) { static_cast<void>(key); if (!call->name.empty()) ++count; }
      return {state.text, state.refusal, count};
    }
    Events finish() {
      if (state.completed) return {};
      state.completed = true;
      auto terminal = terminalEnvelope();
      const auto status = terminal.type == "response.completed" ? "completed" : "incomplete";
      auto events = startEvents(); append(events, closeMessage()); append(events, flushPendingReasoning()); append(events, closeToolCalls(status)); append(events, closeReasoning());
      terminal.response["output"] = state.output;
      terminal.response["usage"] = state.usage ? json::value(*state.usage) : json::value(nullptr);
      events.push_back({{"type", terminal.type}, {"response", std::move(terminal.response)}});
      return events;
    }
  };
}
DeepSeekResponsesTerminalKind deepSeekResponsesTerminalKind(const json::value* finishReason, const responses::ChatOnlyResponsesProfile& profile) {
  /**
     * The provider's own stop reason, captured rather than discarded. The
     * provider's client defers the mapped reason to its terminal sentinel so no
     * chunk follows it and usage always precedes it; this translator's `finish`
     * runs on the same sentinel, so the last non-null value wins.
     */
  const auto disposition = profile.finishDisposition(finishReason);
  if (std::holds_alternative<index::DeepSeekFinishDispositionCompleted>(disposition)) return "completed";
  if (std::holds_alternative<index::DeepSeekFinishDispositionIncomplete>(disposition)) return "incomplete";
  return "failed";
}
DeepSeekResponsesTerminalKind deepSeekResponsesTerminalKind(const json::value& finishReason, const responses::ChatOnlyResponsesProfile& profile) { return deepSeekResponsesTerminalKind(&finishReason, profile); }
/**
 * Accumulates one Chat Completions SSE stream and emits the Responses event
 * sequence under one provider profile. The message item is announced lazily so
 * a tool-only reply never emits an empty text part.
 */
DeepSeekResponsesStreamTranslator createDeepSeekResponsesStreamTranslator(const std::string& requestedModel, const std::string& responseId,
  const DeepSeekResponsesEcho& echo, double createdAtSeconds, const rt::OrderedMap<std::string, responses::OriginalToolName>& toolNames,
  const rt::OrderedSet<std::string>& customToolNames, const responses::ChatOnlyResponsesProfile& profile) {
  auto translator = std::make_shared<Translator>(Translator{requestedModel, responseId, echo, createdAtSeconds, toolNames, customToolNames, profile, StreamState{}});
  return {[translator] { return translator->startEvents(); }, [translator](const json::object& chunk) { return translator->push(chunk); },
    [translator] { return translator->answerBearingOutput(); }, [translator] { return deepSeekResponsesTerminalKind(translator->state.finishReason ? &*translator->state.finishReason : nullptr, translator->profile); },
    [translator]() -> std::optional<std::string> { return translator->state.finishReason && translator->state.finishReason->is_string() ? std::optional(rt::toString(*translator->state.finishReason)) : std::nullopt; },
    [translator] { return translator->finish(); }};
}
/** Responses SSE frames are named events; the client dispatches on `event:`. */
std::string encodeResponsesEvent(const json::object& value) { return "event: " + rt::toString(rt::get(value, "type")) + "\ndata: " + rt::jsonStringify(value) + "\n\n"; }
}
