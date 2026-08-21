import assert from "node:assert/strict";
import {
  fetchMeteredResponses,
  fetchMeteredTokenLogs,
  initializeMeteredPricing,
  METERED_FETCH_TIMEOUT_MS,
  MeteredError,
  type MeteredFetch,
} from "../src/metered.ts";

const jsonResponse = (body: unknown, status = 200, headers: HeadersInit = {}): Response =>
  new Response(JSON.stringify(body), {
    status,
    headers: {
      "Content-Type": "application/json",
      ...headers,
    },
  });

Deno.test("initializeMeteredPricing intersects the current Codex catalog and returns a compact snapshot", async () => {
  const calls: Array<{ url: string; init?: RequestInit }> = [];
  const fetcher: MeteredFetch = (input, init) => {
    const url = input.toString();
    calls.push({ url, init });
    if (url === "https://api.openlux.ai/api/ratio_config") {
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
    if (url === "https://api.openlux.ai/api/status") {
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

  const snapshot = await initializeMeteredPricing({
    codexModelIds: ["gpt-fixed", "missing", "gpt-5.6-sol", "gpt-fixed"],
    fetcher,
    now: () => 1_234_567,
  });

  assert.deepEqual(snapshot, {
    eligible_model_ids: ["gpt-fixed", "gpt-5.6-sol"],
    quota_per_credit: 500_000,
    model_quota_coefficients: {
      "gpt-fixed": 0.25,
      "gpt-5.6-sol": 5,
    },
    checked_at_ms: 1_234_567,
  });
  assert.deepEqual(calls.map((call) => call.url), [
    "https://api.openlux.ai/api/ratio_config",
    "https://api.openlux.ai/api/status",
  ]);
  for (const call of calls) {
    assert.equal(call.init?.method, "GET");
    assert.equal(new Headers(call.init?.headers).get("Accept"), "application/json");
    assert.equal(new Headers(call.init?.headers).has("Authorization"), false);
  }
});

Deno.test("initializeMeteredPricing fails closed and never returns an earlier snapshot", async () => {
  let statusIsValid = true;
  const fetcher: MeteredFetch = (input) => {
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

  const first = await initializeMeteredPricing({
    codexModelIds: ["gpt-5.6-sol"],
    fetcher,
  });
  assert.deepEqual(first.eligible_model_ids, ["gpt-5.6-sol"]);

  statusIsValid = false;
  await assert.rejects(
    () =>
      initializeMeteredPricing({
        codexModelIds: ["gpt-5.6-sol"],
        fetcher,
      }),
    (error: unknown) => error instanceof MeteredError && error.code === "metered_status_invalid",
  );
});

Deno.test("fetchMeteredResponses applies Metered Sol reasoning suffixes and forwards cancellation", async () => {
  const controller = new AbortController();
  const canonicalBody = {
    model: "gpt-5.6-sol",
    input: [{ role: "user", content: [{ type: "input_text", text: "hello" }] }],
    reasoning: { effort: "high" },
    stream: true,
  };
  const calls: Array<{ url: string; init?: RequestInit }> = [];
  const fetcher: MeteredFetch = (input, init) => {
    calls.push({ url: input.toString(), init });
    return Promise.resolve(
      new Response("rate limited", {
        status: 429,
        headers: {
          "X-Request-Id": " intermediary-request-1 ",
          "X-Oneapi-Request-Id": " metered-fallback-request-1 ",
          "X-Api-Request-Id": " metered-request-1 ",
        },
      }),
    );
  };

  const result = await fetchMeteredResponses(canonicalBody, {
    apiKey: "test-metered-key",
    fetcher,
    signal: controller.signal,
  });

  assert.equal(calls.length, 1);
  assert.equal(calls[0].url, "https://api.openlux.ai/v1/responses");
  assert.equal(calls[0].init?.method, "POST");
  // The request signal also carries the provider header deadline, so it is a
  // composed signal rather than the caller's signal by reference.
  assert.ok(calls[0].init?.signal);
  assert.equal(calls[0].init?.signal.aborted, false);
  assert.deepEqual(JSON.parse(String(calls[0].init?.body)), {
    model: "gpt-5.6-sol-high",
    input: canonicalBody.input,
    stream: true,
  });
  const headers = new Headers(calls[0].init?.headers);
  assert.equal(headers.get("Authorization"), "Bearer test-metered-key");
  assert.equal(headers.get("Accept"), "text/event-stream");
  assert.equal(headers.get("Content-Type"), "application/json");
  assert.equal(result.response.status, 429);
  assert.equal(result.request_id, "metered-request-1");
});

Deno.test("fetchMeteredResponses maps no-reasoning and ultra Sol presets to live aliases", async () => {
  const bodies: Record<string, unknown>[] = [];
  const fetcher: MeteredFetch = (_input, init) => {
    bodies.push(JSON.parse(String(init?.body)) as Record<string, unknown>);
    return Promise.resolve(new Response("{}", { status: 200 }));
  };

  await fetchMeteredResponses(
    { model: "gpt-5.6-sol", input: "hello", reasoning: { effort: "minimal" } },
    { apiKey: "test-metered-key", fetcher },
  );
  await fetchMeteredResponses(
    { model: "gpt-5.6-sol", input: "hello", reasoning: { effort: "ultra" } },
    { apiKey: "test-metered-key", fetcher },
  );

  assert.deepEqual(bodies, [
    { model: "gpt-5.6-sol-low", input: "hello" },
    { model: "gpt-5.6-sol-max", input: "hello" },
  ]);
});

Deno.test("fetchMeteredResponses propagates client cancellation through the header deadline signal", async () => {
  const controller = new AbortController();
  const observed = { signal: null as AbortSignal | null };
  const fetcher: MeteredFetch = (_input, init) => {
    observed.signal = init?.signal ?? null;
    return new Promise<Response>((_resolve, reject) => {
      observed.signal?.addEventListener(
        "abort",
        () => reject(observed.signal?.reason ?? new DOMException("Aborted", "AbortError")),
        { once: true },
      );
    });
  };

  const pending = fetchMeteredResponses(
    { model: "gpt-5.6-sol", input: "hello", stream: true },
    {
      apiKey: "test-metered-key",
      fetcher,
      signal: controller.signal,
    },
  );
  const cancellation = new DOMException("Client disconnected", "AbortError");
  controller.abort(cancellation);

  await assert.rejects(
    pending,
    (error: unknown) => error === cancellation,
  );
  const observedSignal = observed.signal;
  assert.ok(observedSignal);
  assert.equal(observedSignal.aborted, true);
  assert.equal(observedSignal.reason, cancellation);
});

Deno.test("fetchMeteredTokenLogs returns only strict allowlisted billing fields", async () => {
  let capturedUrl = "";
  let capturedInit: RequestInit | undefined;
  const fetcher: MeteredFetch = (input, init) => {
    capturedUrl = input.toString();
    capturedInit = init;
    return Promise.resolve(jsonResponse({
      success: true,
      message: "",
      data: {
        total: 2,
        page: 1,
        page_size: 100,
        items: [
          {
            id: 9001,
            quota: 2914,
            prompt_tokens: 71,
            completion_tokens: 231,
            model_name: "gpt-5.6-sol",
            created_at: 1_752_960_000,
            username: "must-not-leak",
            token_name: "must-not-leak",
            ip: "must-not-leak",
            other: '{"request_id":"request-abc","must":"not leak"}',
          },
          {
            other: '{"request_id":"malformed-entry"}',
            quota: "2914",
            prompt_tokens: 71,
            completion_tokens: 231,
            model_name: "gpt-5.6-sol",
            created_at: 1_752_960_000,
          },
        ],
      },
    }));
  };

  const logs = await fetchMeteredTokenLogs({
    apiKey: "test-metered-key",
    fetcher,
  });

  const captured = new URL(capturedUrl);
  assert.equal(captured.origin + captured.pathname, "https://api.openlux.ai/api/log/token");
  assert.equal(captured.searchParams.get("key"), "test-metered-key");
  assert.equal(captured.searchParams.get("page"), "1");
  assert.equal(captured.searchParams.get("page_size"), "100");
  assert.equal(capturedInit?.method, "GET");
  const headers = new Headers(capturedInit?.headers);
  assert.equal(headers.get("Authorization"), null);
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

Deno.test("fetchMeteredTokenLogs aborts a stalled provider fetch at the bounded timeout", async () => {
  const originalTimeout = AbortSignal.timeout;
  const timeoutController = new AbortController();
  let observedSignal: AbortSignal | null = null;
  (AbortSignal as typeof AbortSignal & {
    timeout: (milliseconds: number) => AbortSignal;
  }).timeout = (milliseconds: number) => {
    assert.equal(milliseconds, METERED_FETCH_TIMEOUT_MS);
    return timeoutController.signal;
  };
  const fetcher: MeteredFetch = (_input, init) => {
    observedSignal = init?.signal ?? null;
    return new Promise<Response>((_resolve, reject) => {
      observedSignal?.addEventListener(
        "abort",
        () => reject(observedSignal?.reason ?? new DOMException("Timed out", "AbortError")),
        { once: true },
      );
    });
  };
  try {
    const pending = fetchMeteredTokenLogs({ apiKey: "test-metered-key", fetcher });
    assert.equal(observedSignal, timeoutController.signal);
    timeoutController.abort(new DOMException("Billing log timeout", "TimeoutError"));
    await assert.rejects(
      pending,
      (error: unknown) => error instanceof DOMException && error.name === "TimeoutError",
    );
  } finally {
    (AbortSignal as typeof AbortSignal & {
      timeout: (milliseconds: number) => AbortSignal;
    }).timeout = originalTimeout;
  }
});
