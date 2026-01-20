import { API_KEY_NO_USAGE_LIMIT, USAGE_RESET_PERIOD_MS } from "./api_keys.ts";

export type ReasoningEffort = "none" | "minimal" | "low" | "medium" | "high" | "xhigh";

export const REASONING_EFFORTS: ReadonlySet<ReasoningEffort> = new Set([
  "none",
  "minimal",
  "low",
  "medium",
  "high",
  "xhigh",
]);

export const DEFAULT_MODEL = "gpt-5.2-codex";
export const DEFAULT_REASONING_EFFORT: ReasoningEffort = "medium";
export const DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS = API_KEY_NO_USAGE_LIMIT;
export const DEFAULT_KERNEL_POLICY_WINDOW_MS = USAGE_RESET_PERIOD_MS;

export const DEFAULT_MODEL_KEY = ["default", "model"] as const;
export const DEFAULT_REASONING_EFFORT_KEY = ["default", "reasoning_effort"] as const;
export const DEFAULT_KERNEL_POLICY_LIMIT_KEY = ["default", "kernel_policy_limit_requests"] as const;
export const DEFAULT_KERNEL_POLICY_WINDOW_KEY = ["default", "kernel_policy_window_ms"] as const;

export const normalizeReasoningEffort = (value: unknown): ReasoningEffort | null => {
  if (typeof value !== "string") return null;
  const normalized = value.trim().toLowerCase();
  if (!normalized) return null;
  if (REASONING_EFFORTS.has(normalized as ReasoningEffort)) return normalized as ReasoningEffort;
  return null;
};
