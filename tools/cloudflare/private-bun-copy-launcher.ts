/** Offline preparation only: the existing manifest verifier owns source/artifact authority before this composition. */
import { createHash } from "node:crypto";
import { readFileSync, realpathSync } from "node:fs";
import { dirname, join } from "node:path";
import { privateBunCopyProfile, type PrivateBunCopyProfileFacts } from "./private-bun-copy-profile.ts";
import { PrivateBunCopyPython, type PrivateBunCopyPythonSeal } from "./private-bun-copy-python.ts";

const object = (value: unknown): value is Record<string, unknown> => typeof value === "object" && value !== null && !Array.isArray(value);

/** The trusted preparer records these emitted bytes in its existing immutable file/driver inventory before execution. */
export function privateBunCopyLauncher(
  facts: PrivateBunCopyProfileFacts & PrivateBunCopyPythonSeal
): Readonly<{ source: string; environment: Readonly<Record<string, string>> }> {
  privateBunCopyProfile(facts);
  const binding = PrivateBunCopyPython.fromLauncher(facts);
  const canonical = PrivateBunCopyPython.command(binding);
  const bytes = readFileSync(facts.python_signature_receipt.path);
  if (createHash("sha256").update(bytes).digest("hex") !== facts.python_signature_receipt.sha256) throw new Error("Prepared Python signature receipt differs");
  const receipt: unknown = JSON.parse(bytes.toString("utf8"));
  const matches = (value: unknown, path: string): boolean => object(value) && value.path === path && value.sha256 === facts.expected_python_roles[path];
  const terminal = join(dirname(dirname(canonical)), "Resources/Python.app/Contents/MacOS/Python");
  if (!object(receipt) || !matches(receipt.canonical_executable, canonical) || !matches(receipt.terminal_interpreter, terminal))
    throw new Error("Prepared Python receipt does not bind the actual interpreter tuple");
  if (!Array.isArray(receipt.files) || !receipt.files.some((value: unknown) => matches(value, "/usr/bin/python3")))
    throw new Error("Prepared Python receipt does not bind the original system role");
  const sourceRoot = realpathSync(facts.read_roots[0]);
  const seal: PrivateBunCopyPythonSeal = {
    source_sha: facts.source_sha,
    source_tree: facts.source_tree,
    stage_manifest_sha256: facts.stage_manifest_sha256,
    expected_python_roles: { ...facts.expected_python_roles },
    python_signature_receipt: { ...facts.python_signature_receipt },
  };
  const source = [
    `import { PrivateBunCopyPython } from ${JSON.stringify(join(sourceRoot, "tools/cloudflare/private-bun-copy-python.ts"))};`,
    `const binding = PrivateBunCopyPython.fromLauncher(${JSON.stringify(seal)});`,
    `const { runPrivateBunCopyOperator } = await import(${JSON.stringify(join(sourceRoot, "tools/cloudflare/private-bun-copy.ts"))});`,
    "await runPrivateBunCopyOperator(binding);",
    "",
  ].join("\n");
  // SQLite reads this existing setting from the OS environment, before its first database open.
  return {
    source,
    environment: Object.freeze({ PATH: "/usr/bin:/bin", BUN_RUNTIME_TRANSPILER_CACHE_PATH: "0", SQLITE_TMPDIR: facts.run }),
  };
}
