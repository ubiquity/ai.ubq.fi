import {
  decodeSentinelArtifactKey,
  decryptSentinelArtifact,
  encryptSentinelArtifact,
} from "../scripts/sentinel/artifact-crypto.ts";
import { encryptAndScrubGeneratedEvidence } from "../scripts/sentinel/encrypt-artifacts.ts";

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
      await assertRejects(() => encryptAndScrubGeneratedEvidence(root, key));
      assert(
        equalBytes(await Deno.readFile(output), existing),
        "Pre-existing artifact was changed or removed",
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
  name: "Sentinel encryption verifies the bundle before scrubbing generated plaintext",
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
      const result = await encryptAndScrubGeneratedEvidence(root, key);
      assert(result.fileCount === 3, "Unexpected encrypted file count");
      await assertRejects(() => Deno.stat(`${root}/raw-logs`));
      await assertRejects(() => Deno.stat(`${root}/reports`));
      await assertRejects(() => Deno.stat(`${root}/candidate-worktree`));
      await assertRejects(() => Deno.stat(`${root}/private`));
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
  },
});

Deno.test({
  name: "Sentinel repair workflow retains queued incident signals",
  ignore: fileSystemTestsUnavailable,
  async fn() {
    const workflow = await Deno.readTextFile(".github/workflows/provider-sentinel.yml");
    assert(/^\s+queue: max$/mu.test(workflow), "Sentinel must retain concurrent incident signals");
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
    assert(workflow.includes("github.actor_id == '319834869'"), "Incident mode must require the Sentinel App actor");
    assert(workflow.includes('- cron: "0 * * * *"'), "Sentinel archival must run hourly");
    assert(workflow.includes("mode=hourly"), "Scheduled runs must use archive-only hourly mode");
    assert(
      /- name: Install isolated-agent prerequisites\n\s+if: github\.event_name != 'schedule'/.test(workflow),
      "Hourly archival must not install Codex",
    );
    assert(
      /- name: Validate pinned Codex CLI argument contract\n\s+if: github\.event_name != 'schedule'/.test(workflow),
      "Hourly archival must not run Codex CLI validation",
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
