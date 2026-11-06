import { lstat } from "node:fs/promises";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
import { BUN_RUNTIMES, runtimePath, verifyRuntimeBinary } from "./bun-runtime.ts";
import type { VpsCommand } from "./vps-release-store.ts";

/** Portable physical fence; unsafe canonical entries never authorize a local writer. */
export const assertBunStoreFile = async (root: string): Promise<void> => {
  const info = await lstat(join(root, ".data/kv-bun.sqlite3"));
  if (!info.isFile() || info.isSymbolicLink()) throw new Error("Local Bun authority requires its regular non-symlink database");
};
const verifier = 'const m=await import(process.argv[1]);await m.assertBunConvertedStore(process.argv[2]);console.log("uos-bun-authority-v1");';
export const assertBunServingAuthority = async (root: string, run: VpsCommand): Promise<void> => {
  await assertBunStoreFile(root);
  if (Reflect.has(globalThis, "Bun")) {
    const { assertBunConvertedStore } = await import("./assert-bun-store.ts");
    await assertBunConvertedStore(root);
    return;
  }
  const artifact = BUN_RUNTIMES.linux;
  const runtime = runtimePath(root, artifact);
  try {
    await verifyRuntimeBinary(runtime, artifact);
    const result = await run(runtime, ["--no-env-file", "--no-install", "-e", verifier, pathToFileURL(join(root, "ops/assert-bun-store.ts")).href, root], root);
    if (result.trim() !== "uos-bun-authority-v1") throw new Error("Missing read-only authority verification");
  } catch {
    throw new Error("Bun authority codec verification is unsupported by this bound mutation runner");
  }
};
