import assert from "node:assert/strict";

import { getReasoningEffortForChatRequest, updateReasoningSelectForModel } from "../static/reasoning-select.js";

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

Deno.test("reasoning select includes public model levels and filters internal levels", () => {
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
    ]);
  });
});

Deno.test("reasoning select hides none when model levels omit it", () => {
  withFakeDocument(() => {
    const select = createSelect();
    const selected = updateReasoningSelectForModel(select, {
      default_reasoning_level: "medium",
      supported_reasoning_levels: ["low", "medium", "high", "xhigh"],
    }, "none");

    assert.equal(selected, "medium");
    assert.equal(select.disabled, false);
    assert.deepEqual(select.options.map((option) => [option.value, option.textContent]), [
      ["", "Default"],
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
  assert.equal(getReasoningEffortForChatRequest("ultra"), undefined);
});
