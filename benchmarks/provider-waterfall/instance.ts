// Benchmark-only gateway instance.
//
// Runs the immutable release selected by the main checkout's `.data/current`
// symlink on loopback port 7998 with an isolated Deno KV
// (`.data/benchmark/kv.sqlite3`, seeded from the production KV before first
// use). The benchmark runner narrows the provider selection on this instance
// only, so the production Mac gateway on port 7999 never sees benchmark
// routing changes. The instance reads the same root `.env` upstream
// credentials as production because the benchmark measures provider behavior,
// not credential isolation.
//
// Run from the goal lane (the trailing argument enables the loopback-only
// local development principal, exactly as a dev server does):
//   deno run --unstable-kv --allow-env --allow-net --allow-sys=hostname \
//     --allow-read=<repo-root>,<repo-root>/.codex --allow-write=<repo-root>/.data \
//     benchmarks/provider-waterfall/instance.ts --disable-admin-auth

const PORT = 7998;

const pathExists = (url: URL): boolean => {
  try {
    Deno.statSync(url);
    return true;
  } catch {
    return false;
  }
};

/** Walk up from this module until the checkout that owns `.data/current`. */
const findRepoRoot = (): URL => {
  let dir = new URL(".", import.meta.url);
  for (let depth = 0; depth < 8; depth += 1) {
    if (pathExists(new URL(".data/current", dir)) && pathExists(new URL("serve.ts", dir))) return dir;
    dir = new URL("../", dir);
  }
  throw new Error("Could not locate the ai.ubq.fi checkout root from the benchmark instance module");
};

const root = findRepoRoot();
const rootPath = Deno.realPathSync(root.pathname);
const releasePath = Deno.realPathSync(new URL(".data/current/", root).pathname);
console.log(`[benchmark] repo root ${rootPath}`);
console.log(`[benchmark] release ${releasePath}`);

for (const name of ["DENO_DEPLOY", "DENO_DEPLOYMENT_ID", "DENO_DEPLOY_BUILD_ID", "DENO_REGION", "DENO_TIMELINE"]) {
  Deno.env.delete(name);
}

const kvDir = `${rootPath}/.data/benchmark`;
Deno.mkdirSync(kvDir, { recursive: true });
const kv = await Deno.openKv(`${kvDir}/kv.sqlite3`);
const { initializeKv } = (await import(`file://${releasePath}/src/kv.ts`)) as Readonly<{ initializeKv: (kv: Deno.Kv) => void }>;
initializeKv(kv);

type ReleaseServe = Readonly<{
  default: Readonly<{
    fetch(request: Request, info: Deno.ServeHandlerInfo): Promise<Response>;
    onListen?: (address: Deno.Addr) => void;
  }>;
  shutdownOptionalTelemetry: () => Promise<void>;
  startMacMaintenance: (kv: Deno.Kv) => () => Promise<void>;
}>;

const serve = (await import(`file://${releasePath}/serve.ts`)) as ReleaseServe;
const stopMaintenance = serve.startMacMaintenance(kv);

const server = Deno.serve(
  {
    hostname: "127.0.0.1",
    port: PORT,
    onListen(address) {
      serve.default.onListen?.(address);
      console.log(`[benchmark] listening on ${address.hostname}:${address.port} (loopback only)`);
    },
  },
  serve.default.fetch
);

let stopping = false;
const shutdown = () => {
  if (stopping) return;
  stopping = true;
  console.log("[benchmark] draining before shutdown");
  void stopMaintenance();
  void server.shutdown();
};
Deno.addSignalListener("SIGTERM", shutdown);
Deno.addSignalListener("SIGINT", shutdown);
await server.finished;
await stopMaintenance();
await serve.shutdownOptionalTelemetry();
kv.close();
Deno.exit(0);
