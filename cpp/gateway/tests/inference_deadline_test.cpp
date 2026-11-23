// tests/inference-deadline.test.ts. The synchronous TS tests run as ASYNC_TEST bodies only to
// get a fresh loop for their timers.
#include "uos/inference_deadline.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/number.hpp"
#include "uos/rt/test_async.hpp"

namespace {
  using namespace uos::inference_deadline;
  namespace rt = uos::rt;

  /** The wall-clock budget one inference attempt owns, in milliseconds. */
  constexpr double INFERENCE_BUDGET_MS = 30 * 60'000;

  // new Promise<void>((resolve) => { signal.addEventListener("abort", () => { resolve(); }, { once: true }); })
  rt::Promise<void> abortEvent(rt::AbortSignal signal) {
    return rt::newPromise<void>([signal](rt::Resolve<void> resolve, rt::Reject) {
      signal.addEventListener("abort", rt::EventListener([resolve] { resolve(); }), {.once = true});
    });
  }

  // `signal.reason?.name`
  std::string reasonName(const rt::AbortSignal& signal) { return rt::errorName(signal.reason()).value_or("undefined"); }
}

TEST("inference deadlines pin the documented per-attempt budget") {
  EXPECT_EQ(OPENAI_DEFAULT_REQUEST_TIMEOUT_MS, 10 * 60'000);
  EXPECT_EQ(OPENAI_FLEX_REQUEST_TIMEOUT_MS, 15 * 60'000);
  EXPECT_EQ(STREAM_FIRST_EVENT_DEADLINE_MS, INFERENCE_BUDGET_MS);
  EXPECT_EQ(STREAM_INACTIVITY_DEADLINE_MS, INFERENCE_BUDGET_MS);
  EXPECT_EQ(INFERENCE_DEADLINE_MS, STREAM_FIRST_EVENT_DEADLINE_MS);
  EXPECT_EQ(BUFFERED_INFERENCE_DEADLINE_MS, STREAM_FIRST_EVENT_DEADLINE_MS);
  // The failover reserve is subtracted from this budget before a fallback
  // attempt starts, so it must stay far below it.
  EXPECT_EQ(STREAM_FAILOVER_RESERVE_MS, 15'000);
}

ASYNC_TEST("inference signal propagates downstream cancellation") {
  const rt::AbortController controller;
  const rt::AbortSignal signal = createInferenceSignal(controller.signal());
  const rt::Reason reason = rt::makeReason(rt::DOMException("client disconnected", "AbortError"));

  controller.abort(reason);

  EXPECT_EQ(signal.aborted(), true);
  EXPECT(signal.reason() == reason);
  co_return;
}

ASYNC_TEST("inference signal enforces its deadline") {
  const rt::AbortSignal signal = createInferenceSignal(rt::AbortController().signal(), 1);
  // bun test holds a referenced per-test timeout while the test runs; the unreferenced
  // AbortSignal.timeout timer alone would let the loop run out of work.
  const rt::Timeout runner = rt::setTimeout([] {}, 5'000);

  rt::Promise<void> aborted = abortEvent(signal);
  co_await aborted.wait();
  rt::clearTimeout(runner);

  EXPECT_EQ(signal.aborted(), true);
  EXPECT_EQ(reasonName(signal), "TimeoutError");
}

ASYNC_TEST("failover attempts share one pre-header deadline") {
  const rt::AbortController request;
  const StreamDeadline shared = createStreamFirstEventDeadline(request.signal(), 80);
  const StreamDeadline primary = createStreamSemanticDeadline(shared.signal, 30);
  rt::Promise<void> primaryAborted = abortEvent(primary.signal);
  co_await primaryAborted.wait();
  EXPECT_EQ(reasonName(primary.signal), "TimeoutError");
  EXPECT_EQ(shared.signal.aborted(), false);
  const double remainingAtFallback = shared.remainingMs();
  EXPECT(remainingAtFallback > 0 && remainingAtFallback < 80);
  const StreamDeadline fallback = createStreamSemanticDeadline(shared.signal, rt::Math::ceil(remainingAtFallback) + 20);
  rt::Promise<void> fallbackAborted = abortEvent(fallback.signal);
  co_await fallbackAborted.wait();
  EXPECT_EQ(shared.signal.aborted(), true);
  EXPECT_EQ(reasonName(fallback.signal), "TimeoutError");
  primary.clear();
  fallback.clear();
  shared.clear();
}

ASYNC_TEST("clearing a selected attempt leaves it available after the shared deadline") {
  const rt::AbortController request;
  const StreamDeadline shared = createStreamFirstEventDeadline(request.signal(), 20);
  const StreamDeadline selected = createStreamSemanticDeadline(shared.signal, rt::Math::ceil(shared.remainingMs()));
  selected.clear();
  shared.clear();
  co_await rt::sleep(30);
  EXPECT_EQ(selected.signal.aborted(), false);
  EXPECT_EQ(shared.signal.aborted(), false);
}
