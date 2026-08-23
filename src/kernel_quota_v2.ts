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
export const KERNEL_REPO_RESERVATION_V2_PREFIX = [...KERNEL_QUOTA_V2_PREFIX, "repo_reservation"] as const;
export const KERNEL_ORG_RESERVATION_V2_PREFIX = [...KERNEL_QUOTA_V2_PREFIX, "org_reservation"] as const;
export const KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY = [...KERNEL_QUOTA_V2_PREFIX, "default_window_cutover"] as const;
export const KERNEL_QUOTA_RESERVATION_LEASE_MS = 5 * 60_000;
export const KERNEL_QUOTA_RESERVATION_RENEWAL_MS = 60_000;
const KERNEL_QUOTA_RESERVATION_RETENTION_MS = 7 * 24 * 60 * 60_000;
const KERNEL_DEFAULT_WINDOW_CUTOVER_LEASE_MS = 5 * 60_000;
const KERNEL_QUOTA_SETTLEMENT_RETRY_MS = 1_000;

const MAX_KV_RETRIES = 3;

export const kernelRepoPolicyKey = (owner: string, repo: string) =>
  [...KERNEL_REPO_POLICY_V2_PREFIX, owner, repo] as const;
export const kernelOrgPolicyKey = (owner: string) => [...KERNEL_ORG_POLICY_V2_PREFIX, owner] as const;
export const kernelRepoWindowKey = (owner: string, repo: string) =>
  [...KERNEL_REPO_WINDOW_V2_PREFIX, owner, repo] as const;
export const kernelOrgWindowKey = (owner: string) => [...KERNEL_ORG_WINDOW_V2_PREFIX, owner] as const;
export const kernelRepoReservationKey = (
  owner: string,
  repo: string,
  windowCreatedAtMs: number,
  requestId: string,
) => [...KERNEL_REPO_RESERVATION_V2_PREFIX, owner, repo, windowCreatedAtMs, requestId] as const;
export const kernelOrgReservationKey = (owner: string, windowCreatedAtMs: number, requestId: string) =>
  [...KERNEL_ORG_RESERVATION_V2_PREFIX, owner, windowCreatedAtMs, requestId] as const;

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
  reserved_requests: number;
  usage_reset_at_ms: number;
  applied_window_ms: number;
  created_at_ms: number;
  updated_at_ms: number;
}>;

type KernelQuotaReservationRowV2 = Readonly<{
  v: 2;
  scope: KernelQuotaScope;
  owner: string;
  repo?: string;
  request_id: string;
  route: string;
  window_created_at_ms: number;
  window_reset_at_ms: number;
  state: "reserved" | "committed" | "released";
  terminal_intent: "committed" | "released" | null;
  reserved_at_ms: number;
  lease_expires_at_ms: number;
  committed_at_ms: number | null;
  released_at_ms: number | null;
  release_reason: string | null;
}>;

type KernelDefaultWindowCutoverV2 = Readonly<{
  v: 2;
  id: string;
  created_at_ms: number;
  expires_at_ms: number;
}>;

export type KernelDefaultWindowCutoverGuard = Readonly<{
  key: typeof KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY;
  entry: Deno.KvEntry<KernelDefaultWindowCutoverV2>;
}>;

export type KernelDefaultWindowCutoverDecision =
  | Readonly<{ ok: true; guard: KernelDefaultWindowCutoverGuard }>
  | Readonly<{ ok: false; reason: "active_reservations" | "concurrent_change" | "unavailable" }>;

export type KernelQuotaReservation = Readonly<{
  signal: AbortSignal;
  commit: () => Promise<void>;
  release: (reason?: string) => Promise<void>;
}>;

export type KernelQuotaReservationDecision =
  | Readonly<{ ok: true; reservation: KernelQuotaReservation }>
  | Readonly<{ ok: false; response: Response }>;

export type KernelQuotaPolicyStateDecision =
  | Readonly<{ ok: true; limit_scope: KernelQuotaScope; has_policy: boolean }>
  | Readonly<{ ok: false; response: Response }>;

type KernelDefaults = Readonly<{
  limit: number;
  windowMs: number;
  limitEntry: Deno.KvEntryMaybe<number>;
  windowEntry: Deno.KvEntryMaybe<number>;
}>;

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
    limitEntry,
    windowEntry,
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
  const reservedRequests = value.reserved_requests === undefined ? 0 : value.reserved_requests;
  if (
    !nonNegativeSafeInteger(value.usage_requests) || !nonNegativeSafeInteger(reservedRequests) ||
    !positiveSafeInteger(value.usage_reset_at_ms) ||
    !positiveSafeInteger(value.applied_window_ms) || !nonNegativeSafeInteger(value.created_at_ms) ||
    !nonNegativeSafeInteger(value.updated_at_ms)
  ) return null;
  return {
    v: 2,
    scope,
    owner,
    ...(scope === "repo" ? { repo } : {}),
    usage_requests: value.usage_requests,
    reserved_requests: reservedRequests,
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
  reserved_requests: 0,
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
    return await withKernelQuotaLock(scope, owner, repo, async () => {
      const policyKey = scope === "repo" ? kernelRepoPolicyKey(owner, repo!) : kernelOrgPolicyKey(owner);
      const windowKey = scope === "repo" ? kernelRepoWindowKey(owner, repo!) : kernelOrgWindowKey(owner);
      for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
        const nowMs = Date.now();
        const [defaults, policyEntry, windowEntry, orgPolicyEntry, orgWindowEntry, defaultCutoverEntry] = await Promise
          .all([
            loadDefaults(kv),
            kv.get<KernelQuotaPolicyV2>(policyKey, { consistency: "strong" }),
            kv.get<KernelQuotaWindowV2>(windowKey, { consistency: "strong" }),
            scope === "repo"
              ? kv.get<KernelQuotaPolicyV2>(kernelOrgPolicyKey(owner), { consistency: "strong" })
              : Promise.resolve(null),
            scope === "repo"
              ? kv.get<KernelQuotaWindowV2>(kernelOrgWindowKey(owner), { consistency: "strong" })
              : Promise.resolve(null),
            kv.get<KernelDefaultWindowCutoverV2>(KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, {
              consistency: "strong",
            }),
          ]);
        const current = policyFor(policyEntry, scope, owner, repo, defaults);
        if (current.source === "default" && defaultCutoverEntry.value !== null) return null;
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
        if (windowEntry.value !== null && !currentWindow) return null;
        if (scope === "repo" && policyEntry.value === null && orgPolicyEntry && orgWindowEntry) {
          policyFor(orgPolicyEntry, "org", owner, undefined, defaults);
          const inheritedOrgWindow = normalizeKernelQuotaWindowV2(orgWindowEntry.value, "org", owner, undefined);
          if (orgWindowEntry.value !== null && !inheritedOrgWindow) return null;
          if (
            inheritedOrgWindow && inheritedOrgWindow.usage_reset_at_ms > nowMs &&
            inheritedOrgWindow.reserved_requests > 0
          ) {
            const reclaimed = await reclaimExpiredKernelReservationUnlocked(
              kv,
              "org",
              owner,
              undefined,
              inheritedOrgWindow,
              nowMs,
            );
            if (reclaimed) continue;
            return null;
          }
        }
        const reset = options.resetUsage === true || current.windowMs !== windowMs;
        if (
          reset && currentWindow && currentWindow.usage_reset_at_ms > nowMs && currentWindow.reserved_requests > 0
        ) {
          const reclaimed = await reclaimExpiredKernelReservationUnlocked(
            kv,
            scope,
            owner,
            repo,
            currentWindow,
            nowMs,
          );
          if (reclaimed) continue;
          return null;
        }
        const baseWindow = reset
          ? newWindow(scope, owner, repo, windowMs, nowMs)
          : windowForEffectivePolicy(currentWindow, scope, owner, repo, windowMs, nowMs).window;
        const window: KernelQuotaWindowV2 = reset ? baseWindow : { ...baseWindow, updated_at_ms: nowMs };
        let atomic = kv.atomic()
          .check(policyEntry)
          .check(windowEntry);
        if (current.source === "default") {
          atomic = atomic
            .check(defaults.limitEntry)
            .check(defaults.windowEntry)
            .check(defaultCutoverEntry);
        }
        if (scope === "repo" && policyEntry.value === null && orgPolicyEntry && orgWindowEntry) {
          atomic = atomic
            .check(orgPolicyEntry)
            .check(orgWindowEntry);
        }
        const committed = await atomic
          .set(policyKey, policy)
          .set(windowKey, window)
          .commit();
        if (!committed.ok) continue;
        return scope === "repo"
          ? repoRecord(owner, repo!, { limit: policy.usage_limit_requests, windowMs, expiresAtMs }, window, nowMs)
          : orgRecord(owner, { limit: policy.usage_limit_requests, windowMs, expiresAtMs }, window, nowMs);
      }
      return null;
    });
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

const deletePolicy = async (
  scope: KernelQuotaScope,
  owner: string,
  repo?: string,
): Promise<boolean | "conflict" | null> => {
  try {
    const kv = await getKv();
    if (!kv) return null;
    return await withKernelQuotaLock(scope, owner, repo, async () => {
      const policyKey = scope === "repo" ? kernelRepoPolicyKey(owner, repo!) : kernelOrgPolicyKey(owner);
      const windowKey = scope === "repo" ? kernelRepoWindowKey(owner, repo!) : kernelOrgWindowKey(owner);
      for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
        const nowMs = Date.now();
        const [defaults, policyEntry, windowEntry, defaultCutoverEntry] = await Promise.all([
          loadDefaults(kv),
          kv.get<KernelQuotaPolicyV2>(policyKey, { consistency: "strong" }),
          kv.get<KernelQuotaWindowV2>(windowKey, { consistency: "strong" }),
          kv.get<KernelDefaultWindowCutoverV2>(KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, {
            consistency: "strong",
          }),
        ]);
        if (policyEntry.value === null) return false;
        if (defaultCutoverEntry.value !== null) return "conflict";
        const current = policyFor(policyEntry, scope, owner, repo, defaults);
        const oldWindow = normalizeKernelQuotaWindowV2(windowEntry.value, scope, owner, repo);
        if (windowEntry.value !== null && !oldWindow) return null;
        if (oldWindow && oldWindow.usage_reset_at_ms > nowMs && oldWindow.reserved_requests > 0) {
          const reclaimed = await reclaimExpiredKernelReservationUnlocked(
            kv,
            scope,
            owner,
            repo,
            oldWindow,
            nowMs,
          );
          if (reclaimed) continue;
          return "conflict";
        }
        const nextWindow = current.windowMs === defaults.windowMs
          ? windowForEffectivePolicy(oldWindow, scope, owner, repo, defaults.windowMs, nowMs).window
          : newWindow(scope, owner, repo, defaults.windowMs, nowMs);
        const committed = await kv.atomic()
          .check(policyEntry)
          .check(windowEntry)
          .check(defaults.limitEntry)
          .check(defaults.windowEntry)
          .check(defaultCutoverEntry)
          .delete(policyKey)
          .set(windowKey, nextWindow)
          .commit();
        if (committed.ok) return true;
      }
      return null;
    });
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to delete kernel quota policy:", error);
    return null;
  }
};

export const deleteKernelUsageLimit = async (
  owner: string,
  repo: string,
): Promise<boolean | "conflict" | null> => await deletePolicy("repo", owner, repo);
export const deleteKernelOrgUsageLimit = async (owner: string): Promise<boolean | "conflict" | null> =>
  await deletePolicy("org", owner);

const kernelQuotaUnavailableResponse = (message = "Kernel quota is unavailable"): Response =>
  openaiError(503, message, "server_error", { type: "server_error" });

const kernelQuotaUnavailable = (message = "Kernel quota is unavailable"): KernelQuotaReservationDecision => ({
  ok: false,
  response: kernelQuotaUnavailableResponse(message),
});

const readKernelQuotaPolicyState = async (
  kv: Deno.Kv,
  owner: string,
  repo: string,
): Promise<
  Readonly<{
    limit_scope: KernelQuotaScope;
    has_policy: boolean;
    repo_entry: Deno.KvEntryMaybe<KernelQuotaPolicyV2>;
    org_entry: Deno.KvEntryMaybe<KernelQuotaPolicyV2>;
  }>
> => {
  const [repoEntry, orgEntry] = await Promise.all([
    kv.get<KernelQuotaPolicyV2>(kernelRepoPolicyKey(owner, repo), { consistency: "strong" }),
    kv.get<KernelQuotaPolicyV2>(kernelOrgPolicyKey(owner), { consistency: "strong" }),
  ]);
  if (repoEntry.value !== null && !normalizeKernelQuotaPolicyV2(repoEntry.value, "repo", owner, repo)) {
    throw new Error("Kernel repo quota policy is malformed");
  }
  if (orgEntry.value !== null && !normalizeKernelQuotaPolicyV2(orgEntry.value, "org", owner, undefined)) {
    throw new Error("Kernel org quota policy is malformed");
  }
  if (repoEntry.value !== null) {
    return { limit_scope: "repo", has_policy: true, repo_entry: repoEntry, org_entry: orgEntry };
  }
  return {
    limit_scope: "org",
    has_policy: orgEntry.value !== null,
    repo_entry: repoEntry,
    org_entry: orgEntry,
  };
};

export const resolveKernelQuotaPolicyState = async (
  owner: string,
  repo: string,
  options: Readonly<{ kv?: Deno.Kv | null }> = {},
): Promise<KernelQuotaPolicyStateDecision> => {
  try {
    const kv = options.kv === undefined ? await getKv() : options.kv;
    if (!kv) return { ok: false, response: kernelQuotaUnavailableResponse() };
    const state = await readKernelQuotaPolicyState(kv, owner, repo);
    return { ok: true, limit_scope: state.limit_scope, has_policy: state.has_policy };
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to resolve kernel quota policy scope:", error);
    return { ok: false, response: kernelQuotaUnavailableResponse() };
  }
};

const kernelReservationRetentionMs = (windowResetAtMs: number, nowMs: number): number =>
  Math.max(1, windowResetAtMs + KERNEL_QUOTA_RESERVATION_RETENTION_MS - nowMs);

const kernelReservationKey = (
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  windowCreatedAtMs: number,
  requestId: string,
): Deno.KvKey =>
  scope === "repo"
    ? kernelRepoReservationKey(owner, repo!, windowCreatedAtMs, requestId)
    : kernelOrgReservationKey(owner, windowCreatedAtMs, requestId);

const kernelReservationWindowPrefix = (
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  windowCreatedAtMs: number,
): Deno.KvKey =>
  scope === "repo"
    ? [...KERNEL_REPO_RESERVATION_V2_PREFIX, owner, repo!, windowCreatedAtMs]
    : [...KERNEL_ORG_RESERVATION_V2_PREFIX, owner, windowCreatedAtMs];

const normalizeKernelQuotaReservationRowV2 = (
  value: unknown,
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
): KernelQuotaReservationRowV2 | null => {
  if (!isRecord(value) || !validIdentity(scope, owner, repo, value)) return null;
  const terminalIntent = value.terminal_intent === undefined ? null : value.terminal_intent;
  if (
    typeof value.request_id !== "string" || !value.request_id || typeof value.route !== "string" || !value.route ||
    !nonNegativeSafeInteger(value.window_created_at_ms) || !positiveSafeInteger(value.window_reset_at_ms) ||
    (value.state !== "reserved" && value.state !== "committed" && value.state !== "released") ||
    (terminalIntent !== null && terminalIntent !== "committed" && terminalIntent !== "released") ||
    !nonNegativeSafeInteger(value.reserved_at_ms) || !positiveSafeInteger(value.lease_expires_at_ms) ||
    !(value.committed_at_ms === null || nonNegativeSafeInteger(value.committed_at_ms)) ||
    !(value.released_at_ms === null || nonNegativeSafeInteger(value.released_at_ms)) ||
    !(value.release_reason === null || typeof value.release_reason === "string")
  ) return null;
  if (
    (value.state === "reserved" &&
      (value.committed_at_ms !== null || value.released_at_ms !== null || value.release_reason !== null)) ||
    (value.state === "committed" &&
      (value.committed_at_ms === null || value.released_at_ms !== null || value.release_reason !== null ||
        terminalIntent === "released")) ||
    (value.state === "released" &&
      (value.committed_at_ms !== null || value.released_at_ms === null || !value.release_reason ||
        terminalIntent === "committed"))
  ) return null;
  return { ...value, terminal_intent: terminalIntent } as KernelQuotaReservationRowV2;
};

const kernelQuotaLocks = new Map<string, Promise<void>>();

const withKernelQuotaLock = async <T>(
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  operation: () => Promise<T>,
): Promise<T> => {
  const lockKey = JSON.stringify(scope === "repo" ? kernelRepoWindowKey(owner, repo!) : kernelOrgWindowKey(owner));
  const previous = kernelQuotaLocks.get(lockKey);
  let release!: () => void;
  const current = new Promise<void>((resolve) => {
    release = resolve;
  });
  kernelQuotaLocks.set(lockKey, current);
  if (previous) await previous;
  try {
    return await operation();
  } finally {
    release();
    if (kernelQuotaLocks.get(lockKey) === current) kernelQuotaLocks.delete(lockKey);
  }
};

type KernelReservationSettlement = "settled" | "terminal_mismatch" | "missing" | "invalid" | "conflict";

const recordKernelReservationTerminalIntentUnlocked = async (
  kv: Deno.Kv,
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  windowCreatedAtMs: number,
  requestId: string,
  route: string,
  terminalState: "committed" | "released",
  nowMs: number,
): Promise<KernelReservationSettlement | number> => {
  const reservationKey = kernelReservationKey(scope, owner, repo, windowCreatedAtMs, requestId);
  const reservationEntry = await kv.get<KernelQuotaReservationRowV2>(reservationKey, { consistency: "strong" });
  const reservation = normalizeKernelQuotaReservationRowV2(reservationEntry.value, scope, owner, repo);
  if (!reservation) return reservationEntry.value === null ? "missing" : "invalid";
  if (
    reservation.request_id !== requestId || reservation.route !== route ||
    reservation.window_created_at_ms !== windowCreatedAtMs
  ) return "invalid";
  if (reservation.state !== "reserved") {
    return reservation.state === terminalState ? "settled" : "terminal_mismatch";
  }
  if (reservation.terminal_intent !== null && reservation.terminal_intent !== terminalState) {
    return "terminal_mismatch";
  }
  const leaseExpiresAtMs = nowMs + KERNEL_QUOTA_RESERVATION_LEASE_MS;
  const pendingReservation: KernelQuotaReservationRowV2 = {
    ...reservation,
    terminal_intent: terminalState,
    lease_expires_at_ms: leaseExpiresAtMs,
  };
  const committed = await kv.atomic()
    .check(reservationEntry)
    .set(reservationKey, pendingReservation, {
      expireIn: kernelReservationRetentionMs(reservation.window_reset_at_ms, nowMs),
    })
    .commit();
  return committed.ok ? leaseExpiresAtMs : "conflict";
};

const recordKernelReservationTerminalIntent = async (
  kv: Deno.Kv,
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  windowCreatedAtMs: number,
  requestId: string,
  route: string,
  terminalState: "committed" | "released",
): Promise<number | null> => {
  return await withKernelQuotaLock(scope, owner, repo, async () => {
    let lastError: unknown = null;
    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
      try {
        const outcome = await recordKernelReservationTerminalIntentUnlocked(
          kv,
          scope,
          owner,
          repo,
          windowCreatedAtMs,
          requestId,
          route,
          terminalState,
          Date.now(),
        );
        if (typeof outcome === "number") return outcome;
        if (outcome === "settled") return null;
        if (outcome === "conflict") continue;
        if (outcome === "terminal_mismatch") {
          throw new Error(
            `Kernel quota reservation was already ${terminalState === "committed" ? "released" : "committed"}`,
          );
        }
        throw new Error(
          outcome === "missing" ? "Kernel quota reservation is missing" : "Kernel quota reservation is malformed",
        );
      } catch (error) {
        lastError = error;
      }
    }
    throw new Error("Kernel quota reservation terminal intent could not be recorded", { cause: lastError });
  });
};

const settleKernelReservationUnlocked = async (
  kv: Deno.Kv,
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  windowCreatedAtMs: number,
  requestId: string,
  route: string,
  terminalState: "committed" | "released",
  reason: string,
  nowMs: number,
): Promise<KernelReservationSettlement> => {
  const windowKey = scope === "repo" ? kernelRepoWindowKey(owner, repo!) : kernelOrgWindowKey(owner);
  const reservationKey = kernelReservationKey(scope, owner, repo, windowCreatedAtMs, requestId);
  const [windowEntry, reservationEntry] = await Promise.all([
    kv.get<KernelQuotaWindowV2>(windowKey, { consistency: "strong" }),
    kv.get<KernelQuotaReservationRowV2>(reservationKey, { consistency: "strong" }),
  ]);
  const reservation = normalizeKernelQuotaReservationRowV2(reservationEntry.value, scope, owner, repo);
  if (!reservation) return reservationEntry.value === null ? "missing" : "invalid";
  if (
    reservation.request_id !== requestId || reservation.route !== route ||
    reservation.window_created_at_ms !== windowCreatedAtMs
  ) return "invalid";
  if (reservation.state !== "reserved") {
    return reservation.state === terminalState ? "settled" : "terminal_mismatch";
  }
  if (reservation.terminal_intent !== null && reservation.terminal_intent !== terminalState) {
    return "terminal_mismatch";
  }

  const normalizedWindow = normalizeKernelQuotaWindowV2(windowEntry.value, scope, owner, repo);
  if (windowEntry.value !== null && !normalizedWindow) return "invalid";
  const retentionMs = kernelReservationRetentionMs(reservation.window_reset_at_ms, nowMs);
  if (!normalizedWindow || normalizedWindow.created_at_ms !== reservation.window_created_at_ms) {
    const settledReservation: KernelQuotaReservationRowV2 = terminalState === "committed"
      ? {
        ...reservation,
        state: "committed",
        terminal_intent: "committed",
        committed_at_ms: nowMs,
      }
      : {
        ...reservation,
        state: "released",
        terminal_intent: "released",
        released_at_ms: nowMs,
        release_reason: reason.slice(0, 120) || "request_incomplete",
      };
    const committed = await kv.atomic()
      .check(windowEntry)
      .check(reservationEntry)
      .set(reservationKey, settledReservation, { expireIn: retentionMs })
      .commit();
    return committed.ok ? "settled" : "conflict";
  }
  if (normalizedWindow.reserved_requests < 1) return "invalid";

  const updatedWindow: KernelQuotaWindowV2 = {
    ...normalizedWindow,
    usage_requests: normalizedWindow.usage_requests + (terminalState === "committed" ? 1 : 0),
    reserved_requests: normalizedWindow.reserved_requests - 1,
    updated_at_ms: nowMs,
  };
  const settledReservation: KernelQuotaReservationRowV2 = terminalState === "committed"
    ? {
      ...reservation,
      state: "committed",
      terminal_intent: "committed",
      committed_at_ms: nowMs,
    }
    : {
      ...reservation,
      state: "released",
      terminal_intent: "released",
      released_at_ms: nowMs,
      release_reason: reason.slice(0, 120) || "request_incomplete",
    };
  const committed = await kv.atomic()
    .check(windowEntry)
    .check(reservationEntry)
    .set(windowKey, updatedWindow, { expireIn: retentionMs })
    .set(reservationKey, settledReservation, { expireIn: retentionMs })
    .commit();
  return committed.ok ? "settled" : "conflict";
};

const settleKernelReservation = async (
  kv: Deno.Kv,
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  windowCreatedAtMs: number,
  requestId: string,
  route: string,
  terminalState: "committed" | "released",
  reason: string,
): Promise<void> => {
  await withKernelQuotaLock(scope, owner, repo, async () => {
    let lastError: unknown = null;
    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
      try {
        const outcome = await settleKernelReservationUnlocked(
          kv,
          scope,
          owner,
          repo,
          windowCreatedAtMs,
          requestId,
          route,
          terminalState,
          reason,
          Date.now(),
        );
        if (outcome === "settled") return;
        if (outcome === "conflict") continue;
        if (outcome === "terminal_mismatch") {
          throw new Error(
            `Kernel quota reservation was already ${terminalState === "committed" ? "released" : "committed"}`,
          );
        }
        throw new Error(
          outcome === "missing" ? "Kernel quota reservation is missing" : "Kernel quota reservation is malformed",
        );
      } catch (error) {
        lastError = error;
      }
    }
    throw new Error("Kernel quota reservation could not be settled", { cause: lastError });
  });
};

const renewKernelReservation = async (
  kv: Deno.Kv,
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  windowCreatedAtMs: number,
  requestId: string,
  route: string,
): Promise<number> => {
  let lastError: unknown = null;
  for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
    try {
      const reservationKey = kernelReservationKey(scope, owner, repo, windowCreatedAtMs, requestId);
      const entry = await kv.get<KernelQuotaReservationRowV2>(reservationKey, { consistency: "strong" });
      const reservation = normalizeKernelQuotaReservationRowV2(entry.value, scope, owner, repo);
      if (
        !reservation || reservation.request_id !== requestId || reservation.route !== route ||
        reservation.window_created_at_ms !== windowCreatedAtMs
      ) throw new Error("Kernel quota reservation is missing or malformed");
      if (reservation.state !== "reserved") {
        throw new Error(`Kernel quota reservation was already ${reservation.state}`);
      }
      const nowMs = Date.now();
      const leaseExpiresAtMs = nowMs + KERNEL_QUOTA_RESERVATION_LEASE_MS;
      const renewed: KernelQuotaReservationRowV2 = {
        ...reservation,
        lease_expires_at_ms: leaseExpiresAtMs,
      };
      const committed = await kv.atomic()
        .check(entry)
        .set(reservationKey, renewed, {
          expireIn: kernelReservationRetentionMs(reservation.window_reset_at_ms, nowMs),
        })
        .commit();
      if (committed.ok) return leaseExpiresAtMs;
    } catch (error) {
      lastError = error;
    }
  }
  throw new Error("Kernel quota reservation lease could not be renewed", { cause: lastError });
};

const kernelReservationContext = (
  kv: Deno.Kv,
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  windowCreatedAtMs: number,
  requestId: string,
  route: string,
  initialLeaseExpiresAtMs: number,
  renewalIntervalMs = KERNEL_QUOTA_RESERVATION_RENEWAL_MS,
): KernelQuotaReservation => {
  const leaseAbort = new AbortController();
  let leaseExpiresAtMs = initialLeaseExpiresAtMs;
  let renewalInFlight: Promise<void> | null = null;
  let terminalIntent: "committed" | "released" | null = null;
  let terminalReason = "request_incomplete";
  let terminalSettled: "committed" | "released" | null = null;
  let terminalInFlight: Promise<void> | null = null;
  let renewalTimer: ReturnType<typeof setInterval> | null = null;
  let leaseExpiryTimer: ReturnType<typeof setTimeout> | null = null;
  let settlementRetryTimer: ReturnType<typeof setTimeout> | null = null;
  let settlementRetryDelayMs = KERNEL_QUOTA_SETTLEMENT_RETRY_MS;
  const stopRenewal = (): void => {
    if (renewalTimer === null) return;
    clearInterval(renewalTimer);
    renewalTimer = null;
  };
  const clearLeaseExpiry = (): void => {
    if (leaseExpiryTimer === null) return;
    clearTimeout(leaseExpiryTimer);
    leaseExpiryTimer = null;
  };
  const stopAfterSettlement = (): void => {
    stopRenewal();
    clearLeaseExpiry();
    if (settlementRetryTimer !== null) {
      clearTimeout(settlementRetryTimer);
      settlementRetryTimer = null;
    }
  };
  const scheduleLeaseExpiry = (): void => {
    clearLeaseExpiry();
    if (terminalSettled) return;
    leaseExpiryTimer = setTimeout(() => {
      leaseExpiryTimer = null;
      if (terminalSettled) return;
      if (Date.now() < leaseExpiresAtMs) {
        scheduleLeaseExpiry();
        return;
      }
      if (!leaseAbort.signal.aborted) {
        leaseAbort.abort(new DOMException("Kernel quota reservation lease expired", "TimeoutError"));
      }
      // A terminal intent still needs lease renewal and durable settlement.
      // For in-flight inference, abort is the fail-closed terminal condition.
      if (!terminalIntent) stopRenewal();
    }, Math.max(1, leaseExpiresAtMs - Date.now()));
  };
  const updateLease = (nextLeaseExpiresAtMs: number): void => {
    leaseExpiresAtMs = nextLeaseExpiresAtMs;
    scheduleLeaseExpiry();
  };
  const renew = (): void => {
    if (terminalSettled || renewalInFlight) return;
    const current = renewKernelReservation(kv, scope, owner, repo, windowCreatedAtMs, requestId, route)
      .then((nextLeaseExpiresAtMs) => {
        updateLease(nextLeaseExpiresAtMs);
      })
      .catch((error) => {
        console.warn("[ai.ubq.fi] Failed to renew kernel quota reservation lease:", error);
      })
      .finally(() => {
        if (renewalInFlight === current) renewalInFlight = null;
      });
    renewalInFlight = current;
  };
  renewalTimer = setInterval(renew, renewalIntervalMs);
  scheduleLeaseExpiry();
  const scheduleSettlementRetry = (): void => {
    if (terminalSettled || settlementRetryTimer !== null) return;
    const delayMs = settlementRetryDelayMs;
    settlementRetryDelayMs = Math.min(settlementRetryDelayMs * 2, 30_000);
    settlementRetryTimer = setTimeout(() => {
      settlementRetryTimer = null;
      void launchSettlement().catch((error) => {
        console.warn("[ai.ubq.fi] Failed to settle kernel quota reservation; retrying:", error);
      });
    }, delayMs);
  };
  function launchSettlement(): Promise<void> {
    if (!terminalIntent) return Promise.reject(new Error("Kernel quota reservation has no terminal state"));
    if (terminalSettled === terminalIntent) return Promise.resolve();
    if (terminalInFlight) return terminalInFlight;
    const state = terminalIntent;
    const current = (async () => {
      const nextLeaseExpiresAtMs = await recordKernelReservationTerminalIntent(
        kv,
        scope,
        owner,
        repo,
        windowCreatedAtMs,
        requestId,
        route,
        state,
      );
      if (nextLeaseExpiresAtMs !== null) updateLease(nextLeaseExpiresAtMs);
      await settleKernelReservation(
        kv,
        scope,
        owner,
        repo,
        windowCreatedAtMs,
        requestId,
        route,
        state,
        terminalReason,
      );
    })().then(() => {
      terminalSettled = state;
      stopAfterSettlement();
    }).catch((error) => {
      scheduleSettlementRetry();
      throw error;
    }).finally(() => {
      if (terminalInFlight === current) terminalInFlight = null;
    });
    terminalInFlight = current;
    return current;
  }
  const settle = (state: "committed" | "released", reason: string): Promise<void> => {
    if (terminalIntent === null) {
      terminalIntent = state;
      terminalReason = state === "committed" ? "completed" : reason;
    }
    if (terminalIntent !== state) {
      return Promise.reject(new Error(`Kernel quota reservation terminal state is already ${terminalIntent}`));
    }
    if (terminalSettled === state) return Promise.resolve();
    return launchSettlement();
  };
  return {
    signal: leaseAbort.signal,
    commit: () => settle("committed", "completed"),
    release: (reason = "request_incomplete") => settle("released", reason),
  };
};

const reclaimExpiredKernelReservationUnlocked = async (
  kv: Deno.Kv,
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  window: KernelQuotaWindowV2,
  nowMs: number,
): Promise<boolean> => {
  let sawExpired = false;
  const prefix = kernelReservationWindowPrefix(scope, owner, repo, window.created_at_ms);
  for await (const entry of kv.list<KernelQuotaReservationRowV2>({ prefix }, { consistency: "strong" })) {
    const reservation = normalizeKernelQuotaReservationRowV2(entry.value, scope, owner, repo);
    const requestId = entry.key.at(-1);
    if (
      !reservation || typeof requestId !== "string" || requestId !== reservation.request_id ||
      reservation.window_created_at_ms !== window.created_at_ms
    ) throw new Error("Kernel quota reservation is malformed");
    if (reservation.state !== "reserved" || reservation.lease_expires_at_ms > nowMs) continue;
    sawExpired = true;
    const terminalState = reservation.terminal_intent ?? "released";
    for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
      const outcome = await settleKernelReservationUnlocked(
        kv,
        scope,
        owner,
        repo,
        window.created_at_ms,
        reservation.request_id,
        reservation.route,
        terminalState,
        terminalState === "committed" ? "completed" : "lease_expired",
        nowMs,
      );
      if (outcome === "conflict") continue;
      if (outcome === "invalid") throw new Error("Kernel quota reservation is malformed");
      return true;
    }
  }
  if (sawExpired) throw new Error("Kernel quota reservation changed concurrently");
  return false;
};

const normalizeKernelDefaultWindowCutoverV2 = (value: unknown): KernelDefaultWindowCutoverV2 | null => {
  if (
    !isRecord(value) || value.v !== 2 || typeof value.id !== "string" || !value.id ||
    !nonNegativeSafeInteger(value.created_at_ms) || !positiveSafeInteger(value.expires_at_ms) ||
    value.expires_at_ms <= value.created_at_ms
  ) return null;
  return value as KernelDefaultWindowCutoverV2;
};

const hasLiveDefaultBackedKernelReservations = async (kv: Deno.Kv): Promise<boolean> => {
  for (const scope of ["repo", "org"] as const) {
    const prefix = scope === "repo" ? KERNEL_REPO_WINDOW_V2_PREFIX : KERNEL_ORG_WINDOW_V2_PREFIX;
    for await (const listedEntry of kv.list<KernelQuotaWindowV2>({ prefix }, { consistency: "strong" })) {
      const ownerPart = listedEntry.key[prefix.length];
      const repoPart = listedEntry.key[prefix.length + 1];
      if (typeof ownerPart !== "string" || !ownerPart) throw new Error("Kernel quota window key is malformed");
      const owner = ownerPart;
      const repo = scope === "repo" && typeof repoPart === "string" && repoPart ? repoPart : undefined;
      if (scope === "repo" && !repo) throw new Error("Kernel repo quota window key is malformed");
      const hasLive = await withKernelQuotaLock(scope, owner, repo, async () => {
        const policyKey = scope === "repo" ? kernelRepoPolicyKey(owner, repo!) : kernelOrgPolicyKey(owner);
        const windowKey = scope === "repo" ? kernelRepoWindowKey(owner, repo!) : kernelOrgWindowKey(owner);
        const policyEntry = await kv.get<KernelQuotaPolicyV2>(policyKey, { consistency: "strong" });
        if (policyEntry.value !== null) {
          if (!normalizeKernelQuotaPolicyV2(policyEntry.value, scope, owner, repo)) {
            throw new Error("Kernel quota policy is malformed");
          }
          return false;
        }
        for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
          const nowMs = Date.now();
          const windowEntry = await kv.get<KernelQuotaWindowV2>(windowKey, { consistency: "strong" });
          const window = normalizeKernelQuotaWindowV2(windowEntry.value, scope, owner, repo);
          if (windowEntry.value !== null && !window) throw new Error("Kernel quota window is malformed");
          if (!window || window.usage_reset_at_ms <= nowMs || window.reserved_requests === 0) return false;
          if (!await reclaimExpiredKernelReservationUnlocked(kv, scope, owner, repo, window, nowMs)) return true;
        }
        throw new Error("Kernel quota reservations changed concurrently");
      });
      if (hasLive) return true;
    }
  }
  return false;
};

export const releaseKernelDefaultWindowCutover = async (
  kv: Deno.Kv,
  guard: KernelDefaultWindowCutoverGuard,
): Promise<void> => {
  try {
    await kv.atomic()
      .check(guard.entry)
      .delete(guard.key)
      .commit();
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to release Kernel default-window cutover guard:", error);
  }
};

export const acquireKernelDefaultWindowCutover = async (
  kv: Deno.Kv,
  expectedLimitEntry: Deno.KvEntryMaybe<number>,
  expectedWindowEntry: Deno.KvEntryMaybe<number>,
): Promise<KernelDefaultWindowCutoverDecision> => {
  let guard: KernelDefaultWindowCutoverGuard | null = null;
  try {
    const markerEntry = await kv.get<KernelDefaultWindowCutoverV2>(KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, {
      consistency: "strong",
    });
    if (markerEntry.value !== null) {
      return {
        ok: false,
        reason: normalizeKernelDefaultWindowCutoverV2(markerEntry.value) ? "concurrent_change" : "unavailable",
      };
    }
    const nowMs = Date.now();
    const marker: KernelDefaultWindowCutoverV2 = {
      v: 2,
      id: crypto.randomUUID(),
      created_at_ms: nowMs,
      expires_at_ms: nowMs + KERNEL_DEFAULT_WINDOW_CUTOVER_LEASE_MS,
    };
    const acquired = await kv.atomic()
      .check(markerEntry)
      .check(expectedLimitEntry)
      .check(expectedWindowEntry)
      .set(KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, marker, { expireIn: KERNEL_DEFAULT_WINDOW_CUTOVER_LEASE_MS })
      .commit();
    if (!acquired.ok) return { ok: false, reason: "concurrent_change" };
    const acquiredEntry = await kv.get<KernelDefaultWindowCutoverV2>(KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, {
      consistency: "strong",
    });
    const acquiredMarker = normalizeKernelDefaultWindowCutoverV2(acquiredEntry.value);
    if (!acquiredMarker || acquiredMarker.id !== marker.id || acquiredEntry.versionstamp === null) {
      return { ok: false, reason: "unavailable" };
    }
    guard = {
      key: KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY,
      entry: acquiredEntry as Deno.KvEntry<KernelDefaultWindowCutoverV2>,
    };
    if (await hasLiveDefaultBackedKernelReservations(kv)) {
      await releaseKernelDefaultWindowCutover(kv, guard);
      return { ok: false, reason: "active_reservations" };
    }
    return { ok: true, guard };
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to prepare Kernel default-window cutover:", error);
    if (guard) await releaseKernelDefaultWindowCutover(kv, guard);
    return { ok: false, reason: "unavailable" };
  }
};

const kernelLimitBlocked = (
  message: string,
  code = "rate_limit_exceeded",
): KernelQuotaReservationDecision => ({
  ok: false,
  response: openaiError(429, message, code),
});

const quotaExceeded = (window: KernelQuotaWindowV2, limit: number): KernelQuotaReservationDecision => {
  const admitted = window.usage_requests + window.reserved_requests;
  return kernelLimitBlocked(
    `Usage limit exceeded (${admitted}/${limit}). Resets at ${new Date(window.usage_reset_at_ms).toISOString()}`,
  );
};

const reserveLimit = async (
  scope: KernelQuotaScope,
  owner: string,
  repo: string | undefined,
  requestId: string,
  route: string,
  options: Readonly<{
    kv?: Deno.Kv | null;
    nowMs?: number;
    renewalIntervalMs?: number;
    expectedRepoPolicy?: "present" | "absent";
    guardedRepo?: string;
  }> = {},
): Promise<KernelQuotaReservationDecision> => {
  try {
    const kv = options.kv === undefined ? await getKv() : options.kv;
    if (!kv) return kernelQuotaUnavailable();
    if (!requestId || !route) return kernelQuotaUnavailable("Kernel quota reservation requires a request id and route");
    return await withKernelQuotaLock(scope, owner, repo, async () => {
      let reclaimedExpiredReservation = false;
      for (let attempt = 0; attempt < MAX_KV_RETRIES; attempt += 1) {
        const nowMs = options.nowMs ?? Date.now();
        const policyKey = scope === "repo" ? kernelRepoPolicyKey(owner, repo!) : kernelOrgPolicyKey(owner);
        const windowKey = scope === "repo" ? kernelRepoWindowKey(owner, repo!) : kernelOrgWindowKey(owner);
        const [defaults, policyEntry, windowEntry, repoGuardEntry, defaultCutoverEntry] = await Promise.all([
          loadDefaults(kv),
          kv.get<KernelQuotaPolicyV2>(policyKey, { consistency: "strong" }),
          kv.get<KernelQuotaWindowV2>(windowKey, { consistency: "strong" }),
          options.expectedRepoPolicy === "absent" && options.guardedRepo
            ? kv.get<KernelQuotaPolicyV2>(kernelRepoPolicyKey(owner, options.guardedRepo), { consistency: "strong" })
            : Promise.resolve(null),
          kv.get<KernelDefaultWindowCutoverV2>(KERNEL_DEFAULT_WINDOW_CUTOVER_V2_KEY, { consistency: "strong" }),
        ]);
        if (options.expectedRepoPolicy === "present" && policyEntry.value === null) {
          return kernelQuotaUnavailable("Kernel quota policy scope changed; retry");
        }
        if (options.expectedRepoPolicy === "absent" && repoGuardEntry && repoGuardEntry.value !== null) {
          return kernelQuotaUnavailable("Kernel quota policy scope changed; retry");
        }
        const effective = policyFor(policyEntry, scope, owner, repo, defaults);
        if (effective.source === "default" && defaultCutoverEntry.value !== null) {
          return kernelQuotaUnavailable("Kernel quota defaults are changing; retry");
        }
        if (isExpired(effective.expiresAtMs, nowMs)) {
          return kernelLimitBlocked("Kernel auth usage limit expired; update it via /admin/kernel-usage.");
        }
        if (effective.limit === 0) {
          return kernelLimitBlocked("Kernel auth usage limit is 0; update it via /admin/kernel-usage.");
        }
        const existingWindow = normalizeKernelQuotaWindowV2(windowEntry.value, scope, owner, repo);
        if (windowEntry.value !== null && !existingWindow) {
          return kernelQuotaUnavailable("Kernel quota aggregate is malformed");
        }
        const resolved = windowForEffectivePolicy(existingWindow, scope, owner, repo, effective.windowMs, nowMs);
        const reservationKey = kernelReservationKey(scope, owner, repo, resolved.window.created_at_ms, requestId);
        const reservationEntry = await kv.get<KernelQuotaReservationRowV2>(reservationKey, { consistency: "strong" });
        if (reservationEntry.value !== null) {
          const existingReservation = normalizeKernelQuotaReservationRowV2(
            reservationEntry.value,
            scope,
            owner,
            repo,
          );
          if (
            !existingReservation || existingReservation.request_id !== requestId ||
            existingReservation.route !== route ||
            existingReservation.window_created_at_ms !== resolved.window.created_at_ms
          ) return kernelQuotaUnavailable("Kernel quota request identity conflicts");
          return kernelQuotaUnavailable("Kernel quota request already has a reservation");
        }
        if (
          effective.limit !== API_KEY_NO_USAGE_LIMIT &&
          resolved.window.usage_requests + resolved.window.reserved_requests >= effective.limit
        ) {
          if (reclaimedExpiredReservation) return quotaExceeded(resolved.window, effective.limit);
          try {
            const reclaimed = await reclaimExpiredKernelReservationUnlocked(
              kv,
              scope,
              owner,
              repo,
              resolved.window,
              nowMs,
            );
            reclaimedExpiredReservation = true;
            if (!reclaimed) return quotaExceeded(resolved.window, effective.limit);
          } catch (error) {
            console.warn("[ai.ubq.fi] Failed to reclaim kernel quota lease:", error);
            return kernelQuotaUnavailable();
          }
          continue;
        }
        const reservation: KernelQuotaReservationRowV2 = {
          v: 2,
          scope,
          owner,
          ...(scope === "repo" ? { repo } : {}),
          request_id: requestId,
          route,
          window_created_at_ms: resolved.window.created_at_ms,
          window_reset_at_ms: resolved.window.usage_reset_at_ms,
          state: "reserved",
          terminal_intent: null,
          reserved_at_ms: nowMs,
          lease_expires_at_ms: nowMs + KERNEL_QUOTA_RESERVATION_LEASE_MS,
          committed_at_ms: null,
          released_at_ms: null,
          release_reason: null,
        };
        const reservedWindow: KernelQuotaWindowV2 = {
          ...resolved.window,
          reserved_requests: resolved.window.reserved_requests + 1,
          updated_at_ms: nowMs,
        };
        const retentionMs = kernelReservationRetentionMs(reservedWindow.usage_reset_at_ms, nowMs);
        let atomic = kv.atomic()
          .check(policyEntry)
          .check(windowEntry)
          .check(reservationEntry);
        if (repoGuardEntry) atomic = atomic.check(repoGuardEntry);
        if (effective.source === "default") {
          atomic = atomic
            .check(defaults.limitEntry)
            .check(defaults.windowEntry)
            .check(defaultCutoverEntry);
        }
        const committed = await atomic
          .set(windowKey, reservedWindow, { expireIn: retentionMs })
          .set(reservationKey, reservation, { expireIn: retentionMs })
          .commit();
        if (committed.ok) {
          return {
            ok: true,
            reservation: kernelReservationContext(
              kv,
              scope,
              owner,
              repo,
              reservedWindow.created_at_ms,
              requestId,
              route,
              reservation.lease_expires_at_ms,
              options.renewalIntervalMs,
            ),
          };
        }
      }
      return kernelQuotaUnavailable("Kernel quota changed concurrently; retry");
    });
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to reserve kernel quota:", error);
    return kernelQuotaUnavailable();
  }
};

export const reserveKernelUsageLimit = async (
  owner: string,
  repo: string,
  requestId: string,
  route: string,
  options: Readonly<{ kv?: Deno.Kv | null; nowMs?: number; renewalIntervalMs?: number }> = {},
): Promise<KernelQuotaReservationDecision> => await reserveLimit("repo", owner, repo, requestId, route, options);

export const reserveKernelOrgUsageLimit = async (
  owner: string,
  requestId: string,
  route: string,
  options: Readonly<{ kv?: Deno.Kv | null; nowMs?: number; renewalIntervalMs?: number }> = {},
): Promise<KernelQuotaReservationDecision> => await reserveLimit("org", owner, undefined, requestId, route, options);

export const reserveEffectiveKernelUsageLimit = async (
  owner: string,
  repo: string,
  requestId: string,
  route: string,
  options: Readonly<{ kv?: Deno.Kv | null; nowMs?: number; renewalIntervalMs?: number }> = {},
): Promise<KernelQuotaReservationDecision> => {
  try {
    const kv = options.kv === undefined ? await getKv() : options.kv;
    if (!kv) return kernelQuotaUnavailable();
    const state = await readKernelQuotaPolicyState(kv, owner, repo);
    return state.limit_scope === "repo"
      ? await reserveLimit("repo", owner, repo, requestId, route, {
        kv,
        nowMs: options.nowMs,
        renewalIntervalMs: options.renewalIntervalMs,
        expectedRepoPolicy: "present",
      })
      : await reserveLimit("org", owner, undefined, requestId, route, {
        kv,
        nowMs: options.nowMs,
        renewalIntervalMs: options.renewalIntervalMs,
        expectedRepoPolicy: "absent",
        guardedRepo: repo,
      });
  } catch (error) {
    console.warn("[ai.ubq.fi] Failed to reserve effective kernel quota:", error);
    return kernelQuotaUnavailable();
  }
};
