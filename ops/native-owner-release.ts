import { createHash } from "node:crypto";
import { chmod, copyFile, lstat, mkdir, readFile, readlink, realpath, rename, rm } from "node:fs/promises";
import { dirname, isAbsolute, join, relative, sep } from "node:path";
import { nativeOwnerRecord, type NativeOwnerConfig } from "./native-owner-config.ts";

export const NATIVE_OWNER_BUN_SHA256 = "086c4121c8738a8e0f5ed730e8a461bc3973b4444e372ddb77aef9a747fa2ae9";
export const NATIVE_OWNER_FILES = ["bin/uos-native", "ops/launch-native-owner.ts", "ops/native-owner-config.ts", "ops/native-owner-release.ts"] as const;
export type NativeOwnerManifest = Readonly<{
  v: 1;
  role: "native_owner_service_release";
  git_sha: string;
  git_tree: string;
  cargo_lock_sha256: string;
  build_receipt_sha256: string;
  target: "aarch64-unknown-linux-gnu";
  profile: { name: "release"; test: false; opt_level: "s"; debug_assertions: false; fixture_disabled: true };
  files: readonly { path: string; sha256: string; bytes: number }[];
}>;
export const nativeOwnerDigest = (bytes: string | Uint8Array): string => createHash("sha256").update(bytes).digest("hex");
const invalid = (): never => {
  throw new Error("Native owner immutable release is invalid");
};
const hash = (value: unknown, size: number): string =>
  typeof value === "string" && new RegExp("^[0-9a-f]{" + String(size) + "}$").test(value) ? value : invalid();
export const parseNativeOwnerManifest = (value: unknown): NativeOwnerManifest => {
  const row = nativeOwnerRecord(value, ["v", "role", "git_sha", "git_tree", "cargo_lock_sha256", "build_receipt_sha256", "target", "profile", "files"]);
  const profile = nativeOwnerRecord(row.profile, ["name", "test", "opt_level", "debug_assertions", "fixture_disabled"]);
  if (
    row.v !== 1 ||
    row.role !== "native_owner_service_release" ||
    row.target !== "aarch64-unknown-linux-gnu" ||
    profile.name !== "release" ||
    profile.test !== false ||
    profile.opt_level !== "s" ||
    profile.debug_assertions !== false ||
    profile.fixture_disabled !== true ||
    !Array.isArray(row.files)
  )
    return invalid();
  const files = row.files.map((value) => {
    const file = nativeOwnerRecord(value, ["path", "sha256", "bytes"]);
    if (typeof file.path !== "string" || !NATIVE_OWNER_FILES.some((path) => path === file.path) || !Number.isSafeInteger(file.bytes) || Number(file.bytes) < 1)
      return invalid();
    return { path: file.path, sha256: hash(file.sha256, 64), bytes: Number(file.bytes) };
  });
  if (files.length !== NATIVE_OWNER_FILES.length || new Set(files.map((file) => file.path)).size !== NATIVE_OWNER_FILES.length) return invalid();
  return {
    v: 1,
    role: "native_owner_service_release",
    git_sha: hash(row.git_sha, 40),
    git_tree: hash(row.git_tree, 40),
    cargo_lock_sha256: hash(row.cargo_lock_sha256, 64),
    build_receipt_sha256: hash(row.build_receipt_sha256, 64),
    target: "aarch64-unknown-linux-gnu",
    profile: { name: "release", test: false, opt_level: "s", debug_assertions: false, fixture_disabled: true },
    files,
  };
};
export const readNativeOwnerFile = async (path: string, maximum: number, executable = false): Promise<Buffer> => {
  const before = await lstat(path);
  if (!before.isFile() || before.isSymbolicLink() || before.nlink !== 1 || before.size > maximum || before.uid !== process.getuid?.()) return invalid();
  if ((before.mode & 0o222) !== 0 || (executable && (before.mode & 0o111) === 0)) return invalid();
  if ((await realpath(path)) !== path) return invalid();
  const bytes = await readFile(path);
  const after = await lstat(path);
  if (before.dev !== after.dev || before.ino !== after.ino || before.size !== after.size || before.mtimeMs !== after.mtimeMs) return invalid();
  return bytes;
};
export const resolveNativeOwnerPayload = async (root: string): Promise<string> => {
  const selector = join(root, ".data/native-owner-current");
  const before = await lstat(selector);
  if (!before.isSymbolicLink()) return invalid();
  const target = await readlink(selector);
  const payload = await realpath(isAbsolute(target) ? target : join(dirname(selector), target));
  const suffix = relative(join(root, ".data/releases"), payload);
  if (!/^[0-9a-f]{40}\/native-owner$/.test(suffix.split(sep).join("/"))) return invalid();
  const after = await lstat(selector);
  if (before.dev !== after.dev || before.ino !== after.ino || target !== (await readlink(selector))) return invalid();
  return payload;
};
const verifyBuildReceipt = async (payload: string, manifest: NativeOwnerManifest): Promise<void> => {
  const bytes = await readNativeOwnerFile(join(payload, ".uos-native-owner-build.json"), 65_536);
  if (nativeOwnerDigest(bytes) !== manifest.build_receipt_sha256) return invalid();
  const row = nativeOwnerRecord(JSON.parse(bytes.toString("utf8")), [
    "v",
    "git_sha",
    "git_tree",
    "cargo_lock_sha256",
    "binary_sha256",
    "target_name",
    "target_kind",
    "profile",
    "features",
  ]);
  const profile = nativeOwnerRecord(row.profile, ["test", "opt_level", "debug_assertions"]);
  const binary = manifest.files.find((file) => file.path === "bin/uos-native");
  if (
    row.v !== 1 ||
    row.git_sha !== manifest.git_sha ||
    row.git_tree !== manifest.git_tree ||
    row.cargo_lock_sha256 !== manifest.cargo_lock_sha256 ||
    row.binary_sha256 !== binary?.sha256 ||
    row.target_name !== "uos-native" ||
    row.target_kind !== "bin" ||
    profile.test !== false ||
    profile.opt_level !== "s" ||
    profile.debug_assertions !== false ||
    !Array.isArray(row.features) ||
    row.features.some((feature) => typeof feature !== "string" || feature === "native-import-fixture")
  )
    return invalid();
};
export const verifyNativeOwnerPayload = async (payload: string, config: NativeOwnerConfig): Promise<NativeOwnerManifest> => {
  for (const directory of [payload, join(payload, "bin"), join(payload, "ops")]) {
    const info = await lstat(directory);
    if (!info.isDirectory() || info.isSymbolicLink() || info.uid !== process.getuid?.() || (info.mode & 0o222) !== 0) return invalid();
  }
  const manifestBytes = await readNativeOwnerFile(join(payload, ".uos-native-owner-release.json"), 65_536);
  if (nativeOwnerDigest(manifestBytes) !== config.release.manifest_sha256) return invalid();
  const manifest = parseNativeOwnerManifest(JSON.parse(manifestBytes.toString("utf8")));
  if (manifest.git_sha !== config.release.git_sha || manifest.git_tree !== config.release.git_tree || dirname(payload).split(sep).at(-1) !== manifest.git_sha)
    return invalid();
  await verifyBuildReceipt(payload, manifest);
  for (const file of manifest.files) {
    const bytes = await readNativeOwnerFile(join(payload, file.path), file.bytes, file.path === "bin/uos-native");
    if (bytes.length !== file.bytes || nativeOwnerDigest(bytes) !== file.sha256) return invalid();
  }
  return manifest;
};
export const nativeOwnerRuntime = (root: string): string => join(root, ".data/runtimes/bun/linux-arm64", "1.4.0-" + NATIVE_OWNER_BUN_SHA256, "bun");

/** Preparation only. Refuses an existing SHA release; never called by launch. */
export const publishNativeOwnerPayload = async (root: string, prepared: string, config: NativeOwnerConfig): Promise<string> => {
  await verifyNativeOwnerPayload(prepared, config);
  const destination = join(root, ".data/releases", config.release.git_sha);
  const staging = join(root, ".data/releases", ".native-owner-" + crypto.randomUUID());
  await mkdir(join(staging, "native-owner/bin"), { recursive: true, mode: 0o700 });
  await mkdir(join(staging, "native-owner/ops"), { mode: 0o700 });
  try {
    for (const name of [...NATIVE_OWNER_FILES, ".uos-native-owner-build.json", ".uos-native-owner-release.json"]) {
      const file = join(staging, "native-owner", name);
      await copyFile(join(prepared, name), file);
      await chmod(file, name === "bin/uos-native" ? 0o500 : 0o400);
    }
    await chmod(join(staging, "native-owner/bin"), 0o500);
    await chmod(join(staging, "native-owner/ops"), 0o500);
    await chmod(join(staging, "native-owner"), 0o500);
    await chmod(staging, 0o500);
    await mkdir(destination, { mode: 0o700 });
    await rename(join(staging, "native-owner"), join(destination, "native-owner"));
    await chmod(destination, 0o500);
    return join(destination, "native-owner");
  } finally {
    for (const directory of [staging, join(staging, "native-owner"), join(staging, "native-owner/bin"), join(staging, "native-owner/ops")])
      await chmod(directory, 0o700).catch(() => undefined);
    await rm(staging, { recursive: true, force: true });
  }
};
