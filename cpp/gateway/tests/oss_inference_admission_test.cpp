// Port of tests/oss-inference-admission.test.ts.
#include "uos/inference_admission.hpp"

#include "uos/rt/abort.hpp"
#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/test_async.hpp"

#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <variant>
#include <vector>

namespace rt = uos::rt;
using namespace uos::inference_admission;

namespace {
  using Admitted = InferenceAdmissionGranted;
  using LocalOverload = InferenceAdmissionLocalOverload;
  using Headroom = rt::admission_resources::Headroom;
  constexpr std::uint64_t MiB = 1ULL << 20;

  std::string kindOf(const InferenceAdmissionResult& result) {
    if (const auto* overload = std::get_if<InferenceAdmissionLocalOverload>(&result)) return std::string(overload->kind);
    if (const auto* aborted = std::get_if<InferenceAdmissionCallerAborted>(&result)) return std::string(aborted->kind);
    return "admitted";
  }

  Admitted admittedOrThrow(const InferenceAdmissionResult& result, const std::string& message) {
    if (const auto* admitted = std::get_if<Admitted>(&result)) return *admitted;
    throw std::runtime_error(message + " (received " + kindOf(result) + ")");
  }

  LocalOverload localOverloadOrThrow(const InferenceAdmissionResult& result, const std::string& message) {
    if (std::holds_alternative<Admitted>(result)) throw std::runtime_error(message + " (received an admission)");
    if (const auto* overload = std::get_if<LocalOverload>(&result)) return *overload;
    throw std::runtime_error(message + " (received " + kindOf(result) + ")");
  }

  /** Block the event loop synchronously so no timer callback can run. */
  void holdEventLoopForMs(double ms) { rt::test::clock().advance(ms); }

  // Bun's test runner keeps the process alive while a test awaits a promise that only
  // AbortSignal.timeout (an unreferenced timer) settles; a referenced timer does that here.
  rt::Timeout keepAlive() {
    return rt::setTimeout([] {}, 600'000);
  }

  rt::Task<InferenceAdmissionResult> acquire(const InferenceAdmissionController& controller, InferenceAdmissionRequest request = {}) {
    rt::Promise<InferenceAdmissionResult> promise = controller.acquire(std::move(request));
    InferenceAdmissionResult result = co_await promise.wait();
    co_return result;
  }

  rt::Task<InferenceAdmissionResult> settled(rt::Promise<InferenceAdmissionResult> promise) {
    InferenceAdmissionResult result = co_await promise.wait();
    co_return result;
  }

  rt::Task<std::weak_ptr<void>> abandonQueuedAdmission() {
    const auto controller = createInferenceAdmissionController({.maxActive = 1, .maxWaiting = 1, .maxQueueWaitMs = 60'000});
    const auto occupied = co_await acquire(controller);
    ASSERT(std::holds_alternative<Admitted>(occupied));
    const rt::AbortController caller;
    InferenceAdmissionRequest request;
    request.signal = caller.signal();
    const auto queued = controller.acquire(request);
    EXPECT_EQ(controller.snapshot().waiting, 1.0);
    std::weak_ptr<void> signal = request.signal->state();
    co_return signal;
  }
}

TEST("abandoned queued admission releases its caller signal when the loop ends") {
  std::weak_ptr<void> caller;
  {
    rt::FakeClock clock;
    rt::Loop loop(clock);
    caller = rt::blockOn(loop, abandonQueuedAdmission());
    EXPECT(!caller.expired());
  }
  EXPECT(caller.expired());
}

ASYNC_TEST("admission bounds active permits and reports the queue wait to the integration") {
  const InferenceAdmissionController controller = createInferenceAdmissionController({.maxActive = 2, .maxWaiting = 4, .maxQueueWaitMs = 1'000});
  const InferenceAdmissionResult firstResult = co_await acquire(controller);
  const Admitted first = admittedOrThrow(firstResult, "expected the first attempt to be admitted");
  const InferenceAdmissionResult secondResult = co_await acquire(controller);
  const Admitted second = admittedOrThrow(secondResult, "expected the second attempt to be admitted");
  EXPECT_EQ(controller.snapshot().active, 2.0);
  EXPECT_EQ(controller.snapshot().waiting, 0.0);

  const rt::Promise<InferenceAdmissionResult> queued = controller.acquire({});
  co_await rt::sleep(15);
  EXPECT_EQ(controller.snapshot().active, 2.0);
  EXPECT_EQ(controller.snapshot().waiting, 1.0);

  first.release();
  const InferenceAdmissionResult queuedResult = co_await settled(queued);
  const Admitted admitted = admittedOrThrow(queuedResult, "expected the queued attempt to be admitted on release");
  EXPECT(admitted.waitedMs >= 10);
  EXPECT_EQ(controller.snapshot().active, 2.0);
  EXPECT_EQ(controller.snapshot().waiting, 0.0);

  admitted.release();
  second.release();
  EXPECT_EQ(controller.snapshot().active, 0.0);
}

ASYNC_TEST("admission rejects a new request once the waiting bound is full") {
  const InferenceAdmissionController controller = createInferenceAdmissionController({.maxActive = 1, .maxWaiting = 1, .maxQueueWaitMs = 1'000});
  const InferenceAdmissionResult activeResult = co_await acquire(controller);
  const Admitted active = admittedOrThrow(activeResult, "expected the first attempt to be admitted");
  const rt::Promise<InferenceAdmissionResult> queued = controller.acquire({});
  EXPECT_EQ(controller.snapshot().waiting, 1.0);

  const InferenceAdmissionResult rejectedResult = co_await acquire(controller);
  const LocalOverload rejected = localOverloadOrThrow(rejectedResult, "expected the full waiting bound to reject the new request");
  EXPECT_EQ(rejected.kind, "local_overload");
  EXPECT_EQ(rejected.cause, "queue_limit");
  EXPECT_EQ(rejected.waitedMs, 0.0);
  EXPECT_EQ(controller.snapshot().active, 1.0);

  active.release();
  const InferenceAdmissionResult queuedResult = co_await settled(queued);
  const Admitted admitted = admittedOrThrow(queuedResult, "expected the queued attempt to keep its place");
  admitted.release();
  EXPECT_EQ(controller.snapshot().active, 0.0);
}

ASYNC_TEST("admission enforces finite waiting and recovers after overload") {
  const double maxQueueWaitMs = 25;
  const rt::Timeout alive = keepAlive();
  const InferenceAdmissionController controller = createInferenceAdmissionController({.maxActive = 1, .maxWaiting = 1, .maxQueueWaitMs = maxQueueWaitMs});
  const InferenceAdmissionResult activeResult = co_await acquire(controller);
  const Admitted active = admittedOrThrow(activeResult, "expected the first attempt to be admitted");
  const rt::Promise<InferenceAdmissionResult> waiting = controller.acquire({});
  const InferenceAdmissionResult overBoundResult = co_await acquire(controller);
  const LocalOverload overBound = localOverloadOrThrow(overBoundResult, "expected the full waiting bound to reject the third attempt");
  EXPECT_EQ(overBound.cause, "queue_limit");

  const double startedAtMs = rt::performanceNow();
  const InferenceAdmissionResult expiredResult = co_await settled(waiting);
  const LocalOverload expired = localOverloadOrThrow(expiredResult, "expected the finite queue wait to reject the waiting attempt");
  const double elapsedWallMs = rt::performanceNow() - startedAtMs;
  EXPECT_EQ(expired.kind, "local_overload");
  EXPECT_EQ(expired.cause, "queue_wait_timeout");
  EXPECT(expired.waitedMs >= maxQueueWaitMs - 5);
  EXPECT(elapsedWallMs >= maxQueueWaitMs - 5 && elapsedWallMs < 5'000);
  EXPECT_EQ(controller.snapshot().active, 1.0);
  EXPECT_EQ(controller.snapshot().waiting, 0.0);

  active.release();
  EXPECT_EQ(controller.snapshot().active, 0.0);
  const InferenceAdmissionResult recoveredResult = co_await acquire(controller);
  const Admitted recovered = admittedOrThrow(recoveredResult, "expected the guard to admit after overload");
  EXPECT_EQ(recovered.waitedMs, 0.0);
  recovered.release();
  EXPECT_EQ(controller.snapshot().active, 0.0);
  rt::clearTimeout(alive);
}

ASYNC_TEST("admission removes an aborted waiter promptly and never gives it a permit") {
  const InferenceAdmissionController controller = createInferenceAdmissionController({.maxActive = 1, .maxWaiting = 2, .maxQueueWaitMs = 1'000});
  const InferenceAdmissionResult activeResult = co_await acquire(controller);
  const Admitted active = admittedOrThrow(activeResult, "expected the first attempt to be admitted");

  const rt::AbortController alreadyAborted;
  alreadyAborted.abort();
  const InferenceAdmissionRequest alreadyAbortedRequest{.signal = alreadyAborted.signal()};
  const InferenceAdmissionResult immediate = co_await acquire(controller, alreadyAbortedRequest);
  ASSERT(!std::holds_alternative<Admitted>(immediate));
  EXPECT_EQ(kindOf(immediate), "caller_aborted");
  EXPECT_EQ(std::get<InferenceAdmissionCallerAborted>(immediate).waitedMs, 0.0);
  EXPECT_EQ(controller.snapshot().waiting, 0.0);

  const rt::AbortController abortController;
  const rt::Promise<InferenceAdmissionResult> waiting = controller.acquire({.signal = abortController.signal()});
  EXPECT_EQ(controller.snapshot().waiting, 1.0);
  abortController.abort();
  const InferenceAdmissionResult aborted = co_await settled(waiting);
  ASSERT(!std::holds_alternative<Admitted>(aborted));
  EXPECT_EQ(kindOf(aborted), "caller_aborted");
  EXPECT_EQ(controller.snapshot().waiting, 0.0);

  active.release();
  EXPECT_EQ(controller.snapshot().active, 0.0);
  const InferenceAdmissionResult admittedResult = co_await acquire(controller);
  const Admitted admitted = admittedOrThrow(admittedResult, "expected a fresh attempt to be admitted");
  admitted.release();
  EXPECT_EQ(controller.snapshot().active, 0.0);
}

ASYNC_TEST("admission gives a freed permit to a live waiter, not to one that aborted first") {
  const InferenceAdmissionController controller = createInferenceAdmissionController({.maxActive = 1, .maxWaiting = 3, .maxQueueWaitMs = 1'000});
  const InferenceAdmissionResult activeResult = co_await acquire(controller);
  const Admitted active = admittedOrThrow(activeResult, "expected the first attempt to be admitted");
  const rt::AbortController abortController;
  const rt::Promise<InferenceAdmissionResult> aborted = controller.acquire({.signal = abortController.signal()});
  const rt::Promise<InferenceAdmissionResult> live = controller.acquire({});
  EXPECT_EQ(controller.snapshot().waiting, 2.0);

  abortController.abort();
  active.release();
  const InferenceAdmissionResult rejected = co_await settled(aborted);
  ASSERT(!std::holds_alternative<Admitted>(rejected));
  EXPECT_EQ(kindOf(rejected), "caller_aborted");
  const InferenceAdmissionResult liveResult = co_await settled(live);
  const Admitted admitted = admittedOrThrow(liveResult, "expected the live waiter to receive the freed permit");
  EXPECT_EQ(controller.snapshot().active, 1.0);
  admitted.release();
  EXPECT_EQ(controller.snapshot().active, 0.0);
}

ASYNC_TEST("admission returns a permit exactly once") {
  const InferenceAdmissionController controller = createInferenceAdmissionController({.maxActive = 1, .maxWaiting = 2, .maxQueueWaitMs = 1'000});
  const InferenceAdmissionResult activeResult = co_await acquire(controller);
  const Admitted active = admittedOrThrow(activeResult, "expected the first attempt to be admitted");
  const rt::Promise<InferenceAdmissionResult> queued = controller.acquire({});

  active.release();
  active.release();
  const InferenceAdmissionResult queuedResult = co_await settled(queued);
  const Admitted admitted = admittedOrThrow(queuedResult, "expected the queued attempt to be admitted");
  EXPECT_EQ(controller.snapshot().active, 1.0);

  admitted.release();
  admitted.release();
  EXPECT_EQ(controller.snapshot().active, 0.0);
}

ASYNC_TEST("fixture: an unrelated principal is served promptly despite a sustained backlog") {
  // The noisy principal holds the only permit and sustains a backlog behind it.
  // The first turn belongs to the principal that arrived first; the second turn
  // must then go to the unrelated principal, so the quiet request is admitted
  // after two turns even though the noisy principal still has queued work.
  // Arrival order alone served the whole backlog first and exhausted the quiet
  // request's queue wait (receipt 1c7ed31d reproduced exactly that starvation).
  const InferenceAdmissionController controller = createInferenceAdmissionController({.maxActive = 1, .maxWaiting = 6, .maxQueueWaitMs = 600});
  const InferenceAdmissionRequest noisy{.principal = "noisy"};
  const InferenceAdmissionResult noisyResult = co_await acquire(controller, noisy);
  const Admitted noisyActive = admittedOrThrow(noisyResult, "expected the noisy principal to hold the permit");
  const std::vector<rt::Promise<InferenceAdmissionResult>> noisyBacklog = {controller.acquire({.principal = "noisy"}), controller.acquire({.principal = "noisy"})};
  const rt::Promise<InferenceAdmissionResult> quiet = controller.acquire({.principal = "quiet"});
  EXPECT_EQ(controller.snapshot().waiting, 3.0);
  EXPECT_EQ(controller.snapshot().principals, 2.0);

  noisyActive.release();
  const InferenceAdmissionResult firstTurnResult = co_await settled(noisyBacklog[0]);
  const Admitted firstTurn = admittedOrThrow(firstTurnResult, "expected the first turn to serve the arrival-first principal");

  firstTurn.release();
  const InferenceAdmissionResult quietResult = co_await settled(quiet);
  const Admitted quietTurn = admittedOrThrow(quietResult, "expected the second turn to serve the unrelated principal");
  EXPECT_EQ(controller.snapshot().waiting, 1.0);

  quietTurn.release();
  const InferenceAdmissionResult backlogResult = co_await settled(noisyBacklog[1]);
  const Admitted backlogTurn = admittedOrThrow(backlogResult, "expected the remaining backlog to be served on its next turn");
  backlogTurn.release();
  EXPECT_EQ(controller.snapshot().active, 0.0);
  EXPECT_EQ(controller.snapshot().waiting, 0.0);
  EXPECT_EQ(controller.snapshot().principals, 0.0);
}

ASYNC_TEST("fixture: a delayed event loop cannot grant a permit past the queue wait") {
  const double maxQueueWaitMs = 25;
  const InferenceAdmissionController controller = createInferenceAdmissionController({.maxActive = 1, .maxWaiting = 4, .maxQueueWaitMs = maxQueueWaitMs});
  const InferenceAdmissionRequest holderRequest{.principal = "holder"};
  const InferenceAdmissionResult holderResult = co_await acquire(controller, holderRequest);
  const Admitted holder = admittedOrThrow(holderResult, "expected the holder to be admitted");
  const rt::Promise<InferenceAdmissionResult> queued = controller.acquire({.principal = "waiter"});
  EXPECT_EQ(controller.snapshot().waiting, 1.0);

  // The queue-wait timer cannot run while the loop is blocked, so the release
  // below would otherwise grant this permit after the advertised bound.
  holdEventLoopForMs(40);
  holder.release();

  const InferenceAdmissionResult queuedResult = co_await settled(queued);
  const LocalOverload result = localOverloadOrThrow(queuedResult, "expected the overdue waiter to expire instead of being granted");
  EXPECT_EQ(result.cause, "queue_wait_timeout");
  EXPECT(result.waitedMs >= maxQueueWaitMs);
  EXPECT_EQ(controller.snapshot().active, 0.0);
  EXPECT_EQ(controller.snapshot().waiting, 0.0);
  EXPECT_EQ(controller.snapshot().principals, 0.0);
}

ASYNC_TEST("admission restores an expiring principal's turn to its next waiter") {
  const double maxQueueWaitMs = 25;
  const InferenceAdmissionController controller = createInferenceAdmissionController({.maxActive = 1, .maxWaiting = 6, .maxQueueWaitMs = maxQueueWaitMs});
  const InferenceAdmissionRequest holderRequest{.principal = "holder"};
  const InferenceAdmissionResult holderResult = co_await acquire(controller, holderRequest);
  const Admitted holder = admittedOrThrow(holderResult, "expected the holder to be admitted");
  const rt::Promise<InferenceAdmissionResult> overdue = controller.acquire({.principal = "a"});
  holdEventLoopForMs(40);
  // Both of these arrive after the block, so only the head waiter is overdue.
  const rt::Promise<InferenceAdmissionResult> next = controller.acquire({.principal = "a"});
  const rt::Promise<InferenceAdmissionResult> other = controller.acquire({.principal = "b"});
  EXPECT_EQ(controller.snapshot().waiting, 3.0);

  holder.release();
  const InferenceAdmissionResult overdueResult = co_await settled(overdue);
  const LocalOverload overdueOverload = localOverloadOrThrow(overdueResult, "expected the overdue head waiter to expire");
  EXPECT_EQ(overdueOverload.cause, "queue_wait_timeout");
  const InferenceAdmissionResult nextResult = co_await settled(next);
  const Admitted nextAdmitted = admittedOrThrow(nextResult, "expected the same principal's next waiter to keep the restored turn");
  EXPECT_EQ(controller.snapshot().waiting, 1.0);

  nextAdmitted.release();
  const InferenceAdmissionResult otherResult = co_await settled(other);
  const Admitted otherAdmitted = admittedOrThrow(otherResult, "expected the unrelated principal to be served next");
  otherAdmitted.release();
  EXPECT_EQ(controller.snapshot().active, 0.0);
  EXPECT_EQ(controller.snapshot().principals, 0.0);
}

ASYNC_TEST("admission rotates waiting turns across three principals during a sustained burst") {
  const InferenceAdmissionController controller = createInferenceAdmissionController({.maxActive = 1, .maxWaiting = 12, .maxQueueWaitMs = 500});
  const InferenceAdmissionRequest holderRequest{.principal = "a"};
  const InferenceAdmissionResult holderResult = co_await acquire(controller, holderRequest);
  const Admitted holder = admittedOrThrow(holderResult, "expected the burst principal to hold the permit");
  const std::vector<rt::Promise<InferenceAdmissionResult>> burst = {
    controller.acquire({.principal = "a"}),
    controller.acquire({.principal = "a"}),
    controller.acquire({.principal = "a"}),
    controller.acquire({.principal = "a"}),
  };
  const rt::Promise<InferenceAdmissionResult> other = controller.acquire({.principal = "b"});
  const rt::Promise<InferenceAdmissionResult> third = controller.acquire({.principal = "c"});
  EXPECT_EQ(controller.snapshot().waiting, 6.0);
  EXPECT_EQ(controller.snapshot().principals, 3.0);

  // One turn per principal per rotation, and arrival order inside each
  // principal: the sustained burst takes one waiting turn while `b` and `c`
  // still have requests queued, so neither waits for the whole burst.
  const std::vector<rt::Promise<InferenceAdmissionResult>> turnOrder = {burst[0], other, third, burst[1], burst[2], burst[3]};
  std::vector<Admitted> served;
  Admitted held = holder;
  for (std::size_t index = 0; index < turnOrder.size(); ++index) {
    held.release();
    const InferenceAdmissionResult nextResult = co_await settled(turnOrder[index]);
    held = admittedOrThrow(nextResult, "expected turn " + std::to_string(index + 1) + " to serve the scheduled principal");
    EXPECT_EQ(controller.snapshot().waiting, static_cast<double>(turnOrder.size() - index - 1));
    served.push_back(held);
  }
  EXPECT_EQ(controller.snapshot().principals, 0.0);
  for (const Admitted& permit : served) permit.release();
  EXPECT_EQ(controller.snapshot().active, 0.0);
}

ASYNC_TEST("admission keeps principal-less waiting requests in arrival order") {
  const InferenceAdmissionController controller = createInferenceAdmissionController({.maxActive = 1, .maxWaiting = 4, .maxQueueWaitMs = 500});
  const InferenceAdmissionResult holderResult = co_await acquire(controller);
  const Admitted holder = admittedOrThrow(holderResult, "expected the principal-less holder to be admitted");
  const rt::Promise<InferenceAdmissionResult> first = controller.acquire({});
  const rt::Promise<InferenceAdmissionResult> second = controller.acquire({});
  EXPECT_EQ(controller.snapshot().waiting, 2.0);
  EXPECT_EQ(controller.snapshot().principals, 1.0);

  holder.release();
  const InferenceAdmissionResult firstResult = co_await settled(first);
  const Admitted servedFirst = admittedOrThrow(firstResult, "expected the first principal-less request to be served first");
  EXPECT_EQ(controller.snapshot().waiting, 1.0);
  servedFirst.release();
  const InferenceAdmissionResult secondResult = co_await settled(second);
  const Admitted servedSecond = admittedOrThrow(secondResult, "expected the second principal-less request to be served next");
  servedSecond.release();
  EXPECT_EQ(controller.snapshot().active, 0.0);
  EXPECT_EQ(controller.snapshot().principals, 0.0);
}

ASYNC_TEST("admission removes a cancelled principal's empty waiting state") {
  const InferenceAdmissionController controller = createInferenceAdmissionController({.maxActive = 1, .maxWaiting = 4, .maxQueueWaitMs = 1'000});
  const InferenceAdmissionRequest holderRequest{.principal = "holder"};
  const InferenceAdmissionResult holderResult = co_await acquire(controller, holderRequest);
  const Admitted holder = admittedOrThrow(holderResult, "expected the holder to be admitted");
  const rt::AbortController abortController;
  const rt::Promise<InferenceAdmissionResult> cancelled = controller.acquire({.signal = abortController.signal(), .principal = "cancelled"});
  const rt::Promise<InferenceAdmissionResult> kept = controller.acquire({.principal = "kept"});
  EXPECT_EQ(controller.snapshot().waiting, 2.0);
  EXPECT_EQ(controller.snapshot().principals, 2.0);

  abortController.abort();
  const InferenceAdmissionResult cancelledResult = co_await settled(cancelled);
  ASSERT(!std::holds_alternative<Admitted>(cancelledResult));
  EXPECT_EQ(kindOf(cancelledResult), "caller_aborted");
  EXPECT_EQ(controller.snapshot().waiting, 1.0);
  EXPECT_EQ(controller.snapshot().principals, 1.0);

  holder.release();
  const InferenceAdmissionResult keptResult = co_await settled(kept);
  const Admitted keptAdmitted = admittedOrThrow(keptResult, "expected the surviving principal to keep its turn");
  keptAdmitted.release();
  EXPECT_EQ(controller.snapshot().active, 0.0);
  EXPECT_EQ(controller.snapshot().principals, 0.0);
}

ASYNC_TEST("admission removes expired principals' empty waiting state") {
  const double maxQueueWaitMs = 25;
  const rt::Timeout alive = keepAlive();
  const InferenceAdmissionController controller = createInferenceAdmissionController({.maxActive = 1, .maxWaiting = 4, .maxQueueWaitMs = maxQueueWaitMs});
  const InferenceAdmissionRequest holderRequest{.principal = "holder"};
  const InferenceAdmissionResult holderResult = co_await acquire(controller, holderRequest);
  const Admitted holder = admittedOrThrow(holderResult, "expected the holder to be admitted");
  const rt::Promise<InferenceAdmissionResult> first = controller.acquire({.principal = "expired-a"});
  const rt::Promise<InferenceAdmissionResult> second = controller.acquire({.principal = "expired-b"});
  EXPECT_EQ(controller.snapshot().principals, 2.0);

  const InferenceAdmissionResult firstSettled = co_await settled(first);
  const LocalOverload firstResult = localOverloadOrThrow(firstSettled, "expected the first queue wait to expire");
  const InferenceAdmissionResult secondSettled = co_await settled(second);
  const LocalOverload secondResult = localOverloadOrThrow(secondSettled, "expected the second queue wait to expire");
  EXPECT_EQ(firstResult.cause, "queue_wait_timeout");
  EXPECT_EQ(secondResult.cause, "queue_wait_timeout");
  EXPECT(firstResult.waitedMs >= maxQueueWaitMs - 5);
  EXPECT_EQ(controller.snapshot().waiting, 0.0);
  EXPECT_EQ(controller.snapshot().principals, 0.0);

  holder.release();
  EXPECT_EQ(controller.snapshot().active, 0.0);
  const InferenceAdmissionRequest freshRequest{.principal = "expired-a"};
  const InferenceAdmissionResult freshResult = co_await acquire(controller, freshRequest);
  const Admitted fresh = admittedOrThrow(freshResult, "expected a fresh admission after cleanup");
  fresh.release();
  EXPECT_EQ(controller.snapshot().active, 0.0);
  rt::clearTimeout(alive);
}

ASYNC_TEST("healthy adaptive admission grows past 512 permits without sampling the idle loop") {
  int samples = 0;
  const auto controller = createAdaptiveInferenceAdmissionController([&] {
    ++samples;
    return Headroom{600 * MiB, 2'000};
  });
  EXPECT_EQ(controller.snapshot().maxWaiting, 128.0);
  EXPECT_EQ(controller.snapshot().maxActive, 0.0);
  EXPECT_EQ(rt::loop().timerCount(), 0U);
  EXPECT_EQ(samples, 0);
  const InferenceAdmissionRequest agent{.principal = "agent-pool"};
  std::vector<Admitted> permits;
  for (int i = 0; i < 550; ++i) {
    const auto result = co_await acquire(controller, agent);
    permits.push_back(admittedOrThrow(result, "expected every stream in the burst to be admitted"));
    EXPECT_EQ(permits.back().waitedMs, 0.0);
  }
  EXPECT_EQ(controller.snapshot().active, 550.0);
  EXPECT_EQ(controller.snapshot().maxActive, 600.0);
  EXPECT_EQ(controller.snapshot().waiting, 0.0);
  EXPECT_EQ(controller.snapshot().limitingResource, "memory");
  EXPECT_EQ(rt::loop().timerCount(), 1U);
  EXPECT_EQ(samples, 1);
  for (const auto& permit : permits) permit.release();
  co_await rt::sleep(500);
  EXPECT_EQ(controller.snapshot().active, 0.0);
  EXPECT_EQ(rt::loop().timerCount(), 0U);
  EXPECT_EQ(samples, 1);
}

ASYNC_TEST("adaptive memory shrink preserves active permits and queued demand recovers on its own") {
  Headroom sample{4 * MiB, 32};
  const auto alive = keepAlive();
  const auto controller = createAdaptiveInferenceAdmissionController([&] { return sample; });
  const auto first = admittedOrThrow(co_await acquire(controller), "expected first stream");
  const auto second = admittedOrThrow(co_await acquire(controller), "expected second stream");
  co_await rt::sleep(100);
  sample.memoryBytes = MiB;
  const auto waiting = controller.acquire({});
  EXPECT_EQ(controller.snapshot().active, 2.0);
  EXPECT_EQ(controller.snapshot().maxActive, 1.0);
  EXPECT_EQ(controller.snapshot().waiting, 1.0);
  sample.memoryBytes = 3 * MiB;
  const auto third = admittedOrThrow(co_await settled(waiting), "expected recovery without another arrival or release");
  EXPECT_EQ(third.waitedMs, 100.0);
  EXPECT_EQ(controller.snapshot().active, 3.0);
  EXPECT_EQ(controller.snapshot().waiting, 0.0);
  first.release();
  second.release();
  third.release();
  rt::clearTimeout(alive);
  EXPECT_EQ(rt::loop().timerCount(), 0U);
}

ASYNC_TEST("adaptive admission reserves weighted inputs and bounds queued body memory") {
  Headroom sample{12 * MiB, 64};
  const auto controller = createAdaptiveInferenceAdmissionController([&] { return sample; });
  const auto large = admittedOrThrow(co_await acquire(controller, {.bodyBytes = 2 * MiB}), "expected an 8 MiB reservation");
  EXPECT_EQ(controller.snapshot().maxActive, 5.0);
  const auto waiting = controller.acquire({.bodyBytes = MiB + MiB / 2});
  EXPECT_EQ(controller.snapshot().waiting, 1.0);
  const auto cannotQueue = co_await acquire(controller, {.bodyBytes = 3 * MiB});
  EXPECT_EQ(localOverloadOrThrow(cannotQueue, "expected the retained bodies to leave too little queue space").cause, "queue_memory");
  const auto overflow = co_await acquire(controller, {.bodyBytes = std::numeric_limits<std::uint64_t>::max()});
  EXPECT_EQ(localOverloadOrThrow(overflow, "expected saturated reservation to exceed the memory sample").cause, "queue_memory");
  large.release();
  const auto recovered = admittedOrThrow(co_await settled(waiting), "expected transfer from queued body to live reservation");
  EXPECT_EQ(controller.snapshot().maxActive, 7.0);
  recovered.release();
}

ASYNC_TEST("adaptive admission debits descriptors until a fresh sample even after release") {
  Headroom sample{64 * MiB, 4};
  const auto alive = keepAlive();
  const auto controller = createAdaptiveInferenceAdmissionController([&] { return sample; });
  const auto first = admittedOrThrow(co_await acquire(controller), "expected first FD budget grant");
  const auto second = admittedOrThrow(co_await acquire(controller), "expected second FD budget grant");
  const auto waiting = controller.acquire({});
  first.release();
  EXPECT_EQ(controller.snapshot().active, 1.0);
  EXPECT_EQ(controller.snapshot().maxActive, 1.0);
  EXPECT_EQ(controller.snapshot().waiting, 1.0);
  EXPECT_EQ(controller.snapshot().limitingResource, "descriptors");
  sample.descriptors = 3;
  const auto third = admittedOrThrow(co_await settled(waiting), "expected fresh headroom to reserve one upstream FD and a new stream");
  EXPECT_EQ(controller.snapshot().active, 2.0);
  EXPECT_EQ(controller.snapshot().maxActive, 2.0);
  EXPECT_EQ(third.waitedMs, 100.0);
  second.release();
  third.release();
  rt::clearTimeout(alive);
}

ASYNC_TEST("missing resources block adaptive grants and recovering resources preserve fair queue turns") {
  Headroom sample{std::nullopt, 64};
  const auto alive = keepAlive();
  const auto controller = createAdaptiveInferenceAdmissionController([&] { return sample; });
  const auto firstA = controller.acquire({.principal = "a"});
  const auto secondA = controller.acquire({.principal = "a"});
  const auto firstB = controller.acquire({.principal = "b"});
  EXPECT_EQ(controller.snapshot().limitingResource, "memory_unavailable");
  EXPECT_EQ(controller.snapshot().active, 0.0);
  sample = {MiB, std::nullopt};
  co_await rt::sleep(101);
  EXPECT_EQ(controller.snapshot().limitingResource, "descriptors_unavailable");
  EXPECT_EQ(controller.snapshot().waiting, 3.0);
  sample.descriptors = 64;
  const auto a = admittedOrThrow(co_await settled(firstA), "expected the earliest queued principal");
  EXPECT_EQ(a.waitedMs, 200.0);
  a.release();
  const auto b = admittedOrThrow(co_await settled(firstB), "expected the other principal's fair turn");
  EXPECT_EQ(b.waitedMs, 200.0);
  b.release();
  const auto a2 = admittedOrThrow(co_await settled(secondA), "expected the second request from the first principal");
  EXPECT_EQ(a2.waitedMs, 200.0);
  a2.release();
  rt::clearTimeout(alive);
  EXPECT_EQ(rt::loop().timerCount(), 0U);
}

ASYNC_TEST("a late demand probe pauses grants and two healthy probes recover the queue") {
  const auto alive = keepAlive();
  const auto controller = createAdaptiveInferenceAdmissionController([] { return Headroom{64 * MiB, 64}; });
  const auto first = admittedOrThrow(co_await acquire(controller), "expected admission on a healthy loop");
  holdEventLoopForMs(400);
  const auto waiting = controller.acquire({});
  EXPECT_EQ(controller.snapshot().active, 1.0);
  EXPECT_EQ(controller.snapshot().waiting, 1.0);
  EXPECT_EQ(controller.snapshot().limitingResource, "event_loop_lag");
  EXPECT_EQ(controller.snapshot().eventLoopLagMs, 300.0);
  co_await rt::sleep(101);
  EXPECT_EQ(controller.snapshot().limitingResource, "event_loop_lag");
  EXPECT_EQ(controller.snapshot().waiting, 1.0);
  const auto second = admittedOrThrow(co_await settled(waiting), "expected queue recovery after the second on-time probe");
  EXPECT_EQ(second.waitedMs, 200.0);
  EXPECT_EQ(controller.snapshot().eventLoopLagMs, 0.0);
  first.release();
  second.release();
  rt::clearTimeout(alive);
  EXPECT_EQ(rt::loop().timerCount(), 0U);
}

ASYNC_TEST("adaptive pressure preserves queue capacity, the deadline, and cancellation") {
  const auto alive = keepAlive();
  const auto controller = createAdaptiveInferenceAdmissionController([] { return Headroom{0, 64}; });
  const rt::AbortController caller;
  const auto aborted = controller.acquire({.signal = caller.signal()});
  const auto expires = controller.acquire({});
  for (int i = 0; i < 126; ++i) (void)controller.acquire({});
  const auto rejects = co_await acquire(controller);
  EXPECT_EQ(localOverloadOrThrow(rejects, "expected bounded queue overload").cause, "queue_limit");
  EXPECT_EQ(controller.snapshot().waiting, 128.0);
  caller.abort();
  EXPECT(std::holds_alternative<InferenceAdmissionCallerAborted>(co_await settled(aborted)));
  const auto expired = localOverloadOrThrow(co_await settled(expires), "expected pressure to honor the wait deadline");
  EXPECT_EQ(expired.waitedMs, 5'000.0);
  EXPECT_EQ(expired.cause, "queue_wait_timeout");
  co_await rt::sleep(1);
  EXPECT_EQ(controller.snapshot().waiting, 0.0);
  rt::clearTimeout(alive);
  EXPECT_EQ(rt::loop().timerCount(), 0U);
}

TEST("internal construction rejects limits that cannot bound anything") {
  EXPECT_THROWS(createInferenceAdmissionController({.maxActive = 0}), rt::RangeError);
  EXPECT_THROWS(createInferenceAdmissionController({.maxWaiting = -1}), rt::RangeError);
  EXPECT_THROWS(createInferenceAdmissionController({.maxQueueWaitMs = 1.5}), rt::RangeError);
}
