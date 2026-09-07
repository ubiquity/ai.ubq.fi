import assert from "node:assert/strict";
import serverSource from "../serve.ts" with { type: "text" };
import handlerSource from "../src/handler.ts" with { type: "text" };
import catalogSource from "../src/codex_catalog.ts" with { type: "text" };
import deploymentWorkflow from "../.github/workflows/deno-deploy.yml" with { type: "text" };

Deno.test("gateway entry points do not start Sentinel automation but keep the authorized encrypted export route", () => {
  assert.doesNotMatch(serverSource, /sentinel/i);
  assert.doesNotMatch(handlerSource, /captureAcceptedSentinelReplayInput/u);
  assert.doesNotMatch(handlerSource, /sentinel_incident_admin/u);
  assert.doesNotMatch(handlerSource, /\/admin\/sentinel\/incidents\//u);
  assert.doesNotMatch(handlerSource, /\?\? recordSentinelProviderDegradationFromEnvironment/u);
  assert.doesNotMatch(catalogSource, /\?\? recordSentinelProviderDegradationFromEnvironment/u);
  assert.doesNotMatch(deploymentWorkflow, /sentinel/i);
  assert.match(handlerSource, /\/admin\/sentinel\/replay-captures/u);
});
