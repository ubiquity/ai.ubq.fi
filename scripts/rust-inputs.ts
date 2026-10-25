import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { existsSync, lstatSync, readFileSync, readdirSync } from "node:fs";
import { join, posix, resolve } from "node:path";

type Table = Record<string, unknown>;
export const rustTable = (value: unknown): Table => {
  if (typeof value !== "object" || value === null || Array.isArray(value)) throw new Error("Expected a Rust input table");
  return value as Table;
};
const optionalTable = (value: unknown): Table => (value === undefined ? {} : rustTable(value));
export const rustCanonical = (value: unknown): string =>
  JSON.stringify(value, (_key, item: unknown) =>
    typeof item === "object" && item !== null && !Array.isArray(item)
      ? Object.fromEntries(Object.entries(item).sort(([left], [right]) => left.localeCompare(right)))
      : item
  );
export const rustHash = (value: string | Uint8Array): string => createHash("sha256").update(value).digest("hex");
export const rustUnique = (values: Iterable<string>): string[] => [...new Set(values)].sort((left, right) => left.localeCompare(right));
export const rustPath = (path: string): string => {
  if (!path || path.includes("\0") || path.includes("\\") || path.startsWith("/") || path.split("/").some((part) => !part || part === "." || part === ".."))
    throw new Error(`Unsafe Rust input path: ${path}`);
  return path;
};
const stringArray = (value: unknown): string[] => {
  if (!Array.isArray(value) || !value.every((item) => typeof item === "string")) throw new Error("Expected a Rust string array");
  return value;
};
export const RUST_NATIVE_PACKAGES = [
  "gateway-native",
  "gateway-native-supervisor-read",
  "gateway-quality",
  "gateway-quality-syntax",
  "gateway-claude-runtime-test-support",
  "gateway-migration-test-support",
] as const;
export const isRustDocumentation = (path: string): boolean =>
  /\.(?:md|mdx|markdown)$/i.test(path) && !path.startsWith("static/") && !/^rust\/crates\/[^/]+\/(?:tests|fixtures|examples|src)\//.test(path);
export const isRustGateInput = (path: string): boolean =>
  (path.startsWith("rust/") && !isRustDocumentation(path)) ||
  /^scripts\/rust-(?:verify(?:-plan)?|inputs|scope|target-owner|quality(?:-[a-z-]+)?)(?:\.test)?\.ts$/.test(path) ||
  /^(?:favicon(?:-32)?\.png|entrypoint\.mjs|cloudflare\.config\.ts|wrangler\.config\.ts|package\.json|bun\.lock|ops\/rust-worker-build\.sh)$/.test(path) ||
  path.startsWith("static/");

export type RustEdge = Readonly<{ name: string; kind: "dependencies" | "dev-dependencies" | "build-dependencies"; portable: boolean }>;
export type RustPackage = Readonly<{ name: string; version: string; directory: string; manifest: Table; edges: readonly RustEdge[]; hasTests: boolean }>;
export type RustSnapshot = Readonly<{
  identity: string | null;
  root: Table;
  lock: Table;
  packages: readonly RustPackage[];
  hashes: Readonly<Record<string, string>>;
  compiledOwners: Readonly<Record<string, readonly string[]>>;
  uncertainInputs: readonly string[];
}>;
export type RustInputReader = Readonly<{ identity: string | null; files: readonly string[]; read: (path: string) => Uint8Array }>;
export type RustChangeInputs = Readonly<{ paths: readonly string[]; base: RustSnapshot | null; head: RustSnapshot; immutable: boolean }>;

type QuotedInput = Readonly<{ end: number; value: string | null; uncertain: boolean }>;
const rawRustString = (source: string, offset: number): QuotedInput | null => {
  const raw = /^(?:br|cr|r)(#*)"/.exec(source.slice(offset));
  if (!raw) return null;
  const start = offset + raw[0].length;
  const close = source.indexOf(`"${raw[1]}`, start);
  if (close < 0) return { end: source.length, value: null, uncertain: true };
  return { end: close + raw[1].length + 1, value: source.slice(start, close), uncertain: false };
};
const rustStringStart = (source: string, offset: number): number => {
  if (source[offset] === '"') return offset;
  if (/^[bc]"/.test(source.slice(offset))) return offset + 1;
  return -1;
};
const rustStringEnd = (source: string, start: number): number => {
  for (let end = start + 1; end < source.length; end++) {
    if (source[end] === "\\") end++;
    else if (source[end] === '"') return end;
  }
  return source.length;
};
const quotedRustString = (source: string, offset: number): QuotedInput | null => {
  const raw = rawRustString(source, offset);
  if (raw) return raw;
  const start = rustStringStart(source, offset);
  if (start < 0) return null;
  const end = rustStringEnd(source, start);
  if (end === source.length) return { end, value: null, uncertain: true };
  let value: string | null = null;
  try {
    value = JSON.parse(source.slice(start, end + 1)) as string;
  } catch {
    // Unsupported Rust escapes remain uncertain at include sites.
  }
  return { end: end + 1, value, uncertain: false };
};
const rustCommentEnd = (source: string, start: number): Readonly<{ end: number; uncertain: boolean }> | null => {
  if (source.startsWith("//", start)) {
    const end = source.indexOf("\n", start);
    return { end: end < 0 ? source.length : end + 1, uncertain: false };
  }
  if (!source.startsWith("/*", start)) return null;
  let depth = 1;
  let end = start + 2;
  while (end < source.length && depth) {
    if (source.startsWith("/*", end)) {
      depth++;
      end += 2;
    } else if (source.startsWith("*/", end)) {
      depth--;
      end += 2;
    } else end++;
  }
  return { end, uncertain: depth > 0 };
};
const rustCharacterEnd = (source: string, offset: number): number | null => {
  const character = /^'(?:\\(?:u\{[a-fA-F0-9_]+\}|x[a-fA-F0-9]{2}|.)|[^'\\\r\n])'/.exec(source.slice(offset));
  return character ? offset + character[0].length : null;
};
type RustToken = Readonly<{ end: number; identifier: string | null; uncertain: boolean }>;
const rustInputToken = (source: string, offset: number): RustToken => {
  const comment = rustCommentEnd(source, offset);
  if (comment) return { ...comment, identifier: null };
  const character = rustCharacterEnd(source, offset);
  if (character !== null) return { end: character, identifier: null, uncertain: false };
  const string = quotedRustString(source, offset);
  if (string) return { end: string.end, identifier: null, uncertain: string.uncertain };
  const identifier = /^[A-Za-z_]\w*/.exec(source.slice(offset));
  if (identifier) return { end: offset + identifier[0].length, identifier: identifier[0], uncertain: false };
  return { end: offset + 1, identifier: null, uncertain: false };
};
const rustIncludeCandidate = (source: string, token: RustToken): QuotedInput | null => {
  if (token.identifier !== "include_str" && token.identifier !== "include_bytes") return null;
  const invocation = /^\s*!\s*\(\s*/.exec(source.slice(token.end));
  if (!invocation) return { end: token.end, value: null, uncertain: true };
  return quotedRustString(source, token.end + invocation[0].length) ?? { end: token.end, value: null, uncertain: true };
};
// This conservative candidate scanner is not a Rust syntax parser. Unsupported
// forms expand verification; they never establish that an input is absent.
export const literalRustIncludes = (source: string): Readonly<{ paths: readonly string[]; uncertain: boolean }> => {
  const paths: string[] = [];
  let uncertain = false;
  for (let offset = 0; offset < source.length;) {
    const token = rustInputToken(source, offset);
    const include = rustIncludeCandidate(source, token);
    uncertain = uncertain || token.uncertain;
    offset = token.end;
    if (!include) continue;
    if (include.value) paths.push(include.value);
    else uncertain = true;
    offset = include.end;
  }
  return { paths, uncertain };
};

const nativeTarget = (selector: string): boolean => /^cfg\(\s*all\(\s*unix\s*,\s*not\(\s*target_arch\s*=\s*"wasm32"\s*\)\s*\)\s*\)$/.test(selector);
const localPath = (base: string, path: string): string => {
  if (path.includes("\\") || path.includes("\0") || path.startsWith("/")) throw new Error("Rust dependency path escapes workspace");
  const result = posix.normalize(posix.join(base, path));
  if (!result.startsWith("rust/")) throw new Error("Rust dependency path escapes workspace");
  return rustPath(result);
};
type EdgeContext = Readonly<{ workspace: Table; directory: string; members: ReadonlyMap<string, string> }>;
const inheritedDependency = (raw: unknown, alias: string, context: EdgeContext): Readonly<{ dependency: Table; base: string }> | null => {
  if (typeof raw === "string") return null;
  const dependency = rustTable(raw);
  if (dependency.workspace !== true) return { dependency, base: context.directory };
  const inherited = optionalTable(context.workspace.dependencies)[alias];
  if (inherited === undefined) throw new Error(`Missing inherited Rust dependency: ${alias}`);
  const value = typeof inherited === "string" ? { version: inherited } : rustTable(inherited);
  return { dependency: { ...value, ...dependency }, base: "rust" };
};
const localDependency = (raw: unknown, alias: string, context: EdgeContext): string | null => {
  const resolved = inheritedDependency(raw, alias, context);
  if (resolved?.dependency.path === undefined) return null;
  const { dependency, base } = resolved;
  if (typeof dependency.path !== "string") throw new Error("Malformed Rust dependency path");
  const path = localPath(base, dependency.path);
  const name = context.members.get(path);
  // An unchanged vendored patch is external to the first-party graph.
  if (!name && path.startsWith("rust/vendor/")) return null;
  if (!name || (dependency.package !== undefined && dependency.package !== name) || (dependency.package === undefined && alias !== name))
    throw new Error(`Unregistered or mismatched local Rust dependency: ${alias}`);
  return name;
};
const configurationEdges = (configuration: Table, selector: string, context: EdgeContext): RustEdge[] => {
  const edges: RustEdge[] = [];
  for (const kind of ["dependencies", "dev-dependencies", "build-dependencies"] as const) {
    for (const [alias, raw] of Object.entries(optionalTable(configuration[kind]))) {
      const name = localDependency(raw, alias, context);
      if (name) edges.push({ name, kind, portable: kind !== "dependencies" || !nativeTarget(selector) });
    }
  }
  return edges;
};
const manifestEdges = (manifest: Table, workspace: Table, directory: string, members: ReadonlyMap<string, string>): RustEdge[] => {
  const context = { workspace, directory, members };
  const targets = Object.entries(optionalTable(manifest.target)).flatMap(([selector, value]) => configurationEdges(rustTable(value), selector, context));
  return [...configurationEdges(manifest, "", context), ...targets];
};
type SnapshotAccess = Readonly<{ files: ReadonlySet<string>; hashes: Record<string, string>; text: (path: string) => string; parse: (path: string) => Table }>;
const snapshotAccess = (reader: RustInputReader): SnapshotAccess => {
  const files = new Set(reader.files.map(rustPath));
  const hashes: Record<string, string> = {};
  const text = (path: string): string => {
    if (!files.has(path)) throw new Error(`Required Rust input is missing: ${path}`);
    const bytes = reader.read(path);
    hashes[path] = rustHash(bytes);
    return new TextDecoder().decode(bytes);
  };
  return { files, hashes, text, parse: (path) => rustTable(Bun.TOML.parse(text(path))) };
};
const packageVersion = (data: Table, workspace: Table): string => {
  if (data.version === undefined) return "";
  if (typeof data.version === "string") return data.version;
  const version = optionalTable(workspace.package).version;
  if (version !== undefined && typeof version !== "string") throw new Error("Invalid inherited Rust package version");
  return version ?? "";
};
type MemberManifest = Readonly<{ name: string; version: string; directory: string; manifest: Table }>;
const memberManifest = (member: string, workspace: Table, access: SnapshotAccess): MemberManifest => {
  const directory = localPath("rust", member);
  if (directory !== `rust/${member}`) throw new Error("Rust workspace member escapes rust/ or is not canonical");
  const manifest = access.parse(`${directory}/Cargo.toml`);
  const data = rustTable(manifest.package);
  if (typeof data.name !== "string" || !/^[a-zA-Z0-9_-]+$/.test(data.name)) throw new Error("Rust workspace member has no safe package name");
  return { name: data.name, version: packageVersion(data, workspace), directory, manifest };
};
const snapshotMembers = (workspace: Table, access: SnapshotAccess): readonly MemberManifest[] => {
  const paths = stringArray(workspace.members);
  if (!paths.length || paths.some((path) => /[*?[\]]/.test(path))) throw new Error("Rust workspace needs explicit, nonempty member paths");
  const manifests = paths.map((member) => memberManifest(member, workspace, access));
  const directories = new Set(manifests.map((item) => item.directory));
  const names = new Set(manifests.map((item) => item.name));
  if (directories.size !== manifests.length || names.size !== manifests.length) throw new Error("Rust workspace contains duplicate package names or paths");
  return manifests;
};
const packageHasTests = (directory: string, access: SnapshotAccess): boolean =>
  [...access.files].some((path) => path.startsWith(`${directory}/`) && path.endsWith(".rs") && /#\s*\[\s*(?:test|tokio::test)\s*\]/.test(access.text(path)));
type CompiledInputs = { compiledOwners: Record<string, string[]>; uncertainInputs: string[] };
const bindCompiledInput = (path: string, included: string, owner: string, access: SnapshotAccess, inputs: CompiledInputs): void => {
  const input = rustPath(posix.normalize(posix.join(posix.dirname(path), included)));
  if (!access.files.has(input)) {
    inputs.uncertainInputs.push(path);
    return;
  }
  access.text(input);
  const owners = inputs.compiledOwners[input] ?? [];
  owners.push(owner);
  inputs.compiledOwners[input] = owners;
};
const compiledFileInputs = (path: string, owner: string, access: SnapshotAccess, inputs: CompiledInputs): void => {
  const candidates = literalRustIncludes(access.text(path));
  const audited =
    path === "rust/crates/gateway-routing-runtime-test-support/src/source_catalog.rs" &&
    access.hashes[path] === "269ca47aaf3dcf66e4f5c1ab6fbd0003bdab594bf34e998548c93c543fb9630d";
  if (candidates.uncertain && !audited) inputs.uncertainInputs.push(path);
  const includes = [...candidates.paths];
  if (audited) includes.push("../../gateway-schema/tests/fixtures/source-catalog-sort-v1.json");
  for (const included of includes) bindCompiledInput(path, included, owner, access, inputs);
};
const compiledInputOwners = (packages: readonly RustPackage[], access: SnapshotAccess): CompiledInputs => {
  const inputs: CompiledInputs = { compiledOwners: {}, uncertainInputs: [] };
  for (const path of access.files) {
    if (!path.endsWith(".rs")) continue;
    const owner = packages.find((item) => path.startsWith(`${item.directory}/src/`) || path === `${item.directory}/build.rs`);
    if (owner) compiledFileInputs(path, owner.name, access, inputs);
  }
  return inputs;
};
const authoredRustInput = (path: string): boolean =>
  (path.startsWith("rust/") && !path.startsWith("rust/vendor/")) || isRustGateInput(path) || /^(?:package\.json|bun\.lock)$/.test(path);
const hashAuthoredInputs = (reader: RustInputReader, access: SnapshotAccess): void => {
  for (const path of access.files) if (authoredRustInput(path) && !(path in access.hashes)) access.hashes[path] = rustHash(reader.read(path));
};

export const readRustSnapshot = (reader: RustInputReader): RustSnapshot => {
  const access = snapshotAccess(reader);
  const root = access.parse("rust/Cargo.toml");
  if (!access.files.has("rust/Cargo.lock") || access.text("rust/Cargo.lock").trim() === "")
    throw new Error("Committed rust/Cargo.lock is required; verification never generates it");
  const lock = access.parse("rust/Cargo.lock");
  const workspace = rustTable(root.workspace);
  const manifests = snapshotMembers(workspace, access);
  const members = new Map(manifests.map((item) => [item.directory, item.name]));
  const packages = manifests.map((item) => ({
    ...item,
    edges: manifestEdges(item.manifest, workspace, item.directory, members),
    hasTests: packageHasTests(item.directory, access),
  }));
  const inputs = compiledInputOwners(packages, access);
  hashAuthoredInputs(reader, access);
  return {
    identity: reader.identity,
    root,
    lock,
    packages,
    hashes: access.hashes,
    compiledOwners: inputs.compiledOwners,
    uncertainInputs: rustUnique(inputs.uncertainInputs),
  };
};

const git = (root: string, argv: readonly string[]): string | null => {
  const result = spawnSync("/usr/bin/git", argv, { cwd: root, encoding: "utf8", maxBuffer: 64 * 1024 * 1024 });
  return result.status === 0 ? result.stdout : null;
};
type RustGitEntry = Readonly<{ mode: string; blob: string; path: string }>;
const rustGitTreeEntry = (entry: string): readonly RustGitEntry[] => {
  const match = /^(\d+) (blob|commit) ([a-f0-9]{40})\t([\s\S]+)$/.exec(entry);
  if (!match) throw new Error("Unsupported Rust Git tree entry");
  const path = rustPath(match[4]);
  if (match[2] === "commit") {
    if (path.startsWith("rust/") || isRustGateInput(path)) throw new Error(`Unsupported linked Rust Git input: ${path}`);
    return [];
  }
  return [{ mode: match[1], blob: match[3], path }];
};
const gitReader = (root: string, sha: string): RustInputReader => {
  const listing = git(root, ["ls-tree", "-r", "-z", sha]);
  if (listing === null) throw new Error("Cannot read immutable Rust Git tree");
  const entries = listing.split("\0").filter(Boolean).flatMap(rustGitTreeEntry);
  const byPath = new Map(entries.map((entry) => [entry.path, entry]));
  const cached = new Map<string, Uint8Array>();
  const blobs = rustUnique(
    entries
      .filter((entry) => (entry.path.startsWith("rust/") && !entry.path.startsWith("rust/vendor/")) || isRustGateInput(entry.path))
      .map((entry) => entry.blob)
  );
  if (blobs.length) {
    const batch = spawnSync("/usr/bin/git", ["cat-file", "--batch"], { cwd: root, input: blobs.join("\n") + "\n", maxBuffer: 256 * 1024 * 1024 });
    if (batch.status !== 0) throw new Error("Cannot materialize immutable Rust input blobs");
    let offset = 0;
    for (const blob of blobs) {
      const newline = batch.stdout.indexOf(10, offset);
      const header = batch.stdout.subarray(offset, newline).toString("utf8");
      const match = /^([a-f0-9]{40}) blob (\d+)$/.exec(header);
      if (newline < offset || match?.[1] !== blob) throw new Error("Malformed immutable Rust blob stream");
      const end = newline + 1 + Number(match[2]);
      if (!Number.isSafeInteger(end) || end >= batch.stdout.length || batch.stdout[end] !== 10) throw new Error("Incomplete immutable Rust blob stream");
      cached.set(blob, batch.stdout.subarray(newline + 1, end));
      offset = end + 1;
    }
    if (offset !== batch.stdout.length) throw new Error("Unexpected immutable Rust blob stream output");
  }
  return {
    identity: sha,
    files: entries.map((entry) => entry.path),
    read: (path) => {
      const entry = byPath.get(rustPath(path));
      if (!entry || !["100644", "100755"].includes(entry.mode)) throw new Error(`Missing or linked Rust input: ${path}`);
      const previous = cached.get(entry.blob);
      if (previous) return previous;
      const result = spawnSync("/usr/bin/git", ["cat-file", "blob", entry.blob], { cwd: root, maxBuffer: 64 * 1024 * 1024 });
      if (result.status !== 0) throw new Error(`Cannot read Rust blob: ${path}`);
      const bytes = new Uint8Array(result.stdout);
      cached.set(entry.blob, bytes);
      return bytes;
    },
  };
};
export const workingRustReader = (root: string): RustInputReader => {
  const files: string[] = [];
  const walk = (path: string): void => {
    const absolute = join(root, path);
    const stat = lstatSync(absolute);
    if (stat.isSymbolicLink()) throw new Error(`Linked Rust input: ${path}`);
    if (stat.isDirectory())
      for (const child of readdirSync(absolute)) {
        if (!child.startsWith(".") && child !== "target" && child !== "node_modules") walk(`${path}/${child}`);
      }
    else if (stat.isFile()) files.push(rustPath(path));
  };
  if (!existsSync(join(root, "rust/Cargo.lock"))) throw new Error("Committed rust/Cargo.lock is required; verification never generates it");
  walk("rust");
  for (const path of ["favicon.png", "favicon-32.png", "entrypoint.mjs", "cloudflare.config.ts", "wrangler.config.ts", "package.json", "bun.lock", "static"])
    if (existsSync(join(root, path))) walk(path);
  return {
    identity: null,
    files,
    read: (path) => {
      rustPath(path);
      let prefix = root;
      for (const part of path.split("/")) {
        prefix = join(prefix, part);
        if (lstatSync(prefix).isSymbolicLink()) throw new Error(`Linked Rust input: ${path}`);
      }
      const before = lstatSync(prefix);
      const bytes = readFileSync(prefix);
      const after = lstatSync(prefix);
      if (!before.isFile() || before.ino !== after.ino || before.size !== after.size || before.mtimeMs !== after.mtimeMs)
        throw new Error(`Rust input changed during read: ${path}`);
      return bytes;
    },
  };
};
export const loadRustChangeInputs = (root: string, paths: readonly string[], environment: NodeJS.ProcessEnv = process.env): RustChangeInputs => {
  paths.forEach(rustPath);
  const head = git(root, ["rev-parse", "--verify", "HEAD"])?.trim();
  const top = git(root, ["rev-parse", "--show-toplevel"])?.trim();
  if (!head || top !== resolve(root)) return { paths: rustUnique(paths), base: null, head: readRustSnapshot(workingRustReader(root)), immutable: false };
  const candidates = [
    environment.TEST_SCOPE_BASE,
    environment.GITHUB_BASE_REF ? `origin/${environment.GITHUB_BASE_REF}` : undefined,
    environment.GITHUB_EVENT_BEFORE,
    "origin/development",
  ];
  let base: string | null = null;
  for (const ref of candidates) {
    if (!ref || /^0+$/.test(ref)) continue;
    const revision = git(root, ["rev-parse", "--verify", "--end-of-options", `${ref}^{commit}`])?.trim();
    if (revision) {
      base = git(root, ["merge-base", revision, head])?.trim() ?? null;
      break;
    }
  }
  const dirty = git(root, ["diff", "--name-only", "-z", "HEAD"])?.split("\0").filter(Boolean) ?? [];
  const untracked = git(root, ["ls-files", "--others", "--exclude-standard", "-z"])?.split("\0").filter(Boolean) ?? [];
  const completePaths = rustUnique([
    ...paths,
    ...(base ? (git(root, ["diff", "--no-renames", "--name-only", "-z", base, head])?.split("\0").filter(Boolean) ?? []) : []),
    ...dirty,
    ...untracked.filter((path) => isRustGateInput(path)),
  ]);
  completePaths.forEach(rustPath);
  const immutable = ![...dirty, ...untracked].some(isRustGateInput);
  return {
    paths: completePaths,
    base: base && immutable ? readRustSnapshot(gitReader(root, base)) : null,
    head: readRustSnapshot(immutable ? gitReader(root, head) : workingRustReader(root)),
    immutable,
  };
};
