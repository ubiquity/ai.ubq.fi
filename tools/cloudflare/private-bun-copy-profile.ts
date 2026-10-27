/** Offline trusted-manifest composition. Private operator stdin never supplies these capabilities. */
import { createHash } from "node:crypto";
import { lstatSync, readFileSync, realpathSync } from "node:fs";
import { basename, dirname, isAbsolute, join, resolve } from "node:path";

/** Existing immutable operator contract fields; verification/custody remains with its preparer. */
export type PrivateBunCopyProfileFacts = Readonly<{
  bun: string;
  source: string;
  run: string;
  read_roots: readonly string[];
  read_literals: readonly string[];
  private_metadata_traversal_literals: readonly string[];
  expected_python_roles: Readonly<Record<string, string>>;
  files?: readonly Readonly<{ path: string; source?: string; bytes: number; sha256: string }>[];
}>;

const refuse = (): never => {
  throw new Error("Private Bun copy profile facts are missing, malformed or stale");
};
const pathValue = (path: string): string => {
  if (typeof path !== "string" || !isAbsolute(path) || resolve(path) !== path || /[\0\r\n]/.test(path)) refuse();
  return path;
};
const physical = (path: string): string => {
  pathValue(path);
  try {
    return realpathSync(path);
  } catch (error) {
    if (!(error instanceof Error && "code" in error && error.code === "ENOENT")) throw error;
    return join(realpathSync(dirname(path)), basename(path));
  }
};
const inside = (path: string, root: string): boolean => path === root || path.startsWith(root + "/");
const selectors = (paths: readonly string[], kind: "literal" | "subpath"): string =>
  [...new Set(paths)].map((path) => "(" + kind + " " + JSON.stringify(path) + ")").join(" ");

function pythonRoles(expected: Readonly<Record<string, string>>): string[] {
  const roles = Object.keys(expected);
  if (roles.length !== 3 || !Object.hasOwn(expected, "/usr/bin/python3")) refuse();
  for (const path of roles) {
    if (!/^[a-f0-9]{64}$/.test(expected[path]) || physical(path) !== path) refuse();
    const before = lstatSync(path);
    const linksMatch = path === "/usr/bin/python3" ? before.nlink > 0 : before.nlink === 1;
    if (!before.isFile() || !linksMatch || before.uid !== 0 || (before.mode & 0o777) !== 0o755) refuse();
    if (createHash("sha256").update(readFileSync(path)).digest("hex") !== expected[path]) refuse();
    const after = lstatSync(path);
    if (before.dev !== after.dev || before.ino !== after.ino || before.size !== after.size || before.mtimeMs !== after.mtimeMs || before.nlink !== after.nlink)
      refuse();
  }
  const canonical = roles.find((path) => /\/Library\/Frameworks\/Python3\.framework\/Versions\/[^/]+\/bin\/python[^/]+$/.test(path)) ?? refuse();
  if (!roles.includes(join(dirname(dirname(canonical)), "Resources/Python.app/Contents/MacOS/Python"))) refuse();
  return roles;
}

function icuLiterals(facts: PrivateBunCopyProfileFacts, literals: readonly string[], metadata: readonly string[]): string[] {
  const selected = literals.filter((path) => /^\/usr\/share\/icu\/icudt\d+l\.dat$/.test(path));
  if (selected.length > 1) refuse();
  for (const path of selected) {
    const version = process.versions.icu?.split(".")[0] ?? refuse();
    if (!/^\d+$/.test(version)) refuse();
    if (path !== join("/usr/share/icu", "icudt" + version + "l.dat")) refuse();
    const pin = facts.files?.find((file) => (file.source ?? file.path) === path) ?? refuse();
    if (!/^[a-f0-9]{64}$/.test(pin.sha256) || !metadata.includes(path)) refuse();
    const before = lstatSync(path);
    if (!before.isFile() || before.uid !== 0 || (before.mode & 0o222) !== 0 || before.size !== pin.bytes) refuse();
    if (createHash("sha256").update(readFileSync(path)).digest("hex") !== pin.sha256) refuse();
    const after = lstatSync(path);
    if (before.dev !== after.dev || before.ino !== after.ino || before.size !== after.size || before.mtimeMs !== after.mtimeMs) refuse();
  }
  return selected;
}

/** Install this exact first OS boundary before executing Bun; never tighten an already running product. */
export function privateBunCopyProfile(facts: PrivateBunCopyProfileFacts): string {
  if (process.platform !== "darwin") refuse();
  const bun = physical(facts.bun);
  const source = physical(facts.source);
  const run = physical(facts.run);
  if (bun !== facts.bun || source !== facts.source || run !== facts.run || run === "/" || inside(source, run) || inside(bun, run)) refuse();
  const parent = lstatSync(dirname(run));
  const sourceFile = lstatSync(source);
  if (!parent.isDirectory() || parent.uid !== process.getuid?.() || (parent.mode & 0o077) !== 0) refuse();
  if (!sourceFile.isFile() || sourceFile.nlink !== 1 || sourceFile.uid !== process.getuid?.()) refuse();
  try {
    const output = lstatSync(run);
    if (!output.isDirectory() || output.uid !== process.getuid?.() || (output.mode & 0o077) !== 0) refuse();
  } catch (error) {
    if (!(error instanceof Error && "code" in error && error.code === "ENOENT")) throw error;
  }
  const roles = pythonRoles(facts.expected_python_roles);
  const roots = facts.read_roots.map(physical);
  const literals = facts.read_literals.map(physical);
  const metadata = facts.private_metadata_traversal_literals.map(pathValue);
  if (roots.length === 0 || roots.includes("/") || literals.includes("/") || !metadata.includes("/")) refuse();
  if (!roots.includes("/usr/lib") || !roots.includes("/System/Library")) refuse();
  if ([bun, source, source + "-wal", source + "-shm", ...roles].some((path) => !literals.includes(path))) refuse();
  if (roots.some((root) => inside(root, run) || inside(run, root))) refuse();
  const icu = icuLiterals(facts, literals, metadata);
  const ordinaryLiterals = literals.filter((path) => !icu.includes(path));
  return [
    "(version 1)",
    "(allow default)",
    "(deny file-read*)",
    "(allow file-read-metadata " + selectors(metadata, "literal") + ")",
    // Kernel-proven native bootstrap requirement. This reads root directory names, never descendants.
    '(allow file-read-data (literal "/"))',
    // The already-validated private output anchor needs canonicalization; sibling file bytes stay denied.
    "(allow file-read-data " + selectors([dirname(run)], "literal") + ")",
    ...(icu.length ? ["(allow file-read-data " + selectors(icu, "literal") + ")"] : []),
    "(allow file-read* " + selectors(roots, "subpath") + " " + selectors(ordinaryLiterals, "literal") + " " + selectors([run], "subpath") + ")",
    "(deny file-write*)",
    "(allow file-write* " + selectors([run], "subpath") + ")",
    "(deny network*)",
    "(deny process-exec)",
    "(allow process-exec " + selectors([bun, ...roles], "literal") + ")",
  ].join("\n");
}
