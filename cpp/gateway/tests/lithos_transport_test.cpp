// Links: tests/lithos_transport_test.cpp src/provider/lithos.cpp src/provider/lithos_rate_limits.cpp src/sentinel/upstream_capture.cpp src/utils.cpp
// Port of tests/lithos-transport.test.ts (every case). A json::value cannot be cyclic, so the
// `circular.self = circular` body (JSON.stringify throws TypeError) is a body nested past
// rt::kJsonMaxDepth (JSON.stringify throws RangeError): the transport maps both alike.
#include "uos/provider/lithos.hpp"

#include "uos/api_key_policy.hpp"
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/sentinel/upstream_capture.hpp"

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
namespace upstream_capture = uos::sentinel::upstream_capture;

namespace {
  const std::string REQUESTED_MODEL = "moonshotai/Kimi-K3";
  const std::string API_KEY = "lith_sk_fixture";
  using Dispatch = api_key_policy::ApiKeyProviderDispatch;

  json::object object(std::string_view text) { return rt::jsonParse(text).as_object(); }

  /** Shaped like the real streaming frames probed on 2026-09-23. */
  json::object chunk(const json::object& delta, const json::object& extra = {}) {
    json::object choice;
    choice["index"] = 0;
    choice["delta"] = delta;
    choice["logprobs"] = nullptr;
    choice["finish_reason"] = nullptr;
    json::array choices;
    choices.push_back(std::move(choice));
    json::object base;
    base["id"] = "chatcmpl-lithos-stream";
    base["object"] = "chat.completion.chunk";
    base["created"] = 1790160303;
    base["model"] = REQUESTED_MODEL;
    base["choices"] = std::move(choices);
    return rt::objectSpread(base, extra);
  }

  /** Encodes one recorded frame the way the wire carried it. */
  std::string sseFrame(const json::value& frame) { return "data: " + (frame.is_string() ? std::string(frame.get_string()) : rt::jsonStringify(frame)) + "\n\n"; }

  /** Encodes recorded frames the way the wire carried them, comments included. */
  std::string sse(const std::vector<json::value>& frames, const std::string& prefix = "") {
    std::string out = prefix;
    for (const json::value& frame : frames) out += sseFrame(frame);
    return out;
  }

  /** Drives one recorded SSE body through the transport the route consumes. */
  rt::Task<std::vector<json::object>> readStream(std::string body) {
    rt::ResponseInit init;
    init.status = 200;
    init.headers = rt::Headers{{"Content-Type", "text/event-stream"}};
    const rt::Response response(rt::BodyInit(std::move(body)), init);
    std::vector<json::object> received;
    const rt::AsyncGenerator<json::object> iterator = lithos::iterateLithosChatCompletionStream(response, REQUESTED_MODEL);
    for (;;) {
      rt::IteratorResult<json::object> step = co_await iterator.next();
      if (step.done) break;
      received.push_back(std::move(*step.value));
    }
    co_return received;
  }

  /** A fetch that never settles until its cancellation signal aborts, then rejects with that reason. */
  rt::Task<rt::Response> rejectOnAbort(std::optional<rt::AbortSignal> signal) {
    const rt::Promise<rt::Response> pending = rt::newPromise<rt::Response>([&signal](const rt::Resolve<rt::Response>&, const rt::Reject& reject) {
      if (!signal) {
        reject(rt::makeReason(rt::JsError("Lithos transport dispatched without a cancellation signal")));
        return;
      }
      const std::weak_ptr<rt::detail::AbortSignalState> weakSignal = signal->state();
      const rt::EventListener rejectWithReason([reject, weakSignal] {
        if (const std::shared_ptr<rt::detail::AbortSignalState> state = weakSignal.lock()) {
          const rt::AbortSignal aborted(state);
          reject(rt::isError(aborted.reason()) ? aborted.reason() : rt::makeReason(rt::DOMException("The request was aborted.", "AbortError")));
        }
      });
      if (signal->aborted()) rejectWithReason();
      else signal->addEventListener("abort", rejectWithReason, {.once = true});
    });
    rt::Response response = co_await pending.wait();
    co_return response;
  }

  // `isLithosError(code, status)`: `error instanceof LithosError && error.code === code && error.status === status`.
  bool isLithosError(const rt::Reason& error, std::string_view code, double status) {
    if (!error) return false;
    try {
      std::rethrow_exception(error);
    } catch (const lithos::LithosError& lithosError) {
      return lithosError.code == code && lithosError.status == status;
    } catch (...) {
      return false;
    }
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

  json::object deeplyNestedBody() {
    json::value nested = json::object();
    for (std::size_t level = 0; level <= rt::kJsonMaxDepth; ++level) {
      json::object wrapper;
      wrapper["self"] = std::move(nested);
      nested = std::move(wrapper);
    }
    json::object body;
    body["model"] = REQUESTED_MODEL;
    body["self"] = std::move(nested);
    return body;
  }

  rt::Task<std::optional<Dispatch>> resolvedDispatch(std::optional<Dispatch> dispatch) { return rt::fulfilledTask(std::move(dispatch)); }
}

TEST("lithos transport: projects max_completion_tokens to max_tokens and keeps reasoning_effort verbatim") {
  const json::object body = object(R"({"model":"moonshotai/Kimi-K3","messages":[{"role":"user","content":"hi"}],"max_completion_tokens":512,"reasoning_effort":"xhigh","stream":true})");
  const json::object projected = lithos::projectLithosRequest(body, "MOONSHOTAI/KIMI-K3");
  EXPECT(rt::deepEqual(json::value(projected),
                       rt::jsonParse(R"({"model":"moonshotai/Kimi-K3","messages":[{"role":"user","content":"hi"}],"max_tokens":512,"reasoning_effort":"xhigh","stream":true})")));
  // The caller's own body is not mutated.
  EXPECT_EQ(*rt::getNumber(body, "max_completion_tokens"), 512.0);
  EXPECT_EQ(rt::hasOwn(body, "max_tokens"), false);

  // Every advertised tier reaches the wire unchanged, and so does a value the
  // provider will refuse: only the upstream may reject it.
  for (const std::string_view effort : lithos::LITHOS_REASONING_LEVELS) {
    json::object request;
    request["reasoning_effort"] = effort;
    EXPECT_EQ(*rt::getString(lithos::projectLithosRequest(request, REQUESTED_MODEL), "reasoning_effort"), effort);
  }
  // DeepSeek's `ultra`→`max` map must not run on this provider.
  EXPECT_EQ(*rt::getString(lithos::projectLithosRequest(object(R"({"reasoning_effort":"ultra"})"), REQUESTED_MODEL), "reasoning_effort"), std::string_view("ultra"));

  // A model this provider does not publish is a client error, not an upstream one.
  for (const std::string model : {"nope/nope", ""}) {
    rt::Reason failure;
    try {
      (void)lithos::projectLithosRequest(json::object(), model);
    } catch (...) {
      failure = std::current_exception();
    }
    EXPECT(isLithosError(failure, "lithos_request_invalid", 400));
  }
}

ASYNC_TEST("lithos transport: a buffered dispatch sends the projected body and reports its lifecycle in order") {
  std::vector<std::string> events;
  std::optional<rt::RequestInit> dispatchedInit;
  std::optional<json::value> dispatchedBody;
  bool urlMatched = false;
  const lithos::LithosFetch fetcher([&](rt::RequestInfo input, rt::RequestInit init) -> rt::Response {
    events.push_back("fetch");
    const std::string* url = std::get_if<std::string>(&input);
    urlMatched = url && *url == lithos::LITHOS_CHAT_COMPLETIONS_URL;
    const std::string* text = init.body ? std::get_if<std::string>(&*init.body) : nullptr;
    if (!text) throw rt::JsError("lithos fixture expected a serialized request body");
    dispatchedBody = rt::jsonParse(*text);
    dispatchedInit = init;
    rt::ResponseInit responseInit;
    responseInit.status = 200;
    responseInit.headers = rt::Headers{{"Content-Type", "application/json"}, {"x-request-id", "lithos-must-not-be-read"}};
    return rt::Response::json(
      rt::jsonParse(R"({"id":"chatcmpl-lithos-1","object":"chat.completion","created":1790160326,"model":"moonshotai/Kimi-K3",)"
                    R"("choices":[{"index":0,"message":{"role":"assistant","content":"OK"},"finish_reason":"stop"}]})"),
      responseInit);
  });
  Dispatch dispatch;
  dispatch.markTransportStarted = [&events] { events.push_back("markTransportStarted"); };
  dispatch.cancelBeforeTransport = [&events] {
    events.push_back("cancelBeforeTransport");
    return rt::fulfilledTask();
  };
  const upstream_capture::SentinelUpstreamRecorder recorder = upstream_capture::createSentinelUpstreamRecorder();
  lithos::LithosChatCompletionsOptions options;
  options.apiKey = API_KEY;
  options.fetcher = fetcher;
  options.beforeDispatch = [&events, dispatch] {
    events.push_back("beforeDispatch");
    return resolvedDispatch(dispatch);
  };
  options.onDispatch = [&events] { events.push_back("onDispatch"); };
  options.onHeaders = [&events] { events.push_back("onHeaders"); };
  options.sentinelUpstreamRecorder = recorder;
  const json::object body = object(R"({"model":"moonshotai/Kimi-K3","messages":[{"role":"user","content":"hi"}],"max_completion_tokens":64})");
  auto fetching = lithos::fetchLithosChatCompletions(body, REQUESTED_MODEL, options);
  const rt::Response response = co_await std::move(fetching);

  EXPECT_EQ(response.status(), 200);
  EXPECT(urlMatched);
  // Key admission is the final awaited step before transport, and the
  // recorder wrapper is what the caller receives.
  const std::vector<std::string> expectedEvents{"beforeDispatch", "markTransportStarted", "onDispatch", "fetch", "onHeaders"};
  EXPECT_EQ(events, expectedEvents);
  ASSERT(dispatchedBody.has_value());
  EXPECT(rt::deepEqual(*dispatchedBody, rt::jsonParse(R"({"model":"moonshotai/Kimi-K3","messages":[{"role":"user","content":"hi"}],"max_tokens":64})")));
  ASSERT(dispatchedInit.has_value());
  const rt::Headers headers(dispatchedInit->headers.value_or(rt::Headers()));
  EXPECT_EQ(headers.get("Authorization"), std::optional<std::string>("Bearer " + API_KEY));
  EXPECT_EQ(headers.get("Content-Type"), std::optional<std::string>("application/json"));
  EXPECT_EQ(headers.get("Accept"), std::optional<std::string>("application/json"));
  EXPECT_EQ(dispatchedInit->method, std::optional<std::string>("POST"));
  EXPECT_EQ(dispatchedInit->redirect, std::optional<std::string>("manual"));
  // the transport must own a header deadline signal
  EXPECT(dispatchedInit->signal.has_value());
  auto reading = response.json();
  const json::value parsed = co_await std::move(reading);
  EXPECT_EQ(rt::getString(parsed, "id"), std::optional<std::string_view>("chatcmpl-lithos-1"));
  // No provider request-id header exists on this wire, so none is read even
  // when a middlebox injects one.
  EXPECT_EQ(lithos::getLithosProviderRequestId(response), std::optional<std::string>());

  const upstream_capture::SentinelUpstreamTrace trace = recorder.snapshotAndSeal();
  ASSERT(!trace.attempts.empty());
  const upstream_capture::SentinelUpstreamAttempt& attempt = trace.attempts[0];
  EXPECT_EQ(attempt.provider, std::string("lithos"));
  EXPECT_EQ(attempt.status, std::optional<std::int64_t>(200));
  EXPECT_EQ(attempt.terminal, std::string("eof"));
  recorder.dispose();

  // A body that cannot be serialized fails closed before any key or socket work.
  lithos::LithosChatCompletionsOptions failing;
  failing.apiKey = API_KEY;
  failing.fetcher = lithos::LithosFetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response {
    throw rt::JsError("a non-serializable body must never reach the transport");
  });
  auto unserializable = lithos::fetchLithosChatCompletions(deeplyNestedBody(), REQUESTED_MODEL, failing);
  auto settled = rejectionOf(std::move(unserializable));
  const rt::Reason failure = co_await std::move(settled);
  EXPECT(isLithosError(failure, "lithos_request_invalid", 400));
}

ASYNC_TEST("lithos transport: classifies unreachable, deadline, caller abort and quota failures without rewriting them") {
  const upstream_capture::SentinelUpstreamRecorder failedRecorder = upstream_capture::createSentinelUpstreamRecorder();
  lithos::LithosChatCompletionsOptions unreachable;
  unreachable.apiKey = API_KEY;
  unreachable.fetcher = lithos::LithosFetch(
    [](rt::RequestInfo, rt::RequestInit) { return rt::Promise<rt::Response>::reject(rt::makeReason(rt::TypeError("lithos socket closed"))).wait(); });
  unreachable.sentinelUpstreamRecorder = failedRecorder;
  const json::object body = object(R"({"model":"moonshotai/Kimi-K3"})");
  auto unreachableTask = lithos::fetchLithosChatCompletions(body, REQUESTED_MODEL, unreachable);
  auto unreachableSettled = rejectionOf(std::move(unreachableTask));
  const rt::Reason unreachableFailure = co_await std::move(unreachableSettled);
  EXPECT(isLithosError(unreachableFailure, "lithos_upstream_unreachable", 502));
  const upstream_capture::SentinelUpstreamTrace failedTrace = failedRecorder.snapshotAndSeal();
  ASSERT(!failedTrace.attempts.empty());
  EXPECT_EQ(failedTrace.attempts[0].terminal, std::string("fetch_error"));
  failedRecorder.dispose();

  {
    lithos::setLithosFetchTimeoutMsForTest(10);
    const auto restore = rt::scopeExit([] { lithos::setLithosFetchTimeoutMsForTest(std::nullopt); });
    lithos::LithosChatCompletionsOptions deadline;
    deadline.apiKey = API_KEY;
    deadline.fetcher = lithos::LithosFetch([](rt::RequestInfo, rt::RequestInit init) { return rejectOnAbort(init.signal); });
    auto deadlineTask = lithos::fetchLithosChatCompletions(body, REQUESTED_MODEL, deadline);
    auto deadlineSettled = rejectionOf(std::move(deadlineTask));
    const rt::Reason deadlineFailure = co_await std::move(deadlineSettled);
    EXPECT(isLithosError(deadlineFailure, "gateway_timeout", 504));
  }

  // A caller abort keeps the caller's own reason rather than becoming a
  // provider fault, during transport and before it.
  const rt::Reason abortReason = rt::makeReason(rt::DOMException("client disconnected", "AbortError"));
  const rt::AbortController controller;
  lithos::LithosChatCompletionsOptions callerAbort;
  callerAbort.apiKey = API_KEY;
  callerAbort.signal = controller.signal();
  callerAbort.fetcher = lithos::LithosFetch([controller, abortReason](rt::RequestInfo, rt::RequestInit init) {
    controller.abort(abortReason);
    return rejectOnAbort(init.signal);
  });
  auto callerAbortTask = lithos::fetchLithosChatCompletions(body, REQUESTED_MODEL, callerAbort);
  EXPECT_REJECTS_REASON(std::move(callerAbortTask), abortReason);

  int cancelled = 0;
  int fetchCalls = 0;
  const rt::AbortController preAborted;
  preAborted.abort(abortReason);
  lithos::LithosChatCompletionsOptions preAbortedOptions;
  preAbortedOptions.apiKey = API_KEY;
  preAbortedOptions.signal = preAborted.signal();
  preAbortedOptions.beforeDispatch = [&cancelled] {
    Dispatch dispatch;
    dispatch.markTransportStarted = [] { throw rt::JsError("a cancelled request must not start transport"); };
    dispatch.cancelBeforeTransport = [&cancelled] {
      cancelled += 1;
      return rt::fulfilledTask();
    };
    return resolvedDispatch(std::move(dispatch));
  };
  preAbortedOptions.fetcher = lithos::LithosFetch([&fetchCalls](rt::RequestInfo, rt::RequestInit) {
    fetchCalls += 1;
    return rt::Response::json(json::value(json::object()));
  });
  auto preAbortedTask = lithos::fetchLithosChatCompletions(body, REQUESTED_MODEL, preAbortedOptions);
  EXPECT_REJECTS_REASON(std::move(preAbortedTask), abortReason);
  EXPECT_EQ(cancelled, 1);
  EXPECT_EQ(fetchCalls, 0);

  // A quota admission failure is the caller's to render, never reclassified.
  const rt::Reason quota = rt::makeReason(api_key_policy::ApiKeyQuotaDispatchError("quota reservation is gone"));
  lithos::LithosChatCompletionsOptions quotaOptions;
  quotaOptions.apiKey = API_KEY;
  quotaOptions.beforeDispatch = [quota]() -> rt::Task<std::optional<Dispatch>> { rt::rethrow(quota); };
  quotaOptions.fetcher = lithos::LithosFetch([](rt::RequestInfo, rt::RequestInit) -> rt::Response {
    throw rt::JsError("a refused admission must never reach the transport");
  });
  auto quotaTask = lithos::fetchLithosChatCompletions(body, REQUESTED_MODEL, quotaOptions);
  EXPECT_REJECTS_REASON(std::move(quotaTask), quota);
}

ASYNC_TEST("lithos transport: relays reason-only deltas, a tool-call delta and the empty-choices totals frame until [DONE]") {
  json::object toolCall = object(R"({"reasoning_content":null,"tool_calls":[{"index":0,"id":"get_weather:0","type":"function","function":{"name":"get_weather","arguments":"{\"city\":"}}]})");
  std::vector<json::value> frames;
  frames.emplace_back(chunk(object(R"({"role":"assistant","content":""})")));
  frames.emplace_back(chunk(object(R"({"reasoning_content":"Let me think"})")));
  frames.emplace_back(chunk(object(R"({"content":"Paris"})")));
  frames.emplace_back(chunk(toolCall));
  frames.emplace_back(chunk(object(R"({"tool_calls":[{"index":0,"function":{"arguments":"\"Paris\"}"}}]})")));
  frames.emplace_back(chunk(json::object(), object(R"({"created":1790160304,"choices":[],"usage":{"prompt_tokens":91,"total_tokens":204,"completion_tokens":113,)"
                                                   R"("prompt_tokens_details":null,"completion_tokens_details":{"reasoning_tokens":99}}})")));
  // Keep-alive comments and non-`data:` fields carry no payload and are skipped.
  auto reading = readStream(sse(frames, ": keep-alive\n\nevent: message\n\n") + "data: [DONE]\n\n");
  const std::vector<json::object> received = co_await std::move(reading);

  ASSERT_EQ(received.size(), frames.size());
  EXPECT(rt::deepEqual(*rt::get(received[0], "choices"), rt::jsonParse(R"([{"index":0,"delta":{"role":"assistant","content":""},"finish_reason":null}])")));
  // A reason-only delta is a real frame, not an empty one: it must survive.
  EXPECT(rt::deepEqual(*rt::get(received[1], "choices"), rt::jsonParse(R"([{"index":0,"delta":{"reasoning_content":"Let me think"},"finish_reason":null}])")));
  EXPECT(rt::deepEqual(*rt::get(received[3], "choices"),
                       rt::jsonParse(R"([{"index":0,"delta":{"tool_calls":[{"index":0,"id":"get_weather:0","type":"function",)"
                                     R"("function":{"name":"get_weather","arguments":"{\"city\":"}}]},"finish_reason":null}])")));
  EXPECT(rt::deepEqual(*rt::get(received[4], "choices"),
                       rt::jsonParse(R"([{"index":0,"delta":{"tool_calls":[{"index":0,"function":{"arguments":"\"Paris\"}"}}]},"finish_reason":null}])")));
  // The authoritative totals ride a chunk with an EMPTY choices array, and
  // usage is relayed without any `stream_options.include_usage` gate.
  EXPECT(rt::deepEqual(*rt::get(received[5], "choices"), rt::jsonParse("[]")));
  EXPECT(rt::deepEqual(*rt::get(received[5], "usage"),
                       rt::jsonParse(R"({"prompt_tokens":91,"completion_tokens":113,"total_tokens":204,"completion_tokens_details":{"reasoning_tokens":99}})")));

  // [DONE] terminates: a frame after it is never read.
  std::vector<json::value> paris;
  paris.emplace_back(chunk(object(R"({"content":"Paris"})")));
  std::vector<json::value> never;
  never.emplace_back(chunk(object(R"({"content":"never relayed"})")));
  auto terminatedReading = readStream(sse(paris) + "data: [DONE]\n\n" + sse(never));
  const std::vector<json::object> terminated = co_await std::move(terminatedReading);
  EXPECT_EQ(terminated.size(), std::size_t{1});
}

ASYNC_TEST("lithos transport: fails closed on a malformed, truncated or oversized stream") {
  // Malformed JSON is never skipped: dropping a frame would truncate the answer.
  auto malformed = rejectionOf(readStream("data: {\"id\":\n\n"));
  const rt::Reason malformedFailure = co_await std::move(malformed);
  EXPECT(isLithosError(malformedFailure, "lithos_upstream_invalid_response", 502));
  // A frame that is JSON but not a valid chunk is refused the same way.
  std::vector<json::value> invalidFrames;
  invalidFrames.emplace_back(rt::jsonParse(R"({"id":"chatcmpl-lithos-bad","object":"chat.completion.chunk","created":1,"model":"moonshotai/Kimi-K3","choices":[{"index":0,"delta":{"content":5}}]})"));
  auto invalid = rejectionOf(readStream(sse(invalidFrames)));
  const rt::Reason invalidFailure = co_await std::move(invalid);
  EXPECT(isLithosError(invalidFailure, "lithos_upstream_invalid_response", 502));
  // A stream that ends before [DONE] is truncated, not complete.
  std::vector<json::value> paris;
  paris.emplace_back(chunk(object(R"({"content":"Paris"})")));
  auto truncated = rejectionOf(readStream(sse(paris)));
  const rt::Reason truncatedFailure = co_await std::move(truncated);
  EXPECT(isLithosError(truncatedFailure, "lithos_upstream_invalid_response", 502));
  // One frame may not exceed the retained-tail bound.
  auto oversized = rejectionOf(readStream("data: " + std::string(4 * 1024 * 1024 + 1, 'x') + "\n\n"));
  const rt::Reason oversizedFailure = co_await std::move(oversized);
  EXPECT(isLithosError(oversizedFailure, "lithos_upstream_invalid_response", 502));

  const rt::AbortController controller;
  // A body that never produces a byte keeps the read pending, so the caller's
  // abort is the only thing that can settle it.
  rt::UnderlyingSource<rt::ByteChunk> stalledSource;
  stalledSource.start = [](rt::ReadableStreamDefaultController<rt::ByteChunk>) {};
  const rt::ByteStream stalledStream(stalledSource);
  rt::ResponseInit init;
  init.status = 200;
  init.headers = rt::Headers{{"Content-Type", "text/event-stream"}};
  const rt::Response stalled(rt::BodyInit(stalledStream), init);
  lithos::IterateLithosChatCompletionStreamOptions streamOptions;
  streamOptions.signal = controller.signal();
  const rt::AsyncGenerator<json::object> frames = lithos::iterateLithosChatCompletionStream(stalled, REQUESTED_MODEL, streamOptions);
  const auto onFrame = [](json::object frame) -> rt::Task<void> {
    throw rt::JsError("a stalled stream must not yield a frame: " + rt::jsonStringify(json::value(std::move(frame))));
    co_return;
  };
  const rt::Promise<void> reading(rt::forAwait(frames, onFrame));
  controller.abort(rt::makeReason(rt::DOMException("client disconnected", "AbortError")));
  auto settled = rejectionOf(reading.wait());
  const rt::Reason failure = co_await std::move(settled);
  EXPECT(rt::isError(failure) && rt::errorName(failure) == std::optional<std::string>("AbortError"));
}

TEST("lithos transport: the provider correlation seam reads no header and the detail counters keep their guards") {
  rt::ResponseInit init;
  init.status = 200;
  init.headers = rt::Headers{{"x-request-id", "lithos-1"}, {"x-lithos-request-id", "lithos-2"}};
  const rt::Response response(rt::BodyInit(std::string("{}")), init);
  EXPECT_EQ(lithos::getLithosProviderRequestId(response), std::optional<std::string>());
  EXPECT_EQ(lithos::normalizeLithosProviderRequestId(json::value("lithos-correlation-1")), std::optional<std::string>("lithos-correlation-1"));
  EXPECT_EQ(lithos::normalizeLithosProviderRequestId(json::value(" lithos-1")), std::optional<std::string>());
  EXPECT_EQ(lithos::normalizeLithosProviderRequestId(json::value(rt::repeat("a", 257))), std::optional<std::string>());
  EXPECT_EQ(lithos::normalizeLithosProviderRequestId(json::value(7)), std::optional<std::string>());

  EXPECT_EQ(lithos::lithosCachedPromptTokens(object(R"({"prompt_tokens_details":{"cached_tokens":64}})"), 91), std::optional<double>(64));
  EXPECT_EQ(lithos::lithosCachedPromptTokens(object(R"({"prompt_tokens_details":null})"), 91), std::optional<double>());
  EXPECT_EQ(lithos::lithosCachedPromptTokens(object(R"({"prompt_tokens_details":{"cached_tokens":200}})"), 91), std::optional<double>());
  EXPECT_EQ(lithos::lithosReasoningTokens(object(R"({"completion_tokens_details":{"reasoning_tokens":99}})"), 113), std::optional<double>(99));
  EXPECT_EQ(lithos::lithosReasoningTokens(object(R"({"completion_tokens_details":{"reasoning_tokens":-1}})"), 113), std::optional<double>());
  EXPECT_EQ(lithos::lithosReasoningTokens(json::object(), 113), std::optional<double>());
}
