import assert from "node:assert/strict";
import { setKvForTest } from "../src/kv.ts";
import {
  getOpenRouterTelemetryView,
  OPENROUTER_TELEMETRY_KEY,
  parseOpenRouterTelemetryRecord,
  recordOpenRouterTelemetry,
} from "../src/openrouter_telemetry.ts";
import { CountingKv } from "./helpers/counting_kv.ts";

Deno.test("OpenRouter telemetry exposes a truthful empty view", async () => {
  const kv = new CountingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  try {
    assert.deepEqual(await getOpenRouterTelemetryView(), {
      available: true,
      attempted_provider: null,
      trigger_class: null,
      circuit_transition: null,
      selected_model: null,
      task_type: null,
      latency_ms: null,
      terminal_status: null,
      semantic_commitment: null,
      observed_at_ms: null,
    });
  } finally {
    setKvForTest(null);
  }
});

Deno.test("OpenRouter telemetry stores only bounded aggregate fields", async () => {
  const kv = new CountingKv();
  setKvForTest(kv as unknown as Deno.Kv);
  const secret = "prompt-and-credential-must-not-persist";
  try {
    const untrusted = {
      attempted_provider: "chatgpt_codex,openrouter",
      trigger_class: "http_5xx",
      circuit_transition: "opened",
      selected_model: "google/gemini-2.5-pro",
      task_type: "coding",
      latency_ms: 42,
      terminal_status: "response.completed",
      semantic_commitment: "tool_call",
      observed_at_ms: 1_234,
      [secret]: secret,
    };
    await recordOpenRouterTelemetry(untrusted);
    const entry = kv.entries.get(JSON.stringify(OPENROUTER_TELEMETRY_KEY));
    assert.ok(entry);
    assert.deepEqual(entry.value, {
      v: 1,
      attempted_provider: "chatgpt_codex,openrouter",
      trigger_class: "http_5xx",
      circuit_transition: "opened",
      selected_model: "google/gemini-2.5-pro",
      task_type: "coding",
      latency_ms: 42,
      terminal_status: "response.completed",
      semantic_commitment: "tool_call",
      observed_at_ms: 1_234,
    });
    assert.equal(JSON.stringify(entry.value).includes(secret), false);
    assert.equal(
      parseOpenRouterTelemetryRecord({ ...entry.value, selected_model: secret.repeat(20) })?.selected_model,
      null,
    );
  } finally {
    setKvForTest(null);
  }
});

Deno.test("OpenRouter telemetry reports unavailable KV without inventing an observation", async () => {
  setKvForTest(null);
  assert.deepEqual(await getOpenRouterTelemetryView(), {
    available: false,
    attempted_provider: null,
    trigger_class: null,
    circuit_transition: null,
    selected_model: null,
    task_type: null,
    latency_ms: null,
    terminal_status: null,
    semantic_commitment: null,
    observed_at_ms: null,
  });
});

Deno.test("OpenRouter telemetry keeps provider health available when its KV read fails", async () => {
  setKvForTest({
    get: () => Promise.reject(new Error("telemetry read unavailable")),
  } as unknown as Deno.Kv);
  try {
    assert.deepEqual(await getOpenRouterTelemetryView(), {
      available: false,
      attempted_provider: null,
      trigger_class: null,
      circuit_transition: null,
      selected_model: null,
      task_type: null,
      latency_ms: null,
      terminal_status: null,
      semantic_commitment: null,
      observed_at_ms: null,
    });
  } finally {
    setKvForTest(null);
  }
});
