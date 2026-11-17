import { readFileSync, realpathSync, statSync } from "node:fs";
import { isAbsolute, join } from "node:path";

if (Bun.version !== "1.4.0") throw new Error("Production comparison requires pinned Bun 1.4.0");
const [manifestPath, bindingPath] = process.argv.slice(2);
if (!manifestPath || !bindingPath || !isAbsolute(manifestPath) || !isAbsolute(bindingPath))
  throw new Error("Explicit absolute manifest and native-KV binding required");
const manifest: unknown = JSON.parse(readFileSync(manifestPath, "utf8"));
const object = (value: unknown): value is Record<string, unknown> => value !== null && typeof value === "object" && !Array.isArray(value);
if (
  !object(manifest) ||
  !["uos-gateway-launch-v1", "uos-gateway-bun-source-fixture-v1"].includes(String(manifest.schema)) ||
  manifest.hostname !== "127.0.0.1" ||
  manifest.port !== 0 ||
  typeof manifest.stateRoot !== "string" ||
  !object(manifest.fixture) ||
  typeof manifest.fixture.syntheticOrigin !== "string"
) {
  throw new Error("Bun smoke needs the same private loopback launch manifest");
}
const origin = new URL(manifest.fixture.syntheticOrigin);
if (
  origin.protocol !== "http:" ||
  origin.hostname !== "127.0.0.1" ||
  !origin.port ||
  ["7999", "8000"].includes(origin.port) ||
  origin.pathname !== "/" ||
  origin.search ||
  origin.hash ||
  origin.username ||
  origin.password
) {
  throw new Error("Synthetic upstream must be an exact loopback origin");
}
if (realpathSync(manifest.stateRoot) !== manifest.stateRoot || (statSync(manifest.stateRoot).mode & 0o077) !== 0)
  throw new Error("State root must be private and canonical");
process.chdir(manifest.stateRoot);
const binding: unknown = JSON.parse(readFileSync(bindingPath, "utf8"));
if (
  !object(binding) ||
  typeof binding.artifactDirectory !== "string" ||
  typeof binding.expectedProvenanceSha256 !== "string" ||
  typeof binding.inputsManifest !== "string"
) {
  throw new Error("Genuine explicitly bound native-KV artifacts are required for the Bun comparison");
}
const { prepareNativeKvBackend } = await import("../../../../src/storage/native-addon.ts");
prepareNativeKvBackend({
  artifactDirectory: binding.artifactDirectory,
  expectedProvenanceSha256: binding.expectedProvenanceSha256,
  inputsManifest: binding.inputsManifest,
});
const { openKv } = await import("../../../../src/storage/kv.ts");
const database = await openKv(join(manifest.stateRoot, ".data/kv-bun.sqlite3"));
const { initializeKv } = await import("../../../../src/kv.ts");
initializeKv(database);
const { config } = await import("../../../../src/config.ts");
Object.assign(config, { codexBaseUrl: origin.origin + "/codex" });
const { setLiveUpstreamBasesForTest } = await import("../../../../src/live/upstream.ts");
setLiveUpstreamBasesForTest({ callsBaseUrl: origin.origin + "/codex", sidebandBaseUrl: origin.origin.replace("http:", "ws:") + "/live" });
const originalFetch = globalThis.fetch;
globalThis.fetch = Object.assign(
  async (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
    const text = input instanceof Request ? input.url : String(input);
    if (text === "https://api.deepseek.com/chat/completions") return originalFetch(origin.origin + "/chat/completions", init);
    const selected = new URL(text);
    if (selected.origin === origin.origin && ["/codex/models", "/codex/responses", "/codex/realtime/calls"].includes(selected.pathname))
      return originalFetch(input, init);
    throw new Error("Smoke fixture refused an unmapped provider destination");
  },
  { preconnect: originalFetch.preconnect }
);

// The source module's own import.meta.main remains false, so it cannot bind its development port.
const { application, configureLoopbackServeRuntime, shutdownOptionalTelemetry } = await import("../../../../serve.ts");
await configureLoopbackServeRuntime([]);
const { serveHttp } = await import("#http");
const server = serveHttp({ hostname: "127.0.0.1", port: 0, onListen: application.onListen, upgrade: application.upgrade }, application.fetch);
const shutdown = (): void => {
  void server.shutdown().catch((error: unknown) => {
    console.error(error);
    process.exitCode = 1;
  });
};
process.once("SIGINT", shutdown);
process.once("SIGTERM", shutdown);
try {
  await server.ready;
  await server.finished;
} finally {
  process.removeListener("SIGINT", shutdown);
  process.removeListener("SIGTERM", shutdown);
  try {
    await shutdownOptionalTelemetry();
  } finally {
    database.close();
    console.info("[production_smoke] kv_closed");
  }
}
process.exit(process.exitCode ?? 0);
