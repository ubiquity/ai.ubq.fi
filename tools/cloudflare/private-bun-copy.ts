/** Private offline-copy operator. Neither a stopped-owner proof nor an adoption capability. */
import { createHash } from "node:crypto";
import { closeSync, constants, fstatSync, fsyncSync, lstatSync, mkdirSync, openSync, readSync, realpathSync, unlinkSync, writeFileSync } from "node:fs";
import { basename, dirname, isAbsolute, join, resolve } from "node:path";
import { writePrivateNativeCapture, type PrivateNativeCaptureFileReceipt } from "../../rust/crates/gateway-migration/examples/native-operator-capture.ts";
import { PrivateBunCopyPython } from "./private-bun-copy-python.ts";

export type PrivateBunCopyInput = Readonly<{ v: 1; source: string; directory: string; source_git_sha: string }>;
type FileFact = Readonly<{ path: string; bytes: number; sha256: string }>;
type BackupReceipt = Readonly<{
  v: 1;
  method: "sqlite3.Connection.backup";
  source_mode: "ro";
  source: string;
  source_device: number;
  source_inode: number;
  destination: string;
  bytes: number;
  destination_sha256: string;
  started_at_ms: number;
  finished_at_ms: number;
  progress: Readonly<{ status: 101; remaining: 0; total: number }>;
  consistent_backup_complete: true;
}>;
export type PrivateBunCopyReceipt = Readonly<{
  v: 1;
  kind: "private_bun_copy_capture";
  source_git_sha: string;
  snapshot: FileFact;
  backup_receipt: FileFact;
  capture: PrivateNativeCaptureFileReceipt;
  capture_receipt: FileFact;
  source_quiescence_proven: false;
  custody_verified: false;
  activation_authorized: false;
}>;

const INPUT_MAX_BYTES = 1024 * 1024;
const CHILD_MAX_BYTES = 16 * 1024;
const BACKUP_PROGRAM = String.raw`
import hashlib, json, os, pathlib, sqlite3, stat, sys, time
def refuse():
    raise ValueError("private_backup_refused")
def identity(path):
    item = os.lstat(path)
    if not stat.S_ISREG(item.st_mode) or item.st_nlink != 1 or item.st_uid != os.getuid():
        refuse()
    return item
try:
    raw = sys.stdin.buffer.read(1048577)
    if len(raw) > 1048576:
        refuse()
    value = json.loads(raw)
    if set(value) != {"source", "destination"}:
        refuse()
    source, destination = value["source"], value["destination"]
    before = identity(source)
    parent = pathlib.Path(destination).parent
    info = os.lstat(parent)
    if not stat.S_ISDIR(info.st_mode) or info.st_mode & 0o077 or info.st_uid != os.getuid():
        refuse()
    fd = os.open(destination, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    os.close(fd)
    progress = None
    def observed(status, remaining, total):
        global progress
        progress = {"status": status, "remaining": remaining, "total": total}
    started = time.time_ns() // 1000000
    reader = sqlite3.connect(pathlib.Path(source).as_uri() + "?mode=ro", uri=True)
    try:
        writer = sqlite3.connect(destination)
        try:
            reader.backup(writer, pages=256, progress=observed)
        finally:
            writer.close()
    finally:
        reader.close()
    finished = time.time_ns() // 1000000
    after = identity(source)
    copied = identity(destination)
    if (before.st_dev, before.st_ino) != (after.st_dev, after.st_ino):
        refuse()
    if (copied.st_dev, copied.st_ino) == (before.st_dev, before.st_ino):
        refuse()
    if any(os.path.lexists(destination + suffix) for suffix in ("-wal", "-shm", "-journal")):
        refuse()
    if not progress or progress["status"] != 101 or progress["remaining"] != 0 or progress["total"] <= 0:
        refuse()
    digest = hashlib.sha256()
    with open(destination, "rb") as file:
        os.fsync(file.fileno())
        while True:
            chunk = file.read(1048576)
            if not chunk:
                break
            digest.update(chunk)
    print(json.dumps({"v": 1, "method": "sqlite3.Connection.backup", "source_mode": "ro",
        "source": source, "source_device": before.st_dev, "source_inode": before.st_ino,
        "destination": destination, "bytes": copied.st_size, "destination_sha256": digest.hexdigest(),
        "started_at_ms": started, "finished_at_ms": finished, "progress": progress,
        "consistent_backup_complete": True}))
except Exception:
    sys.stderr.write("private_backup_refused")
    sys.exit(78)
`;

const refused = (): Error => new Error("Private Bun copy capture refused; no success receipt was published");
const object = (value: unknown): value is Record<string, unknown> => typeof value === "object" && value !== null && !Array.isArray(value);
const exactKeys = (value: Record<string, unknown>, expected: readonly string[]): boolean =>
  Object.keys(value).length === expected.length && expected.every((key) => Object.hasOwn(value, key));
const integer = (value: unknown): value is number => typeof value === "number" && Number.isSafeInteger(value) && value >= 0;
const digest = (value: unknown): value is string => typeof value === "string" && /^[a-f0-9]{64}$/.test(value);

function inputValue(value: unknown): PrivateBunCopyInput {
  if (
    !object(value) ||
    !exactKeys(value, ["v", "source", "directory", "source_git_sha"]) ||
    value.v !== 1 ||
    typeof value.source !== "string" ||
    !isAbsolute(value.source) ||
    typeof value.directory !== "string" ||
    !isAbsolute(value.directory) ||
    typeof value.source_git_sha !== "string" ||
    !/^[a-f0-9]{40}$/.test(value.source_git_sha)
  )
    throw refused();
  return { v: 1, source: value.source, directory: value.directory, source_git_sha: value.source_git_sha };
}

function fileFact(path: string): FileFact {
  const before = lstatSync(path);
  if (!before.isFile() || before.nlink !== 1 || before.uid !== process.getuid?.() || (before.mode & 0o777) !== 0o600) throw refused();
  const fd = openSync(path, constants.O_RDONLY | constants.O_NOFOLLOW);
  try {
    const opened = fstatSync(fd);
    if (opened.dev !== before.dev || opened.ino !== before.ino) throw refused();
    const hash = createHash("sha256");
    const bytes = Buffer.alloc(1024 * 1024);
    let count = 0;
    for (;;) {
      const read = readSync(fd, bytes);
      if (read === 0) break;
      hash.update(bytes.subarray(0, read));
      count += read;
    }
    const after = lstatSync(path);
    if (after.dev !== before.dev || after.ino !== before.ino || after.size !== count || after.mtimeMs !== before.mtimeMs) throw refused();
    return { path, bytes: count, sha256: hash.digest("hex") };
  } finally {
    closeSync(fd);
  }
}

function privateJson(path: string, value: unknown): FileFact {
  const fd = openSync(path, constants.O_WRONLY | constants.O_CREAT | constants.O_EXCL | constants.O_NOFOLLOW, 0o600);
  try {
    writeFileSync(fd, JSON.stringify(value) + "\n");
    fsyncSync(fd);
  } finally {
    closeSync(fd);
  }
  return fileFact(path);
}

function publishReceipt(directory: string, value: PrivateBunCopyReceipt): void {
  const path = join(directory, "receipt.json");
  const directoryFd = openSync(directory, constants.O_RDONLY | constants.O_DIRECTORY | constants.O_NOFOLLOW);
  let fd: number | undefined;
  let created: { dev: number; ino: number } | undefined;
  try {
    fsyncSync(directoryFd);
    fd = openSync(path, constants.O_WRONLY | constants.O_CREAT | constants.O_EXCL | constants.O_NOFOLLOW, 0o600);
    created = fstatSync(fd);
    writeFileSync(fd, JSON.stringify(value) + "\n");
    fsyncSync(fd);
    fsyncSync(directoryFd);
  } catch {
    if (created) {
      const current = lstatSync(path);
      if (current.dev === created.dev && current.ino === created.ino) unlinkSync(path);
    }
    throw refused();
  } finally {
    if (fd !== undefined) closeSync(fd);
    closeSync(directoryFd);
  }
}

async function boundedText(stream: ReadableStream<Uint8Array>, maximum: number): Promise<string> {
  const reader = stream.getReader();
  const chunks: Uint8Array[] = [];
  let length = 0;
  try {
    for (;;) {
      const next = await reader.read();
      if (next.done) break;
      length += next.value.length;
      if (length > maximum) throw refused();
      chunks.push(next.value);
    }
    return Buffer.concat(chunks).toString("utf8");
  } finally {
    reader.releaseLock();
  }
}

function backupValue(raw: string, source: string, snapshot: FileFact): BackupReceipt {
  const value: unknown = JSON.parse(raw);
  if (
    !object(value) ||
    !exactKeys(value, [
      "v",
      "method",
      "source_mode",
      "source",
      "source_device",
      "source_inode",
      "destination",
      "bytes",
      "destination_sha256",
      "started_at_ms",
      "finished_at_ms",
      "progress",
      "consistent_backup_complete",
    ]) ||
    value.v !== 1 ||
    value.method !== "sqlite3.Connection.backup" ||
    value.source_mode !== "ro" ||
    value.source !== source ||
    value.destination !== snapshot.path ||
    value.bytes !== snapshot.bytes ||
    value.destination_sha256 !== snapshot.sha256 ||
    !digest(value.destination_sha256) ||
    !integer(value.source_device) ||
    !integer(value.source_inode) ||
    !integer(value.started_at_ms) ||
    !integer(value.finished_at_ms) ||
    value.finished_at_ms < value.started_at_ms ||
    value.consistent_backup_complete !== true ||
    !object(value.progress) ||
    !exactKeys(value.progress, ["status", "remaining", "total"]) ||
    value.progress.status !== 101 ||
    value.progress.remaining !== 0 ||
    !integer(value.progress.total) ||
    value.progress.total === 0
  )
    throw refused();
  return {
    v: 1,
    method: "sqlite3.Connection.backup",
    source_mode: "ro",
    source,
    source_device: value.source_device,
    source_inode: value.source_inode,
    destination: snapshot.path,
    bytes: snapshot.bytes,
    destination_sha256: snapshot.sha256,
    started_at_ms: value.started_at_ms,
    finished_at_ms: value.finished_at_ms,
    progress: { status: 101, remaining: 0, total: value.progress.total },
    consistent_backup_complete: true,
  };
}

async function backup(source: string, destination: string, binding: PrivateBunCopyPython, signal?: AbortSignal): Promise<string> {
  signal?.throwIfAborted();
  const child = Bun.spawn([PrivateBunCopyPython.command(binding), "-I", "-S", "-B", "-c", BACKUP_PROGRAM], {
    cwd: dirname(destination),
    env: { PATH: "/usr/bin:/bin", SQLITE_TMPDIR: dirname(destination) },
    stdin: "pipe",
    stdout: "pipe",
    stderr: "pipe",
  });
  const stop = (): void => {
    if (child.exitCode !== null) return;
    try {
      child.kill("SIGKILL");
    } catch {
      return;
    }
  };
  signal?.addEventListener("abort", stop, { once: true });
  const stdout = boundedText(child.stdout, CHILD_MAX_BYTES);
  const stderr = boundedText(child.stderr, CHILD_MAX_BYTES);
  try {
    if (signal?.aborted) stop();
    await child.stdin.write(JSON.stringify({ source, destination }));
    await child.stdin.end();
    const [text, error, code] = await Promise.all([stdout, stderr, child.exited]);
    PrivateBunCopyPython.command(binding);
    if (code !== 0 || error.length !== 0) throw refused();
    signal?.throwIfAborted();
    return text;
  } catch {
    stop();
    await Promise.allSettled([stdout, stderr, child.exited]);
    throw refused();
  } finally {
    signal?.removeEventListener("abort", stop);
  }
}

/** Source writers stay independent; only the newly owned copy is captured. */
export async function writePrivateBunCopyCapture(
  value: PrivateBunCopyInput,
  signal?: AbortSignal,
  binding?: PrivateBunCopyPython
): Promise<PrivateBunCopyReceipt> {
  try {
    if (process.platform !== "darwin" || Bun.version !== "1.4.0") throw refused();
    if (!binding) throw refused();
    PrivateBunCopyPython.command(binding);
    const input = inputValue(value);
    signal?.throwIfAborted();
    const source = realpathSync(input.source);
    const sourceInfo = lstatSync(input.source);
    if (!sourceInfo.isFile() || sourceInfo.nlink !== 1 || sourceInfo.uid !== process.getuid?.() || source !== resolve(input.source)) throw refused();
    const parent = realpathSync(dirname(input.directory));
    const parentInfo = lstatSync(parent);
    if (!parentInfo.isDirectory() || parentInfo.uid !== process.getuid() || (parentInfo.mode & 0o077) !== 0) throw refused();
    const directory = join(parent, basename(input.directory));
    if (directory === source || resolve(input.directory) !== directory) throw refused();
    mkdirSync(directory, { mode: 0o700 });
    const snapshotPath = join(directory, "snapshot.sqlite3");
    const raw = await backup(source, snapshotPath, binding, signal);
    const snapshot = fileFact(snapshotPath);
    const copied = lstatSync(snapshotPath);
    const currentSource = lstatSync(source);
    if (copied.dev === sourceInfo.dev && copied.ino === sourceInfo.ino) throw refused();
    if (currentSource.dev !== sourceInfo.dev || currentSource.ino !== sourceInfo.ino) throw refused();
    const observed = backupValue(raw, source, snapshot);
    if (observed.source_device !== sourceInfo.dev || observed.source_inode !== sourceInfo.ino) throw refused();
    const backupReceipt = privateJson(join(directory, "backup-receipt.json"), observed);
    signal?.throwIfAborted();
    const capture = await writePrivateNativeCapture(
      snapshotPath,
      directory,
      input.source_git_sha,
      observed.finished_at_ms,
      join(directory, "capture.private.json")
    );
    signal?.throwIfAborted();
    const captureFact = fileFact(capture.destination);
    if (
      captureFact.sha256 !== capture.sha256 ||
      captureFact.bytes !== capture.bytes ||
      capture.source_identity.sha256 !== snapshot.sha256 ||
      capture.source_identity.size !== String(snapshot.bytes)
    )
      throw refused();
    const captureReceipt = privateJson(join(directory, "capture-receipt.json"), capture);
    if (fileFact(snapshotPath).sha256 !== snapshot.sha256) throw refused();
    const receipt: PrivateBunCopyReceipt = {
      v: 1,
      kind: "private_bun_copy_capture",
      source_git_sha: input.source_git_sha,
      snapshot,
      backup_receipt: backupReceipt,
      capture,
      capture_receipt: captureReceipt,
      source_quiescence_proven: false,
      custody_verified: false,
      activation_authorized: false,
    };
    publishReceipt(directory, receipt);
    return receipt;
  } catch {
    throw refused();
  }
}

export async function runPrivateBunCopyOperator(binding: PrivateBunCopyPython): Promise<void> {
  PrivateBunCopyPython.command(binding);
  const controller = new AbortController();
  const stop = (): void => {
    controller.abort();
  };
  process.once("SIGINT", stop);
  process.once("SIGTERM", stop);
  try {
    if (process.argv.length !== 2) throw refused();
    const input: unknown = JSON.parse(await boundedText(Bun.stdin.stream(), INPUT_MAX_BYTES));
    const receipt = await writePrivateBunCopyCapture(inputValue(input), controller.signal, binding);
    console.log(JSON.stringify(receipt));
  } catch {
    console.error(refused().message);
    process.exitCode = 78;
  } finally {
    process.removeListener("SIGINT", stop);
    process.removeListener("SIGTERM", stop);
  }
}
if (import.meta.main) {
  console.error(refused().message);
  process.exitCode = 78;
}
