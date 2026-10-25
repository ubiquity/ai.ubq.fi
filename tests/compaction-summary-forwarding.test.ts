import { test } from "bun:test";
import assert from "node:assert/strict";

import { toDeepSeekResponsesChatBody } from "../src/deepseek/chat-projection.ts";
import { estimateTokens } from "../lib/jev_compaction/state.ts";
import {
  COMPACTION_SUMMARY_POLICY,
  FORWARDED_PAYLOAD_POLICY,
  PROMPT_ADMISSION_POLICY,
  PROMPT_ADMISSION_PROFILE_WINDOWS,
  logForwardedPayloadExemptions,
} from "../src/deepseek/forwarded-payload-policy.ts";
import { CEREBRAS_RESPONSES_PROFILE, DEEPSEEK_RESPONSES_PROFILE, LITHOS_RESPONSES_PROFILE } from "../src/deepseek/responses.ts";
import { handleCerebrasResponses } from "../src/provider/cerebras-responses.ts";
import { projectCerebrasRequest } from "../src/provider/cerebras.ts";
import { createFetchMock } from "./helpers/fetch-mock.ts";

const LIMIT = FORWARDED_PAYLOAD_POLICY.perMessageLimit;
const SUMMARY_TARGET_BYTES = 70 * 1024;
const byteLength = (value: string): number => new TextEncoder().encode(value).byteLength;
const summaryText = (bytes = SUMMARY_TARGET_BYTES): string => {
  const head = `${COMPACTION_SUMMARY_POLICY.prefix}\n`;
  return head + "s".repeat(bytes - byteLength(head));
};
const summaryMessage = (text: string, role = "user"): Record<string, unknown> => ({
  type: "message",
  role,
  content: [{ type: "input_text", text }],
});
const request = (input: unknown[], extra: Record<string, unknown> = {}): Record<string, unknown> => ({ input, ...extra });
const expectedFailure = (result: ReturnType<typeof toDeepSeekResponsesChatBody>): Extract<typeof result, { ok: false }> => {
  if (result.ok) throw new Error("expected the projection to fail closed");
  return result;
};

const SUMMARY = summaryText();
assert.equal(byteLength(SUMMARY), SUMMARY_TARGET_BYTES);

test("compaction summary: the exact stock prefix and 70 KiB envelope are forwarded byte-identically", () => {
  assert.equal(byteLength(COMPACTION_SUMMARY_POLICY.prefix), COMPACTION_SUMMARY_POLICY.prefixBytes);
  assert.equal(new Bun.CryptoHasher("sha256").update(COMPACTION_SUMMARY_POLICY.prefix).digest("hex"), COMPACTION_SUMMARY_POLICY.prefixSha256);

  const result = toDeepSeekResponsesChatBody(request([summaryMessage(SUMMARY)]), "deepseek-flash", false, DEEPSEEK_RESPONSES_PROFILE);
  if (!result.ok) throw new Error(result.message);
  const messages = result.value.body.messages as Record<string, unknown>[];
  assert.equal(messages.length, 1);
  assert.equal(messages[0].role, "user");
  assert.equal(messages[0].content, SUMMARY);
  assert.deepEqual(result.value.elisions, []);
  assert.deepEqual(result.value.exemptions, [
    {
      path: "input[0].content",
      kind: "compaction_summary",
      summaryPolicy: COMPACTION_SUMMARY_POLICY.version,
      prefixSha256: COMPACTION_SUMMARY_POLICY.prefixSha256,
      bytes: SUMMARY_TARGET_BYTES,
      contentParts: 1,
      perMessageLimit: LIMIT,
    },
  ]);
  const admission = result.value.promptAdmission;
  assert.equal(admission.contextWindowTokens, 1_000_000);
  assert.equal(admission.outputReserveTokens, PROMPT_ADMISSION_POLICY.minimumOutputReserveTokens);
  assert.equal(admission.promptBudgetTokens, 1_000_000 - PROMPT_ADMISSION_POLICY.minimumOutputReserveTokens);
  assert.equal(admission.estimatedPromptTokens, estimateTokens(JSON.stringify(result.value.body)) + admission.structuralOverheadTokens);
  assert.equal(admission.estimatedPromptTokens < admission.serializedBodyBytes, true);
  assert.equal(admission.admitted, true);

  const logs: unknown[][] = [];
  const originalInfo = console.info;
  console.info = (...args: unknown[]) => logs.push(args);
  try {
    logForwardedPayloadExemptions(result.value.exemptions, { requestId: "req_test", model: "deepseek-flash", promptAdmission: result.value.promptAdmission });
  } finally {
    console.info = originalInfo;
  }
  assert.equal(logs.length, 1);
  assert.equal(logs[0][0], "[ai.ubq.fi] forwarding_exemption");
  assert.deepEqual(JSON.parse(String(logs[0][1])), {
    policy: FORWARDED_PAYLOAD_POLICY.version,
    request_id: "req_test",
    model: "deepseek-flash",
    prompt_admission: result.value.promptAdmission,
    ...result.value.exemptions[0],
    prefixBytes: COMPACTION_SUMMARY_POLICY.prefixBytes,
  });
});

test("compaction summary: an unprefixed oversized message keeps the generic reduction", () => {
  const oversized = "x".repeat(SUMMARY_TARGET_BYTES);
  const result = toDeepSeekResponsesChatBody(request([summaryMessage(oversized)]), "deepseek-flash", false);
  if (!result.ok) throw new Error(result.message);
  const messages = result.value.body.messages as Record<string, unknown>[];
  const content = String(messages[0].content);
  assert.equal(byteLength(content) <= LIMIT, true);
  assert.equal(content.includes("bytes omitted"), true);
  assert.equal(content.includes(FORWARDED_PAYLOAD_POLICY.version), true);
  assert.deepEqual(result.value.exemptions, []);
  assert.equal(result.value.elisions.length, 1);
  assert.equal(result.value.elisions[0].kind, "message");
});

test("compaction summary: wrong role, extra parts, missing newline, and media/tool parts fail closed", () => {
  const cases: readonly Record<string, unknown>[] = [
    summaryMessage(SUMMARY, "assistant"),
    { type: "agent_message", role: "user", author: "a", recipient: "b", content: [{ type: "input_text", text: SUMMARY }] },
    {
      type: "message",
      role: "user",
      content: [
        { type: "input_text", text: SUMMARY },
        { type: "input_text", text: "extra" },
      ],
    },
    summaryMessage(`${COMPACTION_SUMMARY_POLICY.prefix} without the required newline`),
    {
      type: "message",
      role: "user",
      content: [
        { type: "input_text", text: SUMMARY },
        { type: "input_image", image_url: "data:image/png;base64,AQID" },
      ],
    },
    {
      type: "message",
      role: "user",
      content: [
        { type: "input_text", text: SUMMARY },
        { type: "input_audio", input_audio: "AA==" },
      ],
    },
    {
      type: "message",
      role: "user",
      content: [
        { type: "input_text", text: SUMMARY },
        { type: "function_call_output", output: "{}" },
      ],
    },
  ];
  for (const [index, item] of cases.entries()) {
    const failure = expectedFailure(toDeepSeekResponsesChatBody(request([item]), "deepseek-flash", false));
    assert.equal(failure.code, "context_length_exceeded", `case ${index}`);
    assert.equal(failure.param, "input[0]", `case ${index}`);
    assert.equal(failure.message.includes("The summary was not reduced or forwarded"), true, `case ${index}`);
  }
});

test("compaction summary: two exact envelopes fail closed instead of choosing one", () => {
  const failure = expectedFailure(toDeepSeekResponsesChatBody(request([summaryMessage(SUMMARY), summaryMessage(SUMMARY)]), "deepseek-flash", false));
  assert.equal(failure.code, "context_length_exceeded");
  assert.equal(failure.param, "input[1]");
  assert.equal(failure.message.includes("adds a second summary envelope"), true);
});

test("compaction summary: prefix-bearing string inputs fail closed", () => {
  const topLevel = expectedFailure(toDeepSeekResponsesChatBody({ input: SUMMARY }, "deepseek-flash", false));
  assert.equal(topLevel.code, "context_length_exceeded");
  assert.equal(topLevel.param, "input");

  const arrayItem = expectedFailure(toDeepSeekResponsesChatBody(request([SUMMARY]), "deepseek-flash", false));
  assert.equal(arrayItem.code, "context_length_exceeded");
  assert.equal(arrayItem.param, "input[0]");
});

test("compaction summary: summary exemption does not relax tool-output bounds", () => {
  const output = "o".repeat(SUMMARY_TARGET_BYTES);
  const result = toDeepSeekResponsesChatBody(
    request([
      summaryMessage(SUMMARY),
      { type: "function_call", name: "read_file", arguments: "{}", call_id: "call_after_summary" },
      { type: "function_call_output", call_id: "call_after_summary", output },
    ]),
    "deepseek-flash",
    false
  );
  if (!result.ok) throw new Error(result.message);
  const messages = result.value.body.messages as Record<string, unknown>[];
  assert.equal(messages[0].content, SUMMARY);
  const toolContent = String(messages.at(-1)?.content);
  assert.equal(byteLength(toolContent) <= LIMIT, true);
  assert.equal(toolContent.includes("bytes omitted"), true);
  assert.equal(result.value.exemptions.length, 1);
  assert.equal(result.value.elisions.length, 1);
  assert.equal(result.value.elisions[0].kind, "tool_output");
});

test("compaction summary: tool arguments and reasoning retain their generic bounds", () => {
  const oversized = "x".repeat(LIMIT + 1_024);
  const toolArguments = toDeepSeekResponsesChatBody(
    request([{ type: "function_call", name: "read_file", arguments: oversized, call_id: "call_args" }]),
    "deepseek-flash",
    false
  );
  if (!toolArguments.ok) throw new Error(toolArguments.message);
  const call = (toolArguments.value.body.messages as Record<string, unknown>[])[0].tool_calls as Record<string, unknown>[];
  const functionCall = call[0].function as Record<string, unknown>;
  assert.equal(byteLength(String(functionCall.arguments)) <= LIMIT, true);
  assert.equal(String(functionCall.arguments).includes("bytes omitted"), true);
  const parsedArguments = JSON.parse(String(functionCall.arguments)) as { gateway_elision: Record<string, unknown> };
  assert.equal(parsedArguments.gateway_elision.kind, "tool_arguments");
  assert.equal(parsedArguments.gateway_elision.originalBytes, byteLength(oversized));
  assert.equal(parsedArguments.gateway_elision.omittedBytes, byteLength(oversized));
  assert.deepEqual(
    toolArguments.value.elisions.map((item) => item.kind),
    ["tool_arguments"]
  );

  const reasoning = toDeepSeekResponsesChatBody(
    request([
      { type: "reasoning", summary: [{ type: "summary_text", text: oversized }] },
      { type: "message", role: "assistant", content: [{ type: "output_text", text: "answer" }] },
    ]),
    "deepseek-flash",
    false
  );
  if (!reasoning.ok) throw new Error(reasoning.message);
  const assistant = (reasoning.value.body.messages as Record<string, unknown>[])[0];
  assert.equal(byteLength(String(assistant.reasoning_content)) <= LIMIT, true);
  assert.equal(String(assistant.reasoning_content).includes("bytes omitted"), true);
  assert.deepEqual(
    reasoning.value.elisions.map((item) => item.kind),
    ["reasoning"]
  );

  const refused = expectedFailure(
    toDeepSeekResponsesChatBody(
      request([{ type: "function_call", name: "read_file", arguments: oversized, call_id: "call_args" }], { truncation: "disabled" }),
      "deepseek-flash",
      false
    )
  );
  assert.equal(refused.param, "input[0].arguments");
  assert.equal(refused.code, "context_length_exceeded");
});

test("compaction summary: truncation disabled accepts an exempt summary only inside aggregate admission", () => {
  const accepted = toDeepSeekResponsesChatBody(
    request([summaryMessage(SUMMARY)], { truncation: "disabled" }),
    "gpt-oss-120b",
    false,
    CEREBRAS_RESPONSES_PROFILE
  );
  if (!accepted.ok) throw new Error(accepted.message);
  assert.equal((accepted.value.body.messages as Record<string, unknown>[])[0].content, SUMMARY);
  assert.equal(accepted.value.exemptions.length, 1);

  const oversizedSummary = summaryText(720 * 1024);
  const refused = expectedFailure(
    toDeepSeekResponsesChatBody(request([summaryMessage(oversizedSummary)], { truncation: "disabled" }), "gpt-oss-120b", false, CEREBRAS_RESPONSES_PROFILE)
  );
  assert.equal(refused.code, "context_length_exceeded");
  assert.equal(refused.param, "input");
  assert.equal(refused.message.includes("no payload was dispatched"), true);
});

test("aggregate admission: a steer drops redundant history before an adopted summary", () => {
  const old = Array.from({ length: 100 }, (_, index) => ({
    type: "message",
    role: "user",
    content: [{ type: "input_text", text: `old message ${index} ${"x".repeat(60 * 1024)}` }],
  }));
  const summary = summaryText(720 * 1024);
  const steer = { type: "message", role: "user", content: [{ type: "input_text", text: "steer now" }] };
  const input = [...old, summaryMessage(summary), steer];

  const reduced = toDeepSeekResponsesChatBody(request(input), "deepseek-flash", true, DEEPSEEK_RESPONSES_PROFILE);
  if (!reduced.ok) throw new Error(reduced.message);
  const messages = reduced.value.body.messages as Record<string, unknown>[];
  assert.equal(messages.length, 3);
  assert.equal(messages[0].role, "system");
  assert.match(String(messages[0].content), /omitted 100 earlier messages before the compaction summary/);
  assert.equal(messages[1].role, "user");
  assert.equal(messages[1].content, summary);
  assert.equal(messages[2].role, "user");
  assert.equal(messages[2].content, "steer now");
  assert.equal(reduced.value.elisions.length, 1);
  assert.equal(reduced.value.elisions[0].kind, "history");
  assert.equal(reduced.value.elisions[0].path, "messages[0..99]");
  assert.equal(reduced.value.elisions[0].omittedBytes > 2_000_000, true);
  assert.equal(reduced.value.promptAdmission.admitted, true);
  assert.equal(reduced.value.promptAdmission.policy, "chat-prompt-admission/v2");

  const refused = expectedFailure(toDeepSeekResponsesChatBody(request(input, { truncation: "disabled" }), "deepseek-flash", true, DEEPSEEK_RESPONSES_PROFILE));
  assert.equal(refused.code, "context_length_exceeded");
  assert.equal(refused.param, "input");
});

test("aggregate admission: instructions, tools, and images count beyond the summary envelope", () => {
  const image = `data:image/png;base64,${"A".repeat(55 * 1024)}`;
  const cases: readonly Record<string, unknown>[] = [
    request([summaryMessage(summaryText(620 * 1024))], { instructions: "i".repeat(65_536) }),
    request([summaryMessage(SUMMARY)], {
      tools: [{ type: "function", name: "large", description: "t".repeat(700 * 1024), parameters: { type: "object", properties: {} } }],
    }),
    {
      input: [
        summaryMessage(SUMMARY),
        ...Array.from({ length: 12 }, () => ({
          type: "message",
          role: "user",
          // Each image stays under the per-payload bound; their sum exceeds the prompt budget.
          content: [{ type: "input_image", image_url: image }],
        })),
      ],
    },
  ];
  for (const [index, body] of cases.entries()) {
    const failure = expectedFailure(toDeepSeekResponsesChatBody(body, "gpt-oss-120b", false, CEREBRAS_RESPONSES_PROFILE));
    assert.equal(failure.code, "context_length_exceeded", `aggregate case ${index}`);
    assert.equal(failure.param, "input", `aggregate case ${index}`);
  }
});

test("aggregate admission: DeepSeek, Lithos, and Cerebras windows stay distinct", () => {
  assert.deepEqual(PROMPT_ADMISSION_PROFILE_WINDOWS, {
    deepseek: 1_000_000,
    lithos: 1_048_576,
    cerebras: 131_072,
  });
  assert.equal(new Set(Object.values(PROMPT_ADMISSION_PROFILE_WINDOWS)).size, 3);

  const largeSummary = summaryText(720 * 1024);
  const deepseek = toDeepSeekResponsesChatBody(request([summaryMessage(largeSummary)]), "deepseek-flash", false, DEEPSEEK_RESPONSES_PROFILE);
  assert.equal(deepseek.ok, true);
  const lithos = toDeepSeekResponsesChatBody(request([summaryMessage(largeSummary)]), "deepseek-ai/DeepSeek-V4.1-Flash", false, LITHOS_RESPONSES_PROFILE);
  assert.equal(lithos.ok, true);
  const cerebras = expectedFailure(toDeepSeekResponsesChatBody(request([summaryMessage(largeSummary)]), "gpt-oss-120b", false, CEREBRAS_RESPONSES_PROFILE));
  assert.equal(cerebras.code, "context_length_exceeded");
});

test("aggregate admission: Cerebras returns 400 before any upstream dispatch", async () => {
  const body = request([summaryMessage(summaryText(720 * 1024))]);
  const upstream = new Request("https://ai.ubq.fi/v1/responses", {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });
  const originalFetch = globalThis.fetch;
  let calls = 0;
  globalThis.fetch = createFetchMock(() => {
    calls += 1;
    throw new Error("aggregate overflow must not dispatch");
  });
  try {
    const response = await handleCerebrasResponses(upstream, body, "gpt-oss-120b");
    assert.equal(response.status, 400);
    const payload = (await response.json()) as Record<string, unknown>;
    const error = payload.error as Record<string, unknown>;
    assert.equal(error.code, "context_length_exceeded");
    assert.equal(error.param, "input");
    assert.equal(calls, 0);
  } finally {
    globalThis.fetch = originalFetch;
  }
});

test("aggregate admission: the calibrated estimator admits a 1.5 MiB normal transcript", () => {
  const base =
    "alpha bravo charlie delta echo foxtrot golf hotel india juliet kilo lima mike november oscar papa quebec romeo sierra tango uniform victor whiskey xray yankee zulu ";
  const chunk = base.repeat(Math.ceil((64 * 1024) / base.length)).slice(0, 64 * 1024);
  const input = Array.from({ length: 24 }, () => summaryMessage(chunk));
  const result = toDeepSeekResponsesChatBody(request(input), "deepseek-flash", false, DEEPSEEK_RESPONSES_PROFILE);
  if (!result.ok) throw new Error(result.message);
  assert.equal(result.value.elisions.length, 0);
  assert.equal(result.value.promptAdmission.serializedBodyBytes > 1_500_000, true);
  assert.equal(result.value.promptAdmission.estimatedPromptTokens < 1_000_000, true);
});

test("aggregate admission: Cerebras measures the body after its projection drops reasoning", () => {
  const reasoning = { type: "reasoning", summary: [{ type: "summary_text", text: "r".repeat(60 * 1024) }] };
  const input = [
    summaryMessage(SUMMARY),
    ...Array.from({ length: 20 }, () => reasoning),
    { type: "message", role: "assistant", content: [{ type: "output_text", text: "answer" }] },
  ];
  const reported = request(input);
  const withoutProjection = expectedFailure(toDeepSeekResponsesChatBody(reported, "gpt-oss-120b", false, CEREBRAS_RESPONSES_PROFILE));
  assert.equal(withoutProjection.code, "context_length_exceeded");

  const projected = toDeepSeekResponsesChatBody(reported, "gpt-oss-120b", false, CEREBRAS_RESPONSES_PROFILE, projectCerebrasRequest);
  if (!projected.ok) throw new Error(projected.message);
  assert.equal(projected.value.promptAdmission.admitted, true);
  assert.equal(projected.value.promptAdmission.serializedBodyBytes < 200_000, true);
});

test("forwarded payload: the 744,586-byte image incident is reduced before aggregate admission", () => {
  const imageBytes = 744_586;
  const prefix = "data:image/png;base64,";
  const imageUrl = prefix + "A".repeat(imageBytes - prefix.length);
  assert.equal(byteLength(imageUrl), imageBytes);
  const result = toDeepSeekResponsesChatBody(
    request([
      {
        type: "message",
        role: "user",
        content: [
          { type: "input_text", text: "inspect the image" },
          { type: "input_image", image_url: imageUrl },
        ],
      },
    ]),
    "deepseek-flash",
    false
  );
  if (!result.ok) throw new Error(result.message);
  assert.equal(JSON.stringify(result.value.body).includes(imageUrl), false);
  assert.equal(result.value.elisions.length, 1);
  assert.equal(result.value.elisions[0].kind, "image");
  assert.equal(result.value.elisions[0].originalBytes, imageBytes);
  assert.equal(result.value.elisions[0].omittedBytes, imageBytes);
  assert.equal(result.value.promptAdmission.admitted, true);
});
