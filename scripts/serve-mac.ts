// The launch agent starts this file from an immutable release with its own import map.
if (Deno.build.os !== "darwin") throw new Error("The Mac service requires macOS");
const release = new URL("../", import.meta.url);
const root = new URL("../../../", release);
const releasePath = await Deno.realPath(release);
for (const name of ["DENO_DEPLOY", "DENO_DEPLOYMENT_ID", "DENO_DEPLOY_BUILD_ID", "DENO_REGION", "DENO_TIMELINE"]) {
  Deno.env.delete(name);
}
// Scheduled production billing stays on the VPS. This companion owns only local requests.
const { config, runtimeGitSha } = await import(new URL("src/config.ts", release).href);
const gitSha = runtimeGitSha();
if (!/^[0-9a-f]{40}$/.test(gitSha) || !releasePath.endsWith(`/releases/${gitSha}`)) {
  throw new Error("The immutable Mac release does not match its Git identity");
}
if (!config.adminTokens.size || !config.authTokens.size) {
  throw new Error("The repository-root .env must contain the existing admin and client API tokens");
}
Deno.chdir(root);
const kv = await Deno.openKv(new URL(".data/kv.sqlite3", root).pathname);
const { initializeKv } = await import(new URL("src/kv.ts", release).href);
initializeKv(kv);
const { default: handler } = await import(new URL("serve.ts", release).href) as typeof import("../serve.ts");
const server = Deno.serve({ hostname: "127.0.0.1", port: 8000 }, handler.fetch);
let stopping = false;
const shutdown = () => {
  if (stopping) return;
  stopping = true;
  console.log("[ai.ubq.fi] Draining local requests before shutdown");
  void server.shutdown();
};
Deno.addSignalListener("SIGTERM", shutdown);
Deno.addSignalListener("SIGINT", shutdown);
console.log(`[ai.ubq.fi] Mac serving Git revision ${gitSha}`);
await server.finished;
kv.close();
Deno.exit(0);
