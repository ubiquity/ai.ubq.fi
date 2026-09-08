import assert from "node:assert/strict";

import deploymentWorkflow from "../.github/workflows/deno-deploy.yml" with { type: "text" };

Deno.test("deployment workflow validates pull requests without deploying them", () => {
  assert.match(deploymentWorkflow, /^permissions:\n[ ]{2}contents: read$/mu);
  assert.match(deploymentWorkflow, /^[ ]{2}pull_request:$/mu);

  const deployJob = deploymentWorkflow.match(
    /^[ ]{2}deploy:\n([\s\S]*?)(?=^[ ]{2}verify-deployment:)/mu,
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

Deno.test("reusable deployment pins the proven Deno CLI version", () => {
  const deployJob = deploymentWorkflow.match(
    /^[ ]{2}deploy:\n([\s\S]*?)(?=^[ ]{2}verify-deployment:)/mu,
  )?.[1];
  assert.ok(deployJob, "deploy job must remain present");
  assert.match(deployJob, /^[ ]{6}deno_version: 2\.9\.5$/mu);
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

const RECEIPT_INDENT = " ".repeat(10);
const RECEIPT_BEGIN_MARKER = `${RECEIPT_INDENT}# Begin exact-build receipt`;
const RECEIPT_END_MARKER = `${RECEIPT_INDENT}# End exact-build receipt`;

function attestationStep(): string {
  const step = deploymentWorkflow.match(
    /- name: Promote and verify exact deployment revision\n([\s\S]*?)(?=- name: Upload exact-build receipt)/u,
  )?.[1] ?? "";
  assert.ok(step, "the Promote and verify exact deployment revision step must remain present");
  return step;
}

function uploadReceiptStep(): string {
  const step = deploymentWorkflow.match(
    /- name: Upload exact-build receipt\n([\s\S]*)$/u,
  )?.[1] ?? "";
  assert.ok(step, "the Upload exact-build receipt step must remain present");
  return step;
}

function extractBuildReceiptBlock(): string {
  const begin = deploymentWorkflow.indexOf(RECEIPT_BEGIN_MARKER);
  const end = deploymentWorkflow.indexOf(RECEIPT_END_MARKER);
  assert.ok(begin !== -1, "the '# Begin exact-build receipt' marker must be present in the deployment workflow");
  assert.ok(end !== -1, "the '# End exact-build receipt' marker must be present in the deployment workflow");
  assert.ok(begin < end, "the exact-build receipt markers must appear in order");
  assert.equal(
    deploymentWorkflow.split(RECEIPT_BEGIN_MARKER).length - 1,
    1,
    "the '# Begin exact-build receipt' marker must appear exactly once",
  );
  assert.equal(
    deploymentWorkflow.split(RECEIPT_END_MARKER).length - 1,
    1,
    "the '# End exact-build receipt' marker must appear exactly once",
  );
  const block = deploymentWorkflow.slice(begin, end + RECEIPT_END_MARKER.length);
  return block
    .split("\n")
    .map((line) => line.startsWith(RECEIPT_INDENT) ? line.slice(RECEIPT_INDENT.length) : line)
    .join("\n");
}

Deno.test("exact-build receipt is written after the final unique succeeded check and before promotion", () => {
  const step = attestationStep();
  const uniqueSucceededCount = step.match(
    /select\(\.id == \$id and \.status == "succeeded"\)/gu,
  )?.length ?? 0;
  const finalSucceededCheck = step.indexOf('select(.id == $id and .status == "succeeded")');
  const identityCheck = step.indexOf("The post-baseline revision identity changed before promotion.");
  const receiptStart = step.indexOf(RECEIPT_BEGIN_MARKER);
  const promotion = step.indexOf('promote_status="$(curl');
  assert.equal(uniqueSucceededCount, 1, "exactly one final unique succeeded check must remain");
  assert.ok(identityCheck !== -1, "the post-baseline revision identity check must remain");
  assert.ok(receiptStart !== -1, "the exact-build receipt block must remain");
  assert.ok(promotion !== -1, "the promotion call must remain");
  assert.ok(
    receiptStart > finalSucceededCheck && receiptStart > identityCheck,
    `the receipt must be written after the final unique succeeded check (receipt at ${receiptStart}, unique succeeded check at ${finalSucceededCheck}, identity check at ${identityCheck})`,
  );
  assert.ok(
    promotion > receiptStart,
    `the receipt must be written before promotion (receipt at ${receiptStart}, promotion at ${promotion})`,
  );
});

Deno.test("exact-build receipt ready output is emitted only after the jq write", () => {
  const block = extractBuildReceiptBlock();
  const jqProgram = block.indexOf("jq -en");
  const jqWrite = block.indexOf("> .release/sentinel-build-receipt.json");
  const readyOutput = block.indexOf("build_receipt_ready=true");
  assert.notEqual(jqProgram, -1, "the receipt must be written with jq");
  assert.notEqual(jqWrite, -1, "the receipt must be written to .release/sentinel-build-receipt.json");
  assert.notEqual(readyOutput, -1, "the ready output must remain");
  assert.ok(
    readyOutput > jqWrite && jqWrite > jqProgram,
    `the ready output must be gated on the jq write (jq program at ${jqProgram}, jq write at ${jqWrite}, ready output at ${readyOutput})`,
  );
});

Deno.test("exact-build receipt upload always runs but is gated on the attest output", () => {
  const step = uploadReceiptStep();
  assert.match(step, /if: always\(\) && steps\.attest\.outputs\.build_receipt_ready == 'true'/u);
  assert.match(step, /uses: actions\/upload-artifact@[0-9a-f]{40} # v4/u);
  assert.match(step, /path: \.release\/sentinel-build-receipt\.json/u);
  assert.match(step, /if-no-files-found: error/u);
});

Deno.test("exact-build receipt artifact identity is immutable and bounded", () => {
  const step = uploadReceiptStep();
  const receiptAction = step.match(/actions\/upload-artifact@([0-9a-f]{40})/u)?.[1];
  const sourceUpload = deploymentWorkflow.match(
    /- name: Upload immutable source\n([\s\S]*?)(?=\n{2}[ ]{2}verify-artifact:)/u,
  )?.[1] ?? "";
  assert.ok(sourceUpload, "the Upload immutable source step must remain present");
  const sourceAction = sourceUpload.match(/actions\/upload-artifact@([0-9a-f]{40})/u)?.[1];
  assert.ok(receiptAction, "the receipt upload must pin an upload-artifact action revision");
  assert.ok(sourceAction, "the source upload must pin an upload-artifact action revision");
  assert.equal(
    receiptAction,
    sourceAction,
    "the receipt upload must pin the same upload-artifact action revision as the immutable source upload",
  );
  assert.match(
    step,
    /name: sentinel-build-receipt-\$\{\{ github\.run_id \}\}-\$\{\{ github\.run_attempt \}\}/u,
  );
  assert.doesNotMatch(step, /\$\{\{ github\.sha \}\}/u);
  assert.match(step, /retention-days: 14/u);
  assert.match(step, /include-hidden-files: true/u);
});

const environmentPermissions = await Promise.all([
  Deno.permissions.query({ name: "read" }),
  Deno.permissions.query({ name: "write" }),
  Deno.permissions.query({ name: "run", command: "bash" }),
  Deno.permissions.query({ name: "env", variable: "PATH" }),
]);
const functionalEnabled = environmentPermissions.every((permission) => permission.state === "granted");

const BASE_ENV: Record<string, string> = {
  GITHUB_REPOSITORY: "acme/deploy-demo",
  GITHUB_RUN_ID: "42",
  GITHUB_RUN_ATTEMPT: "1",
  GITHUB_REF: "refs/heads/development",
  GITHUB_WORKFLOW_REF: "acme/deploy-demo/.github/workflows/deno-deploy.yml@refs/heads/development",
  GITHUB_SHA: "5c9a1b2e3f4a5b6c7d8e9f0a1b2c3d4e5f6a7b8c",
  TARGET_PROJECT: "ai-ubq-fi",
};
const VALID_REVISION_ID = "r9k3-7b1c2d";
const RECEIPT_KEYS = [
  "version",
  "repository",
  "run_id",
  "run_attempt",
  "workflow_ref",
  "git_sha",
  "project",
  "revision_id",
  "build_transaction_id",
].sort();

function shSingleQuote(value: string): string {
  return `'${value.replaceAll("'", "'\\''")}'`;
}

function buildReceiptScript(revisionId: string): string {
  return [
    "set -euo pipefail",
    "fail() { exit 1; }",
    `revision_id=${shSingleQuote(revisionId)}`,
    extractBuildReceiptBlock(),
  ].join("\n") + "\n";
}

interface ReceiptRun {
  runDir: string;
  outputPath: string;
  success: boolean;
  code: number;
  stdout: string;
  stderr: string;
}

async function runReceiptScript(
  env: Readonly<Record<string, string | undefined>>,
  revisionId: string,
  parentDir: string,
): Promise<ReceiptRun> {
  const runDir = await Deno.makeTempDir({ dir: parentDir, prefix: "receipt-run-" });
  const outputPath = `${runDir}/GITHUB_OUTPUT.txt`;
  const childEnv: Record<string, string> = {
    PATH: Deno.env.get("PATH") ?? "",
    GITHUB_OUTPUT: outputPath,
  };
  for (const [key, value] of Object.entries(env)) {
    if (value !== undefined) childEnv[key] = value;
  }
  const command = new Deno.Command("bash", {
    args: ["-c", buildReceiptScript(revisionId)],
    cwd: runDir,
    clearEnv: true,
    env: childEnv,
    stdout: "piped",
    stderr: "piped",
  });
  const result = await command.output();
  return {
    runDir,
    outputPath,
    success: result.success,
    code: result.code,
    stdout: new TextDecoder().decode(result.stdout),
    stderr: new TextDecoder().decode(result.stderr),
  };
}

function childOutput(run: ReceiptRun): string {
  return `\n(exit ${run.code}; cwd ${run.runDir})\n-- child stdout --\n${run.stdout}\n-- child stderr --\n${run.stderr}`;
}

function assertRunSucceeded(run: ReceiptRun): void {
  assert.ok(run.success, `the receipt script must succeed${childOutput(run)}`);
}

function assertRunFailed(run: ReceiptRun, expectation: string): void {
  assert.ok(!run.success, `the receipt script must fail when ${expectation}${childOutput(run)}`);
}

async function fileExists(path: string): Promise<boolean> {
  try {
    await Deno.stat(path);
    return true;
  } catch (error) {
    if (error instanceof Deno.errors.NotFound) return false;
    throw error;
  }
}

function expectedReceipt(env: Record<string, string>, revisionId: string): Record<string, unknown> {
  return {
    version: 1,
    repository: env.GITHUB_REPOSITORY,
    run_id: env.GITHUB_RUN_ID,
    run_attempt: env.GITHUB_RUN_ATTEMPT,
    workflow_ref: env.GITHUB_WORKFLOW_REF,
    git_sha: env.GITHUB_SHA,
    project: env.TARGET_PROJECT,
    revision_id: revisionId,
    build_transaction_id: `github-actions:${env.GITHUB_REPOSITORY}:${env.GITHUB_RUN_ID}:${env.GITHUB_RUN_ATTEMPT}`,
  };
}

function readReceipt(run: ReceiptRun): Record<string, unknown> {
  const receiptPath = `${run.runDir}/.release/sentinel-build-receipt.json`;
  return JSON.parse(Deno.readTextFileSync(receiptPath)) as Record<string, unknown>;
}

function functionalTest(name: string, fn: () => Promise<void>): void {
  Deno.test({ name, ignore: !functionalEnabled, fn });
}

functionalTest("exact-build receipt script emits the exact JSON schema and output", async () => {
  const rootDir = await Deno.makeTempDir();
  try {
    const run = await runReceiptScript(BASE_ENV, VALID_REVISION_ID, rootDir);
    assertRunSucceeded(run);
    const receipt = readReceipt(run);
    assert.deepEqual(
      Object.keys(receipt).sort(),
      RECEIPT_KEYS,
      `the receipt must contain exactly the documented keys${childOutput(run)}`,
    );
    assert.deepEqual(
      receipt,
      expectedReceipt(BASE_ENV, VALID_REVISION_ID),
      `the receipt values must equal the run inputs${childOutput(run)}`,
    );
    assert.equal(
      Deno.readTextFileSync(run.outputPath),
      "build_receipt_ready=true\n",
      `the ready output must be written only after the receipt${childOutput(run)}`,
    );
  } finally {
    await Deno.remove(rootDir, { recursive: true });
  }
});

functionalTest("exact-build receipt transaction identity differs between run attempts", async () => {
  const rootDir = await Deno.makeTempDir();
  try {
    const attempt1 = await runReceiptScript(BASE_ENV, VALID_REVISION_ID, rootDir);
    assertRunSucceeded(attempt1);
    const firstReceipt = readReceipt(attempt1);
    const attempt2 = await runReceiptScript(
      { ...BASE_ENV, GITHUB_RUN_ATTEMPT: "2" },
      VALID_REVISION_ID,
      rootDir,
    );
    assertRunSucceeded(attempt2);
    const secondReceipt = readReceipt(attempt2);
    assert.equal(firstReceipt.build_transaction_id, "github-actions:acme/deploy-demo:42:1");
    assert.equal(secondReceipt.build_transaction_id, "github-actions:acme/deploy-demo:42:2");
    assert.notEqual(
      firstReceipt.build_transaction_id,
      secondReceipt.build_transaction_id,
      "different run attempts must produce different build transaction identities",
    );
    assert.equal(firstReceipt.run_id, secondReceipt.run_id);
    assert.equal(firstReceipt.run_attempt, "1");
    assert.equal(secondReceipt.run_attempt, "2");
  } finally {
    await Deno.remove(rootDir, { recursive: true });
  }
});

functionalTest("exact-build receipt script fails when a required environment variable is missing", async () => {
  const rootDir = await Deno.makeTempDir();
  try {
    for (const variable of Object.keys(BASE_ENV)) {
      const env = { ...BASE_ENV };
      delete env[variable];
      const run = await runReceiptScript(env, VALID_REVISION_ID, rootDir);
      assert.ok(
        !run.success,
        `the receipt script must fail without ${variable}${childOutput(run)}`,
      );
    }
  } finally {
    await Deno.remove(rootDir, { recursive: true });
  }
});

functionalTest("exact-build receipt script rejects malformed run, attempt, and repository identity", async () => {
  const rootDir = await Deno.makeTempDir();
  try {
    const cases: Array<{ name: string; env: Partial<Record<string, string>> }> = [
      { name: "GITHUB_RUN_ID zero", env: { GITHUB_RUN_ID: "0" } },
      { name: "GITHUB_RUN_ID leading zero", env: { GITHUB_RUN_ID: "01" } },
      { name: "GITHUB_RUN_ID trailing newline", env: { GITHUB_RUN_ID: "42\n" } },
      { name: "GITHUB_RUN_ID leading space", env: { GITHUB_RUN_ID: " 42" } },
      { name: "GITHUB_RUN_ATTEMPT zero", env: { GITHUB_RUN_ATTEMPT: "0" } },
      { name: "GITHUB_RUN_ATTEMPT negative", env: { GITHUB_RUN_ATTEMPT: "-1" } },
      { name: "GITHUB_RUN_ATTEMPT trailing newline", env: { GITHUB_RUN_ATTEMPT: "1\n" } },
      { name: "GITHUB_REPOSITORY missing owner/name split", env: { GITHUB_REPOSITORY: "acme-only" } },
      { name: "GITHUB_REPOSITORY illegal name character", env: { GITHUB_REPOSITORY: "acme/deploy!demo" } },
      { name: "GITHUB_REPOSITORY trailing newline", env: { GITHUB_REPOSITORY: "acme/deploy-demo\n" } },
    ];
    for (const testCase of cases) {
      const run = await runReceiptScript({ ...BASE_ENV, ...testCase.env }, VALID_REVISION_ID, rootDir);
      assertRunFailed(run, `${testCase.name} is provided`);
    }
  } finally {
    await Deno.remove(rootDir, { recursive: true });
  }
});

functionalTest("exact-build receipt script requires a full lowercase 40-hex Git SHA", async () => {
  const rootDir = await Deno.makeTempDir();
  try {
    const accepted = await runReceiptScript(BASE_ENV, VALID_REVISION_ID, rootDir);
    assertRunSucceeded(accepted);
    const rejected: Array<{ name: string; env: Partial<Record<string, string>> }> = [
      { name: "uppercase SHA-40", env: { GITHUB_SHA: BASE_ENV.GITHUB_SHA.toUpperCase() } },
      { name: "SHA with trailing newline", env: { GITHUB_SHA: `${BASE_ENV.GITHUB_SHA}\n` } },
      { name: "39-character SHA", env: { GITHUB_SHA: BASE_ENV.GITHUB_SHA.slice(0, 39) } },
      { name: "41-character SHA", env: { GITHUB_SHA: `${BASE_ENV.GITHUB_SHA}a` } },
      {
        name: "SHA with an embedded hyphen",
        env: { GITHUB_SHA: `${BASE_ENV.GITHUB_SHA.slice(0, 8)}-${BASE_ENV.GITHUB_SHA.slice(8)}` },
      },
      { name: "non-hex SHA-40", env: { GITHUB_SHA: BASE_ENV.GITHUB_SHA.replace("a", "g") } },
    ];
    for (const testCase of rejected) {
      const run = await runReceiptScript({ ...BASE_ENV, ...testCase.env }, VALID_REVISION_ID, rootDir);
      assertRunFailed(run, testCase.name);
    }
  } finally {
    await Deno.remove(rootDir, { recursive: true });
  }
});

functionalTest("exact-build receipt script enforces the target project allowlist", async () => {
  const rootDir = await Deno.makeTempDir();
  try {
    const cases: Array<{ name: string; value: string; accepted: boolean }> = [
      { name: "ai-ubq-fi", value: "ai-ubq-fi", accepted: true },
      { name: "p-ai-ubq-fi", value: "p-ai-ubq-fi", accepted: true },
      { name: "prod", value: "prod", accepted: false },
      { name: "empty", value: "", accepted: false },
      { name: "mixed case", value: "Ai-Ubq-Fi", accepted: false },
      { name: "trailing space", value: "ai-ubq-fi ", accepted: false },
    ];
    for (const testCase of cases) {
      const run = await runReceiptScript(
        { ...BASE_ENV, TARGET_PROJECT: testCase.value },
        VALID_REVISION_ID,
        rootDir,
      );
      if (testCase.accepted) {
        assertRunSucceeded(run);
      } else {
        assertRunFailed(run, `${testCase.name} is the target project`);
      }
    }
  } finally {
    await Deno.remove(rootDir, { recursive: true });
  }
});

functionalTest("exact-build receipt script rejects malicious or malformed revision ids", async () => {
  const rootDir = await Deno.makeTempDir();
  try {
    const rejected: Array<{ name: string; value: string; noExecutionPayload: boolean }> = [
      { name: "leading hyphen", value: "-abc1", noExecutionPayload: false },
      { name: "uppercase letters", value: "ABC1", noExecutionPayload: false },
      { name: "embedded space", value: "a b1", noExecutionPayload: false },
      { name: "path traversal", value: "../v1", noExecutionPayload: false },
      { name: "embedded dot", value: "a.b1", noExecutionPayload: false },
      { name: "single quote", value: "a'b1", noExecutionPayload: false },
      { name: "command substitution payload", value: "a$(echo pwned)b", noExecutionPayload: true },
      { name: "backtick payload", value: "a`echo pwned`b", noExecutionPayload: true },
      { name: "129-character id", value: "a".repeat(129), noExecutionPayload: false },
      { name: "trailing newline", value: `${VALID_REVISION_ID}\n`, noExecutionPayload: false },
    ];
    for (const testCase of rejected) {
      const run = await runReceiptScript(BASE_ENV, testCase.value, rootDir);
      assert.ok(
        !run.success,
        `the receipt script must reject an id '${testCase.name}'${childOutput(run)}`,
      );
      if (testCase.noExecutionPayload) {
        assert.ok(
          !run.stdout.includes("pwned") && !run.stderr.includes("pwned"),
          `the ${testCase.name} must never be executed${childOutput(run)}`,
        );
      }
    }
    const boundary = await runReceiptScript(BASE_ENV, "a".repeat(128), rootDir);
    assertRunSucceeded(boundary);
    assert.equal(readReceipt(boundary).revision_id, "a".repeat(128));
  } finally {
    await Deno.remove(rootDir, { recursive: true });
  }
});

functionalTest("exact-build receipt script rejects a wrong workflow path or ref without ready output", async () => {
  const rootDir = await Deno.makeTempDir();
  try {
    const cases: Array<{ name: string; env: Partial<Record<string, string>> }> = [
      {
        name: "wrong workflow filename",
        env: { GITHUB_WORKFLOW_REF: "acme/deploy-demo/.github/workflows/deno-deploy-other.yml@refs/heads/development" },
      },
      {
        name: "wrong repository",
        env: { GITHUB_WORKFLOW_REF: "acme/other/.github/workflows/deno-deploy.yml@refs/heads/development" },
      },
      {
        name: "wrong ref",
        env: { GITHUB_WORKFLOW_REF: "acme/deploy-demo/.github/workflows/deno-deploy.yml@refs/tags/v1.0.0" },
      },
      {
        name: "missing ref suffix",
        env: { GITHUB_WORKFLOW_REF: "acme/deploy-demo/.github/workflows/deno-deploy.yml" },
      },
      { name: "trailing newline", env: { GITHUB_WORKFLOW_REF: `${BASE_ENV.GITHUB_WORKFLOW_REF}\n` } },
    ];
    for (const testCase of cases) {
      const run = await runReceiptScript({ ...BASE_ENV, ...testCase.env }, VALID_REVISION_ID, rootDir);
      assertRunFailed(run, testCase.name);
      const outputFile = await fileExists(run.outputPath);
      const readyOutput = outputFile ? Deno.readTextFileSync(run.outputPath) : "";
      assert.ok(
        !readyOutput.includes("build_receipt_ready=true"),
        `${testCase.name} must not emit the ready output${childOutput(run)}`,
      );
      assert.ok(
        !(await fileExists(`${run.runDir}/.release/sentinel-build-receipt.json`)),
        `${testCase.name} must not write a receipt file${childOutput(run)}`,
      );
    }
  } finally {
    await Deno.remove(rootDir, { recursive: true });
  }
});
