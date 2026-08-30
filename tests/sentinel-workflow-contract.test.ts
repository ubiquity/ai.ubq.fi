import assert from "node:assert/strict";
import workflow from "../.github/workflows/provider-sentinel.yml" with { type: "text" };
import bootstrapWorkflow from "../.github/workflows/provider-sentinel-bootstrap.yml" with { type: "text" };
import githubClient from "../scripts/sentinel/github.ts" with { type: "text" };
import orchestrator from "../scripts/sentinel/main.ts" with { type: "text" };

const jobSection = (name: string, nextName: string): string => {
  const start = workflow.indexOf(`\n  ${name}:`);
  const end = workflow.indexOf(`\n  ${nextName}:`, start + 1);
  assert.ok(start >= 0, `workflow is missing ${name} job`);
  assert.ok(end > start, `workflow is missing the boundary after ${name}`);
  return workflow.slice(start, end);
};

const scheduleBlock = (document: string): string => {
  const start = document.indexOf("  schedule:");
  const end = document.indexOf("  workflow_dispatch:", start);
  assert.ok(start >= 0, "workflow is missing a schedule block");
  assert.ok(end > start, "workflow has no bounded schedule block");
  return document.slice(start, end);
};

const cronMinutes = (expression: string): number[] => {
  const minute = expression.split(/\s+/u)[0];
  if (minute.startsWith("*/")) {
    const step = Number(minute.slice(2));
    assert.ok(Number.isSafeInteger(step) && step >= 1, `unsupported minute step ${minute}`);
    return Array.from({ length: Math.floor(60 / step) }, (_, index) => index * step);
  }
  const minutes = minute.split(",").map((value) => Number(value));
  assert.ok(
    minutes.every((value) => Number.isSafeInteger(value) && value >= 0 && value < 60),
    `invalid minute list ${minute}`,
  );
  return minutes;
};

const prepare = jobSection("prepare", "repair");
const repair = jobSection("repair", "converge");
const converge = workflow.slice(workflow.indexOf("\n  converge:") + 1);

Deno.test("Provider Sentinel retains its outer serialized, non-cancelling concurrency contract", () => {
  assert.match(workflow, /group: provider-sentinel-\$\{\{ github\.repository \}\}/u);
  assert.match(workflow, /cancel-in-progress: false/u);
  assert(!/^\s+queue:/mu.test(workflow), "GitHub Actions does not support a concurrency queue key");
});

Deno.test("Provider Sentinel evaluates every five minutes with a staggered bootstrap cadence", () => {
  const mainCron = /- cron: "([^"]+)"/u.exec(scheduleBlock(workflow))?.[1];
  const bootstrapCron = /- cron: "([^"]+)"/u.exec(scheduleBlock(bootstrapWorkflow))?.[1];
  assert.equal(mainCron, "*/5 * * * *");
  assert.equal(bootstrapCron, "2,7,12,17,22,27,32,37,42,47,52,57 * * * *");
  const mainMinutes = new Set(cronMinutes(mainCron));
  const bootstrapMinutes = new Set(cronMinutes(bootstrapCron));
  assert.equal(mainMinutes.size, 12);
  assert.equal(bootstrapMinutes.size, 12);
  assert.ok([...mainMinutes].every((minute) => minute % 5 === 0));
  assert.ok([...bootstrapMinutes].every((minute) => minute % 5 === 2));
  for (const minute of bootstrapMinutes) {
    assert.ok(
      !mainMinutes.has(minute),
      `bootstrap fires in the same scheduled minute as the main workflow (${minute})`,
    );
  }
  // Both schedule identity checks in the main workflow must accept the rapid cadence.
  assert.equal(workflow.match(/\[ "\$\{\{ github\.event\.schedule \}\}" = "\*\/5 \* \* \* \*" \]/gu)?.length, 2);
  // Staggering must not remove either workflow's own serializing, non-cancelling group.
  assert.match(bootstrapWorkflow, /group: provider-sentinel-bootstrap-\$\{\{ github\.repository \}\}/u);
  assert.match(bootstrapWorkflow, /cancel-in-progress: false/u);
});

Deno.test("Provider Sentinel uses Deno eval without unsupported permission flags", () => {
  assert.doesNotMatch(workflow, /deno eval[^'\n]*--allow-/u);
});

Deno.test("Provider Sentinel has explicit prepare, bounded repair, and convergence job contracts", () => {
  assert.doesNotMatch(workflow, /^\s+sentinel:/mu);
  assert.match(workflow, /^\s{2}prepare:/mu);
  assert.match(workflow, /^\s{2}repair:/mu);
  assert.match(workflow, /^\s{2}converge:/mu);

  assert.match(
    prepare,
    /permissions:\n\s+actions: write\n\s+contents: write\n\s+issues: read\n\s+pull-requests: read/u,
  );
  assert.match(prepare, /main\.ts --mode "\$SENTINEL_MODE"/u);
  assert.match(prepare, /\.sentinel\/reports\/matrix-plan\.json/u);
  assert.match(prepare, /Upload encrypted immutable matrix plan/u);
  assert.match(prepare, /path: \.sentinel\/encrypted\/sentinel-evidence-v1\.json/u);
  assert.match(prepare, /Scrub preparation plaintext after durable upload/u);
  assert.match(prepare, /steps\.matrix-plan-upload\.outcome == 'success'/u);

  assert.match(repair, /needs: prepare/u);
  assert.match(repair, /fail-fast: false/u);
  assert.match(repair, /max-parallel: 4/u);
  assert.match(repair, /matrix: \$\{\{ fromJSON\(needs\.prepare\.outputs\.matrix\) \}\}/u);
  assert.match(repair, /ref: \$\{\{ matrix\.base_sha \}\}/u);
  assert.match(repair, /git switch --create "\$CELL_BRANCH" "\$CELL_BASE_SHA"/u);
  assert.match(repair, /runMatrixCell/u);
  assert.match(repair, /reports\/triage\.json/u);
  assert.match(repair, /triageValue\.findings\.filter/u);
  assert.match(repair, /--force-with-lease=/u);
  assert.match(repair, /Upload encrypted cell evidence/u);
  assert.match(repair, /path: \.sentinel\/encrypted\/sentinel-evidence-v1\.json/u);
  assert.match(repair, /Scrub cell plaintext after durable upload/u);
  assert.match(repair, /steps\.cell-evidence-upload\.outcome == 'success'/u);
  assert.match(repair, /Required matrix cell failed closed/u);
  assert.doesNotMatch(repair, /path:\s*\n\s+\.sentinel\/(?:raw-logs|reports)(?:\s|$)/u);

  assert.match(converge, /needs: \[prepare, repair\]/u);
  assert.match(converge, /always\(\)/u);
  assert.match(converge, /ref: \$\{\{ needs\.prepare\.outputs\.base_sha \|\| github\.sha \}\}/u);
  assert.match(converge, /decryptSentinelArtifact/u);
  assert.match(converge, /validateMatrixCellReportV1/u);
  assert.match(converge, /required matrix cell artifact is missing/u);
  assert.match(converge, /Verify exact remote cell heads before convergence/u);
  assert.match(converge, /git ls-remote --heads origin/u);
  assert.match(converge, /remote_sha.*report_sha|report_sha.*remote_sha/u);
  assert.match(converge, /Run Provider Sentinel/u);
  assert.match(converge, /Upload encrypted Sentinel evidence/u);
});

Deno.test("Provider Sentinel matrix transport stays encrypted and uses least-privilege cell permissions", () => {
  assert.match(repair, /permissions:\n\s+actions: write\n\s+contents: write/u);
  assert.doesNotMatch(repair, /permissions:[\s\S]*?issues:\s+write/u);
  assert.doesNotMatch(repair, /permissions:[\s\S]*?pull-requests:\s+write/u);
  assert.match(repair, /SENTINEL_ARTIFACT_KEY/u);
  assert.match(repair, /encrypt-artifacts\.ts/u);
  assert.match(repair, /actions\/download-artifact@d3f86a106a0bac45b974a628896c90dbdf5c8093/u);
  assert.doesNotMatch(repair, /upload-artifact@[\w-]+[\s\S]*?path:\s*\n\s+\.sentinel\/(?:raw-logs|reports)(?:\s|$)/u);
  assert.match(converge, /actions\/download-artifact@d3f86a106a0bac45b974a628896c90dbdf5c8093/u);
  assert.match(converge, /SENTINEL_ARTIFACT_KEY/u);
  assert.match(converge, /SENTINEL_CODEX_AUTH_STATE_KEY/u);
});

Deno.test("Provider Sentinel preserves incident, fixed-model, and delivery/attestation controls", () => {
  assert.match(prepare, /incident_claimed: \$\{\{ steps\.incident-claim\.outcome == 'success' \}\}/u);
  assert.match(repair, /Run immutable Luna matrix cell/u);
  assert.match(converge, /Defer incident after infrastructure failure/u);
  assert.match(converge, /Acknowledge completed incident/u);
  assert.match(converge, /Reconcile GitHub issue delivery/u);
  assert.match(converge, /Publish supervised cycle summary/u);
  assert.match(converge, /scripts\/sentinel\/main\.ts/u);
  assert.match(converge, /SENTINEL_CODEX_AUTH_STATE_ENVELOPE/u);
});

Deno.test("matrix cells cannot review or deploy and convergence has one integrated delivery", () => {
  assert.doesNotMatch(repair, /runNativeCodexReview|dispatchAndResolveRevision|deno-deploy\.yml|promote/u);
  assert.equal(orchestrator.match(/runNativeCodexReview\(/gu)?.length, 1);
  assert.match(orchestrator, /github\.createPullRequest\(/u);
  assert.match(orchestrator, /github\.mergePullRequest\(/u);
  assert.match(githubClient, /merge_method: "merge"/u);
  assert.match(orchestrator, /Merged development lost required ancestry/u);
  assert.match(orchestrator, /writeMatrixDelivery\(\{\s*status: "published"/u);
});
