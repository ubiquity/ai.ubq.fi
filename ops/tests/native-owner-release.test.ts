import { test } from "bun:test";
import assert from "node:assert/strict";
import { chmod, lstat, mkdir, mkdtemp, readFile, rm, symlink, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import {
  nativeOwnerDigest,
  NATIVE_OWNER_FILES,
  parseNativeOwnerManifest,
  publishNativeOwnerPayload,
  resolveNativeOwnerPayload,
  verifyNativeOwnerPayload,
} from "../native-owner-release.ts";
import { parseNativeOwnerConfig } from "../native-owner-config.ts";
import { ownerConfig } from "./native-owner-config.test.ts";

const seed = async () => {
  const root = await mkdtemp(join(tmpdir(), "native-ops-"));
  const sha = "a".repeat(40);
  const payload = join(root, ".data/releases", sha, "native-owner");
  await mkdir(join(payload, "bin"), { recursive: true });
  await mkdir(join(payload, "ops"));
  const files = [];
  for (const path of NATIVE_OWNER_FILES) {
    const bytes = Buffer.from("synthetic " + path);
    await writeFile(join(payload, path), bytes, { mode: path === "bin/uos-native" ? 0o500 : 0o400 });
    files.push({ path, sha256: nativeOwnerDigest(bytes), bytes: bytes.length });
  }
  const manifest = {
    v: 1,
    role: "native_owner_service_release",
    git_sha: sha,
    git_tree: "b".repeat(40),
    cargo_lock_sha256: "d".repeat(64),
    build_receipt_sha256: "e".repeat(64),
    target: "aarch64-unknown-linux-gnu",
    profile: { name: "release", test: false, opt_level: "s", debug_assertions: false, fixture_disabled: true },
    files,
  };
  const receipt = JSON.stringify({
    v: 1,
    git_sha: sha,
    git_tree: manifest.git_tree,
    cargo_lock_sha256: manifest.cargo_lock_sha256,
    binary_sha256: files.find((file) => file.path === "bin/uos-native")?.sha256,
    target_name: "uos-native",
    target_kind: "bin",
    profile: { test: false, opt_level: "s", debug_assertions: false },
    features: ["default"],
  });
  manifest.build_receipt_sha256 = nativeOwnerDigest(receipt);
  await writeFile(join(payload, ".uos-native-owner-build.json"), receipt, { mode: 0o400 });
  const text = JSON.stringify(manifest);
  await writeFile(join(payload, ".uos-native-owner-release.json"), text, { mode: 0o400 });
  for (const dir of ["bin", "ops", ""]) await chmod(join(payload, dir), 0o500);
  const config = ownerConfig();
  config.release.manifest_sha256 = nativeOwnerDigest(text);
  return { root, payload, manifest, config: parseNativeOwnerConfig(config) };
};
test("native role refuses passive/debug/test/fixture artifacts", async () => {
  const f = await seed();
  try {
    parseNativeOwnerManifest(f.manifest);
    for (const patch of [
      { role: "PRIVATE_PASSIVE_PROBE" },
      { profile: { ...f.manifest.profile, test: true } },
      { profile: { ...f.manifest.profile, opt_level: "0" } },
      { profile: { ...f.manifest.profile, fixture_disabled: false } },
    ])
      assert.throws(() => parseNativeOwnerManifest({ ...f.manifest, ...patch }));
  } finally {
    for (const d of ["", "bin", "ops"]) await chmod(join(f.payload, d), 0o700);
    await rm(f.root, { recursive: true });
  }
});
test("selector resolves once and corrupt writable symlinked payload refuses", async () => {
  const f = await seed();
  try {
    await symlink(f.payload, join(f.root, ".data/native-owner-current"));
    const selected = await resolveNativeOwnerPayload(f.root);
    assert.equal(selected, f.payload);
    await verifyNativeOwnerPayload(selected, f.config);
    await rm(join(f.root, ".data/native-owner-current"));
    await symlink("/wrong", join(f.root, ".data/native-owner-current"));
    await verifyNativeOwnerPayload(selected, f.config);
    await chmod(join(f.payload, "bin/uos-native"), 0o700);
    await assert.rejects(verifyNativeOwnerPayload(selected, f.config));
    await chmod(join(f.payload, "bin/uos-native"), 0o500);
    await chmod(join(f.payload, "bin"), 0o700);
    await rm(join(f.payload, "bin/uos-native"));
    await symlink("/bin/true", join(f.payload, "bin/uos-native"));
    await assert.rejects(verifyNativeOwnerPayload(selected, f.config));
  } finally {
    for (const d of ["", "bin", "ops"]) await chmod(join(f.payload, d), 0o700);
    await rm(f.root, { recursive: true });
  }
});
test("publication refuses preexisting release without deleting or overwriting it", async () => {
  const f = await seed();
  const destination = await mkdtemp(join(tmpdir(), "native-publish-"));
  await mkdir(join(destination, ".data/releases", f.config.release.git_sha), { recursive: true });
  await writeFile(join(destination, ".data/releases", f.config.release.git_sha, "preserve"), "owner data");
  try {
    await assert.rejects(publishNativeOwnerPayload(destination, f.payload, f.config));
    assert.equal(await readFile(join(destination, ".data/releases", f.config.release.git_sha, "preserve"), "utf8"), "owner data");
    assert.equal((await lstat(f.payload)).isDirectory(), true);
  } finally {
    for (const d of ["", "bin", "ops"]) await chmod(join(f.payload, d), 0o700);
    await rm(f.root, { recursive: true });
    await rm(destination, { recursive: true });
  }
});

test("publication creates only a new verified immutable payload", async () => {
  const f = await seed();
  const destination = await mkdtemp(join(tmpdir(), "native-publish-ok-"));
  let published: string | undefined;
  try {
    published = await publishNativeOwnerPayload(destination, f.payload, f.config);
    await verifyNativeOwnerPayload(published, f.config);
    assert.equal((await lstat(published)).mode & 0o222, 0);
    assert.equal((await lstat(join(published, "bin/uos-native"))).nlink, 1);
    assert.equal(await readFile(join(published, "bin/uos-native"), "utf8"), "synthetic bin/uos-native");
  } finally {
    for (const d of ["", "bin", "ops"]) await chmod(join(f.payload, d), 0o700);
    await rm(f.root, { recursive: true });
    if (published) {
      await chmod(join(destination, ".data/releases", f.config.release.git_sha), 0o700);
      for (const d of ["", "bin", "ops"]) await chmod(join(published, d), 0o700);
    }
    await rm(destination, { recursive: true });
  }
});
