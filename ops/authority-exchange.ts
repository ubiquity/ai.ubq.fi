import { lstat, readdir } from "node:fs/promises";
import { command } from "./io.ts";

/** Controller-only primitive. Caller holds deploy.lock and proves stopped writers/latest inverse.
 * Never use a normal-rename gap; this does not mint or validate rollback authority.
 */
export const exchangeExistingAuthorityPaths = async (file: string, directory: string): Promise<void> => {
  const left = await lstat(file),
    right = await lstat(directory);
  if (
    !left.isFile() ||
    left.isSymbolicLink() ||
    !right.isDirectory() ||
    right.isSymbolicLink() ||
    left.dev !== right.dev ||
    left.uid !== process.getuid?.() ||
    right.uid !== left.uid ||
    (right.mode & 0o077) !== 0 ||
    (await readdir(directory)).length === 0
  )
    throw new Error("Atomic authority exchange requires an owned file and nonempty private directory on one filesystem");
  await command("/usr/bin/mv", ["--exchange", "--no-copy", "--no-target-directory", "--", file, directory]);
  const afterFile = await lstat(directory),
    afterDirectory = await lstat(file);
  if (afterFile.dev !== left.dev || afterFile.ino !== left.ino || afterDirectory.dev !== right.dev || afterDirectory.ino !== right.ino)
    throw new Error("Atomic authority exchange did not preserve both identities");
};
