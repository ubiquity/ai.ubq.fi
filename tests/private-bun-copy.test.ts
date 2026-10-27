import assert from "node:assert/strict";
import { afterEach, test } from "bun:test";
import { Database } from "bun:sqlite";
import { createHash } from "node:crypto";
import { chmodSync, existsSync, linkSync, lstatSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { dirname, join } from "node:path";
import { serialize } from "node:v8";
import { writePrivateBunCopyCapture as boundCopyCapture, type PrivateBunCopyInput } from "../tools/cloudflare/private-bun-copy.ts";
import { capturePrivateNativeState } from "../rust/crates/gateway-migration/examples/native-operator-capture.ts";
import { packStringKey } from "../src/kv-conversion/cursors.ts";
import { privateBunCopyTestRuntime } from "./fixtures/private-bun-copy-runtime.ts";
import { NATIVE_DDL } from "../src/kv-conversion/native-schema.ts";
import { encodeNativeCaptureValue } from "../src/kv-conversion/typed-value.ts";

const SOURCE_SHA = "b".repeat(40);
const SENTINEL = "PRIVATE_CAPTURE_SENTINEL_NOT_STDOUT";
const owned: string[] = [];
const writePrivateBunCopyCapture = (value: PrivateBunCopyInput, signal?: AbortSignal) =>
  boundCopyCapture(value, signal, privateBunCopyTestRuntime(dirname(value.source)));
afterEach(() => {
  for (const path of owned.splice(0)) rmSync(path, { recursive: true, force: true });
});

function fixture() {
  const directory = realpathSync(mkdtempSync(join(tmpdir(), "uos-private-copy-")));
  chmodSync(directory, 0o700);
  owned.push(directory);
  const source = join(directory, "source.sqlite3");
  const database = new Database(source, { create: true, strict: true, safeIntegers: true });
  database.run("PRAGMA journal_mode=WAL");
  for (const ddl of NATIVE_DDL) database.run(ddl);
  database.run("CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID");
  database.run("INSERT INTO uos_kv_metadata VALUES('codec','bun-jsc-v1')");
  database.run("INSERT INTO migration_state VALUES(0,3)");
  database.run("INSERT INTO data_version VALUES(0,10,0)");
  const backing = new ArrayBuffer(16);
  new Uint8Array(backing).set([0, 1, 2, 3, 4, 5, 6, 7]);
  const view = Buffer.from(backing, 3, 4);
  const graph: Record<string, unknown> = { text: SENTINEL, lone: "\ud83d", backing, view, alias: view };
  graph.self = graph;
  const key = packStringKey(["default", "copy-graph"]);
  database.run("INSERT INTO kv VALUES(?,?,?,?,?,?)", [key, serialize(graph), 1n, 9n, 0n, 9223372036854775807n]);
  database.run("INSERT INTO kv VALUES(?,?,?,?,?,?)", [packStringKey(["default", "counter"]), new Uint8Array(8).fill(255), 2n, 8n, 0n, -1n]);
  database.run("INSERT INTO kv VALUES(?,?,?,?,?,?)", [packStringKey(["default", "bytes"]), new Uint8Array([0, 255]), 3n, 8n, 1n, 9500n]);
  chmodSync(source, 0o600);
  const input: PrivateBunCopyInput = { v: 1, source, directory: join(directory, "run"), source_git_sha: SOURCE_SHA };
  return { directory, source, database, graph, key, input };
}

test("an unbound copy operator refuses before creating a destination", async () => {
  const f = fixture();
  try {
    await assert.rejects(boundCopyCapture(f.input), /refused/);
    assert.equal(existsSync(f.input.directory), false);
  } finally {
    f.database.close(true);
  }
});

test("online readonly backup includes committed WAL while a synthetic source writer remains open and usable", async () => {
  const f = fixture();
  try {
    assert(lstatSync(f.source + "-wal").size > 0);
    f.database.run("BEGIN IMMEDIATE");
    const uncommitted = packStringKey(["default", "uncommitted"]);
    f.database.run("INSERT INTO kv VALUES(?,?,?,?,?,?)", [uncommitted, new Uint8Array([7]), 3n, 10n, 0n, -1n]);
    const receipt = await writePrivateBunCopyCapture(f.input);
    assert.equal(receipt.source_quiescence_proven, false);
    assert.equal(receipt.custody_verified, false);
    assert.equal(receipt.activation_authorized, false);
    assert.equal(receipt.capture.entries, 3);
    assert.equal(receipt.capture.source_identity.sha256, receipt.snapshot.sha256);
    assert.equal(receipt.snapshot.sha256, createHash("sha256").update(readFileSync(receipt.snapshot.path)).digest("hex"));
    assert.notEqual(lstatSync(receipt.snapshot.path).ino, lstatSync(f.source).ino);
    assert.equal(lstatSync(receipt.snapshot.path).mode & 0o777, 0o600);
    assert.equal(lstatSync(receipt.capture.destination).mode & 0o777, 0o600);
    for (const suffix of ["-wal", "-shm", "-journal"]) assert.equal(existsSync(receipt.snapshot.path + suffix), false);
    const captured = await capturePrivateNativeState(receipt.snapshot.path, f.input.directory, SOURCE_SHA, receipt.capture.captured_at_ms);
    assert.equal(captured.value.entries.length, 3);
    assert(!captured.value.entries.some((entry) => Buffer.from(entry.raw_key).equals(uncommitted)));
    const graph = captured.value.entries.find((entry) => Buffer.from(entry.raw_key).equals(f.key));
    assert(graph?.value.encoding === "source-graph-v1");
    assert.deepEqual(graph.value.value, encodeNativeCaptureValue(f.graph));
    assert(readFileSync(receipt.capture.destination, "utf8").includes('"expiration_ms":9223372036854775807'));
    f.database.run("ROLLBACK");
    f.database.run("INSERT INTO kv VALUES(?,?,?,?,?,?)", [packStringKey(["default", "after-copy"]), new Uint8Array([8]), 3n, 10n, 0n, -1n]);
    assert.equal(f.database.query<{ count: bigint }, []>("SELECT COUNT(*) AS count FROM kv").get()?.count, 4n);
    assert.equal(f.database.query<{ journal_mode: string }, []>("PRAGMA journal_mode").get()?.journal_mode, "wal");
    assert.equal(createHash("sha256").update(readFileSync(receipt.snapshot.path)).digest("hex"), receipt.snapshot.sha256);
    assert.equal(JSON.stringify(receipt).includes(SENTINEL), false);
    assert.equal(lstatSync(join(f.input.directory, "receipt.json")).mode & 0o777, 0o600);
  } finally {
    try {
      f.database.run("ROLLBACK");
    } catch {}
    f.database.close(true);
  }
});

test("existing and symlink destinations, source aliases and nonprivate parents refuse without a success receipt", async () => {
  const f = fixture();
  try {
    mkdirSync(f.input.directory, { mode: 0o700 });
    const sentinel = join(f.input.directory, "keep");
    writeFileSync(sentinel, SENTINEL);
    await assert.rejects(writePrivateBunCopyCapture(f.input), /refused/);
    assert.equal(readFileSync(sentinel, "utf8"), SENTINEL);
    const alias = join(f.directory, "alias");
    symlinkSync(f.input.directory, alias);
    await assert.rejects(writePrivateBunCopyCapture({ ...f.input, directory: alias }), /refused/);
    const sourceAlias = join(f.directory, "source-alias");
    symlinkSync(f.source, sourceAlias);
    await assert.rejects(writePrivateBunCopyCapture({ ...f.input, source: sourceAlias, directory: join(f.directory, "alias-run") }), /refused/);
    const hardlink = join(f.directory, "source-hardlink");
    linkSync(f.source, hardlink);
    await assert.rejects(writePrivateBunCopyCapture({ ...f.input, directory: join(f.directory, "hardlink-run") }), /refused/);
    rmSync(hardlink);
    const shared = join(f.directory, "shared");
    mkdirSync(shared, { mode: 0o755 });
    chmodSync(shared, 0o755);
    await assert.rejects(writePrivateBunCopyCapture({ ...f.input, directory: join(shared, "run") }), /refused/);
    assert.equal(existsSync(join(f.input.directory, "receipt.json")), false);
  } finally {
    f.database.close(true);
  }
});

test("unsupported codec, schema, queues and corrupt source never publish an accepted capture", async () => {
  for (const corruption of ["codec", "schema", "queue", "bytes"]) {
    const f = fixture();
    try {
      if (corruption === "codec") f.database.run("UPDATE uos_kv_metadata SET value='deno-v8' WHERE key='codec'");
      if (corruption === "schema") f.database.run("CREATE TABLE unsupported (value TEXT)");
      if (corruption === "queue") f.database.run("INSERT INTO queue VALUES(0,'queued',X'00','[]',X'00')");
      if (corruption === "bytes") {
        f.database.close(true);
        writeFileSync(f.source, "corrupt synthetic SQLite");
      }
      await assert.rejects(writePrivateBunCopyCapture(f.input), /refused/);
      assert.equal(existsSync(join(f.input.directory, "receipt.json")), false);
    } finally {
      if (corruption !== "bytes") f.database.close(true);
    }
  }
});

test("cancelled private-copy child settles without a final receipt", async () => {
  const f = fixture();
  try {
    const already = new AbortController();
    already.abort();
    await assert.rejects(writePrivateBunCopyCapture(f.input, already.signal), /refused/);
    assert.equal(existsSync(f.input.directory), false);
    const during = new AbortController();
    const result = writePrivateBunCopyCapture(f.input, during.signal);
    during.abort();
    await assert.rejects(result, /refused/);
    assert.equal(existsSync(join(f.input.directory, "receipt.json")), false);
  } finally {
    f.database.close(true);
  }
});
