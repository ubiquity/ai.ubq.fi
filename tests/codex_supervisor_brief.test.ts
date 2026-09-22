import assert from "node:assert/strict";

import handler from "../src/handler.ts";
import type { SupervisorSource } from "../src/codex_supervisor.ts";
import {
  buildSupervisorBriefPrompt,
  buildSupervisorBriefRequestBody,
  collectBriefTranscript,
  parseSupervisorBriefOutput,
  redactBriefText,
  type SupervisorBriefContext,
} from "../src/codex_supervisor_brief.ts";
import type { SupervisorConnection } from "../src/codex_supervisor_transport.ts";

const SOURCE: SupervisorSource = {
  id: "local",
  name: "m1.local",
  socketPath: "/var/lib/uos-supervisor/local.sock",
  codexHome: "/Users/example/.codex",
};

const turnPage = (turns: unknown[]) => ({ data: turns, nextCursor: null });

const connectionFor = (pages: { read: unknown; first: unknown[]; recent: unknown[]; full?: unknown[] }): SupervisorConnection => ({
  call: (method, params) => {
    if (method === "thread/read") return Promise.resolve(pages.read);
    if (method === "thread/turns/list") {
      if (params?.itemsView === "full") return Promise.resolve(turnPage(pages.full ?? []));
      return Promise.resolve(turnPage(params?.sortDirection === "asc" ? pages.first : pages.recent));
    }
    return Promise.resolve({});
  },
  close: () => Promise.resolve(),
});

const threadRead = {
  thread: {
    id: "thread-1",
    name: "Fix the supervisor panel",
    cwd: "/repo",
    model: "gpt-6-astra",
    modelProvider: "uos",
    reasoningEffort: "max",
    status: { type: "active", activeFlags: [] },
    updatedAt: 1_790_000_500,
  },
};

const collectWith = async (pages: { read: unknown; first: unknown[]; recent: unknown[]; full?: unknown[] }): Promise<SupervisorBriefContext> =>
  await collectBriefTranscript(connectionFor(pages), SOURCE, "thread-1", new AbortController().signal);

Deno.test("redactBriefText removes credential-shaped text", () => {
  const result = redactBriefText(`Authorization: Bearer ${"a".repeat(40)}\nDEEPSEEK_API_KEY=${"b".repeat(32)}\nplain progress`);
  assert.ok(result.redactions >= 2);
  assert.equal(result.text.includes("a".repeat(40)), false);
  assert.equal(result.text.includes("b".repeat(32)), false);
  assert.match(result.text, /plain progress/);
  assert.match(result.text, /\[redacted\]/);
});

const heavyTurn = (index: number) => ({
  id: `turn-${index}`,
  status: "inProgress",
  items: Array.from({ length: 12 }, (_value, item) => ({ type: "agentMessage", id: `a${index}-${item}`, text: "x".repeat(1_500) })),
});

Deno.test("brief transcript keeps visible progress, bounds the context, and omits private items", async () => {
  const secretLine = `Authorization: Bearer ${"a".repeat(40)}`;
  const context = await collectWith({
    read: threadRead,
    first: [],
    recent: [
      {
        id: "turn-1",
        status: "completed",
        startedAt: 1_790_000_000,
        completedAt: 1_790_000_100,
        items: [
          { type: "userMessage", id: "u1", content: [{ type: "text", text: secretLine }] },
          { type: "reasoning", id: "r1", summary: ["private chain of thought"] },
        ],
      },
      {
        id: "turn-2",
        status: "inProgress",
        startedAt: 1_790_000_200,
        items: [{ type: "commandExecution", id: "c1", command: "deno task test", status: "completed", exitCode: 0, aggregatedOutput: "ok" }],
      },
      heavyTurn(3),
      heavyTurn(4),
      heavyTurn(5),
      heavyTurn(6),
      heavyTurn(7),
      heavyTurn(8),
    ],
  });
  assert.equal(context.transcriptAvailable, true);
  assert.equal(context.title, "Fix the supervisor panel");
  assert.equal(context.state, "active");
  assert.ok(context.redactions >= 1);
  assert.ok(context.contextBytes <= 32 * 1024);
  assert.equal(context.truncated, true);
  const prompt = buildSupervisorBriefPrompt(context);
  assert.equal(prompt.includes("private chain of thought"), false);
  assert.equal(prompt.includes("a".repeat(40)), false);
  assert.equal(prompt.includes("[redacted]"), true);
  assert.match(prompt, /deno task test/);
  assert.ok(new TextEncoder().encode(prompt).length <= 48 * 1024);
});

Deno.test("brief transcript reports a missing transcript instead of inventing progress", async () => {
  const context = await collectWith({ read: threadRead, first: [], recent: [] });
  assert.equal(context.transcriptAvailable, false);
  assert.equal(context.turns.length, 0);
  assert.equal(context.contextBytes, 0);
});

Deno.test("the summarizer request is a no-tools JSON-mode deepseek call on untrusted data", async () => {
  const context = await collectWith({
    read: threadRead,
    first: [{ id: "turn-0", status: "completed", items: [{ type: "userMessage", id: "u0", content: [{ type: "text", text: "Please fix the panel" }] }] }],
    recent: [{ id: "turn-1", status: "inProgress", items: [{ type: "agentMessage", id: "a1", text: "Working on the refresh loop" }] }],
  });
  const body = buildSupervisorBriefRequestBody(context);
  assert.equal("tools" in body, false);
  assert.equal("tool_choice" in body, false);
  assert.equal(body.stream, false);
  assert.deepEqual(body.response_format, { type: "json_object" });
  assert.equal(body.reasoning_effort, "max");
  assert.equal(typeof body.max_completion_tokens, "number");
  assert.ok(Array.isArray(body.messages));
  const messages = body.messages as { role: string; content: string }[];
  assert.equal(messages.length, 2);
  assert.equal(messages[0].role, "system");
  assert.match(messages[0].content, /untrusted data/);
  assert.equal(messages[1].role, "user");
  assert.match(messages[1].content, /<session_log>/);
  assert.match(messages[1].content, /Please fix the panel/);
});

Deno.test("brief output parsing accepts only a grounded {about,status} object", () => {
  assert.deepEqual(parseSupervisorBriefOutput('{"about":"Panel refresh work","status":"Tests are running"}'), {
    about: "Panel refresh work",
    status: "Tests are running",
  });
  assert.deepEqual(parseSupervisorBriefOutput('```json\n{"about":"a","status":"b"}\n```'), { about: "a", status: "b" });
  assert.equal(parseSupervisorBriefOutput('{"about":"only one field"}'), null);
  assert.equal(parseSupervisorBriefOutput("not json at all"), null);
  assert.equal(parseSupervisorBriefOutput(null), null);
});

Deno.test("the brief route stays behind super-admin auth in the real router", async () => {
  const response = await handler(
    new Request("https://ai.ubq.fi/admin/codex/supervisor/brief", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ source: "local", id: "thread-1" }),
    })
  );
  assert.equal(response.status, 401, "an unauthenticated brief request reaches the super-admin gate instead of reading a transcript");
});
