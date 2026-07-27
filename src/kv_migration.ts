import { type KvEntryJSON, toKey, toValue } from "@deno/kv-utils/json";
import { API_KEY_HASH_PREFIX, API_KEY_ID_PREFIX, apiKeyHashKey, apiKeyIdKey } from "./api_keys.ts";
import {
  API_KEY_USAGE_V3_REQUEST_PREFIX,
  API_KEY_USAGE_V3_WINDOW_PREFIX,
  type ApiKeyPolicy,
  apiKeyPolicyFromHashRecord,
  apiKeyUsageV2Key,
  apiKeyUsageV3RetentionMs,
  apiKeyUsageV3WindowKey,
  makeApiKeyUsageWindowV3,
  normalizeApiKeyUsageRequestV3,
  normalizeApiKeyUsageWindowV3,
} from "./api_key_policy.ts";
import {
  KERNEL_ORG_POLICY_V2_PREFIX,
  KERNEL_ORG_WINDOW_V2_PREFIX,
  KERNEL_REPO_POLICY_V2_PREFIX,
  KERNEL_REPO_WINDOW_V2_PREFIX,
  kernelOrgPolicyKey,
  kernelOrgWindowKey,
  type KernelQuotaPolicyV2,
  type KernelQuotaWindowV2,
  kernelRepoPolicyKey,
  kernelRepoWindowKey,
  normalizeKernelQuotaPolicyV2,
  normalizeKernelQuotaWindowV2,
} from "./kernel_quota_v2.ts";
import {
  DEFAULT_KERNEL_POLICY_LIMIT_KEY,
  DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS,
  DEFAULT_KERNEL_POLICY_WINDOW_KEY,
  DEFAULT_KERNEL_POLICY_WINDOW_MS,
} from "./defaults.ts";
import { buildRuntimeConfig, normalizeRuntimeConfig, RUNTIME_CONFIG_V2_KEY } from "./runtime_config.ts";
import type {
  ApiKeyHashRecord,
  ApiKeyRecord,
  ApiKeyRequestLogRecord,
  ApiKeyUsageWindowV3,
  PaidFallbackRequestV3,
  PaidFallbackWindowV3,
} from "./types.ts";
import { hasStrictPaidFallbackKeyPolicy, hasStrictPaidFallbackPolicy } from "./paid_fallback.ts";
import { isRecord } from "./utils.ts";

export type KvMigrationProfile = "local" | "prod";
export type KvMigrationDecisionAction = "import" | "skip" | "optional";

export type KvMigrationDecision = Readonly<{
  action: KvMigrationDecisionAction;
  group: string;
  reason: string;
}>;

export type KvMigrationCounters = {
  total: number;
  imported: number;
  skipped: number;
  optional: number;
  errors: number;
};

export type KvMigrationClassifyOptions = Readonly<{
  profile: KvMigrationProfile;
  includeCache: boolean;
  includeLegacy: boolean;
}>;

export type KvMigrationImportOptions =
  & KvMigrationClassifyOptions
  & Readonly<{
    overwrite: boolean;
    dryRun: boolean;
  }>;

export type KvMigrationImportResult = KvMigrationCounters & {
  groups: Record<string, number>;
};

export type KvMigrationValidationResult = {
  counts: {
    api_key_ids: number;
    api_key_hashes: number;
    api_key_bounded_counters_v2: number;
    api_key_bounded_counter_baselines_v2: number;
    api_key_bounded_counter_reconciled_baselines_v2: number;
    api_key_usage_v3_windows: number;
    api_key_usage_v3_requests: number;
    paid_fallback_ledger: number;
    paid_fallback_v3_windows: number;
    paid_fallback_v3_requests: number;
    paid_fallback_v3_pending: number;
    paid_fallback_v3_reconciliation_leases: number;
    paid_fallback_v3_deletion_guards: number;
    kernel_repo_limits: number;
    kernel_org_limits: number;
    kernel_v2_repo_policies: number;
    kernel_v2_org_policies: number;
    kernel_v2_repo_windows: number;
    kernel_v2_org_windows: number;
    passkey_users: number;
    passkey_credentials: number;
    agent_messages: number;
    embeddings_v2_at_most_10000: number;
    legacy_model_key_configs: number;
    legacy_model_key_health: number;
  };
  settings_present: {
    codex_auth: boolean;
    codex_models: boolean;
    default_model: boolean;
    default_reasoning_effort: boolean;
    default_kernel_policy_limit_requests: boolean;
    default_kernel_policy_window_ms: boolean;
    voyage_api_key: boolean;
    kernel_pubkeys: boolean;
    runtime_config_v2: boolean;
  };
  errors: string[];
};

const DURABLE_PREFIXES: Array<{ group: string; prefix: Deno.KvKey }> = [
  { group: "api_keys_id", prefix: ["ubq_ai", "api_keys", "id"] },
  { group: "api_keys_hash", prefix: ["ubq_ai", "api_keys", "hash"] },
  { group: "api_keys_usage", prefix: ["ubq_ai", "api_keys", "usage"] },
  { group: "api_keys_usage_daily", prefix: ["ubq_ai", "api_keys", "usage_daily"] },
  { group: "api_keys_request_log", prefix: ["ubq_ai", "api_keys", "request_log"] },
  { group: "api_key_usage_v2", prefix: ["uos_ai", "api_key_usage", "v2"] },
  { group: "api_key_usage_v3_windows", prefix: ["uos_ai", "api_key_usage", "v3", "window"] },
  { group: "api_key_usage_v3_requests", prefix: ["uos_ai", "api_key_usage", "v3", "request"] },
  { group: "paid_fallback_ledger", prefix: ["uos_ai", "paid_fallback", "ledger"] },
  { group: "paid_fallback_v3_windows", prefix: ["uos_ai", "paid_fallback", "v3", "window"] },
  { group: "paid_fallback_v3_requests", prefix: ["uos_ai", "paid_fallback", "v3", "request"] },
  { group: "paid_fallback_v3_pending", prefix: ["uos_ai", "paid_fallback", "v3", "pending"] },
  {
    group: "paid_fallback_v3_reconciliation_leases",
    prefix: ["uos_ai", "paid_fallback", "v3", "reconciliation_lease"],
  },
  {
    group: "paid_fallback_v3_deletion_guards",
    prefix: ["uos_ai", "paid_fallback", "v3", "deletion_guard"],
  },
  { group: "runtime_config_v2", prefix: ["uos_ai", "runtime_config", "v2"] },
  { group: "kernel_usage", prefix: ["ubq_ai", "kernel_auth", "usage"] },
  { group: "kernel_usage_daily", prefix: ["ubq_ai", "kernel_auth", "usage_daily"] },
  { group: "kernel_quota_v2_repo_policy", prefix: ["uos_ai", "kernel_quota", "v2", "repo_policy"] },
  { group: "kernel_quota_v2_org_policy", prefix: ["uos_ai", "kernel_quota", "v2", "org_policy"] },
  { group: "kernel_quota_v2_repo_window", prefix: ["uos_ai", "kernel_quota", "v2", "repo_window"] },
  { group: "kernel_quota_v2_org_window", prefix: ["uos_ai", "kernel_quota", "v2", "org_window"] },
  // Kept importable until the two-phase incident migration has replayed old
  // isolate increments into the split V2 window records.
  { group: "kernel_limits_legacy", prefix: ["ubq_ai", "kernel_auth", "limits"] },
  { group: "kernel_org_usage", prefix: ["ubq_ai", "kernel_auth", "org_usage"] },
  { group: "kernel_org_usage_daily", prefix: ["ubq_ai", "kernel_auth", "org_usage_daily"] },
  { group: "kernel_org_limits_legacy", prefix: ["ubq_ai", "kernel_auth", "org_limits"] },
  { group: "defaults", prefix: ["default"] },
  { group: "kernel_pubkeys", prefix: ["uos_ai", "kernel_pubkeys"] },
  { group: "voyage_api_key", prefix: ["uos_ai", "voyage_api_key"] },
  { group: "codex_prompts", prefix: ["uos_ai", "codex_instructions"] },
  { group: "codex_prompts_chunks", prefix: ["uos_ai", "codex_instructions_chunk"] },
  { group: "kernel_policy_queue", prefix: ["uos_ai", "kernel_policy_queue"] },
  { group: "migrations", prefix: ["uos_ai", "migrations"] },
  {
    group: "embeddings_idempotency_responses",
    prefix: ["embeddings", "idempotency", "v1", "response"],
  },
  { group: "embeddings_idempotency", prefix: ["embeddings", "idempotency", "v1"] },
  { group: "passkey_users", prefix: ["uos_ai", "auth", "users"] },
  { group: "passkey_handles", prefix: ["uos_ai", "auth", "handles"] },
  { group: "passkey_credentials", prefix: ["uos_ai", "auth", "credentials"] },
  { group: "agent_messages", prefix: ["agent_messages"] },
];

const CODEX_BOOTSTRAP_PREFIXES: Array<{ group: string; prefix: Deno.KvKey }> = [
  { group: "codex_auth", prefix: ["ubq_ai", "codex_auth"] },
  { group: "codex_models", prefix: ["ubq_ai", "codex_models"] },
];

const LEGACY_DURABLE_PREFIXES: Array<{ group: string; prefix: Deno.KvKey }> = [
  { group: "legacy_model_key_config", prefix: ["key", "config"] },
  { group: "legacy_model_key_health", prefix: ["key", "health"] },
];

const TRANSIENT_PREFIXES: Array<{ group: string; prefix: Deno.KvKey }> = [
  { group: "passkey_challenges", prefix: ["uos_ai", "auth", "challenges"] },
  { group: "passkey_sessions", prefix: ["uos_ai", "auth", "sessions"] },
  { group: "embeddings_rate", prefix: ["embeddings", "v1", "rate"] },
  { group: "embeddings_jobs", prefix: ["embeddings", "jobs"] },
];

const EMBEDDINGS_CACHE_PREFIXES: Array<{ group: string; prefix: Deno.KvKey }> = [
  { group: "embeddings_cache_index", prefix: ["embeddings", "v2", "cache_index"] },
  { group: "embeddings_cache_index_by_hash", prefix: ["embeddings", "v2", "cache_index_by_hash"] },
  { group: "embeddings_cache_values", prefix: ["embeddings", "v2"] },
];

const keyStartsWith = (key: Deno.KvKey, prefix: Deno.KvKey): boolean => {
  if (key.length < prefix.length) return false;
  return prefix.every((part, index) => key[index] === part);
};

const findPrefix = (key: Deno.KvKey, prefixes: Array<{ group: string; prefix: Deno.KvKey }>) =>
  prefixes.find((entry) => keyStartsWith(key, entry.prefix)) ?? null;

export const defaultIncludeLegacyForProfile = (profile: KvMigrationProfile): boolean => profile === "local";

export const classifyKvMigrationKey = (
  key: Deno.KvKey,
  options: KvMigrationClassifyOptions,
): KvMigrationDecision => {
  const transient = findPrefix(key, TRANSIENT_PREFIXES);
  if (transient) return { action: "skip", group: transient.group, reason: "transient_runtime_state" };

  const codex = findPrefix(key, CODEX_BOOTSTRAP_PREFIXES);
  if (codex) {
    if (options.profile === "local") {
      return { action: "import", group: codex.group, reason: "local_replay" };
    }
    return { action: "skip", group: codex.group, reason: "refreshed_by_deploy_bootstrap" };
  }

  const durable = findPrefix(key, DURABLE_PREFIXES);
  if (durable) return { action: "import", group: durable.group, reason: "durable" };

  const legacyDurable = findPrefix(key, LEGACY_DURABLE_PREFIXES);
  if (legacyDurable) {
    if (options.includeLegacy) {
      return { action: "import", group: legacyDurable.group, reason: "legacy_durable" };
    }
    return { action: "skip", group: legacyDurable.group, reason: "legacy_skipped" };
  }

  const cache = findPrefix(key, EMBEDDINGS_CACHE_PREFIXES);
  if (cache) {
    if (options.includeCache) return { action: "import", group: cache.group, reason: "cache_requested" };
    return { action: "optional", group: cache.group, reason: "cache_skipped_by_default" };
  }

  return { action: "skip", group: "unknown", reason: "unknown_prefix" };
};

export const safeKvMigrationValueType = (valueJson: unknown): string => {
  if (!valueJson || typeof valueJson !== "object") return typeof valueJson;
  const type = (valueJson as { type?: unknown }).type;
  return typeof type === "string" ? type : "object";
};

export const parseKvMigrationEntryLine = (line: string): { key: Deno.KvKey; value: unknown; raw: KvEntryJSON } => {
  const raw = JSON.parse(line) as KvEntryJSON;
  return {
    key: toKey(raw.key),
    value: toValue(raw.value),
    raw,
  };
};

export async function importKvMigrationLines(
  kv: Deno.Kv | null,
  lines: AsyncIterable<string> | Iterable<string>,
  options: KvMigrationImportOptions,
): Promise<KvMigrationImportResult> {
  const counters: KvMigrationCounters = { total: 0, imported: 0, skipped: 0, optional: 0, errors: 0 };
  const byGroup = new Map<string, number>();

  for await (const line of lines) {
    if (!line.trim()) continue;
    counters.total += 1;
    try {
      const entry = parseKvMigrationEntryLine(line);
      const decision = classifyKvMigrationKey(entry.key, options);
      byGroup.set(decision.group, (byGroup.get(decision.group) ?? 0) + 1);
      if (decision.action === "skip") {
        counters.skipped += 1;
        continue;
      }
      if (decision.action === "optional") {
        counters.optional += 1;
        continue;
      }
      if (kv && !options.overwrite) {
        const existing = await kv.get(entry.key);
        if (existing.value !== null) {
          counters.skipped += 1;
          continue;
        }
        // Re-check the missing destination in the write transaction. A
        // read-then-set pair can otherwise import the same row twice when two
        // migration workers race with overwrite disabled.
        if (!options.dryRun) {
          const committed = await kv.atomic().check(existing).set(entry.key, entry.value).commit();
          if (!committed.ok) {
            counters.skipped += 1;
            continue;
          }
          counters.imported += 1;
          continue;
        }
      }
      if (!options.dryRun && kv) {
        await kv.set(entry.key, entry.value);
      }
      counters.imported += 1;
    } catch {
      counters.errors += 1;
    }
  }

  return {
    ...counters,
    groups: Object.fromEntries(Array.from(byGroup.entries()).sort((a, b) => b[1] - a[1])),
  };
}

export const listKvMigrationCount = async (
  kv: Deno.Kv,
  prefix: Deno.KvKey,
  limit = Number.POSITIVE_INFINITY,
): Promise<number> => {
  let count = 0;
  for await (const _entry of kv.list({ prefix })) {
    count += 1;
    if (count >= limit) break;
  }
  return count;
};

export const KV_READ_INCIDENT_V2_MIGRATION_KEY = ["uos_ai", "migrations", "kv_read_incident_v2"] as const;
const API_KEY_USAGE_V2_MIGRATION_BASELINE_PREFIX = [
  ...KV_READ_INCIDENT_V2_MIGRATION_KEY,
  "api_key_usage_baseline",
] as const;
const LEGACY_REQUEST_LOG_PREFIX = ["ubq_ai", "api_keys", "request_log"] as const;
const PAID_FALLBACK_LEDGER_PREFIX = ["uos_ai", "paid_fallback", "ledger"] as const;
const PAID_FALLBACK_V3_PREFIX = ["uos_ai", "paid_fallback", "v3"] as const;
const PAID_FALLBACK_WINDOW_V3_PREFIX = [...PAID_FALLBACK_V3_PREFIX, "window"] as const;
const PAID_FALLBACK_REQUEST_V3_PREFIX = [...PAID_FALLBACK_V3_PREFIX, "request"] as const;
const PAID_FALLBACK_PENDING_V3_PREFIX = [...PAID_FALLBACK_V3_PREFIX, "pending"] as const;
const PAID_FALLBACK_RECONCILIATION_LEASE_V3_PREFIX = [
  ...PAID_FALLBACK_V3_PREFIX,
  "reconciliation_lease",
] as const;
const PAID_FALLBACK_DELETION_GUARD_V3_PREFIX = [...PAID_FALLBACK_V3_PREFIX, "deletion_guard"] as const;
const isRoutableApiKeyPrefix = (value: unknown): boolean => typeof value === "string" && /^u_[0-9a-f]{10}$/.test(value);
const isSafeUsageCount = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;
const isApiKeyId = (value: unknown): value is string =>
  typeof value === "string" && value === value.trim() && value.length > 0 && value.length <= 200;
const isApiKeyHash = (value: unknown): value is string =>
  typeof value === "string" && /^[A-Za-z0-9_-]{43}$/.test(value);
const isExpirationTimestamp = (value: unknown): value is number => value === -1 || isSafeUsageCount(value);
const isRevocationTimestamp = (value: unknown): value is number | null => value === null || isSafeUsageCount(value);
const isUsageLimit = (value: unknown): value is number => value === -1 || isSafeUsageCount(value);
const isPositiveSafeInteger = (value: unknown): value is number => isSafeUsageCount(value) && value > 0;
const isFiniteNonNegativeNumber = (value: unknown): value is number =>
  typeof value === "number" && Number.isFinite(value) && value >= 0;
const isNullablePositiveSafeInteger = (value: unknown): value is number | null =>
  value === null || isPositiveSafeInteger(value);
const isNullableNonEmptyString = (value: unknown): value is string | null =>
  value === null || (typeof value === "string" && value.length > 0);

const hasPaidFallbackLedgerIdentity = (value: unknown): value is ApiKeyRequestLogRecord =>
  isRecord(value) &&
  isApiKeyId(value.key_id) &&
  isApiKeyId(value.id) &&
  isPositiveSafeInteger(value.created_at_ms) &&
  value.provider === "yunwu";

const isPendingPaidFallbackLedgerRecord = (value: unknown): value is ApiKeyRequestLogRecord =>
  hasPaidFallbackLedgerIdentity(value) &&
  (value.billing_status === "pending" || value.billing_status === "unresolved");

const paidFallbackLedgerReference = (keyId: string, requestId: string): string => JSON.stringify([keyId, requestId]);

const pendingPaidFallbackLedgerReferenceFromEntry = (
  entry: Pick<Deno.KvEntry<unknown>, "key" | "value">,
  prefix: Deno.KvKey,
): string | null => {
  if (entry.key.length !== prefix.length + 3 || !isPendingPaidFallbackLedgerRecord(entry.value)) return null;
  const [keyId, createdAtMs, requestId] = entry.key.slice(prefix.length);
  if (
    keyId !== entry.value.key_id || createdAtMs !== entry.value.created_at_ms ||
    requestId !== entry.value.id
  ) return null;
  return paidFallbackLedgerReference(entry.value.key_id, entry.value.id);
};

const paidFallbackLedgerEntryMatchesIdentity = (
  entry: Pick<Deno.KvEntry<unknown>, "key" | "value">,
  expected: Pick<ApiKeyRequestLogRecord, "key_id" | "id" | "created_at_ms">,
): boolean => {
  if (
    entry.key.length !== PAID_FALLBACK_LEDGER_PREFIX.length + 3 ||
    !hasPaidFallbackLedgerIdentity(entry.value)
  ) return false;
  const [keyId, createdAtMs, requestId] = entry.key.slice(PAID_FALLBACK_LEDGER_PREFIX.length);
  return keyId === expected.key_id && createdAtMs === expected.created_at_ms && requestId === expected.id &&
    entry.value.key_id === expected.key_id && entry.value.created_at_ms === expected.created_at_ms &&
    entry.value.id === expected.id;
};

const hasStrictApiKeyHashCorePolicy = (value: unknown): value is ApiKeyHashRecord => {
  if (!hasStrictPaidFallbackPolicy(value)) return false;
  const record = value as ApiKeyHashRecord;
  return isApiKeyId(record.id) &&
    isExpirationTimestamp(record.expires_at_ms) &&
    isRevocationTimestamp(record.revoked_at_ms) &&
    isUsageLimit(record.usage_limit_requests) &&
    isSafeUsageCount(record.usage_requests) &&
    isPositiveSafeInteger(record.usage_reset_at_ms) &&
    isPositiveSafeInteger(record.window_ms);
};

const hasStrictApiKeyCorePolicy = (value: unknown): value is ApiKeyRecord => {
  if (!hasStrictPaidFallbackKeyPolicy(value)) return false;
  const record = value as ApiKeyRecord;
  return isApiKeyId(record.id) &&
    isApiKeyHash(record.hash) &&
    isRoutableApiKeyPrefix(record.prefix) &&
    isExpirationTimestamp(record.expires_at_ms) &&
    isRevocationTimestamp(record.revoked_at_ms) &&
    isUsageLimit(record.usage_limit_requests) &&
    isSafeUsageCount(record.usage_requests) &&
    isPositiveSafeInteger(record.usage_reset_at_ms) &&
    isPositiveSafeInteger(record.window_ms);
};

const apiKeyHashPolicyMatches = (record: ApiKeyRecord, hashRecord: ApiKeyHashRecord): boolean =>
  record.id === hashRecord.id &&
  record.expires_at_ms === hashRecord.expires_at_ms &&
  record.revoked_at_ms === hashRecord.revoked_at_ms &&
  record.usage_limit_requests === hashRecord.usage_limit_requests &&
  record.usage_requests === hashRecord.usage_requests &&
  record.usage_reset_at_ms === hashRecord.usage_reset_at_ms &&
  record.window_ms === hashRecord.window_ms &&
  record.usage_quota_version === hashRecord.usage_quota_version &&
  record.paid_fallback_enabled === hashRecord.paid_fallback_enabled &&
  record.paid_fallback_limit_microcredits === hashRecord.paid_fallback_limit_microcredits &&
  record.paid_fallback_spent_microcredits === hashRecord.paid_fallback_spent_microcredits &&
  record.paid_fallback_reserved_microcredits === hashRecord.paid_fallback_reserved_microcredits &&
  record.paid_fallback_reservation_request_id === hashRecord.paid_fallback_reservation_request_id;

const isPaidFallbackWindowV3 = (value: unknown): value is PaidFallbackWindowV3 => {
  if (!isRecord(value)) return false;
  return value.v === 3 &&
    isApiKeyId(value.key_id) &&
    typeof value.policy_version === "string" &&
    value.policy_version.length > 0 &&
    isPositiveSafeInteger(value.window_reset_at_ms) &&
    (value.limit_microcredits === -1 || isPositiveSafeInteger(value.limit_microcredits)) &&
    isSafeUsageCount(value.settled_microcredits) &&
    isSafeUsageCount(value.reserved_microcredits) &&
    isSafeUsageCount(value.pending_count) &&
    isPositiveSafeInteger(value.updated_at_ms);
};

const isPaidFallbackRequestV3 = (value: unknown): value is PaidFallbackRequestV3 => {
  if (!isRecord(value)) return false;
  return value.v === 3 &&
    isApiKeyId(value.key_id) &&
    isApiKeyId(value.request_id) &&
    typeof value.policy_version === "string" &&
    value.policy_version.length > 0 &&
    typeof value.route === "string" &&
    value.route.length > 0 &&
    typeof value.path === "string" &&
    value.path.length > 0 &&
    typeof value.model === "string" &&
    value.model.length > 0 &&
    typeof value.stream === "boolean" &&
    isNullableNonEmptyString(value.reasoning) &&
    isPositiveSafeInteger(value.window_reset_at_ms) &&
    isSafeUsageCount(value.reserved_microcredits) &&
    isPositiveSafeInteger(value.quota_per_credit) &&
    isNullableNonEmptyString(value.provider_request_id) &&
    (value.provider_quota === null || isFiniteNonNegativeNumber(value.provider_quota)) &&
    (value.input_tokens === null || isSafeUsageCount(value.input_tokens)) &&
    (value.output_tokens === null || isSafeUsageCount(value.output_tokens)) &&
    ["reserved", "dispatched", "not_dispatched"].includes(String(value.dispatch_state)) &&
    ["pending", "completed", "failed", "incomplete", "cancelled", "ambiguous"].includes(
      String(value.terminal_state),
    ) &&
    (value.spend_microcredits === null || isSafeUsageCount(value.spend_microcredits)) &&
    ["pending", "settled", "not_billed", "unresolved"].includes(String(value.billing_state)) &&
    isSafeUsageCount(value.reconciliation_attempts) &&
    isNullablePositiveSafeInteger(value.last_reconciliation_at_ms) &&
    isNullablePositiveSafeInteger(value.dispatched_at_ms) &&
    isNullablePositiveSafeInteger(value.terminal_at_ms) &&
    isNullablePositiveSafeInteger(value.settled_at_ms) &&
    isPositiveSafeInteger(value.created_at_ms) &&
    isPositiveSafeInteger(value.updated_at_ms);
};

type PaidFallbackV3Inventory = Readonly<{
  windows: number;
  requests: number;
  pending: number;
  reconciliationLeases: number;
  deletionGuards: number;
  errors: string[];
}>;

const paidFallbackV3Reference = (keyId: string, requestId: string): string => JSON.stringify([keyId, requestId]);
const paidFallbackV3WindowReference = (keyId: string, windowResetAtMs: number): string =>
  JSON.stringify([keyId, windowResetAtMs]);

const inspectPaidFallbackV3 = async (
  kv: Deno.Kv,
  knownKeyIds: ReadonlySet<string>,
): Promise<PaidFallbackV3Inventory> => {
  const errors: string[] = [];
  const windows = new Map<string, PaidFallbackWindowV3>();
  const requests = new Map<string, PaidFallbackRequestV3>();
  const pending = new Set<string>();
  let windowCount = 0;
  let requestCount = 0;
  let pendingCount = 0;
  let reconciliationLeases = 0;
  let deletionGuards = 0;

  for await (const entry of kv.list<unknown>({ prefix: PAID_FALLBACK_WINDOW_V3_PREFIX })) {
    windowCount += 1;
    const [keyId, windowResetAtMs] = entry.key.slice(PAID_FALLBACK_WINDOW_V3_PREFIX.length);
    if (
      entry.key.length !== PAID_FALLBACK_WINDOW_V3_PREFIX.length + 2 ||
      !isApiKeyId(keyId) ||
      !isPositiveSafeInteger(windowResetAtMs) ||
      !isPaidFallbackWindowV3(entry.value) ||
      entry.value.key_id !== keyId ||
      entry.value.window_reset_at_ms !== windowResetAtMs
    ) {
      errors.push(`paid fallback V3 window is malformed: ${JSON.stringify(entry.key)}`);
      continue;
    }
    if (!knownKeyIds.has(keyId)) errors.push(`paid fallback V3 window is orphaned: ${keyId}`);
    windows.set(paidFallbackV3WindowReference(keyId, windowResetAtMs), entry.value);
  }

  for await (const entry of kv.list<unknown>({ prefix: PAID_FALLBACK_REQUEST_V3_PREFIX })) {
    requestCount += 1;
    const [keyId, requestId] = entry.key.slice(PAID_FALLBACK_REQUEST_V3_PREFIX.length);
    if (
      entry.key.length !== PAID_FALLBACK_REQUEST_V3_PREFIX.length + 2 ||
      !isApiKeyId(keyId) ||
      !isApiKeyId(requestId) ||
      !isPaidFallbackRequestV3(entry.value) ||
      entry.value.key_id !== keyId ||
      entry.value.request_id !== requestId
    ) {
      errors.push(`paid fallback V3 request is malformed: ${JSON.stringify(entry.key)}`);
      continue;
    }
    if (!knownKeyIds.has(keyId)) errors.push(`paid fallback V3 request is orphaned: ${keyId}/${requestId}`);
    requests.set(paidFallbackV3Reference(keyId, requestId), entry.value);
  }

  for await (const entry of kv.list<unknown>({ prefix: PAID_FALLBACK_PENDING_V3_PREFIX })) {
    pendingCount += 1;
    const [keyId, requestId] = entry.key.slice(PAID_FALLBACK_PENDING_V3_PREFIX.length);
    if (
      entry.key.length !== PAID_FALLBACK_PENDING_V3_PREFIX.length + 2 ||
      !isApiKeyId(keyId) ||
      !isApiKeyId(requestId) ||
      !isRecord(entry.value) ||
      !isPositiveSafeInteger(entry.value.created_at_ms) ||
      !isPositiveSafeInteger(entry.value.next_reconciliation_at_ms) ||
      entry.value.next_reconciliation_at_ms < entry.value.created_at_ms ||
      ("key_id" in entry.value && entry.value.key_id !== keyId) ||
      ("request_id" in entry.value && entry.value.request_id !== requestId)
    ) {
      errors.push(`paid fallback V3 pending marker is malformed: ${JSON.stringify(entry.key)}`);
      continue;
    }
    const reference = paidFallbackV3Reference(keyId, requestId);
    pending.add(reference);
  }

  for await (
    const entry of kv.list<unknown>({ prefix: PAID_FALLBACK_RECONCILIATION_LEASE_V3_PREFIX })
  ) {
    reconciliationLeases += 1;
    const [keyId] = entry.key.slice(PAID_FALLBACK_RECONCILIATION_LEASE_V3_PREFIX.length);
    if (
      entry.key.length !== PAID_FALLBACK_RECONCILIATION_LEASE_V3_PREFIX.length + 1 ||
      !isApiKeyId(keyId) ||
      !isRecord(entry.value) ||
      typeof entry.value.token !== "string" ||
      entry.value.token.length === 0 ||
      !isPositiveSafeInteger(entry.value.expires_at_ms)
    ) {
      errors.push(`paid fallback V3 reconciliation lease is malformed: ${JSON.stringify(entry.key)}`);
      continue;
    }
    if (!knownKeyIds.has(keyId)) errors.push(`paid fallback V3 reconciliation lease is orphaned: ${keyId}`);
  }

  for await (const entry of kv.list<unknown>({ prefix: PAID_FALLBACK_DELETION_GUARD_V3_PREFIX })) {
    deletionGuards += 1;
    const [keyId] = entry.key.slice(PAID_FALLBACK_DELETION_GUARD_V3_PREFIX.length);
    if (
      entry.key.length !== PAID_FALLBACK_DELETION_GUARD_V3_PREFIX.length + 1 ||
      !isApiKeyId(keyId) ||
      !isRecord(entry.value) ||
      !isPositiveSafeInteger(entry.value.created_at_ms)
    ) {
      errors.push(`paid fallback V3 deletion guard is malformed: ${JSON.stringify(entry.key)}`);
    }
  }

  for (const [reference, request] of requests) {
    const isOutstanding = request.billing_state === "pending" || request.billing_state === "unresolved";
    if (isOutstanding && !pending.has(reference)) {
      errors.push(`paid fallback V3 request is missing its pending marker: ${request.key_id}/${request.request_id}`);
    }
    if (!isOutstanding && pending.has(reference)) {
      errors.push(
        `paid fallback V3 terminal request retains a pending marker: ${request.key_id}/${request.request_id}`,
      );
    }
    const window = windows.get(paidFallbackV3WindowReference(request.key_id, request.window_reset_at_ms));
    if (
      (request.reserved_microcredits > 0 ||
        (request.billing_state === "settled" && (request.spend_microcredits ?? 0) > 0)) &&
      !window
    ) {
      errors.push(`paid fallback V3 bounded request is missing its window: ${request.key_id}/${request.request_id}`);
    }
    if (request.billing_state === "settled" && request.spend_microcredits === null) {
      errors.push(`paid fallback V3 settled request is missing spend: ${request.key_id}/${request.request_id}`);
    }
    if (
      (request.billing_state === "pending" || request.billing_state === "unresolved") &&
      request.spend_microcredits !== null
    ) {
      errors.push(`paid fallback V3 outstanding request has spend: ${request.key_id}/${request.request_id}`);
    }
    // Historical requests retain the policy version that admitted them. A
    // window can carry a newer version after an admin policy edit.
  }
  for (const reference of pending) {
    if (!requests.has(reference)) errors.push(`paid fallback V3 pending marker is orphaned: ${reference}`);
  }

  for (const [reference, window] of windows) {
    const outstanding = [...requests.values()].filter((request) =>
      request.key_id === window.key_id &&
      request.window_reset_at_ms === window.window_reset_at_ms &&
      (request.billing_state === "pending" || request.billing_state === "unresolved")
    );
    const reservedMicrocredits = outstanding.reduce((sum, request) => sum + request.reserved_microcredits, 0);
    const settledRequests = [...requests.values()].filter((request) =>
      request.key_id === window.key_id &&
      request.window_reset_at_ms === window.window_reset_at_ms &&
      request.billing_state === "settled"
    );
    const settledMicrocredits = settledRequests.reduce((sum, request) => sum + (request.spend_microcredits ?? 0), 0);
    if (
      !Number.isSafeInteger(reservedMicrocredits) ||
      !Number.isSafeInteger(settledMicrocredits) ||
      window.pending_count !== outstanding.length ||
      window.reserved_microcredits !== reservedMicrocredits ||
      window.settled_microcredits !== settledMicrocredits
    ) {
      errors.push(`paid fallback V3 window aggregate is inconsistent: ${reference}`);
    }
  }

  return {
    windows: windowCount,
    requests: requestCount,
    pending: pendingCount,
    reconciliationLeases,
    deletionGuards,
    errors,
  };
};

type StrictApiKeyPair = Readonly<{
  record: ApiKeyRecord;
  hashRecord: ApiKeyHashRecord;
}>;

type ApiKeyUsageV2MigrationBaseline = Readonly<{
  version: 1;
  key_id: string;
  policy_version: string;
  window_start_ms: number;
  last_legacy_usage_requests: number;
  seeded_at_ms: number;
  reconciled_at_ms: number | null;
  reconciliation_runs: number;
}>;

const apiKeyUsageV2MigrationBaselineKey = (
  policy: Pick<ApiKeyPolicy, "key_id" | "policy_version" | "window_start_ms">,
) =>
  [
    ...API_KEY_USAGE_V2_MIGRATION_BASELINE_PREFIX,
    policy.key_id,
    policy.policy_version,
    policy.window_start_ms,
  ] as const;

const normalizeApiKeyUsageV2MigrationBaseline = (value: unknown): ApiKeyUsageV2MigrationBaseline | null => {
  if (!value || typeof value !== "object") return null;
  const record = value as Record<string, unknown>;
  if (
    record.version !== 1 || !isApiKeyId(record.key_id) || typeof record.policy_version !== "string" ||
    !record.policy_version || !isSafeUsageCount(record.window_start_ms) ||
    !isSafeUsageCount(record.last_legacy_usage_requests) || !isPositiveSafeInteger(record.seeded_at_ms) ||
    !(record.reconciled_at_ms === null || isPositiveSafeInteger(record.reconciled_at_ms)) ||
    !isSafeUsageCount(record.reconciliation_runs)
  ) return null;
  return record as ApiKeyUsageV2MigrationBaseline;
};

type BoundedCounterHandoffResult = Readonly<{
  baseline_created: boolean;
  baseline_reconciled: boolean;
  legacy_usage_delta_applied: number;
}>;

const currentLegacyUsage = (record: ApiKeyRecord, nowMs: number): number =>
  nowMs < record.usage_reset_at_ms ? record.usage_requests : 0;

// Revoked keys no longer advance their legacy window. Keep migration and
// validation pinned to the last stored window instead of rolling them forward.
const migrationPolicyNow = (record: ApiKeyRecord, nowMs: number): number =>
  record.revoked_at_ms === null ? nowMs : Math.max(0, record.usage_reset_at_ms - 1);

// The first V3 aggregate for a revoked historical window must remain available
// long enough for the post-migration validation pass.
const migrationV3RetentionMs = (windowResetAtMs: number, nowMs: number): number =>
  apiKeyUsageV3RetentionMs(windowResetAtMs, Math.min(nowMs, windowResetAtMs));

const migrateBoundedCounterHandoff = async (
  kv: Deno.Kv,
  policy: ApiKeyPolicy,
  legacyUsageRequests: number,
  nowMs: number,
  handoffAlreadyInitialized: boolean,
): Promise<BoundedCounterHandoffResult> => {
  const counterKey = apiKeyUsageV2Key(policy);
  const baselineKey = apiKeyUsageV2MigrationBaselineKey(policy);
  const windowKey = apiKeyUsageV3WindowKey(policy);

  for (let attempt = 0; attempt < 5; attempt += 1) {
    const [baselineEntry, counterEntry, windowEntry] = await Promise.all([
      kv.get<ApiKeyUsageV2MigrationBaseline>(baselineKey, { consistency: "strong" }),
      kv.get<Deno.KvU64>(counterKey, { consistency: "strong" }),
      kv.get<ApiKeyUsageWindowV3>(windowKey, { consistency: "strong" }),
    ]);
    if (counterEntry.value !== null && typeof counterEntry.value.value !== "bigint") {
      throw new Error(`Legacy V2 API-key counter is invalid: ${policy.key_id}`);
    }
    const observedV2Usage = counterEntry.value === null ? legacyUsageRequests : Number(counterEntry.value.value);
    if (!isSafeUsageCount(observedV2Usage)) {
      throw new Error(`Legacy V2 API-key counter is out of range: ${policy.key_id}`);
    }
    const existingWindow = normalizeApiKeyUsageWindowV3(windowEntry.value);
    if (
      existingWindow &&
      (existingWindow.key_id !== policy.key_id || existingWindow.policy_version !== policy.policy_version ||
        existingWindow.window_start_ms !== policy.window_start_ms ||
        existingWindow.window_reset_at_ms !== policy.usage_reset_at_ms)
    ) {
      throw new Error(`V3 API-key window identity is invalid: ${policy.key_id}`);
    }
    if (baselineEntry.value === null) {
      const baseline: ApiKeyUsageV2MigrationBaseline = {
        version: 1,
        key_id: policy.key_id,
        policy_version: policy.policy_version,
        window_start_ms: policy.window_start_ms,
        last_legacy_usage_requests: observedV2Usage,
        seeded_at_ms: nowMs,
        reconciled_at_ms: handoffAlreadyInitialized ? nowMs : null,
        reconciliation_runs: handoffAlreadyInitialized ? 1 : 0,
      };
      const seededWindow: ApiKeyUsageWindowV3 = {
        ...(existingWindow ?? makeApiKeyUsageWindowV3(policy, nowMs)),
        committed_requests: Math.max(existingWindow?.committed_requests ?? 0, observedV2Usage),
        updated_at_ms: nowMs,
      };
      const committed = await kv.atomic()
        .check(baselineEntry)
        .check(counterEntry)
        .check(windowEntry)
        .set(baselineKey, baseline)
        .set(windowKey, seededWindow, { expireIn: migrationV3RetentionMs(seededWindow.window_reset_at_ms, nowMs) })
        .commit();
      if (committed.ok) {
        return {
          baseline_created: true,
          baseline_reconciled: handoffAlreadyInitialized,
          legacy_usage_delta_applied: observedV2Usage,
        };
      }
      continue;
    }

    const baseline = normalizeApiKeyUsageV2MigrationBaseline(baselineEntry.value);
    if (
      !baseline || baseline.key_id !== policy.key_id || baseline.policy_version !== policy.policy_version ||
      baseline.window_start_ms !== policy.window_start_ms
    ) {
      throw new Error(`Bounded counter handoff baseline is invalid: ${policy.key_id}`);
    }
    if (!existingWindow) throw new Error(`V3 API-key window is missing: ${policy.key_id}`);
    const nextLegacyUsage = Math.max(baseline.last_legacy_usage_requests, observedV2Usage);
    const legacyDelta = nextLegacyUsage - baseline.last_legacy_usage_requests;
    const updatedBaseline: ApiKeyUsageV2MigrationBaseline = {
      ...baseline,
      last_legacy_usage_requests: nextLegacyUsage,
      reconciled_at_ms: handoffAlreadyInitialized ? nowMs : null,
      reconciliation_runs: handoffAlreadyInitialized ? baseline.reconciliation_runs + 1 : 0,
    };
    const updatedWindow: ApiKeyUsageWindowV3 = {
      ...existingWindow,
      committed_requests: existingWindow.committed_requests + legacyDelta,
      updated_at_ms: nowMs,
    };
    const atomic = kv.atomic()
      .check(baselineEntry)
      .check(counterEntry)
      .check(windowEntry)
      .set(baselineKey, updatedBaseline)
      .set(windowKey, updatedWindow, { expireIn: migrationV3RetentionMs(updatedWindow.window_reset_at_ms, nowMs) });
    const committed = await atomic.commit();
    if (committed.ok) {
      return {
        baseline_created: false,
        baseline_reconciled: handoffAlreadyInitialized,
        legacy_usage_delta_applied: legacyDelta,
      };
    }
  }
  throw new Error(`Bounded counter handoff changed concurrently: ${policy.key_id}`);
};

const countReconciledBoundedCounterBaselines = async (kv: Deno.Kv): Promise<number> => {
  let count = 0;
  for await (const entry of kv.list<unknown>({ prefix: API_KEY_USAGE_V2_MIGRATION_BASELINE_PREFIX })) {
    const baseline = normalizeApiKeyUsageV2MigrationBaseline(entry.value);
    if (baseline && baseline.reconciled_at_ms !== null) count += 1;
  }
  return count;
};

type LegacyKernelLimitRecord = Readonly<{
  usage_limit_requests: number;
  usage_requests: number;
  usage_reset_at_ms: number;
  window_ms: number;
  expires_at_ms: number;
  created_at_ms: number;
  updated_at_ms: number;
}>;

const legacyKernelNumber = (value: unknown, fallback: number): number => {
  if (typeof value === "string") value = Number(value);
  if (typeof value !== "number" || !Number.isFinite(value)) return fallback;
  return Math.trunc(value);
};

const normalizeLegacyKernelLimit = (
  value: unknown,
  defaults: { limit: number; windowMs: number },
  nowMs: number,
): LegacyKernelLimitRecord | null => {
  if (!isRecord(value)) return null;
  const limit = legacyKernelNumber(value.usage_limit_requests, defaults.limit);
  const usage = Math.max(0, legacyKernelNumber(value.usage_requests, 0));
  const windowMs = legacyKernelNumber(value.window_ms, defaults.windowMs);
  const resetAtMs = legacyKernelNumber(value.usage_reset_at_ms, nowMs + Math.max(1, windowMs));
  const expiresAtMs = legacyKernelNumber(value.expires_at_ms, -1);
  if (
    !(limit === -1 || limit >= 0) || windowMs <= 0 || resetAtMs <= 0 ||
    !(expiresAtMs === -1 || expiresAtMs >= 0)
  ) return null;
  return {
    usage_limit_requests: limit,
    usage_requests: usage,
    usage_reset_at_ms: resetAtMs,
    window_ms: windowMs,
    expires_at_ms: expiresAtMs,
    created_at_ms: Math.max(0, legacyKernelNumber(value.created_at_ms, nowMs)),
    updated_at_ms: Math.max(0, legacyKernelNumber(value.updated_at_ms, nowMs)),
  };
};

const migrationKernelDefaults = async (kv: Deno.Kv): Promise<{ limit: number; windowMs: number }> => {
  const [limitEntry, windowEntry] = await Promise.all([
    kv.get(DEFAULT_KERNEL_POLICY_LIMIT_KEY, { consistency: "strong" }),
    kv.get(DEFAULT_KERNEL_POLICY_WINDOW_KEY, { consistency: "strong" }),
  ]);
  const limit = legacyKernelNumber(limitEntry.value, DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS);
  const windowMs = legacyKernelNumber(windowEntry.value, DEFAULT_KERNEL_POLICY_WINDOW_MS);
  return {
    limit: limit === -1 || limit >= 0 ? limit : DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS,
    windowMs: windowMs > 0 ? windowMs : DEFAULT_KERNEL_POLICY_WINDOW_MS,
  };
};

const migrateLegacyKernelScope = async (
  kv: Deno.Kv,
  scope: "repo" | "org",
  defaults: { limit: number; windowMs: number },
  nowMs: number,
): Promise<number> => {
  const legacyPrefix = scope === "repo"
    ? ["ubq_ai", "kernel_auth", "limits"] as const
    : ["ubq_ai", "kernel_auth", "org_limits"] as const;
  let migrated = 0;
  for await (const legacyEntry of kv.list<unknown>({ prefix: legacyPrefix })) {
    const ownerPart = legacyEntry.key[legacyPrefix.length];
    const repoPart = legacyEntry.key[legacyPrefix.length + 1];
    if (
      typeof ownerPart !== "string" || !ownerPart || (scope === "repo" && (typeof repoPart !== "string" || !repoPart))
    ) {
      throw new Error(`legacy kernel quota key is malformed: ${JSON.stringify(legacyEntry.key)}`);
    }
    const owner = ownerPart;
    const repo = scope === "repo" ? repoPart as string : undefined;
    const legacy = normalizeLegacyKernelLimit(legacyEntry.value, defaults, nowMs);
    if (!legacy) throw new Error(`legacy kernel quota value is malformed: ${JSON.stringify(legacyEntry.key)}`);
    const defaultBacked = legacy.expires_at_ms === -1 &&
      legacy.usage_limit_requests === defaults.limit && legacy.window_ms === defaults.windowMs;
    const policyKey = scope === "repo" ? kernelRepoPolicyKey(owner, repo!) : kernelOrgPolicyKey(owner);
    const windowKey = scope === "repo" ? kernelRepoWindowKey(owner, repo!) : kernelOrgWindowKey(owner);
    const [policyEntry, windowEntry] = await Promise.all([
      kv.get<KernelQuotaPolicyV2>(policyKey, { consistency: "strong" }),
      kv.get<KernelQuotaWindowV2>(windowKey, { consistency: "strong" }),
    ]);
    if (normalizeKernelQuotaPolicyV2(policyEntry.value, scope, owner, repo)) {
      const committed = await kv.atomic()
        .check(legacyEntry)
        .check(policyEntry)
        .check(windowEntry)
        .delete(legacyEntry.key)
        .commit();
      if (!committed.ok) throw new Error(`legacy kernel quota changed concurrently: ${owner}/${repo ?? ""}`);
      migrated += 1;
      continue;
    }
    const effectiveWindowMs = defaultBacked ? defaults.windowMs : legacy.window_ms;
    const currentWindow = normalizeKernelQuotaWindowV2(windowEntry.value, scope, owner, repo);
    const sameWindow = currentWindow?.applied_window_ms === effectiveWindowMs &&
      currentWindow.usage_reset_at_ms === legacy.usage_reset_at_ms;
    const window: KernelQuotaWindowV2 = sameWindow
      ? {
        ...currentWindow,
        usage_requests: Math.max(currentWindow.usage_requests, legacy.usage_requests),
        updated_at_ms: nowMs,
      }
      : {
        v: 2,
        scope,
        owner,
        ...(scope === "repo" ? { repo } : {}),
        usage_requests: legacy.usage_reset_at_ms <= nowMs ? 0 : legacy.usage_requests,
        usage_reset_at_ms: legacy.usage_reset_at_ms <= nowMs ? nowMs + effectiveWindowMs : legacy.usage_reset_at_ms,
        applied_window_ms: effectiveWindowMs,
        created_at_ms: legacy.created_at_ms,
        updated_at_ms: nowMs,
      };
    const policy: KernelQuotaPolicyV2 = {
      v: 2,
      scope,
      owner,
      ...(scope === "repo" ? { repo } : {}),
      usage_limit_requests: legacy.usage_limit_requests,
      window_ms: legacy.window_ms,
      expires_at_ms: legacy.expires_at_ms,
      created_at_ms: legacy.created_at_ms,
      updated_at_ms: nowMs,
    };
    let atomic = kv.atomic()
      .check(legacyEntry)
      .check(policyEntry)
      .check(windowEntry)
      .set(windowKey, window)
      .delete(legacyEntry.key);
    atomic = defaultBacked ? atomic.delete(policyKey) : atomic.set(policyKey, policy);
    const committed = await atomic.commit();
    if (!committed.ok) throw new Error(`legacy kernel quota changed concurrently: ${owner}/${repo ?? ""}`);
    migrated += 1;
  }
  return migrated;
};

const migrateKernelQuotaV2 = async (kv: Deno.Kv, nowMs: number): Promise<{ repo: number; org: number }> => {
  const defaults = await migrationKernelDefaults(kv);
  return {
    repo: await migrateLegacyKernelScope(kv, "repo", defaults, nowMs),
    org: await migrateLegacyKernelScope(kv, "org", defaults, nowMs),
  };
};

type ApiKeyUsageV3Inventory = Readonly<{
  windows: number;
  requests: number;
  errors: string[];
}>;

const apiKeyUsageV3WindowReference = (keyId: string, policyVersion: string, windowStartMs: number): string =>
  JSON.stringify([keyId, policyVersion, windowStartMs]);

const inspectApiKeyUsageV3 = async (
  kv: Deno.Kv,
  knownKeyIds: ReadonlySet<string>,
): Promise<ApiKeyUsageV3Inventory> => {
  const errors: string[] = [];
  const windows = new Map<string, ApiKeyUsageWindowV3>();
  const reservedByWindow = new Map<string, number>();
  const requestWindows = new Set<string>();
  let windowCount = 0;
  let requestCount = 0;
  for await (const entry of kv.list<unknown>({ prefix: API_KEY_USAGE_V3_WINDOW_PREFIX })) {
    windowCount += 1;
    const [keyId, policyVersion, windowStartMs] = entry.key.slice(API_KEY_USAGE_V3_WINDOW_PREFIX.length);
    const window = normalizeApiKeyUsageWindowV3(entry.value);
    if (
      entry.key.length !== API_KEY_USAGE_V3_WINDOW_PREFIX.length + 3 || !isApiKeyId(keyId) ||
      typeof policyVersion !== "string" || !policyVersion || !isSafeUsageCount(windowStartMs) || !window ||
      window.key_id !== keyId || window.policy_version !== policyVersion || window.window_start_ms !== windowStartMs
    ) {
      errors.push(`API key usage V3 window is malformed: ${JSON.stringify(entry.key)}`);
      continue;
    }
    if (!knownKeyIds.has(keyId)) errors.push(`API key usage V3 window is orphaned: ${keyId}`);
    windows.set(apiKeyUsageV3WindowReference(keyId, policyVersion, windowStartMs), window);
  }
  for await (const entry of kv.list<unknown>({ prefix: API_KEY_USAGE_V3_REQUEST_PREFIX })) {
    requestCount += 1;
    const [keyId, policyVersion, windowStartMs, requestId] = entry.key.slice(API_KEY_USAGE_V3_REQUEST_PREFIX.length);
    const request = normalizeApiKeyUsageRequestV3(entry.value);
    if (
      entry.key.length !== API_KEY_USAGE_V3_REQUEST_PREFIX.length + 4 || !isApiKeyId(keyId) ||
      typeof policyVersion !== "string" || !policyVersion || !isSafeUsageCount(windowStartMs) ||
      !isApiKeyId(requestId) || !request || request.key_id !== keyId || request.request_id !== requestId
    ) {
      errors.push(`API key usage V3 request is malformed: ${JSON.stringify(entry.key)}`);
      continue;
    }
    if (!knownKeyIds.has(keyId)) errors.push(`API key usage V3 request is orphaned: ${keyId}/${requestId}`);
    const reference = apiKeyUsageV3WindowReference(keyId, policyVersion, windowStartMs);
    requestWindows.add(reference);
    if (request.state === "reserved") {
      reservedByWindow.set(reference, (reservedByWindow.get(reference) ?? 0) + 1);
    }
    if (request.state === "dispatched" && (request.provider === null || request.dispatched_at_ms === null)) {
      errors.push(`API key usage V3 dispatched request is incomplete: ${keyId}/${requestId}`);
    }
    if (request.state === "released" && request.released_at_ms === null) {
      errors.push(`API key usage V3 released request is incomplete: ${keyId}/${requestId}`);
    }
  }
  for (const [reference, window] of windows) {
    const reserved = reservedByWindow.get(reference) ?? 0;
    if (window.reserved_requests !== reserved) {
      errors.push(`API key usage V3 reserved aggregate is inconsistent: ${reference}`);
    }
  }
  for (const reference of requestWindows) {
    if (!windows.has(reference)) errors.push(`API key usage V3 request has no window: ${reference}`);
  }
  return { windows: windowCount, requests: requestCount, errors };
};

type KernelQuotaV2Inventory = Readonly<{
  repoPolicies: number;
  orgPolicies: number;
  repoWindows: number;
  orgWindows: number;
  errors: string[];
}>;

const inspectKernelQuotaV2 = async (kv: Deno.Kv): Promise<KernelQuotaV2Inventory> => {
  const errors: string[] = [];
  const inspect = async (
    prefix: Deno.KvKey,
    scope: "repo" | "org",
    kind: "policy" | "window",
  ): Promise<number> => {
    let count = 0;
    for await (const entry of kv.list<unknown>({ prefix })) {
      count += 1;
      const owner = entry.key[prefix.length];
      const repo = entry.key[prefix.length + 1];
      const validKey = typeof owner === "string" && owner &&
        (scope === "org"
          ? entry.key.length === prefix.length + 1
          : typeof repo === "string" && repo && entry.key.length === prefix.length + 2);
      const validValue = validKey &&
        (kind === "policy"
          ? normalizeKernelQuotaPolicyV2(entry.value, scope, owner, scope === "repo" ? repo as string : undefined)
          : normalizeKernelQuotaWindowV2(entry.value, scope, owner, scope === "repo" ? repo as string : undefined));
      if (!validValue) errors.push(`kernel quota V2 ${kind} is malformed: ${JSON.stringify(entry.key)}`);
    }
    return count;
  };
  const [repoPolicies, orgPolicies, repoWindows, orgWindows] = await Promise.all([
    inspect(KERNEL_REPO_POLICY_V2_PREFIX, "repo", "policy"),
    inspect(KERNEL_ORG_POLICY_V2_PREFIX, "org", "policy"),
    inspect(KERNEL_REPO_WINDOW_V2_PREFIX, "repo", "window"),
    inspect(KERNEL_ORG_WINDOW_V2_PREFIX, "org", "window"),
  ]);
  return { repoPolicies, orgPolicies, repoWindows, orgWindows, errors };
};

const inspectStrictApiKeyPairs = async (
  kv: Deno.Kv,
): Promise<{ pairs: StrictApiKeyPair[]; errors: string[] }> => {
  const errors: string[] = [];
  const hashEntries = new Map<string, ApiKeyHashRecord>();
  for await (const entry of kv.list<unknown>({ prefix: API_KEY_HASH_PREFIX })) {
    const hash = entry.key.length === API_KEY_HASH_PREFIX.length + 1 ? entry.key.at(-1) : null;
    if (!isApiKeyHash(hash)) {
      errors.push(`api key hash entry has an invalid key: ${JSON.stringify(entry.key)}`);
      continue;
    }
    if (!hasStrictApiKeyHashCorePolicy(entry.value)) {
      errors.push(`api key hash policy has invalid core fields: ${hash}`);
      continue;
    }
    hashEntries.set(hash, entry.value);
  }

  const pairs: StrictApiKeyPair[] = [];
  const referencedHashes = new Map<string, number>();
  for await (const entry of kv.list<unknown>({ prefix: API_KEY_ID_PREFIX })) {
    const keyId = entry.key.length === API_KEY_ID_PREFIX.length + 1 ? entry.key.at(-1) : null;
    if (!isApiKeyId(keyId)) {
      errors.push(`api key id entry has an invalid key: ${JSON.stringify(entry.key)}`);
      continue;
    }
    if (!hasStrictApiKeyCorePolicy(entry.value)) {
      errors.push(`api key has invalid v2 policy: ${keyId}`);
      continue;
    }
    const record = entry.value;
    if (record.id !== keyId) {
      errors.push(`api key id key suffix does not match record id: key=${keyId} record=${record.id}`);
      continue;
    }
    const hashRecord = hashEntries.get(record.hash);
    if (!hashRecord || !apiKeyHashPolicyMatches(record, hashRecord)) {
      errors.push(`api key hash policy is missing or inconsistent: ${record.id}`);
      continue;
    }
    referencedHashes.set(record.hash, (referencedHashes.get(record.hash) ?? 0) + 1);
    pairs.push({ record, hashRecord });
  }

  for (const hash of hashEntries.keys()) {
    const references = referencedHashes.get(hash) ?? 0;
    if (references === 0) errors.push(`api key hash policy is orphaned: ${hash}`);
    if (references > 1) errors.push(`api key hash policy has multiple id records: ${hash}`);
  }
  return { pairs, errors };
};

type LegacyPaidFallbackLedgerCandidate = Readonly<{
  value: ApiKeyRequestLogRecord;
  ledgerKey: Deno.KvKey;
  existing: Deno.KvEntryMaybe<ApiKeyRequestLogRecord>;
}>;

type LegacyPaidFallbackProjectionCandidate = Readonly<{
  value: ApiKeyRequestLogRecord;
  source_key: Deno.KvKey;
}>;

type LegacyPaidFallbackProjectionResult = Readonly<{
  projected: number;
  pending: number;
}>;

const LEGACY_PAID_FALLBACK_BILLING_STATES = new Set([
  "pending",
  "reconciled",
  "not_billed",
  "unresolved",
]);

const legacyPaidFallbackReference = (keyId: string, requestId: string): string => JSON.stringify([keyId, requestId]);

const legacyPaidFallbackCandidate = (
  entry: Pick<Deno.KvEntry<unknown>, "key" | "value">,
): LegacyPaidFallbackProjectionCandidate | null => {
  if (!isRecord(entry.value) || entry.value.provider !== "yunwu") return null;
  const keyId = isApiKeyId(entry.value.key_id) ? entry.value.key_id : null;
  const keySuffix = entry.key.at(-2);
  const requestSuffix = entry.key.at(-1);
  const requestId = isApiKeyId(entry.value.id) ? entry.value.id : isApiKeyId(requestSuffix) ? requestSuffix : null;
  const createdAtMs = isPositiveSafeInteger(entry.value.created_at_ms)
    ? entry.value.created_at_ms
    : typeof keySuffix === "number" && isPositiveSafeInteger(keySuffix)
    ? keySuffix
    : null;
  if (!keyId || !requestId || createdAtMs === null) return null;
  const billing = entry.value.billing_status;
  if (billing !== undefined && !LEGACY_PAID_FALLBACK_BILLING_STATES.has(String(billing))) return null;
  return {
    source_key: entry.key,
    value: {
      id: requestId,
      key_id: keyId,
      route: typeof entry.value.route === "string" && entry.value.route.trim() ? entry.value.route : "responses",
      path: typeof entry.value.path === "string" && entry.value.path.trim() ? entry.value.path : "/v1/responses",
      method: typeof entry.value.method === "string" && entry.value.method.trim() ? entry.value.method : "POST",
      status_code: typeof entry.value.status_code === "number" && Number.isFinite(entry.value.status_code)
        ? Math.max(0, Math.min(599, Math.trunc(entry.value.status_code)))
        : 0,
      stream: entry.value.stream === true,
      model: typeof entry.value.model === "string" && entry.value.model.trim() ? entry.value.model : "legacy-unknown",
      reasoning: typeof entry.value.reasoning === "string" && entry.value.reasoning.trim()
        ? entry.value.reasoning
        : null,
      created_at_ms: createdAtMs,
      provider: "yunwu",
      fallback_reason: typeof entry.value.fallback_reason === "string" ? entry.value.fallback_reason : "primary_429",
      provider_request_id: typeof entry.value.provider_request_id === "string" &&
          entry.value.provider_request_id.trim()
        ? entry.value.provider_request_id
        : null,
      completed_at_ms: isPositiveSafeInteger(entry.value.completed_at_ms) ? entry.value.completed_at_ms : null,
      latency_ms: isSafeUsageCount(entry.value.latency_ms) ? entry.value.latency_ms : null,
      input_tokens: isSafeUsageCount(entry.value.input_tokens) ? entry.value.input_tokens : null,
      output_tokens: isSafeUsageCount(entry.value.output_tokens) ? entry.value.output_tokens : null,
      provider_quota: isFiniteNonNegativeNumber(entry.value.provider_quota) ? entry.value.provider_quota : null,
      quota_per_credit: isPositiveSafeInteger(entry.value.quota_per_credit) ? entry.value.quota_per_credit : null,
      spend_microcredits: isSafeUsageCount(entry.value.spend_microcredits) ? entry.value.spend_microcredits : null,
      paid_fallback_window_reset_at_ms: isPositiveSafeInteger(entry.value.paid_fallback_window_reset_at_ms)
        ? entry.value.paid_fallback_window_reset_at_ms
        : null,
      billing_status: billing === "reconciled" || billing === "not_billed" || billing === "unresolved"
        ? billing
        : "pending",
    },
  };
};

const listLegacyPaidFallbackProjectionCandidates = async (
  kv: Deno.Kv,
): Promise<LegacyPaidFallbackProjectionCandidate[]> => {
  const byReference = new Map<string, LegacyPaidFallbackProjectionCandidate>();
  for (const prefix of [LEGACY_REQUEST_LOG_PREFIX, PAID_FALLBACK_LEDGER_PREFIX] as const) {
    for await (const entry of kv.list<unknown>({ prefix })) {
      const candidate = legacyPaidFallbackCandidate(entry);
      if (!candidate) continue;
      // The dedicated paid-fallback ledger is the newer copy of a request log.
      // Prefer it when both prefixes contain the same immutable request.
      const reference = legacyPaidFallbackReference(candidate.value.key_id, candidate.value.id);
      if (prefix === PAID_FALLBACK_LEDGER_PREFIX || !byReference.has(reference)) {
        byReference.set(reference, candidate);
      }
    }
  }
  return [...byReference.values()];
};

const legacyWindowResetAtMs = (
  record: ApiKeyRecord,
  request: ApiKeyRequestLogRecord,
): number => {
  if (request.paid_fallback_window_reset_at_ms !== null) return request.paid_fallback_window_reset_at_ms;
  if (request.created_at_ms < record.usage_reset_at_ms) return record.usage_reset_at_ms;
  const initialStart = record.usage_reset_at_ms - record.window_ms;
  const elapsed = Math.floor((request.created_at_ms - initialStart) / record.window_ms);
  return initialStart + (elapsed + 1) * record.window_ms;
};

const legacyPolicyVersion = (
  record: ApiKeyRecord,
  _request: ApiKeyRequestLogRecord,
  windowResetAtMs: number,
): string => `legacy:${windowResetAtMs}:${record.window_ms}:${record.paid_fallback_pricing_checked_at_ms ?? 0}`;

const legacyMaximumExposure = (
  record: ApiKeyRecord,
  request: ApiKeyRequestLogRecord,
): number => {
  if (record.paid_fallback_limit_microcredits === -1) return 0;
  if (
    record.paid_fallback_reservation_request_id === request.id &&
    isSafeUsageCount(record.paid_fallback_reserved_microcredits)
  ) {
    // The legacy policy stores the one live reservation's exact exposure.
    // Prefer it over today's model policy, which may have changed since the
    // request was admitted.
    return record.paid_fallback_reserved_microcredits;
  }
  const configured = record.paid_fallback_max_exposure_microcredits
    ? record.paid_fallback_max_exposure_microcredits[request.model ?? ""]
    : 0;
  if (isPositiveSafeInteger(configured)) return configured;
  return 0;
};

const paidFallbackV3WindowKey = (keyId: string, windowResetAtMs: number): Deno.KvKey => [
  ...PAID_FALLBACK_WINDOW_V3_PREFIX,
  keyId,
  windowResetAtMs,
];

const isPaidFallbackPendingV3 = (value: unknown): value is {
  created_at_ms: number;
  next_reconciliation_at_ms: number;
} =>
  isRecord(value) &&
  isPositiveSafeInteger(value.created_at_ms) &&
  isPositiveSafeInteger(value.next_reconciliation_at_ms) &&
  value.next_reconciliation_at_ms >= value.created_at_ms;

const paidFallbackPendingIdentityMatches = (
  value: unknown,
  keyId: string,
  requestId: string,
): boolean => {
  if (!isPaidFallbackPendingV3(value)) return false;
  const record = value as Record<string, unknown>;
  return (!("key_id" in record) || record.key_id === keyId) &&
    (!("request_id" in record) || record.request_id === requestId);
};

const repairProjectedPaidFallbackPending = async (kv: Deno.Kv, nowMs: number): Promise<void> => {
  for await (const requestEntry of kv.list<unknown>({ prefix: PAID_FALLBACK_REQUEST_V3_PREFIX })) {
    if (!isPaidFallbackRequestV3(requestEntry.value)) continue;
    const request = requestEntry.value;
    const pendingKey = [...PAID_FALLBACK_PENDING_V3_PREFIX, request.key_id, request.request_id] as const;
    const pendingEntry = await kv.get<unknown>(pendingKey, { consistency: "strong" });
    const outstanding = request.billing_state === "pending" || request.billing_state === "unresolved";
    if (
      outstanding &&
      pendingEntry.value !== null &&
      paidFallbackPendingIdentityMatches(pendingEntry.value, request.key_id, request.request_id) &&
      (pendingEntry.value as { created_at_ms: number }).created_at_ms >= request.created_at_ms
    ) {
      continue;
    }
    if (!outstanding && pendingEntry.value === null) continue;
    let atomic = kv.atomic().check(requestEntry).check(pendingEntry);
    if (outstanding) {
      atomic = atomic.set(pendingKey, {
        created_at_ms: request.created_at_ms,
        next_reconciliation_at_ms: Math.max(nowMs, request.created_at_ms),
      });
    } else {
      atomic = atomic.delete(pendingKey);
    }
    const committed = await atomic.commit();
    if (!committed.ok) {
      throw new Error(`Paid fallback V3 pending state changed concurrently: ${request.key_id}/${request.request_id}`);
    }
  }
};

const repairProjectedPaidFallbackWindows = async (
  kv: Deno.Kv,
  pairs: readonly StrictApiKeyPair[],
  candidateWindowReferences: ReadonlySet<string>,
  nowMs: number,
): Promise<void> => {
  const recordsByKey = new Map(pairs.map(({ record }) => [record.id, record]));
  const windows = new Map<string, { key_id: string; window_reset_at_ms: number }>();
  for (const reference of candidateWindowReferences) {
    const parsed = JSON.parse(reference) as unknown;
    if (
      !Array.isArray(parsed) ||
      parsed.length !== 2 ||
      !isApiKeyId(parsed[0]) ||
      !isPositiveSafeInteger(parsed[1])
    ) {
      continue;
    }
    const record = recordsByKey.get(parsed[0]);
    if (record && record.paid_fallback_limit_microcredits !== -1) {
      windows.set(reference, { key_id: parsed[0], window_reset_at_ms: parsed[1] });
    }
  }
  for await (const entry of kv.list<unknown>({ prefix: PAID_FALLBACK_WINDOW_V3_PREFIX })) {
    const [keyId, windowResetAtMs] = entry.key.slice(PAID_FALLBACK_WINDOW_V3_PREFIX.length);
    if (
      isApiKeyId(keyId) &&
      isPositiveSafeInteger(windowResetAtMs) &&
      recordsByKey.get(keyId)?.paid_fallback_limit_microcredits !== -1
    ) {
      windows.set(paidFallbackV3WindowReference(keyId, windowResetAtMs), {
        key_id: keyId,
        window_reset_at_ms: windowResetAtMs,
      });
    }
  }
  for await (const entry of kv.list<unknown>({ prefix: PAID_FALLBACK_REQUEST_V3_PREFIX })) {
    if (!isPaidFallbackRequestV3(entry.value)) continue;
    if (recordsByKey.get(entry.value.key_id)?.paid_fallback_limit_microcredits === -1) continue;
    windows.set(paidFallbackV3WindowReference(entry.value.key_id, entry.value.window_reset_at_ms), {
      key_id: entry.value.key_id,
      window_reset_at_ms: entry.value.window_reset_at_ms,
    });
  }

  for (const { key_id: keyId, window_reset_at_ms: windowResetAtMs } of windows.values()) {
    const windowKey = paidFallbackV3WindowKey(keyId, windowResetAtMs);
    for (let attempt = 0; attempt < 5; attempt += 1) {
      const requests: PaidFallbackRequestV3[] = [];
      for await (const requestEntry of kv.list<unknown>({ prefix: PAID_FALLBACK_REQUEST_V3_PREFIX })) {
        if (
          isPaidFallbackRequestV3(requestEntry.value) &&
          requestEntry.value.key_id === keyId &&
          requestEntry.value.window_reset_at_ms === windowResetAtMs
        ) {
          requests.push(requestEntry.value);
        }
      }
      const settledMicrocredits = requests.reduce(
        (sum, request) => sum + (request.billing_state === "settled" ? request.spend_microcredits ?? 0 : 0),
        0,
      );
      const reservedMicrocredits = requests.reduce(
        (sum, request) =>
          sum +
          (request.billing_state === "pending" || request.billing_state === "unresolved"
            ? request.reserved_microcredits
            : 0),
        0,
      );
      const pendingCount = requests.reduce(
        (count, request) =>
          count + (request.billing_state === "pending" || request.billing_state === "unresolved" ? 1 : 0),
        0,
      );
      if (
        !Number.isSafeInteger(settledMicrocredits) ||
        !Number.isSafeInteger(reservedMicrocredits) ||
        !Number.isSafeInteger(pendingCount)
      ) {
        throw new Error(`Paid fallback V3 window aggregate overflow: ${keyId}/${windowResetAtMs}`);
      }
      const entry = await kv.get<PaidFallbackWindowV3>(windowKey, { consistency: "strong" });
      if (entry.value !== null && !isPaidFallbackWindowV3(entry.value)) {
        throw new Error(`Paid fallback V3 window is invalid: ${keyId}/${windowResetAtMs}`);
      }
      const firstRequest = requests[0];
      const current = entry.value ?? {
        v: 3,
        key_id: keyId,
        policy_version: firstRequest?.policy_version ??
          `legacy:${windowResetAtMs}:${recordsByKey.get(keyId)?.window_ms ?? 0}:0`,
        window_reset_at_ms: windowResetAtMs,
        limit_microcredits: recordsByKey.get(keyId)?.paid_fallback_limit_microcredits ?? 0,
        settled_microcredits: 0,
        reserved_microcredits: 0,
        pending_count: 0,
        updated_at_ms: nowMs,
      } satisfies PaidFallbackWindowV3;
      if (
        entry.value &&
        current.settled_microcredits === settledMicrocredits &&
        current.reserved_microcredits === reservedMicrocredits &&
        current.pending_count === pendingCount
      ) {
        break;
      }
      const next = {
        ...current,
        settled_microcredits: settledMicrocredits,
        reserved_microcredits: reservedMicrocredits,
        pending_count: pendingCount,
        updated_at_ms: Math.max(nowMs, current.updated_at_ms),
      } satisfies PaidFallbackWindowV3;
      const committed = await kv.atomic().check(entry).set(windowKey, next).commit();
      if (committed.ok) break;
      if (attempt === 4) {
        throw new Error(`Paid fallback V3 window changed concurrently: ${keyId}/${windowResetAtMs}`);
      }
    }
  }
};

const projectLegacyPaidFallbackV3 = async (
  kv: Deno.Kv,
  pairs: readonly StrictApiKeyPair[],
  nowMs: number,
): Promise<LegacyPaidFallbackProjectionResult> => {
  const recordsByKey = new Map(pairs.map(({ record }) => [record.id, record]));
  const candidates = await listLegacyPaidFallbackProjectionCandidates(kv);
  let projected = 0;
  let pending = 0;
  const candidateWindowReferences = new Set<string>();
  for (const candidate of candidates) {
    const legacy = candidate.value;
    const record = recordsByKey.get(legacy.key_id);
    if (!record) continue;
    const requestKey = [...PAID_FALLBACK_V3_PREFIX, "request", legacy.key_id, legacy.id] as const;
    const pendingKey = [...PAID_FALLBACK_V3_PREFIX, "pending", legacy.key_id, legacy.id] as const;
    const windowResetAtMs = legacyWindowResetAtMs(record, legacy);
    if (record.paid_fallback_limit_microcredits !== -1) {
      candidateWindowReferences.add(paidFallbackV3WindowReference(legacy.key_id, windowResetAtMs));
    }
    const billingState: PaidFallbackRequestV3["billing_state"] = legacy.billing_status === "reconciled"
      ? "settled"
      : legacy.billing_status === "not_billed"
      ? "not_billed"
      : legacy.billing_status === "unresolved"
      ? "unresolved"
      : "pending";
    const outstanding = billingState === "pending" || billingState === "unresolved";
    // Older request rows sometimes predate the exact spend fields. Preserve
    // their terminal identity in V3 with a zero projection; unresolved rows
    // retain their reservation and are reconciled from provider logs.
    const spend = billingState === "settled" ? legacy.spend_microcredits ?? 0 : null;
    const quotaPerCredit = legacy.quota_per_credit ?? record.paid_fallback_quota_per_credit;
    if (!isPositiveSafeInteger(quotaPerCredit)) {
      throw new Error(`Legacy paid fallback request has no valid pricing: ${legacy.key_id}/${legacy.id}`);
    }
    const dispatchState = legacy.provider_request_id || legacy.status_code > 0 ? "dispatched" : "reserved";
    const terminalState = billingState === "settled"
      ? legacy.status_code >= 200 && legacy.status_code < 300 ? "completed" : "failed"
      : billingState === "not_billed"
      ? "cancelled"
      : billingState === "unresolved"
      ? "ambiguous"
      : "pending";
    const updatedAtMs = Math.max(
      legacy.created_at_ms,
      legacy.completed_at_ms ?? 0,
      nowMs,
    );
    const request: PaidFallbackRequestV3 = {
      v: 3,
      key_id: legacy.key_id,
      request_id: legacy.id,
      policy_version: legacyPolicyVersion(record, legacy, windowResetAtMs),
      route: legacy.route,
      path: legacy.path,
      model: legacy.model ?? "legacy-unknown",
      stream: legacy.stream,
      reasoning: legacy.reasoning,
      window_reset_at_ms: windowResetAtMs,
      reserved_microcredits: outstanding ? legacyMaximumExposure(record, legacy) : 0,
      quota_per_credit: quotaPerCredit,
      provider_request_id: legacy.provider_request_id,
      provider_quota: legacy.provider_quota,
      input_tokens: legacy.input_tokens,
      output_tokens: legacy.output_tokens,
      dispatch_state: dispatchState,
      terminal_state: terminalState,
      spend_microcredits: spend,
      billing_state: billingState,
      reconciliation_attempts: 0,
      last_reconciliation_at_ms: null,
      dispatched_at_ms: dispatchState === "dispatched" ? legacy.created_at_ms : null,
      terminal_at_ms: terminalState === "pending" ? null : legacy.completed_at_ms ?? legacy.created_at_ms,
      settled_at_ms: billingState === "settled" ? legacy.completed_at_ms ?? legacy.created_at_ms : null,
      created_at_ms: legacy.created_at_ms,
      updated_at_ms: updatedAtMs,
    };
    const [requestEntry, pendingEntry] = await Promise.all([
      kv.get<PaidFallbackRequestV3>(requestKey, { consistency: "strong" }),
      kv.get<{ created_at_ms: number; next_reconciliation_at_ms: number }>(pendingKey, { consistency: "strong" }),
    ]);
    const existingRequest = requestEntry.value;
    if (existingRequest) {
      if (
        !isPaidFallbackRequestV3(existingRequest) ||
        existingRequest.key_id !== request.key_id ||
        existingRequest.request_id !== request.request_id
      ) {
        throw new Error(`V3 paid fallback request identity collision: ${legacy.key_id}/${legacy.id}`);
      }
    }
    const effectiveRequest = existingRequest ?? request;
    let atomic = kv.atomic().check(requestEntry).check(pendingEntry);
    let needsCommit = false;
    if (!existingRequest) {
      atomic = atomic.set(requestKey, request);
      projected += 1;
      needsCommit = true;
    }
    const effectiveOutstanding = effectiveRequest.billing_state === "pending" ||
      effectiveRequest.billing_state === "unresolved";
    if (effectiveOutstanding) {
      atomic = atomic.set(pendingKey, {
        created_at_ms: effectiveRequest.created_at_ms,
        next_reconciliation_at_ms: Math.max(nowMs, effectiveRequest.created_at_ms),
      });
      if (pendingEntry.value === null) {
        pending += 1;
        needsCommit = true;
      } else if (!paidFallbackPendingIdentityMatches(pendingEntry.value, legacy.key_id, legacy.id)) {
        needsCommit = true;
      }
    } else if (pendingEntry.value !== null) {
      atomic = atomic.delete(pendingKey);
      needsCommit = true;
    }
    if (needsCommit) {
      const committed = await atomic.commit();
      if (!committed.ok) {
        throw new Error(`Paid fallback V3 migration changed concurrently: ${legacy.key_id}/${legacy.id}`);
      }
    }
  }
  await repairProjectedPaidFallbackPending(kv, nowMs);
  await repairProjectedPaidFallbackWindows(kv, pairs, candidateWindowReferences, nowMs);
  return { projected, pending };
};

const inspectPendingPaidFallbackLedgers = async (
  kv: Deno.Kv,
  pairs: StrictApiKeyPair[],
): Promise<{ candidates: LegacyPaidFallbackLedgerCandidate[]; errors: string[] }> => {
  const errors: string[] = [];
  const availableReferences = new Set<string>();
  for await (const entry of kv.list<unknown>({ prefix: PAID_FALLBACK_LEDGER_PREFIX })) {
    const value = entry.value;
    const pending = isRecord(value) && value.provider === "yunwu" &&
      (value.billing_status === "pending" || value.billing_status === "unresolved");
    if (!pending) continue;
    const reference = pendingPaidFallbackLedgerReferenceFromEntry(entry, PAID_FALLBACK_LEDGER_PREFIX);
    if (reference === null) {
      errors.push(`pending paid fallback ledger is malformed: ${JSON.stringify(entry.key)}`);
      continue;
    }
    availableReferences.add(reference);
  }

  const candidates: LegacyPaidFallbackLedgerCandidate[] = [];
  for await (const entry of kv.list<unknown>({ prefix: LEGACY_REQUEST_LOG_PREFIX })) {
    const value = entry.value;
    const pending = isRecord(value) && value.provider === "yunwu" &&
      (value.billing_status === "pending" || value.billing_status === "unresolved");
    if (!pending) continue;
    const reference = pendingPaidFallbackLedgerReferenceFromEntry(entry, LEGACY_REQUEST_LOG_PREFIX);
    if (reference === null || !isPendingPaidFallbackLedgerRecord(value)) {
      errors.push(`pending legacy paid fallback ledger is malformed: ${JSON.stringify(entry.key)}`);
      continue;
    }
    const ledgerKey = [
      ...PAID_FALLBACK_LEDGER_PREFIX,
      value.key_id,
      value.created_at_ms,
      value.id,
    ] as const;
    const existing = await kv.get<ApiKeyRequestLogRecord>(ledgerKey);
    candidates.push({ value, ledgerKey, existing });
    if (existing.value === null) {
      availableReferences.add(reference);
      continue;
    }
    if (!paidFallbackLedgerEntryMatchesIdentity(existing, value)) {
      errors.push(`paid fallback ledger destination conflicts with legacy record: ${value.id}`);
      continue;
    }
    const existingReference = pendingPaidFallbackLedgerReferenceFromEntry(existing, PAID_FALLBACK_LEDGER_PREFIX);
    if (existingReference === reference) availableReferences.add(reference);
  }

  for (const { record } of pairs) {
    const requestId = record.paid_fallback_reservation_request_id;
    if (requestId === null) continue;
    if (!availableReferences.has(paidFallbackLedgerReference(record.id, requestId))) {
      errors.push(`paid fallback reservation has no pending ledger record: ${record.id}`);
    }
  }
  return { candidates, errors };
};

export type KvReadIncidentV2MigrationResult = Readonly<{
  api_keys: number;
  bounded_counters: number;
  handoff_phase: "no_bounded_keys" | "predeploy_seed" | "postdeploy_reconcile" | "mixed";
  bounded_baselines_created: number;
  bounded_baselines_reconciled: number;
  legacy_usage_delta_applied: number;
  kernel_repo_records: number;
  kernel_org_records: number;
  paid_fallback_records: number;
  runtime_config_written: boolean;
}>;

export const migrateKvReadIncidentV2 = async (kv: Deno.Kv): Promise<KvReadIncidentV2MigrationResult> => {
  const migrationNowMs = Date.now();
  const previousMigration = await kv.get<{
    counter_handoff_version?: unknown;
    api_key_quota_v3_handoff_version?: unknown;
  }>(
    KV_READ_INCIDENT_V2_MIGRATION_KEY,
    { consistency: "strong" },
  );
  const handoffAlreadyInitialized = previousMigration.value?.api_key_quota_v3_handoff_version === 1;
  const codexModels = await kv.get<Record<string, unknown>>(["ubq_ai", "codex_models"]);
  if (!codexModels.value) throw new Error("Codex model snapshot is missing");
  const defaultModel = await kv.get<string>(["default", "model"]);
  const defaultReasoning = await kv.get<string>(["default", "reasoning_effort"]);
  const runtimeConfig = buildRuntimeConfig(codexModels.value as never, {
    defaultModel: defaultModel.value,
    defaultReasoningEffort: defaultReasoning.value,
  });

  const apiKeyInventory = await inspectStrictApiKeyPairs(kv);
  if (apiKeyInventory.errors.length) {
    throw new Error(`API key policy validation failed: ${apiKeyInventory.errors.join("; ")}`);
  }
  const paidFallbackInventory = await inspectPendingPaidFallbackLedgers(kv, apiKeyInventory.pairs);
  if (paidFallbackInventory.errors.length) {
    throw new Error(`Paid fallback ledger validation failed: ${paidFallbackInventory.errors.join("; ")}`);
  }

  let apiKeys = 0;
  let boundedCounters = 0;
  let boundedBaselinesCreated = 0;
  let boundedBaselinesReconciled = 0;
  let legacyUsageDeltaApplied = 0;
  const upgradedApiKeyPairs: Array<{ record: ApiKeyRecord; hashRecord: ApiKeyHashRecord }> = [];
  for (const { record } of apiKeyInventory.pairs) {
    const [idEntry, hashEntry] = await Promise.all([
      kv.get<ApiKeyRecord>(apiKeyIdKey(record.id), { consistency: "strong" }),
      kv.get<ApiKeyHashRecord>(apiKeyHashKey(record.hash), { consistency: "strong" }),
    ]);
    if (!idEntry.value || !hashEntry.value) throw new Error(`API key changed during quota V3 migration: ${record.id}`);
    if (idEntry.value.hash !== record.hash || !apiKeyHashPolicyMatches(idEntry.value, hashEntry.value)) {
      throw new Error(`API key changed during quota V3 migration: ${record.id}`);
    }
    const upgradedRecord: ApiKeyRecord = { ...idEntry.value, usage_quota_version: 3 };
    const upgradedHash: ApiKeyHashRecord = { ...hashEntry.value, usage_quota_version: 3 };
    if (idEntry.value.usage_quota_version !== 3 || hashEntry.value.usage_quota_version !== 3) {
      const upgraded = await kv.atomic()
        .check(idEntry)
        .check(hashEntry)
        .set(apiKeyIdKey(record.id), upgradedRecord)
        .set(apiKeyHashKey(record.hash), upgradedHash)
        .commit();
      if (!upgraded.ok) throw new Error(`API key changed during quota V3 migration: ${record.id}`);
    }
    const policy = apiKeyPolicyFromHashRecord(
      upgradedRecord.hash,
      upgradedHash,
      migrationPolicyNow(upgradedRecord, migrationNowMs),
    );
    if (!policy) throw new Error(`API key ${record.id} policy could not be normalized`);
    apiKeys += 1;
    const handoff = await migrateBoundedCounterHandoff(
      kv,
      policy,
      currentLegacyUsage(upgradedRecord, migrationNowMs),
      migrationNowMs,
      handoffAlreadyInitialized,
    );
    if (policy.usage_limit_requests !== -1) {
      if (handoff.baseline_created) boundedBaselinesCreated += 1;
      if (handoff.baseline_reconciled) boundedBaselinesReconciled += 1;
      boundedCounters += 1;
    }
    legacyUsageDeltaApplied += handoff.legacy_usage_delta_applied;
    upgradedApiKeyPairs.push({ record: upgradedRecord, hashRecord: upgradedHash });
  }

  const handoffPhase = boundedCounters === 0
    ? "no_bounded_keys"
    : boundedBaselinesReconciled === boundedCounters
    ? "postdeploy_reconcile"
    : boundedBaselinesReconciled === 0
    ? "predeploy_seed"
    : "mixed";

  let paidFallbackRecords = 0;
  for (const { value, ledgerKey, existing } of paidFallbackInventory.candidates) {
    if (existing.value === null) {
      const commit = await kv.atomic().check(existing).set(ledgerKey, value).commit();
      if (!commit.ok) {
        const concurrent = await kv.get<ApiKeyRequestLogRecord>(ledgerKey);
        if (concurrent.value === null) {
          throw new Error(`Paid fallback ledger changed during migration: ${value.id}`);
        }
      }
    }
    paidFallbackRecords += 1;
  }
  const kernelMigration = await migrateKernelQuotaV2(kv, migrationNowMs);
  await projectLegacyPaidFallbackV3(
    kv,
    upgradedApiKeyPairs,
    migrationNowMs,
  );

  await kv.atomic()
    .set(RUNTIME_CONFIG_V2_KEY, runtimeConfig)
    .set(KV_READ_INCIDENT_V2_MIGRATION_KEY, {
      version: 2,
      counter_handoff_version: 1,
      api_key_quota_v3_handoff_version: 1,
      completed_at_ms: Date.now(),
      api_keys: apiKeys,
      bounded_counters: boundedCounters,
      handoff_phase: handoffPhase,
      bounded_baselines_created: boundedBaselinesCreated,
      bounded_baselines_reconciled: boundedBaselinesReconciled,
      legacy_usage_delta_applied: legacyUsageDeltaApplied,
      kernel_repo_records: kernelMigration.repo,
      kernel_org_records: kernelMigration.org,
      paid_fallback_records: paidFallbackRecords,
    })
    .commit();
  return {
    api_keys: apiKeys,
    bounded_counters: boundedCounters,
    handoff_phase: handoffPhase,
    bounded_baselines_created: boundedBaselinesCreated,
    bounded_baselines_reconciled: boundedBaselinesReconciled,
    legacy_usage_delta_applied: legacyUsageDeltaApplied,
    kernel_repo_records: kernelMigration.repo,
    kernel_org_records: kernelMigration.org,
    paid_fallback_records: paidFallbackRecords,
    runtime_config_written: true,
  };
};

export const validateKvMigrationTarget = async (kv: Deno.Kv): Promise<KvMigrationValidationResult> => {
  const errors: string[] = [];
  const [
    apiIds,
    apiHashes,
    boundedCounters,
    boundedCounterBaselines,
    reconciledBoundedCounterBaselines,
    paidFallbackLedger,
    kernelLimits,
    kernelOrgLimits,
    apiKeyUsageV3Windows,
    apiKeyUsageV3Requests,
    kernelV2RepoPolicies,
    kernelV2OrgPolicies,
    kernelV2RepoWindows,
    kernelV2OrgWindows,
    passkeyUsers,
    passkeyCredentials,
    agentMessages,
    embeddingCache,
    legacyModelKeyConfigs,
    legacyModelKeyHealth,
  ] = await Promise.all([
    listKvMigrationCount(kv, ["ubq_ai", "api_keys", "id"]),
    listKvMigrationCount(kv, ["ubq_ai", "api_keys", "hash"]),
    listKvMigrationCount(kv, ["uos_ai", "api_key_usage", "v2"]),
    listKvMigrationCount(kv, API_KEY_USAGE_V2_MIGRATION_BASELINE_PREFIX),
    countReconciledBoundedCounterBaselines(kv),
    listKvMigrationCount(kv, ["uos_ai", "paid_fallback", "ledger"]),
    listKvMigrationCount(kv, ["ubq_ai", "kernel_auth", "limits"]),
    listKvMigrationCount(kv, ["ubq_ai", "kernel_auth", "org_limits"]),
    listKvMigrationCount(kv, API_KEY_USAGE_V3_WINDOW_PREFIX),
    listKvMigrationCount(kv, API_KEY_USAGE_V3_REQUEST_PREFIX),
    listKvMigrationCount(kv, KERNEL_REPO_POLICY_V2_PREFIX),
    listKvMigrationCount(kv, KERNEL_ORG_POLICY_V2_PREFIX),
    listKvMigrationCount(kv, KERNEL_REPO_WINDOW_V2_PREFIX),
    listKvMigrationCount(kv, KERNEL_ORG_WINDOW_V2_PREFIX),
    listKvMigrationCount(kv, ["uos_ai", "auth", "users"]),
    listKvMigrationCount(kv, ["uos_ai", "auth", "credentials"]),
    listKvMigrationCount(kv, ["agent_messages"]),
    listKvMigrationCount(kv, ["embeddings", "v2"], 10_000),
    listKvMigrationCount(kv, ["key", "config"]),
    listKvMigrationCount(kv, ["key", "health"]),
  ]);

  if (apiIds !== apiHashes) {
    errors.push(`api key id/hash count mismatch: ids=${apiIds} hashes=${apiHashes}`);
  }

  const apiKeyInventory = await inspectStrictApiKeyPairs(kv);
  errors.push(...apiKeyInventory.errors);
  const apiKeyUsageV3 = await inspectApiKeyUsageV3(
    kv,
    new Set(apiKeyInventory.pairs.map(({ record }) => record.id)),
  );
  errors.push(...apiKeyUsageV3.errors);
  const kernelQuotaV2 = await inspectKernelQuotaV2(kv);
  errors.push(...kernelQuotaV2.errors);
  if (kernelLimits > 0 || kernelOrgLimits > 0) {
    errors.push(
      `legacy combined kernel quota records remain: repo=${kernelLimits} org=${kernelOrgLimits}`,
    );
  }
  const paidFallbackV3 = await inspectPaidFallbackV3(
    kv,
    new Set(apiKeyInventory.pairs.map(({ record }) => record.id)),
  );
  errors.push(...paidFallbackV3.errors);
  const validationNowMs = Date.now();
  for (const { record, hashRecord } of apiKeyInventory.pairs) {
    if (record.paid_fallback_reservation_request_id) {
      let reservationFound = false;
      for await (
        const ledger of kv.list<unknown>({ prefix: [...PAID_FALLBACK_LEDGER_PREFIX, record.id] })
      ) {
        const reference = pendingPaidFallbackLedgerReferenceFromEntry(ledger, PAID_FALLBACK_LEDGER_PREFIX);
        if (
          reference === paidFallbackLedgerReference(record.id, record.paid_fallback_reservation_request_id)
        ) {
          reservationFound = true;
          break;
        }
      }
      if (!reservationFound) errors.push(`paid fallback reservation has no pending ledger record: ${record.id}`);
    }
    if (record.usage_quota_version !== 3 || hashRecord.usage_quota_version !== 3) {
      errors.push(`api key quota ledger version is not V3: ${record.id}`);
      continue;
    }
    const policy = apiKeyPolicyFromHashRecord(record.hash, hashRecord, migrationPolicyNow(record, validationNowMs));
    if (!policy) {
      errors.push(`api key policy could not be normalized: ${record.id}`);
      continue;
    }
    if (record.usage_limit_requests !== -1) {
      const v3Window = normalizeApiKeyUsageWindowV3(
        (await kv.get<ApiKeyUsageWindowV3>(apiKeyUsageV3WindowKey(policy))).value,
      );
      if (
        !v3Window || v3Window.key_id !== policy.key_id || v3Window.policy_version !== policy.policy_version ||
        v3Window.window_start_ms !== policy.window_start_ms || v3Window.window_reset_at_ms !== policy.usage_reset_at_ms
      ) {
        errors.push(`bounded V3 aggregate is missing or invalid: ${record.id}`);
      }
      const baseline = normalizeApiKeyUsageV2MigrationBaseline(
        (await kv.get<ApiKeyUsageV2MigrationBaseline>(apiKeyUsageV2MigrationBaselineKey(policy))).value,
      );
      if (
        !baseline || baseline.key_id !== policy.key_id || baseline.policy_version !== policy.policy_version ||
        baseline.window_start_ms !== policy.window_start_ms
      ) {
        errors.push(`bounded counter migration baseline is missing, stale, or invalid: ${record.id}`);
      }
    }
  }
  const codexModels = await kv.get<Record<string, unknown>>(["ubq_ai", "codex_models"]);
  const defaultModel = await kv.get<string>(["default", "model"]);
  const modelList = Array.isArray(codexModels.value?.models) ? codexModels.value.models : [];
  if (defaultModel.value && codexModels.value !== null && modelList.length === 0) {
    errors.push(`codex model snapshot is empty or malformed; cannot validate default model: ${defaultModel.value}`);
  }
  if (defaultModel.value && modelList.length) {
    const found = modelList.some((model) => {
      if (!model || typeof model !== "object") return false;
      const record = model as Record<string, unknown>;
      return [record.slug, record.id, record.model, record.name].includes(defaultModel.value);
    });
    if (!found) errors.push(`default model is not present in codex model snapshot: ${defaultModel.value}`);
  }

  const knownSettings = await Promise.all([
    kv.get(["ubq_ai", "codex_auth"]),
    kv.get(["ubq_ai", "codex_models"]),
    kv.get(["default", "model"]),
    kv.get(["default", "reasoning_effort"]),
    kv.get(["default", "kernel_policy_limit_requests"]),
    kv.get(["default", "kernel_policy_window_ms"]),
    kv.get(["uos_ai", "voyage_api_key"]),
    kv.get(["uos_ai", "kernel_pubkeys"]),
    kv.get(RUNTIME_CONFIG_V2_KEY),
  ]);
  if (normalizeRuntimeConfig(knownSettings[8].value) === null) {
    errors.push("runtime config v2 is missing or invalid");
  }

  return {
    counts: {
      api_key_ids: apiIds,
      api_key_hashes: apiHashes,
      api_key_bounded_counters_v2: boundedCounters,
      api_key_bounded_counter_baselines_v2: boundedCounterBaselines,
      api_key_bounded_counter_reconciled_baselines_v2: reconciledBoundedCounterBaselines,
      api_key_usage_v3_windows: apiKeyUsageV3Windows,
      api_key_usage_v3_requests: apiKeyUsageV3Requests,
      paid_fallback_ledger: paidFallbackLedger,
      paid_fallback_v3_windows: paidFallbackV3.windows,
      paid_fallback_v3_requests: paidFallbackV3.requests,
      paid_fallback_v3_pending: paidFallbackV3.pending,
      paid_fallback_v3_reconciliation_leases: paidFallbackV3.reconciliationLeases,
      paid_fallback_v3_deletion_guards: paidFallbackV3.deletionGuards,
      kernel_repo_limits: kernelLimits,
      kernel_org_limits: kernelOrgLimits,
      kernel_v2_repo_policies: kernelV2RepoPolicies,
      kernel_v2_org_policies: kernelV2OrgPolicies,
      kernel_v2_repo_windows: kernelV2RepoWindows,
      kernel_v2_org_windows: kernelV2OrgWindows,
      passkey_users: passkeyUsers,
      passkey_credentials: passkeyCredentials,
      agent_messages: agentMessages,
      embeddings_v2_at_most_10000: embeddingCache,
      legacy_model_key_configs: legacyModelKeyConfigs,
      legacy_model_key_health: legacyModelKeyHealth,
    },
    settings_present: {
      codex_auth: knownSettings[0].value !== null,
      codex_models: knownSettings[1].value !== null,
      default_model: knownSettings[2].value !== null,
      default_reasoning_effort: knownSettings[3].value !== null,
      default_kernel_policy_limit_requests: knownSettings[4].value !== null,
      default_kernel_policy_window_ms: knownSettings[5].value !== null,
      voyage_api_key: knownSettings[6].value !== null,
      kernel_pubkeys: knownSettings[7].value !== null,
      runtime_config_v2: normalizeRuntimeConfig(knownSettings[8].value) !== null,
    },
    errors,
  };
};
