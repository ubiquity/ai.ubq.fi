import { mkdir, writeFile } from "node:fs/promises";
import { join } from "node:path";
import type { MatrixScenario } from "./contracts.ts";

export async function fixtureConfig(root: string, workspace: string, scenario?: MatrixScenario): Promise<string> {
  const application = join(root, "rust/crates", scenario ? "gateway-worker-fixture" : "gateway-worker", "build");
  let constructorScenario = scenario;
  if (scenario === "precommit") constructorScenario = "direct";
  if (scenario === "precommit-paid") constructorScenario = "paid";
  const scenarioBinding = constructorScenario ? `, (name = "UOS_MATRIX_SCENARIO", text = ${JSON.stringify(constructorScenario)})` : "";
  const deepseekBinding = scenario?.startsWith("precommit") ? "" : ', (name = "DEEPSEEK_API_KEY", text = "fixture-provider-secret")';
  const paidBindings =
    scenario === "paid" || scenario === "precommit-paid"
      ? ', (name = "SURPLUS_API_KEY", text = "fixture-matrix-surplus"), (name = "OPENLUX_API_KEY", text = "fixture-matrix-openlux-discovery"), (name = "METERED_API_KEY", text = "fixture-matrix-openlux-spend")'
      : "";
  const path = join(workspace, "runtime.capnp");
  const quoted = (value: string): string => JSON.stringify(value);
  await mkdir(join(workspace, "state"), { recursive: true });
  const config = `using Workerd = import ${quoted(join(root, "node_modules/workerd/workerd.capnp"))};
const config :Workerd.Config = (
  services = [
    (name = "internet", network = (allow = [])),
    (name = "state", disk = (path = ${quoted(join(workspace, "state"))}, writable = true)),
    (name = "assets", disk = (path = ${quoted(join(root, "static"))})),
    (name = "gateway", worker = (
      modules = [(name = "index.js", esModule = embed ${quoted(join(application, "index.js"))}), (name = "index_bg.wasm", wasm = embed ${quoted(join(application, "index_bg.wasm"))})],
      compatibilityDate = "2026-10-18", compatibilityFlags = ["enable_request_signal", "request_signal_passthrough"],
      globalOutbound = "driver",
      bindings = [(name = "AUTHORITY", durableObjectNamespace = (className = "GatewayAuthority")), (name = "ASSETS", service = "assets"), (name = "UOS_AI_TOKEN", text = "fixture-access"), (name = "DENO_DEPLOY_TOKEN", text = "fixture-admin"), (name = "LITHOSAI_API_KEY", text = "fixture-provider-secret")${deepseekBinding}, (name = "CEREBRAS_API_KEY", text = "fixture-provider-secret"), (name = "OPENROUTER_API_KEY", text = "fixture-provider-secret"), (name = "VOYAGEAI_API_KEY", text = "fixture-provider-secret"), (name = "SENTINEL_REPLAY_KEY", text = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"), (name = "IMAGE_BASE_MODEL", text = "gpt-matrix-test"), (name = "UOS_AI_PASSPORT_CLIENT_ID", text = "ai"), (name = "UOS_AI_PASSPORT_AUDIENCE", text = "ai"), (name = "UOS_AI_PASSPORT_ORIGIN", text = "https://ai.ubq.fi"), (name = "UOS_AI_PASSPORT_REDIRECT_URI", text = "https://ai.ubq.fi/auth/callback")${scenarioBinding}${paidBindings}],
      durableObjectNamespaces = [(className = "GatewayAuthority", uniqueKey = "uos-rust-runtime-conformance-v1", enableSql = true)],
      durableObjectStorage = (localDisk = "state")
    )),
    (name = "driver", worker = (
      modules = [(name = "driver.mjs", esModule = embed ${quoted(join(root, "tools/cloudflare/fixtures/runtime-driver.mjs"))}), (name = "provider-matrix.mjs", esModule = embed ${quoted(join(root, "tools/cloudflare/fixtures/provider-matrix.mjs"))})],
      compatibilityDate = "2026-10-18", compatibilityFlags = ["enable_request_signal", "request_signal_passthrough"],
      bindings = [(name = "GATEWAY", service = "gateway"), (name = "AUTHORITY", durableObjectNamespace = (className = "GatewayAuthority", serviceName = "gateway")), (name = "CONTROL", durableObjectNamespace = (className = "FixtureControl"))],
      durableObjectNamespaces = [(className = "FixtureControl", uniqueKey = "uos-rust-runtime-control-v1", enableSql = true)],
      durableObjectStorage = (localDisk = "state")
    ))
  ],
  sockets = [(name = "main", http = (), service = "driver"), (name = "public", http = (), service = "gateway")]
);`;
  await writeFile(path, config);
  return path;
}
