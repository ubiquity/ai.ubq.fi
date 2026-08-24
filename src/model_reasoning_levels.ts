export type ModelReasoningLevels = Readonly<{
  model_class: string;
  levels: readonly string[];
  default_level: string | null;
}>;

type ModelReasoningRule = Readonly<{
  model_class: string;
  pattern: RegExp;
  levels: readonly string[];
  default_level: string | null;
}>;

// Curated from provider/vendor documentation for model classes released in the
// six months ending 2026-08-24. These are model-native controls, not a generic
// ladder: e.g. DeepSeek V4 has off/high/max, Grok 4.6 has low..xhigh and cannot
// be disabled, while MiniMax M3 exposes thinking modes rather than effort tiers.
const RECENT_MODEL_REASONING_RULES: readonly ModelReasoningRule[] = [
  {
    model_class: "gpt-5.6",
    pattern: /(?:^|\/)gpt-5\.6(?:$|-(?:sol|terra|luna)(?:$|-)|-)/i,
    levels: ["none", "low", "medium", "high", "xhigh", "max"],
    default_level: "medium",
  },
  {
    model_class: "claude-5",
    pattern: /(?:^|\/)claude-(?:opus|sonnet|fable|mythos)-5(?:$|-)/i,
    levels: ["low", "medium", "high", "xhigh", "max"],
    default_level: "high",
  },
  {
    model_class: "claude-opus-4.8",
    pattern: /(?:^|\/)claude-opus-4\.8(?:$|-)/i,
    levels: ["low", "medium", "high", "xhigh", "max"],
    default_level: "high",
  },
  {
    model_class: "claude-opus-4.7",
    pattern: /(?:^|\/)claude-opus-4\.7(?:$|-)/i,
    levels: ["low", "medium", "high", "xhigh", "max"],
    default_level: "high",
  },
  {
    model_class: "claude-4.6",
    pattern: /(?:^|\/)claude-(?:opus|sonnet)-4\.6(?:$|-)/i,
    levels: ["low", "medium", "high", "max"],
    default_level: "high",
  },
  {
    model_class: "deepseek-v4",
    pattern: /(?:^|\/)deepseek-v4(?:$|-(?:pro|flash)(?:$|-)|-)/i,
    levels: ["none", "high", "max"],
    default_level: "high",
  },
  {
    model_class: "glm-5.3",
    pattern: /(?:^|\/)glm-5\.3(?:$|-)/i,
    levels: ["low", "high", "max"],
    default_level: "max",
  },
  {
    model_class: "glm-5.2",
    pattern: /(?:^|\/)glm-5\.2(?:$|-)/i,
    levels: ["none", "minimal", "low", "medium", "high", "xhigh", "max"],
    default_level: null,
  },
  {
    model_class: "glm-5.1",
    pattern: /(?:^|\/)glm-5\.1(?:$|-)/i,
    levels: ["none", "minimal", "low", "medium", "high", "xhigh"],
    default_level: null,
  },
  {
    model_class: "kimi-k3",
    pattern: /(?:^|\/)kimi-k3(?:$|-)/i,
    levels: ["low", "high", "max"],
    default_level: null,
  },
  {
    model_class: "gemini-3.7-flash",
    pattern: /(?:^|\/)gemini-3\.7-flash(?:$|-)/i,
    levels: ["low", "medium", "high"],
    default_level: "medium",
  },
  {
    model_class: "gemini-3.6-flash",
    pattern: /(?:^|\/)gemini-3\.6-flash(?:$|-)/i,
    levels: ["minimal", "low", "medium", "high"],
    default_level: null,
  },
  {
    model_class: "gemini-3.5-flash",
    pattern: /(?:^|\/)gemini-3\.5-flash(?:$|-)/i,
    levels: ["minimal", "low", "medium", "high"],
    default_level: null,
  },
  {
    model_class: "gemini-3.1-pro",
    pattern: /(?:^|\/)gemini-3\.1-pro(?:$|-)/i,
    levels: ["low", "medium", "high"],
    default_level: null,
  },
  {
    model_class: "grok-4.20-multi-agent",
    pattern: /(?:^|\/)grok-4\.20-multi-agent(?:$|-)/i,
    levels: ["low", "medium", "high", "xhigh"],
    default_level: null,
  },
  {
    model_class: "grok-4.20",
    pattern: /(?:^|\/)grok-4\.20(?:$|-)/i,
    levels: ["reasoning", "none"],
    default_level: "reasoning",
  },
  {
    model_class: "grok-4.6",
    pattern: /(?:^|\/)grok-4\.6(?:$|-)/i,
    levels: ["low", "medium", "high", "xhigh"],
    default_level: "high",
  },
  {
    model_class: "grok-4.5",
    pattern: /(?:^|\/)grok-4\.5(?:$|-)/i,
    levels: ["low", "medium", "high"],
    default_level: "high",
  },
  {
    model_class: "grok-4.3",
    pattern: /(?:^|\/)grok-4\.3(?:$|-)/i,
    levels: ["none", "low", "medium", "high"],
    default_level: null,
  },
  {
    model_class: "qwen3.8-max",
    pattern: /(?:^|\/)qwen3\.8-max(?:$|-)/i,
    levels: ["none", "low", "medium", "high", "xhigh", "max"],
    default_level: null,
  },
  {
    model_class: "qwen3.8",
    pattern: /(?:^|\/)qwen3\.8(?:$|-)/i,
    levels: ["none", "thinking"],
    default_level: null,
  },
  {
    model_class: "minimax-m3",
    pattern: /(?:^|\/)(?:minimax[-/])?m3(?:$|-)|(?:^|\/)minimax-m3(?:$|-)/i,
    levels: ["none", "adaptive", "enabled"],
    default_level: "adaptive",
  },
];

export const reasoningLevelsForRecentModel = (modelId: string): ModelReasoningLevels | null => {
  const id = modelId.trim();
  if (!id) return null;
  const rule = RECENT_MODEL_REASONING_RULES.find((candidate) => candidate.pattern.test(id));
  return rule
    ? {
      model_class: rule.model_class,
      levels: rule.levels,
      default_level: rule.default_level,
    }
    : null;
};
