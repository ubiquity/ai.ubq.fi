#include "uos/deepseek/chat_projection.hpp"
#include "uos/deepseek/index.hpp"
#include "uos/utils.hpp"
#include "uos/rt/console.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include <algorithm>

namespace uos::deepseek::chat_projection {
namespace {
  using namespace forwarded_payload_policy;
  using responses::failure;
  using Failure = responses::DeepSeekResponsesFailure;
  template<class T> using R = responses::DeepSeekResponsesResult<T>;
  template<class T> responses::DeepSeekResponsesResultOk<T> good(T value) { return {std::move(value)}; }
  template<class T> const Failure* failed(const R<T>& value) { return std::get_if<Failure>(&value); }
  template<class T> T& result(R<T>& value) { return std::get<responses::DeepSeekResponsesResultOk<T>>(value).value; }
  template<class T> std::optional<std::string> stringField(const T& value, std::string_view key) { return utils::getString(rt::get(value, key)); }
  using Elisions = std::vector<ForwardedPayloadElision>;
  using Exemptions = std::vector<ForwardedPayloadExemption>;
  struct ChatPart { std::optional<std::string> text{}; std::optional<json::object> image{}; };
  struct ReducedPayload { std::string content; ForwardedPayloadElision elision; };
  double forwardedByteLength(const std::string& value);
  /** The same declaration for an image that cannot be forwarded as an image. */
  std::string omittedImageMarker(double bytes);
  /**
   * The fail-closed answer when the request forbade reduction: the gateway
   * refuses with the item path, the byte counts, the declared limit and the
   * supported recovery, instead of mutating the input.
   */
  Failure oversizedPayloadFailure(const std::string& path, double bytes, const std::optional<std::string>& callId);
  R<json::value> boundedChatContent(const json::value& content, const std::string& path, const std::string& reduction, Elisions& elisions);
  R<std::string> boundedForwardedText(const std::string& value, const std::string& path, const std::string& reduction, Elisions& elisions,
    double limit = FORWARDED_PAYLOAD_POLICY.perMessageLimit, const std::string& kind = "message");
  /** One Responses content part mapped onto the Chat content union for one message role. */
  R<ChatPart> chatContentPart(const json::value& part, const std::string& role, const std::string& reduction, const std::string& path, Elisions& elisions) {
    if (!part.is_object()) return failure("input.content", "input.content items must be objects");
    const auto type = stringField(part, "type");
    if (type == "input_text" || type == "output_text" || type == "text") {
      const auto text = stringField(part, "text");
      if (!text) return failure("input.content.text", "input.content text must be a string");
      return good(ChatPart{*text, std::nullopt});
    }
    // Refusals emitted by this adapter replay only as assistant history.
    if (type == "refusal" && role == "assistant") {
      const auto refusal = stringField(part, "refusal");
      if (!refusal) return failure("input.content.refusal", "input.content refusal must be a string");
      return good(ChatPart{*refusal, std::nullopt});
    }
    if (type != "input_image") return failure("input.content.type", "input.content type '" + type.value_or("unknown") + "' is not supported");
    auto url = stringField(part, "image_url");
    if (!url) url = stringField(part, "file_url");
    if (!url || url->empty()) return failure("input.content.image_url", "input_image requires image_url");
    const auto detail = stringField(part, "detail");
    const auto bytes = forwardedByteLength(*url);
    if (bytes > FORWARDED_PAYLOAD_POLICY.perMessageLimit) {
      if (reduction == "reject") return oversizedPayloadFailure(path, bytes, std::nullopt);
      const auto marker = omittedImageMarker(bytes);
      elisions.push_back({path, std::nullopt, "image", bytes, forwardedByteLength(marker), bytes});
      return good(ChatPart{marker, std::nullopt});
    }
    json::object image{{"url", *url}};
    if (detail && !detail->empty()) image["detail"] = *detail;
    return good(ChatPart{std::nullopt, json::object{{"type", "image_url"}, {"image_url", std::move(image)}}});
  }
  /** Chat Completions content parts are strings or image parts; Responses nests text. */
  R<json::value> chatContentFromResponseParts(const json::value* value, const std::string& role, const std::string& reduction, const std::string& path, Elisions& elisions, bool exemptFromLimit = false) {
    const auto contentPath = path + ".content";
    if (value && value->is_string()) return boundedChatContent(*value, contentPath, reduction, elisions);
    if (!value || !value->is_array()) return failure("input.content", "input.content must be a string or an array");
    json::array images;
    std::vector<std::string> texts;
    for (std::size_t i = 0; i < value->as_array().size(); ++i) {
      auto part = chatContentPart(value->as_array()[i], role, reduction, path + ".content[" + rt::numberToString(static_cast<double>(i)) + "]", elisions);
      if (auto error = failed(part)) return *error;
      if (result(part).text) texts.push_back(*result(part).text);
      if (result(part).image) images.push_back(*result(part).image);
    }
    if (images.empty()) {
      const json::value text(rt::join(texts, ""));
      return exemptFromLimit ? R<json::value>(good(text)) : boundedChatContent(text, contentPath, reduction, elisions);
    }
    if (!texts.empty()) images.insert(images.begin(), json::object{{"type", "text"}, {"text", rt::join(texts, "")}});
    return boundedChatContent(images, contentPath, reduction, elisions);
  }
  R<std::string> boundedToolArguments(const std::string& value, const std::string& path, const std::string& callId, const std::string& reduction, Elisions& elisions) {
    const auto bytes = forwardedByteLength(value);
    if (bytes <= FORWARDED_PAYLOAD_POLICY.perMessageLimit) return good(value);
    if (reduction == "reject") return oversizedPayloadFailure(path, bytes, callId);
    const auto reduced = reduceToolArgumentsPayload(value, path, callId, FORWARDED_PAYLOAD_POLICY.perMessageLimit);
    if (!reduced) return oversizedPayloadFailure(path, bytes, callId);
    elisions.push_back(reduced->elision);
    return good(reduced->content);
  }
  R<json::object> chatToolCallItem(const json::object& item, const std::string& reduction, const std::string& path, Elisions& elisions) {
    auto callId = stringField(item, "call_id"); if (!callId) callId = stringField(item, "id");
    const auto name = stringField(item, "name");
    if (!callId || callId->empty() || !name || name->empty()) return failure("input", "function_call items require call_id and name");
    auto args = boundedToolArguments(stringField(item, "arguments").value_or("{}"), path + ".arguments", *callId, reduction, elisions);
    if (auto error = failed(args)) return *error;
    return good(json::object{{"id", *callId}, {"type", "function"}, {"function", json::object{{"name", *name}, {"arguments", result(args)}}}});
  }
  double forwardedByteLength(const std::string& value) { return static_cast<double>(rt::utf8ByteLength(value)); }
  std::optional<std::string> chatImageUrl(const json::value& part) {
    if (stringField(part, "type") != "image_url" || !rt::getObject(part, "image_url")) return std::nullopt;
    return stringField(*rt::getObject(part, "image_url"), "url");
  }
  double chatContentBytes(const json::value& content) {
    if (content.is_string()) return forwardedByteLength(rt::toString(content));
    double bytes = 0;
    for (const auto& part : content.as_array()) {
      if (stringField(part, "type") == "text") { if (auto text = stringField(part, "text")) { bytes += forwardedByteLength(*text); continue; } }
      if (auto url = chatImageUrl(part)) bytes += forwardedByteLength(*url);
    }
    return bytes;
  }
  /**
   * The visible notice an elided payload carries. It names the declared policy
   * version and the byte counts, and it says outright that the model did not
   * receive the elided bytes, so no transcript can mistake a reduced payload for
   * the original.
   */
  std::string forwardedPayloadMarker(double bytes) {
    return "\n[gateway: " + std::string(FORWARDED_PAYLOAD_POLICY.version) + " elided " + rt::numberToString(bytes) +
      " bytes omitted; the model did not receive the elided bytes; this route forwards at most " + rt::numberToString(FORWARDED_PAYLOAD_POLICY.perMessageLimit) + " bytes per message because the provider counts forwarded payloads as text tokens]";
  }
  std::string omittedImageMarker(double bytes) {
    return "[gateway: " + std::string(FORWARDED_PAYLOAD_POLICY.version) + " image omitted (" + rt::numberToString(bytes) +
      " bytes); the model did not receive this image; this route forwards at most " + rt::numberToString(FORWARDED_PAYLOAD_POLICY.perMessageLimit) + " bytes per message because the provider counts forwarded payloads as text tokens]";
  }
  /** Keeps every prefix cut at a complete UTF-16 code-point boundary. */
  std::string utf16Head(const std::string& value, double length) {
    auto head = rt::slice(value, 0, length);
    if (rt::length(head) < rt::length(value)) { const auto last = rt::charCodeAt(head, static_cast<double>(rt::length(head)) - 1); if (last >= 0xd800 && last <= 0xdbff) head = rt::slice(head, 0, -1); }
    return head;
  }
  /** Cuts a string to a UTF-8 byte budget without splitting a surrogate pair. */
  std::string utf8Head(const std::string& value, double byteBudget) {
    auto head = utf16Head(value, byteBudget);
    while (!head.empty() && forwardedByteLength(head) > byteBudget) head = utf16Head(head, rt::Math::floor(static_cast<double>(rt::length(head)) * 0.9));
    return head;
  }
  Failure oversizedPayloadFailure(const std::string& path, double bytes, const std::optional<std::string>& callId) {
    const auto note = callId ? " (tool call " + *callId + ")" : "";
    return failure(path, "input item " + path + note + " carries " + rt::numberToString(bytes) + " bytes; this route forwards at most " + rt::numberToString(FORWARDED_PAYLOAD_POLICY.perMessageLimit) +
      " bytes per message under " + std::string(FORWARDED_PAYLOAD_POLICY.version) + ", and the request set truncation 'disabled', so the gateway will not reduce it. Reduce the payload or send truncation 'auto' to allow the declared reduction.", "context_length_exceeded");
  }
  /**
   * Deterministic reduction: keep a byte prefix, append the marker, and stay at
   * or below the declared limit. The same input always produces the same output.
   */
  ReducedPayload reduceForwardedPayload(const std::string& value, const std::string& path, const std::optional<std::string>& callId,
    const std::string& kind = "tool_output", double limit = FORWARDED_PAYLOAD_POLICY.perMessageLimit) {
    const auto original = forwardedByteLength(value);
    auto head = utf8Head(value, limit);
    for (;;) {
      const auto bytes = forwardedByteLength(head);
      const auto notice = forwardedPayloadMarker(original - bytes);
      if (head.empty() || bytes + forwardedByteLength(notice) <= limit) {
        const auto content = rt::concat(head, notice);
        return {content, {path, callId, kind, original, forwardedByteLength(content), original - bytes}};
      }
      head = utf16Head(head, rt::Math::floor(static_cast<double>(rt::length(head)) * 0.9));
    }
  }
  R<std::string> boundedForwardedText(const std::string& value, const std::string& path, const std::string& reduction, Elisions& elisions, double limit, const std::string& kind) {
    const auto bytes = forwardedByteLength(value);
    if (bytes <= limit) return good(value);
    if (reduction == "reject") return oversizedPayloadFailure(path, bytes, std::nullopt);
    auto reduced = reduceForwardedPayload(value, path, std::nullopt, kind, limit);
    elisions.push_back(reduced.elision);
    return good(reduced.content);
  }
  R<json::value> boundedChatContent(const json::value& content, const std::string& path, const std::string& reduction, Elisions& elisions) {
    const auto bytes = chatContentBytes(content);
    if (bytes <= FORWARDED_PAYLOAD_POLICY.perMessageLimit) return good(content);
    if (reduction == "reject") return oversizedPayloadFailure(path, bytes, std::nullopt);
    if (content.is_string()) { auto bounded = boundedForwardedText(rt::toString(content), path, reduction, elisions); if (auto error = failed(bounded)) return *error; return good(json::value(result(bounded))); }
    json::array parts = content.as_array();
    while (chatContentBytes(parts) > FORWARDED_PAYLOAD_POLICY.perMessageLimit) {
      std::optional<std::size_t> index;
      for (std::size_t i = 0; i < parts.size(); ++i) if (chatImageUrl(parts[i])) index = i;
      if (!index) break;
      const auto url = chatImageUrl(parts[*index]);
      if (!url) break;
      const auto marker = omittedImageMarker(forwardedByteLength(*url));
      parts[*index] = json::object{{"type", "text"}, {"text", marker}};
      elisions.push_back({path + "[" + rt::numberToString(static_cast<double>(*index)) + "]", std::nullopt, "image", forwardedByteLength(*url), forwardedByteLength(marker), forwardedByteLength(*url)});
    }
    if (chatContentBytes(parts) > FORWARDED_PAYLOAD_POLICY.perMessageLimit) {
      for (auto& part : parts) if (stringField(part, "type") == "text" && stringField(part, "text")) {
        const auto text = *stringField(part, "text");
        const auto available = rt::Math::max(0.0, FORWARDED_PAYLOAD_POLICY.perMessageLimit - chatContentBytes(parts) + forwardedByteLength(text));
        auto reduced = boundedForwardedText(text, path, reduction, elisions, available);
        if (auto error = failed(reduced)) return *error;
        part = json::object{{"type", "text"}, {"text", result(reduced)}};
        break;
      }
    }
    if (chatContentBytes(parts) > FORWARDED_PAYLOAD_POLICY.perMessageLimit) {
      const auto marker = forwardedPayloadMarker(bytes);
      elisions.push_back({path, std::nullopt, "message", bytes, forwardedByteLength(marker), bytes});
      return good(json::value(marker));
    }
    return good(json::value(std::move(parts)));
  }
  /**
   * A replayed tool result. Codex also replays a named unpaired output
   * (`name`/`namespace`, no `call_id`) for tools its own client ran, for example
   * an app-server `toolOutput` turn. Chat Completions can only express a result
   * as the `tool_call_id` answer to a preceding `tool_calls` entry, so an
   * unpaired output has no destination and contributes no message instead of
   * failing the whole request. Re-attaching it to an earlier call of the same
   * name is not safe: two calls of one tool in a turn are indistinguishable here,
   * and a wrong pairing would silently corrupt the replayed conversation.
   */
  R<json::array> chatToolResultItems(const json::object& item, const std::string& reduction, const std::string& path, Elisions& elisions) {
    const auto callId = stringField(item, "call_id");
    if (!callId || callId->empty()) return good(json::array{});
    const auto* output = rt::get(item, "output");
    const auto raw = output && output->is_string() ? rt::toString(*output) : rt::jsonStringify(rt::isNullish(output) ? json::value("") : *output);
    const auto bytes = forwardedByteLength(raw);
    if (bytes <= FORWARDED_PAYLOAD_POLICY.perMessageLimit) return good(json::array{json::object{{"role", "tool"}, {"tool_call_id", *callId}, {"content", raw}}});
    if (reduction == "reject") return oversizedPayloadFailure(path, bytes, callId);
    const auto reduced = reduceForwardedPayload(raw, path, callId);
    elisions.push_back(reduced.elision);
    return good(json::array{json::object{{"role", "tool"}, {"tool_call_id", *callId}, {"content", reduced.content}}});
  }
  /**
   * A freeform (`custom`) tool call replayed from history. Chat Completions has no
   * custom tool type, so the freeform text travels as the single `input` argument
   * the adapter advertises for those tools, wrapped as the JSON string Chat
   * requires. Without this translation Codex rejected the whole request:
   * "input item type 'custom_tool_call' is not supported".
   */
  R<json::object> chatCustomToolCallItem(const json::object& item, const std::string& reduction, const std::string& path, Elisions& elisions) {
    auto callId = stringField(item, "call_id"); if (!callId) callId = stringField(item, "id");
    const auto name = stringField(item, "name");
    if (!callId || callId->empty() || !name || name->empty()) return failure("input", "custom_tool_call items require call_id and name");
    auto args = boundedToolArguments(rt::jsonStringify(json::object{{"input", stringField(item, "input").value_or("")}}), path + ".arguments", *callId, reduction, elisions);
    if (auto error = failed(args)) return *error;
    return good(json::object{{"id", *callId}, {"type", "function"}, {"function", json::object{{"name", *name}, {"arguments", result(args)}}}});
  }
  /**
   * The chain-of-thought text a `reasoning` input item carries. Codex echoes the
   * reasoning item this adapter emitted, either as `summary` parts or as
   * `content` parts depending on the client version.
   */
  std::string reasoningTextFromItem(const json::object& item) {
    std::vector<std::string> texts;
    for (std::string_view key : {"summary", "content"}) if (const auto* parts = rt::getArray(item, key)) for (const auto& part : *parts) if (auto text = stringField(part, "text"); text && !text->empty()) texts.push_back(*text);
    if (auto text = stringField(item, "text"); text && !text->empty()) texts.push_back(*text);
    return rt::join(texts, "");
  }
  /** Appends one `function_call` item, merging consecutive calls into one assistant turn. */
  void appendToolCall(json::array& messages, const json::object& call, std::string& pending) {
    if (!messages.empty()) {
      auto& previous = messages.back().as_object();
      auto* calls = rt::getArray(previous, "tool_calls");
      const auto* content = rt::get(previous, "content");
      if (stringField(previous, "role") == "assistant" && calls && content && content->is_null()) {
        previous["tool_calls"].as_array().push_back(call);
        if (!pending.empty()) { previous["reasoning_content"] = rt::concat(stringField(previous, "reasoning_content").value_or(""), pending); pending.clear(); }
        return;
      }
    }
    json::object turn{{"role", "assistant"}, {"content", nullptr}, {"tool_calls", json::array{call}}};
    if (!pending.empty()) { turn["reasoning_content"] = pending; pending.clear(); }
    messages.push_back(std::move(turn));
  }
  R<std::monostate> appendMessageItem(json::array& messages, const json::object& item, std::string& pending, const std::string& path,
    const std::string& reduction, Elisions& elisions, Exemptions& exemptions, bool compactionSummary) {
    const auto role = stringField(item, "role").value_or("user");
    if (role != "user" && role != "assistant" && role != "developer" && role != "system") return failure("input.role", "input role '" + role + "' is not supported");
    if (role == "user") pending.clear();
    auto content = chatContentFromResponseParts(rt::get(item, "content"), role, reduction, path, elisions, compactionSummary);
    if (auto error = failed(content)) return *error;
    if (compactionSummary && result(content).is_string()) { const auto bytes = forwardedByteLength(rt::toString(result(content))); if (bytes > FORWARDED_PAYLOAD_POLICY.perMessageLimit) exemptions.push_back(compactionSummaryExemption(path + ".content", bytes)); }
    json::object message{{"role", role == "developer" ? "system" : role}, {"content", result(content)}};
    if (role == "assistant" && !pending.empty()) { message["reasoning_content"] = pending; pending.clear(); }
    messages.push_back(std::move(message));
    return good(std::monostate{});
  }
  /**
   * Distinguishes a sealed agent-message payload from a forwarded one. Sealed
   * payloads are Fernet tokens, which begin with the version byte base64-encoded
   * as `gAAAAA` and run long; everything else is content the receiving model can
   * actually read.
   */
  bool isSealedAgentPayload(const std::string& value) { return rt::startsWith(value, "gAAAAA") && rt::length(value) >= 100; }
  R<json::object> chatAgentMessageItem(const json::object& item, const std::string& reduction, const std::string& path, Elisions& elisions) {
    const auto author = stringField(item, "author").value_or("agent"), recipient = stringField(item, "recipient").value_or("root");
    std::vector<std::string> texts;
    double unreadable = 0;
    if (const auto* parts = rt::getArray(item, "content")) for (const auto& part : *parts) {
      if (!part.is_object()) continue;
      if (auto text = stringField(part, "text")) { texts.push_back(*text); continue; }
      auto encrypted = stringField(part, "encrypted_content");
      if (!encrypted) continue;
      if (isSealedAgentPayload(*encrypted)) { ++unreadable; continue; }
      texts.push_back(*encrypted);
    }
    const auto notice = unreadable != 0 ? "\n[gateway: " + rt::numberToString(unreadable) + " sealed agent-message part(s) were not readable and omitted]" : "";
    const auto content = "[agent message] " + author + " -> " + recipient + ":\n" + rt::join(texts, "\n") + notice;
    const auto bytes = forwardedByteLength(content);
    if (bytes <= FORWARDED_PAYLOAD_POLICY.perMessageLimit) return good(json::object{{"role", "user"}, {"content", content}});
    if (reduction == "reject") return oversizedPayloadFailure(path, bytes, std::nullopt);
    auto reduced = reduceForwardedPayload(content, path, std::nullopt, "agent_message");
    elisions.push_back(reduced.elision);
    return good(json::object{{"role", "user"}, {"content", reduced.content}});
  }
  R<std::monostate> appendInputItem(json::array& messages, const json::value& rawItem, std::string& pending, const std::string& path,
    const std::string& reduction, Elisions& elisions, Exemptions& exemptions, bool compactionSummary) {
    if (rawItem.is_string()) {
      auto content = boundedForwardedText(rt::toString(rawItem), path, reduction, elisions);
      if (auto error = failed(content)) return *error;
      messages.push_back(json::object{{"role", "user"}, {"content", result(content)}}); pending.clear(); return good(std::monostate{});
    }
    if (!rawItem.is_object()) return failure("input", "input items must be objects");
    const auto& item = rawItem.as_object();
    const auto type = stringField(item, "type").value_or("message");
    if (type == "reasoning") {
      auto reasoning = boundedForwardedText(reasoningTextFromItem(item), path, reduction, elisions, FORWARDED_PAYLOAD_POLICY.perMessageLimit, "reasoning");
      if (auto error = failed(reasoning)) return *error;
      rt::append(pending, result(reasoning)); return good(std::monostate{});
    }
    if (type == "function_call" || type == "custom_tool_call") {
      auto call = type == "custom_tool_call" ? chatCustomToolCallItem(item, reduction, path, elisions) : chatToolCallItem(item, reduction, path, elisions);
      if (auto error = failed(call)) return *error;
      appendToolCall(messages, result(call), pending); return good(std::monostate{});
    }
    if (type == "function_call_output" || type == "custom_tool_call_output") {
      auto calls = chatToolResultItems(item, reduction, path + ".output", elisions);
      if (auto error = failed(calls)) return *error;
      for (const auto& call : result(calls)) messages.push_back(call);
      return good(std::monostate{});
    }
    if (type == "agent_message") {
      auto message = chatAgentMessageItem(item, reduction, path + ".content", elisions);
      if (auto error = failed(message)) return *error;
      messages.push_back(result(message)); pending.clear(); return good(std::monostate{});
    }
    if (type == "message") return appendMessageItem(messages, item, pending, path, reduction, elisions, exemptions, compactionSummary);
    return failure("input.type", "input item type '" + type + "' is not supported");
  }
}
R<json::array> toDeepSeekChatMessages(const json::value* input, const std::optional<std::string>& instructions, const std::string& reduction, Elisions& elisions, Exemptions& exemptions) {
  json::array messages;
  if (instructions && !instructions->empty()) {
    auto content = boundedForwardedText(*instructions, "instructions", reduction, elisions);
    if (auto error = failed(content)) return *error;
    messages.push_back(json::object{{"role", "system"}, {"content", result(content)}});
  }
  const auto summary = inspectCompactionSummaryInput(input);
  if (const auto* error = std::get_if<CompactionSummaryInspectionNotOk>(&summary)) return failure(error->path, "input item " + error->message, "context_length_exceeded");
  if (input && input->is_string()) {
    const auto text = rt::toString(*input);
    if (!text.empty()) { auto content = boundedForwardedText(text, "input", reduction, elisions); if (auto error = failed(content)) return *error; messages.push_back(json::object{{"role", "user"}, {"content", result(content)}}); }
    return good(std::move(messages));
  }
  if (rt::isNullish(input)) return good(std::move(messages));
  if (!input->is_array()) return failure("input", "input must be a string or an array");
  std::string pending;
  for (std::size_t i = 0; i < input->as_array().size(); ++i) {
    auto appended = appendInputItem(messages, input->as_array()[i], pending, "input[" + rt::numberToString(static_cast<double>(i)) + "]", reduction, elisions, exemptions,
      std::get<CompactionSummaryInspectionOk>(summary).index == static_cast<double>(i));
    if (auto error = failed(appended)) return *error;
  }
  return good(std::move(messages));
}
R<json::array> toDeepSeekChatMessages(const json::value* input, const std::optional<std::string>& instructions, const std::string& reduction) { Elisions elisions; Exemptions exemptions; return toDeepSeekChatMessages(input, instructions, reduction, elisions, exemptions); }
R<json::array> toDeepSeekChatMessages(const json::value& input, const std::optional<std::string>& instructions, const std::string& reduction) { return toDeepSeekChatMessages(&input, instructions, reduction); }
namespace {
  /**
   * DeepSeek rejects a tool-bearing request whose assistant turns omit
   * `reasoning_content` while thinking mode is on:
   *
   *   "The `reasoning_content` in the thinking mode must be passed back to the API."
   *
   * The measured boundary (probed against `deepseek-v4-flash`, `reasoning_effort`
   * other than `none`, with tools advertised) is: every assistant message that
   * follows the last `user` message must carry the field, and an empty string
   * satisfies it. Messages before the last user message are exempt, which is why
   * the fill is restricted to that tail instead of touching replayed history.
   *
   * A tool-call turn is not the only shape that trips this: Codex echoes the
   * assistant's text message ahead of its `function_call`, so the request that
   * continues a tool call carries a plain assistant message in the tail as well.
   * Filling only `tool_calls` turns left that message bare and the follow-up died
   * with HTTP 400 (observed in production on `deepseek-v4-flash`, `max`).
   */
  void ensureTrailingAssistantReasoning(json::array& messages) {
    std::size_t first = 0;
    for (std::size_t i = 0; i < messages.size(); ++i) if (stringField(messages[i], "role") == "user") first = i + 1;
    for (std::size_t i = first; i < messages.size(); ++i) if (stringField(messages[i], "role") == "assistant" && !stringField(messages[i], "reasoning_content")) messages[i].as_object()["reasoning_content"] = "";
  }
  /**
   * Collects Chat functions, flattening Responses `namespace` groups.
   *
   * Responses tool entries are flat and Codex groups related functions under a
   * `namespace` entry; Chat Completions has neither shape. A flattened name must
   * stay inside DeepSeek's `[a-zA-Z0-9_-]{1,128}` function name rule (a dot is
   * not accepted), so a collision takes its namespace as a prefix and is recorded
   * for reverse translation of the returned call. Tool types the official API
   * cannot serve (`web_search`, and anything else non-function) are dropped
   * rather than advertised.
   */
  json::object chatFunctionRecord(const json::object& fn, const std::string& chatName) {
    json::object function{{"name", chatName}};
    if (auto description = stringField(fn, "description")) function["description"] = *description;
    if (const auto* parameters = rt::get(fn, "parameters")) function["parameters"] = *parameters;
    if (auto strict = rt::getBool(fn, "strict")) function["strict"] = *strict;
    return {{"type", "function"}, {"function", std::move(function)}};
  }
  constexpr double CHAT_FUNCTION_NAME_LIMIT = 128;
  /** Names must stay distinct and rule-compliant within the provider limit, so collisions suffix deterministically. */
  std::string uniqueChatName(const rt::OrderedSet<std::string>& used, const std::string& name) {
    auto bounded = rt::slice(name, 0, CHAT_FUNCTION_NAME_LIMIT);
    if (!used.has(bounded)) return bounded;
    for (double suffix = 2;; ++suffix) {
      const auto text = "_" + rt::numberToString(suffix);
      const auto candidate = rt::slice(name, 0, CHAT_FUNCTION_NAME_LIMIT - static_cast<double>(rt::length(text))) + text;
      if (!used.has(candidate)) return candidate;
    }
  }
  struct ToolCollector { json::array tools; rt::OrderedMap<std::string, responses::OriginalToolName> toolNames; rt::OrderedSet<std::string> customNames; rt::OrderedSet<std::string> used; };
  /**
   * The one parameter a freeform tool is advertised with. Codex's `apply_patch`
   * and code-mode `exec` tools take raw text, and Chat Completions only has JSON
   * function arguments, so the adapter asks for that text under `input` and
   * unwraps it again on the way back.
   */
  const json::object CUSTOM_TOOL_PARAMETERS{{"type", "object"}, {"properties", json::object{{"input", json::object{{"type", "string"}, {"description", "Freeform input for the tool."}}}}}, {"required", json::array{"input"}}, {"additionalProperties", false}};
  R<std::monostate> collectCustom(ToolCollector& collector, const json::object& tool) {
    const auto name = stringField(tool, "name");
    if (!name || name->empty()) return failure("tools.name", "custom tools require a name");
    const auto chatName = uniqueChatName(collector.used, *name);
    collector.used.add(chatName); collector.customNames.add(chatName);
    if (chatName != *name) collector.toolNames.set(chatName, {*name, std::nullopt});
    json::object function{{"name", chatName}};
    if (const auto description = stringField(tool, "description")) function["description"] = *description;
    function["parameters"] = CUSTOM_TOOL_PARAMETERS;
    collector.tools.push_back(json::object{{"type", "function"}, {"function", std::move(function)}});
    return good(std::monostate{});
  }
  R<std::monostate> collectFunction(ToolCollector& collector, const json::object& fn, const std::optional<std::string>& namespace_) {
    const auto name = stringField(fn, "name");
    if (!name || name->empty()) return failure("tools.name", "function tools require a name");
    const auto preferred = namespace_ && collector.used.has(*name) ? *namespace_ + "_" + *name : *name;
    const auto chatName = uniqueChatName(collector.used, preferred);
    collector.used.add(chatName);
    if (chatName != *name || namespace_) collector.toolNames.set(chatName, {*name, namespace_});
    collector.tools.push_back(chatFunctionRecord(fn, chatName)); return good(std::monostate{});
  }
  R<std::monostate> collectNamespace(ToolCollector& collector, const json::object& tool) {
    const auto* tools = rt::getArray(tool, "tools");
    if (!tools) return failure("tools.tools", "namespace tools must nest a tools array");
    const auto namespace_ = stringField(tool, "name");
    for (const auto& nested : *tools) {
      if (!nested.is_object()) return failure("tools.tools", "namespace tools must contain objects");
      if (stringField(nested, "type") != "function") continue;
      auto added = collectFunction(collector, nested.as_object(), namespace_);
      if (auto error = failed(added)) return *error;
    }
    return good(std::monostate{});
  }
  R<std::monostate> collectTool(ToolCollector& collector, const json::value& tool) {
    if (!tool.is_object()) return failure("tools", "tools must contain objects");
    const auto type = stringField(tool, "type");
    if (type == "namespace") return collectNamespace(collector, tool.as_object());
    if (type == "custom") return collectCustom(collector, tool.as_object());
    const auto* nested = rt::getObject(tool, "function");
    if (!nested && type != "function") return good(std::monostate{});
    return collectFunction(collector, nested ? *nested : tool.as_object(), std::nullopt);
  }
  R<ToolCollector> toChatTools(const json::value* value) {
    if (!value || !value->is_array()) return failure("tools", "tools must be an array");
    ToolCollector collector;
    for (const auto& tool : value->as_array()) { auto collected = collectTool(collector, tool); if (auto error = failed(collected)) return *error; }
    return good(std::move(collector));
  }
  std::string chatToolName(const std::string& name, const std::optional<std::string>& namespace_, const rt::OrderedMap<std::string, responses::OriginalToolName>& names) {
    if (!namespace_) {
      for (const auto& [chatName, original] : names) if (original.name == name && !original.namespace_) return chatName;
      return responses::originalToolName(name, names);
    }
    for (const auto& [chatName, original] : names) if (original.name == name && original.namespace_ == namespace_) return chatName;
    return name;
  }
  R<std::optional<json::value>> toChatToolChoice(const json::value* value, const rt::OrderedMap<std::string, responses::OriginalToolName>& names) {
    if (rt::isNullish(value)) return good(std::optional<json::value>{});
    if (value->is_string()) {
      const auto text = rt::toString(*value);
      if (text == "none" || text == "auto" || text == "required") return good(std::optional<json::value>(*value));
      return failure("tool_choice", "tool_choice '" + text + "' is not supported");
    }
    if (!value->is_object()) return failure("tool_choice", "tool_choice must be a string or an object");
    const auto* nested = rt::getObject(*value, "function");
    auto name = stringField(*value, "name");
    if (!name && nested) name = stringField(*nested, "name");
    if (!name || name->empty()) return failure("tool_choice.name", "a named tool_choice requires a name");
    const auto namespace_ = rt::get(*value, "namespace") ? stringField(*value, "namespace") : nested ? stringField(*nested, "namespace") : std::nullopt;
    return good(std::optional<json::value>(json::object{{"type", "function"}, {"function", json::object{{"name", chatToolName(*name, namespace_, names)}}}}));
  }
  /** Responses nests the output format under `text.format`; Chat has one flat field. */
  R<std::optional<json::object>> toChatResponseFormat(const json::value* value) {
    if (rt::isNullish(value)) return good(std::optional<json::object>{});
    if (!value->is_object()) return failure("text", "text must be an object");
    const auto* format = rt::get(*value, "format");
    if (rt::isNullish(format)) return good(std::optional<json::object>{});
    if (!format->is_object()) return failure("text.format", "text.format must be an object");
    const auto type = stringField(*format, "type").value_or("text");
    if (type == "text") return good(std::optional<json::object>{});
    if (type == "json_object") return good(std::optional<json::object>(json::object{{"type", "json_object"}}));
    return failure("text.format.type", "text.format type '" + type + "' is not supported upstream");
  }
  std::optional<double> positiveInteger(const json::value* value) { if (!value || !rt::Number::isSafeInteger(*value)) return std::nullopt; auto number = rt::toNumber(*value); return number > 0 ? std::optional(number) : std::nullopt; }
  R<std::monostate> applyOutputLimit(json::object& body, const json::object& raw) {
    const auto* value = rt::get(raw, "max_output_tokens");
    if (rt::isNullish(value)) return good(std::monostate{});
    const auto maxTokens = positiveInteger(value);
    if (!maxTokens) return failure("max_output_tokens", "max_output_tokens must be a positive integer");
    rt::objectSet(body, "max_tokens", *maxTokens); return good(std::monostate{});
  }
  R<std::monostate> applyReasoning(json::object& body, const json::object& raw, const responses::ChatOnlyResponsesProfile& profile) {
    const bool disabled = profile.id == "deepseek" && !index::deepSeekThinkingModeActive(nullptr, rt::get(raw, "thinking"));
    const auto* value = rt::get(raw, "reasoning");
    if (rt::isNullish(value)) { if (disabled) body["reasoning_effort"] = "none"; return good(std::monostate{}); }
    if (!value->is_object()) return failure("reasoning", "reasoning must be an object");
    const auto effort = stringField(*value, "effort");
    if (!effort || effort->empty()) { if (disabled) body["reasoning_effort"] = "none"; return good(std::monostate{}); }
    const auto wire = profile.projectReasoningEffort(*effort);
    if (wire == nullptr) return failure("reasoning.effort", "reasoning.effort '" + *effort + "' is not supported by " + std::string(profile.label));
    rt::objectSet(body, "reasoning_effort", wire); return good(std::monostate{});
  }
  std::vector<std::string> applySamplingControls(json::object& body, const json::object& raw, const responses::ChatOnlyResponsesProfile& profile) {
    const bool active = profile.id == "deepseek" && index::deepSeekThinkingModeActive(rt::get(body, "reasoning_effort"), rt::get(raw, "thinking"));
    std::vector<std::string> warnings;
    for (std::string_view key : {"temperature", "top_p"}) if (const auto* value = rt::get(raw, key)) {
      if (active) warnings.push_back(std::string(key) + "_ignored");
      else body[key] = *value;
    }
    return warnings;
  }
  R<ToolCollector> applyTools(json::object& body, const json::object& raw, const responses::ChatOnlyResponsesProfile& profile) {
    ToolCollector collector;
    if (const auto* tools = rt::get(raw, "tools")) {
      auto translated = toChatTools(tools); if (auto error = failed(translated)) return *error;
      collector = std::move(result(translated));
      if (!collector.tools.empty()) body["tools"] = collector.tools;
    }
    auto choice = toChatToolChoice(rt::get(raw, "tool_choice"), collector.toolNames);
    if (auto error = failed(choice)) return *error;
    if (result(choice)) {
      const auto conflict = profile.thinkingToolChoiceConflict(rt::get(body, "reasoning_effort"), rt::get(raw, "thinking"), &*result(choice));
      if (conflict) return failure("tool_choice", profile.toolChoiceThinkingConflictMessage(*conflict, "reasoning.effort"));
      body["tool_choice"] = *result(choice);
    }
    if (auto parallel = rt::getBool(raw, "parallel_tool_calls")) body["parallel_tool_calls"] = *parallel;
    auto format = toChatResponseFormat(rt::get(raw, "text"));
    if (auto error = failed(format)) return *error;
    if (result(format)) body["response_format"] = *result(format);
    return good(std::move(collector));
  }
  /**
   * Codex can end a turn after a progress-only assistant message while the
   * requested action is still outstanding. This reminder keeps the client's
   * executable tools in play: it extends the caller's instructions (or becomes
   * the system message when the caller sent none) only for tool-bearing requests
   * that permit tool use. It never forces a tool call, forbids a legitimate final
   * answer, or claims any tool action was performed.
   */
  constexpr std::string_view CONTINUATION_INSTRUCTION = "When tools are available, a progress update does not complete a requested action. If required work remains and you can perform it, continue with the next appropriate tool call instead of ending with a status message. Provide a final answer when the requested work is complete, or when you need user input or are blocked. Never claim a tool action has been done unless its result is in the conversation.";
  std::optional<Failure> appendContinuationInstruction(json::array& messages, const std::string& reduction, Elisions& elisions) {
    auto system = std::find_if(messages.begin(), messages.end(), [](const auto& message) { return stringField(message, "role") == "system" && stringField(message, "content"); });
    const auto content = system == messages.end() ? std::string(CONTINUATION_INSTRUCTION) : *stringField(*system, "content") + "\n\n" + std::string(CONTINUATION_INSTRUCTION);
    auto bounded = boundedForwardedText(content, "instructions", reduction, elisions);
    if (auto error = failed(bounded)) return *error;
    if (system == messages.end()) messages.insert(messages.begin(), json::object{{"role", "system"}, {"content", result(bounded)}});
    else system->as_object()["content"] = result(bounded);
    return std::nullopt;
  }
  /**
   * The reduction decision the request authorizes. Only an explicit
   * `truncation: "disabled"` forbids reduction, and it fails closed on an
   * oversized payload instead of letting the gateway mutate the input. An absent
   * field or `"auto"` keeps the declared bounded policy: the clients this route
   * serves omit the field, cannot repair a rejected history, and the 2026-09-24
   * incident showed that forwarding the oversized payload whole poisons the
   * thread. Any other value is rejected rather than guessed at.
   */
  R<std::string> reductionForTruncation(const json::value* value) {
    if (rt::isNullish(value) || (value->is_string() && rt::toString(*value) == "auto")) return good(std::string("reduce"));
    if (value->is_string() && rt::toString(*value) == "disabled") return good(std::string("reject"));
    return failure("truncation", "truncation '" + (value->is_string() ? rt::toString(*value) : std::string(rt::typeOf(value))) + "' is not supported; expected 'auto' or 'disabled'");
  }
  json::object providerPromptCacheFields(const json::object& raw, const responses::ChatOnlyResponsesProfile& profile) {
    json::object fields;
    if (profile.id == "cerebras") if (const auto* key = rt::get(raw, "prompt_cache_key")) fields["prompt_cache_key"] = *key;
    return fields;
  }
  std::optional<Failure> prepareExecutableToolMessages(const json::object& body, const json::object& raw, json::array& messages, const std::string& reduction, Elisions& elisions) {
    const auto* tools = rt::getArray(body, "tools"); if (!tools || tools->empty()) return std::nullopt;
    if (stringField(raw, "tool_choice") != "none") if (auto error = appendContinuationInstruction(messages, reduction, elisions)) return error;
    ensureTrailingAssistantReasoning(messages); return std::nullopt;
  }
}
/**
 * Builds the Chat Completions body for a Responses request under one provider
 * profile. Translation failures are returned so the caller can answer with a
 * precise `invalid_request_error` instead of dispatching an approximation.
 */
R<DeepSeekResponsesChatBody> toDeepSeekResponsesChatBody(const json::object& raw, const std::string& requestedModel, bool stream,
  const responses::ChatOnlyResponsesProfile& profile, const PromptAdmissionProjection& projection) {
  const auto canonical = profile.upstreamModelFor(requestedModel);
  if (!canonical || canonical->empty()) return failure("model", "model '" + requestedModel + "' is not a " + std::string(profile.label) + " official model");
  auto reduction = reductionForTruncation(rt::get(raw, "truncation")); if (auto error = failed(reduction)) return *error;
  auto instructions = stringField(raw, "instructions"); if (instructions && rt::trim(*instructions).empty()) instructions.reset();
  Elisions elisions; Exemptions exemptions;
  auto messages = toDeepSeekChatMessages(rt::get(raw, "input"), instructions, result(reduction), elisions, exemptions);
  if (auto error = failed(messages)) return *error;
  if (result(messages).empty()) return failure("input", "input must contain at least one message");
  json::object body{{"model", *canonical}, {"messages", result(messages)}, {"stream", stream}};
  rt::objectAssign(body, providerPromptCacheFields(raw, profile));
  if (stream && profile.requiresStreamUsageOption) body["stream_options"] = json::object{{"include_usage", true}};
  auto limit = applyOutputLimit(body, raw); if (auto error = failed(limit)) return *error;
  auto reasoning = applyReasoning(body, raw, profile); if (auto error = failed(reasoning)) return *error;
  auto warnings = applySamplingControls(body, raw, profile);
  auto tools = applyTools(body, raw, profile); if (auto error = failed(tools)) return *error;
  if (auto error = prepareExecutableToolMessages(body, raw, result(messages), result(reduction), elisions)) return *error;
  body["messages"] = result(messages);
  const auto window = promptWindowTokensForProfile(profile.id);
  if (!window) return failure("model", std::string(profile.label) + " has no verified prompt window; aggregate admission refused the request.", "context_length_exceeded");
  const auto admission = estimatePromptAdmission(body, *window, projection);
  if (!admission.admitted) return responses::aggregateContextOverflow(promptAdmissionFailureMessage(admission, std::string(profile.label)));
  return good(DeepSeekResponsesChatBody{std::move(body), std::move(result(tools).toolNames), std::move(result(tools).customNames), std::move(elisions), std::move(exemptions), admission, std::move(warnings)});
}
/**
 * The operator-visible record of every payload this gateway reduced: one JSON
 * line per elision, including the declared policy version, the input path, the
 * tool call when there is one, and the before/after byte counts.
 */
void logForwardedPayloadElisions(const std::vector<ForwardedPayloadElision>& elisions) {
  for (const auto& elision : elisions) { json::object material{{"policy", FORWARDED_PAYLOAD_POLICY.version}}; rt::objectAssign(material, toJson(elision)); rt::console::info("[ai.ubq.fi] forwarding_elision", rt::jsonStringify(material)); }
}
}
