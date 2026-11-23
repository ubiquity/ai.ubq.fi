// Port of tests/responses-stream-http.test.ts: src/responses-stream.ts over a real loopback
// serveHttp stream. Bun's fetch is the loopback transport of tests/helpers/loopback_http.
// Links: src/responses_stream.cpp src/inference_deadline.cpp src/utils.cpp src/runtime/http.cpp
//        src/runtime/http_upgrade.cpp src/runtime/http_body.cpp src/runtime/http_early_sse.cpp src/runtime/http_tasks.cpp
//        src/runtime/websocket.cpp tests/helpers/loopback_http.cpp
#include "uos/responses_stream.hpp"
#include "uos/runtime/http.hpp"

#include "tests/helpers/loopback_http.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/fetch.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/ordered.hpp"
#include "uos/rt/regex.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {
  namespace rt = uos::rt;
  using uos::responses_stream::proxyResponsesStream;
  using uos::runtime::http::HttpServer;
  using uos::runtime::http::NetAddr;
  using uos::runtime::http::ServeHandlerInfo;
  using uos::runtime::http::ServeHttpOptions;
  using uos::runtime::http::serveHttp;
  using uos::tests::helpers::loopback_http::ScopedLoopbackFetch;

  // `Set<ReturnType<typeof setTimeout>>`
  using Timers = rt::OrderedMap<double, rt::Timeout>;

  // Timer callbacks are hoisted out of the stream's `start` method so the nested
  // closure depth stays within the lint ceiling: the stream controller, the timer
  // registry, and the payload are passed in as parameters instead of captured.
  void scheduleEnqueue(const std::shared_ptr<Timers>& timers, const rt::ReadableStreamDefaultController<rt::ByteChunk>& controller, rt::ByteChunk payload, double delayMs) {
    auto id = std::make_shared<double>(0);
    const rt::Timeout timer = rt::setTimeout(
      [timers, controller, payload, id] {
        timers->delete_(*id);
        controller.enqueue(payload);
      },
      delayMs);
    *id = timer.id();
    timers->set(*id, timer);
  }

  void scheduleClose(const std::shared_ptr<Timers>& timers, const rt::ReadableStreamDefaultController<rt::ByteChunk>& controller, double delayMs) {
    auto id = std::make_shared<double>(0);
    const rt::Timeout timer = rt::setTimeout(
      [timers, controller, id] {
        timers->delete_(*id);
        controller.close();
      },
      delayMs);
    *id = timer.id();
    timers->set(*id, timer);
  }

  rt::ByteChunk slice(const rt::ByteChunk& bytes, std::size_t start, std::size_t end) { return rt::ByteChunk(bytes.begin() + static_cast<std::ptrdiff_t>(start), bytes.begin() + static_cast<std::ptrdiff_t>(end)); }
}

ASYNC_TEST("Responses proxy terminates a fragmented real HTTP stream without waiting for trailing bytes") {
  const ScopedLoopbackFetch loopback;
  const rt::ByteChunk delta = rt::encodeUtf8("data: {\"type\":\"response.output_text.delta\",\"delta\":\"real HTTP \xF0\x9F\x8C\x8D\"}\n\n");
  const rt::ByteChunk terminal = rt::encodeUtf8("data: {\"type\":\"response.completed\",\"response\":{\"status\":\"completed\"}}\r\n\r\n");
  const auto emoji = std::find(delta.begin(), delta.end(), 0xF0);
  ASSERT(emoji != delta.end());  // the fixture must contain the four-byte UTF-8 emoji
  const std::size_t emojiStart = static_cast<std::size_t>(emoji - delta.begin());
  const std::vector<rt::ByteChunk> chunks{
    slice(delta, 0, emojiStart + 2),
    slice(delta, emojiStart + 2, delta.size()),
    slice(terminal, 0, 7),
    slice(terminal, 7, terminal.size() - 1),
    slice(terminal, terminal.size() - 1, terminal.size()),
  };
  auto timers = std::make_shared<Timers>();
  auto upstreamCancelled = std::make_shared<bool>(false);
  auto cancelled = rt::withResolvers<void>();
  const rt::Resolve<void> resolveCancelled = cancelled.resolve;

  ServeHttpOptions options;
  options.hostname = "127.0.0.1";
  options.port = 0;
  options.onListen = [](const NetAddr&) {};
  const HttpServer server = serveHttp(options, [=](rt::Request, ServeHandlerInfo) -> rt::Task<rt::Response> {
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [=](rt::ReadableStreamDefaultController<rt::ByteChunk> controller) {
      for (std::size_t index = 0; index < chunks.size(); ++index) scheduleEnqueue(timers, controller, chunks[index], 5 * static_cast<double>(index + 1));
      scheduleEnqueue(timers, controller, rt::encodeUtf8("data: {\"type\":\"response.output_text.delta\",\"delta\":\"post-terminal\"}\n\n"), 250);
      scheduleClose(timers, controller, 1'000);
    };
    source.cancel = [=](rt::Reason) {
      *upstreamCancelled = true;
      for (const rt::Timeout& timer : rt::arrayFrom(timers->values())) rt::clearTimeout(timer);
      timers->clear();
      resolveCancelled();
    };
    rt::ResponseInit init;
    init.headers = rt::Headers{{"Content-Type", "text/event-stream"}};
    co_return rt::Response(rt::BodyInit(rt::ByteStream(std::move(source))), std::move(init));
  });
  co_await server.ready.wait();

  const NetAddr address = server.addr();
  auto fetching = rt::fetch("http://127.0.0.1:" + rt::numberToString(address.port) + "/responses");
  const rt::Response upstream = co_await std::move(fetching);
  const std::optional<rt::ByteStream> upstreamBody = upstream.body();
  ASSERT(upstreamBody.has_value());
  const double startedAt = rt::performanceNow();
  const rt::Response proxied(rt::BodyInit(proxyResponsesStream(*upstreamBody)));
  auto reading = proxied.text();
  const std::string output = co_await std::move(reading);
  const double elapsedMs = rt::performanceNow() - startedAt;

  EXPECT(rt::Regex("real HTTP \xF0\x9F\x8C\x8D", "").test(output));
  EXPECT(rt::Regex("response.completed", "").test(output));
  EXPECT(!rt::Regex("post-terminal", "").test(output));
  EXPECT(elapsedMs < 200);  // terminal delivery took elapsedMs
  const rt::Promise<void> deadline(rt::async([]() -> rt::Task<void> {
    co_await rt::sleep(200);
    throw rt::JsError("real HTTP upstream was not cancelled");
  }));
  std::vector<rt::Promise<void>> racers{cancelled.promise, deadline};
  co_await rt::race(std::move(racers)).wait();
  EXPECT_EQ(*upstreamCancelled, true);
  for (const rt::Timeout& timer : rt::arrayFrom(timers->values())) rt::clearTimeout(timer);
  auto stopping = server.shutdown();
  co_await stopping.wait();
}
