// Port of src/responses-buffered.ts.
#include "uos/responses_buffered.hpp"
#include "uos/http.hpp"
#include "uos/chat/stream_translation.hpp"
#include "uos/utils.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/json.hpp"
namespace uos::responses_buffered {
namespace {
  namespace rs = responses_stream;
  namespace fs = responses_failover_stream;
  struct BufferedResponsesAccumulator {
    std::optional<std::string> responseId;
    std::string refusalText;
    rt::OrderedMap<std::string, std::string> deltaTextParts;
    rt::OrderedMap<std::string, std::string> doneTextParts;
    std::vector<std::string> textPartOrder;
    std::vector<json::value> outputItems;
  };
  std::string formatTextPartIndex(const json::value* value) { return rt::isNullish(value) ? "0" : value->is_object() || value->is_array() ? rt::jsonStringify(*value) : rt::toString(value); }
  std::string textPartKeyFromValue(const json::object& value) {
    const auto itemId = rt::trim(rt::getString(value, "item_id").value_or(""));
    if (!itemId.empty()) return "item:" + itemId + ":" + formatTextPartIndex(rt::get(value, "content_index"));
    return "output:" + formatTextPartIndex(rt::get(value, "output_index")) + ":" + formatTextPartIndex(rt::get(value, "content_index"));
  }
  void rememberBufferedTextPart(BufferedResponsesAccumulator& accumulator, const json::object& value, std::string_view text, bool done) {
    if (text.empty()) return;
    const auto key = textPartKeyFromValue(value); if (!rt::includes(accumulator.textPartOrder, key)) accumulator.textPartOrder.push_back(key);
    const auto* delta = accumulator.deltaTextParts.get(key); const auto deltaText = delta ? *delta : std::string();
    if (done) { if (deltaText.empty() || rt::startsWith(text, deltaText)) accumulator.doneTextParts.set(key, std::string(text)); }
    else accumulator.deltaTextParts.set(key, rt::concat(deltaText, text));
  }
  void trackBufferedResponseId(BufferedResponsesAccumulator& accumulator, const rs::ResponsesStreamEvent& event) {
    const std::vector<rs::ResponsesStreamEvent> events{event}; const auto eventId = fs::responseIdFromEvents(events);
    if (eventId && !eventId->empty() && accumulator.responseId && !accumulator.responseId->empty() && eventId != accumulator.responseId) throw rs::ResponsesStreamError("Upstream Responses stream changed response identifiers.", {.kind = "malformed_event"});
    if (!accumulator.responseId) accumulator.responseId = eventId;
  }
  void accumulateBufferedResponsesEvent(BufferedResponsesAccumulator& accumulator, const rs::ResponsesStreamEvent& event, const std::optional<std::string>& warningModel) {
    const auto& value = event.value; const bool suppressed = warningModel && !warningModel->empty() && rt::getNumber(value, "output_index") == 0;
    if (event.type == "response.output_text.delta" && !suppressed) rememberBufferedTextPart(accumulator, value, rt::getString(value, "delta").value_or(""), false);
    if (event.type == "response.output_text.done" && !suppressed) rememberBufferedTextPart(accumulator, value, rt::getString(value, "text").value_or(""), true);
    if (event.type == "response.refusal.delta") accumulator.refusalText = rt::concat(accumulator.refusalText, rt::getString(value, "delta").value_or(""));
    if (event.type == "response.refusal.done" && accumulator.refusalText.empty()) accumulator.refusalText = rt::getString(value, "refusal").value_or("");
    if (event.type == "response.output_item.done") if (const auto* item = rt::get(value, "item"); utils::isRecord(item)) accumulator.outputItems.push_back(*item);
    if (event.type == "response.output") {
      const auto* output = rt::get(value, "output"); if (rt::isNullish(output)) { const auto* response = rt::get(value, "response"); output = rt::get(response, "output"); }
      if (output && output->is_array()) for (const auto& item : output->get_array()) if (utils::isRecord(item)) accumulator.outputItems.push_back(item);
    }
  }
  json::object withBufferedResponseItems(const json::object& response, const std::vector<json::value>& accumulated) {
    std::vector<json::object> records;
    for (const auto& item : accumulated) if (item.is_object()) records.push_back(item.get_object());
    if (records.size() == accumulated.size()) return chat::stream_translation::withAccumulatedResponseItems(response, records);
    // The shared typed helper cannot carry array-valued records accepted by utils::isRecord.
    auto result = response; json::array output;
    if (const auto* existing = rt::getArray(response, "output")) for (const auto& item : *existing) if (utils::isRecord(item)) {
      json::object copy;
      if (item.is_object()) copy = item.get_object();
      else for (std::size_t i = 0; i < item.get_array().size(); ++i) rt::objectDefine(copy, rt::numberToString(static_cast<double>(i)), item.get_array()[i]);
      output.push_back(copy);
    }
    rt::OrderedSet<std::string> ids;
    for (const auto& item : output) if (const auto id = rt::getString(item, "id"); id && !id->empty()) ids.add(std::string(*id));
    for (const auto& item : accumulated) {
      const auto id = rt::getString(item, "id"); if (id && !id->empty() && ids.has(*id)) continue;
      output.push_back(item); if (id && !id->empty()) ids.add(std::string(*id));
    }
    result["output"] = output; return result;
  }
  rs::ResponsesStreamIterator createBufferedResponsesInitial(const responses_attempts::PreparedResponsesAttempt& attempt, const BufferedResponsesOptions& options) {
    if (options.warningModel && !options.warningModel->empty()) {
      fs::OwnedResponsesStreamOptions owned{.initial = attempt.prepared.buffered, .iterator = attempt.prepared.iterator, .responseId = attempt.responseId};
      owned.warning = fs::OwnedResponsesStreamOptions::Warning{*options.warningModel}; owned.validateEvent = options.validateEvent;
      owned.onEvent = [options](rs::ResponsesStreamEvent event) { openai_telemetry::recordResponsesEventTelemetry(options.usageContext, event); if (options.onEvent) options.onEvent(event); };
      owned.onFailure = [options](rt::Reason error, fs::OwnedResponsesStreamFailureDetails details) { if (options.onFailure) (void)options.onFailure(error, &details); };
      return rs::readResponsesStream(fs::createOwnedResponsesStream(owned));
    }
    return rs::ResponsesStreamIterator([prepared = attempt.prepared, options](rt::Yield<rs::ResponsesStreamEvent> yield) -> rt::Task<void> {
      for (const auto& event : prepared.buffered) {
        if (options.validateEvent) { options.validateEvent(event); } openai_telemetry::recordResponsesEventTelemetry(options.usageContext, event); if (options.onEvent) options.onEvent(event); co_await yield(event);
      }
      rt::Reason error;
      try {
        for (;;) { auto next = co_await prepared.iterator.next(); if (next.done) break;
          auto event = *next.value; if (options.validateEvent) { options.validateEvent(event); } openai_telemetry::recordResponsesEventTelemetry(options.usageContext, event); if (options.onEvent) options.onEvent(event); co_await yield(event); }
      } catch (...) { error = std::current_exception(); }
      if (error) { try { co_await prepared.iterator.return_(); } catch (...) {} std::rethrow_exception(error); }
    });
  }
}
rt::Task<rt::Response> collectBufferedResponses(responses_attempts::PreparedResponsesAttempt attempt) {
  const BufferedResponsesOptions options; auto task = collectBufferedResponses(attempt, options); auto awaitedResult1 = co_await std::move(task); co_return awaitedResult1;
}
rt::Task<rt::Response> collectBufferedResponses(responses_attempts::PreparedResponsesAttempt attempt, BufferedResponsesOptions options) {
  auto initial = createBufferedResponsesInitial(attempt, options);
  BufferedResponsesAccumulator accumulator{.responseId = attempt.responseId, .refusalText = "", .deltaTextParts = {}, .doneTextParts = {}, .textPartOrder = {}, .outputItems = {}};
  std::optional<json::object> finalResponse; std::optional<rt::Response> terminalError; rt::Reason error;
  try {
    for (;;) { auto next = co_await initial.next(); if (next.done) break; const auto& event = *next.value;
      trackBufferedResponseId(accumulator, event); accumulateBufferedResponsesEvent(accumulator, event, options.warningModel);
      if (event.type == "error") {
        if (options.onTerminal) options.onTerminal(event);
        terminalError = openai_telemetry::streamErrorResponse(502, std::string(rt::getString(event.value, "message").value_or("Upstream Responses stream ended unexpectedly.")), std::string(rt::getString(event.value, "code").value_or("server_error")), attempt.provider, {}); break;
      }
      if (event.type == "response.completed" || event.type == "response.failed" || event.type == "response.incomplete") if (const auto* response = rt::getObject(event.value, "response")) {
        if (options.onTerminal) { options.onTerminal(event); } finalResponse = *response; break;
      }
    }
    if (finalResponse || terminalError) { co_await initial.return_(); co_await rt::microtask(); }
  } catch (...) { error = std::current_exception(); }
  if (error) {
    try { co_await initial.return_(); } catch (...) {}
    if (options.onFailure) { const auto response = options.onFailure(error, nullptr); if (response) co_return *response; }
    co_return openai_telemetry::streamErrorResponse(502, "Upstream Responses stream ended unexpectedly.", "server_error", attempt.provider, {});
  }
  if (terminalError) co_return *terminalError;
  if (!finalResponse) co_return openai_telemetry::streamErrorResponse(502, "Upstream Responses stream ended unexpectedly.", "server_error", attempt.provider, {});
  std::string outputText;
  for (const auto& key : accumulator.textPartOrder) { const auto* text = accumulator.doneTextParts.get(key); if (!text) text = accumulator.deltaTextParts.get(key); if (text) outputText = rt::concat(outputText, *text); }
  auto response = withBufferedResponseItems(*finalResponse, accumulator.outputItems);
  response = chat::stream_translation::withAccumulatedResponseText(response, outputText, options.warningModel && !options.warningModel->empty() ? 1 : 0);
  response = chat::stream_translation::withAccumulatedResponseRefusal(response, accumulator.refusalText, options.warningModel && !options.warningModel->empty() ? 1 : 0);
  co_return http::json(200, response, {{"x-uos-upstream", attempt.provider}});
}
}
