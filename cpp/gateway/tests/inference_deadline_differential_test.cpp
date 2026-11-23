// Differential cases for src/inference-deadline.ts beyond tests/inference-deadline.test.ts: the
// expected values were recorded from Bun 1.4.0 running the TypeScript module on the same inputs
// (with real timers; here the fake clock makes the same waits instant).
#include "uos/inference_deadline.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

namespace {
  using namespace uos::inference_deadline;
  namespace rt = uos::rt;

  rt::Promise<void> waitAbort(rt::AbortSignal signal) {
    return rt::newPromise<void>([signal](rt::Resolve<void> resolve, rt::Reject) {
      if (signal.aborted()) {
        resolve();
        return;
      }
      signal.addEventListener("abort", rt::EventListener([resolve] { resolve(); }), {.once = true});
    });
  }

  // `${reason.name}: ${reason.message}`, "undefined" before an abort.
  std::string reasonOf(const rt::AbortSignal& signal) {
    if (!signal.reason()) return "undefined";
    return rt::errorName(signal.reason()).value_or("") + ": " + rt::errorMessage(signal.reason()).value_or("");
  }
}

TEST("createInferenceSignal rejects timeouts AbortSignal.timeout refuses") {
  EXPECT_THROWS_MESSAGE(createInferenceSignal(rt::AbortController().signal(), -1), rt::TypeError, "Value -1 is outside the range [0, 9007199254740991]");
  EXPECT_THROWS_MESSAGE(createInferenceSignal(rt::AbortController().signal(), rt::Number::NaN), rt::TypeError, "Value NaN is outside the range [0, 9007199254740991]");
  EXPECT_THROWS_MESSAGE(createInferenceSignal(rt::AbortController().signal(), rt::Number::POSITIVE_INFINITY), rt::TypeError,
                        "Value Infinity is outside the range [0, 9007199254740991]");
}

ASYNC_TEST("remainingMs follows the raw timeout while the timer takes Bun's delay rules") {
  const StreamDeadline notANumber = createStreamFirstEventDeadline(rt::AbortController().signal(), rt::Number::NaN);
  EXPECT(rt::Number::isNaN(notANumber.remainingMs()));
  notANumber.clear();
  EXPECT_EQ(notANumber.signal.aborted(), false);
  const StreamDeadline negative = createStreamFirstEventDeadline(rt::AbortController().signal(), -5);
  EXPECT_EQ(negative.remainingMs(), 0);
  negative.clear();
  co_return;
}

ASYNC_TEST("abort without a reason, repeated aborts and repeated clears") {
  const StreamDeadline unnamed = createStreamSemanticDeadline(rt::AbortController().signal(), 1'000);
  unnamed.abort(nullptr);
  EXPECT_EQ(unnamed.signal.aborted(), true);
  EXPECT_EQ(reasonOf(unnamed.signal), "AbortError: The operation was aborted.");
  const StreamDeadline twice = createStreamSemanticDeadline(rt::AbortController().signal(), 1'000);
  twice.abort(rt::makeReason(rt::JsError("first")));
  twice.abort(rt::makeReason(rt::JsError("second")));
  EXPECT_EQ(reasonOf(twice.signal), "Error: first");
  const StreamDeadline cleared = createStreamSemanticDeadline(rt::AbortController().signal(), 1'000);
  cleared.clear();
  cleared.clear();
  EXPECT_EQ(cleared.signal.aborted(), false);
  co_return;
}

ASYNC_TEST("paid provider deadlines without a request signal, with an aborted one, and with a test override") {
  const StreamDeadline alone = createPaidProviderAttemptDeadline();
  const double remaining = alone.remainingMs();
  EXPECT(remaining > 119'000 && remaining <= 120'000);
  EXPECT_EQ(alone.signal.aborted(), false);
  alone.clear();
  const rt::AbortController request;
  request.abort(rt::makeReason(rt::JsError("gone")));
  const StreamDeadline aborted = createPaidProviderAttemptDeadline(request.signal());
  aborted.clear();
  EXPECT_EQ(aborted.signal.aborted(), true);
  EXPECT_EQ(reasonOf(aborted.signal), "Error: gone");

  setPaidProviderFirstHeadersDeadlineMsForTest(30);
  const StreamDeadline short_ = createPaidProviderAttemptDeadline();
  const double startedAt = rt::performanceNow();
  rt::Promise<void> fired = waitAbort(short_.signal);
  co_await fired.wait();
  EXPECT(rt::performanceNow() - startedAt >= 25);
  EXPECT_EQ(reasonOf(short_.signal), "TimeoutError: Paid provider response headers timed out.");
  EXPECT_EQ(short_.remainingMs(), 0);
  setPaidProviderFirstHeadersDeadlineMsForTest(std::nullopt);
  const StreamDeadline reset = createPaidProviderAttemptDeadline();
  EXPECT(reset.remainingMs() > 119'000);
  reset.clear();
}

ASYNC_TEST("the first-event override applies to omitted timeouts of both stream deadlines until reset") {
  setStreamFirstEventDeadlineMsForTest(20);
  const StreamDeadline first = createStreamFirstEventDeadline(rt::AbortController().signal());
  const StreamDeadline semantic = createStreamSemanticDeadline(rt::AbortController().signal());
  EXPECT(first.remainingMs() <= 20);
  EXPECT(semantic.remainingMs() <= 20);
  rt::Promise<void> firstFired = waitAbort(first.signal);
  co_await firstFired.wait();
  rt::Promise<void> semanticFired = waitAbort(semantic.signal);
  co_await semanticFired.wait();
  EXPECT_EQ(reasonOf(first.signal), "TimeoutError: Upstream response headers or first SSE event timed out.");
  EXPECT_EQ(reasonOf(semantic.signal), "TimeoutError: Upstream response timed out before semantic output.");
  const StreamDeadline undefinedTimeout = createStreamFirstEventDeadline(rt::AbortController().signal(), std::nullopt);
  EXPECT(undefinedTimeout.remainingMs() <= 20);
  undefinedTimeout.clear();
  setStreamFirstEventDeadlineMsForTest(std::nullopt);
  const StreamDeadline reset = createStreamFirstEventDeadline(rt::AbortController().signal());
  EXPECT(reset.remainingMs() > 1'799'000);
  reset.clear();
}

ASYNC_TEST("a cleared deadline never fires and an inference signal times out with Bun's TimeoutError") {
  const StreamDeadline cleared = createStreamFirstEventDeadline(rt::AbortController().signal(), 10);
  cleared.clear();
  co_await rt::sleep(30);
  EXPECT_EQ(cleared.signal.aborted(), false);
  EXPECT_EQ(cleared.remainingMs(), 0);
  const rt::Timeout runner = rt::setTimeout([] {}, 5'000);
  const rt::AbortSignal inference = createInferenceSignal(rt::AbortController().signal(), 20);
  rt::Promise<void> fired = waitAbort(inference);
  co_await fired.wait();
  rt::clearTimeout(runner);
  EXPECT_EQ(reasonOf(inference), "TimeoutError: The operation timed out.");
}
