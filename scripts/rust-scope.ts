import {
  isRustDocumentation,
  isRustGateInput,
  loadRustChangeInputs,
  RUST_NATIVE_PACKAGES,
  rustCanonical,
  rustHash,
  rustTable,
  rustUnique,
  type RustChangeInputs,
  type RustPackage,
  type RustSnapshot,
} from "./rust-inputs.ts";

type Table = Record<string, unknown>;
export type RustScope = Readonly<{
  version: 1;
  mode: "selected" | "full" | "not-required";
  paths: readonly string[];
  inputPaths: readonly string[];
  base: string | null;
  head: string | null;
  inputsHash: string;
  reasons: readonly string[];
  policyActions: readonly string[];
  nativePackages: readonly string[];
  nativeTestPackages: readonly string[];
  wasmPackages: readonly string[];
  fixtureBuilds: readonly string[];
  shippingWorkerBuild: boolean;
}>;
const table = (value: unknown): Table => (value === undefined ? {} : rustTable(value));
const same = (left: unknown, right: unknown): boolean => rustCanonical(left) === rustCanonical(right);
const local = (value: unknown): boolean => typeof value === "object" && value !== null && "path" in value;
const plainLocal = (value: unknown): boolean => local(value) && Object.keys(rustTable(value)).every((key) => ["path", "package"].includes(key));
const withoutLocal = (value: unknown): Table => Object.fromEntries(Object.entries(table(value)).filter(([_name, dependency]) => !local(dependency)));
const rootSemantics = (snapshot: RustSnapshot): Table => {
  const workspace = rustTable(snapshot.root.workspace);
  const rest = Object.fromEntries(Object.entries(workspace).filter(([key]) => key !== "members" && key !== "dependencies"));
  return { ...snapshot.root, workspace: { ...rest, dependencies: withoutLocal(workspace.dependencies) } };
};
const localRootChanges = (base: RustSnapshot, head: RustSnapshot): boolean => {
  const oldDependencies = table(rustTable(base.root.workspace).dependencies);
  const newDependencies = table(rustTable(head.root.workspace).dependencies);
  return rustUnique([...Object.keys(oldDependencies), ...Object.keys(newDependencies)]).every(
    (name) =>
      same(oldDependencies[name], newDependencies[name]) ||
      [oldDependencies[name], newDependencies[name]].every((value) => value === undefined || plainLocal(value))
  );
};
const nonLocalManifestEdges = (entries: Table, workspace: Table): Table =>
  Object.fromEntries(
    Object.entries(entries).filter(([alias, value]) => {
      const dependency = table(typeof value === "string" ? undefined : value);
      const inheritedLocal = dependency.workspace === true && Object.keys(dependency).length === 1 && plainLocal(table(workspace.dependencies)[alias]);
      return !(plainLocal(value) || inheritedLocal);
    })
  );
const manifestWithoutLocalEdges = (manifest: Table, workspace: Table): Table => {
  const kinds = ["dependencies", "dev-dependencies", "build-dependencies"];
  const result = Object.fromEntries(Object.entries(manifest).filter(([key]) => !kinds.includes(key)));
  for (const kind of kinds) {
    const entries = nonLocalManifestEdges(table(manifest[kind]), workspace);
    if (Object.keys(entries).length) result[kind] = entries;
  }
  if (manifest.target !== undefined)
    result.target = Object.fromEntries(
      Object.entries(rustTable(manifest.target)).map(([selector, value]) => [selector, manifestWithoutLocalEdges(rustTable(value), workspace)])
    );
  return result;
};
type LockIdentity = Readonly<{ name: string; version: string; id: string; shortId: string }>;
const lockIdentity = (item: Table): LockIdentity => {
  if (typeof item.name !== "string" || typeof item.version !== "string" || (item.source !== undefined && typeof item.source !== "string"))
    throw new Error("Malformed Rust lock identity");
  const shortId = `${item.name} ${item.version}`;
  const source = item.source === undefined ? "" : " (" + item.source + ")";
  return { name: item.name, version: item.version, shortId, id: shortId + source };
};
const resolveLockDependency = (dependency: unknown, identities: readonly LockIdentity[]): string => {
  if (typeof dependency !== "string") throw new Error("Malformed Rust lock dependency");
  const matches = identities.filter((item) => item.id === dependency || item.shortId === dependency || item.name === dependency);
  if (matches.length !== 1) throw new Error(`Ambiguous or missing Rust lock dependency: ${dependency}`);
  return matches[0].id;
};
type LockInventory = Readonly<{ external: readonly string[]; owned: ReadonlyMap<string, string> }>;
const validateMemberLock = (
  member: RustPackage,
  snapshot: RustSnapshot,
  packages: readonly Table[],
  owned: ReadonlyMap<string, string>,
  resolve: (dependency: unknown) => string
): void => {
  if (!owned.has(member.name)) throw new Error(`Rust lock is missing member identity: ${member.name}`);
  const entry = packages.find((item) => item.name === member.name && item.version === member.version && item.source === undefined);
  const dependencies = entry?.dependencies;
  if (member.edges.length && !Array.isArray(dependencies)) throw new Error(`Rust lock is missing member dependencies: ${member.name}`);
  const resolved = Array.isArray(dependencies) ? dependencies.map(resolve) : [];
  for (const edge of member.edges) {
    const target = snapshot.packages.find((item) => item.name === edge.name);
    if (!target || !resolved.includes(`${target.name} ${target.version}`))
      throw new Error(`Rust lock disagrees with local dependency: ${member.name} -> ${edge.name}`);
  }
};
const lockRecords = (snapshot: RustSnapshot): LockInventory => {
  if (!Array.isArray(snapshot.lock.package)) throw new Error("Rust lock has no package inventory");
  const packages = snapshot.lock.package.map(rustTable);
  const identities = packages.map(lockIdentity);
  if (new Set(identities.map((item) => item.id)).size !== identities.length) throw new Error("Duplicate Rust lock identity");
  const resolve = (dependency: unknown): string => resolveLockDependency(dependency, identities);
  const owned = new Map<string, string>();
  const external: string[] = [];
  packages.forEach((item, index) => {
    const dependencies = item.dependencies === undefined ? [] : item.dependencies;
    if (!Array.isArray(dependencies)) throw new Error("Malformed Rust lock dependency inventory");
    const record = rustCanonical({ ...item, dependencies: rustUnique(dependencies.map(resolve)) });
    const owner = snapshot.packages.find(
      (member) => member.name === item.name && member.version === item.version && item.source === undefined && item.checksum === undefined
    );
    if (owner) owned.set(owner.name, record);
    else external.push(`${identities[index].id}:${record}`);
  });
  for (const member of snapshot.packages) validateMemberLock(member, snapshot, packages, owned, resolve);
  return { external: external.toSorted((left, right) => left.localeCompare(right)), owned };
};
const reverseEdgeMatches = (edge: RustPackage["edges"][number], found: ReadonlySet<string>, tests: boolean, portable: boolean): boolean => {
  if (!tests && edge.kind === "dev-dependencies") return false;
  if (portable && !edge.portable) return false;
  return found.has(edge.name);
};
const reverse = (packages: readonly RustPackage[], seeds: ReadonlySet<string>, tests: boolean, portable = false): Set<string> => {
  const found = new Set(seeds);
  for (;;) {
    const before = found.size;
    for (const item of packages) if (item.edges.some((edge) => reverseEdgeMatches(edge, found, tests, portable))) found.add(item.name);
    if (found.size === before) return found;
  }
};
const portableRuntimeEdges = (item: RustPackage): readonly string[] =>
  item.edges.filter((edge) => edge.kind !== "dev-dependencies" && edge.portable).map((edge) => edge.name);
const workerClosure = (packages: readonly RustPackage[]): Set<string> => {
  const found = new Set(["gateway-worker"]);
  for (;;) {
    const before = found.size;
    for (const item of packages) {
      if (!found.has(item.name)) continue;
      for (const name of portableRuntimeEdges(item)) found.add(name);
    }
    if (found.size === before) return found;
  }
};
const packageManifestDelta = (old: RustPackage, current: RustPackage, base: RustSnapshot, head: RustSnapshot): "production" | "tests" | "equal" | "unknown" => {
  if (same(old.manifest, current.manifest)) return "equal";
  if (
    !same(manifestWithoutLocalEdges(old.manifest, rustTable(base.root.workspace)), manifestWithoutLocalEdges(current.manifest, rustTable(head.root.workspace)))
  )
    return "unknown";
  const kinds = rustUnique(
    [...old.edges, ...current.edges]
      .filter(
        (edge) =>
          !same(
            old.edges.filter((item) => item.kind === edge.kind),
            current.edges.filter((item) => item.kind === edge.kind)
          )
      )
      .map((edge) => edge.kind)
  );
  return kinds.length && kinds.every((kind) => kind === "dev-dependencies") ? "tests" : "production";
};
type ScopeSeeds = { production: Set<string>; tests: Set<string>; reasons: string[]; workerInput: boolean };
const knownLockFormat = (snapshot: RustSnapshot): boolean =>
  snapshot.lock.version === 4 && Object.keys(snapshot.lock).every((key) => ["version", "package"].includes(key));
const changedLocalLockMembers = (base: RustSnapshot, head: RustSnapshot, seeds: ScopeSeeds): Set<string> => {
  if (!knownLockFormat(base) || !knownLockFormat(head)) {
    seeds.reasons.push("Unsupported Cargo.lock format");
    return new Set();
  }
  const oldLock = lockRecords(base);
  const newLock = lockRecords(head);
  if (!same(oldLock.external, newLock.external)) seeds.reasons.push("External or vendor lock identities changed");
  return new Set(rustUnique([...oldLock.owned.keys(), ...newLock.owned.keys()]).filter((name) => oldLock.owned.get(name) !== newLock.owned.get(name)));
};
const simpleMemberManifest = (member: RustPackage): boolean =>
  Object.keys(member.manifest).every((key) => ["package", "lints", "dependencies", "dev-dependencies", "build-dependencies"].includes(key)) &&
  Object.keys(rustTable(member.manifest.package)).every((key) =>
    ["name", "version", "edition", "rust-version", "publish", "description", "license"].includes(key)
  );
const seedPackageManifest = (name: string, base: RustSnapshot, head: RustSnapshot, lockChanged: ReadonlySet<string>, seeds: ScopeSeeds): void => {
  const old = base.packages.find((item) => item.name === name);
  const current = head.packages.find((item) => item.name === name);
  if (!old || !current) {
    const member = old ?? current;
    if (member && !simpleMemberManifest(member)) seeds.reasons.push(`Unproven new/removed member semantics: ${name}`);
    seeds.production.add(name);
    return;
  }
  const delta = packageManifestDelta(old, current, base, head);
  if (delta === "unknown") seeds.reasons.push(`Unproven package manifest semantics: ${name}`);
  else if (delta === "production") seeds.production.add(name);
  else if (delta === "tests") seeds.tests.add(name);
  else if (lockChanged.has(name)) seeds.production.add(name);
};
const manifestPath = (path: string): boolean => path === "rust/Cargo.toml" || path === "rust/Cargo.lock" || path.endsWith("/Cargo.toml");
const seedManifestChanges = (inputs: RustChangeInputs, changed: readonly string[], seeds: ScopeSeeds): void => {
  if (!changed.some(manifestPath)) return;
  const { base, head } = inputs;
  if (!base || !inputs.immutable) seeds.reasons.push("Manifest/lock narrowing requires immutable matching base and head snapshots");
  if (!base) return;
  if (!same(rootSemantics(base), rootSemantics(head)) || !localRootChanges(base, head)) seeds.reasons.push("Unproven root workspace/dependency semantics");
  const lockChanged = changedLocalLockMembers(base, head, seeds);
  for (const name of rustUnique([...base.packages, ...head.packages].map((item) => item.name))) seedPackageManifest(name, base, head, lockChanged, seeds);
};
const seedOwnedInput = (path: string, owner: RustPackage, seeds: ScopeSeeds): void => {
  const local = path.slice(owner.directory.length + 1);
  if (/^(?:tests|fixtures|examples|benches)\//.test(local)) seeds.tests.add(owner.name);
  else if (local.startsWith("src/") && local.endsWith(".rs")) seeds.production.add(owner.name);
  else if (local === "build.rs") seeds.reasons.push(`Build-script changes require complete input acceptance: ${path}`);
  else if (!isRustDocumentation(path)) seeds.reasons.push(`Uncatalogued package input: ${path}`);
};
const workerInputPath = (path: string): boolean =>
  /^(?:favicon(?:-32)?\.png|entrypoint\.mjs|cloudflare\.config\.ts|wrangler\.config\.ts|package\.json|bun\.lock|ops\/rust-worker-build\.sh)$/.test(path) ||
  path.startsWith("static/");
const snapshotInputOwners = (snapshot: RustSnapshot | null, path: string, tests: boolean): readonly string[] => {
  if (!snapshot) return [];
  const ownership = tests ? snapshot.testInputOwners : snapshot.compiledOwners;
  return Object.hasOwn(ownership, path) ? ownership[path] : [];
};
const inputOwners = (inputs: RustChangeInputs, path: string, tests: boolean): readonly string[] =>
  rustUnique([...snapshotInputOwners(inputs.base, path, tests), ...snapshotInputOwners(inputs.head, path, tests)]);
const actionInputPath = (inputs: RustChangeInputs, path: string): boolean =>
  isRustGateInput(path) || inputOwners(inputs, path, false).length > 0 || inputOwners(inputs, path, true).length > 0;
const seedChangedPath = (path: string, inputs: RustChangeInputs, graph: readonly RustPackage[], seeds: ScopeSeeds): void => {
  if (manifestPath(path)) return;
  const production = inputOwners(inputs, path, false);
  const tests = inputOwners(inputs, path, true);
  for (const name of production) seeds.production.add(name);
  for (const name of tests) seeds.tests.add(name);
  const owner = graph.find((item) => path.startsWith(`${item.directory}/`));
  if (owner) {
    seedOwnedInput(path, owner, seeds);
    return;
  }
  if (path.startsWith("rust/crates/")) throw new Error(`Changed Rust path has no registered Cargo member: ${path}`);
  if (workerInputPath(path)) {
    seeds.production.add("gateway-worker");
    seeds.workerInput = true;
    return;
  }
  if (!production.length && !tests.length) seeds.reasons.push(`Rust policy or unclassified input changed: ${path}`);
};

const nativeOnlyPackages = (inputs: RustChangeInputs, graph: readonly RustPackage[]): Set<string> => {
  const nativeOnly = new Set<string>(RUST_NATIVE_PACKAGES);
  const base = inputs.base;
  if (!base) return nativeOnly;
  const added = inputs.head.packages.filter((candidate) => !base.packages.some((old) => old.name === candidate.name));
  // New helpers require at least one consumer and every consumer proven native.
  for (;;) {
    const before = nativeOnly.size;
    for (const item of added) {
      const consumers = reverse(graph, new Set([item.name]), true);
      if (consumers.size > 1 && [...consumers].every((name) => name === item.name || nativeOnly.has(name))) nativeOnly.add(item.name);
    }
    if (before === nativeOnly.size) return nativeOnly;
  }
};
const assertWorkerPortable = (worker: ReadonlySet<string>, nativeOnly: Iterable<string>): void => {
  if ([...nativeOnly].some((name) => worker.has(name))) throw new Error("Worker dependency closure contains a native-only package");
};
const testInputClosure = (graph: readonly RustPackage[], production: ReadonlySet<string>, tests: ReadonlySet<string>): Set<string> => {
  const affected = new Set(production);
  for (const item of graph) if (item.edges.some((edge) => production.has(edge.name))) affected.add(item.name);
  for (const name of tests) affected.add(name);
  return affected;
};
type ScopeActions = Pick<RustScope, "nativePackages" | "nativeTestPackages" | "wasmPackages" | "fixtureBuilds" | "shippingWorkerBuild">;
const scopeActions = (
  inputs: RustChangeInputs,
  graph: readonly RustPackage[],
  nativeOnly: ReadonlySet<string>,
  seeds: ScopeSeeds,
  hasChanges: boolean
): ScopeActions => {
  const full = seeds.reasons.length > 0;
  const production = full ? new Set(inputs.head.packages.map((item) => item.name)) : reverse(graph, seeds.production, false);
  const tests = testInputClosure(graph, production, seeds.tests);
  const affected = new Set([...production, ...tests]);
  const portable = full ? production : reverse(graph, seeds.production, false, true);
  const nativePackages = rustUnique(inputs.head.packages.filter((item) => affected.has(item.name)).map((item) => item.name));
  return {
    nativePackages,
    wasmPackages: rustUnique(inputs.head.packages.filter((item) => !nativeOnly.has(item.name) && portable.has(item.name)).map((item) => item.name)),
    nativeTestPackages: rustUnique(inputs.head.packages.filter((item) => item.hasTests && hasChanges && tests.has(item.name)).map((item) => item.name)),
    fixtureBuilds: nativePackages.filter((name) => name === "gateway-worker-fixture" || name === "gateway-worker-fixture-setup"),
    shippingWorkerBuild: full || seeds.workerInput || portable.has("gateway-worker"),
  };
};
const scopeMode = (seeds: ScopeSeeds, actions: ScopeActions): RustScope["mode"] => {
  if (seeds.reasons.length) return "full";
  return actions.nativePackages.length ? "selected" : "not-required";
};
const scopeReasons = (seeds: ScopeSeeds, actions: ScopeActions): readonly string[] => {
  if (seeds.reasons.length) return seeds.reasons;
  return [actions.nativePackages.length ? "Union of old/new production and test input closures" : "No compiler inputs changed"];
};
const scopePolicyActions = (actions: ScopeActions, complete: boolean, hasChanges: boolean): readonly string[] => {
  if (actions.nativePackages.length || complete) return ["all-source-structural-policy", "format"];
  return hasChanges ? ["format"] : [];
};
export const selectRustScope = (inputs: RustChangeInputs, complete = false): RustScope => {
  const { base, head, paths } = inputs;
  const changed = paths.filter((path) => actionInputPath(inputs, path));
  const graph = [...(base?.packages ?? []), ...head.packages];
  const seeds: ScopeSeeds = { production: new Set(), tests: new Set(), reasons: [], workerInput: false };
  const worker = workerClosure(graph);
  assertWorkerPortable(worker, RUST_NATIVE_PACKAGES);
  if (complete) seeds.reasons.push("Complete acceptance explicitly retains every package and a fresh Worker artifact");
  const uncertain = rustUnique([...(base?.uncertainInputs ?? []), ...head.uncertainInputs]);
  if (changed.length && uncertain.length) seeds.reasons.push(`Unproven compiled input syntax: ${uncertain.join(", ")}`);
  seedManifestChanges(inputs, changed, seeds);
  for (const path of changed) seedChangedPath(path, inputs, graph, seeds);
  const nativeOnly = nativeOnlyPackages(inputs, graph);
  assertWorkerPortable(worker, nativeOnly);
  const actions = scopeActions(inputs, graph, nativeOnly, seeds, changed.length > 0);
  return {
    version: 1,
    mode: scopeMode(seeds, actions),
    paths,
    inputPaths: changed,
    base: base?.identity ?? null,
    head: head.identity,
    inputsHash: rustHash(rustCanonical({ base: base?.hashes ?? null, head: head.hashes, paths })),
    reasons: scopeReasons(seeds, actions),
    policyActions: scopePolicyActions(actions, complete, changed.length > 0),
    ...actions,
  };
};
export const rustScopeForChanges = (root: string, paths: readonly string[], complete = false, environment: NodeJS.ProcessEnv = process.env): RustScope =>
  selectRustScope(loadRustChangeInputs(root, paths, environment), complete);
