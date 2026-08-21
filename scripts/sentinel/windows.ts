import type { SentinelMode } from "./policy.ts";
import type { SentinelInterval } from "./types.ts";

export const DAILY_WINDOW_MS = 24 * 60 * 60 * 1_000;
export const INCIDENT_WINDOW_MS = 20 * 60 * 1_000;
// The private observation workflow runs every two hours. Five minutes of
// overlap avoids gaps when GitHub starts a scheduled job late.
export const OBSERVE_WINDOW_MS = 125 * 60 * 1_000;

export const computeSentinelInterval = (mode: SentinelMode, nowMs: number): SentinelInterval => {
  if (!Number.isSafeInteger(nowMs) || nowMs <= 0) throw new Error("Sentinel clock must be a positive integer");
  const durationMs = mode === "daily" ? DAILY_WINDOW_MS : mode === "observe" ? OBSERVE_WINDOW_MS : INCIDENT_WINDOW_MS;
  return {
    start: new Date(nowMs - durationMs).toISOString(),
    end: new Date(nowMs).toISOString(),
    duration_ms: durationMs,
  };
};

export const eventDedupeKey = async (
  input: Readonly<{
    repository: string;
    event: string;
    interval: SentinelInterval;
    signalId?: string;
  }>,
): Promise<string> => {
  const signalId = input.signalId?.trim() || null;
  const data = JSON.stringify(
    signalId ? { repository: input.repository, event: input.event, signal_id: signalId } : {
      repository: input.repository,
      event: input.event,
      start: input.interval.start,
      end: input.interval.end,
      signal_id: null,
    },
  );
  const digest = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(data));
  return [...new Uint8Array(digest)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
};

export const deduplicateEvents = <T>(items: readonly T[], key: (item: T) => string): T[] => {
  const seen = new Set<string>();
  return items.filter((item) => {
    const fingerprint = key(item);
    if (seen.has(fingerprint)) return false;
    seen.add(fingerprint);
    return true;
  });
};
