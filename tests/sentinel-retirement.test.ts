import assert from "node:assert/strict";
import serverSource from "../serve.ts" with { type: "text" };
import handlerSource from "../src/handler.ts" with { type: "text" };
import catalogSource from "../src/codex_catalog.ts" with { type: "text" };
import deploymentWorkflow from "../.github/workflows/deno-deploy.yml" with { type: "text" };

Deno.test("gateway entry points start no Sentinel automation but keep the authorized capture/export", () => {
  assert.doesNotMatch(serverSource, /sentinel/i);
  // m06 adds only the passive, read-only incident index GET: no automation,
  // dispatch, control or claim/ack/defer wiring may return. The route is now a
  // table entry rather than an inline `method === "GET" && path === ...` chain,
  // so this pins the entry itself -- including its superAdmin level, which the
  // previous shape never checked.
  assert.match(handlerSource, /path: "\/admin\/sentinel\/incidents", superAdmin: true, run: \(req\) => handleAdminSentinelIncidents\(req\)/u);
  assert.match(handlerSource, /handleAdminSentinelIncidents/u);
  assert.doesNotMatch(handlerSource, /coalesceSentinelIncidentFailureEvents/u);
  assert.doesNotMatch(handlerSource, /\?\? recordSentinelProviderDegradationFromEnvironment/u);
  assert.doesNotMatch(catalogSource, /\?\? recordSentinelProviderDegradationFromEnvironment/u);
  assert.match(deploymentWorkflow, /sentinel:test-local/u);
  assert.doesNotMatch(deploymentWorkflow, /provider-sentinel|sentinel-revision-control|scripts\/sentinel/u);
  assert.match(handlerSource, /captureAcceptedSentinelReplayInput/u);
  assert.match(handlerSource, /\/admin\/sentinel\/replay-captures/u);
});
