import { writeFileSync } from "node:fs";
import { resolve } from "node:path";
import "./fixture-loader.ts";
const sourceAdminPath = "./source-admin-refresh.ts.txt";
const { runClaudeUsageRefresh, setResult, blockReads, withCors } = await import(sourceAdminPath);
const at = 1800000000000;
const usage = (state: string) => ({
  source: "oauth_usage",
  state,
  status: (state === "ok" ? 200 : 429) as number | null,
  message: state === "ok" ? null : " diagnostic\ud800 ",
  attempted_at_ms: at,
  observed_at_ms: state === "ok" ? at : null,
});
const cases = [];
for (const kind of [
  "changed",
  "busy",
  "fresh",
  "success",
  "scope_missing",
  "unsupported_credential",
  "auth_invalid",
  "rate_limited",
  "upstream_error",
  "http_error",
  "unreachable",
  "invalid_response",
]) {
  const retries = kind === "rate_limited" ? [null, 0, 2200, 1e24, Infinity] : [null];
  for (const retry of retries) {
    const good = kind === "success";
    const state = good ? "ok" : kind;
    const u = usage(state);
    const statuses: Record<string, number | null> = {
      scope_missing: 403,
      unsupported_credential: null,
      auth_invalid: 401,
      rate_limited: 429,
      upstream_error: 503,
      http_error: 418,
      unreachable: null,
      invalid_response: 200,
    };
    if (!good) u.status = statuses[kind] ?? null;
    const refresh =
      kind === "fresh"
        ? { kind, usage: usage("ok") }
        : kind === "changed" || kind === "busy"
          ? { kind }
          : {
              kind: "completed",
              usage: u,
              result: good
                ? { ok: true, status: 200, observed_at_ms: at, observation: {} }
                : { ok: false, kind, status: u.status, observed_at_ms: at, message: " diagnostic\ud800 ", retry_after_ms: retry },
            };
    setResult(refresh);
    const response = withCors(
      await runClaudeUsageRefresh({} as any, {} as any, {}),
      new Request("https://ai.ubq.fi/admin/providers/claude-code/quota/refresh")
    );
    cases.push({
      kind,
      retry: retry === null ? null : String(retry),
      refresh,
      expected: {
        status: response.status,
        headers: [...response.headers].filter(([name]) => ["content-type", "cache-control", "retry-after"].includes(name)),
        body: await response.text(),
        block_reads: blockReads(),
      },
    });
  }
}
writeFileSync(
  resolve(import.meta.dir, "../fixtures/admin-oracle.json"),
  JSON.stringify({ source_sha: "b59243d674567d418d1410f8390fc7fe47a08c92", cases }, null, 2) + "\n"
);
console.log(JSON.stringify({ cases: cases.length }));
