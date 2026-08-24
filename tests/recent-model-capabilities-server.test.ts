import assert from "node:assert/strict";

import { getRecentGatewayModelCapabilities } from "../src/recent_model_capabilities.ts";

Deno.test("recent server capabilities expose safe context and compaction limits", () => {
  for (
    const model of [
      "gpt-5.6-sol",
      "gpt-5.6-terra",
      "gpt-5.6-luna",
      "claude-opus-5",
      "claude-sonnet-5",
      "deepseek-v4-pro",
      "deepseek-v4-flash",
      "glm-5.3",
      "kimi-k3",
      "gemini-3.7-flash",
      "grok-4.6",
      "qwen3.8-max",
      "minimax-m3",
    ]
  ) {
    const capability = getRecentGatewayModelCapabilities(model);
    assert.ok(capability, `${model}: missing capability envelope`);
    assert.ok(capability.supported_reasoning_levels.length > 0, `${model}: missing reasoning levels`);
    assert.ok(capability.context_window_tokens <= capability.max_context_window_tokens);
    assert.ok(capability.auto_compact_token_limit_tokens <= capability.context_window_tokens);
    if (capability.max_output_tokens !== null) {
      assert.ok(
        capability.context_window_tokens + capability.max_output_tokens <=
          capability.max_context_window_tokens,
      );
    }
  }
});

Deno.test("unknown or older model classes do not receive invented capabilities", () => {
  assert.equal(getRecentGatewayModelCapabilities("gpt-4o"), null);
  assert.equal(getRecentGatewayModelCapabilities("unknown-model"), null);
});
