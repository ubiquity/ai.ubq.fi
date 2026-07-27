import assert from "node:assert/strict";
import {
  fetchYunwuResponses,
  fetchYunwuTokenLogs,
  initializeYunwuPricing,
  YunwuError,
  type YunwuFetch,
} from "../src/yunwu.ts";

const jsonResponse = (body: unknown, status = 200, headers: HeadersInit = {}): Response =>
  new Response(JSON.stringify(body), {
    status,
    headers: {
      "Content-Type": "application/json",
      ...headers,
    },
  });

Deno.test("initializeYunwuPricing intersects the current Codex catalog and returns a compact snapshot", async () => {
  const calls: Array<{ url: string; init?: RequestInit }> = [];
  const fetcher: YunwuFetch = (input, init) => {
    const url = input.toString();
    calls.push({ url, init });
    if (url === "https://yunwu.ai/api/ratio_config") {
      return Promise.resolve(jsonResponse({
        success: true,
        message: "",
        data: {
          model_ratio: {
            "gpt-5.6-sol": 2.5,
            "not-in-codex": 1,
            "disabled-model": 0,
          },
          model_price: {
            "gpt-fixed": 0.25,
          },
        },
      }));
    }
    if (url === "https://yunwu.ai/api/status") {
      return Promise.resolve(jsonResponse({
        success: true,
        message: "",
        data: {
          setup: true,
          quota_per_unit: 500_000,
          server_name_en: "not retained",
        },
      }));
    }
    throw new Error(`Unexpected URL: ${url}`);
  };

  const snapshot = await initializeYunwuPricing({
    codexModelIds: ["gpt-fixed", "missing", "gpt-5.6-sol", "gpt-fixed", "disabled-model"],
    fetcher,
    now: () => 1_234_567,
  });

  assert.deepEqual(snapshot, {
    eligible_model_ids: ["gpt-fixed", "gpt-5.6-sol"],
    quota_per_credit: 500_000,
    checked_at_ms: 1_234_567,
  });
  assert.deepEqual(calls.map((call) => call.url), [
    "https://yunwu.ai/api/ratio_config",
    "https://yunwu.ai/api/status",
  ]);
  for (const call of calls) {
    assert.equal(call.init?.method, "GET");
    assert.equal(new Headers(call.init?.headers).get("Accept"), "application/json");
    assert.equal(new Headers(call.init?.headers).has("Authorization"), false);
  }
});

Deno.test("initializeYunwuPricing fails closed and never returns an earlier snapshot", async () => {
  let statusIsValid = true;
  const fetcher: YunwuFetch = (input) => {
    if (input.toString().endsWith("/api/ratio_config")) {
      return Promise.resolve(jsonResponse({
        success: true,
        data: {
          model_ratio: { "gpt-5.6-sol": 2.5 },
          model_price: {},
        },
      }));
    }
    return Promise.resolve(
      jsonResponse(
        statusIsValid
          ? { success: true, data: { setup: true, quota_per_unit: 500_000 } }
          : { success: true, data: { setup: true, quota_per_unit: "500000" } },
      ),
    );
  };

  const first = await initializeYunwuPricing({
    codexModelIds: ["gpt-5.6-sol"],
    fetcher,
  });
  assert.deepEqual(first.eligible_model_ids, ["gpt-5.6-sol"]);

  statusIsValid = false;
  await assert.rejects(
    () =>
      initializeYunwuPricing({
        codexModelIds: ["gpt-5.6-sol"],
        fetcher,
      }),
    (error: unknown) => error instanceof YunwuError && error.code === "yunwu_status_invalid",
  );
});

Deno.test("fetchYunwuResponses makes one canonical request and forwards headers and cancellation", async () => {
  const controller = new AbortController();
  const canonicalBody = {
    model: "gpt-5.6-sol",
    input: [{ role: "user", content: [{ type: "input_text", text: "hello" }] }],
    reasoning: { effort: "high" },
    stream: true,
  };
  const calls: Array<{ url: string; init?: RequestInit }> = [];
  const fetcher: YunwuFetch = (input, init) => {
    calls.push({ url: input.toString(), init });
    return Promise.resolve(
      new Response("rate limited", {
        status: 429,
        headers: { "X-Oneapi-Request-Id": " yunwu-request-1 " },
      }),
    );
  };

  const result = await fetchYunwuResponses(canonicalBody, {
    apiKey: "test-yunwu-key",
    fetcher,
    signal: controller.signal,
  });

  assert.equal(calls.length, 1);
  assert.equal(calls[0].url, "https://yunwu.ai/v1/responses");
  assert.equal(calls[0].init?.method, "POST");
  assert.equal(calls[0].init?.signal, controller.signal);
  assert.deepEqual(JSON.parse(String(calls[0].init?.body)), canonicalBody);
  const headers = new Headers(calls[0].init?.headers);
  assert.equal(headers.get("Authorization"), "Bearer test-yunwu-key");
  assert.equal(headers.get("Accept"), "text/event-stream");
  assert.equal(headers.get("Content-Type"), "application/json");
  assert.equal(result.response.status, 429);
  assert.equal(result.request_id, "yunwu-request-1");
});

Deno.test("fetchYunwuResponses propagates streaming cancellation", async () => {
  const controller = new AbortController();
  let observedSignal: AbortSignal | null = null;
  const fetcher: YunwuFetch = (_input, init) => {
    observedSignal = init?.signal ?? null;
    return new Promise<Response>((_resolve, reject) => {
      observedSignal?.addEventListener(
        "abort",
        () => reject(observedSignal?.reason ?? new DOMException("Aborted", "AbortError")),
        { once: true },
      );
    });
  };

  const pending = fetchYunwuResponses(
    { model: "gpt-5.6-sol", input: "hello", stream: true },
    {
      apiKey: "test-yunwu-key",
      fetcher,
      signal: controller.signal,
    },
  );
  controller.abort(new DOMException("Client disconnected", "AbortError"));

  await assert.rejects(
    pending,
    (error: unknown) => error instanceof DOMException && error.name === "AbortError",
  );
  assert.equal(observedSignal, controller.signal);
});

Deno.test("fetchYunwuTokenLogs returns only strict allowlisted billing fields", async () => {
  let capturedUrl = "";
  let capturedInit: RequestInit | undefined;
  const fetcher: YunwuFetch = (input, init) => {
    capturedUrl = input.toString();
    capturedInit = init;
    return Promise.resolve(jsonResponse({
      success: true,
      message: "",
      data: [
        {
          id: 9001,
          request_id: "request-abc",
          quota: 2914,
          prompt_tokens: 71,
          completion_tokens: 231,
          model_name: "gpt-5.6-sol",
          created_at: 1_752_960_000,
          username: "must-not-leak",
          token_name: "must-not-leak",
          ip: "must-not-leak",
          other: '{"must":"not leak"}',
        },
        {
          request_id: "malformed-entry",
          quota: "2914",
          prompt_tokens: 71,
          completion_tokens: 231,
          model_name: "gpt-5.6-sol",
          created_at: 1_752_960_000,
        },
      ],
    }));
  };

  const logs = await fetchYunwuTokenLogs({
    apiKey: "test-yunwu-key",
    fetcher,
  });

  assert.equal(capturedUrl, "https://yunwu.ai/api/log/token");
  assert.equal(capturedInit?.method, "GET");
  const headers = new Headers(capturedInit?.headers);
  assert.equal(headers.get("Authorization"), "Bearer test-yunwu-key");
  assert.equal(headers.get("Accept"), "application/json");
  assert.deepEqual(logs, [{
    request_id: "request-abc",
    quota: 2914,
    prompt_tokens: 71,
    completion_tokens: 231,
    model: "gpt-5.6-sol",
    created_at: 1_752_960_000,
  }]);
  assert.deepEqual(Object.keys(logs[0]), [
    "request_id",
    "quota",
    "prompt_tokens",
    "completion_tokens",
    "model",
    "created_at",
  ]);
});
