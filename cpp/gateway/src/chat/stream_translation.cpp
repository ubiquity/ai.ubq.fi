#include "uos/chat/stream_translation.hpp"

#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/utils.hpp"

#include <algorithm>
#include <exception>
#include <utility>

namespace uos::chat::stream_translation {
  namespace telemetry = openai_telemetry;
  using responses_stream::ResponsesStreamError;
  using responses_stream::ResponsesStreamEvent;
  namespace {
    using Parts = rt::OrderedMap<std::string, std::string>;
    template <class T> std::optional<std::string> fieldString(const T& value, std::string_view key) { return utils::getString(rt::get(value, key)); }
    bool responseHasContent(const json::value* output, double startIndex, bool refusal) {
      if (!output || !output->is_array()) return false;
      const auto& items = output->get_array();
      double start = rt::Math::trunc(startIndex);
      if (rt::Number::isNaN(start)) start = 0;
      const double count = static_cast<double>(items.size());
      if (start < 0) start = std::max(count + start, 0.0);
      else start = std::min(start, count);
      for (std::size_t i = static_cast<std::size_t>(start); i < items.size(); ++i) {
        const auto* content = rt::getArray(items[i], "content");
        if (!content) continue;
        for (const auto& part : *content) {
          if (!utils::isRecord(part)) continue;
          if (fieldString(part, "type") == (refusal ? "refusal" : "output_text") &&
              !fieldString(part, refusal ? "refusal" : "text").value_or("").empty()) return true;
        }
      }
      return false;
    }
    std::string chatOutputTextPartIndexText(const json::value* value) {
      return value && (rt::isNumber(*value) || value->is_string()) ? rt::toString(*value) : "0";
    }
    std::optional<std::string> chatOutputTextPartPositionKey(const json::object& event) {
      const auto* index = rt::get(event, "output_index");
      if (!index || (!rt::isNumber(*index) && !index->is_string())) return std::nullopt;
      return "output:" + chatOutputTextPartIndexText(index) + ":" + chatOutputTextPartIndexText(rt::get(event, "content_index"));
    }
    std::optional<std::string> completedPartRepeatsEmittedContent(const Parts& parts, const json::object& event, const std::string& completed) {
      if (completed.empty()) return std::nullopt;
      const auto matches = [&completed](const std::string* emitted) -> std::optional<std::string> {
        if (emitted && (*emitted == completed || rt::startsWith(*emitted, completed))) return *emitted;
        return std::nullopt;
      };
      if (const auto position = chatOutputTextPartPositionKey(event)) {
        if (const auto repeated = matches(parts.get(*position))) return repeated;
      }
      std::vector<std::string> itemParts;
      std::vector<std::string> positionalParts;
      std::vector<std::string> allParts;
      for (const auto& [partKey, value] : parts) {
        allParts.push_back(value);
        if (rt::startsWith(partKey, "item:")) itemParts.push_back(value);
        if (rt::startsWith(partKey, "output:")) positionalParts.push_back(value);
      }
      if (itemParts.size() == 1) {
        if (const auto repeated = matches(&itemParts[0])) return repeated;
      }
      const auto& aggregateParts = positionalParts.empty() ? allParts : positionalParts;
      if (aggregateParts.size() < 2) return std::nullopt;
      const auto aggregate = rt::join(aggregateParts, "");
      return aggregate == completed ? std::optional<std::string>(aggregate) : std::nullopt;
    }
    json::object withAccumulatedContent(const json::object& response, const std::string& content, double ignoredOutputPrefix, bool refusal) {
      const auto* existingOutput = rt::get(response, "output");
      if (content.empty() || responseHasContent(existingOutput, ignoredOutputPrefix, refusal)) return response;
      json::array output = existingOutput && existingOutput->is_array() ? existingOutput->get_array() : json::array();
      json::object part{{"type", refusal ? "refusal" : "output_text"}, {refusal ? "refusal" : "text", content}};
      if (!refusal) part["annotations"] = json::array();
      output.push_back(json::object{{"id", "msg_" + rt::replaceAll(rt::crypto::randomUUID(), "-", "")}, {"type", "message"},
                                    {"status", "completed"}, {"role", "assistant"}, {"content", json::array{part}}});
      auto result = response;
      rt::objectSet(result, "output", output);
      return result;
    }
    struct PreparedState {
      std::string outputText;
      std::string refusal;
      Parts outputTextParts;
      Parts refusalParts;
      ChatFunctionCallAccumulator functionCalls;
      bool completed = false;
    };
    std::optional<std::string> applyPreparedChatEvent(const ResponsesStreamEvent& event, PreparedState& state) {
      const auto& ev = event.value;
      const auto& type = event.type;
      if (type == "response.output_text.delta" || type == "response.refusal.delta") {
        const bool refusal = type == "response.refusal.delta";
        const auto delta = fieldString(ev, "delta");
        if (!delta) return refusal ? "Upstream refusal delta is not a string." : "Upstream output-text delta is not a string.";
        auto& parts = refusal ? state.refusalParts : state.outputTextParts;
        const auto partKey = chatOutputTextPartStorageKey(ev);
        const auto* prior = parts.get(partKey);
        setChatOutputTextPart(parts, ev, rt::concat(prior ? *prior : "", *delta));
        rt::append(refusal ? state.refusal : state.outputText, *delta);
      } else if (type == "response.output_text.done" || type == "response.refusal.done") {
        const bool refusal = type == "response.refusal.done";
        const auto completed = fieldString(ev, refusal ? "refusal" : "text");
        if (!completed) return refusal ? "Upstream completed refusal is not a string." : "Upstream completed output text is not a string.";
        auto& parts = refusal ? state.refusalParts : state.outputTextParts;
        const auto* prior = parts.get(chatOutputTextPartStorageKey(ev));
        const auto emitted = prior ? *prior : "";
        const auto suffix = refusal ? reconcileCompletedRefusal(emitted, *completed) : reconcileCompletedOutputText(emitted, *completed);
        setChatOutputTextPart(parts, ev, rt::concat(emitted, suffix));
        rt::append(refusal ? state.refusal : state.outputText, suffix);
      } else if (type == "response.content_part.done") {
        const auto result = reconcileChatContentPart(state.outputTextParts, state.refusalParts, ev, rt::get(ev, "part"));
        rt::append(state.outputText, result.outputText); rt::append(state.refusal, result.refusal);
      } else if (type == "response.output_item.added") state.functionCalls.add(ev, rt::get(ev, "item"));
      else if (type == "response.function_call_arguments.delta") state.functionCalls.delta(ev);
      else if (type == "response.function_call_arguments.done") state.functionCalls.done(ev);
      else if (type == "response.output_item.done") {
        if (!state.functionCalls.reconcileItem(ev, rt::get(ev, "item"))) {
          const auto result = reconcileChatOutputItemContent(state.outputTextParts, state.refusalParts, ev, rt::get(ev, "item"));
          rt::append(state.outputText, result.outputText); rt::append(state.refusal, result.refusal);
        }
      } else if (type == "response.output" || type == "response.completed") {
        const auto* response = rt::get(ev, "response");
        if (type == "response.completed" && !rt::isRecord(response)) return "Upstream response.completed event is missing its response object.";
        const auto* output = type == "response.completed" ? rt::get(*response, "output") : rt::get(ev, "output");
        if (type == "response.output" && rt::isNullish(output) && response && utils::isRecord(*response)) output = rt::get(*response, "output");
        const auto result = reconcileChatResponseOutputContent(state.outputTextParts, state.refusalParts, ev, output);
        rt::append(state.outputText, result.outputText); rt::append(state.refusal, result.refusal);
        state.functionCalls.reconcileOutput(ev, output);
        if (type == "response.completed") { state.functionCalls.assertFinalized(); state.completed = true; }
      }
      return std::nullopt;
    }
  }

  void recordResponsesTerminal(const ResponsesStreamEvent& event, const telemetry::UsageContext* usageContext) {
    if (!event.terminal) return;
    telemetry::recordResponsesEventTelemetry(usageContext, event);
    telemetry::recordStreamTerminalType(usageContext, event.type);
    const auto* response = rt::get(event.value, "response");
    const auto usage = response && utils::isRecord(*response) ? telemetry::extractUsageTokens(rt::get(*response, "usage")) : std::nullopt;
    if (event.type == "response.completed") (void)telemetry::recordCompletionUsage(usageContext, usage);
    else telemetry::recordTerminalUsage(usageContext, usage, false);
  }
  std::string reconcileCompletedOutputText(const std::string& emittedText, const std::string& completedText) {
    if (completedText.empty() || completedText == emittedText || rt::startsWith(emittedText, completedText)) return "";
    if (rt::startsWith(completedText, emittedText)) return rt::slice(completedText, rt::length(emittedText));
    malformedFunctionCallStream("Upstream response output text conflicts with prior text deltas.");
  }
  std::string reconcileCompletedRefusal(const std::string& emittedRefusal, const std::string& completedRefusal) {
    if (completedRefusal.empty() || completedRefusal == emittedRefusal || rt::startsWith(emittedRefusal, completedRefusal)) return "";
    if (rt::startsWith(completedRefusal, emittedRefusal)) return rt::slice(completedRefusal, rt::length(emittedRefusal));
    malformedFunctionCallStream("Upstream response refusal conflicts with prior refusal deltas.");
  }
  std::string chatOutputTextPartKey(const json::object& event) {
    const auto itemId = rt::trim(fieldString(event, "item_id").value_or(""));
    if (!itemId.empty()) return "item:" + itemId + ":" + chatOutputTextPartIndexText(rt::get(event, "content_index"));
    return "output:" + chatOutputTextPartIndexText(rt::get(event, "output_index")) + ":" + chatOutputTextPartIndexText(rt::get(event, "content_index"));
  }
  std::string chatOutputTextPartStorageKey(const json::object& event) { return chatOutputTextPartPositionKey(event).value_or(chatOutputTextPartKey(event)); }
  void setChatOutputTextPart(Parts& parts, const json::object& event, const std::string& value) {
    parts.set(chatOutputTextPartStorageKey(event), value);
    if (chatOutputTextPartPositionKey(event) && !rt::trim(fieldString(event, "item_id").value_or("")).empty()) parts.set(chatOutputTextPartKey(event), value);
  }
  ReconciledChatContent reconcileChatContentPart(Parts& outputTextParts, Parts& refusalParts, const json::object& event, const json::value* part) {
    if (!rt::isRecord(part)) malformedFunctionCallStream("Upstream completed content part is missing its part object.");
    const auto type = fieldString(*part, "type");
    if (type != "output_text" && type != "text" && type != "refusal") return {"", ""};
    const bool refusal = type == "refusal";
    const auto completed = fieldString(*part, refusal ? "refusal" : "text");
    if (!completed) malformedFunctionCallStream(refusal ? "Upstream completed content part is missing string refusal text." :
                                                       "Upstream completed content part is missing string output text.");
    auto& parts = refusal ? refusalParts : outputTextParts;
    const bool identified = !rt::trim(fieldString(event, "item_id").value_or("")).empty();
    if (!identified) {
      if (const auto repeated = completedPartRepeatsEmittedContent(parts, event, *completed)) { setChatOutputTextPart(parts, event, *repeated); return {"", ""}; }
    }
    const auto* prior = parts.get(chatOutputTextPartStorageKey(event));
    const auto emitted = prior ? *prior : "";
    const auto suffix = refusal ? reconcileCompletedRefusal(emitted, *completed) : reconcileCompletedOutputText(emitted, *completed);
    setChatOutputTextPart(parts, event, rt::concat(emitted, suffix));
    return refusal ? ReconciledChatContent{"", suffix} : ReconciledChatContent{suffix, ""};
  }
  ReconciledChatContent reconcileChatContentPart(Parts& output, Parts& refusal, const json::object& event, const json::value& part) {
    return reconcileChatContentPart(output, refusal, event, &part);
  }
  ReconciledChatContent reconcileChatOutputItemContent(Parts& output, Parts& refusal, const json::object& event, const json::value* item) {
    if (!rt::isRecord(item)) return {"", ""};
    const auto* content = rt::getArray(*item, "content");
    if (!content) return {"", ""};
    ReconciledChatContent result{"", ""};
    for (std::size_t i = 0; i < content->size(); ++i) {
      auto partEvent = event;
      const auto itemId = fieldString(*item, "id");
      if (itemId) rt::objectSet(partEvent, "item_id", json::value(*itemId));
      rt::objectSet(partEvent, "content_index", rt::makeNumber(static_cast<double>(i)));
      const auto part = reconcileChatContentPart(output, refusal, partEvent, (*content)[i]);
      rt::append(result.outputText, part.outputText); rt::append(result.refusal, part.refusal);
    }
    return result;
  }
  ReconciledChatContent reconcileChatOutputItemContent(Parts& output, Parts& refusal, const json::object& event, const json::value& item) {
    return reconcileChatOutputItemContent(output, refusal, event, &item);
  }
  ReconciledChatContent reconcileChatResponseOutputContent(Parts& output, Parts& refusal, const json::object& event, const json::value* items) {
    if (!items || !items->is_array()) return {"", ""};
    ReconciledChatContent result{"", ""};
    for (std::size_t i = 0; i < items->get_array().size(); ++i) {
      auto itemEvent = event; rt::objectSet(itemEvent, "output_index", rt::makeNumber(static_cast<double>(i)));
      const auto part = reconcileChatOutputItemContent(output, refusal, itemEvent, items->get_array()[i]);
      rt::append(result.outputText, part.outputText); rt::append(result.refusal, part.refusal);
    }
    return result;
  }
  ReconciledChatContent reconcileChatResponseOutputContent(Parts& output, Parts& refusal, const json::object& event, const json::value& items) {
    return reconcileChatResponseOutputContent(output, refusal, event, &items);
  }
  json::object withAccumulatedResponseText(const json::object& response, const std::string& text, double ignoredOutputPrefix) {
    return withAccumulatedContent(response, text, ignoredOutputPrefix, false);
  }
  json::object withAccumulatedResponseRefusal(const json::object& response, const std::string& refusal, double ignoredOutputPrefix) {
    return withAccumulatedContent(response, refusal, ignoredOutputPrefix, true);
  }
  json::object withAccumulatedResponseItems(const json::object& response, const std::vector<json::object>& accumulated) {
    if (accumulated.empty()) return response;
    json::array output;
    rt::OrderedSet<std::string> existingIds;
    if (const auto* existing = rt::getArray(response, "output")) {
      for (const auto& item : *existing) if (utils::isRecord(item)) {
        json::object copy;
        if (item.is_object()) copy = item.get_object();
        else for (std::size_t i = 0; i < item.get_array().size(); ++i) rt::objectDefine(copy, rt::numberToString(static_cast<double>(i)), item.get_array()[i]);
        output.push_back(copy);
        if (const auto id = fieldString(item, "id"); id && !id->empty()) existingIds.add(*id);
      }
    }
    for (const auto& item : accumulated) {
      const auto id = fieldString(item, "id");
      if (id && !id->empty() && existingIds.has(*id)) continue;
      output.push_back(item); if (id && !id->empty()) existingIds.add(*id);
    }
    auto result = response; rt::objectSet(result, "output", output); return result;
  }
  [[noreturn]] void malformedFunctionCallStream(std::string message) {
    throw ResponsesStreamError(std::move(message), {.kind = "malformed_event"});
  }
  bool ChatFunctionCallAccumulator::hasCalls() const { return !calls_.empty(); }
  const std::vector<std::shared_ptr<ChatFunctionCall>>& ChatFunctionCallAccumulator::calls() const { return calls_; }
  void ChatFunctionCallAccumulator::assertFinalized() const {
    for (const auto& call : calls_) if (!call->argumentsDone) malformedFunctionCallStream("Upstream function-call stream ended before finalized arguments were received.");
  }
  std::optional<std::string> ChatFunctionCallAccumulator::key(const json::object& event, const json::object* item) {
    auto itemId = fieldString(event, "item_id");
    if (!itemId && item) itemId = fieldString(*item, "id");
    if (itemId && !rt::trim(*itemId).empty()) return "item:" + *itemId;
    const auto index = rt::getNumber(event, "output_index");
    if (index && rt::Number::isInteger(*index) && *index >= 0) return "output:" + rt::numberToString(*index);
    return std::nullopt;
  }
  bool ChatFunctionCallAccumulator::has(const json::object& event, const json::object* item) const { const auto k = key(event, item); return k && byKey_.has(*k); }
  std::shared_ptr<ChatFunctionCall> ChatFunctionCallAccumulator::create(const json::object& event, const json::object& item) {
    if (fieldString(item, "type") != "function_call") malformedFunctionCallStream("Upstream function-call event did not contain a function_call item.");
    const auto k = key(event, &item);
    if (!k) malformedFunctionCallStream("Upstream function-call event omitted item_id and output_index.");
    const auto callId = rt::trim(fieldString(item, "call_id").value_or(""));
    const auto name = rt::trim(fieldString(item, "name").value_or(""));
    const auto argumentsText = rt::get(item, "arguments") ? fieldString(item, "arguments") : std::optional<std::string>("");
    if (callId.empty() || name.empty() || !argumentsText) malformedFunctionCallStream("Upstream function-call item is missing call_id, name, or string arguments.");
    if (const auto* existing = byKey_.get(*k)) {
      if ((*existing)->callId != callId || (*existing)->name != name) malformedFunctionCallStream("Upstream function-call item changed its call_id or name.");
      reconcileArguments(**existing, *argumentsText); return *existing;
    }
    auto call = std::make_shared<ChatFunctionCall>(ChatFunctionCall{*k, static_cast<double>(calls_.size()), callId, name, *argumentsText, false});
    calls_.push_back(call); byKey_.set(*k, call); return call;
  }
  std::string ChatFunctionCallAccumulator::reconcileArguments(ChatFunctionCall& call, const std::string& finalArguments) {
    if (finalArguments == call.arguments) return "";
    if (!rt::startsWith(finalArguments, call.arguments)) malformedFunctionCallStream("Upstream function-call arguments conflict with prior argument deltas.");
    auto suffix = rt::slice(finalArguments, rt::length(call.arguments)); call.arguments = finalArguments; return suffix;
  }
  std::optional<ChatFunctionCallAccumulator::Added> ChatFunctionCallAccumulator::add(const json::object& event, const json::value* item) {
    if (!rt::isRecord(item) || fieldString(*item, "type") != "function_call") return std::nullopt;
    const auto* existing = byKey_.get(key(event, &item->get_object()).value_or(""));
    const std::optional<std::string> prior = existing ? std::optional<std::string>((*existing)->arguments) : std::nullopt;
    const auto call = create(event, item->get_object());
    return Added{call, !prior.has_value(), prior ? rt::slice(call->arguments, rt::length(*prior)) : call->arguments};
  }
  std::optional<ChatFunctionCallAccumulator::Added> ChatFunctionCallAccumulator::add(const json::object& event, const json::value& item) { return add(event, &item); }
  ChatFunctionCallAccumulator::Delta ChatFunctionCallAccumulator::delta(const json::object& event) {
    const auto k = key(event); const auto* found = k ? byKey_.get(*k) : nullptr;
    if (!found) malformedFunctionCallStream("Upstream function-call argument delta has no matching item.");
    const auto call = *found;
    if (call->argumentsDone) malformedFunctionCallStream("Upstream function-call emitted arguments after its completion event.");
    const auto value = fieldString(event, "delta");
    if (!value) malformedFunctionCallStream("Upstream function-call argument delta is not a string.");
    rt::append(call->arguments, *value); return {call, *value};
  }
  ChatFunctionCallAccumulator::Reconciled ChatFunctionCallAccumulator::done(const json::object& event) {
    const auto k = key(event); const auto* found = k ? byKey_.get(*k) : nullptr;
    if (!found) malformedFunctionCallStream("Upstream function-call completion has no matching item.");
    const auto call = *found;
    const auto value = fieldString(event, "arguments");
    if (!value) malformedFunctionCallStream("Upstream function-call completion is missing string arguments.");
    if (call->argumentsDone) {
      if (*value != call->arguments) malformedFunctionCallStream("Upstream function-call completion changed finalized arguments.");
      return {call, ""};
    }
    const auto suffix = reconcileArguments(*call, *value); call->argumentsDone = true; return {call, suffix};
  }
  std::optional<ChatFunctionCallAccumulator::Reconciled> ChatFunctionCallAccumulator::reconcileItem(const json::object& event, const json::value* item) {
    if (!rt::isRecord(item) || fieldString(*item, "type") != "function_call") return std::nullopt;
    const auto k = key(event, &item->get_object()); const auto* found = k ? byKey_.get(*k) : nullptr;
    const auto value = fieldString(*item, "arguments");
    if (!value) malformedFunctionCallStream("Upstream function-call item is missing string arguments.");
    if (!found) { const auto created = create(event, item->get_object()); created->argumentsDone = true; return Reconciled{created, created->arguments}; }
    const auto call = *found;
    if (call->callId != rt::trim(fieldString(*item, "call_id").value_or("")) || call->name != rt::trim(fieldString(*item, "name").value_or("")))
      malformedFunctionCallStream("Upstream function-call item changed its call_id or name.");
    if (call->argumentsDone) {
      if (*value != call->arguments) malformedFunctionCallStream("Upstream function-call item changed finalized arguments.");
      return Reconciled{call, ""};
    }
    const auto suffix = reconcileArguments(*call, *value); call->argumentsDone = true; return Reconciled{call, suffix};
  }
  std::optional<ChatFunctionCallAccumulator::Reconciled> ChatFunctionCallAccumulator::reconcileItem(const json::object& event, const json::value& item) {
    return reconcileItem(event, &item);
  }
  std::vector<ChatFunctionCallAccumulator::Reconciled> ChatFunctionCallAccumulator::reconcileOutput(const json::object& event, const json::value* output) {
    std::vector<Reconciled> result;
    if (output && output->is_array()) for (const auto& item : output->get_array()) if (auto reconciled = reconcileItem(event, item)) result.push_back(*reconciled);
    return result;
  }
  std::vector<ChatFunctionCallAccumulator::Reconciled> ChatFunctionCallAccumulator::reconcileOutput(const json::object& event, const json::value& output) {
    return reconcileOutput(event, &output);
  }
  bool preparedChatCompletionIsEmpty(const responses_failover_stream::PreparedResponsesStream& prepared) {
    PreparedState state;
    for (const auto& event : prepared.buffered) if (const auto failure = applyPreparedChatEvent(event, state)) malformedFunctionCallStream(*failure);
    if (!state.completed) malformedFunctionCallStream("Chat semantic preflight did not retain a completed terminal.");
    return state.outputText.empty() && state.refusal.empty() && !state.functionCalls.hasCalls();
  }
  json::object chatToolCallDelta(const ChatFunctionCall& call) { return chatToolCallDelta(call, ChatToolCallDeltaOptions{}); }
  json::object chatToolCallDelta(const ChatFunctionCall& call, const ChatToolCallDeltaOptions& options) {
    json::object fn;
    if (options.includeIdentity) fn["name"] = call.name;
    rt::objectSet(fn, "arguments", options.argumentsDelta);
    json::object result{{"index", rt::makeNumber(call.index)}, {"function", fn}};
    if (options.includeIdentity) { result["id"] = call.callId; result["type"] = "function"; }
    return result;
  }
  responses_stream::PreflightedResponsesStream chatSourceFromPrepared(responses_stream::PreflightedResponsesStream source,
                                                                    responses_failover_stream::PreparedResponsesStream prepared) {
    if (prepared.buffered.empty()) throw ResponsesStreamError("Chat preflight did not retain its first event.", {.kind = "read_error"});
    const auto first = prepared.buffered.front();
    auto state = std::make_shared<responses_failover_stream::PreparedResponsesStream>(std::move(prepared));
    responses_stream::ResponsesStreamIterator iterator([state](rt::Yield<ResponsesStreamEvent> yield) -> rt::Task<void> {
      rt::Reason failure;
      try {
        for (std::size_t i = 1; i < state->buffered.size(); ++i) co_await yield(state->buffered[i]);
        for (;;) { auto next = co_await state->iterator.next(); if (next.done) break; co_await yield(*next.value); }
      } catch (...) { failure = std::current_exception(); }
      try { co_await state->iterator.return_(); } catch (...) {}
      co_await rt::microtask();
      if (failure) std::rethrow_exception(failure);
    });
    auto cancel = [source, iterator](rt::Reason reason) mutable -> rt::Task<void> {
      rt::Promise<void> cancelled(source.cancel(reason)); co_await cancelled.wait();
      try { co_await iterator.return_(); } catch (...) {}
      co_await rt::microtask();
    };
    return {first, iterator, cancel};
  }
  json::object emptyUpstreamCompletionError() {
    return {{"error", json::object{{"message", std::string(EMPTY_UPSTREAM_COMPLETION_MESSAGE)}, {"type", "server_error"}, {"code", "empty_upstream_completion"}, {"param", nullptr}}}};
  }
  void markChatSemanticOutput(const telemetry::UsageContext* context) { if (context && context->responseTelemetry) context->responseTelemetry->semanticOutputObserved = true; }
  void markFinalizedChatToolOutput(const telemetry::UsageContext* context, const ChatFunctionCallAccumulator& calls) {
    for (const auto& call : calls.calls()) if (call->argumentsDone) { markChatSemanticOutput(context); return; }
  }
  bool translatedChatOutputObserved(const std::string& outputText, const std::string& refusal, const ChatFunctionCallAccumulator& functionCalls) {
    return !outputText.empty() || !refusal.empty() || functionCalls.hasCalls();
  }
  rt::Task<void> recordSuccessfulChatCompletion(const telemetry::UsageContext* context, telemetry::MeteredTransportLifecycle lifecycle,
                                               std::optional<telemetry::UsageTokens> usage, std::function<void(telemetry::ResponseStreamTerminalType)> onResponseTerminal) {
    markChatSemanticOutput(context);
    if (onResponseTerminal) onResponseTerminal("response.completed");
    lifecycle.terminal("response.completed", usage);
    telemetry::recordStreamTerminalType(context, "response.completed");
    auto recorded = telemetry::recordCompletionUsage(context, usage); co_await recorded.wait();
  }
  void recordEmptyUpstreamCompletion(const telemetry::UsageContext* context, const telemetry::MeteredTransportLifecycle& lifecycle,
                                    const std::optional<telemetry::UsageTokens>& usage,
                                    const std::function<void(telemetry::ResponseStreamTerminalType)>& onResponseTerminal) {
    if (context && context->responseTelemetry) { context->responseTelemetry->failureKind = "empty_upstream_completion"; context->responseTelemetry->semanticOutputObserved = false; }
    if (onResponseTerminal) onResponseTerminal("error");
    lifecycle.terminal("response.failed", usage);
    telemetry::recordStreamTerminalType(context, "error"); telemetry::recordTerminalUsage(context, usage, false);
  }
}
