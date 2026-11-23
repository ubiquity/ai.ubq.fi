#include "uos/provider/anthropic_responses.hpp"

#include "uos/bounded_response_body.hpp"
#include "uos/inference_deadline.hpp"
#include "uos/provider/anthropic_replay.hpp"
#include "uos/responses_stream.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/crypto.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/utils.hpp"

#include <exception>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

namespace uos::provider::anthropic_responses {
  namespace {
    using JsonRecord = json::object;
    using anthropic_response_wire::ClaudeToolName;
    using responses_stream::MAX_RESPONSES_SSE_EVENT_BYTES;
    using responses_stream::ResponsesStreamError;
    using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;
    using ReadResult = rt::ReadableStreamReadResult<rt::ByteChunk>;

    json::value text(std::string_view value) { return json::value(json::string(value)); }
    json::value numberOrNull(const std::optional<double>& value) { return value ? rt::makeNumber(*value) : json::value(nullptr); }
    bool equalsString(const json::value* value, std::string_view expected) { return value && value->is_string() && value->get_string() == expected; }

    bool record(const json::value* value) { return utils::isRecord(value) && !value->is_array(); }
    bool has(const JsonRecord& value, std::string_view key) { return rt::hasOwn(value, key); }
    std::optional<double> countNumber(double value) {
      return rt::Number::isSafeInteger(value) && value >= 0 ? std::optional<double>(value) : std::nullopt;
    }
    std::optional<double> count(const json::value* value) {
      if (!value || !rt::isNumber(*value)) return std::nullopt;
      return countNumber(*rt::getNumber(*value));
    }
    ResponsesStreamError malformed(std::string message) { return ResponsesStreamError(std::move(message), {.cause = nullptr, .kind = "malformed_event"}); }

    std::string withoutDashes(const std::string& value) {
      std::string out;
      for (char character : value)
        if (character != '-') out += character;
      return out;
    }

    std::string responseIdFor(std::string_view id) {
      static const rt::Regex NON_ALPHANUMERIC("[^A-Za-z0-9]", "g");
      const std::string cleaned = rt::slice(rt::replace(id, NON_ALPHANUMERIC, ""), 0, 40);
      return "resp_" + (cleaned.empty() ? withoutDashes(rt::crypto::randomUUID()) : cleaned);
    }

    std::optional<JsonRecord> nativeReasoningDetails(const JsonRecord& value, const std::optional<double>& output) {
      const json::value* raw = rt::get(value, "output_tokens_details");
      const JsonRecord* details = record(raw) ? &raw->get_object() : nullptr;
      if (!details || !has(*details, "thinking_tokens")) return std::nullopt;
      const std::optional<double> thinking = count(rt::get(*details, "thinking_tokens"));
      const std::optional<double> reasoning = thinking && (!output || *thinking <= *output) ? thinking : std::nullopt;
      JsonRecord result;
      rt::objectDefine(result, "reasoning_tokens", numberOrNull(reasoning));
      return result;
    }
  }

  std::optional<json::object> toAnthropicResponsesUsage(const json::value* value) {
    if (!record(value)) return std::nullopt;
    const JsonRecord& usage = value->get_object();
    const std::optional<double> net = count(rt::get(usage, "input_tokens"));
    const std::optional<double> output = count(rt::get(usage, "output_tokens"));
    const std::optional<double> read = count(rt::get(usage, "cache_read_input_tokens"));
    const std::optional<double> write = count(rt::get(usage, "cache_creation_input_tokens"));
    const bool invalidCache = (has(usage, "cache_read_input_tokens") && !read) || (has(usage, "cache_creation_input_tokens") && !write);
    const std::optional<double> gross = !net || invalidCache ? std::nullopt : countNumber(*net + read.value_or(0) + write.value_or(0));
    JsonRecord details;
    if (has(usage, "cache_read_input_tokens")) rt::objectDefine(details, "cached_tokens", numberOrNull(read));
    if (has(usage, "cache_creation_input_tokens")) rt::objectDefine(details, "cache_write_tokens", numberOrNull(write));
    const std::optional<JsonRecord> outputDetails = nativeReasoningDetails(usage, output);
    JsonRecord result;
    rt::objectDefine(result, "input_tokens", numberOrNull(gross));
    if (!details.empty()) rt::objectDefine(result, "input_tokens_details", std::move(details));
    rt::objectDefine(result, "output_tokens", numberOrNull(output));
    if (outputDetails) rt::objectDefine(result, "output_tokens_details", *outputDetails);
    rt::objectDefine(result, "total_tokens", numberOrNull(!gross || !output ? std::nullopt : countNumber(*gross + *output)));
    if (has(usage, "buyer_cost_micro")) rt::objectDefine(result, "buyer_cost_micro", *rt::get(usage, "buyer_cost_micro"));
    return result;
  }

  std::optional<json::object> toAnthropicResponsesUsage(const json::value& value) { return toAnthropicResponsesUsage(&value); }

  namespace {
    struct ContentBlock {
      double nativeIndex;
      double outputIndex;
      // "text" | "thinking" | "tool_use" | "redacted_thinking"
      std::string kind;
      std::string id;
      std::string text;
      std::string callId;
      std::string name;
      std::string args;
      JsonRecord input;
      bool argumentDelta;
      bool suppressed;
      bool closed;
      JsonRecord native;
    };

    std::string stringField(const JsonRecord& value, std::string_view key) {
      const json::value* field = rt::get(value, key);
      if (!field || !field->is_string()) throw malformed("Anthropic Messages " + std::string(key) + " must be a string.");
      return std::string(field->get_string());
    }

    double blockIndex(const JsonRecord& value) {
      const std::optional<double> index = count(rt::get(value, "index"));
      if (!index) throw malformed("Anthropic Messages content block index must be a nonnegative integer.");
      return *index;
    }

    std::string itemPrefix(std::string_view kind) {
      if (kind == "text") return "msg";
      if (kind == "thinking") return "rs";
      return "fc";
    }

    ContentBlock newContentBlock(double nativeIndex, double outputIndex, const std::string& responseId, const JsonRecord& content, const AnthropicResponsesOptions& options) {
      const json::value* kindValue = rt::get(content, "type");
      if (!equalsString(kindValue, "text") && !equalsString(kindValue, "thinking") && !equalsString(kindValue, "tool_use") && !equalsString(kindValue, "redacted_thinking")) {
        throw malformed("Anthropic Messages emitted an unsupported content block.");
      }
      const std::string kind(kindValue->get_string());
      ContentBlock block{
        .nativeIndex = nativeIndex,
        .outputIndex = outputIndex,
        .kind = kind,
        .id = itemPrefix(kind) + "_" + responseId + "_" + rt::numberToString(nativeIndex),
        .text = "",
        .callId = "",
        .name = "",
        .args = "",
        .input = {},
        .argumentDelta = false,
        .suppressed = kind == "redacted_thinking" || (kind == "thinking" && options.reasoningSummary == std::optional<std::string>("none")),
        .closed = false,
        .native = rt::objectSpread(content),
      };
      if (kind == "tool_use") {
        block.callId = stringField(content, "id");
        block.name = stringField(content, "name");
        const json::value* input = rt::get(content, "input");
        if (block.callId.empty() || block.name.empty() || !record(input)) throw malformed("Anthropic Messages tool_use must have an id, name and object input.");
        block.input = input->get_object();
      }
      return block;
    }

    std::string initialBlockText(std::string_view kind, const JsonRecord& content) {
      if (kind == "text") return stringField(content, "text");
      if (kind == "thinking") return stringField(content, "thinking");
      return "";
    }

    /** Retain signatures privately and reconstruct tool inputs only when complete. */
    std::optional<JsonRecord> nativeReplayBlock(const ContentBlock& block) {
      JsonRecord native = rt::objectSpread(block.native);
      if (block.kind == "text") rt::objectSet(native, "text", text(block.text));
      if (block.kind == "thinking") rt::objectSet(native, "thinking", text(block.text));
      if (block.kind != "tool_use" || !block.argumentDelta) return native;
      json::value input;
      try {
        input = rt::jsonParse(block.args);
      } catch (...) {
        return std::nullopt;
      }
      if (!record(&input)) return std::nullopt;
      JsonRecord withInput = rt::objectSpread(native);
      rt::objectDefine(withInput, "input", std::move(input));
      return withInput;
    }

    // "completed" | "incomplete" | "failed"; `reason: unknown` (std::nullopt is undefined).
    std::string stopStatus(const std::optional<json::value>& reason) {
      const json::value* value = reason ? &*reason : nullptr;
      if (equalsString(value, "end_turn") || equalsString(value, "tool_use") || equalsString(value, "stop_sequence")) return "completed";
      if (equalsString(value, "max_tokens")) return "incomplete";
      return "failed";
    }

    std::string nativeErrorCode(const JsonRecord& error) {
      if (const std::optional<std::string_view> code = rt::getString(error, "code")) return std::string(*code);
      if (const std::optional<std::string_view> type = rt::getString(error, "type")) return std::string(*type);
      return "anthropic_upstream_error";
    }

    const JsonRecord& objectField(const JsonRecord& value, std::string_view key) {
      const json::value* child = rt::get(value, key);
      if (!record(child)) throw malformed("Anthropic Messages event has no object " + std::string(key) + ".");
      return child->get_object();
    }

    /** Native blocks are closed before the next output item, matching Codex's single active item. */
    class MessagesTranslator {
    public:
      explicit MessagesTranslator(AnthropicResponsesOptions options_)
        : options(std::move(options_)),
          responseId(responseIdFor(options.requestId ? *options.requestId : rt::crypto::randomUUID())),
          createdAt(rt::Math::floor(rt::dateNow() / 1000)) {}

      std::vector<JsonRecord> push(const JsonRecord& event) {
        if (terminal) throw malformed("Anthropic Messages emitted an event after its terminal.");
        const json::value* type = rt::get(event, "type");
        if (equalsString(type, "ping")) return {};
        if (!started && !equalsString(type, "message_start") && !equalsString(type, "error")) throw malformed("Anthropic Messages emitted content before message_start.");
        if (equalsString(type, "error")) {
          const JsonRecord& error = objectField(event, "error");
          const std::string message = stringField(error, "message");
          const std::string code = nativeErrorCode(error);
          terminal = true;
          std::vector<JsonRecord> events{JsonRecord{{"type", "error"}, {"code", code}, {"message", message}, {"param", nullptr}}};
          return numbered(std::move(events));
        }
        if (equalsString(type, "message_start")) return start(objectField(event, "message"));
        if (equalsString(type, "content_block_start")) {
          const double index = blockIndex(event);
          return open(index, objectField(event, "content_block"));
        }
        if (equalsString(type, "content_block_delta")) {
          const double index = blockIndex(event);
          return delta(index, objectField(event, "delta"));
        }
        if (equalsString(type, "content_block_stop")) return close(blockIndex(event));
        if (equalsString(type, "message_delta")) {
          const JsonRecord& change = objectField(event, "delta");
          if (has(change, "stop_reason")) stopReason = *rt::get(change, "stop_reason");
          if (has(event, "usage")) {
            const json::value* eventUsage = rt::get(event, "usage");
            if (!record(eventUsage)) throw malformed("Anthropic Messages message_delta has invalid usage.");
            usage = rt::objectSpread(usage ? *usage : JsonRecord(), eventUsage->get_object());
          }
          if (!has(event, "usage")) return {};
          std::vector<JsonRecord> events{JsonRecord{{"type", "response.in_progress"}, {"response", inProgressResponse()}}};
          return numbered(std::move(events));
        }
        if (equalsString(type, "message_stop")) return finish();
        throw malformed("Anthropic Messages emitted an unsupported event type.");
      }

      JsonRecord buffered(const json::value& message) {
        const json::array* content = record(&message) ? rt::getArray(message, "content") : nullptr;
        if (!content) throw malformed("Anthropic Messages returned an invalid message body.");
        start(message.get_object());
        for (std::size_t index = 0; index < content->size(); ++index) {
          const json::value& block = (*content)[index];
          if (!record(&block)) throw malformed("Anthropic Messages returned a non-object content block.");
          open(static_cast<double>(index), block.get_object());
          close(static_cast<double>(index));
        }
        finish();
        if (!finalResponse) throw malformed("Anthropic Messages did not produce a terminal response.");
        return *finalResponse;
      }

      bool done() const { return terminal; }

    private:
      JsonRecord envelope(std::string_view status) const {
        return anthropic_response_wire::claudeResponsesEnvelope(responseId, options.model, createdAt, status, echo);
      }

      std::vector<JsonRecord> numbered(std::vector<JsonRecord> events) {
        std::vector<JsonRecord> result;
        for (const JsonRecord& event : events) {
          JsonRecord value = rt::objectSpread(event);
          rt::objectDefine(value, "sequence_number", rt::makeNumber(sequence++));
          result.push_back(std::move(value));
        }
        return result;
      }

      json::value usageValue() const {
        const std::optional<JsonRecord> converted = usage ? toAnthropicResponsesUsage(json::value(*usage)) : std::nullopt;
        return converted ? json::value(*converted) : json::value(nullptr);
      }

      JsonRecord inProgressResponse() const {
        JsonRecord response = rt::objectSpread(envelope("in_progress"));
        rt::objectDefine(response, "usage", usageValue());
        return response;
      }

      bool anyOpenBlock() const {
        for (const ContentBlock& block : blocks.values())
          if (!block.closed) return true;
        return false;
      }

      std::vector<JsonRecord> start(const JsonRecord& message) {
        if (started || terminal) throw malformed("Anthropic Messages emitted more than one message_start.");
        if (!equalsString(rt::get(message, "type"), "message") || !equalsString(rt::get(message, "role"), "assistant"))
          throw malformed("Anthropic Messages returned an invalid assistant message.");
        const std::optional<std::string_view> id = rt::getString(message, "id");
        if (id && !id->empty()) responseId = responseIdFor(*id);
        started = true;
        const json::value* messageUsage = rt::get(message, "usage");
        usage = record(messageUsage) ? std::optional<JsonRecord>(rt::objectSpread(messageUsage->get_object())) : std::nullopt;
        if (has(message, "stop_reason")) stopReason = *rt::get(message, "stop_reason");
        std::vector<JsonRecord> events{JsonRecord{{"type", "response.created"}, {"response", envelope("in_progress")}},
                                       JsonRecord{{"type", "response.in_progress"}, {"response", inProgressResponse()}}};
        return numbered(std::move(events));
      }

      JsonRecord itemFor(const ContentBlock& block, std::string_view status) const {
        if (block.kind == "text") {
          return JsonRecord{{"id", block.id},
                            {"type", "message"},
                            {"status", status},
                            {"role", "assistant"},
                            {"content", json::array{JsonRecord{{"type", "output_text"}, {"text", block.text}, {"annotations", json::array()}}}}};
        }
        if (block.kind == "thinking") {
          return JsonRecord{
            {"id", block.id}, {"type", "reasoning"}, {"status", status}, {"summary", json::array{JsonRecord{{"type", "summary_text"}, {"text", block.text}}}}};
        }
        const ClaudeToolName* mapped = options.toolNames ? options.toolNames->get(block.name) : nullptr;
        const ClaudeToolName original = mapped ? *mapped : ClaudeToolName{.name = block.name, .namespace_ = std::nullopt};
        JsonRecord item;
        rt::objectDefine(item, "id", text(block.id));
        rt::objectDefine(item, "type", text("function_call"));
        rt::objectDefine(item, "status", text(status));
        rt::objectDefine(item, "call_id", text(block.callId));
        rt::objectDefine(item, "name", text(original.name));
        if (original.namespace_) rt::objectDefine(item, "namespace", text(*original.namespace_));
        rt::objectDefine(item, "arguments", text(block.args));
        return item;
      }

      std::vector<JsonRecord> open(double nativeIndex, const JsonRecord& content) {
        if (!started || terminal || blocks.has(nativeIndex) || anyOpenBlock()) throw malformed("Anthropic Messages content block lifecycle is invalid.");
        const double outputIndex = static_cast<double>(output.size());
        ContentBlock created = newContentBlock(nativeIndex, outputIndex, responseId, content, options);
        const std::string kind = created.kind;
        blocks.set(nativeIndex, std::move(created));
        ContentBlock& block = *blocks.get(nativeIndex);
        // Opaque redacted thinking and provider signatures have no public Responses
        // representation. They never become invented encrypted_content payloads.
        if (block.suppressed) {
          block.text = initialBlockText(kind, content);
          return {};
        }
        JsonRecord item = itemFor(block, "in_progress");
        if (kind == "text") rt::objectSet(item, "content", json::array());
        if (kind == "thinking") rt::objectSet(item, "summary", json::array());
        output.push_back(item);
        std::vector<JsonRecord> events{JsonRecord{{"type", "response.output_item.added"}, {"output_index", rt::makeNumber(outputIndex)}, {"item", item}}};
        if (kind == "text") {
          events.push_back(JsonRecord{{"type", "response.content_part.added"},
                                      {"item_id", block.id},
                                      {"output_index", rt::makeNumber(outputIndex)},
                                      {"content_index", 0},
                                      {"part", JsonRecord{{"type", "output_text"}, {"text", ""}, {"annotations", json::array()}}}});
        } else if (kind == "thinking") {
          events.push_back(JsonRecord{{"type", "response.reasoning_summary_part.added"},
                                      {"item_id", block.id},
                                      {"output_index", rt::makeNumber(outputIndex)},
                                      {"summary_index", 0},
                                      {"part", JsonRecord{{"type", "summary_text"}, {"text", ""}}}});
        }
        const std::string initial = initialBlockText(kind, content);
        std::vector<JsonRecord> numberedEvents = numbered(std::move(events));
        if (initial.empty()) return numberedEvents;
        JsonRecord initialDelta{{"type", kind == "text" ? "text_delta" : "thinking_delta"}};
        rt::objectDefine(initialDelta, kind, text(initial));
        std::vector<JsonRecord> deltaEvents = delta(nativeIndex, initialDelta);
        for (JsonRecord& event : deltaEvents) numberedEvents.push_back(std::move(event));
        return numberedEvents;
      }

      std::vector<JsonRecord> delta(double nativeIndex, const JsonRecord& value) {
        ContentBlock* block = blocks.get(nativeIndex);
        if (!block || block->closed || terminal) throw malformed("Anthropic Messages delta references no open content block.");
        const json::value* type = rt::get(value, "type");
        if (block->kind == "thinking" && equalsString(type, "signature_delta")) {
          const std::string signature = stringField(value, "signature");
          const std::string initial(rt::getString(block->native, "signature").value_or(""));
          rt::objectSet(block->native, "signature", text(rt::concat(initial, signature)));
          return {};
        }
        if (block->kind == "redacted_thinking") throw malformed("Anthropic Messages emitted a delta for redacted thinking.");
        if (block->kind == "text" && equalsString(type, "text_delta")) {
          const std::string deltaText = stringField(value, "text");
          rt::append(block->text, deltaText);
          std::vector<JsonRecord> events{JsonRecord{{"type", "response.output_text.delta"},
                                                    {"item_id", block->id},
                                                    {"output_index", rt::makeNumber(block->outputIndex)},
                                                    {"content_index", 0},
                                                    {"delta", deltaText},
                                                    {"logprobs", json::array()}}};
          return numbered(std::move(events));
        }
        if (block->kind == "thinking" && equalsString(type, "thinking_delta")) {
          const std::string deltaText = stringField(value, "thinking");
          rt::append(block->text, deltaText);
          if (block->suppressed) return {};
          std::vector<JsonRecord> events{JsonRecord{{"type", "response.reasoning_summary_text.delta"},
                                                    {"item_id", block->id},
                                                    {"output_index", rt::makeNumber(block->outputIndex)},
                                                    {"summary_index", 0},
                                                    {"delta", deltaText}}};
          return numbered(std::move(events));
        }
        if (block->kind == "tool_use" && equalsString(type, "input_json_delta")) {
          const std::string deltaText = stringField(value, "partial_json");
          block->argumentDelta = true;
          rt::append(block->args, deltaText);
          std::vector<JsonRecord> events{JsonRecord{
            {"type", "response.function_call_arguments.delta"}, {"item_id", block->id}, {"output_index", rt::makeNumber(block->outputIndex)}, {"delta", deltaText}}};
          return numbered(std::move(events));
        }
        throw malformed("Anthropic Messages content delta does not match its block type.");
      }

      std::vector<JsonRecord> close(double nativeIndex) {
        ContentBlock* block = blocks.get(nativeIndex);
        if (!block || block->closed || terminal) throw malformed("Anthropic Messages content_block_stop references no open block.");
        block->closed = true;
        if (block->suppressed) return {};
        const json::value itemId = text(block->id);
        const json::value outputIndex = rt::makeNumber(block->outputIndex);
        std::vector<JsonRecord> events;
        if (block->kind == "text") {
          events.push_back(JsonRecord{{"type", "response.output_text.done"},
                                      {"item_id", itemId},
                                      {"output_index", outputIndex},
                                      {"content_index", 0},
                                      {"text", block->text},
                                      {"logprobs", json::array()}});
          events.push_back(JsonRecord{{"type", "response.content_part.done"},
                                      {"item_id", itemId},
                                      {"output_index", outputIndex},
                                      {"content_index", 0},
                                      {"part", JsonRecord{{"type", "output_text"}, {"text", block->text}, {"annotations", json::array()}}}});
        } else if (block->kind == "thinking") {
          events.push_back(JsonRecord{
            {"type", "response.reasoning_summary_text.done"}, {"item_id", itemId}, {"output_index", outputIndex}, {"summary_index", 0}, {"text", block->text}});
          events.push_back(JsonRecord{{"type", "response.reasoning_summary_part.done"},
                                      {"item_id", itemId},
                                      {"output_index", outputIndex},
                                      {"summary_index", 0},
                                      {"part", JsonRecord{{"type", "summary_text"}, {"text", block->text}}}});
        } else {
          if (!block->argumentDelta) {
            block->args = rt::jsonStringify(json::value(block->input));
            events.push_back(JsonRecord{{"type", "response.function_call_arguments.delta"}, {"item_id", itemId}, {"output_index", outputIndex}, {"delta", block->args}});
          }
          events.push_back(JsonRecord{{"type", "response.function_call_arguments.done"}, {"item_id", itemId}, {"output_index", outputIndex}, {"arguments", block->args}});
        }
        JsonRecord item = itemFor(*block, "completed");
        output[static_cast<std::size_t>(block->outputIndex)] = item;
        events.push_back(JsonRecord{{"type", "response.output_item.done"}, {"output_index", outputIndex}, {"item", item}});
        return numbered(std::move(events));
      }

      void rememberCompletedTurn() {
        if (!rt::truthy(options.reasoningReplayScope) || !rt::truthy(options.provider)) return;
        json::array nativeBlocks;
        for (const ContentBlock& block : blocks.values()) {
          std::optional<JsonRecord> native = nativeReplayBlock(block);
          if (!native) return;
          nativeBlocks.push_back(std::move(*native));
        }
        anthropic_replay::ReplayPublication publication;
        publication.scope = *options.reasoningReplayScope;
        publication.provider = *options.provider;
        publication.model = options.model;
        for (const JsonRecord& item : output) publication.output.push_back(item);
        publication.nativeBlocks = std::move(nativeBlocks);
        anthropic_replay::rememberAnthropicReplay(publication);
      }

      std::vector<JsonRecord> finish() {
        if (!started || terminal || anyOpenBlock()) throw malformed("Anthropic Messages ended with an invalid message lifecycle.");
        terminal = true;
        const std::string status = stopStatus(stopReason);
        JsonRecord response = rt::objectSpread(envelope(status));
        json::array outputItems;
        for (const JsonRecord& item : output) outputItems.push_back(item);
        rt::objectDefine(response, "output", std::move(outputItems));
        rt::objectDefine(response, "usage", usageValue());
        if (status == "incomplete") rt::objectSet(response, "incomplete_details", JsonRecord{{"reason", "max_output_tokens"}});
        if (status == "failed") {
          rt::objectSet(response, "error",
                        JsonRecord{{"code", "anthropic_unrecognized_stop_reason"},
                                   {"message", "Anthropic Messages stopped with reason " + rt::toString(stopReason ? &*stopReason : nullptr) + "."}});
        }
        if (status == "completed") rememberCompletedTurn();
        finalResponse = response;
        std::vector<JsonRecord> events{JsonRecord{{"type", "response." + status}, {"response", response}}};
        return numbered(std::move(events));
      }

      AnthropicResponsesOptions options;
      std::string responseId;
      double createdAt;
      anthropic_response_wire::ClaudeResponseEcho echo;
      rt::OrderedMap<double, ContentBlock> blocks;
      std::vector<JsonRecord> output;
      bool started = false;
      bool terminal = false;
      double sequence = 0;
      std::optional<json::value> stopReason;
      std::optional<JsonRecord> usage;
      std::optional<JsonRecord> finalResponse;
    };

    std::optional<JsonRecord> parseFrame(const std::string& raw) {
      static const rt::Regex LINE_BREAK("\\r\\n|\\r|\\n");
      static const rt::Regex LEADING_SPACE("^ ");
      std::vector<std::string> data;
      for (const std::string& line : rt::split(raw, LINE_BREAK)) {
        if (line == "data") data.push_back("");
        else if (line.starts_with("data:")) data.push_back(rt::replace(rt::slice(line, 5), LEADING_SPACE, ""));
      }
      if (data.empty()) return std::nullopt;
      std::string joined;
      for (std::size_t index = 0; index < data.size(); ++index) {
        if (index) joined += "\n";
        joined += data[index];
      }
      json::value value;
      try {
        value = rt::jsonParse(joined);
      } catch (...) {
        throw ResponsesStreamError("Anthropic Messages emitted malformed SSE JSON.", {.cause = std::current_exception(), .kind = "malformed_event"});
      }
      if (!record(&value) || !rt::getString(value, "type")) throw malformed("Anthropic Messages emitted an SSE event without a type.");
      return std::move(value.get_object());
    }

    rt::Reason abortError(const rt::AbortSignal& signal) {
      return rt::isError(signal.reason()) ? signal.reason() : rt::makeReason(rt::DOMException("Anthropic Messages stream was aborted.", "AbortError"));
    }

    rt::Task<ReadResult> readWithAbort(rt::ReadableStreamDefaultReader<rt::ByteChunk> reader, rt::AbortSignal signal) {
      rt::EventListener abort = [] {};
      const rt::Promise<ReadResult> aborted = rt::newPromise<ReadResult>([&abort, &signal](const rt::Resolve<ReadResult>&, const rt::Reject& reject) {
        std::weak_ptr weakSignal = signal.state();
        abort = rt::EventListener([reject, weakSignal] {
          if (auto state = weakSignal.lock()) reject(abortError(rt::AbortSignal(state)));
        });
        signal.addEventListener("abort", abort, {.once = true});
        if (signal.aborted()) abort();
      });
      std::optional<ReadResult> result;
      rt::Reason failure;
      try {
        const rt::Promise<ReadResult> pending = reader.readPromise();
        std::vector<rt::Promise<ReadResult>> racers{pending, aborted};
        const rt::Promise<ReadResult> race = rt::race(std::move(racers));
        result = co_await race.wait();
      } catch (...) {
        failure = std::current_exception();
      }
      signal.removeEventListener("abort", abort);
      if (failure) {
        std::rethrow_exception(failure);
      }
      co_return std::move(*result);
    }

    // MessagesFrame: Readonly<{ kind: "event"; value }> | Readonly<{ kind: "comment" }>.
    struct MessagesFrameEvent {
      [[maybe_unused]] static constexpr std::string_view kind = "event";
      JsonRecord value;
    };
    struct MessagesFrameComment {
      [[maybe_unused]] static constexpr std::string_view kind = "comment";
    };
    using MessagesFrame = std::variant<MessagesFrameEvent, MessagesFrameComment>;

    void assertFrameBound(std::string_view value) {
      if (static_cast<double>(rt::utf8ByteLength(value)) > MAX_RESPONSES_SSE_EVENT_BYTES) {
        throw ResponsesStreamError("Anthropic Messages SSE event exceeded its bound.", {.cause = nullptr, .kind = "event_too_large"});
      }
    }

    /** One framing concern, shared by ordinary chunks and the final unterminated frame. */
    class MessagesFrameReader {
    public:
      // The state of one `frames(incoming)` generator; `incoming` is nullptr once the stream ended.
      struct Frames {
        const rt::ByteChunk* incoming;
        bool started = false;
        bool done = false;
      };

      std::optional<MessagesFrame> next(Frames& frames) {
        if (frames.done) return std::nullopt;
        if (!frames.started) {
          frames.started = true;
          buffer += frames.incoming ? decoder.decode(std::span<const std::uint8_t>(*frames.incoming), {.stream = true}) : decoder.decode();
        }
        static const rt::Regex BOUNDARY("\\r\\n\\r\\n|\\n\\n|\\r\\r");
        for (;;) {
          const std::optional<rt::RegexMatch> boundary = BOUNDARY.exec(buffer);
          if (!boundary && frames.incoming) {
            assertFrameBound(buffer);
            frames.done = true;
            return std::nullopt;
          }
          const std::size_t end = boundary ? boundary->byteIndex : buffer.size();
          const std::string raw = buffer.substr(0, end);
          assertFrameBound(raw);
          buffer = boundary ? buffer.substr(boundary->byteEndIndex()) : std::string();
          std::optional<JsonRecord> event = parseFrame(raw);
          if (!boundary) frames.done = true;
          if (event) return MessagesFrame(MessagesFrameEvent{.value = std::move(*event)});
          if (rt::trim(raw).starts_with(':')) return MessagesFrame(MessagesFrameComment{});
          if (!boundary) return std::nullopt;
        }
      }

    private:
      rt::TextDecoder decoder;
      std::string buffer;
    };

    rt::Reason streamReadFailure(const rt::Reason& error, const AnthropicResponsesOptions& options, const rt::AbortSignal& consumerAbort, const rt::AbortSignal& deadline) {
      if (options.signal && options.signal->aborted()) return abortError(*options.signal);
      if (consumerAbort.aborted()) return abortError(consumerAbort);
      if (deadline.aborted())
        return rt::makeReason(ResponsesStreamError("Anthropic Messages stream became inactive.", {.cause = error, .kind = "inactivity_timeout"}));
      if (responses_stream::responsesStreamErrorOf(error)) return error;
      return rt::makeReason(ResponsesStreamError("Anthropic Messages stream could not be read.", {.cause = error, .kind = std::nullopt}));
    }

    // The mutable state the abort listener and the watchdog share with the generator body.
    struct ReaderState {
      rt::ReadableStreamDefaultReader<rt::ByteChunk> reader;
      std::optional<rt::Timeout> watchdog;
      bool sawFrame = false;
      bool cancelled = false;

      void cancelReader(rt::Reason reason) {
        if (cancelled) return;
        cancelled = true;
        rt::spawn(reader.cancel(std::move(reason)), rt::ignoreRejection);
      }
    };

    /** Owns native framing, read deadlines and the upstream reader's cancellation. */
    rt::Task<void> readMessagesFramesBody(rt::Response response, AnthropicResponsesOptions options, rt::AbortSignal consumerAbort, rt::Yield<MessagesFrame> yield) {
      const std::optional<rt::ByteStream> body = response.body();
      if (!body) throw ResponsesStreamError("Anthropic Messages returned no stream body.", {.cause = nullptr, .kind = "premature_eof"});
      auto state = std::make_shared<ReaderState>(ReaderState{.reader = body->getReader(), .watchdog = std::nullopt, .sawFrame = false, .cancelled = false});
      MessagesFrameReader framer;
      const rt::AbortController deadline;
      std::vector<rt::AbortSignal> sources;
      if (options.signal) sources = {*options.signal, consumerAbort, deadline.signal()};
      else sources = {consumerAbort, deadline.signal()};
      const rt::AbortSignal signal = rt::AbortSignal::any(sources);
      std::weak_ptr weakSignal = signal.state();
      const rt::EventListener abort([state, weakSignal] {
        rt::Reason reason;
        if (auto signalState = weakSignal.lock()) reason = rt::AbortSignal(signalState).reason();
        state->cancelReader(reason);
      });
      signal.addEventListener("abort", abort, {.once = true});
      const auto armDeadline = [&state, &deadline] {
        rt::clearTimeout(state->watchdog);
        state->watchdog =
          rt::setTimeout([deadline] { deadline.abort(rt::makeReason(rt::DOMException("Anthropic Messages stream stalled.", "TimeoutError"))); },
                         state->sawFrame ? inference_deadline::STREAM_INACTIVITY_DEADLINE_MS : inference_deadline::STREAM_FIRST_EVENT_DEADLINE_MS);
      };
      armDeadline();
      rt::Reason failure;
      bool returning = false;
      try {
        for (;;) {
          if (signal.aborted()) rt::rethrow(abortError(signal));
          const ReadResult next = co_await readWithAbort(state->reader, signal);
          MessagesFrameReader::Frames frames{.incoming = next.done ? nullptr : &*next.value};
          for (;;) {
            std::optional<MessagesFrame> frame = framer.next(frames);
            if (!frame) break;
            if (std::holds_alternative<MessagesFrameEvent>(*frame)) state->sawFrame = true;
            armDeadline();
            MessagesFrame yielded = std::move(*frame);
            co_await yield(std::move(yielded));
          }
          if (next.done) throw ResponsesStreamError("Anthropic Messages stream ended before message_stop.", {.cause = nullptr, .kind = "premature_eof"});
        }
      } catch (const rt::GeneratorReturn&) {
        returning = true;
      } catch (...) {
        failure = std::current_exception();
      }
      const rt::Reason thrown = failure ? streamReadFailure(failure, options, consumerAbort, deadline.signal()) : nullptr;
      rt::clearTimeout(state->watchdog);
      signal.removeEventListener("abort", abort);
      state->cancelReader(rt::thrown(json::value("Anthropic Messages stream consumer finished")));
      state->reader.releaseLock();
      if (thrown) std::rethrow_exception(thrown);
      if (returning) throw rt::GeneratorReturn{};
    }

    rt::AsyncGenerator<MessagesFrame> readMessagesFrames(rt::Response response, AnthropicResponsesOptions options, rt::AbortSignal consumerAbort) {
      return rt::AsyncGenerator<MessagesFrame>([response, options, consumerAbort](rt::Yield<MessagesFrame> yield) {
        return readMessagesFramesBody(response, options, consumerAbort, std::move(yield));
      });
    }

    /** Progressive translation stops only at an actual native message_stop or error. */
    rt::Task<void> translatedMessagesStreamBody(rt::Response response, AnthropicResponsesOptions options, rt::AbortSignal consumerAbort, rt::Yield<rt::ByteChunk> yield) {
      MessagesTranslator translator(options);
      const rt::AsyncGenerator<MessagesFrame> frames = readMessagesFrames(response, options, consumerAbort);
      // for await (const frame of frames) { ... }
      for (;;) {
        rt::IteratorResult<MessagesFrame> step = co_await frames.next();
        if (step.done) co_return;
        rt::Reason failure;
        bool returning = false;
        bool finished = false;
        try {
          if (std::holds_alternative<MessagesFrameComment>(*step.value)) {
            rt::ByteChunk keepalive = rt::encodeUtf8(": keepalive\n\n");
            co_await yield(std::move(keepalive));
          } else {
            const std::vector<JsonRecord> translated = translator.push(std::get<MessagesFrameEvent>(*step.value).value);
            for (const JsonRecord& event : translated) {
              rt::ByteChunk encoded = rt::encodeUtf8(anthropic_response_wire::encodeClaudeResponsesEvent(event));
              co_await yield(std::move(encoded));
            }
            finished = translator.done();
          }
        } catch (const rt::GeneratorReturn&) {
          returning = true;
        } catch (...) {
          failure = std::current_exception();
        }
        if (failure) {
          // Leaving the loop by a throw closes the iterator and keeps the throw.
          try {
            co_await frames.return_();
          } catch (...) {
          }
          std::rethrow_exception(failure);
        }
        if (returning || finished) {
          co_await frames.return_();
          if (returning) throw rt::GeneratorReturn{};
          co_return;
        }
      }
    }

    rt::AsyncGenerator<rt::ByteChunk> translatedMessagesStream(rt::Response response, AnthropicResponsesOptions options, rt::AbortSignal consumerAbort) {
      return rt::AsyncGenerator<rt::ByteChunk>([response, options, consumerAbort](rt::Yield<rt::ByteChunk> yield) {
        return translatedMessagesStreamBody(response, options, consumerAbort, std::move(yield));
      });
    }

    rt::ResponseInit responseInitFrom(const rt::Response& response, rt::Headers headers) {
      return rt::ResponseInit{.status = response.status(), .statusText = response.statusText(), .headers = std::move(headers)};
    }

    /** Normalize only recognizable native error JSON; leave other HTTP bodies intact. */
    rt::Task<rt::Response> normalizeMessagesHttpError(rt::Response response, AnthropicResponsesOptions options) {
      bounded_response_body::ReadBoundedResponseBodyOptions readOptions;
      readOptions.signal = options.signal;
      const rt::Response cloned = response.clone();
      auto reading = bounded_response_body::readBoundedResponseBody(cloned, std::move(readOptions));
      const bounded_response_body::BoundedResponseBody body = co_await std::move(reading);
      if (options.signal && options.signal->aborted()) rt::rethrow(abortError(*options.signal));
      if (!body.complete) co_return response;
      json::value value;
      bool parsed = true;
      try {
        value = rt::jsonParse(rt::decodeUtf8(body.bytes));
      } catch (...) {
        parsed = false;
      }
      if (!parsed) co_return response;
      const json::value* nested = rt::get(value, "error");
      if (!record(&value) || !equalsString(rt::get(value, "type"), "error") || !record(nested) || !rt::getString(*nested, "message")) co_return response;
      const JsonRecord& source = nested->get_object();
      const std::string type(rt::getString(source, "type").value_or("api_error"));
      JsonRecord error;
      rt::objectDefine(error, "message", *rt::get(source, "message"));
      rt::objectDefine(error, "type", text(type));
      rt::objectDefine(error, "code", text(rt::getString(source, "code").value_or(type)));
      const json::value* param = rt::get(source, "param");
      if (param && (param->is_null() || param->is_string())) rt::objectDefine(error, "param", *param);
      rt::Headers headers(response.headers());
      headers.delete_("content-length");
      headers.delete_("content-encoding");
      headers.set("content-type", "application/json");
      if (const std::optional<rt::ByteStream> originalBody = response.body()) {
        rt::spawn(originalBody->cancel(rt::thrown(json::value("Native Messages HTTP error normalized"))), rt::ignoreRejection);
      }
      JsonRecord payload;
      rt::objectDefine(payload, "error", std::move(error));
      const rt::Response normalized(rt::BodyInit(rt::jsonStringify(json::value(std::move(payload)))), responseInitFrom(response, std::move(headers)));
      co_return normalized;
    }

    struct TranslatedStreamState {
      rt::AsyncGenerator<rt::ByteChunk> iterator;
      rt::AbortController cancellation;
      rt::Response response;
      bool iteratorStarted = false;
    };

    rt::Task<void> translatedPull(std::shared_ptr<TranslatedStreamState> state, Controller controller) {
      state->iteratorStarted = true;
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
      if (!state->iteratorStarted) {
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
  }

  rt::Task<rt::Response> normalizeAnthropicResponsesResponse(rt::Response response, AnthropicResponsesOptions options) {
    if (!response.ok()) {
      auto normalizing = normalizeMessagesHttpError(response, options);
      rt::Response normalized = co_await std::move(normalizing);
      co_return normalized;
    }
    rt::Headers headers(response.headers());
    headers.delete_("content-length");
    headers.delete_("content-encoding");
    const std::optional<std::string> contentType = headers.get("content-type");
    if (contentType && rt::includes(rt::toLowerCase(*contentType), "text/event-stream")) {
      const rt::AbortController cancellation;
      auto state = std::make_shared<TranslatedStreamState>(TranslatedStreamState{.iterator = translatedMessagesStream(response, options, cancellation.signal()),
                                                                                 .cancellation = cancellation,
                                                                                 .response = response,
                                                                                 .iteratorStarted = false});
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
    const bounded_response_body::BoundedResponseBody body = co_await std::move(reading);
    if (options.signal && options.signal->aborted()) rt::rethrow(abortError(*options.signal));
    if (!body.complete) throw ResponsesStreamError("Anthropic Messages body was incomplete or exceeded its bound.", {.cause = nullptr, .kind = "read_error"});
    json::value message;
    try {
      message = rt::jsonParse(rt::decodeUtf8(body.bytes));
    } catch (...) {
      throw ResponsesStreamError("Anthropic Messages returned malformed JSON.", {.cause = std::current_exception(), .kind = "malformed_event"});
    }
    MessagesTranslator translator(options);
    const JsonRecord translated = translator.buffered(message);
    headers.set("content-type", "application/json");
    const rt::Response normalized(rt::BodyInit(rt::jsonStringify(json::value(translated))), responseInitFrom(response, std::move(headers)));
    co_return normalized;
  }
}
