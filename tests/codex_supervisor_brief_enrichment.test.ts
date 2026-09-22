import assert from "node:assert/strict";

import type { SupervisorSource } from "../src/codex_supervisor.ts";
import { buildSupervisorBriefPrompt, collectBriefTranscript } from "../src/codex_supervisor_brief.ts";
import type { SupervisorConnection } from "../src/codex_supervisor_transport.ts";

const SOURCE: SupervisorSource = {
  id: "local",
  name: "m1.local",
  socketPath: "/var/lib/uos-supervisor/local.sock",
  codexHome: "/Users/example/.codex",
};

const turnPage = (turns: unknown[]) => ({ data: turns, nextCursor: null });

const threadRead = {
  thread: {
    id: "thread-1",
    name: "Older empty summary",
    status: { type: "active", activeFlags: [] },
    updatedAt: 1_790_000_500,
  },
};

const turn = (id: string, text: string) => ({
  id,
  status: "completed",
  items: [{ type: "agentMessage", id: `${id}-item`, text }],
});

const emptyTurn = (id: string) => ({ id, status: "completed", items: [] });

type FullPageSource = (direction: "asc" | "desc", limit: number) => unknown[];

/** Serves direction- and limit-aware summary and full pages, recording full reads. */
const collectWith = async (
  summaries: { first: unknown[]; recent: unknown[] },
  full: FullPageSource
): Promise<{ prompt: string; fullReads: { direction: string; limit: number }[] }> => {
  const fullReads: { direction: string; limit: number }[] = [];
  const connection: SupervisorConnection = {
    call: (method, params) => {
      if (method === "thread/read") return Promise.resolve(threadRead);
      if (method === "thread/turns/list") {
        if (params?.itemsView === "full") {
          const direction = params.sortDirection === "asc" ? "asc" : "desc";
          const requestedLimit = params.limit;
          const limit = typeof requestedLimit === "number" ? requestedLimit : 0;
          fullReads.push({ direction, limit });
          return Promise.resolve(turnPage(full(direction, limit)));
        }
        return Promise.resolve(turnPage(params?.sortDirection === "asc" ? summaries.first : summaries.recent));
      }
      return Promise.resolve({});
    },
    close: () => Promise.resolve(),
  };
  const context = await collectBriefTranscript(connection, SOURCE, "thread-1", new AbortController().signal);
  return { prompt: buildSupervisorBriefPrompt(context), fullReads };
};

Deno.test("an older empty summary turn is enriched from its own full page", async () => {
  const summaries = {
    first: [],
    recent: [
      turn("turn-new", "newest summary"),
      turn("turn-2", "second summary"),
      turn("turn-3", "third summary"),
      turn("turn-4", "fourth summary"),
      emptyTurn("turn-old"),
    ],
  };
  const fullRecent = [
    turn("turn-new", "newest full text"),
    turn("turn-2", "second full text"),
    turn("turn-3", "third full text"),
    turn("turn-4", "fourth full text"),
    turn("turn-old", "recovered older progress"),
  ];
  const { prompt, fullReads } = await collectWith(summaries, (direction, limit) => (direction === "asc" ? [] : fullRecent.slice(0, limit)));
  assert.match(prompt, /recovered older progress/, "the empty turn must be enriched from its own full page");
  assert.ok(fullReads.some((read) => read.direction === "desc"), "the newest page carries the older empty turn");
  assert.ok(fullReads.length <= 2, `enrichment reads stay bounded: ${fullReads.length}`);
});

Deno.test("the oldest empty summary turn is enriched from the first full page", async () => {
  const summaries = {
    first: [emptyTurn("turn-oldest")],
    recent: [
      turn("turn-new", "newest summary"),
      turn("turn-2", "second summary"),
      turn("turn-3", "third summary"),
      turn("turn-4", "fourth summary"),
      turn("turn-5", "fifth summary"),
      turn("turn-6", "sixth summary"),
    ],
  };
  const fullRecent = [
    turn("turn-new", "newest full text"),
    turn("turn-2", "second full text"),
    turn("turn-3", "third full text"),
    turn("turn-4", "fourth full text"),
    turn("turn-5", "fifth full text"),
    turn("turn-6", "sixth full text"),
  ];
  const { prompt, fullReads } = await collectWith(summaries, (direction, limit) =>
    direction === "asc" ? [turn("turn-oldest", "oldest recovered request")].slice(0, limit) : fullRecent.slice(0, limit)
  );
  assert.match(prompt, /oldest recovered request/, "the oldest empty turn must be enriched from the first full page");
  assert.ok(fullReads.some((read) => read.direction === "asc" && read.limit === 1), "the oldest turn reads the first full page");
  assert.ok(fullReads.length <= 2, `enrichment reads stay bounded: ${fullReads.length}`);
});

Deno.test("a turn whose full page has no items does not consume an enrichment slot", async () => {
  const summaries = { first: [], recent: [emptyTurn("turn-empty"), emptyTurn("turn-recoverable")] };
  const fullRecent = [emptyTurn("turn-empty"), turn("turn-recoverable", "second empty turn recovered")];
  const { prompt, fullReads } = await collectWith(summaries, (direction, limit) => (direction === "asc" ? [] : fullRecent.slice(0, limit)));
  assert.match(prompt, /second empty turn recovered/, "a later empty turn is still enriched after an unmatched one");
  assert.ok(fullReads.length <= 2, `enrichment reads stay bounded: ${fullReads.length}`);
});
