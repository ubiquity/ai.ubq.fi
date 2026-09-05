import assert from "node:assert/strict";
import { PAID_FALLBACK_NO_LIMIT } from "../src/api_keys.ts";
import type { ApiKeyHashRecord, ApiKeyRecord, PaidFallbackRequestV3 } from "../src/types.ts";
import { sha256Base64Url } from "../src/utils.ts";

const loopbackPermission = await Deno.permissions.query({ name: "net", host: "127.0.0.1" });

const outputText = (payload: Record<string, unknown>): string => {
  if (!Array.isArray(payload.output)) return "";
  return payload.output.flatMap((item) => {
    if (!item || typeof item !== "object" || !Array.isArray((item as { content?: unknown }).content)) return [];
    return (item as { content: Array<{ text?: unknown }> }).content.map((content) =>
      typeof content.text === "string" ? content.text : ""
    );
  }).join("");
};

const listEntries = async <T>(kv: Deno.Kv, prefix: Deno.KvKey): Promise<Deno.KvEntry<T>[]> => {
  const entries: Deno.KvEntry<T>[] = [];
  for await (const entry of kv.list<T>({ prefix }, { consistency: "strong" })) entries.push(entry);
  return entries;
};

const awaitWithin = async (promise: Promise<void>, milliseconds: number, message: () => string): Promise<void> => {
  let timeout: ReturnType<typeof setTimeout> | undefined;
  try {
    await Promise.race([
      promise,
      new Promise<never>((_, reject) => {
        timeout = setTimeout(() => reject(new Error(message())), milliseconds);
      }),
    ]);
  } finally {
    if (timeout !== undefined) clearTimeout(timeout);
  }
};

Deno.test({
  name: "100 concurrent real HTTP 429 failovers reach unlimited Metered together and settle exactly once",
  ignore: loopbackPermission.state !== "granted" || typeof Deno.openKv !== "function",
  sanitizeResources: false,
  sanitizeOps: false,
  async fn() {
    const kv = await Deno.openKv(":memory:");
    const originalFetch = globalThis.fetch;
    const originalApiKey = Deno.env.get("METERED_API_KEY");
    const originalSurplusApiKey = Deno.env.get("SURPLUS_API_KEY");
    const originalInfo = console.info;
    const originalWarn = console.warn;
    const warnings: string[] = [];
    let originalDeployFlag: boolean | null = null;
    let providerServer: Deno.HttpServer | null = null;
    let gatewayServer: Deno.HttpServer | null = null;
    let releaseMeteredResponses = (): void => {};

    try {
      Deno.env.set("METERED_API_KEY", "metered-real-http-stress-key");
      Deno.env.delete("SURPLUS_API_KEY");
      const { setKvForTest } = await import("../src/kv.ts");
      const {
        fetchMeteredModels,
        resetMeteredModelsCacheForTest,
        setMeteredModelsFetchForTest,
      } = await import("../src/metered.ts");
      const { config } = await import("../src/config.ts");
      setKvForTest(kv);
      originalDeployFlag = config.isDeploy;
      resetMeteredModelsCacheForTest();
      setMeteredModelsFetchForTest(() =>
        Promise.resolve(Response.json({
          data: [{
            id: "gpt-5.6-sol",
            owned_by: "openlux",
            supported_endpoint_types: ["openai-response"],
          }],
        }))
      );
      assert.deepEqual((await fetchMeteredModels({ force: true }))?.models.map((entry) => entry.id), [
        "gpt-5.6-sol",
      ]);
      console.info = () => {};
      console.warn = (...args: unknown[]) => warnings.push(args.map(String).join(" "));

      const keyId = "real-http-stress-key";
      const token = `u_${"a".repeat(64)}`;
      const tokenHash = await sha256Base64Url(token);
      const model = "gpt-5.6-sol";
      const now = Date.now();
      const windowMs = 60 * 60_000;
      const windowResetAtMs = now + windowMs;
      const pricingCheckedAtMs = now;
      const quotaPerCredit = 500_000;
      const commonPolicy = {
        expires_at_ms: -1,
        revoked_at_ms: null,
        usage_limit_requests: -1,
        usage_requests: 0,
        usage_reset_at_ms: windowResetAtMs,
        window_ms: windowMs,
        usage_quota_version: 3,
        paid_fallback_enabled: true,
        paid_fallback_limit_microcredits: PAID_FALLBACK_NO_LIMIT,
        paid_fallback_spent_microcredits: 0,
        paid_fallback_reserved_microcredits: 0,
        paid_fallback_reservation_request_id: null,
      } satisfies Omit<ApiKeyHashRecord, "id">;
      const keyRecord: ApiKeyRecord = {
        id: keyId,
        name: "Real HTTP stress key",
        prefix: token.slice(0, 10),
        hash: tokenHash,
        created_at_ms: now,
        ...commonPolicy,
        paid_fallback_model_ids: [model],
        paid_fallback_quota_per_credit: quotaPerCredit,
        paid_fallback_max_exposure_microcredits: {},
        paid_fallback_pricing_checked_at_ms: pricingCheckedAtMs,
      };
      await kv.set(["ubq_ai", "api_keys", "id", keyId], keyRecord);
      await kv.set(
        ["ubq_ai", "api_keys", "hash", tokenHash],
        {
          id: keyId,
          ...commonPolicy,
        } satisfies ApiKeyHashRecord,
      );
      await kv.set(["ubq_ai", "codex_auth"], {
        accounts: [{
          access_token: "real-http-access-token",
          refresh_token: "real-http-refresh-token",
          account_id: "real-http-account",
          updated_at_ms: now,
        }],
        updated_at_ms: now,
      });
      const catalog = {
        source: "codex_cli",
        client_version: "0.145.0",
        updated_at_ms: now,
        models: [{
          slug: model,
          context_window: 272_000,
          max_context_window: 1_000_000,
          auto_compact_token_limit: null,
          default_reasoning_level: "low",
          supported_reasoning_levels: ["none", "low", "medium", "high", "xhigh", "max", "ultra"],
          reasoning_effort_wire_map: { ultra: "max" },
        }],
      };
      await kv.set(["ubq_ai", "codex_models"], catalog);
      await kv.set(["uos_ai", "runtime_config", "v2"], {
        version: 2,
        default_model: model,
        default_reasoning_effort: "low",
        codex_models: catalog,
        updated_at_ms: now,
      });

      const providerLogs = new Map<string, {
        request_id: string;
        quota: number;
        prompt_tokens: number;
        completion_tokens: number;
        model_name: string;
        created_at: number;
      }>();
      let codexCalls = 0;
      let meteredCalls = 0;
      let meteredInFlight = 0;
      let maxMeteredInFlight = 0;
      let billingLogCalls = 0;
      let resolveAllMeteredDispatched = (): void => {};
      const allMeteredDispatched = new Promise<void>((resolve) => {
        resolveAllMeteredDispatched = resolve;
      });
      const meteredResponseBarrier = new Promise<void>((resolve) => {
        let released = false;
        releaseMeteredResponses = () => {
          if (released) return;
          released = true;
          resolve();
        };
      });
      const codexRetryAfter = new Date((Math.floor(Date.now() / 1_000) + 60) * 1_000).toUTCString();
      providerServer = Deno.serve(
        { hostname: "127.0.0.1", port: 0, onListen: () => {} },
        async (request) => {
          const url = new URL(request.url);
          if (url.pathname === "/codex/responses") {
            codexCalls += 1;
            return Response.json(
              { error: { message: "Primary quota exhausted", type: "usage_limit_reached" } },
              { status: 429, headers: { "Retry-After": codexRetryAfter } },
            );
          }
          if (url.pathname === "/metered/responses") {
            const callNumber = ++meteredCalls;
            if (callNumber > 100) throw new Error(`Unexpected Metered dispatch ${callNumber}`);
            meteredInFlight += 1;
            maxMeteredInFlight = Math.max(maxMeteredInFlight, meteredInFlight);
            if (callNumber === 100) resolveAllMeteredDispatched();
            try {
              const body = await request.json() as {
                input?: string | Array<{ content?: Array<{ text?: unknown }> }>;
              };
              const sentinel = typeof body.input === "string"
                ? body.input
                : body.input?.flatMap((item) => item.content ?? [])
                  .map((content) => typeof content.text === "string" ? content.text : "")
                  .join("") ?? "";
              await meteredResponseBarrier;
              const providerRequestId = `metered-real-http-${callNumber}`;
              providerLogs.set(providerRequestId, {
                request_id: providerRequestId,
                quota: 500,
                prompt_tokens: 2,
                completion_tokens: 1,
                model_name: model,
                created_at: Math.trunc(Date.now() / 1_000),
              });
              const responseId = `resp_${providerRequestId}`;
              const completed = {
                id: responseId,
                object: "response",
                created_at: Math.trunc(Date.now() / 1_000),
                status: "completed",
                model,
                output: [{
                  type: "message",
                  id: `msg_${providerRequestId}`,
                  status: "completed",
                  role: "assistant",
                  content: [{ type: "output_text", text: sentinel, annotations: [] }],
                }],
                usage: { input_tokens: 2, output_tokens: 1, total_tokens: 3 },
              };
              const sse = [
                `data: ${JSON.stringify({ type: "response.created", response: { id: responseId } })}\n\n`,
                `data: ${JSON.stringify({ type: "response.output_text.delta", delta: sentinel })}\n\n`,
                `data: ${JSON.stringify({ type: "response.completed", response: completed })}\n\n`,
              ].join("");
              return new Response(sse, {
                headers: {
                  "Content-Type": "text/event-stream",
                  "X-Oneapi-Request-Id": providerRequestId,
                },
              });
            } finally {
              meteredInFlight -= 1;
            }
          }
          if (url.pathname === "/metered/log/token") {
            billingLogCalls += 1;
            return Response.json({
              success: true,
              data: {
                items: [...providerLogs.values()],
                total: providerLogs.size,
              },
            });
          }
          return new Response("not found", { status: 404 });
        },
      );
      const providerAddress = providerServer.addr as Deno.NetAddr;
      const providerBaseUrl = `http://127.0.0.1:${providerAddress.port}`;
      globalThis.fetch = (input: RequestInfo | URL, init?: RequestInit): Promise<Response> => {
        const sourceUrl = typeof input === "string" ? input : input instanceof URL ? input.toString() : input.url;
        if (sourceUrl === "https://chatgpt.com/backend-api/codex/responses") {
          return originalFetch(`${providerBaseUrl}/codex/responses`, init);
        }
        if (sourceUrl === "https://api.openlux.ai/v1/responses") {
          return originalFetch(`${providerBaseUrl}/metered/responses`, init);
        }
        if (sourceUrl.startsWith("https://api.openlux.ai/api/log/token?")) {
          return originalFetch(`${providerBaseUrl}/metered/log/token${new URL(sourceUrl).search}`, init);
        }
        return originalFetch(input, init);
      };

      const { default: handler } = await import("../src/handler.ts");
      const { createServeHandler } = await import("../src/serve_handler.ts");
      (config as { isDeploy: boolean }).isDeploy = true;
      const {
        paidFallbackWindowV3Key,
        reconcileDuePaidFallbacksV3,
      } = await import("../src/paid_fallback_ledger.ts");
      const requestPrefix = ["uos_ai", "paid_fallback", "v3", "request", keyId] as const;
      const pendingPrefix = ["uos_ai", "paid_fallback", "v3", "pending", keyId] as const;
      gatewayServer = Deno.serve(
        { hostname: "127.0.0.1", port: 0, onListen: () => {} },
        createServeHandler(handler),
      );
      const gatewayAddress = gatewayServer.addr as Deno.NetAddr;
      const gatewayUrl = `http://127.0.0.1:${gatewayAddress.port}/v1/responses`;

      const pendingResults = Promise.all(
        Array.from({ length: 100 }, async (_, index) => {
          const sentinel = `UOS_REAL_HTTP_FAILOVER_${index}`;
          const response = await fetch(gatewayUrl, {
            method: "POST",
            headers: {
              "Authorization": `Bearer ${token}`,
              "Content-Type": "application/json",
            },
            body: JSON.stringify({
              model,
              input: sentinel,
              reasoning: { effort: "low" },
              client_metadata: { acceptance: "real-http-stress" },
              stream: false,
            }),
          });
          const payload = await response.json() as Record<string, unknown>;
          return {
            status: response.status,
            provider: response.headers.get("x-uos-upstream"),
            completed: payload.status === "completed",
            exact: outputText(payload) === sentinel,
            error: payload.error ?? null,
          };
        }),
      );
      let dispatchBarrierError: unknown = null;
      try {
        await awaitWithin(
          allMeteredDispatched,
          15_000,
          () => `Only ${meteredCalls}/100 Metered requests dispatched before the concurrency deadline`,
        );
        assert.equal(meteredCalls, 100);
        assert.equal(meteredInFlight, 100);
        assert.equal(maxMeteredInFlight, 100);
        assert.equal(codexCalls, 100);
      } catch (error) {
        dispatchBarrierError = error;
      } finally {
        releaseMeteredResponses();
      }
      const results = await pendingResults;
      if (dispatchBarrierError) {
        throw new Error(
          `${dispatchBarrierError instanceof Error ? dispatchBarrierError.message : String(dispatchBarrierError)}; ` +
            `Codex calls: ${codexCalls}; first results: ${JSON.stringify(results.slice(0, 3))}; ` +
            `warnings: ${JSON.stringify(warnings.slice(0, 5))}`,
          { cause: dispatchBarrierError },
        );
      }

      assert.deepEqual(results.map((result) => result.status), Array(100).fill(200));
      assert.deepEqual(results.map((result) => result.provider), Array(100).fill("metered"));
      const invalidResults = results.filter((result) => !result.completed || !result.exact || result.error !== null);
      assert.deepEqual(invalidResults, []);
      assert.equal(codexCalls, 100);
      assert.equal(meteredCalls, 100);
      assert.equal(meteredInFlight, 0);
      assert.equal(maxMeteredInFlight, 100);
      assert.equal(providerLogs.size, 100);

      let requests: Deno.KvEntry<PaidFallbackRequestV3>[] = [];
      for (let attempt = 0; attempt < 100; attempt += 1) {
        requests = await listEntries(kv, requestPrefix);
        if (
          requests.length === 100 &&
          requests.every((entry) =>
            entry.value.terminal_state === "completed" &&
            entry.value.dispatch_state === "dispatched" &&
            entry.value.provider_request_id !== null
          )
        ) break;
        await new Promise((resolve) => setTimeout(resolve, 10));
      }
      assert.equal(requests.length, 100);
      assert.equal(new Set(requests.map((entry) => entry.value.provider_request_id)).size, 100);
      assert.equal(requests.every((entry) => entry.value.terminal_state === "completed"), true);

      const settled = await reconcileDuePaidFallbacksV3(Date.now() + 1_000, kv);
      assert.equal(settled, 100);
      assert.equal(billingLogCalls, 1);
      requests = await listEntries(kv, requestPrefix);
      assert.equal(requests.length, 100);
      assert.equal(requests.every((entry) => entry.value.billing_state === "settled"), true);
      assert.equal(requests.every((entry) => entry.value.spend_microcredits === 1_000), true);
      assert.equal(requests.every((entry) => entry.value.reserved_microcredits === 0), true);
      assert.equal(requests.every((entry) => entry.value.reconciliation_attempts === 1), true);
      assert.equal(
        requests.reduce((total, entry) => total + (entry.value.spend_microcredits ?? 0), 0),
        100_000,
      );
      assert.equal((await listEntries(kv, pendingPrefix)).length, 0);

      const window = await kv.get(
        paidFallbackWindowV3Key(keyId, windowResetAtMs),
        { consistency: "strong" },
      );
      assert.equal(window.value, null);

      assert.equal(await reconcileDuePaidFallbacksV3(Date.now() + 2_000, kv), 0);
      const replayedRequests = await listEntries<PaidFallbackRequestV3>(kv, requestPrefix);
      assert.equal(replayedRequests.length, 100);
      assert.equal(replayedRequests.every((entry) => entry.value.billing_state === "settled"), true);
      assert.equal(replayedRequests.every((entry) => entry.value.spend_microcredits === 1_000), true);
      assert.equal(replayedRequests.every((entry) => entry.value.reconciliation_attempts === 1), true);
      assert.equal(billingLogCalls, 1);
      assert.equal(
        warnings.some((warning) =>
          warning.includes("Paid fallback policy changed concurrently") ||
          warning.includes("Paid fallback request changed concurrently") ||
          warning.includes("quota_accounting_failed")
        ),
        false,
        warnings.join("\n"),
      );
    } finally {
      releaseMeteredResponses();
      globalThis.fetch = originalFetch;
      const { setKvForTest } = await import("../src/kv.ts");
      const {
        resetMeteredModelsCacheForTest,
        setMeteredModelsFetchForTest,
      } = await import("../src/metered.ts");
      const { config } = await import("../src/config.ts");
      setKvForTest(null);
      setMeteredModelsFetchForTest(null);
      resetMeteredModelsCacheForTest();
      if (originalDeployFlag !== null) (config as { isDeploy: boolean }).isDeploy = originalDeployFlag;
      console.info = originalInfo;
      console.warn = originalWarn;
      if (originalApiKey === undefined) Deno.env.delete("METERED_API_KEY");
      else Deno.env.set("METERED_API_KEY", originalApiKey);
      if (originalSurplusApiKey === undefined) Deno.env.delete("SURPLUS_API_KEY");
      else Deno.env.set("SURPLUS_API_KEY", originalSurplusApiKey);
      if (gatewayServer) await gatewayServer.shutdown();
      if (providerServer) await providerServer.shutdown();
      kv.close();
    }
  },
});
