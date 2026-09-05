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

  // A retry_pending cell is a valid durable publication exactly when its
  // authenticated retry evidence (recovery identity and candidate snapshot)
  // was captured beside the report and the encrypted cell evidence was
  // durably uploaded. The gate runs before the plaintext scrub so the
  // evidence is still present and fails closed on any other status.
  assert.match(repair, /CELL_EVIDENCE_UPLOAD_OUTCOME/u);
  assert.match(repair, /case "\$status" in/u);
  assert.match(repair, /retry_pending\)/u);
  assert.match(repair, /recovery-record\.json/u);
  assert.match(repair, /manifest\.json/u);
  assert.match(repair, /Required matrix cell retry evidence is missing: status=\$status/u);
  const publicationStart = repair.indexOf("      - name: Require successful immutable cell publication");
  const scrubStart = repair.indexOf("      - name: Scrub cell plaintext after durable upload");
  assert.ok(
    publicationStart >= 0 && scrubStart > publicationStart,
    "the publication gate must run before the plaintext scrub",
  );

  assert.match(converge, /needs: \[prepare, repair\]/u);
  assert.match(converge, /always\(\)/u);
  assert.match(converge, /ref: \$\{\{ needs\.prepare\.outputs\.base_sha \|\| github\.sha \}\}/u);
  assert.match(converge, /decryptSentinelArtifact/u);
  assert.match(converge, /validateMatrixCellReportV1/u);
  assert.match(converge, /required matrix cell artifact is missing/u);
  // The materialized convergence inputs prove retry evidence presence inside
  // the ciphertext: a retry_pending report without its manifest and recovery
  // record (or without any report at all) fails closed.
  assert.match(converge, /retry_pending matrix cell report is missing durable retry evidence/u);
  assert.match(converge, /Verify exact remote cell heads before convergence/u);
  assert.match(converge, /git ls-remote --heads origin/u);
  assert.match(converge, /remote_sha.*report_sha|report_sha.*remote_sha/u);
  // Retry_pending cells have no published head to verify: the converge step
  // accepts them only without a claimed head and keeps verifying succeeded
  // cells against their exact immutable remote head.
  assert.match(converge, /report_status" = "retry_pending"/u);
  assert.match(converge, /Retry_pending cell \$branch claims a published head/u);
  assert.match(converge, /has no succeeded report with an exact head SHA/u);
  assert.match(converge, /Run Provider Sentinel/u);
  assert.match(converge, /Upload encrypted Sentinel evidence/u);
});

Deno.test("prepare installs and verifies tool prerequisites before the immutable matrix plan", () => {
  const prerequisitesStart = prepare.indexOf("      - name: Install preparation prerequisites");
  const matrixPlanStart = prepare.indexOf("      - name: Prepare immutable MatrixPlanV1");
  assert.ok(prerequisitesStart >= 0, "prepare is missing its preparation prerequisites step");
  assert.ok(
    matrixPlanStart > prerequisitesStart,
    "preparation prerequisites must install and verify before Prepare immutable MatrixPlanV1",
  );
  const step = prepare.slice(prerequisitesStart, matrixPlanStart);
  const commandIndex = (needle: string): number => {
    const index = step.indexOf(needle);
    assert.ok(index >= 0, `preparation prerequisites step is missing: ${needle}`);
    return index;
  };
  const updateIndex = commandIndex("sudo apt-get update");
  const bubblewrapIndex = commandIndex("sudo apt-get install --yes bubblewrap");
  const npmIndex = commandIndex(
    "npm install --global --ignore-scripts --no-audit --no-fund @openai/codex@0.149.0",
  );
  const bwrapVersionIndex = commandIndex("bwrap --version");
  const codexVersionIndex = commandIndex("codex --version");
  assert.ok(bwrapVersionIndex > bubblewrapIndex, "bwrap must be verified after it is installed");
  assert.ok(codexVersionIndex > npmIndex, "codex must be verified after it is installed");
  assert.ok(bubblewrapIndex > updateIndex, "bubblewrap must be installed after apt-get update");
  // The prepare step installs and verifies the exact Codex pin the repair and
  // converge jobs install; a drift between the pins is a contract break.
  const codexPin = (document: string): string => {
    const pin = /@openai\/codex@([0-9][0-9A-Za-z.-]*)/u.exec(document);
    assert.ok(pin !== null, "workflow is missing its pinned @openai/codex install");
    return pin[1];
  };
  assert.equal(codexPin(step), codexPin(repair));
  assert.equal(codexPin(step), codexPin(converge));
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

Deno.test("recovery reconciliation runs before selection even when convergence head verification fails", () => {
  const verifyIndex = converge.indexOf("      - name: Verify exact remote cell heads before convergence");
  const reconcileIndex = converge.indexOf("      - name: Reconcile durable recovery records");
  const selectionIndex = converge.indexOf("      - name: Select agent work");
  assert.ok(verifyIndex >= 0, "converge is missing exact remote head verification");
  assert.ok(reconcileIndex > verifyIndex, "recovery reconciliation must run after head verification");
  assert.ok(selectionIndex > reconcileIndex, "recovery reconciliation must run before normal selection");
  const reconcileStep = converge.slice(reconcileIndex, selectionIndex);
  assert.match(reconcileStep, /\n\s+if: always\(\)\n/u);
});

Deno.test("matrix cell retry evidence is encrypted from the cell report path", () => {
  assert.match(repair, /sentinel-cell-report\.json\.retry-evidence/u);
  assert.match(repair, /mkdir -p "\.sentinel\/reports\/matrix\/\$\{CELL_ID\}"/u);
  assert.match(repair, /cp -a "\$RUNNER_TEMP\/sentinel-cell-report\.json\.retry-evidence"/u);
  assert.doesNotMatch(repair, /upload-artifact@[\w-]+[\s\S]*?path:\s*\n\s+\.sentinel\/(?:raw-logs|reports)(?:\s|$)/u);
});

Deno.test("the cell runner receives the authoritative work selection and scrubs partial evidence staging", () => {
  assert.match(repair, /matrixCellWorkSelectionFromArtifact/u);
  assert.match(repair, /sentinel-matrix-work-selection\.json/u);
  assert.match(repair, /workSelection: workSelection \?\? undefined/u);
  assert.match(repair, /sentinel-cell-report\.json\.retry-evidence\.staging/u);
  // JSON.stringify yields a string, which Deno.writeFile rejects with
  // `TypeError: expected Uint8Array, got string`. The materialized selection
  // must use the text API so this exact bug cannot return.
  assert.match(
    repair,
    /Deno\.writeTextFile\([^\n]*sentinel-matrix-work-selection\.json[^\n]*JSON\.stringify\(workSelection\)/u,
    "the work selection write must use Deno.writeTextFile for its JSON.stringify output",
  );
  assert.doesNotMatch(
    repair,
    /Deno\.writeFile\([^\n]*JSON\.stringify\(workSelection\)/u,
    "Deno.writeFile must never receive the JSON.stringify work selection string",
  );
  // Work selection is plaintext evidence metadata that must be scrubbed.
  const scrubStep = repair.slice(
    repair.indexOf('for target in "$RUNNER_TEMP/sentinel-codex-auth-state"'),
    repair.indexOf('for target in "$RUNNER_TEMP/sentinel-codex-auth-state"') + 800,
  );
  assert.match(scrubStep, /sentinel-matrix-work-selection\.json/u);
  assert.match(scrubStep, /retry-evidence\.staging/u);
  // A moved-aside prior evidence set is also plaintext and must be scrubbed.
  assert.match(scrubStep, /retry-evidence\.previous/u);
});

Deno.test("matrix cells cannot review or deploy and convergence has one integrated delivery", () => {
  assert.doesNotMatch(repair, /runNativeCodexReview|dispatchAndResolveRevision|deno-deploy\.yml|promote/u);
  assert.equal(orchestrator.match(/runNativeCodexReview\(/gu)?.length ?? 0, 0);
  assert.match(orchestrator, /github\.createPullRequest\(/u);
  assert.match(orchestrator, /github\.mergePullRequest\(/u);
  assert.match(githubClient, /merge_method: "merge"/u);
  assert.match(orchestrator, /Merged development lost required ancestry/u);
  assert.match(orchestrator, /writeMatrixDelivery\(\{\s*status: "published"/u);
});

Deno.test("Provider Sentinel verification is exactly the canonical local command", () => {
  assert.equal(
    workflow.match(/deno task sentinel:test-local/gu)?.length ?? 0,
    1,
    "CI must invoke exactly `deno task sentinel:test-local` for Sentinel verification",
  );
  assert.doesNotMatch(workflow, /deno test\b/u, "CI must not define its own Sentinel verification steps");
});

Deno.test("rolling review discovery independently drives the async reviewer gate", () => {
  const selectorStart = workflow.indexOf("- name: Select agent work");
  const selectorEnd = workflow.indexOf(
    "- name: Install isolated-agent prerequisites",
    selectorStart,
  );
  assert.ok(selectorStart >= 0 && selectorEnd > selectorStart, "the agent work selector must precede prerequisites");
  const selector = workflow.slice(selectorStart, selectorEnd);
  const reviewStart = workflow.indexOf("- name: Review delivered Sentinel pull requests asynchronously", selectorEnd);
  const reviewEnd = workflow.indexOf("- name: Install issue-delivery development-push gate", reviewStart);
  assert.ok(reviewStart >= 0 && reviewEnd > reviewStart, "the asynchronous review worker step must exist");
  const reviewStep = workflow.slice(reviewStart, reviewEnd);

  // The selector folds a bounded read-only rolling review discovery into the
  // same needs_agent output that gates prerequisite installation, pinned CLI
  // validation, and the async reviewer.
  assert.match(selector, /git ls-tree -r --name-only origin\/development -- docs\/sentinel-review-results/u);
  assert.match(selector, /sentinel-review-results\.txt/u);
  assert.match(selector, /listPullRequests\(\{ state: "all" \}\)/u);
  assert.match(selector, /parseRollingReviewResultFileNames/u);
  assert.match(selector, /selectRollingReviewTaskFromIdentities/u);
  assert.match(
    selector,
    /selectRollingReviewTaskFromIdentities\(pulls, identities\) === null \? "false" : "true"/u,
    "an eligible unreviewed pull request must turn the agent gate on",
  );
  assert.match(
    selector,
    /if \[ "\$needs_agent" = "false" \] && \[ "\$hint_ready" = "true" \]; then/u,
    "review discovery must run only after backlog and issue selection are both absent",
  );
  assert.match(selector, /installing agent prerequisites conservatively/u);
  // The discovery is read-only: no workflow dispatch, merge, or pull creation.
  assert.doesNotMatch(selector, /dispatchWorkflow|mergePullRequest|createPullRequest/u);

  // The reviewer is gated by exactly the same needs_agent value: with neither
  // backlog/issue work nor an eligible unreviewed pull request the step is
  // skipped and no review invocation runs.
  assert.match(reviewStep, /if: steps\.agent-work\.outputs\.needs_agent == 'true'/u);
  assert.match(reviewStep, /scripts\/sentinel\/rolling-review-worker\.ts/u);
  assert.match(reviewStep, /--allow-run=git,codex,gitleaks,deno/u);
});

Deno.test("matrix convergence accepts durable retry_pending reports and stops publication", () => {
  // The orchestrator verifies exact remote heads only for succeeded cells. A
  // retry_pending report is a durable retry publication without a head; one
  // that claims a head is invalid and fails closed.
  assert.match(orchestrator, /status === "retry_pending"/u);
  assert.match(orchestrator, /is retry_pending with a published head/u);
  assert.match(orchestrator, /Required matrix cell \$\{report\.cell_id\} did not succeed/u);
  // A retry_pending cell stops the cycle before integration: no merge, no
  // delivery, and the retry_pending disposition is recorded in the immutable
  // cycle report while the bounded retry circuit schedules the retry.
  assert.match(orchestrator, /retryPendingCells\.length > 0/u);
  assert.match(orchestrator, /matrix_retry_pending_cell_branches_retained/u);
  assert.match(orchestrator, /cell_dispositions: cellDispositions/u);
  assert.match(orchestrator, /integrated_candidate: null/u);
  assert.match(orchestrator, /matrix-cycle\.json/u);
});
