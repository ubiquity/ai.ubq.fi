import { join } from "node:path";
import { readRustSnapshot, workingRustReader } from "./rust-inputs.ts";
import { rustScopeForChanges } from "./rust-scope.ts";
import { rustClippyCommand, rustQualityCommands } from "./rust-verify-quality.ts";
import type { RustCommand, RustPlan, RustStage } from "./rust-verify.ts";

export const rustPackages = (root: string) =>
  readRustSnapshot(workingRustReader(root)).packages.map((item) => ({
    ...item,
    directory: join(root, item.directory),
    dependencies: [...new Set(item.edges.map((edge) => edge.name))],
    runtimeDependencies: [...new Set(item.edges.filter((edge) => edge.kind !== "dev-dependencies" && edge.portable).map((edge) => edge.name))],
  }));
const testCommands = (names: readonly string[], target: string, cwd: string): RustCommand[] =>
  names.flatMap<RustCommand>((name) => {
    const argv = ["cargo", "test", "--locked", "--offline", "--target", target, "-p", name];
    // Darwin prelaunch tests install their own child profiles; nesting those
    // beneath the package-wide Seatbelt changes the policy being tested.
    if (name === "gateway-native" && target === "aarch64-apple-darwin")
      return [
        { argv, cwd, isolated: true, darwinNativeTests: true },
        { argv: [...argv, "--features", "native-import-fixture"], cwd, isolated: true, nativeImportFixture: true },
      ];
    return [
      { argv: [...argv, "--", "--list"], cwd, isolated: true, testListing: true },
      { argv, cwd, isolated: true, testRun: true },
    ];
  });
export const createRustVerificationPlan = (root: string, paths: readonly string[], stage: RustStage, complete: boolean, target: string): RustPlan => {
  const scope = rustScopeForChanges(root, paths, complete);
  const names = stage === "native" ? scope.nativePackages : scope.wasmPackages;
  if (names.length === 0) throw new Error(`No Rust work selected; Rust ${stage} gate has no selected packages and is not a verification result`);
  const flags = names.flatMap((name) => ["-p", name]);
  const cwd = join(root, "rust");
  const cargo = ["--locked", "--offline", "--target", target, ...flags];
  const commands: RustCommand[] = [
    ...rustQualityCommands(root, cwd),
    { argv: ["cargo", "fmt", "--all", "--", "--check"], cwd, isolated: true },
    rustClippyCommand(stage, cwd, cargo, names),
  ];
  const tests = stage === "native" ? scope.nativeTestPackages : [];
  commands.push(...testCommands(tests, target, cwd));
  const buildWorker = stage === "wasm" && scope.shippingWorkerBuild;
  if (buildWorker) {
    const worker = rustPackages(root).find((item) => item.name === "gateway-worker");
    if (!worker) throw new Error("Rust workspace is missing gateway-worker");
    commands.push({ argv: ["worker-build", "--release", "--locked"], cwd: worker.directory, isolated: true });
  }
  return { stage, packages: names, testPackages: tests, buildWorker, commands, scope };
};
