// Port of tests/responses-stream.test.ts.
// Links: src/responses_stream.cpp src/utils.cpp src/inference_deadline.cpp
#include "uos/responses_stream.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/json.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/string.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "helpers/live_allocations.hpp"

#include <cmath>
#include <cstddef>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using uos::responses_stream::MAX_RESPONSES_SSE_EVENT_BYTES;
using uos::responses_stream::preflightResponsesStream;
using uos::responses_stream::PreflightedResponsesStream;
using uos::responses_stream::ProxyResponsesStreamOptions;
using uos::responses_stream::proxyResponsesStream;
using uos::responses_stream::ReadResponsesStreamOptions;
using uos::responses_stream::readResponsesStream;
using uos::responses_stream::ResponsesStreamError;
using uos::responses_stream::responsesStreamErrorOf;
using uos::responses_stream::ResponsesStreamEvent;
using uos::responses_stream::ResponsesStreamIterator;
using uos::responses_stream::WithSseKeepaliveOptions;
using uos::responses_stream::withSseKeepalive;

namespace {
  using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;
  using ReadResult = rt::ReadableStreamReadResult<rt::ByteChunk>;

  rt::ByteChunk bytes(std::string_view value) { return rt::encodeUtf8(value); }
  std::string decode(const rt::ByteChunk& value) { return rt::decodeUtf8(value); }
  bool matches(std::string_view pattern, std::string_view subject) { return rt::Regex(pattern).test(subject); }

  // `Array.fromAsync(iterator)` / `for await (const event of iterator) events.push(event)`.
  rt::Task<std::vector<ResponsesStreamEvent>> fromAsync(ResponsesStreamIterator iterator) {
    std::vector<ResponsesStreamEvent> events;
    for (;;) {
      rt::IteratorResult<ResponsesStreamEvent> step = co_await iterator.next();
      if (step.done) break;
      events.push_back(std::move(*step.value));
    }
    co_return events;
  }

  // captureError(run): the rejection, failing the test when there is none.
  template <class T> rt::Task<rt::Reason> captureError(rt::Task<T> run) {
    rt::Reason caught;
    try {
      co_await std::move(run);
    } catch (...) {
      caught = std::current_exception();
    }
    if (!caught) rt::test::report(__FILE__, __LINE__, "Expected operation to reject", true);
    co_return caught;
  }

  // captureError(async () => { await Array.fromAsync(readResponsesStream(source, undefined, options)); })
  rt::Task<rt::Reason> drainError(rt::ByteStream source, ReadResponsesStreamOptions options) {
    const ResponsesStreamIterator iterator = readResponsesStream(source, std::nullopt, std::move(options));
    rt::Task<std::vector<ResponsesStreamEvent>> drained = fromAsync(iterator);
    rt::Reason caught = co_await captureError(std::move(drained));
    co_return caught;
  }
  rt::Task<rt::Reason> drainError(rt::ByteStream source) {
    ReadResponsesStreamOptions options;
    return drainError(std::move(source), std::move(options));
  }

  rt::Task<std::vector<ResponsesStreamEvent>> drain(rt::ByteStream source) {
    const ResponsesStreamIterator iterator = readResponsesStream(source);
    std::vector<ResponsesStreamEvent> events = co_await fromAsync(iterator);
    co_return events;
  }

  std::vector<std::string> typesOf(const std::vector<ResponsesStreamEvent>& events) {
    std::vector<std::string> types;
    for (const ResponsesStreamEvent& event : events) types.push_back(event.type);
    return types;
  }

  // chunked(value, boundaries, cancel)
  rt::ByteStream chunked(std::string value, std::vector<double> boundaries = {}, std::function<void()> cancel = nullptr) {
    auto offset = std::make_shared<double>(0);
    auto remaining = std::make_shared<std::vector<double>>(std::move(boundaries));
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [value, offset, remaining](Controller controller) {
      const double length = static_cast<double>(rt::length(value));
      double end = length;
      if (!remaining->empty()) {
        end = remaining->front();
        remaining->erase(remaining->begin());
      }
      if (*offset >= length) {
        controller.close();
        return;
      }
      controller.enqueue(bytes(rt::slice(value, *offset, end)));
      *offset = end;
      if (*offset >= length) controller.close();
    };
    if (cancel) source.cancel = [cancel](rt::Reason) { cancel(); };
    return rt::ByteStream(std::move(source));
  }

  // new Promise<never>((_, reject) => setTimeout(() => reject(new Error(message)), ms))
  template <class T> rt::Promise<T> rejectAfter(double ms, std::string message) {
    return rt::newPromise<T>([ms, message](const rt::Resolve<T>&, const rt::Reject& reject) {
      rt::setTimeout([reject, message] { reject(rt::makeReason(rt::JsError(message))); }, ms);
    });
  }
}

ASYNC_TEST("Responses inactivity waits release consumed chunks on the running loop") {
  const auto readEvents = []() -> rt::Task<void> {
    const auto timers = rt::loop().timerCount();
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [](Controller controller) {
      controller.enqueue(bytes("data: {\"type\":\"response.output_text.delta\",\"delta\":\"a\"}\n\n"));
      const std::string comment = ":" + std::string(64 * 1024, 'x') + "\n\n";
      controller.enqueue(bytes(comment));
      controller.enqueue(bytes("data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n"));
      controller.close();
    };
    const rt::ByteStream body(std::move(source));
    ReadResponsesStreamOptions options;
    options.firstEventTimeoutMs = 10;
    options.inactivityTimeoutMs = 10;
    const auto iterator = readResponsesStream(body, std::nullopt, options);
    const auto events = co_await fromAsync(iterator);
    ASSERT_EQ(events.size(), 2u);
    EXPECT(events.back().terminal);
    EXPECT_EQ(rt::loop().timerCount(), timers);
  };
  auto checking = uos::tests::helpers::expectStableLiveAllocations(readEvents, 32, 20);
  co_await std::move(checking);
}

ASYNC_TEST("Responses SSE parser handles mixed separators, multiline data, and fragmented UTF-8") {
  const std::string payload = std::string(": keepalive\r\ndata: {\"type\":\"response.output_text.delta\",\r\ndata: \"delta\":\"hi \xF0\x9F\x8C\x8D\"}\r\n\r\n") +
                              "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}";
  const rt::ByteChunk encoded = bytes(payload);
  for (std::size_t boundary = 1; boundary < encoded.size(); boundary += 1) {
    rt::ByteChunk head(encoded.begin(), encoded.begin() + static_cast<std::ptrdiff_t>(boundary));
    rt::ByteChunk tail(encoded.begin() + static_cast<std::ptrdiff_t>(boundary), encoded.end());
    rt::UnderlyingSource<rt::ByteChunk> init;
    init.start = [head, tail](Controller controller) {
      controller.enqueue(head);
      controller.enqueue(tail);
      controller.close();
    };
    const rt::ByteStream source(std::move(init));
    const std::vector<ResponsesStreamEvent> events = co_await drain(source);
    EXPECT_EQ(typesOf(events), (std::vector<std::string>{"response.output_text.delta", "response.completed"}));
  }
}

ASYNC_TEST("Responses SSE parser accepts an LF then CRLF event boundary") {
  const rt::ByteStream source = chunked(std::string("data: {\"type\":\"response.output_text.delta\",\"delta\":\"x\"}\n\r\n") +
                                        "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\r\n\r\n");
  const std::vector<ResponsesStreamEvent> events = co_await drain(source);
  EXPECT_EQ(typesOf(events), (std::vector<std::string>{"response.output_text.delta", "response.completed"}));
}

ASYNC_TEST("Responses SSE parser rejects malformed JSON and EOF before terminal") {
  const rt::ByteStream malformedSource = chunked("data: nope\n\n");
  const rt::Reason malformed = co_await drainError(malformedSource);
  const ResponsesStreamError* malformedError = responsesStreamErrorOf(malformed);
  ASSERT(malformedError);
  EXPECT_EQ(malformedError->kind, std::string("malformed_event"));
  const rt::ByteStream eofSource = chunked("data: {\"type\":\"response.output_text.delta\",\"delta\":\"x\"}\n\n");
  const rt::Reason eof = co_await drainError(eofSource);
  const ResponsesStreamError* eofError = responsesStreamErrorOf(eof);
  ASSERT(eofError);
  EXPECT(matches("before a terminal event", eofError->message));
  EXPECT_EQ(eofError->kind, std::string("premature_eof"));
}

ASYNC_TEST("Responses parser rejects terminal events without their protocol payload") {
  const rt::ByteStream errorSource = chunked("data: {\"type\":\"response.completed\"}\n\n");
  const rt::Reason error = co_await drainError(errorSource);
  const ResponsesStreamError* streamError = responsesStreamErrorOf(error);
  ASSERT(streamError);
  EXPECT_EQ(streamError->kind, std::string("malformed_event"));
}

ASYNC_TEST("Responses parser rejects array-valued terminal response payloads") {
  for (const std::string type : {"response.completed", "response.failed", "response.incomplete"}) {
    json::object value;
    value["type"] = type;
    value["response"] = json::array();
    const rt::ByteStream errorSource = chunked("data: " + rt::jsonStringify(json::value(value)) + "\n\n");
    const rt::Reason error = co_await drainError(errorSource);
    const ResponsesStreamError* streamError = responsesStreamErrorOf(error);
    ASSERT(streamError);
    EXPECT_EQ(streamError->kind, std::string("malformed_event"));
  }
}

ASYNC_TEST("Responses parser rejects an array-valued nested error payload") {
  const rt::ByteStream errorSource = chunked("data: {\"type\":\"error\",\"error\":[]}\n\n");
  const rt::Reason error = co_await drainError(errorSource);
  const ResponsesStreamError* streamError = responsesStreamErrorOf(error);
  ASSERT(streamError);
  EXPECT_EQ(streamError->kind, std::string("malformed_event"));
}

ASYNC_TEST("Responses parser rejects an array-valued nested error despite valid flat fields") {
  const rt::ByteStream errorSource = chunked("data: {\"type\":\"error\",\"error\":[],\"code\":\"provider_error\",\"message\":\"Provider stopped.\",\"param\":null}\n\n");
  const rt::Reason error = co_await drainError(errorSource);
  const ResponsesStreamError* streamError = responsesStreamErrorOf(error);
  ASSERT(streamError);
  EXPECT_EQ(streamError->kind, std::string("malformed_event"));
}

ASYNC_TEST("Responses parser accepts an official flat error terminal") {
  const rt::ByteStream eventsSource = chunked("data: {\"type\":\"error\",\"code\":\"provider_error\",\"message\":\"Provider stopped.\",\"param\":null}\n\n");
  const std::vector<ResponsesStreamEvent> events = co_await drain(eventsSource);
  ASSERT_EQ(events.size(), std::size_t{1});
  EXPECT_EQ(events[0].type, std::string("error"));
  EXPECT_EQ(rt::getString(events[0].value, "code"), std::optional<std::string_view>("provider_error"));
}

ASYNC_TEST("Responses parser accepts a nullable code in an official flat error terminal") {
  const rt::ByteStream eventsSource = chunked("data: {\"type\":\"error\",\"code\":null,\"message\":\"Provider stopped.\",\"param\":\"input\"}\n\n");
  const std::vector<ResponsesStreamEvent> events = co_await drain(eventsSource);
  ASSERT_EQ(events.size(), std::size_t{1});
  const json::value* code = rt::get(events[0].value, "code");
  EXPECT(code && code->is_null());
  EXPECT_EQ(rt::getString(events[0].value, "param"), std::optional<std::string_view>("input"));
}

ASYNC_TEST("Responses preflight surfaces immediate failures before a stream response is created") {
  rt::Task<PreflightedResponsesStream> malformedRun = preflightResponsesStream(chunked("data: nope\n\n"));
  const rt::Reason malformed = co_await captureError(std::move(malformedRun));
  const ResponsesStreamError* malformedError = responsesStreamErrorOf(malformed);
  ASSERT(malformedError);
  EXPECT_EQ(malformedError->kind, std::string("malformed_event"));

  rt::Task<PreflightedResponsesStream> eofRun = preflightResponsesStream(chunked(""));
  const rt::Reason eof = co_await captureError(std::move(eofRun));
  const ResponsesStreamError* eofError = responsesStreamErrorOf(eof);
  ASSERT(eofError);
  EXPECT_EQ(eofError->kind, std::string("premature_eof"));
}

ASYNC_TEST("Responses activity observes comments and partial frames before parsed events") {
  rt::UnderlyingSource<rt::ByteChunk> init;
  init.start = [](Controller controller) {
    controller.enqueue(bytes(": keepalive\n\n"));
    controller.enqueue(bytes("data: {\"type\":\"response.output_text.delta\","));
    controller.enqueue(bytes("\"delta\":\"x\"}\n\n"));
    controller.enqueue(bytes("data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n"));
    controller.close();
  };
  const rt::ByteStream source(std::move(init));
  auto activityCount = std::make_shared<int>(0);
  ReadResponsesStreamOptions options;
  options.onActivity = [activityCount] { *activityCount += 1; };
  const ResponsesStreamIterator iterator = readResponsesStream(source, std::nullopt, std::move(options));
  const std::vector<ResponsesStreamEvent> events = co_await fromAsync(iterator);

  EXPECT_EQ(*activityCount, 4);
  EXPECT_EQ(typesOf(events), (std::vector<std::string>{"response.output_text.delta", "response.completed"}));
}

ASYNC_TEST("Responses parser wraps reader exceptions and releases its lock") {
  rt::UnderlyingSource<rt::ByteChunk> init;
  init.pull = [](Controller controller) { controller.error(rt::makeReason(rt::JsError("socket reset"))); };
  const rt::ByteStream source(std::move(init));
  const rt::Reason error = co_await drainError(source);
  const ResponsesStreamError* streamError = responsesStreamErrorOf(error);
  ASSERT(streamError);
  EXPECT_EQ(streamError->kind, std::string("read_error"));
  EXPECT_EQ(source.locked(), false);
}

ASYNC_TEST("Responses parser keeps one absolute first-event deadline across non-event frames") {
  auto cancelled = std::make_shared<int>(0);
  auto active = std::make_shared<bool>(true);
  rt::UnderlyingSource<rt::ByteChunk> init;
  init.pull = [active](Controller controller) {
    rt::setTimeout(
      [active, controller] {
        if (*active) controller.enqueue(bytes(": keepalive\n\n"));
      },
      2);
  };
  init.cancel = [active, cancelled](rt::Reason) {
    *active = false;
    *cancelled += 1;
  };
  const rt::ByteStream source(std::move(init));
  // No data event is ever emitted.
  ReadResponsesStreamOptions options;
  options.firstEventTimeoutMs = 12;
  const rt::Reason error = co_await drainError(source, std::move(options));
  const ResponsesStreamError* streamError = responsesStreamErrorOf(error);
  ASSERT(streamError);
  EXPECT_EQ(streamError->kind, std::string("inactivity_timeout"));
  EXPECT_EQ(*cancelled, 1);
}

ASYNC_TEST("Responses parser rejects one fragmented oversized SSE event and cancels once") {
  auto cancelled = std::make_shared<int>(0);
  const std::string oversized = "data: " + std::string(static_cast<std::size_t>(MAX_RESPONSES_SSE_EVENT_BYTES) + 1, 'x');
  rt::UnderlyingSource<rt::ByteChunk> init;
  init.start = [oversized](Controller controller) {
    const rt::ByteChunk value = bytes(oversized);
    const std::size_t middle = static_cast<std::size_t>(std::floor(static_cast<double>(value.size()) / 2));
    controller.enqueue(rt::ByteChunk(value.begin(), value.begin() + static_cast<std::ptrdiff_t>(middle)));
    controller.enqueue(rt::ByteChunk(value.begin() + static_cast<std::ptrdiff_t>(middle), value.end()));
  };
  init.cancel = [cancelled](rt::Reason) { *cancelled += 1; };
  const rt::ByteStream source(std::move(init));
  const rt::Reason error = co_await drainError(source);
  const ResponsesStreamError* streamError = responsesStreamErrorOf(error);
  ASSERT(streamError);
  EXPECT_EQ(streamError->kind, std::string("event_too_large"));
  EXPECT_EQ(*cancelled, 1);
}

ASYNC_TEST("Responses proxy forwards only the first terminal and cancels a hanging upstream once") {
  auto cancelCount = std::make_shared<int>(0);
  rt::UnderlyingSource<rt::ByteChunk> init;
  init.start = [](Controller controller) {
    controller.enqueue(bytes(std::string("data: {\"type\":\"response.incomplete\",\"response\":{\"status\":\"incomplete\"}}\n\n") +
                             "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n" +
                             "data: {\"type\":\"response.output_text.delta\",\"delta\":\"post-terminal\"}\n\n"));
  };
  init.cancel = [cancelCount](rt::Reason) { *cancelCount += 1; };
  const rt::ByteStream source(std::move(init));
  const rt::ByteStream proxied = proxyResponsesStream(source);
  const rt::Response response(rt::BodyInit{proxied});
  const std::string output = co_await response.text();
  EXPECT(matches("response.incomplete", output));
  EXPECT(!matches("response.completed", output));
  EXPECT(!matches("post-terminal", output));
  EXPECT_EQ(*cancelCount, 1);
  // `await new Promise((resolve) => setImmediate(resolve))`: one macrotask turn.
  for (int turn = 0; source.locked() && turn < 10; turn += 1) co_await rt::sleep(0);
  EXPECT_EQ(source.locked(), false);
}

ASYNC_TEST("Responses proxy keeps first-event delivery, byte framing, and terminal cancellation incremental") {
  const std::string firstFrame = "data: {\"type\":\"response.output_text.delta\",\"delta\":\"first\"}\n\n";
  const std::string terminalFrame = "data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n";
  const rt::Resolvers<void> terminalGate = rt::withResolvers<void>();
  struct Flags {
    bool firstSent = false;
    bool terminalScheduled = false;
    int upstreamCancelCount = 0;
  };
  auto flags = std::make_shared<Flags>();
  const rt::Resolvers<void> cancelled = rt::withResolvers<void>();
  rt::UnderlyingSource<rt::ByteChunk> init;
  init.pull = [flags, firstFrame, terminalFrame, gate = terminalGate.promise](Controller controller) -> rt::Task<void> {
    if (!flags->firstSent) {
      flags->firstSent = true;
      controller.enqueue(bytes(firstFrame));
      co_return;
    }
    if (flags->terminalScheduled) co_return;
    flags->terminalScheduled = true;
    co_await gate.wait();
    controller.enqueue(bytes(terminalFrame));
  };
  init.cancel = [flags, resolveCancelled = cancelled.resolve](rt::Reason) {
    flags->upstreamCancelCount += 1;
    resolveCancelled();
  };
  const rt::ByteStream source(std::move(init));

  const rt::ByteStream proxied = proxyResponsesStream(source);
  const auto reader = proxied.getReader();
  const rt::Promise<ReadResult> firstRead(reader.read());
  const rt::Promise<ReadResult> firstDelayed = rejectAfter<ReadResult>(100, "first SSE event was delayed");
  std::vector<rt::Promise<ReadResult>> firstRacers{firstRead, firstDelayed};
  const rt::Promise<ReadResult> firstRace = rt::race(std::move(firstRacers));
  const ReadResult first = co_await firstRace.wait();
  if (first.done) rt::test::report(__FILE__, __LINE__, "Expected the first SSE event before the terminal event was released.", true);
  EXPECT_EQ(decode(*first.value), firstFrame);

  const rt::Promise<ReadResult> terminalRead(reader.read());
  const rt::Promise<bool> terminalDelivered = terminalRead.then([](const ReadResult&) { return true; });
  const rt::Promise<bool> notYet = rt::newPromise<bool>([](const rt::Resolve<bool>& resolve, const rt::Reject&) {
    rt::setTimeout([resolve] { resolve(false); }, 20);
  });
  std::vector<rt::Promise<bool>> terminalRacers{terminalDelivered, notYet};
  const rt::Promise<bool> terminalRace = rt::race(std::move(terminalRacers));
  const bool terminalBeforeRelease = co_await terminalRace.wait();
  EXPECT_EQ(terminalBeforeRelease, false);
  terminalGate.resolve();

  const ReadResult terminal = co_await terminalRead.wait();
  if (terminal.done) rt::test::report(__FILE__, __LINE__, "Expected the terminal SSE event.", true);
  EXPECT_EQ(decode(*terminal.value), terminalFrame);
  const ReadResult done = co_await reader.read();
  EXPECT_EQ(done.done, true);
  const rt::Promise<void> cancelDelayed = rejectAfter<void>(100, "terminal did not cancel upstream");
  std::vector<rt::Promise<void>> cancelRacers{cancelled.promise, cancelDelayed};
  const rt::Promise<void> cancelRace = rt::race(std::move(cancelRacers));
  co_await cancelRace.wait();
  EXPECT_EQ(flags->upstreamCancelCount, 1);
  EXPECT_EQ(bytes(decode(*first.value) + decode(*terminal.value)).size(), bytes(firstFrame).size() + bytes(terminalFrame).size());
  EXPECT_EQ(decode(*first.value) + decode(*terminal.value), firstFrame + terminalFrame);
  EXPECT_EQ(source.locked(), false);
}

ASYNC_TEST("Responses proxy emits an official error event after premature EOF") {
  const rt::ByteStream source = chunked("data: {\"type\":\"response.output_text.delta\",\"delta\":\"x\"}\n\n");
  const rt::ByteStream proxied = proxyResponsesStream(source);
  const rt::Response response(rt::BodyInit{proxied});
  const std::string output = co_await response.text();
  EXPECT(matches("response.output_text.delta", output));
  EXPECT(matches("event: error", output));
  EXPECT(!matches("response.completed", output));
}

ASYNC_TEST("SSE keepalive emits short comment bursts while the provider is quiet") {
  const rt::Resolvers<void> gate = rt::withResolvers<void>();
  rt::UnderlyingSource<rt::ByteChunk> init;
  init.start = [opened = gate.promise](Controller controller) {
    (void)opened.then([controller] {
      controller.enqueue(bytes("data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n"));
      controller.close();
    });
  };
  const rt::ByteStream source(std::move(init));
  WithSseKeepaliveOptions options;
  options.intervalMs = 5;
  const rt::ByteStream keepalive = withSseKeepalive(source, options);
  const auto reader = keepalive.getReader();
  const rt::Promise<ReadResult> heartbeatRead(reader.read());
  const rt::Promise<ReadResult> heartbeatDelayed = rejectAfter<ReadResult>(100, "keepalive was delayed");
  std::vector<rt::Promise<ReadResult>> racers{heartbeatRead, heartbeatDelayed};
  const rt::Promise<ReadResult> race = rt::race(std::move(racers));
  const ReadResult heartbeat = co_await race.wait();
  EXPECT_EQ(heartbeat.done, false);
  EXPECT_EQ(decode(*heartbeat.value), std::string(": keepalive\n\n"));

  gate.resolve();
  const ReadResult terminal = co_await reader.read();
  EXPECT_EQ(terminal.done, false);
  EXPECT(matches("response.completed", decode(*terminal.value)));
  const ReadResult last = co_await reader.read();
  EXPECT_EQ(last.done, true);
}

ASYNC_TEST("Responses proxy does not wait for lifecycle callbacks before terminal delivery") {
  const rt::Promise<void> never;
  rt::UnderlyingSource<rt::ByteChunk> init;
  init.start = [](Controller controller) { controller.enqueue(bytes("data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\n\n")); };
  const rt::ByteStream source(std::move(init));
  ProxyResponsesStreamOptions options;
  options.onEvent = [never](ResponsesStreamEvent) -> rt::Task<void> { co_await never.wait(); };
  const rt::ByteStream proxied = proxyResponsesStream(source, options);
  const rt::Response response(rt::BodyInit{proxied});
  const rt::Promise<std::string> text(response.text());
  const rt::Promise<std::string> delayed = rejectAfter<std::string>(100, "terminal delivery was delayed");
  std::vector<rt::Promise<std::string>> racers{text, delayed};
  const rt::Promise<std::string> race = rt::race(std::move(racers));
  const std::string output = co_await race.wait();
  EXPECT(matches("response.completed", output));
}

ASYNC_TEST("Responses proxy cancellation is forwarded once without waiting for bookkeeping") {
  auto upstreamCancelCount = std::make_shared<int>(0);
  auto downstreamCancelCount = std::make_shared<int>(0);
  const rt::Promise<void> never;
  rt::UnderlyingSource<rt::ByteChunk> init;
  init.start = [](Controller controller) { controller.enqueue(bytes("data: {\"type\":\"response.output_text.delta\",\"delta\":\"x\"}\n\n")); };
  init.cancel = [upstreamCancelCount](rt::Reason) { *upstreamCancelCount += 1; };
  const rt::ByteStream source(std::move(init));
  ProxyResponsesStreamOptions options;
  options.onCancel = [downstreamCancelCount, never](rt::Reason) -> rt::Task<void> {
    *downstreamCancelCount += 1;
    co_await never.wait();
  };
  const rt::ByteStream proxied = proxyResponsesStream(source, options);
  const auto reader = proxied.getReader();
  const ReadResult first = co_await reader.read();
  EXPECT_EQ(first.done, false);
  const rt::Promise<void> cancel(reader.cancel(rt::thrown(json::value("downstream disconnected"))));
  const rt::Promise<void> delayed = rejectAfter<void>(100, "cancel was delayed");
  std::vector<rt::Promise<void>> racers{cancel, delayed};
  const rt::Promise<void> race = rt::race(std::move(racers));
  co_await race.wait();
  co_await rt::microtask();
  EXPECT_EQ(*downstreamCancelCount, 1);
  EXPECT_EQ(*upstreamCancelCount, 1);
  EXPECT_EQ(source.locked(), false);
}
