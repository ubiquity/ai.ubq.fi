import { API_KEY_NO_EXPIRATION_MS, API_KEY_NO_USAGE_LIMIT } from "./api_keys.ts";
import {
  DEFAULT_KERNEL_POLICY_LIMIT_KEY,
  DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS,
  DEFAULT_KERNEL_POLICY_WINDOW_KEY,
  DEFAULT_KERNEL_POLICY_WINDOW_MS,
} from "./defaults.ts";
import { openaiError } from "./http.ts";
import { getKv } from "./kv.ts";
import type { KernelAuthLimitRecord, KernelOrgLimitRecord } from "./types.ts";
import { isRecord } from "./utils.ts";

export const KERNEL_QUOTA_V2_PREFIX = ["uos_ai", "kernel_quota", "v2"] as const;
export const KERNEL_REPO_POLICY_V2_PREFIX = [...KERNEL_QUOTA_V2_PREFIX, "repo_policy"] as const;
export const KERNEL_ORG_POLICY_V2_PREFIX = [...KERNEL_QUOTA_V2_PREFIX, "org_policy"] as const;
export const KERNEL_REPO_WINDOW_V2_PREFIX = [...KERNEL_QUOTA_V2_PREFIX, "repo_window"] as const;
export const KERNEL_ORG_WINDOW_V2_PREFIX = [...KERNEL_QUOTA_V2_PREFIX, "org_window"] as const;

const MAX_KV_RETRIES = 3;

export const kernelRepoPolicyKey = (owner: string, repo: string) =>
  [...KERNEL_REPO_POLICY_V2_PREFIX, owner, repo] as const;
export const kernelOrgPolicyKey = (owner: string) => [...KERNEL_ORG_POLICY_V2_PREFIX, owner] as const;
export const kernelRepoWindowKey = (owner: string, repo: string) =>
  [...KERNEL_REPO_WINDOW_V2_PREFIX, owner, repo] as const;
export const kernelOrgWindowKey = (owner: string) => [...KERNEL_ORG_WINDOW_V2_PREFIX, owner] as const;

// Existing admin and queue call sites retain these names, but they now point
// only at policy records. Usage is deliberately never co-located with policy.
export const kernelLimitKey = kernelRepoPolicyKey;
export const kernelOrgLimitKey = kernelOrgPolicyKey;

type KernelQuotaScope = "repo" | "org";

export type KernelQuotaPolicyV2 = Readonly<{
  v: 2;
  scope: KernelQuotaScope;
  owner: string;
  repo?: string;
  usage_limit_requests: number;
  window_ms: number;
  expires_at_ms: number;
  created_at_ms: number;
  updated_at_ms: number;
}>;

export type KernelQuotaWindowV2 = Readonly<{
  v: 2;
  scope: KernelQuotaScope;
  owner: string;
  repo?: string;
  usage_requests: number;
  usage_reset_at_ms: number;
  applied_window_ms: number;
  created_at_ms: number;
  updated_at_ms: number;
}>;

type KernelDefaults = Readonly<{ limit: number; windowMs: number }>;

const positiveSafeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0;
const nonNegativeSafeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const normalizeUsageLimit = (value: unknown, fallback: number): number => {
  if (typeof value === "string") value = Number(value);
  if (typeof value !== "number" || !Number.isFinite(value)) return fallback;
  const parsed = Math.trunc(value);
  if (parsed === API_KEY_NO_USAGE_LIMIT) return parsed;
  return parsed >= 0 ? parsed : fallback;
};

const normalizeWindow = (value: unknown, fallback: number): number => {
  if (typeof value === "string") value = Number(value);
  if (typeof value !== "number" || !Number.isFinite(value)) return fallback;
  const parsed = Math.trunc(value);
  return parsed > 0 ? parsed : fallback;
};

const normalizeExpiration = (value: unknown): number => {
  if (typeof value !== "number" || !Number.isFinite(value)) return API_KEY_NO_EXPIRATION_MS;
  const parsed = Math.trunc(value);
  return parsed === API_KEY_NO_EXPIRATION_MS || parsed >= 0 ? parsed : API_KEY_NO_EXPIRATION_MS;
};

const loadDefaults = async (kv: Deno.Kv): Promise<KernelDefaults> => {
  const [limitEntry, windowEntry] = await Promise.all([
    kv.get<number>(DEFAULT_KERNEL_POLICY_LIMIT_KEY, { consistency: "strong" }),
    kv.get<number>(DEFAULT_KERNEL_POLICY_WINDOW_KEY, { consistency: "strong" }),
  ]);
  return {
    limit: normalizeUsageLimit(limitEntry.value, DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS),
    windowMs: normalizeWindow(windowEntry.value, DEFAULT_KERNEL_POLICY_WINDOW_MS),
  };
};

const validIdentity = (
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  value: Record<string, unknown>,
): boolean =>
  value.v === 2 && value.scope === scope && value.owner === owner &&
  (scope === "org" ? value.repo === undefined || value.repo === null : value.repo === repo);

export const normalizeKernelQuotaPolicyV2 = (
  value: unknown,
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
): KernelQuotaPolicyV2 | null => {
  if (!isRecord(value) || !validIdentity(scope, owner, repo, value)) return null;
  if (!nonNegativeSafeInteger(value.created_at_ms) || !nonNegativeSafeInteger(value.updated_at_ms)) return null;
  const windowMs = normalizeWindow(value.window_ms, 0);
  const limit = normalizeUsageLimit(value.usage_limit_requests, Number.NaN);
  if (!positiveSafeInteger(windowMs) || !(limit === API_KEY_NO_USAGE_LIMIT || nonNegativeSafeInteger(limit))) {
    return null;
  }
  const expiresAtMs = normalizeExpiration(value.expires_at_ms);
  return {
    v: 2,
    scope,
    owner,
    ...(scope === "repo" ? { repo } : {}),
    usage_limit_requests: limit,
    window_ms: windowMs,
    expires_at_ms: expiresAtMs,
    created_at_ms: value.created_at_ms,
    updated_at_ms: value.updated_at_ms,
  };
};

export const normalizeKernelQuotaWindowV2 = (
  value: unknown,
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
): KernelQuotaWindowV2 | null => {
  if (!isRecord(value) || !validIdentity(scope, owner, repo, value)) return null;
  if (
    !nonNegativeSafeInteger(value.usage_requests) || !positiveSafeInteger(value.usage_reset_at_ms) ||
    !positiveSafeInteger(value.applied_window_ms) || !nonNegativeSafeInteger(value.created_at_ms) ||
    !nonNegativeSafeInteger(value.updated_at_ms)
  ) return null;
  return {
    v: 2,
    scope,
    owner,
    ...(scope === "repo" ? { repo } : {}),
    usage_requests: value.usage_requests,
    usage_reset_at_ms: value.usage_reset_at_ms,
    applied_window_ms: value.applied_window_ms,
    created_at_ms: value.created_at_ms,
    updated_at_ms: value.updated_at_ms,
  };
};

const newWindow = (
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  windowMs: number,
  nowMs: number,
): KernelQuotaWindowV2 => ({
  v: 2,
  scope,
  owner,
  ...(scope === "repo" ? { repo } : {}),
  usage_requests: 0,
  usage_reset_at_ms: nowMs + windowMs,
  applied_window_ms: windowMs,
  created_at_ms: nowMs,
  updated_at_ms: nowMs,
});

const windowForEffectivePolicy = (
  existing: KernelQuotaWindowV2 | null,
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  effectiveWindowMs: number,
  nowMs: number,
): { window: KernelQuotaWindowV2; needsWrite: boolean } => {
  if (!existing || existing.applied_window_ms !== effectiveWindowMs || existing.usage_reset_at_ms <= nowMs) {
    return { window: newWindow(scope, owner, repo, effectiveWindowMs, nowMs), needsWrite: true };
  }
  return { window: existing, needsWrite: false };
};

const policyFor = (
  entry: Deno.KvEntryMaybe<KernelQuotaPolicyV2>,
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  defaults: KernelDefaults,
): {
  policy: KernelQuotaPolicyV2 | null;
  limit: number;
  windowMs: number;
  expiresAtMs: number;
  source: "default" | "kv";
} => {
  if (entry.value === null) {
    return {
      policy: null,
      limit: defaults.limit,
      windowMs: defaults.windowMs,
      expiresAtMs: API_KEY_NO_EXPIRATION_MS,
      source: "default",
    };
  }
  const policy = normalizeKernelQuotaPolicyV2(entry.value, scope, owner, repo);
  if (!policy) {
    // A corrupt explicit policy must never silently turn into a permissive
    // default; callers surface it as an unavailable quota record.
    throw new Error("kernel quota policy is malformed");
  }
  return {
    policy,
    limit: policy.usage_limit_requests,
    windowMs: policy.window_ms,
    expiresAtMs: policy.expires_at_ms,
    source: "kv",
  };
};

const isExpired = (expiresAtMs: number, nowMs: number): boolean =>
  expiresAtMs !== API_KEY_NO_EXPIRATION_MS && expiresAtMs <= nowMs;

const repoRecord = (
  owner: string,
  repo: string,
  policy: { limit: number; windowMs: number; expiresAtMs: number },
  window: KernelQuotaWindowV2,
  nowMs: number,
): KernelAuthLimitRecord => ({
  owner,
  repo,
  usage_limit_requests: policy.limit,
  usage_requests: window.usage_requests,
  usage_reset_at_ms: window.usage_reset_at_ms,
  window_ms: policy.windowMs,
  expires_at_ms: policy.expiresAtMs,
  created_at_ms: window.created_at_ms || nowMs,
  updated_at_ms: Math.max(window.updated_at_ms, nowMs),
});

const orgRecord = (
  owner: string,
  policy: { limit: number; windowMs: number; expiresAtMs: number },
  window: KernelQuotaWindowV2,
  nowMs: number,
): KernelOrgLimitRecord => ({
  owner,
  usage_limit_requests: policy.limit,
  usage_requests: window.usage_requests,
  usage_reset_at_ms: window.usage_reset_at_ms,
  window_ms: policy.windowMs,
  expires_at_ms: policy.expiresAtMs,
  created_at_ms: window.created_at_ms || nowMs,
  updated_at_ms: Math.max(window.updated_at_ms, nowMs),
});

export type KernelAuthLimitSnapshot = Readonly<{ record: KernelAuthLimitRecord; source: "default" | "kv" }>;
export type KernelOrgLimitSnapshot = Readonly<{ record: KernelOrgLimitRecord; source: "default" | "kv" }>;

const getSnapshot = async (
  scope: KernelQuotaScope,
  owner: string,
  repo?: string,
): Promise<{ record: KernelAuthLimitRecord | KernelOrgLimitRecord; source: "default" | "kv" } | null> => {
  try {
    const kv = await getKv();
    if (!kv) return null;
    const policyKey = scope === "repo" ? kernelRepoPolicyKey(owner, repo!) : kernelOrgPolicyKey(owner);
    const windowKey = scope === "repo" ? kernelRepoWindowKey(owner, repo!) : kernelOrgWindowKey(owner);
    const [defaults, policyEntry, windowEntry] = await Promise.all([
      loadDefaults(kv),
      kv.get<KernelQuotaPolicyV2>(policyKey, { consistency: "strong" }),
      kv.get<KernelQuotaWindowV2>(windowKey, { consistency: "strong" }),
    ]);
    const effective = policyFor(policyEntry, scope, owner, repo, defaults);
    const window = windowForEffectivePolicy(
      normalizeKernelQuotaWindowV2(windowEntry.value, scope, owner, repo),
      scope,
      owner,
      repo,
      effective.windowMs,
      Date.now(),
    ).window;
    return {
      record: scope === "repo"
        ? repoRecord(owner, repo!, effective, window, Date.now())
        : orgRecord(owner, effective, window, Date.now()),
      source: effective.source,
    };
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to load kernel quota snapshot:", error);
    return null;
  }
};

export const getKernelUsageLimitSnapshot = async (
  owner: string,
  repo: string,
): Promise<KernelAuthLimitSnapshot | null> => {
  const snapshot = await getSnapshot("repo", owner, repo);
  return snapshot ? snapshot as KernelAuthLimitSnapshot : null;
};

export const getKernelOrgUsageLimitSnapshot = async (owner: string): Promise<KernelOrgLimitSnapshot | null> => {
  const snapshot = await getSnapshot("org", owner);
  return snapshot ? snapshot as KernelOrgLimitSnapshot : null;
};

const listPolicies = async (
  scope: KernelQuotaScope,
): Promise<(KernelAuthLimitRecord | KernelOrgLimitRecord)[] | null> => {
  try {
    const kv = await getKv();
    if (!kv) return null;
    const prefix = scope === "repo" ? KERNEL_REPO_POLICY_V2_PREFIX : KERNEL_ORG_POLICY_V2_PREFIX;
    const rows: (KernelAuthLimitRecord | KernelOrgLimitRecord)[] = [];
    for await (const entry of kv.list<KernelQuotaPolicyV2>({ prefix })) {
      const ownerPart = entry.key[prefix.length];
      if (typeof ownerPart !== "string" || !ownerPart) continue;
      const owner = ownerPart;
      const repoPart = entry.key[prefix.length + 1];
      const repo = scope === "repo" && typeof repoPart === "string" && repoPart ? repoPart : undefined;
      if (scope === "repo" && !repo) continue;
      const policy = normalizeKernelQuotaPolicyV2(entry.value, scope, owner, repo);
      if (!policy) continue;
      const windowKey = scope === "repo" ? kernelRepoWindowKey(owner, repo!) : kernelOrgWindowKey(owner);
      const windowEntry = await kv.get<KernelQuotaWindowV2>(windowKey, { consistency: "strong" });
      const window = windowForEffectivePolicy(
        normalizeKernelQuotaWindowV2(windowEntry.value, scope, owner, repo),
        scope,
        owner,
        repo,
        policy.window_ms,
        Date.now(),
      ).window;
      rows.push(
        scope === "repo"
          ? repoRecord(
            owner,
            repo!,
            { limit: policy.usage_limit_requests, windowMs: policy.window_ms, expiresAtMs: policy.expires_at_ms },
            window,
            Date.now(),
          )
          : orgRecord(
            owner,
            { limit: policy.usage_limit_requests, windowMs: policy.window_ms, expiresAtMs: policy.expires_at_ms },
            window,
            Date.now(),
          ),
      );
    }
    rows.sort((a, b) =>
      a.owner.localeCompare(b.owner) || ("repo" in a && "repo" in b ? a.repo.localeCompare(b.repo) : 0)
    );
    return rows;
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to list kernel quota policies:", error);
    return null;
  }
};

export const listKernelUsageLimits = async (): Promise<KernelAuthLimitRecord[] | null> => {
  const rows = await listPolicies("repo");
  return rows as KernelAuthLimitRecord[] | null;
};

export const listKernelOrgUsageLimits = async (): Promise<KernelOrgLimitRecord[] | null> => {
  const rows = await listPolicies("org");
  return rows as KernelOrgLimitRecord[] | null;
};

const setPolicy = async (
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  usageLimitRequests: number,
  options: { resetUsage?: boolean; windowMs?: number; expiresAtMs?: number },
): Promise<KernelAuthLimitRecord | KernelOrgLimitRecord | null> => {
  try {
    const kv = await getKv();
    if (!kv) return null;
    const policyKey = scope === "repo" ? kernelRepoPolicyKey(owner, repo!) : kernelOrgPolicyKey(owner);
    const windowKey = scope === "repo" ? kernelRepoWindowKey(owner, repo!) : kernelOrgWindowKey(owner);
    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
      const nowMs = Date.now();
      const [defaults, policyEntry, windowEntry] = await Promise.all([
        loadDefaults(kv),
        kv.get<KernelQuotaPolicyV2>(policyKey, { consistency: "strong" }),
        kv.get<KernelQuotaWindowV2>(windowKey, { consistency: "strong" }),
      ]);
      const current = policyFor(policyEntry, scope, owner, repo, defaults);
      const windowMs = options.windowMs === undefined
        ? current.windowMs
        : normalizeWindow(options.windowMs, current.windowMs);
      const expiresAtMs = options.expiresAtMs === undefined
        ? current.expiresAtMs
        : normalizeExpiration(options.expiresAtMs);
      const policy: KernelQuotaPolicyV2 = {
        v: 2,
        scope,
        owner,
        ...(scope === "repo" ? { repo } : {}),
        usage_limit_requests: normalizeUsageLimit(usageLimitRequests, current.limit),
        window_ms: windowMs,
        expires_at_ms: expiresAtMs,
        created_at_ms: current.policy?.created_at_ms ?? nowMs,
        updated_at_ms: nowMs,
      };
      const currentWindow = normalizeKernelQuotaWindowV2(windowEntry.value, scope, owner, repo);
      const reset = options.resetUsage === true || current.windowMs !== windowMs;
      const baseWindow = reset
        ? newWindow(scope, owner, repo, windowMs, nowMs)
        : windowForEffectivePolicy(currentWindow, scope, owner, repo, windowMs, nowMs).window;
      const window: KernelQuotaWindowV2 = reset ? baseWindow : { ...baseWindow, updated_at_ms: nowMs };
      const committed = await kv.atomic()
        .check(policyEntry)
        .check(windowEntry)
        .set(policyKey, policy)
        .set(windowKey, window)
        .commit();
      if (!committed.ok) continue;
      return scope === "repo"
        ? repoRecord(owner, repo!, { limit: policy.usage_limit_requests, windowMs, expiresAtMs }, window, nowMs)
        : orgRecord(owner, { limit: policy.usage_limit_requests, windowMs, expiresAtMs }, window, nowMs);
    }
    return null;
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to set kernel quota policy:", error);
    return null;
  }
};

export const setKernelUsageLimit = async (
  owner: string,
  repo: string,
  usageLimitRequests: number,
  options: { resetUsage?: boolean; windowMs?: number; expiresAtMs?: number } = {},
): Promise<KernelAuthLimitRecord | null> =>
  await setPolicy("repo", owner, repo, usageLimitRequests, options) as KernelAuthLimitRecord | null;

export const setKernelOrgUsageLimit = async (
  owner: string,
  usageLimitRequests: number,
  options: { resetUsage?: boolean; windowMs?: number; expiresAtMs?: number } = {},
): Promise<KernelOrgLimitRecord | null> =>
  await setPolicy("org", owner, undefined, usageLimitRequests, options) as KernelOrgLimitRecord | null;

const deletePolicy = async (scope: KernelQuotaScope, owner: string, repo?: string): Promise<boolean | null> => {
  try {
    const kv = await getKv();
    if (!kv) return null;
    const policyKey = scope === "repo" ? kernelRepoPolicyKey(owner, repo!) : kernelOrgPolicyKey(owner);
    const windowKey = scope === "repo" ? kernelRepoWindowKey(owner, repo!) : kernelOrgWindowKey(owner);
    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
      const nowMs = Date.now();
      const [defaults, policyEntry, windowEntry] = await Promise.all([
        loadDefaults(kv),
        kv.get<KernelQuotaPolicyV2>(policyKey, { consistency: "strong" }),
        kv.get<KernelQuotaWindowV2>(windowKey, { consistency: "strong" }),
      ]);
      if (policyEntry.value === null) return false;
      const current = policyFor(policyEntry, scope, owner, repo, defaults);
      const oldWindow = normalizeKernelQuotaWindowV2(windowEntry.value, scope, owner, repo);
      const nextWindow = current.windowMs === defaults.windowMs
        ? windowForEffectivePolicy(oldWindow, scope, owner, repo, defaults.windowMs, nowMs).window
        : newWindow(scope, owner, repo, defaults.windowMs, nowMs);
      const committed = await kv.atomic()
        .check(policyEntry)
        .check(windowEntry)
        .delete(policyKey)
        .set(windowKey, nextWindow)
        .commit();
      if (committed.ok) return true;
    }
    return null;
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to delete kernel quota policy:", error);
    return null;
  }
};

export const deleteKernelUsageLimit = async (owner: string, repo: string): Promise<boolean | null> =>
  await deletePolicy("repo", owner, repo);
export const deleteKernelOrgUsageLimit = async (owner: string): Promise<boolean | null> =>
  await deletePolicy("org", owner);

const checkLimit = async (
  scope: KernelQuotaScope,
  owner: string,
  repo?: string,
): Promise<{ ok: true } | { ok: false; response: Response }> => {
  try {
    const kv = await getKv();
    if (!kv) return { ok: true };
    const policyKey = scope === "repo" ? kernelRepoPolicyKey(owner, repo!) : kernelOrgPolicyKey(owner);
    const windowKey = scope === "repo" ? kernelRepoWindowKey(owner, repo!) : kernelOrgWindowKey(owner);
    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
      const nowMs = Date.now();
      const [defaults, policyEntry, windowEntry] = await Promise.all([
        loadDefaults(kv),
        kv.get<KernelQuotaPolicyV2>(policyKey, { consistency: "strong" }),
        kv.get<KernelQuotaWindowV2>(windowKey, { consistency: "strong" }),
      ]);
      const effective = policyFor(policyEntry, scope, owner, repo, defaults);
      if (isExpired(effective.expiresAtMs, nowMs)) {
        return {
          ok: false,
          response: openaiError(
            429,
            "Kernel auth usage limit expired; update it via /admin/kernel-usage.",
            "rate_limit_exceeded",
          ),
        };
      }
      if (effective.limit === 0) {
        return {
          ok: false,
          response: openaiError(
            429,
            "Kernel auth usage limit is 0; update it via /admin/kernel-usage.",
            "rate_limit_exceeded",
          ),
        };
      }
      const resolved = windowForEffectivePolicy(
        normalizeKernelQuotaWindowV2(windowEntry.value, scope, owner, repo),
        scope,
        owner,
        repo,
        effective.windowMs,
        nowMs,
      );
      if (resolved.needsWrite) {
        const refreshed = await kv.atomic().check(policyEntry).check(windowEntry).set(windowKey, resolved.window)
          .commit();
        if (!refreshed.ok) continue;
      }
      if (effective.limit !== API_KEY_NO_USAGE_LIMIT && resolved.window.usage_requests >= effective.limit) {
        return {
          ok: false,
          response: openaiError(
            429,
            `Usage limit exceeded (${resolved.window.usage_requests}/${effective.limit}). Resets at ${
              new Date(resolved.window.usage_reset_at_ms).toISOString()
            }`,
            "rate_limit_exceeded",
          ),
        };
      }
      return { ok: true };
    }
    return { ok: false, response: openaiError(503, "Kernel quota changed concurrently; retry", "server_error") };
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to check kernel quota:", error);
    return { ok: false, response: openaiError(503, "Kernel quota is unavailable", "server_error") };
  }
};

export const checkKernelUsageLimit = async (owner: string, repo: string) => await checkLimit("repo", owner, repo);
export const checkKernelOrgUsageLimit = async (owner: string) => await checkLimit("org", owner);

const incrementLimit = async (scope: KernelQuotaScope, owner: string, repo?: string): Promise<void> => {
  try {
    const kv = await getKv();
    if (!kv) return;
    const policyKey = scope === "repo" ? kernelRepoPolicyKey(owner, repo!) : kernelOrgPolicyKey(owner);
    const windowKey = scope === "repo" ? kernelRepoWindowKey(owner, repo!) : kernelOrgWindowKey(owner);
    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
      const nowMs = Date.now();
      const [defaults, policyEntry, windowEntry] = await Promise.all([
        loadDefaults(kv),
        kv.get<KernelQuotaPolicyV2>(policyKey, { consistency: "strong" }),
        kv.get<KernelQuotaWindowV2>(windowKey, { consistency: "strong" }),
      ]);
      const effective = policyFor(policyEntry, scope, owner, repo, defaults);
      const resolved = windowForEffectivePolicy(
        normalizeKernelQuotaWindowV2(windowEntry.value, scope, owner, repo),
        scope,
        owner,
        repo,
        effective.windowMs,
        nowMs,
      );
      const updated: KernelQuotaWindowV2 = {
        ...resolved.window,
        usage_requests: resolved.window.usage_requests + 1,
        updated_at_ms: nowMs,
      };
      const committed = await kv.atomic().check(policyEntry).check(windowEntry).set(windowKey, updated).commit();
      if (committed.ok) return;
    }
    console.warn("[ai.ubq.fi] Failed to increment kernel quota after retries:", `${owner}/${repo ?? ""}`);
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to increment kernel quota:", error);
  }
};

export const incrementKernelUsageLimit = async (owner: string, repo: string): Promise<void> =>
  await incrementLimit("repo", owner, repo);
export const incrementKernelOrgUsageLimit = async (owner: string): Promise<void> => await incrementLimit("org", owner);
