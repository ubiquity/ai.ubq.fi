import { test } from "bun:test";
import assert from "node:assert/strict";
import { Database } from "bun:sqlite";
import { lstat, mkdir, mkdtemp, readFile, rename, rm, symlink, writeFile } from "node:fs/promises";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { assertBunConvertedStore } from "../assert-bun-store.ts";
import { assertBunStoreFile } from "../assert-serving-authority.ts";
import { exchangeExistingAuthorityPaths } from "../authority-exchange.ts";
import { composeCloudflareAuthorityIngress } from "../authority-ingress.ts";
import { makeVpsDriverFixture, mutation } from "./vps-release-driver-fixture.ts";
import { withVpsDeployLock } from "../vps-release-journal.ts";
const root = async (): Promise<string> => {
  const path = await mkdtemp(join(tmpdir(), "authority-"));
  await mkdir(join(path, ".data"));
  return path;
};
test("canonical physical fence rejects missing, symlink and directory even with malformed receipt", async () => {
  const path = await root(),
    db = join(path, ".data/kv-bun.sqlite3");
  try {
    await assert.rejects(assertBunStoreFile(path));
    await writeFile(join(path, "other"), "not a database");
    await symlink(join(path, "other"), db);
    await assert.rejects(assertBunStoreFile(path));
    await rm(db);
    await mkdir(db, { mode: 0o700 });
    await writeFile(join(db, "authority.json"), "malformed");
    await assert.rejects(assertBunConvertedStore(path));
    await rm(db, { recursive: true });
    const store = new Database(db);
    store.run("CREATE TABLE uos_kv_metadata(key TEXT PRIMARY KEY,value TEXT)");
    store.run("INSERT INTO uos_kv_metadata VALUES('codec','bun-jsc-v1')");
    store.close();
    await assertBunConvertedStore(path);
  } finally {
    await rm(path, { recursive: true });
  }
});
test("actual fixed OS exchange preserves both inode identities without absent canonical path", async () => {
  const path = await root(),
    file = join(path, "file"),
    directory = join(path, "fence");
  await writeFile(file, "LATEST_SYNTHETIC_INVERSE");
  await mkdir(directory, { mode: 0o700 });
  await writeFile(join(directory, "authority.json"), "synthetic-receipt");
  const beforeFile = await lstat(file),
    beforeDirectory = await lstat(directory);
  try {
    await exchangeExistingAuthorityPaths(file, directory);
    assert.equal((await lstat(file)).ino, beforeDirectory.ino);
    assert.equal((await lstat(directory)).ino, beforeFile.ino);
    assert.equal(await readFile(directory, "utf8"), "LATEST_SYNTHETIC_INVERSE");
    assert.equal(await readFile(join(file, "authority.json"), "utf8"), "synthetic-receipt");
    await assert.rejects(exchangeExistingAuthorityPaths(file, directory));
  } finally {
    await rm(path, { recursive: true });
  }
});
test("bound driver rechecks after acquiring lock and refuses before deployment commands", async () => {
  const f = await makeVpsDriverFixture();
  let release: () => void = () => undefined;
  let entered: () => void = () => undefined;
  const acquired = new Promise<void>((resolve) => {
    entered = resolve;
  });
  const hold = new Promise<void>((resolve) => {
    release = resolve;
  });
  const blocker = withVpsDeployLock(f.root, async () => {
    entered();
    await hold;
  });
  try {
    await acquired;
    const pending = f.prepare();
    const db = join(f.root, ".data/kv-bun.sqlite3");
    await rename(db, db + ".retained");
    await mkdir(db, { mode: 0o700 });
    await writeFile(join(db, "authority.json"), "synthetic");
    release();
    await blocker;
    await assert.rejects(pending, /authority|regular/i);
    assert.equal(f.system.calls.length, 0);
    await f.driver.inspectPrior();
  } finally {
    release();
    await blocker;
    await f.cleanup();
  }
});
test("bound promote and rollback cannot mutate selector unit or ingress while fenced", async () => {
  const f = await makeVpsDriverFixture();
  try {
    const prepared = await f.prepare();
    const calls = f.system.calls.length;
    const db = join(f.root, ".data/kv-bun.sqlite3");
    await rename(db, db + ".retained");
    await mkdir(db, { mode: 0o700 });
    await assert.rejects(f.driver.activate(await mutation(prepared)), /authority|regular/i);
    await assert.rejects(f.driver.restore(await mutation(prepared)), /authority|regular/i);
    assert.equal(f.system.calls.length, calls);
  } finally {
    await f.cleanup();
  }
});
test("both stale AI ingress paths fail closed and native handlers precede AI regardless of header", () => {
  const publicAi = "ai.ubq.fi {\n reverse_proxy 127.0.0.1:7999 {\n flush_interval -1\n }\n}\n";
  const origin =
    "origin.ubq.fi {\n @ai header X-Forwarded-Host ai.ubq.fi\n handle @ai {\n reverse_proxy 127.0.0.1:7999 {\n header_up Host ai.ubq.fi\n }\n }\n handle { respond notfound 404 }\n}\n";
  const result = composeCloudflareAuthorityIngress(publicAi, origin);
  assert(!result.publicAi.includes("127.0.0.1:7999"));
  assert(!result.origin.includes("127.0.0.1:7999"));
  assert(result.publicAi.includes("503"));
  assert(result.origin.includes("503"));
  assert(result.origin.indexOf("handle @uos_authority_native") < result.origin.indexOf("handle @ai"));
  assert(!result.origin.includes("handle_path"));
  assert(!result.origin.includes("flush_interval"));
});
