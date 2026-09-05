import assert from "node:assert/strict";

const readPermission = await Deno.permissions.query({ name: "read" });
const writePermission = await Deno.permissions.query({ name: "write" });
const runPermission = await Deno.permissions.query({ name: "run" });
const isolationUnavailable = readPermission.state !== "granted" || writePermission.state !== "granted" ||
  runPermission.state !== "granted";

const BOOTSTRAP_PACKAGE_DIR = "scripts/sentinel/bootstrap";

// Frozen manifest that the bootstrap workflow enforces at runtime. Reconstructed
// below from the real package bytes (never trusted from a second constant), so
// a tampered package, a removed file, or an omitted nested file fails this test
// and the workflow fails closed.
const BOOTSTRAP_PACKAGE_MANIFEST_DIGEST = "a9a3ab1bfb652ec2caafdb74fff02d03a2809134b7ac603c6a8e8a763353f32c";

// Provider/evolving sources that must never be reachable from the pinned
// bootstrap package. Each is deliberately broken so an accidental import
// fails the isolated module load instead of silently resolving.
const BROKEN_PROVIDER_FILES: ReadonlyArray<readonly [string, string]> = [
  ["src/cerebras.ts", "export const syntaxError = 1 + ;"],
  ["src/harmony/classifier.ts", "export const syntaxError = 1 + ;"],
  ["scripts/sentinel/main.ts", "export const syntaxError = 1 + ;"],
  ["scripts/sentinel/recovery-ledger.ts", "export const syntaxError = 1 + ;"],
  ["scripts/sentinel/retry.ts", "export const syntaxError = 1 + ;"],
];

type DenoInvocation = Readonly<{ exitCode: number; stdout: string; stderr: string }>;

const invokeDeno = async (args: readonly string[], cwd: string): Promise<DenoInvocation> => {
  const command = new Deno.Command(Deno.execPath(), {
    args: [...args],
    cwd,
    stdout: "piped",
    stderr: "piped",
    // The isolated children inherit this test process environment so the
    // toolchain variables (PATH/DENO_DIR) stay available; they run without a
    // network permission flag. This test proves environment inheritance and
    // package/config isolation, not a full security sandbox.
  });
  const outcome = await command.output();
  const decoder = new TextDecoder();
  return {
    exitCode: outcome.code,
    stdout: decoder.decode(outcome.stdout),
    stderr: decoder.decode(outcome.stderr),
  };
};

const sha256Hex = async (data: Uint8Array): Promise<string> => {
  // `new Uint8Array(data)` copies into a fresh ArrayBuffer-backed buffer,
  // satisfying crypto.subtle.digest's BufferSource typing regardless of the
  // source's ArrayBufferLike generic.
  const digest = new Uint8Array(await crypto.subtle.digest("SHA-256", new Uint8Array(data)));
  return [...digest].map((byte) => byte.toString(16).padStart(2, "0")).join("");
};

// Every regular file under the package, recursively, ordered byte-wise like
// `LC_ALL=C sort`. Symlinks and any other non-regular entry are rejected so
// the manifest cannot be redirected outside the package.
const packageFileEntries = async (
  packageDir: string,
): Promise<ReadonlyArray<readonly [string, string]>> => {
  const entries: Array<readonly [string, string]> = [];
  const walk = async (directory: string, relativeDirectory: string): Promise<void> => {
    for await (const entry of Deno.readDir(directory)) {
      const absolute = `${directory}/${entry.name}`;
      const relative = relativeDirectory === "" ? entry.name : `${relativeDirectory}/${entry.name}`;
      if (entry.isDirectory) {
        await walk(absolute, relative);
      } else if (entry.isFile) {
        entries.push([absolute, relative]);
      } else {
        throw new Error(`bootstrap package contains a non-regular entry: ${absolute}`);
      }
    }
  };
  await walk(packageDir, "");
  entries.sort(([, relativeA], [, relativeB]) => (relativeA < relativeB ? -1 : relativeA > relativeB ? 1 : 0));
  return entries;
};

// sha256sum-style manifest digest: "<hex>  <repo-relative-path>\n" per file in
// sorted path order, hashed once more — the workflow's exact pipeline (find
// -type f -print0 | LC_ALL=C sort -z | xargs -0 sha256sum | sha256sum).
const packageManifestDigest = async (
  entries: ReadonlyArray<readonly [string, string]>,
): Promise<string> => {
  const lines: string[] = [];
  for (const [absolute, relative] of entries) {
    lines.push(`${await sha256Hex(await Deno.readFile(absolute))}  ${BOOTSTRAP_PACKAGE_DIR}/${relative}`);
  }
  return sha256Hex(new TextEncoder().encode(`${lines.join("\n")}\n`));
};

Deno.test({
  name: "bootstrap entrypoint loads from an isolated package with no provider, evolving, or config dependency",
  ignore: isolationUnavailable,
  async fn() {
    const parent = await Deno.makeTempDir({ prefix: "sentinel-bootstrap-isolation-" });
    const root = `${parent}/nested`;
    const packageDir = `${root}/${BOOTSTRAP_PACKAGE_DIR}`;
    try {
      await Deno.mkdir(packageDir, { recursive: true });
      // The surrounding repository config is invalid JSON, so any invocation
      // without --no-config fails and cannot influence the package.
      await Deno.writeTextFile(`${parent}/deno.json`, '{"imports": {');
      for (const [path, content] of BROKEN_PROVIDER_FILES) {
        const target = `${root}/${path}`;
        await Deno.mkdir(target.slice(0, target.lastIndexOf("/")), { recursive: true });
        await Deno.writeTextFile(target, content);
      }
      // Bind the tracked package to the frozen manifest first: the digest is
      // computed from the real bytes, so any tampering or missing nested file
      // fails here rather than being masked by comparing two constants.
      const sourceEntries = await packageFileEntries(`${Deno.cwd()}/${BOOTSTRAP_PACKAGE_DIR}`);
      const sourceDigest = await packageManifestDigest(sourceEntries);
      assert.equal(
        sourceDigest,
        BOOTSTRAP_PACKAGE_MANIFEST_DIGEST,
        "the tracked bootstrap package must match its frozen manifest",
      );
      // Copy EVERY package file (any depth, any extension) into the isolated
      // tree, then prove the copy regenerates the same digest byte-for-byte.
      for (const [sourceAbsolute, relative] of sourceEntries) {
        const target = `${packageDir}/${relative}`;
        await Deno.mkdir(target.slice(0, target.lastIndexOf("/")), { recursive: true });
        await Deno.copyFile(sourceAbsolute, target);
      }
      const copiedEntries = await packageFileEntries(packageDir);
      assert.equal(
        await packageManifestDigest(copiedEntries),
        sourceDigest,
        "the isolated copy must be byte-identical and include every package file",
      );
      const entrypoint = `${packageDir}/main.ts`;
      const expectedModules = new Set(copiedEntries.map(([, relative]) => `${packageDir}/${relative}`));

      // The parent config is discovered without --no-config, which is the
      // exact fragility this prerequisite removes.
      const discovered = await invokeDeno(["info", "--json", "--no-lock", entrypoint], parent);
      assert.notEqual(
        discovered.exitCode,
        0,
        "an invalid surrounding deno.json must be discovered without --no-config",
      );

      // 1. Full deno info graph: closed, complete, and entirely local. The
      // entrypoint is discovered via --no-config/--no-lock; `deno info` has no
      // --cached-only flag, and `deno check` keeps --no-config --no-lock so the
      // invocation matches the pinned CI Deno (2.9.5), so only the `deno run`
      // invocation below carries --cached-only.
      const first = await invokeDeno(["info", "--json", "--no-config", "--no-lock", entrypoint], parent);
      assert.equal(first.exitCode, 0, first.stderr);
      const graph = JSON.parse(first.stdout) as { modules: ReadonlyArray<{ specifier: string }> };
      const modulePaths = graph.modules.map((module) => new URL(module.specifier).pathname);
      assert.equal(
        modulePaths.length,
        copiedEntries.length,
        "bootstrap graph must contain exactly the package modules",
      );
      for (const modulePath of modulePaths) {
        assert.ok(expectedModules.has(modulePath), `bootstrap graph escapes its package: ${modulePath}`);
      }
      for (const expected of expectedModules) {
        assert.ok(modulePaths.includes(expected), `bootstrap package module is missing from the graph: ${expected}`);
      }
      assert.doesNotMatch(first.stdout, /https?:\/\//u, "bootstrap graph must contain no remote module");
      assert.doesNotMatch(first.stdout, /jsr:|npm:|deno\.land/u, "bootstrap graph must contain no registry module");

      // 2. Type-checked module load of the entrypoint resolves with the
      // workflow's --no-config/--no-lock configuration flags.
      const check = await invokeDeno(["check", "--no-config", "--no-lock", entrypoint], parent);
      assert.equal(check.exitCode, 0, check.stderr);

      // 3. Runtime module load succeeds without performing main's remote
      // effects: the entrypoint is imported (never executed as main), so the
      // GitHub observation/dispatch block cannot run even without credentials.
      const wrapper = `${root}/load-entrypoint.ts`;
      await Deno.writeTextFile(wrapper, 'import "./scripts/sentinel/bootstrap/main.ts";\n');
      const loaded = await invokeDeno(["run", "--no-config", "--no-lock", "--cached-only", wrapper], parent);
      assert.equal(loaded.exitCode, 0, loaded.stderr);
      assert.equal(loaded.stdout.trim(), "", "entrypoint import must have no remote effects");

      // 4. A different parent config cannot alter the isolated graph: the
      // --no-config invocations keep resolution identical under a poisoned
      // import map and different compiler options.
      await Deno.writeTextFile(
        `${parent}/deno.json`,
        JSON.stringify({
          imports: { "sentinel-bootstrap-isolation-only": "https://bogus.invalid/never-imported.ts" },
          compilerOptions: { jsx: "react", jsxImportSource: "https://bogus.invalid/jsx" },
        }),
      );
      const second = await invokeDeno(["info", "--json", "--no-config", "--no-lock", entrypoint], parent);
      assert.equal(second.exitCode, 0, second.stderr);
      assert.equal(second.stdout, first.stdout, "parent config must not alter the bootstrap graph");

      // Negative checks with actual bytes: a tampered package file and an
      // omitted nested file must both be rejected by the manifest pipeline.
      const tamperedTarget = `${packageDir}/${copiedEntries[0]![1]}`;
      const originalBytes = await Deno.readFile(tamperedTarget);
      await Deno.writeFile(tamperedTarget, new Uint8Array([...originalBytes, 0x2a]));
      const tamperedDigest = await packageManifestDigest(await packageFileEntries(packageDir));
      assert.notEqual(tamperedDigest, sourceDigest, "a tampered package byte must change the manifest digest");
      assert.notEqual(
        tamperedDigest,
        BOOTSTRAP_PACKAGE_MANIFEST_DIGEST,
        "a tampered package must fail against the frozen manifest",
      );
      // Restore the byte, then add a nested file: a walk that omits nested
      // files leaves the digest unchanged, so only the recursive walk that
      // feeds the manifest detects the omitted file.
      await Deno.writeFile(tamperedTarget, originalBytes);
      const nestedDir = `${packageDir}/nested`;
      await Deno.mkdir(nestedDir, { recursive: true });
      await Deno.writeTextFile(`${nestedDir}/extra.dat`, "nested bytes\n");
      const shallowEntries: Array<readonly [string, string]> = [];
      for await (const entry of Deno.readDir(packageDir)) {
        if (entry.isFile) shallowEntries.push([`${packageDir}/${entry.name}`, entry.name] as const);
      }
      shallowEntries.sort((
        [, relativeA],
        [, relativeB],
      ) => (relativeA < relativeB ? -1 : relativeA > relativeB ? 1 : 0));
      assert.equal(
        await packageManifestDigest(shallowEntries),
        sourceDigest,
        "a nested file omitted from the manifest would be invisible to a top-level-only walk",
      );
      assert.notEqual(
        await packageManifestDigest(await packageFileEntries(packageDir)),
        sourceDigest,
        "the recursive walk must include the nested file and reject it",
      );
    } finally {
      await Deno.remove(parent, { recursive: true }).catch(() => undefined);
    }
  },
});
