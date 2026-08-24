// Reasoning controls for model classes released in the six months ending
// 2026-08-24. Match classes, not individual provider aliases, so dated
// snapshots and compatible-provider names inherit the same native controls.
//
// Sources used for this audit:
// - OpenAI GPT-5.6 model docs: https://developers.openai.com/api/docs/models
// - Anthropic effort docs: https://platform.claude.com/docs/en/build-with-claude/effort
// - DeepSeek V4 / Qwen provider behavior: https://qwenlm.github.io/qwen-code-docs/en/users/configuration/model-providers/
// - Z.AI GLM docs: https://docs.z.ai/guides/llm/glm-5.3
// - Google Gemini thinking docs: https://ai.google.dev/gemini-api/docs/thinking
// - xAI reasoning docs: https://docs.x.ai/developers/model-capabilities/text/reasoning
// - Qwen 3.8 effort docs: https://qwenlm.github.io/qwen-code-docs/en/users/configuration/model-providers/
//
// `levels` deliberately reflects each class's native control vocabulary rather
// than pretending every provider implements OpenAI's complete effort ladder.
const RULES = [
  {
    modelClass: "gpt-5.6",
    pattern: /(?:^|\/)gpt-5\.6(?:$|-)/i,
    levels: ["none", "low", "medium", "high", "xhigh", "max"],
    defaultLevel: "medium",
  },
  {
    modelClass: "claude-5",
    pattern: /(?:^|\/)claude-(?:opus|sonnet|fable|mythos)-5(?:$|-)/i,
    levels: ["low", "medium", "high", "xhigh", "max"],
    defaultLevel: "high",
  },
  {
    modelClass: "claude-opus-4.8",
    pattern: /(?:^|\/)claude-opus-4\.8(?:$|-)/i,
    levels: ["low", "medium", "high", "xhigh", "max"],
    defaultLevel: "high",
  },
  {
    modelClass: "deepseek-v4",
    pattern: /(?:^|\/)deepseek-v4(?:$|-)/i,
    levels: ["none", "high", "max"],
    defaultLevel: "high",
  },
  {
    modelClass: "glm-5.3",
    pattern: /(?:^|\/)glm-5\.3(?:$|-)/i,
    levels: ["low", "high", "max"],
    defaultLevel: "max",
  },
  {
    modelClass: "glm-5.2",
    pattern: /(?:^|\/)glm-5\.2(?:$|-)/i,
    levels: ["none", "minimal", "low", "medium", "high", "xhigh", "max"],
    defaultLevel: null,
  },
  {
    modelClass: "glm-5.1",
    pattern: /(?:^|\/)glm-5\.1(?:$|-)/i,
    levels: ["none", "minimal", "low", "medium", "high", "xhigh"],
    defaultLevel: null,
  },
  {
    modelClass: "kimi-k3",
    pattern: /(?:^|\/)kimi-k3(?:$|-)/i,
    levels: ["low", "high", "max"],
    defaultLevel: null,
  },
  {
    modelClass: "gemini-3.7-flash",
    pattern: /(?:^|\/)gemini-3\.7-flash(?:$|-)/i,
    levels: ["low", "medium", "high"],
    defaultLevel: "medium",
  },
  {
    modelClass: "gemini-3.6-flash",
    pattern: /(?:^|\/)gemini-3\.6-flash(?:$|-)/i,
    levels: ["minimal", "low", "medium", "high"],
    defaultLevel: null,
  },
  {
    modelClass: "gemini-3.5-flash",
    pattern: /(?:^|\/)gemini-3\.5-flash(?:$|-)/i,
    levels: ["minimal", "low", "medium", "high"],
    defaultLevel: null,
  },
  {
    modelClass: "gemini-3.1-pro",
    pattern: /(?:^|\/)gemini-3\.1-pro(?:$|-)/i,
    levels: ["low", "medium", "high"],
    defaultLevel: null,
  },
  {
    modelClass: "grok-4.20-multi-agent",
    pattern: /(?:^|\/)grok-4\.20-multi-agent(?:$|-)/i,
    levels: ["low", "medium", "high", "xhigh"],
    defaultLevel: null,
  },
  {
    modelClass: "grok-4.20-non-reasoning",
    pattern: /(?:^|\/)grok-4\.20[^/]*non-reasoning(?:$|-)/i,
    levels: ["none"],
    defaultLevel: "none",
  },
  {
    modelClass: "grok-4.20-reasoning",
    pattern: /(?:^|\/)grok-4\.20(?:$|-)/i,
    levels: ["reasoning"],
    defaultLevel: "reasoning",
  },
  {
    modelClass: "grok-4.6",
    pattern: /(?:^|\/)grok-4\.6(?:$|-)/i,
    levels: ["low", "medium", "high", "xhigh"],
    defaultLevel: "high",
  },
  {
    modelClass: "grok-4.5",
    pattern: /(?:^|\/)grok-4\.5(?:$|-)/i,
    levels: ["low", "medium", "high"],
    defaultLevel: "high",
  },
  {
    modelClass: "grok-4.3",
    pattern: /(?:^|\/)grok-4\.3(?:$|-)/i,
    levels: ["none", "low", "medium", "high"],
    defaultLevel: null,
  },
  {
    modelClass: "qwen3.8-max",
    pattern: /(?:^|\/)qwen3\.8-max(?:$|-)/i,
    levels: ["none", "low", "medium", "high", "xhigh", "max"],
    defaultLevel: null,
  },
  {
    modelClass: "qwen3.8",
    pattern: /(?:^|\/)qwen3\.8(?:$|-)/i,
    levels: ["none", "thinking"],
    defaultLevel: null,
  },
  {
    modelClass: "minimax-m3",
    pattern: /(?:^|\/)(?:minimax[-/])?m3(?:$|-)|(?:^|\/)minimax-m3(?:$|-)/i,
    levels: ["none", "adaptive", "enabled"],
    defaultLevel: "adaptive",
  },
];

export const reasoningLevelsForRecentModel = (modelId) => {
  const id = typeof modelId === "string" ? modelId.trim() : "";
  if (!id) return null;
  const rule = RULES.find(({ pattern }) => pattern.test(id));
  return rule
    ? { modelClass: rule.modelClass, levels: [...rule.levels], defaultLevel: rule.defaultLevel }
    : null;
};
