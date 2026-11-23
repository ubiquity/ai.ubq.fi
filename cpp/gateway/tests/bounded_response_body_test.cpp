// Port of tests/bounded-response-body.test.ts.
#include "uos/bounded_response_body.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/http.hpp"
#include "uos/rt/stream.hpp"
#include "uos/rt/test_async.hpp"
#include "uos/rt/text_codec.hpp"
#include "helpers/live_allocations.hpp"

#include <cstdint>
#include <exception>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace rt = uos::rt;
namespace json = uos::json;
using uos::bounded_response_body::BOUNDED_RESPONSE_BODY_MAX_BYTES;
using uos::bounded_response_body::BoundedResponseBody;
using uos::bounded_response_body::ReadBoundedResponseBodyOptions;
using uos::bounded_response_body::readBoundedResponseBody;

namespace {
  using Controller = rt::ReadableStreamDefaultController<rt::ByteChunk>;

  // `await new Promise<void>((resolve) => { queueMicrotask(() => { resolve(); }); })`
  rt::Promise<void> microtaskHop() {
    return rt::newPromise<void>([](const rt::Resolve<void>& resolve, const rt::Reject&) { rt::queueMicrotask([resolve] { resolve(); }); });
  }

  // tests/helpers/sequence.ts `step(name, run)`: a failing step is reported with
  // its name and the following steps still run.
  rt::Task<void> step(std::string name, rt::Task<void> run) {
    rt::Reason failure;
    try {
      co_await std::move(run);
    } catch (...) {
      failure = std::current_exception();
    }
    if (failure) rt::test::report(__FILE__, __LINE__, "step " + name + " failed: " + rt::reasonToString(failure), false);
  }

  // bun:test arms a per-test timeout timer (5 s by default), which keeps the event
  // loop alive while an unreferenced AbortSignal.timeout timer runs; an
  // ASYNC_TEST loop has no such timer.
  struct BunTestTimeout {
    rt::Timeout timer = rt::setTimeout([] {}, 5'000);
    BunTestTimeout() = default;
    BunTestTimeout(const BunTestTimeout&) = delete;
    BunTestTimeout& operator=(const BunTestTimeout&) = delete;
    ~BunTestTimeout() { rt::clearTimeout(timer); }
  };

  rt::Response pendingResponse(std::function<void()> onCancel) {
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.pull = [](Controller) -> rt::Task<void> {
      rt::Promise<void> never;
      co_await never.wait();
    };
    source.cancel = [onCancel](rt::Reason) { onCancel(); };
    return rt::Response(rt::BodyInit(rt::ByteStream(std::move(source))));
  }
}

ASYNC_TEST("bounded response body reads fragmented complete bodies") {
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.start = [](Controller controller) {
    controller.enqueue(rt::encodeUtf8("frag"));
    controller.enqueue(rt::encodeUtf8("mented"));
    controller.close();
  };
  const rt::Response response(rt::BodyInit(rt::ByteStream(std::move(source))));

  auto read = readBoundedResponseBody(response);
  const BoundedResponseBody result = co_await std::move(read);
  EXPECT_EQ(result.complete, true);
  EXPECT_EQ(rt::decodeUtf8(result.bytes), std::string("fragmented"));
}

ASYNC_TEST("bounded response body recognizes EOF exactly at the byte limit") {
  auto cancellations = std::make_shared<int>(0);
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.start = [](Controller controller) {
    controller.enqueue(rt::ByteChunk(static_cast<std::size_t>(BOUNDED_RESPONSE_BODY_MAX_BYTES)));
    controller.close();
  };
  source.cancel = [cancellations](rt::Reason) { *cancellations += 1; };
  const rt::Response response(rt::BodyInit(rt::ByteStream(std::move(source))));

  auto read = readBoundedResponseBody(response);
  const BoundedResponseBody result = co_await std::move(read);
  EXPECT_EQ(result.complete, true);
  EXPECT_EQ(result.bytes.size(), static_cast<std::size_t>(BOUNDED_RESPONSE_BODY_MAX_BYTES));
  EXPECT_EQ(*cancellations, 0);
}

ASYNC_TEST("completed bounded response reads release chunks on the running loop") {
  const auto readBody = []() -> rt::Task<void> {
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [](Controller controller) {
      controller.enqueue(rt::ByteChunk(64 * 1024, 'x'));
      controller.close();
    };
    const rt::Response response(rt::BodyInit(rt::ByteStream(std::move(source))));
    ReadBoundedResponseBodyOptions options;
    options.timeoutMs = 10;
    auto read = readBoundedResponseBody(response, options);
    const auto result = co_await std::move(read);
    EXPECT(result.complete);
    EXPECT_EQ(result.bytes.size(), 64u * 1024u);
  };
  auto checking = uos::tests::helpers::expectStableLiveAllocations(readBody, 32, 20);
  co_await std::move(checking);
}

ASYNC_TEST("bounded response body truncates and cancels oversized bodies") {
  auto cancellations = std::make_shared<int>(0);
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.start = [](Controller controller) { controller.enqueue(rt::ByteChunk(static_cast<std::size_t>(BOUNDED_RESPONSE_BODY_MAX_BYTES) + 1)); };
  source.cancel = [cancellations](rt::Reason) { *cancellations += 1; };
  const rt::Response response(rt::BodyInit(rt::ByteStream(std::move(source))));

  auto read = readBoundedResponseBody(response);
  const BoundedResponseBody result = co_await std::move(read);
  EXPECT_EQ(result.complete, false);
  EXPECT_EQ(result.bytes.size(), static_cast<std::size_t>(BOUNDED_RESPONSE_BODY_MAX_BYTES));
  co_await microtaskHop().wait();
  EXPECT_EQ(*cancellations, 1);
}

namespace {
  rt::Task<void> timeoutStep() {
    auto cancellations = std::make_shared<int>(0);
    const double startedAt = rt::performanceNow();
    ReadBoundedResponseBodyOptions options;
    options.timeoutMs = 10;
    auto read = readBoundedResponseBody(pendingResponse([cancellations] { *cancellations += 1; }), std::move(options));
    const BoundedResponseBody result = co_await std::move(read);
    EXPECT_EQ(result.complete, false);
    EXPECT(rt::performanceNow() - startedAt < 500);
    co_await microtaskHop().wait();
    EXPECT_EQ(*cancellations, 1);
  }

  rt::Task<void> callerAbortStep() {
    auto cancellations = std::make_shared<int>(0);
    const rt::AbortController controller;
    rt::queueMicrotask([controller] { controller.abort(rt::thrown(json::value("caller cancelled"))); });
    ReadBoundedResponseBodyOptions options;
    options.signal = controller.signal();
    options.timeoutMs = 1'000;
    auto read = readBoundedResponseBody(pendingResponse([cancellations] { *cancellations += 1; }), std::move(options));
    const BoundedResponseBody result = co_await std::move(read);
    EXPECT_EQ(result.complete, false);
    co_await microtaskHop().wait();
    EXPECT_EQ(*cancellations, 1);
  }
}

ASYNC_TEST("bounded response body respects timeout and caller abort") {
  const BunTestTimeout testTimeout;
  auto timeout = step(std::string("timeout"), timeoutStep());
  co_await std::move(timeout);
  auto callerAbort = step(std::string("caller abort"), callerAbortStep());
  co_await std::move(callerAbort);
}

// The TS hands readBoundedResponseBody a cast object whose hand-written reader
// counts releaseLock() calls. An rt::Response always reads a real stream, so the
// same reads come from a stream whose pull yields two bytes each time, and the
// single release shows as the stream being unlocked afterwards.
ASYNC_TEST("bounded response body releases an incomplete reader exactly once") {
  auto cancellations = std::make_shared<int>(0);
  rt::UnderlyingSource<rt::ByteChunk> source;
  source.pull = [](Controller controller) { controller.enqueue(rt::ByteChunk(2)); };
  source.cancel = [cancellations](rt::Reason) { *cancellations += 1; };
  const rt::ByteStream body(std::move(source));

  ReadBoundedResponseBodyOptions options;
  options.maxBytes = 1;
  auto read = readBoundedResponseBody(rt::Response(rt::BodyInit(body)), std::move(options));
  const BoundedResponseBody result = co_await std::move(read);
  EXPECT_EQ(result.complete, false);
  co_await microtaskHop().wait();
  EXPECT_EQ(*cancellations, 1);
  EXPECT_EQ(body.locked(), false);
}

namespace {
  rt::Task<void> stalledCancellationStep(std::string mode) {
    auto cancellations = std::make_shared<int>(0);
    auto cancellationSettled = std::make_shared<bool>(false);
    auto resolvers = rt::withResolvers<void>();
    const rt::Promise<void> cancellation = resolvers.promise.then([cancellationSettled] { *cancellationSettled = true; });
    rt::UnderlyingSource<rt::ByteChunk> source;
    source.start = [mode](Controller controller) {
      if (mode == "overflow") controller.enqueue(rt::ByteChunk{1, 2});
    };
    source.cancel = [cancellations, cancellation](rt::Reason) -> rt::Task<void> {
      *cancellations += 1;
      co_await cancellation.wait();
    };
    const rt::ByteStream body(std::move(source));
    const double startedAt = rt::performanceNow();
    ReadBoundedResponseBodyOptions options;
    options.maxBytes = 1;
    options.timeoutMs = 10;
    const rt::Promise<BoundedResponseBody> boundedRead(readBoundedResponseBody(rt::Response(rt::BodyInit(body)), std::move(options)));
    rt::Timeout watchdog;
    rt::Reason failure;
    try {
      const rt::Promise<BoundedResponseBody> stalled = rt::newPromise<BoundedResponseBody>([&watchdog](const rt::Resolve<BoundedResponseBody>&, const rt::Reject& reject) {
        watchdog = rt::setTimeout([reject] { reject(rt::makeReason(rt::JsError("Bounded read waited for stalled cancellation."))); }, 500);
      });
      std::vector<rt::Promise<BoundedResponseBody>> racers{boundedRead, stalled};
      const rt::Promise<BoundedResponseBody> race = rt::race(std::move(racers));
      const BoundedResponseBody result = co_await race.wait();
      EXPECT(rt::performanceNow() - startedAt < 500);
      EXPECT_EQ(result.complete, false);
      EXPECT_EQ(result.bytes, (mode == "overflow" ? std::vector<std::uint8_t>{1} : std::vector<std::uint8_t>{}));
      EXPECT_EQ(*cancellations, 1);
      EXPECT_EQ(*cancellationSettled, false);
      EXPECT_EQ(body.locked(), false);
      const rt::ReadableStreamDefaultReader<rt::ByteChunk> reader = body.getReader();
      rt::Reason readFailure;
      try {
        const rt::ReadableStreamReadResult<rt::ByteChunk> next = co_await reader.read();
        EXPECT_EQ(next.done, true);
        EXPECT(!next.value);
      } catch (...) {
        readFailure = std::current_exception();
      }
      reader.releaseLock();
      if (readFailure) std::rethrow_exception(readFailure);
    } catch (...) {
      failure = std::current_exception();
    }
    rt::clearTimeout(watchdog);
    resolvers.resolve();
    co_await cancellation.wait();
    co_await boundedRead.wait();
    if (failure) std::rethrow_exception(failure);
    EXPECT_EQ(*cancellations, 1);
    EXPECT_EQ(body.locked(), false);
  }
}

ASYNC_TEST("bounded response body releases native readers before stalled cancellation settles") {
  for (const std::string mode : {"timeout", "overflow"}) {
    auto run = step(mode, stalledCancellationStep(mode));
    co_await std::move(run);
  }
}
