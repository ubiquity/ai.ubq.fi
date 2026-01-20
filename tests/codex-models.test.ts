import assert from "node:assert/strict";
import { extractCodexModelsFromText, resolveCodexBinaryPath } from "../scripts/codex-models.ts";

Deno.test("resolveCodexBinaryPath uses vendor binary for wrapper script", async () => {
  const wrapper = `#!/usr/bin/env node
const vendorRoot = "vendor";
const targetTriple = "aarch64-apple-darwin";
const codexBinaryName = "codex";
`;
  const resolved = await resolveCodexBinaryPath(
    "/opt/bin/codex",
    () => Promise.resolve(wrapper),
    "darwin",
    "aarch64",
  );
  assert.equal(resolved, "/opt/vendor/aarch64-apple-darwin/codex/codex");
});

Deno.test("resolveCodexBinaryPath falls back for non-wrapper input", async () => {
  const resolved = await resolveCodexBinaryPath(
    "/usr/local/bin/codex",
    () => Promise.resolve("binary"),
    "darwin",
    "aarch64",
  );
  assert.equal(resolved, "/usr/local/bin/codex");
});

Deno.test("resolveCodexBinaryPath uses realPath for symlink wrappers", async () => {
  const wrapper = `#!/usr/bin/env node
const vendorRoot = "vendor";
const targetTriple = "aarch64-apple-darwin";
const codexBinaryName = "codex";
`;
  let readPath = "";
  const resolved = await resolveCodexBinaryPath(
    "/usr/local/bin/codex",
    (path) => {
      readPath = path;
      return Promise.resolve(wrapper);
    },
    "darwin",
    "aarch64",
    () => Promise.resolve("/opt/lib/node_modules/@openai/codex/bin/codex.js"),
  );
  assert.equal(readPath, "/opt/lib/node_modules/@openai/codex/bin/codex.js");
  assert.equal(resolved, "/opt/lib/node_modules/@openai/codex/vendor/aarch64-apple-darwin/codex/codex");
});

Deno.test("extractCodexModelsFromText parses slugs and reasoning levels", () => {
  const text =
    'codex_cli_rs/0.99.0 {"slug":"gpt-5.2-codex","supported_reasoning_levels":[{"effort":"low"},{"effort":"high"}]}';
  const extracted = extractCodexModelsFromText(text);
  assert.ok(extracted);
  assert.equal(extracted?.clientVersion, "0.99.0");
  assert.equal(extracted?.models[0]?.slug, "gpt-5.2-codex");
  assert.deepEqual(extracted?.models[0]?.supported_reasoning_levels, ["low", "high"]);
});

Deno.test("extractCodexModelsFromText trims large fields", () => {
  const text =
    'codex_cli_rs/0.99.0 {"slug":"gpt-5.2-codex","display_name":"Codex","description":"desc","base_instructions":"big","supported_reasoning_levels":["low"]}';
  const extracted = extractCodexModelsFromText(text);
  const model = extracted?.models[0] as Record<string, unknown>;
  assert.equal(model?.slug, "gpt-5.2-codex");
  assert.equal(model?.display_name, "Codex");
  assert.equal(model?.description, "desc");
  assert.equal("base_instructions" in model, false);
});
