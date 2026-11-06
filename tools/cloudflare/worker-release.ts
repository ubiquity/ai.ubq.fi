import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { copyFile, lstat, mkdir, readFile, readdir, writeFile } from "node:fs/promises";
import { dirname, join, relative, resolve } from "node:path";
import { workerTarget, WORKER_ACCOUNT_ID, WORKER_COMPATIBILITY_DATE, WORKER_COMPATIBILITY_FLAGS } from "../../cloudflare-targets.ts";
import type { WorkerTarget } from "../../cloudflare-targets.ts";

type FileDigest = { path: string; sha256: string };
export type ReleaseIdentity = { gitSha: string; gitTree: string };
export type WorkerReleaseReceipt = ReleaseIdentity & {
  version: 2;
  mode: WorkerTarget["mode"];
  worker: string;
  accountId: string;
  compatibilityDate: string;
  compatibilityFlags: string[];
  authority: { worker: string; exportName: "GatewayAuthority"; storage: "sqlite" };
  tools: { bun: string; cf: string; wrangler: string };
  files: FileDigest[];
  outputFiles: FileDigest[];
};

const outputPath = ".cloudflare/output/v0";
const requiredSource = [
  "cloudflare.config.ts",
  "cloudflare-targets.ts",
  "wrangler.config.ts",
  "package.json",
  "bun.lock",
  "rust/Cargo.lock",
  "rust/crates/gateway-worker/entrypoint.mjs",
  "rust/crates/gateway-worker/build/index.js",
  "rust/crates/gateway-worker/build/index_bg.wasm",
  "ops/rust-worker-build.sh",
  "ops/rust-worker-deploy.sh",
  "tools/cloudflare/worker-release.ts",
  "tools/cloudflare/generate-types.ts",
];

function record(value: unknown): Record<string, unknown> {
  assert(typeof value === "object" && value !== null && !Array.isArray(value), "Expected a release object.");
  return value as Record<string, unknown>;
}

function fileDigests(value: unknown): FileDigest[] {
  assert(Array.isArray(value) && value.length > 0, "Missing artifact manifest.");
  const paths = new Set<string>();
  return value.map((entry: unknown) => {
    const item = record(entry);
    assert(typeof item.path === "string" && !item.path.startsWith("/") && !item.path.includes("\\"), "Invalid artifact path.");
    assert(!item.path.split("/").some((part) => part === "." || part === ".." || part === ""), "Invalid artifact path.");
    assert(!paths.has(item.path), "Duplicate artifact path.");
    paths.add(item.path);
    assert(typeof item.sha256 === "string" && /^[a-f0-9]{64}$/.test(item.sha256), "Invalid artifact hash.");
    return { path: item.path, sha256: item.sha256 };
  });
}

export function validateBuildOutput(target: WorkerTarget, rootValue: unknown, workerValue: unknown): void {
  const root = record(rootValue);
  const context = record(root.buildContext);
  const worker = record(workerValue);
  const authority = record(record(worker.env).AUTHORITY);
  const authorityExport = record(record(worker.exports).GatewayAuthority);
  assert.equal(root.accountId, WORKER_ACCOUNT_ID, "Build Output account mismatch.");
  // beta.12 checks this only when --mode was supplied. Omission must fail here.
  assert.equal(context.mode, target.mode, "Build Output mode mismatch.");
  assert.equal(context.isPreview, false, "Preview output cannot be deployed.");
  assert.equal(worker.name, target.worker, "Build Output Worker mismatch.");
  assert.equal(worker.compatibilityDate, WORKER_COMPATIBILITY_DATE, "Compatibility date mismatch.");
  assert.deepEqual(worker.compatibilityFlags, WORKER_COMPATIBILITY_FLAGS, "Compatibility flags mismatch.");
  assert.deepEqual(authority, { type: "durable-object", worker: target.worker, exportName: "GatewayAuthority" }, "Authority binding mismatch.");
  assert.deepEqual(authorityExport, { type: "durable-object", storage: "sqlite" }, "Authority namespace mismatch.");
  assert.deepEqual(record(worker.exports).InitialControlInstaller, { type: "worker" }, "Installer export mismatch.");
  assert.equal(worker.workersDev, true);
  assert.equal(worker.previewUrls, false);
}

export function validateReleaseReceipt(value: unknown, target: WorkerTarget, identity: ReleaseIdentity): WorkerReleaseReceipt {
  const receipt = record(value);
  assert(/^[a-f0-9]{40}$/.test(identity.gitSha) && /^[a-f0-9]{40}$/.test(identity.gitTree), "Exact source identity required.");
  for (const [key, expected] of Object.entries({
    version: 2,
    ...identity,
    mode: target.mode,
    worker: target.worker,
    accountId: WORKER_ACCOUNT_ID,
    compatibilityDate: WORKER_COMPATIBILITY_DATE,
  })) {
    assert.equal(receipt[key], expected, `Release ${key} mismatch.`);
  }
  assert.deepEqual(receipt.compatibilityFlags, WORKER_COMPATIBILITY_FLAGS);
  const authority = { worker: target.worker, exportName: "GatewayAuthority" as const, storage: "sqlite" as const };
  assert.deepEqual(receipt.authority, authority, "Release authority mismatch.");
  const tools = { bun: "1.4.0", cf: "1.0.0-beta.12", wrangler: "4.151.1" };
  assert.deepEqual(receipt.tools, tools, "Release tool mismatch.");
  const files = fileDigests(receipt.files);
  const outputFiles = fileDigests(receipt.outputFiles);
  const sourcePaths = new Set(files.map((file) => file.path));
  for (const path of requiredSource) assert(sourcePaths.has(path), `Missing release input: ${path}`);
  const outputPaths = new Set(outputFiles.map((file) => file.path));
  for (const path of ["config.json", "workers/default/worker.config.json"]) assert(outputPaths.has(path), `Missing Build Output: ${path}`);
  return {
    version: 2,
    ...identity,
    mode: target.mode,
    worker: target.worker,
    accountId: WORKER_ACCOUNT_ID,
    compatibilityDate: WORKER_COMPATIBILITY_DATE,
    compatibilityFlags: [...WORKER_COMPATIBILITY_FLAGS],
    authority,
    tools,
    files,
    outputFiles,
  };
}

function git(root: string, args: string[]): string {
  const result = Bun.spawnSync(["git", ...args], { cwd: root });
  assert.equal(result.exitCode, 0, "Git provenance unavailable.");
  return result.stdout.toString();
}

function sourceIdentity(root: string): ReleaseIdentity {
  assert.equal(git(root, ["status", "--porcelain", "--untracked-files=all"]).trim(), "", "Exact-source release requires a clean checkout.");
  return { gitSha: git(root, ["rev-parse", "HEAD"]).trim(), gitTree: git(root, ["rev-parse", "HEAD^{tree}"]).trim() };
}

function sourceFiles(root: string): string[] {
  const tracked = git(root, ["ls-files", "-z", "--", "rust", "static", ...requiredSource]);
  return [...new Set([...tracked.split("\0").filter(Boolean), ...requiredSource])].sort((left, right) => left.localeCompare(right));
}

export function workerReleaseDirectory(root: string, identity: ReleaseIdentity, target: WorkerTarget): string {
  assert(/^[a-f0-9]{40}$/.test(identity.gitSha), "Exact source identity required.");
  return join(root, ".data/rust-worker-build", identity.gitSha, target.mode);
}

async function realDirectories(root: string, directory: string): Promise<void> {
  const path = relative(root, directory);
  assert(!path.startsWith("..") && !path.startsWith("/"), "Artifact directory escaped its root.");
  let current = root;
  assert((await lstat(current)).isDirectory(), "Artifact root links are forbidden.");
  for (const part of path.split("/").filter(Boolean)) {
    current = join(current, part);
    assert((await lstat(current)).isDirectory(), "Artifact directory links are forbidden.");
  }
}

async function digest(path: string): Promise<string> {
  assert((await lstat(path)).isFile(), "Artifact links or special files are forbidden.");
  return createHash("sha256")
    .update(await readFile(path))
    .digest("hex");
}

async function walk(root: string, prefix = ""): Promise<string[]> {
  const files: string[] = [];
  for (const entry of await readdir(join(root, prefix), { withFileTypes: true })) {
    const path = prefix ? `${prefix}/${entry.name}` : entry.name;
    if (entry.isDirectory()) files.push(...(await walk(root, path)));
    else {
      assert(entry.isFile(), "Artifact links or special files are forbidden.");
      files.push(path);
    }
  }
  return files.sort((left, right) => left.localeCompare(right));
}

export async function verifyReleaseFiles(root: string, entries: readonly FileDigest[]): Promise<void> {
  for (const file of entries) {
    let directory = dirname(file.path);
    while (directory !== ".") {
      assert((await lstat(join(root, directory))).isDirectory(), "Artifact directory links are forbidden.");
      directory = dirname(directory);
    }
    assert.equal(await digest(join(root, file.path)), file.sha256, "Prebuilt artifact changed.");
  }
}

async function checkOutput(root: string, target: WorkerTarget): Promise<void> {
  const rootConfig: unknown = JSON.parse(await readFile(join(root, "config.json"), "utf8"));
  const workerConfig: unknown = JSON.parse(await readFile(join(root, "workers/default/worker.config.json"), "utf8"));
  validateBuildOutput(target, rootConfig, workerConfig);
}

export async function publishWorkerRelease(root: string, mode: string): Promise<string> {
  const target = workerTarget(mode);
  const identity = sourceIdentity(root);
  const output = join(root, outputPath);
  await realDirectories(root, output);
  await checkOutput(output, target);
  const release = workerReleaseDirectory(root, identity, target);
  await mkdir(dirname(release), { recursive: true });
  await mkdir(release); // An existing or incomplete target is never overwritten.
  const outputFiles: FileDigest[] = [];
  for (const path of await walk(output)) {
    const destination = join(release, outputPath, path);
    await mkdir(dirname(destination), { recursive: true });
    await copyFile(join(output, path), destination);
    outputFiles.push({ path, sha256: await digest(destination) });
  }
  const files: FileDigest[] = [];
  for (const path of sourceFiles(root)) files.push({ path, sha256: await digest(join(root, path)) });
  const receipt = validateReleaseReceipt(
    {
      version: 2,
      ...identity,
      mode: target.mode,
      worker: target.worker,
      accountId: WORKER_ACCOUNT_ID,
      compatibilityDate: WORKER_COMPATIBILITY_DATE,
      compatibilityFlags: WORKER_COMPATIBILITY_FLAGS,
      authority: { worker: target.worker, exportName: "GatewayAuthority", storage: "sqlite" },
      tools: { bun: "1.4.0", cf: "1.0.0-beta.12", wrangler: "4.151.1" },
      files,
      outputFiles,
    },
    target,
    identity
  );
  await writeFile(join(release, "receipt.json"), JSON.stringify(receipt, null, 2) + "\n", { flag: "wx" });
  return release;
}

export async function verifyWorkerRelease(root: string, mode: string): Promise<string> {
  const target = workerTarget(mode);
  const identity = sourceIdentity(root);
  const release = workerReleaseDirectory(root, identity, target);
  await realDirectories(root, release);
  const value: unknown = JSON.parse(await readFile(join(release, "receipt.json"), "utf8"));
  const receipt = validateReleaseReceipt(value, target, identity);
  assert.deepEqual(
    receipt.files.map((file) => file.path).sort((left, right) => left.localeCompare(right)),
    sourceFiles(root),
    "Release source inventory changed."
  );
  await verifyReleaseFiles(root, receipt.files);
  const output = join(release, outputPath);
  assert((await lstat(output)).isDirectory(), "Build Output directory must be real.");
  await verifyReleaseFiles(output, receipt.outputFiles);
  assert.deepEqual(
    await walk(output),
    receipt.outputFiles.map((file) => file.path).sort((left, right) => left.localeCompare(right)),
    "Build Output inventory changed."
  );
  await checkOutput(output, target);
  assert.equal(resolve(release), release);
  return release;
}

export async function validateDeploymentSecrets(path: string, target: WorkerTarget): Promise<void> {
  const stat = await lstat(path);
  assert(stat.isFile() && (stat.mode & 0o077) === 0, "Deployment secrets require a private regular file.");
  let parsed: unknown;
  try {
    parsed = JSON.parse(await readFile(path, "utf8"));
  } catch {
    throw new Error("Deployment secrets must contain valid JSON.");
  }
  const values = record(parsed);
  const rehearsal = target.mode === "rehearsal-20261106";
  const allowed = rehearsal
    ? ["DENO_DEPLOY_TOKEN", "UOS_STATE_TRANSFER_INTENT_SHA256"]
    : [
        "DENO_DEPLOY_TOKEN",
        "UOS_AI_TOKEN",
        "DEEPSEEK_API_KEY",
        "CEREBRAS_API_KEY",
        "LITHOSAI_API_KEY",
        "OPENROUTER_API_KEY",
        "SURPLUS_API_KEY",
        "METERED_API_KEY",
        "CLAUDE_API_KEY",
        "UOS_AI_PASSPORT_CLIENT_ID",
        "UOS_AI_PASSPORT_ORIGIN",
        "UOS_AI_PASSPORT_REDIRECT_URI",
        "UOS_NATIVE_CARRIER",
        "UOS_STATE_TRANSFER_INTENT_SHA256",
      ];
  for (const [name, value] of Object.entries(values)) {
    assert(allowed.includes(name), "Secret is outside the selected target's allowed bindings.");
    assert(typeof value === "string" && value.length > 0, "Deployment secret values must be nonempty strings.");
  }
  if (rehearsal) {
    assert(typeof values.DENO_DEPLOY_TOKEN === "string", "Rehearsal requires its admin binding.");
    assert(
      typeof values.UOS_STATE_TRANSFER_INTENT_SHA256 === "string" && /^[a-f0-9]{64}$/.test(values.UOS_STATE_TRANSFER_INTENT_SHA256),
      "Rehearsal requires the existing transfer intent digest."
    );
  }
}
