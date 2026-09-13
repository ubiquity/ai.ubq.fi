import { getKv } from "./kv.ts";
import { readCodexResetUsage as readBankedResetUsage } from "./codex_reset_settings.ts";
import {
  type CodexUsageResetProvider,
  providerReceiptIdsSafeToPersistAndLog,
  providerSupportsLiveRedemption,
  providerSupportsResetType,
  providerTreatsRedeemOutcomeAsFinal,
  type RedeemResetResult,
  type ResetAccountContext,
  type ResetInventory,
  type ResetInventoryCredit,
} from "./codex_banked_reset_provider.ts";
import type { CodexResetGlobalDailyRecord, CodexResetRedemptionRecord, CodexResetRedemptionState, CodexResetShadowDecisionRecord } from "./types.ts";
import { isRecord, sha256Hex } from "./utils.ts";

/**
 * A banked reset is an externally visible mutation. Its ledger deliberately
 * does not share the routing key: a routing update must never erase a record
 * describing a provider-side redemption that may still need reconciliation.
 */
export const CODEX_RESET_REDEMPTION_KV_PREFIX = ["uos_ai", "codex_reset_redemption", "v1"] as const;
export const CODEX_RESET_GLOBAL_DAILY_KV_PREFIX = ["uos_ai", "codex_reset_redemption", "global_day", "v1"] as const;
export const CODEX_RESET_SHADOW_DECISION_KV_PREFIX = ["uos_ai", "codex_reset_shadow_decision", "v1"] as const;
export const CODEX_BANKED_RESET_LEASE_MS = 30_000;
/** Inventory is an authorization input for an external spend, not a cache. */
export const CODEX_BANKED_RESET_INVENTORY_MAX_AGE_MS = 30_000;
/** A reset preflight may not hold an otherwise healthy fallback indefinitely. */
export const CODEX_BANKED_RESET_INVENTORY_TIMEOUT_MS = 5_000;
const MAX_CAS_ATTEMPTS = 4;

export const codexResetRedemptionKey = (accountIdHash: string, quotaGeneration: string): Deno.KvKey => [
  ...CODEX_RESET_REDEMPTION_KV_PREFIX,
  accountIdHash,
  quotaGeneration,
];

export const codexResetGlobalDailyKey = (day: string): Deno.KvKey => [...CODEX_RESET_GLOBAL_DAILY_KV_PREFIX, day];

export const codexResetShadowDecisionKey = (episodeHash: string): Deno.KvKey => [...CODEX_RESET_SHADOW_DECISION_KV_PREFIX, episodeHash];

export type CodexBankedResetMode = "disabled" | "shadow" | "live";

export type CodexBankedResetConfig = Readonly<{
  enabled: boolean;
  mode: CodexBankedResetMode;
  maxGlobalPerDay: number;
  maxPerAccountPerWindow: number;
}>;

const getEnv = (key: string): string | undefined => {
  try {
    return Deno.env.get(key);
  } catch {
    return undefined;
  }
};

const parseStrictBoolean = (value: string | undefined, defaultValue: boolean): boolean =>
  value === undefined ? defaultValue : value.trim().toLowerCase() === "true";

const parseNonNegativeInteger = (value: string | undefined, defaultValue: number): number => {
  if (value === undefined) return defaultValue;
  const normalized = value.trim();
  if (!/^\d+$/.test(normalized)) return 0;
  const parsed = Number(normalized);
  return Number.isSafeInteger(parsed) && parsed >= 0 ? parsed : 0;
};

const parseMode = (value: string | undefined): CodexBankedResetMode => {
  if (value === undefined) return "shadow";
  switch (value.trim().toLowerCase()) {
    case "shadow":
      return "shadow";
    case "live":
      return "live";
    case "disabled":
      return "disabled";
    default:
      return "disabled";
  }
};

/**
 * This is intentionally read at use time rather than module load time. A
 * configuration update can kill new claims immediately without deleting an
 * existing submitted/unknown record that still requires reconciliation.
 */
export const parseCodexBankedResetConfig = (readEnv: (key: string) => string | undefined = getEnv): CodexBankedResetConfig => ({
  // Shadow telemetry never consumes a credit. A spend still requires
  // explicit live mode, valid caps, and an approved provider contract.
  enabled: parseStrictBoolean(readEnv("CODEX_BANKED_RESET_ENABLED"), true),
  mode: parseMode(readEnv("CODEX_BANKED_RESET_MODE")),
  maxGlobalPerDay: parseNonNegativeInteger(readEnv("CODEX_BANKED_RESET_MAX_GLOBAL_PER_DAY"), 0),
  maxPerAccountPerWindow: parseNonNegativeInteger(readEnv("CODEX_BANKED_RESET_MAX_PER_ACCOUNT_PER_WINDOW"), 1),
});

export const loadCodexBankedResetConfig = (): CodexBankedResetConfig => parseCodexBankedResetConfig();

/**
 * The usage gate that every path writing a durable submission passes through.
 * An unavailable configuration and a disabled subscription both stop the
 * caller. Reset settings are owned by the account, never by an API key.
 */
const readUsageGate = async (
  kv: Deno.Kv,
  accountIdHash: string
): Promise<Readonly<{ kind: "allowed"; entries: readonly Deno.KvEntryMaybe<unknown>[] }> | Readonly<{ kind: "failure"; code: string }>> => {
  let usage: Awaited<ReturnType<typeof readBankedResetUsage>>;
  try {
    usage = await readBankedResetUsage(kv, accountIdHash);
  } catch {
    return { kind: "failure", code: "configuration_unavailable" };
  }
  if (!usage.allowed) return { kind: "failure", code: "usage_disabled" };
  return { kind: "allowed", entries: usage.entries };
};

export type CodexBankedResetEvent =
  | "codex_reset_eligible"
  | "codex_reset_skipped_healthy_fallback"
  | "codex_reset_claimed"
  | "codex_reset_submit_started"
  | "codex_reset_submitted"
  | "codex_reset_unknown"
  | "codex_reset_rejected"
  | "codex_reset_verified"
  | "codex_reset_inference_retry"
  | "codex_reset_inference_retry_result"
  | "codex_reset_duplicate_prevented"
  | "codex_reset_shadow_candidate";

export type CodexBankedResetMetric =
  | "codex_reset_eligible_total"
  | "codex_reset_shadow_candidates_total"
  | "codex_reset_submission_attempts_total"
  | "codex_reset_verified_total"
  | "codex_reset_unknown_total"
  | "codex_reset_duplicate_prevented_total"
  | "codex_reset_verification_latency_ms"
  | "codex_reset_post_retry_total"
  | "codex_reset_estimated_spend_total";

export type CodexBankedResetTelemetryFields = Readonly<Record<string, string | number | boolean | null>>;

export type CodexBankedResetTelemetry = Readonly<{
  event?: (event: CodexBankedResetEvent, fields: CodexBankedResetTelemetryFields) => void;
  metric?: (metric: CodexBankedResetMetric, value: number, fields: CodexBankedResetTelemetryFields) => void;
}>;

const defaultTelemetry: CodexBankedResetTelemetry = {
  event(event, fields) {
    try {
      console.info("[ai.ubq.fi] codex_banked_reset", JSON.stringify({ event, ...fields }));
    } catch {
      // Telemetry may never make a reset safer-looking or less safe.
    }
  },
  metric(metric, value, fields) {
    try {
      console.info("[ai.ubq.fi] codex_banked_reset_metric", JSON.stringify({ metric, value, ...fields }));
    } catch {
      // Metrics are deliberately best effort.
    }
  },
};

export type CodexBankedResetCandidate = Readonly<{
  /** Raw account id is passed only to the provider; it is never persisted or logged. */
  accountId: string;
  /**
   * An opaque routing credential fence. It is hashed before it reaches a
   * durable record, provider context, or telemetry sink.
   */
  credentialVersion: string;
  quotaResetAtMs: number;
  routingGeneration: number;
  /**
   * Each fence is strongly read and atomically checked at both the initial
   * claim and the durable `claimed -> submitted` side-effect boundary.
   * Production passes routing and auth-pool fences; fakes use the same KV
   * shape without exposing a privileged bypass.
   */
  fences: readonly CodexBankedResetFence[];
  requestId: string | null;
  /**
   * A fresh, account-bound credit selected by the pool evaluator. It remains
   * in memory only; durable records retain its hash at most.
   */
  selectedCredit?: ResetInventoryCredit;
  signal?: AbortSignal;
}>;

export type CodexBankedResetFence = Readonly<{
  key: Deno.KvKey;
  isCurrent: (value: unknown) => boolean;
}>;

export type CodexBankedResetDependencies = Readonly<{
  config: CodexBankedResetConfig;
  /** Re-read before a new submission so an operator kill switch wins mid-request. */
  reloadConfig?: () => CodexBankedResetConfig;
  provider: CodexUsageResetProvider;
  /** `undefined` uses the production KV accessor; explicit null fails closed. */
  kv?: Deno.Kv | null;
  now?: () => number;
  newOwnerToken?: () => string;
  hash?: (value: string) => Promise<string>;
  telemetry?: CodexBankedResetTelemetry;
  /**
   * Hermetic integration-test seam for pre-shadow legacy fixtures. Production
   * never supplies this and always requires a matching shadow decision.
   */
  allowLiveWithoutShadowForTest?: boolean;
}>;

export type CodexBankedResetOutcome = Readonly<{
  kind: "verified" | "pending" | "rejected" | "skipped";
  reason: string;
  accountIdHash: string | null;
  quotaGeneration: string | null;
  idempotencyKeyHash: string | null;
  record: CodexResetRedemptionRecord | null;
}>;

/** One currently fenced account supplied to the blocked-cohort evaluator. */
export type CodexBankedResetPoolCandidate = Readonly<{
  slot: number;
  candidate: CodexBankedResetCandidate;
  provider: CodexUsageResetProvider;
}>;

/**
 * The evaluator returns the in-memory selected account only after the shadow
 * evidence or live reset path has succeeded. No raw account or credit value
 * is written by this return value.
 */
export type CodexBankedResetPoolOutcome = Readonly<{
  kind: "shadow" | "verified" | "skipped" | "pending" | "rejected";
  reason: string;
  selected: CodexBankedResetPoolCandidate | null;
  reset: CodexBankedResetOutcome | null;
}>;

type ResetContext = Readonly<{
  account: ResetAccountContext;
  idempotencyKey: string;
  idempotencyKeyHash: string;
}>;

const isSafeMs = (value: unknown): value is number => typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const isSafeNonnegativeInteger = (value: unknown): value is number => typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const isNonEmptyText = (value: unknown, max = 512): value is string => typeof value === "string" && value.length > 0 && value.length <= max;

const isState = (value: unknown): value is CodexResetRedemptionState =>
  value === "claimed" || value === "submitted" || value === "unknown" || value === "verified" || value === "rejected";

/** The stored record minus the version tag, which the parser checks separately. */
type ValidatedRedemptionRecordFields = Omit<CodexResetRedemptionRecord, "v">;

/**
 * A state is only durable when it agrees with the timestamps and error fields
 * that state allows. `isState` has already narrowed `state` to the five durable
 * states, so the default branch is unreachable; it assigns to a `never`-typed
 * local so a future state cannot be accepted without a compile error here.
 */
const redemptionRecordStateIsConsistent = (
  state: CodexResetRedemptionState,
  providerReceiptId: string | null,
  submittedAtMs: number | null,
  verifiedAtMs: number | null,
  lastErrorCode: string | null
): boolean => {
  switch (state) {
    case "claimed":
      return providerReceiptId === null && submittedAtMs === null && verifiedAtMs === null && lastErrorCode === null;
    case "submitted":
      return submittedAtMs !== null && verifiedAtMs === null && lastErrorCode === null;
    case "unknown":
      return submittedAtMs !== null && verifiedAtMs === null && lastErrorCode !== null;
    case "verified":
      return submittedAtMs !== null && verifiedAtMs !== null && lastErrorCode === null;
    case "rejected":
      return verifiedAtMs === null && lastErrorCode !== null;
    default: {
      const unreachableState: never = state;
      throw new Error(`unhandled codex reset redemption state: ${JSON.stringify(unreachableState)}`);
    }
  }
};

/** Ordering that every accepted record must satisfy, independent of its state. */
const redemptionRecordTimestampsAreOrdered = (createdAtMs: number, updatedAtMs: number, submittedAtMs: number | null, verifiedAtMs: number | null): boolean =>
  updatedAtMs >= createdAtMs &&
  (submittedAtMs === null || submittedAtMs >= createdAtMs) &&
  (submittedAtMs === null || updatedAtMs >= submittedAtMs) &&
  (verifiedAtMs === null || (submittedAtMs !== null && verifiedAtMs >= submittedAtMs)) &&
  (verifiedAtMs === null || updatedAtMs >= verifiedAtMs);

/**
 * Field-by-field validation of one stored record. The predicate form keeps the
 * narrowing at the call site, so the parser only assembles an object it has
 * already proven field by field, in the same order as before.
 */
const hasValidRedemptionRecordFields = (value: Record<string, unknown>): value is ValidatedRedemptionRecordFields => {
  if (
    !isNonEmptyText(value.account_id_hash) ||
    !isNonEmptyText(value.credential_version) ||
    !isNonEmptyText(value.quota_generation) ||
    !isNonEmptyText(value.idempotency_key_hash) ||
    !isState(value.state) ||
    !isNonEmptyText(value.owner_token) ||
    !isSafeNonnegativeInteger(value.routing_generation) ||
    !isSafeNonnegativeInteger(value.fence) ||
    !isSafeMs(value.lease_expires_at_ms) ||
    !isSafeMs(value.created_at_ms) ||
    !isSafeMs(value.updated_at_ms)
  )
    return false;
  if (value.provider_receipt_id !== null && !isNonEmptyText(value.provider_receipt_id)) return false;
  if (value.submitted_at_ms !== null && !isSafeMs(value.submitted_at_ms)) return false;
  if (value.verified_at_ms !== null && !isSafeMs(value.verified_at_ms)) return false;
  if (value.last_error_code !== null && !isNonEmptyText(value.last_error_code, 128)) return false;
  if (!redemptionRecordTimestampsAreOrdered(value.created_at_ms, value.updated_at_ms, value.submitted_at_ms, value.verified_at_ms)) return false;
  return redemptionRecordStateIsConsistent(value.state, value.provider_receipt_id, value.submitted_at_ms, value.verified_at_ms, value.last_error_code);
};

export const parseCodexResetRedemptionRecord = (value: unknown): CodexResetRedemptionRecord | null => {
  if (!isRecord(value) || value.v !== 1) return null;
  if (!hasValidRedemptionRecordFields(value)) return null;
  return {
    v: 1,
    account_id_hash: value.account_id_hash,
    credential_version: value.credential_version,
    quota_generation: value.quota_generation,
    routing_generation: value.routing_generation,
    idempotency_key_hash: value.idempotency_key_hash,
    state: value.state,
    owner_token: value.owner_token,
    fence: value.fence,
    lease_expires_at_ms: value.lease_expires_at_ms,
    provider_receipt_id: value.provider_receipt_id,
    created_at_ms: value.created_at_ms,
    updated_at_ms: value.updated_at_ms,
    submitted_at_ms: value.submitted_at_ms,
    verified_at_ms: value.verified_at_ms,
    last_error_code: value.last_error_code,
  };
};

const parseGlobalDailyRecord = (value: unknown, day: string): CodexResetGlobalDailyRecord | null => {
  if (!isRecord(value) || value.v !== 1 || value.day !== day || !isSafeMs(value.updated_at_ms)) return null;
  if (!isSafeNonnegativeInteger(value.submission_count)) return null;
  return { v: 1, day, submission_count: value.submission_count, updated_at_ms: value.updated_at_ms };
};

const parseShadowDecisionFence = (value: unknown): CodexResetShadowDecisionRecord["fences"][number] | null => {
  if (
    !isRecord(value) ||
    !isSafeNonnegativeInteger(value.slot) ||
    !isNonEmptyText(value.account_id_hash) ||
    !isNonEmptyText(value.quota_generation) ||
    !isSafeNonnegativeInteger(value.routing_generation) ||
    !isSafeMs(value.quota_reset_at_ms)
  )
    return null;
  return {
    slot: value.slot,
    account_id_hash: value.account_id_hash,
    quota_generation: value.quota_generation,
    routing_generation: value.routing_generation,
    quota_reset_at_ms: value.quota_reset_at_ms,
  };
};

/** Parse only redacted, safe-to-return shadow decision evidence. */
export const parseCodexResetShadowDecisionRecord = (value: unknown): CodexResetShadowDecisionRecord | null => {
  if (
    !isRecord(value) ||
    value.v !== 1 ||
    !isNonEmptyText(value.episode_hash) ||
    !isSafeMs(value.created_at_ms) ||
    !isSafeMs(value.expires_at_ms) ||
    value.expires_at_ms < value.created_at_ms ||
    !isNonEmptyText(value.decision_reason, 128) ||
    !Array.isArray(value.fences)
  )
    return null;
  if (value.selected_account_id_hash !== null && !isNonEmptyText(value.selected_account_id_hash)) return null;
  if (value.selected_credit_id_hash !== null && !isNonEmptyText(value.selected_credit_id_hash)) return null;
  if (value.selected_credit_expires_at_ms !== null && !isSafeMs(value.selected_credit_expires_at_ms)) return null;
  if ((value.selected_account_id_hash === null) !== (value.selected_credit_id_hash === null)) return null;
  const fences = value.fences.map(parseShadowDecisionFence);
  if (!fences.length || fences.some((fence) => !fence)) return null;
  const parsedFences = fences as CodexResetShadowDecisionRecord["fences"];
  const slots = new Set<number>();
  if (parsedFences.some((fence) => slots.has(fence.slot) || (slots.add(fence.slot), false))) return null;
  return {
    v: 1,
    episode_hash: value.episode_hash,
    created_at_ms: value.created_at_ms,
    expires_at_ms: value.expires_at_ms,
    decision_reason: value.decision_reason,
    selected_account_id_hash: value.selected_account_id_hash,
    selected_credit_id_hash: value.selected_credit_id_hash,
    selected_credit_expires_at_ms: value.selected_credit_expires_at_ms,
    fences: parsedFences,
  };
};

const utcDay = (nowMs: number): string | null => {
  if (!isSafeMs(nowMs)) return null;
  try {
    return new Date(nowMs).toISOString().slice(0, 10);
  } catch {
    return null;
  }
};

/**
 * A global cap is a cap on externally visible submissions, not merely on
 * ledger claims. A claim that crosses UTC midnight must never carry an old
 * day's reservation into a new day, where it could bypass that day's cap.
 */
const claimedDuringCurrentUtcDay = (record: CodexResetRedemptionRecord, nowMs: number): boolean => {
  const claimedDay = utcDay(record.created_at_ms);
  const currentDay = utcDay(nowMs);
  return claimedDay !== null && claimedDay === currentDay;
};

const outcome = (
  kind: CodexBankedResetOutcome["kind"],
  reason: string,
  context: ResetContext | null = null,
  record: CodexResetRedemptionRecord | null = null
): CodexBankedResetOutcome => ({
  kind,
  reason,
  accountIdHash: context?.account.accountIdHash ?? null,
  quotaGeneration: context?.account.quotaGeneration ?? null,
  idempotencyKeyHash: context?.idempotencyKeyHash ?? null,
  record,
});

const safeOwnerToken = (source: () => string): string | null => {
  try {
    const token = source();
    return isNonEmptyText(token, 256) ? token : null;
  } catch {
    return null;
  }
};

const policyReason = (config: CodexBankedResetConfig): string | null => {
  try {
    if (!config.enabled) return "feature_disabled";
    if (config.mode === "disabled") return "mode_disabled";
    // This single-candidate state-machine seam makes no provider call in
    // shadow. The production cohort evaluator requires a positive cap before
    // its bounded inventory reads.
    if (config.mode === "shadow") return null;
    if (config.maxGlobalPerDay <= 0) return "global_limit_disabled";
    // The non-negotiable at-most-once rule is stronger than a mutable setting.
    if (config.maxPerAccountPerWindow !== 1) return "per_account_window_limit_invalid";
    return null;
  } catch {
    return "configuration_invalid";
  }
};

const providerPolicyReason = (config: CodexBankedResetConfig, provider: CodexUsageResetProvider): string | null =>
  config.mode === "live" && providerTreatsRedeemOutcomeAsFinal(provider) && config.maxGlobalPerDay !== 1 ? "terminal_outcome_global_limit_must_be_one" : null;

const boundedInventorySignal = (signal?: AbortSignal): AbortSignal => {
  const timeout = AbortSignal.timeout(CODEX_BANKED_RESET_INVENTORY_TIMEOUT_MS);
  return signal ? AbortSignal.any([signal, timeout]) : timeout;
};

const telemetryFields = (
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  extras: CodexBankedResetTelemetryFields = {}
): CodexBankedResetTelemetryFields => ({
  request_id: candidate.requestId,
  account_id_hash: context.account.accountIdHash,
  credential_version: context.account.credentialVersion,
  quota_generation: context.account.quotaGeneration,
  idempotency_key_hash: context.idempotencyKeyHash,
  routing_generation: candidate.routingGeneration,
  ...extras,
});

const emit = (telemetry: CodexBankedResetTelemetry, event: CodexBankedResetEvent, fields: CodexBankedResetTelemetryFields): void => {
  try {
    telemetry.event?.(event, fields);
  } catch {
    // An observer must not affect state transitions.
  }
};

const metric = (telemetry: CodexBankedResetTelemetry, name: CodexBankedResetMetric, value: number, fields: CodexBankedResetTelemetryFields): void => {
  try {
    telemetry.metric?.(name, value, fields);
  } catch {
    // An observer must not affect state transitions.
  }
};

/** Allows gateway-level events and metrics to share the same injectable sink. */
export const reportCodexBankedResetEvent = (
  telemetry: CodexBankedResetTelemetry | undefined,
  event: CodexBankedResetEvent,
  fields: CodexBankedResetTelemetryFields
): void => {
  emit(telemetry ?? defaultTelemetry, event, fields);
};

/** Allows gateway-level post-retry metrics to share the default sink. */
export const reportCodexBankedResetMetric = (
  telemetry: CodexBankedResetTelemetry | undefined,
  name: CodexBankedResetMetric,
  value: number,
  fields: CodexBankedResetTelemetryFields
): void => {
  metric(telemetry ?? defaultTelemetry, name, value, fields);
};

const makeResetContext = async (candidate: CodexBankedResetCandidate, hash: (value: string) => Promise<string>): Promise<ResetContext | null> => {
  if (
    !isNonEmptyText(candidate.accountId, 1024) ||
    !isNonEmptyText(candidate.credentialVersion, 512) ||
    !isSafeMs(candidate.quotaResetAtMs) ||
    !Number.isSafeInteger(candidate.routingGeneration) ||
    candidate.routingGeneration < 0
  ) {
    return null;
  }
  try {
    const accountIdHash = await hash(candidate.accountId);
    // The routing layer permits this deadline-derived identity only while its
    // durable fence proves the absolute observation has not been revised.
    // A future provider-proven quota generation should replace this deadline
    // identity; an observed deadline change fails closed before this path.
    // Credential version remains a separate routing fence, so refresh cannot
    // manufacture a second logical redemption for one observed window.
    const credentialVersionInput = `uos_ai\u0000codex_reset_credential_version\u0000${candidate.credentialVersion}`;
    const credentialVersion = `v1:${await hash(credentialVersionInput)}`;
    const quotaGenerationInput = `uos_ai\u0000codex_reset_generation\u0000${accountIdHash}\u0000${candidate.quotaResetAtMs}`;
    const quotaGeneration = `v1:${await hash(quotaGenerationInput)}`;
    const idempotencyKeyInput = `uos_ai\u0000codex_reset_idempotency\u0000${accountIdHash}\u0000${quotaGeneration}`;
    const idempotencyKey = `uos_ai_codex_reset_v1_${await hash(idempotencyKeyInput)}`;
    const idempotencyKeyHash = await hash(idempotencyKey);
    if (!isNonEmptyText(accountIdHash) || !isNonEmptyText(credentialVersion) || !isNonEmptyText(quotaGeneration) || !isNonEmptyText(idempotencyKeyHash)) {
      return null;
    }
    return {
      account: {
        accountId: candidate.accountId,
        accountIdHash,
        credentialVersion,
        quotaGeneration,
      },
      idempotencyKey,
      idempotencyKeyHash,
    };
  } catch {
    return null;
  }
};

type ClaimResult =
  | Readonly<{ kind: "submit"; record: CodexResetRedemptionRecord; tookOver: boolean }>
  | Readonly<{ kind: "reconcile"; record: CodexResetRedemptionRecord; tookOver: boolean }>
  | Readonly<{ kind: "verified"; record: CodexResetRedemptionRecord }>
  | Readonly<{ kind: "rejected"; record: CodexResetRedemptionRecord }>
  | Readonly<{ kind: "in_progress"; record: CodexResetRedemptionRecord }>
  | Readonly<{ kind: "no_transaction" }>
  | Readonly<{ kind: "global_limit" }>
  | Readonly<{ kind: "failure"; code: string }>;

type FenceRead =
  Readonly<{ kind: "valid"; entries: readonly Deno.KvEntryMaybe<unknown>[] }> | Readonly<{ kind: "stale" }> | Readonly<{ kind: "failure"; code: string }>;

type SubmissionPreparation = Readonly<{ kind: "submitted"; record: CodexResetRedemptionRecord }> | Readonly<{ kind: "failure"; code: string }>;

type SubmissionRenewal = Readonly<{ kind: "renewed"; record: CodexResetRedemptionRecord }> | Readonly<{ kind: "failure"; code: string }>;

const matchesContext = (record: CodexResetRedemptionRecord, context: ResetContext): boolean =>
  record.account_id_hash === context.account.accountIdHash &&
  record.credential_version === context.account.credentialVersion &&
  record.quota_generation === context.account.quotaGeneration &&
  record.idempotency_key_hash === context.idempotencyKeyHash;

/**
 * Ownership fence for a record this worker still holds: same reset context, the
 * same owner token and fence it wrote, and the expected durable state.
 */
const isOwnedRecordInState = (
  current: CodexResetRedemptionRecord | null,
  context: ResetContext,
  expected: CodexResetRedemptionRecord,
  state: CodexResetRedemptionState
): current is CodexResetRedemptionRecord =>
  current !== null &&
  matchesContext(current, context) &&
  current.owner_token === expected.owner_token &&
  current.fence === expected.fence &&
  current.state === state;

const leaseUntil = (nowMs: number): number | null => {
  const next = nowMs + CODEX_BANKED_RESET_LEASE_MS;
  return isSafeMs(next) ? next : null;
};

const nextFence = (fence: number): number | null => {
  const next = fence + 1;
  return Number.isSafeInteger(next) ? next : null;
};

const quotaWindowIsOpen = (candidate: CodexBankedResetCandidate, nowMs: number): boolean => nowMs < candidate.quotaResetAtMs;

const readClock = (clock: () => number): number | null => {
  try {
    const value = clock();
    return isSafeMs(value) ? value : null;
  } catch {
    return null;
  }
};

const hasUsableFences = (candidate: CodexBankedResetCandidate): boolean =>
  Array.isArray(candidate.fences) &&
  candidate.fences.length > 0 &&
  candidate.fences.every((fence) => isRecord(fence) && Array.isArray(fence.key) && typeof fence.isCurrent === "function");

const readCurrentFences = async (kv: Deno.Kv, candidate: CodexBankedResetCandidate): Promise<FenceRead> => {
  if (!hasUsableFences(candidate)) return { kind: "failure", code: "routing_fence_missing" };
  const entries: Deno.KvEntryMaybe<unknown>[] = [];
  for (const fence of candidate.fences) {
    let entry: Deno.KvEntryMaybe<unknown>;
    try {
      entry = await kv.get(fence.key, { consistency: "strong" });
    } catch {
      return { kind: "failure", code: "kv_unavailable" };
    }
    try {
      if (!fence.isCurrent(entry.value)) return { kind: "stale" };
    } catch {
      return { kind: "stale" };
    }
    entries.push(entry);
  }
  return { kind: "valid", entries };
};

const withFenceChecks = (operation: Deno.AtomicOperation, entries: readonly Deno.KvEntryMaybe<unknown>[]): Deno.AtomicOperation => {
  let next = operation;
  for (const entry of entries) next = next.check(entry);
  return next;
};

/**
 * A fence read that a caller may act on. An unavailable read and a stale fence
 * are both stops, but only the stale fence reports a routing fence problem.
 */
type RequiredFences = Readonly<{ kind: "valid"; entries: readonly Deno.KvEntryMaybe<unknown>[] }> | Readonly<{ kind: "failure"; code: string }>;

const readRequiredFences = async (kv: Deno.Kv, candidate: CodexBankedResetCandidate): Promise<RequiredFences> => {
  const fences = await readCurrentFences(kv, candidate);
  if (fences.kind === "failure") return { kind: "failure", code: fences.code };
  if (fences.kind === "stale") return { kind: "failure", code: "routing_fence_stale" };
  return { kind: "valid", entries: fences.entries };
};

const readExistingRecord = async (
  kv: Deno.Kv,
  context: ResetContext
): Promise<Readonly<{ record: CodexResetRedemptionRecord | null; code: string | null }>> => {
  const key = codexResetRedemptionKey(context.account.accountIdHash, context.account.quotaGeneration);
  try {
    const entry = await kv.get<CodexResetRedemptionRecord>(key, { consistency: "strong" });
    if (entry.value === null) return { record: null, code: null };
    const record = parseCodexResetRedemptionRecord(entry.value);
    return record ? { record, code: null } : { record: null, code: "redemption_record_invalid" };
  } catch {
    return { record: null, code: "kv_unavailable" };
  }
};

/**
 * Create the first durable claim for a quota window. `null` means the atomic
 * write lost a race and the caller must re-read the record and try again.
 */
const createClaimRecord = async (
  kv: Deno.Kv,
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  nowMs: number,
  clock: () => number,
  ownerToken: string,
  allowNewSubmission: boolean,
  expiresAtMs: number,
  entry: Deno.KvEntryMaybe<CodexResetRedemptionRecord>,
  key: Deno.KvKey
): Promise<ClaimResult | null> => {
  if (!allowNewSubmission) return { kind: "no_transaction" };
  // Do not claim a quota window that has already recovered. In particular, this
  // must precede the daily-cap write so an expired candidate cannot consume
  // capacity without reaching the provider.
  if (!quotaWindowIsOpen(candidate, nowMs)) return { kind: "failure", code: "quota_window_expired" };
  const fences = await readRequiredFences(kv, candidate);
  if (fences.kind === "failure") return { kind: "failure", code: fences.code };
  const created: CodexResetRedemptionRecord = {
    v: 1,
    account_id_hash: context.account.accountIdHash,
    credential_version: context.account.credentialVersion,
    quota_generation: context.account.quotaGeneration,
    routing_generation: candidate.routingGeneration,
    idempotency_key_hash: context.idempotencyKeyHash,
    state: "claimed",
    owner_token: ownerToken,
    fence: 1,
    lease_expires_at_ms: expiresAtMs,
    provider_receipt_id: null,
    created_at_ms: nowMs,
    updated_at_ms: nowMs,
    submitted_at_ms: null,
    verified_at_ms: null,
    last_error_code: null,
  };
  // Fence reads are asynchronous. Re-check immediately before the atomic write
  // so a window that expired during those reads cannot create a fresh
  // submission path.
  const nowBeforeClaim = readClock(clock);
  if (nowBeforeClaim === null) return { kind: "failure", code: "invalid_clock" };
  if (!quotaWindowIsOpen(candidate, nowBeforeClaim)) {
    return { kind: "failure", code: "quota_window_expired" };
  }
  try {
    const committed = await withFenceChecks(kv.atomic().check(entry), fences.entries).set(key, created).commit();
    if (committed.ok) return { kind: "submit", record: created, tookOver: false };
  } catch {
    return { kind: "failure", code: "kv_unavailable" };
  }
  return null;
};

/** Terminal or still-owned records short-circuit a takeover before any write. */
const existingRecordDisposition = (
  record: CodexResetRedemptionRecord,
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  nowMs: number,
  allowNewSubmission: boolean
): ClaimResult | null => {
  if (!matchesContext(record, context)) return { kind: "failure", code: "redemption_record_context_mismatch" };
  if (record.state === "verified") return { kind: "verified", record };
  if (record.state === "rejected") return { kind: "rejected", record };
  if (record.lease_expires_at_ms > nowMs) return { kind: "in_progress", record };
  if (record.state === "claimed" && !allowNewSubmission) return { kind: "in_progress", record };
  // A takeover of an expired `claimed` record would otherwise create a fresh
  // submission path after the observed quota window has reopened.
  // Submitted/unknown records deliberately bypass this guard and reconcile
  // lookup-only; they are never re-redeemed here.
  if (record.state === "claimed" && !quotaWindowIsOpen(candidate, nowMs)) {
    return { kind: "failure", code: "quota_window_expired" };
  }
  return null;
};

/** Only a `claimed` record is fenced on takeover; reconciliation reads none. */
const takeOverFences = async (kv: Deno.Kv, candidate: CodexBankedResetCandidate, record: CodexResetRedemptionRecord): Promise<RequiredFences> => {
  if (record.state !== "claimed") return { kind: "valid", entries: [] };
  if (record.routing_generation !== candidate.routingGeneration) {
    return { kind: "failure", code: "routing_fence_stale" };
  }
  return await readRequiredFences(kv, candidate);
};

/** Commit the takeover. `null` means the CAS lost and the caller must retry. */
const commitTakeOver = async (
  kv: Deno.Kv,
  candidate: CodexBankedResetCandidate,
  record: CodexResetRedemptionRecord,
  nowMs: number,
  clock: () => number,
  ownerToken: string,
  expiresAtMs: number,
  fenceEntries: readonly Deno.KvEntryMaybe<unknown>[],
  entry: Deno.KvEntryMaybe<CodexResetRedemptionRecord>,
  key: Deno.KvKey
): Promise<ClaimResult | null> => {
  // A stale claimant may have spent time reading its routing/auth fences.
  // Recheck here before it can become the new owner of an expired quota
  // window. Submitted/unknown reconciliation remains outside this path.
  const nowBeforeTakeover = readClock(clock);
  if (nowBeforeTakeover === null) return { kind: "failure", code: "invalid_clock" };
  if (record.state === "claimed" && !quotaWindowIsOpen(candidate, nowBeforeTakeover)) {
    return { kind: "failure", code: "quota_window_expired" };
  }
  const renewedFence = nextFence(record.fence);
  if (renewedFence === null) return { kind: "failure", code: "owner_fence_exhausted" };
  const takenOver: CodexResetRedemptionRecord = {
    ...record,
    owner_token: ownerToken,
    fence: renewedFence,
    lease_expires_at_ms: expiresAtMs,
    updated_at_ms: nowMs,
  };
  try {
    const committed = await withFenceChecks(kv.atomic().check(entry), fenceEntries).set(key, takenOver).commit();
    if (!committed.ok) return null;
  } catch {
    return { kind: "failure", code: "kv_unavailable" };
  }
  return record.state === "claimed" ? { kind: "submit", record: takenOver, tookOver: true } : { kind: "reconcile", record: takenOver, tookOver: true };
};

/**
 * One compare-and-set attempt. `null` means a concurrent writer changed the
 * record between the read and the write, so the caller must try again.
 */
const claimAttempt = async (
  kv: Deno.Kv,
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  nowMs: number,
  clock: () => number,
  ownerToken: string,
  allowNewSubmission: boolean,
  expiresAtMs: number,
  key: Deno.KvKey
): Promise<ClaimResult | null> => {
  let entry: Deno.KvEntryMaybe<CodexResetRedemptionRecord>;
  try {
    entry = await kv.get<CodexResetRedemptionRecord>(key, { consistency: "strong" });
  } catch {
    return { kind: "failure", code: "kv_unavailable" };
  }
  const record = entry.value === null ? null : parseCodexResetRedemptionRecord(entry.value);
  if (entry.value !== null && !record) return { kind: "failure", code: "redemption_record_invalid" };
  if (!record) {
    return await createClaimRecord(kv, context, candidate, nowMs, clock, ownerToken, allowNewSubmission, expiresAtMs, entry, key);
  }

  const disposition = existingRecordDisposition(record, context, candidate, nowMs, allowNewSubmission);
  if (disposition) return disposition;
  const fences = await takeOverFences(kv, candidate, record);
  if (fences.kind === "failure") return { kind: "failure", code: fences.code };
  return await commitTakeOver(kv, candidate, record, nowMs, clock, ownerToken, expiresAtMs, fences.entries, entry, key);
};

const claimTransaction = async (
  kv: Deno.Kv,
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  nowMs: number,
  clock: () => number,
  ownerToken: string,
  allowNewSubmission: boolean
): Promise<ClaimResult> => {
  const expiresAtMs = leaseUntil(nowMs);
  if (expiresAtMs === null) return { kind: "failure", code: "invalid_clock" };
  const key = codexResetRedemptionKey(context.account.accountIdHash, context.account.quotaGeneration);

  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const claimed = await claimAttempt(kv, context, candidate, nowMs, clock, ownerToken, allowNewSubmission, expiresAtMs, key);
    if (claimed) return claimed;
  }
  return { kind: "failure", code: "kv_cas_exhausted" };
};

const updateOwnedRecord = async (
  kv: Deno.Kv,
  context: ResetContext,
  expected: CodexResetRedemptionRecord,
  mutate: (record: CodexResetRedemptionRecord) => CodexResetRedemptionRecord
): Promise<CodexResetRedemptionRecord | null> => {
  const key = codexResetRedemptionKey(context.account.accountIdHash, context.account.quotaGeneration);
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    let entry: Deno.KvEntryMaybe<CodexResetRedemptionRecord>;
    try {
      entry = await kv.get<CodexResetRedemptionRecord>(key, { consistency: "strong" });
    } catch {
      return null;
    }
    const current = parseCodexResetRedemptionRecord(entry.value);
    if (!current || !matchesContext(current, context) || current.owner_token !== expected.owner_token || current.fence !== expected.fence) {
      return null;
    }
    const next = mutate(current);
    try {
      const committed = await kv.atomic().check(entry).set(key, next).commit();
      if (committed.ok) return next;
    } catch {
      return null;
    }
  }
  return null;
};

/**
 * Atomically renew the owner lease and fence the `claimed -> submitted`
 * transition against both routing and auth. The external call follows only
 * after this durable side-effect boundary succeeds.
 */
/**
 * Strong read of the UTC day's submission budget. A corrupt daily record fails
 * closed rather than resetting the cap to zero.
 */
const readDailySubmissionBudget = async (
  kv: Deno.Kv,
  dailyKey: Deno.KvKey,
  day: string,
  maxGlobalPerDay: number
): Promise<
  Readonly<{ kind: "ok"; entry: Deno.KvEntryMaybe<CodexResetGlobalDailyRecord>; submissionCount: number }> | Readonly<{ kind: "failure"; code: string }>
> => {
  let dailyEntry: Deno.KvEntryMaybe<CodexResetGlobalDailyRecord>;
  try {
    dailyEntry = await kv.get<CodexResetGlobalDailyRecord>(dailyKey, { consistency: "strong" });
  } catch {
    return { kind: "failure", code: "kv_unavailable" };
  }
  const daily = dailyEntry.value === null ? null : parseGlobalDailyRecord(dailyEntry.value, day);
  if (dailyEntry.value !== null && !daily) return { kind: "failure", code: "global_limit_record_invalid" };
  const submissionCount = daily?.submission_count ?? 0;
  if (submissionCount >= maxGlobalPerDay) return { kind: "failure", code: "global_limit_reached" };
  return { kind: "ok", entry: dailyEntry, submissionCount };
};

/**
 * One compare-and-set attempt of the durable `claimed -> submitted` boundary.
 * `null` means the CAS lost and the caller must try again.
 */
const prepareSubmissionAttempt = async (
  kv: Deno.Kv,
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  expected: CodexResetRedemptionRecord,
  nowMs: number,
  clock: () => number,
  maxGlobalPerDay: number,
  expiresAtMs: number,
  day: string,
  key: Deno.KvKey,
  dailyKey: Deno.KvKey
): Promise<SubmissionPreparation | null> => {
  let entry: Deno.KvEntryMaybe<CodexResetRedemptionRecord>;
  try {
    entry = await kv.get<CodexResetRedemptionRecord>(key, { consistency: "strong" });
  } catch {
    return { kind: "failure", code: "kv_unavailable" };
  }
  const current = parseCodexResetRedemptionRecord(entry.value);
  if (!isOwnedRecordInState(current, context, expected, "claimed")) return { kind: "failure", code: "stale_owner" };
  if (!claimedDuringCurrentUtcDay(current, nowMs)) return { kind: "failure", code: "claim_day_elapsed" };
  if (current.routing_generation !== candidate.routingGeneration || current.lease_expires_at_ms <= nowMs) {
    return { kind: "failure", code: "stale_owner" };
  }
  const fences = await readRequiredFences(kv, candidate);
  if (fences.kind === "failure") return { kind: "failure", code: fences.code };

  const usage = await readUsageGate(kv, context.account.accountIdHash);
  if (usage.kind === "failure") return { kind: "failure", code: usage.code };
  const budget = await readDailySubmissionBudget(kv, dailyKey, day, maxGlobalPerDay);
  if (budget.kind === "failure") return { kind: "failure", code: budget.code };
  const submissionCount = budget.submissionCount;
  // Inventory, fences, and the daily budget are all strong reads. Check
  // again after them so a naturally recovered quota window cannot cross the
  // durable submission boundary or consume the daily budget.
  const nowBeforeCommit = readClock(clock);
  if (nowBeforeCommit === null) return { kind: "failure", code: "invalid_clock" };
  if (!quotaWindowIsOpen(candidate, nowBeforeCommit)) return { kind: "failure", code: "quota_window_expired" };
  if (!claimedDuringCurrentUtcDay(current, nowBeforeCommit)) return { kind: "failure", code: "claim_day_elapsed" };
  const submitted = {
    ...stateWith(current, "submitted", nowMs, { submitted_at_ms: nowMs, last_error_code: null }),
    lease_expires_at_ms: expiresAtMs,
  };
  const nextDaily: CodexResetGlobalDailyRecord = {
    v: 1,
    day,
    submission_count: submissionCount + 1,
    updated_at_ms: nowMs,
  };
  try {
    const committed = await withFenceChecks(kv.atomic().check(entry).check(budget.entry), [...fences.entries, ...usage.entries])
      .set(key, submitted)
      .set(dailyKey, nextDaily)
      .commit();
    if (committed.ok) return { kind: "submitted", record: submitted };
  } catch {
    return { kind: "failure", code: "kv_unavailable" };
  }
  return null;
};

const prepareSubmission = async (
  kv: Deno.Kv,
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  expected: CodexResetRedemptionRecord,
  nowMs: number,
  clock: () => number,
  maxGlobalPerDay: number
): Promise<SubmissionPreparation> => {
  const expiresAtMs = leaseUntil(nowMs);
  const day = utcDay(nowMs);
  if (expiresAtMs === null || !day) return { kind: "failure", code: "invalid_clock" };
  const key = codexResetRedemptionKey(context.account.accountIdHash, context.account.quotaGeneration);
  const dailyKey = codexResetGlobalDailyKey(day);
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const prepared = await prepareSubmissionAttempt(kv, context, candidate, expected, nowMs, clock, maxGlobalPerDay, expiresAtMs, day, key, dailyKey);
    if (prepared) return prepared;
  }
  return { kind: "failure", code: "kv_cas_exhausted" };
};

/**
 * Last-moment checks before the renewal CAS: the lease may only be extended
 * while the observed window is still open and inside the claim's UTC day.
 */
const renewalCommitGate = (
  current: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  clock: () => number
): Readonly<{ kind: "ready"; nowMs: number }> | Readonly<{ kind: "failure"; code: string }> => {
  // A slow strong read must not commit a new lease after the observed quota
  // window has naturally reopened.
  const nowMs = readClock(clock);
  if (nowMs === null) return { kind: "failure", code: "invalid_clock" };
  if (!quotaWindowIsOpen(candidate, nowMs)) return { kind: "failure", code: "quota_window_expired" };
  if (!claimedDuringCurrentUtcDay(current, nowMs)) return { kind: "failure", code: "claim_day_elapsed" };
  if (current.lease_expires_at_ms <= nowMs) return { kind: "failure", code: "stale_owner" };
  return { kind: "ready", nowMs };
};

/** The renewed record, or the failure code that stops the lease extension. */
const renewedLeaseRecord = (
  current: CodexResetRedemptionRecord,
  nowMs: number
): Readonly<{ kind: "renewed"; record: CodexResetRedemptionRecord }> | Readonly<{ kind: "failure"; code: string }> => {
  const expiresAtMs = leaseUntil(nowMs);
  const renewedFence = nextFence(current.fence);
  if (expiresAtMs === null) return { kind: "failure", code: "invalid_clock" };
  if (renewedFence === null) return { kind: "failure", code: "owner_fence_exhausted" };
  return {
    kind: "renewed",
    record: {
      ...current,
      fence: renewedFence,
      lease_expires_at_ms: expiresAtMs,
      updated_at_ms: nowMs,
    },
  };
};

/**
 * One compare-and-set attempt of the pre-redeem lease renewal. `null` means the
 * CAS lost and the caller must try again.
 */
const renewSubmittedAttempt = async (
  kv: Deno.Kv,
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  expected: CodexResetRedemptionRecord,
  clock: () => number,
  key: Deno.KvKey
): Promise<SubmissionRenewal | null> => {
  const nowBeforeRead = readClock(clock);
  if (nowBeforeRead === null) return { kind: "failure", code: "invalid_clock" };
  if (!quotaWindowIsOpen(candidate, nowBeforeRead)) return { kind: "failure", code: "quota_window_expired" };

  let entry: Deno.KvEntryMaybe<CodexResetRedemptionRecord>;
  try {
    entry = await kv.get<CodexResetRedemptionRecord>(key, { consistency: "strong" });
  } catch {
    return { kind: "failure", code: "kv_unavailable" };
  }
  const current = parseCodexResetRedemptionRecord(entry.value);
  if (!isOwnedRecordInState(current, context, expected, "submitted")) return { kind: "failure", code: "stale_owner" };
  if (current.routing_generation !== candidate.routingGeneration || current.lease_expires_at_ms <= nowBeforeRead) {
    return { kind: "failure", code: "stale_owner" };
  }
  if (!claimedDuringCurrentUtcDay(current, nowBeforeRead)) return { kind: "failure", code: "claim_day_elapsed" };

  const usage = await readUsageGate(kv, context.account.accountIdHash);
  if (usage.kind === "failure") return { kind: "failure", code: usage.code };

  const fences = await readRequiredFences(kv, candidate);
  if (fences.kind === "failure") return { kind: "failure", code: fences.code };

  const gate = renewalCommitGate(current, candidate, clock);
  if (gate.kind === "failure") return { kind: "failure", code: gate.code };
  const renewed = renewedLeaseRecord(current, gate.nowMs);
  if (renewed.kind === "failure") return { kind: "failure", code: renewed.code };
  try {
    const committed = await withFenceChecks(kv.atomic().check(entry), [...fences.entries, ...usage.entries])
      .set(key, renewed.record)
      .commit();
    if (committed.ok) return { kind: "renewed", record: renewed.record };
  } catch {
    return { kind: "failure", code: "kv_unavailable" };
  }
  return null;
};

/**
 * The `submitted` state is intentionally durable before a provider invocation
 * because a process can die after issuing it. Renew ownership again at the
 * last possible moment so a worker paused after `prepareSubmission()` cannot
 * spend a reset after losing its lease, routing fence, or auth-pool fence.
 *
 * There must be no await between a successful return and invoking `redeem`.
 */
const renewSubmittedForRedeem = async (
  kv: Deno.Kv,
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  expected: CodexResetRedemptionRecord,
  clock: () => number
): Promise<SubmissionRenewal> => {
  const key = codexResetRedemptionKey(context.account.accountIdHash, context.account.quotaGeneration);
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    const renewed = await renewSubmittedAttempt(kv, context, candidate, expected, clock, key);
    if (renewed) return renewed;
  }
  return { kind: "failure", code: "kv_cas_exhausted" };
};

const receiptId = (value: unknown): string | null => (isNonEmptyText(value, 512) ? value : null);

/**
 * Receipt identifiers are optional optimization hints: reconciliation is
 * required to work by deterministic idempotency key. Keep an unapproved
 * receipt in process memory only, never in the durable record or telemetry.
 */
const durableReceiptId = (provider: Pick<CodexUsageResetProvider, "contract">, value: unknown): string | null =>
  providerReceiptIdsSafeToPersistAndLog(provider) ? receiptId(value) : null;

const validInventory = (inventory: unknown, nowMs: number): inventory is ResetInventory =>
  isRecord(inventory) &&
  isSafeNonnegativeInteger(inventory.availableCount) &&
  isSafeMs(inventory.observedAtMs) &&
  Array.isArray(inventory.credits) &&
  inventory.observedAtMs <= nowMs &&
  nowMs - inventory.observedAtMs <= CODEX_BANKED_RESET_INVENTORY_MAX_AGE_MS &&
  inventory.credits.every(
    (credit) =>
      isRecord(credit) &&
      isNonEmptyText(credit.id, 512) &&
      isNonEmptyText(credit.status, 128) &&
      isNonEmptyText(credit.resetType, 128) &&
      (credit.expiresAtMs === null || isSafeMs(credit.expiresAtMs))
  ) &&
  // The production adapter rejects duplicate opaque IDs. Retain that same
  // invariant at the evaluator boundary so an injected or future provider
  // cannot make an ambiguous inventory look selectable.
  new Set(inventory.credits.map((credit) => credit.id)).size === inventory.credits.length &&
  inventory.credits.filter((credit) => credit.status === "available").length === inventory.availableCount;

type InventoryCreditSelection =
  Readonly<{ kind: "selected"; credit: ResetInventoryCredit }> | Readonly<{ kind: "empty" }> | Readonly<{ kind: "no_eligible_credit" }>;

/**
 * The only selectable credits are explicit, currently valid Codex
 * rate-limit credits. Finite expiry wins over non-expiring credits; callers
 * add the account slot as the next global tie-breaker.
 */
const selectInventoryCredit = (inventory: ResetInventory, provider: CodexUsageResetProvider, nowMs: number): InventoryCreditSelection => {
  if (inventory.availableCount === 0) return { kind: "empty" };
  const candidates = inventory.credits.filter(
    (credit) =>
      credit.status === "available" &&
      credit.resetType === "codex_rate_limits" &&
      providerSupportsResetType(provider, credit.resetType) &&
      (credit.expiresAtMs === null || credit.expiresAtMs > nowMs)
  );
  if (!candidates.length) return { kind: "no_eligible_credit" };
  candidates.sort((left, right) => {
    const leftExpiry = left.expiresAtMs ?? Number.POSITIVE_INFINITY;
    const rightExpiry = right.expiresAtMs ?? Number.POSITIVE_INFINITY;
    return leftExpiry - rightExpiry || left.id.localeCompare(right.id);
  });
  const best = candidates.at(0);
  if (!best) return { kind: "no_eligible_credit" };
  return { kind: "selected", credit: best };
};

const validRedeemResult = (value: unknown): value is RedeemResetResult => {
  if (!isRecord(value) || typeof value.kind !== "string") return false;
  switch (value.kind) {
    case "completed":
    case "accepted":
    case "already_redeemed":
      return receiptId(value.providerReceiptId) !== null;
    case "rejected":
      return typeof value.reason === "string";
    case "unknown":
      return value.providerReceiptId === null || receiptId(value.providerReceiptId) !== null;
    default:
      return false;
  }
};

const stateWith = (
  record: CodexResetRedemptionRecord,
  state: CodexResetRedemptionState,
  nowMs: number,
  patch: Partial<Pick<CodexResetRedemptionRecord, "provider_receipt_id" | "submitted_at_ms" | "verified_at_ms" | "last_error_code">> = {}
): CodexResetRedemptionRecord => ({
  ...record,
  ...patch,
  state,
  updated_at_ms: nowMs,
});

const rejectOwned = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  nowMs: number,
  code: string
): Promise<CodexResetRedemptionRecord | null> =>
  await updateOwnedRecord(kv, context, record, (current) => stateWith(current, "rejected", nowMs, { last_error_code: code }));

const unknownOwned = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  nowMs: number,
  code: string,
  providerReceiptId: string | null
): Promise<CodexResetRedemptionRecord | null> =>
  await updateOwnedRecord(kv, context, record, (current) =>
    stateWith(current, "unknown", nowMs, {
      provider_receipt_id: providerReceiptId ?? current.provider_receipt_id,
      last_error_code: code,
    })
  );

const preserveReceipt = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  nowMs: number,
  providerReceiptId: string | null
): Promise<CodexResetRedemptionRecord | null> =>
  await updateOwnedRecord(kv, context, record, (current) =>
    stateWith(current, "submitted", nowMs, {
      provider_receipt_id: providerReceiptId,
      submitted_at_ms: current.submitted_at_ms ?? nowMs,
      last_error_code: null,
    })
  );

const unknownOutcome = (
  telemetry: CodexBankedResetTelemetry,
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  reason: string,
  record: CodexResetRedemptionRecord
): CodexBankedResetOutcome => {
  emit(telemetry, "codex_reset_unknown", telemetryFields(context, candidate, { state: record.state, reason }));
  metric(telemetry, "codex_reset_unknown_total", 1, telemetryFields(context, candidate, {}));
  return outcome("pending", reason, context, record);
};

const liveSubmissionPolicyReason = (dependencies: CodexBankedResetDependencies): string | null => {
  return loadLiveSubmissionConfig(dependencies).reason;
};

const loadLiveSubmissionConfig = (
  dependencies: CodexBankedResetDependencies
): Readonly<{ config: CodexBankedResetConfig; reason: null }> | Readonly<{ config: null; reason: string }> => {
  let config: CodexBankedResetConfig;
  try {
    config = dependencies.reloadConfig?.() ?? dependencies.config;
  } catch {
    return { config: null, reason: "configuration_unavailable" };
  }
  const reason = policyReason(config) ?? providerPolicyReason(config, dependencies.provider);
  return reason || config.mode !== "live" ? { config: null, reason: reason ?? "mode_not_live" } : { config, reason: null };
};

const verifyOwned = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  provider: CodexUsageResetProvider,
  clock: () => number,
  telemetry: CodexBankedResetTelemetry
): Promise<CodexBankedResetOutcome> => {
  if (candidate.signal?.aborted) return outcome("pending", "client_aborted_before_verification", context, record);
  const startedAt = performance.now();
  let result: unknown;
  try {
    result = await provider.verifyApplied(context.account, candidate.signal ?? new AbortController().signal);
  } catch {
    const nowMs = readClock(clock);
    if (nowMs === null) return outcome("pending", "invalid_clock", context, record);
    const unknown = await unknownOwned(kv, context, record, nowMs, "verification_unavailable", record.provider_receipt_id);
    return unknownOutcome(telemetry, context, candidate, "verification_unavailable", unknown ?? record);
  }
  const nowMs = readClock(clock);
  if (nowMs === null) return outcome("pending", "invalid_clock", context, record);
  if (result !== true) {
    const reason = typeof result === "boolean" ? "verification_not_applied" : "verification_response_invalid";
    const unknown = await unknownOwned(kv, context, record, nowMs, reason, record.provider_receipt_id);
    return unknownOutcome(telemetry, context, candidate, reason, unknown ?? record);
  }
  const finalized = await updateOwnedRecord(kv, context, record, (current) =>
    stateWith(current, "verified", nowMs, { verified_at_ms: nowMs, last_error_code: null })
  );
  if (!finalized) return outcome("pending", "verification_cas_failed", context, record);
  emit(telemetry, "codex_reset_verified", telemetryFields(context, candidate, { state: "verified" }));
  metric(telemetry, "codex_reset_verified_total", 1, telemetryFields(context, candidate, {}));
  metric(telemetry, "codex_reset_verification_latency_ms", Math.max(0, Math.round(performance.now() - startedAt)), telemetryFields(context, candidate, {}));
  metric(telemetry, "codex_reset_estimated_spend_total", 1, telemetryFields(context, candidate, {}));
  return outcome("verified", "verified", context, finalized);
};

/**
 * The upstream adapter has already parsed a documented terminal redemption
 * result (`reset` or `already_redeemed`). Lost, malformed, non-2xx, and unknown
 * responses never reach this path and remain durable `unknown`.
 */
const finalizeDocumentedRedeemOutcome = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  nowMs: number,
  telemetry: CodexBankedResetTelemetry,
  redeemOutcome: "reset" | "already_redeemed"
): Promise<CodexBankedResetOutcome> => {
  const finalized = await updateOwnedRecord(kv, context, record, (current) =>
    stateWith(current, "verified", nowMs, { verified_at_ms: nowMs, last_error_code: null })
  );
  if (!finalized) return outcome("pending", "redeem_outcome_finalization_cas_failed", context, record);
  emit(
    telemetry,
    "codex_reset_verified",
    telemetryFields(context, candidate, {
      state: "verified",
      verification_source: "redeem_outcome",
      redeem_outcome: redeemOutcome,
    })
  );
  metric(telemetry, "codex_reset_verified_total", 1, telemetryFields(context, candidate, {}));
  metric(telemetry, "codex_reset_verification_latency_ms", 0, telemetryFields(context, candidate, {}));
  metric(telemetry, "codex_reset_estimated_spend_total", 1, telemetryFields(context, candidate, {}));
  return outcome("verified", `redeem_outcome_${redeemOutcome}`, context, finalized);
};

/**
 * The capability fields an injected contract may be missing. The contract is
 * supplied by the caller, so it is read the same defensive way
 * `codex_banked_reset_provider.ts` reads it before trusting one: an absent
 * capability is never evidence of support.
 */
type UnverifiedResetContract = Readonly<{
  lookup?: Readonly<{ byIdempotencyKey?: boolean; byProviderReceiptId?: boolean }> | null;
  verification?: Readonly<{ independentlyVerifiable?: boolean }> | null;
}>;

/**
 * A terminal redeem outcome can only be reconciled when the contract proves a
 * lookup or an independent verification. The provider check runs first so a
 * provider that does not claim a final outcome is never inspected further.
 */
const terminalOutcomeCannotReconcile = (provider: CodexUsageResetProvider): boolean => {
  if (!providerTreatsRedeemOutcomeAsFinal(provider)) return false;
  const contract: UnverifiedResetContract = provider.contract;
  return !contract.lookup?.byIdempotencyKey && !contract.lookup?.byProviderReceiptId && !contract.verification?.independentlyVerifiable;
};

const reconcileOwned = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  provider: CodexUsageResetProvider,
  clock: () => number,
  telemetry: CodexBankedResetTelemetry
): Promise<CodexBankedResetOutcome> => {
  if (candidate.signal?.aborted) return outcome("pending", "client_aborted_before_reconciliation", context, record);
  let cannotReconcile: boolean;
  try {
    cannotReconcile = terminalOutcomeCannotReconcile(provider);
  } catch {
    return outcome("pending", "provider_contract_unproven", context, record);
  }
  if (cannotReconcile) {
    return outcome("pending", "terminal_outcome_ambiguous", context, record);
  }
  let lookedUp: RedeemResetResult;
  try {
    lookedUp = await provider.lookup(
      { ...context.account, idempotencyKey: context.idempotencyKey, providerReceiptId: record.provider_receipt_id },
      candidate.signal ?? new AbortController().signal
    );
  } catch {
    const nowMs = readClock(clock);
    if (nowMs === null) return outcome("pending", "invalid_clock", context, record);
    const unknown = await unknownOwned(kv, context, record, nowMs, "lookup_unavailable", record.provider_receipt_id);
    return unknownOutcome(telemetry, context, candidate, "lookup_unavailable", unknown ?? record);
  }
  const nowMs = readClock(clock);
  if (nowMs === null) return outcome("pending", "invalid_clock", context, record);
  if (!validRedeemResult(lookedUp)) {
    const unknown = await unknownOwned(kv, context, record, nowMs, "lookup_response_invalid", record.provider_receipt_id);
    return unknownOutcome(telemetry, context, candidate, "lookup_response_invalid", unknown ?? record);
  }
  if (lookedUp.kind === "rejected" || lookedUp.kind === "unknown") {
    // A recovery lookup can race a slow original provider invocation after its
    // lease expires. A negative lookup alone is therefore not proof that a
    // reset was never spent; only independent verification may resolve it.
    return await verifyOwned(kv, context, record, candidate, provider, clock, telemetry);
  }
  const receipt = receiptId(lookedUp.providerReceiptId);
  if (!receipt) {
    const unknown = await unknownOwned(kv, context, record, nowMs, "lookup_response_invalid", record.provider_receipt_id);
    return unknownOutcome(telemetry, context, candidate, "lookup_response_invalid", unknown ?? record);
  }
  const submitted = await preserveReceipt(kv, context, record, nowMs, durableReceiptId(provider, receipt));
  if (!submitted) return outcome("pending", "lookup_cas_failed", context, record);
  return await verifyOwned(kv, context, submitted, candidate, provider, clock, telemetry);
};

/**
 * A claimed record may only be submitted while its UTC day and quota window are
 * still open. A violation is durably rejected, exactly as before.
 */
const rejectIfClaimWindowClosed = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  nowMs: number
): Promise<CodexBankedResetOutcome | null> => {
  if (!claimedDuringCurrentUtcDay(record, nowMs)) {
    const rejected = await rejectOwned(kv, context, record, nowMs, "claim_day_elapsed");
    return outcome("rejected", "claim_day_elapsed", context, rejected ?? record);
  }
  if (!quotaWindowIsOpen(candidate, nowMs)) {
    const rejected = await rejectOwned(kv, context, record, nowMs, "quota_window_expired");
    return outcome("rejected", "quota_window_expired", context, rejected ?? record);
  }
  return null;
};

/** Pre-inventory policy and clock checks; `ready` carries the observed time. */
const submissionPreflight = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  dependencies: CodexBankedResetDependencies,
  clock: () => number
): Promise<Readonly<{ kind: "ready"; nowMs: number }> | Readonly<{ kind: "outcome"; outcome: CodexBankedResetOutcome }>> => {
  const initialPolicy = liveSubmissionPolicyReason(dependencies);
  if (initialPolicy) return { kind: "outcome", outcome: outcome("pending", `new_submission_${initialPolicy}`, context, record) };
  const nowBeforeInventory = readClock(clock);
  if (nowBeforeInventory === null) return { kind: "outcome", outcome: outcome("pending", "invalid_clock", context, record) };
  const closed = await rejectIfClaimWindowClosed(kv, context, record, candidate, nowBeforeInventory);
  if (closed) return { kind: "outcome", outcome: closed };
  if (candidate.signal?.aborted) {
    const rejected = await rejectOwned(kv, context, record, nowBeforeInventory, "client_aborted_before_submission");
    return { kind: "outcome", outcome: outcome("rejected", "client_aborted_before_submission", context, rejected ?? record) };
  }
  return { kind: "ready", nowMs: nowBeforeInventory };
};

/** Inventory fallback: read once, re-check the claim, and select one credit. */
const resolveInventoryCredit = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  dependencies: CodexBankedResetDependencies,
  clock: () => number,
  telemetry: CodexBankedResetTelemetry
): Promise<Readonly<{ kind: "selected"; credit: ResetInventoryCredit; nowMs: number }> | Readonly<{ kind: "outcome"; outcome: CodexBankedResetOutcome }>> => {
  let inventory: ResetInventory;
  try {
    inventory = await dependencies.provider.readInventory(context.account, boundedInventorySignal(candidate.signal));
  } catch {
    const nowMs = readClock(clock);
    if (nowMs === null) return { kind: "outcome", outcome: outcome("pending", "invalid_clock", context, record) };
    const rejected = await rejectOwned(kv, context, record, nowMs, "inventory_unavailable");
    emit(telemetry, "codex_reset_rejected", telemetryFields(context, candidate, { state: "rejected", reason: "inventory_unavailable" }));
    return { kind: "outcome", outcome: outcome("rejected", "inventory_unavailable", context, rejected ?? record) };
  }
  const nowMs = readClock(clock);
  if (nowMs === null) return { kind: "outcome", outcome: outcome("pending", "invalid_clock", context, record) };
  const closed = await rejectIfClaimWindowClosed(kv, context, record, candidate, nowMs);
  if (closed) return { kind: "outcome", outcome: closed };
  if (!validInventory(inventory, nowMs)) {
    const rejected = await rejectOwned(kv, context, record, nowMs, "inventory_response_invalid_or_unsupported");
    emit(telemetry, "codex_reset_rejected", telemetryFields(context, candidate, { state: "rejected", reason: "inventory_response_invalid_or_unsupported" }));
    return { kind: "outcome", outcome: outcome("rejected", "inventory_response_invalid_or_unsupported", context, rejected ?? record) };
  }
  const selection = selectInventoryCredit(inventory, dependencies.provider, nowMs);
  if (selection.kind !== "selected") {
    const reason = selection.kind === "empty" ? "inventory_empty" : "inventory_no_eligible_codex_credit";
    const rejected = await rejectOwned(kv, context, record, nowMs, reason);
    emit(telemetry, "codex_reset_rejected", telemetryFields(context, candidate, { state: "rejected", reason }));
    return { kind: "outcome", outcome: outcome("rejected", reason, context, rejected ?? record) };
  }
  return { kind: "selected", credit: selection.credit, nowMs };
};

/** The credit to spend must still be a live, supported Codex rate-limit credit. */
const isUsableSelectedCredit = (credit: ResetInventoryCredit | undefined, provider: CodexUsageResetProvider, nowMs: number): credit is ResetInventoryCredit =>
  credit !== undefined &&
  isNonEmptyText(credit.id, 512) &&
  credit.status === "available" &&
  credit.resetType === "codex_rate_limits" &&
  providerSupportsResetType(provider, credit.resetType) &&
  (credit.expiresAtMs === null || (isSafeMs(credit.expiresAtMs) && credit.expiresAtMs > nowMs));

/**
 * Resolve the credit for this submission: the evaluator's in-memory pick when
 * present, otherwise one read of the provider inventory.
 */
const resolveSelectedCredit = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  dependencies: CodexBankedResetDependencies,
  clock: () => number,
  telemetry: CodexBankedResetTelemetry,
  nowBeforeInventory: number
): Promise<Readonly<{ kind: "selected"; credit: ResetInventoryCredit; nowMs: number }> | Readonly<{ kind: "outcome"; outcome: CodexBankedResetOutcome }>> => {
  let selectedCredit = candidate.selectedCredit;
  let nowAfterInventory = nowBeforeInventory;
  if (!selectedCredit) {
    const resolved = await resolveInventoryCredit(kv, context, record, candidate, dependencies, clock, telemetry);
    if (resolved.kind === "outcome") return { kind: "outcome", outcome: resolved.outcome };
    selectedCredit = resolved.credit;
    nowAfterInventory = resolved.nowMs;
  }
  if (!isUsableSelectedCredit(selectedCredit, dependencies.provider, nowAfterInventory)) {
    const rejected = await rejectOwned(kv, context, record, nowAfterInventory, "selected_credit_invalid_or_expired");
    emit(telemetry, "codex_reset_rejected", telemetryFields(context, candidate, { state: "rejected", reason: "selected_credit_invalid_or_expired" }));
    return { kind: "outcome", outcome: outcome("rejected", "selected_credit_invalid_or_expired", context, rejected ?? record) };
  }
  const closed = await rejectIfClaimWindowClosed(kv, context, record, candidate, nowAfterInventory);
  if (closed) return { kind: "outcome", outcome: closed };
  return { kind: "selected", credit: selectedCredit, nowMs: nowAfterInventory };
};

/**
 * Cross the durable `claimed -> submitted` side-effect boundary after the last
 * policy re-read, keeping the observed inventory time as the commit time.
 */
const prepareLiveSubmission = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  dependencies: CodexBankedResetDependencies,
  clock: () => number,
  nowAfterInventory: number
): Promise<Readonly<{ kind: "prepared"; record: CodexResetRedemptionRecord }> | Readonly<{ kind: "outcome"; outcome: CodexBankedResetOutcome }>> => {
  // Re-read the kill switch after inventory and immediately before the fenced
  // side-effect boundary. A disable leaves `claimed` intact and makes no call.
  const finalConfig = loadLiveSubmissionConfig(dependencies);
  if (finalConfig.config === null) return { kind: "outcome", outcome: outcome("pending", `new_submission_${finalConfig.reason}`, context, record) };
  if (candidate.signal?.aborted) {
    const rejected = await rejectOwned(kv, context, record, nowAfterInventory, "client_aborted_before_submission");
    return { kind: "outcome", outcome: outcome("rejected", "client_aborted_before_submission", context, rejected ?? record) };
  }
  // Inventory validation and policy checks may take long enough for the
  // current quota window to end. Do not cross the durable side-effect
  // boundary after that deadline.
  const nowBeforePreparation = readClock(clock);
  if (nowBeforePreparation === null) return { kind: "outcome", outcome: outcome("pending", "invalid_clock", context, record) };
  const closed = await rejectIfClaimWindowClosed(kv, context, record, candidate, nowBeforePreparation);
  if (closed) return { kind: "outcome", outcome: closed };
  const prepared = await prepareSubmission(kv, context, candidate, record, nowBeforePreparation, clock, finalConfig.config.maxGlobalPerDay);
  if (prepared.kind === "failure") {
    return { kind: "outcome", outcome: outcome(prepared.code === "global_limit_reached" ? "skipped" : "pending", prepared.code, context, record) };
  }
  return { kind: "prepared", record: prepared.record };
};

const submitClaimed = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  dependencies: CodexBankedResetDependencies,
  clock: () => number,
  telemetry: CodexBankedResetTelemetry
): Promise<CodexBankedResetOutcome> => {
  const preflight = await submissionPreflight(kv, context, record, candidate, dependencies, clock);
  if (preflight.kind === "outcome") return preflight.outcome;
  const resolution = await resolveSelectedCredit(kv, context, record, candidate, dependencies, clock, telemetry, preflight.nowMs);
  if (resolution.kind === "outcome") return resolution.outcome;
  const preparation = await prepareLiveSubmission(kv, context, record, candidate, dependencies, clock, resolution.nowMs);
  if (preparation.kind === "outcome") return preparation.outcome;
  return await renewAndRedeem(kv, context, candidate, dependencies, clock, telemetry, resolution.credit, preparation.record);
};
/**
 * The last checks before the provider call. This function deliberately does not
 * await: nothing may run between the final kill-switch read and `redeem`
 * except these in-memory checks.
 */
const redeemGate = (
  candidate: CodexBankedResetCandidate,
  record: CodexResetRedemptionRecord,
  context: ResetContext,
  clock: () => number
): Readonly<{ kind: "ready"; nowMs: number }> | Readonly<{ kind: "outcome"; outcome: CodexBankedResetOutcome }> => {
  const nowMs = readClock(clock);
  if (nowMs === null) return { kind: "outcome", outcome: outcome("pending", "invalid_clock", context, record) };
  if (!claimedDuringCurrentUtcDay(record, nowMs)) return { kind: "outcome", outcome: outcome("pending", "claim_day_elapsed", context, record) };
  if (!quotaWindowIsOpen(candidate, nowMs)) return { kind: "outcome", outcome: outcome("pending", "quota_window_expired", context, record) };
  if (record.lease_expires_at_ms <= nowMs) return { kind: "outcome", outcome: outcome("pending", "stale_owner", context, record) };
  return { kind: "ready", nowMs };
};

/**
 * A parsed terminal redeem outcome is finalized directly when the contract says
 * that outcome is authoritative; `null` leaves the result to the ordinary path.
 */
const finalizeTerminalRedeemResult = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  provider: CodexUsageResetProvider,
  nowMs: number,
  submittedResult: RedeemResetResult,
  telemetry: CodexBankedResetTelemetry
): Promise<CodexBankedResetOutcome | null> => {
  if (submittedResult.kind !== "completed" && submittedResult.kind !== "already_redeemed") return null;
  if (!providerTreatsRedeemOutcomeAsFinal(provider)) return null;
  emit(telemetry, "codex_reset_submitted", telemetryFields(context, candidate, { state: "submitted", provider_receipt_id: null }));
  return await finalizeDocumentedRedeemOutcome(
    kv,
    context,
    record,
    candidate,
    nowMs,
    telemetry,
    submittedResult.kind === "completed" ? "reset" : "already_redeemed"
  );
};

/** Record the receipt durably and verify the reset it belongs to. */
const persistReceiptAndVerify = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  provider: CodexUsageResetProvider,
  nowMs: number,
  receipt: string,
  clock: () => number,
  telemetry: CodexBankedResetTelemetry
): Promise<CodexBankedResetOutcome> => {
  const persistedReceipt = await preserveReceipt(kv, context, record, nowMs, durableReceiptId(provider, receipt));
  if (!persistedReceipt) return outcome("pending", "receipt_cas_failed", context, record);
  emit(
    telemetry,
    "codex_reset_submitted",
    telemetryFields(context, candidate, {
      state: "submitted",
      provider_receipt_id: durableReceiptId(provider, receipt),
    })
  );
  return await verifyOwned(kv, context, persistedReceipt, candidate, provider, clock, telemetry);
};

/** Invoke `redeem` once and finalize the durable record for its outcome. */
const redeemSubmittedOwned = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  provider: CodexUsageResetProvider,
  credit: ResetInventoryCredit,
  clock: () => number,
  telemetry: CodexBankedResetTelemetry
): Promise<CodexBankedResetOutcome> => {
  let submittedPromise: Promise<RedeemResetResult>;
  try {
    // Do not insert telemetry or another await between the final synchronous
    // kill-switch check in the caller and starting the provider invocation.
    submittedPromise = provider.redeem(
      { ...context.account, idempotencyKey: context.idempotencyKey, creditId: credit.id },
      candidate.signal ?? new AbortController().signal
    );
  } catch {
    const nowMs = readClock(clock);
    if (nowMs === null) return outcome("pending", "invalid_clock", context, record);
    const unknown = await unknownOwned(kv, context, record, nowMs, "submit_transport_unknown", null);
    return unknownOutcome(telemetry, context, candidate, "submit_transport_unknown", unknown ?? record);
  }
  emit(telemetry, "codex_reset_submit_started", telemetryFields(context, candidate, { state: "submitted" }));
  metric(telemetry, "codex_reset_submission_attempts_total", 1, telemetryFields(context, candidate, {}));

  let submittedResult: RedeemResetResult;
  try {
    submittedResult = await submittedPromise;
  } catch {
    const nowMs = readClock(clock);
    if (nowMs === null) return outcome("pending", "invalid_clock", context, record);
    const unknown = await unknownOwned(kv, context, record, nowMs, "submit_transport_unknown", null);
    return unknownOutcome(telemetry, context, candidate, "submit_transport_unknown", unknown ?? record);
  }
  const nowAfterRedeem = readClock(clock);
  if (nowAfterRedeem === null) return outcome("pending", "invalid_clock", context, record);
  if (!validRedeemResult(submittedResult)) {
    const unknown = await unknownOwned(kv, context, record, nowAfterRedeem, "submit_response_invalid", null);
    return unknownOutcome(telemetry, context, candidate, "submit_response_invalid", unknown ?? record);
  }
  if (submittedResult.kind === "rejected") {
    const rejected = await rejectOwned(kv, context, record, nowAfterRedeem, "provider_rejected");
    emit(telemetry, "codex_reset_rejected", telemetryFields(context, candidate, { state: "rejected", reason: "provider_rejected" }));
    return outcome("rejected", "provider_rejected", context, rejected ?? record);
  }
  if (submittedResult.kind === "unknown") {
    const unknown = await unknownOwned(
      kv,
      context,
      record,
      nowAfterRedeem,
      "provider_commit_unknown",
      durableReceiptId(provider, submittedResult.providerReceiptId)
    );
    return unknownOutcome(telemetry, context, candidate, "provider_commit_unknown", unknown ?? record);
  }
  const terminal = await finalizeTerminalRedeemResult(kv, context, record, candidate, provider, nowAfterRedeem, submittedResult, telemetry);
  if (terminal) return terminal;
  const receipt = receiptId(submittedResult.providerReceiptId);
  if (!receipt) {
    const unknown = await unknownOwned(kv, context, record, nowAfterRedeem, "submit_response_invalid", null);
    return unknownOutcome(telemetry, context, candidate, "submit_response_invalid", unknown ?? record);
  }
  return await persistReceiptAndVerify(kv, context, record, candidate, provider, nowAfterRedeem, receipt, clock, telemetry);
};

/**
 * Renew the lease at the last possible moment and then invoke the provider.
 * Every policy read here is synchronous, so a disable observed after the final
 * renewal still prevents the redemption.
 */
const renewAndRedeem = async (
  kv: Deno.Kv,
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  dependencies: CodexBankedResetDependencies,
  clock: () => number,
  telemetry: CodexBankedResetTelemetry,
  credit: ResetInventoryCredit,
  preparedRecord: CodexResetRedemptionRecord
): Promise<CodexBankedResetOutcome> => {
  // `prepareSubmission` itself awaits strong reads and a CAS. Re-read the
  // kill switch after that durable transition. A disable visible at this
  // final pre-renewal check leaves the conservative `submitted` record
  // available for non-submitting recovery and makes no provider call.
  const beforeRedeemPolicy = liveSubmissionPolicyReason(dependencies);
  if (beforeRedeemPolicy) return outcome("pending", `new_submission_${beforeRedeemPolicy}`, context, preparedRecord);
  if (candidate.signal?.aborted) {
    const nowMs = readClock(clock);
    if (nowMs === null) return outcome("pending", "invalid_clock", context, preparedRecord);
    const unknown = await unknownOwned(kv, context, preparedRecord, nowMs, "client_aborted_after_submission", null);
    return unknownOutcome(telemetry, context, candidate, "client_aborted_after_submission", unknown ?? preparedRecord);
  }
  const renewed = await renewSubmittedForRedeem(kv, context, candidate, preparedRecord, clock);
  if (renewed.kind === "failure") return outcome("pending", renewed.code, context, preparedRecord);
  // The last lease/fence renewal itself awaits KV. Re-read the kill switch
  // synchronously after it returns so a disable that landed during that final
  // renewal cannot proceed to the provider call. `reloadConfig` is
  // deliberately synchronous; do not introduce an await after this point.
  const afterRenewalPolicy = liveSubmissionPolicyReason(dependencies);
  if (afterRenewalPolicy) {
    return outcome("pending", `new_submission_${afterRenewalPolicy}`, context, renewed.record);
  }
  if (candidate.signal?.aborted) {
    const nowMs = readClock(clock);
    if (nowMs === null) return outcome("pending", "invalid_clock", context, renewed.record);
    const unknown = await unknownOwned(kv, context, renewed.record, nowMs, "client_aborted_after_submission", null);
    return unknownOutcome(telemetry, context, candidate, "client_aborted_after_submission", unknown ?? renewed.record);
  }
  const gate = redeemGate(candidate, renewed.record, context, clock);
  if (gate.kind === "outcome") return gate.outcome;
  return await redeemSubmittedOwned(kv, context, renewed.record, candidate, dependencies.provider, credit, clock, telemetry);
};

/**
 * The production KV accessor. An unavailable accessor and an explicit null
 * override both fail closed, exactly as before.
 */
const openResetKv = async (dependencies: CodexBankedResetDependencies): Promise<Deno.Kv | null> => {
  try {
    return dependencies.kv === undefined ? await getKv() : dependencies.kv;
  } catch {
    return null;
  }
};

/** A settled record is terminal: it is reported and never reopened. */
const settledRecordDisposition = (
  telemetry: CodexBankedResetTelemetry,
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  existing: CodexResetRedemptionRecord | null,
  fields: CodexBankedResetTelemetryFields
): CodexBankedResetOutcome | null => {
  if (existing?.state === "verified") {
    if (existing.routing_generation !== candidate.routingGeneration) {
      // The provider reset was verified for an older routing observation. The
      // old credit is already spent, and the post-reset probe may already have
      // re-blocked the account. Never present that old verification as a
      // recovery candidate for a newer quota circuit.
      emit(
        telemetry,
        "codex_reset_duplicate_prevented",
        telemetryFields(context, candidate, {
          state: "verified",
          fence: existing.fence,
          reason: "routing_generation_stale",
        })
      );
      metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
      return outcome("skipped", "verified_routing_generation_stale", context, existing);
    }
    emit(telemetry, "codex_reset_duplicate_prevented", telemetryFields(context, candidate, { state: "verified", fence: existing.fence }));
    metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
    return outcome("verified", "previously_verified", context, existing);
  }
  if (existing?.state === "rejected") {
    emit(telemetry, "codex_reset_duplicate_prevented", telemetryFields(context, candidate, { state: "rejected", fence: existing.fence }));
    metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
    return outcome("rejected", existing.last_error_code ?? "previously_rejected", context, existing);
  }
  return null;
};

/** Policy gate for the very first claim of an observed quota window. */
const resolveNewSubmissionAllowance = async (
  kv: Deno.Kv,
  context: ResetContext,
  dependencies: CodexBankedResetDependencies,
  reconcileOnly: boolean,
  telemetry: CodexBankedResetTelemetry,
  fields: CodexBankedResetTelemetryFields
): Promise<Readonly<{ kind: "allow"; allowNewSubmission: boolean }> | Readonly<{ kind: "outcome"; outcome: CodexBankedResetOutcome }>> => {
  if (reconcileOnly) return { kind: "outcome", outcome: outcome("skipped", "no_existing_transaction", context) };
  let configForClaim: CodexBankedResetConfig;
  try {
    if (!(await readBankedResetUsage(kv, context.account.accountIdHash)).allowed) {
      return { kind: "outcome", outcome: outcome("skipped", "usage_disabled", context) };
    }
    configForClaim = dependencies.reloadConfig?.() ?? dependencies.config;
  } catch {
    return { kind: "outcome", outcome: outcome("skipped", "configuration_unavailable", context) };
  }
  const reason = policyReason(configForClaim);
  if (reason) return { kind: "outcome", outcome: outcome("skipped", reason, context) };
  const providerReason = providerPolicyReason(configForClaim, dependencies.provider);
  if (providerReason) return { kind: "outcome", outcome: outcome("skipped", providerReason, context) };
  emit(telemetry, "codex_reset_eligible", fields);
  metric(telemetry, "codex_reset_eligible_total", 1, fields);
  if (configForClaim.mode === "shadow") {
    emit(telemetry, "codex_reset_shadow_candidate", fields);
    metric(telemetry, "codex_reset_shadow_candidates_total", 1, fields);
    // Shadow mode deliberately makes no provider call, including inventory.
    return { kind: "outcome", outcome: outcome("skipped", "shadow", context) };
  }
  return { kind: "allow", allowNewSubmission: true };
};

/**
 * Whether this call may create a new submission. An existing `submitted` or
 * `unknown` record is only ever reconciled, never re-submitted.
 */
const resolveSubmissionAllowance = async (
  kv: Deno.Kv,
  context: ResetContext,
  existing: CodexResetRedemptionRecord | null,
  dependencies: CodexBankedResetDependencies,
  reconcileOnly: boolean,
  telemetry: CodexBankedResetTelemetry,
  fields: CodexBankedResetTelemetryFields
): Promise<Readonly<{ kind: "allow"; allowNewSubmission: boolean }> | Readonly<{ kind: "outcome"; outcome: CodexBankedResetOutcome }>> => {
  if (!existing) return await resolveNewSubmissionAllowance(kv, context, dependencies, reconcileOnly, telemetry, fields);
  if (existing.state !== "claimed") return { kind: "allow", allowNewSubmission: false };
  if (reconcileOnly) return { kind: "outcome", outcome: outcome("pending", "unsubmitted_transaction", context, existing) };
  const reason = liveSubmissionPolicyReason(dependencies);
  if (reason) return { kind: "outcome", outcome: outcome("pending", `new_submission_${reason}`, context, existing) };
  return { kind: "allow", allowNewSubmission: true };
};

/** Translate one claim result into the caller-visible outcome. */
const handleClaimResult = async (
  kv: Deno.Kv,
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  dependencies: CodexBankedResetDependencies,
  clock: () => number,
  telemetry: CodexBankedResetTelemetry,
  fields: CodexBankedResetTelemetryFields,
  claimed: ClaimResult
): Promise<CodexBankedResetOutcome> => {
  switch (claimed.kind) {
    case "failure":
      return outcome("skipped", claimed.code, context);
    case "no_transaction":
      return outcome("skipped", "no_existing_transaction", context);
    case "global_limit":
      return outcome("skipped", "global_limit_reached", context);
    case "in_progress":
      emit(telemetry, "codex_reset_duplicate_prevented", telemetryFields(context, candidate, { state: claimed.record.state, fence: claimed.record.fence }));
      metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
      return outcome("pending", "transaction_in_progress", context, claimed.record);
    case "rejected":
      emit(telemetry, "codex_reset_duplicate_prevented", telemetryFields(context, candidate, { state: "rejected", fence: claimed.record.fence }));
      metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
      return outcome("rejected", claimed.record.last_error_code ?? "previously_rejected", context, claimed.record);
    case "verified":
      emit(telemetry, "codex_reset_duplicate_prevented", telemetryFields(context, candidate, { state: "verified", fence: claimed.record.fence }));
      metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
      return outcome("verified", "previously_verified", context, claimed.record);
    case "submit":
      emit(
        telemetry,
        "codex_reset_claimed",
        telemetryFields(context, candidate, {
          state: "claimed",
          fence: claimed.record.fence,
          takeover: claimed.tookOver,
        })
      );
      return await submitClaimed(kv, context, claimed.record, candidate, dependencies, clock, telemetry);
    case "reconcile":
      emit(
        telemetry,
        "codex_reset_duplicate_prevented",
        telemetryFields(context, candidate, {
          state: claimed.record.state,
          fence: claimed.record.fence,
          takeover: claimed.tookOver,
        })
      );
      metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
      return await reconcileOwned(kv, context, claimed.record, candidate, dependencies.provider, clock, telemetry);
    default: {
      // Every variant above is handled, so this branch is unreachable. Assigning
      // to a `never` makes a future variant a compile error instead of a silent
      // `undefined` return.
      const unhandled: never = claimed;
      throw new Error(`unhandled codex banked reset claim result: ${JSON.stringify(unhandled)}`);
    }
  }
};

const attemptInternal = async (
  candidate: CodexBankedResetCandidate,
  dependencies: CodexBankedResetDependencies,
  reconcileOnly: boolean
): Promise<CodexBankedResetOutcome> => {
  const hash = dependencies.hash ?? sha256Hex;
  const context = await makeResetContext(candidate, hash);
  if (!context) return outcome("skipped", "invalid_quota_generation");
  const telemetry = dependencies.telemetry ?? defaultTelemetry;
  const fields = telemetryFields(context, candidate, {});
  const kv = await openResetKv(dependencies);
  if (!kv) return outcome("skipped", "kv_unavailable", context);

  const existing = await readExistingRecord(kv, context);
  if (existing.code) return outcome("skipped", existing.code, context);
  if (existing.record && !matchesContext(existing.record, context)) {
    return outcome("skipped", "redemption_record_context_mismatch", context, existing.record);
  }
  const settled = settledRecordDisposition(telemetry, context, candidate, existing.record, fields);
  if (settled) return settled;
  const allowance = await resolveSubmissionAllowance(kv, context, existing.record, dependencies, reconcileOnly, telemetry, fields);
  if (allowance.kind === "outcome") return allowance.outcome;

  if (!providerSupportsLiveRedemption(dependencies.provider)) {
    if (!existing.record) {
      emit(telemetry, "codex_reset_rejected", telemetryFields(context, candidate, { state: "rejected", reason: "provider_contract_unproven" }));
    }
    return outcome("skipped", "provider_contract_unproven", context, existing.record);
  }
  const clock = dependencies.now ?? Date.now;
  const nowMs = readClock(clock);
  if (nowMs === null) return outcome("skipped", "invalid_clock", context, existing.record);
  const ownerToken = safeOwnerToken(dependencies.newOwnerToken ?? (() => crypto.randomUUID()));
  if (!ownerToken) return outcome("skipped", "owner_token_unavailable", context, existing.record);
  const claimed = await claimTransaction(kv, context, candidate, nowMs, clock, ownerToken, allowance.allowNewSubmission);
  return await handleClaimResult(kv, context, candidate, dependencies, clock, telemetry, fields, claimed);
};

/**
 * Attempt or reconcile exactly one logical reset after normal account
 * failover. New external submissions require live policy and all durable
 * fences; an existing submitted/unknown record is reconciled even while the
 * kill switch is disabled.
 */
export const attemptCodexBankedReset = async (
  candidate: CodexBankedResetCandidate,
  dependencies: CodexBankedResetDependencies
): Promise<CodexBankedResetOutcome> => await attemptInternal(candidate, dependencies, false);

/**
 * Recovery-only path for a durable submitted/unknown record. It never creates
 * a claim or calls `redeem`, so it remains safe during a rollback.
 */
export const reconcileCodexBankedReset = async (
  candidate: CodexBankedResetCandidate,
  dependencies: CodexBankedResetDependencies
): Promise<CodexBankedResetOutcome> => await attemptInternal(candidate, dependencies, true);

type ResolvedPoolCandidate = Readonly<{
  pool: CodexBankedResetPoolCandidate;
  context: ResetContext;
}>;

type SelectedPoolCredit = Readonly<{
  resolved: ResolvedPoolCandidate;
  credit: ResetInventoryCredit;
  creditIdHash: string;
}>;

const poolOutcome = (
  kind: CodexBankedResetPoolOutcome["kind"],
  reason: string,
  selected: CodexBankedResetPoolCandidate | null = null,
  reset: CodexBankedResetOutcome | null = null
): CodexBankedResetPoolOutcome => ({ kind, reason, selected, reset });

const sameShadowFences = (left: CodexResetShadowDecisionRecord["fences"], right: CodexResetShadowDecisionRecord["fences"]): boolean =>
  left.length === right.length &&
  left.every((fence, index) => {
    // The lengths are equal, so this index is always inside `right`.
    const other = right[index];
    return (
      fence.slot === other.slot &&
      fence.account_id_hash === other.account_id_hash &&
      fence.quota_generation === other.quota_generation &&
      fence.routing_generation === other.routing_generation &&
      fence.quota_reset_at_ms === other.quota_reset_at_ms
    );
  });

const loadCurrentPoolConfig = (
  dependencies: CodexBankedResetDependencies
): Readonly<{ config: CodexBankedResetConfig; reason: null }> | Readonly<{ config: null; reason: string }> => {
  try {
    const config = dependencies.reloadConfig?.() ?? dependencies.config;
    if (!config.enabled) return { config: null, reason: "feature_disabled" };
    if (config.mode === "disabled") return { config: null, reason: "mode_disabled" };
    if (config.maxGlobalPerDay <= 0) return { config: null, reason: "global_limit_disabled" };
    if (config.maxPerAccountPerWindow !== 1) return { config: null, reason: "per_account_window_limit_invalid" };
    return { config, reason: null };
  } catch {
    return { config: null, reason: "configuration_unavailable" };
  }
};

/**
 * An existing decision for this exact episode is a duplicate; anything else --
 * an unparseable record, a different episode, or an expired decision -- is
 * unusable and fails closed.
 */
const duplicateShadowDecision = (
  value: unknown,
  record: CodexResetShadowDecisionRecord,
  nowMs: number
): Readonly<{ kind: "duplicate"; record: CodexResetShadowDecisionRecord }> | null => {
  const existing = parseCodexResetShadowDecisionRecord(value);
  if (!existing) return null;
  if (existing.episode_hash !== record.episode_hash || existing.expires_at_ms <= nowMs) return null;
  return { kind: "duplicate", record: existing };
};

const shadowDecisionRecord = async (
  kv: Deno.Kv,
  record: CodexResetShadowDecisionRecord,
  nowMs: number,
  settingsEntries: Deno.KvEntryMaybe<unknown>[]
): Promise<Readonly<{ kind: "written" | "duplicate"; record: CodexResetShadowDecisionRecord }> | null> => {
  const key = codexResetShadowDecisionKey(record.episode_hash);
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    let entry: Deno.KvEntryMaybe<unknown>;
    try {
      entry = await kv.get(key, { consistency: "strong" });
    } catch {
      return null;
    }
    if (entry.value !== null) return duplicateShadowDecision(entry.value, record, nowMs);
    try {
      const usage = record.selected_account_id_hash ? await readBankedResetUsage(kv, record.selected_account_id_hash) : { allowed: true, entries: [] };
      if (!usage.allowed) return null;
      const committed = await withFenceChecks(kv.atomic().check(entry), [...usage.entries, ...settingsEntries])
        .set(key, record)
        .commit();
      if (committed.ok) return { kind: "written", record };
    } catch {
      return null;
    }
  }
  return null;
};

const readShadowDecision = async (kv: Deno.Kv, episodeHash: string): Promise<CodexResetShadowDecisionRecord | null> => {
  try {
    const entry = await kv.get(codexResetShadowDecisionKey(episodeHash), { consistency: "strong" });
    return entry.value === null ? null : parseCodexResetShadowDecisionRecord(entry.value);
  } catch {
    return null;
  }
};

/** Read-only, redacted administrative projection of recent shadow evidence. */
export const listCodexResetShadowDecisions = async (kvOverride?: Deno.Kv | null): Promise<readonly CodexResetShadowDecisionRecord[] | null> => {
  let kv: Deno.Kv | null;
  try {
    kv = kvOverride === undefined ? await getKv() : kvOverride;
  } catch {
    return null;
  }
  if (!kv) return null;
  const decisions: CodexResetShadowDecisionRecord[] = [];
  try {
    for await (const entry of kv.list({ prefix: CODEX_RESET_SHADOW_DECISION_KV_PREFIX }, { limit: 100 })) {
      const parsed = parseCodexResetShadowDecisionRecord(entry.value);
      if (parsed) decisions.push(parsed);
    }
  } catch {
    return null;
  }
  decisions.sort((left, right) => right.created_at_ms - left.created_at_ms || left.episode_hash.localeCompare(right.episode_hash));
  return decisions;
};

/**
 * Evaluate one complete currently fenced blocked cohort. Shadow reads each
 * blocked account's inventory and persists exactly one redacted decision.
 * Live without a decision first performs that same read-only arm phase. A
 * later live evaluation repeats the fence and inventory proof; it reaches the
 * durable ledger only when the exact account and exact opaque credit still
 * match.
 */
/** Pool candidates in ascending slot order, or the reason the pool is malformed. */
const orderPoolCandidates = (
  candidates: readonly CodexBankedResetPoolCandidate[]
): Readonly<{ kind: "ordered"; ordered: readonly CodexBankedResetPoolCandidate[] }> | Readonly<{ kind: "failure"; reason: string }> => {
  if (!candidates.length) return { kind: "failure", reason: "full_pool_missing" };
  const ordered = [...candidates].sort((left, right) => left.slot - right.slot);
  if (!poolSlotsAreValid(ordered)) return { kind: "failure", reason: "full_pool_invalid" };
  return { kind: "ordered", ordered };
};

/** Slots must be safe non-negative integers and unique after the sort. */
const poolSlotsAreValid = (ordered: readonly CodexBankedResetPoolCandidate[]): boolean => {
  let previousSlot: number | null = null;
  for (const candidate of ordered) {
    if (!isSafeNonnegativeInteger(candidate.slot)) return false;
    if (previousSlot !== null && previousSlot === candidate.slot) return false;
    previousSlot = candidate.slot;
  }
  return true;
};

const anyPoolProviderTreatsRedeemOutcomeAsFinal = (ordered: readonly CodexBankedResetPoolCandidate[]): boolean =>
  ordered.some(({ provider }) => providerTreatsRedeemOutcomeAsFinal(provider));

/** Pool policy gate: the usable configuration, or the reason the pool stops. */
const loadPoolSubmissionConfig = (
  ordered: readonly CodexBankedResetPoolCandidate[],
  dependencies: CodexBankedResetDependencies
): Readonly<{ kind: "config"; config: CodexBankedResetConfig }> | Readonly<{ kind: "failure"; reason: string }> => {
  const loadedConfig = loadCurrentPoolConfig(dependencies);
  if (!loadedConfig.config) return { kind: "failure", reason: loadedConfig.reason };
  const config = loadedConfig.config;
  if (config.mode === "live" && config.maxGlobalPerDay !== 1 && anyPoolProviderTreatsRedeemOutcomeAsFinal(ordered)) {
    return { kind: "failure", reason: "terminal_outcome_global_limit_must_be_one" };
  }
  return { kind: "config", config };
};

/** Every candidate must produce a valid reset context, or the pool is unusable. */
const resolvePoolCandidates = async (
  ordered: readonly CodexBankedResetPoolCandidate[],
  hash: (value: string) => Promise<string>
): Promise<ResolvedPoolCandidate[] | null> => {
  const resolved = await Promise.all(
    ordered.map(async (pool) => {
      const context = await makeResetContext(pool.candidate, hash);
      return context ? ({ pool, context } satisfies ResolvedPoolCandidate) : null;
    })
  );
  if (resolved.some((candidate) => candidate === null)) return null;
  return resolved as ResolvedPoolCandidate[];
};

/** The redacted fence evidence that identifies one episode. */
const poolFences = (complete: readonly ResolvedPoolCandidate[]): CodexResetShadowDecisionRecord["fences"] =>
  complete.map(({ pool, context }) => ({
    slot: pool.slot,
    account_id_hash: context.account.accountIdHash,
    quota_generation: context.account.quotaGeneration,
    routing_generation: pool.candidate.routingGeneration,
    quota_reset_at_ms: pool.candidate.quotaResetAtMs,
  }));

/** Episode hash, or null when the hash cannot be used as an audit key. */
const poolEpisodeHash = async (
  settingsVersion: string,
  fences: CodexResetShadowDecisionRecord["fences"],
  hash: (value: string) => Promise<string>
): Promise<string | null> => {
  const episodeHash = await hash(
    `uos_ai\u0000codex_reset_shadow_episode\u0000${settingsVersion}\u0000${fences
      .map(
        (fence) => `${fence.slot}\u0000${fence.account_id_hash}\u0000${fence.quota_generation}\u0000${fence.routing_generation}\u0000${fence.quota_reset_at_ms}`
      )
      .join("\u0001")}`
  );
  return isNonEmptyText(episodeHash) ? episodeHash : null;
};

/**
 * Re-check every fence. A selection based on any stale account state is not
 * audit evidence and can never become a live spend.
 */
const stalePoolFenceReason = async (kv: Deno.Kv, complete: readonly ResolvedPoolCandidate[]): Promise<string | null> => {
  for (const { pool } of complete) {
    const current = await readCurrentFences(kv, pool.candidate);
    if (current.kind !== "valid") return current.kind === "stale" ? "routing_fence_stale" : current.code;
  }
  return null;
};

/** A persisted audit decision only counts while it proves this exact episode. */
const auditedDecisionMatches = (audited: CodexResetShadowDecisionRecord, fences: CodexResetShadowDecisionRecord["fences"], nowMs: number): boolean =>
  audited.expires_at_ms > nowMs && audited.decision_reason === "selected" && sameShadowFences(audited.fences, fences);

/** Live mode reads its arm evidence; `audited === null` means arming is needed. */
const resolveLiveArmingState = async (
  kv: Deno.Kv,
  config: CodexBankedResetConfig,
  dependencies: CodexBankedResetDependencies,
  episodeHash: string,
  fences: CodexResetShadowDecisionRecord["fences"],
  nowMs: number
): Promise<
  | Readonly<{ kind: "state"; audited: CodexResetShadowDecisionRecord | null; liveNeedsArming: boolean }>
  | Readonly<{ kind: "outcome"; outcome: CodexBankedResetPoolOutcome }>
> => {
  if (config.mode !== "live" || dependencies.allowLiveWithoutShadowForTest) return { kind: "state", audited: null, liveNeedsArming: false };
  const audited = await readShadowDecision(kv, episodeHash);
  if (audited && !auditedDecisionMatches(audited, fences, nowMs)) {
    return { kind: "outcome", outcome: poolOutcome("skipped", "shadow_decision_missing_or_expired") };
  }
  return { kind: "state", audited, liveNeedsArming: audited === null };
};

/** The episode evidence: proven contexts, fences, hash, and live arm state. */
const preparePoolEpisode = async (
  kv: Deno.Kv,
  ordered: readonly CodexBankedResetPoolCandidate[],
  config: CodexBankedResetConfig,
  dependencies: CodexBankedResetDependencies,
  hash: (value: string) => Promise<string>,
  nowMs: number
): Promise<
  | Readonly<{
      kind: "episode";
      complete: ResolvedPoolCandidate[];
      eligible: ResolvedPoolCandidate[];
      settingsEntries: Deno.KvEntryMaybe<unknown>[];
      fences: CodexResetShadowDecisionRecord["fences"];
      episodeHash: string;
      audited: CodexResetShadowDecisionRecord | null;
      liveNeedsArming: boolean;
    }>
  | Readonly<{ kind: "outcome"; outcome: CodexBankedResetPoolOutcome }>
> => {
  const complete = await resolvePoolCandidates(ordered, hash);
  if (!complete) return { kind: "outcome", outcome: poolOutcome("skipped", "invalid_quota_generation") };
  if (complete.some(({ pool }) => !quotaWindowIsOpen(pool.candidate, nowMs))) {
    return { kind: "outcome", outcome: poolOutcome("skipped", "quota_window_expired") };
  }
  // Reset settings are owned by the subscription: a disabled account is removed
  // from this episode's candidate list, while an unreadable record fails the
  // whole episode closed instead of silently spending.
  let settings: { entry: ResolvedPoolCandidate; usage: Awaited<ReturnType<typeof readBankedResetUsage>> }[];
  try {
    settings = await Promise.all(complete.map(async (entry) => ({ entry, usage: await readBankedResetUsage(kv, entry.context.account.accountIdHash) })));
  } catch {
    return { kind: "outcome", outcome: poolOutcome("skipped", "configuration_unavailable") };
  }
  const eligible = settings.filter(({ usage }) => usage.allowed).map(({ entry }) => entry);
  if (!eligible.length) return { kind: "outcome", outcome: poolOutcome("skipped", "usage_disabled") };
  const settingsEntries = settings.flatMap(({ usage }) => usage.entries);
  const settingsVersion = settingsEntries.map((entry) => entry.versionstamp ?? "unset").join(",");
  const fences = poolFences(complete);
  const episodeHash = await poolEpisodeHash(settingsVersion, fences, hash);
  if (!episodeHash) return { kind: "outcome", outcome: poolOutcome("skipped", "episode_hash_unavailable") };
  const arming = await resolveLiveArmingState(kv, config, dependencies, episodeHash, fences, nowMs);
  if (arming.kind === "outcome") return { kind: "outcome", outcome: arming.outcome };
  return {
    kind: "episode",
    complete,
    eligible,
    settingsEntries,
    fences,
    episodeHash,
    audited: arming.audited,
    liveNeedsArming: arming.liveNeedsArming,
  };
};

/** The pool was already rejected as `full_pool_missing`, so it is never empty. */
const firstResolvedPoolCandidate = (complete: readonly ResolvedPoolCandidate[]): ResolvedPoolCandidate => {
  const first = complete.at(0);
  if (!first) throw new Error("codex banked reset pool is empty");
  return first;
};

/** The already-persisted shadow decision for this episode, when it still holds. */
const existingShadowDecisionOutcome = async (
  kv: Deno.Kv,
  config: CodexBankedResetConfig,
  dependencies: CodexBankedResetDependencies,
  complete: readonly ResolvedPoolCandidate[],
  episodeHash: string,
  fences: CodexResetShadowDecisionRecord["fences"],
  nowMs: number
): Promise<CodexBankedResetPoolOutcome | null> => {
  if (config.mode !== "shadow") return null;
  const existing = await readShadowDecision(kv, episodeHash);
  if (existing === null) return null;
  if (existing.expires_at_ms <= nowMs) return null;
  if (!sameShadowFences(existing.fences, fences)) return null;
  const selected = complete.find(({ context }) => context.account.accountIdHash === existing.selected_account_id_hash) ?? null;
  // A subscription disabled after its decision was persisted must not report a
  // duplicate would-spend, let alone arm a live spend.
  if (selected && !(await readBankedResetUsage(kv, selected.context.account.accountIdHash)).allowed) {
    return poolOutcome("skipped", "usage_disabled");
  }
  const telemetry = dependencies.telemetry ?? defaultTelemetry;
  const telemetryCandidate = selected ?? firstResolvedPoolCandidate(complete);
  const fields = telemetryFields(telemetryCandidate.context, telemetryCandidate.pool.candidate, {
    episode_hash: episodeHash,
    credit_id_hash: existing.selected_credit_id_hash,
    selected: existing.selected_account_id_hash !== null,
    reason: existing.decision_reason,
  });
  emit(telemetry, "codex_reset_duplicate_prevented", { ...fields, reason: "shadow_decision_exists" });
  metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
  const wouldSpend =
    existing.decision_reason === "selected" && existing.selected_account_id_hash !== null && existing.selected_credit_id_hash !== null && selected !== null;
  return poolOutcome("shadow", wouldSpend ? "already_would_spend_once" : existing.decision_reason, wouldSpend ? selected.pool : null);
};

/**
 * Evaluate one complete currently fenced blocked cohort. Shadow reads each
 * blocked account's inventory and persists exactly one redacted decision.
 * Live without a decision first performs that same read-only arm phase. A
 * later live evaluation repeats the fence and inventory proof; it reaches the
 * durable ledger only when the exact account and exact opaque credit still
 * match.
 */
export const evaluateCodexBankedResetPool = async (
  candidates: readonly CodexBankedResetPoolCandidate[],
  dependencies: CodexBankedResetDependencies
): Promise<CodexBankedResetPoolOutcome> => {
  const clock = dependencies.now ?? Date.now;
  const nowMs = readClock(clock);
  if (nowMs === null) return poolOutcome("skipped", "invalid_clock");
  const orderedResult = orderPoolCandidates(candidates);
  if (orderedResult.kind === "failure") return poolOutcome("skipped", orderedResult.reason);
  const ordered = orderedResult.ordered;
  const configResult = loadPoolSubmissionConfig(ordered, dependencies);
  if (configResult.kind === "failure") return poolOutcome("skipped", configResult.reason);
  const config = configResult.config;
  const kv = await openResetKv(dependencies);
  if (!kv) return poolOutcome("skipped", "kv_unavailable");
  const hash = dependencies.hash ?? sha256Hex;
  const episode = await preparePoolEpisode(kv, ordered, config, dependencies, hash, nowMs);
  if (episode.kind === "outcome") return episode.outcome;
  const staleFence = await stalePoolFenceReason(kv, episode.complete);
  if (staleFence) return poolOutcome("skipped", staleFence);
  const duplicateShadow = await existingShadowDecisionOutcome(kv, config, dependencies, episode.complete, episode.episodeHash, episode.fences, nowMs);
  if (duplicateShadow) return duplicateShadow;

  const observed = await observePoolInventories(kv, episode.eligible, clock);
  if (observed.kind === "outcome") return observed.outcome;
  const selection = await selectPoolCredits(observed.inventoryResults, observed.nowMs, hash);
  if (selection.kind === "outcome") return selection.outcome;
  const decisionResult = buildShadowDecision(episode.episodeHash, observed.nowMs, selection.decisionReason, selection.selected, episode.fences);
  if (decisionResult.kind === "outcome") return decisionResult.outcome;
  const decision = decisionResult.decision;
  if (config.mode === "shadow") {
    return await persistShadowDecisionOutcome(
      kv,
      decision,
      episode.episodeHash,
      dependencies,
      episode.complete,
      episode.settingsEntries,
      selection,
      observed.nowMs
    );
  }
  return await runLivePoolSubmission(kv, dependencies, episode, decision, selection, observed.nowMs);
};

type PoolInventoryObservation = Readonly<{ resolvedCandidate: ResolvedPoolCandidate; inventory: ResetInventory | null }>;

/** Read every candidate's inventory concurrently; a failed read is recorded as null. */
const readPoolInventories = async (complete: readonly ResolvedPoolCandidate[]): Promise<readonly PoolInventoryObservation[]> =>
  await Promise.all(
    complete.map(async (resolvedCandidate) => {
      try {
        const inventory = await resolvedCandidate.pool.provider.readInventory(
          resolvedCandidate.context.account,
          boundedInventorySignal(resolvedCandidate.pool.candidate.signal)
        );
        return { resolvedCandidate, inventory } as const;
      } catch {
        return { resolvedCandidate, inventory: null } as const;
      }
    })
  );

/** Narrows an observation to one that carries a real inventory. */
const hasPoolInventory = (
  observation: PoolInventoryObservation
): observation is Readonly<{ resolvedCandidate: ResolvedPoolCandidate; inventory: ResetInventory }> => observation.inventory !== null;

/**
 * Read every inventory, then re-prove the fences and the observed windows.
 * A selection based on any stale account state is not audit evidence and can
 * never become a live spend.
 */
const observePoolInventories = async (
  kv: Deno.Kv,
  complete: readonly ResolvedPoolCandidate[],
  clock: () => number
): Promise<
  | Readonly<{ kind: "observed"; inventoryResults: readonly PoolInventoryObservation[]; nowMs: number }>
  | Readonly<{ kind: "outcome"; outcome: CodexBankedResetPoolOutcome }>
> => {
  const inventoryResults = await readPoolInventories(complete);
  const nowMs = readClock(clock);
  if (nowMs === null) return { kind: "outcome", outcome: poolOutcome("skipped", "invalid_clock") };
  if (inventoryResults.some(({ inventory }) => inventory === null)) {
    return { kind: "outcome", outcome: poolOutcome("skipped", "inventory_unavailable") };
  }
  const staleFence = await stalePoolFenceReason(kv, complete);
  if (staleFence) return { kind: "outcome", outcome: poolOutcome("skipped", staleFence) };
  if (complete.some(({ pool }) => !quotaWindowIsOpen(pool.candidate, nowMs))) {
    return { kind: "outcome", outcome: poolOutcome("skipped", "quota_window_expired") };
  }
  return { kind: "observed", inventoryResults, nowMs };
};

/**
 * A pool inventory is only selectable while it validates and no credit that is
 * still marked available has already expired.
 */
const poolInventoryIsUsable = (inventory: ResetInventory, nowMs: number): boolean =>
  validInventory(inventory, nowMs) &&
  !inventory.credits.some((credit) => credit.status === "available" && credit.expiresAtMs !== null && credit.expiresAtMs <= nowMs);

/** Pick one usable credit per candidate and rank them deterministically. */
const selectPoolCredits = async (
  inventoryResults: readonly PoolInventoryObservation[],
  nowMs: number,
  hash: (value: string) => Promise<string>
): Promise<
  | Readonly<{ kind: "selection"; selected: SelectedPoolCredit | null; decisionReason: string }>
  | Readonly<{ kind: "outcome"; outcome: CodexBankedResetPoolOutcome }>
> => {
  const selectedCredits: SelectedPoolCredit[] = [];
  let decisionReason = "inventory_empty";
  for (const observation of inventoryResults) {
    if (!hasPoolInventory(observation)) continue;
    const inventory = observation.inventory;
    if (!poolInventoryIsUsable(inventory, nowMs)) {
      decisionReason = "inventory_response_invalid_or_expired";
      selectedCredits.length = 0;
      break;
    }
    const selection = selectInventoryCredit(inventory, observation.resolvedCandidate.pool.provider, nowMs);
    if (selection.kind !== "selected") {
      if (selection.kind === "no_eligible_credit") decisionReason = "inventory_no_eligible_codex_credit";
      continue;
    }
    const creditIdHash = await hash(`uos_ai\u0000codex_reset_credit\u0000${selection.credit.id}`);
    if (!isNonEmptyText(creditIdHash)) return { kind: "outcome", outcome: poolOutcome("skipped", "credit_hash_unavailable") };
    selectedCredits.push({ resolved: observation.resolvedCandidate, credit: selection.credit, creditIdHash });
  }
  selectedCredits.sort((left, right) => {
    const leftExpiry = left.credit.expiresAtMs ?? Number.POSITIVE_INFINITY;
    const rightExpiry = right.credit.expiresAtMs ?? Number.POSITIVE_INFINITY;
    return leftExpiry - rightExpiry || left.resolved.pool.slot - right.resolved.pool.slot || left.credit.id.localeCompare(right.credit.id);
  });
  const selected = selectedCredits.at(0) ?? null;
  if (selected) return { kind: "selection", selected, decisionReason: "selected" };
  return { kind: "selection", selected: null, decisionReason: decisionReason === "inventory_empty" ? "no_eligible_credit" : decisionReason };
};

/** The redacted decision record, or the reason it must not be written. */
const buildShadowDecision = (
  episodeHash: string,
  nowMs: number,
  decisionReason: string,
  selected: SelectedPoolCredit | null,
  fences: CodexResetShadowDecisionRecord["fences"]
): Readonly<{ kind: "decision"; decision: CodexResetShadowDecisionRecord }> | Readonly<{ kind: "outcome"; outcome: CodexBankedResetPoolOutcome }> => {
  const episodeExpiresAtMs = Math.min(...fences.map((fence) => fence.quota_reset_at_ms), selected?.credit.expiresAtMs ?? Number.POSITIVE_INFINITY);
  if (!isSafeMs(episodeExpiresAtMs) || episodeExpiresAtMs <= nowMs) {
    return { kind: "outcome", outcome: poolOutcome("skipped", "shadow_decision_expired") };
  }
  return {
    kind: "decision",
    decision: {
      v: 1,
      episode_hash: episodeHash,
      created_at_ms: nowMs,
      expires_at_ms: episodeExpiresAtMs,
      decision_reason: decisionReason,
      selected_account_id_hash: selected?.resolved.context.account.accountIdHash ?? null,
      selected_credit_id_hash: selected?.creditIdHash ?? null,
      selected_credit_expires_at_ms: selected?.credit.expiresAtMs ?? null,
      fences,
    },
  };
};

/** Persist the shadow decision and emit the telemetry that describes it. */
const persistShadowDecisionOutcome = async (
  kv: Deno.Kv,
  decision: CodexResetShadowDecisionRecord,
  episodeHash: string,
  dependencies: CodexBankedResetDependencies,
  complete: readonly ResolvedPoolCandidate[],
  settingsEntries: Deno.KvEntryMaybe<unknown>[],
  selection: Readonly<{ selected: SelectedPoolCredit | null; decisionReason: string }>,
  nowMs: number
): Promise<CodexBankedResetPoolOutcome> => {
  const selected = selection.selected;
  const persisted = await shadowDecisionRecord(kv, decision, nowMs, settingsEntries);
  if (!persisted) return poolOutcome("skipped", "shadow_decision_unavailable");
  const telemetry = dependencies.telemetry ?? defaultTelemetry;
  const telemetryCandidate = selected?.resolved ?? firstResolvedPoolCandidate(complete);
  const fields = telemetryFields(telemetryCandidate.context, telemetryCandidate.pool.candidate, {
    episode_hash: episodeHash,
    credit_id_hash: selected?.creditIdHash ?? null,
    selected: selected !== null,
    reason: persisted.record.decision_reason,
  });
  if (persisted.kind === "duplicate") {
    emit(telemetry, "codex_reset_duplicate_prevented", { ...fields, reason: "shadow_decision_exists" });
    metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
    return poolOutcome("shadow", "already_would_spend_once", selected?.resolved.pool ?? null);
  }
  emit(telemetry, "codex_reset_eligible", fields);
  emit(telemetry, "codex_reset_shadow_candidate", fields);
  metric(telemetry, "codex_reset_eligible_total", 1, fields);
  metric(telemetry, "codex_reset_shadow_candidates_total", 1, fields);
  return poolOutcome("shadow", selected ? "shadow_selected" : selection.decisionReason, selected?.resolved.pool ?? null);
};

/** Arm a live episode with the persisted decision for its exact evidence. */
const armLiveWithShadowDecision = async (
  kv: Deno.Kv,
  decision: CodexResetShadowDecisionRecord,
  dependencies: CodexBankedResetDependencies,
  settingsEntries: Deno.KvEntryMaybe<unknown>[],
  selected: SelectedPoolCredit,
  nowMs: number
): Promise<CodexBankedResetPoolOutcome> => {
  const persisted = await shadowDecisionRecord(kv, decision, nowMs, settingsEntries);
  if (!persisted) return poolOutcome("skipped", "shadow_decision_unavailable");
  if (
    persisted.record.decision_reason !== "selected" ||
    persisted.record.selected_account_id_hash !== selected.resolved.context.account.accountIdHash ||
    persisted.record.selected_credit_id_hash !== selected.creditIdHash ||
    persisted.record.selected_credit_expires_at_ms !== selected.credit.expiresAtMs ||
    !sameShadowFences(persisted.record.fences, decision.fences)
  )
    return poolOutcome("skipped", "shadow_decision_drift");

  const telemetry = dependencies.telemetry ?? defaultTelemetry;
  const fields = telemetryFields(selected.resolved.context, selected.resolved.pool.candidate, {
    episode_hash: decision.episode_hash,
    credit_id_hash: selected.creditIdHash,
    selected: true,
    reason: persisted.record.decision_reason,
  });
  if (persisted.kind === "duplicate") {
    emit(telemetry, "codex_reset_duplicate_prevented", { ...fields, reason: "shadow_decision_exists" });
    metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
  } else {
    emit(telemetry, "codex_reset_eligible", fields);
    emit(telemetry, "codex_reset_shadow_candidate", fields);
    metric(telemetry, "codex_reset_eligible_total", 1, fields);
    metric(telemetry, "codex_reset_shadow_candidates_total", 1, fields);
  }
  return poolOutcome("shadow", "live_armed", selected.resolved.pool);
};

/** The audit evidence must still name this exact account and exact credit. */
const liveAuditMatches = (audited: CodexResetShadowDecisionRecord | null, selected: SelectedPoolCredit, nowMs: number): boolean => {
  if (audited === null) return false;
  if (audited.expires_at_ms <= nowMs) return false;
  return (
    audited.selected_account_id_hash === selected.resolved.context.account.accountIdHash &&
    audited.selected_credit_id_hash === selected.creditIdHash &&
    audited.selected_credit_expires_at_ms === selected.credit.expiresAtMs
  );
};

/** The live tail: arming or audit proof, then one durable submission attempt. */
const runLivePoolSubmission = async (
  kv: Deno.Kv,
  dependencies: CodexBankedResetDependencies,
  episode: Readonly<{
    complete: ResolvedPoolCandidate[];
    settingsEntries: Deno.KvEntryMaybe<unknown>[];
    episodeHash: string;
    audited: CodexResetShadowDecisionRecord | null;
    liveNeedsArming: boolean;
  }>,
  decision: CodexResetShadowDecisionRecord,
  selection: Readonly<{ selected: SelectedPoolCredit | null; decisionReason: string }>,
  nowMs: number
): Promise<CodexBankedResetPoolOutcome> => {
  // Preserve the precise inventory failure in live mode. This is especially
  // useful for an expired credit after a valid shadow decision: it proves
  // that no external redemption was attempted because the fresh inventory
  // itself was no longer eligible.
  const selected = selection.selected;
  if (!selected) return poolOutcome("skipped", selection.decisionReason);
  if (episode.liveNeedsArming) return await armLiveWithShadowDecision(kv, decision, dependencies, episode.settingsEntries, selected, nowMs);
  if (!dependencies.allowLiveWithoutShadowForTest && !liveAuditMatches(episode.audited, selected, nowMs)) {
    return poolOutcome("skipped", "shadow_decision_drift");
  }
  // The durable claim/submission path must continue to fence every blocked
  // account that established the audited episode, not just the selected
  // owner. Recovery or rotation after the shadow read makes the episode stale
  // and must block the external consume.
  const fullPoolFences = episode.complete.flatMap(({ pool }) => pool.candidate.fences);
  const reset = await attemptCodexBankedReset(
    { ...selected.resolved.pool.candidate, fences: fullPoolFences, selectedCredit: selected.credit },
    { ...dependencies, provider: selected.resolved.pool.provider }
  );
  return poolOutcome(reset.kind, reset.reason, selected.resolved.pool, reset);
};
