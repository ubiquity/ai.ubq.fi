// `ubiquity/deepseek-v4.1-flash` — the synthetic DeepSeek V4.1 Flash model that
// walks the measured provider waterfall on the client's behalf. The order is
// data from the 2026-10-06 benchmark (docs/deepseek-waterfall-model-plan.md),
// not control flow: phase 1 covers the three direct routes, and Surplus joins
// as the final hop in phase 2 through the paid pipeline.

import type { DeepSeekWaterfallFallbackReason } from "../openai-telemetry.ts";
import { readDeepSeekApiKey } from "./index.ts";
import { readLithosApiKey } from "../provider/lithos.ts";
import { readOpenRouterApiKey } from "../provider/openrouter.ts";
import { isProviderEnabled, type ProviderSelection } from "../provider/selection.ts";

export const DEEPSEEK_WATERFALL_MODEL_ID = "ubiquity/deepseek-v4.1-flash";
export const DEEPSEEK_WATERFALL_DISPLAY_NAME = "Ubiquity DeepSeek V4.1 Flash";
export const DEEPSEEK_WATERFALL_DESCRIPTION =
  "Automatic DeepSeek V4.1 Flash waterfall: OpenRouter, then LithosAI, then the official DeepSeek API. Falls back only on infrastructure or serving failures.";
export const DEEPSEEK_WATERFALL_CONTEXT_WINDOW_TOKENS = 1_048_576;
export const DEEPSEEK_WATERFALL_EFFECTIVE_CONTEXT_WINDOW_PERCENT = 95;
export const DEEPSEEK_WATERFALL_AUTO_COMPACT_TOKEN_LIMIT = 891_289;
/** Stable creation instant for catalogue rows (2026-10-06T00:00:00Z). */
export const DEEPSEEK_WATERFALL_CREATED = 1_791_244_800;
export const DEEPSEEK_WATERFALL_REASONING_LEVELS = ["low", "high", "max"] as const;
export const DEEPSEEK_WATERFALL_DEFAULT_REASONING_LEVEL = "high";

export type DeepSeekWaterfallProvider = "openrouter" | "lithos" | "deepseek";

export const DEEPSEEK_WATERFALL_ORDER: readonly DeepSeekWaterfallProvider[] = ["openrouter", "lithos", "deepseek"];

/** The provider-specific id each hop is dispatched with. */
export const DEEPSEEK_WATERFALL_PROVIDER_MODEL: Readonly<Record<DeepSeekWaterfallProvider, string>> = {
  openrouter: "deepseek/deepseek-v4.1-flash",
  lithos: "deepseek-ai/DeepSeek-V4.1-Flash",
  deepseek: "deepseek-flash",
};

export const isDeepSeekWaterfallModel = (model: string): boolean => model.trim().toLowerCase() === DEEPSEEK_WATERFALL_MODEL_ID;

export const isDeepSeekWaterfallReasoningLevel = (effort: string): boolean => (DEEPSEEK_WATERFALL_REASONING_LEVELS as readonly string[]).includes(effort);

/** Whether the route has a credential for one hop. */
export const deepSeekWaterfallProviderConfigured = (provider: DeepSeekWaterfallProvider): boolean => {
  if (provider === "openrouter") return readOpenRouterApiKey() !== null;
  if (provider === "lithos") return readLithosApiKey() !== null;
  return readDeepSeekApiKey() !== null;
};

/** The hop order after removing providers the operator switched off or that have no credential. */
export const deepSeekWaterfallPlan = (
  input: Readonly<{
    enabled: (provider: DeepSeekWaterfallProvider) => boolean;
    configured: (provider: DeepSeekWaterfallProvider) => boolean;
  }>
): readonly DeepSeekWaterfallProvider[] => DEEPSEEK_WATERFALL_ORDER.filter((provider) => input.enabled(provider) && input.configured(provider));

/**
 * A hop status worth trying the next provider for: upstream transport, quota or
 * server failures that are not the client's fault. A request-validation 4xx is
 * the client's answer and must be returned as-is, and a valid completion — even
 * a wrong one — is a model result, never a serving failure.
 */
export const isDeepSeekWaterfallRetryStatus = (status: number): boolean => status >= 500 || status === 429 || status === 402 || status === 403;

/** Requested reasoning effort from a Responses record, or null when omitted. */
export const deepSeekWaterfallRequestedEffort = (rawRecord: Readonly<Record<string, unknown>>): string | null => {
  const reasoning = rawRecord.reasoning;
  if (typeof reasoning === "object" && reasoning !== null && !Array.isArray(reasoning)) {
    const effort = (reasoning as Readonly<Record<string, unknown>>).effort;
    if (typeof effort === "string" && effort.trim()) return effort.trim();
  }
  const legacy = rawRecord.reasoning_effort;
  if (typeof legacy === "string" && legacy.trim()) return legacy.trim();
  return null;
};

/** The exact fallback reason recorded for one failed hop. */
export const deepSeekWaterfallFailureReason = (provider: DeepSeekWaterfallProvider, failure: number | "transport"): DeepSeekWaterfallFallbackReason =>
  `deepseek_waterfall:${provider}:${failure === "transport" ? "transport_failure" : failure}`;

/** Whether at least one hop is switched on and credentialed, for catalogue gating. */
export const deepSeekWaterfallCatalogEnabled = (selection: ProviderSelection | null): boolean =>
  DEEPSEEK_WATERFALL_ORDER.some((provider) => isProviderEnabled(provider, selection) && deepSeekWaterfallProviderConfigured(provider));
