import assert from "node:assert/strict";

import deploymentWorkflow from "../.github/workflows/deno-deploy.yml" with { type: "text" };

Deno.test("deployment workflow validates pull requests without deploying them", () => {
  assert.match(deploymentWorkflow, /^permissions:\n[ ]{2}contents: read$/mu);
  assert.match(deploymentWorkflow, /^[ ]{2}pull_request:$/mu);

  const deployJob = deploymentWorkflow.match(
    /^[ ]{2}deploy:\n([\s\S]*?)(?=^[ ]{2}attest-sentinel-candidate:)/mu,
  )?.[1];
  assert.ok(deployJob, "deploy job must remain present");
  assert.match(deployJob, /github\.event_name != 'pull_request'/u);
  assert.doesNotMatch(deployJob, /github\.event_name == 'pull_request'/u);
});
