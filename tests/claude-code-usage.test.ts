import { test } from "bun:test";
import assert from "node:assert/strict";
import { CLAUDE_CODE_USAGE_URL, CLAUDE_CODE_USAGE_USER_AGENT, fetchClaudeCodeUsage } from "../src/provider/claude-code-usage.ts";

const NOW_MS = Date.UTC(2026, 9, 25, 19, 0, 0);
const CREDENTIAL = { kind: "oauth" as const, secret: "sk-ant-oat-fixture-usage" };
const requestUrl = (input: RequestInfo | URL): string => {
  if (typeof input === "string") return input;
  if (input instanceof URL) return input.href;
  return input.url;
};

test("direct Claude usage parses percentages, resets, and the required OAuth headers", async () => {
  const resetFiveHour = "2026-10-25T23:00:00.123456+00:00";
  const resetSevenDay = "2026-11-01T18:00:00.123456+00:00";
  let seenUrl: string | null = null;
  let seenInit: RequestInit | undefined;
  const fetcher = (input: RequestInfo | URL, init?: RequestInit) => {
    seenUrl = requestUrl(input);
    seenInit = init;
    return Promise.resolve(
      Response.json({
        five_hour: { utilization: 42, resets_at: resetFiveHour },
        seven_day: { utilization: 12.5, resets_at: resetSevenDay },
        seven_day_opus: null,
      })
    );
  };

  const result = await fetchClaudeCodeUsage(CREDENTIAL, { fetcher, now: () => NOW_MS });

  assert(result.ok);
  assert(seenUrl);
  assert(seenInit);
  assert.equal(seenUrl, CLAUDE_CODE_USAGE_URL);
  assert.equal(seenInit.method, "GET");
  assert.equal(seenInit.body, undefined, "usage reads never send an inference body");
  const headers = new Headers(seenInit.headers);
  assert.equal(headers.get("authorization"), `Bearer ${CREDENTIAL.secret}`);
  assert.equal(headers.get("anthropic-beta"), "oauth-2025-04-20");
  assert.equal(headers.get("anthropic-version"), "2023-06-01");
  assert.equal(headers.get("user-agent"), CLAUDE_CODE_USAGE_USER_AGENT);
  assert.equal(headers.get("accept"), "application/json");
  assert.equal(headers.get("content-type"), "application/json");
  assert.equal(result.observation.observed_at_ms, NOW_MS);
  assert.deepEqual(result.observation.windows["5h"], {
    status: null,
    utilization: 0.42,
    reset_at_ms: Date.parse(resetFiveHour),
    observed_at_ms: NOW_MS,
  });
  const sevenDay = result.observation.windows["7d"];
  assert(sevenDay);
  assert.equal(sevenDay.utilization, 0.125);
  assert.equal(sevenDay.reset_at_ms, Date.parse(resetSevenDay));
  assert.deepEqual(result.observation.standard, {});
  assert.equal(result.observation.representative_claim, null);
});

test("a setup token without the profile scope is an explicit non-fatal usage result", async () => {
  const result = await fetchClaudeCodeUsage(CREDENTIAL, {
    fetcher: () =>
      Promise.resolve(
        Response.json(
          { type: "error", error: { type: "permission_error", message: "OAuth token does not meet scope requirement user:profile" } },
          { status: 403 }
        )
      ),
    now: () => NOW_MS,
  });

  assert(!result.ok);
  assert.equal(result.kind, "scope_missing");
  assert.equal(result.status, 403);
  assert.equal(result.observed_at_ms, NOW_MS);
  assert.match(result.message ?? "", /user:profile/);
});

test("malformed Claude usage payloads fail closed as invalid responses", async () => {
  const payloads: readonly (readonly [string, () => Response])[] = [
    ["invalid JSON", () => new Response("not-json", { status: 200, headers: { "content-type": "application/json" } })],
    ["unrecognized shape", () => Response.json({ unknown_window: { utilization: 10, resets_at: null } })],
    ["invalid reset type", () => Response.json({ five_hour: { utilization: 10, resets_at: 123 }, seven_day: { utilization: 2, resets_at: null } })],
    ["missing utilization", () => Response.json({ five_hour: { resets_at: null }, seven_day: { utilization: 2, resets_at: null } })],
  ];

  for (const [label, response] of payloads) {
    const result = await fetchClaudeCodeUsage(CREDENTIAL, { fetcher: () => Promise.resolve(response()), now: () => NOW_MS });
    assert(!result.ok, label);
    assert.equal(result.kind, "invalid_response", label);
    assert.equal(result.status, 200, label);
  }
});

test("a Console API key does not call the OAuth usage endpoint", async () => {
  let calls = 0;
  const result = await fetchClaudeCodeUsage(
    { kind: "api_key", secret: "sk-ant-usr-fixture" },
    {
      fetcher: () => {
        calls += 1;
        return Promise.resolve(Response.json({}));
      },
      now: () => NOW_MS,
    }
  );
  assert.equal(calls, 0);
  assert(!result.ok);
  assert.equal(result.kind, "unsupported_credential");
  assert.equal(result.status, null);
});
