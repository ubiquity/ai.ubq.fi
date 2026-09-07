import assert from "node:assert/strict";
import serverSource from "../serve.ts" with { type: "text" };
import handlerSource from "../src/handler.ts" with { type: "text" };
import catalogSource from "../src/codex_catalog.ts" with { type: "text" };
import deploymentWorkflow from "../.github/workflows/deno-deploy.yml" with { type: "text" };

Deno.test("gateway entry points do not start Sentinel automation", () => {
  assert.doesNotMatch(serverSource, /sentinel/i);
  assert.doesNotMatch(handlerSource, /captureAcceptedSentinelReplayInput|\/admin\/sentinel\//u);
  assert.doesNotMatch(handlerSource, /\?\? recordSentinelProviderDegradationFromEnvironment/u);
  assert.doesNotMatch(catalogSource, /\?\? recordSentinelProviderDegradationFromEnvironment/u);
  assert.doesNotMatch(deploymentWorkflow, /sentinel/i);
});
