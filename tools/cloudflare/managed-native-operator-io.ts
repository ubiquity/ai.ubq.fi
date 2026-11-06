/** Strict private operator IO. Credentials stay in owner-only files and process memory. */
import { constants, openSync, closeSync, fstatSync, lstatSync, readFileSync, realpathSync, fsyncSync, writeSync } from "node:fs";
import { isAbsolute, resolve, join } from "node:path";
import { createHash } from "node:crypto";
import { ManagedAdoptionError, type ManagedAdoptionExpectation } from "../../src/codex/managed-native-adoption.ts";

export function adoptionRefused(code: string): never {
  throw new ManagedAdoptionError(code);
}
export const bytesSha256 = (value: Uint8Array): string => createHash("sha256").update(value).digest("hex");
export const canonicalOwnedDirectory = (path: string): string => {
  if (!isAbsolute(path) || resolve(path) !== path || realpathSync(path) !== path) adoptionRefused("directory_identity");
  const info = lstatSync(path);
  if (!info.isDirectory() || info.uid !== process.getuid?.()) adoptionRefused("directory_owner");
  return path;
};
export const fileIdentity = (path: string): string => {
  const info = lstatSync(path, { bigint: true });
  return [info.dev, info.ino, info.mode, info.uid, info.nlink, info.size, info.mtimeNs, info.ctimeNs].join(":");
};
export const readBoundFile = (path: string, maximum: number, privateData: boolean): Readonly<{ bytes: Buffer; identity: string }> => {
  if (!isAbsolute(path) || realpathSync(path) !== path) adoptionRefused("file_identity");
  const fd = openSync(path, constants.O_RDONLY | constants.O_NOFOLLOW);
  try {
    const info = fstatSync(fd);
    const mask = privateData ? 0o077 : 0o022;
    if (!info.isFile() || info.nlink !== 1 || info.uid !== process.getuid?.() || (info.mode & mask) !== 0 || info.size > maximum)
      adoptionRefused("file_owner_or_bound");
    const identity = fileIdentity(path);
    if (info.dev !== lstatSync(path).dev || info.ino !== lstatSync(path).ino) adoptionRefused("file_changed");
    const bytes = readFileSync(fd);
    if (bytes.length !== info.size || fileIdentity(path) !== identity) adoptionRefused("file_changed");
    return { bytes, identity };
  } finally {
    closeSync(fd);
  }
};
const outputReference = (value: unknown): string => {
  if (typeof value !== "string" || !/^[a-z0-9][a-z0-9.-]{0,80}\.json$/.test(value)) adoptionRefused("output_reference");
  return value;
};
/** The open descriptor pins the authorized directory even during path replacement. */
export class PrivateOutputAnchor {
  readonly #path: string;
  readonly #fd: number;
  readonly #identity: string;
  #closed = false;
  private constructor(path: string, fd: number) {
    this.#path = path;
    this.#fd = fd;
    this.#identity = this.#current();
  }
  static open(root: string, path: string): PrivateOutputAnchor {
    const data = join(canonicalOwnedDirectory(root), ".data");
    canonicalOwnedDirectory(data);
    canonicalOwnedDirectory(path);
    if (
      process.platform !== "linux" ||
      !path.startsWith(data + "/") ||
      ["releases", "runtimes"].some((name) => path === join(data, name) || path.startsWith(join(data, name) + "/"))
    )
      adoptionRefused("output_scope");
    const fd = openSync(path, constants.O_RDONLY | constants.O_DIRECTORY | constants.O_NOFOLLOW);
    try {
      return new PrivateOutputAnchor(path, fd);
    } catch (error) {
      closeSync(fd);
      throw error;
    }
  }
  #current(): string {
    const stat = fstatSync(this.#fd);
    if (!stat.isDirectory() || stat.uid !== process.getuid?.() || (stat.mode & 0o777) !== 0o700) adoptionRefused("output_directory_private");
    return [stat.dev, stat.ino, stat.uid, stat.mode].join(":");
  }
  assert(): void {
    if (this.#closed || this.#current() !== this.#identity || realpathSync(this.#path) !== this.#path) adoptionRefused("output_anchor_changed");
    const stat = lstatSync(this.#path);
    if (!stat.isDirectory() || [stat.dev, stat.ino, stat.uid, stat.mode].join(":") !== this.#identity) adoptionRefused("output_anchor_changed");
  }
  #file(reference: string): string {
    this.assert();
    return `/proc/self/fd/${this.#fd}/${outputReference(reference)}`;
  }
  exists(reference: string): boolean {
    try {
      lstatSync(this.#file(reference));
      return true;
    } catch (error) {
      if (error instanceof Error && "code" in error && error.code === "ENOENT") {
        this.assert();
        return false;
      }
      throw error;
    }
  }
  write(reference: string, value: unknown): void {
    const fd = openSync(this.#file(reference), constants.O_WRONLY | constants.O_CREAT | constants.O_EXCL | constants.O_NOFOLLOW, 0o600);
    try {
      const bytes = Buffer.from(JSON.stringify(value) + "\n");
      let offset = 0;
      while (offset < bytes.length) offset += writeSync(fd, bytes, offset);
      fsyncSync(fd);
      this.assert();
      fsyncSync(this.#fd);
    } finally {
      closeSync(fd);
    }
  }
  close(): void {
    if (!this.#closed) {
      closeSync(this.#fd);
      this.#closed = true;
    }
  }
}
export const boundedOperatorText = async (stream: ReadableStream<Uint8Array>, maximum: number): Promise<string> => {
  const reader = stream.getReader();
  const chunks: Uint8Array[] = [];
  let size = 0;
  try {
    for (;;) {
      const item = await reader.read();
      if (item.done) break;
      size += item.value.length;
      if (size > maximum) adoptionRefused("input_or_output_bound");
      chunks.push(item.value);
    }
    return Buffer.concat(chunks).toString("utf8");
  } finally {
    await reader.cancel();
    reader.releaseLock();
  }
};
const object = (value: unknown, keys: readonly string[]): Record<string, unknown> => {
  if (!value || typeof value !== "object" || Array.isArray(value) || Object.keys(value).length !== keys.length || !keys.every((k) => Object.hasOwn(value, k)))
    adoptionRefused("input_schema");
  return value as Record<string, unknown>;
};
export type ManagedAdoptionInput = Readonly<{ expected: ManagedAdoptionExpectation; outputs: Readonly<{ backup: string; intent: string; receipt: string }> }>;
export const decodeManagedAdoptionInput = (text: string): ManagedAdoptionInput => {
  if (!text || Buffer.byteLength(text) > 4096) adoptionRefused("input_schema");
  let raw: unknown;
  try {
    raw = JSON.parse(text);
  } catch {
    adoptionRefused("input_schema");
  }
  const value = object(raw, ["v", "expected", "outputs"]);
  const expected = object(value.expected, ["pool_versionstamp", "pool_sha256", "native_sha256"]);
  const outputs = object(value.outputs, ["backup", "intent", "receipt"]);
  if (value.v !== 1 || typeof expected.pool_versionstamp !== "string" || !/^[a-f0-9]{20}$/.test(expected.pool_versionstamp)) adoptionRefused("input_schema");
  for (const key of ["pool_sha256", "native_sha256"])
    if (typeof expected[key] !== "string" || !/^[a-f0-9]{64}$/.test(expected[key])) adoptionRefused("input_schema");
  for (const path of Object.values(outputs)) outputReference(path);
  if (new Set(Object.values(outputs)).size !== 3) adoptionRefused("output_reference");
  return {
    expected: { poolVersionstamp: expected.pool_versionstamp, poolSha256: String(expected.pool_sha256), nativeSha256: String(expected.native_sha256) },
    outputs: outputs as ManagedAdoptionInput["outputs"],
  };
};
/** Direct method callers receive the same closed validation as stdin callers. */
export const revalidateManagedAdoptionInput = (input: unknown): ManagedAdoptionInput => {
  const value = object(input, ["expected", "outputs"]);
  const expected = object(value.expected, ["poolVersionstamp", "poolSha256", "nativeSha256"]);
  return decodeManagedAdoptionInput(
    JSON.stringify({
      v: 1,
      expected: {
        pool_versionstamp: expected.poolVersionstamp,
        pool_sha256: expected.poolSha256,
        native_sha256: expected.nativeSha256,
      },
      outputs: value.outputs,
    })
  );
};
