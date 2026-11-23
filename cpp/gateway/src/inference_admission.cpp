#include "uos/inference_admission.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/number.hpp"

#include <algorithm>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uos::inference_admission {
  namespace {
    using Principal = std::optional<std::string>;

    struct Waiter {
      Principal principal;
      double queuedAtMs = 0;
      std::optional<rt::AbortSignal> callerSignal;
      std::optional<rt::AbortSignal> waitSignal;
      rt::EventListener onWaitAbort;
      std::weak_ptr<rt::detail::TimerState> timeout;
      bool settled = false;
      std::optional<rt::Resolve<InferenceAdmissionResult>> resolve;

      void stopWaiting() {
        if (waitSignal) waitSignal->removeEventListener("abort", onWaitAbort);
        waitSignal.reset();
        onWaitAbort = {};
        // Clear the exact timer without invoking the global clearTimeout hook.
        rt::clearInterval(rt::Timeout(timeout.lock()));
        timeout.reset();
      }

      ~Waiter() { stopWaiting(); }
    };

    double elapsedMs(double since) { return rt::Math::max(0, rt::Math::round(rt::performanceNow() - since)); }

    void validateLimits(const InferenceAdmissionLimits& limits) {
      if (!rt::Number::isSafeInteger(limits.maxActive) || limits.maxActive < 1) {
        throw rt::RangeError("Inference admission maxActive must be a positive safe integer.");
      }
      if (!rt::Number::isSafeInteger(limits.maxWaiting) || limits.maxWaiting < 0) {
        throw rt::RangeError("Inference admission maxWaiting must be a non-negative safe integer.");
      }
      if (!rt::Number::isSafeInteger(limits.maxQueueWaitMs) || limits.maxQueueWaitMs < 0) {
        throw rt::RangeError("Inference admission maxQueueWaitMs must be a non-negative safe integer.");
      }
    }

    struct Controller : std::enable_shared_from_this<Controller> {
      double maxActive = 0;
      double maxWaiting = 0;
      double maxQueueWaitMs = 0;

      double active = 0;
      double waitingCount = 0;
      // One arrival-ordered queue per principal, and the rotation of principals
      // whose turn it is to place one request into an active slot.  A principal
      // owns a queue and a turn only while it has waiting requests, so a burst that
      // drains leaves no per-principal state behind.
      std::unordered_map<Principal, std::vector<std::shared_ptr<Waiter>>> waitingByPrincipal;
      std::vector<Principal> turnOrder;

      void detachWaiter(const std::shared_ptr<Waiter>& waiter) {
        waiter->stopWaiting();
        const auto found = waitingByPrincipal.find(waiter->principal);
        if (found == waitingByPrincipal.end()) return;
        std::vector<std::shared_ptr<Waiter>>& queue = found->second;
        const auto index = std::find(queue.begin(), queue.end(), waiter);
        if (index == queue.end()) return;
        queue.erase(index);
        waitingCount -= 1;
        if (!queue.empty()) return;
        // An empty principal must not keep a waiting turn or a map entry alive, or
        // cancelled and expired waiters would grow the guard without bound.
        waitingByPrincipal.erase(found);
        const auto turn = std::find(turnOrder.begin(), turnOrder.end(), waiter->principal);
        if (turn != turnOrder.end()) turnOrder.erase(turn);
      }

      void settleWaiter(std::shared_ptr<Waiter> waiter, InferenceAdmissionResult result) {
        if (waiter->settled) return;
        waiter->settled = true;
        detachWaiter(waiter);
        (*waiter->resolve)(std::move(result));
      }

      InferenceAdmissionResult admit(double waitedMs) {
        active += 1;
        auto released = std::make_shared<bool>(false);
        std::shared_ptr<Controller> self = shared_from_this();
        return InferenceAdmissionGranted{
          .waitedMs = waitedMs,
          .release =
            [self, released] {
              // A request may be torn down more than once; only the first release
              // returns the permit, so `active` can never drift below the truth.
              if (*released) return;
              *released = true;
              self->active -= 1;
              self->admitWaiting();
            },
        };
      }

      /**
       * Settles a waiter that cannot be admitted now. The principal keeps the turn
       * it was just rotated out of, so its next waiting request is considered
       * immediately instead of losing its place to an unrelated principal.
       */
      void refuseWaiter(const std::shared_ptr<Waiter>& waiter, const Principal& principal, InferenceAdmissionResult result) {
        settleWaiter(waiter, std::move(result));
        if (waitingByPrincipal.contains(principal)) turnOrder.insert(turnOrder.begin(), principal);
      }

      void admitWaiting() {
        while (active < maxActive && waitingCount > 0) {
          if (turnOrder.empty()) break;
          const Principal principal = turnOrder.front();
          turnOrder.erase(turnOrder.begin());
          const auto found = waitingByPrincipal.find(principal);
          if (found == waitingByPrincipal.end() || found->second.empty()) continue;
          const std::shared_ptr<Waiter> waiter = found->second.front();
          const double waitedMs = elapsedMs(waiter->queuedAtMs);
          if (waiter->callerSignal && waiter->callerSignal->aborted()) {
            // The caller's own abort always wins, even over an expired queue wait.
            refuseWaiter(waiter, principal, InferenceAdmissionCallerAborted{.waitedMs = waitedMs});
            continue;
          }
          if (waitedMs >= maxQueueWaitMs) {
            // A blocked event loop delays the queue-wait timer callback, so the
            // grant rechecks the elapsed wait and expires an overdue waiter instead
            // of admitting it past the advertised bound.
            refuseWaiter(waiter, principal, InferenceAdmissionLocalOverload{.cause = "queue_wait_timeout", .waitedMs = waitedMs});
            continue;
          }
          InferenceAdmissionResult granted = admit(waitedMs);
          settleWaiter(waiter, std::move(granted));
          // The principal rotates to the back only while it still has waiting
          // requests; an empty queue is removed by `detachWaiter` instead.
          if (waitingByPrincipal.contains(principal)) turnOrder.push_back(principal);
        }
      }

      void enqueueWaiter(const std::shared_ptr<Waiter>& waiter) {
        const auto found = waitingByPrincipal.find(waiter->principal);
        if (found != waitingByPrincipal.end()) {
          found->second.push_back(waiter);
        } else {
          waitingByPrincipal.emplace(waiter->principal, std::vector<std::shared_ptr<Waiter>>{waiter});
          turnOrder.push_back(waiter->principal);
        }
        waitingCount += 1;
      }

      rt::Promise<InferenceAdmissionResult> acquire(const InferenceAdmissionRequest& request) {
        const Principal principal = request.principal;
        const std::optional<rt::AbortSignal> callerSignal = request.signal;
        if (callerSignal && callerSignal->aborted()) return rt::Promise<InferenceAdmissionResult>::resolve(InferenceAdmissionCallerAborted{.waitedMs = 0});
        if (active < maxActive && waitingCount == 0) return rt::Promise<InferenceAdmissionResult>::resolve(admit(0));
        if (waitingCount >= maxWaiting) {
          return rt::Promise<InferenceAdmissionResult>::resolve(InferenceAdmissionLocalOverload{.cause = "queue_limit", .waitedMs = 0});
        }
        std::shared_ptr<Controller> self = shared_from_this();
        return rt::newPromise<InferenceAdmissionResult>([&](rt::Resolve<InferenceAdmissionResult> resolve, rt::Reject) {
          const double queuedAtMs = rt::performanceNow();
          // One signal covers both ways a wait can end without a permit: the caller
          // went away, or the finite queue wait expired.
          const rt::AbortController deadline;
          std::vector<rt::AbortSignal> sources;
          if (callerSignal) sources.push_back(*callerSignal);
          sources.push_back(deadline.signal());
          const rt::AbortSignal waitSignal = rt::AbortSignal::any(sources);
          auto waiter = std::make_shared<Waiter>();
          waiter->principal = principal;
          waiter->queuedAtMs = queuedAtMs;
          waiter->callerSignal = callerSignal;
          waiter->waitSignal = waitSignal;
          waiter->resolve = resolve;
          const rt::EventListener onWaitAbort([owner = std::weak_ptr(self), queued = std::weak_ptr(waiter)] {
            const auto controller = owner.lock();
            const auto live = queued.lock();
            if (!controller || !live) return;
            const double waitedMs = elapsedMs(live->queuedAtMs);
            if (live->callerSignal && live->callerSignal->aborted()) controller->settleWaiter(live, InferenceAdmissionCallerAborted{.waitedMs = waitedMs});
            else controller->settleWaiter(live, InferenceAdmissionLocalOverload{.cause = "queue_wait_timeout", .waitedMs = waitedMs});
          });
          waiter->onWaitAbort = onWaitAbort;
          auto cleanup = rt::scopeExit([waiter] { waiter->stopWaiting(); });
          waitSignal.addEventListener("abort", onWaitAbort);
          // The loop owns the pending wait until settlement or teardown. Keep
          // the combined signal so abort still runs after caller listeners.
          auto timer = rt::detail::createExactTimer([self, deadline] { (void)self; deadline.abort(); }, maxQueueWaitMs);
          waiter->timeout = timer.unref().handle();
          enqueueWaiter(waiter);
          cleanup.release();
          if (waitSignal.aborted()) onWaitAbort();
        });
      }

      InferenceAdmissionSnapshot snapshot() const {
        return InferenceAdmissionSnapshot{active, waitingCount, static_cast<double>(waitingByPrincipal.size()), maxActive, maxWaiting};
      }
    };

    InferenceAdmissionController& sharedInferenceAdmission() {
      static InferenceAdmissionController controller = createInferenceAdmissionController();
      return controller;
    }
  }

  InferenceAdmissionController createInferenceAdmissionController(PartialInferenceAdmissionLimits limits) {
    const InferenceAdmissionLimits effective{
      limits.maxActive.value_or(DEFAULT_INFERENCE_ADMISSION_LIMITS.maxActive),
      limits.maxWaiting.value_or(DEFAULT_INFERENCE_ADMISSION_LIMITS.maxWaiting),
      limits.maxQueueWaitMs.value_or(DEFAULT_INFERENCE_ADMISSION_LIMITS.maxQueueWaitMs),
    };
    validateLimits(effective);
    auto controller = std::make_shared<Controller>();
    controller->maxActive = effective.maxActive;
    controller->maxWaiting = effective.maxWaiting;
    controller->maxQueueWaitMs = effective.maxQueueWaitMs;
    return InferenceAdmissionController{
      .acquire = [controller](InferenceAdmissionRequest request) { return controller->acquire(request); },
      .snapshot = [controller] { return controller->snapshot(); },
    };
  }

  rt::Promise<InferenceAdmissionResult> acquireInferenceAdmission(InferenceAdmissionRequest request) { return sharedInferenceAdmission().acquire(std::move(request)); }

  InferenceAdmissionSnapshot inferenceAdmissionSnapshot() { return sharedInferenceAdmission().snapshot(); }
}
