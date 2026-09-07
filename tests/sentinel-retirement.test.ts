import assert from "node:assert/strict";
import serverSource from "../serve.ts" with { type: "text" };
import handlerSource from "../src/handler.ts" with { type: "text" };
import catalogSource from "../src/codex_catalog.ts" with { type: "text" };
import deploymentWorkflow from "../.github/workflows/deno-deploy.yml" with { type: "text" };

Deno.test("gateway entry points start no Sentinel automation but keep the authorized capture/export", () => {
  assert.doesNotMatch(serverSource, /sentinel/i);
  assert.doesNotMatch(handlerSource, /sentinel_incident_admin/u);
  assert.doesNotMatch(handlerSource, /\/admin\/sentinel\/incidents\//u);
  assert.doesNotMatch(handlerSource, /\?\? recordSentinelProviderDegradationFromEnvironment/u);
  assert.doesNotMatch(catalogSource, /\?\? recordSentinelProviderDegradationFromEnvironment/u);
  assert.match(deploymentWorkflow, /sentinel:test-local/u);
  assert.doesNotMatch(deploymentWorkflow, /provider-sentinel|sentinel-revision-control|scripts\/sentinel/u);
  assert.match(handlerSource, /captureAcceptedSentinelReplayInput/u);
  assert.match(handlerSource, /\/admin\/sentinel\/replay-captures/u);
});
