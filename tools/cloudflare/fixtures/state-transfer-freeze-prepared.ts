// Private prepared-tool inputs. No provisioning, download, ambient flags or
// executable discovery: every helper is bound by the existing --frames input.
import assert from "node:assert/strict";
import { lstat, readFile, writeFile } from "node:fs/promises";
import { isAbsolute, join } from "node:path";
import { ARTIFACT_SHA, ARTIFACT_TREE, bytesHash, object, type Fixture, type Json, type WriterFixture } from "./state-transfer-freeze-support.ts";

export class FixturePrerequisite extends Error {}
export const preparedPath = (frames: string): string => `${frames}.prepared.json`;

async function preparedInput(fixture: Fixture, name: string): Promise<Json> {
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
  return object(manifest[name]);
}

async function preparedFile(value: unknown): Promise<Readonly<{ path: string; sha256: string }>> {
  const tool = object(value);
  assert(typeof tool.path === "string" && isAbsolute(tool.path));
  assert(typeof tool.sha256 === "string" && /^[a-f0-9]{64}$/u.test(tool.sha256));
  const file = await lstat(tool.path);
  assert(file.isFile() && !file.isSymbolicLink(), "Prepared input must be a regular file");
  assert.equal(await bytesHash(tool.path), tool.sha256);
  return { path: tool.path, sha256: tool.sha256 };
}

export async function preparedTool(fixture: Fixture, name: string): Promise<Readonly<{ path: string; sha256: string }>> {
  const tool = await preparedFile(await preparedInput(fixture, name));
  assert(((await lstat(tool.path)).mode & 0o111) !== 0, "Prepared helper must be executable");
  return tool;
}

export async function preparedWriter(fixture: Fixture): Promise<WriterFixture> {
  const value = await preparedInput(fixture, "writer_fixture");
  const js = await preparedFile(value.js);
  const wasm = await preparedFile(value.wasm);
  assert(typeof value.source_sha === "string" && /^[a-f0-9]{40}$/u.test(value.source_sha));
  assert(typeof value.source_tree === "string" && /^[a-f0-9]{40}$/u.test(value.source_tree));
  return { js: js.path, jsSha256: js.sha256, wasm: wasm.path, wasmSha256: wasm.sha256, sourceSha: value.source_sha, sourceTree: value.source_tree };
}

export async function preparedBunSupport(fixture: Fixture): Promise<Readonly<{ path: string; sha256: string; sourceRoot: string }>> {
  const value = await preparedInput(fixture, "bun_inverse");
  const tool = await preparedFile(value);
  assert.equal(tool.sha256, "5369b18b7fc6794dfe483438b1919c01053838d88acb9863ea1100b6fe6dbafa", "Reviewed standalone Bun support changed");
  assert(typeof value.source_root === "string" && isAbsolute(value.source_root));
  return { ...tool, sourceRoot: value.source_root };
}

function helperArgv(path: string, name: string): string[] {
  if (name === "native_constructor_fixture") return [path, "--exact", "produce_freeze_native_fixture", "--nocapture"];
  if (name === "bun_inverse_restore") return [process.execPath, "--no-env-file", "--no-install", path];
  return [path];
}

export async function invokePrepared(fixture: Fixture, tool: Readonly<{ path: string; sha256: string }>, name: string, input: Json): Promise<Json> {
  const raw = JSON.stringify(input);
  assert(Buffer.byteLength(raw) <= 65_536);
  const inputPath = join(fixture.directory, `${name}.input.private.json`);
  await writeFile(inputPath, raw, { flag: "wx", mode: 0o600 });
  const argv = helperArgv(tool.path, name);
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
