import { getKv } from "./kv.ts";
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
import type {
  CodexResetGlobalDailyRecord,
  CodexResetRedemptionRecord,
  CodexResetRedemptionState,
  CodexResetShadowDecisionRecord,
} from "./types.ts";
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

export const codexResetGlobalDailyKey = (day: string): Deno.KvKey => [
  ...CODEX_RESET_GLOBAL_DAILY_KV_PREFIX,
  day,
];

export const codexResetShadowDecisionKey = (episodeHash: string): Deno.KvKey => [
  ...CODEX_RESET_SHADOW_DECISION_KV_PREFIX,
  episodeHash,
];

export type CodexBankedResetMode = "disabled" | "shadow" | "live";

export type CodexBankedResetConfig = Readonly<{
  enabled: boolean;
  mode: CodexBankedResetMode;
  accountAllowlist: ReadonlySet<string>;
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
  const normalized = value?.trim() ?? "";
  if (!/^\d+$/.test(normalized)) return 0;
  const parsed = Number(normalized);
  return Number.isSafeInteger(parsed) && parsed >= 0 ? parsed : 0;
};

const parseMode = (value: string | undefined): CodexBankedResetMode => {
  if (value === undefined) return "shadow";
  switch (value?.trim().toLowerCase()) {
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

const parseAllowlist = (value: string | undefined): ReadonlySet<string> =>
  new Set(
    (value ?? "")
      .split(/[\n,]/g)
      .map((part) => part.trim())
      .filter(Boolean),
  );

/**
 * This is intentionally read at use time rather than module load time. A
 * configuration update can kill new claims immediately without deleting an
 * existing submitted/unknown record that still requires reconciliation.
 */
export const parseCodexBankedResetConfig = (
  readEnv: (key: string) => string | undefined = getEnv,
): CodexBankedResetConfig => ({
  // Shadow telemetry may read an allowlisted blocked account's inventory, but
  // it never consumes a credit. A spend still requires explicit live mode,
  // caps, an allowlist, and an approved provider contract.
  enabled: parseStrictBoolean(readEnv("CODEX_BANKED_RESET_ENABLED"), true),
  mode: parseMode(readEnv("CODEX_BANKED_RESET_MODE")),
  accountAllowlist: parseAllowlist(readEnv("CODEX_BANKED_RESET_ACCOUNT_ALLOWLIST")),
  maxGlobalPerDay: parseNonNegativeInteger(readEnv("CODEX_BANKED_RESET_MAX_GLOBAL_PER_DAY"), 0),
  maxPerAccountPerWindow: parseNonNegativeInteger(readEnv("CODEX_BANKED_RESET_MAX_PER_ACCOUNT_PER_WINDOW"), 1),
});

export const loadCodexBankedResetConfig = (): CodexBankedResetConfig => parseCodexBankedResetConfig();

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

const isSafeMs = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const isSafeNonnegativeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const isNonEmptyText = (value: unknown, max = 512): value is string =>
  typeof value === "string" && value.length > 0 && value.length <= max;

const isState = (value: unknown): value is CodexResetRedemptionState =>
  value === "claimed" || value === "submitted" || value === "unknown" || value === "verified" || value === "rejected";

export const parseCodexResetRedemptionRecord = (value: unknown): CodexResetRedemptionRecord | null => {
  if (!isRecord(value) || value.v !== 1) return null;
  if (
    !isNonEmptyText(value.account_id_hash) || !isNonEmptyText(value.credential_version) ||
    !isNonEmptyText(value.quota_generation) || !isNonEmptyText(value.idempotency_key_hash) ||
    !isState(value.state) || !isNonEmptyText(value.owner_token) ||
    !isSafeNonnegativeInteger(value.routing_generation) || !isSafeNonnegativeInteger(value.fence) ||
    !isSafeMs(value.lease_expires_at_ms) ||
    !isSafeMs(value.created_at_ms) || !isSafeMs(value.updated_at_ms)
  ) return null;
  if (value.provider_receipt_id !== null && !isNonEmptyText(value.provider_receipt_id)) return null;
  if (value.submitted_at_ms !== null && !isSafeMs(value.submitted_at_ms)) return null;
  if (value.verified_at_ms !== null && !isSafeMs(value.verified_at_ms)) return null;
  if (value.last_error_code !== null && !isNonEmptyText(value.last_error_code, 128)) return null;
  if (value.updated_at_ms < value.created_at_ms) return null;
  if (value.submitted_at_ms !== null && value.submitted_at_ms < value.created_at_ms) return null;
  if (value.submitted_at_ms !== null && value.updated_at_ms < value.submitted_at_ms) return null;
  if (
    value.verified_at_ms !== null &&
    (value.submitted_at_ms === null || value.verified_at_ms < value.submitted_at_ms)
  ) return null;
  if (value.verified_at_ms !== null && value.updated_at_ms < value.verified_at_ms) return null;
  switch (value.state) {
    case "claimed":
      if (
        value.submitted_at_ms !== null || value.verified_at_ms !== null || value.provider_receipt_id !== null ||
        value.last_error_code !== null
      ) return null;
      break;
    case "submitted":
      if (value.submitted_at_ms === null || value.verified_at_ms !== null || value.last_error_code !== null) {
        return null;
      }
      break;
    case "unknown":
      if (value.submitted_at_ms === null || value.verified_at_ms !== null || value.last_error_code === null) {
        return null;
      }
      break;
    case "verified":
      if (
        value.submitted_at_ms === null || value.verified_at_ms === null || value.last_error_code !== null
      ) return null;
      break;
    case "rejected":
      if (value.verified_at_ms !== null || value.last_error_code === null) return null;
      break;
  }
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

const parseShadowDecisionFence = (
  value: unknown,
): CodexResetShadowDecisionRecord["fences"][number] | null => {
  if (
    !isRecord(value) || !isSafeNonnegativeInteger(value.slot) || !isNonEmptyText(value.account_id_hash) ||
    !isNonEmptyText(value.quota_generation) || !isSafeNonnegativeInteger(value.routing_generation) ||
    !isSafeMs(value.quota_reset_at_ms)
  ) return null;
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
    !isRecord(value) || value.v !== 1 || !isNonEmptyText(value.episode_hash) || !isSafeMs(value.created_at_ms) ||
    !isSafeMs(value.expires_at_ms) || value.expires_at_ms < value.created_at_ms ||
    !isNonEmptyText(value.decision_reason, 128) || !Array.isArray(value.fences)
  ) return null;
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
  record: CodexResetRedemptionRecord | null = null,
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

const policyReason = (config: CodexBankedResetConfig, context: ResetContext): string | null => {
  try {
    if (!config.enabled) return "feature_disabled";
    if (config.mode === "disabled") return "mode_disabled";
    // This single-candidate state-machine seam makes no provider call in
    // shadow. The production cohort evaluator separately requires an
    // allowlist and positive cap before its bounded inventory reads.
    if (config.mode === "shadow") return null;
    if (config.accountAllowlist.size === 0) return "account_allowlist_required";
    if (
      !config.accountAllowlist.has(context.account.accountId) &&
      !config.accountAllowlist.has(context.account.accountIdHash)
    ) {
      return "account_not_allowlisted";
    }
    if (config.maxGlobalPerDay <= 0) return "global_limit_disabled";
    // The non-negotiable at-most-once rule is stronger than a mutable setting.
    if (config.maxPerAccountPerWindow !== 1) return "per_account_window_limit_invalid";
    return null;
  } catch {
    return "configuration_invalid";
  }
};

const providerPolicyReason = (
  config: CodexBankedResetConfig,
  provider: CodexUsageResetProvider,
): string | null =>
  config.mode === "live" && providerTreatsRedeemOutcomeAsFinal(provider) && config.maxGlobalPerDay !== 1
    ? "terminal_outcome_global_limit_must_be_one"
    : null;

const boundedInventorySignal = (signal?: AbortSignal): AbortSignal => {
  const timeout = AbortSignal.timeout(CODEX_BANKED_RESET_INVENTORY_TIMEOUT_MS);
  return signal ? AbortSignal.any([signal, timeout]) : timeout;
};

const telemetryFields = (
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  extras: CodexBankedResetTelemetryFields = {},
): CodexBankedResetTelemetryFields => ({
  request_id: candidate.requestId,
  account_id_hash: context.account.accountIdHash,
  credential_version: context.account.credentialVersion,
  quota_generation: context.account.quotaGeneration,
  idempotency_key_hash: context.idempotencyKeyHash,
  routing_generation: candidate.routingGeneration,
  ...extras,
});

const emit = (
  telemetry: CodexBankedResetTelemetry,
  event: CodexBankedResetEvent,
  fields: CodexBankedResetTelemetryFields,
): void => {
  try {
    telemetry.event?.(event, fields);
  } catch {
    // An observer must not affect state transitions.
  }
};

const metric = (
  telemetry: CodexBankedResetTelemetry,
  name: CodexBankedResetMetric,
  value: number,
  fields: CodexBankedResetTelemetryFields,
): void => {
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
  fields: CodexBankedResetTelemetryFields,
): void => emit(telemetry ?? defaultTelemetry, event, fields);

/** Allows gateway-level post-retry metrics to share the default sink. */
export const reportCodexBankedResetMetric = (
  telemetry: CodexBankedResetTelemetry | undefined,
  name: CodexBankedResetMetric,
  value: number,
  fields: CodexBankedResetTelemetryFields,
): void => metric(telemetry ?? defaultTelemetry, name, value, fields);

const makeResetContext = async (
  candidate: CodexBankedResetCandidate,
  hash: (value: string) => Promise<string>,
): Promise<ResetContext | null> => {
  if (
    !isNonEmptyText(candidate.accountId, 1024) || !isNonEmptyText(candidate.credentialVersion, 512) ||
    !isSafeMs(candidate.quotaResetAtMs) || !Number.isSafeInteger(candidate.routingGeneration) ||
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
    const credentialVersion = `v1:${await hash(
      `uos_ai\u0000codex_reset_credential_version\u0000${candidate.credentialVersion}`,
    )}`;
    const quotaGeneration = `v1:${await hash(
      `uos_ai\u0000codex_reset_generation\u0000${accountIdHash}\u0000${candidate.quotaResetAtMs}`,
    )}`;
    const idempotencyKey = `uos_ai_codex_reset_v1_${await hash(
      `uos_ai\u0000codex_reset_idempotency\u0000${accountIdHash}\u0000${quotaGeneration}`,
    )}`;
    const idempotencyKeyHash = await hash(idempotencyKey);
    if (
      !isNonEmptyText(accountIdHash) || !isNonEmptyText(credentialVersion) || !isNonEmptyText(quotaGeneration) ||
      !isNonEmptyText(idempotencyKeyHash)
    ) {
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
  | Readonly<{ kind: "valid"; entries: readonly Deno.KvEntryMaybe<unknown>[] }>
  | Readonly<{ kind: "stale" }>
  | Readonly<{ kind: "failure"; code: string }>;

type SubmissionPreparation =
  | Readonly<{ kind: "submitted"; record: CodexResetRedemptionRecord }>
  | Readonly<{ kind: "failure"; code: string }>;

type SubmissionRenewal =
  | Readonly<{ kind: "renewed"; record: CodexResetRedemptionRecord }>
  | Readonly<{ kind: "failure"; code: string }>;

const matchesContext = (record: CodexResetRedemptionRecord, context: ResetContext): boolean =>
  record.account_id_hash === context.account.accountIdHash &&
  record.credential_version === context.account.credentialVersion &&
  record.quota_generation === context.account.quotaGeneration &&
  record.idempotency_key_hash === context.idempotencyKeyHash;

const leaseUntil = (nowMs: number): number | null => {
  const next = nowMs + CODEX_BANKED_RESET_LEASE_MS;
  return isSafeMs(next) ? next : null;
};

const nextFence = (fence: number): number | null => {
  const next = fence + 1;
  return Number.isSafeInteger(next) ? next : null;
};

const quotaWindowIsOpen = (candidate: CodexBankedResetCandidate, nowMs: number): boolean =>
  nowMs < candidate.quotaResetAtMs;

const readClock = (clock: () => number): number | null => {
  try {
    const value = clock();
    return isSafeMs(value) ? value : null;
  } catch {
    return null;
  }
};

const hasUsableFences = (candidate: CodexBankedResetCandidate): boolean =>
  Array.isArray(candidate.fences) && candidate.fences.length > 0 &&
  candidate.fences.every((fence) =>
    isRecord(fence) && Array.isArray(fence.key) && typeof fence.isCurrent === "function"
  );

const readCurrentFences = async (kv: Deno.Kv, candidate: CodexBankedResetCandidate): Promise<FenceRead> => {
  if (!hasUsableFences(candidate)) return { kind: "failure", code: "routing_fence_missing" };
  const entries: Deno.KvEntryMaybe<unknown>[] = [];
  for (const fence of candidate.fences) {
    let entry: Deno.KvEntryMaybe<unknown>;
    try {
      entry = await kv.get<unknown>(fence.key, { consistency: "strong" });
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

const withFenceChecks = (
  operation: Deno.AtomicOperation,
  entries: readonly Deno.KvEntryMaybe<unknown>[],
): Deno.AtomicOperation => {
  let next = operation;
  for (const entry of entries) next = next.check(entry);
  return next;
};

const readExistingRecord = async (
  kv: Deno.Kv,
  context: ResetContext,
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

const claimTransaction = async (
  kv: Deno.Kv,
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  nowMs: number,
  clock: () => number,
  ownerToken: string,
  allowNewSubmission: boolean,
): Promise<ClaimResult> => {
  const expiresAtMs = leaseUntil(nowMs);
  if (expiresAtMs === null) return { kind: "failure", code: "invalid_clock" };
  const key = codexResetRedemptionKey(context.account.accountIdHash, context.account.quotaGeneration);

  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    let entry: Deno.KvEntryMaybe<CodexResetRedemptionRecord>;
    try {
      entry = await kv.get<CodexResetRedemptionRecord>(key, { consistency: "strong" });
    } catch {
      return { kind: "failure", code: "kv_unavailable" };
    }
    const record = entry.value === null ? null : parseCodexResetRedemptionRecord(entry.value);
    if (entry.value !== null && !record) return { kind: "failure", code: "redemption_record_invalid" };

    if (!record) {
      if (!allowNewSubmission) return { kind: "no_transaction" };
      // Do not claim a quota window that has already recovered. In
      // particular, this must precede the daily-cap write so an expired
      // candidate cannot consume capacity without reaching the provider.
      if (!quotaWindowIsOpen(candidate, nowMs)) return { kind: "failure", code: "quota_window_expired" };
      const fences = await readCurrentFences(kv, candidate);
      if (fences.kind === "failure") return { kind: "failure", code: fences.code };
      if (fences.kind === "stale") return { kind: "failure", code: "routing_fence_stale" };
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
      // Fence reads are asynchronous. Re-check immediately before the atomic
      // write so a window that expired during those reads cannot create a
      // fresh submission path.
      const nowBeforeClaim = readClock(clock);
      if (nowBeforeClaim === null) return { kind: "failure", code: "invalid_clock" };
      if (!quotaWindowIsOpen(candidate, nowBeforeClaim)) {
        return { kind: "failure", code: "quota_window_expired" };
      }
      try {
        const committed = await withFenceChecks(kv.atomic().check(entry), fences.entries)
          .set(key, created)
          .commit();
        if (committed.ok) return { kind: "submit", record: created, tookOver: false };
      } catch {
        return { kind: "failure", code: "kv_unavailable" };
      }
      continue;
    }

    if (!matchesContext(record, context)) return { kind: "failure", code: "redemption_record_context_mismatch" };
    if (record.state === "verified") return { kind: "verified", record };
    if (record.state === "rejected") return { kind: "rejected", record };
    if (record.lease_expires_at_ms > nowMs) return { kind: "in_progress", record };
    if (record.state === "claimed" && !allowNewSubmission) return { kind: "in_progress", record };
    // A takeover of an expired `claimed` record would otherwise create a
    // fresh submission path after the observed quota window has reopened.
    // Submitted/unknown records deliberately bypass this guard and reconcile
    // lookup-only; they are never re-redeemed here.
    if (record.state === "claimed" && !quotaWindowIsOpen(candidate, nowMs)) {
      return { kind: "failure", code: "quota_window_expired" };
    }

    let fenceEntries: readonly Deno.KvEntryMaybe<unknown>[] = [];
    if (record.state === "claimed") {
      if (record.routing_generation !== candidate.routingGeneration) {
        return { kind: "failure", code: "routing_fence_stale" };
      }
      const fences = await readCurrentFences(kv, candidate);
      if (fences.kind === "failure") return { kind: "failure", code: fences.code };
      if (fences.kind === "stale") return { kind: "failure", code: "routing_fence_stale" };
      fenceEntries = fences.entries;
    }
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
      if (!committed.ok) continue;
    } catch {
      return { kind: "failure", code: "kv_unavailable" };
    }
    return record.state === "claimed"
      ? { kind: "submit", record: takenOver, tookOver: true }
      : { kind: "reconcile", record: takenOver, tookOver: true };
  }
  return { kind: "failure", code: "kv_cas_exhausted" };
};

const updateOwnedRecord = async (
  kv: Deno.Kv,
  context: ResetContext,
  expected: CodexResetRedemptionRecord,
  mutate: (record: CodexResetRedemptionRecord) => CodexResetRedemptionRecord,
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
    if (
      !current || !matchesContext(current, context) || current.owner_token !== expected.owner_token ||
      current.fence !== expected.fence
    ) {
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
const prepareSubmission = async (
  kv: Deno.Kv,
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  expected: CodexResetRedemptionRecord,
  nowMs: number,
  clock: () => number,
  maxGlobalPerDay: number,
): Promise<SubmissionPreparation> => {
  const expiresAtMs = leaseUntil(nowMs);
  const day = utcDay(nowMs);
  if (expiresAtMs === null || !day) return { kind: "failure", code: "invalid_clock" };
  const key = codexResetRedemptionKey(context.account.accountIdHash, context.account.quotaGeneration);
  const dailyKey = codexResetGlobalDailyKey(day);
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    let entry: Deno.KvEntryMaybe<CodexResetRedemptionRecord>;
    try {
      entry = await kv.get<CodexResetRedemptionRecord>(key, { consistency: "strong" });
    } catch {
      return { kind: "failure", code: "kv_unavailable" };
    }
    const current = parseCodexResetRedemptionRecord(entry.value);
    if (
      !current || !matchesContext(current, context) || current.owner_token !== expected.owner_token ||
      current.fence !== expected.fence || current.state !== "claimed"
    ) return { kind: "failure", code: "stale_owner" };
    if (!claimedDuringCurrentUtcDay(current, nowMs)) return { kind: "failure", code: "claim_day_elapsed" };
    if (current.routing_generation !== candidate.routingGeneration || current.lease_expires_at_ms <= nowMs) {
      return { kind: "failure", code: "stale_owner" };
    }
    const fences = await readCurrentFences(kv, candidate);
    if (fences.kind === "failure") return { kind: "failure", code: fences.code };
    if (fences.kind === "stale") return { kind: "failure", code: "routing_fence_stale" };
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
      const committed = await withFenceChecks(kv.atomic().check(entry).check(dailyEntry), fences.entries)
        .set(key, submitted)
        .set(dailyKey, nextDaily)
        .commit();
      if (committed.ok) return { kind: "submitted", record: submitted };
    } catch {
      return { kind: "failure", code: "kv_unavailable" };
    }
  }
  return { kind: "failure", code: "kv_cas_exhausted" };
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
  clock: () => number,
): Promise<SubmissionRenewal> => {
  const key = codexResetRedemptionKey(context.account.accountIdHash, context.account.quotaGeneration);
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
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
    if (
      !current || !matchesContext(current, context) || current.owner_token !== expected.owner_token ||
      current.fence !== expected.fence || current.state !== "submitted" ||
      current.routing_generation !== candidate.routingGeneration || current.lease_expires_at_ms <= nowBeforeRead
    ) {
      return { kind: "failure", code: "stale_owner" };
    }
    if (!claimedDuringCurrentUtcDay(current, nowBeforeRead)) return { kind: "failure", code: "claim_day_elapsed" };

    const fences = await readCurrentFences(kv, candidate);
    if (fences.kind === "failure") return { kind: "failure", code: fences.code };
    if (fences.kind === "stale") return { kind: "failure", code: "routing_fence_stale" };

    // A slow strong read must not commit a new lease after the observed quota
    // window has naturally reopened.
    const nowBeforeCommit = readClock(clock);
    if (nowBeforeCommit === null) return { kind: "failure", code: "invalid_clock" };
    if (!quotaWindowIsOpen(candidate, nowBeforeCommit)) return { kind: "failure", code: "quota_window_expired" };
    if (!claimedDuringCurrentUtcDay(current, nowBeforeCommit)) {
      return { kind: "failure", code: "claim_day_elapsed" };
    }
    if (current.lease_expires_at_ms <= nowBeforeCommit) return { kind: "failure", code: "stale_owner" };
    const expiresAtMs = leaseUntil(nowBeforeCommit);
    const renewedFence = nextFence(current.fence);
    if (expiresAtMs === null) return { kind: "failure", code: "invalid_clock" };
    if (renewedFence === null) return { kind: "failure", code: "owner_fence_exhausted" };
    const renewed: CodexResetRedemptionRecord = {
      ...current,
      fence: renewedFence,
      lease_expires_at_ms: expiresAtMs,
      updated_at_ms: nowBeforeCommit,
    };
    try {
      const committed = await withFenceChecks(kv.atomic().check(entry), fences.entries).set(key, renewed).commit();
      if (committed.ok) return { kind: "renewed", record: renewed };
    } catch {
      return { kind: "failure", code: "kv_unavailable" };
    }
  }
  return { kind: "failure", code: "kv_cas_exhausted" };
};

const receiptId = (value: unknown): string | null => isNonEmptyText(value, 512) ? value : null;

/**
 * Receipt identifiers are optional optimization hints: reconciliation is
 * required to work by deterministic idempotency key. Keep an unapproved
 * receipt in process memory only, never in the durable record or telemetry.
 */
const durableReceiptId = (
  provider: Pick<CodexUsageResetProvider, "contract">,
  value: unknown,
): string | null => providerReceiptIdsSafeToPersistAndLog(provider) ? receiptId(value) : null;

const validInventory = (
  inventory: unknown,
  nowMs: number,
): inventory is ResetInventory =>
  isRecord(inventory) && isSafeNonnegativeInteger(inventory.availableCount) &&
  isSafeMs(inventory.observedAtMs) && Array.isArray(inventory.credits) &&
  inventory.observedAtMs <= nowMs && nowMs - inventory.observedAtMs <= CODEX_BANKED_RESET_INVENTORY_MAX_AGE_MS &&
  inventory.credits.every((credit) =>
    isRecord(credit) && isNonEmptyText(credit.id, 512) && isNonEmptyText(credit.status, 128) &&
    isNonEmptyText(credit.resetType, 128) &&
    (credit.expiresAtMs === null || isSafeMs(credit.expiresAtMs))
  ) &&
  // The production adapter rejects duplicate opaque IDs. Retain that same
  // invariant at the evaluator boundary so an injected or future provider
  // cannot make an ambiguous inventory look selectable.
  new Set(inventory.credits.map((credit) => credit.id)).size === inventory.credits.length &&
  inventory.credits.filter((credit) => credit.status === "available").length === inventory.availableCount;

type InventoryCreditSelection =
  | Readonly<{ kind: "selected"; credit: ResetInventoryCredit }>
  | Readonly<{ kind: "empty" }>
  | Readonly<{ kind: "no_eligible_credit" }>;

/**
 * The only selectable credits are explicit, currently valid Codex
 * rate-limit credits. Finite expiry wins over non-expiring credits; callers
 * add the account slot as the next global tie-breaker.
 */
const selectInventoryCredit = (
  inventory: ResetInventory,
  provider: CodexUsageResetProvider,
  nowMs: number,
): InventoryCreditSelection => {
  if (inventory.availableCount === 0) return { kind: "empty" };
  const candidates = inventory.credits.filter((credit) =>
    credit.status === "available" && credit.resetType === "codex_rate_limits" &&
    providerSupportsResetType(provider, credit.resetType) &&
    (credit.expiresAtMs === null || credit.expiresAtMs > nowMs)
  );
  if (!candidates.length) return { kind: "no_eligible_credit" };
  candidates.sort((left, right) => {
    const leftExpiry = left.expiresAtMs ?? Number.POSITIVE_INFINITY;
    const rightExpiry = right.expiresAtMs ?? Number.POSITIVE_INFINITY;
    return leftExpiry - rightExpiry || left.id.localeCompare(right.id);
  });
  return { kind: "selected", credit: candidates[0]! };
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
  patch: Partial<
    Pick<CodexResetRedemptionRecord, "provider_receipt_id" | "submitted_at_ms" | "verified_at_ms" | "last_error_code">
  > = {},
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
  code: string,
): Promise<CodexResetRedemptionRecord | null> =>
  await updateOwnedRecord(
    kv,
    context,
    record,
    (current) => stateWith(current, "rejected", nowMs, { last_error_code: code }),
  );

const unknownOwned = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  nowMs: number,
  code: string,
  providerReceiptId: string | null,
): Promise<CodexResetRedemptionRecord | null> =>
  await updateOwnedRecord(kv, context, record, (current) =>
    stateWith(current, "unknown", nowMs, {
      provider_receipt_id: providerReceiptId ?? current.provider_receipt_id,
      last_error_code: code,
    }));

const preserveReceipt = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  nowMs: number,
  providerReceiptId: string | null,
): Promise<CodexResetRedemptionRecord | null> =>
  await updateOwnedRecord(kv, context, record, (current) =>
    stateWith(current, "submitted", nowMs, {
      provider_receipt_id: providerReceiptId,
      submitted_at_ms: current.submitted_at_ms ?? nowMs,
      last_error_code: null,
    }));

const unknownOutcome = (
  telemetry: CodexBankedResetTelemetry,
  context: ResetContext,
  candidate: CodexBankedResetCandidate,
  reason: string,
  record: CodexResetRedemptionRecord,
): CodexBankedResetOutcome => {
  emit(telemetry, "codex_reset_unknown", telemetryFields(context, candidate, { state: record.state, reason }));
  metric(telemetry, "codex_reset_unknown_total", 1, telemetryFields(context, candidate, {}));
  return outcome("pending", reason, context, record);
};

const liveSubmissionPolicyReason = (
  dependencies: CodexBankedResetDependencies,
  context: ResetContext,
): string | null => {
  return loadLiveSubmissionConfig(dependencies, context).reason;
};

const loadLiveSubmissionConfig = (
  dependencies: CodexBankedResetDependencies,
  context: ResetContext,
): Readonly<{ config: CodexBankedResetConfig; reason: null }> | Readonly<{ config: null; reason: string }> => {
  let config: CodexBankedResetConfig;
  try {
    config = dependencies.reloadConfig?.() ?? dependencies.config;
  } catch {
    return { config: null, reason: "configuration_unavailable" };
  }
  const reason = policyReason(config, context) ?? providerPolicyReason(config, dependencies.provider);
  return reason || config.mode !== "live"
    ? { config: null, reason: reason ?? "mode_not_live" }
    : { config, reason: null };
};

const verifyOwned = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  provider: CodexUsageResetProvider,
  clock: () => number,
  telemetry: CodexBankedResetTelemetry,
): Promise<CodexBankedResetOutcome> => {
  if (candidate.signal?.aborted) return outcome("pending", "client_aborted_before_verification", context, record);
  const startedAt = performance.now();
  let result: unknown;
  try {
    result = await provider.verifyApplied(context.account, candidate.signal ?? new AbortController().signal);
  } catch {
    const nowMs = readClock(clock);
    if (nowMs === null) return outcome("pending", "invalid_clock", context, record);
    const unknown = await unknownOwned(
      kv,
      context,
      record,
      nowMs,
      "verification_unavailable",
      record.provider_receipt_id,
    );
    return unknownOutcome(telemetry, context, candidate, "verification_unavailable", unknown ?? record);
  }
  const nowMs = readClock(clock);
  if (nowMs === null) return outcome("pending", "invalid_clock", context, record);
  if (result !== true) {
    const reason = typeof result === "boolean" ? "verification_not_applied" : "verification_response_invalid";
    const unknown = await unknownOwned(kv, context, record, nowMs, reason, record.provider_receipt_id);
    return unknownOutcome(telemetry, context, candidate, reason, unknown ?? record);
  }
  const finalized = await updateOwnedRecord(
    kv,
    context,
    record,
    (current) => stateWith(current, "verified", nowMs, { verified_at_ms: nowMs, last_error_code: null }),
  );
  if (!finalized) return outcome("pending", "verification_cas_failed", context, record);
  emit(telemetry, "codex_reset_verified", telemetryFields(context, candidate, { state: "verified" }));
  metric(telemetry, "codex_reset_verified_total", 1, telemetryFields(context, candidate, {}));
  metric(
    telemetry,
    "codex_reset_verification_latency_ms",
    Math.max(0, Math.round(performance.now() - startedAt)),
    telemetryFields(context, candidate, {}),
  );
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
  redeemOutcome: "reset" | "already_redeemed",
): Promise<CodexBankedResetOutcome> => {
  const finalized = await updateOwnedRecord(
    kv,
    context,
    record,
    (current) => stateWith(current, "verified", nowMs, { verified_at_ms: nowMs, last_error_code: null }),
  );
  if (!finalized) return outcome("pending", "redeem_outcome_finalization_cas_failed", context, record);
  emit(
    telemetry,
    "codex_reset_verified",
    telemetryFields(context, candidate, {
      state: "verified",
      verification_source: "redeem_outcome",
      redeem_outcome: redeemOutcome,
    }),
  );
  metric(telemetry, "codex_reset_verified_total", 1, telemetryFields(context, candidate, {}));
  metric(telemetry, "codex_reset_verification_latency_ms", 0, telemetryFields(context, candidate, {}));
  metric(telemetry, "codex_reset_estimated_spend_total", 1, telemetryFields(context, candidate, {}));
  return outcome("verified", `redeem_outcome_${redeemOutcome}`, context, finalized);
};

const reconcileOwned = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  provider: CodexUsageResetProvider,
  clock: () => number,
  telemetry: CodexBankedResetTelemetry,
): Promise<CodexBankedResetOutcome> => {
  if (candidate.signal?.aborted) return outcome("pending", "client_aborted_before_reconciliation", context, record);
  let terminalOutcomeCannotReconcile: boolean;
  try {
    terminalOutcomeCannotReconcile = providerTreatsRedeemOutcomeAsFinal(provider) &&
      provider.contract.lookup?.byIdempotencyKey !== true &&
      provider.contract.lookup?.byProviderReceiptId !== true &&
      provider.contract.verification?.independentlyVerifiable !== true;
  } catch {
    return outcome("pending", "provider_contract_unproven", context, record);
  }
  if (terminalOutcomeCannotReconcile) {
    return outcome("pending", "terminal_outcome_ambiguous", context, record);
  }
  let lookedUp: RedeemResetResult;
  try {
    lookedUp = await provider.lookup(
      { ...context.account, idempotencyKey: context.idempotencyKey, providerReceiptId: record.provider_receipt_id },
      candidate.signal ?? new AbortController().signal,
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
    const unknown = await unknownOwned(
      kv,
      context,
      record,
      nowMs,
      "lookup_response_invalid",
      record.provider_receipt_id,
    );
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
    const unknown = await unknownOwned(
      kv,
      context,
      record,
      nowMs,
      "lookup_response_invalid",
      record.provider_receipt_id,
    );
    return unknownOutcome(telemetry, context, candidate, "lookup_response_invalid", unknown ?? record);
  }
  const submitted = await preserveReceipt(kv, context, record, nowMs, durableReceiptId(provider, receipt));
  if (!submitted) return outcome("pending", "lookup_cas_failed", context, record);
  return await verifyOwned(kv, context, submitted, candidate, provider, clock, telemetry);
};

const submitClaimed = async (
  kv: Deno.Kv,
  context: ResetContext,
  record: CodexResetRedemptionRecord,
  candidate: CodexBankedResetCandidate,
  dependencies: CodexBankedResetDependencies,
  clock: () => number,
  telemetry: CodexBankedResetTelemetry,
): Promise<CodexBankedResetOutcome> => {
  const initialPolicy = liveSubmissionPolicyReason(dependencies, context);
  if (initialPolicy) return outcome("pending", `new_submission_${initialPolicy}`, context, record);
  const nowBeforeInventory = readClock(clock);
  if (nowBeforeInventory === null) return outcome("pending", "invalid_clock", context, record);
  if (!claimedDuringCurrentUtcDay(record, nowBeforeInventory)) {
    const rejected = await rejectOwned(kv, context, record, nowBeforeInventory, "claim_day_elapsed");
    return outcome("rejected", "claim_day_elapsed", context, rejected ?? record);
  }
  if (!quotaWindowIsOpen(candidate, nowBeforeInventory)) {
    const rejected = await rejectOwned(kv, context, record, nowBeforeInventory, "quota_window_expired");
    return outcome("rejected", "quota_window_expired", context, rejected ?? record);
  }
  if (candidate.signal?.aborted) {
    const rejected = await rejectOwned(kv, context, record, nowBeforeInventory, "client_aborted_before_submission");
    return outcome("rejected", "client_aborted_before_submission", context, rejected ?? record);
  }
  let selectedCredit = candidate.selectedCredit;
  let nowAfterInventory = nowBeforeInventory;
  if (!selectedCredit) {
    let inventory: ResetInventory;
    try {
      inventory = await dependencies.provider.readInventory(
        context.account,
        boundedInventorySignal(candidate.signal),
      );
    } catch {
      const nowMs = readClock(clock);
      if (nowMs === null) return outcome("pending", "invalid_clock", context, record);
      const rejected = await rejectOwned(kv, context, record, nowMs, "inventory_unavailable");
      emit(
        telemetry,
        "codex_reset_rejected",
        telemetryFields(context, candidate, { state: "rejected", reason: "inventory_unavailable" }),
      );
      return outcome("rejected", "inventory_unavailable", context, rejected ?? record);
    }
    const observedAfterInventory = readClock(clock);
    if (observedAfterInventory === null) return outcome("pending", "invalid_clock", context, record);
    nowAfterInventory = observedAfterInventory;
    if (!claimedDuringCurrentUtcDay(record, nowAfterInventory)) {
      const rejected = await rejectOwned(kv, context, record, nowAfterInventory, "claim_day_elapsed");
      return outcome("rejected", "claim_day_elapsed", context, rejected ?? record);
    }
    if (!quotaWindowIsOpen(candidate, nowAfterInventory)) {
      const rejected = await rejectOwned(kv, context, record, nowAfterInventory, "quota_window_expired");
      return outcome("rejected", "quota_window_expired", context, rejected ?? record);
    }
    if (!validInventory(inventory, nowAfterInventory)) {
      const rejected = await rejectOwned(
        kv,
        context,
        record,
        nowAfterInventory,
        "inventory_response_invalid_or_unsupported",
      );
      emit(
        telemetry,
        "codex_reset_rejected",
        telemetryFields(context, candidate, { state: "rejected", reason: "inventory_response_invalid_or_unsupported" }),
      );
      return outcome("rejected", "inventory_response_invalid_or_unsupported", context, rejected ?? record);
    }
    const selection = selectInventoryCredit(inventory, dependencies.provider, nowAfterInventory);
    if (selection.kind !== "selected") {
      const reason = selection.kind === "empty" ? "inventory_empty" : "inventory_no_eligible_codex_credit";
      const rejected = await rejectOwned(kv, context, record, nowAfterInventory, reason);
      emit(telemetry, "codex_reset_rejected", telemetryFields(context, candidate, { state: "rejected", reason }));
      return outcome("rejected", reason, context, rejected ?? record);
    }
    selectedCredit = selection.credit;
  }
  if (
    !selectedCredit || !isNonEmptyText(selectedCredit.id, 512) || selectedCredit.status !== "available" ||
    selectedCredit.resetType !== "codex_rate_limits" ||
    !providerSupportsResetType(dependencies.provider, selectedCredit.resetType) ||
    (selectedCredit.expiresAtMs !== null &&
      (!isSafeMs(selectedCredit.expiresAtMs) || selectedCredit.expiresAtMs <= nowAfterInventory))
  ) {
    const rejected = await rejectOwned(kv, context, record, nowAfterInventory, "selected_credit_invalid_or_expired");
    emit(
      telemetry,
      "codex_reset_rejected",
      telemetryFields(context, candidate, { state: "rejected", reason: "selected_credit_invalid_or_expired" }),
    );
    return outcome("rejected", "selected_credit_invalid_or_expired", context, rejected ?? record);
  }
  if (!claimedDuringCurrentUtcDay(record, nowAfterInventory)) {
    const rejected = await rejectOwned(kv, context, record, nowAfterInventory, "claim_day_elapsed");
    return outcome("rejected", "claim_day_elapsed", context, rejected ?? record);
  }
  if (!quotaWindowIsOpen(candidate, nowAfterInventory)) {
    const rejected = await rejectOwned(kv, context, record, nowAfterInventory, "quota_window_expired");
    return outcome("rejected", "quota_window_expired", context, rejected ?? record);
  }
  // Re-read the kill switch after inventory and immediately before the fenced
  // side-effect boundary. A disable leaves `claimed` intact and makes no call.
  const finalConfig = loadLiveSubmissionConfig(dependencies, context);
  if (finalConfig.config === null) return outcome("pending", `new_submission_${finalConfig.reason}`, context, record);
  if (candidate.signal?.aborted) {
    const rejected = await rejectOwned(kv, context, record, nowAfterInventory, "client_aborted_before_submission");
    return outcome("rejected", "client_aborted_before_submission", context, rejected ?? record);
  }
  // Inventory validation and policy checks may take long enough for the
  // current quota window to end. Do not cross the durable side-effect
  // boundary after that deadline.
  const nowBeforePreparation = readClock(clock);
  if (nowBeforePreparation === null) return outcome("pending", "invalid_clock", context, record);
  if (!claimedDuringCurrentUtcDay(record, nowBeforePreparation)) {
    const rejected = await rejectOwned(kv, context, record, nowBeforePreparation, "claim_day_elapsed");
    return outcome("rejected", "claim_day_elapsed", context, rejected ?? record);
  }
  if (!quotaWindowIsOpen(candidate, nowBeforePreparation)) {
    const rejected = await rejectOwned(kv, context, record, nowBeforePreparation, "quota_window_expired");
    return outcome("rejected", "quota_window_expired", context, rejected ?? record);
  }
  const prepared = await prepareSubmission(
    kv,
    context,
    candidate,
    record,
    nowBeforePreparation,
    clock,
    finalConfig.config.maxGlobalPerDay,
  );
  if (prepared.kind === "failure") {
    return outcome(prepared.code === "global_limit_reached" ? "skipped" : "pending", prepared.code, context, record);
  }
  // `prepareSubmission` itself awaits strong reads and a CAS. Re-read the
  // kill switch after that durable transition. A disable visible at this
  // final pre-renewal check leaves the conservative `submitted` record
  // available for non-submitting recovery and makes no provider call.
  const beforeRedeemPolicy = liveSubmissionPolicyReason(dependencies, context);
  if (beforeRedeemPolicy) return outcome("pending", `new_submission_${beforeRedeemPolicy}`, context, prepared.record);
  if (candidate.signal?.aborted) {
    const nowMs = readClock(clock);
    if (nowMs === null) return outcome("pending", "invalid_clock", context, prepared.record);
    const unknown = await unknownOwned(kv, context, prepared.record, nowMs, "client_aborted_after_submission", null);
    return unknownOutcome(telemetry, context, candidate, "client_aborted_after_submission", unknown ?? prepared.record);
  }
  const renewed = await renewSubmittedForRedeem(kv, context, candidate, prepared.record, clock);
  if (renewed.kind === "failure") return outcome("pending", renewed.code, context, prepared.record);
  // The last lease/fence renewal itself awaits KV. Re-read the kill switch
  // synchronously after it returns so a disable that landed during that final
  // renewal cannot proceed to the provider call. `reloadConfig` is
  // deliberately synchronous; do not introduce an await after this point.
  const afterRenewalPolicy = liveSubmissionPolicyReason(dependencies, context);
  if (afterRenewalPolicy) {
    return outcome("pending", `new_submission_${afterRenewalPolicy}`, context, renewed.record);
  }
  if (candidate.signal?.aborted) {
    const nowMs = readClock(clock);
    if (nowMs === null) return outcome("pending", "invalid_clock", context, renewed.record);
    const unknown = await unknownOwned(kv, context, renewed.record, nowMs, "client_aborted_after_submission", null);
    return unknownOutcome(telemetry, context, candidate, "client_aborted_after_submission", unknown ?? renewed.record);
  }
  const nowBeforeRedeem = readClock(clock);
  if (nowBeforeRedeem === null) return outcome("pending", "invalid_clock", context, renewed.record);
  if (!claimedDuringCurrentUtcDay(renewed.record, nowBeforeRedeem)) {
    return outcome("pending", "claim_day_elapsed", context, renewed.record);
  }
  if (!quotaWindowIsOpen(candidate, nowBeforeRedeem)) {
    return outcome("pending", "quota_window_expired", context, renewed.record);
  }
  if (renewed.record.lease_expires_at_ms <= nowBeforeRedeem) {
    return outcome("pending", "stale_owner", context, renewed.record);
  }

  let submittedPromise: Promise<RedeemResetResult>;
  try {
    // Do not insert telemetry or another await between the final synchronous
    // kill-switch check above and starting the provider invocation.
    submittedPromise = dependencies.provider.redeem(
      { ...context.account, idempotencyKey: context.idempotencyKey, creditId: selectedCredit.id },
      candidate.signal ?? new AbortController().signal,
    );
  } catch {
    const nowMs = readClock(clock);
    if (nowMs === null) return outcome("pending", "invalid_clock", context, renewed.record);
    const unknown = await unknownOwned(kv, context, renewed.record, nowMs, "submit_transport_unknown", null);
    return unknownOutcome(telemetry, context, candidate, "submit_transport_unknown", unknown ?? renewed.record);
  }
  emit(telemetry, "codex_reset_submit_started", telemetryFields(context, candidate, { state: "submitted" }));
  metric(telemetry, "codex_reset_submission_attempts_total", 1, telemetryFields(context, candidate, {}));

  let submittedResult: RedeemResetResult;
  try {
    submittedResult = await submittedPromise;
  } catch {
    const nowMs = readClock(clock);
    if (nowMs === null) return outcome("pending", "invalid_clock", context, renewed.record);
    const unknown = await unknownOwned(kv, context, renewed.record, nowMs, "submit_transport_unknown", null);
    return unknownOutcome(telemetry, context, candidate, "submit_transport_unknown", unknown ?? renewed.record);
  }
  const nowAfterRedeem = readClock(clock);
  if (nowAfterRedeem === null) return outcome("pending", "invalid_clock", context, renewed.record);
  if (!validRedeemResult(submittedResult)) {
    const unknown = await unknownOwned(kv, context, renewed.record, nowAfterRedeem, "submit_response_invalid", null);
    return unknownOutcome(telemetry, context, candidate, "submit_response_invalid", unknown ?? renewed.record);
  }
  if (submittedResult.kind === "rejected") {
    const rejected = await rejectOwned(kv, context, renewed.record, nowAfterRedeem, "provider_rejected");
    emit(
      telemetry,
      "codex_reset_rejected",
      telemetryFields(context, candidate, { state: "rejected", reason: "provider_rejected" }),
    );
    return outcome("rejected", "provider_rejected", context, rejected ?? renewed.record);
  }
  if (submittedResult.kind === "unknown") {
    const unknown = await unknownOwned(
      kv,
      context,
      renewed.record,
      nowAfterRedeem,
      "provider_commit_unknown",
      durableReceiptId(dependencies.provider, submittedResult.providerReceiptId),
    );
    return unknownOutcome(telemetry, context, candidate, "provider_commit_unknown", unknown ?? renewed.record);
  }
  if (
    (submittedResult.kind === "completed" || submittedResult.kind === "already_redeemed") &&
    providerTreatsRedeemOutcomeAsFinal(dependencies.provider)
  ) {
    emit(
      telemetry,
      "codex_reset_submitted",
      telemetryFields(context, candidate, { state: "submitted", provider_receipt_id: null }),
    );
    return await finalizeDocumentedRedeemOutcome(
      kv,
      context,
      renewed.record,
      candidate,
      nowAfterRedeem,
      telemetry,
      submittedResult.kind === "completed" ? "reset" : "already_redeemed",
    );
  }
  const receipt = receiptId(submittedResult.providerReceiptId);
  if (!receipt) {
    const unknown = await unknownOwned(kv, context, renewed.record, nowAfterRedeem, "submit_response_invalid", null);
    return unknownOutcome(telemetry, context, candidate, "submit_response_invalid", unknown ?? renewed.record);
  }
  const persistedReceipt = await preserveReceipt(
    kv,
    context,
    renewed.record,
    nowAfterRedeem,
    durableReceiptId(dependencies.provider, receipt),
  );
  if (!persistedReceipt) return outcome("pending", "receipt_cas_failed", context, renewed.record);
  emit(
    telemetry,
    "codex_reset_submitted",
    telemetryFields(context, candidate, {
      state: "submitted",
      provider_receipt_id: durableReceiptId(dependencies.provider, receipt),
    }),
  );
  return await verifyOwned(kv, context, persistedReceipt, candidate, dependencies.provider, clock, telemetry);
};

const attemptInternal = async (
  candidate: CodexBankedResetCandidate,
  dependencies: CodexBankedResetDependencies,
  reconcileOnly: boolean,
): Promise<CodexBankedResetOutcome> => {
  const hash = dependencies.hash ?? sha256Hex;
  const context = await makeResetContext(candidate, hash);
  if (!context) return outcome("skipped", "invalid_quota_generation");
  const telemetry = dependencies.telemetry ?? defaultTelemetry;
  const fields = telemetryFields(context, candidate, {});
  let kv: Deno.Kv | null;
  try {
    kv = dependencies.kv === undefined ? await getKv() : dependencies.kv;
  } catch {
    return outcome("skipped", "kv_unavailable", context);
  }
  if (!kv) return outcome("skipped", "kv_unavailable", context);

  const existing = await readExistingRecord(kv, context);
  if (existing.code) return outcome("skipped", existing.code, context);
  if (existing.record && !matchesContext(existing.record, context)) {
    return outcome("skipped", "redemption_record_context_mismatch", context, existing.record);
  }
  if (existing.record?.state === "verified") {
    emit(
      telemetry,
      "codex_reset_duplicate_prevented",
      telemetryFields(context, candidate, { state: "verified", fence: existing.record.fence }),
    );
    metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
    return outcome("verified", "previously_verified", context, existing.record);
  }
  if (existing.record?.state === "rejected") {
    emit(
      telemetry,
      "codex_reset_duplicate_prevented",
      telemetryFields(context, candidate, { state: "rejected", fence: existing.record.fence }),
    );
    metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
    return outcome("rejected", existing.record.last_error_code ?? "previously_rejected", context, existing.record);
  }

  let allowNewSubmission = false;
  let configForClaim: CodexBankedResetConfig | null = null;
  if (!existing.record) {
    if (reconcileOnly) return outcome("skipped", "no_existing_transaction", context);
    try {
      configForClaim = dependencies.reloadConfig?.() ?? dependencies.config;
    } catch {
      return outcome("skipped", "configuration_unavailable", context);
    }
    const reason = policyReason(configForClaim, context);
    if (reason) return outcome("skipped", reason, context);
    const providerReason = providerPolicyReason(configForClaim, dependencies.provider);
    if (providerReason) return outcome("skipped", providerReason, context);
    emit(telemetry, "codex_reset_eligible", fields);
    metric(telemetry, "codex_reset_eligible_total", 1, fields);
    if (configForClaim.mode === "shadow") {
      emit(telemetry, "codex_reset_shadow_candidate", fields);
      metric(telemetry, "codex_reset_shadow_candidates_total", 1, fields);
      // Shadow mode deliberately makes no provider call, including inventory.
      return outcome("skipped", "shadow", context);
    }
    allowNewSubmission = true;
  } else if (existing.record.state === "claimed") {
    if (reconcileOnly) return outcome("pending", "unsubmitted_transaction", context, existing.record);
    const reason = liveSubmissionPolicyReason(dependencies, context);
    if (reason) return outcome("pending", `new_submission_${reason}`, context, existing.record);
    allowNewSubmission = true;
  }

  if (!providerSupportsLiveRedemption(dependencies.provider)) {
    if (!existing.record) {
      emit(
        telemetry,
        "codex_reset_rejected",
        telemetryFields(context, candidate, { state: "rejected", reason: "provider_contract_unproven" }),
      );
    }
    return outcome("skipped", "provider_contract_unproven", context, existing.record);
  }
  const clock = dependencies.now ?? Date.now;
  const nowMs = readClock(clock);
  if (nowMs === null) return outcome("skipped", "invalid_clock", context, existing.record);
  const ownerToken = safeOwnerToken(dependencies.newOwnerToken ?? crypto.randomUUID);
  if (!ownerToken) return outcome("skipped", "owner_token_unavailable", context, existing.record);
  const claimed = await claimTransaction(
    kv,
    context,
    candidate,
    nowMs,
    clock,
    ownerToken,
    allowNewSubmission,
  );
  switch (claimed.kind) {
    case "failure":
      return outcome("skipped", claimed.code, context);
    case "no_transaction":
      return outcome("skipped", "no_existing_transaction", context);
    case "global_limit":
      return outcome("skipped", "global_limit_reached", context);
    case "in_progress":
      emit(
        telemetry,
        "codex_reset_duplicate_prevented",
        telemetryFields(context, candidate, { state: claimed.record.state, fence: claimed.record.fence }),
      );
      metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
      return outcome("pending", "transaction_in_progress", context, claimed.record);
    case "rejected":
      emit(
        telemetry,
        "codex_reset_duplicate_prevented",
        telemetryFields(context, candidate, { state: "rejected", fence: claimed.record.fence }),
      );
      metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
      return outcome("rejected", claimed.record.last_error_code ?? "previously_rejected", context, claimed.record);
    case "verified":
      emit(
        telemetry,
        "codex_reset_duplicate_prevented",
        telemetryFields(context, candidate, { state: "verified", fence: claimed.record.fence }),
      );
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
        }),
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
        }),
      );
      metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
      return await reconcileOwned(kv, context, claimed.record, candidate, dependencies.provider, clock, telemetry);
  }
};

/**
 * Attempt or reconcile exactly one logical reset after normal account
 * failover. New external submissions require live policy and all durable
 * fences; an existing submitted/unknown record is reconciled even while the
 * kill switch is disabled.
 */
export const attemptCodexBankedReset = async (
  candidate: CodexBankedResetCandidate,
  dependencies: CodexBankedResetDependencies,
): Promise<CodexBankedResetOutcome> => await attemptInternal(candidate, dependencies, false);

/**
 * Recovery-only path for a durable submitted/unknown record. It never creates
 * a claim or calls `redeem`, so it remains safe during a rollback.
 */
export const reconcileCodexBankedReset = async (
  candidate: CodexBankedResetCandidate,
  dependencies: CodexBankedResetDependencies,
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
  reset: CodexBankedResetOutcome | null = null,
): CodexBankedResetPoolOutcome => ({ kind, reason, selected, reset });

const sameShadowFences = (
  left: CodexResetShadowDecisionRecord["fences"],
  right: CodexResetShadowDecisionRecord["fences"],
): boolean =>
  left.length === right.length && left.every((fence, index) => {
    const other = right[index];
    return other !== undefined && fence.slot === other.slot && fence.account_id_hash === other.account_id_hash &&
      fence.quota_generation === other.quota_generation && fence.routing_generation === other.routing_generation &&
      fence.quota_reset_at_ms === other.quota_reset_at_ms;
  });

const loadCurrentPoolConfig = (
  dependencies: CodexBankedResetDependencies,
): Readonly<{ config: CodexBankedResetConfig; reason: null }> | Readonly<{ config: null; reason: string }> => {
  try {
    const config = dependencies.reloadConfig?.() ?? dependencies.config;
    if (!config.enabled) return { config: null, reason: "feature_disabled" };
    if (config.mode === "disabled") return { config: null, reason: "mode_disabled" };
    if (config.accountAllowlist.size === 0) return { config: null, reason: "account_allowlist_required" };
    if (config.maxGlobalPerDay <= 0) return { config: null, reason: "global_limit_disabled" };
    if (config.maxPerAccountPerWindow !== 1) return { config: null, reason: "per_account_window_limit_invalid" };
    return { config, reason: null };
  } catch {
    return { config: null, reason: "configuration_unavailable" };
  }
};

const shadowDecisionRecord = async (
  kv: Deno.Kv,
  record: CodexResetShadowDecisionRecord,
  nowMs: number,
): Promise<Readonly<{ kind: "written" | "duplicate"; record: CodexResetShadowDecisionRecord }> | null> => {
  const key = codexResetShadowDecisionKey(record.episode_hash);
  for (let attempt = 0; attempt < MAX_CAS_ATTEMPTS; attempt += 1) {
    let entry: Deno.KvEntryMaybe<unknown>;
    try {
      entry = await kv.get<unknown>(key, { consistency: "strong" });
    } catch {
      return null;
    }
    if (entry.value !== null) {
      const existing = parseCodexResetShadowDecisionRecord(entry.value);
      if (!existing || existing.episode_hash !== record.episode_hash || existing.expires_at_ms <= nowMs) return null;
      return { kind: "duplicate", record: existing };
    }
    try {
      const committed = await kv.atomic().check(entry).set(key, record).commit();
      if (committed.ok) return { kind: "written", record };
    } catch {
      return null;
    }
  }
  return null;
};

const readShadowDecision = async (
  kv: Deno.Kv,
  episodeHash: string,
): Promise<CodexResetShadowDecisionRecord | null> => {
  try {
    const entry = await kv.get<unknown>(codexResetShadowDecisionKey(episodeHash), { consistency: "strong" });
    return entry.value === null ? null : parseCodexResetShadowDecisionRecord(entry.value);
  } catch {
    return null;
  }
};

/** Read-only, redacted administrative projection of recent shadow evidence. */
export const listCodexResetShadowDecisions = async (
  kvOverride?: Deno.Kv | null,
): Promise<readonly CodexResetShadowDecisionRecord[] | null> => {
  let kv: Deno.Kv | null;
  try {
    kv = kvOverride === undefined ? await getKv() : kvOverride;
  } catch {
    return null;
  }
  if (!kv) return null;
  const decisions: CodexResetShadowDecisionRecord[] = [];
  try {
    for await (const entry of kv.list<unknown>({ prefix: CODEX_RESET_SHADOW_DECISION_KV_PREFIX }, { limit: 100 })) {
      const parsed = parseCodexResetShadowDecisionRecord(entry.value);
      if (parsed) decisions.push(parsed);
    }
  } catch {
    return null;
  }
  decisions.sort((left, right) =>
    right.created_at_ms - left.created_at_ms || left.episode_hash.localeCompare(right.episode_hash)
  );
  return decisions;
};

/**
 * Evaluate one complete currently fenced blocked cohort. Shadow reads each
 * blocked account's inventory and persists exactly one redacted decision.
 * Live first requires that decision, then repeats the fence and inventory
 * proof; it reaches the durable ledger only when the exact account and exact
 * opaque credit still match.
 */
export const evaluateCodexBankedResetPool = async (
  candidates: readonly CodexBankedResetPoolCandidate[],
  dependencies: CodexBankedResetDependencies,
): Promise<CodexBankedResetPoolOutcome> => {
  const clock = dependencies.now ?? Date.now;
  const nowMs = readClock(clock);
  if (nowMs === null) return poolOutcome("skipped", "invalid_clock");
  if (!candidates.length) return poolOutcome("skipped", "full_pool_missing");
  const ordered = [...candidates].sort((left, right) => left.slot - right.slot);
  if (
    ordered.some((candidate, index) =>
      !isSafeNonnegativeInteger(candidate.slot) || (index > 0 && ordered[index - 1]!.slot === candidate.slot)
    )
  ) return poolOutcome("skipped", "full_pool_invalid");

  const loadedConfig = loadCurrentPoolConfig(dependencies);
  if (!loadedConfig.config) return poolOutcome("skipped", loadedConfig.reason);
  const config = loadedConfig.config;
  if (
    config.mode === "live" && config.maxGlobalPerDay !== 1 &&
    ordered.some(({ provider }) => providerTreatsRedeemOutcomeAsFinal(provider))
  ) {
    return poolOutcome("skipped", "terminal_outcome_global_limit_must_be_one");
  }
  let kv: Deno.Kv | null;
  try {
    kv = dependencies.kv === undefined ? await getKv() : dependencies.kv;
  } catch {
    return poolOutcome("skipped", "kv_unavailable");
  }
  if (!kv) return poolOutcome("skipped", "kv_unavailable");

  const hash = dependencies.hash ?? sha256Hex;
  const resolved = await Promise.all(ordered.map(async (pool) => {
    const context = await makeResetContext(pool.candidate, hash);
    return context ? { pool, context } satisfies ResolvedPoolCandidate : null;
  }));
  if (resolved.some((candidate) => candidate === null)) return poolOutcome("skipped", "invalid_quota_generation");
  const complete = resolved as ResolvedPoolCandidate[];
  if (complete.some(({ pool }) => !quotaWindowIsOpen(pool.candidate, nowMs))) {
    return poolOutcome("skipped", "quota_window_expired");
  }
  const fences: CodexResetShadowDecisionRecord["fences"] = complete.map(({ pool, context }) => ({
    slot: pool.slot,
    account_id_hash: context.account.accountIdHash,
    quota_generation: context.account.quotaGeneration,
    routing_generation: pool.candidate.routingGeneration,
    quota_reset_at_ms: pool.candidate.quotaResetAtMs,
  }));
  const episodeHash = await hash(
    `uos_ai\u0000codex_reset_shadow_episode\u0000${
      fences.map((fence) =>
        `${fence.slot}\u0000${fence.account_id_hash}\u0000${fence.quota_generation}\u0000${fence.routing_generation}\u0000${fence.quota_reset_at_ms}`
      ).join("\u0001")
    }`,
  );
  if (!isNonEmptyText(episodeHash)) return poolOutcome("skipped", "episode_hash_unavailable");

  let audited: CodexResetShadowDecisionRecord | null = null;
  if (config.mode === "live" && !dependencies.allowLiveWithoutShadowForTest) {
    audited = await readShadowDecision(kv, episodeHash);
    if (
      !audited || audited.expires_at_ms <= nowMs || audited.decision_reason !== "selected" ||
      !sameShadowFences(audited.fences, fences)
    ) return poolOutcome("skipped", "shadow_decision_missing_or_expired");
  }

  for (const { pool } of complete) {
    const current = await readCurrentFences(kv, pool.candidate);
    if (current.kind !== "valid") {
      return poolOutcome("skipped", current.kind === "stale" ? "routing_fence_stale" : current.code);
    }
  }

  if (config.mode === "shadow") {
    const existing = await readShadowDecision(kv, episodeHash);
    if (
      existing && existing.expires_at_ms > nowMs && sameShadowFences(existing.fences, fences)
    ) {
      const selected = complete.find(({ context }) =>
        context.account.accountIdHash === existing.selected_account_id_hash
      ) ?? null;
      const telemetry = dependencies.telemetry ?? defaultTelemetry;
      const telemetryCandidate = selected ?? complete[0]!;
      const fields = telemetryFields(telemetryCandidate.context, telemetryCandidate.pool.candidate, {
        episode_hash: episodeHash,
        credit_id_hash: existing.selected_credit_id_hash,
        selected: existing.selected_account_id_hash !== null,
        reason: existing.decision_reason,
      });
      emit(telemetry, "codex_reset_duplicate_prevented", { ...fields, reason: "shadow_decision_exists" });
      metric(telemetry, "codex_reset_duplicate_prevented_total", 1, fields);
      const wouldSpend = existing.decision_reason === "selected" &&
        existing.selected_account_id_hash !== null &&
        existing.selected_credit_id_hash !== null &&
        selected !== null;
      return poolOutcome(
        "shadow",
        wouldSpend ? "already_would_spend_once" : existing.decision_reason,
        wouldSpend ? selected.pool : null,
      );
    }
  }

  const inventoryResults = await Promise.all(complete.map(async (resolvedCandidate) => {
    try {
      const inventory = await resolvedCandidate.pool.provider.readInventory(
        resolvedCandidate.context.account,
        boundedInventorySignal(resolvedCandidate.pool.candidate.signal),
      );
      return { resolvedCandidate, inventory } as const;
    } catch {
      return { resolvedCandidate, inventory: null } as const;
    }
  }));
  const nowAfterInventory = readClock(clock);
  if (nowAfterInventory === null) return poolOutcome("skipped", "invalid_clock");
  if (inventoryResults.some(({ inventory }) => inventory === null)) {
    return poolOutcome("skipped", "inventory_unavailable");
  }
  // Re-check all fences after remote reads. A selection based on any stale
  // account state is not audit evidence and can never become a live spend.
  for (const { pool } of complete) {
    const current = await readCurrentFences(kv, pool.candidate);
    if (current.kind !== "valid") {
      return poolOutcome("skipped", current.kind === "stale" ? "routing_fence_stale" : current.code);
    }
  }
  if (complete.some(({ pool }) => !quotaWindowIsOpen(pool.candidate, nowAfterInventory))) {
    return poolOutcome("skipped", "quota_window_expired");
  }

  const selectedCredits: SelectedPoolCredit[] = [];
  let decisionReason = "inventory_empty";
  for (const result of inventoryResults) {
    const inventory = result.inventory!;
    if (
      !validInventory(inventory, nowAfterInventory) ||
      inventory.credits.some((credit) =>
        credit.status === "available" && credit.expiresAtMs !== null && credit.expiresAtMs <= nowAfterInventory
      )
    ) {
      decisionReason = "inventory_response_invalid_or_expired";
      selectedCredits.length = 0;
      break;
    }
    const selection = selectInventoryCredit(inventory, result.resolvedCandidate.pool.provider, nowAfterInventory);
    if (selection.kind !== "selected") {
      if (selection.kind === "no_eligible_credit") decisionReason = "inventory_no_eligible_codex_credit";
      continue;
    }
    const context = result.resolvedCandidate.context;
    if (
      !config.accountAllowlist.has(context.account.accountId) &&
      !config.accountAllowlist.has(context.account.accountIdHash)
    ) {
      continue;
    }
    const creditIdHash = await hash(`uos_ai\u0000codex_reset_credit\u0000${selection.credit.id}`);
    if (!isNonEmptyText(creditIdHash)) return poolOutcome("skipped", "credit_hash_unavailable");
    selectedCredits.push({ resolved: result.resolvedCandidate, credit: selection.credit, creditIdHash });
  }
  selectedCredits.sort((left, right) => {
    const leftExpiry = left.credit.expiresAtMs ?? Number.POSITIVE_INFINITY;
    const rightExpiry = right.credit.expiresAtMs ?? Number.POSITIVE_INFINITY;
    return leftExpiry - rightExpiry || left.resolved.pool.slot - right.resolved.pool.slot ||
      left.credit.id.localeCompare(right.credit.id);
  });
  const selected = selectedCredits[0] ?? null;
  if (selected) decisionReason = "selected";
  else if (decisionReason === "inventory_empty") decisionReason = "no_allowlisted_eligible_credit";

  const episodeExpiresAtMs = Math.min(
    ...fences.map((fence) => fence.quota_reset_at_ms),
    selected?.credit.expiresAtMs ?? Number.POSITIVE_INFINITY,
  );
  if (!isSafeMs(episodeExpiresAtMs) || episodeExpiresAtMs <= nowAfterInventory) {
    return poolOutcome("skipped", "shadow_decision_expired");
  }
  const decision: CodexResetShadowDecisionRecord = {
    v: 1,
    episode_hash: episodeHash,
    created_at_ms: nowAfterInventory,
    expires_at_ms: episodeExpiresAtMs,
    decision_reason: decisionReason,
    selected_account_id_hash: selected?.resolved.context.account.accountIdHash ?? null,
    selected_credit_id_hash: selected?.creditIdHash ?? null,
    selected_credit_expires_at_ms: selected?.credit.expiresAtMs ?? null,
    fences,
  };

  if (config.mode === "shadow") {
    const persisted = await shadowDecisionRecord(kv, decision, nowAfterInventory);
    if (!persisted) return poolOutcome("skipped", "shadow_decision_unavailable");
    const telemetry = dependencies.telemetry ?? defaultTelemetry;
    const telemetryCandidate = selected?.resolved ?? complete[0]!;
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
    return poolOutcome("shadow", selected ? "shadow_selected" : decisionReason, selected?.resolved.pool ?? null);
  }

  // Preserve the precise inventory failure in live mode. This is especially
  // useful for an expired credit after a valid shadow decision: it proves
  // that no external redemption was attempted because the fresh inventory
  // itself was no longer eligible.
  if (!selected) return poolOutcome("skipped", decisionReason);
  if (
    !dependencies.allowLiveWithoutShadowForTest &&
    (!audited || audited.selected_account_id_hash !== selected.resolved.context.account.accountIdHash ||
      audited.selected_credit_id_hash !== selected.creditIdHash ||
      audited.selected_credit_expires_at_ms !== selected.credit.expiresAtMs ||
      audited.expires_at_ms <= nowAfterInventory)
  ) return poolOutcome("skipped", "shadow_decision_drift");

  // The durable claim/submission path must continue to fence every blocked
  // account that established the audited episode, not just the selected
  // owner. Recovery or rotation after the shadow read makes the episode stale
  // and must block the external consume.
  const fullPoolFences = complete.flatMap(({ pool }) => pool.candidate.fences);

  const reset = await attemptCodexBankedReset(
    { ...selected.resolved.pool.candidate, fences: fullPoolFences, selectedCredit: selected.credit },
    { ...dependencies, provider: selected.resolved.pool.provider },
  );
  return poolOutcome(reset.kind, reset.reason, selected.resolved.pool, reset);
};
