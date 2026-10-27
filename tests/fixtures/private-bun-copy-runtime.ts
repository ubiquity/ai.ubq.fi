/** Synthetic trusted preparation; no credential, capture or serving store is opened. */
import assert from "node:assert/strict";
import { spawnSync } from "node:child_process";
import { createHash } from "node:crypto";
import { lstatSync, readFileSync, realpathSync, writeFileSync } from "node:fs";
import { basename, dirname, isAbsolute, join } from "node:path";
import { fileURLToPath } from "node:url";
import { PrivateBunCopyPython, type PrivateBunCopyPythonSeal } from "../../tools/cloudflare/private-bun-copy-python.ts";

function systemPythonRole(): string[] {
  const environment = { PATH: "/usr/bin:/bin" };
  const inspect = (argv: string[]): string => {
    const result = spawnSync(argv[0], argv.slice(1), { env: environment, encoding: "utf8", timeout: 10_000 });
    assert.equal(result.status, 0, "Fixed Python role metadata verification failed");
    return result.stdout + result.stderr;
  };
  const selected = inspect(["/usr/bin/xcrun", "--find", "python3"]).trim();
  assert(isAbsolute(selected) && !selected.includes("\n"));
  const canonical = realpathSync(selected);
  const identity: unknown = JSON.parse(
    inspect([
      "/usr/bin/python3",
      "-I",
      "-S",
      "-B",
      "-c",
      String.raw`
import ctypes, json, os, sys
buffer = ctypes.create_string_buffer(4096)
length = ctypes.CDLL("/usr/lib/libproc.dylib").proc_pidpath(os.getpid(), buffer, len(buffer))
if length <= 0:
    raise RuntimeError("Python executable identity unavailable")
print(json.dumps({"selected":sys.executable,"base":sys._base_executable,"terminal":os.fsdecode(buffer.value)}))
`,
    ])
  );
  assert(typeof identity === "object" && identity !== null && "selected" in identity && "base" in identity && "terminal" in identity);
  assert.equal(identity.selected, selected);
  assert.equal(identity.base, selected);
  assert(typeof identity.terminal === "string");
  const terminal = identity.terminal;
  assert(canonical.includes("/Library/Frameworks/Python3.framework/Versions/"));
  assert.equal(terminal, join(dirname(dirname(canonical)), "Resources/Python.app/Contents/MacOS/Python"));
  assert.equal(realpathSync(terminal), terminal);
  const roles = ["/usr/bin/python3", canonical, terminal];
  const facts = roles.map((path, index) => {
    const info = lstatSync(path);
    assert(info.isFile() && !info.isSymbolicLink() && info.uid === 0 && (info.mode & 0o777) === 0o755);
    inspect(["/usr/bin/codesign", "--verify", "--strict", path]);
    const identity = inspect(["/usr/bin/codesign", "--display", "--verbose=4", "-r-", path]);
    const expected = index === 0 ? "com.apple.dt.xcode_select.tool-shim-public" : "com.apple.python3";
    assert(identity.includes('identifier "' + expected + '" and anchor apple'));
    return { path, bytes: info.size, sha256: createHash("sha256").update(readFileSync(path)).digest("hex"), designated_identity: expected };
  });
  assert.equal(realpathSync(inspect(["/usr/bin/xcrun", "--find", "python3"]).trim()), canonical);
  console.log(JSON.stringify({ kind: "verified_private_copy_python_role", selected, canonical, terminal, files: facts }));
  return roles;
}

let roles: string[] | undefined;
const seals = new Map<string, PrivateBunCopyPythonSeal>();
let icu: Readonly<{ path: string; source: string; bytes: number; sha256: string }> | undefined;

export function privateBunCopyTestIcu() {
  if (icu) return icu;
  const version = process.versions.icu?.split(".")[0];
  assert(version && /^\d+$/.test(version));
  const source = join("/usr/share/icu", "icudt" + version + "l.dat");
  const before = lstatSync(source);
  assert(before.isFile() && before.uid === 0 && (before.mode & 0o222) === 0 && realpathSync(source) === source);
  icu = Object.freeze({
    path: "runtime/icu/" + basename(source),
    source,
    bytes: before.size,
    sha256: createHash("sha256").update(readFileSync(source)).digest("hex"),
  });
  const after = lstatSync(source);
  assert.equal(before.ino, after.ino);
  assert.equal(before.mtimeMs, after.mtimeMs);
  return icu;
}

export function privateBunCopyTestSeal(directory: string): PrivateBunCopyPythonSeal {
  const retained = seals.get(directory);
  if (retained) return retained;
  roles ??= systemPythonRole();
  const files = roles.map((path) => ({ path, sha256: createHash("sha256").update(readFileSync(path)).digest("hex") }));
  const canonical = files.find((value) => value.path !== "/usr/bin/python3" && /\/bin\/python[^/]+$/.test(value.path));
  const terminal = files.find((value) => value.path.endsWith("/Resources/Python.app/Contents/MacOS/Python"));
  assert(canonical && terminal);
  const receipt =
    JSON.stringify({ v: 2, files, canonical_executable: canonical, terminal_interpreter: terminal, closure: { exact_exec_role_paths: roles } }) + "\n";
  const receiptPath = join(directory, "python-signature-receipt.json");
  writeFileSync(receiptPath, receipt, { mode: 0o600, flag: "wx" });
  const sourceRoot = fileURLToPath(new URL("../../", import.meta.url));
  const manifest = JSON.stringify({
    version: 1,
    source_sha: "a".repeat(40),
    source_tree: "b".repeat(40),
    files: ["private-bun-copy.ts", "private-bun-copy-python.ts", "private-bun-copy-launcher.ts", "private-bun-copy-profile.ts"]
      .map((name) => {
        const path = join(sourceRoot, "tools/cloudflare", name);
        return { path: "source/tools/cloudflare/" + name, bytes: lstatSync(path).size, sha256: createHash("sha256").update(readFileSync(path)).digest("hex") };
      })
      .concat([privateBunCopyTestIcu()]),
  });
  writeFileSync(join(directory, "prepared-source-manifest.json"), manifest, { mode: 0o600, flag: "wx" });
  const seal: PrivateBunCopyPythonSeal = {
    source_sha: "a".repeat(40),
    source_tree: "b".repeat(40),
    stage_manifest_sha256: createHash("sha256").update(manifest).digest("hex"),
    expected_python_roles: Object.fromEntries(files.map((value) => [value.path, value.sha256])),
    python_signature_receipt: { path: receiptPath, sha256: createHash("sha256").update(receipt).digest("hex") },
  };
  seals.set(directory, seal);
  return seal;
}

export function privateBunCopyTestRuntime(directory: string): PrivateBunCopyPython {
  return PrivateBunCopyPython.fromLauncher(privateBunCopyTestSeal(directory));
}
