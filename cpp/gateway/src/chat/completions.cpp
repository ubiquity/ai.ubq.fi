#include "uos/chat/completions.hpp"

#include "uos/chat/stream_translation.hpp"
#include "uos/http.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/weak.hpp"
#include "uos/utils.hpp"

#include <deque>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace uos::chat::completions {
  namespace {
    namespace translation = stream_translation;
    namespace telemetry = openai_telemetry;
    using responses_stream::ResponsesStreamError;
    using responses_stream::ResponsesStreamEvent;
    using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;
    using Parts = rt::OrderedMap<std::string, std::string>;
    using Usage = std::optional<telemetry::UsageTokens>;
    struct ChatData {
      std::string id = "chatcmpl_" + rt::replaceAll(rt::crypto::randomUUID(), "-", "");
      double created = rt::Math::floor(rt::dateNow() / 1000);
      std::string model;
      std::string outputText;
      std::string refusal;
      Parts outputTextParts;
      Parts refusalParts;
      translation::ChatFunctionCallAccumulator functionCalls;
      std::optional<telemetry::UsageContext> usageContext;
      telemetry::MeteredTransportLifecycle lifecycle;
      rt::AbortSignal signal = rt::AbortController().signal();
      rt::AbortSignal downstreamSignal = rt::AbortController().signal();
      std::function<void(telemetry::ResponseStreamTerminalType)> onResponseTerminal;
      const telemetry::UsageContext* context() const { return usageContext ? &*usageContext : nullptr; }
      void terminal(const std::string& type) const { if (onResponseTerminal) onResponseTerminal(type); }
      bool applyCreatedEvent(const ResponsesStreamEvent& event) {
        const auto* response = rt::get(event.value, "response");
        if (event.type != "response.created" || !utils::isRecord(response)) return false;
        if (const auto upstreamId = utils::getString(rt::get(*response, "id")); upstreamId && !upstreamId->empty()) id = *upstreamId;
        if (const auto createdAt = rt::getNumber(*response, "created_at"); createdAt && rt::truthy(*createdAt)) created = *createdAt;
        return true;
      }
      struct TextOutcome { bool handled; std::optional<std::pair<std::string, std::string>> emission; };
      TextOutcome applyTextEvent(const ResponsesStreamEvent& event) {
        const auto& ev = event.value; const auto& type = event.type;
        if (type == "response.output_text.delta" || type == "response.refusal.delta") {
          const bool refusing = type == "response.refusal.delta"; const auto delta = utils::getString(rt::get(ev, "delta"));
          if (!delta) translation::malformedFunctionCallStream(refusing ? "Upstream refusal delta is not a string." : "Upstream output-text delta is not a string.");
          auto& parts = refusing ? refusalParts : outputTextParts; const auto* prior = parts.get(translation::chatOutputTextPartStorageKey(ev));
          translation::setChatOutputTextPart(parts, ev, rt::concat(prior ? *prior : "", *delta)); rt::append(refusing ? refusal : outputText, *delta);
          return {true, std::pair<std::string, std::string>{refusing ? "refusal" : "content", *delta}};
        }
        if (type == "response.output_text.done" || type == "response.refusal.done") {
          const bool refusing = type == "response.refusal.done"; const auto completed = utils::getString(rt::get(ev, refusing ? "refusal" : "text"));
          if (!completed) translation::malformedFunctionCallStream(refusing ? "Upstream completed refusal is not a string." : "Upstream completed output text is not a string.");
          auto& parts = refusing ? refusalParts : outputTextParts; const auto* prior = parts.get(translation::chatOutputTextPartStorageKey(ev));
          const auto emitted = prior ? *prior : "";
          const auto suffix = refusing ? translation::reconcileCompletedRefusal(emitted, *completed) : translation::reconcileCompletedOutputText(emitted, *completed);
          translation::setChatOutputTextPart(parts, ev, rt::concat(emitted, suffix)); rt::append(refusing ? refusal : outputText, suffix);
          if (!suffix.empty()) return {true, std::pair<std::string, std::string>{refusing ? "refusal" : "content", suffix}};
          return {true, std::nullopt};
        }
        if (type == "response.content_part.done") {
          const auto result = translation::reconcileChatContentPart(outputTextParts, refusalParts, ev, rt::get(ev, "part"));
          rt::append(outputText, result.outputText); rt::append(refusal, result.refusal);
          if (!result.outputText.empty()) return {true, std::pair<std::string, std::string>{"content", result.outputText}};
          if (!result.refusal.empty()) return {true, std::pair<std::string, std::string>{"refusal", result.refusal}};
          return {true, std::nullopt};
        }
        return {false, std::nullopt};
      }
      const json::value* responseOutput(const json::object& ev) const {
        const auto* output = rt::get(ev, "output"); const auto* response = rt::get(ev, "response");
        if (rt::isNullish(output) && utils::isRecord(response)) return rt::get(*response, "output");
        return output;
      }
      bool hasSemanticOutput() const { return translation::translatedChatOutputObserved(outputText, refusal, functionCalls); }
    };
    Usage eventUsage(const ResponsesStreamEvent& event) {
      const auto* response = rt::get(event.value, "response"); return utils::isRecord(response) ? telemetry::extractUsageTokens(rt::get(*response, "usage")) : std::nullopt;
    }
    rt::Task<void> returnIterator(responses_stream::ResponsesStreamIterator iterator) { try { co_await iterator.return_(); } catch (...) {} co_await rt::microtask(); }
    struct StreamState : ChatData {
      responses_stream::PreflightedResponsesStream source;
      responses_stream::ResponsesStreamIterator iterator;
      std::optional<ResponsesStreamEvent> pending;
      bool includeUsage = false;
      bool sentRole = false;
      bool closed = false;
      bool terminalSettled = false;
      std::optional<Usage> observedCompletedUsage;
      rt::WeakSet<ResponsesStreamEvent> observedEvents;
      struct QueuedDelta {
        std::string kind;
        std::string text;
        std::shared_ptr<translation::ChatFunctionCall> call{};
        bool includeIdentity = false;
      };
      std::deque<QueuedDelta> queuedDeltas;
      explicit StreamState(responses_stream::PreflightedResponsesStream input) : source(std::move(input)), iterator(source.iterator), pending(source.first) {}
      void enqueue(const Controller& controller, const json::value& chunk) const { controller.enqueue(rt::encodeUtf8("data: " + rt::jsonStringify(chunk) + "\n\n")); }
      void emitContent(const Controller& controller, const std::string& content) {
        json::object delta; if (!sentRole) delta["role"] = "assistant"; delta["content"] = content;
        const json::object chunk{{"id", id}, {"object", "chat.completion.chunk"}, {"created", rt::makeNumber(created)}, {"model", model},
                                  {"choices", json::array{json::object{{"index", 0}, {"delta", delta}, {"finish_reason", nullptr}}}}};
        sentRole = true; if (!content.empty()) translation::markChatSemanticOutput(context()); enqueue(controller, chunk);
      }
      void emitRefusal(const Controller& controller, const std::string& value) {
        json::object delta; if (!sentRole) delta["role"] = "assistant"; delta["refusal"] = value;
        const json::object chunk{{"id", id}, {"object", "chat.completion.chunk"}, {"created", rt::makeNumber(created)}, {"model", model},
                                  {"choices", json::array{json::object{{"index", 0}, {"delta", delta}, {"finish_reason", nullptr}}}}};
        sentRole = true; if (!value.empty()) translation::markChatSemanticOutput(context()); enqueue(controller, chunk);
      }
      void emitToolCall(const Controller& controller, const translation::ChatFunctionCall& call, bool includeIdentity, const std::optional<std::string>& argumentsDelta) {
        translation::ChatToolCallDeltaOptions options; options.includeIdentity = includeIdentity; options.argumentsDelta = argumentsDelta;
        const auto toolCall = translation::chatToolCallDelta(call, options);
        json::object delta; if (!sentRole) delta["role"] = "assistant"; delta["tool_calls"] = json::array{toolCall};
        const json::object chunk{{"id", id}, {"object", "chat.completion.chunk"}, {"created", rt::makeNumber(created)}, {"model", model},
                                  {"choices", json::array{json::object{{"index", 0}, {"delta", delta}, {"finish_reason", nullptr}}}}};
        sentRole = true; enqueue(controller, chunk);
      }
      void queueFinalOutput(const json::object& event, const json::value* output) {
        const auto completed = translation::reconcileChatResponseOutputContent(outputTextParts, refusalParts, event, output);
        if (!completed.outputText.empty()) { rt::append(outputText, completed.outputText); queuedDeltas.push_back({"content", completed.outputText}); }
        if (!completed.refusal.empty()) { rt::append(refusal, completed.refusal); queuedDeltas.push_back({"refusal", completed.refusal}); }
        const auto beforeCount = functionCalls.calls().size(); const auto reconciled = functionCalls.reconcileOutput(event, output);
        if (!reconciled.empty()) translation::markFinalizedChatToolOutput(context(), functionCalls);
        for (const auto& result : reconciled) {
          const bool includeIdentity = result.call->index >= static_cast<double>(beforeCount);
          if (includeIdentity || !result.suffix.empty()) queuedDeltas.push_back({"tool", result.suffix, result.call, includeIdentity});
        }
      }
      bool emitNextQueuedDelta(const Controller& controller) {
        if (queuedDeltas.empty()) return false;
        const auto queued = queuedDeltas.front(); queuedDeltas.pop_front();
        if (queued.kind == "content") emitContent(controller, queued.text);
        else if (queued.kind == "refusal") emitRefusal(controller, queued.text);
        else emitToolCall(controller, *queued.call, queued.includeIdentity, queued.text);
        return true;
      }
      std::optional<bool> handleToolEvent(const ResponsesStreamEvent& event, const Controller& controller) {
        const auto& ev = event.value; const auto& type = event.type;
        if (type == "response.output_item.added") {
          const auto added = functionCalls.add(ev, rt::get(ev, "item"));
          if (added && (added->includeIdentity || !added->suffix.empty())) { emitToolCall(controller, *added->call, added->includeIdentity, added->suffix); return true; }
          return false;
        }
        if (type == "response.function_call_arguments.delta") { const auto result = functionCalls.delta(ev); emitToolCall(controller, *result.call, false, result.delta); return true; }
        if (type == "response.function_call_arguments.done") {
          const auto result = functionCalls.done(ev); translation::markFinalizedChatToolOutput(context(), functionCalls);
          if (!result.suffix.empty()) { emitToolCall(controller, *result.call, false, result.suffix); return true; } return false;
        }
        if (type == "response.output_item.done") {
          const auto* item = rt::get(ev, "item"); const bool wasKnown = rt::isRecord(item) && functionCalls.has(ev, &item->get_object());
          const auto reconciled = functionCalls.reconcileItem(ev, item);
          if (reconciled) {
            translation::markFinalizedChatToolOutput(context(), functionCalls);
            if (!wasKnown || !reconciled->suffix.empty()) { emitToolCall(controller, *reconciled->call, !wasKnown, reconciled->suffix); return true; }
          } else {
            const auto completed = translation::reconcileChatOutputItemContent(outputTextParts, refusalParts, ev, item);
            rt::append(outputText, completed.outputText); rt::append(refusal, completed.refusal);
            if (!completed.outputText.empty()) queuedDeltas.push_back({"content", completed.outputText});
            if (!completed.refusal.empty()) queuedDeltas.push_back({"refusal", completed.refusal});
            if (emitNextQueuedDelta(controller)) return true;
          }
          return false;
        }
        return std::nullopt;
      }
      rt::Task<void> handleCompletedEvent(ResponsesStreamEvent event, Controller controller) {
        const auto* response = rt::get(event.value, "response");
        if (!rt::isRecord(response)) translation::malformedFunctionCallStream("Upstream response.completed event is missing its response object.");
        observedCompletedUsage = telemetry::extractUsageTokens(rt::get(*response, "usage"));
        queueFinalOutput(event.value, rt::get(*response, "output")); functionCalls.assertFinalized(); const auto usageTokens = *observedCompletedUsage;
        if (!terminalSettled) {
          terminalSettled = true;
          if (!hasSemanticOutput()) {
            translation::recordEmptyUpstreamCompletion(context(), lifecycle, usageTokens, onResponseTerminal); enqueue(controller, translation::emptyUpstreamCompletionError());
            closed = true; controller.close(); rt::spawn(returnIterator(iterator), rt::ignoreRejection); co_return;
          }
          auto task = translation::recordSuccessfulChatCompletion(context(), lifecycle, usageTokens, onResponseTerminal); rt::Promise<void> recorded(std::move(task)); co_await recorded.wait();
        }
        if (!queuedDeltas.empty()) { pending = event; emitNextQueuedDelta(controller); co_return; }
        json::object delta; if (!sentRole) delta["role"] = "assistant";
        const json::object chunk{{"id", id}, {"object", "chat.completion.chunk"}, {"created", rt::makeNumber(created)}, {"model", model},
                                  {"choices", json::array{json::object{{"index", 0}, {"delta", delta}, {"finish_reason", functionCalls.hasCalls() ? "tool_calls" : "stop"}}}}};
        enqueue(controller, chunk);
        if (const auto usage = telemetry::toChatUsage(usageTokens); includeUsage && usage)
          enqueue(controller, json::object{{"id", id}, {"object", "chat.completion.chunk"}, {"created", rt::makeNumber(created)}, {"model", model}, {"choices", json::array()}, {"usage", *usage}});
        controller.enqueue(rt::encodeUtf8("data: [DONE]\n\n")); closed = true; controller.close(); rt::spawn(returnIterator(iterator), rt::ignoreRejection);
      }
      void handleTerminalEvent(const ResponsesStreamEvent& event, const Controller& controller) {
        const auto usage = eventUsage(event); terminal(event.type); lifecycle.terminal(event.type, usage); telemetry::recordStreamTerminalType(context(), event.type);
        telemetry::recordTerminalUsage(context(), usage, false); terminalSettled = true;
        enqueue(controller, json::object{{"error", json::object{{"message", "Upstream terminated with " + event.type + "."}, {"type", "server_error"}, {"code", "upstream_stream_error"}}}});
        closed = true; controller.close(); rt::spawn(returnIterator(iterator), rt::ignoreRejection);
      }
      rt::Task<bool> handleStreamEvent(ResponsesStreamEvent event, Controller controller) {
        if (applyCreatedEvent(event)) co_return false;
        const auto text = applyTextEvent(event);
        if (text.handled) {
          if (!text.emission) co_return false;
          if (text.emission->first == "content") emitContent(controller, text.emission->second); else emitRefusal(controller, text.emission->second); co_return true;
        }
        if (const auto tool = handleToolEvent(event, controller)) co_return *tool;
        if (event.type == "response.output") {
          queueFinalOutput(event.value, responseOutput(event.value));
          if (!queuedDeltas.empty()) { pending = event; emitNextQueuedDelta(controller); co_return true; } co_return false;
        }
        if (event.type == "response.completed") { rt::Promise<void> pendingCompleted(handleCompletedEvent(event, controller)); co_await pendingCompleted.wait(); co_return true; }
        if (event.terminal) { handleTerminalEvent(event, controller); co_return true; } co_return false;
      }
      rt::Task<void> pumpStream(Controller controller) {
        if (emitNextQueuedDelta(controller)) co_return;
        while (!closed) {
          std::optional<ResponsesStreamEvent> event;
          if (pending) event = *pending;
          else { auto next = co_await iterator.next(); if (!next.done) event = *next.value; }
          pending.reset();
          if (!event) throw ResponsesStreamError("Upstream Responses stream ended before a terminal event.", {.kind = "premature_eof"});
          if (!observedEvents.has(*event)) { observedEvents.add(*event); telemetry::recordResponsesEventTelemetry(context(), *event); }
          rt::Promise<bool> pendingEvent(handleStreamEvent(*event, controller)); const bool emitted = co_await pendingEvent.wait(); if (emitted) co_return;
        }
      }
      rt::Task<void> settleStreamFailure(rt::Reason error, Controller controller) {
        if (closed) co_return;
        rt::Promise<void> returned(returnIterator(iterator)); co_await returned.wait();
        if (!terminalSettled) {
          telemetry::recordResponsesFailureTelemetry(context(), error);
          if (observedCompletedUsage) {
            terminal("error"); lifecycle.terminal("response.failed", *observedCompletedUsage); telemetry::recordStreamTerminalType(context(), "error");
            telemetry::recordTerminalUsage(context(), *observedCompletedUsage, false); terminalSettled = true;
          } else {
            const auto type = telemetry::classifyStreamFailure(error, signal, downstreamSignal); terminal(type); telemetry::recordStreamTerminalType(context(), type);
            if (type == "cancelled") lifecycle.cancelled(); else lifecycle.ambiguous(); (void)telemetry::recordErrorUsage(context());
          }
        }
        if (!downstreamSignal.aborted()) enqueue(controller, json::object{{"error", json::object{{"message", "The upstream stream ended unexpectedly."}, {"type", "server_error"}, {"code", "upstream_stream_error"}}}});
        closed = true; controller.close();
      }
      rt::Task<void> pull(Controller controller) {
        if (closed) co_return;
        rt::Reason failure;
        try { rt::Promise<void> pendingPump(pumpStream(controller)); co_await pendingPump.wait(); } catch (...) { failure = std::current_exception(); }
        if (failure) { rt::Promise<void> settled(settleStreamFailure(failure, controller)); co_await settled.wait(); }
      }
      rt::Task<void> settleInitialTerminalOnCancel() {
        const auto event = source.first; if (terminalSettled || !event.terminal) co_return;
        telemetry::recordResponsesEventTelemetry(context(), event); const auto usage = eventUsage(event);
        if (event.type == "response.completed") {
          const auto* response = rt::get(event.value, "response");
          if (!rt::isRecord(response)) {
            const auto error = rt::makeReason(ResponsesStreamError("Upstream response.completed event is missing its response object.", {.kind = "malformed_event"}));
            telemetry::recordResponsesFailureTelemetry(context(), error); terminal("error"); lifecycle.ambiguous(); telemetry::recordStreamTerminalType(context(), "error"); terminalSettled = true; co_return;
          }
          rt::Reason failure;
          try {
            const auto completed = translation::reconcileChatResponseOutputContent(outputTextParts, refusalParts, event.value, rt::get(*response, "output"));
            rt::append(outputText, completed.outputText); rt::append(refusal, completed.refusal); functionCalls.reconcileOutput(event.value, rt::get(*response, "output")); functionCalls.assertFinalized();
          } catch (...) { failure = std::current_exception(); }
          if (failure) {
            telemetry::recordResponsesFailureTelemetry(context(), failure); terminal("error"); lifecycle.terminal("response.failed", usage);
            telemetry::recordStreamTerminalType(context(), "error"); telemetry::recordTerminalUsage(context(), usage, false); terminalSettled = true; co_return;
          }
          terminalSettled = true;
          if (hasSemanticOutput()) { auto task = translation::recordSuccessfulChatCompletion(context(), lifecycle, usage, onResponseTerminal); rt::Promise<void> recorded(std::move(task)); co_await recorded.wait(); }
          else translation::recordEmptyUpstreamCompletion(context(), lifecycle, usage, onResponseTerminal);
          co_return;
        }
        terminal(event.type); lifecycle.terminal(event.type, usage); telemetry::recordStreamTerminalType(context(), event.type); telemetry::recordTerminalUsage(context(), usage, false); terminalSettled = true;
      }
      rt::Task<void> cancel(rt::Reason reason) {
        if (closed) co_return;
        closed = true; rt::Promise<void> initial(settleInitialTerminalOnCancel()); co_await initial.wait();
        if (!terminalSettled) { terminal("cancelled"); telemetry::recordStreamTerminalType(context(), "cancelled"); lifecycle.cancelled(); (void)telemetry::recordErrorUsage(context()); }
        rt::Promise<void> cancelled(source.cancel(reason)); co_await cancelled.wait();
      }
    };
    enum class BufferedOutcome { next, break_, complete, empty };
    struct BufferedState : ChatData {
      std::optional<json::object> usage;
      bool completed = false;
      std::optional<std::string> terminalType;
      std::optional<Usage> observedCompletedUsage;
      void applyTerminalEvent(const ResponsesStreamEvent& event) {
        terminalType = event.type; const auto tokens = eventUsage(event); terminal(event.type); lifecycle.terminal(event.type, tokens);
        telemetry::recordStreamTerminalType(context(), event.type); telemetry::recordTerminalUsage(context(), tokens, false);
      }
      void applyOutputItemDone(const json::object& ev) {
        if (functionCalls.reconcileItem(ev, rt::get(ev, "item"))) translation::markFinalizedChatToolOutput(context(), functionCalls);
        else {
          const auto value = translation::reconcileChatOutputItemContent(outputTextParts, refusalParts, ev, rt::get(ev, "item"));
          rt::append(outputText, value.outputText); rt::append(refusal, value.refusal);
          if (!value.outputText.empty() || !value.refusal.empty()) translation::markChatSemanticOutput(context());
        }
      }
      void applyResponseOutput(const json::object& ev) {
        const auto* output = responseOutput(ev); const auto value = translation::reconcileChatResponseOutputContent(outputTextParts, refusalParts, ev, output);
        rt::append(outputText, value.outputText); rt::append(refusal, value.refusal); const auto calls = functionCalls.reconcileOutput(ev, output);
        if (!value.outputText.empty() || !value.refusal.empty()) translation::markChatSemanticOutput(context());
        if (!calls.empty()) translation::markFinalizedChatToolOutput(context(), functionCalls);
      }
      rt::Task<BufferedOutcome> applyResponseCompleted(json::object ev, json::object response) {
        observedCompletedUsage = telemetry::extractUsageTokens(rt::get(response, "usage"));
        const auto value = translation::reconcileChatResponseOutputContent(outputTextParts, refusalParts, ev, rt::get(response, "output"));
        rt::append(outputText, value.outputText); rt::append(refusal, value.refusal); functionCalls.reconcileOutput(ev, rt::get(response, "output")); functionCalls.assertFinalized();
        const auto tokens = *observedCompletedUsage; usage = telemetry::toChatUsage(tokens);
        if (!hasSemanticOutput()) { translation::recordEmptyUpstreamCompletion(context(), lifecycle, tokens, onResponseTerminal); co_return BufferedOutcome::empty; }
        completed = true; auto task = translation::recordSuccessfulChatCompletion(context(), lifecycle, tokens, onResponseTerminal); rt::Promise<void> recorded(std::move(task)); co_await recorded.wait();
        co_return BufferedOutcome::complete;
      }
      rt::Task<BufferedOutcome> consumeEvent(ResponsesStreamEvent event) {
        telemetry::recordResponsesEventTelemetry(context(), event);
        if (event.terminal && event.type != "response.completed") applyTerminalEvent(event);
        if (applyCreatedEvent(event)) co_return BufferedOutcome::next;
        const auto text = applyTextEvent(event);
        if (text.handled) { if (text.emission && !text.emission->second.empty()) translation::markChatSemanticOutput(context()); co_return BufferedOutcome::next; }
        const auto& ev = event.value; const auto& type = event.type;
        if (type == "response.output_item.added") functionCalls.add(ev, rt::get(ev, "item"));
        else if (type == "response.function_call_arguments.delta") functionCalls.delta(ev);
        else if (type == "response.function_call_arguments.done") { functionCalls.done(ev); translation::markFinalizedChatToolOutput(context(), functionCalls); }
        else if (type == "response.output_item.done") applyOutputItemDone(ev);
        else if (type == "response.output") applyResponseOutput(ev);
        else if (type == "response.completed" && rt::isRecord(rt::get(ev, "response"))) {
          auto task = applyResponseCompleted(ev, rt::get(ev, "response")->get_object()); rt::Promise<BufferedOutcome> pending(std::move(task)); auto outcome = co_await pending.wait(); co_return outcome;
        } else if (event.terminal) co_return BufferedOutcome::break_;
        co_return BufferedOutcome::next;
      }
      void settleFailure(const rt::Reason& error) {
        telemetry::recordResponsesFailureTelemetry(context(), error);
        if (observedCompletedUsage) {
          terminalType = "error"; terminal("error"); lifecycle.terminal("response.failed", *observedCompletedUsage); telemetry::recordStreamTerminalType(context(), "error");
          telemetry::recordTerminalUsage(context(), *observedCompletedUsage, false);
        } else {
          terminalType = telemetry::classifyStreamFailure(error, signal, downstreamSignal); terminal(*terminalType); telemetry::recordStreamTerminalType(context(), *terminalType);
          if (*terminalType == "cancelled") lifecycle.cancelled(); else lifecycle.ambiguous();
        }
        completed = false;
      }
      json::object buildCompletedBody() const {
        json::object message{{"role", "assistant"}, {"content", !outputText.empty() || (!functionCalls.hasCalls() && refusal.empty()) ? json::value(outputText) : json::value(nullptr)}};
        if (!refusal.empty()) message["refusal"] = refusal;
        if (functionCalls.hasCalls()) {
          json::array tools; for (const auto& call : functionCalls.calls()) tools.push_back(json::object{{"id", call->callId}, {"type", "function"}, {"function", json::object{{"name", call->name}, {"arguments", call->arguments}}}});
          message["tool_calls"] = tools;
        }
        json::object body{{"id", id}, {"object", "chat.completion"}, {"created", rt::makeNumber(created)}, {"model", model},
                           {"choices", json::array{json::object{{"index", 0}, {"message", message}, {"finish_reason", functionCalls.hasCalls() ? "tool_calls" : "stop"}}}}};
        if (usage) body["usage"] = *usage;
        return body;
      }
    };
  }
  rt::Response streamChatCompletions(responses_stream::PreflightedResponsesStream source, std::string model, bool includeUsage,
                                     const openai_telemetry::UsageContext* usageContext, openai_telemetry::UpstreamProvider provider,
                                     openai_telemetry::MeteredTransportLifecycle lifecycle, rt::AbortSignal signal, rt::AbortSignal downstreamSignal,
                                     std::function<void(openai_telemetry::ResponseStreamTerminalType)> onResponseTerminal) {
    auto state = std::make_shared<StreamState>(std::move(source)); state->model = std::move(model); state->includeUsage = includeUsage;
    if (usageContext) state->usageContext = *usageContext;
    state->lifecycle = std::move(lifecycle); state->signal = std::move(signal); state->downstreamSignal = std::move(downstreamSignal); state->onResponseTerminal = std::move(onResponseTerminal);
    rt::UnderlyingSource<rt::ByteChunk> algorithms;
    algorithms.pull = [state](Controller controller) { return state->pull(std::move(controller)); };
    algorithms.cancel = [state](rt::Reason reason) { return state->cancel(std::move(reason)); };
    rt::ByteStream stream(algorithms);
    return rt::Response(responses_stream::withSseKeepalive(stream), rt::ResponseInit{.status = 200, .headers = rt::Headers{{"Content-Type", "text/event-stream"}, {"Cache-Control", "no-cache"}, {"x-uos-upstream", provider}}});
  }
  rt::Task<rt::Response> completeChatCompletions(responses_stream::PreflightedResponsesStream source, std::string model,
                                                const openai_telemetry::UsageContext* usageContext, openai_telemetry::UpstreamProvider provider,
                                                openai_telemetry::MeteredTransportLifecycle lifecycle, rt::AbortSignal signal, rt::AbortSignal downstreamSignal,
                                                std::vector<std::string> warnings, std::function<void(openai_telemetry::ResponseStreamTerminalType)> onResponseTerminal) {
    BufferedState state; state.model = std::move(model); if (usageContext) state.usageContext = *usageContext;
    state.lifecycle = std::move(lifecycle); state.signal = std::move(signal); state.downstreamSignal = std::move(downstreamSignal); state.onResponseTerminal = std::move(onResponseTerminal);
    bool emptyCompletion = false;
    try {
      std::optional<ResponsesStreamEvent> pending = source.first;
      for (;;) {
        std::optional<ResponsesStreamEvent> event;
        if (pending) event = *pending; else { auto next = co_await source.iterator.next(); if (!next.done) event = *next.value; }
        pending.reset(); if (!event) break;
        rt::Promise<BufferedOutcome> consumed(state.consumeEvent(*event)); const auto outcome = co_await consumed.wait();
        if (outcome == BufferedOutcome::break_) break;
        if (outcome == BufferedOutcome::empty) { emptyCompletion = true; break; }
        if (outcome == BufferedOutcome::complete) { state.completed = true; break; }
      }
    } catch (...) { state.settleFailure(std::current_exception()); }
    rt::Promise<void> returned(returnIterator(source.iterator)); co_await returned.wait();
    if (emptyCompletion) co_return telemetry::streamErrorResponse(502, std::string(translation::EMPTY_UPSTREAM_COMPLETION_MESSAGE), "empty_upstream_completion", provider, warnings, "server_error", nullptr);
    if (!state.completed) {
      auto recorded = telemetry::recordErrorUsage(state.context()); co_await recorded.wait();
      if (state.terminalType == "cancelled") co_return telemetry::streamErrorResponse(499, "Request was cancelled.", "request_cancelled", provider, warnings, "server_error", nullptr);
      if (state.terminalType == "deadline") co_return telemetry::streamErrorResponse(504, "Upstream request exceeded the gateway deadline.", "gateway_timeout", provider, warnings, "server_error", nullptr);
      co_return telemetry::streamErrorResponse(502, "Upstream stream ended without response.completed.", "upstream_stream_error", provider, warnings);
    }
    co_return http::json(200, state.buildCompletedBody(), rt::Headers{{"x-uos-upstream", provider}});
  }
}
