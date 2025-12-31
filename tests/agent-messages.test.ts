import assert from "node:assert/strict";
import { handleAgentMessagesList, handleAgentMessagesPost } from "../src/agent_messages.ts";

type AgentMessage = Readonly<{
  id: string;
  owner: string;
  repo: string;
  state_id: string;
  agent_id: string;
  channel: string | null;
  kind: string | null;
  body: string;
  metadata: Record<string, unknown> | null;
  created_at_ms: number;
}>;

const jsonRequest = (url: string, body: unknown): Request =>
  new Request(url, {
    method: "POST",
    headers: { "Content-Type": "application/json" },
    body: JSON.stringify(body),
  });

const makeAuth = (owner = "acme", repo = "demo", stateId = "state-1") =>
  async (_req: Request) => ({
    ok: true as const,
    token: "ghs_test_token",
    method: { kind: "github_token" as const, owner, repo, state_id: stateId },
  });

const compareKvKeyPart = (left: Deno.KvKeyPart, right: Deno.KvKeyPart): number => {
  if (left === right) return 0;
  if (typeof left === "number" && typeof right === "number") return left - right;
  if (typeof left === "string" && typeof right === "string") return left < right ? -1 : 1;
  const leftStr = String(left);
  const rightStr = String(right);
  if (leftStr === rightStr) return 0;
  return leftStr < rightStr ? -1 : 1;
};

const compareKvKey = (left: Deno.KvKey, right: Deno.KvKey): number => {
  const max = Math.max(left.length, right.length);
  for (let i = 0; i < max; i += 1) {
    if (i >= left.length) return -1;
    if (i >= right.length) return 1;
    const partCompare = compareKvKeyPart(left[i], right[i]);
    if (partCompare !== 0) return partCompare;
  }
  return 0;
};

const matchesPrefix = (key: Deno.KvKey, prefix: Deno.KvKey): boolean =>
  prefix.every((part, index) => key[index] === part);

class MemoryKv {
  #counter = 0;
  entries: Array<Deno.KvEntry<unknown>> = [];

  async set(key: Deno.KvKey, value: unknown, _options?: { expireIn?: number }): Promise<{ ok: true }> {
    this.#counter += 1;
    this.entries.push({ key, value, versionstamp: String(this.#counter) });
    return { ok: true };
  }

  list<T>(selector: Deno.KvListSelector, options: Deno.KvListOptions = {}): MemoryKvListIterator<T> {
    const prefix = "prefix" in selector ? selector.prefix : [];
    let results = this.entries.filter((entry) => matchesPrefix(entry.key, prefix));
    results = results.sort((a, b) => compareKvKey(a.key, b.key));

    if ("start" in selector) {
      results = results.filter((entry) => compareKvKey(entry.key, selector.start ?? []) >= 0);
    }

    const limit = options.limit ?? results.length;
    const sliced = results.slice(0, limit);
    const cursor = results.length > sliced.length ? "next" : "";
    return new MemoryKvListIterator<T>(sliced as Deno.KvEntry<T>[], cursor);
  }
}

class MemoryKvListIterator<T> implements Deno.KvListIterator<T> {
  #entries: Deno.KvEntry<T>[];
  #cursor: string;
  #index = 0;

  constructor(entries: Deno.KvEntry<T>[], cursor: string) {
    this.#entries = entries;
    this.#cursor = cursor;
  }

  get cursor(): string {
    return this.#cursor;
  }

  async next(): Promise<IteratorResult<Deno.KvEntry<T>>> {
    if (this.#index >= this.#entries.length) return { done: true, value: undefined };
    const value = this.#entries[this.#index];
    this.#index += 1;
    return { done: false, value };
  }

  [Symbol.asyncIterator](): AsyncIterableIterator<Deno.KvEntry<T>> {
    return this;
  }
}

Deno.test("agent-messages: post stores a message", async () => {
  const kv = new MemoryKv();
  const req = jsonRequest("https://ai.ubq.fi/v1/agent-messages", {
    agent_id: "agent-1",
    body: "hello",
    channel: "general",
    kind: "note",
    metadata: { ok: true },
  });

  const res = await handleAgentMessagesPost(req, {
    authenticateClient: makeAuth(),
    kv,
    now: () => 1700000000000,
    uuid: () => "msg-1",
  });

  assert.equal(res.status, 200);
  const payload = (await res.json()) as { ok: boolean; message: AgentMessage };
  assert.equal(payload.ok, true);
  assert.equal(payload.message.id, "msg-1");
  assert.equal(payload.message.owner, "acme");
  assert.equal(payload.message.repo, "demo");
  assert.equal(payload.message.state_id, "state-1");
  assert.equal(payload.message.agent_id, "agent-1");
  assert.equal(payload.message.channel, "general");
  assert.equal(payload.message.kind, "note");
  assert.equal(payload.message.body, "hello");
  assert.equal(payload.message.created_at_ms, 1700000000000);
  assert.equal(kv.entries.length, 1);
});

Deno.test("agent-messages: list returns messages and cursor info", async () => {
  const kv = new MemoryKv();
  const auth = makeAuth();

  await handleAgentMessagesPost(
    jsonRequest("https://ai.ubq.fi/v1/agent-messages", { agent_id: "agent-1", body: "one" }),
    { authenticateClient: auth, kv, now: () => 1000, uuid: () => "msg-1" },
  );
  await handleAgentMessagesPost(
    jsonRequest("https://ai.ubq.fi/v1/agent-messages", { agent_id: "agent-1", body: "two" }),
    { authenticateClient: auth, kv, now: () => 2000, uuid: () => "msg-2" },
  );

  const res = await handleAgentMessagesList(new Request("https://ai.ubq.fi/v1/agent-messages?limit=1"), {
    authenticateClient: auth,
    kv,
  });

  assert.equal(res.status, 200);
  const payload = (await res.json()) as {
    ok: boolean;
    messages: AgentMessage[];
    next_since: number | null;
    next_cursor: string | null;
    has_more: boolean;
  };
  assert.equal(payload.ok, true);
  assert.equal(payload.messages.length, 1);
  assert.equal(payload.messages[0].id, "msg-1");
  assert.equal(payload.next_since, 1000);
  assert.equal(payload.has_more, true);
  assert.equal(payload.next_cursor, "next");
});

Deno.test("agent-messages: list returns null next_since when empty", async () => {
  const kv = new MemoryKv();
  const res = await handleAgentMessagesList(new Request("https://ai.ubq.fi/v1/agent-messages"), {
    authenticateClient: makeAuth(),
    kv,
  });

  assert.equal(res.status, 200);
  const payload = (await res.json()) as { next_since: number | null; messages: AgentMessage[] };
  assert.equal(payload.messages.length, 0);
  assert.equal(payload.next_since, null);
});
