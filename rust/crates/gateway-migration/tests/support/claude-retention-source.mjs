// Synthetic source-writer oracle only. No native store, real credential or provider request.
import assert from "node:assert/strict";
import { createHash } from "node:crypto";
import { readFileSync } from "node:fs";
import { SentinelKvStub } from "../../../../../tests/helpers/sentinel-kv-stub.ts";
import { storeClaudeCodeStoredCredential } from "../../../../../src/provider/claude-code-credentials.ts";
import { resolveClaudeCodeAccount } from "../../../../../src/provider/claude-code-accounts.ts";
import { CLAUDE_CODE_USAGE_URL, parseClaudeCodeUsagePayload } from "../../../../../src/provider/claude-code-usage.ts";
import { recordClaudeCodeQuotaOutcome, registerClaudeCodeObservationRecorder } from "../../../../../src/provider/claude-code-quota.ts";
import { refreshClaudeCodeUsage } from "../../../../../src/provider/claude-code-usage-refresh.ts";
import { recordClaudeCapacityObservation } from "../../../../../src/provider/claude-capacity-history.ts";

assert.equal(process.argv.length, 2, "The source fixture takes no arguments");
const at = 2200;
const kv = new SentinelKvStub();
const expiry = new Map();
const originalAtomic = kv.atomic.bind(kv);
kv.atomic = () => {
  const operation = originalAtomic();
  const changes = [];
  const observed = new Proxy(operation, {
    get(target, name) {
      const method = Reflect.get(target, name);
      if (typeof method !== "function") return method;
      return (...args) => {
        if (name === "set") changes.push([JSON.stringify(args[0]), args[2]?.expireIn === undefined ? null : at + args[2].expireIn]);
        if (name === "delete") changes.push([JSON.stringify(args[0]), null]);
        const result = Reflect.apply(method, target, args);
        if (name === "commit")
          return result.then((receipt) => {
            if (receipt.ok) for (const [key, absolute] of changes) expiry.set(key, absolute);
            return receipt;
          });
        return result === target ? observed : result;
      };
    },
  });
  return observed;
};
let sequence = 0;
let collecting = false;
let rows;
const originalNow = Date.now;
const originalUuid = crypto.randomUUID;
Date.now = () => at;
crypto.randomUUID = () => `00000000-0000-4000-8000-${String(++sequence).padStart(12, "0")}`;
const unregister = registerClaudeCodeObservationRecorder(async (store, event) => {
  await recordClaudeCapacityObservation(store, event);
  if (!collecting) return;
  rows = [...kv.storage.entries.values()]
    .filter(
      ({ key }) =>
        key[0] === "uos_ai" && (key[1] === "claude_code_usage" || key[1] === "claude_code_quota" || (key[1] === "provider_capacity" && key[2] === "claude"))
    )
    .map(({ key, value, versionstamp }) => ({ key, value, revision: BigInt(versionstamp).toString(), expires_at_ms: expiry.get(JSON.stringify(key)) ?? null }));
});
try {
  await storeClaudeCodeStoredCredential(kv, { secret: "sk-ant-oat-synthetic-migration-only" });
  const binding = await resolveClaudeCodeAccount(kv, () => null);
  assert(binding);
  const payload = {
    five_hour: { utilization: 42, resets_at: null },
    limits: [{ kind: "future\ud800", group: "weekly", percent: 121.5, resets_at: null, scope: { model: { display_name: "model\udfff" } } }],
  };
  const observation = parseClaudeCodeUsagePayload(payload, at - 1);
  assert(observation);
  await recordClaudeCodeQuotaOutcome(kv, { binding, observation, nowMs: at - 1 });
  collecting = true;
  const result = await refreshClaudeCodeUsage(kv, binding, {
    now: () => at,
    fetcher: (input, init) => {
      assert.equal(String(input), CLAUDE_CODE_USAGE_URL);
      assert.equal(init.method, "GET");
      return Promise.resolve(Response.json(payload));
    },
  });
  assert.equal(result.kind, "completed");
  assert(rows?.some(({ key }) => key[1] === "claude_code_usage"));
  assert(rows.some(({ key }) => key[1] === "claude_code_quota"));
  for (const kind of ["history", "rollup", "usage_rollup"]) assert(rows.some(({ key }) => key[4] === kind));
  const source_files = [
    "claude-code-accounts.ts",
    "claude-code-credentials.ts",
    "claude-code-usage.ts",
    "claude-code-usage-schema.ts",
    "claude-code-usage-refresh.ts",
    "claude-code-quota.ts",
    "claude-capacity-history.ts",
    "claude-capacity-rollups.ts",
  ].map((name) => {
    const bytes = readFileSync(new URL(`../../../../../src/provider/${name}`, import.meta.url));
    return { path: `src/provider/${name}`, sha256: createHash("sha256").update(bytes).digest("hex") };
  });
  process.stdout.write(
    JSON.stringify(
      {
        v: 1,
        source_sha: "b59243d674567d418d1410f8390fc7fe47a08c92",
        synthetic: true,
        fixture_clock_ms: at,
        expiry_contract: "fixed synthetic CAS commit clock plus the actual source expireIn; no native KV expiry claim",
        source_files,
        rows,
      },
      null,
      2
    ) + "\n"
  );
} finally {
  unregister();
  crypto.randomUUID = originalUuid;
  Date.now = originalNow;
  kv.close();
}
