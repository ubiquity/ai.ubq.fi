import { apiKeyIdKey, MICROCREDITS_PER_CREDIT, PAID_FALLBACK_NO_LIMIT } from "./api_keys.ts";
import {
  admitPaidFallbackV3,
  markPaidFallbackTerminalV3,
  releasePaidFallbackBeforeProviderFetchV3,
  updatePaidFallbackRequestV3,
} from "./paid_fallback_ledger.ts";
import { loadFullCodexModelsSnapshot } from "./codex.ts";
import { getKv } from "./kv.ts";
import type { ApiKeyHashRecord, ApiKeyRecord } from "./types.ts";
import { getString, isRecord } from "./utils.ts";
import { initializeYunwuPricing, readYunwuApiKey, YunwuError } from "./yunwu.ts";

export type PaidFallbackPolicyFields = Pick<
  ApiKeyRecord,
  | "paid_fallback_enabled"
  | "paid_fallback_limit_microcredits"
  | "paid_fallback_spent_microcredits"
  | "paid_fallback_reserved_microcredits"
  | "paid_fallback_reservation_request_id"
  | "paid_fallback_model_ids"
  | "paid_fallback_quota_per_credit"
  | "paid_fallback_max_exposure_microcredits"
  | "paid_fallback_pricing_checked_at_ms"
>;

export type PaidFallbackReservation = Readonly<{
  key_id: string;
  request_id: string;
  created_at_ms: number;
  reserved_microcredits: number;
  quota_per_credit: number;
  window_reset_at_ms: number;
  quota_used_percent: number | null;
}>;

export type PaidFallbackReservationDecision =
  | { kind: "reserved"; reservation: PaidFallbackReservation }
  | {
    kind: "skip";
    reason: "disabled" | "provider_unconfigured" | "model_not_priced";
  }
  | {
    kind: "blocked";
    reason: "limit_exceeded" | "reconciliation_pending" | "invalid_policy" | "concurrent_update";
    reset_at_ms: number | null;
  };

type PaidFallbackEligibility =
  | { kind: "eligible"; unlimited: boolean }
  | Exclude<PaidFallbackReservationDecision, { kind: "reserved" }>;

const evaluatePaidFallbackEligibility = (
  record: ApiKeyRecord,
  model: string,
): PaidFallbackEligibility => {
  if (!record.paid_fallback_enabled) return { kind: "skip", reason: "disabled" };
  if (!record.paid_fallback_model_ids.includes(model)) {
    return { kind: "skip", reason: "model_not_priced" };
  }
  const unlimited = record.paid_fallback_limit_microcredits === PAID_FALLBACK_NO_LIMIT;
  const exposure = record.paid_fallback_max_exposure_microcredits?.[model] ?? null;
  if (
    (!unlimited && !isPositiveSafeInteger(record.paid_fallback_limit_microcredits)) ||
    !isPositiveSafeInteger(record.paid_fallback_quota_per_credit) ||
    (!unlimited && !isPositiveSafeInteger(exposure))
  ) {
    return { kind: "blocked", reason: "invalid_policy", reset_at_ms: record.usage_reset_at_ms };
  }
  return { kind: "eligible", unlimited };
};

export const defaultPaidFallbackPolicy = (): PaidFallbackPolicyFields => ({
  paid_fallback_enabled: false,
  paid_fallback_limit_microcredits: 0,
  paid_fallback_spent_microcredits: 0,
  paid_fallback_reserved_microcredits: 0,
  paid_fallback_reservation_request_id: null,
  paid_fallback_model_ids: [],
  paid_fallback_quota_per_credit: 0,
  paid_fallback_max_exposure_microcredits: {},
  paid_fallback_pricing_checked_at_ms: null,
});

const isNonNegativeSafeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const isPositiveSafeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0;

const isPaidFallbackLimit = (value: unknown): value is number =>
  value === PAID_FALLBACK_NO_LIMIT || isNonNegativeSafeInteger(value);

const isNullableString = (value: unknown): value is string | null =>
  value === null || (typeof value === "string" && value.trim().length > 0);

export const hasStrictPaidFallbackPolicy = (value: unknown): boolean => {
  if (!isRecord(value)) return false;
  if (typeof value.paid_fallback_enabled !== "boolean") return false;
  if (!isPaidFallbackLimit(value.paid_fallback_limit_microcredits)) return false;
  if (!isNonNegativeSafeInteger(value.paid_fallback_spent_microcredits)) return false;
  if (!isNonNegativeSafeInteger(value.paid_fallback_reserved_microcredits)) return false;
  if (!isNullableString(value.paid_fallback_reservation_request_id)) return false;

  const limit = value.paid_fallback_limit_microcredits;
  if (value.paid_fallback_enabled && limit !== PAID_FALLBACK_NO_LIMIT && !isPositiveSafeInteger(limit)) return false;
  return true;
};

export const hasStrictPaidFallbackKeyPolicy = (value: unknown): boolean => {
  if (!hasStrictPaidFallbackPolicy(value) || !isRecord(value)) return false;
  if (
    !Array.isArray(value.paid_fallback_model_ids) ||
    value.paid_fallback_model_ids.some((model) => typeof model !== "string" || !model.trim())
  ) {
    return false;
  }
  if (!isNonNegativeSafeInteger(value.paid_fallback_quota_per_credit)) return false;
  if (
    value.paid_fallback_pricing_checked_at_ms !== null &&
    !isPositiveSafeInteger(value.paid_fallback_pricing_checked_at_ms)
  ) {
    return false;
  }
  if (value.paid_fallback_enabled) {
    if (value.paid_fallback_model_ids.length === 0) return false;
    if (!isPositiveSafeInteger(value.paid_fallback_quota_per_credit)) return false;
    if (!isPositiveSafeInteger(value.paid_fallback_pricing_checked_at_ms)) return false;
  }
  return true;
};

export const paidFallbackHashFields = (record: ApiKeyRecord): Pick<
  ApiKeyHashRecord,
  | "paid_fallback_enabled"
  | "paid_fallback_limit_microcredits"
  | "paid_fallback_spent_microcredits"
  | "paid_fallback_reserved_microcredits"
  | "paid_fallback_reservation_request_id"
> => ({
  paid_fallback_enabled: record.paid_fallback_enabled,
  paid_fallback_limit_microcredits: record.paid_fallback_limit_microcredits,
  paid_fallback_spent_microcredits: record.paid_fallback_spent_microcredits,
  paid_fallback_reserved_microcredits: record.paid_fallback_reserved_microcredits,
  paid_fallback_reservation_request_id: record.paid_fallback_reservation_request_id,
});

export const initializePaidFallbackPolicy = async (signal?: AbortSignal): Promise<
  Pick<
    PaidFallbackPolicyFields,
    | "paid_fallback_model_ids"
    | "paid_fallback_quota_per_credit"
    | "paid_fallback_pricing_checked_at_ms"
    | "paid_fallback_max_exposure_microcredits"
  >
> => {
  if (!readYunwuApiKey()) {
    throw new YunwuError(
      "YunWu paid fallback cannot be enabled because YUNWU_API_KEY is not configured.",
      "yunwu_api_key_missing",
      503,
    );
  }

  // The compact runtime catalog intentionally omits context bounds to fit one
  // KV read unit. Paid-admission exposure must use the immutable full catalog.
  const snapshot = await loadFullCodexModelsSnapshot();
  const modelIds: string[] = [];
  const contextByModel = new Map<string, number>();
  const seen = new Set<string>();
  for (const model of snapshot?.models ?? []) {
    const id = getString(model.slug) ?? getString(model.id) ?? getString(model.model) ?? getString(model.name);
    const normalized = id?.trim();
    if (!normalized || seen.has(normalized)) continue;
    seen.add(normalized);
    modelIds.push(normalized);
    const context = [model.context_window, model.max_context_window, model.auto_compact_token_limit]
      .filter((value): value is number => typeof value === "number" && Number.isSafeInteger(value) && value > 0);
    if (context.length) contextByModel.set(normalized, Math.max(...context));
  }
  if (!modelIds.length) {
    throw new YunwuError(
      "YunWu paid fallback cannot be enabled before the Codex model catalog is initialized.",
      "yunwu_pricing_invalid",
      503,
    );
  }

  const pricing = await initializeYunwuPricing({ codexModelIds: modelIds, signal });
  if (!pricing.eligible_model_ids.length) {
    throw new YunwuError(
      "YunWu paid fallback found no priced models in the current Codex catalog.",
      "yunwu_pricing_invalid",
      503,
    );
  }
  const maximumExposure: Record<string, number> = {};
  for (const model of pricing.eligible_model_ids) {
    const context = contextByModel.get(model);
    const coefficient = pricing.model_quota_coefficients[model];
    if (!context || !Number.isFinite(coefficient) || coefficient <= 0) continue;
    const exposure = Math.ceil(context * coefficient * MICROCREDITS_PER_CREDIT / pricing.quota_per_credit);
    if (Number.isSafeInteger(exposure) && exposure > 0) maximumExposure[model] = exposure;
  }
  const missingExposure = pricing.eligible_model_ids.some((model) => !isPositiveSafeInteger(maximumExposure[model]));
  if (missingExposure) {
    throw new YunwuError(
      "YunWu paid fallback cannot be enabled because a priced Codex model has no finite context bound.",
      "yunwu_pricing_invalid",
      503,
    );
  }
  return {
    paid_fallback_model_ids: [...pricing.eligible_model_ids],
    paid_fallback_quota_per_credit: pricing.quota_per_credit,
    paid_fallback_max_exposure_microcredits: maximumExposure,
    paid_fallback_pricing_checked_at_ms: pricing.checked_at_ms,
  };
};

type PaidFallbackPolicyEntry = Readonly<{
  record: ApiKeyRecord;
  check: Readonly<{ key: Deno.KvKey; versionstamp: string | null }>;
}>;

const loadStrictKeyRecord = async (
  kv: Deno.Kv,
  keyId: string,
): Promise<PaidFallbackPolicyEntry | null> => {
  const idEntry = await kv.get<ApiKeyRecord>(apiKeyIdKey(keyId), { consistency: "strong" });
  if (!idEntry.value || !hasStrictPaidFallbackKeyPolicy(idEntry.value)) return null;
  return {
    record: idEntry.value,
    check: { key: idEntry.key, versionstamp: idEntry.versionstamp },
  };
};

const advanceUsageWindow = (resetAtMs: number, windowMs: number, nowMs: number): number => {
  if (nowMs < resetAtMs) return resetAtMs;
  const initialStart = resetAtMs - windowMs;
  const elapsedWindows = Math.floor((nowMs - initialStart) / windowMs);
  return initialStart + (elapsedWindows + 1) * windowMs;
};

export const reservePaidFallback = async (
  input: Readonly<{
    keyId: string;
    requestId: string;
    createdAtMs: number;
    model: string;
    route: string;
    path: string;
    stream: boolean;
    reasoning: string | null;
    reason: "primary_401" | "primary_403" | "primary_429";
  }>,
): Promise<PaidFallbackReservationDecision> => {
  const kv = await getKv();
  if (!kv) return { kind: "blocked", reason: "invalid_policy", reset_at_ms: null };
  for (let attempt = 0; attempt < 3; attempt += 1) {
    // This strongly-read check is part of the admission CAS. A just-committed
    // disable or cap reduction therefore prevents new paid exposure even when
    // a request had already read the older policy snapshot.
    const policy = await loadStrictKeyRecord(kv, input.keyId);
    if (!policy) return { kind: "blocked", reason: "invalid_policy", reset_at_ms: null };
    const record = policy.record;
    const eligibility = evaluatePaidFallbackEligibility(record, input.model);
    if (eligibility.kind !== "eligible") return eligibility;
    if (!readYunwuApiKey()) return { kind: "skip", reason: "provider_unconfigured" };
    const windowResetAtMs = advanceUsageWindow(record.usage_reset_at_ms, record.window_ms, input.createdAtMs);
    const policyVersion = `${record.window_ms}:${record.paid_fallback_pricing_checked_at_ms ?? 0}`;
    const admitted = await admitPaidFallbackV3({
      ...input,
      policyVersion,
      policyCheck: policy.check,
      limitMicrocredits: record.paid_fallback_limit_microcredits,
      maximumExposureMicrocredits: record.paid_fallback_max_exposure_microcredits?.[input.model] ?? null,
      initialSettledMicrocredits: 0,
      quotaPerCredit: record.paid_fallback_quota_per_credit,
      windowResetAtMs,
      // Persist the billable-ambiguity boundary in the admission transaction.
      // Provider fetch must never begin from a row that still looks safely
      // undispatched.
      dispatchIntent: true,
    });
    if (admitted.kind === "reserved") return { kind: "reserved", reservation: admitted.reservation };
    if (admitted.reason === "concurrent_update" && attempt < 2) continue;
    return { kind: "blocked", reason: admitted.reason, reset_at_ms: windowResetAtMs };
  }
  return { kind: "blocked", reason: "concurrent_update", reset_at_ms: null };
};

export const recordYunwuUpstreamResponse = async (
  reservation: PaidFallbackReservation,
  _response: Response,
  providerRequestId: string | null,
): Promise<void> => {
  await updatePaidFallbackRequestV3(reservation, {
    provider_request_id: providerRequestId,
    dispatch_state: "dispatched",
  });
};

export const recordYunwuAmbiguousFailure = async (reservation: PaidFallbackReservation): Promise<void> => {
  await updatePaidFallbackRequestV3(reservation, { dispatch_state: "dispatched" });
  await markPaidFallbackTerminalV3(reservation, "ambiguous");
};

export const recordYunwuUndispatchedCancellation = async (
  reservation: PaidFallbackReservation,
): Promise<void> => {
  await releasePaidFallbackBeforeProviderFetchV3(reservation);
};

export const recordYunwuPrefetchCancellation = async (
  reservation: PaidFallbackReservation,
): Promise<void> => {
  await releasePaidFallbackBeforeProviderFetchV3(reservation);
};

export const recordYunwuTerminal = async (
  reservation: PaidFallbackReservation,
  terminalState: "completed" | "failed" | "incomplete" | "cancelled" | "ambiguous",
): Promise<void> => {
  await markPaidFallbackTerminalV3(reservation, terminalState);
};
