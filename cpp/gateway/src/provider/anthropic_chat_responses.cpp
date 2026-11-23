#include "uos/provider/anthropic_chat_responses.hpp"

#include "uos/bounded_response_body.hpp"
#include "uos/inference_deadline.hpp"
#include "uos/provider/anthropic_chat_stream.hpp"
#include "uos/provider/anthropic_replay.hpp"
#include "uos/provider/anthropic_response_wire.hpp"
#include "uos/responses_stream.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"

#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace uos::provider::anthropic_chat_responses {
  namespace {
    using JsonRecord = json::object;
    using anthropic_chat_stream::ChatFrame;
    using anthropic_chat_stream::ChatFrameChunk;
    using anthropic_chat_stream::ChatFrameComment;
    using responses_stream::MAX_RESPONSES_SSE_EVENT_BYTES;
    using responses_stream::ResponsesStreamError;
    using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;

    json::value text(std::string_view value) { return json::value(json::string(value)); }
    bool equalsString(const json::value* value, std::string_view expected) { return value && value->is_string() && value->get_string() == expected; }

    bool record(const json::value* value) { return rt::isRecord(value); }
    bool has(const JsonRecord& value, std::string_view key) { return rt::hasOwn(value, key); }
    ResponsesStreamError malformed(std::string message) { return ResponsesStreamError(std::move(message), {.cause = nullptr, .kind = "malformed_event"}); }

    std::string withoutDashes(const std::string& value) {
      std::string out;
      for (char character : value)
        if (character != '-') out += character;
      return out;
    }

    // `responseId(id: unknown)`: nullptr is undefined.
    std::string responseId(const json::value* id) {
      static const rt::Regex NON_ALPHANUMERIC("[^A-Za-z0-9]", "g");
      if (id && id->is_string()) return "resp_" + rt::slice(rt::replace(id->get_string(), NON_ALPHANUMERIC, ""), 0, 40);
      return "resp_" + withoutDashes(rt::crypto::randomUUID());
    }

    json::value orNull(const std::optional<JsonRecord>& value) { return value ? json::value(*value) : json::value(nullptr); }
  }

  std::optional<json::object> toAnthropicChatResponsesUsage(const json::value* usage) {
    if (!record(usage)) return std::nullopt;
    JsonRecord result;
    static constexpr std::pair<std::string_view, std::string_view> fields[] = {
      {"prompt_tokens", "input_tokens"},
      {"prompt_tokens_details", "input_tokens_details"},
      {"completion_tokens", "output_tokens"},
      {"completion_tokens_details", "output_tokens_details"},
      {"total_tokens", "total_tokens"},
      {"buyer_cost_micro", "buyer_cost_micro"},
      {"cost", "cost"},
      {"cost_details", "cost_details"},
    };
    for (const auto& [source, target] : fields) {
      if (has(usage->get_object(), source)) rt::objectSet(result, target, *rt::get(*usage, source));
    }
    return result;
  }

  std::optional<json::object> toAnthropicChatResponsesUsage(const json::value& usage) { return toAnthropicChatResponsesUsage(&usage); }

  namespace {
    std::string detailText(const json::value* value) {
      if (!value || !value->is_array()) return "";
      std::string out;
      for (const json::value& detail : value->get_array()) {
        const std::optional<std::string_view> detailText = rt::getString(detail, "text");
        if (record(&detail) && equalsString(rt::get(detail, "type"), "reasoning.text") && detailText) out = rt::concat(out, *detailText);
      }
      return out;
    }

    std::string readableReasoning(const JsonRecord& message) {
      const std::optional<std::string_view> reasoningContent = rt::getString(message, "reasoning_content");
      if (reasoningContent && !reasoningContent->empty()) return std::string(*reasoningContent);
      const std::optional<std::string_view> reasoning = rt::getString(message, "reasoning");
      if (reasoning && !reasoning->empty()) return std::string(*reasoning);
      return detailText(rt::get(message, "reasoning_details"));
    }

    JsonRecord projectedMessage(const JsonRecord& message, const AnthropicChatResponsesOptions& options) {
      const std::string reasoning = readableReasoning(message);
      JsonRecord projected = rt::objectSpread(message);
      rt::objectDelete(projected, "reasoning_details");
      rt::objectDelete(projected, "reasoning_content");
      rt::objectDelete(projected, "reasoning");
      if (options.reasoningSummary != std::optional<std::string>("none") && !reasoning.empty()) rt::objectSet(projected, "reasoning_content", text(reasoning));
      return projected;
    }

    std::string deltaReasoning(const JsonRecord& delta) {
      if (const std::optional<std::string_view> reasoningContent = rt::getString(delta, "reasoning_content")) return std::string(*reasoningContent);
      const std::optional<std::string_view> reasoning = rt::getString(delta, "reasoning");
      return reasoning ? std::string(*reasoning) : std::string();
    }

    void publishReplay(const JsonRecord& payload, const JsonRecord& assistant, const AnthropicChatResponsesOptions& options, bool valid = true) {
      const json::array* output = rt::getArray(payload, "output");
      if (!valid || !equalsString(rt::get(payload, "status"), "completed") || !rt::truthy(options.provider) || !rt::truthy(options.reasoningReplayScope) || !output) return;
      anthropic_replay::ChatReplayPublication publication;
      publication.scope = *options.reasoningReplayScope;
      publication.provider = *options.provider;
      publication.model = options.model;
      publication.output = *output;
      publication.assistant = assistant;
      if (options.toolNames) {
        rt::OrderedMap<std::string, anthropic_replay::ChatReplayPublication::ToolName> toolNames;
        for (const auto& [name, original] : *options.toolNames) {
          toolNames.set(name, anthropic_replay::ChatReplayPublication::ToolName{.name = original.name, .namespace_ = original.namespace_});
        }
        publication.toolNames = std::move(toolNames);
      }
      publication.customToolNames = options.customToolNames;
      anthropic_replay::rememberAnthropicChatReplay(publication);
    }

    // Readonly<{ completion; choice; assistant }>.
    struct ReadChoice {
      const JsonRecord* completion;
      const JsonRecord* choice;
      const JsonRecord* assistant;
    };

    ReadChoice readChoice(const json::value& completion) {
      const json::array* choices = record(&completion) ? rt::getArray(completion, "choices") : nullptr;
      if (!choices || choices->size() != 1 || !record(&(*choices)[0])) throw malformed("Claude Chat returned an invalid single-choice completion.");
      const JsonRecord& choice = (*choices)[0].get_object();
      const json::value* message = rt::get(choice, "message");
      if (!record(message) || !equalsString(rt::get(*message, "role"), "assistant") || !rt::getString(choice, "finish_reason")) {
        throw malformed("Claude Chat completion has no assistant message or finish reason.");
      }
      return ReadChoice{.completion = &completion.get_object(), .choice = &choice, .assistant = &message->get_object()};
    }

    anthropic_response_wire::WriterOptions writerOptions(const AnthropicChatResponsesOptions& options, std::string id, double created) {
      return anthropic_response_wire::WriterOptions{
        .model = options.model, .id = std::move(id), .created = created, .toolNames = options.toolNames, .customToolNames = options.customToolNames};
    }

    JsonRecord bufferedPayload(const json::value& value, const AnthropicChatResponsesOptions& options) {
      const ReadChoice read = readChoice(value);
      const JsonRecord& completion = *read.completion;
      const std::optional<double> created = rt::getNumber(completion, "created");
      const anthropic_response_wire::ClaudeResponsesWriter writer = anthropic_response_wire::createClaudeResponsesWriter(
        writerOptions(options, responseId(rt::get(completion, "id")), created ? *created : rt::Math::floor(rt::dateNow() / 1000)));
      writer.push(projectedMessage(*read.assistant, options));
      std::vector<JsonRecord> tools;
      if (const json::array* calls = rt::getArray(*read.assistant, "tool_calls")) {
        for (const json::value& call : *calls)
          if (record(&call)) tools.push_back(call.get_object());
      }
      const std::optional<json::value> usage = orNull(toAnthropicChatResponsesUsage(rt::get(completion, "usage")));
      const std::optional<json::value> finishReason = *rt::get(*read.choice, "finish_reason");
      writer.finish(tools, finishReason, usage);
      const std::optional<JsonRecord> payload = writer.response();
      if (!payload) throw malformed("Claude Chat produced no terminal response.");
      publishReplay(*payload, *read.assistant, options);
      return *payload;
    }

    struct ToolCall {
      std::string id;
      std::string name;
      std::string arguments;
    };

    bool mergeReasoningDetail(JsonRecord& prior, const JsonRecord& item) {
      const std::optional<std::string_view> itemSignature = rt::getString(item, "signature");
      const std::optional<std::string_view> priorSignature = rt::getString(prior, "signature");
      bool valid = !(itemSignature && !itemSignature->empty() && priorSignature && !priorSignature->empty());
      for (const json::key_value_pair* entry : rt::objectEntries(item)) {
        const std::string_view key = entry->key();
        const json::value& value = entry->value();
        if (key == "text" && value.is_string()) {
          const std::string priorText(rt::getString(prior, "text").value_or(""));
          rt::objectSet(prior, "text", text(rt::concat(priorText, value.get_string())));
        } else if (key == "signature" && value.is_string() && !rt::truthy(rt::get(prior, "signature"))) {
          rt::objectSet(prior, "signature", value);
        } else if (key != "signature" && has(prior, key) && !rt::strictEquals(*rt::get(prior, key), value)) {
          valid = false;
        } else if (!has(prior, key)) {
          rt::objectSet(prior, key, value);
        }
      }
      return valid;
    }

    /** Accumulate the exact standard Chat assistant fields, without synthesizing native signatures. */
    class ChatState {
    public:
      JsonRecord push(const JsonRecord& chunk) {
        if (has(chunk, "usage") && !rt::get(chunk, "usage")->is_null()) usage_ = *rt::get(chunk, "usage");
        const json::array* choices = rt::getArray(chunk, "choices");
        if (!choices || choices->size() > 1) throw malformed("Claude Chat emitted an invalid single-choice stream chunk.");
        if (choices->empty()) {
          JsonRecord empty = rt::objectSpread(chunk);
          rt::objectDefine(empty, "choices", json::array());
          return empty;
        }
        const json::value& choiceValue = (*choices)[0];
        if (!record(&choiceValue)) throw malformed("Claude Chat emitted a non-object choice.");
        const JsonRecord& choice = choiceValue.get_object();
        if (const std::optional<std::string_view> finishReason = rt::getString(choice, "finish_reason")) finish_ = std::string(*finishReason);
        const json::value* choiceDelta = rt::get(choice, "delta");
        const JsonRecord delta = record(choiceDelta) ? choiceDelta->get_object() : JsonRecord();
        appendField("content", delta);
        appendField("reasoning_content", delta);
        appendField("reasoning", delta);
        appendField("refusal", delta);
        mergeDetails(rt::get(delta, "reasoning_details"));
        mergeTools(rt::get(delta, "tool_calls"));
        const std::string fromContent = deltaReasoning(delta);
        const std::string fromDetails = detailText(rt::get(delta, "reasoning_details"));
        if (!reasoningSource && (!fromContent.empty() || !fromDetails.empty())) reasoningSource = !fromContent.empty() ? "content" : "details";
        const std::string reasoning = reasoningSource == std::optional<std::string>("content") ? fromContent : fromDetails;
        JsonRecord projected = rt::objectSpread(delta);
        rt::objectDelete(projected, "reasoning_details");
        rt::objectDelete(projected, "reasoning_content");
        rt::objectDelete(projected, "reasoning");
        // Tools are announced once their names/arguments are complete at [DONE].
        rt::objectDelete(projected, "tool_calls");
        if (!reasoning.empty()) rt::objectSet(projected, "reasoning_content", text(reasoning));
        JsonRecord projectedChoice = rt::objectSpread(choice);
        rt::objectDefine(projectedChoice, "delta", std::move(projected));
        JsonRecord result = rt::objectSpread(chunk);
        rt::objectDefine(result, "choices", json::array{std::move(projectedChoice)});
        return result;
      }

      JsonRecord message() {
        if (details.size()) {
          std::vector<std::pair<double, JsonRecord>> entries;
          for (const auto& [index, detail] : details) entries.emplace_back(index, detail);
          rt::sort(entries, [](const std::pair<double, JsonRecord>& left, const std::pair<double, JsonRecord>& right) { return left.first - right.first; });
          json::array reasoningDetails;
          for (const auto& entry : entries) reasoningDetails.push_back(entry.second);
          rt::objectSet(assistant, "reasoning_details", std::move(reasoningDetails));
        }
        if (tools.size()) {
          json::array calls;
          for (const JsonRecord& call : toolCalls()) {
            JsonRecord original = rt::objectSpread(call);
            rt::objectDelete(original, "index");
            calls.push_back(std::move(original));
          }
          rt::objectSet(assistant, "tool_calls", std::move(calls));
        }
        return assistant;
      }

      std::vector<JsonRecord> toolCalls() const {
        std::vector<std::pair<double, ToolCall>> entries;
        for (const auto& [index, call] : tools) entries.emplace_back(index, call);
        rt::sort(entries, [](const std::pair<double, ToolCall>& left, const std::pair<double, ToolCall>& right) { return left.first - right.first; });
        std::vector<JsonRecord> calls;
        for (const auto& [index, call] : entries) {
          calls.push_back(JsonRecord{{"index", rt::makeNumber(index)},
                                     {"id", call.id},
                                     {"type", "function"},
                                     {"function", JsonRecord{{"name", call.name}, {"arguments", call.arguments}}}});
        }
        return calls;
      }

      // `usage: unknown` (std::nullopt is undefined).
      const std::optional<json::value>& usage() const { return usage_; }
      // `string | null`.
      const std::optional<std::string>& finish() const { return finish_; }
      bool replayable() const { return replayable_; }

    private:
      void appendField(std::string_view key, const JsonRecord& delta) {
        const json::value* value = rt::get(delta, key);
        if (!has(delta, key) || value->is_null()) return;
        if (!value->is_string()) throw malformed("Claude Chat " + std::string(key) + " delta must be text.");
        const std::string prior(rt::getString(assistant, key).value_or(""));
        rt::objectSet(assistant, key, text(rt::concat(prior, value->get_string())));
      }

      void mergeDetails(const json::value* raw) {
        if (!raw || raw->is_null()) return;
        if (!raw->is_array()) {
          replayable_ = false;
          return;
        }
        for (const json::value& item : raw->get_array()) {
          const json::value* index = rt::get(item, "index");
          if (!record(&item) || !index || !rt::Number::isSafeInteger(*index) || rt::toNumber(index) < 0 || !equalsString(rt::get(item, "type"), "reasoning.text")) {
            replayable_ = false;
            continue;
          }
          const double position = rt::toNumber(index);
          JsonRecord* prior = details.get(position);
          if (!prior) {
            details.set(position, rt::objectSpread(item.get_object()));
            continue;
          }
          // Text is a standard delta; undocumented repeated signatures or changing
          // metadata cannot safely become an original signed replay receipt.
          if (!mergeReasoningDetail(*prior, item.get_object())) replayable_ = false;
        }
      }

      void mergeTools(const json::value* raw) {
        if (!raw || raw->is_null()) return;
        if (!raw->is_array()) throw malformed("Claude Chat tool_calls delta must be an array.");
        const json::array& items = raw->get_array();
        for (std::size_t position = 0; position < items.size(); ++position) {
          const json::value& item = items[position];
          const json::value* function = rt::get(item, "function");
          if (!record(&item) || !record(function)) throw malformed("Claude Chat tool call delta must have a function.");
          const json::value* indexValue = rt::get(item, "index");
          const double index = indexValue && rt::isNumber(*indexValue) ? *rt::getNumber(*indexValue) : static_cast<double>(position);
          if (!rt::Number::isSafeInteger(index) || index < 0) throw malformed("Claude Chat tool call index is invalid.");
          const ToolCall* existing = tools.get(index);
          ToolCall call = existing ? *existing : ToolCall{};
          if (const std::optional<std::string_view> id = rt::getString(item, "id")) call.id = std::string(*id);
          if (const std::optional<std::string_view> name = rt::getString(*function, "name")) call.name = rt::concat(call.name, *name);
          if (const std::optional<std::string_view> arguments = rt::getString(*function, "arguments")) call.arguments = rt::concat(call.arguments, *arguments);
          tools.set(index, std::move(call));
        }
      }

      JsonRecord assistant{{"role", "assistant"}, {"content", nullptr}};
      rt::OrderedMap<double, ToolCall> tools;
      rt::OrderedMap<double, JsonRecord> details;
      bool replayable_ = true;
      std::optional<json::value> usage_;
      std::optional<std::string> finish_;
      // "content" | "details" | null
      std::optional<std::string> reasoningSource;
    };

    rt::Task<void> translateChatStreamBody(rt::Response response, AnthropicChatResponsesOptions options, rt::AbortSignal signal, rt::Yield<rt::ByteChunk> yield) {
      ChatState state;
      const json::value requestId = json::string(options.requestId ? *options.requestId : rt::crypto::randomUUID());
      const anthropic_response_wire::ClaudeResponsesWriter translator =
        anthropic_response_wire::createClaudeResponsesWriter(writerOptions(options, responseId(&requestId), rt::Math::floor(rt::dateNow() / 1000)));
      double sequence = 0;
      const auto encode = [&sequence](const JsonRecord& event) {
        JsonRecord numbered = rt::objectSpread(event);
        rt::objectDefine(numbered, "sequence_number", rt::makeNumber(sequence++));
        return rt::encodeUtf8(anthropic_response_wire::encodeClaudeResponsesEvent(numbered));
      };
      const auto chunkEvents = [&state, &options, &translator](const JsonRecord& chunk) {
        const JsonRecord projected = state.push(chunk);
        const json::array* choices = rt::getArray(projected, "choices");
        const json::value* choice = choices && !choices->empty() ? &(*choices)[0] : nullptr;
        const json::value* choiceDelta = record(choice) ? rt::get(*choice, "delta") : nullptr;
        JsonRecord delta = record(choiceDelta) ? choiceDelta->get_object() : JsonRecord();
        if (options.reasoningSummary == std::optional<std::string>("none")) rt::objectDelete(delta, "reasoning_content");
        std::vector<JsonRecord> events = translator.push(delta);
        if (has(chunk, "usage") && !rt::get(chunk, "usage")->is_null()) {
          const std::optional<json::value> usage = orNull(toAnthropicChatResponsesUsage(state.usage() ? &*state.usage() : nullptr));
          events.push_back(translator.observedUsage(usage));
        }
        return events;
      };
      const auto finishEvents = [&state, &options, &translator] {
        if (!rt::truthy(state.finish())) throw ResponsesStreamError("Claude Chat ended without a finish reason.", {.cause = nullptr, .kind = "premature_eof"});
        const std::optional<json::value> usage = orNull(toAnthropicChatResponsesUsage(state.usage() ? &*state.usage() : nullptr));
        const std::optional<json::value> reason = text(*state.finish());
        std::vector<JsonRecord> events = translator.finish(state.toolCalls(), reason, usage);
        const std::optional<JsonRecord> finished = translator.response();
        if (finished) publishReplay(*finished, state.message(), options, state.replayable());
        return events;
      };
      const rt::AsyncGenerator<ChatFrame> frames = anthropic_chat_stream::readAnthropicChatStream(response, signal);
      rt::Reason failure;
      bool returning = false;
      try {
        // for await (const frame of readAnthropicChatStream(response, signal)) { ... }
        for (;;) {
          rt::IteratorResult<ChatFrame> step = co_await frames.next();
          if (step.done) break;
          rt::Reason bodyFailure;
          bool bodyReturning = false;
          try {
            if (const ChatFrameComment* comment = std::get_if<ChatFrameComment>(&*step.value)) {
              rt::ByteChunk bytes = rt::encodeUtf8(comment->text + "\n\n");
              co_await yield(std::move(bytes));
            } else if (const ChatFrameChunk* chunk = std::get_if<ChatFrameChunk>(&*step.value)) {
              const std::vector<JsonRecord> events = chunkEvents(chunk->value);
              for (const JsonRecord& event : events) {
                rt::ByteChunk bytes = encode(event);
                co_await yield(std::move(bytes));
              }
            } else {
              const std::vector<JsonRecord> events = finishEvents();
              for (const JsonRecord& event : events) {
                rt::ByteChunk bytes = encode(event);
                co_await yield(std::move(bytes));
              }
            }
          } catch (const rt::GeneratorReturn&) {
            bodyReturning = true;
          } catch (...) {
            bodyFailure = std::current_exception();
          }
          if (bodyFailure) {
            // Leaving the loop by a throw closes the iterator and keeps the throw.
            try {
              co_await frames.return_();
            } catch (...) {
            }
            std::rethrow_exception(bodyFailure);
          }
          if (bodyReturning) {
            co_await frames.return_();
            throw rt::GeneratorReturn{};
          }
        }
      } catch (const rt::GeneratorReturn&) {
        returning = true;
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) {
        if (rt::isError(failure)) std::rethrow_exception(failure);
        throw ResponsesStreamError("Claude Chat stream failed.", {.cause = failure, .kind = std::nullopt});
      }
      if (returning) throw rt::GeneratorReturn{};
    }

    rt::AsyncGenerator<rt::ByteChunk> translateChatStream(rt::Response response, AnthropicChatResponsesOptions options, rt::AbortSignal signal) {
      return rt::AsyncGenerator<rt::ByteChunk>([response, options, signal](rt::Yield<rt::ByteChunk> yield) {
        return translateChatStreamBody(response, options, signal, std::move(yield));
      });
    }

    struct TranslatedStreamState {
      rt::AsyncGenerator<rt::ByteChunk> iterator;
      rt::AbortController cancellation;
      rt::Response response;
      bool started = false;
    };

    rt::Task<void> translatedPull(std::shared_ptr<TranslatedStreamState> state, Controller controller) {
      state->started = true;
      rt::Reason failure;
      try {
        rt::IteratorResult<rt::ByteChunk> next = co_await state->iterator.next();
        if (next.done) controller.close();
        else controller.enqueue(std::move(*next.value));
      } catch (...) {
        failure = std::current_exception();
      }
      if (failure) controller.error(failure);
    }

    rt::Task<void> translatedCancel(std::shared_ptr<TranslatedStreamState> state, rt::Reason reason) {
      state->cancellation.abort(reason);
      if (!state->started) {
        // `await response.body?.cancel(reason).catch(() => {})`
        const std::optional<rt::ByteStream> body = state->response.body();
        if (body) {
          rt::Task<void> cancelled = body->cancel(reason);
          try {
            co_await std::move(cancelled);
          } catch (...) {
          }
        }
        co_await rt::microtask();
      }
      try {
        co_await state->iterator.return_();
      } catch (...) {
      }
      co_await rt::microtask();
    }

    rt::ResponseInit responseInitFrom(const rt::Response& response, rt::Headers headers) {
      return rt::ResponseInit{.status = response.status(), .statusText = response.statusText(), .headers = std::move(headers)};
    }
  }

  rt::Task<rt::Response> normalizeAnthropicChatResponsesResponse(rt::Response response, AnthropicChatResponsesOptions options) {
    if (!response.ok()) co_return response;
    rt::Headers headers(response.headers());
    headers.delete_("content-length");
    headers.delete_("content-encoding");
    const std::optional<std::string> contentType = headers.get("content-type");
    if (contentType && rt::includes(rt::toLowerCase(*contentType), "text/event-stream")) {
      const rt::AbortController cancellation;
      std::vector<rt::AbortSignal> sources;
      if (options.signal) sources = {*options.signal, cancellation.signal()};
      const rt::AbortSignal signal = options.signal ? rt::AbortSignal::any(sources) : cancellation.signal();
      auto state = std::make_shared<TranslatedStreamState>(
        TranslatedStreamState{.iterator = translateChatStream(response, options, signal), .cancellation = cancellation, .response = response, .started = false});
      rt::UnderlyingSource<rt::ByteChunk> source;
      source.pull = [state](Controller controller) { return translatedPull(state, std::move(controller)); };
      source.cancel = [state](rt::Reason reason) { return translatedCancel(state, std::move(reason)); };
      const rt::Response translated(rt::BodyInit(rt::ByteStream(std::move(source))), responseInitFrom(response, std::move(headers)));
      co_return translated;
    }
    bounded_response_body::ReadBoundedResponseBodyOptions readOptions;
    readOptions.signal = options.signal;
    readOptions.maxBytes = MAX_RESPONSES_SSE_EVENT_BYTES;
    readOptions.timeoutMs = inference_deadline::BUFFERED_INFERENCE_DEADLINE_MS;
    auto reading = bounded_response_body::readBoundedResponseBody(response, std::move(readOptions));
    const bounded_response_body::BoundedResponseBody captured = co_await std::move(reading);
    if (options.signal && options.signal->aborted()) {
      if (rt::isError(options.signal->reason())) rt::rethrow(options.signal->reason());
      throw rt::DOMException("Claude Chat was aborted.", "AbortError");
    }
    if (!captured.complete) throw ResponsesStreamError("Claude Chat body was incomplete or oversized.", {.cause = nullptr, .kind = "read_error"});
    json::value completion;
    try {
      completion = rt::jsonParse(rt::decodeUtf8(captured.bytes));
    } catch (...) {
      throw ResponsesStreamError("Claude Chat returned malformed JSON.", {.cause = std::current_exception(), .kind = "malformed_event"});
    }
    headers.set("content-type", "application/json");
    const JsonRecord payload = bufferedPayload(completion, options);
    const rt::Response normalized(rt::BodyInit(rt::jsonStringify(json::value(payload))), responseInitFrom(response, std::move(headers)));
    co_return normalized;
  }
}
