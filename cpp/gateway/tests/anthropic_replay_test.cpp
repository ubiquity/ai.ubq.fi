// Port of tests/anthropic-replay.test.ts.
#include "uos/provider/anthropic_replay.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test.hpp"
#include <limits>

namespace {
  namespace rt = uos::rt;
  namespace json = uos::json;
  using namespace uos::provider::anthropic_replay;

  const AnthropicReplayContext context{"opaque-authenticated-principal", "surplus", "claude-opus-5.5"};

  json::value parse(const std::string& text) { return rt::jsonParse(text); }

  ReplayPublication fixture(const std::string& suffix = "turn1", bool summary = true) {
    ReplayPublication publication;
    static_cast<AnthropicReplayContext&>(publication) = context;
    publication.nativeBlocks = parse(R"([
      { "type": "thinking", "thinking": "Inspect the input.", "signature": "real-provider-signature" },
      { "type": "redacted_thinking", "data": "real-provider-opaque-block" },
      { "type": "text", "text": "Reading the file." },
      { "type": "tool_use", "id": "call-)" + suffix + R"(", "name": "read_file", "input": { "path": "a.txt", "lines": 3 } }
    ])").as_array();
    if (summary)
      publication.output.push_back(parse(R"({ "type": "reasoning", "id": "rs_resp_)" + suffix +
                                         R"(_0", "status": "completed", "summary": [{ "type": "summary_text", "text": "Inspect the input." }] })"));
    publication.output.push_back(parse(R"({ "type": "message", "id": "msg_resp_)" + suffix + R"(_2", "status": "completed", "role": "assistant",
      "content": [{ "type": "output_text", "text": "Reading the file.", "annotations": [] }] })"));
    publication.output.push_back(parse(R"({ "type": "function_call", "id": "fc_resp_)" + suffix + R"(_3", "status": "completed", "call_id": "call-)" +
                                       suffix + R"(", "name": "read_file", "arguments": "{\"path\":\"a.txt\",\"lines\":3}" })"));
    return publication;
  }

  AnthropicReplayContext changedContext(const std::string& field, const std::string& value) {
    AnthropicReplayContext changed = context;
    if (field == "scope") changed.scope = value;
    if (field == "provider") changed.provider = value;
    if (field == "model") changed.model = value;
    return changed;
  }

  json::value entriesJson(const std::optional<std::vector<NativeReplayEntry>>& entries) {
    if (!entries) return nullptr;
    json::array out;
    for (const auto& entry : *entries) {
      json::object item;
      if (const auto* original = std::get_if<NativeReplayEntryOriginal>(&entry)) {
        item["type"] = "original";
        item["item"] = original->item;
      } else {
        item["type"] = "native_assistant";
        item["blocks"] = std::get<NativeReplayEntryNativeAssistant>(entry).blocks;
      }
      out.emplace_back(std::move(item));
    }
    return out;
  }
  json::value entriesJson(const std::optional<std::vector<AnthropicChatReplayEntry>>& entries) {
    if (!entries) return nullptr;
    json::array out;
    for (const auto& entry : *entries) {
      json::object item;
      if (const auto* original = std::get_if<NativeReplayEntryOriginal>(&entry)) {
        item["type"] = "original";
        item["item"] = original->item;
      } else {
        item["type"] = "native_chat_assistant";
        item["message"] = std::get<AnthropicChatReplayEntryNativeChatAssistant>(entry).message;
      }
      out.emplace_back(std::move(item));
    }
    return out;
  }
  json::value nativeEntry(const json::array& blocks) {
    json::object entry;
    entry["type"] = "native_assistant";
    entry["blocks"] = blocks;
    return json::array{json::value(std::move(entry))};
  }
  json::value chatEntry(const json::object& message) {
    json::object entry;
    entry["type"] = "native_chat_assistant";
    entry["message"] = message;
    return json::array{json::value(std::move(entry))};
  }
  json::object withChanges(const json::value& base, std::initializer_list<std::pair<std::string, std::optional<json::value>>> changes) {
    json::object out = base.as_object();
    for (const auto& [key, value] : changes) {
      if (value) rt::objectDefine(out, key, *value);
      else rt::objectDelete(out, key);
    }
    return out;
  }
  json::array slice(const json::array& values, std::size_t start, std::size_t end) {
    json::array out;
    for (std::size_t index = start; index < end && index < values.size(); ++index) out.push_back(values[index]);
    return out;
  }
}

TEST("Anthropic replay restores complete signed blocks and original order within authenticated history") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  const ReplayPublication receipt = fixture();
  EXPECT_EQ(store.remember(receipt), true);
  const json::value question = parse(R"({ "type": "message", "role": "user", "content": "Read a.txt" })");
  const json::value result = parse(R"({ "type": "function_call_output", "call_id": "call-turn1", "output": "file contents" })");
  json::array input{question};
  for (const auto& item : receipt.output) input.push_back(item);
  input.push_back(result);
  json::array expected;
  expected.push_back(json::object{{"type", "original"}, {"item", question}});
  expected.push_back(json::object{{"type", "native_assistant"}, {"blocks", receipt.nativeBlocks}});
  expected.push_back(json::object{{"type", "original"}, {"item", result}});
  EXPECT(rt::deepEqual(entriesJson(store.restore(input, context)), expected));
}

TEST("Anthropic replay accepts official Codex nulls and removed status/annotations without private wire data") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  const ReplayPublication receipt = fixture();
  EXPECT_EQ(store.remember(receipt), true);
  json::array replay;
  replay.push_back(withChanges(receipt.output[0], {{"status", std::nullopt}, {"content", json::value(nullptr)}, {"encrypted_content", json::value(nullptr)}}));
  replay.push_back(withChanges(receipt.output[1], {{"status", std::nullopt}, {"content", parse(R"([{ "type": "output_text", "text": "Reading the file." }])")}, {"phase", json::value(nullptr)}}));
  replay.push_back(withChanges(receipt.output[2], {{"status", std::nullopt},
                                                   {"namespace", json::value(nullptr)},
                                                   {"encrypted_function_args", json::value(nullptr)},
                                                   {"arguments", json::value("{ \"lines\": 3, \"path\": \"a.txt\" }")}}));
  EXPECT(rt::deepEqual(entriesJson(store.restore(replay, context)), nativeEntry(receipt.nativeBlocks)));
  EXPECT_EQ(rt::includes(rt::jsonStringify(json::value(receipt.output)), "real-provider-signature"), false);
  EXPECT_EQ(rt::includes(rt::jsonStringify(json::value(receipt.output)), "encrypted_content"), false);
}

TEST("Anthropic replay restores hidden thinking/redacted blocks when summaries were disabled") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  const ReplayPublication receipt = fixture("hidden1", false);
  EXPECT_EQ(store.remember(receipt), true);
  EXPECT(rt::deepEqual(entriesJson(store.restore(receipt.output, context)), nativeEntry(receipt.nativeBlocks)));
}

TEST("Anthropic replay never resolves another principal, provider, model or absent scope") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  for (const bool summary : {true, false}) {
    const ReplayPublication receipt = fixture(std::string("scope") + (summary ? "1" : "0"), summary);
    EXPECT_EQ(store.remember(receipt), true);
    for (const auto& changed : {changedContext("scope", "another-principal"), changedContext("provider", "metered"), changedContext("model", "claude-other"),
                                changedContext("scope", "")}) {
      EXPECT(!store.restore(receipt.output, changed).has_value());
    }
  }
}

TEST("Anthropic replay rejects opaque or unknown reasoning and unknown native output IDs") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  EXPECT(!store.restore(parse(R"([{ "type": "reasoning", "summary": [] }])").as_array(), context).has_value());
  EXPECT(!store.restore(parse(R"([{ "type": "reasoning", "id": "rs_external", "summary": [], "encrypted_content": "opaque-upstream-content" }])").as_array(), context)
              .has_value());
  EXPECT(!store.restore(fixture("unknown1", false).output, context).has_value());
  const json::value ordinary = parse(R"({ "type": "function_call", "id": "fc_external", "call_id": "call", "name": "read", "arguments": "{}" })");
  json::array expected;
  expected.push_back(json::object{{"type", "original"}, {"item", ordinary}});
  EXPECT(rt::deepEqual(entriesJson(store.restore(json::array{ordinary}, context)), expected));
}

TEST("Anthropic replay falls back on tampered content, reordered/missing/duplicated output groups") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  const ReplayPublication receipt = fixture();
  EXPECT_EQ(store.remember(receipt), true);
  const json::array& output = receipt.output;
  std::vector<json::array> inputs;
  inputs.push_back(slice(output, 1, 3));
  inputs.push_back(slice(output, 0, 2));
  inputs.push_back(json::array{output[0], output[2], output[1]});
  json::array doubled = output;
  for (const auto& item : output) doubled.push_back(item);
  inputs.push_back(doubled);
  json::array changedSummary{withChanges(output[0], {{"summary", parse(R"([{ "type": "summary_text", "text": "changed" }])")}})};
  for (const auto& item : slice(output, 1, 3)) changedSummary.push_back(item);
  inputs.push_back(changedSummary);
  inputs.push_back(json::array{output[0], withChanges(output[1], {{"content", parse(R"([{ "type": "output_text", "text": "changed" }])")}}), output[2]});
  inputs.push_back(json::array{output[0], output[1], withChanges(output[2], {{"arguments", json::value("{\"path\":\"secret.txt\"}")}})});
  json::array encrypted{withChanges(output[0], {{"encrypted_content", json::value("opaque")}})};
  for (const auto& item : slice(output, 1, 3)) encrypted.push_back(item);
  inputs.push_back(encrypted);
  json::array unknownContent{withChanges(output[0], {{"content", parse(R"([{ "type": "reasoning_text", "text": "unknown" }])")}})};
  for (const auto& item : slice(output, 1, 3)) unknownContent.push_back(item);
  inputs.push_back(unknownContent);
  for (const auto& input : inputs) EXPECT(!store.restore(input, context).has_value());
}

TEST("Anthropic replay clones retained and returned native blocks so callers cannot mutate receipts") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  ReplayPublication receipt = fixture();
  const json::array original = receipt.nativeBlocks;
  EXPECT_EQ(store.remember(receipt), true);
  receipt.nativeBlocks[0].as_object()["signature"] = "changed";
  auto first = store.restore(receipt.output, context);
  ASSERT(first.has_value());
  ASSERT(std::holds_alternative<NativeReplayEntryNativeAssistant>((*first)[0]));
  EXPECT(rt::deepEqual(json::value(std::get<NativeReplayEntryNativeAssistant>((*first)[0]).blocks), json::value(original)));
  std::get<NativeReplayEntryNativeAssistant>((*first)[0]).blocks[0].as_object()["signature"] = "changed-again";
  EXPECT(rt::deepEqual(entriesJson(store.restore(receipt.output, context)), nativeEntry(original)));
}

TEST("Anthropic replay expires receipts without extending TTL on reads, including hidden-summary turns") {
  double at = 100;
  ReplayLimits limits;
  limits.now = [&at] { return at; };
  limits.ttlMs = 10;
  const AnthropicReplayStore store = createAnthropicReplayStore(limits);
  const ReplayPublication receipt = fixture("expires1", false);
  EXPECT_EQ(store.remember(receipt), true);
  at = 109;
  EXPECT(store.restore(receipt.output, context).has_value());
  at = 110;
  EXPECT(!store.restore(receipt.output, context).has_value());
}

TEST("Anthropic replay bounds receipt count and evicts oldest without refreshing lookup order") {
  ReplayLimits limits;
  limits.maxEntries = 2;
  const AnthropicReplayStore store = createAnthropicReplayStore(limits);
  const ReplayPublication first = fixture("old1");
  const ReplayPublication second = fixture("old2");
  const ReplayPublication third = fixture("new3");
  EXPECT_EQ(store.remember(first), true);
  EXPECT_EQ(store.remember(second), true);
  EXPECT(store.restore(first.output, context).has_value());
  EXPECT_EQ(store.remember(third), true);
  EXPECT(!store.restore(first.output, context).has_value());
  EXPECT(store.restore(second.output, context).has_value());
  EXPECT(store.restore(third.output, context).has_value());
}

TEST("Anthropic replay bounds retained bytes and refuses oversized individual receipts") {
  const ReplayPublication small = fixture("small1");
  ReplayLimits limits;
  limits.maxBytes = 1700;
  const AnthropicReplayStore store = createAnthropicReplayStore(limits);
  EXPECT_EQ(store.remember(small), true);
  const ReplayPublication second = fixture("small2");
  EXPECT_EQ(store.remember(second), true);
  EXPECT(!store.restore(small.output, context).has_value());
  EXPECT(store.restore(second.output, context).has_value());
  ReplayLimits tiny;
  tiny.maxEntryBytes = 100;
  EXPECT_EQ(createAnthropicReplayStore(tiny).remember(fixture()), false);
}

TEST("Anthropic replay rejects unsigned thinking, invalid tools, inconsistent public output and ID collisions") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  ReplayPublication unsignedFixture = fixture("unsigned1");
  unsignedFixture.nativeBlocks[0].as_object()["signature"] = "";
  EXPECT_EQ(store.remember(unsignedFixture), false);
  ReplayPublication mismatch = fixture("mismatch1");
  mismatch.nativeBlocks[2].as_object()["text"] = "different";
  EXPECT_EQ(store.remember(mismatch), false);
  ReplayPublication invalid = fixture("invalid1");
  invalid.output[2].as_object()["arguments"] = "invalid-json";
  EXPECT_EQ(store.remember(invalid), false);
  ReplayPublication duplicate = fixture("duplicate1");
  duplicate.output[1].as_object()["id"] = duplicate.output[0].as_object()["id"];
  EXPECT_EQ(store.remember(duplicate), false);
  const ReplayPublication receipt = fixture();
  EXPECT_EQ(store.remember(receipt), true);
  EXPECT_EQ(store.remember(receipt), false);
  EXPECT(store.restore(receipt.output, context).has_value());
}

// The TS case's third assertion (a circular native block) has no json::value form.
TEST("Anthropic replay rejects excessive blocks, non-JSON numbers and circular native state") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  const ReplayPublication receipt = fixture();
  ReplayPublication excessive = receipt;
  excessive.nativeBlocks.clear();
  for (int index = 0; index < 257; ++index) excessive.nativeBlocks.push_back(receipt.nativeBlocks[0]);
  EXPECT_EQ(store.remember(excessive), false);
  ReplayPublication nonJson = fixture("nonjson1");
  json::object input;
  input["path"] = "a.txt";
  input["lines"] = std::numeric_limits<double>::quiet_NaN();
  nonJson.nativeBlocks[3].as_object()["input"] = std::move(input);
  EXPECT_EQ(store.remember(nonJson), false);
}

namespace {
  ChatReplayPublication chatFixture(const std::string& suffix = "chat1", bool summary = true) {
    ChatReplayPublication publication;
    static_cast<AnthropicReplayContext&>(publication) = context;
    publication.output = fixture(suffix, summary).output;
    publication.assistant = parse(R"({
      "role": "assistant",
      "content": "Reading the file.",
      "reasoning_details": [
        { "type": "reasoning.text", "text": "Inspect the input.", "signature": "original-chat-signature", "format": "anthropic-claude-v1", "index": 0 }
      ],
      "tool_calls": [{ "id": "call-)" + suffix + R"(", "type": "function", "function": { "name": "read_file", "arguments": "{\"path\":\"a.txt\",\"lines\":3}" } }]
    })").as_object();
    return publication;
  }
  json::object& toolCall(ChatReplayPublication& publication) { return publication.assistant["tool_calls"].as_array()[0].as_object(); }
  json::object& reasoningDetail(ChatReplayPublication& publication) { return publication.assistant["reasoning_details"].as_array()[0].as_object(); }
}

TEST("Anthropic Chat replay preserves exact original assistant and reasoning_details with Chat provenance") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  const ChatReplayPublication receipt = chatFixture();
  EXPECT_EQ(store.rememberChat(receipt), true);
  EXPECT(rt::deepEqual(entriesJson(store.restoreChat(receipt.output, context)), chatEntry(receipt.assistant)));
  EXPECT(!store.restore(receipt.output, context).has_value());
  const ReplayPublication native = fixture("native1");
  EXPECT_EQ(store.remember(native), true);
  EXPECT(!store.restoreChat(native.output, context).has_value());
  EXPECT(store.restore(native.output, context).has_value());
  EXPECT_EQ(rt::includes(rt::jsonStringify(json::value(receipt.output)), "original-chat-signature"), false);
}

TEST("Anthropic Chat replay keeps original details when explicit summaries were disabled") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  const ChatReplayPublication receipt = chatFixture("hiddenchat1", false);
  EXPECT_EQ(store.rememberChat(receipt), true);
  EXPECT(rt::deepEqual(entriesJson(store.restoreChat(receipt.output, context)), chatEntry(receipt.assistant)));
  for (const auto& changed : {changedContext("scope", "another-principal"), changedContext("provider", "metered"), changedContext("model", "claude-other")}) {
    EXPECT(!store.restoreChat(receipt.output, changed).has_value());
  }
}

TEST("Anthropic Chat replay validates namespaced and freeform tool mapping without converting original wire fields") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  ChatReplayPublication namespaced = chatFixture("namespace1");
  toolCall(namespaced)["function"].as_object()["name"] = "functions__read_file";
  namespaced.output[2].as_object()["namespace"] = "functions";
  namespaced.toolNames = rt::OrderedMap<std::string, ChatReplayPublication::ToolName>{{"functions__read_file", ChatReplayPublication::ToolName{"read_file", "functions"}}};
  EXPECT_EQ(store.rememberChat(namespaced), true);
  EXPECT(rt::deepEqual(entriesJson(store.restoreChat(namespaced.output, context)), chatEntry(namespaced.assistant)));
  json::array tampered = namespaced.output;
  tampered[2] = withChanges(tampered[2], {{"namespace", json::value("other")}});
  EXPECT(!store.restoreChat(tampered, context).has_value());

  ChatReplayPublication custom = chatFixture("custom1", false);
  toolCall(custom)["function"] = parse(R"({ "name": "apply_patch", "arguments": "{\"input\":\"*** Begin Patch\\n*** End Patch\"}" })");
  custom.output[1] = parse(R"({ "type": "custom_tool_call", "id": "ctc_resp_custom1_3", "call_id": "call-custom1", "name": "apply_patch", "input": "*** Begin Patch\n*** End Patch" })");
  custom.customToolNames = rt::OrderedSet<std::string>{"apply_patch"};
  EXPECT_EQ(store.rememberChat(custom), true);
  EXPECT(rt::deepEqual(entriesJson(store.restoreChat(custom.output, context)), chatEntry(custom.assistant)));
}

TEST("Anthropic Chat replay shares expiry, byte/count bounds and clone isolation with Messages receipts") {
  double at = 100;
  ReplayLimits limits;
  limits.now = [&at] { return at; };
  limits.ttlMs = 10;
  limits.maxEntries = 1;
  const AnthropicReplayStore store = createAnthropicReplayStore(limits);
  ChatReplayPublication chat = chatFixture("isolatedchat1", false);
  const json::object original = chat.assistant;
  EXPECT_EQ(store.rememberChat(chat), true);
  reasoningDetail(chat)["signature"] = "changed-after-publication";
  auto first = store.restoreChat(chat.output, context);
  ASSERT(first.has_value());
  ASSERT(std::holds_alternative<AnthropicChatReplayEntryNativeChatAssistant>((*first)[0]));
  std::get<AnthropicChatReplayEntryNativeChatAssistant>((*first)[0]).message["reasoning_details"] = json::array{};
  EXPECT(rt::deepEqual(entriesJson(store.restoreChat(chat.output, context)), chatEntry(original)));
  EXPECT_EQ(store.remember(fixture("nativeeviction1")), true);
  EXPECT(!store.restoreChat(chat.output, context).has_value());
  const ChatReplayPublication second = chatFixture("expireschat2", false);
  EXPECT_EQ(store.rememberChat(second), true);
  at = 110;
  EXPECT(!store.restoreChat(second.output, context).has_value());
  ReplayLimits tiny;
  tiny.maxEntryBytes = 100;
  EXPECT_EQ(createAnthropicReplayStore(tiny).rememberChat(chatFixture()), false);
}

TEST("Anthropic Chat replay rejects unknown reasoning variants, malformed tools, tampering and partial groups") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  ChatReplayPublication malformed = chatFixture("malformedchat1");
  reasoningDetail(malformed)["signature"] = "";
  EXPECT_EQ(store.rememberChat(malformed), false);
  ChatReplayPublication opaque = chatFixture("opaquechat1");
  reasoningDetail(opaque)["type"] = "reasoning.encrypted";
  EXPECT_EQ(store.rememberChat(opaque), false);
  ChatReplayPublication invalid = chatFixture("invalidchat1");
  toolCall(invalid)["function"].as_object()["arguments"] = "invalid-json";
  EXPECT_EQ(store.rememberChat(invalid), false);
  const ChatReplayPublication receipt = chatFixture();
  EXPECT_EQ(store.rememberChat(receipt), true);
  EXPECT(!store.restoreChat(slice(receipt.output, 1, receipt.output.size()), context).has_value());
  json::array doubled = receipt.output;
  for (const auto& item : receipt.output) doubled.push_back(item);
  EXPECT(!store.restoreChat(doubled, context).has_value());
  json::array altered{withChanges(receipt.output[0], {{"encrypted_content", json::value("opaque")}})};
  for (const auto& item : slice(receipt.output, 1, receipt.output.size())) altered.push_back(item);
  EXPECT(!store.restoreChat(altered, context).has_value());
}

TEST("Anthropic Chat replay preserves refusal and prefers canonical readable reasoning content") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  ChatReplayPublication receipt = chatFixture("prioritychat1");
  reasoningDetail(receipt)["text"] = "Original signed detail.";
  ChatReplayPublication withContent = receipt;
  rt::objectDefine(withContent.assistant, "reasoning_content", json::string("Inspect the input."));
  EXPECT_EQ(store.rememberChat(withContent), true);
  ChatReplayPublication refused;
  static_cast<AnthropicReplayContext&>(refused) = context;
  refused.assistant = parse(R"({ "role": "assistant", "content": null, "refusal": "Cannot do that." })").as_object();
  refused.output = parse(R"([{ "type": "message", "id": "msg_resp_refused1_0", "role": "assistant", "content": [{ "type": "refusal", "refusal": "Cannot do that." }] }])")
                       .as_array();
  EXPECT_EQ(store.rememberChat(refused), true);
  EXPECT(rt::deepEqual(entriesJson(store.restoreChat(refused.output, context)), chatEntry(refused.assistant)));
}

TEST("Anthropic Chat replay preserves provider reasoning spelling and its readable precedence") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  ChatReplayPublication receipt = chatFixture("nativereasoning1");
  reasoningDetail(receipt)["text"] = "Original signed detail.";
  json::object assistant = receipt.assistant;
  rt::objectDefine(assistant, "reasoning", json::string("Inspect the input."));
  ChatReplayPublication withReasoning = receipt;
  withReasoning.assistant = assistant;
  EXPECT_EQ(store.rememberChat(withReasoning), true);
  EXPECT(rt::deepEqual(entriesJson(store.restoreChat(receipt.output, context)), chatEntry(assistant)));
  ChatReplayPublication bad = chatFixture("badreasoning1");
  bad.assistant = assistant;
  rt::objectDefine(bad.assistant, "reasoning", 10);
  EXPECT_EQ(store.rememberChat(bad), false);
}

// The exported module-level store (rememberAnthropicReplay & co. share one store).
TEST("module-level replay functions share one store") {
  const ReplayPublication receipt = fixture("module1");
  EXPECT_EQ(rememberAnthropicReplay(receipt), true);
  EXPECT(rt::deepEqual(entriesJson(restoreAnthropicReplay(receipt.output, context)), nativeEntry(receipt.nativeBlocks)));
  EXPECT(!restoreAnthropicChatReplay(receipt.output, context).has_value());
  const ChatReplayPublication chat = chatFixture("modulechat1");
  EXPECT_EQ(rememberAnthropicChatReplay(chat), true);
  EXPECT(rt::deepEqual(entriesJson(restoreAnthropicChatReplay(chat.output, context)), chatEntry(chat.assistant)));
}

// Differential cases recorded from Bun 1.4.0 running src/provider/anthropic-replay.ts (not in
// the TS test file): exact receipt byte accounting and the key order of restored payloads.
namespace {
  bool acceptsAt(double maxEntryBytes, const ReplayPublication& publication) {
    ReplayLimits limits;
    limits.maxEntryBytes = maxEntryBytes;
    return createAnthropicReplayStore(limits).remember(publication);
  }
  bool acceptsChatAt(double maxEntryBytes, const ChatReplayPublication& publication) {
    ReplayLimits limits;
    limits.maxEntryBytes = maxEntryBytes;
    return createAnthropicReplayStore(limits).rememberChat(publication);
  }
}

TEST("differential: receipt byte accounting matches Bun") {
  EXPECT(acceptsAt(1144, fixture("small1")));
  EXPECT(!acceptsAt(1143, fixture("small1")));
  EXPECT(acceptsAt(911, fixture("hidden1", false)));
  EXPECT(!acceptsAt(910, fixture("hidden1", false)));
  EXPECT(acceptsChatAt(1190, chatFixture()));
  EXPECT(!acceptsChatAt(1189, chatFixture()));
  EXPECT(acceptsChatAt(989, chatFixture("hiddenchat1", false)));
  EXPECT(!acceptsChatAt(988, chatFixture("hiddenchat1", false)));
}

TEST("differential: restored payloads carry the store's canonical key order") {
  const AnthropicReplayStore store = createAnthropicReplayStore();
  const ReplayPublication receipt = fixture();
  EXPECT_EQ(store.remember(receipt), true);
  EXPECT_EQ(rt::jsonStringify(entriesJson(store.restore(receipt.output, context))),
            std::string(R"([{"type":"native_assistant","blocks":[{"signature":"real-provider-signature","thinking":"Inspect the input.","type":"thinking"},{"data":"real-provider-opaque-block","type":"redacted_thinking"},{"text":"Reading the file.","type":"text"},{"id":"call-turn1","input":{"lines":3,"path":"a.txt"},"name":"read_file","type":"tool_use"}]}])"));
  const ChatReplayPublication chat = chatFixture("orderchat1");
  EXPECT_EQ(store.rememberChat(chat), true);
  EXPECT_EQ(rt::jsonStringify(entriesJson(store.restoreChat(chat.output, context))),
            std::string(R"([{"type":"native_chat_assistant","message":{"content":"Reading the file.","reasoning_details":[{"format":"anthropic-claude-v1","index":0,"signature":"original-chat-signature","text":"Inspect the input.","type":"reasoning.text"}],"role":"assistant","tool_calls":[{"function":{"arguments":"{\"path\":\"a.txt\",\"lines\":3}","name":"read_file"},"id":"call-orderchat1","type":"function"}]}}])"));
  ReplayPublication odd = fixture("odd1");
  odd.nativeBlocks[3].as_object()["input"] = parse(R"({ "10": 1, "9": 2, "b": [1.5, -0, 1e21, "é😀"], "a": { "z": null, "": true }, "B": false, "_": 0 })");
  odd.output[2].as_object()["arguments"] = json::string(rt::jsonStringify(odd.nativeBlocks[3].as_object()["input"]));
  EXPECT_EQ(store.remember(odd), true);
  EXPECT_EQ(rt::jsonStringify(entriesJson(store.restore(odd.output, context))),
            std::string(R"([{"type":"native_assistant","blocks":[{"signature":"real-provider-signature","thinking":"Inspect the input.","type":"thinking"},{"data":"real-provider-opaque-block","type":"redacted_thinking"},{"text":"Reading the file.","type":"text"},{"id":"call-odd1","input":{"_":0,"10":1,"9":2,"a":{"":true,"z":null},"b":[1.5,0,1e+21,"é😀"],"B":false},"name":"read_file","type":"tool_use"}]}])"));
  ReplayPublication nullPart = fixture("nullpart1");
  nullPart.output[1].as_object()["content"] = parse("[null]");
  EXPECT_EQ(store.remember(nullPart), false);
}
