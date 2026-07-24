import assert from "node:assert/strict";
import type { ApiKeyHashRecord, ApiKeyRecord, PaidFallbackRequestV3, PaidFallbackWindowV3 } from "../src/types.ts";
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

Deno.test({
  name: "100 concurrent real HTTP 429 failovers settle exactly once without lost ledger rows",
  ignore: loopbackPermission.state !== "granted" || typeof Deno.openKv !== "function",
  sanitizeResources: false,
  sanitizeOps: false,
  async fn() {
    const kv = await Deno.openKv(":memory:");
    const originalFetch = globalThis.fetch;
    const originalApiKey = Deno.env.get("YUNWU_API_KEY");
    const originalInfo = console.info;
    const originalWarn = console.warn;
    const warnings: string[] = [];
    let providerServer: Deno.HttpServer | null = null;
    let gatewayServer: Deno.HttpServer | null = null;

    try {
      Deno.env.set("YUNWU_API_KEY", "yunwu-real-http-stress-key");
      const { setKvForTest } = await import("../src/kv.ts");
      setKvForTest(kv);
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
      const limitMicrocredits = 1_000_000;
      const reservationMicrocredits = 10_000;
      const quotaPerCredit = 500_000;
      const commonPolicy = {
        expires_at_ms: -1,
        revoked_at_ms: null,
        usage_limit_requests: -1,
        usage_requests: 0,
        usage_reset_at_ms: windowResetAtMs,
        window_ms: windowMs,
        paid_fallback_enabled: true,
        paid_fallback_limit_microcredits: limitMicrocredits,
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
        paid_fallback_max_exposure_microcredits: { [model]: reservationMicrocredits },
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
      let yunwuCalls = 0;
      let billingLogCalls = 0;
      providerServer = Deno.serve(
        { hostname: "127.0.0.1", port: 0, onListen: () => {} },
        async (request) => {
          const url = new URL(request.url);
          if (url.pathname === "/codex/responses") {
            codexCalls += 1;
            return Response.json(
              { error: { message: "Primary quota exhausted", type: "rate_limit_error", code: "rate_limit_exceeded" } },
              { status: 429, headers: { "Retry-After": "60" } },
            );
          }
          if (url.pathname === "/yunwu/responses") {
            yunwuCalls += 1;
            const body = await request.json() as {
              input?: string | Array<{ content?: Array<{ text?: unknown }> }>;
            };
            const sentinel = typeof body.input === "string"
              ? body.input
              : body.input?.flatMap((item) => item.content ?? [])
                .map((content) => typeof content.text === "string" ? content.text : "")
                .join("") ?? "";
            const providerRequestId = `yunwu-real-http-${yunwuCalls}`;
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
          }
          if (url.pathname === "/yunwu/log/token") {
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
        if (sourceUrl === "https://yunwu.ai/v1/responses") {
          return originalFetch(`${providerBaseUrl}/yunwu/responses`, init);
        }
        if (sourceUrl.startsWith("https://yunwu.ai/api/log/token?")) {
          return originalFetch(`${providerBaseUrl}/yunwu/log/token${new URL(sourceUrl).search}`, init);
        }
        return originalFetch(input, init);
      };

      const { default: handler } = await import("../src/handler.ts");
      const {
        paidFallbackWindowV3Key,
        reconcileDuePaidFallbacksV3,
      } = await import("../src/paid_fallback_ledger.ts");
      const requestPrefix = ["uos_ai", "paid_fallback", "v3", "request", keyId] as const;
      const pendingPrefix = ["uos_ai", "paid_fallback", "v3", "pending", keyId] as const;
      gatewayServer = Deno.serve(
        { hostname: "127.0.0.1", port: 0, onListen: () => {} },
        handler,
      );
      const gatewayAddress = gatewayServer.addr as Deno.NetAddr;
      const gatewayUrl = `http://127.0.0.1:${gatewayAddress.port}/v1/responses`;

      const results = await Promise.all(
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

      assert.deepEqual(results.map((result) => result.status), Array(100).fill(200));
      assert.deepEqual(results.map((result) => result.provider), Array(100).fill("yunwu"));
      const invalidResults = results.filter((result) => !result.completed || !result.exact || result.error !== null);
      assert.deepEqual(invalidResults, []);
      assert.equal(codexCalls, 100);
      assert.equal(yunwuCalls, 100);
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
      assert.equal(requests.every((entry) => entry.value.reconciliation_attempts === 1), true);
      assert.equal((await listEntries(kv, pendingPrefix)).length, 0);

      const window = await kv.get<PaidFallbackWindowV3>(
        paidFallbackWindowV3Key(keyId, windowResetAtMs),
        { consistency: "strong" },
      );
      assert.ok(window.value);
      assert.equal(window.value.settled_microcredits, 100_000);
      assert.equal(window.value.reserved_microcredits, 0);
      assert.equal(window.value.pending_count, 0);
      assert.equal(window.value.settled_microcredits >= 0, true);
      assert.equal(window.value.reserved_microcredits >= 0, true);

      assert.equal(await reconcileDuePaidFallbacksV3(Date.now() + 2_000, kv), 0);
      const replayedWindow = await kv.get<PaidFallbackWindowV3>(
        paidFallbackWindowV3Key(keyId, windowResetAtMs),
        { consistency: "strong" },
      );
      assert.equal(replayedWindow.value?.settled_microcredits, 100_000);
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
      globalThis.fetch = originalFetch;
      const { setKvForTest } = await import("../src/kv.ts");
      setKvForTest(null);
      console.info = originalInfo;
      console.warn = originalWarn;
      if (originalApiKey === undefined) Deno.env.delete("YUNWU_API_KEY");
      else Deno.env.set("YUNWU_API_KEY", originalApiKey);
      if (gatewayServer) await gatewayServer.shutdown();
      if (providerServer) await providerServer.shutdown();
      kv.close();
    }
  },
});
