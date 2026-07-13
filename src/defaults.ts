import { API_KEY_NO_USAGE_LIMIT, USAGE_RESET_PERIOD_MS } from "./api_keys.ts";

export type ReasoningEffort = string;

export const DEFAULT_REASONING_EFFORT: ReasoningEffort = "medium";
export const DEFAULT_KERNEL_POLICY_LIMIT_REQUESTS = API_KEY_NO_USAGE_LIMIT;
export const DEFAULT_KERNEL_POLICY_WINDOW_MS = USAGE_RESET_PERIOD_MS;

export const DEFAULT_MODEL_KEY = ["default", "model"] as const;
export const DEFAULT_REASONING_EFFORT_KEY = ["default", "reasoning_effort"] as const;
export const DEFAULT_KERNEL_POLICY_LIMIT_KEY = ["default", "kernel_policy_limit_requests"] as const;
export const DEFAULT_KERNEL_POLICY_WINDOW_KEY = ["default", "kernel_policy_window_ms"] as const;

export const normalizeReasoningEffort = (value: unknown): ReasoningEffort | null => {
  if (typeof value !== "string") return null;
  const normalized = value.trim();
  if (!normalized) return null;
  return normalized;
};
