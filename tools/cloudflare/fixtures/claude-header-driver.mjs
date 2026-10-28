// Test-only outbound audit around the existing controlled protocol builders.
import baseDriver, { FixtureControl } from "./runtime-driver.mjs";
import { MATRIX_KEYS } from "./provider-matrix.mjs";

export { FixtureControl };

// Both unified and standard response headers use positive integer Unix seconds.
// ISO resets_at belongs to the separate JSON usage-snapshot wire contract.
const startedSeconds = Math.floor(Date.now() / 1000);
const resets = { five: String(startedSeconds + 5 * 60 * 60), seven: String(startedSeconds + 7 * 24 * 60 * 60) };
const allowed = new Map([
  ["api.anthropic.com", new Set(["/v1/messages", "/v1/models", "/v1/organizations/workspaces"])],
  ["passport.ubq.fi", null],
]);
const local = new Set(["127.0.0.1", "localhost", "[::1]"]);
const modes = new Set(["headers-json", "headers-sse", "headers-partial", "headers-none", "headers-account-b", "quota"]);
const oauthCredential = "sk-ant-oat-fixture-header-meter";

const category = (url) => {
  if (url.pathname === "/api/oauth/usage") return "oauth_usage";
  if (url.pathname === "/v1/messages") return "inference";
  if (url.pathname === "/v1/models") return "models";
  if (url.pathname === "/v1/organizations/workspaces") return "workspaces";
  return "other";
};

async function record(env, event) {
  const response = await env.CONTROL.getByName("runtime-control-v1").fetch("https://control.internal/events", {
    method: "POST",
    body: JSON.stringify(event),
  });
  if (response.status !== 204) throw new Error("The complete upstream-attempt audit could not be persisted.");
}

async function modeFor(request, url) {
  if (url.hostname !== "api.anthropic.com" || url.pathname !== "/v1/messages" || request.method !== "POST") return null;
  const body = await request.clone().json();
  const text = JSON.stringify(body.messages ?? body.input ?? "");
  const mode = /fixture:matrix:claude:([a-z0-9-]+)/u.exec(text)?.[1];
  return modes.has(mode) ? mode : null;
}

function controlledCredential(request, url) {
  if (url.hostname !== "api.anthropic.com" || request.headers.get("authorization") !== `Bearer ${oauthCredential}`) return request;
  // Keep an actual OAuth account in Rust (so the removed collector is reachable
  // in a negative candidate), while reusing the existing fixed provider builder.
  // This accepts one synthetic token only, never an arbitrary bearer.
  const headers = new Headers(request.headers);
  headers.delete("authorization");
  headers.set("x-api-key", MATRIX_KEYS.claude);
  return new Request(request, { headers });
}

function observedHeaders(response, mode) {
  const headers = new Headers(response.headers);
  if (["headers-json", "headers-sse", "headers-account-b"].includes(mode)) {
    headers.set("anthropic-ratelimit-unified-5h-utilization", mode === "headers-account-b" ? "0.1" : "0.4");
    headers.set("anthropic-ratelimit-unified-5h-reset", resets.five);
    headers.set("anthropic-ratelimit-unified-7d-utilization", "0.2");
    headers.set("anthropic-ratelimit-unified-7d-reset", resets.seven);
    headers.set("anthropic-ratelimit-requests-limit", "100");
    headers.set("anthropic-ratelimit-requests-remaining", "70");
    headers.set("anthropic-ratelimit-requests-reset", resets.five);
  }
  if (mode === "headers-partial") {
    headers.set("anthropic-ratelimit-unified-5h-utilization", "0.6");
    headers.set("anthropic-ratelimit-unified-5h-reset", resets.five);
  }
  if (mode === "quota") {
    headers.set("anthropic-ratelimit-unified-7d-utilization", "0.25");
    headers.set("anthropic-ratelimit-unified-7d-reset", resets.seven);
    headers.set("retry-after", "60");
  }
  // Keep the original controlled body, including progressive SSE and refusals.
  return new Response(response.body, { status: response.status, statusText: response.statusText, headers });
}

export default {
  async fetch(request, env, context) {
    const url = new URL(request.url);
    const incoming = local.has(url.hostname) && (url.pathname === "/control" || url.pathname === "/store" || url.pathname.startsWith("/public/"));
    if (incoming) return baseDriver.fetch(request, env, context);
    // Audit before parsing, allowlist refusal, or protocol dispatch. Unknown and
    // rejected OAuth requests must remain visible, rather than disappearing from
    // matrix_dispatch (which records only admitted controlled provider requests).
    await record(env, {
      kind: "claude_header_upstream_attempt",
      method: request.method,
      hostname: url.hostname,
      pathname: url.pathname,
      category: category(url),
    });
    const paths = allowed.get(url.hostname);
    if (!allowed.has(url.hostname) || (paths !== null && !paths.has(url.pathname)))
      return Response.json({ error: { type: "fixture_outbound_refused" } }, { status: 503 });
    const mode = await modeFor(request, url);
    const response = await baseDriver.fetch(controlledCredential(request, url), env, context);
    if (mode === null) return response;
    const observed = observedHeaders(response, mode);
    await record(env, {
      kind: "claude_header_response",
      mode,
      status: observed.status,
      headers: Object.fromEntries([...observed.headers].filter(([name]) => name.startsWith("anthropic-ratelimit-"))),
    });
    return observed;
  },
};
