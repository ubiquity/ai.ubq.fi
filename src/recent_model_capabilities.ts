import { getRecentModelCapabilities } from "../static/reasoning-select.js";

export type RecentGatewayModelCapabilities = Readonly<{
  model_class: string;
  supported_reasoning_levels: readonly string[];
  default_reasoning_effort: string | null;
  max_context_window_tokens: number;
  context_window_tokens: number;
  max_output_tokens: number | null;
  auto_compact_token_limit_tokens: number;
}>;

type BrowserCapability = Readonly<{
  modelClass?: unknown;
  levels?: unknown;
  defaultLevel?: unknown;
  max_context_window_tokens?: unknown;
  context_window_tokens?: unknown;
  max_output_tokens?: unknown;
  auto_compact_token_limit_tokens?: unknown;
}>;

const nonEmptyString = (value: unknown): string | null => {
  if (typeof value !== "string") return null;
  const trimmed = value.trim();
  return trimmed || null;
};

const positiveSafeInteger = (value: unknown): number | null =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0 ? value : null;

const reasoningLevels = (value: unknown): readonly string[] => {
  if (!Array.isArray(value)) return [];
  const levels = value.flatMap((entry) => {
    const level = nonEmptyString(entry);
    return level ? [level] : [];
  });
  return [...new Set(levels)];
};

/**
 * Returns the audited capability envelope for a recent model class.
 *
 * Context values are generated from the live Codex, OpenLux, and Surplus
 * catalogs. The selected values are conservative across routes so the same
 * model ID remains safe when gateway failover changes its upstream provider.
 */
export const getRecentGatewayModelCapabilities = (
  modelId: string,
): RecentGatewayModelCapabilities | null => {
  const value = getRecentModelCapabilities(modelId) as BrowserCapability | null;
  if (!value) return null;

  const modelClass = nonEmptyString(value.modelClass);
  const levels = reasoningLevels(value.levels);
  const maxContext = positiveSafeInteger(value.max_context_window_tokens);
  const context = positiveSafeInteger(value.context_window_tokens);
  const autoCompact = positiveSafeInteger(value.auto_compact_token_limit_tokens);
  const maxOutput = value.max_output_tokens === null ? null : positiveSafeInteger(value.max_output_tokens);
  const defaultReasoning = value.defaultLevel === null ? null : nonEmptyString(value.defaultLevel);

  if (!modelClass || !levels.length || maxContext === null || context === null || autoCompact === null) return null;
  if (context > maxContext || autoCompact > context) return null;
  if (maxOutput !== null && context + maxOutput > maxContext) return null;

  return {
    model_class: modelClass,
    supported_reasoning_levels: levels,
    default_reasoning_effort: defaultReasoning,
    max_context_window_tokens: maxContext,
    context_window_tokens: context,
    max_output_tokens: maxOutput,
    auto_compact_token_limit_tokens: autoCompact,
  };
};
