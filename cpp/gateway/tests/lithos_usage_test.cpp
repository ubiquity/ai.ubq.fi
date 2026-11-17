// Links: tests/lithos_usage_test.cpp src/provider/lithos.cpp src/provider/lithos_rate_limits.cpp src/utils.cpp
// Port of tests/lithos-usage.test.ts (every case).
#include "uos/provider/lithos.hpp"

#include "uos/rt/json.hpp"
#include "uos/rt/nullable.hpp"
#include "uos/rt/test.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace lithos = uos::provider::lithos;

namespace {
  const std::string REQUESTED_MODEL = "moonshotai/Kimi-K3";
  using Result = lithos::NormalizationResult<json::object>;

  json::object object(std::string_view text) { return rt::jsonParse(text).as_object(); }

  /** Shaped like the real buffered success probed on 2026-09-23, nulls included. */
  json::object completion(const json::object& overrides = {}) {
    const json::object base = object(
      R"({"id":"chatcmpl-lithos-1","object":"chat.completion","created":1790160326,"model":"moonshotai/Kimi-K3",)"
      R"("choices":[{"index":0,"message":{"role":"assistant","content":"OK","reasoning_content":null,"tool_calls":null},"logprobs":null,"finish_reason":"stop"}],)"
      R"("usage":{"prompt_tokens":15,"total_tokens":17,"completion_tokens":2,"prompt_tokens_details":null,"completion_tokens_details":{"reasoning_tokens":0}}})");
    return rt::objectSpread(base, overrides);
  }

  /** Shaped like the real streaming frames probed on 2026-09-23. */
  json::object chunk(const json::object& overrides = {}) {
    const json::object base = object(
      R"({"id":"chatcmpl-lithos-2","object":"chat.completion.chunk","created":1790160303,"model":"moonshotai/Kimi-K3",)"
      R"("choices":[{"index":0,"delta":{"reasoning_content":"The"},"logprobs":null,"finish_reason":null}]})");
    return rt::objectSpread(base, overrides);
  }

  bool isOk(const Result& result) { return std::holds_alternative<lithos::NormalizationResultOk<json::object>>(result); }

  // `if (!result.ok) throw new Error(result.message); ... result.value`
  json::object valueOf(const Result& result) {
    if (!isOk(result)) {
      rt::test::report(__FILE__, __LINE__, "expected an accepted payload, got: " + std::get<lithos::NormalizationResultNotOk>(result).message, true);
    }
    return std::get<lithos::NormalizationResultOk<json::object>>(result).value;
  }

  std::string messageOf(const Result& result) {
    if (isOk(result)) rt::test::report(__FILE__, __LINE__, "expected a refused payload", true);
    return std::get<lithos::NormalizationResultNotOk>(result).message;
  }

  // assert.deepEqual(actual, JSON.parse(expected))
  void expectJson(const json::value* actual, std::string_view expected, int line) {
    const json::value wanted = rt::jsonParse(expected);
    if (!actual || !rt::deepEqual(*actual, wanted)) {
      rt::test::report(__FILE__, line, "expected " + std::string(expected) + "\n      actual:   " + (actual ? rt::jsonStringify(*actual) : std::string("undefined")), false);
    }
  }
}

TEST("lithos: relays the measured prompt, completion, total, cache and reasoning counters") {
  const json::value payload(completion(object(
    R"({"usage":{"prompt_tokens":91,"total_tokens":204,"completion_tokens":113,"prompt_tokens_details":{"cached_tokens":64},"completion_tokens_details":{"reasoning_tokens":99}}})")));
  const json::object value = valueOf(lithos::normalizeLithosChatCompletion(payload, REQUESTED_MODEL));
  expectJson(rt::get(value, "usage"),
             R"({"prompt_tokens":91,"completion_tokens":113,"total_tokens":204,"prompt_tokens_details":{"cached_tokens":64},"completion_tokens_details":{"reasoning_tokens":99}})",
             __LINE__);
  // The client-facing id is echoed rather than the upstream spelling.
  EXPECT_EQ(*rt::getString(value, "model"), REQUESTED_MODEL);
}

TEST("lithos: an explicit null details object stays absent instead of becoming a measured zero") {
  const json::value payload(completion(object(
    R"({"usage":{"prompt_tokens":91,"total_tokens":204,"completion_tokens":113,"prompt_tokens_details":null,"completion_tokens_details":{"reasoning_tokens":99}}})")));
  const json::object value = valueOf(lithos::normalizeLithosChatCompletion(payload, REQUESTED_MODEL));
  // The real wire shape: `prompt_tokens_details` is present and null, which is
  // no cache measurement at all, while the reasoning count is a real integer.
  expectJson(rt::get(value, "usage"), R"({"prompt_tokens":91,"completion_tokens":113,"total_tokens":204,"completion_tokens_details":{"reasoning_tokens":99}})", __LINE__);

  // An explicit provider zero is a measurement, so it is relayed as one.
  const json::value zeroedPayload(
    completion(object(R"({"usage":{"prompt_tokens":15,"total_tokens":17,"completion_tokens":2,"prompt_tokens_details":{"cached_tokens":0}}})")));
  const json::object zeroed = valueOf(lithos::normalizeLithosChatCompletion(zeroedPayload, REQUESTED_MODEL));
  expectJson(rt::get(zeroed, "usage"), R"({"prompt_tokens":15,"completion_tokens":2,"total_tokens":17,"prompt_tokens_details":{"cached_tokens":0}})", __LINE__);
}

TEST("lithos: present-and-null message fields stay absent while a tool call keeps its opaque id") {
  const json::value plainPayload(completion());
  const json::object plain = valueOf(lithos::normalizeLithosChatCompletion(plainPayload, REQUESTED_MODEL));
  // `reasoning_content: null` and `tool_calls: null` are how this provider
  // reports "none"; neither may travel as an empty or null payload.
  expectJson(rt::get(plain, "choices"), R"([{"index":0,"message":{"role":"assistant","content":"OK"},"finish_reason":"stop"}])", __LINE__);

  const json::value calledPayload(completion(object(
    R"({"choices":[{"index":0,"message":{"role":"assistant","content":"","reasoning_content":null,)"
    R"("tool_calls":[{"id":"get_weather:0","index":0,"type":"function","function":{"name":"get_weather","arguments":"{\"city\": \"Paris\"}"}}]},)"
    R"("logprobs":null,"finish_reason":"tool_calls"}]})")));
  const json::object called = valueOf(lithos::normalizeLithosChatCompletion(calledPayload, REQUESTED_MODEL));
  expectJson(rt::get(called, "choices"),
             R"([{"index":0,"message":{"role":"assistant","content":"",)"
             R"("tool_calls":[{"id":"get_weather:0","type":"function","function":{"name":"get_weather","arguments":"{\"city\": \"Paris\"}"}}]},)"
             R"("finish_reason":"tool_calls"}])",
             __LINE__);
}

TEST("lithos: the chunk normalizer accepts the empty-choices totals frame and relays its usage") {
  // The authoritative totals arrive on a trailing chunk whose `choices` is an
  // EMPTY ARRAY. Rejecting this frame loses every streamed token measurement,
  // so it is accepted explicitly and its usage is relayed ungated.
  const json::value totalsPayload(chunk(object(
    R"({"created":1790160304,"choices":[],"usage":{"prompt_tokens":91,"total_tokens":204,"completion_tokens":113,"prompt_tokens_details":null,"completion_tokens_details":{"reasoning_tokens":99}}})")));
  const json::object totals = valueOf(lithos::normalizeLithosChatCompletionChunk(totalsPayload, REQUESTED_MODEL));
  expectJson(rt::get(totals, "choices"), "[]", __LINE__);
  EXPECT_EQ(*rt::getString(totals, "model"), REQUESTED_MODEL);
  expectJson(rt::get(totals, "usage"), R"({"prompt_tokens":91,"completion_tokens":113,"total_tokens":204,"completion_tokens_details":{"reasoning_tokens":99}})", __LINE__);

  // A content frame carries usage too, with no `stream_options` involved.
  const json::value contentPayload(chunk(object(
    R"({"usage":{"prompt_tokens":91,"total_tokens":92,"completion_tokens":1,"prompt_tokens_details":null,"completion_tokens_details":{"reasoning_tokens":0}}})")));
  const json::object content = valueOf(lithos::normalizeLithosChatCompletionChunk(contentPayload, REQUESTED_MODEL));
  expectJson(rt::get(content, "choices"), R"([{"index":0,"delta":{"reasoning_content":"The"},"finish_reason":null}])", __LINE__);
  expectJson(rt::get(content, "usage"), R"({"prompt_tokens":91,"completion_tokens":1,"total_tokens":92,"completion_tokens_details":{"reasoning_tokens":0}})", __LINE__);

  // The opening role frame is all nulls and no usage, and the terminal frame
  // carries only a finish reason.
  const json::value openingPayload(
    chunk(object(R"({"choices":[{"index":0,"delta":{"reasoning_content":null,"role":"assistant","content":""},"logprobs":null,"finish_reason":null}]})")));
  const json::object opening = valueOf(lithos::normalizeLithosChatCompletionChunk(openingPayload, REQUESTED_MODEL));
  expectJson(rt::get(opening, "choices"), R"([{"index":0,"delta":{"role":"assistant","content":""},"finish_reason":null}])", __LINE__);

  const json::value terminalPayload(
    chunk(object(R"({"created":1790160304,"choices":[{"index":0,"delta":{"reasoning_content":null},"logprobs":null,"finish_reason":"stop"}]})")));
  const json::object terminal = valueOf(lithos::normalizeLithosChatCompletionChunk(terminalPayload, REQUESTED_MODEL));
  expectJson(rt::get(terminal, "choices"), R"([{"index":0,"delta":{},"finish_reason":"stop"}])", __LINE__);
}

TEST("lithos: a malformed payload is refused with a message instead of throwing") {
  std::vector<json::value> malformedCompletions{
    json::value(nullptr),
    json::value("not a chat completion"),
    json::value(json::object()),
    json::value(completion(object(R"({"id":""})"))),
    json::value(completion(object(R"({"created":-1})"))),
    json::value(completion(object(R"({"created":"1790160326"})"))),
    json::value(completion(object(R"({"object":"chat.completion.chunk"})"))),
    json::value(completion(object(R"({"choices":[]})"))),
    json::value(completion(object(R"({"choices":[{"index":0,"message":{"role":"user","content":"hi"}}]})"))),
    json::value(completion(object(R"({"choices":[{"index":0,"message":{"role":"assistant","content":"hi"},"finish_reason":7}]})"))),
    json::value(completion(object(R"({"usage":{"prompt_tokens":15}})"))),
  };
  for (const json::value& payload : malformedCompletions) {
    const Result result = lithos::normalizeLithosChatCompletion(payload, REQUESTED_MODEL);
    if (isOk(result)) rt::test::report(__FILE__, __LINE__, "expected a refused buffered payload: " + rt::jsonStringify(payload), true);
    EXPECT(!isOk(result));
    EXPECT(messageOf(result).size() > 0);
  }

  std::vector<json::value> malformedChunks{
    json::value(nullptr),
    json::value(json::object()),
    json::value(chunk(object(R"({"choices":"none"})"))),
    json::value(chunk(object(R"({"id":""})"))),
    json::value(chunk(object(R"({"choices":[{"index":0,"delta":"text"}]})"))),
  };
  for (const json::value& payload : malformedChunks) {
    const Result result = lithos::normalizeLithosChatCompletionChunk(payload, REQUESTED_MODEL);
    if (isOk(result)) rt::test::report(__FILE__, __LINE__, "expected a refused chunk payload: " + rt::jsonStringify(payload), true);
    EXPECT(!isOk(result));
    EXPECT(messageOf(result).size() > 0);
  }
}

TEST("lithos: model resolution is case-insensitive and returns the canonical id") {
  EXPECT_EQ(lithos::LITHOS_MODEL_IDS.size(), std::size_t{8});
  for (const std::string_view id : lithos::LITHOS_MODEL_IDS) EXPECT_EQ(lithos::lithosUpstreamModelFor(std::string(id)), std::optional<std::string>(id));
  EXPECT_EQ(lithos::lithosUpstreamModelFor("moonshotai/kimi-k3-ultra-chat"), std::optional<std::string>("moonshotai/Kimi-K3-ultra-chat"));
  EXPECT_EQ(lithos::lithosUpstreamModelFor("  DEEPSEEK-AI/DEEPSEEK-V4.1-FLASH  "), std::optional<std::string>("deepseek-ai/DeepSeek-V4.1-Flash"));
  EXPECT_EQ(lithos::lithosUpstreamModelFor("nope/nope"), std::optional<std::string>());
  EXPECT_EQ(lithos::lithosUpstreamModelFor(""), std::optional<std::string>());
  EXPECT_EQ(lithos::lithosUpstreamModelFor("constructor"), std::optional<std::string>());
}

TEST("lithos: a supplied key wins and a missing key fails closed") {
  EXPECT_EQ(lithos::requireLithosApiKey(rt::Nullable<std::string>(std::string("lith_sk_fixture"))), std::string("lith_sk_fixture"));
  bool matched = false;
  try {
    (void)lithos::requireLithosApiKey(rt::Nullable<std::string>(std::string("   ")));
  } catch (const lithos::LithosError& error) {
    matched = error.code == "lithos_api_key_missing" && error.status == 503;
  } catch (...) {
  }
  EXPECT(matched);
}

TEST("lithos: the ultra-chat tier's normalized /models/ echo is accepted and the requested id is echoed back") {
  // Probed live on 2026-09-23: every completion for this tier came back with
  // the engine's own base slug, while GET /v1/models/<full id> still reported
  // the full id. An exact-equality check would refuse every real response.
  const std::string ultraChat = "deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat";
  const std::string normalizedEcho = "/models/DeepSeek-V4.1-Flash";

  json::object modelOverride;
  modelOverride["model"] = normalizedEcho;
  const json::value bufferedPayload(completion(modelOverride));
  const json::object buffered = valueOf(lithos::normalizeLithosChatCompletion(bufferedPayload, ultraChat));
  // The client-facing id is echoed rather than the upstream spelling.
  EXPECT_EQ(*rt::getString(buffered, "model"), ultraChat);

  json::object streamedOverride = object(
    R"({"created":1790160304,"choices":[],"usage":{"prompt_tokens":15,"total_tokens":17,"completion_tokens":2,"prompt_tokens_details":null,"completion_tokens_details":{"reasoning_tokens":0}}})");
  streamedOverride["model"] = normalizedEcho;
  const json::value streamedPayload(chunk(streamedOverride));
  const json::object streamed = valueOf(lithos::normalizeLithosChatCompletionChunk(streamedPayload, ultraChat));
  EXPECT_EQ(*rt::getString(streamed, "model"), ultraChat);
}

TEST("lithos: an absent, matching or slug-normalized model echo is accepted while a different model is refused") {
  // Rule 1: an absent echo claims nothing about the model, so it is no mismatch.
  // `completion({ model: null })` and `completion({ model: undefined })` (an undefined member is absent).
  json::object nullModel = completion();
  nullModel["model"] = nullptr;
  json::object undefinedModel = completion();
  rt::objectDelete(undefinedModel, "model");
  for (const json::object& payload : {nullModel, undefinedModel}) {
    const json::object value = valueOf(lithos::normalizeLithosChatCompletion(json::value(payload), REQUESTED_MODEL));
    EXPECT_EQ(*rt::getString(value, "model"), REQUESTED_MODEL);
  }

  // Rule 2: the ordinary verbatim echo, matched case-insensitively.
  const json::object matching = valueOf(lithos::normalizeLithosChatCompletion(json::value(completion(object(R"({"model":"MOONSHOTAI/KIMI-K3"})"))), REQUESTED_MODEL));
  EXPECT_EQ(*rt::getString(matching, "model"), REQUESTED_MODEL);

  // Rule 3: a normalized echo naming only the requested trailing path segment.
  const json::object slug = valueOf(lithos::normalizeLithosChatCompletion(json::value(completion(object(R"({"model":"/models/Kimi-K3"})"))), REQUESTED_MODEL));
  EXPECT_EQ(*rt::getString(slug, "model"), REQUESTED_MODEL);

  // A genuinely different model is still refused, buffered and streamed alike,
  // with the message the malformed-payload test relies on staying intact.
  for (const std::string model : {"deepseek-ai/DeepSeek-V4.1-Flash", "gpt-4"}) {
    json::object modelOverride;
    modelOverride["model"] = model;
    const Result buffered = lithos::normalizeLithosChatCompletion(json::value(completion(modelOverride)), REQUESTED_MODEL);
    if (isOk(buffered)) rt::test::report(__FILE__, __LINE__, "expected the " + model + " echo to be refused", true);
    EXPECT_EQ(messageOf(buffered), std::string("Upstream returned a different model than requested."));
    const Result streamed = lithos::normalizeLithosChatCompletionChunk(json::value(chunk(modelOverride)), REQUESTED_MODEL);
    if (isOk(streamed)) rt::test::report(__FILE__, __LINE__, "expected the " + model + " chunk echo to be refused", true);
    EXPECT_EQ(messageOf(streamed), std::string("Upstream returned a different model than requested."));
  }
}

