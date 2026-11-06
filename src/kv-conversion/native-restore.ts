/** Offline reverse step: one private NativeCapture v2 into a NEW codec-marked Bun KV.
 * It never opens, converts or replaces a live or existing database. The destination is
 * built beside itself, re-read and validated (raw keys, revisions, absolute expiry and
 * typed values), then published with an exclusive hard link so nothing is overwritten.
 */
import { Database } from "bun:sqlite";
import { createHash } from "node:crypto";
import { closeSync, existsSync, fsyncSync, linkSync, lstatSync, openSync, readFileSync, realpathSync, rmSync } from "node:fs";
import { basename, dirname, join } from "node:path";
import { deserialize, serialize } from "node:v8";
import { createNativeSnapshotReader, NATIVE_DDL, validateNativeRow, verifyNativeSchema, type NativeRow } from "./native-schema.ts";
import { decodeNativeCaptureValue, encodeNativeCaptureValue, type TypedValue } from "./typed-value.ts";

type Revision = { version: string; sequence: string };
type Value = { encoding: "source-graph-v1"; value: TypedValue } | { encoding: "counter-le64" | "bytes"; value: number[] };
type Entry = { raw_key: number[]; value: Value; revision: Revision; expiration_ms: bigint };
type Capture = {
  v: 2;
  source_git_sha: string;
  captured_at_ms: number;
  source_codec: "bun-jsc-v1";
  native_schema_version: 3;
  data_revision: Revision;
  queue_entries: 0;
  running_queue_entries: 0;
  entries: Entry[];
};
export type NativeRestoreReceipt = Readonly<{
  v: 1;
  kind: "native_capture_restore";
  capture_sha256: string;
  destination: string;
  destination_sha256: string;
  destination_bytes: number;
  entries: number;
  data_revision: Revision;
  source_git_sha: string;
  captured_at_ms: number;
  activation_authorized: false;
}>;

const sha256 = (bytes: Uint8Array): string => createHash("sha256").update(bytes).digest("hex");
const decimal = (value: unknown): value is string => typeof value === "string" && /^(0|[1-9]\d{0,18})$/.test(value) && BigInt(value) <= 9223372036854775807n;
const isObject = (value: unknown): value is Record<string, unknown> => typeof value === "object" && value !== null && !Array.isArray(value);
const closed = (value: unknown, keys: readonly string[]): value is Record<string, unknown> =>
  isObject(value) && Object.keys(value).length === keys.length && keys.every((key) => Object.hasOwn(value, key));
const bytes = (value: unknown): value is number[] =>
  Array.isArray(value) && value.every((byte) => typeof byte === "number" && Number.isInteger(byte) && byte >= 0 && byte <= 255);
const revision = (value: unknown): value is Revision => closed(value, ["version", "sequence"]) && decimal(value.version) && decimal(value.sequence);

type SourceContext = Readonly<{ source?: string }>;
/** An entry's i64 expiry is read from its exact JSON source text, never rounded through Number. */
function exactExpiry(this: unknown, key: string, value: unknown, context?: SourceContext): unknown {
  if (key !== "expiration_ms" || typeof value !== "number" || !isObject(this) || !Object.hasOwn(this, "raw_key")) return value;
  if (typeof context?.source !== "string" || !/^-?\d+$/.test(context.source)) throw new Error("Native capture expiry source text is unavailable");
  return BigInt(context.source);
}

const parseCapture = (text: string): Capture => {
  // Pinned Bun passes the JSON.parse source-text context as the reviver's third argument.
  const capture: unknown = JSON.parse(text, exactExpiry as (this: unknown, key: string, value: unknown) => unknown);
  const header = [
    "v",
    "source_git_sha",
    "captured_at_ms",
    "source_codec",
    "native_schema_version",
    "data_revision",
    "queue_entries",
    "running_queue_entries",
    "entries",
  ];
  if (
    !closed(capture, header) ||
    capture.v !== 2 ||
    typeof capture.source_git_sha !== "string" ||
    !/^[a-f0-9]{40}$/.test(capture.source_git_sha) ||
    !Number.isSafeInteger(capture.captured_at_ms) ||
    capture.source_codec !== "bun-jsc-v1" ||
    capture.native_schema_version !== 3 ||
    !revision(capture.data_revision) ||
    capture.queue_entries !== 0 ||
    capture.running_queue_entries !== 0 ||
    !Array.isArray(capture.entries)
  )
    throw new Error("Native capture header is unsupported");
  const top = BigInt(capture.data_revision.version);
  for (const entry of capture.entries as unknown[]) validateEntry(entry, top);
  return capture as Capture;
};

const typedValue = (value: unknown): boolean => {
  if (!closed(value, ["encoding", "value"])) return false;
  if (value.encoding === "source-graph-v1") return true;
  if (value.encoding === "bytes") return bytes(value.value);
  return value.encoding === "counter-le64" && bytes(value.value) && value.value.length === 8;
};
const validateEntry = (entry: unknown, top: bigint): void => {
  if (!closed(entry, ["raw_key", "value", "revision", "expiration_ms"]) || !bytes(entry.raw_key) || entry.raw_key.length === 0)
    throw new Error("Native capture entry key is invalid");
  if (!revision(entry.revision) || BigInt(entry.revision.version) > top) throw new Error("Native capture entry revision is invalid");
  if (typeof entry.expiration_ms !== "bigint" || entry.expiration_ms < -1n || entry.expiration_ms > 9223372036854775807n)
    throw new Error("Native capture absolute expiry is invalid");
  if (!typedValue(entry.value)) throw new Error("Native capture typed value is invalid");
};

const ENCODINGS: Readonly<Record<Value["encoding"], bigint>> = { "source-graph-v1": 1n, "counter-le64": 2n, bytes: 3n };
const encodingOf = (value: Value): bigint => ENCODINGS[value.encoding];

const write = (path: string, capture: Capture): void => {
  const database = new Database(path, { create: true, strict: true, safeIntegers: true });
  try {
    database.run("PRAGMA journal_mode=DELETE");
    database.run("BEGIN IMMEDIATE");
    for (const ddl of NATIVE_DDL) database.run(ddl);
    database.run("CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID");
    database.run("INSERT INTO uos_kv_metadata(key,value) VALUES('codec','bun-jsc-v1')");
    database.run("INSERT INTO migration_state(k,version) VALUES(0,3)");
    database.run("INSERT INTO data_version(k,version,seq) VALUES(0,?,?)", [BigInt(capture.data_revision.version), BigInt(capture.data_revision.sequence)]);
    const insert = database.prepare("INSERT INTO kv(k,v,v_encoding,version,seq,expiration_ms) VALUES(?,?,?,?,?,?)");
    for (const entry of capture.entries) {
      const value = entry.value.encoding === "source-graph-v1" ? serialize(decodeNativeCaptureValue(entry.value.value)) : new Uint8Array(entry.value.value);
      insert.run(
        new Uint8Array(entry.raw_key),
        value,
        encodingOf(entry.value),
        BigInt(entry.revision.version),
        BigInt(entry.revision.sequence),
        entry.expiration_ms
      );
    }
    database.run("COMMIT");
  } finally {
    database.close(true);
  }
};

const verify = (path: string, capture: Capture): void => {
  const reader = createNativeSnapshotReader(new Database(path, { readonly: true, strict: true, safeIntegers: true }));
  try {
    const actual = verifyNativeSchema(reader, "bun-jsc-v1");
    if (String(actual.version) !== capture.data_revision.version || String(actual.seq) !== capture.data_revision.sequence)
      throw new Error("Restored data revision differs");
    const expected = new Map(capture.entries.map((entry) => [Buffer.from(entry.raw_key).toString("hex"), entry]));
    if (expected.size !== capture.entries.length) throw new Error("Native capture has duplicate raw keys");
    let rows = 0;
    for (const row of reader.query<NativeRow>("SELECT k,v,v_encoding,version,seq,expiration_ms FROM kv ORDER BY k").iterate()) {
      validateNativeRow(row, actual);
      const entry = expected.get(Buffer.from(row.k).toString("hex"));
      if (
        !entry ||
        row.v_encoding !== encodingOf(entry.value) ||
        String(row.version) !== entry.revision.version ||
        String(row.seq) !== entry.revision.sequence ||
        row.expiration_ms !== entry.expiration_ms
      )
        throw new Error("Restored raw key, revision or absolute expiry differs");
      const same =
        entry.value.encoding === "source-graph-v1"
          ? JSON.stringify(encodeNativeCaptureValue(deserialize(row.v))) === JSON.stringify(entry.value.value)
          : Buffer.from(row.v).equals(new Uint8Array(entry.value.value));
      if (!same) throw new Error("Restored typed value differs");
      rows++;
    }
    if (rows !== capture.entries.length) throw new Error("Restored row inventory differs");
  } finally {
    reader.close();
  }
};

const syncFile = (path: string): void => {
  const fd = openSync(path, "r");
  try {
    fsyncSync(fd);
  } finally {
    closeSync(fd);
  }
};

export const restoreNativeCapture = (capturePath: string, expectedSha256: string, destination: string): NativeRestoreReceipt => {
  if (!/^[a-f0-9]{64}$/.test(expectedSha256)) throw new Error("Expected capture SHA-256 is malformed");
  const raw = readFileSync(capturePath);
  if (sha256(raw) !== expectedSha256) throw new Error("Native capture hash differs from its receipt");
  const capture = parseCapture(raw.toString("utf8"));
  const parent = realpathSync(dirname(destination));
  const parentStat = lstatSync(parent);
  if (!parentStat.isDirectory() || (parentStat.mode & 0o077) !== 0) throw new Error("Restore destination parent must be an owner-only directory");
  const target = join(parent, basename(destination));
  for (const suffix of ["", "-wal", "-shm", "-journal"])
    if (existsSync(`${target}${suffix}`)) throw new Error("Restore destination must be NEW; an existing database is never replaced");
  const staging = `${target}.restore-${process.pid}-${Date.now()}`;
  try {
    // Owner-only from creation: the restored rows can contain credentials.
    closeSync(openSync(staging, "wx", 0o600));
    write(staging, capture);
    verify(staging, capture);
    syncFile(staging);
    linkSync(staging, target);
  } finally {
    rmSync(staging, { force: true });
    for (const suffix of ["-wal", "-shm", "-journal"]) rmSync(`${staging}${suffix}`, { force: true });
  }
  verify(target, capture);
  const published = readFileSync(target);
  return {
    v: 1,
    kind: "native_capture_restore",
    capture_sha256: expectedSha256,
    destination: target,
    destination_sha256: sha256(published),
    destination_bytes: published.length,
    entries: capture.entries.length,
    data_revision: capture.data_revision,
    source_git_sha: capture.source_git_sha,
    captured_at_ms: capture.captured_at_ms,
    activation_authorized: false,
  };
};
