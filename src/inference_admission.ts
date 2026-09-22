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
export type InferenceAdmissionLimits = Readonly<{
  /** Maximum terminal inference requests holding a permit at once. */
  maxActive: number;
  /** Maximum requests parked waiting for a permit before new arrivals are rejected. */
  maxWaiting: number;
  /** Maximum time one request may wait for a permit before it is rejected. */
  maxQueueWaitMs: number;
}>;

export const DEFAULT_INFERENCE_ADMISSION_LIMITS: InferenceAdmissionLimits = Object.freeze({
  maxActive: 64,
  maxWaiting: 128,
  maxQueueWaitMs: 5_000,
});

export type InferenceAdmissionSnapshot = Readonly<{
  active: number;
  waiting: number;
  /** Distinct principals with waiting requests; zero while the queue is empty. */
  principals: number;
  maxActive: number;
  maxWaiting: number;
}>;

/** Why the local finite guard rejected a request instead of admitting it. */
export type InferenceAdmissionLocalOverloadCause = "queue_limit" | "queue_wait_timeout";

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
export type InferenceAdmissionResult =
  | Readonly<{ ok: true; waitedMs: number; release: () => void }>
  | Readonly<{ ok: false; kind: "local_overload"; cause: InferenceAdmissionLocalOverloadCause; waitedMs: number }>
  | Readonly<{ ok: false; kind: "caller_aborted"; waitedMs: number }>;

export type InferenceAdmissionRequest = Readonly<{
  /** Cancels a waiting request; the caller's own abort always wins over an admission. */
  signal?: AbortSignal;
  /**
   * Stable authenticated caller identity, supplied by the handler that already
   * resolved it.  It only decides waiting turns: one principal's requests are
   * served in arrival order and turns rotate across principals.  It never caps
   * how many requests a principal may hold active, and omitting it keeps every
   * principal-less caller in one arrival-ordered group.
   */
  principal?: string;
}>;

export type InferenceAdmissionController = Readonly<{
  acquire: (request?: InferenceAdmissionRequest) => Promise<InferenceAdmissionResult>;
  snapshot: () => InferenceAdmissionSnapshot;
}>;

type Waiter = {
  principal: string | null;
  queuedAtMs: number;
  callerSignal: AbortSignal | undefined;
  waitSignal: AbortSignal | null;
  onWaitAbort: (() => void) | null;
  settled: boolean;
  resolve: (result: InferenceAdmissionResult) => void;
};

const elapsedMs = (since: number): number => Math.max(0, Math.round(performance.now() - since));

const validateLimits = (limits: InferenceAdmissionLimits): void => {
  if (!Number.isSafeInteger(limits.maxActive) || limits.maxActive < 1) {
    throw new RangeError("Inference admission maxActive must be a positive safe integer.");
  }
  if (!Number.isSafeInteger(limits.maxWaiting) || limits.maxWaiting < 0) {
    throw new RangeError("Inference admission maxWaiting must be a non-negative safe integer.");
  }
  if (!Number.isSafeInteger(limits.maxQueueWaitMs) || limits.maxQueueWaitMs < 0) {
    throw new RangeError("Inference admission maxQueueWaitMs must be a non-negative safe integer.");
  }
};

/**
 * Build one admission controller.  Production uses the shared controller below;
 * tests construct a controller with explicit limits so waiting behavior is
 * deterministic.
 */
export const createInferenceAdmissionController = (limits: Partial<InferenceAdmissionLimits> = {}): InferenceAdmissionController => {
  const effective: InferenceAdmissionLimits = {
    maxActive: limits.maxActive ?? DEFAULT_INFERENCE_ADMISSION_LIMITS.maxActive,
    maxWaiting: limits.maxWaiting ?? DEFAULT_INFERENCE_ADMISSION_LIMITS.maxWaiting,
    maxQueueWaitMs: limits.maxQueueWaitMs ?? DEFAULT_INFERENCE_ADMISSION_LIMITS.maxQueueWaitMs,
  };
  validateLimits(effective);
  const { maxActive, maxWaiting, maxQueueWaitMs } = effective;

  let active = 0;
  let waitingCount = 0;
  // One arrival-ordered queue per principal, and the rotation of principals
  // whose turn it is to place one request into an active slot.  A principal
  // owns a queue and a turn only while it has waiting requests, so a burst that
  // drains leaves no per-principal state behind.
  const waitingByPrincipal = new Map<string | null, Waiter[]>();
  const turnOrder: Array<string | null> = [];

  const detachWaiter = (waiter: Waiter): void => {
    if (waiter.waitSignal && waiter.onWaitAbort) waiter.waitSignal.removeEventListener("abort", waiter.onWaitAbort);
    waiter.waitSignal = null;
    waiter.onWaitAbort = null;
    const queue = waitingByPrincipal.get(waiter.principal);
    if (!queue) return;
    const index = queue.indexOf(waiter);
    if (index < 0) return;
    queue.splice(index, 1);
    waitingCount -= 1;
    if (queue.length > 0) return;
    // An empty principal must not keep a waiting turn or a map entry alive, or
    // cancelled and expired waiters would grow the guard without bound.
    waitingByPrincipal.delete(waiter.principal);
    const turn = turnOrder.indexOf(waiter.principal);
    if (turn >= 0) turnOrder.splice(turn, 1);
  };

  const settleWaiter = (waiter: Waiter, result: InferenceAdmissionResult): void => {
    if (waiter.settled) return;
    waiter.settled = true;
    detachWaiter(waiter);
    waiter.resolve(result);
  };

  const admit = (waitedMs: number): InferenceAdmissionResult => {
    active += 1;
    let released = false;
    return {
      ok: true,
      waitedMs,
      release: () => {
        // A request may be torn down more than once; only the first release
        // returns the permit, so `active` can never drift below the truth.
        if (released) return;
        released = true;
        active -= 1;
        admitWaiting();
      },
    };
  };

  const admitWaiting = (): void => {
    while (active < maxActive && waitingCount > 0) {
      const principal = turnOrder.shift();
      if (principal === undefined) break;
      const waiter = waitingByPrincipal.get(principal)?.[0];
      if (!waiter) continue;
      const waitedMs = elapsedMs(waiter.queuedAtMs);
      if (waiter.callerSignal?.aborted) {
        // The caller's own abort always wins, even over an expired queue wait,
        // and it does not consume the turn owed to the principal it belonged to.
        settleWaiter(waiter, { ok: false, kind: "caller_aborted", waitedMs });
        if (waitingByPrincipal.has(principal)) turnOrder.unshift(principal);
        continue;
      }
      if (waitedMs >= maxQueueWaitMs) {
        // A blocked event loop delays the queue-wait timer callback, so the
        // grant rechecks the elapsed wait and expires an overdue waiter instead
        // of admitting it past the advertised bound.  The turn stays with the
        // principal, whose next waiter is considered immediately.
        settleWaiter(waiter, { ok: false, kind: "local_overload", cause: "queue_wait_timeout", waitedMs });
        if (waitingByPrincipal.has(principal)) turnOrder.unshift(principal);
        continue;
      }
      settleWaiter(waiter, admit(waitedMs));
      // The principal rotates to the back only while it still has waiting
      // requests; an empty queue is removed by `detachWaiter` instead.
      if (waitingByPrincipal.has(principal)) turnOrder.push(principal);
    }
  };

  const enqueueWaiter = (waiter: Waiter): void => {
    const queue = waitingByPrincipal.get(waiter.principal);
    if (queue) {
      queue.push(waiter);
    } else {
      waitingByPrincipal.set(waiter.principal, [waiter]);
      turnOrder.push(waiter.principal);
    }
    waitingCount += 1;
  };

  const acquire = (request: InferenceAdmissionRequest = {}): Promise<InferenceAdmissionResult> => {
    const principal = request.principal ?? null;
    const callerSignal = request.signal;
    if (callerSignal?.aborted) return Promise.resolve({ ok: false, kind: "caller_aborted", waitedMs: 0 });
    if (active < maxActive && waitingCount === 0) return Promise.resolve(admit(0));
    if (waitingCount >= maxWaiting) {
      return Promise.resolve({ ok: false, kind: "local_overload", cause: "queue_limit", waitedMs: 0 });
    }
    return new Promise<InferenceAdmissionResult>((resolve) => {
      const queuedAtMs = performance.now();
      // One signal covers both ways a wait can end without a permit: the caller
      // went away, or the finite queue wait expired.
      const waitSignal = AbortSignal.any(callerSignal ? [callerSignal, AbortSignal.timeout(maxQueueWaitMs)] : [AbortSignal.timeout(maxQueueWaitMs)]);
      const waiter: Waiter = { principal, queuedAtMs, callerSignal, waitSignal, onWaitAbort: null, settled: false, resolve };
      const onWaitAbort = (): void => {
        settleWaiter(
          waiter,
          callerSignal?.aborted
            ? { ok: false, kind: "caller_aborted", waitedMs: elapsedMs(queuedAtMs) }
            : { ok: false, kind: "local_overload", cause: "queue_wait_timeout", waitedMs: elapsedMs(queuedAtMs) }
        );
      };
      waiter.onWaitAbort = onWaitAbort;
      waitSignal.addEventListener("abort", onWaitAbort);
      enqueueWaiter(waiter);
      if (waitSignal.aborted) onWaitAbort();
    });
  };

  const snapshot = (): InferenceAdmissionSnapshot => ({ active, waiting: waitingCount, principals: waitingByPrincipal.size, maxActive, maxWaiting });

  return { acquire, snapshot };
};

const sharedInferenceAdmission = createInferenceAdmissionController();

/** Acquire one permit from the shared process-wide admission guard. */
export const acquireInferenceAdmission = (request?: InferenceAdmissionRequest): Promise<InferenceAdmissionResult> => sharedInferenceAdmission.acquire(request);

/** Current shared-guard occupancy, for integration telemetry and tests. */
export const inferenceAdmissionSnapshot = (): InferenceAdmissionSnapshot => sharedInferenceAdmission.snapshot();
