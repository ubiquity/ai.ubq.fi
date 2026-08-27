import assert from "node:assert/strict";

import {
  appendChatMessageStats,
  createChatMessageElement,
  formatCacheHitPercent,
  formatChatDuration,
  formatChatStatsLine,
  formatChatTokens,
  parseChatCompletionStreamEvent,
  parseChatSseEvent,
  readChatCompletionUsage,
  setChatMessageContent,
  splitChatSseEvents,
} from "../static/chat-stats.js";

class FakeElement {
  readonly dataset: Record<string, string> = {};
  readonly children: FakeElement[] = [];
  readonly ownerDocument: FakeDocument;
  parentElement: FakeElement | null = null;
  textContent = "";
  title = "";

  constructor(ownerDocument: FakeDocument) {
    this.ownerDocument = ownerDocument;
  }

  appendChild(child: FakeElement): FakeElement {
    child.parentElement = this;
    this.children.push(child);
    return child;
  }

  querySelector(selector: string): FakeElement | null {
    const dataName = selector.match(/^\[data-([a-z-]+)\]$/)?.[1];
    if (!dataName) return null;
    const datasetKey = dataName.replace(/-([a-z])/g, (_match, letter: string) => letter.toUpperCase());
    return this.children.find((child) => datasetKey in child.dataset) ?? null;
  }

  remove(): void {
    if (!this.parentElement) return;
    const index = this.parentElement.children.indexOf(this);
    if (index >= 0) this.parentElement.children.splice(index, 1);
    this.parentElement = null;
  }
}

class FakeDocument {
  createElement(_tag: string): FakeElement {
    return new FakeElement(this);
  }
}

Deno.test("chat stats format the DeepSeek Harness reference line", () => {
  const line = formatChatStatsLine({
    turns: 1,
    steps: 2,
    llmMs: 3_300,
    toolMs: 100,
    ttftMs: 1_100,
    decodeMs: 170 / 158 * 1_000,
    decodeTokens: 170,
    usage: {
      prompt_tokens: 15_800,
      completion_tokens: 170,
      total_tokens: 15_970,
      prompt_tokens_details: { cached_tokens: 7_742 },
    },
  });

  assert.equal(
    line,
    "1 turns · 2 steps | LLM 3.3s · Tool call 0.1s | TTFT avg 1.1s · 158 tok/s | " +
      "Cache hit 49% | Input 15.8K tok · Output 170 tok",
  );
});

Deno.test("chat stats omit metrics that the browser cannot measure", () => {
  const line = formatChatStatsLine({ turns: 1, steps: 1, llmMs: 900 });
  assert.equal(line, "1 turns · 1 steps | LLM 0.9s");
  assert.doesNotMatch(line, /Tool call|TTFT|tok\/s|Cache hit|Input|Output|NaN|Infinity/);
});

Deno.test("chat stats keep cache-write details out of the OpenAI prompt total", () => {
  assert.deepEqual(
    readChatCompletionUsage({
      prompt_tokens: 100,
      completion_tokens: 25,
      total_tokens: 125,
      prompt_tokens_details: { cached_tokens: 40, cache_write_tokens: 20 },
    }),
    {
      inputTokens: 100,
      outputTokens: 25,
      cachedInputTokens: 40,
      cacheWriteInputTokens: 20,
    },
  );
  assert.match(
    formatChatStatsLine({
      usage: {
        prompt_tokens: 100,
        completion_tokens: 25,
        total_tokens: 125,
        prompt_tokens_details: { cached_tokens: 40, cache_write_tokens: 20 },
      },
    }),
    /Cache hit 40% \| Input 100 tok · Output 25 tok$/,
  );
});

Deno.test("chat stats omit throughput without an interval-aligned token count", () => {
  const usage = {
    prompt_tokens: 100,
    completion_tokens: 90,
    total_tokens: 190,
    completion_tokens_details: { reasoning_tokens: 80 },
  };

  assert.doesNotMatch(formatChatStatsLine({ decodeMs: 1_000, usage }), /tok\/s/);
  assert.match(formatChatStatsLine({ decodeMs: 1_000, decodeTokens: 10, usage }), /10 tok\/s/);
});

Deno.test("chat stats keep partial cache ratios below 100 percent", () => {
  assert.equal(formatCacheHitPercent(1_000, 1_000), "100");
  assert.equal(formatCacheHitPercent(999, 1_000), "99.9");
  assert.equal(formatCacheHitPercent(null, 1_000), null);
});

Deno.test("chat stats use compact token and duration formatting", () => {
  assert.equal(formatChatTokens(517), "517");
  assert.equal(formatChatTokens(12_200), "12.2K");
  assert.equal(formatChatTokens(517_000), "517K");
  assert.equal(formatChatTokens(1_200_000), "1.2M");
  assert.equal(formatChatDuration(45_200), "45.2s");
  assert.equal(formatChatDuration(162_000), "2m42s");
});

Deno.test("chat message content updates preserve an appended stats footer", () => {
  const documentRef = new FakeDocument();
  const message = createChatMessageElement(documentRef, "assistant", "First token");
  const footer = appendChatMessageStats(message, { turns: 1, steps: 1, llmMs: 1_200 });
  assert.ok(footer);

  setChatMessageContent(message, "Complete streamed response");

  assert.equal(message.querySelector("[data-message-content]")?.textContent, "Complete streamed response");
  assert.equal(message.querySelector("[data-message-stats]")?.textContent, "1 turns · 1 steps | LLM 1.2s");
  assert.equal(message.children.length, 2);
});

Deno.test("chat stream events preserve refusals, usage, completion, and failures", () => {
  assert.deepEqual(
    parseChatCompletionStreamEvent('{"choices":[{"delta":{"refusal":"I cannot help with that."}}]}'),
    { kind: "event", delta: "I cannot help with that.", usage: undefined },
  );
  assert.deepEqual(
    parseChatCompletionStreamEvent(
      '{"choices":[],"usage":{"prompt_tokens":10,"completion_tokens":2,"total_tokens":12}}',
    ),
    {
      kind: "event",
      delta: "",
      usage: { prompt_tokens: 10, completion_tokens: 2, total_tokens: 12 },
    },
  );
  assert.deepEqual(parseChatCompletionStreamEvent("[DONE]"), { kind: "done" });
  assert.deepEqual(
    parseChatCompletionStreamEvent(
      '{"error":{"message":"The upstream stream ended unexpectedly.","code":"upstream_stream_error"}}',
    ),
    { kind: "error", message: "The upstream stream ended unexpectedly." },
  );
  assert.deepEqual(parseChatCompletionStreamEvent("not JSON"), { kind: "invalid" });
  assert.deepEqual(parseChatCompletionStreamEvent("{}"), { kind: "invalid" });
  assert.deepEqual(parseChatCompletionStreamEvent('{"choices":"bad"}'), { kind: "invalid" });
  assert.deepEqual(parseChatCompletionStreamEvent('{"choices":[{"delta":"bad"}]}'), { kind: "invalid" });
  assert.deepEqual(parseChatCompletionStreamEvent('{"choices":[],"usage":{}}'), { kind: "invalid" });
});

Deno.test("chat SSE framing accepts all line endings and a final unterminated event", () => {
  assert.deepEqual(
    splitChatSseEvents("data: first\r\n\r\ndata: second\n\ndata: final", true),
    {
      events: ["data: first", "data: second", "data: final"],
      remaining: "",
    },
  );

  assert.deepEqual(splitChatSseEvents("data: partial\r\n"), {
    events: [],
    remaining: "data: partial\r\n",
  });

  assert.deepEqual(splitChatSseEvents("data: lf-crlf\n\r\ndata: crlf-lf\r\n\n"), {
    events: ["data: lf-crlf", "data: crlf-lf"],
    remaining: "",
  });
});

Deno.test("chat SSE parsing accepts CR-only comments and multiline data", () => {
  assert.deepEqual(
    parseChatSseEvent(
      ': keep-alive\rdata: {"choices":[\rdata: {"delta":{"content":"CR stream"}}\rdata: ]}',
    ),
    { kind: "event", delta: "CR stream", usage: undefined },
  );
});
