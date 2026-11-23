#include "uos/provider/anthropic_replay.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/text_codec.hpp"
#include <cstddef>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace uos::provider::anthropic_replay {
  struct AnthropicReplayStore::State {
    struct Receipt {
      std::vector<std::string> keys;
      std::vector<std::string> fingerprints;
      std::string payloadJson;
      std::string transport;  // "messages" | "chat"
      double expiresAt = 0;
      double bytes = 0;
    };

    std::function<double()> now;
    double ttlMs = 0;
    double maxEntries = 0;
    double maxBytes = 0;
    double maxEntryBytes = 0;
    rt::OrderedSet<std::shared_ptr<Receipt>> receipts;
    std::unordered_map<std::string, std::shared_ptr<Receipt>> index;
    double totalBytes = 0;
  };

  namespace {
    using JsonRecord = json::object;
    using State = AnthropicReplayStore::State;
    using Receipt = State::Receipt;

    constexpr double TTL_MS = 30 * 60 * 1000;
    constexpr double MAX_ENTRIES = 64;
    constexpr double MAX_BYTES = 8 * 1024 * 1024;
    constexpr double MAX_ENTRY_BYTES = 1024 * 1024;
    constexpr std::size_t MAX_ITEMS = 256;
    bool record(const json::value* value) { return rt::isRecord(value); }
    bool absent(const json::value* value) { return rt::isNullish(value); }

    // `value === "text"` for a value that may be undefined.
    bool equalsString(const json::value* value, std::string_view text) {
      const std::optional<std::string_view> string = rt::getString(value);
      return string && *string == text;
    }
    bool isString(const json::value* value) { return value && value->is_string(); }
    // `a === b` for two values that may be undefined.
    bool strictEqualsMember(const json::value* left, const json::value* right) {
      if (!left || !right) return !left && !right;
      return rt::strictEquals(*left, *right);
    }
    // `part.type` on an array element: a TypeError for null (undefined cannot occur in a JSON array).
    const json::value* member(const json::value& value, std::string_view key, std::string_view expression) {
      if (value.is_null()) throw rt::TypeError("null is not an object (evaluating '" + std::string(expression) + "')");
      return rt::get(value, key);
    }

    std::optional<std::string> contextKey(const AnthropicReplayContext& value) {
      if (value.scope.empty() || rt::length(value.scope) > 512 || value.provider.empty() || rt::length(value.provider) > 64 || value.model.empty() ||
          rt::length(value.model) > 256)
        return std::nullopt;
      return "[" + rt::jsonQuote(value.scope) + "," + rt::jsonQuote(value.provider) + "," + rt::jsonQuote(value.model) + "]";
    }

    // Only finite JSON values are retained. Private native signatures never leave this store.
    // A json::value is a tree: the TypeScript's guard against circular input cannot trigger.
    std::optional<std::string> canonical(const json::value* value, int depth = 0) {
      if (depth > 64) return std::nullopt;
      if (!value) return std::nullopt;
      if (value->is_null()) return std::string("null");
      if (const auto text = rt::getString(*value)) return rt::jsonQuote(*text);
      if (const auto boolean = rt::getBool(*value)) return std::string(*boolean ? "true" : "false");
      if (const auto number = rt::getNumber(*value)) return rt::Number::isFinite(*number) ? std::optional<std::string>(rt::numberToString(*number)) : std::nullopt;
      if (const json::array* array = rt::getArray(*value)) {
        std::string children;
        for (std::size_t position = 0; position < array->size(); ++position) {
          const std::optional<std::string> child = canonical(&(*array)[position], depth + 1);
          if (!child) return std::nullopt;
          if (position) children += ",";
          children += *child;
        }
        return "[" + children + "]";
      }
      const json::object& object = value->get_object();
      std::vector<std::string> keys = rt::objectKeys(object);
      rt::sort(keys, [](const std::string& left, const std::string& right) { return rt::localeCompare(left, right); });
      std::string children;
      for (std::size_t position = 0; position < keys.size(); ++position) {
        const std::optional<std::string> child = canonical(rt::get(object, keys[position]), depth + 1);
        if (!child) return std::nullopt;
        if (position) children += ",";
        children += rt::jsonQuote(keys[position]) + ":" + *child;
      }
      return "{" + children + "}";
    }
    std::optional<std::string> canonical(const json::value& value) { return canonical(&value); }

    std::optional<std::string> itemId(const JsonRecord& item) {
      static const rt::Regex ITEM_ID("^(?:rs|fc|msg|ctc)_[A-Za-z0-9_-]{1,240}$");
      const std::optional<std::string_view> id = rt::getString(item, "id");
      return id && ITEM_ID.test(*id) ? std::optional<std::string>(std::string(*id)) : std::nullopt;
    }
    bool nativeReplayId(const std::optional<std::string>& id) {
      static const rt::Regex NATIVE_REPLAY_ID("^(?:rs|fc|msg|ctc)_resp_[A-Za-z0-9_-]+_\\d+$");
      return id && NATIVE_REPLAY_ID.test(*id);
    }

    // JSON.stringify([context, id])
    std::string replayKey(const std::string& context, const std::optional<std::string>& id) {
      return "[" + rt::jsonQuote(context) + "," + (id ? rt::jsonQuote(*id) : std::string("null")) + "]";
    }

    // Codex drops display status/annotations and adds null optional fields when replaying.
    // Compare model-visible content rather than the incidental output envelope.
    std::optional<std::string> reasoningFingerprint(const JsonRecord& item, const std::string& id) {
      const json::value* content = rt::get(item, "content");
      if (!absent(rt::get(item, "encrypted_content")) || (!absent(content) && (!content->is_array() || !content->get_array().empty()))) return std::nullopt;
      const json::array* summary = rt::getArray(item, "summary");
      if (!summary) return std::nullopt;
      for (const json::value& part : *summary) {
        if (!record(&part) || !equalsString(rt::get(part, "type"), "summary_text") || !isString(rt::get(part, "text"))) return std::nullopt;
      }
      json::array parts;
      for (const json::value& part : *summary) {
        json::object projected;
        projected["type"] = *rt::get(part, "type");
        projected["text"] = *rt::get(part, "text");
        parts.emplace_back(std::move(projected));
      }
      json::object fingerprinted;
      fingerprinted["id"] = id;
      fingerprinted["type"] = *rt::get(item, "type");
      fingerprinted["summary"] = std::move(parts);
      return canonical(json::value(std::move(fingerprinted)));
    }

    std::optional<JsonRecord> publicContent(const json::value& part) {
      if (!record(&part)) return std::nullopt;
      const json::value* type = rt::get(part, "type");
      if (equalsString(type, "output_text") && isString(rt::get(part, "text"))) {
        JsonRecord out;
        out["type"] = *type;
        out["text"] = *rt::get(part, "text");
        return out;
      }
      if (equalsString(type, "refusal") && isString(rt::get(part, "refusal"))) {
        JsonRecord out;
        out["type"] = *type;
        out["refusal"] = *rt::get(part, "refusal");
        return out;
      }
      return std::nullopt;
    }

    std::optional<std::string> messageFingerprint(const JsonRecord& item, const std::string& id) {
      const json::array* content = rt::getArray(item, "content");
      if (!equalsString(rt::get(item, "role"), "assistant") || !content) return std::nullopt;
      json::array projected;
      for (const json::value& part : *content) {
        std::optional<JsonRecord> visible = publicContent(part);
        if (!visible) return std::nullopt;
        projected.emplace_back(std::move(*visible));
      }
      json::object fingerprinted;
      fingerprinted["id"] = id;
      fingerprinted["type"] = *rt::get(item, "type");
      fingerprinted["role"] = *rt::get(item, "role");
      fingerprinted["content"] = std::move(projected);
      return canonical(json::value(std::move(fingerprinted)));
    }

    std::optional<std::string> toolFingerprint(const JsonRecord& item, const std::string& id) {
      const std::optional<std::string_view> callId = rt::getString(item, "call_id");
      const std::optional<std::string_view> name = rt::getString(item, "name");
      if (!callId || callId->empty() || !name || name->empty()) return std::nullopt;
      const json::value* itemNamespace = rt::get(item, "namespace");
      if ((!absent(itemNamespace) && !itemNamespace->is_string()) || !absent(rt::get(item, "encrypted_function_args"))) return std::nullopt;
      json::object base;
      base["id"] = id;
      base["type"] = *rt::get(item, "type");
      base["call_id"] = *rt::get(item, "call_id");
      base["name"] = *rt::get(item, "name");
      base["namespace"] = absent(itemNamespace) ? json::value(nullptr) : *itemNamespace;
      if (equalsString(rt::get(item, "type"), "custom_tool_call")) {
        const json::value* input = rt::get(item, "input");
        if (!isString(input)) return std::nullopt;
        json::object fingerprinted = rt::objectSpread(base);
        rt::objectDefine(fingerprinted, "input", *input);
        return canonical(json::value(std::move(fingerprinted)));
      }
      const std::optional<std::string_view> arguments = rt::getString(item, "arguments");
      if (!arguments) return std::nullopt;
      try {
        json::value args = rt::jsonParse(*arguments);
        if (!record(&args)) return std::nullopt;
        json::object fingerprinted = rt::objectSpread(base);
        rt::objectDefine(fingerprinted, "arguments", std::move(args));
        return canonical(json::value(std::move(fingerprinted)));
      } catch (const rt::JsError&) {
        return std::nullopt;
      }
    }

    std::optional<std::string> fingerprint(const json::value& value) {
      if (!record(&value)) return std::nullopt;
      const JsonRecord& item = value.get_object();
      const std::optional<std::string> id = itemId(item);
      if (!id) return std::nullopt;
      const json::value* type = rt::get(item, "type");
      if (equalsString(type, "reasoning")) return reasoningFingerprint(item, *id);
      if (equalsString(type, "message")) return messageFingerprint(item, *id);
      return equalsString(type, "function_call") || equalsString(type, "custom_tool_call") ? toolFingerprint(item, *id) : std::nullopt;
    }

    bool textMatchesOutput(const json::value& block, const json::value* item) {
      const json::value* text = rt::get(block, "text");
      if (!isString(text) || !equalsString(rt::get(item, "type"), "message")) return false;
      const json::array* content = rt::getArray(rt::get(item, "content"));
      if (!content) return false;
      // item.content.map((part) => ({ type: part.type, text: part.text })): an undefined member
      // makes the canonical form null.
      std::optional<std::string> projected;
      json::array parts;
      bool defined = true;
      for (const json::value& part : *content) {
        const json::value* type = member(part, "type", "part.type");
        const json::value* partText = member(part, "text", "part.text");
        if (!type || !partText) {
          defined = false;
          continue;
        }
        json::object entry;
        entry["type"] = *type;
        entry["text"] = *partText;
        parts.emplace_back(std::move(entry));
      }
      if (defined) projected = canonical(json::value(std::move(parts)));
      json::object expected;
      expected["type"] = "output_text";
      expected["text"] = *text;
      json::array expectedParts;
      expectedParts.emplace_back(std::move(expected));
      return projected == canonical(json::value(std::move(expectedParts)));
    }

    bool toolMatchesOutput(const json::value& block, const json::value* item) {
      if (!record(rt::get(block, "input")) || !equalsString(rt::get(item, "type"), "function_call")) return false;
      if (!strictEqualsMember(rt::get(block, "id"), rt::get(item, "call_id")) || !strictEqualsMember(rt::get(block, "name"), rt::get(item, "name"))) return false;
      const json::value arguments = rt::jsonParse(rt::toString(rt::get(item, "arguments")));
      return canonical(rt::get(block, "input")) == canonical(arguments);
    }

    std::optional<std::size_t> thinkingBlockAdvance(const json::value& block, const json::value* item) {
      const json::value* thinking = rt::get(block, "thinking");
      const std::optional<std::string_view> signature = rt::getString(block, "signature");
      if (!isString(thinking) || !signature || signature->empty()) return std::nullopt;
      if (!equalsString(rt::get(item, "type"), "reasoning")) return 0;
      json::object part;
      part["type"] = "summary_text";
      part["text"] = *thinking;
      json::array expected;
      expected.emplace_back(std::move(part));
      return canonical(rt::get(item, "summary")) == canonical(json::value(std::move(expected))) ? std::optional<std::size_t>(1) : std::nullopt;
    }

    std::optional<std::size_t> nativeBlockAdvance(const json::value& block, const json::value* item) {
      if (!record(&block)) return std::nullopt;
      const json::value* type = rt::get(block, "type");
      if (equalsString(type, "redacted_thinking")) {
        const std::optional<std::string_view> data = rt::getString(block, "data");
        return data && !data->empty() ? std::optional<std::size_t>(0) : std::nullopt;
      }
      if (equalsString(type, "thinking")) return thinkingBlockAdvance(block, item);
      if (equalsString(type, "text")) return textMatchesOutput(block, item) ? std::optional<std::size_t>(1) : std::nullopt;
      return equalsString(type, "tool_use") && toolMatchesOutput(block, item) ? std::optional<std::size_t>(1) : std::nullopt;
    }

    bool nativeMatchesOutput(const json::array& native, const json::array& output) {
      std::size_t next = 0;
      for (const json::value& block : native) {
        const std::optional<std::size_t> advance = nativeBlockAdvance(block, rt::at(output, next));
        if (!advance) return false;
        next += *advance;
      }
      return next == output.size();
    }

    std::optional<std::string> chatReadableReasoning(const JsonRecord& assistant, const std::string& detailText) {
      const json::value* reasoningContent = rt::get(assistant, "reasoning_content");
      const json::value* reasoning = rt::get(assistant, "reasoning");
      if (!absent(reasoningContent) && !reasoningContent->is_string()) return std::nullopt;
      if (!absent(reasoning) && !reasoning->is_string()) return std::nullopt;
      if (const auto text = rt::getString(reasoningContent); text && !text->empty()) return std::string(*text);
      if (const auto text = rt::getString(reasoning); text && !text->empty()) return std::string(*text);
      return detailText;
    }

    std::optional<std::string> chatReasoning(const JsonRecord& assistant) {
      const json::value* details = rt::get(assistant, "reasoning_details");
      if (!absent(details) && (!details->is_array() || details->get_array().size() > MAX_ITEMS)) return std::nullopt;
      std::string texts;
      if (const json::array* list = rt::getArray(details)) {
        for (const json::value& detail : *list) {
          const std::optional<std::string_view> text = rt::getString(detail, "text");
          const std::optional<std::string_view> signature = rt::getString(detail, "signature");
          if (!record(&detail) || !equalsString(rt::get(detail, "type"), "reasoning.text") || !text || !signature || signature->empty()) return std::nullopt;
          const json::value* index = rt::get(detail, "index");
          if (!absent(index) && (!rt::isNumber(*index) || !rt::Number::isSafeInteger(*index) || *rt::getNumber(*index) < 0)) return std::nullopt;
          rt::append(texts, *text);
        }
      }
      return chatReadableReasoning(assistant, texts);
    }

    std::optional<JsonRecord> expectedChatTool(const json::value& call, const ChatReplayPublication& publication) {
      const json::object* function = rt::getObject(call, "function");
      const std::optional<std::string_view> id = rt::getString(call, "id");
      if (!record(&call) || !equalsString(rt::get(call, "type"), "function") || !id || id->empty() || !function) return std::nullopt;
      const std::optional<std::string_view> name = rt::getString(*function, "name");
      const std::optional<std::string_view> arguments = rt::getString(*function, "arguments");
      if (!name || name->empty() || !arguments) return std::nullopt;
      const ChatReplayPublication::ToolName* mapped = publication.toolNames ? publication.toolNames->get(*name) : nullptr;
      const ChatReplayPublication::ToolName original = mapped ? *mapped : ChatReplayPublication::ToolName{std::string(*name), std::nullopt};
      JsonRecord base;
      base["call_id"] = std::string(*id);
      base["name"] = original.name;
      base["namespace"] = original.namespace_ ? json::value(*original.namespace_) : json::value(nullptr);
      if (!publication.customToolNames || !publication.customToolNames->has(*name)) {
        JsonRecord expected = rt::objectSpread(base);
        rt::objectDefine(expected, "type", "function_call");
        rt::objectDefine(expected, "arguments", json::string(*arguments));
        return expected;
      }
      try {
        const json::value args = rt::jsonParse(*arguments);
        const json::object* fields = rt::getObject(args);
        const std::optional<std::string_view> input = rt::getString(args, "input");
        if (!fields || fields->size() != 1 || !input) return std::nullopt;
        JsonRecord expected;
        expected["call_id"] = std::string(*id);
        expected["name"] = original.name;
        expected["type"] = "custom_tool_call";
        expected["input"] = std::string(*input);
        return expected;
      } catch (const rt::JsError&) {
        return std::nullopt;
      }
    }

    std::optional<json::array> chatContent(const JsonRecord& assistant) {
      const json::value* content = rt::get(assistant, "content");
      const json::value* refusal = rt::get(assistant, "refusal");
      if ((!absent(content) && !content->is_string()) || (!absent(refusal) && !refusal->is_string())) return std::nullopt;
      json::array parts;
      if (rt::truthy(content)) {
        json::object part;
        part["type"] = "output_text";
        part["text"] = *content;
        parts.emplace_back(std::move(part));
      }
      if (rt::truthy(refusal)) {
        json::object part;
        part["type"] = "refusal";
        part["refusal"] = *refusal;
        parts.emplace_back(std::move(part));
      }
      return parts;
    }

    bool chatMatchesOutput(const ChatReplayPublication& publication) {
      const JsonRecord& assistant = publication.assistant;
      const json::array& output = publication.output;
      if (!equalsString(rt::get(assistant, "role"), "assistant")) return false;
      const std::optional<std::string> reasoning = chatReasoning(assistant);
      const std::optional<json::array> content = chatContent(assistant);
      if (!reasoning || !content) return false;
      std::size_t next = 0;
      if (equalsString(rt::get(rt::at(output, next), "type"), "reasoning")) {
        json::object part;
        part["type"] = "summary_text";
        part["text"] = *reasoning;
        json::array expected;
        expected.emplace_back(std::move(part));
        if (reasoning->empty() || canonical(rt::get(output[next], "summary")) != canonical(json::value(std::move(expected)))) return false;
        next += 1;
      }
      if (!content->empty()) {
        const json::value* item = rt::at(output, next);
        const json::array* parts = rt::getArray(rt::get(item, "content"));
        if (!equalsString(rt::get(item, "type"), "message") || !parts) return false;
        json::array visible;
        for (const json::value& part : *parts) {
          std::optional<JsonRecord> projected = publicContent(part);
          visible.emplace_back(projected ? json::value(std::move(*projected)) : json::value(nullptr));
        }
        if (canonical(json::value(std::move(visible))) != canonical(json::value(*content))) return false;
        next += 1;
      }
      const json::value* toolCalls = rt::get(assistant, "tool_calls");
      if (!absent(toolCalls) && (!toolCalls->is_array() || toolCalls->get_array().size() > MAX_ITEMS)) return false;
      if (const json::array* calls = rt::getArray(toolCalls)) {
        for (const json::value& call : *calls) {
          const std::optional<JsonRecord> expected = expectedChatTool(call, publication);
          const json::value* item = rt::at(output, next);
          if (!expected || !rt::truthy(item)) return false;
          json::object candidate = rt::objectSpread(*expected);
          if (const json::value* id = rt::get(*item, "id")) rt::objectDefine(candidate, "id", *id);
          if (fingerprint(json::value(std::move(candidate))) != fingerprint(*item)) return false;
          next += 1;
        }
      }
      return next == output.size();
    }

    bool receiptMatchesInput(const Receipt& receipt, const json::array& input, std::size_t position, const std::string& context) {
      for (std::size_t offset = 0; offset < receipt.keys.size(); offset += 1) {
        const json::value* candidate = rt::get(input, position + offset);
        if (!record(candidate) || replayKey(context, itemId(candidate->get_object())) != receipt.keys[offset] || fingerprint(*candidate) != receipt.fingerprints[offset])
          return false;
      }
      return true;
    }

    double boundedLimit(std::optional<double> value, double maximum) {
      return value && rt::Number::isSafeInteger(*value) && *value > 0 ? rt::Math::min(*value, maximum) : maximum;
    }

    void remove(State& state, const std::shared_ptr<Receipt>& receipt) {
      state.receipts.delete_(receipt);
      for (const std::string& key : receipt->keys) state.index.erase(key);
      state.totalBytes -= receipt->bytes;
    }
    void prune(State& state, double at) {
      for (const std::shared_ptr<Receipt>& receipt : state.receipts) {
        if (receipt->expiresAt <= at) {
          const std::shared_ptr<Receipt> held = receipt;
          remove(state, held);
        }
      }
    }

    bool publish(State& state, const AnthropicReplayContext& publication, const json::array& output, std::string payloadJson, std::string_view transport) {
      const std::optional<std::string> context = contextKey(publication);
      if (!context || output.empty() || output.size() > MAX_ITEMS) return false;
      std::vector<std::optional<std::string>> fingerprints;
      for (const json::value& item : output) fingerprints.push_back(fingerprint(item));
      for (const auto& value : fingerprints)
        if (!value) return false;
      std::vector<std::string> keys;
      for (const json::value& item : output) keys.push_back(replayKey(*context, itemId(item.get_object())));
      if (std::unordered_set<std::string>(keys.begin(), keys.end()).size() != keys.size()) return false;
      std::vector<std::string> validFingerprints;
      for (auto& value : fingerprints)
        if (value) validFingerprints.push_back(std::move(*value));
      json::array keyList(keys.begin(), keys.end());
      json::array fingerprintList(validFingerprints.begin(), validFingerprints.end());
      json::array measured;
      measured.emplace_back(std::move(keyList));
      measured.emplace_back(std::move(fingerprintList));
      measured.emplace_back(payloadJson);
      measured.emplace_back(std::string(transport));
      const double bytes = static_cast<double>(rt::utf8ByteLength(rt::jsonStringify(json::value(std::move(measured)))));
      if (bytes > state.maxEntryBytes || bytes > state.maxBytes) return false;
      const double at = state.now();
      if (!rt::Number::isFinite(at)) return false;
      prune(state, at);
      // Never replace a receipt whose public IDs have already been emitted.
      for (const std::string& key : keys)
        if (state.index.count(key)) return false;
      while (static_cast<double>(state.receipts.size()) >= state.maxEntries || state.totalBytes + bytes > state.maxBytes) {
        if (state.receipts.size() == 0) return false;
        const std::shared_ptr<Receipt> oldest = *state.receipts.begin();
        remove(state, oldest);
      }
      auto receipt = std::make_shared<Receipt>(Receipt{keys, std::move(validFingerprints), std::move(payloadJson), std::string(transport), at + state.ttlMs, bytes});
      state.receipts.add(receipt);
      state.totalBytes += bytes;
      for (const std::string& key : keys) state.index[key] = receipt;
      return true;
    }

    template <class Entry, class MakeEntry>
    std::optional<std::vector<Entry>> restoreAs(State& state, const json::array& input, const AnthropicReplayContext& contextValue, std::string_view transport,
                                                MakeEntry entry) {
      const std::optional<std::string> context = contextKey(contextValue);
      const double at = state.now();
      if (!context || !rt::Number::isFinite(at)) return std::nullopt;
      prune(state, at);
      std::vector<Entry> result;
      std::unordered_set<const Receipt*> used;
      for (std::size_t position = 0; position < input.size(); position += 1) {
        const json::value& item = input[position];
        if (!record(&item)) return std::nullopt;
        const std::optional<std::string> id = itemId(item.get_object());
        const std::string key = replayKey(*context, id);
        std::shared_ptr<Receipt> receipt;
        if (id && !id->empty()) {
          const auto found = state.index.find(key);
          if (found != state.index.end()) receipt = found->second;
        }
        if (!receipt) {
          if (equalsString(rt::get(item, "type"), "reasoning") || nativeReplayId(id)) return std::nullopt;
          result.push_back(NativeReplayEntryOriginal{item.get_object()});
          continue;
        }
        if (receipt->transport != transport || key != receipt->keys[0] || used.count(receipt.get())) return std::nullopt;
        if (!receiptMatchesInput(*receipt, input, position, *context)) return std::nullopt;
        result.push_back(entry(receipt->payloadJson));
        used.insert(receipt.get());
        position += receipt->keys.size() - 1;
      }
      return result;
    }
  }

  AnthropicReplayStore::AnthropicReplayStore(std::shared_ptr<State> state) : state_(std::move(state)) {}

  bool AnthropicReplayStore::remember(const ReplayPublication& publication) const {
    if (publication.nativeBlocks.empty() || publication.nativeBlocks.size() > MAX_ITEMS) return false;
    std::optional<std::string> json = canonical(json::value(publication.nativeBlocks));
    if (!json) return false;
    for (const json::value& item : publication.output)
      if (!fingerprint(item)) return false;
    if (!nativeMatchesOutput(publication.nativeBlocks, publication.output)) return false;
    return publish(*state_, publication, publication.output, std::move(*json), "messages");
  }

  bool AnthropicReplayStore::rememberChat(const ChatReplayPublication& publication) const {
    std::optional<std::string> json = canonical(json::value(publication.assistant));
    if (!json) return false;
    for (const json::value& item : publication.output)
      if (!fingerprint(item)) return false;
    if (!chatMatchesOutput(publication)) return false;
    return publish(*state_, publication, publication.output, std::move(*json), "chat");
  }

  std::optional<std::vector<NativeReplayEntry>> AnthropicReplayStore::restore(const json::array& input, const AnthropicReplayContext& context) const {
    return restoreAs<NativeReplayEntry>(*state_, input, context, "messages",
                                        [](const std::string& json) { return NativeReplayEntryNativeAssistant{rt::jsonParse(json).as_array()}; });
  }

  std::optional<std::vector<AnthropicChatReplayEntry>> AnthropicReplayStore::restoreChat(const json::array& input, const AnthropicReplayContext& context) const {
    return restoreAs<AnthropicChatReplayEntry>(*state_, input, context, "chat",
                                               [](const std::string& json) { return AnthropicChatReplayEntryNativeChatAssistant{rt::jsonParse(json).as_object()}; });
  }

  AnthropicReplayStore createAnthropicReplayStore(ReplayLimits limits) {
    auto state = std::make_shared<AnthropicReplayStore::State>();
    state->now = limits.now ? limits.now : rt::globalDateNow();
    state->ttlMs = boundedLimit(limits.ttlMs, TTL_MS);
    state->maxEntries = boundedLimit(limits.maxEntries, MAX_ENTRIES);
    state->maxBytes = boundedLimit(limits.maxBytes, MAX_BYTES);
    state->maxEntryBytes = boundedLimit(limits.maxEntryBytes, MAX_ENTRY_BYTES);
    return AnthropicReplayStore(std::move(state));
  }

  namespace {
    const AnthropicReplayStore replayStore = createAnthropicReplayStore();
  }

  bool rememberAnthropicReplay(const ReplayPublication& publication) { return replayStore.remember(publication); }
  std::optional<std::vector<NativeReplayEntry>> restoreAnthropicReplay(const json::array& input, const AnthropicReplayContext& context) {
    return replayStore.restore(input, context);
  }
  bool rememberAnthropicChatReplay(const ChatReplayPublication& publication) { return replayStore.rememberChat(publication); }
  std::optional<std::vector<AnthropicChatReplayEntry>> restoreAnthropicChatReplay(const json::array& input, const AnthropicReplayContext& context) {
    return replayStore.restoreChat(input, context);
  }
}
