import { test } from "bun:test";
import assert from "node:assert/strict";
import { mkdirSync, mkdtempSync, rmSync } from "node:fs";
import { resolve } from "node:path";
import { openKv, type Kv } from "#kv";
import { setKvForTest } from "../src/kv.ts";
import { attemptClaudeCodeSubscription } from "../src/paid-fallback/claude-code-route.ts";
import { handleAdminClaudeCodeQuotaRefresh } from "../src/admin/claude-code.ts";
import { claudeCapacitySource, handleProviderCapacity } from "../src/provider/capacity.ts";
import { readClaudeCapacityHistory } from "../src/provider/claude-capacity-history.ts";
import { resolveClaudeCodeAccount, updateClaudeCodeAccount } from "../src/provider/claude-code-accounts.ts";
import { resetClaudeCodeCredentialCacheForTest, storeClaudeCodeStoredCredential } from "../src/provider/claude-code-credentials.ts";
import { loadClaudeCodeQuotaState, observeClaudeCodeQuota, recordClaudeCodeQuotaOutcome } from "../src/provider/claude-code-quota.ts";
import { parseClaudeCodeUsagePayload } from "../src/provider/claude-code-usage.ts";
import { resetProviderHealthThrottleForTest } from "../src/provider/health.ts";
import { PROVIDER_SELECTION_KV_KEY, resetProviderSelectionCacheForTest } from "../src/provider/selection.ts";

const model = "claude-opus-5-5";
const env = (name: string) => (name === "CLAUDE_CODE_ALLOWED_KEY_IDS" ? "header-owner" : null);
const message = {
  id: "msg_headers",
  type: "message",
  role: "assistant",
  model,
  content: [{ type: "text", text: "OK" }],
  stop_reason: "end_turn",
  stop_sequence: null,
  usage: { input_tokens: 1, output_tokens: 1 },
};
const sse = () =>
  [
    { type: "message_start", message: { ...message, content: [], stop_reason: null } },
    { type: "content_block_start", index: 0, content_block: { type: "text", text: "" } },
    { type: "content_block_delta", index: 0, delta: { type: "text_delta", text: "OK" } },
    { type: "content_block_stop", index: 0 },
    { type: "message_delta", delta: { stop_reason: "end_turn", stop_sequence: null }, usage: { output_tokens: 1 } },
    { type: "message_stop" },
  ]
    .map((event) => `event: ${event.type}\ndata: ${JSON.stringify(event)}\n\n`)
    .join("");

const fixture = async () => {
  const root = resolve(".cleanup-evidence/claude-header-meter");
  mkdirSync(root, { recursive: true });
  const directory = mkdtempSync(`${root}/case-`);
  const path = `${directory}/kv.sqlite3`;
  let kv: Kv = await openKv(path);
  setKvForTest(kv);
  resetProviderSelectionCacheForTest();
  resetProviderHealthThrottleForTest();
  resetClaudeCodeCredentialCacheForTest();
  await storeClaudeCodeStoredCredential(kv, { secret: "sk-ant-oat-fixture-normal-header-traffic" });
  await kv.set(PROVIDER_SELECTION_KV_KEY, { provider_ids: ["claude_code"], updated_at_ms: Date.now() });
  const account = await resolveClaudeCodeAccount(kv, env);
  assert(account);
  const reply = { status: 200, headers: new Headers() };
  let requests = 0;
  const server = Bun.serve({
    hostname: "127.0.0.1",
    port: 0,
    async fetch(request) {
      requests += 1;
      const body: unknown = await request.json();
      if (reply.status >= 400)
        return Response.json(
          { type: "error", error: { type: "rate_limit_error", message: "fixture rejection" } },
          { status: reply.status, headers: reply.headers }
        );
      const stream = typeof body === "object" && body !== null && "stream" in body && body.stream === true;
      return stream
        ? new Response(sse(), { headers: { ...Object.fromEntries(reply.headers), "Content-Type": "text/event-stream" } })
        : Response.json(message, { headers: reply.headers });
    },
  });
  return {
    get kv() {
      return kv;
    },
    account,
    reply,
    requests: () => requests,
    async infer(stream: boolean) {
      const routed = await attemptClaudeCodeSubscription(
        { model, input: "ordinary fixture request", stream },
        {
          model,
          env,
          usageContext: { keyId: "header-owner", kernelRepo: null, kernelOrg: null },
          fetcher: (input, init) => {
            assert.equal(input instanceof Request ? input.url : input.toString(), "https://api.anthropic.com/v1/messages");
            return fetch(`http://127.0.0.1:${server.port}/messages`, init);
          },
        }
      );
      assert(routed);
      await routed.response.text();
      return routed.response.status;
    },
    async reopen() {
      kv.close();
      kv = await openKv(path);
      setKvForTest(kv);
    },
    async close() {
      await server.stop(true);
      setKvForTest(null);
      kv.close();
      rmSync(directory, { recursive: true, force: true });
    },
  };
};

test("normal streaming, nonstream and error HTTP headers update the meter without borrowing an old direct snapshot", async () => {
  const current = await fixture();
  try {
    const oldAt = Date.now() - 60_000;
    const direct = parseClaudeCodeUsagePayload({ five_hour: { utilization: 99, resets_at: null } }, oldAt);
    assert(direct);
    await recordClaudeCodeQuotaOutcome(current.kv, {
      binding: current.account,
      observation: direct,
      nowMs: oldAt,
      usage: { source: "oauth_usage", state: "scope_missing", status: 403, message: "historical", attempted_at_ms: oldAt, observed_at_ms: null },
    });
    assert.equal((await claudeCapacitySource(current.kv, Date.now(), env)).windows.five_hour, null);
    current.reply.headers = new Headers({ "anthropic-ratelimit-unified-5h-utilization": "0.4", "anthropic-ratelimit-unified-7d-utilization": "0.2" });
    assert.equal(await current.infer(true), 200);
    const streaming = await claudeCapacitySource(current.kv, Date.now(), env);
    assert.equal(streaming.windows.five_hour?.used_percent, 40);
    assert.equal(streaming.windows.seven_day?.used_percent, 20);
    assert.equal(streaming.state, "available");
    assert.equal(streaming.quota_source, "response_headers");
    assert.equal(streaming.usage, null);
    assert.equal(streaming.usage_snapshot, undefined);
    current.reply.headers = new Headers({ "anthropic-ratelimit-unified-5h-utilization": "0.6" });
    assert.equal(await current.infer(false), 200);
    const partial = await claudeCapacitySource(current.kv, Date.now(), env);
    assert.equal(partial.windows.five_hour?.used_percent, 60);
    assert.equal(partial.windows.seven_day?.used_percent, 20);
    current.reply.headers = new Headers();
    assert.equal(await current.infer(false), 200);
    assert.equal((await claudeCapacitySource(current.kv, Date.now(), env)).source_observed_at_ms, partial.source_observed_at_ms);
    current.reply.status = 429;
    current.reply.headers = new Headers({ "anthropic-ratelimit-unified-7d-utilization": "0.25", "Retry-After": "60" });
    assert.equal(await current.infer(false), 429);
    const rejected = await claudeCapacitySource(current.kv, Date.now(), env);
    assert.equal(rejected.windows.seven_day?.used_percent, 25);
    assert.equal(rejected.state, "unavailable");
    const quota = await loadClaudeCodeQuotaState(current.kv, current.account);
    assert(quota?.cooldown_until_ms);
    assert.equal(quota.blocked_until_ms, null);
    assert.deepEqual(quota.usage_snapshot, direct.usage_snapshot);
    const points = (await readClaudeCapacityHistory(current.kv, Date.now())).points;
    assert(points.some((point) => point.source === "response_headers"));
    assert(points.filter((point) => point.source === "response_headers").every((point) => point.usage_snapshot === undefined));
    assert.equal(current.requests(), 4);
  } finally {
    await current.close();
  }
});

test("native reopen retains only proven header meter evidence and a replacement cohort starts awaiting headers", async () => {
  const current = await fixture();
  try {
    current.reply.headers = new Headers({ "anthropic-ratelimit-unified-5h-utilization": "0.3" });
    assert.equal(await current.infer(false), 200);
    const before = await claudeCapacitySource(current.kv, Date.now(), env);
    await current.reopen();
    const after = await claudeCapacitySource(current.kv, Date.now(), env);
    assert.deepEqual(after.windows, before.windows);
    assert.equal(after.source_observed_at_ms, before.source_observed_at_ms);
    await updateClaudeCodeAccount(current.kv, {
      accountId: current.account.account_id,
      credential: { ...current.account.credential, secret: "sk-ant-oat-fixture-replacement" },
    });
    const replaced = await claudeCapacitySource(current.kv, Date.now(), env);
    assert.equal(replaced.source_observed_at_ms, null);
    assert.equal(replaced.windows.five_hour, null);
  } finally {
    await current.close();
  }
});

test("automatic and manual capacity reads make no Claude requests after ordinary inference", async () => {
  const current = await fixture();
  const originalFetch = globalThis.fetch;
  try {
    const oldAt = Date.now() - 60_000;
    const direct = parseClaudeCodeUsagePayload({ five_hour: { utilization: 98, resets_at: null } }, oldAt);
    assert(direct);
    await recordClaudeCodeQuotaOutcome(current.kv, { binding: current.account, observation: direct, nowMs: oldAt });
    current.reply.headers = new Headers({ "anthropic-ratelimit-unified-5h-utilization": "0.15" });
    assert.equal(await current.infer(false), 200);
    let claudeCalls = 0;
    const noClaudeFetch = (input: RequestInfo | URL) => {
      const url = input instanceof Request ? input.url : input.toString();
      if (url.startsWith("https://api.anthropic.com/") || url.startsWith("https://platform.claude.com/")) claudeCalls += 1;
      return Promise.resolve(Response.json({}, { status: 503 }));
    };
    globalThis.fetch = Object.assign(
      (input: RequestInfo | URL, init?: RequestInit) => {
        const url = input instanceof Request ? input.url : input.toString();
        if (url.startsWith("https://api.anthropic.com/") || url.startsWith("https://platform.claude.com/")) {
          claudeCalls += 1;
          return Promise.resolve(Response.json({}, { status: 503 }));
        }
        return originalFetch(input, init);
      },
      { preconnect: originalFetch.preconnect }
    );
    assert.equal(globalThis.fetch.preconnect, originalFetch.preconnect);
    const before = await loadClaudeCodeQuotaState(current.kv, current.account);
    assert(before?.response_headers);
    for (const query of ["", "", "?refresh=live"]) {
      const response = await handleProviderCapacity(new Request(`http://localhost/admin/providers/capacity${query}`), {
        kv: current.kv,
        fetcher: noClaudeFetch,
      });
      assert.equal(response.status, 200);
      const view = await response.json();
      assert.equal(view.claude.windows.five_hour.used_percent, 15);
      assert.equal(view.claude.usage_snapshot, undefined);
      assert.equal(view.claude.usage, null);
    }
    const response = await handleAdminClaudeCodeQuotaRefresh(
      new Request("http://localhost/admin/providers/claude-code/quota/refresh", {
        method: "POST",
        body: JSON.stringify({ account_id: current.account.account_id, account_cohort_id: current.account.account_cohort_id }),
      }),
      { env, fetcher: noClaudeFetch }
    );
    assert.equal(response.status, 200);
    const body = await response.json();
    assert.equal(body.refreshed, false);
    assert.equal(body.reason, "response_headers");
    assert.equal(body.observed_at_ms, before.response_headers.observed_at_ms);
    assert.equal(claudeCalls, 0);
    assert.equal(current.requests(), 1);
    assert.deepEqual(await loadClaudeCodeQuotaState(current.kv, current.account), before);
  } finally {
    globalThis.fetch = originalFetch;
    await current.close();
  }
});

test("header family timestamps merge independently and unmarked canonical values are never relabeled", async () => {
  const current = await fixture();
  try {
    const at = Date.now();
    const observation = observeClaudeCodeQuota(new Headers({ "anthropic-ratelimit-unified-5h-utilization": "0.8" }), at);
    assert(observation);
    await recordClaudeCodeQuotaOutcome(current.kv, { binding: current.account, observation, nowMs: at });
    assert.equal((await claudeCapacitySource(current.kv, at, env)).windows.five_hour, null);
    await recordClaudeCodeQuotaOutcome(current.kv, { binding: current.account, observation, observation_source: "response_headers", nowMs: at });
    const older = observeClaudeCodeQuota(
      new Headers({ "anthropic-ratelimit-unified-5h-utilization": "0.1", "anthropic-ratelimit-requests-remaining": "49" }),
      at - 10
    );
    assert(older);
    await recordClaudeCodeQuotaOutcome(current.kv, { binding: current.account, observation: older, observation_source: "response_headers", nowMs: at });
    const read = await claudeCapacitySource(current.kv, at, env);
    assert.equal(read.windows.five_hour?.used_percent, 80);
    assert.equal(read.standard[0]?.remaining, 49);
    assert.equal(read.windows.seven_day, null);
  } finally {
    await current.close();
  }
});
