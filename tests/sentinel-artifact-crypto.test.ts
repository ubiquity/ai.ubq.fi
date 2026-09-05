import {
  decodeSentinelArtifactKey,
  decryptSentinelArtifact,
  encryptSentinelArtifact,
} from "../scripts/sentinel/artifact-crypto.ts";
import { encryptAndVerifyGeneratedEvidence, scrubGeneratedEvidence } from "../scripts/sentinel/encrypt-artifacts.ts";

const requiredFileSystemPermissions = await Promise.all([
  Deno.permissions.query({ name: "read" }),
  Deno.permissions.query({ name: "write" }),
]);
const fileSystemTestsUnavailable = requiredFileSystemPermissions.some(
  (permission) => permission.state !== "granted",
);

const assert: (condition: unknown, message: string) => asserts condition = (
  condition,
  message,
) => {
  if (!condition) throw new Error(message);
};

const equalBytes = (left: Uint8Array, right: Uint8Array): boolean =>
  left.byteLength === right.byteLength &&
  left.every((byte, index) => byte === right[index]);

const assertRejects = async (
  operation: () => Promise<unknown>,
): Promise<void> => {
  let rejected = false;
  try {
    await operation();
  } catch {
    rejected = true;
  }
  assert(rejected, "Expected operation to reject");
};

Deno.test("Sentinel evidence is authenticated ciphertext and round-trips exact bytes", async () => {
  const key = new Uint8Array(32).fill(7);
  const marker = "provider-input-must-never-appear-in-public-artifacts";
  const files = [
    { path: "raw-logs/deno.jsonl", bytes: new TextEncoder().encode(marker) },
    { path: "reports/A.json", bytes: new TextEncoder().encode("uppercase") },
    { path: "reports/empty.txt", bytes: new Uint8Array() },
    { path: "reports/a.json", bytes: new TextEncoder().encode("lowercase") },
    { path: "reports/triage.json", bytes: new Uint8Array([0, 1, 2, 255]) },
  ];
  const encrypted = await encryptSentinelArtifact(
    files,
    key,
    new Uint8Array(12).fill(9),
  );
  assert(
    !new TextDecoder().decode(encrypted).includes(marker),
    "Ciphertext exposed plaintext evidence",
  );
  const decrypted = await decryptSentinelArtifact(encrypted, key);
  assert(decrypted.length === files.length, "Decrypted file count differs");
  for (const decryptedFile of decrypted) {
    const original = files.find((file) => file.path === decryptedFile.path);
    assert(original, "Decrypted path differs");
    assert(
      equalBytes(decryptedFile.bytes, original.bytes),
      "Decrypted bytes differ",
    );
    decryptedFile.bytes.fill(0);
  }
  const wrongKey = new Uint8Array(32).fill(8);
  await assertRejects(() => decryptSentinelArtifact(encrypted, wrongKey));
  const tampered = encrypted.slice();
  tampered[tampered.length - 2] ^= 1;
  await assertRejects(() => decryptSentinelArtifact(tampered, key));
  encrypted.fill(0);
  tampered.fill(0);
  key.fill(0);
  wrongKey.fill(0);
});

Deno.test({
  name: "Sentinel encryption never removes a pre-existing artifact",
  ignore: fileSystemTestsUnavailable,
  async fn() {
    const root = await Deno.makeTempDir({
      prefix: "sentinel-artifact-existing-",
    });
    const key = new Uint8Array(32).fill(13);
    const existing = new TextEncoder().encode(
      "pre-existing ciphertext fixture",
    );
    try {
      await Deno.mkdir(`${root}/raw-logs`, { recursive: true });
      await Deno.mkdir(`${root}/encrypted`, { recursive: true });
      await Deno.writeTextFile(`${root}/raw-logs/deno.jsonl`, "new evidence");
      const output = `${root}/encrypted/sentinel-evidence-v1.json`;
      await Deno.writeFile(output, existing);
      await assertRejects(() => encryptAndVerifyGeneratedEvidence(root, key));
      assert(
        equalBytes(await Deno.readFile(output), existing),
        "Pre-existing artifact was changed or removed",
      );
      assert(
        await Deno.readTextFile(`${root}/raw-logs/deno.jsonl`) === "new evidence",
        "Rejected encryption removed plaintext evidence",
      );
    } finally {
      key.fill(0);
      existing.fill(0);
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test("Sentinel artifact key requires exactly 32 standard-base64 bytes", () => {
  const encoded = btoa(String.fromCharCode(...new Uint8Array(32).fill(11)));
  const decoded = decodeSentinelArtifactKey(encoded);
  assert(
    decoded.byteLength === 32 && decoded.every((byte) => byte === 11),
    "Decoded key differs",
  );
  decoded.fill(0);
  let rejected = false;
  try {
    decodeSentinelArtifactKey(btoa("short"));
  } catch {
    rejected = true;
  }
  assert(rejected, "Short key was accepted");
});

Deno.test({
  name: "Sentinel scrubs generated plaintext only after verified encryption is available for upload",
  ignore: fileSystemTestsUnavailable,
  async fn() {
    const root = await Deno.makeTempDir({ prefix: "sentinel-artifact-test-" });
    const key = new Uint8Array(32).fill(12);
    try {
      await Deno.mkdir(`${root}/raw-logs`, { recursive: true });
      await Deno.mkdir(`${root}/reports`, { recursive: true });
      await Deno.mkdir(`${root}/reports/failed-implementation-candidate/files`, { recursive: true });
      await Deno.mkdir(`${root}/candidate-worktree`, { recursive: true });
      await Deno.mkdir(`${root}/private`, { recursive: true });
      const raw = new TextEncoder().encode("raw provider log fixture");
      const report = new TextEncoder().encode('{"status":"observed"}');
      const candidatePayload = new Uint8Array([0, 1, 254, 255]);
      await Deno.writeFile(`${root}/raw-logs/deno.jsonl`, raw);
      await Deno.writeFile(`${root}/reports/triage.json`, report);
      await Deno.writeFile(`${root}/reports/failed-implementation-candidate/files/0000.bin`, candidatePayload);
      const result = await encryptAndVerifyGeneratedEvidence(root, key);
      assert(result.fileCount === 3, "Unexpected encrypted file count");
      await Deno.stat(`${root}/raw-logs`);
      await Deno.stat(`${root}/reports`);
      await Deno.stat(`${root}/candidate-worktree`);
      await Deno.stat(`${root}/private`);
      const encrypted = await Deno.readFile(result.outputPath);
      const decrypted = await decryptSentinelArtifact(encrypted, key);
      assert(
        decrypted[0]!.path === "raw-logs/deno.jsonl",
        "Raw-log path missing",
      );
      assert(equalBytes(decrypted[0]!.bytes, raw), "Raw-log bytes differ");
      assert(
        decrypted[1]!.path === "reports/failed-implementation-candidate/files/0000.bin",
        "Failed candidate payload path missing",
      );
      assert(equalBytes(decrypted[1]!.bytes, candidatePayload), "Failed candidate payload bytes differ");
      assert(
        decrypted[2]!.path === "reports/triage.json",
        "Report path missing",
      );
      assert(equalBytes(decrypted[2]!.bytes, report), "Report bytes differ");
      for (const file of decrypted) file.bytes.fill(0);
      await scrubGeneratedEvidence(root);
      await assertRejects(() => Deno.stat(`${root}/raw-logs`));
      await assertRejects(() => Deno.stat(`${root}/reports`));
      await assertRejects(() => Deno.stat(`${root}/candidate-worktree`));
      await assertRejects(() => Deno.stat(`${root}/private`));
      await Deno.stat(result.outputPath);
      encrypted.fill(0);
      raw.fill(0);
      report.fill(0);
      candidatePayload.fill(0);
    } finally {
      key.fill(0);
      await Deno.remove(root, { recursive: true });
    }
  },
});

Deno.test({
  name: "public Sentinel workflow uploads only the verified evidence ciphertext",
  ignore: fileSystemTestsUnavailable,
  async fn() {
    const workflow = await Deno.readTextFile(
      ".github/workflows/provider-sentinel.yml",
    );
    assert(
      workflow.includes("runs-on: ubuntu-22.04"),
      "Sentinel must use the Bubblewrap-compatible runner",
    );
    assert(
      workflow.includes("public|private|internal"),
      "Public repositories must use the ciphertext artifact policy",
    );
    assert(
      workflow.includes(
        "path: .sentinel/encrypted/sentinel-evidence-v1.json",
      ),
      "Evidence upload must name the exact ciphertext envelope",
    );
    assert(
      !/Upload encrypted Sentinel evidence[\s\S]*?path:\s*\|[\s\S]*?\.sentinel\/(?:raw-logs|reports)/u
        .test(workflow),
      "Evidence upload must never use a plaintext directory",
    );
    const encryptionPosition = workflow.indexOf("- name: Encrypt and verify Sentinel evidence");
    const uploadPosition = workflow.indexOf("- name: Upload encrypted Sentinel evidence");
    const scrubPosition = workflow.indexOf("- name: Scrub Sentinel plaintext after evidence upload");
    assert(
      encryptionPosition >= 0 && uploadPosition > encryptionPosition && scrubPosition > uploadPosition,
      "Verified ciphertext must upload before generated Sentinel plaintext is scrubbed",
    );
    const scrubStep = workflow.slice(scrubPosition, workflow.indexOf("\n      - name:", scrubPosition + 1));
    assert(
      workflow.slice(uploadPosition, scrubPosition).includes("id: sentinel-evidence-upload") &&
        scrubStep.includes("steps.sentinel-evidence-upload.outcome == 'success'") &&
        scrubStep.includes("scripts/sentinel/scrub-artifacts.ts"),
      "Plaintext scrubbing must require a successful durable evidence upload",
    );
  },
});

Deno.test({
  name: "Sentinel repair workflow uses supported concurrency and durable incident retry",
  ignore: fileSystemTestsUnavailable,
  async fn() {
    const workflow = await Deno.readTextFile(".github/workflows/provider-sentinel.yml");
    assert(!/^\s+queue:/mu.test(workflow), "Sentinel must use only supported GitHub concurrency keys");
    for (
      const path of [
        ".github/workflows/deno-deploy.yml",
        ".github/workflows/sentinel-revision-control.yml",
      ]
    ) {
      const workflow = await Deno.readTextFile(path);
      assert(!/^\s+queue:/mu.test(workflow), `${path} must keep its existing deployment concurrency policy`);
    }
  },
});

Deno.test({
  name: "Sentinel durably refreshes exclusive Codex auth before every work selection",
  ignore: fileSystemTestsUnavailable,
  async fn() {
    const workflow = await Deno.readTextFile(".github/workflows/provider-sentinel.yml");
    const orchestrator = await Deno.readTextFile("scripts/sentinel/main.ts");
    const maintenance = await Deno.readTextFile("scripts/sentinel/auth-maintenance.ts");
    const authState = await Deno.readTextFile("scripts/sentinel/auth-state.ts");
    assert(workflow.includes("SENTINEL_CODEX_AUTH_STATE_KEY"), "Auth state must use its own encryption key");
    assert(
      workflow.includes("SENTINEL_CODEX_AUTH_GENERATION"),
      "Auth state reseeding must use an explicit generation",
    );
    assert(
      !workflow.includes("secrets.SENTINEL_CODEX_AUTH_SLOT_1_B64 || secrets.CODEX_AUTH_JSON_B64"),
      "Sentinel auth must never share the gateway auth seed",
    );
    const restore = workflow.indexOf("Restore or bootstrap encrypted Codex auth state");
    const maintain = workflow.indexOf("Let pinned Codex maintain its auth files");
    const seal = workflow.indexOf("Seal refreshed Codex auth state");
    const upload = workflow.indexOf("Upload encrypted Codex auth state");
    const readiness = workflow.indexOf("Probe Codex auth readiness");
    const workSelection = workflow.indexOf("Select agent work");
    assert(
      restore >= 0 && restore < maintain && maintain < seal && seal < upload && upload < readiness &&
        readiness < workSelection,
      "Durable auth persistence and readiness must precede even a quiet hourly work selection",
    );
    assert(
      workflow.includes("steps.auth-state-upload.outcome == 'success'") &&
        workflow.includes("Enforce durable Codex auth readiness") &&
        /Probe Codex auth readiness[\s\S]*?--allow-write="\$RUNNER_TEMP"[\s\S]*?auth-state\.ts[\s\S]*?probe/u
          .test(workflow),
      "The workflow must gate agents on a successful auth-state upload and readiness probe",
    );
    const maskMaintained = workflow.indexOf("Mask maintained Codex auth state");
    const maintenanceDiagnostics = workflow.slice(maintain, maskMaintained);
    for (
      const safeField of [
        ".due",
        ".invoked",
        ".duplicateAccountSkipped",
        ".rpcSucceeded",
        ".managedAccountAvailable",
        ".commandCode",
        ".timedOut",
        ".outputExceeded",
        ".stdoutBytes",
        ".stderrBytes",
        ".stateChanged",
        ".readyForMaintenanceWindow",
      ]
    ) {
      assert(
        maintenanceDiagnostics.includes(safeField),
        `Auth maintenance summary is missing safe disposition field ${safeField}`,
      );
    }
    assert(
      maintenanceDiagnostics.includes('echo "### Codex auth maintenance"') &&
        maintenanceDiagnostics.includes('| tee -a "$GITHUB_STEP_SUMMARY"'),
      "Auth maintenance must log and publish its validated categorical and numeric dispositions",
    );
    for (const forbidden of ["auth.json", "id_token", "access_token", "refresh_token", "account_id", "sha256"]) {
      assert(
        !maintenanceDiagnostics.includes(forbidden),
        `Auth maintenance diagnostics must not expose ${forbidden}`,
      );
    }
    const authPreflight = workflow.indexOf("Enforce durable Codex auth readiness");
    const authDiagnostics = workflow.slice(authPreflight, workSelection);
    for (
      const safeOutput of [
        "steps.auth-state-readiness.outputs.auth_usable",
        "steps.auth-state-readiness.outputs.selected_slot",
        "steps.auth-state-readiness.outputs.slot_1_code",
        "steps.auth-state-readiness.outputs.slot_1_http_status",
        "steps.auth-state-readiness.outputs.slot_1_headroom_percent",
        "steps.auth-state-readiness.outputs.slot_2_code",
        "steps.auth-state-readiness.outputs.slot_2_http_status",
        "steps.auth-state-readiness.outputs.slot_2_headroom_percent",
      ]
    ) {
      assert(authDiagnostics.includes(safeOutput), `Auth gate is missing safe probe output ${safeOutput}`);
    }
    assert(
      authDiagnostics.includes("safe_probe_code()") &&
        authDiagnostics.includes("### Codex auth durability preflight") &&
        authDiagnostics.includes("Probe slot 1: code=") &&
        authDiagnostics.includes("Probe slot 2: code=") &&
        authDiagnostics.includes("::error::Sentinel Codex auth probe failed:") &&
        authDiagnostics.includes('if [ "$failed" = "true" ]'),
      "The failing auth gate must publish allowlisted per-slot diagnostics before agent work",
    );
    assert(
      authState.includes("Deno.makeTempDir({") && authState.includes("dir: runnerTemp"),
      "Artifact-backed auth restore must unpack only inside the authorized private runner directory",
    );
    assert(
      workflow.includes("$RUNNER_TEMP/sentinel-codex-auth-state") &&
        !workflow.includes(".sentinel/sentinel-codex-auth-state"),
      "Plaintext auth state must remain outside every evidence path",
    );
    const classifyDeferral = workflow.indexOf("Classify incident infrastructure deferral");
    const runSentinel = workflow.indexOf("Run Provider Sentinel");
    const deferIncident = workflow.indexOf("Defer incident after infrastructure failure");
    const acknowledgeIncident = workflow.indexOf("Acknowledge completed incident");
    const deferralStep = workflow.slice(deferIncident, acknowledgeIncident);
    assert(
      runSentinel >= 0 && runSentinel < classifyDeferral && classifyDeferral < deferIncident &&
        deferIncident < acknowledgeIncident,
      "Incident infrastructure deferral must classify the exact run and precede acknowledgement",
    );
    for (
      const required of [
        "inputs.sentinel_mode == 'incident'",
        "steps.incident-claim.outcome == 'success'",
        "steps.sentinel-run.outcome == 'skipped'",
        "steps.sentinel-run.outcome == 'success' && failure()",
        "codex_auth_preflight_failed",
        "sentinel_infrastructure_preflight_failed",
        '--arg workflow_run_id "$GITHUB_RUN_ID"',
        '--arg ack_nonce "$SENTINEL_INCIDENT_ACK_NONCE"',
        'if [ "$curl_status" = "0" ] && [ "$status" = "204" ]',
        "for request_attempt in 1 2 3",
        '[[ "$status" =~ ^5[0-9][0-9]$ ]]',
        "x-sentinel-incident-disposition",
        "dead_letter)",
        "/admin/sentinel/incidents/defer",
      ]
    ) {
      assert(deferralStep.includes(required), `Incident deferral is missing its exact ${required} contract`);
    }
    assert(
      orchestrator.includes('requiredEnvironment("SENTINEL_CODEX_AUTH_STATE_DIR")') &&
        !orchestrator.includes('optionalEnvironment("SENTINEL_CODEX_AUTH_SLOT_1_B64")'),
      "The orchestrator must read only the prepared private files",
    );
    assert(
      maintenance.includes('"app-server"') &&
        maintenance.includes(`'cli_auth_credentials_store="file"'`) &&
        maintenance.includes('await send({ method: "initialized" });') &&
        maintenance.includes('method: "account/read"') &&
        maintenance.includes("params: { refreshToken: true }") &&
        maintenance.includes("CODEX_HOME: stage.directory") &&
        maintenance.includes("after.tokens.id_token !== before.tokens.id_token") &&
        maintenance.includes('requiredExecutableEnvironment("SENTINEL_CODEX_AUTH_EXECUTABLE")') &&
        workflow.includes('resolveFromCodex.resolve("@openai/codex-linux-x64/package.json")') &&
        workflow.includes("vendor/x86_64-unknown-linux-musl/bin/codex") &&
        workflow.includes('--allow-run="$SENTINEL_CODEX_AUTH_EXECUTABLE"'),
      "Pinned native Codex must explicitly refresh staged file auth and preserve its complete rewrite",
    );
  },
});

Deno.test({
  name: "Sentinel uses failure events with durable retry and no resident watchdog",
  ignore: fileSystemTestsUnavailable,
  async fn() {
    let watchdogExists = true;
    try {
      await Deno.stat(".github/workflows/provider-sentinel-watchdog.yml");
    } catch (error) {
      if (!(error instanceof Deno.errors.NotFound)) throw error;
      watchdogExists = false;
    }
    assert(!watchdogExists, "The resident watchdog workflow must be removed");
    const workflow = await Deno.readTextFile(".github/workflows/provider-sentinel.yml");
    const orchestrator = await Deno.readTextFile("scripts/sentinel/main.ts");
    const validation = await Deno.readTextFile("scripts/sentinel/validation.ts");
    const issueDelivery = await Deno.readTextFile("scripts/sentinel/issue-delivery.ts");
    const issuePushGate = await Deno.readTextFile("scripts/sentinel/issue-pr-pre-push.ts");
    const issueReconciliation = await Deno.readTextFile("scripts/sentinel/issue-delivery-reconcile.ts");
    const server = await Deno.readTextFile("serve.ts");
    const deploy = await Deno.readTextFile(".github/workflows/deno-deploy.yml");
    for (
      const input of [
        "sentinel_mode:",
        "incident_id:",
        "incident_attempt:",
        "incident_start_ms:",
        "incident_ack_nonce:",
      ]
    ) {
      assert(workflow.includes(input), `Sentinel workflow is missing ${input}`);
    }
    const sentinelModeInput = workflow.slice(
      workflow.indexOf("      sentinel_mode:"),
      workflow.indexOf("      incident_id:"),
    );
    assert(
      sentinelModeInput.includes("        default: hourly"),
      "A standard manual dispatch must run the autonomous hourly work cycle",
    );
    assert(workflow.includes("github.actor_id == '319834869'"), "Incident mode must require the Sentinel App actor");
    assert(workflow.includes('- cron: "*/5 * * * *"'), "Sentinel evaluation must run every five minutes");
    assert(workflow.includes("mode=hourly"), "Scheduled runs must use hourly mode");
    assert(workflow.includes("inputs.sentinel_mode == 'hourly'"), "Maintainers must be able to start an hourly run");
    assert(workflow.includes("preview|hourly)"), "Manual hourly runs must select the hourly orchestrator mode");
    assert(workflow.includes("selectNextReviewBacklogEntry"), "Hourly runs must preflight eligible backlog work");
    assert(
      workflow.includes("selectNextGitHubIssueJobSelection"),
      "Hourly runs must preflight eligible GitHub issue and checkpoint work",
    );
    assert(
      workflow.includes("renderGitHubIssueJobHint(") && workflow.includes("issueSelection?.checkpoint ?? null"),
      "Hourly preflight must persist selected, empty, and retry-checkpoint hints",
    );
    assert(
      orchestrator.includes("githubIssueJobMatchesHint(") && orchestrator.includes("selectedIssueCheckpoint") &&
        orchestrator.includes("hourly_deferred_github_issue_changed"),
      "Hourly runtime must defer when the bound GitHub issue or checkpoint selection changes",
    );
    assert(
      orchestrator.includes("GITHUB_REPOSITORY: repository"),
      "Sentinel git network environment must carry the repository identity for the push gate",
    );
    assert(
      /^\s+issues: write$/mu.test(workflow),
      "Sentinel must be able to post evidence and close delivered GitHub issues",
    );
    assert(
      /^\s+pull-requests: write$/mu.test(workflow),
      "Sentinel must be able to open and merge delivery pull requests",
    );
    assert(
      workflow.includes('--allow-read="$repository_root"'),
      "The issue-delivery push gate must read the workspace root (Deno.realPath)",
    );
    assert(
      workflow.includes("git show origin/development:docs/sentinel-review-backlog.md"),
      "Hourly agent setup must inspect the current development backlog",
    );
    assert(
      workflow.includes("git show origin/development:docs/sentinel-issue-jobs.md"),
      "Hourly agent setup must inspect the current issue-job ledger",
    );
    assert(
      workflow.includes('echo "SENTINEL_BACKLOG_HINT_SHA=$hint_sha" >> "$GITHUB_ENV"'),
      "Hourly work must bind the orchestrator to the prerequisite hint revision",
    );
    assert(
      workflow.includes("installing agent prerequisites conservatively"),
      "Backlog hint failures must fail toward installing the agent",
    );
    const nonRuntimeStart = orchestrator.indexOf(
      "if (selectedBacklogState.disposition !== null && !selectedBacklogState.continueToRuntimeValidation)",
    );
    const nonRuntimeEnd = orchestrator.indexOf("const aggregateCandidatePaths", nonRuntimeStart);
    assert(
      nonRuntimeStart >= 0 && nonRuntimeEnd > nonRuntimeStart,
      "Non-runtime backlog completion must have a bounded early-return lane",
    );
    const nonRuntimeLane = orchestrator.slice(nonRuntimeStart, nonRuntimeEnd);
    assert(
      nonRuntimeLane.includes("HEAD:${SENTINEL_POLICY.developmentRef}") &&
        nonRuntimeLane.includes('"development_docs_only_backlog_already_fixed"') &&
        nonRuntimeLane.includes('"development_docs_only_manual_required"'),
      "Non-runtime backlog completion must persist either trusted documentation disposition",
    );
    assert(
      nonRuntimeLane.includes("runDocumentationValidation({") && !nonRuntimeLane.includes("runCandidateValidation({"),
      "Non-runtime backlog completion must use scoped documentation validation instead of the runtime suite",
    );
    for (
      const forbidden of ["pushTemporaryCandidate", "dispatchAndResolveRevision", "dispatchSerializedPromotion"]
    ) {
      assert(!nonRuntimeLane.includes(forbidden), `Non-runtime backlog completion must not call ${forbidden}`);
    }
    const historyGate = nonRuntimeLane.indexOf("if (currentHead !== baseSha)");
    const gitControlGate = nonRuntimeLane.indexOf("await assertGitControlStateUnchanged(gitControlState)");
    const sensitiveValueGate = nonRuntimeLane.indexOf("await assertImplementationFilesExcludeValues(");
    const gitleaksGate = nonRuntimeLane.indexOf("await scanCandidateWithGitleaks({");
    const snapshotPermit = nonRuntimeLane.indexOf("snapshotAllowed = true");
    const snapshotCapture = nonRuntimeLane.indexOf("await captureFailedCandidateSnapshot(");
    assert(
      historyGate >= 0 && gitControlGate > historyGate && sensitiveValueGate > gitControlGate &&
        gitleaksGate > sensitiveValueGate && snapshotPermit > gitleaksGate && snapshotCapture > snapshotPermit,
      "Non-runtime backlog failure evidence must pass history, Git-control, sensitive-value, and Gitleaks gates before snapshotting",
    );
    assert(
      nonRuntimeLane.includes("preserved: false") &&
        nonRuntimeLane.includes("safe candidate snapshot could not be preserved"),
      "Unsafe or failed non-runtime snapshots must not claim durable preservation",
    );
    const validationCommandStart = validation.indexOf("const runValidationCommand = async");
    const validationCommandEnd = validation.indexOf("export const runCandidateValidation", validationCommandStart);
    const validationCommand = validation.slice(validationCommandStart, validationCommandEnd);
    assert(
      validationCommand.includes('"--unshare-net"') &&
        validationCommand.includes('SENTINEL_GIT_WRAPPER_BYPASS: "1"'),
      "Network-isolated candidate validation must bypass only the workflow Git wrapper used by local fixtures",
    );
    const issueCompletionStart = orchestrator.indexOf(
      "const completeNonRuntimeGitHubIssueDisposition = async",
    );
    const issueCompletionEnd = orchestrator.indexOf("let implementationResult", issueCompletionStart);
    assert(
      issueCompletionStart >= 0 && issueCompletionEnd > issueCompletionStart,
      "Non-runtime GitHub issue completion must use one bounded helper",
    );
    const issueCompletionLane = orchestrator.slice(issueCompletionStart, issueCompletionEnd);
    assert(
      issueCompletionLane.includes("HEAD:${SENTINEL_POLICY.developmentRef}"),
      "Non-runtime GitHub issue completion must persist its trusted ledger change",
    );
    for (const forbidden of ["pushTemporaryCandidate", "dispatchAndResolveRevision", "dispatchSerializedPromotion"]) {
      assert(
        !issueCompletionLane.includes(forbidden),
        `Non-runtime GitHub issue completion must not call ${forbidden}`,
      );
    }
    const issueDispositionStart = orchestrator.indexOf(
      'if (selectedIssueState.disposition === "manual_required" || selectedIssueState.disposition === "retry_pending")',
    );
    const issueDispositionEnd = orchestrator.indexOf("const aggregateCandidatePaths", issueDispositionStart);
    assert(
      issueDispositionStart >= 0 && issueDispositionEnd > issueDispositionStart,
      "Non-runtime GitHub issue completion must have a bounded early-return lane",
    );
    const issueDispositionLane = orchestrator.slice(issueDispositionStart, issueDispositionEnd);
    assert(
      issueDispositionLane.includes("await completeNonRuntimeGitHubIssueDisposition()") &&
        issueDispositionLane.includes("return"),
      "Non-runtime GitHub issue dispositions must complete and return before candidate delivery",
    );
    for (const forbidden of ["pushTemporaryCandidate", "dispatchAndResolveRevision", "dispatchSerializedPromotion"]) {
      assert(
        !issueDispositionLane.includes(forbidden),
        `Non-runtime GitHub issue completion must not call ${forbidden}`,
      );
    }
    const retryPushStart = issuePushGate.indexOf('if (dispositionRecord?.disposition === "retry_pending")');
    const retryPushEnd = issuePushGate.indexOf(
      "if (!cycle.temporary_branch || !cycle.temporary_branch.startsWith",
      retryPushStart,
    );
    assert(
      retryPushStart >= 0 && retryPushEnd > retryPushStart,
      "Retry-pending issue pushes must return before ordinary candidate delivery",
    );
    const retryPushLane = issuePushGate.slice(retryPushStart, retryPushEnd);
    assert(
      retryPushLane.includes("validateRetryPendingDevelopmentPush({") && retryPushLane.includes("return null") &&
        !retryPushLane.includes("requireExactRemoteCandidateBranch("),
      "Retry-pending issue pushes must validate their exact atomic ref set and have no delivery record",
    );
    for (
      const forbidden of [
        "ensureRemoteCandidateBranch",
        "listPullRequests",
        "githubRequest",
        "ensureCandidateWorkflowValidation",
      ]
    ) {
      assert(!retryPushLane.includes(forbidden), `Retry-pending issue push must not call ${forbidden}`);
    }
    const retryReconciliationStart = issueReconciliation.indexOf(
      'if (dispositionRecord?.disposition === "retry_pending")',
    );
    const retryReconciliationEnd = issueReconciliation.indexOf(
      "const pullValue = await optionalJson",
      retryReconciliationStart,
    );
    assert(
      retryReconciliationStart >= 0 && retryReconciliationEnd > retryReconciliationStart,
      "Retry-pending reconciliation must finish before ordinary pull-request delivery",
    );
    const retryReconciliationLane = issueReconciliation.slice(
      retryReconciliationStart,
      retryReconciliationEnd,
    );
    assert(
      retryReconciliationLane.includes("return") && !retryReconciliationLane.includes("closeIssue(") &&
        !retryReconciliationLane.includes("mergeDeliveryPullRequest(") &&
        !retryReconciliationLane.includes("upsertComment("),
      "Retry-pending reconciliation must keep the issue open without a PR or evidence comment",
    );
    const manualPushStart = issuePushGate.indexOf(
      'dispositionRecord.phase === "native_review_exhausted"',
    );
    const manualPushEnd = issuePushGate.indexOf(
      "if (!cycle.temporary_branch || !cycle.temporary_branch.startsWith",
      manualPushStart,
    );
    assert(
      manualPushStart >= 0 && manualPushEnd > manualPushStart,
      "Native-review exhaustion must have a bounded no-delivery push lane",
    );
    const manualPushLane = issuePushGate.slice(manualPushStart, manualPushEnd);
    assert(
      manualPushLane.includes("validateManualRequiredDevelopmentPush({") &&
        manualPushLane.includes("workflowRunAttempt: input.workflowRunAttempt") &&
        manualPushLane.includes("return null"),
      "Native-review exhaustion must validate the exact run-attempt checkpoint before returning without delivery",
    );
    for (
      const forbidden of [
        "ensureRemoteCandidateBranch",
        "listPullRequests",
        "githubRequest",
        "ensureCandidateWorkflowValidation",
      ]
    ) {
      assert(!manualPushLane.includes(forbidden), `Manual checkpoint push must not call ${forbidden}`);
    }
    const manualReconciliationStart = issueReconciliation.indexOf(
      'dispositionRecord.phase === "native_review_exhausted"',
    );
    const manualReconciliationEnd = issueReconciliation.indexOf(
      "const pullValue = await optionalJson",
      manualReconciliationStart,
    );
    assert(
      manualReconciliationStart >= 0 && manualReconciliationEnd > manualReconciliationStart,
      "Native-review exhaustion must have a bounded no-delivery reconciliation lane",
    );
    const manualReconciliationLane = issueReconciliation.slice(
      manualReconciliationStart,
      manualReconciliationEnd,
    );
    assert(
      manualReconciliationLane.includes("validateNativeReviewExhaustedManualCheckpointReconciliation({") &&
        manualReconciliationLane.includes("workflowRunAttempt: input.workflowRunAttempt") &&
        manualReconciliationLane.includes("return") &&
        !manualReconciliationLane.includes("closeIssue(") &&
        !manualReconciliationLane.includes("mergeDeliveryPullRequest(") &&
        !manualReconciliationLane.includes("upsertComment("),
      "Native-review exhaustion reconciliation must retain the exact run attempt and leave the issue open",
    );
    const manualCycleParserStart = issueDelivery.indexOf("export const parseSentinelManualRequiredCycleReport =");
    const manualCycleParserEnd = issueDelivery.indexOf(
      "export const validateRetryPendingCheckpointPhaseBinding",
      manualCycleParserStart,
    );
    const manualCycleParserLane = issueDelivery.slice(manualCycleParserStart, manualCycleParserEnd);
    assert(
      manualCycleParserStart >= 0 && manualCycleParserEnd > manualCycleParserStart &&
        manualCycleParserLane.includes("runAttempt: number;") &&
        manualCycleParserLane.includes(
          "currentWorkflowCandidateBranch(cycle.temporary_branch, expected.runId, expected.runAttempt)",
        ),
      "Manual checkpoint cycles must bind their candidate branch to the exact workflow run attempt",
    );
    const issuePushWorkflowStart = workflow.indexOf("- name: Install issue-delivery development-push gate");
    const issuePushWorkflowEnd = workflow.indexOf("\n      - name:", issuePushWorkflowStart + 1);
    const issuePushWorkflowLane = workflow.slice(issuePushWorkflowStart, issuePushWorkflowEnd);
    assert(
      issuePushWorkflowStart >= 0 && issuePushWorkflowEnd > issuePushWorkflowStart &&
        issuePushWorkflowLane.includes('GITHUB_RUN_ATTEMPT="$GITHUB_RUN_ATTEMPT"') &&
        issuePushWorkflowLane.includes('"$repository_root/scripts/sentinel/issue-pr-pre-push.ts"'),
      "The isolated issue push gate must forward GITHUB_RUN_ATTEMPT to validate manual checkpoints",
    );
    const reconcileWorkflowStart = workflow.indexOf("- name: Reconcile GitHub issue delivery");
    const reconcileWorkflowEnd = workflow.indexOf("\n      - name:", reconcileWorkflowStart + 1);
    const reconcileWorkflowLane = workflow.slice(reconcileWorkflowStart, reconcileWorkflowEnd);
    assert(
      reconcileWorkflowStart >= 0 && reconcileWorkflowEnd > reconcileWorkflowStart &&
        /--allow-env=[^\r\n]*\bGITHUB_RUN_ATTEMPT\b/u.test(reconcileWorkflowLane),
      "GitHub issue reconciliation must receive GITHUB_RUN_ATTEMPT through its environment allowlist",
    );
    assert(
      !workflow.includes("github_issue_title") && !workflow.includes("github_issue_body"),
      "Untrusted GitHub issue text must not enter workflow environment or summary fields",
    );
    assert(
      workflow.includes('echo "- Selected GitHub issue: #$selected_issue"'),
      "The public cycle summary must identify a selected GitHub issue by its validated numeric identifier",
    );
    assert(
      workflow.includes("github-issue-production-outcome.json"),
      "The public cycle summary must distinguish the final GitHub issue production outcome from implementation status",
    );
    assert(
      orchestrator.includes("github-issue-production-outcome.json") &&
        orchestrator.includes('writeGitHubIssueProductionOutcome("kept", production.revision)') &&
        orchestrator.includes('writeGitHubIssueProductionOutcome("rolled_back", productionRevision)'),
      "GitHub issue evidence must record the final production outcome after the implementation disposition",
    );
    assert(
      orchestrator.includes("const gitEnvironment = gitNetworkEnvironment(githubToken, repository)"),
      "Manual backlog pushes must use the non-recursive workflow GITHUB_TOKEN",
    );
    assert(
      orchestrator.includes("error instanceof CandidateValidationError") &&
        orchestrator.includes("implementation_validation_fix_"),
      "Candidate validation failures must consume an existing implementation-review round",
    );
    assert(
      orchestrator.match(/runImplementationStageWithContinuation\(\{/gu)?.length === 3,
      "Every implementation, validation-fix, and replay-evaluation stage must share continuation policy",
    );
    const retryDeferStart = orchestrator.indexOf("const deferGitHubIssueImplementationFailure = async");
    const retryDeferEnd = orchestrator.indexOf(
      "const completeNonRuntimeGitHubIssueDisposition = async",
      retryDeferStart,
    );
    const retryDeferLane = orchestrator.slice(retryDeferStart, retryDeferEnd);
    const preservePosition = retryDeferLane.indexOf("await preserveFailedImplementation");
    const rollbackPosition = retryDeferLane.indexOf("await beforeDiscard?.()");
    const checkpointPosition = retryDeferLane.indexOf(
      "await prepareGitHubIssueCandidateCheckpoint(",
    );
    const checkpointStatePosition = retryDeferLane.indexOf("retryCheckpoint = checkpoint");
    const discardPosition = retryDeferLane.indexOf("await discardCandidateChanges");
    const dispositionPosition = retryDeferLane.indexOf("await writeSelectedIssueDisposition");
    assert(
      preservePosition >= 0 && rollbackPosition > preservePosition && checkpointPosition > rollbackPosition &&
        discardPosition > checkpointPosition,
      "Retry deferral must preserve evidence, restore preview state, prepare a checkpoint, and only then discard local changes",
    );
    assert(
      checkpointStatePosition > checkpointPosition && checkpointStatePosition < discardPosition &&
        checkpointStatePosition < dispositionPosition,
      "Retry deferral must synchronize nullable checkpoint state before discard and durable reports",
    );
    const checkpointPublishStart = orchestrator.indexOf("const prepareGitHubIssueCandidateCheckpoint = async");
    const failedPreservationStart = orchestrator.indexOf("const preserveFailedImplementation = async");
    const failedPreservationLane = orchestrator.slice(failedPreservationStart, checkpointPublishStart);
    assert(
      failedPreservationStart >= 0 && checkpointPublishStart > failedPreservationStart &&
        failedPreservationLane.includes("candidate could not be preserved safely") &&
        !failedPreservationLane.includes("pushTemporaryCandidate") &&
        !failedPreservationLane.includes("commitChanges("),
      "Failed implementation evidence must block discard when preservation fails and must never publish unvalidated refs",
    );
    const checkpointPublishEnd = orchestrator.indexOf(
      "const deferGitHubIssueImplementationFailure = async",
      checkpointPublishStart,
    );
    const checkpointPublishLane = orchestrator.slice(checkpointPublishStart, checkpointPublishEnd);
    const historyAssertionPosition = checkpointPublishLane.indexOf(
      "await assertAgentDidNotCommitOrSwitch(checkout, preInvocationSha, branch, gitControlState)",
    );
    const scopeValidationPosition = checkpointPublishLane.indexOf("await assertImplementationAgentScope(checkout)");
    const trustedRestorePosition = checkpointPublishLane.indexOf('"restore",');
    assert(
      historyAssertionPosition >= 0 && scopeValidationPosition > historyAssertionPosition &&
        trustedRestorePosition > scopeValidationPosition,
      "Retry checkpoint history and scope validation must run before trusted ledger and backlog restoration",
    );
    assert(
      orchestrator.includes("captureFailedCandidateSnapshot(checkout, snapshotDirectory, baseSha)") &&
        orchestrator.includes("let retryCheckpoint = selectedIssueCheckpoint") &&
        orchestrator.includes("let retryCheckpointExpectedRemoteSha = selectedIssueCheckpoint?.sha ?? null") &&
        orchestrator.includes("let lastPushedCandidateSha: string | null = null") &&
        orchestrator.includes("lastPushedCandidateSha = pushedCandidateSha") &&
        checkpointPublishLane.includes("selectedIssueAggregatePaths()") &&
        checkpointPublishLane.includes("assertImplementationFilesExcludeValues(checkout, sensitiveValues, paths)") &&
        checkpointPublishLane.includes("assertProtectedFilesUnchanged(checkout, baseProtectedHashes)") &&
        checkpointPublishLane.includes("scanCandidateWithGitleaks({") &&
        checkpointPublishLane.includes("await commitChanges(") &&
        checkpointPublishLane.includes("assertGitHistoryExcludesValues") &&
        retryDeferLane.includes("lastPushedCandidateSha,") &&
        checkpointPublishLane.includes("expectedRemoteSha,") &&
        checkpointPublishLane.includes("prepareImmutableTemporaryCheckpoint(") &&
        !checkpointPublishLane.includes('"push",') &&
        issueCompletionLane.includes('github-issue-${retryPending ? "retry" : "manual"}-retained-candidate.json') &&
        issueCompletionLane.includes("head_sha: retainedCheckpoint.sha"),
      "Retry evidence must validate and prepare the aggregate candidate before atomic publication records its remote SHA",
    );
    assert(
      checkpointPublishLane.indexOf("const preparedSha = await prepareImmutableTemporaryCheckpoint") >= 0 &&
        checkpointStatePosition > checkpointPosition &&
        !checkpointPublishLane.includes("pushRetryPendingRefsAtomically("),
      "An issue-candidate checkpoint must remain runner-local until its ledger commit is ready for atomic publication",
    );
    const issueRevalidationPosition = checkpointPublishLane.indexOf(
      "const checkpointIssueJob = await getCurrentGitHubIssueJob",
    );
    const checkpointPushPosition = checkpointPublishLane.indexOf(
      "const preparedSha = await prepareImmutableTemporaryCheckpoint",
    );
    assert(
      issueRevalidationPosition > checkpointPublishLane.indexOf("await assertGitHistoryExcludesValues") &&
        checkpointPublishLane.indexOf("githubIssueJobsMatch(workSelection.issueJob, checkpointIssueJob)") >
          issueRevalidationPosition &&
        checkpointPushPosition > issueRevalidationPosition,
      "Retry checkpoint preparation must revalidate the exact GitHub issue before retaining the local object",
    );
    assert(
      issueCompletionLane.includes("await pushRetryPendingRefsAtomically({") &&
        orchestrator.includes('"--atomic",') &&
        orchestrator.includes("`--force-with-lease=refs/heads/${input.checkpoint.branch}:") &&
        orchestrator.includes("`${input.checkpoint.sha}:refs/heads/${input.checkpoint.branch}`") &&
        workflow.includes('SENTINEL_GIT_PUSH_ATOMIC="$atomic_push"') &&
        workflow.includes('SENTINEL_GIT_CHECKPOINT_LEASE_SHA="$normalized_checkpoint_lease_sha"'),
      "Checkpoint retries must publish development and the raw checkpoint SHA in one atomic lease-protected push",
    );
    const atomicPushStart = orchestrator.indexOf("export const pushRetryPendingRefsAtomically = async");
    const atomicPushEnd = orchestrator.indexOf("export const prepareResumedGitHubIssueCandidate", atomicPushStart);
    const atomicPushLane = orchestrator.slice(atomicPushStart, atomicPushEnd);
    const startingPosition = atomicPushLane.indexOf("await input.onAtomicPushStarting?.()");
    const atomicGitPosition = atomicPushLane.indexOf("await runTrustedGit({");
    const acceptedUnverifiedPosition = atomicPushLane.indexOf("await input.onAtomicPushAcceptedUnverified?.()");
    const remoteVerificationPosition = atomicPushLane.indexOf("const [remoteDevelopment, remoteCheckpoint]");
    assert(
      startingPosition >= 0 && atomicGitPosition > startingPosition && acceptedUnverifiedPosition > atomicGitPosition &&
        remoteVerificationPosition > acceptedUnverifiedPosition &&
        issueCompletionLane.includes('"runner_local_atomic_push_in_flight"') &&
        issueCompletionLane.includes("onAtomicPushStarting: () =>") &&
        issueCompletionLane.includes("branch_disposition: retryPending") &&
        issueCompletionLane.includes('"remote_retained_atomic_push_in_flight"') &&
        issueCompletionLane.includes('"atomic_retry_push_accepted_unverified"') &&
        issueCompletionLane.includes('"remote_retained_issue_retry_pending"') &&
        issueCompletionLane.includes('"runner_local_manual_atomic_push_in_flight"') &&
        issueCompletionLane.includes('"atomic_manual_push_accepted_unverified"') &&
        issueCompletionLane.includes('"remote_retained_issue_manual_required"') &&
        orchestrator.includes('state.branch_disposition === "atomic_retry_push_accepted_unverified"') &&
        orchestrator.includes('state.branch_disposition === "runner_local_atomic_push_in_flight"') &&
        orchestrator.includes('state.branch_disposition === "remote_retained_atomic_push_in_flight"') &&
        orchestrator.includes('"atomic_retry_push_requires_reconciliation"') &&
        orchestrator.includes('state.branch_disposition === "atomic_manual_push_accepted_unverified"') &&
        orchestrator.includes('state.branch_disposition === "runner_local_manual_atomic_push_in_flight"') &&
        orchestrator.includes('"atomic_manual_push_requires_reconciliation"'),
      "Every checkpoint atomic push must enter a durable in-flight state and remain unverified until both remote refs match",
    );
    assert(
      checkpointPublishLane.includes("await restoreIssueRetryAggregateIfEmpty(") &&
        checkpointPublishLane.includes("if (paths.length === 0) return null") &&
        !checkpointPublishLane.includes("if (paths.length === 0) return retryCheckpoint"),
      "A fresh empty attempt may cool down without a checkpoint, while a resumed aggregate is restored before push",
    );
    assert(
      orchestrator.includes("prepareResumedGitHubIssueCandidate({") &&
        orchestrator.includes("retryCheckpointResumeFailureDisposition(error)") &&
        orchestrator.includes('failureDisposition === "retry_pending"') &&
        orchestrator.includes("Sentinel retry checkpoint conflicts with current development") &&
        orchestrator.includes('"manual_required",') &&
        orchestrator.includes('"retry_checkpoint_resume_failed"'),
      "Checkpoint resume must retry transient failures and fail closed on deterministic integrity failures",
    );
    assert(
      orchestrator.includes('state.branch_disposition === "remote_retained_pending_decision" ||') &&
        orchestrator.includes('state.branch_disposition === "remote_retained_issue_retry_pending"') &&
        orchestrator.includes('return "remote_retained_after_failed_cycle"') &&
        orchestrator.includes('state.stage === "validated_retry_pending_atomic_push"'),
      "A failed retry-ledger push must preserve the already-pushed candidate branch disposition",
    );
    const rollingCutoverCycleStart = orchestrator.indexOf("let reviewRound = 0;");
    const rollingCutoverCycleEnd = orchestrator.indexOf(
      'if (workSelection.source === "review_backlog" && selectedBacklogState.disposition !== "resolved")',
      rollingCutoverCycleStart,
    );
    const rollingCutoverLane = orchestrator.slice(rollingCutoverCycleStart, rollingCutoverCycleEnd);
    assert(
      rollingCutoverCycleStart >= 0 && rollingCutoverCycleEnd > rollingCutoverCycleStart &&
        rollingCutoverLane.includes("canStartReviewRound(reviewRound)") &&
        rollingCutoverLane.includes("sentinel/report.md") === false,
      "The bounded deterministic repair lane must start before runtime validation",
    );
    assert(
      !orchestrator.includes("runNativeCodexReview(") &&
        !orchestrator.includes("implementation_review_fix_") &&
        !orchestrator.includes("terminalizeGitHubIssueNativeReviewExhaustion") &&
        !orchestrator.includes("native_review_exhausted"),
      "Delivery must no longer wait on or correct a synchronous native Codex review",
    );
    assert(
      orchestrator.includes("Deterministic repair attempts remain after the bounded candidate preparation rounds"),
      "The bounded lane must be the last failure boundary before runtime validation",
    );
    const previewPushPosition = orchestrator.indexOf(
      "const pushedCandidateSha = await pushTemporaryCandidate",
    );
    const previewDeploymentPosition = orchestrator.indexOf(
      "const preview = await dispatchAndResolveRevision({",
    );
    assert(
      previewPushPosition > rollingCutoverCycleEnd && previewDeploymentPosition > rollingCutoverCycleEnd,
      "Deterministic validation must finish before the candidate preview push and deployment",
    );
    for (
      const [startNeedle, endNeedle, stage] of [
        ["const stage = `implementation_validation_fix_", "      continue;", "implementation_validation_fix_"],
        ["const replayEvaluationStage = `replay_evaluation_", "    break;", "replay_evaluation_"],
      ] as const
    ) {
      const start = orchestrator.indexOf(startNeedle);
      const end = orchestrator.indexOf(endNeedle, start);
      const lane = orchestrator.slice(start, end);
      assert(
        start >= 0 && end > start && lane.includes("deferGitHubIssueImplementationFailure(") &&
          lane.includes("restoreGitHubIssuePreviewBeforeRetry") &&
          lane.includes("await completeNonRuntimeGitHubIssueDisposition()") && lane.includes("return;"),
        `${stage} infrastructure failures must enter the durable GitHub issue retry path`,
      );
    }
    const validationStart = orchestrator.indexOf("const validationStage = `validation_");
    const validationEnd = orchestrator.indexOf("await updateState(`preview_deploy_", validationStart);
    const validationLane = orchestrator.slice(validationStart, validationEnd);
    assert(
      validationStart >= 0 && validationEnd > validationStart &&
        validationLane.includes("await preserveFailedImplementation(error, validationStage, candidateSha)") &&
        validationLane.indexOf("await preserveFailedImplementation(error, validationStage, candidateSha)") <
          validationLane.indexOf("throw error;"),
      "Terminal candidate validation failures must preserve safe encrypted evidence before failing",
    );
    const retryPreviewStart = orchestrator.indexOf("const restoreGitHubIssuePreviewBeforeRetry = async");
    const retryPreviewEnd = orchestrator.indexOf("while (true)", retryPreviewStart);
    const retryPreviewLane = orchestrator.slice(retryPreviewStart, retryPreviewEnd);
    assert(
      retryPreviewStart >= 0 && retryPreviewEnd > retryPreviewStart &&
        retryPreviewLane.includes("issueRetryPreviewTarget") &&
        retryPreviewLane.includes("issueRetryPreviewCandidate") &&
        retryPreviewLane.includes("dispatchSerializedPromotion({") &&
        retryPreviewLane.includes("await deno.verifyHealthIdentity(") &&
        retryPreviewLane.includes("github-issue-retry-preview-rollback.json"),
      "Every post-preview GitHub issue retry must restore and record the original preview identity",
    );
    assert(
      orchestrator.includes("if (workSelection.issueJob && issueRetryPreviewTarget === null)") &&
        orchestrator.includes("issueRetryPreviewCandidate = Object.freeze"),
      "Multi-round GitHub issue work must retain its first preview target and latest promoted candidate",
    );
    assert(
      /- name: Install isolated-agent prerequisites\n\s+if: steps\.agent-work\.outputs\.needs_agent == 'true'/.test(
        workflow,
      ),
      "Quiet hourly archival must not install Codex",
    );
    assert(
      /- name: Run canonical local Sentinel verification\n\s+shell: bash\n\s+run: \|\n\s+set -euo pipefail\n\s+# Verification belongs to one canonical local command; CI only repeats\n\s+# it and defines no Sentinel verification steps of its own\.\n\s+deno task sentinel:test-local/u
        .test(
          workflow,
        ),
      "Quiet hourly archival must run exactly the canonical local Sentinel verification",
    );
    assert(
      !workflow.includes("Validate pinned Codex CLI argument contract"),
      "Sentinel must not reinstate the removed independent pinned Codex CLI validation",
    );
    assert(workflow.includes("github.run_attempt == 1"), "Incident mode must reject human-triggered workflow re-runs");
    assert(workflow.includes("SENTINEL_AUTONOMY_ENABLED == 'true'"), "Incident mode must require autonomy");
    assert(workflow.includes("Acknowledge completed incident"), "Successful incident runs must ACK the durable outbox");
    assert(workflow.includes("Claim incident workflow run"), "Incident runs must claim one durable workflow identity");
    assert(
      workflow.indexOf("Claim incident workflow run") < workflow.indexOf("Check out full repository history"),
      "Duplicate incident runs must stop before checkout or repair work",
    );
    assert(
      server.includes('Deno.cron("deliver pending Provider Sentinel incidents", "* * * * *"'),
      "Deno cron must retry only durable pending incidents",
    );
    assert(
      deploy.includes('method: "PATCH"') && deploy.includes('contexts: ["production"]') &&
        deploy.includes("secret: true"),
      "The GitHub App key must use one production-only secret patch",
    );
    assert(
      !/lines\.push\(`SENTINEL_GITHUB_APP_PRIVATE_KEY=/u.test(deploy),
      "The GitHub App key must never enter the general deploy environment file",
    );
  },
});
