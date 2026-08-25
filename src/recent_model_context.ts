export const CONTEXT_COMPACTION_TRIGGER_RATIO = 0.85;
export const CONTEXT_COMPACTION_RESERVED_TOKENS = 50_000;
export const CODEX_EFFECTIVE_CONTEXT_WINDOW_PERCENT = 95;

export type RecentModelContext = Readonly<{
  model_class: string;
  context_window_tokens: number;
  max_context_window_tokens: number;
  auto_compact_token_limit_tokens: number;
  effective_context_window_percent: number;
}>;

export type RecentModelContextOverrides = Readonly<{
  context_window_tokens?: number | null;
  max_context_window_tokens?: number | null;
  auto_compact_token_limit_tokens?: number | null;
  effective_context_window_percent?: number | null;
}>;

type RecentModelContextRule = Readonly<{
  model_class: string;
  pattern: RegExp;
  context_window_tokens: number;
}>;

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
  return Math.min(
    Math.floor(contextWindowTokens * CONTEXT_COMPACTION_TRIGGER_RATIO),
    contextWindowTokens - CONTEXT_COMPACTION_RESERVED_TOKENS,
  );
};

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
const RECENT_MODEL_CONTEXT_RULES: readonly RecentModelContextRule[] = [
  {
    model_class: "gpt-5.4-mini-nano",
    pattern: /^gpt-5\.4-(?:mini|nano)(?:-|:|$)/,
    context_window_tokens: 400_000,
  },
  {
    model_class: "gpt-5.4-5.6",
    pattern: /^gpt-(?:5\.4(?:-pro)?|5\.5(?:-pro)?|5\.6(?:-(?:sol|terra|luna)(?:-pro)?)?)(?:-|:|$)/,
    context_window_tokens: 1_050_000,
  },
  {
    model_class: "claude-5",
    pattern: /^claude-(?:opus|sonnet|fable|mythos)-5(?:-|:|$)/,
    context_window_tokens: 1_000_000,
  },
  {
    model_class: "claude-4.6-4.8",
    pattern: /^claude-(?:opus|sonnet)-4[.-](?:6|7|8)(?:-|:|$)/,
    context_window_tokens: 1_000_000,
  },
  {
    model_class: "deepseek-v4",
    pattern: /^deepseek-v4(?:-(?:pro|flash))?(?:-|:|$)/,
    context_window_tokens: 1_000_000,
  },
  {
    model_class: "glm-5.2-5.3",
    pattern: /^(?:e2ee-)?glm-5\.(?:2|3)(?:-|:|$)/,
    context_window_tokens: 1_000_000,
  },
  {
    model_class: "glm-5-5.1",
    pattern: /^(?:e2ee-)?glm-(?:5(?:\.1)?|5v)(?:-|:|$)/,
    context_window_tokens: 200_000,
  },
  {
    model_class: "gemini-3.x",
    pattern: /^gemini-3(?:[.-](?:1|5|6|7))?-(?:flash(?:-lite)?|pro)(?:-|:|$)/,
    context_window_tokens: 1_048_576,
  },
  {
    model_class: "grok-4.20",
    pattern: /^grok-4\.20(?:-|:|$)/,
    context_window_tokens: 1_000_000,
  },
  {
    model_class: "grok-4.3",
    pattern: /^grok-4\.3(?:-|:|$)/,
    context_window_tokens: 1_000_000,
  },
  {
    model_class: "grok-4.5-4.6",
    pattern: /^grok-4\.(?:5|6)(?:-|:|$)/,
    context_window_tokens: 500_000,
  },
  {
    model_class: "grok-build-code-fast",
    pattern: /^grok-(?:build-0-1|code-fast-1)(?:-|:|$)/,
    context_window_tokens: 256_000,
  },
  {
    model_class: "kimi-k3",
    pattern: /^kimi-k3(?:-|:|$)/,
    context_window_tokens: 1_048_576,
  },
  {
    model_class: "kimi-k2.5-2.7",
    pattern: /^kimi-k2\.(?:5|6|7)(?:-code)?(?:-|:|$)/,
    context_window_tokens: 262_144,
  },
  {
    model_class: "qwen3.5-397b",
    pattern: /^(?:e2ee-)?qwen3\.5-397b-a17b(?:-|:|$)/,
    context_window_tokens: 262_144,
  },
  {
    model_class: "qwen3.6-27b",
    pattern: /^(?:e2ee-)?qwen3\.6-27b(?:-|:|$)/,
    context_window_tokens: 262_144,
  },
  {
    model_class: "qwen3.5-flash-plus",
    pattern: /^(?:e2ee-)?qwen3\.5-(?:flash|plus)(?:-|:|$)/,
    context_window_tokens: 1_000_000,
  },
  {
    model_class: "qwen3.6-plus",
    pattern: /^(?:e2ee-)?qwen3\.6-plus(?:-|:|$)/,
    context_window_tokens: 1_000_000,
  },
  {
    model_class: "qwen3.7-max-plus",
    pattern: /^(?:e2ee-)?qwen(?:3\.7|-3-7)-(?:max|plus)(?:-|:|$)/,
    context_window_tokens: 1_000_000,
  },
  {
    model_class: "qwen3.8-max",
    pattern: /^(?:e2ee-)?qwen(?:3\.8|-3-8)-max(?:-|:|$)/,
    context_window_tokens: 1_000_000,
  },
  {
    model_class: "qwen3.8-2.4t-a95b",
    pattern: /^(?:e2ee-)?qwen3\.8-2\.4t-a95b(?:-|:|$)/,
    context_window_tokens: 1_000_000,
  },
  {
    model_class: "minimax-m3",
    pattern: /^minimax-m3(?:-|:|$)/,
    context_window_tokens: 1_000_000,
  },
  {
    model_class: "minimax-m2.5-2.7",
    pattern: /^minimax-m2\.(?:5|7)(?:-|:|$)/,
    context_window_tokens: 204_800,
  },
];

const positiveSafeInteger = (value: number | null | undefined): number | null =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0 ? value : null;

const resolvedAutoCompactTokenLimit = (
  contextWindowTokens: number,
  configuredLimit: number | null | undefined,
): number => {
  const configured = positiveSafeInteger(configuredLimit);
  if (configured !== null && configured <= contextWindowTokens) return configured;
  if (contextWindowTokens > CONTEXT_COMPACTION_RESERVED_TOKENS) {
    return deriveAutoCompactTokenLimit(contextWindowTokens);
  }
  return Math.max(1, Math.min(contextWindowTokens - 1, Math.floor(contextWindowTokens * 0.85)));
};

export const recentModelContextFor = (
  modelId: string,
  overrides: RecentModelContextOverrides = {},
): RecentModelContext | null => {
  const normalized = modelId.trim().toLowerCase().split("/").at(-1) ?? "";
  if (!normalized) return null;
  const rule = RECENT_MODEL_CONTEXT_RULES.find((candidate) => candidate.pattern.test(normalized));
  if (!rule) return null;

  const nativeContextWindow = positiveSafeInteger(overrides.context_window_tokens);
  const nativeMaxContextWindow = positiveSafeInteger(overrides.max_context_window_tokens);
  const contextWindow = nativeContextWindow ?? nativeMaxContextWindow ?? rule.context_window_tokens;
  const maxContextWindow = Math.max(contextWindow, nativeMaxContextWindow ?? contextWindow);
  const effectiveContextWindowPercent = typeof overrides.effective_context_window_percent === "number" &&
      Number.isSafeInteger(overrides.effective_context_window_percent) &&
      overrides.effective_context_window_percent >= 1 &&
      overrides.effective_context_window_percent <= 100
    ? overrides.effective_context_window_percent
    : CODEX_EFFECTIVE_CONTEXT_WINDOW_PERCENT;

  return {
    model_class: rule.model_class,
    context_window_tokens: contextWindow,
    max_context_window_tokens: maxContextWindow,
    auto_compact_token_limit_tokens: resolvedAutoCompactTokenLimit(
      contextWindow,
      overrides.auto_compact_token_limit_tokens,
    ),
    effective_context_window_percent: effectiveContextWindowPercent,
  };
};
