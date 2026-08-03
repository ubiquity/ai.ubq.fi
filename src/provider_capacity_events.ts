import {
  CODEX_RESET_REDEMPTION_KV_PREFIX,
  CODEX_RESET_SHADOW_DECISION_KV_PREFIX,
  parseCodexResetRedemptionRecord,
  parseCodexResetShadowDecisionRecord,
} from "./codex_banked_reset.ts";
import { getKv } from "./kv.ts";
import { isRecord } from "./utils.ts";

/** Redacted, short-lived evidence used only to annotate the capacity chart. */
export const PROVIDER_CAPACITY_RESET_EVENT_KV_PREFIX = [
  "uos_ai",
  "provider_capacity",
  "v1",
  "reset_event",
] as const;
export const PROVIDER_CAPACITY_RESET_EVENT_RETENTION_MS = 7 * 24 * 60 * 60_000;

export type ProviderCapacityResetEvent = Readonly<{
  v: 1;
  event_id: string;
  slot: 1 | 2;
  observed_at_ms: number;
}>;

export const providerCapacityResetEventKey = (eventId: string): Deno.KvKey => [
  ...PROVIDER_CAPACITY_RESET_EVENT_KV_PREFIX,
  eventId,
];

const isSafeTimestamp = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0;

const isNonEmptyText = (value: unknown): value is string =>
  typeof value === "string" && value.length > 0 && value.length <= 512;

export const parseProviderCapacityResetEvent = (value: unknown): ProviderCapacityResetEvent | null => {
  if (!isRecord(value) || value.v !== 1 || !isNonEmptyText(value.event_id)) return null;
  if ((value.slot !== 1 && value.slot !== 2) || !isSafeTimestamp(value.observed_at_ms)) return null;
  return {
    v: 1,
    event_id: value.event_id,
    slot: value.slot,
    observed_at_ms: value.observed_at_ms,
  };
};

const resolveKv = async (kvOverride: Deno.Kv | null | undefined): Promise<Deno.Kv | null> => {
  if (kvOverride !== undefined) return kvOverride;
  try {
    return await getKv();
  } catch {
    return null;
  }
};

const writeResetEvent = async (kv: Deno.Kv, event: ProviderCapacityResetEvent): Promise<boolean> => {
  const key = providerCapacityResetEventKey(event.event_id);
  for (let attempt = 0; attempt < 4; attempt += 1) {
    let entry: Deno.KvEntryMaybe<unknown>;
    try {
      entry = await kv.get<unknown>(key, { consistency: "strong" });
    } catch {
      return false;
    }
    const existing = entry.value === null ? null : parseProviderCapacityResetEvent(entry.value);
    if (existing) return existing.slot === event.slot && existing.observed_at_ms === event.observed_at_ms;
    try {
      const committed = await kv.atomic()
        .check(entry)
        .set(key, event, { expireIn: PROVIDER_CAPACITY_RESET_EVENT_RETENTION_MS })
        .commit();
      if (committed.ok) return true;
    } catch {
      return false;
    }
  }
  return false;
};

export const recordProviderCapacityResetEvent = async (
  event: ProviderCapacityResetEvent,
  kvOverride?: Deno.Kv | null,
): Promise<boolean> => {
  const kv = await resolveKv(kvOverride);
  return kv ? await writeResetEvent(kv, event) : false;
};

type ResetShadowFence = Readonly<{
  slot: number;
  account_id_hash: string;
  quota_generation: string;
}>;

const readBackfillableEvents = async (
  kv: Deno.Kv,
  cutoffMs: number,
  nowMs: number,
): Promise<ProviderCapacityResetEvent[]> => {
  const decisions: ResetShadowFence[] = [];
  const redemptions: ProviderCapacityResetEvent[] = [];
  try {
    for await (const entry of kv.list<unknown>({ prefix: CODEX_RESET_SHADOW_DECISION_KV_PREFIX })) {
      const decision = parseCodexResetShadowDecisionRecord(entry.value);
      if (!decision) continue;
      for (const fence of decision.fences) {
        if (fence.slot === 1 || fence.slot === 2) {
          decisions.push({
            slot: fence.slot,
            account_id_hash: fence.account_id_hash,
            quota_generation: fence.quota_generation,
          });
        }
      }
    }
    for await (const entry of kv.list<unknown>({ prefix: CODEX_RESET_REDEMPTION_KV_PREFIX })) {
      const record = parseCodexResetRedemptionRecord(entry.value);
      if (
        !record || record.state !== "verified" || record.verified_at_ms === null ||
        record.verified_at_ms < cutoffMs || record.verified_at_ms > nowMs
      ) continue;
      const fence = decisions.find((candidate) =>
        candidate.account_id_hash === record.account_id_hash && candidate.quota_generation === record.quota_generation
      );
      if (!fence || (fence.slot !== 1 && fence.slot !== 2)) continue;
      redemptions.push({
        v: 1,
        event_id: record.idempotency_key_hash,
        slot: fence.slot,
        observed_at_ms: record.verified_at_ms,
      });
    }
  } catch {
    return [];
  }
  return redemptions;
};

/**
 * Read recent reset markers and derive any missing markers from verified
 * redemption records. The derivation is intentionally best effort: an
 * unverified or unmapped redemption must never become a chart assertion.
 */
export const listProviderCapacityResetEvents = async (
  options: Readonly<{ kv?: Deno.Kv | null; now?: () => number }> = {},
): Promise<readonly ProviderCapacityResetEvent[]> => {
  const kv = await resolveKv(options.kv);
  if (!kv) return [];
  const rawNow = Math.trunc(options.now?.() ?? Date.now());
  const nowMs = Number.isSafeInteger(rawNow) && rawNow >= 0 ? rawNow : Date.now();
  const cutoffMs = Math.max(0, nowMs - PROVIDER_CAPACITY_RESET_EVENT_RETENTION_MS);
  const events = new Map<string, ProviderCapacityResetEvent>();
  try {
    for await (const entry of kv.list<unknown>({ prefix: PROVIDER_CAPACITY_RESET_EVENT_KV_PREFIX })) {
      const event = parseProviderCapacityResetEvent(entry.value);
      if (event && event.observed_at_ms >= cutoffMs && event.observed_at_ms <= nowMs) {
        events.set(event.event_id, event);
      }
    }
  } catch {
    return [];
  }

  for (const event of await readBackfillableEvents(kv, cutoffMs, nowMs)) {
    if (!events.has(event.event_id)) {
      await writeResetEvent(kv, event);
      events.set(event.event_id, event);
    }
  }

  return [...events.values()].sort((left, right) =>
    left.observed_at_ms - right.observed_at_ms || left.event_id.localeCompare(right.event_id)
  );
};
