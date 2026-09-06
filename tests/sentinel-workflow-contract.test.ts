import assert from "node:assert/strict";
import workflow from "../.github/workflows/provider-sentinel.yml" with { type: "text" };
import bootstrapWorkflow from "../.github/workflows/provider-sentinel-bootstrap.yml" with { type: "text" };
import githubClient from "../scripts/sentinel/github.ts" with { type: "text" };
import orchestrator from "../scripts/sentinel/main.ts" with { type: "text" };

type Validator = (input: { checkoutPath: string }) => Promise<{
  passed: boolean;
  checks: Array<{ name: string; passed: boolean; detail: string }>;
}>;

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
  // Retained planning runs inside the immutable preparation step: the existing
  // artifact key is passed there under its existing name so scheduled
  // preparation can decrypt retained frozen evidence. Model and validation
  // children never receive it.
  const prepareStepStart = prepare.indexOf("      - name: Prepare immutable MatrixPlanV1");
  assert.ok(prepareStepStart >= 0, "prepare is missing its immutable matrix plan step");
  const prepareStep = prepare.slice(
    prepareStepStart,
    prepare.indexOf("      - name: ", prepareStepStart + 1),
  );
  assert.match(prepareStep, /SENTINEL_ARTIFACT_KEY: \$\{\{ secrets\.SENTINEL_ARTIFACT_KEY \}\}/u);
  assert.doesNotMatch(prepareStep, /--allow-env=SENTINEL_ARTIFACT_KEY/u);
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

Deno.test("recovery reconciliation runs immediately after the sentinel run and always cleans up", () => {
  const sentinelRunIndex = converge.indexOf("      - name: Run Provider Sentinel");
  const reconcileIndex = converge.indexOf("      - name: Reconcile durable recovery records");
  const selectionIndex = converge.indexOf("      - name: Select agent work");
  const issueDeliveryIndex = converge.indexOf("      - name: Reconcile GitHub issue delivery");
  assert.ok(sentinelRunIndex >= 0, "converge is missing the provider sentinel run");
  assert.ok(reconcileIndex > sentinelRunIndex, "recovery reconciliation must run after Run Provider Sentinel");
  assert.ok(reconcileIndex > selectionIndex, "recovery reconciliation must not run before normal selection");
  assert.ok(
    issueDeliveryIndex > reconcileIndex,
    "recovery reconciliation must run before Reconcile GitHub issue delivery",
  );
  // Exact source order: no step may sit between Run Provider Sentinel and the
  // recovery reconciliation, so the freshly claimed prepare record is never
  // mutated before the sentinel run consumes it.
  const nextStepIndex = converge.indexOf("      - name: ", sentinelRunIndex + 1);
  assert.equal(
    nextStepIndex,
    reconcileIndex,
    "recovery reconciliation must be the next step after Run Provider Sentinel",
  );
  // The moved step keeps its always() cleanup contract and every command,
  // permission, and env unchanged.
  const reconcileStepEnd = converge.indexOf("      - name: ", reconcileIndex + 1);
  const reconcileStep = converge.slice(reconcileIndex, reconcileStepEnd);
  assert.match(reconcileStep, /\n\s+if: always\(\)\n/u);
  assert.match(reconcileStep, /GITHUB_TOKEN: \$\{\{ github\.token \}\}/u);
  assert.match(reconcileStep, /set -euo pipefail/u);
  assert.match(reconcileStep, /scripts\/sentinel\/recovery-controller\.ts/u);
  assert.match(reconcileStep, /--cached-only/u);
  assert.match(reconcileStep, /--frozen/u);
  assert.match(reconcileStep, /--lock=deno\.lock/u);
  assert.match(reconcileStep, /--allow-env=GITHUB_TOKEN,GITHUB_REPOSITORY,GITHUB_RUN_ID/u);
  assert.match(reconcileStep, /--allow-net=api\.github\.com/u);
  // The sentinel run step receives the existing artifact key under its
  // existing name so authenticated retained-plan recovery can decrypt the
  // original evidence envelopes; agent/validation children never receive it.
  const sentinelRunStepStart = converge.indexOf("      - name: Run Provider Sentinel");
  const sentinelRunStep = converge.slice(
    sentinelRunStepStart,
    converge.indexOf("      - name: ", sentinelRunStepStart + 1),
  );
  assert.match(sentinelRunStep, /SENTINEL_ARTIFACT_KEY: \$\{\{ secrets\.SENTINEL_ARTIFACT_KEY \}\}/u);
  assert.match(sentinelRunStep, /SENTINEL_REPLAY_KEY: \$\{\{ secrets\.SENTINEL_REPLAY_KEY \}\}/u);
  assert.match(sentinelRunStep, /--allow-env/u);
  // The agent/validation children must not carry the artifact key: their
  // deno invocations never pass it, and no bwrap/codex/gitleaks child leak.
  assert.doesNotMatch(sentinelRunStep, /--allow-env=SENTINEL_ARTIFACT_KEY/u);
});

Deno.test("convergence authentically materializes the prepared recovery and issue selection", () => {
  const materializeStart = converge.indexOf(
    "      - name: Materialize and verify encrypted matrix convergence inputs",
  );
  const materializeEnd = converge.indexOf(
    "      - name: Require complete matrix convergence contract",
    materializeStart,
  );
  assert.ok(
    materializeStart >= 0 && materializeEnd > materializeStart,
    "the convergence materialization step must exist",
  );
  const materialize = converge.slice(materializeStart, materializeEnd);
  // Materialization runs inside the authenticated decryption: no plaintext
  // matrix artifact is ever trusted as a convergence input.
  assert.match(materialize, /decodeSentinelArtifactKey/u);
  assert.match(materialize, /decryptSentinelArtifact/u);
  assert.match(materialize, /parseSentinelRecoveryRecord/u);
  assert.match(materialize, /parseGitHubIssueSelectionReport/u);
  // The prepared claimed recovery record and its issue selection are
  // materialized for exactly the cell-bearing matrix plan before the run.
  assert.match(materialize, /plan\.cells\.length > 0/u);
  assert.match(materialize, /reports\/recovery-record-v1\.json/u);
  assert.match(materialize, /reports\/github-issue-selection\.json/u);
  assert.match(
    materialize,
    /recovery\.run_id !== plan\.run_id \|\| recovery\.base_sha !== plan\.base_sha/u,
    "the materialized recovery record must keep its exact matrix plan identity",
  );
  assert.match(
    materialize,
    /recovery\.phase !== "claimed" \|\| recovery\.disposition !== "active"/u,
    "the prepared recovery record must still be claimed and active when materialized",
  );
  assert.match(
    materialize,
    /github issue selection does not match the sentinel recovery identity/u,
    "a github-issue recovery must match its materialized issue selection exactly",
  );
});

Deno.test("matrix convergence guards checkpoint resume and reuses the prepared recovery record", () => {
  // Matrix convergence must never resume an issue retry checkpoint, never take
  // the recovery_pending no-change path, and never prepare a retried issue
  // candidate: the prepared claimed record is reused exactly.
  assert.match(orchestrator, /!matrixConvergePhase && workSelection\.issueJob && retryCheckpoint/u);
  assert.match(orchestrator, /!matrixConvergePhase && currentRecoveryRecord && !retryIsDue/u);
  assert.match(orchestrator, /!matrixConvergePhase && currentRecoveryRecord && retryIsDue/u);
  assert.match(orchestrator, /reuseMatrixPreparedRecoveryRecord/u);
  assert.match(orchestrator, /Prepared sentinel recovery record differs from the ledger recovery record/u);
  assert.match(orchestrator, /Ledger sentinel recovery record is not an active claimed recovery/u);
  assert.match(orchestrator, /Matrix convergence lost its current recovery record/u);
  assert.match(orchestrator, /Matrix convergence lost its prepared recovery record/u);
});

Deno.test("matrix convergence applies selection dispositions and wires the issue PR helper", () => {
  // The focused matrix implementation applies the exact same initial
  // dispositions as the normal path and wires the issue-delivery PR helper
  // into the matrix integration path.
  assert.match(orchestrator, /applyInitialSelectedBacklogDisposition\(implementationReport\)/u);
  assert.match(orchestrator, /applyInitialSelectedIssueDisposition\(implementationReport\)/u);
  // Selected issue and backlog work must never take the matrix not_attempted
  // no-change early return: the exact early-return condition excludes both
  // selections so an already_fixed integration reaches the selection
  // disposition handlers and the docs-only completion path.
  assert.match(
    orchestrator,
    /if \(\s*integration\.cycle_report\.integrated_candidate\?\.head_sha === baseSha && !workSelection\.issueJob &&\s*!workSelection\.backlogEntry\s*\) \{\s*await writeMatrixDelivery\(\{\s*status: "not_attempted",[\s\S]*?All accepted matrix findings were already fixed at the immutable base/u,
    "the matrix no-change early return must exclude selected issue and backlog work",
  );
  assert.match(orchestrator, /ensureIssuePullRequestForDevelopmentPush/u);
  assert.match(orchestrator, /Matrix issue delivery did not produce an issue pull request record/u);
  assert.match(orchestrator, /Matrix issue pull request record does not match the immutable candidate/u);
  assert.match(orchestrator, /Matrix issue pull request is missing or ambiguous/u);
  // The converge job installs the pre-push gate that invokes the helper script.
  assert.match(converge, /Install issue-delivery development-push gate/u);
  assert.match(converge, /scripts\/sentinel\/issue-pr-pre-push\.ts/u);
  assert.match(converge, /SENTINEL_GIT_CHECKPOINT_LEASE_SHA/u);
});

Deno.test("matrix cell retry evidence is encrypted from the cell report path", () => {
  assert.match(repair, /sentinel-cell-report\.json\.retry-evidence/u);
  assert.match(repair, /mkdir -p "\.sentinel\/reports\/matrix\/\$\{CELL_ID\}"/u);
  assert.match(repair, /cp -a "\$RUNNER_TEMP\/sentinel-cell-report\.json\.retry-evidence"/u);
  assert.doesNotMatch(repair, /upload-artifact@[\w-]+[\s\S]*?path:\s*\n\s+\.sentinel\/(?:raw-logs|reports)(?:\s|$)/u);
});

Deno.test("focused validation evidence is staged before cell evidence encryption and absent matches are skipped", () => {
  const encryptStart = repair.indexOf("      - name: Encrypt cell report");
  const encryptEnd = repair.indexOf("      - name: Upload encrypted cell evidence", encryptStart);
  assert.ok(encryptStart >= 0 && encryptEnd > encryptStart, "the Encrypt cell report step must exist");
  const encryptStep = repair.slice(encryptStart, encryptEnd);
  // The per-invocation candidate validation reports (and their binary
  // stdout/stderr sidecars) are copied beside the cell report before the
  // encrypt-artifacts invocation; the compgen guard skips the copy when no
  // validation evidence exists.
  assert.match(encryptStep, /compgen -G "\$RUNNER_TEMP\/sentinel-cell-validation-\*\.json\*"/u);
  assert.match(
    encryptStep,
    /cp "\$RUNNER_TEMP"\/sentinel-cell-validation-\*\.json\* "\.sentinel\/reports\/matrix\/\$\{CELL_ID\}\/"/u,
  );
  const copyIndex = encryptStep.indexOf("sentinel-cell-validation-");
  const encryptIndex = encryptStep.indexOf("scripts/sentinel/encrypt-artifacts.ts");
  assert.ok(copyIndex >= 0, "the Encrypt cell report step must stage focused validation evidence");
  assert.ok(
    encryptIndex > copyIndex,
    "focused validation evidence must be copied before encrypt-artifacts runs",
  );
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

Deno.test("isolated jobs lift the unprivileged userns restriction and probe network isolation before validation", () => {
  const installStep = (document: string, name: string, nextName: string): string => {
    const start = document.indexOf(`      - name: ${name}`);
    assert.ok(start >= 0, `${name} step is missing`);
    const end = document.indexOf(`      - name: ${nextName}`, start + 1);
    assert.ok(end > start, `${name} step is missing its following boundary`);
    return document.slice(start, end);
  };
  const sysctl = "sudo sysctl -w kernel.apparmor_restrict_unprivileged_userns=0";
  const fallback = "|| echo 'apparmor userns knob absent — the functional probe decides'";
  const probe =
    "bwrap --die-with-parent --new-session --unshare-net --unshare-pid --unshare-ipc --unshare-uts --ro-bind / / --dev /dev --proc /proc --tmpfs /tmp -- /bin/true";
  const cellInstall = installStep(
    repair,
    "Install isolated cell prerequisites",
    "Run immutable Luna matrix cell",
  );
  const agentInstall = installStep(
    converge,
    "Install isolated-agent prerequisites",
    "Prime locked Deno dependency cache",
  );
  for (
    const [label, step, document, validationStep] of [
      ["repair", cellInstall, repair, "Run immutable Luna matrix cell"],
      ["converge", agentInstall, converge, "Run canonical local Sentinel verification"],
    ] as const
  ) {
    const bubblewrapIndex = step.indexOf("sudo apt-get install --yes bubblewrap");
    const sysctlIndex = step.indexOf(sysctl);
    const fallbackIndex = step.indexOf(fallback);
    const probeIndex = step.indexOf(probe);
    assert.ok(
      bubblewrapIndex >= 0 && sysctlIndex > bubblewrapIndex && fallbackIndex > sysctlIndex &&
        probeIndex > sysctlIndex,
      `${label} must install bubblewrap, lift the AppArmor userns restriction, and probe network isolation in order`,
    );
    assert.ok(
      document.indexOf(probe) < document.indexOf(`      - name: ${validationStep}`),
      `${label} must prove network isolation before validation`,
    );
  }
  assert.equal(
    workflow.split(probe).length - 1,
    2,
    "the functional network-isolation probe must exist exactly once per isolated job",
  );
  assert.ok(
    !prepare.includes(sysctl) && !prepare.includes(probe),
    "the userns prerequisite and probe must stay scoped to the isolated repair and converge jobs",
  );
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
  // The embedded prerequisite selector and runtime selection consume the same
  // authoritative sentinel/recovery-state snapshot, bound to the exact
  // prepared convergence record, so the hint and runtime never disagree.
  assert.match(selector, /readGitHubSentinelRecoveryLedger\(/u);
  assert.match(selector, /selectNextReviewBacklogEntry\(backlog, recovery\)/u);
  assert.match(selector, /selectNextGitHubIssueJobSelection\(github, repository, ledger, recovery\)/u);
  assert.match(selector, /continuation_record: continuationRecord/u);
  assert.match(orchestrator, /readGitHubSentinelRecoveryLedger\(\{ token: githubToken, repository \}\)/u);
  assert.match(orchestrator, /continuation_record: matrixConvergencePreparedRecovery/u);
  assert.match(orchestrator, /eligibility\.available/u);

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

Deno.test("the focused cell validation runner is carried in the runMatrixCell options object", async () => {
  const counterStart = repair.indexOf("let validationAttempt = 0");
  assert.ok(counterStart >= 0, "repair is missing the validation attempt counter");
  const callStart = repair.indexOf("const report = await runMatrixCell({");
  assert.ok(callStart >= 0, "repair is missing the runMatrixCell call");
  assert.ok(
    counterStart < callStart,
    "the validation attempt counter must precede the runMatrixCell call",
  );
  const snippetEnd = repair.indexOf("await Deno.writeTextFile", callStart);
  assert.ok(
    snippetEnd > callStart,
    "the runMatrixCell call must be bounded by the following cell status write",
  );
  const snippet = repair.slice(counterStart, snippetEnd).trim();
  // The snippet is executed as plain JavaScript by new Function, so the
  // extractable workflow body must stay free of TypeScript-only annotations.
  assert.doesNotMatch(snippet, /\s(?:as|satisfies|implements)\s/u);

  // Execute the exact repair snippet with harmless stand-ins: runMatrixCell
  // returns its captured options, so the test can prove the focused validation
  // runner is carried in the options object under the `validation` key that
  // runMatrixCell consults (`options.validation ?? dependencies.validate`).
  const capturedOptions: Record<string, unknown> = {};
  const runMatrixCell = (options: Record<string, unknown>): unknown => {
    Object.assign(capturedOptions, options);
    return options;
  };
  const validationCalls: Array<Record<string, unknown>> = [];
  const snippetSecret = "fixture-artifact-key-12345678";
  const snippetFailure = {
    phase: "type_check",
    exit_code: 42,
    stdout_excerpt: `stdout leak ${snippetSecret} ${"x".repeat(2000)}`,
    stderr_excerpt: `stderr leak ${snippetSecret}`,
  };
  class CandidateValidationError extends Error {
    readonly failure: typeof snippetFailure;
    constructor(failure: typeof snippetFailure) {
      super("Candidate validation failed");
      this.name = "CandidateValidationError";
      this.failure = failure;
    }
  }
  let validationMode: "success" | "generic" | "typed" = "success";
  const runCandidateValidation = (candidate: Record<string, unknown>): void => {
    validationCalls.push(candidate);
    if (validationMode === "generic") throw new Error("candidate validation exploded");
    if (validationMode === "typed") throw new CandidateValidationError(snippetFailure);
  };
  const stdoutLines: unknown[] = [];
  const consoleStub = {
    log: (...values: unknown[]): void => {
      stdoutLines.push(...values);
    },
  };
  const executeRepair = new Function(
    "runMatrixCell",
    "runCandidateValidation",
    "CandidateValidationError",
    "console",
    "Deno",
    "env",
    "root",
    "plan",
    "cell",
    "authSlots",
    "sensitiveValues",
    "triageValue",
    "workSelection",
    `return (async () => {${snippet}; return report;})();`,
  ) as (...args: unknown[]) => Promise<Record<string, unknown>>;
  const report = await executeRepair(
    runMatrixCell,
    runCandidateValidation,
    CandidateValidationError,
    consoleStub,
    { cwd: () => "/repo/checkout" },
    (name: string): string => (name === "DENO_DIR" ? "/home/runner/.deno-cache" : ""),
    "/tmp/runner",
    { cells: [] },
    { cell_id: "cell-1", finding_ids: [] },
    {},
    [snippetSecret],
    { findings: [] },
    null,
  );
  assert.equal(report.checkoutPath, "/repo/checkout");

  const validation = capturedOptions.validation as
    | Validator
    | undefined;
  assert.ok(
    typeof validation === "function",
    "runMatrixCell options must carry the focused validation runner under `validation`",
  );
  const runValidation = validation as Validator;

  const success = await runValidation({ checkoutPath: "/repo/checkout" });
  assert.deepEqual(validationCalls, [
    {
      cwd: "/repo/checkout",
      reportPath: "/tmp/runner/sentinel-cell-validation-1.json",
      privateDir: "/tmp/runner",
      denoDirectory: "/home/runner/.deno-cache",
    },
  ]);
  assert.deepEqual(success, {
    passed: true,
    checks: [{
      name: "candidate-validation",
      passed: true,
      detail: "existing candidate validation passed",
    }],
  });

  validationMode = "generic";
  const failure = await runValidation({ checkoutPath: "/repo/checkout" });
  assert.deepEqual(validationCalls, [
    {
      cwd: "/repo/checkout",
      reportPath: "/tmp/runner/sentinel-cell-validation-1.json",
      privateDir: "/tmp/runner",
      denoDirectory: "/home/runner/.deno-cache",
    },
    {
      cwd: "/repo/checkout",
      reportPath: "/tmp/runner/sentinel-cell-validation-2.json",
      privateDir: "/tmp/runner",
      denoDirectory: "/home/runner/.deno-cache",
    },
  ]);
  assert.deepEqual(failure, {
    passed: false,
    checks: [{
      name: "candidate-validation",
      passed: false,
      detail: "candidate validation exploded",
    }],
  });
  assert.equal(stdoutLines.length, 0, "generic failures emit no public validation stdout");

  validationMode = "typed";
  const typedFailure = await runValidation({ checkoutPath: "/repo/checkout" });
  assert.equal(
    validationCalls[2]!.reportPath,
    "/tmp/runner/sentinel-cell-validation-3.json",
  );
  assert.notEqual(validationCalls[1]!.reportPath, validationCalls[2]!.reportPath);
  assert.equal(typedFailure.passed, false);
  const typedDetail = JSON.parse(typedFailure.checks[0]!.detail) as Record<string, unknown>;
  assert.deepEqual(
    Object.keys(typedDetail).sort(),
    ["exit_code", "phase", "stderr_excerpt", "stdout_excerpt"],
    "the typed failure detail must be bounded to phase, exit code, and redacted excerpts",
  );
  assert.equal(typedDetail.phase, "type_check");
  assert.equal(typedDetail.exit_code, 42);
  const stdoutExcerpt = typedDetail.stdout_excerpt as string;
  const stderrExcerpt = typedDetail.stderr_excerpt as string;
  // Secrets are removed before the 1500-character bound so no excerpt can
  // survive redaction as a partial secret prefix.
  assert.equal(stdoutExcerpt.length, 1500);
  assert.equal(stdoutExcerpt.includes(snippetSecret), false);
  assert.equal(stdoutExcerpt.includes("[REDACTED]"), true);
  assert.equal(stdoutExcerpt.includes("stdout leak [REDACTED]"), true);
  assert.equal(stderrExcerpt.includes(snippetSecret), false);
  assert.equal(stderrExcerpt.includes("[REDACTED]"), true);
  // Public stdout carries only the fixed phase and exit-code JSON: no report
  // path, key, or raw excerpt may be logged.
  assert.equal(stdoutLines.length, 1, "typed failures emit exactly one public stdout line");
  assert.deepEqual(JSON.parse(stdoutLines[0] as string), { phase: "type_check", exit_code: 42 });
  assert.equal(stdoutLines.every((line) => !JSON.stringify(line).includes(snippetSecret)), true);
});
