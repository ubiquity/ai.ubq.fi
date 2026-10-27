import { isRecord } from "../utils.ts";

/** Native Claude Code 2.1.310 usage contract; these are meters, never routing guards. */
export const CLAUDE_USAGE_LEGACY_WINDOWS = ["five_hour", "seven_day", "seven_day_oauth_apps", "seven_day_opus", "seven_day_sonnet"] as const;
type UsageWindow = Readonly<Record<string, unknown>> & Readonly<{ utilization: number | null; resets_at: string | null }>;
type UsageScope = Readonly<{ model?: Readonly<{ display_name: string }> | null; surface?: Readonly<{ display_name: string }> | null }>;
type UsageLimit = Readonly<{
  kind: string;
  group: string;
  percent: number;
  resets_at: string | null;
  severity: string;
  is_active: boolean;
  scope?: UsageScope | null;
}>;
export type ClaudeCodeUsagePayload = Readonly<Record<string, unknown>> &
  Readonly<Partial<Record<(typeof CLAUDE_USAGE_LEGACY_WINDOWS)[number], UsageWindow | null>>> &
  Readonly<{ limits?: readonly UsageLimit[] | null }>;
export type ClaudeCodeUsageMeter = Readonly<{
  id: string;
  label: string;
  kind: string;
  group: string;
  scope: UsageScope | null;
  severity: string | null;
  is_active: boolean;
  used_percent: number | null;
  resets_at: string | null;
  reset_at_ms: number | null;
  observed_at_ms: number;
}>;
export type ClaudeCodeUsageSnapshot = Readonly<{
  v: 1;
  observed_at_ms: number;
  payload: ClaudeCodeUsagePayload;
  meters: readonly ClaudeCodeUsageMeter[];
}>;
const finite = (value: unknown): value is number => typeof value === "number" && Number.isFinite(value);
const nullableNumber = (value: unknown): boolean => value === null || finite(value);
const nullableString = (value: unknown): boolean => value === null || typeof value === "string";
const optionalString = (value: unknown): boolean => value === undefined || nullableString(value);
const validWindow = (value: unknown): value is UsageWindow => isRecord(value) && nullableNumber(value.utilization) && nullableString(value.resets_at);
const validScope = (value: unknown): boolean => {
  if (value === undefined || value === null) return true;
  if (!isRecord(value)) return false;
  return [value.model, value.surface].every((scope) => scope === undefined || scope === null || (isRecord(scope) && typeof scope.display_name === "string"));
};
const validLimit = (value: unknown): boolean =>
  isRecord(value) &&
  [value.kind, value.group, value.severity].every((item) => typeof item === "string") &&
  finite(value.percent) &&
  nullableString(value.resets_at) &&
  typeof value.is_active === "boolean" &&
  validScope(value.scope);
const validExtraUsage = (value: unknown): boolean =>
  isRecord(value) &&
  typeof value.is_enabled === "boolean" &&
  [value.monthly_limit, value.used_credits, value.utilization].every(nullableNumber) &&
  [value.currency, value.disabled_reason].every(optionalString);
const validCredit = (value: unknown): boolean => {
  if (!validWindow(value) || !isRecord(value)) return false;
  if (
    !optionalString(value.label) ||
    ![value.limit_dollars, value.used_dollars, value.remaining_dollars].every((item) => item === undefined || nullableNumber(item))
  )
    return false;
  return (
    value.periods === undefined ||
    value.periods === null ||
    (Array.isArray(value.periods) &&
      value.periods.every((period: unknown) => isRecord(period) && typeof period.starts_at === "string" && typeof period.ends_at === "string"))
  );
};
const recognized = [...CLAUDE_USAGE_LEGACY_WINDOWS, "limits", "extra_usage", "cinder_cove", "wattle_ember"];
const validPayload = (value: unknown): value is ClaudeCodeUsagePayload => {
  if (!isRecord(value) || !recognized.some((key) => Object.hasOwn(value, key))) return false;
  for (const key of CLAUDE_USAGE_LEGACY_WINDOWS) {
    if (value[key] !== undefined && value[key] !== null && !validWindow(value[key])) return false;
  }
  if (value.limits !== undefined && value.limits !== null && (!Array.isArray(value.limits) || !value.limits.every(validLimit))) return false;
  for (const [key, validate] of [
    ["extra_usage", validExtraUsage],
    ["cinder_cove", validWindow],
    ["wattle_ember", validCredit],
  ] as const) {
    if (value[key] !== undefined && value[key] !== null && !validate(value[key])) return false;
  }
  return true;
};
const resetTime = (value: string | null): number | null => {
  const parsed = value === null ? NaN : Date.parse(value);
  return Number.isSafeInteger(parsed) && parsed > 0 ? parsed : null;
};
const legacyLabels: Readonly<Record<(typeof CLAUDE_USAGE_LEGACY_WINDOWS)[number], string>> = {
  five_hour: "5-hour window",
  seven_day: "Weekly window",
  seven_day_oauth_apps: "Weekly OAuth apps",
  seven_day_opus: "Weekly Opus",
  seven_day_sonnet: "Weekly Sonnet",
};
const metersForPayload = (payload: ClaudeCodeUsagePayload, at: number): ClaudeCodeUsageMeter[] => {
  if (Array.isArray(payload.limits)) {
    const occurrences = new Map<string, number>();
    return payload.limits.map((row) => {
      const scope = row.scope ?? null;
      const tuple = JSON.stringify([row.kind, row.group, scope]);
      const occurrence = occurrences.get(tuple) ?? 0;
      occurrences.set(tuple, occurrence + 1);
      const labels = [scope?.model?.display_name, scope?.surface?.display_name].filter((label): label is string => typeof label === "string");
      return {
        id: `limits:${tuple}:${occurrence}`,
        label: [row.group, row.kind, ...labels].join(" · "),
        kind: row.kind,
        group: row.group,
        scope,
        severity: row.severity,
        is_active: row.is_active,
        used_percent: row.percent,
        resets_at: row.resets_at,
        reset_at_ms: resetTime(row.resets_at),
        observed_at_ms: at,
      };
    });
  }
  return CLAUDE_USAGE_LEGACY_WINDOWS.flatMap((key) => {
    const row = payload[key];
    return row
      ? [
          {
            id: `legacy:${key}`,
            label: legacyLabels[key],
            kind: key,
            group: key === "five_hour" ? "session" : "weekly",
            scope: null,
            severity: null,
            is_active: false,
            used_percent: row.utilization,
            resets_at: row.resets_at,
            reset_at_ms: resetTime(row.resets_at),
            observed_at_ms: at,
          },
        ]
      : [];
  });
};
/** Successful snapshots replace the complete prior shape; missing is not zero or last-known data. */
export const parseClaudeCodeUsageSnapshot = (payload: unknown, observedAtMs: number): ClaudeCodeUsageSnapshot | null => {
  if (!Number.isSafeInteger(observedAtMs) || observedAtMs <= 0 || !validPayload(payload)) return null;
  return { v: 1, observed_at_ms: observedAtMs, payload, meters: metersForPayload(payload, observedAtMs) };
};
/** Recompute projections from validated provenance rather than trusting stored derived fields. */
export const readClaudeCodeUsageSnapshot = (value: unknown): ClaudeCodeUsageSnapshot | null =>
  isRecord(value) && value.v === 1 && typeof value.observed_at_ms === "number" ? parseClaudeCodeUsageSnapshot(value.payload, value.observed_at_ms) : null;

export const readClaudeCodeUsageMeter = (value: unknown): ClaudeCodeUsageMeter | null => {
  if (!isRecord(value) || ![value.id, value.label, value.kind, value.group].every((item) => typeof item === "string")) return null;
  if (
    !nullableNumber(value.used_percent) ||
    !nullableString(value.resets_at) ||
    !nullableString(value.severity) ||
    typeof value.is_active !== "boolean" ||
    !validScope(value.scope)
  )
    return null;
  if (typeof value.observed_at_ms !== "number" || !Number.isSafeInteger(value.observed_at_ms) || value.observed_at_ms <= 0) return null;
  if (value.reset_at_ms !== null && (typeof value.reset_at_ms !== "number" || !Number.isSafeInteger(value.reset_at_ms) || value.reset_at_ms <= 0)) return null;
  return value as ClaudeCodeUsageMeter;
};
