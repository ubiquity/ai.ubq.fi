import assert from "node:assert/strict";

import {
  CODEX_EFFECTIVE_CONTEXT_WINDOW_PERCENT,
  deriveAutoCompactTokenLimit,
  recentModelContextFor,
} from "../src/recent_model_context.ts";

type ContextCase = readonly [model: string, modelClass: string, contextWindow: number, autoCompact: number];

const cases: readonly ContextCase[] = [
  ["gpt-5.4-mini", "gpt-5.4-mini-nano", 400_000, 340_000],
  ["gpt-5.4-nano-2026-03-17", "gpt-5.4-mini-nano", 400_000, 340_000],
  ["gpt-5.4-pro", "gpt-5.4-5.6", 1_050_000, 892_500],
  ["gpt-5.5-pro", "gpt-5.4-5.6", 1_050_000, 892_500],
  ["openlux/gpt-5.6-sol-pro", "gpt-5.4-5.6", 1_050_000, 892_500],
  ["claude-opus-4-7-fast", "claude-4.6-4.8", 1_000_000, 850_000],
  ["claude-sonnet-5", "claude-5", 1_000_000, 850_000],
  ["deepseek-v4-flash-0731", "deepseek-v4", 1_000_000, 850_000],
  ["glm-5.1-non-thinking:web", "glm-5-5.1", 200_000, 150_000],
  ["glm-5v-turbo", "glm-5-5.1", 200_000, 150_000],
  ["glm-5.2:web", "glm-5.2-5.3", 1_000_000, 850_000],
  ["glm-5.3", "glm-5.2-5.3", 1_000_000, 850_000],
  ["gemini-3-5-flash", "gemini-3.x", 1_048_576, 891_289],
  ["gemini-3.1-pro-preview", "gemini-3.x", 1_048_576, 891_289],
  ["gemini-3.7-flash", "gemini-3.x", 1_048_576, 891_289],
  ["grok-4.20-multi-agent-beta", "grok-4.20", 1_000_000, 850_000],
  ["grok-4.3", "grok-4.3", 1_000_000, 850_000],
  ["grok-4.6", "grok-4.5-4.6", 500_000, 425_000],
  ["grok-code-fast-1", "grok-build-code-fast", 256_000, 206_000],
  ["kimi-k2.5:web", "kimi-k2.5-2.7", 262_144, 212_144],
  ["kimi-k2.7-code:web", "kimi-k2.5-2.7", 262_144, 212_144],
  ["kimi-k3", "kimi-k3", 1_048_576, 891_289],
  ["qwen3.5-397b-a17b", "qwen3.5-397b", 262_144, 212_144],
  ["qwen3.5-flash", "qwen3.5-flash-plus", 1_000_000, 850_000],
  ["qwen3.6-27b", "qwen3.6-27b", 262_144, 212_144],
  ["qwen3.6-plus-uncensored", "qwen3.6-plus", 1_000_000, 850_000],
  ["qwen-3-7-max", "qwen3.7-max-plus", 1_000_000, 850_000],
  ["qwen-3-8-max", "qwen3.8-max", 1_000_000, 850_000],
  ["qwen3.8-2.4t-a95b", "qwen3.8-2.4t-a95b", 1_000_000, 850_000],
  ["minimax-m2.7-highspeed", "minimax-m2.5-2.7", 204_800, 154_800],
  ["minimax-m3", "minimax-m3", 1_000_000, 850_000],
];

Deno.test("recent model context resolves provider aliases by model class", () => {
  for (const [model, modelClass, contextWindow, autoCompact] of cases) {
    const resolved = recentModelContextFor(model);
    assert.equal(resolved?.model_class, modelClass, model);
    assert.equal(resolved?.context_window_tokens, contextWindow, model);
    assert.equal(resolved?.max_context_window_tokens, contextWindow, model);
    assert.equal(resolved?.auto_compact_token_limit_tokens, autoCompact, model);
    assert.equal(resolved?.effective_context_window_percent, CODEX_EFFECTIVE_CONTEXT_WINDOW_PERCENT, model);
  }
});

Deno.test("recent model context leaves unrelated and older classes unclaimed", () => {
  for (const model of ["gpt-4o", "claude-sonnet-4.5", "deepseek-v3.2", "qwen3-32b"]) {
    assert.equal(recentModelContextFor(model), null, model);
  }
});

Deno.test("auto-compaction uses the earlier 85 percent or 50k-reserve boundary", () => {
  assert.equal(deriveAutoCompactTokenLimit(1_000_000), 850_000);
  assert.equal(deriveAutoCompactTokenLimit(400_000), 340_000);
  assert.equal(deriveAutoCompactTokenLimit(262_144), 212_144);
  assert.equal(deriveAutoCompactTokenLimit(204_800), 154_800);
  assert.throws(() => deriveAutoCompactTokenLimit(50_000), RangeError);
});
