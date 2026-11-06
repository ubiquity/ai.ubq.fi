import assert from "node:assert/strict";
import { test } from "bun:test";
import { Database } from "bun:sqlite";
import { createHash } from "node:crypto";
import { chmodSync, lstatSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { serialize } from "node:v8";
import { capturePrivateNativeState } from "../rust/crates/gateway-migration/examples/native-operator-capture.ts";
import { packStringKey } from "../src/kv-conversion/cursors.ts";
import { restoreNativeCapture } from "../src/kv-conversion/native-restore.ts";
import { NATIVE_DDL } from "../src/kv-conversion/native-schema.ts";

const sha256 = (bytes: string | Uint8Array): string => createHash("sha256").update(bytes).digest("hex");

const source = (path: string): void => {
  const database = new Database(path, { create: true, strict: true, safeIntegers: true });
  database.run("PRAGMA journal_mode=DELETE");
  for (const ddl of NATIVE_DDL) database.run(ddl);
  database.run("CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID");
  database.run("INSERT INTO uos_kv_metadata(key,value) VALUES('codec','bun-jsc-v1')");
  database.run("INSERT INTO migration_state(k,version) VALUES(0,3)");
  database.run("INSERT INTO data_version(k,version,seq) VALUES(0,12,4)");
  const graph: Record<string, unknown> = { text: "\ud83d", buffer: Buffer.from([1, 2, 3]), when: new Date(5), nested: [1, -0, 7n] };
  graph.self = graph;
  const insert = "INSERT INTO kv(k,v,v_encoding,version,seq,expiration_ms) VALUES(?,?,?,?,?,?)";
  database.run(insert, [packStringKey(["uos_ai", "api_keys", "graph"]), serialize(graph), 1n, 12n, 3n, 9500n]);
  database.run(insert, [packStringKey(["default", "counter"]), new Uint8Array(8).fill(255), 2n, 11n, 0n, -1n]);
  database.run(insert, [packStringKey(["default", "bytes"]), new Uint8Array([0, 255]), 3n, 10n, 1n, 9223372036854775807n]);
  database.close(true);
};

const captureFile = async (directory: string, database: string, name: string): Promise<{ path: string; sha: string; text: string }> => {
  const capture = await capturePrivateNativeState(database, directory, "a".repeat(40), 2300);
  // The serialized capture keeps the exact raw i64 expiry produced by the reviewed producer.
  const text = JSON.stringify(capture.value);
  const path = join(directory, name);
  writeFileSync(path, text, { mode: 0o600, flag: "wx" });
  return { path, sha: sha256(text), text };
};

test("a NativeCapture restores into a NEW owner-only Bun KV whose recapture is identical", async () => {
  const directory = mkdtempSync(join(tmpdir(), "uos-native-restore-"));
  try {
    chmodSync(directory, 0o700);
    const original = join(directory, "source.sqlite3");
    source(original);
    const first = await captureFile(directory, original, "capture.json");
    assert.ok(first.text.includes('"expiration_ms":9223372036854775807'));
    const destination = join(directory, "restored.sqlite3");
    const receipt = restoreNativeCapture(first.path, first.sha, destination);
    assert.equal(receipt.entries, 3);
    assert.deepEqual(receipt.data_revision, { version: "12", sequence: "4" });
    assert.equal(receipt.activation_authorized, false);
    assert.equal(receipt.destination_sha256, sha256(readFileSync(destination)));
    assert.equal(lstatSync(destination).mode & 0o777, 0o600);
    const second = await captureFile(directory, destination, "recapture.json");
    assert.equal(second.text, first.text, "raw keys, revisions, absolute expiry and typed values survive exactly");
    // The destination is never replaced, and nothing is left behind on refusal.
    assert.throws(() => restoreNativeCapture(first.path, first.sha, destination), /must be NEW/);
    assert.throws(() => restoreNativeCapture(first.path, "0".repeat(64), join(directory, "other.sqlite3")), /hash differs/);
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});

test("restore refuses revisions above the data revision, malformed expiry and unsupported headers", async () => {
  const directory = mkdtempSync(join(tmpdir(), "uos-native-restore-refuse-"));
  try {
    chmodSync(directory, 0o700);
    const original = join(directory, "source.sqlite3");
    source(original);
    const { text } = await captureFile(directory, original, "capture.json");
    // Edit the exact text: a Number round trip would itself round the i64 expiry.
    const replace = (from: string, to: string): string => {
      assert.ok(text.includes(from), from);
      return text.replace(from, to);
    };
    const variants: [string, string, RegExp][] = [
      ["revision", replace('{"version":"12","sequence":"3"}', '{"version":"99","sequence":"3"}'), /revision is invalid/],
      ["expiry", replace('"expiration_ms":9500', '"expiration_ms":-2'), /absolute expiry/],
      ["queues", replace('"queue_entries":0', '"queue_entries":1'), /header is unsupported/],
      ["counter", replace('{"encoding":"bytes","value":[0,255]}', '{"encoding":"counter-le64","value":[0,255]}'), /typed value/],
      ["rounded", JSON.stringify(JSON.parse(text)), /absolute expiry/],
    ];
    for (const [name, body, expected] of variants) {
      const path = join(directory, `${name}.json`);
      writeFileSync(path, body, { mode: 0o600 });
      const destination = join(directory, `${name}.sqlite3`);
      assert.throws(() => restoreNativeCapture(path, sha256(body), destination), expected, name);
      assert.throws(() => lstatSync(destination), /ENOENT/);
    }
  } finally {
    rmSync(directory, { recursive: true, force: true });
  }
});
