import { getKv } from "./kv.ts";
import { getString, isRecord } from "./utils.ts";
import { YUNWU_BASE_URL, type YunwuFetch } from "./yunwu.ts";

export const YUNWU_QUOTA_FRESH_MS = 5 * 60_000;
export const YUNWU_QUOTA_RETENTION_MS = 24 * 60 * 60_000;
export const YUNWU_QUOTA_REFRESH_LEASE_MS = 15_000;
export const YUNWU_QUOTA_COLD_WAIT_MS = 2_000;
export const YUNWU_QUOTA_FETCH_TIMEOUT_MS = 10_000;

export const YUNWU_SYSTEM_TOKEN_ENV = "YUNWU_SYSTEM_TOKEN";
export const YUNWU_USER_ID_ENV = "YUNWU_USER_ID";

export const YUNWU_QUOTA_STATE_KEY = ["uos_ai", "yunwu_quota", "v1", "state"] as const;
export const YUNWU_QUOTA_REFRESH_LEASE_KEY = ["uos_ai", "yunwu_quota", "v1", "refresh_lease"] as const;
export const YUNWU_QUOTA_INVALIDATION_KEY = ["uos_ai", "yunwu_quota", "v1", "invalidation"] as const;

const YUNWU_ACCOUNT_URL = `${YUNWU_BASE_URL}/api/user/self`;
const YUNWU_TOPUP_RECORDS_URL = `${YUNWU_BASE_URL}/api/user/topuprecords?page=1&page_size=10`;
const YUNWU_STATUS_URL = `${YUNWU_BASE_URL}/api/status`;

export type YunwuAccountCredentials = Readonly<{
  systemToken: string;
  userId: string;
}>;

export type YunwuRefillObservation = Readonly<{
  id: string;
  amount_credits: number;
  completed_at_ms: number;
}>;

export type YunwuQuotaObservation = Readonly<{
  balance_quota: number;
  used_quota: number;
  quota_per_credit: number;
  observed_at_ms: number;
  latest_refill: YunwuRefillObservation | null;
}>;

export type YunwuQuotaConfidence = "provisional" | "refill_observed" | "inferred_adjustment";

export type YunwuQuotaState = Readonly<{
  current_balance_quota: number;
  post_refill_baseline_quota: number;
  last_observed_used_quota: number;
  quota_per_credit: number;
  observed_at_ms: number;
  cycle_started_at_ms: number;
  confidence: YunwuQuotaConfidence;
  last_known_debits_quota: number;
  last_inferred_credit_quota: number;
  last_credit_at_ms: number | null;
  latest_refill_id: string | null;
  latest_refill_amount_credits: number | null;
  latest_refill_completed_at_ms: number | null;
}>;

export type YunwuQuotaCacheState = "fresh" | "refreshed" | "stale" | "wait";

export type YunwuQuotaSnapshot = Readonly<{
  state: YunwuQuotaState;
  cache_state: YunwuQuotaCacheState;
  balance_credits: number;
  baseline_credits: number;
  last_inferred_credit_credits: number;
  remaining_percent: number | null;
  used_percent: number | null;
}>;

export type YunwuQuotaDiagnostics = Readonly<{
  configured: boolean;
  available: boolean;
  cache_state: YunwuQuotaCacheState | null;
  confidence: YunwuQuotaConfidence | null;
  balance_credits: number | null;
  baseline_credits: number | null;
  remaining_percent: number | null;
  used_percent: number | null;
  observed_at_ms: number | null;
  cycle_started_at_ms: number | null;
  last_known_debits_credits: number | null;
  last_inferred_credit_credits: number | null;
  last_credit_at_ms: number | null;
  latest_refill_id: string | null;
  latest_refill_amount_credits: number | null;
  latest_refill_completed_at_ms: number | null;
}>;

export type GetYunwuQuotaSnapshotOptions = Readonly<{
  kv?: Deno.Kv | null;
  fetcher?: YunwuFetch;
  now?: () => number;
  signal?: AbortSignal;
  createLeaseOwner?: () => string;
}>;

type RefreshLease = Readonly<{
  owner: string;
  lease_until_ms: number;
}>;

type QuotaInvalidation = Readonly<{
  invalidated_at_ms: number;
}>;

type JsonRecord = Record<string, unknown>;

const isSafeInteger = (value: unknown): value is number => typeof value === "number" && Number.isSafeInteger(value);

const isNonNegativeSafeInteger = (value: unknown): value is number => isSafeInteger(value) && value >= 0;

const isPositiveSafeInteger = (value: unknown): value is number => isSafeInteger(value) && value > 0;

const isNonNegativeFiniteNumber = (value: unknown): value is number =>
  typeof value === "number" && Number.isFinite(value) && value >= 0;

const isConfidence = (value: unknown): value is YunwuQuotaConfidence =>
  value === "provisional" || value === "refill_observed" || value === "inferred_adjustment";

const isQuotaInvalidation = (value: unknown): value is QuotaInvalidation =>
  isRecord(value) && isNonNegativeSafeInteger(value.invalidated_at_ms);

const parseCredentials = (credentials: YunwuAccountCredentials): YunwuAccountCredentials | null => {
  const systemToken = credentials.systemToken.trim();
  const userId = credentials.userId.trim();
  if (!systemToken || /\s/.test(systemToken)) return null;
  if (!/^\d+$/.test(userId)) return null;
  return { systemToken, userId };
};

const getEnv = (key: string): string | undefined => {
  try {
    return Deno.env.get(key);
  } catch {
    return undefined;
  }
};

export const readYunwuAccountCredentials = (): YunwuAccountCredentials | null =>
  parseCredentials({
    systemToken: getEnv(YUNWU_SYSTEM_TOKEN_ENV) ?? "",
    userId: getEnv(YUNWU_USER_ID_ENV) ?? "",
  });

const authenticatedHeaders = (credentials: YunwuAccountCredentials): Headers => {
  const headers = new Headers({
    Accept: "application/json",
    Authorization: `Bearer ${credentials.systemToken}`,
    "New-API-User": credentials.userId,
  });
  return headers;
};

const successfulEnvelopeData = (value: unknown): JsonRecord | null => {
  if (!isRecord(value) || value.success !== true || !isRecord(value.data)) return null;
  return value.data;
};

const fetchJson = async (
  url: string,
  fetcher: YunwuFetch,
  headers: Headers,
  signal: AbortSignal,
): Promise<unknown> => {
  const response = await fetcher(url, {
    method: "GET",
    headers,
    redirect: "manual",
    signal,
  });
  if (!response.ok) throw new Error(`YunWu account API returned HTTP ${response.status}`);
  const contentType = response.headers.get("Content-Type")?.toLowerCase() ?? "";
  if (!contentType.includes("application/json")) throw new Error("YunWu account API returned non-JSON data");
  try {
    return await response.json() as unknown;
  } catch {
    throw new Error("YunWu account API returned invalid JSON");
  }
};

const parseLatestRefill = (data: JsonRecord): YunwuRefillObservation | null => {
  if (!Array.isArray(data.records)) return null;
  const refills: YunwuRefillObservation[] = [];
  for (const value of data.records) {
    if (!isRecord(value) || getString(value.status)?.toLowerCase() !== "success") continue;
    const id = typeof value.id === "string" ? value.id.trim() : isSafeInteger(value.id) ? String(value.id) : "";
    const completedAtSeconds = value.complete_time;
    if (!id || !isNonNegativeFiniteNumber(value.amount) || !isPositiveSafeInteger(completedAtSeconds)) continue;
    const completedAtMs = completedAtSeconds * 1000;
    if (!Number.isSafeInteger(completedAtMs)) continue;
    refills.push({
      id,
      amount_credits: value.amount,
      completed_at_ms: completedAtMs,
    });
  }
  refills.sort((left, right) => right.completed_at_ms - left.completed_at_ms || right.id.localeCompare(left.id));
  return refills[0] ?? null;
};

const combinedSignal = (signal: AbortSignal | undefined): AbortSignal => {
  const timeout = AbortSignal.timeout(YUNWU_QUOTA_FETCH_TIMEOUT_MS);
  return signal ? AbortSignal.any([signal, timeout]) : timeout;
};

export const fetchYunwuQuotaObservation = async (
  credentialsInput: YunwuAccountCredentials,
  options: Readonly<{
    fetcher?: YunwuFetch;
    now?: () => number;
    signal?: AbortSignal;
  }> = {},
): Promise<YunwuQuotaObservation> => {
  const credentials = parseCredentials(credentialsInput);
  if (!credentials) throw new Error("YunWu account credentials are invalid");
  const fetcher = options.fetcher ?? fetch;
  const signal = combinedSignal(options.signal);
  const [accountEnvelope, topupsEnvelope, statusEnvelope] = await Promise.all([
    fetchJson(YUNWU_ACCOUNT_URL, fetcher, authenticatedHeaders(credentials), signal),
    fetchJson(YUNWU_TOPUP_RECORDS_URL, fetcher, authenticatedHeaders(credentials), signal),
    fetchJson(YUNWU_STATUS_URL, fetcher, new Headers({ Accept: "application/json" }), signal),
  ]);
  const account = successfulEnvelopeData(accountEnvelope);
  const topups = successfulEnvelopeData(topupsEnvelope);
  const status = successfulEnvelopeData(statusEnvelope);
  if (!account || !topups || !status) throw new Error("YunWu account API returned an invalid envelope");
  if (
    !isSafeInteger(account.quota) ||
    !isNonNegativeSafeInteger(account.used_quota) ||
    !isPositiveSafeInteger(status.quota_per_unit)
  ) {
    throw new Error("YunWu account API returned invalid quota data");
  }
  const observedAtMs = Math.trunc((options.now ?? Date.now)());
  if (!isNonNegativeSafeInteger(observedAtMs)) throw new Error("YunWu quota observation clock is invalid");
  return {
    balance_quota: account.quota,
    used_quota: account.used_quota,
    quota_per_credit: status.quota_per_unit,
    observed_at_ms: observedAtMs,
    latest_refill: parseLatestRefill(topups),
  };
};

export const updateYunwuQuotaState = (
  previous: YunwuQuotaState | null,
  observation: YunwuQuotaObservation,
): YunwuQuotaState => {
  if (!previous) {
    const refillBaselineQuota = observation.latest_refill
      ? Math.round(observation.latest_refill.amount_credits * observation.quota_per_credit)
      : 0;
    const postRefillBaselineQuota = Number.isSafeInteger(refillBaselineQuota) && refillBaselineQuota > 0
      ? Math.max(observation.balance_quota, refillBaselineQuota)
      : observation.balance_quota;
    return {
      current_balance_quota: observation.balance_quota,
      post_refill_baseline_quota: postRefillBaselineQuota,
      last_observed_used_quota: observation.used_quota,
      quota_per_credit: observation.quota_per_credit,
      observed_at_ms: observation.observed_at_ms,
      cycle_started_at_ms: observation.latest_refill?.completed_at_ms ?? observation.observed_at_ms,
      confidence: "provisional",
      last_known_debits_quota: 0,
      last_inferred_credit_quota: 0,
      last_credit_at_ms: null,
      latest_refill_id: observation.latest_refill?.id ?? null,
      latest_refill_amount_credits: observation.latest_refill?.amount_credits ?? null,
      latest_refill_completed_at_ms: observation.latest_refill?.completed_at_ms ?? null,
    };
  }

  const usedCounterAdvanced = observation.used_quota >= previous.last_observed_used_quota;
  const knownDebits = usedCounterAdvanced ? observation.used_quota - previous.last_observed_used_quota : 0;
  const expectedBalance = previous.current_balance_quota - knownDebits;
  const inferredCredit = usedCounterAdvanced
    ? Math.max(0, observation.balance_quota - expectedBalance)
    : Math.max(0, observation.balance_quota - previous.current_balance_quota);
  const newRefillObserved = Boolean(
    observation.latest_refill && observation.latest_refill.id !== previous.latest_refill_id,
  );
  const creditObserved = inferredCredit > 0 || newRefillObserved;
  const refillBaselineQuota = newRefillObserved && observation.latest_refill
    ? Math.round(observation.latest_refill.amount_credits * observation.quota_per_credit)
    : 0;
  const balancePlusKnownDebits = observation.balance_quota + knownDebits;
  const reconstructedRefillCapacityQuota = newRefillObserved && knownDebits > 0 &&
      Number.isSafeInteger(balancePlusKnownDebits)
    ? balancePlusKnownDebits
    : observation.balance_quota;
  const postRefillBaselineQuota = creditObserved
    ? Math.max(
      reconstructedRefillCapacityQuota,
      Number.isSafeInteger(refillBaselineQuota) && refillBaselineQuota > 0 ? refillBaselineQuota : 0,
    )
    : previous.post_refill_baseline_quota;

  return {
    current_balance_quota: observation.balance_quota,
    post_refill_baseline_quota: postRefillBaselineQuota,
    last_observed_used_quota: observation.used_quota,
    quota_per_credit: observation.quota_per_credit,
    observed_at_ms: observation.observed_at_ms,
    cycle_started_at_ms: creditObserved
      ? observation.latest_refill?.completed_at_ms ?? observation.observed_at_ms
      : previous.cycle_started_at_ms,
    confidence: creditObserved ? (newRefillObserved ? "refill_observed" : "inferred_adjustment") : previous.confidence,
    last_known_debits_quota: knownDebits,
    last_inferred_credit_quota: creditObserved ? inferredCredit : 0,
    last_credit_at_ms: creditObserved ? observation.observed_at_ms : previous.last_credit_at_ms,
    latest_refill_id: observation.latest_refill?.id ?? previous.latest_refill_id,
    latest_refill_amount_credits: observation.latest_refill?.amount_credits ??
      previous.latest_refill_amount_credits,
    latest_refill_completed_at_ms: observation.latest_refill?.completed_at_ms ??
      previous.latest_refill_completed_at_ms,
  };
};

export const isYunwuQuotaState = (value: unknown): value is YunwuQuotaState => {
  if (!isRecord(value)) return false;
  return isSafeInteger(value.current_balance_quota) &&
    isSafeInteger(value.post_refill_baseline_quota) &&
    isNonNegativeSafeInteger(value.last_observed_used_quota) &&
    isPositiveSafeInteger(value.quota_per_credit) &&
    isNonNegativeSafeInteger(value.observed_at_ms) &&
    isNonNegativeSafeInteger(value.cycle_started_at_ms) &&
    isConfidence(value.confidence) &&
    isNonNegativeSafeInteger(value.last_known_debits_quota) &&
    isNonNegativeSafeInteger(value.last_inferred_credit_quota) &&
    (value.last_credit_at_ms === null || isNonNegativeSafeInteger(value.last_credit_at_ms)) &&
    (value.latest_refill_id === null || typeof value.latest_refill_id === "string") &&
    (value.latest_refill_amount_credits === null || isNonNegativeFiniteNumber(value.latest_refill_amount_credits)) &&
    (value.latest_refill_completed_at_ms === null ||
      isNonNegativeSafeInteger(value.latest_refill_completed_at_ms));
};

const toSnapshot = (state: YunwuQuotaState, cacheState: YunwuQuotaCacheState): YunwuQuotaSnapshot => {
  const balanceCredits = state.current_balance_quota / state.quota_per_credit;
  const baselineCredits = state.post_refill_baseline_quota / state.quota_per_credit;
  const remainingPercent = state.post_refill_baseline_quota > 0
    ? Math.min(100, Math.max(0, state.current_balance_quota / state.post_refill_baseline_quota * 100))
    : null;
  return {
    state,
    cache_state: cacheState,
    balance_credits: balanceCredits,
    baseline_credits: baselineCredits,
    last_inferred_credit_credits: state.last_inferred_credit_quota / state.quota_per_credit,
    remaining_percent: remainingPercent,
    used_percent: remainingPercent === null ? null : 100 - remainingPercent,
  };
};

const loadRetainedState = async (
  kv: Deno.Kv,
  nowMs: number,
): Promise<Deno.KvEntryMaybe<YunwuQuotaState> | null> => {
  const entry = await kv.get<YunwuQuotaState>(YUNWU_QUOTA_STATE_KEY);
  if (!isYunwuQuotaState(entry.value)) return null;
  if (nowMs - entry.value.observed_at_ms >= YUNWU_QUOTA_RETENTION_MS) return null;
  return entry;
};

const loadInvalidation = (kv: Deno.Kv): Promise<Deno.KvEntryMaybe<unknown>> =>
  kv.get<unknown>(YUNWU_QUOTA_INVALIDATION_KEY);

const quotaInvalidationValue = (value: unknown): QuotaInvalidation | null => isQuotaInvalidation(value) ? value : null;

const isInvalidated = (state: YunwuQuotaState, invalidation: QuotaInvalidation | null): boolean =>
  Boolean(invalidation && invalidation.invalidated_at_ms >= state.observed_at_ms);

const acquireRefreshLease = async (
  kv: Deno.Kv,
  owner: string,
  nowMs: number,
): Promise<boolean> => {
  const entry = await kv.get<RefreshLease>(YUNWU_QUOTA_REFRESH_LEASE_KEY);
  if (entry.value && entry.value.lease_until_ms > nowMs) return false;
  const lease: RefreshLease = { owner, lease_until_ms: nowMs + YUNWU_QUOTA_REFRESH_LEASE_MS };
  return (await kv.atomic()
    .check(entry)
    .set(YUNWU_QUOTA_REFRESH_LEASE_KEY, lease, { expireIn: YUNWU_QUOTA_REFRESH_LEASE_MS * 2 })
    .commit()).ok;
};

const releaseRefreshLease = async (kv: Deno.Kv, owner: string): Promise<void> => {
  try {
    const entry = await kv.get<RefreshLease>(YUNWU_QUOTA_REFRESH_LEASE_KEY);
    if (entry.value?.owner === owner) {
      await kv.atomic().check(entry).delete(YUNWU_QUOTA_REFRESH_LEASE_KEY).commit();
    }
  } catch (error) {
    console.warn("[ai.ubq.fi] YunWu quota refresh lease release failed:", error);
  }
};

const waitForColdState = async (kv: Deno.Kv): Promise<YunwuQuotaState | null> => {
  const deadline = Date.now() + YUNWU_QUOTA_COLD_WAIT_MS;
  while (Date.now() < deadline) {
    await new Promise((resolve) => setTimeout(resolve, 50));
    const entry = await loadRetainedState(kv, Date.now());
    if (entry?.value) return entry.value;
  }
  return null;
};

export const getYunwuQuotaSnapshot = async (
  credentials: YunwuAccountCredentials,
  options: GetYunwuQuotaSnapshotOptions = {},
): Promise<YunwuQuotaSnapshot | null> => {
  const kv = options.kv === undefined ? await getKv() : options.kv;
  if (!kv || !parseCredentials(credentials)) return null;
  const now = options.now ?? Date.now;
  const nowMs = Math.trunc(now());
  const [cachedEntry, cachedInvalidationEntry] = await Promise.all([
    loadRetainedState(kv, nowMs).catch((error) => {
      console.warn("[ai.ubq.fi] YunWu quota cache read failed:", error);
      return null;
    }),
    loadInvalidation(kv).catch((error) => {
      console.warn("[ai.ubq.fi] YunWu quota invalidation read failed:", error);
      return null;
    }),
  ]);
  const cached = cachedEntry?.value ?? null;
  const cachedInvalidated = cached
    ? isInvalidated(cached, quotaInvalidationValue(cachedInvalidationEntry?.value))
    : false;
  if (cached && !cachedInvalidated && nowMs - cached.observed_at_ms < YUNWU_QUOTA_FRESH_MS) {
    return toSnapshot(cached, "fresh");
  }

  const owner = (options.createLeaseOwner ?? (() => crypto.randomUUID()))();
  const acquired = await acquireRefreshLease(kv, owner, nowMs).catch((error) => {
    console.warn("[ai.ubq.fi] YunWu quota refresh lease acquisition failed:", error);
    return false;
  });
  if (!acquired) {
    if (cached) return toSnapshot(cached, "stale");
    const waited = await waitForColdState(kv).catch(() => null);
    return waited ? toSnapshot(waited, "wait") : null;
  }

  try {
    const [stateEntry, refreshInvalidationEntry] = await Promise.all([
      kv.get<YunwuQuotaState>(YUNWU_QUOTA_STATE_KEY),
      loadInvalidation(kv),
    ]);
    const observation = await fetchYunwuQuotaObservation(credentials, {
      fetcher: options.fetcher,
      now,
      signal: options.signal,
    });
    const leaseEntry = await kv.get<RefreshLease>(YUNWU_QUOTA_REFRESH_LEASE_KEY);
    if (leaseEntry.value?.owner !== owner || leaseEntry.value.lease_until_ms <= observation.observed_at_ms) {
      const replacement = await loadRetainedState(kv, Date.now()).catch(() => null);
      return replacement?.value ? toSnapshot(replacement.value, "stale") : cached ? toSnapshot(cached, "stale") : null;
    }
    const previous = isYunwuQuotaState(stateEntry.value) ? stateEntry.value : null;
    const state = updateYunwuQuotaState(previous, observation);
    const committed = await kv.atomic()
      .check(stateEntry)
      .check(refreshInvalidationEntry)
      .check(leaseEntry)
      .set(YUNWU_QUOTA_STATE_KEY, state, { expireIn: YUNWU_QUOTA_RETENTION_MS })
      .delete(YUNWU_QUOTA_INVALIDATION_KEY)
      .delete(YUNWU_QUOTA_REFRESH_LEASE_KEY)
      .commit();
    if (committed.ok) return toSnapshot(state, "refreshed");
    const replacement = await loadRetainedState(kv, Date.now()).catch(() => null);
    return replacement?.value ? toSnapshot(replacement.value, "stale") : cached ? toSnapshot(cached, "stale") : null;
  } catch (error) {
    if (options.signal?.aborted) throw error;
    console.warn(
      "[ai.ubq.fi] YunWu quota refresh failed:",
      error instanceof Error ? error.message : String(error),
    );
    return cached ? toSnapshot(cached, "stale") : null;
  } finally {
    await releaseRefreshLease(kv, owner);
  }
};

export const getCachedYunwuQuotaSnapshot = async (
  options: Pick<GetYunwuQuotaSnapshotOptions, "kv" | "now"> = {},
): Promise<YunwuQuotaSnapshot | null> => {
  const kv = options.kv === undefined ? await getKv() : options.kv;
  if (!kv) return null;
  const nowMs = Math.trunc((options.now ?? Date.now)());
  try {
    const [stateEntry, invalidationEntry] = await Promise.all([
      loadRetainedState(kv, nowMs),
      loadInvalidation(kv),
    ]);
    const state = stateEntry?.value ?? null;
    if (!state) return null;
    const fresh = !isInvalidated(state, quotaInvalidationValue(invalidationEntry.value)) &&
      nowMs - state.observed_at_ms < YUNWU_QUOTA_FRESH_MS;
    return toSnapshot(state, fresh ? "fresh" : "stale");
  } catch (error) {
    console.warn("[ai.ubq.fi] YunWu quota cache peek failed:", error);
    return null;
  }
};

export const invalidateYunwuQuotaSnapshot = async (
  options: Pick<GetYunwuQuotaSnapshotOptions, "kv" | "now"> = {},
): Promise<void> => {
  const kv = options.kv === undefined ? await getKv() : options.kv;
  if (!kv) return;
  const invalidatedAtMs = Math.trunc((options.now ?? Date.now)());
  if (!isNonNegativeSafeInteger(invalidatedAtMs)) throw new Error("YunWu quota invalidation clock is invalid");
  const committed = await kv.atomic()
    .set(
      YUNWU_QUOTA_INVALIDATION_KEY,
      { invalidated_at_ms: invalidatedAtMs } satisfies QuotaInvalidation,
      { expireIn: YUNWU_QUOTA_RETENTION_MS },
    )
    .commit();
  if (!committed.ok) throw new Error("Deno KV could not invalidate the YunWu quota snapshot");
};

export const getConfiguredYunwuQuotaSnapshot = async (
  options: GetYunwuQuotaSnapshotOptions = {},
): Promise<YunwuQuotaSnapshot | null> => {
  const credentials = readYunwuAccountCredentials();
  return credentials ? await getYunwuQuotaSnapshot(credentials, options) : null;
};

export const getCachedConfiguredYunwuQuotaSnapshot = async (
  options: Pick<GetYunwuQuotaSnapshotOptions, "kv" | "now"> = {},
): Promise<YunwuQuotaSnapshot | null> =>
  readYunwuAccountCredentials() ? await getCachedYunwuQuotaSnapshot(options) : null;

export const invalidateConfiguredYunwuQuotaSnapshot = async (
  options: Pick<GetYunwuQuotaSnapshotOptions, "kv" | "now"> = {},
): Promise<void> => {
  if (readYunwuAccountCredentials()) await invalidateYunwuQuotaSnapshot(options);
};

const unavailableDiagnostics = (configured: boolean): YunwuQuotaDiagnostics => ({
  configured,
  available: false,
  cache_state: null,
  confidence: null,
  balance_credits: null,
  baseline_credits: null,
  remaining_percent: null,
  used_percent: null,
  observed_at_ms: null,
  cycle_started_at_ms: null,
  last_known_debits_credits: null,
  last_inferred_credit_credits: null,
  last_credit_at_ms: null,
  latest_refill_id: null,
  latest_refill_amount_credits: null,
  latest_refill_completed_at_ms: null,
});

export const getYunwuQuotaDiagnostics = async (
  options: GetYunwuQuotaSnapshotOptions = {},
): Promise<YunwuQuotaDiagnostics> => {
  const credentials = readYunwuAccountCredentials();
  if (!credentials) return unavailableDiagnostics(false);
  const snapshot = await getYunwuQuotaSnapshot(credentials, options);
  if (!snapshot) return unavailableDiagnostics(true);
  return {
    configured: true,
    available: true,
    cache_state: snapshot.cache_state,
    confidence: snapshot.state.confidence,
    balance_credits: snapshot.balance_credits,
    baseline_credits: snapshot.baseline_credits,
    remaining_percent: snapshot.remaining_percent,
    used_percent: snapshot.used_percent,
    observed_at_ms: snapshot.state.observed_at_ms,
    cycle_started_at_ms: snapshot.state.cycle_started_at_ms,
    last_known_debits_credits: snapshot.state.last_known_debits_quota / snapshot.state.quota_per_credit,
    last_inferred_credit_credits: snapshot.last_inferred_credit_credits,
    last_credit_at_ms: snapshot.state.last_credit_at_ms,
    latest_refill_id: snapshot.state.latest_refill_id,
    latest_refill_amount_credits: snapshot.state.latest_refill_amount_credits,
    latest_refill_completed_at_ms: snapshot.state.latest_refill_completed_at_ms,
  };
};
