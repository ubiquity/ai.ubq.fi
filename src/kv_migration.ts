import { type KvEntryJSON, toKey, toValue } from "@deno/kv-utils/json";
import { API_KEY_HASH_PREFIX, API_KEY_ID_PREFIX } from "./api_keys.ts";
import {
  API_KEY_USAGE_V2_PREFIX,
  type ApiKeyPolicy,
  apiKeyPolicyFromHashRecord,
  apiKeyUsageV2Key,
} from "./api_key_policy.ts";
import { buildRuntimeConfig, normalizeRuntimeConfig, RUNTIME_CONFIG_V2_KEY } from "./runtime_config.ts";
import type { ApiKeyHashRecord, ApiKeyRecord, ApiKeyRequestLogRecord } from "./types.ts";
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
    paid_fallback_ledger: number;
    kernel_repo_limits: number;
    kernel_org_limits: number;
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
  { group: "paid_fallback_ledger", prefix: ["uos_ai", "paid_fallback", "ledger"] },
  { group: "runtime_config_v2", prefix: ["uos_ai", "runtime_config", "v2"] },
  { group: "kernel_usage", prefix: ["ubq_ai", "kernel_auth", "usage"] },
  { group: "kernel_usage_daily", prefix: ["ubq_ai", "kernel_auth", "usage_daily"] },
  { group: "kernel_limits", prefix: ["ubq_ai", "kernel_auth", "limits"] },
  { group: "kernel_org_usage", prefix: ["ubq_ai", "kernel_auth", "org_usage"] },
  { group: "kernel_org_usage_daily", prefix: ["ubq_ai", "kernel_auth", "org_usage_daily"] },
  { group: "kernel_org_limits", prefix: ["ubq_ai", "kernel_auth", "org_limits"] },
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
  record.paid_fallback_enabled === hashRecord.paid_fallback_enabled &&
  record.paid_fallback_limit_microcredits === hashRecord.paid_fallback_limit_microcredits &&
  record.paid_fallback_spent_microcredits === hashRecord.paid_fallback_spent_microcredits &&
  record.paid_fallback_reserved_microcredits === hashRecord.paid_fallback_reserved_microcredits &&
  record.paid_fallback_reservation_request_id === hashRecord.paid_fallback_reservation_request_id;

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

const migrateBoundedCounterHandoff = async (
  kv: Deno.Kv,
  policy: ApiKeyPolicy,
  legacyUsageRequests: number,
  nowMs: number,
  handoffAlreadyInitialized: boolean,
): Promise<BoundedCounterHandoffResult> => {
  const counterKey = apiKeyUsageV2Key(policy);
  const baselineKey = apiKeyUsageV2MigrationBaselineKey(policy);

  for (let attempt = 0; attempt < 5; attempt += 1) {
    const baselineEntry = await kv.get<ApiKeyUsageV2MigrationBaseline>(baselineKey, { consistency: "strong" });
    if (baselineEntry.value === null) {
      const counterEntry = await kv.get<Deno.KvU64>(counterKey, { consistency: "strong" });
      if (handoffAlreadyInitialized) {
        if (counterEntry.value !== null && typeof counterEntry.value.value !== "bigint") {
          throw new Error(`Bounded counter handoff counter is invalid: ${policy.key_id}`);
        }
        const baseline: ApiKeyUsageV2MigrationBaseline = {
          version: 1,
          key_id: policy.key_id,
          policy_version: policy.policy_version,
          window_start_ms: policy.window_start_ms,
          last_legacy_usage_requests: legacyUsageRequests,
          seeded_at_ms: nowMs,
          reconciled_at_ms: nowMs,
          reconciliation_runs: 1,
        };
        const committed = await kv.atomic()
          .check(baselineEntry)
          .sum(counterKey, BigInt(legacyUsageRequests))
          .set(baselineKey, baseline)
          .commit();
        if (committed.ok) {
          return {
            baseline_created: true,
            baseline_reconciled: true,
            legacy_usage_delta_applied: legacyUsageRequests,
          };
        }
        continue;
      }
      const baseline: ApiKeyUsageV2MigrationBaseline = {
        version: 1,
        key_id: policy.key_id,
        policy_version: policy.policy_version,
        window_start_ms: policy.window_start_ms,
        last_legacy_usage_requests: legacyUsageRequests,
        seeded_at_ms: nowMs,
        reconciled_at_ms: null,
        reconciliation_runs: 0,
      };
      const committed = await kv.atomic()
        .check(baselineEntry)
        .check(counterEntry)
        .set(counterKey, new Deno.KvU64(BigInt(legacyUsageRequests)))
        .set(baselineKey, baseline)
        .commit();
      if (committed.ok) {
        return { baseline_created: true, baseline_reconciled: false, legacy_usage_delta_applied: 0 };
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
    const counterEntry = await kv.get<Deno.KvU64>(counterKey, { consistency: "strong" });
    if (!counterEntry.value || typeof counterEntry.value.value !== "bigint") {
      throw new Error(`Bounded counter handoff counter is missing or invalid: ${policy.key_id}`);
    }
    const nextLegacyUsage = Math.max(baseline.last_legacy_usage_requests, legacyUsageRequests);
    const legacyDelta = nextLegacyUsage - baseline.last_legacy_usage_requests;
    const updatedBaseline: ApiKeyUsageV2MigrationBaseline = {
      ...baseline,
      last_legacy_usage_requests: nextLegacyUsage,
      reconciled_at_ms: handoffAlreadyInitialized ? nowMs : null,
      reconciliation_runs: handoffAlreadyInitialized ? baseline.reconciliation_runs + 1 : 0,
    };
    let atomic = kv.atomic().check(baselineEntry).set(baselineKey, updatedBaseline);
    if (legacyDelta > 0) atomic = atomic.sum(counterKey, BigInt(legacyDelta));
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

const deleteStaleKeys = async (kv: Deno.Kv, prefix: Deno.KvKey, currentKey?: Deno.KvKey): Promise<void> => {
  const current = currentKey ? JSON.stringify(currentKey) : null;
  const stale: Deno.KvKey[] = [];
  for await (const entry of kv.list({ prefix })) {
    if (current === null || JSON.stringify(entry.key) !== current) stale.push(entry.key);
  }
  for (const key of stale) await kv.delete(key);
};

const countReconciledBoundedCounterBaselines = async (kv: Deno.Kv): Promise<number> => {
  let count = 0;
  for await (const entry of kv.list<unknown>({ prefix: API_KEY_USAGE_V2_MIGRATION_BASELINE_PREFIX })) {
    const baseline = normalizeApiKeyUsageV2MigrationBaseline(entry.value);
    if (baseline && baseline.reconciled_at_ms !== null) count += 1;
  }
  return count;
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
  paid_fallback_records: number;
  runtime_config_written: boolean;
}>;

export const migrateKvReadIncidentV2 = async (kv: Deno.Kv): Promise<KvReadIncidentV2MigrationResult> => {
  const migrationNowMs = Date.now();
  const previousMigration = await kv.get<{ counter_handoff_version?: unknown }>(
    KV_READ_INCIDENT_V2_MIGRATION_KEY,
    { consistency: "strong" },
  );
  const handoffAlreadyInitialized = previousMigration.value?.counter_handoff_version === 1;
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
  for (const { record, hashRecord } of apiKeyInventory.pairs) {
    const policy = apiKeyPolicyFromHashRecord(record.hash, hashRecord, migrationPolicyNow(record, migrationNowMs));
    if (!policy) throw new Error(`API key ${record.id} policy could not be normalized`);
    apiKeys += 1;
    if (policy.usage_limit_requests !== -1) {
      const counterKey = apiKeyUsageV2Key(policy);
      const baselineKey = apiKeyUsageV2MigrationBaselineKey(policy);
      const handoff = await migrateBoundedCounterHandoff(
        kv,
        policy,
        currentLegacyUsage(record, migrationNowMs),
        migrationNowMs,
        handoffAlreadyInitialized,
      );
      if (handoff.baseline_created) boundedBaselinesCreated += 1;
      if (handoff.baseline_reconciled) boundedBaselinesReconciled += 1;
      legacyUsageDeltaApplied += handoff.legacy_usage_delta_applied;
      if (!handoffAlreadyInitialized) {
        await deleteStaleKeys(kv, [...API_KEY_USAGE_V2_PREFIX, record.id], counterKey);
        await deleteStaleKeys(kv, [...API_KEY_USAGE_V2_MIGRATION_BASELINE_PREFIX, record.id], baselineKey);
      }
      boundedCounters += 1;
    } else if (!handoffAlreadyInitialized) {
      await deleteStaleKeys(kv, [...API_KEY_USAGE_V2_PREFIX, record.id]);
      await deleteStaleKeys(kv, [...API_KEY_USAGE_V2_MIGRATION_BASELINE_PREFIX, record.id]);
    }
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

  await kv.atomic()
    .set(RUNTIME_CONFIG_V2_KEY, runtimeConfig)
    .set(KV_READ_INCIDENT_V2_MIGRATION_KEY, {
      version: 2,
      counter_handoff_version: 1,
      completed_at_ms: Date.now(),
      api_keys: apiKeys,
      bounded_counters: boundedCounters,
      handoff_phase: handoffPhase,
      bounded_baselines_created: boundedBaselinesCreated,
      bounded_baselines_reconciled: boundedBaselinesReconciled,
      legacy_usage_delta_applied: legacyUsageDeltaApplied,
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
  const validationNowMs = Date.now();
  for (const { record, hashRecord } of apiKeyInventory.pairs) {
    const policy = apiKeyPolicyFromHashRecord(record.hash, hashRecord, migrationPolicyNow(record, validationNowMs));
    if (!policy) {
      errors.push(`api key policy could not be normalized: ${record.id}`);
      continue;
    }
    if (record.usage_limit_requests !== -1) {
      const counter = await kv.get<Deno.KvU64>(apiKeyUsageV2Key(policy));
      if (!counter.value || typeof counter.value.value !== "bigint") {
        errors.push(`bounded counter is missing or invalid: ${record.id}`);
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
      paid_fallback_ledger: paidFallbackLedger,
      kernel_repo_limits: kernelLimits,
      kernel_org_limits: kernelOrgLimits,
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
