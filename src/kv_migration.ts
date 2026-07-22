import { type KvEntryJSON, toKey, toValue } from "@deno/kv-utils/json";
import { API_KEY_ID_PREFIX, apiKeyHashKey } from "./api_keys.ts";
import { API_KEY_USAGE_V2_PREFIX, apiKeyPolicyFromHashRecord, apiKeyUsageV2Key } from "./api_key_policy.ts";
import { buildRuntimeConfig, normalizeRuntimeConfig, RUNTIME_CONFIG_V2_KEY } from "./runtime_config.ts";
import type { ApiKeyHashRecord, ApiKeyRecord, ApiKeyRequestLogRecord } from "./types.ts";
import { hasStrictPaidFallbackKeyPolicy, hasStrictPaidFallbackPolicy } from "./paid_fallback.ts";

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
  { group: "codex_rate_limit", prefix: ["uos_ai", "codex_rate_limit"] },
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
const LEGACY_REQUEST_LOG_PREFIX = ["ubq_ai", "api_keys", "request_log"] as const;
const PAID_FALLBACK_LEDGER_PREFIX = ["uos_ai", "paid_fallback", "ledger"] as const;
const isRoutableApiKeyPrefix = (value: unknown): boolean => typeof value === "string" && /^u_[0-9a-f]{10}$/.test(value);

export type KvReadIncidentV2MigrationResult = Readonly<{
  api_keys: number;
  bounded_counters: number;
  paid_fallback_records: number;
  runtime_config_written: boolean;
}>;

export const migrateKvReadIncidentV2 = async (kv: Deno.Kv): Promise<KvReadIncidentV2MigrationResult> => {
  const codexModels = await kv.get<Record<string, unknown>>(["ubq_ai", "codex_models"]);
  if (!codexModels.value) throw new Error("Codex model snapshot is missing");
  const defaultModel = await kv.get<string>(["default", "model"]);
  const defaultReasoning = await kv.get<string>(["default", "reasoning_effort"]);
  const runtimeConfig = buildRuntimeConfig(codexModels.value as never, {
    defaultModel: defaultModel.value,
    defaultReasoningEffort: defaultReasoning.value,
  });

  let apiKeys = 0;
  let boundedCounters = 0;
  for await (const entry of kv.list<ApiKeyRecord>({ prefix: API_KEY_ID_PREFIX })) {
    const record = entry.value;
    if (!record || !hasStrictPaidFallbackKeyPolicy(record) || !isRoutableApiKeyPrefix(record.prefix)) {
      throw new Error(`API key ${String(entry.key.at(-1))} has an invalid policy`);
    }
    const hashEntry = await kv.get<ApiKeyHashRecord>(apiKeyHashKey(record.hash));
    if (!hashEntry.value || !hasStrictPaidFallbackPolicy(hashEntry.value) || hashEntry.value.id !== record.id) {
      throw new Error(`API key ${record.id} is missing its matching hash policy`);
    }
    const policy = apiKeyPolicyFromHashRecord(record.hash, hashEntry.value, Date.now());
    if (!policy) throw new Error(`API key ${record.id} policy could not be normalized`);
    apiKeys += 1;
    if (policy.usage_limit_requests !== -1) {
      const counterKey = apiKeyUsageV2Key(policy);
      let usage = BigInt(Math.max(0, record.usage_requests));
      const staleCounterKeys: Deno.KvKey[] = [];
      for await (const counter of kv.list<Deno.KvU64>({ prefix: [...API_KEY_USAGE_V2_PREFIX, record.id] })) {
        if (counter.value && counter.value.value > usage) usage = counter.value.value;
        if (JSON.stringify(counter.key) !== JSON.stringify(counterKey)) staleCounterKeys.push(counter.key);
      }
      await kv.set(counterKey, new Deno.KvU64(usage));
      for (const staleKey of staleCounterKeys) await kv.delete(staleKey);
      boundedCounters += 1;
    }
  }

  let paidFallbackRecords = 0;
  for await (const entry of kv.list<ApiKeyRequestLogRecord>({ prefix: LEGACY_REQUEST_LOG_PREFIX })) {
    const value = entry.value;
    if (
      !value || value.provider !== "yunwu" ||
      (value.billing_status !== "pending" && value.billing_status !== "unresolved")
    ) continue;
    await kv.set(
      [...PAID_FALLBACK_LEDGER_PREFIX, value.key_id, value.created_at_ms, value.id],
      value,
    );
    paidFallbackRecords += 1;
  }

  await kv.atomic()
    .set(RUNTIME_CONFIG_V2_KEY, runtimeConfig)
    .set(KV_READ_INCIDENT_V2_MIGRATION_KEY, {
      version: 2,
      completed_at_ms: Date.now(),
      api_keys: apiKeys,
      bounded_counters: boundedCounters,
      paid_fallback_records: paidFallbackRecords,
    })
    .commit();
  return {
    api_keys: apiKeys,
    bounded_counters: boundedCounters,
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

  let boundedApiKeys = 0;
  for await (const entry of kv.list<ApiKeyRecord>({ prefix: API_KEY_ID_PREFIX })) {
    if (
      !entry.value || !hasStrictPaidFallbackKeyPolicy(entry.value) ||
      !isRoutableApiKeyPrefix(entry.value.prefix)
    ) {
      errors.push(`api key has invalid v2 policy: ${String(entry.key.at(-1))}`);
      continue;
    }
    const hashEntry = await kv.get<ApiKeyHashRecord>(apiKeyHashKey(entry.value.hash));
    if (
      !hashEntry.value || hashEntry.value.id !== entry.value.id ||
      !hasStrictPaidFallbackPolicy(hashEntry.value)
    ) {
      errors.push(`api key hash policy is missing or inconsistent: ${entry.value.id}`);
      continue;
    }
    const policy = apiKeyPolicyFromHashRecord(entry.value.hash, hashEntry.value, Date.now());
    if (!policy) {
      errors.push(`api key policy could not be normalized: ${entry.value.id}`);
      continue;
    }
    if (entry.value.usage_limit_requests !== -1) {
      boundedApiKeys += 1;
      const counter = await kv.get<Deno.KvU64>(apiKeyUsageV2Key(policy));
      if (!counter.value || typeof counter.value.value !== "bigint") {
        errors.push(`bounded counter is missing or invalid: ${entry.value.id}`);
      }
    }
    if (entry.value.paid_fallback_reservation_request_id) {
      let reservationFound = false;
      for await (
        const ledger of kv.list<ApiKeyRequestLogRecord>({ prefix: [...PAID_FALLBACK_LEDGER_PREFIX, entry.value.id] })
      ) {
        if (ledger.value?.id === entry.value.paid_fallback_reservation_request_id) {
          reservationFound = true;
          break;
        }
      }
      if (!reservationFound) errors.push(`paid fallback reservation has no ledger record: ${entry.value.id}`);
    }
  }
  if (boundedCounters < boundedApiKeys) {
    errors.push(`bounded counter count is incomplete: keys=${boundedApiKeys} counters=${boundedCounters}`);
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
