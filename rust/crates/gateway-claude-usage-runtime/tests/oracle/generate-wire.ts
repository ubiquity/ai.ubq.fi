import { writeFileSync } from "node:fs";
import { resolve } from "node:path";
import "./fixture-loader.ts";

const claude_code_usage_path = "./src/provider/claude-code-usage.ts.txt";
const { fetchClaudeCodeUsage } = await import(claude_code_usage_path);
const timeouts: number[] = [];
const original = AbortSignal.timeout;
AbortSignal.timeout = (ms: number) => {
  timeouts.push(ms);
  return original.call(AbortSignal, ms);
};
const requests: unknown[] = [];
const fetcher = async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
  requests.push({
    url: String(input),
    method: init?.method,
    redirect: init?.redirect,
    headers: [...new Headers(init?.headers)],
    aborted: init?.signal?.aborted,
  });
  return new Response('{"limits":[]}');
};
await fetchClaudeCodeUsage({ kind: "oauth", secret: "public-fixture-only" }, { fetcher, now: () => 1800000000000 });
const before = requests.length;
const unsupported = await fetchClaudeCodeUsage({ kind: "api_key", secret: "public-fixture-only" }, { fetcher, now: () => 1800000000000 });
AbortSignal.timeout = original;
writeFileSync(
  resolve(import.meta.dir, "../fixtures/wire-oracle.json"),
  JSON.stringify(
    { source_sha: "b59243d674567d418d1410f8390fc7fe47a08c92", timeouts, requests, unsupported, unsupported_requests: requests.length - before },
    null,
    2
  ) + "\n"
);
console.log(JSON.stringify({ requests: requests.length, timeouts, unsupported_requests: requests.length - before }));
