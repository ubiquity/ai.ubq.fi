#include "uos/inference_admission.hpp"

#include "uos/rt/clock.hpp"
#include "uos/rt/error.hpp"
#include "uos/rt/number.hpp"

#include <algorithm>
#include <limits>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

namespace uos::inference_admission {
  namespace {
    using Principal = std::optional<std::string>;
    constexpr std::uint64_t memoryPerStream = 1ULL << 20;
    constexpr std::uint64_t descriptorsPerStream = 2;
    constexpr double probeIntervalMs = 100;
    constexpr double pauseLagMs = 250;
    constexpr double recoverLagMs = 100;

    std::uint64_t remaining(std::uint64_t budget, std::uint64_t used) { return budget - std::min(budget, used); }

    std::uint64_t streamMemory(std::uint64_t bodyBytes) {
      constexpr std::uint64_t max = std::numeric_limits<std::uint64_t>::max();
      return bodyBytes > max / 4 ? max : std::max(memoryPerStream, bodyBytes * 4);
    }

    struct AdaptiveCapacity {
      AdmissionResourceSampler sample;
      rt::admission_resources::Headroom headroom{};
      double sampleAgainAt = -1;
      std::uint64_t activeMemory = 0;
      std::uint64_t waitingMemory = 0;
      std::uint64_t descriptorCommitment = 0;
      std::weak_ptr<rt::detail::TimerState> probe;
      double probeDueAt = 0;
      double eventLoopLagMs = 0;
      unsigned healthyProbes = 0;
      bool lagPaused = false;

      explicit AdaptiveCapacity(AdmissionResourceSampler sampler) : sample(std::move(sampler)) {}

      void refresh(double now, std::uint64_t active) {
        if (now < sampleAgainAt) return;
        headroom = sample();
        sampleAgainAt = now + probeIntervalMs;
        // The sample includes already-open client and upstream descriptors.
        // Allow one future upstream descriptor for each already-admitted request.
        descriptorCommitment = active;
      }

      void observeLag(double lateness) {
        eventLoopLagMs = lateness;
        if (lateness >= pauseLagMs) {
          healthyProbes = 0;
          lagPaused = true;
        } else if (lagPaused) {
          healthyProbes = lateness <= recoverLagMs ? healthyProbes + 1 : 0;
          if (healthyProbes >= 2) lagPaused = false;
        }
      }

      bool canAdmit(std::uint64_t bodyBytes, bool queued) const {
        const auto otherWaiting = waitingMemory - (queued ? bodyBytes : 0);
        return !lagPaused && headroom.memoryBytes && headroom.descriptors &&
               streamMemory(bodyBytes) <= remaining(remaining(*headroom.memoryBytes, otherWaiting), activeMemory) &&
               descriptorsPerStream <= remaining(*headroom.descriptors, descriptorCommitment);
      }

      bool canQueue(std::uint64_t bodyBytes) const {
        if (bodyBytes > remaining(std::numeric_limits<std::uint64_t>::max(), waitingMemory)) return false;
        const auto weight = streamMemory(bodyBytes);
        // Allow up to 32 MiB of ordinary request bodies across the bounded queue
        // even under pressure. Larger bodies require room in the sampled budget.
        return weight == memoryPerStream ||
               (headroom.memoryBytes && weight <= *headroom.memoryBytes &&
                bodyBytes <= remaining(remaining(*headroom.memoryBytes, activeMemory), waitingMemory));
      }

      std::uint64_t descriptorCapacity(std::uint64_t active) const {
        const auto descriptors = *headroom.descriptors;
        if (descriptors >= descriptorCommitment) return active + (descriptors - descriptorCommitment) / descriptorsPerStream;
        const auto deficit = descriptorCommitment - descriptors;
        return remaining(active, deficit / descriptorsPerStream + (deficit % descriptorsPerStream != 0));
      }

      void describe(InferenceAdmissionSnapshot& snapshot, std::uint64_t active) const {
        snapshot.memoryHeadroomBytes = headroom.memoryBytes;
        snapshot.descriptorHeadroom = headroom.descriptors;
        snapshot.eventLoopLagMs = eventLoopLagMs;
        snapshot.maxActive = 0;
        if (lagPaused) snapshot.limitingResource = "event_loop_lag";
        else if (!headroom.memoryBytes) snapshot.limitingResource = "memory_unavailable";
        else if (!headroom.descriptors) snapshot.limitingResource = "descriptors_unavailable";
        else {
          const auto extraMemory = activeMemory - active * memoryPerStream;
          const auto memoryCapacity = remaining(remaining(*headroom.memoryBytes, extraMemory), waitingMemory) / memoryPerStream;
          const auto fdCapacity = descriptorCapacity(active);
          snapshot.maxActive = static_cast<double>(std::min(memoryCapacity, fdCapacity));
          snapshot.limitingResource = memoryCapacity <= fdCapacity ? "memory" : "descriptors";
        }
      }
    };

    struct Waiter {
      Principal principal;
      std::uint64_t bodyBytes = 0;
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

      std::uint64_t active = 0;
      double waitingCount = 0;
      std::optional<AdaptiveCapacity> adaptive;
      // One arrival-ordered queue per principal, and the rotation of principals
      // whose turn it is to place one request into an active slot.  A principal
      // owns a queue and a turn only while it has waiting requests, so a burst that
      // drains leaves no per-principal state behind.
      std::unordered_map<Principal, std::vector<std::shared_ptr<Waiter>>> waitingByPrincipal;
      std::vector<Principal> turnOrder;

      void stopProbe() {
        if (!adaptive) return;
        rt::clearInterval(rt::Timeout(adaptive->probe.lock()));
        adaptive->probe.reset();
      }

      void armProbe() {
        if (!adaptive || rt::Timeout(adaptive->probe.lock()).active()) return;
        adaptive->probeDueAt = rt::performanceNow() + probeIntervalMs;
        auto timer = rt::detail::createExactTimer([owner = weak_from_this()] {
          if (auto self = owner.lock()) {
            auto& capacity = *self->adaptive;
            capacity.probe.reset();
            capacity.observeLag(std::max(0.0, rt::performanceNow() - capacity.probeDueAt));
            self->admitWaiting();
          }
        }, probeIntervalMs);
        adaptive->probe = timer.unref().handle();
      }

      bool canAdmit(std::uint64_t bodyBytes, bool queued = false) {
        if (!adaptive) return static_cast<double>(active) < maxActive;
        const auto now = rt::performanceNow();
        // An arrival can run before an overdue timer callback. Apply its lag
        // before granting so a busy loop cannot dispatch through the pause.
        if (rt::Timeout(adaptive->probe.lock()).active() && now - adaptive->probeDueAt >= pauseLagMs)
          adaptive->observeLag(now - adaptive->probeDueAt);
        adaptive->refresh(now, active);
        return adaptive->canAdmit(bodyBytes, queued);
      }

      void detachWaiter(const std::shared_ptr<Waiter>& waiter) {
        waiter->stopWaiting();
        const auto found = waitingByPrincipal.find(waiter->principal);
        if (found == waitingByPrincipal.end()) return;
        std::vector<std::shared_ptr<Waiter>>& queue = found->second;
        const auto index = std::find(queue.begin(), queue.end(), waiter);
        if (index == queue.end()) return;
        queue.erase(index);
        waitingCount -= 1;
        if (adaptive) adaptive->waitingMemory -= waiter->bodyBytes;
        if (waitingCount == 0) stopProbe();
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

      InferenceAdmissionResult admit(double waitedMs, std::uint64_t bodyBytes) {
        active += 1;
        const auto weight = adaptive ? streamMemory(bodyBytes) : 0;
        if (adaptive) {
          adaptive->activeMemory += weight;
          // Debit grants since the sample even if a permit ends. A downstream
          // or pooled upstream socket may still be open until the next sample.
          adaptive->descriptorCommitment += descriptorsPerStream;
        }
        auto released = std::make_shared<bool>(false);
        std::shared_ptr<Controller> self = shared_from_this();
        return InferenceAdmissionGranted{
          .waitedMs = waitedMs,
          .release =
            [self, released, weight] {
              // A request may be torn down more than once; only the first release
              // returns the permit, so `active` can never drift below the truth.
              if (*released) return;
              *released = true;
              self->active -= 1;
              if (self->adaptive) self->adaptive->activeMemory -= weight;
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
        while (waitingCount > 0) {
          if (turnOrder.empty()) break;
          const Principal principal = turnOrder.front();
          const auto found = waitingByPrincipal.find(principal);
          if (found == waitingByPrincipal.end() || found->second.empty()) {
            turnOrder.erase(turnOrder.begin());
            continue;
          }
          const std::shared_ptr<Waiter> waiter = found->second.front();
          const bool available = canAdmit(waiter->bodyBytes, true);
          const double waitedMs = elapsedMs(waiter->queuedAtMs);
          if (waiter->callerSignal && waiter->callerSignal->aborted()) {
            // The caller's own abort always wins, even over an expired queue wait.
            turnOrder.erase(turnOrder.begin());
            refuseWaiter(waiter, principal, InferenceAdmissionCallerAborted{.waitedMs = waitedMs});
            continue;
          }
          if (waitedMs >= maxQueueWaitMs) {
            // A blocked event loop delays the queue-wait timer callback, so the
            // grant rechecks the elapsed wait and expires an overdue waiter instead
            // of admitting it past the advertised bound.
            turnOrder.erase(turnOrder.begin());
            refuseWaiter(waiter, principal, InferenceAdmissionLocalOverload{.cause = "queue_wait_timeout", .waitedMs = waitedMs});
            continue;
          }
          if (!available) break;
          turnOrder.erase(turnOrder.begin());
          InferenceAdmissionResult granted = admit(waitedMs, waiter->bodyBytes);
          settleWaiter(waiter, std::move(granted));
          // The principal rotates to the back only while it still has waiting
          // requests; an empty queue is removed by `detachWaiter` instead.
          if (waitingByPrincipal.contains(principal)) turnOrder.push_back(principal);
        }
        if (waitingCount > 0) armProbe();
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
        if (adaptive) adaptive->waitingMemory += waiter->bodyBytes;
      }

      rt::Promise<InferenceAdmissionResult> acquire(const InferenceAdmissionRequest& request) {
        const Principal principal = request.principal;
        const std::optional<rt::AbortSignal> callerSignal = request.signal;
        if (callerSignal && callerSignal->aborted()) return rt::Promise<InferenceAdmissionResult>::resolve(InferenceAdmissionCallerAborted{.waitedMs = 0});
        if (adaptive && waitingCount > 0) admitWaiting();
        armProbe();
        if (waitingCount == 0 && canAdmit(request.bodyBytes))
          return rt::Promise<InferenceAdmissionResult>::resolve(admit(0, request.bodyBytes));
        if (waitingCount >= maxWaiting) {
          return rt::Promise<InferenceAdmissionResult>::resolve(InferenceAdmissionLocalOverload{.cause = "queue_limit", .waitedMs = 0});
        }
        if (adaptive && !adaptive->canQueue(request.bodyBytes)) {
          return rt::Promise<InferenceAdmissionResult>::resolve(InferenceAdmissionLocalOverload{.cause = "queue_memory", .waitedMs = 0});
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
          waiter->bodyBytes = request.bodyBytes;
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
        InferenceAdmissionSnapshot result{
          .active = static_cast<double>(active),
          .waiting = waitingCount,
          .principals = static_cast<double>(waitingByPrincipal.size()),
          .maxActive = maxActive,
          .maxWaiting = maxWaiting,
          .limitingResource = "fixed",
          .memoryHeadroomBytes = {},
          .descriptorHeadroom = {},
        };
        if (adaptive) adaptive->describe(result, active);
        return result;
      }
    };

    InferenceAdmissionController wrap(std::shared_ptr<Controller> controller) {
      return InferenceAdmissionController{
        .acquire = [controller](InferenceAdmissionRequest request) { return controller->acquire(request); },
        .snapshot = [controller] { return controller->snapshot(); },
      };
    }

    InferenceAdmissionController& sharedInferenceAdmission() {
      static InferenceAdmissionController controller = createAdaptiveInferenceAdmissionController();
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
    return wrap(std::move(controller));
  }

  InferenceAdmissionController createAdaptiveInferenceAdmissionController(AdmissionResourceSampler sample) {
    auto controller = std::make_shared<Controller>();
    controller->maxWaiting = DEFAULT_INFERENCE_ADMISSION_LIMITS.maxWaiting;
    controller->maxQueueWaitMs = DEFAULT_INFERENCE_ADMISSION_LIMITS.maxQueueWaitMs;
    controller->adaptive.emplace(std::move(sample));
    return wrap(std::move(controller));
  }

  rt::Promise<InferenceAdmissionResult> acquireInferenceAdmission(InferenceAdmissionRequest request) { return sharedInferenceAdmission().acquire(std::move(request)); }

  InferenceAdmissionSnapshot inferenceAdmissionSnapshot() { return sharedInferenceAdmission().snapshot(); }
}
