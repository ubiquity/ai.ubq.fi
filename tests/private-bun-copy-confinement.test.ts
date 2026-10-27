/** Finite trusted prelaunch controller; every product/probe child applies its first OS boundary. */
import assert from "node:assert/strict";
import { afterEach, test } from "bun:test";
import { Database } from "bun:sqlite";
import { createHash } from "node:crypto";
import { chmodSync, existsSync, lstatSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, rmSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { basename, dirname, join } from "node:path";
import { fileURLToPath } from "node:url";
import { serialize } from "node:v8";
import { testNetworkArgv } from "../scripts/test-network.ts";
import { privateBunCopyProfile, type PrivateBunCopyProfileFacts } from "../tools/cloudflare/private-bun-copy-profile.ts";
import { privateBunCopyLauncher } from "../tools/cloudflare/private-bun-copy-launcher.ts";
import { PrivateBunCopyPython } from "../tools/cloudflare/private-bun-copy-python.ts";
import { privateBunCopyTestIcu, privateBunCopyTestRuntime, privateBunCopyTestSeal } from "./fixtures/private-bun-copy-runtime.ts";

const SOURCE_SHA = "b".repeat(40);
const SENTINEL = "PRIVATE_CAPTURE_SENTINEL_NOT_STDOUT";
const ROOT = fileURLToPath(new URL("../", import.meta.url));
const owned: string[] = [];
afterEach(() => {
  for (const path of owned.splice(0)) rmSync(path, { recursive: true, force: true });
});

function profileFacts(output: string, entry?: string): PrivateBunCopyProfileFacts {
  const seal = privateBunCopyTestSeal(dirname(output));
  const pythonRole = Object.keys(seal.expected_python_roles);
  const canonical = pythonRole.find((path) => /\/bin\/python[^/]+$/.test(path) && path !== "/usr/bin/python3");
  assert(canonical);
  const versionRoot = dirname(dirname(canonical));
  const source = join(dirname(output), "source.sqlite3");
  const icu = privateBunCopyTestIcu();
  const readRoots = [
    ROOT.replace(/\/$/, ""),
    realpathSync(join(ROOT, "node_modules")),
    "/usr/lib",
    "/System/Library",
    join(versionRoot, "lib", basename(canonical)),
    ...(entry ? [dirname(entry)] : []),
  ];
  const readLiterals = [
    realpathSync(process.execPath),
    ...pythonRole,
    join(versionRoot, "Python3"),
    join(versionRoot, "Resources/Python.app/Contents/Info.plist"),
    source,
    source + "-wal",
    source + "-shm",
    "/dev/null",
    "/dev/urandom",
    "/dev/random",
    icu.source,
    ...(entry ? [entry] : []),
  ];
  const icuVersion = process.versions.icu?.split(".")[0];
  assert(icuVersion && /^\d+$/.test(icuVersion));
  const metadata = new Set(["/", "/usr/share", "/usr/share/icu", join("/usr/share/icu", "icudt" + icuVersion + "l.dat")]);
  for (const path of [...readRoots, ...readLiterals, output]) {
    for (let parent = dirname(path); parent !== "/"; parent = dirname(parent)) metadata.add(parent);
  }
  return {
    bun: realpathSync(process.execPath),
    source,
    run: output,
    read_roots: readRoots,
    read_literals: readLiterals,
    private_metadata_traversal_literals: [...metadata],
    expected_python_roles: seal.expected_python_roles,
    files: [icu],
  };
}

function fixture(keepWriter = false) {
  const directory = realpathSync(mkdtempSync(join(tmpdir(), "uos-private-confinement-")));
  chmodSync(directory, 0o700);
  owned.push(directory);
  const source = join(directory, "source.sqlite3");
  const database = new Database(source, { create: true, strict: true, safeIntegers: true });
  if (keepWriter) database.run("PRAGMA journal_mode=WAL");
  // Closed synthetic schema3 fixture; deliberately no product module import.
  for (const ddl of [
    "CREATE TABLE data_version (k INTEGER PRIMARY KEY, version INTEGER NOT NULL, seq INTEGER NOT NULL DEFAULT 0)",
    "CREATE TABLE kv (k BLOB PRIMARY KEY, v BLOB NOT NULL, v_encoding INTEGER NOT NULL, version INTEGER NOT NULL, seq INTEGER NOT NULL DEFAULT 0, expiration_ms INTEGER NOT NULL DEFAULT -1) WITHOUT ROWID",
    "CREATE INDEX kv_expiration_ms_idx ON kv (expiration_ms)",
    "CREATE TABLE migration_state (k INTEGER NOT NULL PRIMARY KEY, version INTEGER NOT NULL)",
    "CREATE TABLE queue (ts INTEGER NOT NULL, id TEXT NOT NULL, data BLOB NOT NULL, backoff_schedule TEXT NOT NULL, keys_if_undelivered BLOB NOT NULL, PRIMARY KEY (ts, id))",
    "CREATE TABLE queue_running (deadline INTEGER NOT NULL, id TEXT NOT NULL, data BLOB NOT NULL, backoff_schedule TEXT NOT NULL, keys_if_undelivered BLOB NOT NULL, PRIMARY KEY (deadline, id))",
    "CREATE TABLE uos_kv_metadata (key TEXT PRIMARY KEY, value TEXT NOT NULL) WITHOUT ROWID",
  ])
    database.run(ddl);
  database.run("INSERT INTO uos_kv_metadata VALUES('codec','bun-jsc-v1')");
  database.run("INSERT INTO migration_state VALUES(0,3)");
  database.run("INSERT INTO data_version VALUES(0,10,0)");
  const rawKey = new Uint8Array([2, ...Buffer.from("default"), 0, 2, ...Buffer.from("confinement"), 0]);
  database.run("INSERT INTO kv VALUES(?,?,?,?,?,?)", [rawKey, serialize({ text: SENTINEL }), 1n, 9n, 0n, -1n]);
  if (!keepWriter) database.close(true);
  chmodSync(source, 0o600);
  return { directory, database: keepWriter ? database : undefined, input: { v: 1, source, directory: join(directory, "run"), source_git_sha: SOURCE_SHA } };
}

type ChildResult = Readonly<{ code: number; stdout: string; stderr: string }>;
async function runChild(argv: readonly string[], input: unknown, environment?: Readonly<Record<string, string>>): Promise<ChildResult> {
  const child = Bun.spawn([...argv], {
    cwd: ROOT,
    env: environment ?? { PATH: "/usr/bin:/bin", BUN_RUNTIME_TRANSPILER_CACHE_PATH: "0" },
    stdin: "pipe",
    stdout: "pipe",
    stderr: "pipe",
  });
  await child.stdin.write(JSON.stringify(input));
  await child.stdin.end();
  const [stdout, stderr, code] = await Promise.all([new Response(child.stdout).text(), new Response(child.stderr).text(), child.exited]);
  return { code, stdout, stderr };
}

function confined(child: readonly string[], output: string, forbidden: string): string[] {
  assert.equal(process.platform, "darwin", "These operator containment cases require actual Darwin Seatbelt");
  const argv = testNetworkArgv(child);
  const encoded: unknown = JSON.parse(argv[5] ?? "null");
  assert(Array.isArray(encoded) && encoded.every((item: unknown) => typeof item === "string"));
  const inner: string[] = encoded;
  assert.equal(inner[0], "/usr/bin/sandbox-exec");
  assert.equal(inner[1], "-p");
  const profile = inner[2];
  assert(profile.includes("(deny network*)"));
  assert(!forbidden.startsWith(ROOT) && !forbidden.startsWith(output + "/"));
  const entry = child[0] === process.execPath ? child[3] : undefined;
  inner[2] = privateBunCopyProfile(profileFacts(output, entry));
  console.log(
    JSON.stringify({
      kind: "private_copy_stricter_profile",
      original_profile: profile,
      final_profile: inner[2],
      original_sha256: createHash("sha256").update(profile).digest("hex"),
      final_sha256: createHash("sha256").update(inner[2]).digest("hex"),
      read_boundary: "explicit immutable source/artifact roots and source-file literals; unrelated reads denied by default",
    })
  );
  return [...argv.slice(0, 5), JSON.stringify(inner)];
}

test("private stdin CLI publishes only success metadata under inherited Python write/read/process confinement", async () => {
  const f = fixture(true);
  try {
    assert(lstatSync(f.input.source + "-wal").size > 0);
    const forbidden = join(f.directory, "forbidden");
    mkdirSync(forbidden, { mode: 0o700 });
    writeFileSync(join(forbidden, "auth.json"), SENTINEL, { mode: 0o600 });
    const codeRoot = join(f.directory, "immutable-launcher");
    mkdirSync(codeRoot, { mode: 0o700 });
    const launcher = join(codeRoot, "bound-launcher.mjs");
    const seal = privateBunCopyTestSeal(f.directory);
    const prepared = privateBunCopyLauncher({ ...profileFacts(f.input.directory), ...seal });
    writeFileSync(launcher, prepared.source, {
      mode: 0o400,
      flag: "wx",
    });
    const launcherHash = createHash("sha256").update(prepared.source).digest("hex");
    const launcherIdentity = lstatSync(launcher);
    const command = [process.execPath, "--no-env-file", "--no-install", launcher];
    const argv = confined(command, f.input.directory, forbidden);
    const result = await runChild(argv, f.input, prepared.environment);
    assert.equal(result.code, 0, result.stderr);
    assert.equal(result.stderr, "");
    assert.equal(result.stdout.includes(SENTINEL), false);
    assert.equal(result.stdout.includes('"entries":['), false);
    const metadata: unknown = JSON.parse(result.stdout);
    assert(typeof metadata === "object" && metadata !== null && "kind" in metadata);
    assert.equal(metadata.kind, "private_bun_copy_capture");
    assert(f.database);
    f.database.run("INSERT INTO kv VALUES(?,?,?,?,?,?)", [new Uint8Array([2, 97, 0]), new Uint8Array([1]), 3n, 10n, 0n, -1n]);
    assert.equal(f.database.query<{ count: bigint }, []>("SELECT COUNT(*) AS count FROM kv").get()?.count, 2n);
    assert.equal(readFileSync(join(forbidden, "auth.json"), "utf8"), SENTINEL);
    const failed = await runChild(argv, { ...f.input, runtime: SENTINEL }, prepared.environment);
    assert.equal(failed.code, 78);
    assert.equal(failed.stdout, "");
    assert.equal(failed.stderr.includes(SENTINEL), false);
    assert.equal(createHash("sha256").update(readFileSync(launcher)).digest("hex"), launcherHash);
    assert.equal(lstatSync(launcher).ino, launcherIdentity.ino);
    console.log(
      JSON.stringify({
        kind: "private_copy_executed_launcher",
        sha256: launcherHash,
        bytes: launcherIdentity.size,
        mode: launcherIdentity.mode & 0o777,
        source_sha: seal.source_sha,
        source_tree: seal.source_tree,
        stage_manifest_sha256: seal.stage_manifest_sha256,
        python_signature_receipt_sha256: seal.python_signature_receipt.sha256,
        environment: prepared.environment,
      })
    );
  } finally {
    f.database?.close(true);
    rmSync(f.directory, { recursive: true, force: true });
  }
});

test("the unbound direct CLI refuses valid input without creating an output directory", async () => {
  const f = fixture();
  const command = [process.execPath, "--no-env-file", "--no-install", join(ROOT, "tools/cloudflare/private-bun-copy.ts")];
  const forbidden = join(f.directory, "forbidden");
  mkdirSync(forbidden, { mode: 0o700 });
  const result = await runChild(confined(command, f.input.directory, forbidden), f.input);
  assert.equal(result.code, 78);
  assert.equal(result.stdout, "");
  assert.equal(existsSync(f.input.directory), false);
});

test("actual Python descendants cannot write/read forbidden fixtures or exec another role inside the operator boundary", async () => {
  const f = fixture();
  try {
    mkdirSync(f.input.directory, { mode: 0o700 });
    const forbidden = join(f.directory, "forbidden");
    mkdirSync(forbidden, { mode: 0o700 });
    const hidden = join(forbidden, "auth.json");
    const sibling = join(f.directory, "sibling.txt");
    writeFileSync(hidden, SENTINEL, { mode: 0o600 });
    writeFileSync(sibling, SENTINEL, { mode: 0o600 });
    const program = String.raw`
import json, os, pathlib, sys
value = json.load(sys.stdin)
def denied(operation):
    try:
        operation()
    except PermissionError:
        return True
    return False
assert denied(lambda: pathlib.Path(value["hidden"]).read_bytes())
assert denied(lambda: pathlib.Path(value["sibling"]).read_bytes())
assert denied(lambda: pathlib.Path(value["outside"]).write_text("forbidden"))
assert denied(lambda: os.open(value["source"], os.O_WRONLY))
assert denied(lambda: os.execv("/bin/sh", ["/bin/sh", "-c", "exit 99"]))
pathlib.Path(value["inside"]).write_text("allowed")
print("PYTHON_BOUNDARY_CONFIRMED")
`;
    const command = [PrivateBunCopyPython.command(privateBunCopyTestRuntime(f.directory)), "-I", "-S", "-B", "-c", program];
    const result = await runChild(confined(command, f.input.directory, forbidden), {
      hidden,
      sibling,
      outside: join(forbidden, "new"),
      source: f.input.source,
      inside: join(f.input.directory, "allowed"),
    });
    assert.equal(result.code, 0, result.stderr);
    assert.equal(result.stdout.trim(), "PYTHON_BOUNDARY_CONFIRMED");
    assert.equal(existsSync(join(forbidden, "new")), false);
    assert.equal(readFileSync(join(f.input.directory, "allowed"), "utf8"), "allowed");
    assert.equal(readFileSync(sibling, "utf8"), SENTINEL);
  } finally {
    rmSync(f.directory, { recursive: true, force: true });
  }
});

test("strict operator profile refuses a filesystem read root or source-overlapping output before launch", () => {
  const f = fixture();
  const facts = profileFacts(f.input.directory);
  assert.throws(() => privateBunCopyProfile({ ...facts, read_roots: ["/", ...facts.read_roots] }), /profile facts/);
  assert.throws(() => privateBunCopyProfile({ ...facts, run: dirname(f.input.source) }), /profile facts/);
});

test("actual Python outbound denial has matching outside-operator-boundary positive probes", async () => {
  const f = fixture();
  // The outer repository fixture boundary permits loopback. The stricter
  // operator boundary denies all outbound sockets, including this fixture.
  const { createServer } = await import("node:net");
  const address = "127.0.0.1";
  const server = createServer((socket) => socket.end());
  await new Promise<void>((resolve, reject) => {
    server.once("error", reject);
    server.listen(0, address, resolve);
  });
  try {
    const bound = server.address();
    assert(bound && typeof bound === "object");
    const program = String.raw`
import errno, json, socket, sys
value = json.load(sys.stdin)
print("PYTHON_STARTED", flush=True)
try:
    connection = socket.create_connection((value["host"], value["port"]), 2)
except PermissionError as error:
    if error.errno != errno.EPERM:
        raise
    print("PYTHON_OUTBOUND_DENIED", flush=True)
    sys.exit(77)
connection.close()
print("CONNECTED", flush=True)
`;
    const command = [PrivateBunCopyPython.command(privateBunCopyTestRuntime(f.directory)), "-I", "-S", "-B", "-c", program];
    const input = { host: address, port: bound.port };
    const before = await runChild(testNetworkArgv(command), input);
    assert.equal(before.code, 0, before.stderr);
    assert.equal(before.stdout, "PYTHON_STARTED\nCONNECTED\n");
    assert.equal(before.stderr, "");
    const forbidden = join(f.directory, "forbidden");
    mkdirSync(forbidden, { mode: 0o700 });
    const inside = await runChild(confined(command, f.input.directory, forbidden), input);
    assert.equal(inside.code, 77, inside.stderr);
    assert.equal(inside.stdout, "PYTHON_STARTED\nPYTHON_OUTBOUND_DENIED\n");
    assert.equal(inside.stderr, "");
    const after = await runChild(testNetworkArgv(command), input);
    assert.equal(after.code, 0, after.stderr);
    assert.equal(after.stdout, "PYTHON_STARTED\nCONNECTED\n");
    assert.equal(after.stderr, "");
  } finally {
    await new Promise<void>((resolve, reject) => {
      server.close((error) => {
        if (error) reject(error);
        else resolve();
      });
    });
    rmSync(f.directory, { recursive: true, force: true });
  }
});
