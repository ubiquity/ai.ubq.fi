import assert from "node:assert/strict";

const readPermission = await Deno.permissions.query({ name: "read" });
const writePermission = await Deno.permissions.query({ name: "write" });
const runPermission = await Deno.permissions.query({ name: "run" });
const isolationUnavailable = readPermission.state !== "granted" || writePermission.state !== "granted" ||
  runPermission.state !== "granted";

const BOOTSTRAP_PACKAGE_DIR = "scripts/sentinel/bootstrap";

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
      const packageFiles: string[] = [];
      for await (const entry of Deno.readDir(`${Deno.cwd()}/${BOOTSTRAP_PACKAGE_DIR}`)) {
        if (entry.isFile && entry.name.endsWith(".ts")) packageFiles.push(entry.name);
      }
      packageFiles.sort();
      // Copy ONLY the bootstrap package files into the isolated tree.
      for (const name of packageFiles) {
        await Deno.copyFile(`${Deno.cwd()}/${BOOTSTRAP_PACKAGE_DIR}/${name}`, `${packageDir}/${name}`);
      }
      const entrypoint = `${packageDir}/main.ts`;
      const expectedModules = new Set(packageFiles.map((name) => `${packageDir}/${name}`));

      // The parent config is discovered without --no-config, which is the
      // exact fragility this prerequisite removes.
      const discovered = await invokeDeno(["info", "--json", "--no-lock", entrypoint], parent);
      assert.notEqual(
        discovered.exitCode,
        0,
        "an invalid surrounding deno.json must be discovered without --no-config",
      );

      // 1. Full deno info graph: closed, complete, and entirely local.
      const first = await invokeDeno(["info", "--json", "--no-config", "--no-lock", entrypoint], parent);
      assert.equal(first.exitCode, 0, first.stderr);
      const graph = JSON.parse(first.stdout) as { modules: ReadonlyArray<{ specifier: string }> };
      const modulePaths = graph.modules.map((module) => new URL(module.specifier).pathname);
      assert.equal(modulePaths.length, packageFiles.length, "bootstrap graph must contain exactly the package modules");
      for (const modulePath of modulePaths) {
        assert.ok(expectedModules.has(modulePath), `bootstrap graph escapes its package: ${modulePath}`);
      }
      for (const expected of expectedModules) {
        assert.ok(modulePaths.includes(expected), `bootstrap package module is missing from the graph: ${expected}`);
      }
      assert.doesNotMatch(first.stdout, /https?:\/\//u, "bootstrap graph must contain no remote module");
      assert.doesNotMatch(first.stdout, /jsr:|npm:|deno\.land/u, "bootstrap graph must contain no registry module");

      // 2. Type-checked module load of the entrypoint succeeds.
      const check = await invokeDeno(["check", "--no-config", "--no-lock", entrypoint], parent);
      assert.equal(check.exitCode, 0, check.stderr);

      // 3. Runtime module load succeeds without performing main's remote
      // effects: the entrypoint is imported (never executed as main), so the
      // GitHub observation/dispatch block cannot run even without credentials.
      const wrapper = `${root}/load-entrypoint.ts`;
      await Deno.writeTextFile(wrapper, 'import "./scripts/sentinel/bootstrap/main.ts";\n');
      const loaded = await invokeDeno(["run", "--no-config", "--no-lock", wrapper], parent);
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
    } finally {
      await Deno.remove(parent, { recursive: true }).catch(() => undefined);
    }
  },
});
