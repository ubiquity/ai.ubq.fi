import assert from "node:assert/strict";

import { handleRoot, handleStaticAsset, hasStaticAsset } from "../src/static.ts";
import adminHtml from "../static/admin.html" with { type: "text" };
import adminScript from "../static/admin.js" with { type: "text" };
import openApiText from "../static/openapi.json" with { type: "text" };

Deno.test("static assets register frontend module dependencies", () => {
  for (
    const path of [
      "/admin.js",
      "/auth.js",
      "/auth-relay.js",
      "/foreground-refresh.js",
      "/network.js",
      "/reasoning-select.js",
    ]
  ) {
    assert.equal(hasStaticAsset(path), true, `${path} should be registered`);
  }
});

Deno.test("admin provider view places capacity history before current providers", () => {
  const listIndex = adminHtml.indexOf('id="provider-capacity-list"');
  const chartIndex = adminHtml.indexOf('id="provider-capacity-chart"');
  assert.ok(chartIndex >= 0);
  assert.ok(listIndex > chartIndex);

  assert.match(adminHtml, /admin\.js\?v=browser-cache-20260816-v2/);
  assert.doesNotMatch(adminHtml, /removed_provider-failover|debug-routing/);
  assert.doesNotMatch(adminScript, /RemovedProviderFailover|refresh=live/);
  assert.match(adminScript, /fetch\(apiUrl\("\/admin\/providers\/capacity"\)/);
  assert.match(adminScript, /const loadId = \+\+providersLoadId/);
  assert.match(adminScript, /if \(loadId !== providersLoadId\) return/);
  assert.match(adminScript, /cache: "no-store"/);
  assert.match(adminScript, /Authorization: `Bearer \$\{token\}`/);
});

Deno.test("static assets register autonomous agent discovery documents", () => {
  for (const path of ["/llms.txt", "/llms-full.txt", "/docs/llms-agents.md", "/openapi.json"]) {
    assert.equal(hasStaticAsset(path), true, `${path} should be registered`);
  }
});

Deno.test("OpenAPI discovery contract describes the public inference API", () => {
  const document = JSON.parse(openApiText);

  assert.equal(document.openapi, "3.1.0");
  assert.equal(document.servers[0].url, "https://ai.ubq.fi");
  assert.ok(document.components.securitySchemes.bearerAuth);
  assert.ok(document.paths["/v1/models"].get);
  assert.ok(document.paths["/v1/chat/completions"].post);
  assert.ok(document.paths["/v1/responses"].post);
});

Deno.test("agent discovery documents are served with useful media types", async () => {
  const llms = await handleStaticAsset("/llms.txt");
  const llmsFull = await handleStaticAsset("/llms-full.txt");
  const markdown = await handleStaticAsset("/docs/llms-agents.md");
  const openapi = await handleStaticAsset("/openapi.json");

  assert.equal(llms?.status, 200);
  assert.equal(llms?.headers.get("content-type"), "text/plain; charset=utf-8");
  assert.match(await llms!.text(), /https:\/\/ai\.ubq\.fi\/openapi\.json/);
  assert.equal(llmsFull?.headers.get("content-type"), "text/plain; charset=utf-8");
  assert.equal(markdown?.headers.get("content-type"), "text/markdown; charset=utf-8");
  assert.equal(openapi?.headers.get("content-type"), "application/json; charset=utf-8");
});

Deno.test("JSON root advertises autonomous agent discovery documents", async () => {
  const response = await handleRoot(new Request("https://ai.ubq.fi/", { headers: { Accept: "application/json" } }));
  const body = await response.json();

  assert.equal(body.discovery.llms, "/llms.txt");
  assert.equal(body.discovery.llms_full, "/llms-full.txt");
  assert.equal(body.discovery.openapi, "/openapi.json");
});
