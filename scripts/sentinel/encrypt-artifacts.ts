import {
  decodeSentinelArtifactKey,
  decryptSentinelArtifact,
  encryptSentinelArtifact,
  type SentinelArtifactFile,
} from "./artifact-crypto.ts";

const OUTPUT_NAME = "sentinel-evidence-v1.json";

const equalBytes = (left: Uint8Array, right: Uint8Array): boolean => {
  if (left.byteLength !== right.byteLength) return false;
  let difference = 0;
  for (let index = 0; index < left.byteLength; index += 1) {
    difference |= left[index]! ^ right[index]!;
  }
  return difference === 0;
};

const compareArchivePaths = (
  left: SentinelArtifactFile,
  right: SentinelArtifactFile,
): number => left.path < right.path ? -1 : left.path > right.path ? 1 : 0;

const writeAll = async (
  file: Deno.FsFile,
  bytes: Uint8Array<ArrayBuffer>,
): Promise<void> => {
  let offset = 0;
  while (offset < bytes.byteLength) {
    const written = await file.write(bytes.subarray(offset));
    if (written === 0) {
      throw new Error("Sentinel encrypted artifact write stalled");
    }
    offset += written;
  }
};

const collectFiles = async (
  diskPath: string,
  archivePath: string,
  destination: SentinelArtifactFile[],
): Promise<void> => {
  let information: Deno.FileInfo;
  try {
    information = await Deno.lstat(diskPath);
  } catch (error) {
    if (error instanceof Deno.errors.NotFound) return;
    throw error;
  }
  if (information.isSymlink) {
    throw new Error("Sentinel evidence paths must not contain symbolic links");
  }
  if (information.isFile) {
    destination.push({
      path: archivePath,
      bytes: await Deno.readFile(diskPath),
    });
    return;
  }
  if (!information.isDirectory) {
    throw new Error(
      "Sentinel evidence paths may contain only files and directories",
    );
  }
  const entries = [];
  for await (const entry of Deno.readDir(diskPath)) entries.push(entry.name);
  entries.sort();
  for (const name of entries) {
    await collectFiles(
      `${diskPath}/${name}`,
      `${archivePath}/${name}`,
      destination,
    );
  }
};

const removeIfPresent = async (path: string): Promise<void> => {
  try {
    await Deno.remove(path, { recursive: true });
  } catch (error) {
    if (!(error instanceof Deno.errors.NotFound)) throw error;
  }
};

export const encryptAndVerifyGeneratedEvidence = async (
  sentinelRoot: string,
  keyBytes: Uint8Array<ArrayBuffer>,
): Promise<Readonly<{ fileCount: number; outputPath: string }>> => {
  const files: SentinelArtifactFile[] = [];
  const encryptedRoot = `${sentinelRoot}/encrypted`;
  const outputPath = `${encryptedRoot}/${OUTPUT_NAME}`;
  await collectFiles(`${sentinelRoot}/raw-logs`, "raw-logs", files);
  await collectFiles(`${sentinelRoot}/reports`, "reports", files);
  const encrypted = await encryptSentinelArtifact(files, keyBytes);
  let createdOutput = false;
  try {
    await Deno.mkdir(encryptedRoot, { recursive: true });
    const output = await Deno.open(outputPath, {
      write: true,
      createNew: true,
    });
    createdOutput = true;
    try {
      await writeAll(output, encrypted);
    } finally {
      output.close();
    }
    const persisted = await Deno.readFile(outputPath);
    try {
      const verified = await decryptSentinelArtifact(persisted, keyBytes);
      try {
        const expected = [...files].sort(compareArchivePaths);
        if (
          verified.length !== expected.length ||
          verified.some((file, index) =>
            file.path !== expected[index]!.path ||
            !equalBytes(file.bytes, expected[index]!.bytes)
          )
        ) {
          throw new Error("Sentinel encrypted artifact verification failed");
        }
      } finally {
        for (const file of verified) file.bytes.fill(0);
      }
    } finally {
      persisted.fill(0);
    }
    return { fileCount: files.length, outputPath };
  } catch (error) {
    if (createdOutput) await removeIfPresent(outputPath).catch(() => {});
    throw error;
  } finally {
    encrypted.fill(0);
    for (const file of files) file.bytes.fill(0);
  }
};

export const scrubGeneratedEvidence = async (sentinelRoot: string): Promise<void> => {
  await removeIfPresent(`${sentinelRoot}/raw-logs`);
  await removeIfPresent(`${sentinelRoot}/reports`);
  await removeIfPresent(`${sentinelRoot}/candidate-worktree`);
  await removeIfPresent(`${sentinelRoot}/private`);
};

if (import.meta.main) {
  if (Deno.env.get("GITHUB_ACTIONS") !== "true") {
    throw new Error(
      "Sentinel artifact encryption may run only in GitHub Actions",
    );
  }
  const encodedKey = Deno.env.get("SENTINEL_ARTIFACT_KEY");
  if (!encodedKey) throw new Error("SENTINEL_ARTIFACT_KEY is required");
  const keyBytes = decodeSentinelArtifactKey(encodedKey);
  try {
    const result = await encryptAndVerifyGeneratedEvidence(
      ".sentinel",
      keyBytes,
    );
    console.log(
      `Encrypted and verified ${result.fileCount} Sentinel evidence files.`,
    );
  } finally {
    keyBytes.fill(0);
  }
}
