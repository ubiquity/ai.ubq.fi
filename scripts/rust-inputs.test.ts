import { afterAll, test } from "bun:test";
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { mkdirSync, mkdtempSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { literalRustIncludes, loadRustChangeInputs, readRustSnapshot, rustPath, workingRustReader } from "./rust-inputs.ts";

const temporary = mkdtempSync(join(tmpdir(), "uos-rust-inputs-"));
afterAll(() => {
  rmSync(temporary, { recursive: true, force: true });
});
const files = (): Record<string, string> => ({
  "rust/Cargo.toml":
    '[workspace]\nmembers=["crates/core", "crates/consumer"]\n[workspace.package]\nversion="0.1.0"\n[workspace.dependencies]\nalias={package="core",path="crates/core"}\n',
  "rust/Cargo.lock": 'version=4\n[[package]]\nname="core"\nversion="0.1.0"\n[[package]]\nname="consumer"\nversion="0.1.0"\ndependencies=["core"]\n',
  "rust/crates/core/Cargo.toml": '[package]\nname="core"\nversion.workspace=true\n',
  "rust/crates/core/src/lib.rs": "pub fn answer() -> u8 { 42 }\n",
  "rust/crates/consumer/Cargo.toml": '[package]\nname="consumer"\nversion="0.1.0"\n[dependencies]\nalias.workspace=true\n',
  "rust/crates/consumer/tests/contract.rs": "#[test] fn contract() {}\n",
});
const snapshot = (contents = files()) =>
  readRustSnapshot({ identity: "1".repeat(40), files: Object.keys(contents), read: (path) => new TextEncoder().encode(contents[path]) });
const materialize = (contents = files()): string => {
  const root = mkdtempSync(join(temporary, "source-"));
  for (const [path, value] of Object.entries(contents)) {
    mkdirSync(join(root, path, ".."), { recursive: true });
    writeFileSync(join(root, path), value);
  }
  return root;
};
const git = (root: string, ...argv: string[]): string => {
  const result = spawnSync("/usr/bin/git", argv, { cwd: root, encoding: "utf8", env: { PATH: "/usr/bin:/bin", HOME: temporary } });
  assert.equal(result.status, 0, result.stderr);
  return result.stdout.trim();
};

test("immutable input parsing resolves inherited renamed local dependencies by path", () => {
  const result = snapshot();
  assert.deepEqual(result.packages[1].edges, [{ name: "core", kind: "dependencies", portable: true }]);
  assert.equal(result.packages[0].version, "0.1.0");
  assert.equal(result.packages[1].hasTests, true);
  assert.match(result.hashes["rust/Cargo.lock"], /^[a-f0-9]{64}$/);
  const changed = files();
  changed["rust/Cargo.toml"] += "# comment\n";
  assert.deepEqual(snapshot(changed).root, result.root);
  assert.notEqual(snapshot(changed).hashes["rust/Cargo.toml"], result.hashes["rust/Cargo.toml"]);
});

test("input reader rejects missing manifests, malformed locks, member wildcards and ambiguous identities", () => {
  const missing = files();
  delete missing["rust/Cargo.lock"];
  assert.throws(() => snapshot(missing), /Cargo.lock is required/);
  const malformed = files();
  malformed["rust/Cargo.lock"] = "not valid = [";
  assert.throws(() => snapshot(malformed));
  const wildcard = files();
  wildcard["rust/Cargo.toml"] = '[workspace]\nmembers=["crates/*"]\n';
  assert.throws(() => snapshot(wildcard), /explicit, nonempty/);
  const duplicate = files();
  duplicate["rust/crates/consumer/Cargo.toml"] = '[package]\nname="core"\n';
  assert.throws(() => snapshot(duplicate), /duplicate/);
  const mismatch = files();
  mismatch["rust/crates/consumer/Cargo.toml"] = '[package]\nname="consumer"\n[dependencies]\nwrong={path="../core"}\n';
  assert.throws(() => snapshot(mismatch), /mismatched/);
});

test("unsafe paths and linked member/source inputs fail before any compiler entrypoint", () => {
  for (const path of ["../secret", join(temporary, "secret"), "rust//file", "rust/./file", "rust\\file", "rust/../file", "rust/\0file"])
    assert.throws(() => rustPath(path), /Unsafe/);
  const escaping = files();
  escaping["rust/Cargo.toml"] = '[workspace]\nmembers=["../outside"]\n';
  assert.throws(() => snapshot(escaping), /escapes/);
  const root = materialize();
  rmSync(join(root, "rust/crates/core/src/lib.rs"));
  symlinkSync(join(root, "rust/Cargo.lock"), join(root, "rust/crates/core/src/lib.rs"));
  assert.throws(() => workingRustReader(root), /Linked Rust input/);
});

test("literal compiled fixture ownership overrides its documentation extension", () => {
  const contents = files();
  contents["rust/crates/core/src/lib.rs"] = 'pub const TEXT: &str = include_str!("../tests/data.md");\n';
  contents["rust/crates/core/tests/data.md"] = "embedded input";
  assert.deepEqual(snapshot(contents).compiledOwners["rust/crates/core/tests/data.md"], ["core"]);
  delete contents["rust/crates/core/tests/data.md"];
  assert.deepEqual(snapshot(contents).uncertainInputs, ["rust/crates/core/src/lib.rs"]);
});

test("integration, example, bench and support-module includes retain test ownership", () => {
  const contents = files();
  contents["fixtures/corpus.md"] = "external test input";
  for (const directory of ["tests", "examples", "benches"])
    contents[`rust/crates/consumer/${directory}/fixture.rs`] = 'fn fixture() { let _ = include_str!("../../../../fixtures/corpus.md"); }';
  contents["rust/crates/consumer/tests/support/mod.rs"] = 'fn fixture() { let _ = include_str!("../../../../../fixtures/corpus.md"); }';
  const result = snapshot(contents);
  assert.deepEqual([...new Set(result.testInputOwners["fixtures/corpus.md"])], ["consumer"]);
  assert.equal(Object.hasOwn(result.compiledOwners, "fixtures/corpus.md"), false);
  assert.match(result.hashes["fixtures/corpus.md"], /^[a-f0-9]{64}$/);
});

test("an external include cannot cross a linked directory", () => {
  const contents = files();
  contents["data/fixture.json"] = "{}";
  contents["rust/crates/consumer/tests/contract.rs"] = 'fn fixture() { let _ = include_str!("../../../../linked/fixture.json"); }';
  const root = materialize(contents);
  symlinkSync(join(root, "data"), join(root, "linked"));
  assert.throws(() => readRustSnapshot(workingRustReader(root)), /Linked Rust input/);
});

test("literal input discovery ignores Rust source strings and nested comments", () => {
  const source =
    'const SOURCE: &str = r###"include_str!("fake.md")"###; /* include_str!("comment.md") /* nested */ */\nconst REAL: &str = include_str!(r#"real.md"#);\nconst DATA: &[u8] = include_bytes!("real.bin");';
  assert.deepEqual(literalRustIncludes(source), { paths: ["real.md", "real.bin"], uncertain: false });
  assert.equal(literalRustIncludes('const INPUT: &str = include_str!(concat!(env!("ROOT"), "/input"));').uncertain, true);
  assert.equal(literalRustIncludes('const INPUT: &str = include_str! { "input.md" };').uncertain, true);
  assert.equal(literalRustIncludes('const INPUT: &str = include_str /* comment */ !("input.md");').uncertain, true);
});

test("Git snapshots include root deltas and deleted paths even if an explicit path list omits them", () => {
  const root = materialize();
  git(root, "init", "--initial-branch=development");
  git(root, "config", "user.name", "Rust scope fixture");
  git(root, "config", "user.email", "scope-fixture@example.invalid");
  git(root, "add", ".");
  git(root, "commit", "-m", "fixture base");
  const base = git(root, "rev-parse", "HEAD");
  writeFileSync(join(root, "rust/Cargo.toml"), files()["rust/Cargo.toml"] + "# head snapshot\n");
  rmSync(join(root, "rust/crates/consumer/tests/contract.rs"));
  mkdirSync(join(root, "lib/codex"), { recursive: true });
  git(root, "add", "-A");
  git(root, "update-index", "--add", "--cacheinfo", "160000", base, "lib/codex");
  git(root, "commit", "-m", "fixture head");
  const result = loadRustChangeInputs(root, ["rust/crates/core/src/lib.rs"], { TEST_SCOPE_BASE: base });
  assert.equal(result.immutable, true);
  assert.equal(result.base?.identity, base);
  assert.equal(result.head.identity, git(root, "rev-parse", "HEAD"));
  assert.deepEqual(result.paths, ["lib/codex", "rust/Cargo.toml", "rust/crates/consumer/tests/contract.rs", "rust/crates/core/src/lib.rs"]);
  writeFileSync(join(root, "rust/crates/core/src/lib.rs"), "uncommitted compiler input\n");
  const dirty = loadRustChangeInputs(root, [], { TEST_SCOPE_BASE: base });
  assert.equal(dirty.immutable, false);
  assert.equal(dirty.base, null);
  assert.equal(dirty.head.identity, null);
});

test("dirty external includes and ignored Cargo configs invalidate immutable source proof", () => {
  const contents = files();
  contents["data/fixture.json"] = "{}";
  contents["rust/crates/consumer/tests/contract.rs"] = '#[test] fn fixture() { let _ = include_str!("../../../../data/fixture.json"); }';
  const root = materialize(contents);
  git(root, "init", "--initial-branch=development");
  git(root, "config", "user.name", "Rust scope fixture");
  git(root, "config", "user.email", "scope-fixture@example.invalid");
  git(root, "add", ".");
  git(root, "commit", "-m", "external input fixture");
  const base = git(root, "rev-parse", "HEAD");
  writeFileSync(join(root, "data/fixture.json"), '{"changed":true}');
  const dirty = loadRustChangeInputs(root, [], { TEST_SCOPE_BASE: base });
  assert.equal(dirty.immutable, false);
  assert.equal(dirty.head.identity, null);
  assert.ok(dirty.paths.includes("data/fixture.json"));
  writeFileSync(join(root, "data/fixture.json"), "{}");
  writeFileSync(join(root, ".git/info/exclude"), ".cargo/\n");
  mkdirSync(join(root, ".cargo"));
  writeFileSync(join(root, ".cargo/config.toml"), "[build]\njobs=1\n");
  const ignored = loadRustChangeInputs(root, [], { TEST_SCOPE_BASE: base });
  assert.equal(ignored.immutable, false);
  assert.equal(ignored.head.identity, null);
  assert.ok(ignored.paths.includes(".cargo/config.toml"));
});
