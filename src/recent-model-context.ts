/**
 * Context-window arithmetic shared by every model surface.
 *
 * This module used to own a curated table mapping model-id patterns to context
 * windows. That table is commented out below while the dynamic sources prove
 * themselves: `src/model_metadata.ts` now resolves a model's context from the
 * uploaded Codex catalog, the serving provider's discovery row, and OpenRouter
 * enrichment, and reports `unknown` when no source describes the id.
 *
 * What remains here is derivation, not per-model knowledge: given a context
 * window, the auto-compaction trigger is arithmetic, and it stays in one place so
 * every surface computes the same boundary.
 */
export const CONTEXT_COMPACTION_TRIGGER_RATIO = 0.85;
export const CONTEXT_COMPACTION_RESERVED_TOKENS = 50_000;
export const CODEX_EFFECTIVE_CONTEXT_WINDOW_PERCENT = 95;

/**
 * Trigger before either 85% of the model window is consumed or fewer than
 * 50,000 tokens remain, whichever happens first. This mirrors Kimi Code's
 * conservative long-running-agent defaults and stays below Codex's 90%
 * derived ceiling.
 */
export const deriveAutoCompactTokenLimit = (contextWindowTokens: number): number => {
  if (!Number.isSafeInteger(contextWindowTokens) || contextWindowTokens <= CONTEXT_COMPACTION_RESERVED_TOKENS) {
    throw new RangeError("contextWindowTokens must be a safe integer greater than the reserved token budget");
  }
  return Math.min(Math.floor(contextWindowTokens * CONTEXT_COMPACTION_TRIGGER_RATIO), contextWindowTokens - CONTEXT_COMPACTION_RESERVED_TOKENS);
};

const positiveSafeInteger = (value: number | null | undefined): number | null =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0 ? value : null;

export const resolvedAutoCompactTokenLimit = (contextWindowTokens: number, configuredLimit: number | null | undefined): number => {
  const configured = positiveSafeInteger(configuredLimit);
  if (configured !== null && configured <= contextWindowTokens) return configured;
  if (contextWindowTokens > CONTEXT_COMPACTION_RESERVED_TOKENS) {
    return deriveAutoCompactTokenLimit(contextWindowTokens);
  }
  return Math.max(1, Math.min(contextWindowTokens - 1, Math.floor(contextWindowTokens * 0.85)));
};

// ---------------------------------------------------------------------------
// Disabled curated table (kept for the coverage comparison, not in use).
//
// Restoring this means restoring `recentModelContextFor`, whose callers now use
// `resolveModelMetadata` from `src/model_metadata.ts`. It is deliberately left
// here verbatim so a gap in the dynamic sources can be judged against what the
// curated table claimed.
//
// Model-native context windows for relevant classes released in the six months
// ending 2026-08-24. Provider-prefixed and dated aliases inherit their class.
//
// Primary references:
// - OpenAI: https://developers.openai.com/api/docs/models
// - Anthropic: https://platform.claude.com/docs/en/build-with-claude/context-windows
// - DeepSeek: https://api-docs.deepseek.com/news/news260424/
// - Z.AI: https://z.ai/blog/glm-5.2 and https://z.ai/blog/glm-5.3
// - Gemini: https://ai.google.dev/gemini-api/docs/models
// - xAI: https://docs.x.ai/developers/pricing
// - Kimi: https://github.com/MoonshotAI/Kimi-K2.5 and MoonshotAI/kimi-code
// - Qwen: https://help.aliyun.com/en/model-studio/text-generation-model/
// - MiniMax: https://platform.minimax.io/docs/guides/text-generation and https://www.minimax.io/blog/minimax-m3
//
// type RecentModelContextRule = Readonly<{
//   model_class: string;
//   pattern: RegExp;
//   context_window_tokens: number;
// }>;
//
// const RECENT_MODEL_CONTEXT_RULES: readonly RecentModelContextRule[] = [
//   {
//     model_class: "gpt-5.4-mini-nano",
//     pattern: /^gpt-5\.4-(?:mini|nano)(?:-|:|$)/,
//     context_window_tokens: 400_000,
//   },
//   {
//     model_class: "gpt-5.4-5.6",
//     pattern: /^gpt-(?:5\.4(?:-pro)?|5\.5(?:-pro)?|5\.6(?:-(?:sol|terra|luna)(?:-pro)?)?)(?:-|:|$)/,
//     context_window_tokens: 1_050_000,
//   },
//   {
//     model_class: "claude-5",
//     pattern: /^claude-(?:opus|sonnet|fable|mythos)-5(?:-|:|$)/,
//     context_window_tokens: 1_000_000,
//   },
//   {
//     model_class: "claude-4.6-4.8",
//     pattern: /^claude-(?:opus|sonnet)-4[.-][678](?:-|:|$)/,
//     context_window_tokens: 1_000_000,
//   },
//   {
//     // `deepseek-flash` is the official API's canonical id (its `GET /models` lists
//     // only `deepseek-flash` and `deepseek-v4-pro`) and shares the V4 family window
//     // (https://api-docs.deepseek.com/quick_start/pricing). `deepseek-v4-flash` is
//     // the accepted alias; `deepseek-v4.1-flash` is a different, paid catalogue id.
//     model_class: "deepseek-v4",
//     pattern: /^deepseek-(?:v4(?:-(?:pro|flash))?|flash)(?:-|:|$)/,
//     context_window_tokens: 1_000_000,
//   },
//   {
//     model_class: "glm-5.2-5.3",
//     pattern: /^(?:e2ee-)?glm-5\.[23](?:-|:|$)/,
//     context_window_tokens: 1_000_000,
//   },
//   {
//     model_class: "glm-5-5.1",
//     pattern: /^(?:e2ee-)?glm-(?:5(?:\.1)?|5v)(?:-|:|$)/,
//     context_window_tokens: 200_000,
//   },
//   {
//     model_class: "gemini-3.x",
//     pattern: /^gemini-3(?:[.-][1567])?-(?:flash(?:-lite)?|pro)(?:-|:|$)/,
//     context_window_tokens: 1_048_576,
//   },
//   {
//     model_class: "grok-4.20",
//     pattern: /^grok-4\.20(?:-|:|$)/,
//     context_window_tokens: 1_000_000,
//   },
//   {
//     model_class: "grok-4.3",
//     pattern: /^grok-4\.3(?:-|:|$)/,
//     context_window_tokens: 1_000_000,
//   },
//   {
//     model_class: "grok-4.5-4.6",
//     pattern: /^grok-4\.[56](?:-|:|$)/,
//     context_window_tokens: 500_000,
//   },
//   {
//     model_class: "grok-build-code-fast",
//     pattern: /^grok-(?:build-0-1|code-fast-1)(?:-|:|$)/,
//     context_window_tokens: 256_000,
//   },
//   {
//     model_class: "kimi-k3",
//     pattern: /^kimi-k3(?:-|:|$)/,
//     context_window_tokens: 1_048_576,
//   },
//   {
//     model_class: "kimi-k2.5-2.7",
//     pattern: /^kimi-k2\.[567](?:-code)?(?:-|:|$)/,
//     context_window_tokens: 262_144,
//   },
//   {
//     model_class: "qwen3.5-397b",
//     pattern: /^(?:e2ee-)?qwen3\.5-397b-a17b(?:-|:|$)/,
//     context_window_tokens: 262_144,
//   },
//   {
//     model_class: "qwen3.6-27b",
//     pattern: /^(?:e2ee-)?qwen3\.6-27b(?:-|:|$)/,
//     context_window_tokens: 262_144,
//   },
//   {
//     model_class: "qwen3.5-flash-plus",
//     pattern: /^(?:e2ee-)?qwen3\.5-(?:flash|plus)(?:-|:|$)/,
//     context_window_tokens: 1_000_000,
//   },
//   {
//     model_class: "qwen3.6-plus",
//     pattern: /^(?:e2ee-)?qwen3\.6-plus(?:-|:|$)/,
//     context_window_tokens: 1_000_000,
//   },
//   {
//     model_class: "qwen3.7-max-plus",
//     pattern: /^(?:e2ee-)?qwen(?:3\.7|-3-7)-(?:max|plus)(?:-|:|$)/,
//     context_window_tokens: 1_000_000,
//   },
//   {
//     model_class: "qwen3.8-max",
//     pattern: /^(?:e2ee-)?qwen(?:3\.8|-3-8)-max(?:-|:|$)/,
//     context_window_tokens: 1_000_000,
//   },
//   {
//     model_class: "qwen3.8-2.4t-a95b",
//     pattern: /^(?:e2ee-)?qwen3\.8-2\.4t-a95b(?:-|:|$)/,
//     context_window_tokens: 1_000_000,
//   },
//   {
//     model_class: "minimax-m3",
//     pattern: /^minimax-m3(?:-|:|$)/,
//     context_window_tokens: 1_000_000,
//   },
//   {
//     model_class: "minimax-m2.5-2.7",
//     pattern: /^minimax-m2\.[57](?:-|:|$)/,
//     context_window_tokens: 204_800,
//   },
// ];
// ---------------------------------------------------------------------------
