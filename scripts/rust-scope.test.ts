import { test } from "bun:test";
import assert from "node:assert/strict";
import { readRustSnapshot, type RustSnapshot } from "./rust-inputs.ts";
import { selectRustScope } from "./rust-scope.ts";

type Options = Readonly<{
  manifests?: Readonly<Record<string, string>>;
  rootExtra?: string;
  extraFiles?: Readonly<Record<string, string>>;
  lockExtra?: string;
  lockVersion?: number;
  members?: readonly string[];
}>;
const fixture = (options: Options = {}): RustSnapshot => {
  const manifests: Record<string, string> = {
    "gateway-core": "",
    "gateway-worker": '[dependencies]\ngateway-core={path="../gateway-core"}\n',
    "gateway-native": '[dependencies]\ngateway-core={path="../gateway-core"}\n',
    ...options.manifests,
  };
  const names = options.members ?? Object.keys(manifests);
  const memberPaths = names.map((name) => `crates/${name}`);
  const contents: Record<string, string> = {
    "rust/Cargo.toml": `[workspace]\nmembers=${JSON.stringify(memberPaths)}\n${options.rootExtra ?? ""}`,
    ...options.extraFiles,
  };
  const locked: string[] = [];
  for (const name of names) {
    const body = manifests[name] ?? "";
    contents[`rust/crates/${name}/Cargo.toml`] = `[package]\nname="${name}"\nversion="0.1.0"\n${body}`;
    contents[`rust/crates/${name}/src/lib.rs`] ??= "pub fn value() {}\n";
    contents[`rust/crates/${name}/tests/contracts.rs`] ??= "#[test] fn contract() {}\n";
    const dependencies = body.split("\n").flatMap((line) => {
      const dependency = /^([a-z-]+)=\{(?:package="([^"]+)",)?path="\.\.\/[^"]+"/.exec(line);
      return dependency ? [dependency[2] || dependency[1]] : [];
    });
    locked.push(`[[package]]\nname="${name}"\nversion="0.1.0"\ndependencies=${JSON.stringify([...new Set(dependencies)])}\n`);
  }
  contents["rust/Cargo.lock"] = `version=${options.lockVersion ?? 4}\n${locked.join("\n")}${options.lockExtra ?? ""}`;
  return readRustSnapshot({ identity: "1".repeat(40), files: Object.keys(contents), read: (path) => new TextEncoder().encode(contents[path]) });
};
const select = (base: RustSnapshot, head: RustSnapshot, paths: readonly string[], complete = false) =>
  selectRustScope({ paths, base, head, immutable: true }, complete);

test("native-only local extraction includes full root/lock deltas without selecting WASM or Worker", () => {
  const base = fixture();
  const head = fixture({
    manifests: {
      "gateway-native-helper": "",
      "gateway-native": '[dependencies]\ngateway-core={path="../gateway-core"}\ngateway-native-helper={path="../gateway-native-helper"}\n',
    },
    rootExtra: '[workspace.dependencies]\ngateway-native-helper={path="crates/gateway-native-helper"}\n',
  });
  const scope = select(base, head, [
    "rust/Cargo.toml",
    "rust/Cargo.lock",
    "rust/crates/gateway-native/Cargo.toml",
    "rust/crates/gateway-native-helper/src/lib.rs",
  ]);
  assert.equal(scope.mode, "selected");
  assert.deepEqual(scope.nativePackages, ["gateway-native", "gateway-native-helper"]);
  assert.deepEqual(scope.nativeTestPackages, ["gateway-native", "gateway-native-helper"]);
  assert.deepEqual(scope.wasmPackages, []);
  assert.equal(scope.shippingWorkerBuild, false);
  assert.ok(scope.paths.includes("rust/Cargo.lock"));
  assert.deepEqual(scope.policyActions, ["all-source-structural-policy", "format"]);
});

test("test and runtime fixture changes stop at the owner even when its library has production consumers", () => {
  const source = fixture();
  for (const path of [
    "rust/crates/gateway-core/tests/contracts.rs",
    "rust/crates/gateway-core/tests/data.md",
    "rust/crates/gateway-core/examples/fixture.rs",
  ]) {
    const scope = select(source, source, [path]);
    assert.deepEqual(scope.nativePackages, ["gateway-core"]);
    assert.deepEqual(scope.nativeTestPackages, ["gateway-core"]);
    assert.deepEqual(scope.wasmPackages, []);
    assert.equal(scope.shippingWorkerBuild, false);
  }
});

test("production source keeps the same consumer closure for private and public edits", () => {
  const source = fixture();
  const scope = select(source, source, ["rust/crates/gateway-core/src/lib.rs"]);
  assert.deepEqual(scope.nativePackages, ["gateway-core", "gateway-native", "gateway-worker"]);
  assert.deepEqual(scope.wasmPackages, ["gateway-core", "gateway-worker"]);
  assert.equal(scope.shippingWorkerBuild, true);
});

test("dev-only consumers receive test checks without propagating through production edges", () => {
  const source = fixture({
    manifests: {
      support: "",
      "gateway-core": '[dev-dependencies]\nsupport={path="../support"}\n',
    },
  });
  const scope = select(source, source, ["rust/crates/support/src/lib.rs"]);
  assert.deepEqual(scope.nativePackages, ["gateway-core", "support"]);
  assert.equal(scope.shippingWorkerBuild, false);
});

test("edge deletion and redirect retain old consumers and deletion owners", () => {
  const base = fixture({ manifests: { "old-helper": "", "gateway-native": '[dependencies]\nold-helper={path="../old-helper"}\n' } });
  const head = fixture({ manifests: { "new-helper": "", "gateway-native": '[dependencies]\nnew-helper={path="../new-helper"}\n' } });
  const scope = select(base, head, [
    "rust/Cargo.toml",
    "rust/Cargo.lock",
    "rust/crates/old-helper/src/lib.rs",
    "rust/crates/gateway-native/Cargo.toml",
    "rust/crates/new-helper/src/lib.rs",
  ]);
  assert.deepEqual(scope.nativePackages, ["gateway-native", "new-helper"]);
  assert.equal(scope.shippingWorkerBuild, false);
});

test("optional, build, proc-macro and unknown target dependencies cannot hide native Worker inputs", () => {
  for (const declaration of [
    '[dependencies]\ngateway-native={path="../gateway-native",optional=true}\n[features]\nentrypoint=["dep:gateway-native"]\n',
    '[build-dependencies]\ngateway-native={path="../gateway-native"}\n',
    '[target.\'cfg(feature = "unknown")\'.dependencies]\ngateway-native={path="../gateway-native"}\n',
    '[target.\'cfg(all(unix, not(target_arch = "wasm32")))\'.build-dependencies]\ngateway-native={path="../gateway-native"}\n',
  ]) {
    const source = fixture({ manifests: { "gateway-worker": declaration } });
    assert.throws(() => select(source, source, ["rust/crates/gateway-worker/src/lib.rs"]), /native-only package/);
  }
  const reviewed = fixture({
    manifests: { "gateway-worker": '[target.\'cfg(all(unix, not(target_arch = "wasm32")))\'.dependencies]\ngateway-native={path="../gateway-native"}\n' },
  });
  assert.equal(select(reviewed, reviewed, ["rust/crates/gateway-native/src/lib.rs"]).shippingWorkerBuild, false);
});

test("root feature/profile/config and external lock changes select the complete relevant stages", () => {
  const base = fixture();
  const changes = [
    fixture({ rootExtra: "[profile.dev]\ndebug=0\n" }),
    fixture({ lockVersion: 5 }),
    fixture({ lockExtra: '\n[[package]]\nname="external"\nversion="1.0.0"\nsource="registry+fixture"\nchecksum="changed"\n' }),
    fixture({ manifests: { "gateway-core": '[features]\ndefault=["new"]\nnew=[]\n' } }),
  ];
  for (const head of changes) {
    const scope = select(base, head, ["rust/Cargo.toml", "rust/Cargo.lock", "rust/crates/gateway-core/Cargo.toml"]);
    assert.equal(scope.mode, "full");
    assert.equal(scope.shippingWorkerBuild, true);
    assert.deepEqual(scope.nativePackages, ["gateway-core", "gateway-native", "gateway-worker"]);
  }
  assert.equal(select(base, base, ["rust/.cargo/config.toml"]).mode, "full");
  assert.equal(selectRustScope({ paths: ["rust/Cargo.lock"], base: null, head: base, immutable: false }).mode, "full");
});

test("compiled assets, packaging assets and literal fixture includes require a new Worker artifact", () => {
  const source = fixture({
    extraFiles: {
      "rust/crates/gateway-worker/src/lib.rs": 'pub const DATA: &str = include_str!("../tests/data.md");',
      "rust/crates/gateway-worker/tests/data.md": "compiled",
    },
  });
  for (const path of [
    "favicon.png",
    "favicon-32.png",
    "entrypoint.mjs",
    "static/readme.md",
    "cloudflare.config.ts",
    "package.json",
    "rust/crates/gateway-worker/tests/data.md",
  ]) {
    const scope = select(source, source, [path]);
    assert.equal(scope.shippingWorkerBuild, true, path);
    assert.ok(scope.wasmPackages.includes("gateway-worker"));
  }
  assert.equal(select(source, source, ["docs/unrelated.md"]).mode, "not-required");
  assert.equal(select(source, source, ["docs/unrelated.md"], true).shippingWorkerBuild, true);
});

test("lock identity ambiguity and incomplete local inventories are errors rather than narrow passes", () => {
  const source = fixture();
  const duplicate = fixture({ lockExtra: '\n[[package]]\nname="gateway-core"\nversion="0.1.0"\n' });
  assert.throws(() => select(source, duplicate, ["rust/Cargo.lock"]), /Duplicate Rust lock identity/);
  const ambiguous = fixture({ lockExtra: '\n[[package]]\nname="gateway-core"\nversion="2.0.0"\nsource="registry+fixture"\n' });
  assert.throws(() => select(source, ambiguous, ["rust/Cargo.lock"]), /Ambiguous/);
});
