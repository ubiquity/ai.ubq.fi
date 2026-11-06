import { bindings, defineConfig, exports } from "cf/config";
import { workerTarget, WORKER_ACCOUNT_ID, WORKER_COMPATIBILITY_DATE, WORKER_COMPATIBILITY_FLAGS } from "./cloudflare-targets.ts";

// Candidate and production keep their existing direct-provider and Passport bindings.
// Native credentials stay with their owner; UOS_NATIVE_CARRIER remains out of band.
const inferenceBindings = {
  UOS_AI_TOKEN: bindings.secret(),
  DEEPSEEK_API_KEY: bindings.secret(),
  CEREBRAS_API_KEY: bindings.secret(),
  LITHOSAI_API_KEY: bindings.secret(),
  OPENROUTER_API_KEY: bindings.secret(),
  SURPLUS_API_KEY: bindings.secret(),
  METERED_API_KEY: bindings.secret(),
  CLAUDE_API_KEY: bindings.secret(),
  UOS_AI_PASSPORT_CLIENT_ID: bindings.secret(),
  UOS_AI_PASSPORT_ORIGIN: bindings.secret(),
  UOS_AI_PASSPORT_REDIRECT_URI: bindings.secret(),
};

export default defineConfig(({ mode }) => {
  const { worker: workerName } = workerTarget(mode);
  return {
    accountId: WORKER_ACCOUNT_ID,
    worker: {
      name: workerName,
      compatibilityDate: WORKER_COMPATIBILITY_DATE,
      compatibilityFlags: WORKER_COMPATIBILITY_FLAGS,
      entrypoint: "./rust/crates/gateway-worker/entrypoint.mjs",
      workersDev: true,
      previewUrls: false,
      exports: {
        GatewayAuthority: exports.durableObject({ storage: "sqlite" }),
        InitialControlInstaller: exports.worker(),
      },
      env: {
        AUTHORITY: bindings.durableObject({ worker: workerName, exportName: "GatewayAuthority" }),
        ASSETS: bindings.assets(),
        DENO_DEPLOY_TOKEN: bindings.secret(),
        ...(mode === "rehearsal-20261106" ? {} : inferenceBindings),
      },
      assets: { runWorkerFirst: true, notFoundHandling: "none" },
      observability: { enabled: true, traces: { enabled: true } },
    },
  };
});
