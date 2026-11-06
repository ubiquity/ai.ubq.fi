import assert from "node:assert/strict";
import { afterAll, test } from "bun:test";
import { createHash } from "node:crypto";
import { chmod, mkdir, mkdtemp, rm, symlink, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import {
  workerTarget,
  workerTargetArguments,
  workerDeploymentArguments,
  WORKER_ACCOUNT_ID,
  WORKER_COMPATIBILITY_DATE,
  WORKER_COMPATIBILITY_FLAGS,
} from "../cloudflare-targets.ts";
import {
  validateBuildOutput,
  validateDeploymentSecrets,
  validateReleaseReceipt,
  verifyReleaseFiles,
  workerReleaseDirectory,
} from "../tools/cloudflare/worker-release.ts";
import type { WorkerReleaseReceipt } from "../tools/cloudflare/worker-release.ts";

// The Cloudflare config has its own strict Worker project; execute its factory
// through a runtime boundary so the Bun/Node test project does not import Worker globals.
type ConfigurationFactory = (context: { isPreview: boolean; mode: string | undefined }) => {
  accountId: string;
  worker: { name: string; env: Record<string, unknown>; exports: Record<string, unknown> };
};
const loaded: unknown = await import(new URL("../cloudflare.config.ts", import.meta.url).href);
assert(typeof loaded === "object" && loaded !== null && "default" in loaded && typeof loaded.default === "function");
const configuration = loaded.default as ConfigurationFactory;

const temporary = await mkdtemp(join(tmpdir(), "uos-worker-targets-"));
afterAll(async () => rm(temporary, { recursive: true, force: true }));
const identity = { gitSha: "1".repeat(40), gitTree: "2".repeat(40) };
const hash = (value: string): string => createHash("sha256").update(value).digest("hex");
const modes = ["candidate", "rehearsal-20261106", "production-20261106"] as const;
const sourcePaths = [
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

function receipt(mode = "candidate"): WorkerReleaseReceipt {
  const target = workerTarget(mode);
  return {
    version: 2,
    ...identity,
    mode: target.mode,
    worker: target.worker,
    accountId: WORKER_ACCOUNT_ID,
    compatibilityDate: WORKER_COMPATIBILITY_DATE,
    compatibilityFlags: [...WORKER_COMPATIBILITY_FLAGS],
    authority: { worker: target.worker, exportName: "GatewayAuthority", storage: "sqlite" },
    tools: { bun: "1.4.0", cf: "1.0.0-beta.12", wrangler: "4.151.1" },
    files: sourcePaths.map((path) => ({ path, sha256: hash(path) })),
    outputFiles: ["config.json", "workers/default/worker.config.json"].map((path) => ({ path, sha256: hash(path) })),
  };
}

test("real configuration defaults to candidate and gives each explicit mode a self-bound SQLite authority", () => {
  assert.equal(configuration({ isPreview: false, mode: undefined }).worker.name, "uos-ai-rust-candidate");
  for (const mode of modes) {
    const target = workerTarget(mode);
    const config = configuration({ isPreview: false, mode });
    assert.equal(config.worker.name, target.worker);
    validateBuildOutput(target, { accountId: config.accountId, buildContext: { mode, isPreview: false } }, config.worker);
  }
  assert.throws(() => configuration({ isPreview: false, mode: "production" }), /Unknown Rust Worker mode/);
});

test("only the existing cf mode flag selects a target; unknown, missing or duplicate inputs fail", () => {
  assert.equal(workerTargetArguments([]).mode, "candidate");
  assert.equal(workerTargetArguments(["--mode", modes[1]]).mode, modes[1]);
  assert.equal(workerTargetArguments([`--mode=${modes[2]}`]).mode, modes[2]);
  for (const args of [["--mode"], ["--mode="], ["--name", "other"], ["--mode", "candidate", "--mode", modes[1]]]) {
    assert.throws(() => workerTargetArguments(args));
  }
});

test("Build Output rejects omitted mode and cross-target or altered deployment capabilities", () => {
  const target = workerTarget(modes[1]);
  const config = configuration({ isPreview: false, mode: target.mode });
  const context = { accountId: WORKER_ACCOUNT_ID, buildContext: { mode: target.mode, isPreview: false } };
  assert.throws(() => {
    validateBuildOutput(target, { ...context, buildContext: { isPreview: false } }, config.worker);
  }, /mode mismatch/);
  assert.throws(() => {
    validateBuildOutput(target, { ...context, accountId: "different" }, config.worker);
  }, /account mismatch/);
  assert.throws(() => {
    validateBuildOutput(target, { ...context, buildContext: { mode: target.mode, isPreview: true } }, config.worker);
  }, /Preview/);
  for (const worker of [
    { ...config.worker, name: workerTarget().worker },
    { ...config.worker, compatibilityDate: "2026-01-01" },
    { ...config.worker, compatibilityFlags: [] },
    { ...config.worker, env: { ...config.worker.env, AUTHORITY: { type: "durable-object", worker: workerTarget().worker, exportName: "GatewayAuthority" } } },
    { ...config.worker, exports: { ...config.worker.exports, GatewayAuthority: { type: "durable-object", storage: "legacy-kv" } } },
    { ...config.worker, exports: { GatewayAuthority: config.worker.exports.GatewayAuthority } },
  ])
    assert.throws(() => {
      validateBuildOutput(target, context, worker);
    });
});

test("receipt validation rejects stale source and profile swaps before artifact access", () => {
  for (const mode of modes) validateReleaseReceipt(receipt(mode), workerTarget(mode), identity);
  assert.throws(() => validateReleaseReceipt(receipt(modes[1]), workerTarget(modes[2]), identity), /mode mismatch/);
  for (const changed of [
    { ...receipt(), version: 1 },
    { ...receipt(), gitSha: "3".repeat(40) },
    { ...receipt(), gitTree: "4".repeat(40) },
    { ...receipt(), accountId: "different" },
    { ...receipt(), worker: "different" },
    { ...receipt(), compatibilityDate: "2026-01-01" },
    { ...receipt(), compatibilityFlags: [] },
    { ...receipt(), authority: { worker: "other", exportName: "GatewayAuthority", storage: "sqlite" } },
    { ...receipt(), tools: { bun: "1.4.0", cf: "latest", wrangler: "4.151.1" } },
    { ...receipt(), files: receipt().files.filter((file) => file.path !== "cloudflare-targets.ts") },
    { ...receipt(), outputFiles: [] },
  ])
    assert.throws(() => validateReleaseReceipt(changed, workerTarget(), identity));
});

test("manifest paths, duplicate files and malformed hashes fail closed", () => {
  for (const file of [
    { path: "../escape", sha256: hash("x") },
    { path: "/absolute", sha256: hash("x") },
    { path: "a\\b", sha256: hash("x") },
    { path: "a/./b", sha256: hash("x") },
    { path: "a//b", sha256: hash("x") },
    { path: "normal", sha256: "bad" },
  ])
    assert.throws(() => validateReleaseReceipt({ ...receipt(), files: [...receipt().files, file] }, workerTarget(), identity));
  assert.throws(() => validateReleaseReceipt({ ...receipt(), files: [...receipt().files, ...receipt().files] }, workerTarget(), identity));
});

test("artifact verification detects modified bytes, direct links and directory links", async () => {
  const root = join(temporary, "manifest");
  await mkdir(root);
  await writeFile(join(root, "artifact"), "accepted");
  await verifyReleaseFiles(root, [{ path: "artifact", sha256: hash("accepted") }]);
  await assert.rejects(verifyReleaseFiles(root, [{ path: "artifact", sha256: hash("changed") }]), /artifact changed/);
  await symlink(join(root, "artifact"), join(root, "linked"));
  await assert.rejects(verifyReleaseFiles(root, [{ path: "linked", sha256: hash("accepted") }]), /links/);
  await symlink(root, join(root, "directory"));
  await assert.rejects(verifyReleaseFiles(root, [{ path: "directory/artifact", sha256: hash("accepted") }]), /directory links/);
});

test("same-source mode snapshots have separate immutable destinations", () => {
  const directories = modes.map((mode) => workerReleaseDirectory(temporary, identity, workerTarget(mode)));
  assert.equal(new Set(directories).size, modes.length);
  for (const [index, directory] of directories.entries()) assert(directory.endsWith(`/${identity.gitSha}/${modes[index]}`));
  assert.throws(() => workerReleaseDirectory(temporary, { ...identity, gitSha: "HEAD" }, workerTarget()), /Exact source/);
});

test("rehearsal declares only admin auth while candidate and production retain all twelve", () => {
  const rehearsal = configuration({ isPreview: false, mode: modes[1] });
  assert.deepEqual(
    Object.keys(rehearsal.worker.env).sort((a, b) => a.localeCompare(b)),
    ["ASSETS", "AUTHORITY", "DENO_DEPLOY_TOKEN"]
  );
  for (const mode of [modes[0], modes[2]]) assert.equal(Object.keys(configuration({ isPreview: false, mode }).worker.env).length, 14);
  assert.equal(workerDeploymentArguments(["--mode", modes[1], "--secrets-file", "private.json"]).secretsFile, "private.json");
  assert.throws(() => workerDeploymentArguments(["--secrets-file", "a", "--secrets-file", "b"]));
});

test("private rehearsal bootstrap accepts admin plus intent and refuses unrelated credentials or public files", async () => {
  const path = join(temporary, "bootstrap.json");
  const minimal = { DENO_DEPLOY_TOKEN: hash("fixture admin"), UOS_STATE_TRANSFER_INTENT_SHA256: hash("fixture intent") };
  await writeFile(path, JSON.stringify(minimal), { mode: 0o600 });
  await validateDeploymentSecrets(path, workerTarget(modes[1]));
  await writeFile(path, JSON.stringify({ ...minimal, OPENROUTER_API_KEY: hash("fixture provider") }));
  await assert.rejects(validateDeploymentSecrets(path, workerTarget(modes[1])), /outside/);
  await writeFile(path, JSON.stringify({ DENO_DEPLOY_TOKEN: minimal.DENO_DEPLOY_TOKEN }));
  await assert.rejects(validateDeploymentSecrets(path, workerTarget(modes[1])), /intent/);
  await writeFile(path, "invalid private payload");
  await assert.rejects(validateDeploymentSecrets(path, workerTarget(modes[1])), /^Error: Deployment secrets must contain valid JSON\.$/);
  const publicPath = join(temporary, "public-bootstrap.json");
  await writeFile(publicPath, JSON.stringify(minimal), { mode: 0o644 });
  await chmod(publicPath, 0o644);
  await assert.rejects(validateDeploymentSecrets(publicPath, workerTarget(modes[1])), /private regular/);
});
