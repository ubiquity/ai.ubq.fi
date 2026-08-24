import assert from "node:assert/strict";

import { getRecentModelCapabilities } from "../static/reasoning-select.js";

const RECENT_CATALOG_MODELS = [
  "gpt-5.6-sol",
  "gpt-5.6-terra",
  "gpt-5.6-luna",
  "claude-opus-5",
  "claude-sonnet-5",
  "claude-fable-5",
  "claude-opus-4.8",
  "deepseek-v4-pro",
  "deepseek-v4-flash",
  "glm-5.3",
  "kimi-k3",
  "gemini-3.7-flash",
  "grok-4.5",
  "grok-4.6",
  "grok-4.20-beta",
  "qwen3.8-max",
  "qwen3.8-2.4t-a95b",
  "minimax-m3",
] as const;

const positiveSafeInteger = (value: unknown): value is number =>
  typeof value === "number" && Number.isSafeInteger(value) && value > 0;

Deno.test("recent catalog classes carry usable context and auto-compaction limits", () => {
  for (const model of RECENT_CATALOG_MODELS) {
    const capability = getRecentModelCapabilities(model);
    assert.ok(capability, `${model}: missing class capability`);
    assert.ok(
      positiveSafeInteger(capability.max_context_window_tokens),
      `${model}: missing total context window`,
    );
    assert.ok(
      positiveSafeInteger(capability.context_window_tokens),
      `${model}: missing usable prompt/history window`,
    );
    assert.ok(
      positiveSafeInteger(capability.auto_compact_token_limit_tokens),
      `${model}: missing auto-compaction threshold`,
    );
    assert.ok(
      capability.context_window_tokens <= capability.max_context_window_tokens,
      `${model}: usable history exceeds total context`,
    );
    assert.ok(
      capability.auto_compact_token_limit_tokens <= capability.context_window_tokens,
      `${model}: compaction starts after the usable history budget`,
    );
    if (positiveSafeInteger(capability.max_output_tokens)) {
      assert.ok(
        capability.context_window_tokens + capability.max_output_tokens <=
          capability.max_context_window_tokens,
        `${model}: prompt plus output allowance exceeds total context`,
      );
    }
  }
});

Deno.test("recent context aliases inherit their model-class limits", () => {
  const base = getRecentModelCapabilities("gpt-5.6-sol");
  const alias = getRecentModelCapabilities("gpt-5.6-sol-max");
  assert.ok(base?.context_window_tokens);
  assert.equal(alias?.modelClass, "gpt-5.6");
  assert.equal(alias?.context_window_tokens, base.context_window_tokens);
  assert.equal(
    alias?.auto_compact_token_limit_tokens,
    base.auto_compact_token_limit_tokens,
  );
});
