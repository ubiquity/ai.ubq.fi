/** Constructor-held execution binding from the independently verified immutable operator launcher. */
import { createHash } from "node:crypto";
import { lstatSync, readFileSync, realpathSync } from "node:fs";
import { dirname, isAbsolute, join } from "node:path";

export type PrivateBunCopyPythonSeal = Readonly<{
  source_sha: string;
  source_tree: string;
  stage_manifest_sha256: string;
  expected_python_roles: Readonly<Record<string, string>>;
  python_signature_receipt: Readonly<{ path: string; sha256: string }>;
}>;

const refused = (): never => {
  throw new Error("Prepared private Python binding is missing, stale or invalid");
};

function identities(expected: Readonly<Record<string, string>>): Map<string, string> {
  const current = new Map<string, string>();
  for (const [path, hash] of Object.entries(expected)) {
    if (!isAbsolute(path) || realpathSync(path) !== path || !/^[a-f0-9]{64}$/.test(hash)) refused();
    const before = lstatSync(path, { bigint: true });
    // Apple's fixed system shim is a hard-linked tool multiplexer; the actual interpreter roles remain single-link.
    const linksMatch = path === "/usr/bin/python3" ? before.nlink > 0n : before.nlink === 1n;
    if (!before.isFile() || before.uid !== 0n || !linksMatch || (before.mode & 0o777n) !== 0o755n) refused();
    if (createHash("sha256").update(readFileSync(path)).digest("hex") !== hash) refused();
    const after = lstatSync(path, { bigint: true });
    const fact = (value: typeof before): string => [value.dev, value.ino, value.size, value.mtimeNs, value.mode, value.uid, value.nlink].join(":");
    if (fact(before) !== fact(after)) refused();
    current.set(path, fact(after));
  }
  return current;
}

export class PrivateBunCopyPython {
  readonly #canonical: string;
  readonly #expected: Readonly<Record<string, string>>;
  readonly #identities: Map<string, string>;

  private constructor(canonical: string, expected: Readonly<Record<string, string>>) {
    this.#canonical = canonical;
    this.#expected = Object.freeze({ ...expected });
    this.#identities = identities(this.#expected);
  }

  /** Called only by source literals in the independently hash-bound prepared launcher, never operator stdin. */
  static fromLauncher(seal: PrivateBunCopyPythonSeal): PrivateBunCopyPython {
    if (!/^[a-f0-9]{40}$/.test(seal.source_sha) || !/^[a-f0-9]{40}$/.test(seal.source_tree) || !/^[a-f0-9]{64}$/.test(seal.stage_manifest_sha256)) refused();
    if (!isAbsolute(seal.python_signature_receipt.path) || !/^[a-f0-9]{64}$/.test(seal.python_signature_receipt.sha256)) refused();
    const paths = Object.keys(seal.expected_python_roles);
    if (paths.length !== 3 || !paths.includes("/usr/bin/python3")) refused();
    const canonical = paths.find((path) => /\/Library\/Frameworks\/Python3\.framework\/Versions\/[^/]+\/bin\/python[^/]+$/.test(path)) ?? refused();
    if (!paths.includes(join(dirname(dirname(canonical)), "Resources/Python.app/Contents/MacOS/Python"))) refused();
    return new PrivateBunCopyPython(canonical, seal.expected_python_roles);
  }

  static command(binding: PrivateBunCopyPython | undefined): string {
    if (!binding) return refused();
    const current = identities(binding.#expected);
    for (const [path, identity] of binding.#identities) if (current.get(path) !== identity) refused();
    return binding.#canonical;
  }
}
