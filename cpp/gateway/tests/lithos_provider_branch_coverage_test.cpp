// Links: tests/lithos_provider_branch_coverage_test.cpp src/provider/lithos.cpp src/provider/lithos_rate_limits.cpp src/utils.cpp
// Port of tests/lithos-provider-branch-coverage.test.ts (every case). A json::value has no
// `toJSON` member, so the `{ toJSON: () => undefined }` body (JSON.stringify yields undefined) is a
// body nested past rt::kJsonMaxDepth (JSON.stringify throws): the transport refuses both with the
// same LithosError before dispatch.
#include "uos/provider/lithos.hpp"

#include "uos/api_key_policy.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/text_codec.hpp"
#include "uos/rt/test_async.hpp"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
namespace lithos = uos::provider::lithos;
namespace api_key_policy = uos::api_key_policy;

/**
 * Branch coverage for the LithosAI Chat Completions provider.
 *
 * Every case drives an exported entry point with an in-memory fixture and
 * asserts the observable result: the normalized frame, the rejection message,
 * the client-visible error code and status, or the frames a stream yields.
 * There is no network, no KV, no port and no wall-clock assertion.
 */
namespace {
  const std::string MODEL = "deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat";
  const std::string KIMI = "moonshotai/Kimi-K3";
  const std::string FIRST_MODEL(lithos::LITHOS_MODEL_IDS[0]);
  using Result = lithos::NormalizationResult<json::object>;
  using Dispatch = api_key_policy::ApiKeyProviderDispatch;

  json::object object(std::string_view text) { return rt::jsonParse(text).as_object(); }

  json::value USAGE() { return rt::jsonParse(R"({"prompt_tokens":3,"completion_tokens":2,"total_tokens":5})"); }

  json::object completion(const json::object& message, const json::object& overrides = {}) {
    json::object choice;
    choice["index"] = 0;
    choice["message"] = message;
    choice["finish_reason"] = "stop";
    json::array choices;
    choices.push_back(std::move(choice));
    json::object base;
    base["id"] = "chatcmpl-lithos-branch";
    base["object"] = "chat.completion";
    base["created"] = 1780000000;
    base["model"] = MODEL;
    base["choices"] = std::move(choices);
    base["usage"] = USAGE();
    return rt::objectSpread(base, overrides);
  }

  json::object chunk(const json::value& choices, const json::object& extra = {}) {
    json::object base;
    base["id"] = "chatcmpl-lithos-chunk";
    base["object"] = "chat.completion.chunk";
    base["created"] = 1780000001;
    base["model"] = MODEL;
    base["choices"] = choices;
    return rt::objectSpread(base, extra);
  }

  bool isOk(const Result& result) { return std::holds_alternative<lithos::NormalizationResultOk<json::object>>(result); }

  std::string rejectionMessage(const Result& result) {
    if (isOk(result)) rt::test::report(__FILE__, __LINE__, "expected the payload to be rejected", true);
    return std::get<lithos::NormalizationResultNotOk>(result).message;
  }

  json::object valueOf(const Result& result) {
    if (!isOk(result)) rt::test::report(__FILE__, __LINE__, std::get<lithos::NormalizationResultNotOk>(result).message, true);
    return std::get<lithos::NormalizationResultOk<json::object>>(result).value;
  }

  json::object normalizedCompletion(const json::object& value, const std::string& requestedModel = MODEL) {
    return valueOf(lithos::normalizeLithosChatCompletion(json::value(value), requestedModel));
  }

  // `normalizedChoice(message)` keeps the fixture's own finish_reason; `omitFinishReason` is the
  // TS `null` argument, which removes the field so the normalizer has to derive it.
  json::object normalizedChoice(const json::object& message, bool omitFinishReason = false, const std::string& requestedModel = MODEL) {
    json::object raw = completion(message);
    if (omitFinishReason) {
      for (json::value& entry : raw["choices"].as_array()) rt::objectDelete(entry.as_object(), "finish_reason");
    }
    raw["model"] = requestedModel;
    const json::object value = valueOf(lithos::normalizeLithosChatCompletion(json::value(raw), requestedModel));
    return rt::get(value, "choices")->as_array()[0].as_object();
  }

  json::object normalizedChunk(const json::object& value) { return valueOf(lithos::normalizeLithosChatCompletionChunk(json::value(value), MODEL)); }

  rt::Response sseResponse(std::string body) {
    rt::ResponseInit init;
    init.status = 200;
    init.headers = rt::Headers{{"Content-Type", "text/event-stream"}};
    return rt::Response(rt::BodyInit(std::move(body)), init);
  }

  rt::Task<std::vector<json::object>> collectFrames(rt::Response response, std::optional<rt::AbortSignal> signal) {
    std::vector<json::object> frames;
    lithos::IterateLithosChatCompletionStreamOptions options;
    if (signal) options.signal = signal;
    const rt::AsyncGenerator<json::object> iterator = lithos::iterateLithosChatCompletionStream(response, MODEL, options);
    for (;;) {
      rt::IteratorResult<json::object> step = co_await iterator.next();
      if (step.done) break;
      frames.push_back(std::move(*step.value));
    }
    co_return frames;
  }

  // `promise.catch((error) => error)`: the rejection reason, or a null Reason when it resolved.
  template <class T> rt::Task<rt::Reason> rejectionOf(rt::Task<T> task) {
    rt::Reason failure;
    try {
      co_await std::move(task);
    } catch (...) {
      failure = std::current_exception();
    }
    co_return failure;
  }

  struct LithosErrorFields {
    std::string code;
    double status;
    std::string message;
  };
  // `error instanceof LithosError` with its fields.
  std::optional<LithosErrorFields> lithosErrorOf(const rt::Reason& error) {
    if (!error) return std::nullopt;
    try {
      std::rethrow_exception(error);
    } catch (const lithos::LithosError& lithosError) {
      return LithosErrorFields{lithosError.code, lithosError.status, lithosError.message};
    } catch (...) {
      return std::nullopt;
    }
  }

  // assert.match(text, /pattern/)
  void expectMatch(const std::string& text, std::string_view pattern, int line) {
    if (!rt::Regex(pattern).test(text)) rt::test::report(__FILE__, line, rt::test::show(text) + " does not match /" + std::string(pattern) + "/", false);
  }

  json::object deeplyNestedBody() {
    json::value nested = json::object();
    for (std::size_t level = 0; level <= rt::kJsonMaxDepth; ++level) {
      json::object wrapper;
      wrapper["toJSON"] = std::move(nested);
      nested = std::move(wrapper);
    }
    return nested.as_object();
  }
}

ASYNC_TEST("lithos transport rejects an unknown model and an unserializable body before dispatch") {
  bool dispatched = false;
  const lithos::LithosFetch fetcher([&dispatched](rt::RequestInfo, rt::RequestInit) {
    dispatched = true;
    rt::ResponseInit init;
    init.status = 200;
    return rt::Response(rt::BodyInit(std::string("{}")), init);
  });

  // The projection's own client error is rethrown unchanged rather than rewritten.
  lithos::LithosChatCompletionsOptions options;
  options.apiKey = std::string("lith_sk_test");
  options.fetcher = fetcher;
  auto unknownTask = lithos::fetchLithosChatCompletions(json::object(), "not/a-lithos-model", options);
  auto unknownSettled = rejectionOf(std::move(unknownTask));
  const rt::Reason unknownReason = co_await std::move(unknownSettled);
  const std::optional<LithosErrorFields> unknownModel = lithosErrorOf(unknownReason);
  ASSERT(unknownModel.has_value());
  EXPECT_EQ(unknownModel->code, std::string("lithos_request_invalid"));
  EXPECT_EQ(unknownModel->status, 400.0);
  expectMatch(unknownModel->message, "not configured", __LINE__);

  // A body whose `toJSON` probe yields no string is not serializable on this wire.
  auto unserializableTask = lithos::fetchLithosChatCompletions(deeplyNestedBody(), FIRST_MODEL, options);
  auto unserializableSettled = rejectionOf(std::move(unserializableTask));
  const rt::Reason unserializableReason = co_await std::move(unserializableSettled);
  const std::optional<LithosErrorFields> unserializable = lithosErrorOf(unserializableReason);
  ASSERT(unserializable.has_value());
  EXPECT_EQ(unserializable->code, std::string("lithos_request_invalid"));
  EXPECT_EQ(unserializable->status, 400.0);
  expectMatch(unserializable->message, "JSON-serializable body", __LINE__);
  EXPECT_EQ(dispatched, false);

  // A blank supplied key is treated as absent, not as an override.
  lithos::LithosChatCompletionsOptions blankOptions;
  blankOptions.apiKey = std::string("   ");
  blankOptions.fetcher = fetcher;
  auto blankTask = lithos::fetchLithosChatCompletions(json::object(), FIRST_MODEL, blankOptions);
  auto blankSettled = rejectionOf(std::move(blankTask));
  const rt::Reason blankReason = co_await std::move(blankSettled);
  const std::optional<LithosErrorFields> blankKey = lithosErrorOf(blankReason);
  ASSERT(blankKey.has_value());
  EXPECT_EQ(blankKey->code, std::string("lithos_api_key_missing"));
  EXPECT_EQ(blankKey->status, 503.0);
  EXPECT_EQ(dispatched, false);
}

ASYNC_TEST("lithos transport classifies a caller timeout and a blown header deadline as gateway timeouts") {
  const rt::AbortController caller;
  caller.abort(rt::makeReason(rt::DOMException("caller deadline", "TimeoutError")));
  int dispatched = 0;
  const lithos::LithosFetch fetcher([&dispatched](rt::RequestInfo, rt::RequestInit) {
    dispatched += 1;
    rt::ResponseInit init;
    init.status = 200;
    return rt::Response(rt::BodyInit(std::string("{}")), init);
  });

  // The caller's own TimeoutError identity wins over the abort classification.
  lithos::LithosChatCompletionsOptions callerOptions;
  callerOptions.apiKey = std::string("lith_sk_test");
  callerOptions.signal = caller.signal();
  callerOptions.fetcher = fetcher;
  auto callerTask = lithos::fetchLithosChatCompletions(json::object(), FIRST_MODEL, callerOptions);
  auto callerSettled = rejectionOf(std::move(callerTask));
  const rt::Reason callerReason = co_await std::move(callerSettled);
  const std::optional<LithosErrorFields> callerTimeout = lithosErrorOf(callerReason);
  ASSERT(callerTimeout.has_value());
  EXPECT_EQ(callerTimeout->code, std::string("gateway_timeout"));
  EXPECT_EQ(callerTimeout->status, 504.0);
  EXPECT_EQ(dispatched, 0);

  // An expiring header deadline aborts the dispatch before the transport is touched.
  lithos::setLithosFetchTimeoutMsForTest(1);
  const auto restore = rt::scopeExit([] { lithos::setLithosFetchTimeoutMsForTest(std::nullopt); });
  lithos::LithosChatCompletionsOptions deadlineOptions;
  deadlineOptions.apiKey = std::string("lith_sk_test");
  deadlineOptions.fetcher = fetcher;
  // `() => new Promise<void>((resolve) => setTimeout(resolve, 5))`
  deadlineOptions.beforeDispatch = []() -> rt::Task<std::optional<Dispatch>> {
    co_await rt::sleep(5);
    co_return std::nullopt;
  };
  auto deadlineTask = lithos::fetchLithosChatCompletions(json::object(), FIRST_MODEL, deadlineOptions);
  auto deadlineSettled = rejectionOf(std::move(deadlineTask));
  const rt::Reason deadlineReason = co_await std::move(deadlineSettled);
  const std::optional<LithosErrorFields> deadline = lithosErrorOf(deadlineReason);
  ASSERT(deadline.has_value());
  EXPECT_EQ(deadline->code, std::string("gateway_timeout"));
  EXPECT_EQ(deadline->status, 504.0);
  EXPECT_EQ(dispatched, 0);
}

TEST("lithos buffered normalization accepts an absent model echo and rejects an unusable one") {
  // A non-string echo carries no model claim, so it is not a mismatch.
  const json::object scalarEcho = normalizedCompletion(completion(object(R"({"role":"assistant","content":"hello"})"), object(R"({"model":42})")));
  EXPECT_EQ(rt::getString(scalarEcho, "model"), std::optional<std::string_view>(MODEL));

  // The provider folds the `-ultra-chat` tier onto the base slug; that echo is accepted.
  const json::object foldedEcho =
    normalizedCompletion(completion(object(R"({"role":"assistant","content":"hello"})"), object(R"({"model":"/models/DeepSeek-V4.1-Flash"})")));
  EXPECT_EQ(rt::getString(foldedEcho, "model"), std::optional<std::string_view>(MODEL));

  // An exact canonical echo (any casing) is accepted for the other models too.
  const json::object kim = normalizedCompletion(
    object(R"({"id":"chatcmpl-lithos-kimi","object":"chat.completion","created":1780000000,"model":"moonshotai/kimi-k3",)"
           R"("choices":[{"index":0,"message":{"role":"assistant","content":"hi"},"finish_reason":"stop"}]})"),
    KIMI);
  EXPECT_EQ(rt::getString(kim, "model"), std::optional<std::string_view>(KIMI));

  const Result mismatch =
    lithos::normalizeLithosChatCompletion(json::value(completion(object(R"({"role":"assistant","content":"hello"})"), object(R"({"model":"moonshotai/Kimi-K3"})"))), MODEL);
  expectMatch(rejectionMessage(mismatch), "different model than requested", __LINE__);
}

TEST("lithos buffered normalization rejects every unusable message field") {
  const auto rejected = [](std::string_view message, const json::object& overrides = {}) {
    return rejectionMessage(lithos::normalizeLithosChatCompletion(json::value(completion(object(message), overrides)), MODEL));
  };
  expectMatch(rejected(R"({"role":"user","content":"hello"})"), "does not contain an assistant message", __LINE__);
  expectMatch(rejected(R"({"role":"assistant","content":42})"), "unsupported message content", __LINE__);
  expectMatch(rejected(R"({"role":"assistant","content":"hi","reasoning_content":42})"), "invalid reasoning content", __LINE__);
  expectMatch(rejected(R"({"role":"assistant","content":"hi","refusal":5})"), "invalid refusal", __LINE__);
  // A message with neither content nor a tool call is not answer-bearing.
  expectMatch(rejected(R"({"role":"assistant"})"), "neither content nor a tool call", __LINE__);
  expectMatch(rejected(R"({"role":"assistant","content":"hi","tool_calls":"none"})"), "invalid tool calls", __LINE__);
  expectMatch(rejected(R"({"role":"assistant","content":"hi"})", object(R"({"choices":[{"index":0,"message":{"role":"assistant","content":"hi"},"finish_reason":7}]})")),
              "invalid finish reason", __LINE__);

  // An answer-bearing refusal string is carried through unchanged.
  const json::object refused = normalizedChoice(object(R"({"role":"assistant","content":"hi","refusal":"I cannot help with that."})"));
  EXPECT_EQ(rt::getString(*rt::get(refused, "message"), "refusal"), std::optional<std::string_view>("I cannot help with that."));

  // A choice without an explicit finish reason derives it from its payload shape.
  EXPECT_EQ(rt::getString(normalizedChoice(object(R"({"role":"assistant","content":"hi"})"), true), "finish_reason"), std::optional<std::string_view>("stop"));
  EXPECT_EQ(rt::getString(normalizedChoice(object(R"({"role":"assistant","content":null,"tool_calls":[{"id":"f:0","type":"function","function":{"name":"f","arguments":"{}"}}]})"), true),
                          "finish_reason"),
            std::optional<std::string_view>("tool_calls"));

  // A choice whose message is not an object is rejected before any message field.
  const Result badChoice =
    lithos::normalizeLithosChatCompletion(json::value(completion(object(R"({"role":"assistant","content":"hi"})"), object(R"({"choices":[{"index":0,"message":"nope"}]})"))), MODEL);
  expectMatch(rejectionMessage(badChoice), "missing an assistant message", __LINE__);
  const Result badIndex = lithos::normalizeLithosChatCompletion(
    json::value(completion(object(R"({"role":"assistant","content":"hi"})"), object(R"({"choices":[{"message":{"role":"assistant","content":"hi"}}]})"))), MODEL);
  expectMatch(rejectionMessage(badIndex), "has an invalid index", __LINE__);
}

TEST("lithos tool calls are validated field by field on the buffered path") {
  const auto withToolCall = [](std::string_view toolCall) {
    json::object message = object(R"({"role":"assistant","content":null})");
    message["tool_calls"] = rt::jsonParse(toolCall);
    return lithos::normalizeLithosChatCompletion(json::value(completion(message)), MODEL);
  };

  expectMatch(rejectionMessage(withToolCall("[5]")), "tool call 0 is not an object", __LINE__);
  expectMatch(rejectionMessage(withToolCall(R"([{"type":"function","function":{"name":"f","arguments":"{}"}}])")), "missing an id", __LINE__);
  expectMatch(rejectionMessage(withToolCall(R"([{"id":"f:0","function":{"name":"f","arguments":"{}"}}])")), "unsupported type", __LINE__);
  expectMatch(rejectionMessage(withToolCall(R"([{"id":"f:0","type":"function","function":"nope"}])")), "missing its function", __LINE__);
  expectMatch(rejectionMessage(withToolCall(R"([{"id":"f:0","type":"function","function":{"arguments":"{}"}}])")), "missing a function name", __LINE__);
  expectMatch(rejectionMessage(withToolCall(R"([{"id":"f:0","type":"function","function":{"name":"f","arguments":5}}])")), "non-string arguments", __LINE__);
  // An id with surrounding whitespace is not an opaque wire value.
  expectMatch(rejectionMessage(withToolCall(R"([{"id":" f:0 ","type":"function","function":{"name":"f","arguments":"{}"}}])")), "missing an id", __LINE__);

  // The accepted call is relayed verbatim, name-index id included.
  const json::object accepted =
    normalizedCompletion(completion(object(R"({"role":"assistant","content":null,"tool_calls":[{"id":"f:0","type":"function","function":{"name":"f","arguments":"{\"a\":1}"}}]})")));
  const json::value& message = *rt::get(rt::get(accepted, "choices")->as_array()[0], "message");
  EXPECT(rt::deepEqual(*rt::get(message, "tool_calls"), rt::jsonParse(R"([{"id":"f:0","type":"function","function":{"name":"f","arguments":"{\"a\":1}"}}])")));
  EXPECT(rt::get(message, "content") && rt::get(message, "content")->is_null());
}

TEST("lithos chunk normalization validates delta fields and the chunk envelope") {
  const auto chunkOutcome = [](const json::object& frame) { return lithos::normalizeLithosChatCompletionChunk(json::value(frame), MODEL); };
  const auto choices = [](std::string_view text) { return chunk(rt::jsonParse(text)); };

  expectMatch(rejectionMessage(chunkOutcome(choices(R"([{"index":0,"delta":"nope"}])"))), "invalid delta", __LINE__);
  expectMatch(rejectionMessage(chunkOutcome(choices(R"([{"index":0,"delta":{"role":5}}])"))), "invalid delta role", __LINE__);
  expectMatch(rejectionMessage(chunkOutcome(choices(R"([{"index":0,"delta":{"content":5}}])"))), "invalid content", __LINE__);
  expectMatch(rejectionMessage(chunkOutcome(choices(R"([{"index":0,"delta":{"reasoning_content":5}}])"))), "invalid reasoning_content", __LINE__);
  expectMatch(rejectionMessage(chunkOutcome(choices(R"([{"index":0,"delta":{"refusal":5}}])"))), "invalid refusal", __LINE__);
  expectMatch(rejectionMessage(chunkOutcome(choices(R"([{"index":0,"delta":{"tool_calls":[5]}}])"))), "tool call delta 0 is not an object", __LINE__);
  expectMatch(rejectionMessage(chunkOutcome(choices(R"([{"index":0,"delta":{"tool_calls":[{"index":-1}]}}])"))), "invalid index", __LINE__);
  expectMatch(rejectionMessage(chunkOutcome(choices(R"([{"index":0,"delta":{"tool_calls":[{"id":" padded "}]}}])"))), "invalid id", __LINE__);
  expectMatch(rejectionMessage(chunkOutcome(choices(R"([{"index":0,"delta":{"tool_calls":[{"type":"other"}]}}])"))), "unsupported type", __LINE__);
  expectMatch(rejectionMessage(chunkOutcome(choices(R"([{"index":0,"delta":{"tool_calls":[{"function":"nope"}]}}])"))), "invalid function", __LINE__);
  expectMatch(rejectionMessage(chunkOutcome(choices(R"([{"index":0,"delta":{"tool_calls":[{"function":{"name":5}}]}}])"))), "invalid function name", __LINE__);
  expectMatch(rejectionMessage(chunkOutcome(choices(R"([{"index":0,"delta":{"tool_calls":[{"function":{"arguments":5}}]}}])"))), "non-string arguments", __LINE__);
  // A tool-call delta carrying no field at all is not a frame the client can apply.
  expectMatch(rejectionMessage(chunkOutcome(choices(R"([{"index":0,"delta":{"tool_calls":[{"function":{}}]}}])"))), "carries no fields", __LINE__);
  // A choice delta with no fields is relayed as an empty delta rather than dropped.
  EXPECT(rt::deepEqual(*rt::get(normalizedChunk(choices(R"([{"index":0,"delta":{}}])")), "choices"), rt::jsonParse(R"([{"index":0,"delta":{}}])")));
  expectMatch(rejectionMessage(chunkOutcome(choices(R"([{"delta":{"content":"hi"}}])"))), "invalid index", __LINE__);
  expectMatch(rejectionMessage(chunkOutcome(choices(R"([{"index":0,"delta":{"content":"hi"},"finish_reason":7}])"))), "invalid finish reason", __LINE__);
  expectMatch(rejectionMessage(chunkOutcome(chunk(json::value("nope")))), "no choices array", __LINE__);
  expectMatch(rejectionMessage(chunkOutcome(chunk(rt::jsonParse(R"([{"index":0,"delta":{"content":"hi"}}])"), object(R"({"object":"chat.completion"})")))),
              "did not return a Chat Completion chunk", __LINE__);
  expectMatch(rejectionMessage(chunkOutcome(chunk(rt::jsonParse(R"([{"index":0,"delta":{"content":"hi"}}])"), object(R"({"usage":{"prompt_tokens":1}})")))),
              "usage is incomplete", __LINE__);

  // A delta refusal string and a delta tool-call continuation are relayed.
  const json::object refused = normalizedChunk(choices(R"([{"index":0,"delta":{"content":"hi","refusal":"no thanks"}}])"));
  EXPECT(rt::deepEqual(*rt::get(refused, "choices"), rt::jsonParse(R"([{"index":0,"delta":{"content":"hi","refusal":"no thanks"}}])")));
  const json::object toolDelta = normalizedChunk(choices(R"([{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"name":"f","arguments":"{}"}}]}}])"));
  EXPECT(rt::deepEqual(*rt::get(toolDelta, "choices"), rt::jsonParse(R"([{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"name":"f","arguments":"{}"}}]}}])")));
  // The totals frame is a valid chunk with an empty choices array.
  json::object usageExtra;
  usageExtra["usage"] = USAGE();
  const json::object totals = normalizedChunk(chunk(json::value(json::array()), usageExtra));
  EXPECT(rt::deepEqual(*rt::get(totals, "choices"), rt::jsonParse("[]")));
  EXPECT(rt::deepEqual(*rt::get(totals, "usage"), USAGE()));
}

ASYNC_TEST("lithos SSE framing skips a data-free frame and rejects a bare data line") {
  const std::string payload = rt::jsonStringify(json::value(chunk(rt::jsonParse(R"([{"index":0,"delta":{"content":"hi"}}])"))));
  // A leading blank frame carries no `data:` line and is skipped, not relayed.
  auto collecting = collectFrames(sseResponse("\r\rdata: " + payload + "\r\rdata: [DONE]\r\r"), std::nullopt);
  const std::vector<json::object> frames = co_await std::move(collecting);
  ASSERT_EQ(frames.size(), std::size_t{1});
  EXPECT(rt::deepEqual(*rt::get(frames[0], "choices"), rt::jsonParse(R"([{"index":0,"delta":{"content":"hi"}}])")));

  // A `data` line without a colon is an empty payload, which is malformed JSON.
  const rt::AsyncGenerator<json::object> bareIterator = lithos::iterateLithosChatCompletionStream(sseResponse("data\n\n"), MODEL);
  auto bareSettled = rejectionOf(bareIterator.next());
  const rt::Reason bareReason = co_await std::move(bareSettled);
  const std::optional<LithosErrorFields> bare = lithosErrorOf(bareReason);
  ASSERT(bare.has_value());
  EXPECT_EQ(bare->code, std::string("lithos_upstream_invalid_response"));
  EXPECT_EQ(bare->status, 502.0);

  // A comment-only frame is skipped rather than relayed.
  const rt::AsyncGenerator<json::object> commentedIterator = lithos::iterateLithosChatCompletionStream(sseResponse(": keep-alive\n\ndata: [DONE]\n\n"), MODEL);
  const rt::IteratorResult<json::object> commented = co_await commentedIterator.next();
  EXPECT_EQ(commented.done, true);

  // A response without a body cannot be framed.
  rt::ResponseInit init;
  init.status = 200;
  const rt::AsyncGenerator<json::object> bodylessIterator = lithos::iterateLithosChatCompletionStream(rt::Response(nullptr, init), MODEL);
  auto bodylessSettled = rejectionOf(bodylessIterator.next());
  const rt::Reason bodylessReason = co_await std::move(bodylessSettled);
  const std::optional<LithosErrorFields> bodyless = lithosErrorOf(bodylessReason);
  ASSERT(bodyless.has_value());
  EXPECT_EQ(bodyless->code, std::string("lithos_upstream_invalid_response"));
  EXPECT_EQ(bodyless->status, 502.0);
  expectMatch(bodyless->message, "no Chat Completions stream body", __LINE__);

  // A stream that ends without [DONE] is truncated, not a finished answer.
  auto truncatedSettled = rejectionOf(collectFrames(sseResponse("data: " + payload + "\n\n"), std::nullopt));
  const rt::Reason truncatedReason = co_await std::move(truncatedSettled);
  const std::optional<LithosErrorFields> truncated = lithosErrorOf(truncatedReason);
  ASSERT(truncated.has_value());
  EXPECT_EQ(truncated->code, std::string("lithos_upstream_invalid_response"));
  expectMatch(truncated->message, "ended before \\[DONE\\]", __LINE__);
}

ASYNC_TEST("lithos stream failures keep the caller's abort reason and fail closed on a broken reader") {
  // An already-aborted signal rejects the pending read with the caller's reason.
  const rt::AbortController controller;
  const rt::Reason abortReason = rt::makeReason(rt::JsError("caller left"));
  controller.abort(abortReason);
  lithos::IterateLithosChatCompletionStreamOptions options;
  options.signal = controller.signal();
  const rt::AsyncGenerator<json::object> abortedIterator = lithos::iterateLithosChatCompletionStream(sseResponse("data: [DONE]\n\n"), MODEL, options);
  auto abortedSettled = rejectionOf(abortedIterator.next());
  const rt::Reason aborted = co_await std::move(abortedSettled);
  EXPECT(aborted == abortReason);

  // A reader that rejects is an invalid upstream response, not a caller abort.
  rt::UnderlyingSource<rt::ByteChunk> brokenSource;
  brokenSource.start = [](rt::ReadableStreamDefaultController<rt::ByteChunk> source) { source.error(rt::makeReason(rt::JsError("socket reset"))); };
  const rt::ByteStream broken(brokenSource);
  const rt::AsyncGenerator<json::object> brokenIterator = lithos::iterateLithosChatCompletionStream(rt::Response(rt::BodyInit(broken)), MODEL);
  auto failedSettled = rejectionOf(brokenIterator.next());
  const rt::Reason failedReason = co_await std::move(failedSettled);
  const std::optional<LithosErrorFields> failed = lithosErrorOf(failedReason);
  ASSERT(failed.has_value());
  EXPECT_EQ(failed->code, std::string("lithos_upstream_invalid_response"));
  expectMatch(failed->message, "stream failed", __LINE__);
}

ASYNC_TEST("lithos stream sessions release their reader lock even when the upstream cancels badly") {
  // The upstream `cancel` itself throws, and the release still happens.
  const auto cancelAttempted = std::make_shared<bool>(false);
  rt::UnderlyingSource<rt::ByteChunk> throwingSource;
  throwingSource.start = [](rt::ReadableStreamDefaultController<rt::ByteChunk> source) { source.enqueue(rt::encodeUtf8("data: [DONE]\n\n")); };
  throwingSource.cancel = [cancelAttempted](rt::Reason) {
    *cancelAttempted = true;
    throw rt::JsError("cancel refused");
  };
  const rt::ByteStream throwingCancel(throwingSource);
  const rt::AsyncGenerator<json::object> iterator = lithos::iterateLithosChatCompletionStream(rt::Response(rt::BodyInit(throwingCancel)), MODEL);
  const rt::IteratorResult<json::object> first = co_await iterator.next();
  EXPECT_EQ(first.done, true);
  co_await iterator.return_();
  EXPECT_EQ(*cancelAttempted, true);
}
