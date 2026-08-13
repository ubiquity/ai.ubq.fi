import assert from "node:assert/strict";

import { hasStaticAsset } from "../src/static.ts";
import adminHtml from "../static/admin.html" with { type: "text" };
import adminScript from "../static/admin.js" with { type: "text" };

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

Deno.test("admin provider view declares and renders the OpenRouter failover card", () => {
  for (
    const id of [
      "card-openrouter-failover",
      "openrouter-failover-badge",
      "openrouter-failover-observed",
      "openrouter-failover-facts",
    ]
  ) assert.match(adminHtml, new RegExp(`id=["']${id}["']`));

  assert.match(adminHtml, /admin\.js\?v=openrouter-failover-20260813-v2/);
  assert.match(adminScript, /renderOpenRouterFailover\(payload\.openrouter\)/);
  assert.match(adminScript, /const loadId = \+\+providersLoadId/);
  assert.match(adminScript, /if \(loadId !== providersLoadId\) return/);
  assert.match(adminScript, /cache: "no-store"/);
  assert.match(adminScript, /Authorization: `Bearer \$\{token\}`/);
  for (
    const field of [
      "attempted_provider",
      "trigger_class",
      "circuit_transition",
      "selected_model",
      "task_type",
      "latency_ms",
      "terminal_status",
      "semantic_commitment",
    ]
  ) assert.match(adminScript, new RegExp(`telemetry\\.${field}`));
});
