import { getKv } from "./kv.ts";
import { isRecord } from "./utils.ts";

export const OPENROUTER_TELEMETRY_KEY = ["uos_ai", "openrouter_failover", "telemetry", "v1"] as const;
export const OPENROUTER_TELEMETRY_TTL_MS = 30 * 24 * 60 * 60_000;

export type OpenRouterTelemetryRecord = Readonly<{
  v: 1;
  attempted_provider: string | null;
  trigger_class: string | null;
  circuit_transition: string | null;
  selected_model: string | null;
  task_type: string | null;
  latency_ms: number | null;
  terminal_status: string | null;
  semantic_commitment: string | null;
  observed_at_ms: number;
}>;

const boundedString = (value: unknown, maxLength = 128): string | null => {
  if (typeof value !== "string") return null;
  const normalized = value.trim();
  if (
    !normalized || normalized.length > maxLength ||
    [...normalized].some((character) => character < " " || character === "\u007f")
  ) return null;
  return normalized;
};

const boundedMs = (value: unknown): number | null =>
  typeof value === "number" && Number.isSafeInteger(value) && value >= 0 ? value : null;

export const parseOpenRouterTelemetryRecord = (value: unknown): OpenRouterTelemetryRecord | null => {
  if (!isRecord(value) || value.v !== 1) return null;
  const observedAt = boundedMs(value.observed_at_ms);
  if (observedAt === null) return null;
  const latency = value.latency_ms === null ? null : boundedMs(value.latency_ms);
  if (value.latency_ms !== null && latency === null) return null;
  return {
    v: 1,
    attempted_provider: value.attempted_provider === null ? null : boundedString(value.attempted_provider),
    trigger_class: value.trigger_class === null ? null : boundedString(value.trigger_class),
    circuit_transition: value.circuit_transition === null ? null : boundedString(value.circuit_transition),
    selected_model: value.selected_model === null ? null : boundedString(value.selected_model, 256),
    task_type: value.task_type === null ? null : boundedString(value.task_type),
    latency_ms: latency,
    terminal_status: value.terminal_status === null ? null : boundedString(value.terminal_status),
    semantic_commitment: value.semantic_commitment === null ? null : boundedString(value.semantic_commitment),
    observed_at_ms: observedAt,
  };
};

export type OpenRouterTelemetryInput = Readonly<{
  attempted_provider?: string | null;
  trigger_class?: string | null;
  circuit_transition?: string | null;
  selected_model?: string | null;
  task_type?: string | null;
  latency_ms?: number | null;
  terminal_status?: string | null;
  semantic_commitment?: string | null;
  observed_at_ms?: number;
}>;

const normalizeTelemetry = (input: OpenRouterTelemetryInput): OpenRouterTelemetryRecord => ({
  v: 1,
  attempted_provider: boundedString(input.attempted_provider),
  trigger_class: boundedString(input.trigger_class),
  circuit_transition: boundedString(input.circuit_transition),
  selected_model: boundedString(input.selected_model, 256),
  task_type: boundedString(input.task_type),
  latency_ms: boundedMs(input.latency_ms),
  terminal_status: boundedString(input.terminal_status),
  semantic_commitment: boundedString(input.semantic_commitment),
  observed_at_ms: boundedMs(input.observed_at_ms ?? Date.now()) ?? Date.now(),
});

export const recordOpenRouterTelemetry = async (input: OpenRouterTelemetryInput): Promise<void> => {
  const kv = await getKv();
  if (!kv) return;
  const value = normalizeTelemetry(input);
  await kv.set(OPENROUTER_TELEMETRY_KEY, value, { expireIn: OPENROUTER_TELEMETRY_TTL_MS });
};

export const getOpenRouterTelemetryView = async (): Promise<Record<string, unknown>> => {
  const unavailable = (available: boolean): Record<string, unknown> => ({
    available,
    attempted_provider: null,
    trigger_class: null,
    circuit_transition: null,
    selected_model: null,
    task_type: null,
    latency_ms: null,
    terminal_status: null,
    semantic_commitment: null,
    observed_at_ms: null,
  });
  const kv = await getKv();
  if (!kv) return unavailable(false);
  try {
    const entry = await kv.get<OpenRouterTelemetryRecord>(OPENROUTER_TELEMETRY_KEY);
    const parsed = parseOpenRouterTelemetryRecord(entry.value);
    return { ...unavailable(true), ...(parsed ?? {}) };
  } catch {
    // Provider health is passive and best effort. A telemetry read failure
    // must not hide the remaining provider snapshot.
    return unavailable(false);
  }
};
