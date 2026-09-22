/**
 * A small bounded, best-effort queue for optional telemetry writes.
 *
 * Optional telemetry must never decide client-visible latency or response
 * truthfulness, and it must never grow without a ceiling when its sink is slow
 * or unavailable. This primitive therefore owns five internal bounds - retained
 * entries, retained payload bytes, entry age, concurrent writer calls and the
 * time a drain waits for progress - and reports every drop, failure, timeout,
 * coalesced flush and drain in its snapshot.
 *
 * It is deliberately not a general job framework: it has one writer, one FIFO
 * of sanitized entries, and no retry policy. Required durable evidence (quota
 * accounting, admin errors, Sentinel replay capture) is never routed here.
 */

export type OptionalTelemetryQueueBounds = Readonly<{
  /** Maximum charged entries, including unresolved in-flight writes. */
  maxEntries: number;
  /** Maximum charged UTF-8 payload bytes, including unresolved in-flight writes. */
  maxBytes: number;
  /** Maximum age of a queued entry before it is dropped instead of written. */
  maxAgeMs: number;
  /** Maximum writer calls in flight at once. */
  maxConcurrentWrites: number;
  /**
   * Absolute wait bound for one drain operation. Progress does not reset it: a
   * drain that runs out of it ends instead of hanging, stops dispatching, keeps
   * the unresolved writes charged, and reports the timeout, so `flush` and
   * `close` always settle.
   */
  maxDrainWaitMs: number;
}>;

/**
 * Conservative internal limits. They are not operator knobs: there is no
 * environment variable, CLI flag or configuration surface for them.
 */
export const OPTIONAL_TELEMETRY_QUEUE_BOUNDS: OptionalTelemetryQueueBounds = Object.freeze({
  maxEntries: 256,
  maxBytes: 256 * 1024,
  maxAgeMs: 60_000,
  maxConcurrentWrites: 4,
  maxDrainWaitMs: 5_000,
});

export type OptionalTelemetryEnqueueOutcome = "enqueued" | "dropped_closed" | "dropped_capacity" | "dropped_bytes";

export type OptionalTelemetryQueueSnapshot = Readonly<{
  bounds: OptionalTelemetryQueueBounds;
  /** Charged entries, including unresolved in-flight writes. */
  retained_entries: number;
  /** Charged UTF-8 payload bytes, including unresolved in-flight writes. */
  retained_bytes: number;
  /** Charged entries waiting to be dispatched. */
  queued_entries: number;
  /** Age of the oldest charged entry, including unresolved in-flight writes. */
  oldest_retained_age_ms: number | null;
  writes_in_flight: number;
  drain_in_progress: boolean;
  /** Drains that ended because the operation ran out of its wait bound. */
  drain_timeouts: number;
  /** True while stuck in-flight writes retain capacity and new dispatch is stopped. */
  dispatch_stalled: boolean;
  /**
   * close() ended with unresolved retained work (a timeout, or work already
   * withheld by a stall). The queue never dispatches again; the unresolved
   * charges stay reported instead of being silently written or forgotten.
   */
  shutdown_incomplete: boolean;
  coalesced_flushes: number;
  closed: boolean;
  enqueued: number;
  delivered: number;
  failed: number;
  dropped_by_entries: number;
  dropped_by_bytes: number;
  dropped_by_age: number;
  dropped_after_closed: number;
  /** Error class name only; never a message, key or payload. */
  last_error_class: string | null;
}>;

/** Resolves true when the entry was recorded durably, false when it was not. */
export type OptionalTelemetryWriter<TEntry> = (entry: TEntry) => Promise<boolean>;

export type OptionalTelemetryQueue<TEntry> = Readonly<{
  enqueue: (entry: TEntry) => OptionalTelemetryEnqueueOutcome;
  /**
   * Starts or joins one bounded drain operation. The wait bound is absolute for
   * that operation: progress does not reset it. Always settles within
   * `maxDrainWaitMs`, reporting `drain_timeouts`/`dispatch_stalled` instead of
   * hanging; a later explicit flush may dispatch once capacity is free again.
   */
  flush: () => Promise<void>;
  /**
   * Stops accepting entries, then performs one bounded drain operation. If
   * unresolved work remains, the queue reports `shutdown_incomplete` and never
   * dispatches again, so shutdown can neither hang nor write after giving up.
   */
  close: () => Promise<void>;
  snapshot: () => OptionalTelemetryQueueSnapshot;
}>;

export type OptionalTelemetryQueueOptions<TEntry> = Readonly<{
  write: OptionalTelemetryWriter<TEntry>;
  bounds?: Partial<OptionalTelemetryQueueBounds>;
  now?: () => number;
  /**
   * Charged UTF-8 payload bytes of one entry. The default measures the JSON
   * payload, which excludes any process-local delivery handle the caller stores
   * on the entry; bounded metadata is what the byte ceiling is meant to cap. A
   * non-finite or negative result fails closed: the entry is dropped, never
   * charged as free.
   */
  measure?: (entry: TEntry) => number;
}>;

const boundedInteger = (value: number | undefined, fallback: number): number =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0 ? value : fallback;

const UTF8_ENCODER = new TextEncoder();

/**
 * Measures one JSON payload in real UTF-8 bytes. Returns `NaN` when the payload
 * cannot be serialized faithfully (a cycle, a BigInt, or a non-finite number
 * that JSON would silently rewrite as `null`), so the caller drops instead of
 * charging a wrong size.
 */
export const measureJsonPayloadBytes = (value: unknown): number => {
  try {
    const serialized = JSON.stringify(value, (_key, item: unknown) => {
      if (typeof item === "number" && !Number.isFinite(item)) throw new TypeError("non-finite number");
      if (typeof item === "bigint") throw new TypeError("bigint");
      return item;
    });
    return serialized === undefined ? Number.NaN : UTF8_ENCODER.encode(serialized).length;
  } catch {
    return Number.NaN;
  }
};

const defaultMeasure = <TEntry>(entry: TEntry): number => measureJsonPayloadBytes(entry);

export const createOptionalTelemetryQueue = <TEntry>(options: OptionalTelemetryQueueOptions<TEntry>): OptionalTelemetryQueue<TEntry> => {
  const bounds: OptionalTelemetryQueueBounds = Object.freeze({
    maxEntries: boundedInteger(options.bounds?.maxEntries, OPTIONAL_TELEMETRY_QUEUE_BOUNDS.maxEntries),
    maxBytes: boundedInteger(options.bounds?.maxBytes, OPTIONAL_TELEMETRY_QUEUE_BOUNDS.maxBytes),
    maxAgeMs: boundedInteger(options.bounds?.maxAgeMs, OPTIONAL_TELEMETRY_QUEUE_BOUNDS.maxAgeMs),
    maxConcurrentWrites: boundedInteger(options.bounds?.maxConcurrentWrites, OPTIONAL_TELEMETRY_QUEUE_BOUNDS.maxConcurrentWrites),
    maxDrainWaitMs: boundedInteger(options.bounds?.maxDrainWaitMs, OPTIONAL_TELEMETRY_QUEUE_BOUNDS.maxDrainWaitMs),
  });
  const now: () => number = options.now ?? Date.now;
  const measure: (entry: TEntry) => number = options.measure ?? defaultMeasure;

  type PendingEntry = Readonly<{ entry: TEntry; bytes: number; enqueued_at_ms: number }>;
  const queued: PendingEntry[] = [];
  /** Dispatched writes that have not settled, with the charge they still hold. */
  const inFlight = new Map<Promise<void>, PendingEntry>();
  const counters = {
    enqueued: 0,
    delivered: 0,
    failed: 0,
    dropped_by_entries: 0,
    dropped_by_bytes: 0,
    dropped_by_age: 0,
    dropped_after_closed: 0,
    coalesced_flushes: 0,
    drain_timeouts: 0,
  };
  let retainedEntries = 0;
  let retainedBytes = 0;
  let drain: Promise<void> | null = null;
  let closed = false;
  let stalled = false;
  let shutdownIncomplete = false;
  let lastErrorClass: string | null = null;

  const timestamp = (): number => {
    const value = Math.trunc(now());
    return Number.isSafeInteger(value) && value >= 0 ? value : Date.now();
  };

  /** Releases the charge of one entry. It is held until the write settles, so the bounds cover in-flight work too. */
  const releaseCharge = (item: PendingEntry): void => {
    retainedEntries -= 1;
    retainedBytes -= item.bytes;
  };

  /**
   * Drops queued entries that outlived the age ceiling. Dispatched writes keep
   * their charge until they settle; the queue never abandons an unresolved
   * write by forgetting it.
   */
  const dropExpiredQueued = (): void => {
    if (queued.length === 0) return;
    const cutoff = timestamp() - bounds.maxAgeMs;
    for (let index = queued.length - 1; index >= 0; index -= 1) {
      if (queued[index].enqueued_at_ms > cutoff) continue;
      releaseCharge(queued[index]);
      queued.splice(index, 1);
      counters.dropped_by_age += 1;
    }
  };

  const isExpired = (item: PendingEntry): boolean => timestamp() - item.enqueued_at_ms >= bounds.maxAgeMs;

  const writeOne = (item: PendingEntry): void => {
    const task = (async () => {
      try {
        if (await options.write(item.entry)) counters.delivered += 1;
        else counters.failed += 1;
      } catch (error) {
        counters.failed += 1;
        lastErrorClass = error instanceof Error ? error.name : typeof error;
      }
    })();
    inFlight.set(task, item);
    void task.finally(() => {
      inFlight.delete(task);
      releaseCharge(item);
      // Full capacity is free again, so a stalled drain may dispatch once more.
      if (inFlight.size === 0) stalled = false;
    });
  };

  /**
   * Waits for any in-flight write to settle, at most `timeoutMs`. Resolves
   * false when no write made progress in time; the race never cancels a write,
   * so the caller keeps its charge until it settles instead of orphaning it.
   */
  const waitForProgress = async (timeoutMs: number): Promise<boolean> => {
    if (inFlight.size === 0) return true;
    let cancelTimeout: () => void = () => {};
    try {
      return await Promise.race([
        Promise.race([...inFlight.keys()]).then(() => true),
        new Promise<boolean>((resolve) => {
          const timer = setTimeout(() => resolve(false), timeoutMs);
          cancelTimeout = () => {
            clearTimeout(timer);
          };
        }),
      ]);
    } finally {
      cancelTimeout();
    }
  };

  /**
   * Writes queued entries while keeping at most `maxConcurrentWrites` writer
   * calls in flight, and drops queued entries that outlived the age ceiling.
   *
   * The wait bound is absolute for this drain operation: it is measured once
   * from the operation's start, so a slowly progressing sink cannot extend it
   * one item at a time. Running out of it ends the operation with
   * `drain_timeouts` and `dispatch_stalled` set; the unresolved writes keep
   * their charge and no further dispatch happens until full capacity is free.
   */
  const drainPending = async (): Promise<void> => {
    if (shutdownIncomplete) return;
    const deadlineAtMs = timestamp() + bounds.maxDrainWaitMs;
    for (;;) {
      dropExpiredQueued();
      if (shutdownIncomplete) return;
      if (stalled && inFlight.size > 0) return;
      while (queued.length > 0 && inFlight.size < bounds.maxConcurrentWrites) {
        const item = queued.shift() as PendingEntry;
        if (isExpired(item)) {
          releaseCharge(item);
          counters.dropped_by_age += 1;
          continue;
        }
        writeOne(item);
      }
      if (queued.length === 0 && inFlight.size === 0) return;
      const remainingMs = deadlineAtMs - timestamp();
      if (remainingMs <= 0) break;
      if (await waitForProgress(remainingMs)) continue;
      break;
    }
    counters.drain_timeouts += 1;
    stalled = true;
  };

  const startDrain = (): Promise<void> => {
    const running = drain;
    if (running) {
      counters.coalesced_flushes += 1;
      return running;
    }
    // The drain promise is published before its loop can run, so a drain that
    // completes synchronously (or coalesces concurrently) still clears the
    // right state and never leaves a settled promise behind as `drain`.
    let settle!: () => void;
    const current = new Promise<void>((resolve) => {
      settle = resolve;
    });
    drain = current;
    void drainPending()
      .catch((error: unknown) => {
        counters.failed += 1;
        lastErrorClass = error instanceof Error ? error.name : typeof error;
      })
      .then(() => {
        if (drain === current) drain = null;
        settle();
      });
    return current;
  };

  const enqueue = (entry: TEntry): OptionalTelemetryEnqueueOutcome => {
    if (closed) {
      counters.dropped_after_closed += 1;
      return "dropped_closed";
    }
    dropExpiredQueued();
    const size = measure(entry);
    if (!Number.isFinite(size) || size < 0) {
      counters.dropped_by_bytes += 1;
      return "dropped_bytes";
    }
    const bytes = Math.ceil(size);
    // The charge covers every retained entry, including unresolved in-flight
    // writes, so a stuck sink cannot let extra entries past the ceilings.
    if (retainedEntries >= bounds.maxEntries) {
      counters.dropped_by_entries += 1;
      return "dropped_capacity";
    }
    if (retainedBytes + bytes > bounds.maxBytes) {
      counters.dropped_by_bytes += 1;
      return "dropped_bytes";
    }
    queued.push({ entry, bytes, enqueued_at_ms: timestamp() });
    retainedEntries += 1;
    retainedBytes += bytes;
    counters.enqueued += 1;
    // Best effort: a writer fault is counted in the snapshot, never surfaced as
    // an unhandled rejection on the request path.
    void startDrain().catch(() => {});
    return "enqueued";
  };

  const close = async (): Promise<void> => {
    closed = true;
    // One bounded drain operation. If it ends with unresolved work, shutdown is
    // reported incomplete and the queue never dispatches again: shutdown can
    // neither hang nor write after giving up.
    if (!shutdownIncomplete) {
      await startDrain();
      if (retainedEntries > 0 || inFlight.size > 0 || queued.length > 0) shutdownIncomplete = true;
    }
  };

  const oldestRetainedAgeMs = (): number | null => {
    if (retainedEntries === 0) return null;
    let oldestAtMs: number | null = null;
    for (const item of queued) {
      if (oldestAtMs === null || item.enqueued_at_ms < oldestAtMs) oldestAtMs = item.enqueued_at_ms;
    }
    for (const item of inFlight.values()) {
      if (oldestAtMs === null || item.enqueued_at_ms < oldestAtMs) oldestAtMs = item.enqueued_at_ms;
    }
    return oldestAtMs === null ? null : Math.max(0, timestamp() - oldestAtMs);
  };

  const snapshot = (): OptionalTelemetryQueueSnapshot => ({
    bounds,
    retained_entries: retainedEntries,
    retained_bytes: retainedBytes,
    queued_entries: queued.length,
    oldest_retained_age_ms: oldestRetainedAgeMs(),
    writes_in_flight: inFlight.size,
    drain_in_progress: drain !== null,
    drain_timeouts: counters.drain_timeouts,
    dispatch_stalled: stalled,
    shutdown_incomplete: shutdownIncomplete,
    coalesced_flushes: counters.coalesced_flushes,
    closed,
    enqueued: counters.enqueued,
    delivered: counters.delivered,
    failed: counters.failed,
    dropped_by_entries: counters.dropped_by_entries,
    dropped_by_bytes: counters.dropped_by_bytes,
    dropped_by_age: counters.dropped_by_age,
    dropped_after_closed: counters.dropped_after_closed,
    last_error_class: lastErrorClass,
  });

  return Object.freeze({ enqueue, flush: startDrain, close, snapshot });
};
