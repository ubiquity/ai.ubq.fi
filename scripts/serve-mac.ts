// The launch agent starts this file from an immutable release with its own import map.
if (Deno.build.os !== "darwin") throw new Error("The Mac service requires macOS");
const release = new URL("../", import.meta.url);
const root = new URL("../../../", release);
const releasePath = await Deno.realPath(release);
for (const name of ["DENO_DEPLOY", "DENO_DEPLOYMENT_ID", "DENO_DEPLOY_BUILD_ID", "DENO_REGION", "DENO_TIMELINE"]) {
  Deno.env.delete(name);
}
// Scheduled production billing stays on the VPS. Local capacity sampling uses this Mac's KV.
const { runtimeGitSha } = await import(new URL("src/config.ts", release).href);
const gitSha = runtimeGitSha();
if (!/^[0-9a-f]{40}$/.test(gitSha) || !releasePath.endsWith(`/releases/${gitSha}`)) {
  throw new Error("The immutable Mac release does not match its Git identity");
}
Deno.chdir(root);
const kv = await Deno.openKv(new URL(".data/kv.sqlite3", root).pathname);
const { initializeKv } = await import(new URL("src/kv.ts", release).href);
initializeKv(kv);
const { default: handler, shutdownOptionalTelemetry } = (await import(new URL("serve.ts", release).href)) as typeof import("../serve.ts");
const { configureAdminAuthPeerForRequest, configureMacLocalAdminAuthBypassForListener } = await import(new URL("src/local_admin_auth.ts", release).href);
// The Mac service answers LAN clients, so it provisions the unlimited local
// development key that loopback inference authenticates as; LAN clients keep
// authenticating with their own credentials.
const { ensureLocalDevelopmentApiKey } = await import(new URL("src/local_development_key.ts", release).href);
try {
  const status = await ensureLocalDevelopmentApiKey(kv);
  if (status === "created") console.log("[ai.ubq.fi] Provisioned the local development API key for loopback inference.");
  else if (status === "revoked") console.warn("[ai.ubq.fi] The local development API key is revoked; local paid-provider routing stays off.");
} catch (error) {
  console.warn("[ai.ubq.fi] Local development key provisioning failed:", error instanceof Error ? error.message : String(error));
}
const server = Deno.serve(
  {
    hostname: "0.0.0.0",
    port: 7999,
    onListen(address) {
      // LAN-facing service: only an actual loopback peer receives the
      // passwordless local development bypass; LAN clients stay authenticated.
      configureMacLocalAdminAuthBypassForListener(address);
    },
  },
  (request, info) => {
    configureAdminAuthPeerForRequest(info.remoteAddr, request);
    return handler.fetch(request, info);
  }
);
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
// The bounded optional-analytics drain runs after in-flight work settles and
// before the KV handle closes, so a stalled optional write cannot outlive it.
await shutdownOptionalTelemetry();
kv.close();
Deno.exit(0);
