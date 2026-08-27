import assert from "node:assert/strict";

import deploymentWorkflow from "../.github/workflows/deno-deploy.yml" with { type: "text" };

Deno.test("deployment workflow validates pull requests without deploying them", () => {
  assert.match(deploymentWorkflow, /^permissions:\n[ ]{2}contents: read$/mu);
  assert.match(deploymentWorkflow, /^[ ]{2}pull_request:$/mu);
  assert.match(
    deploymentWorkflow,
    /^[ ]{2}pull_request:\n[ ]{4}paths-ignore:\n[ ]{6}- docs\/sentinel-issue-jobs\.md$/mu,
  );

  const deployJob = deploymentWorkflow.match(
    /^[ ]{2}deploy:\n([\s\S]*?)(?=^[ ]{2}attest-sentinel-candidate:)/mu,
  )?.[1];
  assert.ok(deployJob, "deploy job must remain present");
  assert.match(deployJob, /github\.event_name != 'pull_request'/u);
  assert.doesNotMatch(deployJob, /github\.event_name == 'pull_request'/u);
});

Deno.test("development pushes deploy and promote without a manual production gate", () => {
  assert.doesNotMatch(deploymentWorkflow, /production-approval/u);
  assert.match(deploymentWorkflow, /branches:\n[ ]{6}- development/u);
  assert.match(
    deploymentWorkflow,
    /github\.ref == 'refs\/heads\/development' \|\|/u,
  );
  assert.match(
    deploymentWorkflow,
    /https:\/\/api\.deno\.com\/v2\/revisions\/\$\{revision_id\}\/promote/u,
  );
});

Deno.test("deployment attestation ignores failed builder retry revisions", () => {
  assert.match(
    deploymentWorkflow,
    /select\(\.status != "failed"\)/u,
  );
  assert.match(
    deploymentWorkflow,
    /More than one non-failed revision appeared after the baseline/u,
  );
  assert.match(
    deploymentWorkflow,
    /post-baseline non-failed revision set changed before promotion/u,
  );
  assert.doesNotMatch(
    deploymentWorkflow,
    /More than one revision appeared after the baseline/u,
  );
  assert.doesNotMatch(
    deploymentWorkflow,
    /select\(\.status == "succeeded"\)/u,
  );
});
