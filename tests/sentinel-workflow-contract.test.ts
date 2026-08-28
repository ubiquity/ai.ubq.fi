import assert from "node:assert/strict";
import workflow from "../.github/workflows/provider-sentinel.yml" with { type: "text" };
import githubClient from "../scripts/sentinel/github.ts" with { type: "text" };
import orchestrator from "../scripts/sentinel/main.ts" with { type: "text" };

const jobSection = (name: string, nextName: string): string => {
  const start = workflow.indexOf(`\n  ${name}:`);
  const end = workflow.indexOf(`\n  ${nextName}:`, start + 1);
  assert.ok(start >= 0, `workflow is missing ${name} job`);
  assert.ok(end > start, `workflow is missing the boundary after ${name}`);
  return workflow.slice(start, end);
};

const prepare = jobSection("prepare", "repair");
const repair = jobSection("repair", "converge");
const converge = workflow.slice(workflow.indexOf("\n  converge:") + 1);

Deno.test("Provider Sentinel retains its outer serialized, non-cancelling concurrency contract", () => {
  assert.match(workflow, /group: provider-sentinel-\$\{\{ github\.repository \}\}/u);
  assert.match(workflow, /cancel-in-progress: false/u);
  assert.match(workflow, /^\s+queue: max$/mu);
});

Deno.test("Provider Sentinel has explicit prepare, bounded repair, and convergence job contracts", () => {
  assert.doesNotMatch(workflow, /^\s+sentinel:/mu);
  assert.match(workflow, /^\s{2}prepare:/mu);
  assert.match(workflow, /^\s{2}repair:/mu);
  assert.match(workflow, /^\s{2}converge:/mu);

  assert.match(prepare, /permissions:\n\s+actions: write\n\s+contents: read\n\s+issues: read\n\s+pull-requests: read/u);
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
