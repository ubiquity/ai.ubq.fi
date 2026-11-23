#pragma once
// Port of src/inference-admission.ts.
/**
 * Finite in-process admission for terminal inference routes.
 *
 * The guard bounds how many terminal inference requests may hold a permit at
 * once and how many may wait for one, so sustained overload produces a single
 * distinct local decision instead of unbounded queueing.  Waiting turns rotate
 * across principals and requests from one principal are served in arrival
 * order, so a sustained burst from one caller cannot consume every waiting turn
 * while an unrelated caller waits.  Requests that supply no principal share one
 * arrival-ordered group.
 *
 * It is deliberately not caller-lane admission: it derives no caller lane,
 * holds no per-caller lease, caps no principal's active permits, and never
 * rejects a request because another request already uses the same credential.
 * The waiting bound is global, so true saturation still rejects a new arrival
 * with `local_overload` instead of growing an unbounded queue.
 *
 * Limits are fixed at construction; there is no environment, CLI, or
 * request-level knob.
 */
#include "uos/rt/abort.hpp"
#include "uos/rt/async.hpp"

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <variant>

namespace uos::inference_admission {
  struct InferenceAdmissionLimits {
    /** Maximum terminal inference requests holding a permit at once. */
    double maxActive = 0;
    /** Maximum requests parked waiting for a permit before new arrivals are rejected. */
    double maxWaiting = 0;
    /** Maximum time one request may wait for a permit before it is rejected. */
    double maxQueueWaitMs = 0;
  };

  inline constexpr InferenceAdmissionLimits DEFAULT_INFERENCE_ADMISSION_LIMITS{64, 128, 5'000};

  struct InferenceAdmissionSnapshot {
    double active = 0;
    double waiting = 0;
    /** Distinct principals with waiting requests; zero while the queue is empty. */
    double principals = 0;
    double maxActive = 0;
    double maxWaiting = 0;
  };

  /** Why the local finite guard rejected a request instead of admitting it. */
  using InferenceAdmissionLocalOverloadCause = std::string;  // "queue_limit" | "queue_wait_timeout"

  /**
   * One admission attempt.  `local_overload` is this guard's own finite-overload
   * disposition: it is not upstream quota, capacity, or a transport outcome, and
   * it must never advance the provider waterfall or reach the paid-fallback
   * ledger.
   *
   * `waitedMs` is the time the attempt spent queued, so the integration can make
   * the wait visible instead of hiding it.  An admitted attempt keeps its permit
   * until `release` is called; a caller that aborts after admission still owns the
   * permit and must release it during its own request teardown.
   */
  // `{ ok: true; waitedMs; release }`
  struct InferenceAdmissionGranted {
    static constexpr bool ok = true;
    double waitedMs = 0;
    std::function<void()> release = {};
  };
  // `{ ok: false; kind: "local_overload"; cause; waitedMs }`
  struct InferenceAdmissionLocalOverload {
    static constexpr bool ok = false;
    static constexpr std::string_view kind = "local_overload";
    InferenceAdmissionLocalOverloadCause cause = {};
    double waitedMs = 0;
  };
  // `{ ok: false; kind: "caller_aborted"; waitedMs }`
  struct InferenceAdmissionCallerAborted {
    static constexpr bool ok = false;
    static constexpr std::string_view kind = "caller_aborted";
    double waitedMs = 0;
  };
  using InferenceAdmissionResult = std::variant<InferenceAdmissionGranted, InferenceAdmissionLocalOverload, InferenceAdmissionCallerAborted>;

  struct InferenceAdmissionRequest {
    /** Cancels a waiting request; the caller's own abort always wins over an admission. */
    std::optional<rt::AbortSignal> signal = std::nullopt;
    /**
     * Stable authenticated caller identity, supplied by the handler that already
     * resolved it.  It only decides waiting turns: one principal's requests are
     * served in arrival order and turns rotate across principals.  It never caps
     * how many requests a principal may hold active, and omitting it keeps every
     * principal-less caller in one arrival-ordered group.
     */
    std::optional<std::string> principal = std::nullopt;
  };

  // `acquire(request?)`: an omitted request is `{}`.
  struct InferenceAdmissionController {
    std::function<rt::Promise<InferenceAdmissionResult>(InferenceAdmissionRequest)> acquire = {};
    std::function<InferenceAdmissionSnapshot()> snapshot = {};
  };

  // `Partial<InferenceAdmissionLimits>`
  struct PartialInferenceAdmissionLimits {
    std::optional<double> maxActive = std::nullopt;
    std::optional<double> maxWaiting = std::nullopt;
    std::optional<double> maxQueueWaitMs = std::nullopt;
  };

  /**
   * Build one admission controller.  Production uses the shared controller below;
   * tests construct a controller with explicit limits so waiting behavior is
   * deterministic.
   */
  // Throws rt::RangeError for limits that cannot bound anything.
  InferenceAdmissionController createInferenceAdmissionController(PartialInferenceAdmissionLimits limits = {});

  /** Acquire one permit from the shared process-wide admission guard. */
  rt::Promise<InferenceAdmissionResult> acquireInferenceAdmission(InferenceAdmissionRequest request = {});

  /** Current shared-guard occupancy, for integration telemetry and tests. */
  InferenceAdmissionSnapshot inferenceAdmissionSnapshot();
}
