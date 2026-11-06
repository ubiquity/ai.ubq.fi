// Private prepared-tool inputs. No provisioning, download, ambient flags or
// executable discovery: every helper is bound by the existing --frames input.
import assert from "node:assert/strict";
import { lstat, readFile, writeFile } from "node:fs/promises";
import { isAbsolute, join } from "node:path";
import { ARTIFACT_SHA, ARTIFACT_TREE, bytesHash, object, type Fixture, type Json } from "./state-transfer-freeze-support.ts";

export class FixturePrerequisite extends Error {}
export const preparedPath = (frames: string): string => `${frames}.prepared.json`;

export async function preparedTool(fixture: Fixture, name: string): Promise<Readonly<{ path: string; sha256: string }>> {
  const path = preparedPath(fixture.inputs.framesPath);
  let metadata;
  try {
    metadata = await lstat(path);
  } catch (error) {
    if (error instanceof Error && "code" in error && error.code === "ENOENT")
      throw new FixturePrerequisite(`Prepared ${name} helper manifest is absent: ${path}`);
    throw error;
  }
  assert(metadata.isFile() && !metadata.isSymbolicLink() && (metadata.mode & 0o077) === 0, "Prepared fixture manifest must be a private regular file");
  const manifest = object(JSON.parse(await readFile(path, "utf8")));
  assert.equal(manifest.v, 1);
  assert.equal(manifest.artifact_sha, ARTIFACT_SHA);
  assert.equal(manifest.artifact_tree, ARTIFACT_TREE);
  assert.equal(manifest.frames_sha256, await bytesHash(fixture.inputs.framesPath));
  if (!manifest[name]) throw new FixturePrerequisite(`Prepared ${name} helper is not bound in ${path}`);
  const tool = object(manifest[name]);
  assert(typeof tool.path === "string" && isAbsolute(tool.path));
  assert(typeof tool.sha256 === "string" && /^[a-f0-9]{64}$/u.test(tool.sha256));
  const file = await lstat(tool.path);
  assert(file.isFile() && !file.isSymbolicLink() && (file.mode & 0o111) !== 0, "Prepared helper must be a regular executable");
  assert.equal(await bytesHash(tool.path), tool.sha256);
  return { path: tool.path, sha256: tool.sha256 };
}

export async function invokePrepared(fixture: Fixture, tool: Readonly<{ path: string; sha256: string }>, name: string, input: Json): Promise<Json> {
  const raw = JSON.stringify(input);
  assert(Buffer.byteLength(raw) <= 65_536);
  const inputPath = join(fixture.directory, `${name}.input.private.json`);
  await writeFile(inputPath, raw, { flag: "wx", mode: 0o600 });
  const argv = name === "native_constructor_fixture" ? [tool.path, "--exact", "produce_freeze_native_fixture", "--nocapture"] : [tool.path];
  const child = Bun.spawn(argv, {
    cwd: fixture.directory,
    env: { PATH: "/usr/bin:/bin", TMPDIR: fixture.directory },
    stdin: Bun.file(inputPath),
    stdout: "pipe",
    stderr: "pipe",
  });
  const timer = setTimeout(() => {
    child.kill("SIGKILL");
  }, 15_000);
  try {
    const [code, output, errors] = await Promise.all([child.exited, new Response(child.stdout).text(), new Response(child.stderr).text()]);
    await writeFile(join(fixture.directory, `${name}.stdout.private.json`), output, { flag: "wx", mode: 0o600 });
    await writeFile(join(fixture.directory, `${name}.stderr.log`), errors, { flag: "wx", mode: 0o600 });
    assert.equal(code, 0, `${name} helper failed: ${errors}`);
    assert.equal(await bytesHash(tool.path), tool.sha256, "Prepared helper changed during execution");
    fixture.facts.push({ operation: name, argv, executable_sha256: tool.sha256, input_path: inputPath, exit_code: code });
    if (name === "native_constructor_fixture") {
      const prefix = "UOS_FREEZE_NATIVE_FIXTURE=";
      const records = output.split("\n").filter((line) => line.startsWith(prefix));
      assert.equal(records.length, 1, "The exact successful constructor test must emit one complete fixture record");
      return object(JSON.parse(records[0].slice(prefix.length)));
    }
    return object(JSON.parse(output));
  } finally {
    clearTimeout(timer);
  }
}
