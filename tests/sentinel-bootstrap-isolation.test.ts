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
const BOOTSTRAP_PACKAGE_MANIFEST_DIGEST = "3b1430379f05ebd9faa804d73d137557ee81f4ceacc171846979c73d9b38c7b3";

// Provider/evolving sources that must never be reachable from the pinned
// bootstrap package. Each is deliberately broken so an accidental import
// fails the isolated module load instead of silently resolving.
const BROKEN_PROVIDER_FILES: ReadonlyArray<readonly [string, string]> = [
  ["src/cerebras.ts", "export const syntaxError = 1 + ;"],
  ["src/harmony/classifier.ts", "export const syntaxError = 1 + ;"],
  ["scripts/sentinel/main.ts", "export const syntaxError = 1 + ;"],
  ["scripts/sentinel/deploy.ts", "export const syntaxError = 1 + ;"],
  ["scripts/sentinel/revision-control.ts", "export const syntaxError = 1 + ;"],
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
  name: "bootstrap entrypoints load from an isolated package with no provider, evolving, or config dependency",
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
      const mainEntrypoint = `${packageDir}/main.ts`;
      const executorEntrypoint = `${packageDir}/revision-control.ts`;
      const expectedModules = new Set(copiedEntries.map(([, relative]) => `${packageDir}/${relative}`));

      // The parent config is discovered without --no-config, which is the
      // exact fragility this prerequisite removes.
      const discovered = await invokeDeno(["info", "--json", "--no-lock", mainEntrypoint], parent);
      assert.notEqual(
        discovered.exitCode,
        0,
        "an invalid surrounding deno.json must be discovered without --no-config",
      );

      // 1. Full deno info graphs for BOTH entrypoints: closed, complete, and
      // entirely local. The entrypoints are discovered via
      // --no-config/--no-lock; `deno info` has no --cached-only flag, and
      // `deno check` keeps --no-config --no-lock so the invocation matches the
      // pinned CI Deno (2.9.5), so only the `deno run` invocations below carry
      // --cached-only.
      const graphModules = async (
        entrypoint: string,
      ): Promise<Readonly<{ modules: readonly string[]; stdout: string }>> => {
        const result = await invokeDeno(["info", "--json", "--no-config", "--no-lock", entrypoint], parent);
        assert.equal(result.exitCode, 0, result.stderr);
        const graph = JSON.parse(result.stdout) as { modules: ReadonlyArray<{ specifier: string }> };
        assert.doesNotMatch(result.stdout, /https?:\/\//u, "bootstrap graph must contain no remote module");
        assert.doesNotMatch(result.stdout, /jsr:|npm:|deno\.land/u, "bootstrap graph must contain no registry module");
        return {
          modules: graph.modules.map((module) => new URL(module.specifier).pathname).sort(),
          stdout: result.stdout,
        };
      };
      const mainInfo = await graphModules(mainEntrypoint);
      const executorInfo = await graphModules(executorEntrypoint);
      const mainModules = mainInfo.modules;
      const executorModules = executorInfo.modules;

      // The union of both entrypoint graphs is exactly the copied package:
      // nothing outside it, nothing inside it unreachable from an entrypoint.
      const unionModules = [...new Set([...mainModules, ...executorModules])].sort();
      assert.deepEqual(
        unionModules,
        [...expectedModules].sort(),
        "the two bootstrap entrypoints must reach exactly the package modules",
      );
      for (const modulePath of unionModules) {
        assert.ok(expectedModules.has(modulePath), `bootstrap graph escapes its package: ${modulePath}`);
      }
      // Each graph alone stays wholly local inside the package.
      for (const modulePath of [...mainModules, ...executorModules]) {
        assert.ok(expectedModules.has(modulePath), `bootstrap graph escapes its package: ${modulePath}`);
      }
      // The controller entrypoint must not load the Deno credential runtime:
      // deploy.ts (and its executor revision-control.ts) are pinned to their
      // own protected executor graph, never into bootstrap main.
      assert.ok(
        !mainModules.includes(`${packageDir}/deploy.ts`),
        "bootstrap main must not load the Deno credential runtime",
      );
      assert.ok(
        !mainModules.includes(`${packageDir}/revision-control.ts`),
        "bootstrap main must not load the revision-control executor",
      );
      assert.deepEqual(
        executorModules,
        [`${packageDir}/deploy.ts`, `${packageDir}/revision-control.ts`].sort(),
        "the executor graph must contain only revision-control.ts and deploy.ts",
      );

      // 2. Type-checked module loads of both entrypoints resolve with the
      // workflow's --no-config/--no-lock configuration flags.
      const mainCheck = await invokeDeno(["check", "--no-config", "--no-lock", mainEntrypoint], parent);
      assert.equal(mainCheck.exitCode, 0, mainCheck.stderr);
      const executorCheck = await invokeDeno(["check", "--no-config", "--no-lock", executorEntrypoint], parent);
      assert.equal(executorCheck.exitCode, 0, executorCheck.stderr);

      // 3. Runtime module loads succeed without performing remote effects:
      // both entrypoints are imported (never executed as main), so the GitHub
      // observation/dispatch block and the promotion CLI cannot run even
      // without credentials or network permission.
      const mainWrapper = `${root}/load-main-entrypoint.ts`;
      await Deno.writeTextFile(mainWrapper, 'import "./scripts/sentinel/bootstrap/main.ts";\n');
      const mainLoaded = await invokeDeno(["run", "--no-config", "--no-lock", "--cached-only", mainWrapper], parent);
      assert.equal(mainLoaded.exitCode, 0, mainLoaded.stderr);
      assert.equal(mainLoaded.stdout.trim(), "", "main entrypoint import must have no remote effects");
      const executorWrapper = `${root}/load-executor-entrypoint.ts`;
      await Deno.writeTextFile(executorWrapper, 'import "./scripts/sentinel/bootstrap/revision-control.ts";\n');
      const executorLoaded = await invokeDeno(
        ["run", "--no-config", "--no-lock", "--cached-only", executorWrapper],
        parent,
      );
      assert.equal(executorLoaded.exitCode, 0, executorLoaded.stderr);
      assert.equal(executorLoaded.stdout.trim(), "", "executor entrypoint import must have no remote effects");

      // 4. A different parent config cannot alter either isolated graph: the
      // --no-config invocations keep resolution identical under a poisoned
      // import map and different compiler options.
      await Deno.writeTextFile(
        `${parent}/deno.json`,
        JSON.stringify({
          imports: { "sentinel-bootstrap-isolation-only": "https://bogus.invalid/never-imported.ts" },
          compilerOptions: { jsx: "react", jsxImportSource: "https://bogus.invalid/jsx" },
        }),
      );
      const secondMain = await invokeDeno(["info", "--json", "--no-config", "--no-lock", mainEntrypoint], parent);
      assert.equal(secondMain.exitCode, 0, secondMain.stderr);
      assert.equal(secondMain.stdout, mainInfo.stdout, "parent config must not alter the bootstrap main graph");
      const secondExecutor = await invokeDeno(
        ["info", "--json", "--no-config", "--no-lock", executorEntrypoint],
        parent,
      );
      assert.equal(secondExecutor.exitCode, 0, secondExecutor.stderr);
      assert.equal(secondExecutor.stdout, executorInfo.stdout, "parent config must not alter the executor graph");

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
