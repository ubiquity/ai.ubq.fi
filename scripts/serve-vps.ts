// Keep the database and secrets in the repository root across code updates.
const root = new URL("../", import.meta.url);
Deno.chdir(root);
// Resolve the symlink once: static assets and lazy imports must stay on this release.
const releasePath = await Deno.realPath(new URL(".data/current", root));
const release = new URL(`file://${releasePath}/`);
const { config, runtimeGitSha } = await import(new URL("src/config.ts", release).href);
const { initializeKv } = await import(new URL("src/kv.ts", release).href);
if (config.isDeploy) throw new Error("The VPS entrypoint cannot run in Deno Deploy");
if (!config.adminTokens.size) throw new Error("DENO_DEPLOY_TOKEN must configure administrator authentication");
if (Deno.env.get("DENO_TIMELINE") !== "production") throw new Error("The VPS service requires the production timeline");

const gitSha = runtimeGitSha();
if (!/^[0-9a-f]{40}$/.test(gitSha) || !releasePath.endsWith(`/releases/${gitSha}`)) {
  throw new Error("The immutable release does not match its Git identity");
}

const database = new URL(".data/kv.sqlite3", root);
if (!(await Deno.stat(database)).isFile) {
  throw new Error("Migrate the production KV database before starting the service");
}
const kv = await Deno.openKv(database.pathname);
initializeKv(kv);

const { default: handler } = await import(new URL("serve.ts", release).href);
const server = Deno.serve({ hostname: "127.0.0.1", port: 8001, ...handler });
let stopping = false;
const shutdown = () => {
  if (stopping) return;
  stopping = true;
  console.log("[ai.ubq.fi] Draining requests before shutdown");
  void server.shutdown();
};
Deno.addSignalListener("SIGTERM", shutdown);
Deno.addSignalListener("SIGINT", shutdown);
console.log(`[ai.ubq.fi] VPS serving Git revision ${gitSha}`);
await server.finished;
kv.close();
Deno.exit(0);
