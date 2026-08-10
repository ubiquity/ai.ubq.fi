import {
  beginCodexCacheScopeExperiment,
  CodexCacheScopeExperimentError,
  fetchCodexResponsesForCacheScopeExperiment,
  getCodexResponseSlot,
  refreshCodexCacheScopeExperimentSlot,
  releaseCodexResponseProbe,
} from "./codex.ts";
import { getKv } from "./kv.ts";
import { extractUsageTokens } from "./openai.ts";
import { readResponsesStream } from "./responses_stream.ts";
import { loadRuntimeConfig } from "./runtime_config.ts";
import { isRecord } from "./utils.ts";
import type { ResponseInputItem } from "./types.ts";

export const PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER = "chatgpt_codex" as const;
export const PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES = 3;
export const PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX = ["uos_ai", "prompt_cache_scope", "v1"] as const;
const PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS = 5 * 60_000;
// Leave a full minute for the fenced evidence write and lease release. Every
// individual request also has its own shorter transport deadline.
const PROMPT_CACHE_SCOPE_EXPERIMENT_RUN_DEADLINE_MS = PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS - 60_000;

const CACHE_SCOPE_STEP_NAMES = [
  "slot_1_warm",
  "slot_1_repeat",
  "slot_2_first",
  "slot_2_repeat",
  "slot_1_after_slot_2",
  "slot_1_after_refresh",
  "slot_1_post_refresh_repeat",
  "slot_1_conversation_changed",
  "slot_1_conversation_changed_repeat",
  "slot_1_original_conversation_recheck",
] as const;
const CACHE_SCOPE_EXPECTED_SLOTS = [1, 1, 2, 2, 1, 1, 1, 1, 1, 1] as const;

type CacheScopeStepName = typeof CACHE_SCOPE_STEP_NAMES[number];
export type PromptCacheScopeClassification = "account_scoped" | "not_account_scoped" | "inconclusive";
export type PromptCacheScopeExperimentFailureCode = "deadline_exceeded" | "lease_lost" | "execution_failed";

export type PromptCacheScopeUsage = Readonly<{
  input_tokens: number;
  cached_tokens: number;
  cache_write_tokens: number;
  output_tokens: number;
  total_tokens: number;
}>;

export type PromptCacheScopeSample = Readonly<{
  slot: number;
  raw_usage: PromptCacheScopeUsage;
  normalized_usage: PromptCacheScopeUsage;
  elapsed_ms: number;
}>;

type PromptCacheScopeCycle = Readonly<{ samples: readonly PromptCacheScopeSample[] }>;

export type PromptCacheScopeExperimentResult = Readonly<{
  provider: typeof PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER;
  model: string;
  classification: PromptCacheScopeClassification;
  verified_at_ms: number;
  cycles: readonly Readonly<{
    cycle: number;
    steps: readonly (PromptCacheScopeSample & { name: CacheScopeStepName })[];
  }>[];
}>;

type PromptCacheScopeStoredEvidence = Readonly<{
  v: 1;
  classification: PromptCacheScopeClassification;
  failure_code: PromptCacheScopeExperimentFailureCode | null;
  verified_at_ms: number;
  cycles: readonly PromptCacheScopeCycle[];
}>;

type PromptCacheScopeLease = Readonly<{
  owner: string;
  lease_until_ms: number;
}>;

export class PromptCacheScopeExperimentBusyError extends Error {
  constructor() {
    super("A prompt-cache scope experiment is already running for this provider and model.");
    this.name = "PromptCacheScopeExperimentBusyError";
  }
}

export class PromptCacheScopeExperimentUnavailableError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "PromptCacheScopeExperimentUnavailableError";
  }
}

/** A safe, operator-visible reason for a matrix that did not finish. */
export class PromptCacheScopeExperimentFailedError extends Error {
  readonly failureCode: PromptCacheScopeExperimentFailureCode;

  constructor(failureCode: PromptCacheScopeExperimentFailureCode, message: string) {
    super(message);
    this.name = "PromptCacheScopeExperimentFailedError";
    this.failureCode = failureCode;
  }
}

const experimentKey = (model: string): Deno.KvKey => [
  ...PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX,
  PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER,
  model,
];

const experimentLeaseKey = (model: string): Deno.KvKey => [...experimentKey(model), "lease"];

type PromptCacheScopeExperimentLeaseHandle = {
  readonly key: Deno.KvKey;
  readonly owner: string;
};

const isSafeNonNegativeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const cancelResponse = (response: Response): void => {
  try {
    const cancellation = response.body?.cancel();
    if (cancellation) void cancellation.catch(() => {});
  } catch {
    // The response is diagnostic-only and cancellation is best effort.
  }
};

const staticPrefix = Array.from(
  { length: 2_560 },
  () => "cache",
).join(" ");

const buildExperimentRequest = (model: string, cycleId: string, cacheKey: string): Record<string, unknown> => {
  const input: ResponseInputItem[] = [
    {
      type: "message",
      role: "developer",
      content: [{
        type: "input_text",
        text: `${staticPrefix}\n\ncache-scope-cycle:${cycleId}`,
        prompt_cache_breakpoint: { mode: "explicit" },
      }],
    },
    {
      type: "message",
      role: "user",
      content: [{ type: "input_text", text: "Reply with exactly: cache scope experiment." }],
    },
  ];
  return {
    model,
    input,
    store: false,
    // The control-plane response is buffered, but the upstream protocol stays
    // SSE so this runner can verify one completed terminal event itself.
    stream: true,
    max_output_tokens: 16,
    reasoning: { effort: "none" },
    prompt_cache_key: cacheKey,
    prompt_cache_options: { mode: "explicit", ttl: "30m" },
  };
};

const rawUsageSample = (value: unknown): PromptCacheScopeUsage | null => {
  if (!isRecord(value) || Array.isArray(value)) return null;
  const details = isRecord(value.input_tokens_details) && !Array.isArray(value.input_tokens_details)
    ? value.input_tokens_details
    : null;
  if (!details) return null;
  const inputTokens = value.input_tokens;
  const cachedTokens = details.cached_tokens;
  const cacheWriteTokens = details.cache_write_tokens;
  const outputTokens = value.output_tokens;
  const totalTokens = value.total_tokens;
  if (
    !isSafeNonNegativeInteger(inputTokens) || !isSafeNonNegativeInteger(cachedTokens) ||
    !isSafeNonNegativeInteger(cacheWriteTokens) || !isSafeNonNegativeInteger(outputTokens) ||
    !isSafeNonNegativeInteger(totalTokens)
  ) {
    return null;
  }
  return {
    input_tokens: inputTokens,
    cached_tokens: cachedTokens,
    cache_write_tokens: cacheWriteTokens,
    output_tokens: outputTokens,
    total_tokens: totalTokens,
  };
};

const sameUsage = (left: PromptCacheScopeUsage, right: PromptCacheScopeUsage): boolean =>
  left.input_tokens === right.input_tokens &&
  left.cached_tokens === right.cached_tokens &&
  left.cache_write_tokens === right.cache_write_tokens &&
  left.output_tokens === right.output_tokens &&
  left.total_tokens === right.total_tokens;

const readCompletedUsage = async (
  response: Response,
  expectedSlot: number,
  startedAtMs: number,
  signal: AbortSignal,
): Promise<PromptCacheScopeSample> => {
  try {
    if (!response.ok || !response.body || getCodexResponseSlot(response) !== expectedSlot) {
      cancelResponse(response);
      throw new CodexCacheScopeExperimentError(
        "Prompt-cache scope experiment response was not pinned to the requested slot.",
      );
    }

    let terminalResponse: Record<string, unknown> | null = null;
    try {
      for await (const event of readResponsesStream(response.body, signal)) {
        if (
          event.type === "response.completed" && isRecord(event.value.response) && !Array.isArray(event.value.response)
        ) {
          terminalResponse = event.value.response;
          break;
        }
        if (event.terminal) break;
      }
    } catch (error) {
      throw new CodexCacheScopeExperimentError(
        error instanceof Error
          ? `Prompt-cache scope experiment stream failed: ${error.message}`
          : "Prompt-cache scope experiment stream failed.",
      );
    }
    if (!terminalResponse) {
      throw new CodexCacheScopeExperimentError(
        "Prompt-cache scope experiment did not receive a completed terminal response.",
      );
    }

    const raw = rawUsageSample(terminalResponse.usage);
    const normalized = extractUsageTokens(terminalResponse.usage);
    if (
      !raw || !normalized || normalized.status !== "reported" || normalized.inputTokens === null ||
      normalized.cachedInputTokens === null || normalized.cacheWriteInputTokens === null ||
      normalized.outputTokens === null || normalized.totalTokens === null
    ) {
      throw new CodexCacheScopeExperimentError(
        "Prompt-cache scope experiment usage telemetry is incomplete or invalid.",
      );
    }
    const normalizedUsage: PromptCacheScopeUsage = {
      input_tokens: normalized.inputTokens,
      cached_tokens: normalized.cachedInputTokens,
      cache_write_tokens: normalized.cacheWriteInputTokens,
      output_tokens: normalized.outputTokens,
      total_tokens: normalized.totalTokens,
    };
    if (!sameUsage(raw, normalizedUsage) || raw.input_tokens < 1_024) {
      throw new CodexCacheScopeExperimentError(
        "Prompt-cache scope experiment usage did not match normalized telemetry.",
      );
    }
    return {
      slot: expectedSlot,
      raw_usage: raw,
      normalized_usage: normalizedUsage,
      elapsed_ms: Math.max(0, Math.round(performance.now() - startedAtMs)),
    };
  } finally {
    try {
      await releaseCodexResponseProbe(response);
    } catch {
      // Probe release is best effort after the sample's terminal outcome.
    }
  }
};

const hasCacheRead = (sample: PromptCacheScopeSample): boolean => sample.normalized_usage.cached_tokens > 0;
const hasCacheWrite = (sample: PromptCacheScopeSample): boolean => sample.normalized_usage.cache_write_tokens > 0;

type CycleClassification = "account_scoped" | "not_account_scoped" | "inconclusive";

const classifyCycle = (cycle: PromptCacheScopeCycle): CycleClassification => {
  if (cycle.samples.length !== CACHE_SCOPE_STEP_NAMES.length) return "inconclusive";
  const [
    warm,
    repeat,
    slot2First,
    slot2Repeat,
    slot1AfterSlot2,
    afterRefresh,
    postRefreshRepeat,
    conversationChanged,
    conversationChangedRepeat,
    originalConversationRecheck,
  ] = cycle.samples;
  if (
    !warm || !repeat || !slot2First || !slot2Repeat || !slot1AfterSlot2 || !afterRefresh || !postRefreshRepeat ||
    !conversationChanged || !conversationChangedRepeat || !originalConversationRecheck ||
    !hasCacheWrite(warm) || !hasCacheRead(repeat) || !hasCacheRead(slot2Repeat) || !hasCacheRead(slot1AfterSlot2) ||
    !hasCacheRead(postRefreshRepeat) || !hasCacheRead(conversationChangedRepeat) ||
    !hasCacheRead(originalConversationRecheck)
  ) {
    return "inconclusive";
  }
  if (
    cycle.samples.some((sample, index) => sample.slot !== CACHE_SCOPE_EXPECTED_SLOTS[index]) ||
    cycle.samples.some((sample) => sample.normalized_usage.input_tokens !== warm.normalized_usage.input_tokens)
  ) {
    return "inconclusive";
  }

  const slotScope = hasCacheRead(slot2First) ? "shared" : hasCacheWrite(slot2First) ? "account" : "inconclusive";
  const refreshScope = hasCacheRead(afterRefresh)
    ? "account_stable"
    : hasCacheWrite(afterRefresh)
    ? "token_sensitive"
    : "inconclusive";
  const conversationScope = hasCacheRead(conversationChanged)
    ? "independent"
    : hasCacheWrite(conversationChanged)
    ? "partitioned"
    : "inconclusive";

  if (slotScope === "inconclusive" || refreshScope === "inconclusive" || conversationScope === "inconclusive") {
    return "inconclusive";
  }
  return slotScope === "account" && refreshScope === "account_stable" && conversationScope === "independent"
    ? "account_scoped"
    : "not_account_scoped";
};

const classifyExperiment = (cycles: readonly PromptCacheScopeCycle[]): PromptCacheScopeClassification => {
  if (cycles.length !== PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES) return "inconclusive";
  const classifications = cycles.map(classifyCycle);
  if (classifications.every((classification) => classification === "account_scoped")) return "account_scoped";
  if (classifications.every((classification) => classification === "not_account_scoped")) return "not_account_scoped";
  return "inconclusive";
};

const publicResult = (
  model: string,
  classification: PromptCacheScopeClassification,
  verifiedAtMs: number,
  cycles: readonly PromptCacheScopeCycle[],
): PromptCacheScopeExperimentResult => ({
  provider: PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER,
  model,
  classification,
  verified_at_ms: verifiedAtMs,
  cycles: cycles.map((cycle, cycleIndex) => ({
    cycle: cycleIndex + 1,
    steps: cycle.samples.map((sample, index) => ({
      name: CACHE_SCOPE_STEP_NAMES[index]!,
      ...sample,
    })),
  })),
});

const leaseLost = (): PromptCacheScopeExperimentFailedError =>
  new PromptCacheScopeExperimentFailedError(
    "lease_lost",
    "Prompt-cache scope experiment lost its exclusive lease before completion.",
  );

const isLeaseHeldBy = (value: PromptCacheScopeLease | null, owner: string, now = Date.now()): boolean =>
  value?.owner === owner && Number.isSafeInteger(value.lease_until_ms) && value.lease_until_ms > now;

const acquireLease = async (kv: Deno.Kv, model: string): Promise<PromptCacheScopeExperimentLeaseHandle> => {
  const key = experimentLeaseKey(model);
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const entry = await kv.get<PromptCacheScopeLease>(key, { consistency: "strong" });
    const current = entry.value;
    if (current && current.lease_until_ms > Date.now()) throw new PromptCacheScopeExperimentBusyError();
    const owner = crypto.randomUUID();
    const committed = await kv.atomic()
      .check(entry)
      .set(
        key,
        { owner, lease_until_ms: Date.now() + PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS } satisfies PromptCacheScopeLease,
        { expireIn: PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS * 2 },
      )
      .commit();
    if (committed.ok) return { key, owner };
  }
  throw new PromptCacheScopeExperimentBusyError();
};

/**
 * Fence every stage against a stale owner. An expired owner must never renew
 * itself: a second isolate may already have acquired the same model lease.
 */
const renewLease = async (kv: Deno.Kv, lease: PromptCacheScopeExperimentLeaseHandle): Promise<void> => {
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const entry = await kv.get<PromptCacheScopeLease>(lease.key, { consistency: "strong" });
    if (!isLeaseHeldBy(entry.value, lease.owner)) throw leaseLost();
    const committed = await kv.atomic()
      .check(entry)
      .set(
        lease.key,
        {
          owner: lease.owner,
          lease_until_ms: Date.now() + PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS,
        } satisfies PromptCacheScopeLease,
        { expireIn: PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS * 2 },
      )
      .commit();
    if (committed.ok) return;
  }
  const current = await kv.get<PromptCacheScopeLease>(lease.key, { consistency: "strong" });
  if (!isLeaseHeldBy(current.value, lease.owner)) throw leaseLost();
  throw new PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiment lease could not be renewed.");
};

const releaseLease = async (kv: Deno.Kv, lease: PromptCacheScopeExperimentLeaseHandle): Promise<void> => {
  try {
    const entry = await kv.get<PromptCacheScopeLease>(lease.key, { consistency: "strong" });
    if (entry.value?.owner !== lease.owner) return;
    await kv.atomic().check(entry).delete(lease.key).commit();
  } catch {
    // The short lease expires without retaining experiment data.
  }
};

const persistEvidence = async (
  kv: Deno.Kv,
  model: string,
  classification: PromptCacheScopeClassification,
  failureCode: PromptCacheScopeExperimentFailureCode | null,
  verifiedAtMs: number,
  cycles: readonly PromptCacheScopeCycle[],
  lease: PromptCacheScopeExperimentLeaseHandle,
): Promise<void> => {
  const evidence: PromptCacheScopeStoredEvidence = {
    v: 1,
    classification,
    failure_code: failureCode,
    verified_at_ms: verifiedAtMs,
    cycles,
  };
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const [entry, leaseEntry] = await Promise.all([
      kv.get<PromptCacheScopeStoredEvidence>(experimentKey(model), { consistency: "strong" }),
      kv.get<PromptCacheScopeLease>(lease.key, { consistency: "strong" }),
    ]);
    if (!isLeaseHeldBy(leaseEntry.value, lease.owner)) throw leaseLost();
    const committed = await kv.atomic()
      .check(entry)
      .check(leaseEntry)
      .set(experimentKey(model), evidence)
      .commit();
    if (committed.ok) return;
  }
  throw new PromptCacheScopeExperimentUnavailableError(
    "Prompt-cache scope experiment evidence could not be persisted.",
  );
};

const dispatchSample = async (
  body: Record<string, unknown>,
  expectedBody: string,
  session: Awaited<ReturnType<typeof beginCodexCacheScopeExperiment>>,
  slot: number,
  conversationId: string,
  runSignal: AbortSignal,
): Promise<PromptCacheScopeSample> => {
  if (JSON.stringify(body) !== expectedBody) {
    throw new CodexCacheScopeExperimentError("Prompt-cache scope experiment request body drifted.");
  }
  const startedAtMs = performance.now();
  const signal = AbortSignal.any([runSignal, AbortSignal.timeout(100_000)]);
  const response = await fetchCodexResponsesForCacheScopeExperiment(body, {
    session,
    slot,
    conversationId,
    signal,
  });
  return await readCompletedUsage(response, slot, startedAtMs, signal);
};

const abortReason = (signal: AbortSignal): unknown =>
  signal.reason ?? new DOMException("Prompt-cache scope experiment was aborted.", "AbortError");

const throwIfAborted = (signal: AbortSignal): void => {
  if (signal.aborted) throw abortReason(signal);
};

const failureFor = (
  error: unknown,
  runSignal: AbortSignal,
): PromptCacheScopeExperimentFailedError => {
  if (error instanceof PromptCacheScopeExperimentFailedError) return error;
  if (runSignal.aborted) {
    return new PromptCacheScopeExperimentFailedError(
      "deadline_exceeded",
      "Prompt-cache scope experiment exceeded its safe execution deadline.",
    );
  }
  return new PromptCacheScopeExperimentFailedError(
    "execution_failed",
    "Prompt-cache scope experiment did not complete its evidence matrix.",
  );
};

/** `signal` is internal-only composition for callers that must stop earlier; it can never extend the hard deadline. */
export const runPromptCacheScopeExperiment = async (
  options: Readonly<{ signal?: AbortSignal }> = {},
): Promise<PromptCacheScopeExperimentResult> => {
  const kv = await getKv();
  if (!kv) {
    throw new PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiments require Deno KV.");
  }
  const runtime = await loadRuntimeConfig(kv);
  const model = runtime?.default_model?.trim();
  if (!model) {
    throw new PromptCacheScopeExperimentUnavailableError(
      "Prompt-cache scope experiment requires a configured default model.",
    );
  }

  const lease = await acquireLease(kv, model);
  const hardDeadline = AbortSignal.timeout(PROMPT_CACHE_SCOPE_EXPERIMENT_RUN_DEADLINE_MS);
  const runSignal = options.signal ? AbortSignal.any([hardDeadline, options.signal]) : hardDeadline;
  const cycles: Array<{ samples: PromptCacheScopeSample[] }> = [];
  let classification: PromptCacheScopeClassification = "inconclusive";
  let verifiedAtMs = 0;
  let failure: PromptCacheScopeExperimentFailedError | null = null;
  try {
    throwIfAborted(runSignal);
    const session = await beginCodexCacheScopeExperiment();
    const dispatch = async (
      body: Record<string, unknown>,
      expectedBody: string,
      slot: number,
      conversationId: string,
    ): Promise<PromptCacheScopeSample> => {
      throwIfAborted(runSignal);
      await renewLease(kv, lease);
      throwIfAborted(runSignal);
      return await dispatchSample(body, expectedBody, session, slot, conversationId, runSignal);
    };
    for (let cycle = 0; cycle < PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES; cycle += 1) {
      const cycleId = crypto.randomUUID();
      const cacheKey = `uos-cache-scope-v1-${crypto.randomUUID()}`;
      const conversationA = crypto.randomUUID();
      const conversationB = crypto.randomUUID();
      const body = buildExperimentRequest(model, cycleId, cacheKey);
      const expectedBody = JSON.stringify(body);
      const evidence = { samples: [] as PromptCacheScopeSample[] };
      cycles.push(evidence);

      evidence.samples.push(await dispatch(body, expectedBody, 1, conversationA));
      evidence.samples.push(await dispatch(body, expectedBody, 1, conversationA));
      evidence.samples.push(await dispatch(body, expectedBody, 2, conversationA));
      evidence.samples.push(await dispatch(body, expectedBody, 2, conversationA));
      evidence.samples.push(await dispatch(body, expectedBody, 1, conversationA));

      throwIfAborted(runSignal);
      await renewLease(kv, lease);
      throwIfAborted(runSignal);
      const refresh = await refreshCodexCacheScopeExperimentSlot(session, 1, runSignal);
      throwIfAborted(runSignal);
      if (!refresh.tokenChanged) {
        throw new CodexCacheScopeExperimentError(
          "Codex OAuth refresh did not change credentials during the prompt-cache scope experiment.",
        );
      }
      evidence.samples.push(await dispatch(body, expectedBody, 1, conversationA));
      evidence.samples.push(await dispatch(body, expectedBody, 1, conversationA));
      evidence.samples.push(await dispatch(body, expectedBody, 1, conversationB));
      evidence.samples.push(await dispatch(body, expectedBody, 1, conversationB));
      evidence.samples.push(await dispatch(body, expectedBody, 1, conversationA));
    }
    classification = classifyExperiment(cycles);
  } catch (error) {
    classification = "inconclusive";
    failure = failureFor(error, runSignal);
  } finally {
    verifiedAtMs = Date.now();
    try {
      await persistEvidence(kv, model, classification, failure?.failureCode ?? null, verifiedAtMs, cycles, lease);
    } finally {
      await releaseLease(kv, lease);
    }
  }
  if (failure) throw failure;
  return publicResult(model, classification, verifiedAtMs, cycles);
};
