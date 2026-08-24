import assert from "node:assert/strict";

import {
  getReasoningEffortForChatRequest,
  getRecentModelReasoning,
  updateReasoningSelectForModel,
} from "../static/reasoning-select.js";

type FakeOption = {
  disabled?: boolean;
  textContent: string;
  value: string;
};

type FakeSelect = {
  disabled: boolean;
  options: FakeOption[];
  textContent: string;
  value: string;
  appendChild: (option: FakeOption) => void;
};

const withFakeDocument = (fn: () => void) => {
  const originalDocument = Object.getOwnPropertyDescriptor(globalThis, "document");
  Object.defineProperty(globalThis, "document", {
    configurable: true,
    value: {
      createElement: (tag: string): FakeOption => {
        assert.equal(tag, "option");
        return { textContent: "", value: "" };
      },
    },
  });
  try {
    fn();
  } finally {
    if (originalDocument) {
      Object.defineProperty(globalThis, "document", originalDocument);
    } else {
      Reflect.deleteProperty(globalThis, "document");
    }
  }
};

const createSelect = (): FakeSelect => ({
  disabled: true,
  options: [],
  textContent: "stale",
  value: "",
  appendChild(option) {
    this.options.push(option);
  },
});

Deno.test("recent model reasoning resolves provider aliases by model class", () => {
  const cases = [
    ["gpt-5.6-sol-max", "gpt-5.6", ["none", "low", "medium", "high", "xhigh", "max"]],
    ["gpt-5.6-terra", "gpt-5.6", ["none", "low", "medium", "high", "xhigh", "max"]],
    ["claude-sonnet-5-20260801", "claude-5", ["low", "medium", "high", "xhigh", "max"]],
    ["claude-opus-4.8", "claude-opus-4.8", ["low", "medium", "high", "xhigh", "max"]],
    ["claude-opus-4.7", "claude-opus-4.7", ["low", "medium", "high", "xhigh", "max"]],
    ["deepseek-v4-flash", "deepseek-v4", ["none", "high", "max"]],
    ["deepseek-v4-pro", "deepseek-v4", ["none", "high", "max"]],
    ["glm-5.3", "glm-5.3", ["low", "high", "max"]],
    ["glm-5.2-fast-preview", "glm-5.2", ["none", "minimal", "low", "medium", "high", "xhigh", "max"]],
    ["glm-5.1", "glm-5.1", ["none", "minimal", "low", "medium", "high", "xhigh"]],
    ["kimi-k3", "kimi-k3", ["low", "high", "max"]],
    ["gemini-3.7-flash-preview", "gemini-3.7-flash", ["low", "medium", "high"]],
    ["grok-4.6", "grok-4.6", ["low", "medium", "high", "xhigh"]],
    ["grok-4.5", "grok-4.5", ["low", "medium", "high"]],
    ["grok-4.3", "grok-4.3", ["none", "low", "medium", "high"]],
    ["grok-4.20-beta", "grok-4.20-reasoning", ["reasoning"]],
    ["grok-4.20-beta-latest-non-reasoning", "grok-4.20-non-reasoning", ["none"]],
    ["grok-4.20-multi-agent-0309", "grok-4.20-multi-agent", ["low", "medium", "high", "xhigh"]],
    ["qwen3.8-max-20260803", "qwen3.8-max", ["none", "low", "medium", "high", "xhigh", "max"]],
    ["qwen3.8-2.4t-a95b", "qwen3.8", ["none", "thinking"]],
    ["minimax-m3", "minimax-m3", ["none", "adaptive", "enabled"]],
  ] as const;

  for (const [model, modelClass, levels] of cases) {
    const resolved = getRecentModelReasoning(model);
    assert.equal(resolved?.modelClass, modelClass, model);
    assert.deepEqual(resolved?.levels, levels, model);
  }
  assert.equal(getRecentModelReasoning("gpt-4o"), null);
});

Deno.test("reasoning select preserves every tier advertised by the model catalog", () => {
  withFakeDocument(() => {
    const select = createSelect();
    const selected = updateReasoningSelectForModel(select, {
      default_reasoning_level: "medium",
      supported_reasoning_levels: [{ effort: null }, "low", "medium", "high", "xhigh", "max", "ultra"],
    }, "none");

    assert.equal(selected, "none");
    assert.equal(select.disabled, false);
    assert.deepEqual(select.options.map((option) => [option.value, option.textContent]), [
      ["", "Default"],
      ["none", "None"],
      ["low", "low"],
      ["medium", "medium"],
      ["high", "high"],
      ["xhigh", "xhigh"],
      ["max", "max"],
      ["ultra", "ultra"],
    ]);
  });
});

Deno.test("reasoning select preserves none when model levels omit it", () => {
  withFakeDocument(() => {
    const select = createSelect();
    const selected = updateReasoningSelectForModel(select, {
      default_reasoning_level: "medium",
      supported_reasoning_levels: ["low", "medium", "high", "xhigh"],
    }, "none");

    assert.equal(selected, "none");
    assert.equal(select.disabled, false);
    assert.deepEqual(select.options.map((option) => [option.value, option.textContent]), [
      ["", "Default"],
      ["none", "None"],
      ["low", "low"],
      ["medium", "medium"],
      ["high", "high"],
      ["xhigh", "xhigh"],
    ]);
  });
});

Deno.test("reasoning select shows none when model default is none", () => {
  withFakeDocument(() => {
    const select = createSelect();
    const selected = updateReasoningSelectForModel(select, {
      default_reasoning_level: null,
      supported_reasoning_levels: ["low", "medium", "high"],
    }, "none");

    assert.equal(selected, "none");
    assert.equal(select.disabled, false);
    assert.deepEqual(select.options.map((option) => [option.value, option.textContent]), [
      ["", "Default"],
      ["none", "None"],
      ["low", "low"],
      ["medium", "medium"],
      ["high", "high"],
    ]);
  });
});

Deno.test("chat reasoning none selection uses OpenAI wire value", () => {
  assert.equal(getReasoningEffortForChatRequest("none"), "none");
  assert.equal(getReasoningEffortForChatRequest("max"), "max");
  assert.equal(getReasoningEffortForChatRequest("ultra"), "ultra");
});
