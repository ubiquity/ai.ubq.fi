type DenoWithKv = typeof Deno & {
  openKv?: () => Promise<Deno.Kv>;
};

export type KvOperationName = "get" | "getMany" | "set" | "delete" | "list" | "atomic.commit";

export const KV_OPERATION_TIMEOUT_MS = 1_000;
export const KV_CIRCUIT_FAILURE_THRESHOLD = 3;
export const KV_CIRCUIT_DIAGNOSTIC_INTERVAL_MS = 10_000;

const KV_CIRCUIT_OPEN_BASE_MS = 500;
const KV_CIRCUIT_OPEN_MAX_MS = 30_000;

export class KvCircuitOpenError extends Error {
  readonly operation: KvOperationName;
  readonly retryAfterMs: number;

  constructor(operation: KvOperationName, retryAfterMs: number) {
    super("Deno KV is temporarily unavailable");
    this.name = "KvCircuitOpenError";
    this.operation = operation;
    this.retryAfterMs = Math.max(0, Math.trunc(retryAfterMs));
  }
}

export class KvOperationTimeoutError extends Error {
  readonly operation: KvOperationName;
  readonly timeoutMs: number;

  constructor(operation: KvOperationName, timeoutMs: number) {
    super("Deno KV operation timed out");
    this.name = "KvOperationTimeoutError";
    this.operation = operation;
    this.timeoutMs = Math.max(1, Math.trunc(timeoutMs));
  }
}

type KvCircuitState = {
  status: "closed" | "open" | "half_open";
  failureCount: number;
  openUntilMs: number;
  halfOpenProbe: Promise<void> | null;
  resolveHalfOpenProbe: (() => void) | null;
  lastDiagnosticAtMs: number;
  suppressedDiagnostics: number;
};

type KvCircuitPermit = Readonly<{ isHalfOpenProbe: boolean }>;

const newCircuitState = (): KvCircuitState => ({
  status: "closed",
  failureCount: 0,
  openUntilMs: 0,
  halfOpenProbe: null,
  resolveHalfOpenProbe: null,
  lastDiagnosticAtMs: 0,
  suppressedDiagnostics: 0,
});

let circuitStates = new WeakMap<object, KvCircuitState>();
const managedKvTargets = new WeakMap<object, Deno.Kv>();

let openPromise: Promise<Deno.Kv | null> | null = null;
let openedKv: Deno.Kv | null = null;
let openedClient: Deno.Kv | null = null;
let nextOpenAttemptAtMs = 0;
let openFailureCount = 0;

const retryDelayMs = (failureCount: number): number => {
  const capped = Math.min(5_000, 250 * 2 ** Math.min(5, Math.max(0, failureCount - 1)));
  return Math.trunc(capped * (0.75 + Math.random() * 0.5));
};

const circuitOpenDelayMs = (failureCount: number): number => {
  const capped = Math.min(KV_CIRCUIT_OPEN_MAX_MS, KV_CIRCUIT_OPEN_BASE_MS * 2 ** Math.min(6, failureCount - 1));
  return Math.trunc(capped * (0.75 + Math.random() * 0.5));
};

const circuitStateFor = (kv: Deno.Kv): KvCircuitState => {
  const key = kv as unknown as object;
  let state = circuitStates.get(key);
  if (!state) {
    state = newCircuitState();
    circuitStates.set(key, state);
  }
  return state;
};

const rawKvFor = (kv: Deno.Kv): Deno.Kv => managedKvTargets.get(kv as unknown as object) ?? kv;

const withTimeout = async <T>(promise: Promise<T> | PromiseLike<T>, timeoutMs: number, timeout: () => Error): Promise<T> => {
  const boundedTimeoutMs = Math.max(1, Math.trunc(timeoutMs));
  let timer: ReturnType<typeof setTimeout> | undefined;
  try {
    return await Promise.race([
      Promise.resolve(promise),
      new Promise<T>((_, reject) => {
        timer = setTimeout(() => reject(timeout()), boundedTimeoutMs);
      }),
    ]);
  } finally {
    if (timer !== undefined) clearTimeout(timer);
  }
};

const failureClass = (error: unknown): string => {
  if (error instanceof KvOperationTimeoutError) return "timeout";
  const text = error instanceof Error ? error.message.toLowerCase() : String(error).toLowerCase();
  if (text.includes("502") || text.includes("503") || text.includes("overload") || text.includes("upstream")) {
    return "upstream_unavailable";
  }
  if (text.includes("network") || text.includes("connect") || text.includes("socket") || text.includes("eof")) {
    return "transport";
  }
  return "operation_error";
};

const recordDiagnostic = (
  state: KvCircuitState,
  operation: KvOperationName,
  error: unknown,
  circuitTransition: "none" | "open",
  force = false,
): void => {
  const nowMs = Date.now();
  if (
    !force && state.lastDiagnosticAtMs !== 0 &&
    nowMs - state.lastDiagnosticAtMs < KV_CIRCUIT_DIAGNOSTIC_INTERVAL_MS
  ) {
    state.suppressedDiagnostics += 1;
    return;
  }
  const suppressed = state.suppressedDiagnostics;
  state.suppressedDiagnostics = 0;
  state.lastDiagnosticAtMs = nowMs;
  console.warn(
    "[ai.ubq.fi] KV operation health",
    JSON.stringify({
      event: circuitTransition === "open" ? "circuit_open" : "operation_failed",
      operation,
      failure_class: failureClass(error),
      failure_count: state.failureCount,
      suppressed_diagnostics: suppressed,
      retry_after_ms: circuitTransition === "open" ? Math.max(0, state.openUntilMs - nowMs) : undefined,
    }),
  );
};

const closeAfterProbe = (state: KvCircuitState): void => {
  const resolve = state.resolveHalfOpenProbe;
  state.status = "closed";
  state.failureCount = 0;
  state.openUntilMs = 0;
  state.halfOpenProbe = null;
  state.resolveHalfOpenProbe = null;
  if (resolve) resolve();
  if (state.lastDiagnosticAtMs !== 0) {
    console.info(
      "[ai.ubq.fi] KV operation health",
      JSON.stringify({
        event: "circuit_recovered",
        suppressed_diagnostics: state.suppressedDiagnostics,
      }),
    );
    state.lastDiagnosticAtMs = 0;
    state.suppressedDiagnostics = 0;
  }
};

const openAfterFailure = (state: KvCircuitState, operation: KvOperationName, error: unknown): void => {
  state.failureCount += 1;
  if (state.failureCount < KV_CIRCUIT_FAILURE_THRESHOLD) {
    recordDiagnostic(state, operation, error, "none");
    return;
  }
  state.status = "open";
  state.openUntilMs = Date.now() + circuitOpenDelayMs(state.failureCount);
  recordDiagnostic(state, operation, error, "open", true);
};

const failHalfOpenProbe = (state: KvCircuitState, operation: KvOperationName, error: unknown): void => {
  state.failureCount += 1;
  state.status = "open";
  state.openUntilMs = Date.now() + circuitOpenDelayMs(state.failureCount);
  const resolve = state.resolveHalfOpenProbe;
  state.halfOpenProbe = null;
  state.resolveHalfOpenProbe = null;
  recordDiagnostic(state, operation, error, "open", true);
  // Wake waiters so each observes the newly opened interval. Resolving avoids
  // an unhandled rejection when a half-open probe has no waiters.
  if (resolve) resolve();
};

const acquirePermit = async (
  state: KvCircuitState,
  operation: KvOperationName,
  timeoutMs: number,
): Promise<KvCircuitPermit> => {
  for (;;) {
    if (state.status === "closed") return { isHalfOpenProbe: false };
    const nowMs = Date.now();
    if (state.status === "open" && nowMs < state.openUntilMs) {
      throw new KvCircuitOpenError(operation, state.openUntilMs - nowMs);
    }
    if (state.status === "open") {
      let resolve!: () => void;
      const probe = new Promise<void>((resolveProbe) => {
        resolve = resolveProbe;
      });
      state.status = "half_open";
      state.halfOpenProbe = probe;
      state.resolveHalfOpenProbe = resolve;
      return { isHalfOpenProbe: true };
    }
    const probe = state.halfOpenProbe;
    if (!probe) {
      state.status = "open";
      state.openUntilMs = Date.now() + circuitOpenDelayMs(Math.max(1, state.failureCount));
      continue;
    }
    await withTimeout(probe, timeoutMs, () => new KvOperationTimeoutError(operation, timeoutMs));
  }
};

export const withKvOperation = async <T>(
  kv: Deno.Kv,
  operation: KvOperationName,
  work: (rawKv: Deno.Kv) => Promise<T> | T,
  options: Readonly<{ timeoutMs?: number }> = {},
): Promise<T> => {
  const rawKv = rawKvFor(kv);
  const state = circuitStateFor(rawKv);
  const timeoutMs = Math.max(1, Math.trunc(options.timeoutMs ?? KV_OPERATION_TIMEOUT_MS));
  const permit = await acquirePermit(state, operation, timeoutMs);
  try {
    const result = await withTimeout(
      Promise.resolve().then(() => work(rawKv)),
      timeoutMs,
      () => new KvOperationTimeoutError(operation, timeoutMs),
    );
    if (permit.isHalfOpenProbe) closeAfterProbe(state);
    else if (state.status === "closed") state.failureCount = 0;
    return result;
  } catch (error) {
    if (permit.isHalfOpenProbe) failHalfOpenProbe(state, operation, error);
    else if (state.status === "closed") openAfterFailure(state, operation, error);
    throw error;
  }
};

const monitoredList = <T>(
  rawKv: Deno.Kv,
  selector: Deno.KvListSelector,
  options?: Deno.KvListOptions,
): Deno.KvListIterator<T> => {
  let iterator: Deno.KvListIterator<T> | null = null;
  const wrapped = (async function* (): AsyncGenerator<Deno.KvEntry<T>> {
    for (;;) {
      const result = await withKvOperation(
        rawKv,
        "list",
        (target) => {
          iterator ??= target.list<T>(selector, options);
          return iterator.next();
        },
      );
      if (result.done) return;
      yield result.value;
    }
  })() as unknown as Deno.KvListIterator<T>;
  Object.defineProperty(wrapped, "cursor", { get: () => iterator?.cursor ?? "" });
  return wrapped;
};

const managedAtomic = (rawKv: Deno.Kv): Deno.AtomicOperation => {
  const target = rawKv.atomic();
  let wrapped!: Deno.AtomicOperation;
  wrapped = new Proxy(target, {
    get(operation, property, receiver) {
      const value = Reflect.get(operation, property, operation);
      if (property === "commit" && typeof value === "function") {
        return (...args: unknown[]) =>
          withKvOperation(rawKv, "atomic.commit", () => Reflect.apply(value, operation, args));
      }
      if (typeof value !== "function") return value;
      return (...args: unknown[]) => {
        const result = Reflect.apply(value, operation, args);
        return result === operation ? wrapped : result;
      };
    },
  });
  return wrapped;
};

const managedKvFor = (rawKv: Deno.Kv): Deno.Kv => {
  const managed = new Proxy(rawKv, {
    get(target, property, receiver) {
      if (property === "get") {
        return <T>(key: Deno.KvKey, options?: Readonly<{ consistency?: "strong" | "eventual" }>) =>
          withKvOperation(rawKv, "get", (kv) => kv.get<T>(key, options));
      }
      if (property === "getMany") {
        return <T extends readonly unknown[]>(
          keys: readonly Deno.KvKey[],
          options?: Readonly<{ consistency?: "strong" | "eventual" }>,
        ) => withKvOperation(
          rawKv,
          "getMany",
          (kv) => kv.getMany<T>(keys as unknown as [...{ [K in keyof T]: Deno.KvKey }], options),
        );
      }
      if (property === "set") {
        return (key: Deno.KvKey, value: unknown, options?: Readonly<{ expireIn?: number }>) =>
          withKvOperation(rawKv, "set", (kv) => kv.set(key, value, options));
      }
      if (property === "delete") {
        return (key: Deno.KvKey) => withKvOperation(rawKv, "delete", (kv) => kv.delete(key));
      }
      if (property === "list") {
        return <T>(selector: Deno.KvListSelector, options?: Deno.KvListOptions) =>
          monitoredList<T>(rawKv, selector, options);
      }
      if (property === "atomic") return () => managedAtomic(rawKv);
      const value = Reflect.get(target, property, target);
      return typeof value === "function" ? value.bind(target) : value;
    },
  });
  managedKvTargets.set(managed as unknown as object, rawKv);
  return managed;
};

const openKv = async (): Promise<Deno.Kv | null> => {
  const denoOpenKv = (Deno as DenoWithKv).openKv;
  if (typeof denoOpenKv !== "function") return null;
  try {
    const kv = await denoOpenKv();
    openedKv = kv;
    openedClient = managedKvFor(kv);
    openFailureCount = 0;
    nextOpenAttemptAtMs = 0;
    return openedClient;
  } catch (error) {
    openFailureCount += 1;
    nextOpenAttemptAtMs = Date.now() + retryDelayMs(openFailureCount);
    console.error(
      "[ai.ubq.fi] Failed to open Deno KV; a later request will retry:",
      JSON.stringify({ event: "kv_open_failed", failure_class: failureClass(error) }),
    );
    return null;
  }
};

export const getKv = (): Promise<Deno.Kv | null> => {
  if (openedKv) return Promise.resolve(openedClient ?? managedKvFor(openedKv));
  const denoOpenKv = (Deno as DenoWithKv).openKv;
  if (typeof denoOpenKv !== "function") return Promise.resolve(null);
  if (Date.now() < nextOpenAttemptAtMs) return Promise.resolve(null);
  openPromise ??= openKv().finally(() => {
    openPromise = null;
  });
  return openPromise;
};

export const resetKvCircuitForTest = (): void => {
  circuitStates = new WeakMap<object, KvCircuitState>();
};

export const setKvForTest = (kv: Deno.Kv | null): void => {
  openedKv = kv;
  openedClient = kv ? managedKvFor(kv) : null;
  openPromise = null;
  openFailureCount = 0;
  nextOpenAttemptAtMs = 0;
  resetKvCircuitForTest();
};
