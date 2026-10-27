import { createHash } from "node:crypto";
import { readFile } from "node:fs/promises";
import { join } from "node:path";

type Selection = { name: "synthetic-precommit" | "stream-runtime"; expectedCases: readonly string[] };

const INPUT_PATHS = [
  "tools/cloudflare/runtime-conformance.ts",
  "tools/cloudflare/fixtures/provider-matrix.mjs",
  "tools/cloudflare/runtime-matrix/contracts.ts",
  "tools/cloudflare/runtime-matrix/index.ts",
  "tools/cloudflare/runtime-matrix/precommit.ts",
  "tools/cloudflare/runtime-matrix/conformance-receipt.ts",
  "tools/cloudflare/runtime-matrix/workerd-config.ts",
  "tools/cloudflare/runtime-matrix/stream-settlement.ts",
  "tools/cloudflare/runtime-matrix/claude-replay.ts",
  "tools/cloudflare/runtime-matrix/stream-termination.ts",
  "tools/cloudflare/runtime-matrix/lithos-refusal.ts",
];

async function fixtureInputHashes(root: string): Promise<Record<string, string>> {
  return Object.fromEntries(
    await Promise.all(
      INPUT_PATHS.map(
        async (path) =>
          [
            path,
            createHash("sha256")
              .update(await readFile(join(root, path)))
              .digest("hex"),
          ] as const
      )
    )
  );
}

export async function emitConformanceReceipt(
  root: string,
  sha: string,
  workerdBinary: string,
  harnessPath: string,
  cases: string[],
  selection: Selection | null
): Promise<void> {
  console.log(
    JSON.stringify({
      sourceSha: sha,
      wasmSha256: createHash("sha256")
        .update(await readFile(join(root, "rust/crates/gateway-worker/build/index_bg.wasm")))
        .digest("hex"),
      fixtureWasmSha256: createHash("sha256")
        .update(await readFile(join(root, "rust/crates/gateway-worker-fixture/build/index_bg.wasm")))
        .digest("hex"),
      providerMatrixSha256: createHash("sha256")
        .update(await readFile(join(root, "tools/cloudflare/fixtures/provider-matrix.mjs")))
        .digest("hex"),
      workerd: "1.20261012.1",
      platform: process.platform,
      publicEntry: "direct-workerd-http-socket",
      harnessSha256: createHash("sha256")
        .update(await readFile(harnessPath))
        .digest("hex"),
      fixtureSha256: createHash("sha256")
        .update(await readFile(join(root, "tools/cloudflare/fixtures/runtime-driver.mjs")))
        .digest("hex"),
      workerdSha256: createHash("sha256")
        .update(await readFile(workerdBinary))
        .digest("hex"),
      ...(selection
        ? {
            selection: selection.name,
            caseCount: cases.length,
            selectedCases: selection.expectedCases,
            fixtureInputSha256: await fixtureInputHashes(root),
          }
        : {}),
      cases,
    })
  );
}
