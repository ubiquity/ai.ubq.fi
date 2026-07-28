import {
  beginCodexCacheScopeExperiment,
  CodexCacheScopeExperimentError,
  fetchCodexResponsesForCacheScopeExperiment,
  getCodexResponseSlot,
  refreshCodexCacheScopeExperimentSlot,
  releaseCodexResponseProbe,
} from "./codex.ts";
import { promoteCodexPromptCacheScope } from "./codex_catalog.ts";
import {
  CODEX_CHATGPT_PROMPT_CACHE_PROVIDER,
  PROMPT_CACHE_SCOPE_PROBE_PROFILE,
  type PromptCacheAccountSlots,
  type PromptCacheConversationId,
  type PromptCacheScope,
  type PromptCacheTokenRefresh,
} from "./codex_models.ts";
import { getKv } from "./kv.ts";
import { extractUsageTokens } from "./openai.ts";
import { type PromptCacheTelemetryProvider, readPromptCacheTelemetryBaseline } from "./prompt_cache_telemetry_gate.ts";
import {
  loadPromptCacheScopeTargetInventory,
  type PromptCacheScopeTarget,
  type PromptCacheScopeTargetInventory,
} from "./prompt_cache_scope_targets.ts";
import { readResponsesStream } from "./responses_stream.ts";
import { normalizeRuntimeConfig, RUNTIME_CONFIG_V2_KEY, type RuntimeConfigV2 } from "./runtime_config.ts";
import { getString, isRecord } from "./utils.ts";
import type { ResponseInputItem } from "./types.ts";

export const PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER = CODEX_CHATGPT_PROMPT_CACHE_PROVIDER;
const CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER: PromptCacheTelemetryProvider = "chatgpt_codex";
/**
 * v3 is a hard cutover from the singleton/default-model experiment state.
 * Every durable key below includes the fixed probe profile and exact target.
 */
export const PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX = ["uos_ai", "prompt_cache_scope_experiment", "v3"] as const;
export const PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES = 3;
export const PROMPT_CACHE_SCOPE_EXPERIMENT_SAMPLES_PER_CYCLE = 10;

const PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS = 120_000;
const PROMPT_CACHE_SCOPE_EXPERIMENT_SESSION_MS = 15 * 60_000;
const PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLE_DEADLINE_MS = 100_000;
const PROMPT_CACHE_SCOPE_EXPERIMENT_SAMPLE_DEADLINE_MS = 7_000;

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

/**
 * Test-only dependency injection. The admin route calls the assertion without
 * arguments, binding the gate to this immutable artifact's release identity.
 */
type PromptCacheScopeExperimentTelemetryBaselineTestOptions = Readonly<{
  kv?: Deno.Kv | null;
  release?: string;
}>;

/**
 * An in-process attestation created immediately after the immutable-release
 * Stage 0 read. It is intentionally not returned from the admin endpoint:
 * the runner uses it only to ensure that it cannot silently retarget paid
 * work after the gate has selected a model.
 */
export type PromptCacheScopeExperimentTelemetryBaseline = Readonly<{
  target: PromptCacheScopeTargetBinding;
}>;

type CacheScopeStepName = typeof CACHE_SCOPE_STEP_NAMES[number];
type CacheSignal = "read" | "write";
type InconclusiveReason =
  | "auth_pool_drift"
  | "capability_changed"
  | "cycle_disagreement"
  | "effective_model_drift"
  | "incomplete_telemetry"
  | "invalid_cache_signal"
  | "invalid_input_size"
  | "inventory_drift"
  | "lease_lost"
  | "model_drift"
  | "promotion_conflict"
  | "refresh_unchanged"
  | "runtime_drift"
  | "session_expired"
  | "slot_drift"
  | "target_catalog_drift";

type ConcreteScopeObservation = Readonly<{
  probe_profile: typeof PROMPT_CACHE_SCOPE_PROBE_PROFILE;
  account_slots: Exclude<PromptCacheAccountSlots, "unknown">;
  token_refresh: Exclude<PromptCacheTokenRefresh, "unknown">;
  conversation_id: Exclude<PromptCacheConversationId, "unknown">;
  effective_model: string;
}>;

type NormalizedUsage = Readonly<{
  input_tokens: number;
  cached_tokens: number;
  cache_write_tokens: number;
  output_tokens: number;
  total_tokens: number;
}>;

type PromptCacheScopeSample = Readonly<{
  step: CacheScopeStepName;
  slot: number;
  /**
   * The provider-reported five-counter tuple before gateway normalization.
   * It deliberately excludes all other response fields so durable experiment
   * evidence remains redacted.
   */
  raw_usage: NormalizedUsage;
  usage: NormalizedUsage;
  elapsed_ms: number;
}>;

type CycleEvidence = Readonly<{
  cycle: number;
  samples: readonly PromptCacheScopeSample[];
  classification?: ConcreteScopeObservation;
  inconclusive_reason?: InconclusiveReason;
}>;

/**
 * The target definition is stable across benign catalog metadata refreshes;
 * the dynamic catalog/runtime/auth versionstamps fence the next dispatch or
 * publication. The inventory fingerprint covers the complete provider/model
 * roster, so an active campaign cannot silently skip a new or removed target.
 */
type PromptCacheScopeTargetBinding = Readonly<{
  id: string;
  provider: typeof PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER;
  telemetry_provider: typeof CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER;
  topology_kind: "codex_account_pool";
  model: string;
  probe_profile: typeof PROMPT_CACHE_SCOPE_PROBE_PROFILE;
  capability_fingerprint: string;
  inventory_fingerprint: string;
  catalog_versionstamp: string;
  runtime_versionstamp: string;
  auth_pool_versionstamp: string;
  auth_pool_identity_fingerprint: string;
  catalog_client_version: string | null;
}>;

type StoredEvidence = Readonly<{
  v: 3;
  target: PromptCacheScopeTargetBinding;
  outcome: "in_progress" | "ready_to_promote" | "completed" | "inconclusive" | "failed";
  started_at_ms: number;
  verified_at_ms: number;
  cycles: readonly CycleEvidence[];
  inconclusive_reason?: InconclusiveReason;
}>;

type ExperimentState = Readonly<{
  v: 3;
  target: PromptCacheScopeTargetBinding;
  campaign_owner: string;
  started_at_ms: number;
  expires_at_ms: number;
  auth_pool_versionstamp: string;
  next_cycle: number;
  classifications: readonly ConcreteScopeObservation[];
  pending_scope?: ConcreteScopeObservation;
}>;

type ExperimentLease = Readonly<{ owner: string; lease_until_ms: number }>;
type CampaignLease = Readonly<{
  owner: string;
  target_id: string;
  inventory_fingerprint: string;
  lease_until_ms: number;
}>;

export type PromptCacheScopeExperimentResult = Readonly<{
  provider: typeof PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER;
  telemetry_provider: typeof CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER;
  target_id: string;
  model: string;
  status: "in_progress" | "completed" | "inconclusive";
  completed_cycles: number;
  verified_at_ms: number;
  scope?: ConcreteScopeObservation;
  inconclusive_reason?: InconclusiveReason;
}>;

export class PromptCacheScopeExperimentBusyError extends Error {
  constructor() {
    super("A prompt-cache scope experiment campaign is already running for the active provider.");
    this.name = "PromptCacheScopeExperimentBusyError";
  }
}

export class PromptCacheScopeExperimentUnavailableError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "PromptCacheScopeExperimentUnavailableError";
  }
}

export class PromptCacheScopeExperimentFailedError extends Error {
  constructor(message: string) {
    super(message);
    this.name = "PromptCacheScopeExperimentFailedError";
  }
}

const isSafeNonNegativeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const hasOwn = (value: Record<string, unknown>, key: string): boolean =>
  Object.prototype.hasOwnProperty.call(value, key);

const hasOnlyKeys = (value: Record<string, unknown>, allowed: readonly string[]): boolean =>
  Object.keys(value).every((key) => allowed.includes(key));

const hasExactKeys = (value: Record<string, unknown>, expected: readonly string[]): boolean =>
  Object.keys(value).length === expected.length && expected.every((key) => hasOwn(value, key));

const targetKeyParts = (
  target: Pick<
    PromptCacheScopeTargetBinding,
    "provider" | "telemetry_provider" | "topology_kind" | "probe_profile" | "model"
  >,
): readonly string[] => [
  target.provider,
  target.telemetry_provider,
  target.topology_kind,
  target.probe_profile,
  target.model,
];

const stateKey = (target: PromptCacheScopeTargetBinding): Deno.KvKey => [
  ...PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX,
  "state",
  ...targetKeyParts(target),
];
const evidenceKey = (target: PromptCacheScopeTargetBinding): Deno.KvKey => [
  ...PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX,
  "evidence",
  ...targetKeyParts(target),
];
const cycleLeaseKey = (target: PromptCacheScopeTargetBinding): Deno.KvKey => [
  ...PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX,
  "cycle_lease",
  ...targetKeyParts(target),
];
/** OAuth refresh mutates a shared Codex pool, so this fence is provider-wide. */
const campaignLeaseKey = (
  target: Pick<PromptCacheScopeTargetBinding, "provider" | "telemetry_provider" | "probe_profile">,
): Deno.KvKey => [
  ...PROMPT_CACHE_SCOPE_EXPERIMENT_KV_PREFIX,
  "campaign",
  target.provider,
  target.telemetry_provider,
  target.probe_profile,
];

const isConcreteObservation = (value: unknown): value is ConcreteScopeObservation =>
  isRecord(value) && !Array.isArray(value) &&
  hasExactKeys(value, ["probe_profile", "account_slots", "token_refresh", "conversation_id", "effective_model"]) &&
  value.probe_profile === PROMPT_CACHE_SCOPE_PROBE_PROFILE &&
  (value.account_slots === "shared" || value.account_slots === "account_scoped") &&
  (value.token_refresh === "preserved" || value.token_refresh === "changed") &&
  (value.conversation_id === "independent" || value.conversation_id === "scoped") &&
  Boolean(getString(value.effective_model)?.trim());

const parseTargetBinding = (value: unknown): PromptCacheScopeTargetBinding | null => {
  if (
    !isRecord(value) || Array.isArray(value) ||
    !hasExactKeys(value, [
      "id",
      "provider",
      "telemetry_provider",
      "topology_kind",
      "model",
      "probe_profile",
      "capability_fingerprint",
      "inventory_fingerprint",
      "catalog_versionstamp",
      "runtime_versionstamp",
      "auth_pool_versionstamp",
      "auth_pool_identity_fingerprint",
      "catalog_client_version",
    ]) ||
    value.provider !== PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER ||
    value.telemetry_provider !== CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER ||
    value.topology_kind !== "codex_account_pool" ||
    value.probe_profile !== PROMPT_CACHE_SCOPE_PROBE_PROFILE
  ) return null;
  const required = [
    value.id,
    value.model,
    value.capability_fingerprint,
    value.inventory_fingerprint,
    value.catalog_versionstamp,
    value.runtime_versionstamp,
    value.auth_pool_versionstamp,
    value.auth_pool_identity_fingerprint,
  ].every((entry) => Boolean(getString(entry)?.trim()));
  const clientVersion = value.catalog_client_version === null
    ? null
    : getString(value.catalog_client_version)?.trim() || null;
  if (!required || (value.catalog_client_version !== null && clientVersion === null)) return null;
  return {
    id: getString(value.id)!.trim(),
    provider: PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER,
    telemetry_provider: CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER,
    topology_kind: "codex_account_pool",
    model: getString(value.model)!.trim(),
    probe_profile: PROMPT_CACHE_SCOPE_PROBE_PROFILE,
    capability_fingerprint: getString(value.capability_fingerprint)!.trim(),
    inventory_fingerprint: getString(value.inventory_fingerprint)!.trim(),
    catalog_versionstamp: getString(value.catalog_versionstamp)!.trim(),
    runtime_versionstamp: getString(value.runtime_versionstamp)!.trim(),
    auth_pool_versionstamp: getString(value.auth_pool_versionstamp)!.trim(),
    auth_pool_identity_fingerprint: getString(value.auth_pool_identity_fingerprint)!.trim(),
    catalog_client_version: clientVersion,
  };
};

const sameTargetDefinition = (left: PromptCacheScopeTargetBinding, right: PromptCacheScopeTargetBinding): boolean =>
  left.id === right.id && left.provider === right.provider && left.telemetry_provider === right.telemetry_provider &&
  left.topology_kind === right.topology_kind && left.model === right.model &&
  left.probe_profile === right.probe_profile && left.capability_fingerprint === right.capability_fingerprint &&
  left.inventory_fingerprint === right.inventory_fingerprint &&
  left.auth_pool_identity_fingerprint === right.auth_pool_identity_fingerprint;

const sameObservation = (left: ConcreteScopeObservation, right: ConcreteScopeObservation): boolean =>
  left.probe_profile === right.probe_profile &&
  left.account_slots === right.account_slots &&
  left.token_refresh === right.token_refresh &&
  left.conversation_id === right.conversation_id &&
  left.effective_model === right.effective_model;

const parseState = (value: unknown): ExperimentState | null => {
  if (
    !isRecord(value) || value.v !== 3 || Array.isArray(value) ||
    !hasOnlyKeys(value, [
      "v",
      "target",
      "campaign_owner",
      "started_at_ms",
      "expires_at_ms",
      "auth_pool_versionstamp",
      "next_cycle",
      "classifications",
      "pending_scope",
    ]) ||
    ![
      "target",
      "campaign_owner",
      "started_at_ms",
      "expires_at_ms",
      "auth_pool_versionstamp",
      "next_cycle",
      "classifications",
    ].every((key) => hasOwn(value, key))
  ) return null;
  const target = parseTargetBinding(value.target);
  const campaignOwner = getString(value.campaign_owner)?.trim();
  const authPoolVersionstamp = getString(value.auth_pool_versionstamp)?.trim();
  const nextCycle = value.next_cycle;
  const classifications = Array.isArray(value.classifications) ? value.classifications : null;
  const now = Date.now();
  if (typeof nextCycle !== "number" || !Number.isSafeInteger(nextCycle)) return null;
  if (
    !target || !campaignOwner || !authPoolVersionstamp || authPoolVersionstamp !== target.auth_pool_versionstamp ||
    !isSafeNonNegativeInteger(value.started_at_ms) || !isSafeNonNegativeInteger(value.expires_at_ms) ||
    value.started_at_ms <= 0 || value.started_at_ms > now || value.expires_at_ms <= value.started_at_ms ||
    nextCycle < 1 ||
    nextCycle > PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES + 1 ||
    !classifications || !classifications.every(isConcreteObservation) ||
    classifications.some((classification) => classification.effective_model !== target.model)
  ) return null;
  if (classifications.length !== Math.min(nextCycle - 1, PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES)) {
    return null;
  }
  const pending = value.pending_scope;
  if (pending !== undefined && !isConcreteObservation(pending)) return null;
  if (
    nextCycle === PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES + 1 &&
    (!isConcreteObservation(pending) || pending.effective_model !== target.model ||
      !classifications.every((classification) => sameObservation(classification, classifications[0]!)) ||
      !sameObservation(pending, classifications[0]!))
  ) return null;
  if (nextCycle <= PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES && pending !== undefined) return null;
  return {
    v: 3,
    target,
    campaign_owner: campaignOwner,
    started_at_ms: value.started_at_ms,
    expires_at_ms: value.expires_at_ms,
    auth_pool_versionstamp: authPoolVersionstamp,
    next_cycle: nextCycle,
    classifications,
    ...(pending !== undefined ? { pending_scope: pending } : {}),
  };
};

const ownsLease = (value: unknown, owner: string): boolean =>
  isRecord(value) && value.owner === owner && isSafeNonNegativeInteger(value.lease_until_ms) &&
  value.lease_until_ms > Date.now();

const ownsCampaignLease = (value: unknown, state: ExperimentState): boolean =>
  isRecord(value) && value.owner === state.campaign_owner && value.target_id === state.target.id &&
  value.inventory_fingerprint === state.target.inventory_fingerprint &&
  isSafeNonNegativeInteger(value.lease_until_ms) && value.lease_until_ms > Date.now();

const cancelResponse = (response: Response): void => {
  try {
    const cancelled = response.body?.cancel();
    if (cancelled) void cancelled.catch(() => {});
  } catch {
    // Cancellation is diagnostic cleanup only.
  }
};

const staticPrefix = Array.from({ length: 2_560 }, () => "cache").join(" ");

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
    stream: true,
    max_output_tokens: 16,
    reasoning: { effort: "none" },
    prompt_cache_key: cacheKey,
    prompt_cache_options: { mode: "explicit", ttl: "30m" },
  };
};

const rawUsageSample = (value: unknown): NormalizedUsage | null => {
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
  ) return null;
  return {
    input_tokens: inputTokens,
    cached_tokens: cachedTokens,
    cache_write_tokens: cacheWriteTokens,
    output_tokens: outputTokens,
    total_tokens: totalTokens,
  };
};

const sameUsage = (left: NormalizedUsage, right: NormalizedUsage): boolean =>
  left.input_tokens === right.input_tokens &&
  left.cached_tokens === right.cached_tokens &&
  left.cache_write_tokens === right.cache_write_tokens &&
  left.output_tokens === right.output_tokens &&
  left.total_tokens === right.total_tokens;

const cacheSignal = (usage: NormalizedUsage): CacheSignal | null => {
  // OpenAI reports cache reads and writes as independent dimensions. A request
  // can read an earlier matching breakpoint while also writing a later one, so
  // a positive cached_tokens value is still conclusive cache-read evidence.
  if (usage.cached_tokens > 0) return "read";
  if (usage.cache_write_tokens > 0) return "write";
  return null;
};

type ReadSampleResult =
  | Readonly<{ status: "sample"; sample: Omit<PromptCacheScopeSample, "step">; signal: CacheSignal }>
  | Readonly<{ status: "inconclusive"; reason: InconclusiveReason }>;

export const readPromptCacheScopeExperimentCompletedUsage = async (
  response: Response,
  expectedSlot: number,
  expectedModel: string,
  startedAtMs: number,
  signal: AbortSignal,
): Promise<ReadSampleResult> => {
  try {
    if (!response.ok || !response.body) {
      cancelResponse(response);
      throw new CodexCacheScopeExperimentError(
        "Prompt-cache scope experiment did not receive a readable upstream response.",
      );
    }
    if (getCodexResponseSlot(response) !== expectedSlot) {
      cancelResponse(response);
      return { status: "inconclusive", reason: "slot_drift" };
    }
    let terminalResponse: Record<string, unknown> | null = null;
    for await (const event of readResponsesStream(response.body, signal)) {
      if (
        event.type === "response.completed" && isRecord(event.value.response) && !Array.isArray(event.value.response)
      ) {
        terminalResponse = event.value.response;
        break;
      }
      if (event.terminal) break;
    }
    if (!terminalResponse) {
      throw new CodexCacheScopeExperimentError(
        "Prompt-cache scope experiment did not receive a completed terminal response.",
      );
    }
    if (getString(terminalResponse.model)?.trim() !== expectedModel) {
      return { status: "inconclusive", reason: "effective_model_drift" };
    }
    const raw = rawUsageSample(terminalResponse.usage);
    const normalized = extractUsageTokens(terminalResponse.usage);
    if (
      !raw || !normalized || normalized.status !== "reported" || normalized.inputTokens === null ||
      normalized.cachedInputTokens === null || normalized.cacheWriteInputTokens === null ||
      normalized.outputTokens === null || normalized.totalTokens === null
    ) return { status: "inconclusive", reason: "incomplete_telemetry" };
    const usage: NormalizedUsage = {
      input_tokens: normalized.inputTokens,
      cached_tokens: normalized.cachedInputTokens,
      cache_write_tokens: normalized.cacheWriteInputTokens,
      output_tokens: normalized.outputTokens,
      total_tokens: normalized.totalTokens,
    };
    if (!sameUsage(raw, usage)) return { status: "inconclusive", reason: "incomplete_telemetry" };
    if (usage.input_tokens < 2_000 || usage.input_tokens > 4_000) {
      return { status: "inconclusive", reason: "invalid_input_size" };
    }
    const signalValue = cacheSignal(usage);
    if (!signalValue) return { status: "inconclusive", reason: "invalid_cache_signal" };
    return {
      status: "sample",
      signal: signalValue,
      sample: {
        slot: expectedSlot,
        raw_usage: raw,
        usage,
        elapsed_ms: Math.max(0, Math.round(performance.now() - startedAtMs)),
      },
    };
  } catch (error) {
    if (error instanceof CodexCacheScopeExperimentError) throw error;
    throw new CodexCacheScopeExperimentError(
      error instanceof Error
        ? `Prompt-cache scope experiment stream failed: ${error.message}`
        : "Prompt-cache scope experiment stream failed.",
    );
  } finally {
    try {
      await releaseCodexResponseProbe(response);
    } catch {
      // Probe release is best effort after terminal handling.
    }
  }
};

const classifyCycle = (
  model: string,
  samples: readonly PromptCacheScopeSample[],
  signals: readonly CacheSignal[],
): ConcreteScopeObservation | null => {
  if (samples.length !== PROMPT_CACHE_SCOPE_EXPERIMENT_SAMPLES_PER_CYCLE || signals.length !== samples.length) {
    return null;
  }
  if (
    samples.some((sample, index) => sample.slot !== CACHE_SCOPE_EXPECTED_SLOTS[index]) ||
    samples.some((sample) => sample.usage.input_tokens !== samples[0]?.usage.input_tokens)
  ) return null;
  const expected: Array<CacheSignal | null> = [
    "write",
    "read",
    null,
    "read",
    "read",
    null,
    "read",
    null,
    "read",
    "read",
  ];
  if (signals.some((signal, index) => expected[index] !== null && signal !== expected[index])) return null;
  const accountSlots = signals[2] === "read" ? "shared" : signals[2] === "write" ? "account_scoped" : null;
  const tokenRefresh = signals[5] === "read" ? "preserved" : signals[5] === "write" ? "changed" : null;
  const conversationId = signals[7] === "read" ? "independent" : signals[7] === "write" ? "scoped" : null;
  if (!accountSlots || !tokenRefresh || !conversationId) return null;
  return {
    probe_profile: PROMPT_CACHE_SCOPE_PROBE_PROFILE,
    account_slots: accountSlots,
    token_refresh: tokenRefresh,
    conversation_id: conversationId,
    effective_model: model,
  };
};

const INCONCLUSIVE_REASONS = new Set<InconclusiveReason>([
  "auth_pool_drift",
  "capability_changed",
  "cycle_disagreement",
  "effective_model_drift",
  "incomplete_telemetry",
  "invalid_cache_signal",
  "invalid_input_size",
  "inventory_drift",
  "lease_lost",
  "model_drift",
  "promotion_conflict",
  "refresh_unchanged",
  "runtime_drift",
  "session_expired",
  "slot_drift",
  "target_catalog_drift",
]);

const isNormalizedUsage = (value: unknown): value is NormalizedUsage =>
  isRecord(value) && !Array.isArray(value) &&
  hasExactKeys(value, ["input_tokens", "cached_tokens", "cache_write_tokens", "output_tokens", "total_tokens"]) &&
  isSafeNonNegativeInteger(value.input_tokens) &&
  isSafeNonNegativeInteger(value.cached_tokens) &&
  isSafeNonNegativeInteger(value.cache_write_tokens) &&
  isSafeNonNegativeInteger(value.output_tokens) &&
  isSafeNonNegativeInteger(value.total_tokens);

const isPromptCacheScopeSample = (value: unknown): value is PromptCacheScopeSample => {
  if (
    !isRecord(value) || Array.isArray(value) ||
    !hasExactKeys(value, ["step", "slot", "raw_usage", "usage", "elapsed_ms"]) ||
    typeof value.step !== "string" || !CACHE_SCOPE_STEP_NAMES.includes(value.step as CacheScopeStepName)
  ) return false;
  const stepIndex = CACHE_SCOPE_STEP_NAMES.indexOf(value.step as CacheScopeStepName);
  return value.slot === CACHE_SCOPE_EXPECTED_SLOTS[stepIndex] &&
    isNormalizedUsage(value.raw_usage) &&
    isNormalizedUsage(value.usage) &&
    sameUsage(value.raw_usage, value.usage) &&
    isSafeNonNegativeInteger(value.elapsed_ms);
};

const isCycleEvidence = (value: unknown, model: string): value is CycleEvidence => {
  if (
    !isRecord(value) || Array.isArray(value) ||
    !hasOnlyKeys(value, ["cycle", "samples", "classification", "inconclusive_reason"]) ||
    !hasOwn(value, "cycle") || !hasOwn(value, "samples") ||
    !isSafeNonNegativeInteger(value.cycle) || value.cycle < 1 || value.cycle > PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES ||
    !Array.isArray(value.samples) || value.samples.length > PROMPT_CACHE_SCOPE_EXPERIMENT_SAMPLES_PER_CYCLE ||
    !value.samples.every(isPromptCacheScopeSample) ||
    value.samples.some((sample, index) => sample.step !== CACHE_SCOPE_STEP_NAMES[index])
  ) return false;

  const hasClassification = hasOwn(value, "classification");
  const hasReason = hasOwn(value, "inconclusive_reason");
  if (hasClassification === hasReason) return false;
  if (!hasReason) {
    if (
      !isConcreteObservation(value.classification) || value.classification.effective_model !== model ||
      value.samples.length !== PROMPT_CACHE_SCOPE_EXPERIMENT_SAMPLES_PER_CYCLE
    ) return false;
    const signals: CacheSignal[] = [];
    for (const sample of value.samples) {
      const signal = cacheSignal(sample.usage);
      if (!signal) return false;
      signals.push(signal);
    }
    const classification = classifyCycle(model, value.samples, signals);
    return classification !== null && sameObservation(value.classification, classification);
  }
  return typeof value.inconclusive_reason === "string" &&
    INCONCLUSIVE_REASONS.has(value.inconclusive_reason as InconclusiveReason);
};

const parseEvidence = (value: unknown): StoredEvidence | null => {
  if (
    !isRecord(value) || Array.isArray(value) ||
    !hasOnlyKeys(value, [
      "v",
      "target",
      "outcome",
      "started_at_ms",
      "verified_at_ms",
      "cycles",
      "inconclusive_reason",
    ]) ||
    value.v !== 3
  ) return null;
  const target = parseTargetBinding(value.target);
  const outcome = String(value.outcome) as StoredEvidence["outcome"];
  const hasInconclusiveReason = hasOwn(value, "inconclusive_reason");
  const now = Date.now();
  if (
    !target || !isSafeNonNegativeInteger(value.started_at_ms) || !isSafeNonNegativeInteger(value.verified_at_ms) ||
    value.started_at_ms <= 0 || value.verified_at_ms < value.started_at_ms || value.verified_at_ms > now ||
    !Array.isArray(value.cycles) || value.cycles.length > PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES ||
    !["in_progress", "ready_to_promote", "completed", "inconclusive", "failed"].includes(outcome) ||
    (hasInconclusiveReason &&
      (typeof value.inconclusive_reason !== "string" ||
        !INCONCLUSIVE_REASONS.has(value.inconclusive_reason as InconclusiveReason))) ||
    !value.cycles.every((cycle, index) => isCycleEvidence(cycle, target.model) && cycle.cycle === index + 1)
  ) return null;
  const cycles = value.cycles as readonly CycleEvidence[];
  const allClassified = cycles.every((cycle) => cycle.classification !== undefined);
  const classificationsAgree = allClassified && cycles.length > 0 &&
    cycles.every((cycle) => sameObservation(cycle.classification!, cycles[0]!.classification!));
  const finalCycle = cycles.at(-1);
  const inconclusiveCyclesCoherent = cycles.length === 0 ||
    (cycles.slice(0, -1).every((cycle) => cycle.classification !== undefined) &&
      (finalCycle?.classification !== undefined ||
        finalCycle?.inconclusive_reason === value.inconclusive_reason));
  if (
    (outcome === "in_progress" &&
      (hasInconclusiveReason || !allClassified || cycles.length >= PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES)) ||
    ((outcome === "ready_to_promote" || outcome === "completed") &&
      (hasInconclusiveReason || cycles.length !== PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES || !classificationsAgree)) ||
    (outcome === "inconclusive" && (!hasInconclusiveReason || !inconclusiveCyclesCoherent)) ||
    (outcome === "failed" && (hasInconclusiveReason || !allClassified))
  ) return null;
  return {
    v: 3,
    target,
    outcome,
    started_at_ms: value.started_at_ms,
    verified_at_ms: value.verified_at_ms,
    cycles,
    ...(hasInconclusiveReason ? { inconclusive_reason: value.inconclusive_reason as InconclusiveReason } : {}),
  };
};

/** Active state and evidence are one atomic campaign record, never independent hints. */
const activeStateEvidenceIsConsistent = (state: ExperimentState, evidence: StoredEvidence | null): boolean => {
  if (state.classifications.length === 0) return evidence === null;
  if (
    !evidence || !sameTargetDefinition(evidence.target, state.target) ||
    evidence.started_at_ms !== state.started_at_ms || evidence.cycles.length !== state.classifications.length ||
    !evidence.cycles.every((cycle, index) =>
      cycle.classification !== undefined && sameObservation(cycle.classification, state.classifications[index]!)
    )
  ) return false;
  return evidence.outcome ===
    (state.next_cycle === PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES + 1 ? "ready_to_promote" : "in_progress");
};

const throwIfAborted = (signal: AbortSignal): void => {
  if (signal.aborted) {
    throw signal.reason ?? new DOMException("Prompt-cache scope experiment was aborted.", "AbortError");
  }
};

type RuntimeBinding = Readonly<{ versionstamp: string; default_model: string }>;
type ResolvedScopeTarget = Readonly<{ binding: PromptCacheScopeTargetBinding }>;
type ScopeTargetResolution =
  | Readonly<{ status: "resolved"; value: ResolvedScopeTarget }>
  | Readonly<{ status: "inconclusive"; reason: InconclusiveReason }>;

const loadRuntimeBinding = async (kv: Deno.Kv): Promise<RuntimeBinding | null> => {
  const entry = await kv.get<RuntimeConfigV2>(RUNTIME_CONFIG_V2_KEY, { consistency: "strong" });
  const runtime = normalizeRuntimeConfig(entry.value);
  if (!runtime || !entry.versionstamp) return null;
  return { versionstamp: entry.versionstamp, default_model: runtime.default_model };
};

const targetBindingFromInventory = (
  inventory: PromptCacheScopeTargetInventory,
  target: PromptCacheScopeTarget,
  runtime: RuntimeBinding,
): PromptCacheScopeTargetBinding | null => {
  const inventoryFingerprint = getString(inventory.inventory_fingerprint)?.trim();
  const catalogVersionstamp = getString(target.catalog_versionstamp)?.trim();
  const authPoolVersionstamp = getString(target.codex_auth_pool_versionstamp)?.trim();
  const authPoolIdentityFingerprint = getString(target.codex_auth_pool_identity_fingerprint)?.trim();
  const model = target.model.trim();
  const targetId = target.id.trim();
  const capabilityFingerprint = target.capability_fingerprint.trim();
  const clientVersion = target.catalog_client_version === null ? null : target.catalog_client_version.trim() || null;
  if (
    inventory.status !== "ready" || !inventoryFingerprint || !catalogVersionstamp || !authPoolVersionstamp ||
    !authPoolIdentityFingerprint || !model ||
    !targetId || !capabilityFingerprint || target.provider !== PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER ||
    target.telemetry_provider !== CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER ||
    target.topology.kind !== "codex_account_pool" || target.probeability.status !== "probeable"
  ) return null;
  return {
    id: targetId,
    provider: PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER,
    telemetry_provider: CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER,
    topology_kind: "codex_account_pool",
    model,
    probe_profile: PROMPT_CACHE_SCOPE_PROBE_PROFILE,
    capability_fingerprint: capabilityFingerprint,
    inventory_fingerprint: inventoryFingerprint,
    catalog_versionstamp: catalogVersionstamp,
    runtime_versionstamp: runtime.versionstamp,
    auth_pool_versionstamp: authPoolVersionstamp,
    auth_pool_identity_fingerprint: authPoolIdentityFingerprint,
    catalog_client_version: clientVersion,
  };
};

const sameTargetCore = (left: PromptCacheScopeTargetBinding, right: PromptCacheScopeTargetBinding): boolean =>
  left.id === right.id && left.provider === right.provider && left.telemetry_provider === right.telemetry_provider &&
  left.topology_kind === right.topology_kind && left.model === right.model &&
  left.probe_profile === right.probe_profile && left.capability_fingerprint === right.capability_fingerprint &&
  left.auth_pool_identity_fingerprint === right.auth_pool_identity_fingerprint &&
  left.catalog_client_version === right.catalog_client_version;

const loadInventoryAndRuntime = async (
  kv: Deno.Kv,
): Promise<Readonly<{ inventory: PromptCacheScopeTargetInventory; runtime: RuntimeBinding }> | null> => {
  const [inventory, runtime] = await Promise.all([
    loadPromptCacheScopeTargetInventory({ kv }),
    loadRuntimeBinding(kv),
  ]);
  if (inventory.status !== "ready" || !runtime) return null;
  return { inventory, runtime };
};

/**
 * Re-read the canonical target before a paid sample. Runtime/default-model
 * changes are harmless when the exact target is stable. Catalog capability,
 * inventory, auth-pool, and client-version bindings instead fence dispatch.
 */
const resolveBoundTarget = async (
  kv: Deno.Kv,
  expected: PromptCacheScopeTargetBinding,
): Promise<ScopeTargetResolution> => {
  const loaded = await loadInventoryAndRuntime(kv);
  if (!loaded) return { status: "inconclusive", reason: "target_catalog_drift" };
  const current = loaded.inventory.targets.find((target) => target.id === expected.id);
  const currentBinding = current ? targetBindingFromInventory(loaded.inventory, current, loaded.runtime) : null;
  if (!currentBinding) {
    return { status: "inconclusive", reason: "target_catalog_drift" };
  }
  if (currentBinding.auth_pool_identity_fingerprint !== expected.auth_pool_identity_fingerprint) {
    return { status: "inconclusive", reason: "auth_pool_drift" };
  }
  if (!sameTargetCore(currentBinding, expected)) return { status: "inconclusive", reason: "target_catalog_drift" };
  if (currentBinding.inventory_fingerprint !== expected.inventory_fingerprint) {
    return { status: "inconclusive", reason: "inventory_drift" };
  }
  if (currentBinding.auth_pool_versionstamp !== expected.auth_pool_versionstamp) {
    return { status: "inconclusive", reason: "auth_pool_drift" };
  }
  return { status: "resolved", value: { binding: currentBinding } };
};

type SelectedCampaignTarget = Readonly<{ binding: PromptCacheScopeTargetBinding; active_state?: ExperimentState }>;

/**
 * The public route has no target selector. It resumes an existing campaign or
 * chooses the first nonterminal probeable Codex target from the canonical
 * stable ordering. Malformed durable state/evidence is a hard no-dispatch
 * condition rather than an invitation to overwrite it.
 */
const selectCampaignTarget = async (
  kv: Deno.Kv,
  inventory: PromptCacheScopeTargetInventory,
  runtime: RuntimeBinding,
): Promise<SelectedCampaignTarget> => {
  const bindings = inventory.targets
    .map((target) => targetBindingFromInventory(inventory, target, runtime))
    .filter((target): target is PromptCacheScopeTargetBinding => target !== null);
  if (!bindings.length) {
    throw new PromptCacheScopeExperimentUnavailableError(
      "Prompt-cache scope experiment has no probeable Codex target in the current inventory.",
    );
  }

  const records = await Promise.all(bindings.map(async (binding) => {
    const [stateEntry, evidenceEntry] = await Promise.all([
      kv.get<ExperimentState>(stateKey(binding), { consistency: "strong" }),
      kv.get<StoredEvidence>(evidenceKey(binding), { consistency: "strong" }),
    ]);
    const state = stateEntry.value === null ? null : parseState(stateEntry.value);
    const evidence = evidenceEntry.value === null ? null : parseEvidence(evidenceEntry.value);
    if ((stateEntry.value !== null && !state) || (evidenceEntry.value !== null && !evidence)) {
      throw new PromptCacheScopeExperimentUnavailableError(
        "Prompt-cache scope experiment has malformed target-scoped durable state.",
      );
    }
    if (state && !activeStateEvidenceIsConsistent(state, evidence)) {
      throw new PromptCacheScopeExperimentUnavailableError(
        "Prompt-cache scope experiment has inconsistent target-scoped durable evidence.",
      );
    }
    return { binding, state, evidence };
  }));

  const active = records.filter((record) => record.state && record.state.expires_at_ms > Date.now());
  if (active.length > 1) {
    throw new PromptCacheScopeExperimentUnavailableError(
      "Prompt-cache scope experiment found more than one active provider campaign.",
    );
  }
  if (active.length === 1) {
    const record = active[0]!;
    const state = record.state!;
    if (!sameTargetCore(state.target, record.binding)) {
      throw new PromptCacheScopeExperimentUnavailableError(
        "Prompt-cache scope experiment target changed while its campaign was active.",
      );
    }
    if (state.target.inventory_fingerprint !== record.binding.inventory_fingerprint) {
      throw new PromptCacheScopeExperimentUnavailableError(
        "Prompt-cache scope experiment inventory changed while its campaign was active.",
      );
    }
    return { binding: record.binding, active_state: state };
  }

  for (const record of records) {
    const terminal = record.evidence && record.evidence.started_at_ms > 0 &&
      sameTargetDefinition(record.evidence.target, record.binding) &&
      record.evidence.outcome === "completed";
    if (!terminal) return { binding: record.binding };
  }
  throw new PromptCacheScopeExperimentUnavailableError(
    "Prompt-cache scope experiment has no nonterminal probeable Codex target in the current inventory.",
  );
};

/**
 * The live matrix is a paid, stateful control-plane action. Keep its Stage 0
 * prerequisite at the public admin boundary so direct unit fixtures can
 * exercise the fenced runner without inventing a deployed release baseline.
 */
export const assertPromptCacheScopeExperimentTelemetryBaseline = async (
  options: PromptCacheScopeExperimentTelemetryBaselineTestOptions = {},
): Promise<PromptCacheScopeExperimentTelemetryBaseline> => {
  const kv = options.kv === undefined ? await getKv() : options.kv;
  if (!kv) {
    throw new PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiments require Deno KV.");
  }
  const loaded = await loadInventoryAndRuntime(kv);
  if (!loaded) {
    throw new PromptCacheScopeExperimentUnavailableError(
      "Prompt-cache scope experiment requires a current full target inventory and runtime configuration.",
    );
  }
  const selected = await selectCampaignTarget(kv, loaded.inventory, loaded.runtime);
  const resolved = await resolveBoundTarget(kv, selected.binding);
  if (resolved.status !== "resolved") {
    throw new PromptCacheScopeExperimentUnavailableError(
      "Prompt-cache scope experiment target changed before its Stage 0 baseline could be checked.",
    );
  }
  const baseline = await readPromptCacheTelemetryBaseline(
    { provider: CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER, model: resolved.value.binding.model },
    { ...options, kv },
  );
  // The live matrix uses the Responses transport. Aggregate telemetry or a
  // Chat-only cohort cannot establish that its terminal usage parser is ready
  // for paid Responses samples.
  const responsesRoute = baseline.routes.find((route) => route.route === "responses");
  if (
    baseline.status !== "eligible" || !responsesRoute?.completed_minimum_passed ||
    !responsesRoute.reported_coverage_passed || !responsesRoute.cache_write_reported_coverage_passed
  ) {
    throw new PromptCacheScopeExperimentUnavailableError(
      "Prompt-cache scope experiment requires a passing current-release Stage 0 telemetry baseline.",
    );
  }
  return { target: resolved.value.binding };
};

type AcquiredCycle = Readonly<{ state: ExperimentState; cycle_owner: string }>;

const hasActiveLease = (value: unknown): boolean =>
  isRecord(value) && isSafeNonNegativeInteger(value.lease_until_ms) && value.lease_until_ms > Date.now();

const acquireCycle = async (
  kv: Deno.Kv,
  target: PromptCacheScopeTargetBinding,
  authPoolVersionstamp: string,
): Promise<AcquiredCycle> => {
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const [stateEntry, evidenceEntry, campaignEntry, cycleEntry] = await Promise.all([
      kv.get<ExperimentState>(stateKey(target), { consistency: "strong" }),
      kv.get<StoredEvidence>(evidenceKey(target), { consistency: "strong" }),
      kv.get<CampaignLease>(campaignLeaseKey(target), { consistency: "strong" }),
      kv.get<ExperimentLease>(cycleLeaseKey(target), { consistency: "strong" }),
    ]);
    const existing = parseState(stateEntry.value);
    const existingEvidence = evidenceEntry.value === null ? null : parseEvidence(evidenceEntry.value);
    if (stateEntry.value !== null && !existing) {
      throw new PromptCacheScopeExperimentUnavailableError(
        "Prompt-cache scope experiment has malformed target-scoped state.",
      );
    }
    if (evidenceEntry.value !== null && !existingEvidence) {
      throw new PromptCacheScopeExperimentUnavailableError(
        "Prompt-cache scope experiment has malformed target-scoped evidence.",
      );
    }
    // An active campaign must never pick up evidence from another target or
    // session. Detect this before claiming a lease, so a corrupted durable
    // record cannot be overwritten after a paid dispatch has started.
    if (existing && !activeStateEvidenceIsConsistent(existing, existingEvidence)) {
      throw new PromptCacheScopeExperimentUnavailableError(
        "Prompt-cache scope experiment has inconsistent target-scoped durable evidence.",
      );
    }
    const now = Date.now();
    const activeState = existing && existing.expires_at_ms > now ? existing : null;
    const resettingPriorSession = !activeState && (stateEntry.value !== null || evidenceEntry.value !== null);
    if (hasActiveLease(cycleEntry.value)) throw new PromptCacheScopeExperimentBusyError();

    let state: ExperimentState;
    let campaignOwner: string;
    if (activeState) {
      if (
        !sameTargetCore(activeState.target, target) ||
        activeState.target.inventory_fingerprint !== target.inventory_fingerprint
      ) {
        throw new PromptCacheScopeExperimentUnavailableError(
          "Prompt-cache scope experiment target changed while its campaign was active.",
        );
      }
      if (!ownsCampaignLease(campaignEntry.value, activeState)) {
        if (hasActiveLease(campaignEntry.value)) throw new PromptCacheScopeExperimentBusyError();
        throw new PromptCacheScopeExperimentUnavailableError(
          "Prompt-cache scope experiment lost its provider campaign lease.",
        );
      }
      campaignOwner = activeState.campaign_owner;
      state = { ...activeState, target };
    } else {
      if (hasActiveLease(campaignEntry.value)) throw new PromptCacheScopeExperimentBusyError();
      campaignOwner = crypto.randomUUID();
      state = {
        v: 3,
        target,
        campaign_owner: campaignOwner,
        started_at_ms: now,
        expires_at_ms: now + PROMPT_CACHE_SCOPE_EXPERIMENT_SESSION_MS,
        auth_pool_versionstamp: authPoolVersionstamp,
        next_cycle: 1,
        classifications: [],
      };
    }

    const cycleOwner = crypto.randomUUID();
    const campaignLease: CampaignLease = {
      owner: campaignOwner,
      target_id: state.target.id,
      inventory_fingerprint: state.target.inventory_fingerprint,
      lease_until_ms: state.expires_at_ms,
    };
    const atomic = kv.atomic()
      .check(stateEntry)
      .check(evidenceEntry)
      .check(campaignEntry)
      .check(cycleEntry)
      .set(
        cycleLeaseKey(target),
        { owner: cycleOwner, lease_until_ms: now + PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS } satisfies ExperimentLease,
        { expireIn: PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS * 2 },
      )
      .set(campaignLeaseKey(target), campaignLease, { expireIn: Math.max(1, state.expires_at_ms - now) })
      .set(stateKey(target), state, { expireIn: Math.max(1, state.expires_at_ms - now) });
    if (resettingPriorSession) atomic.delete(evidenceKey(target));
    const committed = await atomic.commit();
    if (committed.ok) return { state, cycle_owner: cycleOwner };
  }
  throw new PromptCacheScopeExperimentBusyError();
};

const renewLease = async (kv: Deno.Kv, state: ExperimentState, cycleOwner: string): Promise<void> => {
  const cycleKey = cycleLeaseKey(state.target);
  const campaignKey = campaignLeaseKey(state.target);
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const [cycleEntry, campaignEntry] = await Promise.all([
      kv.get<ExperimentLease>(cycleKey, { consistency: "strong" }),
      kv.get<CampaignLease>(campaignKey, { consistency: "strong" }),
    ]);
    if (!ownsLease(cycleEntry.value, cycleOwner) || !ownsCampaignLease(campaignEntry.value, state)) {
      throw new PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment lost its lease.");
    }
    const now = Date.now();
    const commit = await kv.atomic()
      .check(cycleEntry)
      .check(campaignEntry)
      .set(
        cycleKey,
        { owner: cycleOwner, lease_until_ms: now + PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS } satisfies ExperimentLease,
        { expireIn: PROMPT_CACHE_SCOPE_EXPERIMENT_LEASE_MS * 2 },
      )
      .set(
        campaignKey,
        {
          owner: state.campaign_owner,
          target_id: state.target.id,
          inventory_fingerprint: state.target.inventory_fingerprint,
          lease_until_ms: state.expires_at_ms,
        } satisfies CampaignLease,
        { expireIn: Math.max(1, state.expires_at_ms - now) },
      )
      .commit();
    if (commit.ok) return;
  }
  throw new PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment could not renew its lease.");
};

const existingCycles = (state: ExperimentState, evidenceValue: unknown): readonly CycleEvidence[] => {
  const evidence = parseEvidence(evidenceValue);
  if (!activeStateEvidenceIsConsistent(state, evidence)) {
    throw new PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment evidence is inconsistent.");
  }
  return evidence?.cycles ?? [];
};

const persistIntermediate = async (
  kv: Deno.Kv,
  state: ExperimentState,
  cycleOwner: string,
  cycle: CycleEvidence,
  nextState: ExperimentState,
): Promise<void> => {
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const [stateEntry, evidenceEntry, cycleEntry, campaignEntry] = await Promise.all([
      kv.get<ExperimentState>(stateKey(state.target), { consistency: "strong" }),
      kv.get<StoredEvidence>(evidenceKey(state.target), { consistency: "strong" }),
      kv.get<ExperimentLease>(cycleLeaseKey(state.target), { consistency: "strong" }),
      kv.get<CampaignLease>(campaignLeaseKey(state.target), { consistency: "strong" }),
    ]);
    if (!ownsLease(cycleEntry.value, cycleOwner) || !ownsCampaignLease(campaignEntry.value, state)) {
      throw new PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment lost its lease.");
    }
    const persistedState = parseState(stateEntry.value);
    if (
      !persistedState || persistedState.started_at_ms !== state.started_at_ms ||
      persistedState.next_cycle !== state.next_cycle || persistedState.campaign_owner !== state.campaign_owner ||
      !sameTargetDefinition(persistedState.target, state.target)
    ) {
      throw new PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment state changed concurrently.");
    }
    const cycles = [...existingCycles(state, evidenceEntry.value), cycle];
    const evidence: StoredEvidence = {
      v: 3,
      target: nextState.target,
      outcome: nextState.next_cycle === PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES + 1 ? "ready_to_promote" : "in_progress",
      started_at_ms: state.started_at_ms,
      verified_at_ms: Date.now(),
      cycles,
    };
    const atomic = kv.atomic()
      .check(stateEntry)
      .check(evidenceEntry)
      .check(cycleEntry)
      .check(campaignEntry)
      .set(stateKey(nextState.target), nextState, { expireIn: Math.max(1, nextState.expires_at_ms - Date.now()) })
      .set(
        evidenceKey(nextState.target),
        evidence,
        { expireIn: Math.max(1, nextState.expires_at_ms - Date.now()) },
      );
    if (nextState.next_cycle <= PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES) atomic.delete(cycleLeaseKey(state.target));
    const commit = await atomic.commit();
    if (commit.ok) return;
  }
  throw new PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment evidence could not be persisted.");
};

const finalize = async (
  kv: Deno.Kv,
  state: ExperimentState,
  cycleOwner: string,
  outcome: "completed" | "inconclusive" | "failed",
  options: Readonly<{
    scope?: ConcreteScopeObservation;
    reason?: InconclusiveReason;
    cycle?: CycleEvidence;
    completedCycles?: number;
  }> = {},
): Promise<PromptCacheScopeExperimentResult> => {
  for (let attempt = 0; attempt < 3; attempt += 1) {
    const [stateEntry, evidenceEntry, cycleEntry, campaignEntry] = await Promise.all([
      kv.get<ExperimentState>(stateKey(state.target), { consistency: "strong" }),
      kv.get<StoredEvidence>(evidenceKey(state.target), { consistency: "strong" }),
      kv.get<ExperimentLease>(cycleLeaseKey(state.target), { consistency: "strong" }),
      kv.get<CampaignLease>(campaignLeaseKey(state.target), { consistency: "strong" }),
    ]);
    if (!ownsLease(cycleEntry.value, cycleOwner) || !ownsCampaignLease(campaignEntry.value, state)) {
      throw new PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment lost its lease.");
    }
    const persistedState = parseState(stateEntry.value);
    if (
      !persistedState || persistedState.started_at_ms !== state.started_at_ms ||
      persistedState.campaign_owner !== state.campaign_owner ||
      !sameTargetDefinition(persistedState.target, state.target)
    ) {
      throw new PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment state changed concurrently.");
    }
    const prior = parseEvidence(evidenceEntry.value);
    if (!activeStateEvidenceIsConsistent(persistedState, prior)) {
      throw new PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment evidence changed concurrently.");
    }
    const cycles = [
      ...(prior?.cycles ?? []),
      ...(options.cycle ? [options.cycle] : []),
    ];
    const evidence: StoredEvidence = {
      v: 3,
      target: state.target,
      outcome,
      started_at_ms: state.started_at_ms,
      verified_at_ms: Date.now(),
      cycles,
      ...(options.reason ? { inconclusive_reason: options.reason } : {}),
    };
    const commit = await kv.atomic()
      .check(stateEntry)
      .check(evidenceEntry)
      .check(cycleEntry)
      .check(campaignEntry)
      // Terminal evidence is the durable per-target campaign ledger. It is
      // redacted and must outlive the 15-minute active session so later
      // bodyless invocations can advance to sibling targets rather than
      // silently reprobe an already-terminal one.
      .set(evidenceKey(state.target), evidence)
      .delete(stateKey(state.target))
      .delete(cycleLeaseKey(state.target))
      .delete(campaignLeaseKey(state.target))
      .commit();
    if (!commit.ok) continue;
    return {
      provider: PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER,
      telemetry_provider: CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER,
      target_id: state.target.id,
      model: state.target.model,
      status: outcome === "completed" ? "completed" : "inconclusive",
      completed_cycles: options.completedCycles ??
        state.classifications.length + (options.cycle?.classification ? 1 : 0),
      verified_at_ms: evidence.verified_at_ms,
      ...(options.scope ? { scope: options.scope } : {}),
      ...(options.reason ? { inconclusive_reason: options.reason } : {}),
    };
  }
  throw new PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment final result could not be persisted.");
};

const runCycle = async (
  kv: Deno.Kv,
  state: ExperimentState,
  cycleOwner: string,
  cycleNumber: number,
  initialSession: Awaited<ReturnType<typeof beginCodexCacheScopeExperiment>>,
): Promise<
  Readonly<{
    evidence: CycleEvidence;
    session: Awaited<ReturnType<typeof beginCodexCacheScopeExperiment>>;
    binding: PromptCacheScopeTargetBinding;
  }>
> => {
  const cycleSignal = AbortSignal.timeout(PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLE_DEADLINE_MS);
  const body = buildExperimentRequest(
    state.target.model,
    crypto.randomUUID(),
    `uos-cache-scope-v3-${crypto.randomUUID()}`,
  );
  const expectedBody = JSON.stringify(body);
  const conversationA = crypto.randomUUID();
  const conversationB = crypto.randomUUID();
  const conversations = [
    conversationA,
    conversationA,
    conversationA,
    conversationA,
    conversationA,
    conversationA,
    conversationA,
    conversationB,
    conversationB,
    conversationA,
  ] as const;
  const samples: PromptCacheScopeSample[] = [];
  const signals: CacheSignal[] = [];
  let session = initialSession;
  let binding = state.target;
  const inconclusive = (reason: InconclusiveReason): Readonly<{
    evidence: CycleEvidence;
    session: Awaited<ReturnType<typeof beginCodexCacheScopeExperiment>>;
    binding: PromptCacheScopeTargetBinding;
  }> => ({
    evidence: { cycle: cycleNumber, samples, inconclusive_reason: reason },
    session,
    binding,
  });

  for (let index = 0; index < PROMPT_CACHE_SCOPE_EXPERIMENT_SAMPLES_PER_CYCLE; index += 1) {
    throwIfAborted(cycleSignal);
    if (index === 5) {
      const beforeRefresh = await resolveBoundTarget(kv, binding);
      if (beforeRefresh.status !== "resolved") return inconclusive(beforeRefresh.reason);
      binding = beforeRefresh.value.binding;
      await renewLease(kv, state, cycleOwner);
      const refresh = await refreshCodexCacheScopeExperimentSlot(session, 1, cycleSignal);
      if (refresh.status === "auth_pool_drift") return inconclusive("auth_pool_drift");
      session = refresh.session;
      if (!refresh.tokenChanged) return inconclusive("refresh_unchanged");
      binding = { ...binding, auth_pool_versionstamp: session.authPoolVersionstamp };
    }
    await renewLease(kv, state, cycleOwner);
    // Every paid sample re-reads the canonical inventory. Exact target
    // capability, inventory, auth-pool, and client-version drift stops before
    // dispatch; a runtime default-only change simply refreshes the binding.
    const currentTarget = await resolveBoundTarget(kv, binding);
    if (currentTarget.status !== "resolved") return inconclusive(currentTarget.reason);
    binding = currentTarget.value.binding;
    if (JSON.stringify(body) !== expectedBody) {
      throw new PromptCacheScopeExperimentFailedError("Prompt-cache scope experiment request body drifted.");
    }
    const startedAtMs = performance.now();
    const sampleSignal = AbortSignal.any([
      cycleSignal,
      AbortSignal.timeout(PROMPT_CACHE_SCOPE_EXPERIMENT_SAMPLE_DEADLINE_MS),
    ]);
    const response = await fetchCodexResponsesForCacheScopeExperiment(body, {
      session,
      slot: CACHE_SCOPE_EXPECTED_SLOTS[index]!,
      conversationId: conversations[index]!,
      clientVersion: binding.catalog_client_version,
      signal: sampleSignal,
    });
    const parsed = await readPromptCacheScopeExperimentCompletedUsage(
      response,
      CACHE_SCOPE_EXPECTED_SLOTS[index]!,
      binding.model,
      startedAtMs,
      sampleSignal,
    );
    if (parsed.status === "inconclusive") return inconclusive(parsed.reason);
    samples.push({ step: CACHE_SCOPE_STEP_NAMES[index]!, ...parsed.sample });
    signals.push(parsed.signal);
  }
  const classification = classifyCycle(binding.model, samples, signals);
  return classification
    ? { evidence: { cycle: cycleNumber, samples, classification }, session, binding }
    : inconclusive("slot_drift");
};

const resultForIntermediate = (state: ExperimentState): PromptCacheScopeExperimentResult => ({
  provider: PROMPT_CACHE_SCOPE_EXPERIMENT_PROVIDER,
  telemetry_provider: CODEX_CHATGPT_PROMPT_CACHE_TELEMETRY_PROVIDER,
  target_id: state.target.id,
  model: state.target.model,
  status: "in_progress",
  completed_cycles: state.classifications.length,
  verified_at_ms: Date.now(),
});

/**
 * Runs exactly one fixed ten-row cycle per invocation. The route is bodyless;
 * model, slots, prompt key, conversations, and provider stay gateway-owned.
 * The caller must supply the Stage 0 attestation it just read. The runner
 * fences target identity and refreshes only benign runtime-default changes.
 */
export const runPromptCacheScopeExperiment = async (
  telemetryBaseline: PromptCacheScopeExperimentTelemetryBaseline,
): Promise<PromptCacheScopeExperimentResult> => {
  const kv = await getKv();
  if (!kv) {
    throw new PromptCacheScopeExperimentUnavailableError("Prompt-cache scope experiments require Deno KV.");
  }
  const baselineTarget = parseTargetBinding(telemetryBaseline.target);
  if (!baselineTarget) {
    throw new PromptCacheScopeExperimentUnavailableError(
      "Prompt-cache scope experiment requires a current-release Stage 0 telemetry baseline.",
    );
  }
  // Close the Stage 0 TOCTOU window before acquiring an OAuth-mutating
  // campaign lease. A bodyless request may never resolve a different target.
  const preflight = await resolveBoundTarget(kv, baselineTarget);
  if (preflight.status !== "resolved") {
    throw new PromptCacheScopeExperimentUnavailableError(
      "Prompt-cache scope experiment target changed after its Stage 0 telemetry baseline.",
    );
  }
  const initialSession = await beginCodexCacheScopeExperiment();
  const acquired = await acquireCycle(kv, preflight.value.binding, initialSession.authPoolVersionstamp);
  const { state, cycle_owner: cycleOwner } = acquired;
  let promotionSucceeded = false;

  try {
    if (state.expires_at_ms <= Date.now()) {
      return await finalize(kv, state, cycleOwner, "inconclusive", { reason: "session_expired" });
    }
    if (state.auth_pool_versionstamp !== initialSession.authPoolVersionstamp) {
      return await finalize(kv, state, cycleOwner, "inconclusive", { reason: "auth_pool_drift" });
    }
    if (!sameTargetCore(state.target, preflight.value.binding)) {
      return await finalize(kv, state, cycleOwner, "inconclusive", { reason: "target_catalog_drift" });
    }

    const rebound = await resolveBoundTarget(kv, state.target);
    if (rebound.status !== "resolved") {
      return await finalize(kv, state, cycleOwner, "inconclusive", { reason: rebound.reason });
    }
    const boundState: ExperimentState = { ...state, target: rebound.value.binding };

    if (state.next_cycle === PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES + 1) {
      const scope = state.pending_scope;
      if (!scope) return await finalize(kv, boundState, cycleOwner, "inconclusive", { reason: "cycle_disagreement" });
      const promotion = await promoteCodexPromptCacheScope(kv, {
        model: boundState.target.model,
        scope: {
          ...scope,
          reproducible_cycles: PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES,
          source: "live_probe",
          verified_at_ms: Date.now(),
        } satisfies PromptCacheScope,
        lease: { key: cycleLeaseKey(boundState.target), owner: cycleOwner },
        authPoolVersionstamp: boundState.auth_pool_versionstamp,
        catalogVersionstamp: boundState.target.catalog_versionstamp,
        runtimeVersionstamp: boundState.target.runtime_versionstamp,
      });
      if (promotion.status === "promoted") {
        promotionSucceeded = true;
        return await finalize(kv, boundState, cycleOwner, "completed", { scope });
      }
      return await finalize(kv, boundState, cycleOwner, "inconclusive", {
        reason: promotion.reason === "model_drift" || promotion.reason === "catalog_drift" ||
            promotion.reason === "capability_changed"
          ? "target_catalog_drift"
          : promotion.reason === "auth_pool_drift"
          ? "auth_pool_drift"
          : promotion.reason === "runtime_drift"
          ? "runtime_drift"
          : "promotion_conflict",
      });
    }

    const cycle = await runCycle(
      kv,
      boundState,
      cycleOwner,
      boundState.next_cycle,
      initialSession,
    );
    const classification = cycle.evidence.classification;
    if (!classification) {
      return await finalize(kv, boundState, cycleOwner, "inconclusive", {
        reason: cycle.evidence.inconclusive_reason ?? "incomplete_telemetry",
        cycle: cycle.evidence,
      });
    }
    const classifications = [...boundState.classifications, classification];
    const nextCycle = boundState.next_cycle + 1;
    const agreed = classifications.length === PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES &&
      classifications.every((entry) => sameObservation(entry, classifications[0]!));
    if (classifications.length === PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES && !agreed) {
      return await finalize(kv, boundState, cycleOwner, "inconclusive", {
        reason: "cycle_disagreement",
        cycle: cycle.evidence,
        completedCycles: classifications.length,
      });
    }
    const nextState: ExperimentState = {
      ...boundState,
      target: cycle.binding,
      auth_pool_versionstamp: cycle.session.authPoolVersionstamp,
      next_cycle: nextCycle,
      classifications,
      ...(nextCycle === PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES + 1 ? { pending_scope: classification } : {}),
    };
    await persistIntermediate(kv, boundState, cycleOwner, cycle.evidence, nextState);
    if (nextCycle <= PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES) return resultForIntermediate(nextState);

    const promotion = await promoteCodexPromptCacheScope(kv, {
      model: nextState.target.model,
      scope: {
        ...classification,
        reproducible_cycles: PROMPT_CACHE_SCOPE_EXPERIMENT_CYCLES,
        source: "live_probe",
        verified_at_ms: Date.now(),
      } satisfies PromptCacheScope,
      lease: { key: cycleLeaseKey(nextState.target), owner: cycleOwner },
      authPoolVersionstamp: nextState.auth_pool_versionstamp,
      catalogVersionstamp: nextState.target.catalog_versionstamp,
      runtimeVersionstamp: nextState.target.runtime_versionstamp,
    });
    if (promotion.status === "promoted") {
      promotionSucceeded = true;
      return await finalize(kv, nextState, cycleOwner, "completed", { scope: classification });
    }
    return await finalize(kv, nextState, cycleOwner, "inconclusive", {
      reason: promotion.reason === "model_drift" || promotion.reason === "catalog_drift" ||
          promotion.reason === "capability_changed"
        ? "target_catalog_drift"
        : promotion.reason === "auth_pool_drift"
        ? "auth_pool_drift"
        : promotion.reason === "runtime_drift"
        ? "runtime_drift"
        : "promotion_conflict",
    });
  } catch (error) {
    if (
      error instanceof PromptCacheScopeExperimentBusyError ||
      error instanceof PromptCacheScopeExperimentUnavailableError
    ) {
      throw error;
    }
    const failure = error instanceof PromptCacheScopeExperimentFailedError
      ? error
      : error instanceof CodexCacheScopeExperimentError
      ? new PromptCacheScopeExperimentFailedError(error.message)
      : new PromptCacheScopeExperimentFailedError(
        error instanceof Error ? error.message : "Prompt-cache scope experiment cycle failed.",
      );
    if (!promotionSucceeded) {
      try {
        await finalize(kv, state, cycleOwner, "failed");
      } catch {
        // A lost lease already fences this run. Its short expiry is the safe
        // fallback when terminal evidence cannot be persisted.
      }
    }
    throw failure;
  }
};
