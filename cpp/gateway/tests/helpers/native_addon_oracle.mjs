// Regenerate only the Linux x64 fixture from Bun 1.4.0 and the real TypeScript modules.
// Run with --no-env-file --no-install, an empty environment and task-owned HOME/TMPDIR/cwd.
import { createHash } from "node:crypto";
import { chmodSync, linkSync, mkdirSync, mkdtempSync, readFileSync, realpathSync, renameSync, rmSync, symlinkSync, writeFileSync } from "node:fs";
import { tmpdir } from "node:os";
import { join } from "node:path";
import { pathToFileURL } from "node:url";
import { verifyNativeKvArtifact } from "../../../../src/storage/native-addon.ts";
import { readNativeKvDevelopmentBinding } from "../../../../src/runtime/native-kv-bootstrap.ts";

if (Bun.version !== "1.4.0" || process.platform !== "linux" || process.arch !== "x64") throw new Error("Requires Bun 1.4.0 on Linux x64");
const source = readFileSync(new URL("../native_addon_differential_test.cpp", import.meta.url), "utf8");
const vectors = JSON.parse(source.match(/R"VECTORS\(([\s\S]*?)\)VECTORS"/)[1]);
const original = structuredClone(vectors);
const sha = (value) => createHash("sha256").update(value).digest("hex");
const base = JSON.parse(vectors.texts.inputs);
base.artifact.platform = process.platform;
base.artifact.architecture = process.arch;
const setPath = (root, path, value) => {
  const parts = path.split(".");
  const last = parts.pop();
  let parent = root;
  for (const part of parts) parent = parent[part];
  parent[last] = structuredClone(value);
};
const deletePath = (root, path) => {
  const parts = path.split(".");
  const last = parts.pop();
  let parent = root;
  for (const part of parts) parent = parent[part];
  delete parent[last];
};
const buildTexts = (spec) => {
  const inputs = structuredClone(base);
  for (const [path, value] of Object.entries(spec.inputsSet ?? {})) setPath(inputs, path, value);
  for (const path of spec.inputsDelete ?? []) deletePath(inputs, path);
  const inputsText = spec.inputsText ?? JSON.stringify(inputs);
  const provenance = {
    ...inputs,
    inputsSha256: sha(inputsText),
    artifact: { ...inputs.artifact, sha256: sha(new Uint8Array([1, 2, 3])) },
    build: { exitCode: 0, logSha256: "c".repeat(64) },
  };
  for (const [path, value] of Object.entries(spec.provenanceSet ?? {})) setPath(provenance, path, value);
  for (const path of spec.provenanceDelete ?? []) deletePath(provenance, path);
  let provenanceText = spec.provenanceText ?? JSON.stringify(provenance);
  if (spec.provenanceReplace) provenanceText = provenanceText.replace(...spec.provenanceReplace);
  return { inputsText, provenanceText, bytes: new Uint8Array(spec.artifactBytes ?? [1, 2, 3]) };
};
const outcome = (root, fn) => {
  try {
    return { value: JSON.parse(JSON.stringify(fn()).replaceAll(root, "<root>")) };
  } catch (error) {
    return { error: { name: error.name, message: error.message.replaceAll(root, "<root>") } };
  }
};
const directory = (path, mode = 0o777, recursive = false) => mkdirSync(path, { mode, recursive });
const write = (path, data, mode = 0o666) => writeFileSync(path, data, { mode });
const scratch = realpathSync(mkdtempSync(join(tmpdir(), "native-oracle-")));
try {
  for (const entry of vectors.verify) {
    const spec = entry.spec;
    const root = mkdtempSync(join(scratch, "verify-"));
    const artifact = join(root, "artifact");
    directory(artifact);
    const texts = buildTexts(spec);
    write(join(artifact, "inputs.json"), texts.inputsText);
    write(join(artifact, "native-kv.provenance.json"), texts.provenanceText);
    write(join(artifact, "native-kv.node"), texts.bytes);
    let artifactDirectory = artifact;
    let inputsManifest = join(artifact, "inputs.json");
    if (spec.fs === "missing-dir") artifactDirectory = join(root, "absent");
    if (spec.fs === "dir-symlink") {
      renameSync(artifact, artifact + ".saved");
      symlinkSync(artifact + ".saved", artifact);
      inputsManifest = join(artifact + ".saved", "inputs.json");
    }
    if (spec.fs === "provenance-symlink") {
      renameSync(join(artifact, "native-kv.provenance.json"), join(artifact, "provenance.saved"));
      symlinkSync(join(artifact, "provenance.saved"), join(artifact, "native-kv.provenance.json"));
    }
    if (spec.fs === "provenance-missing") rmSync(join(artifact, "native-kv.provenance.json"));
    if (spec.fs === "inputs-missing") inputsManifest = join(artifact, "absent.json");
    if (spec.fs === "artifact-symlink") {
      renameSync(join(artifact, "native-kv.node"), join(artifact, "node.saved"));
      symlinkSync(join(artifact, "node.saved"), join(artifact, "native-kv.node"));
    }
    if (spec.fs === "artifact-missing") rmSync(join(artifact, "native-kv.node"));
    if (spec.forms === "url-dir") artifactDirectory = pathToFileURL(artifact);
    if (spec.forms === "url-inputs") inputsManifest = pathToFileURL(join(artifact, "inputs.json"));
    entry.outcome = outcome(root, () =>
      verifyNativeKvArtifact({ artifactDirectory, inputsManifest, expectedProvenanceSha256: spec.expectedHash ?? sha(texts.provenanceText) })
    );
  }
  const valid = buildTexts({});
  for (const entry of vectors.bootstrap) {
    const tweak = entry.spec.tweak;
    const outer = mkdtempSync(join(scratch, "development-"));
    let root = outer;
    // Construct fixture paths independently: the tested helper itself rejects x64.
    const paths = {
      data: join(root, ".data"),
      inputs: join(root, "tools", "native-kv", "inputs.json"),
      receipt: join(root, ".data", "native-kv.binding.json"),
    };
    if (tweak !== "data-missing") directory(paths.data, 0o700);
    let parent = paths.data;
    if (tweak === "artifact-nested" || tweak === "nested-0750") {
      parent = join(paths.data, "nested");
      directory(parent, 0o700);
      if (tweak === "nested-0750") chmodSync(parent, 0o750);
    }
    const artifact = join(parent, "native-kv-fixture");
    if (tweak !== "data-missing") {
      directory(artifact, 0o700);
      chmodSync(artifact, tweak === "artifact-dir-0710" ? 0o710 : 0o700);
      write(join(artifact, "native-kv.node"), valid.bytes, 0o600);
      if (tweak !== "provenance-missing") write(join(artifact, "native-kv.provenance.json"), valid.provenanceText, 0o600);
      if (tweak === "artifact-hardlink") linkSync(join(artifact, "native-kv.node"), join(artifact, "node.link"));
    }
    directory(join(root, "tools", "native-kv"), 0o777, true);
    write(paths.inputs, valid.inputsText, 0o600);
    chmodSync(paths.inputs, tweak === "inputs-0644" ? 0o644 : tweak === "inputs-0620" ? 0o620 : 0o600);
    let receiptDirectory = artifact;
    if (tweak === "artifact-outside") receiptDirectory = root;
    if (tweak === "artifact-is-data") receiptDirectory = paths.data;
    if (tweak === "artifact-traverse") receiptDirectory = join(paths.data, "..", "tools");
    const document = {
      artifactDirectory: receiptDirectory,
      expectedProvenanceSha256: tweak === "wrong-hash" ? "0".repeat(64) : sha(valid.provenanceText),
      inputsManifest: paths.inputs,
    };
    if (tweak === "receipt-no-inputs") delete document.inputsManifest;
    if (tweak === "receipt-extra-key") document.extra = 1;
    if (tweak === "receipt-relative-dir") document.artifactDirectory = "native-kv-fixture";
    if (tweak === "receipt-inputs-null") document.inputsManifest = null;
    if (tweak === "receipt-inputs-number") document.inputsManifest = 7;
    if (tweak === "receipt-inputs-other") document.inputsManifest = "/elsewhere/inputs.json";
    let receiptText = JSON.stringify(document);
    if (tweak === "receipt-proto") receiptText = receiptText.slice(0, -1) + ',"__proto__":{}}';
    if (tweak === "receipt-array") receiptText = "[]";
    if (tweak === "receipt-null") receiptText = "null";
    if (tweak === "receipt-bom") receiptText = "\uFEFF" + receiptText;
    if (tweak === "receipt-oversize") receiptText += " ".repeat(17000);
    if (tweak !== "data-missing") write(paths.receipt, receiptText, 0o600);
    if (tweak === "receipt-hardlink") linkSync(paths.receipt, paths.receipt + ".link");
    if (tweak === "receipt-0640") chmodSync(paths.receipt, 0o640);
    if (tweak === "root-symlink") {
      symlinkSync(outer, outer + ".link");
      root = outer + ".link";
    }
    if (tweak === "root-missing") root = join(outer, "absent");
    entry.outcome = outcome(outer, () => readNativeKvDevelopmentBinding(root));
  }
  vectors.texts.inputs = valid.inputsText;
  vectors.texts.provenance = valid.provenanceText;
} finally {
  rmSync(scratch, { recursive: true, force: true });
}
for (const kind of ["verify", "bootstrap"]) {
  const changed = vectors[kind].filter((entry, index) => JSON.stringify(entry.outcome) !== JSON.stringify(original[kind][index].outcome));
  console.error(`${kind}: ${vectors[kind].length} real Bun outcomes, ${changed.length} platform-dependent outcomes`);
}
console.log('R"VECTORS(' + JSON.stringify(vectors) + ')VECTORS"');
