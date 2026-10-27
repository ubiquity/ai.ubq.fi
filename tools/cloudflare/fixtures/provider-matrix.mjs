// Controlled upstream only. M05's driver invokes this before its existing handlers.
// No application route, authority proof, arbitrary endpoint or live credential is implemented here.
export const MATRIX_MODELS = Object.freeze({
  codex: "gpt-matrix-test",
  unavailable: "gpt-matrix-unavailable",
  claude: "claude-sonnet-4-5",
  lithos: "deepseek-ai/DeepSeek-V4.1-Flash",
  openrouter: "openai/gpt-matrix-test",
  flash: "deepseek-v4.1-flash",
  decision: "baseten/elephant-1:free",
});
export const MATRIX_KEYS = Object.freeze({
  surplus: "fixture-matrix-surplus",
  openlux_discovery: "fixture-matrix-openlux-discovery",
  openlux_spend: "fixture-matrix-openlux-spend",
  claude: "sk-ant-api03-fixture-matrix-key",
});

const destinations = new Map([
  ["chatgpt.com", { provider: "codex", paths: ["/backend-api/codex/responses", "/backend-api/codex/models"] }],
  ["api.anthropic.com", { provider: "claude", paths: ["/v1/messages", "/v1/models", "/v1/organizations/workspaces"] }],
  ["api.surplusintelligence.ai", { provider: "surplus", paths: ["/v1/responses", "/v1/chat/completions", "/v1/models"] }],
  ["api.openlux.ai", { provider: "openlux", paths: ["/v1/responses", "/v1/messages", "/v1/models", "/api/ratio_config", "/api/status", "/api/log/token"] }],
  ["api.lithosai.cloud", { provider: "lithos", paths: ["/v1/chat/completions", "/v1/models"] }],
  [
    "openrouter.ai",
    { provider: "openrouter", paths: ["/api/v1/responses", "/api/v1/chat/completions", "/api/v1/models", "/api/v1/systemone", "/api/alpha/decisions"] },
  ],
  ["api.deepseek.com", { provider: "deepseek", paths: ["/chat/completions"] }],
]);

const isObject = (value) => value !== null && typeof value === "object" && !Array.isArray(value);
const markerIn = (body) => JSON.stringify(body.input ?? body.messages ?? "").match(/fixture:matrix:[a-z0-9_-]+:[a-z0-9_-]+/u)?.[0] ?? null;
const part = (type, data) => new TextEncoder().encode(`${type ? `event: ${type}\n` : ""}data: ${JSON.stringify(data)}\n\n`);
const billingLogs = new Map();
const requestId = (provider, marker) => `matrix-${provider}-${marker.split(":").at(-1)}`;

function codexAccount(request) {
  const bearer = request.headers.get("authorization")?.replace(/^Bearer /u, "");
  try {
    const encoded = bearer?.split(".")[1];
    if (!encoded) return null;
    const claims = JSON.parse(atob(encoded.replaceAll("-", "+").replaceAll("_", "/")));
    const account = claims["https://api.openai.com/auth"]?.chatgpt_account_id;
    return claims.iss === "uos-runtime-matrix" && typeof account === "string" && account.startsWith("fixture-matrix-") ? account : null;
  } catch {
    return null;
  }
}

function credentialMatches(request, provider, discovery) {
  if (provider === "codex") return codexAccount(request) === request.headers.get("chatgpt-account-id") && codexAccount(request) !== null;
  if (provider === "claude") return request.headers.get("x-api-key") === MATRIX_KEYS.claude;
  if (provider === "surplus" && discovery) return !request.headers.has("authorization");
  if (provider === "surplus") return request.headers.get("authorization") === `Bearer ${MATRIX_KEYS.surplus}`;
  if (provider === "openlux") return request.headers.get("authorization") === `Bearer ${discovery ? MATRIX_KEYS.openlux_discovery : MATRIX_KEYS.openlux_spend}`;
  return (discovery && provider === "openrouter") || request.headers.get("authorization") === "Bearer fixture-provider-secret";
}

function models(provider) {
  if (provider === "codex") {
    return {
      models: [MATRIX_MODELS.codex, MATRIX_MODELS.unavailable].map((slug) => ({
        slug,
        display_name: slug,
        supported_reasoning_levels: [{ effort: "none" }, { effort: "max" }, { effort: "matrix-provider-tier" }],
        default_reasoning_level: "none",
        context_window: 8192,
      })),
    };
  }
  if (provider === "claude") {
    return {
      data: [
        {
          id: MATRIX_MODELS.claude,
          type: "model",
          display_name: "Controlled Claude",
          created_at: "2026-01-01T00:00:00Z",
          max_input_tokens: 8192,
          max_tokens: 1024,
          capabilities: {
            thinking: { types: { disabled: { supported: true }, adaptive: { supported: true } } },
            effort: { supported: true, medium: { supported: true }, high: { supported: true } },
          },
        },
      ],
      has_more: false,
    };
  }
  let ids = [MATRIX_MODELS[provider] ?? MATRIX_MODELS.codex];
  if (provider === "openrouter")
    ids = [MATRIX_MODELS.openrouter, "typesafe/jev-fast", "~deepseek/deepseek-flash-latest", MATRIX_MODELS.decision, "~typesafe/jev-latest"];
  if (provider === "surplus" || provider === "openlux") ids = [MATRIX_MODELS.codex, MATRIX_MODELS.flash];
  return {
    data: ids.map((id) => ({
      id,
      object: "model",
      ...(provider === "openrouter" && [MATRIX_MODELS.decision, "~typesafe/jev-latest"].includes(id)
        ? { architecture: { output_modalities: ["decisions"] } }
        : {}),
      name: id,
      display_name: id,
      created: 1_800_000_000,
      created_at: "2026-01-01T00:00:00Z",
      context_length: 131072,
      supported_endpoint_types: ["openai", "openai-response"],
      supported_parameters: ["tools", "reasoning"],
      pricing: { prompt: "0.000001", completion: "0.000002" },
    })),
  };
}

function replayMessage() {
  return {
    role: "assistant",
    content: "replay answer 😀",
    reasoning_content: "replay thought é",
    tool_calls: [
      { id: "call_context_lookup", type: "function", function: { name: "lookup", arguments: '{"query":"kept😀"}' } },
      { id: "call_context_patch", type: "function", function: { name: "apply_patch", arguments: '{"input":"*** Begin Patch\\nfixture😀\\n*** End Patch"}' } },
    ],
  };
}

function completion(provider, body, marker) {
  const replay = marker.startsWith("fixture:matrix:replay:producer-");
  return {
    id: `chatcmpl_matrix_${provider}`,
    object: "chat.completion",
    created: 1_800_000_000,
    model: body.model,
    choices: [{ index: 0, message: replay ? replayMessage() : { role: "assistant", content: "matrix answer" }, finish_reason: replay ? "tool_calls" : "stop" }],
    usage: { prompt_tokens: 7, completion_tokens: 3, total_tokens: 10, ...(provider === "surplus" ? { buyer_cost_micro: 5 } : {}) },
  };
}

function response(provider, body) {
  return {
    id: `resp_matrix_${provider}`,
    object: "response",
    created_at: 1_800_000_000,
    model: body.model,
    status: "completed",
    output: [
      {
        id: `msg_matrix_${provider}`,
        type: "message",
        role: "assistant",
        status: "completed",
        content: [{ type: "output_text", text: "matrix answer", annotations: [] }],
      },
    ],
    usage: { input_tokens: 7, output_tokens: 3, total_tokens: 10, ...(provider === "surplus" ? { buyer_cost_micro: 5 } : {}) },
  };
}

function anthropic(body, marker = "") {
  const replay = marker.startsWith("fixture:matrix:replay:claude-producer-");
  return {
    id: "msg_matrix_claude",
    type: "message",
    role: "assistant",
    model: body.model,
    content: replay
      ? [
          { type: "thinking", thinking: "thought", signature: "private-fixture-signature" },
          { type: "text", text: "answer" },
          { type: "tool_use", id: "call_1", name: "read_file", input: { path: "a.txt" } },
        ]
      : [{ type: "text", text: "matrix answer" }],
    stop_reason: replay ? "tool_use" : "end_turn",
    stop_sequence: null,
    usage: { input_tokens: 7, output_tokens: 3 },
  };
}

function claudeReplayParts(body, marker) {
  const message = anthropic(body, marker);
  return [
    part("message_start", { type: "message_start", message: { ...message, content: [], stop_reason: null, usage: { input_tokens: 7, output_tokens: 0 } } }),
    part("content_block_start", { type: "content_block_start", index: 0, content_block: { type: "thinking", thinking: "" } }),
    part("content_block_delta", { type: "content_block_delta", index: 0, delta: { type: "thinking_delta", thinking: "thought" } }),
    part("content_block_delta", { type: "content_block_delta", index: 0, delta: { type: "signature_delta", signature: "private-fixture-signature" } }),
    part("content_block_stop", { type: "content_block_stop", index: 0 }),
    part("content_block_start", { type: "content_block_start", index: 1, content_block: { type: "text", text: "" } }),
    part("content_block_delta", { type: "content_block_delta", index: 1, delta: { type: "text_delta", text: "answer" } }),
    part("content_block_stop", { type: "content_block_stop", index: 1 }),
    part("content_block_start", { type: "content_block_start", index: 2, content_block: { type: "tool_use", id: "call_1", name: "read_file", input: {} } }),
    part("content_block_delta", { type: "content_block_delta", index: 2, delta: { type: "input_json_delta", partial_json: '{"path":"a.txt"}' } }),
    part("content_block_stop", { type: "content_block_stop", index: 2 }),
    part("message_delta", { type: "message_delta", delta: { stop_reason: "tool_use", stop_sequence: null }, usage: { output_tokens: 3 } }),
    part("message_stop", { type: "message_stop" }),
  ];
}

function replayChunks(provider, body) {
  const base = { id: `chatcmpl_matrix_${provider}`, object: "chat.completion.chunk", created: 1_800_000_000, model: body.model };
  const message = replayMessage();
  return [
    part(null, { ...base, choices: [{ index: 0, delta: { role: "assistant", reasoning_content: message.reasoning_content }, finish_reason: null }] }),
    part(null, { ...base, choices: [{ index: 0, delta: { content: message.content }, finish_reason: null }] }),
    part(null, { ...base, choices: [{ index: 0, delta: { tool_calls: message.tool_calls.map((call, index) => ({ ...call, index })) }, finish_reason: null }] }),
    part(null, {
      ...base,
      choices: [{ index: 0, delta: {}, finish_reason: "tool_calls" }],
      usage: { prompt_tokens: 7, completion_tokens: 3, total_tokens: 10 },
    }),
    new TextEncoder().encode("data: [DONE]\n\n"),
  ];
}

function streamParts(provider, body, wire, truncate, marker) {
  if (provider === "surplus" && marker === "fixture:matrix:precommit-paid:paid-eof") {
    return [
      part(null, { type: "response.created", sequence_number: 0, response: { ...response(provider, body), status: "in_progress", output: [], usage: null } }),
    ];
  }
  if (wire === "chat" && marker.startsWith("fixture:matrix:replay:producer-")) return replayChunks(provider, body);
  if (wire === "anthropic" && marker.startsWith("fixture:matrix:replay:claude-producer-")) return claudeReplayParts(body, marker);
  if (wire === "anthropic") {
    const message = anthropic(body);
    return [
      part("message_start", { type: "message_start", message: { ...message, content: [], stop_reason: null, usage: { input_tokens: 7, output_tokens: 0 } } }),
      part("content_block_start", { type: "content_block_start", index: 0, content_block: { type: "text", text: "" } }),
      part("content_block_delta", { type: "content_block_delta", index: 0, delta: { type: "text_delta", text: "matrix " } }),
      ...(truncate
        ? []
        : [
            part("content_block_delta", { type: "content_block_delta", index: 0, delta: { type: "text_delta", text: "answer" } }),
            part("content_block_stop", { type: "content_block_stop", index: 0 }),
            part("message_delta", { type: "message_delta", delta: { stop_reason: "end_turn", stop_sequence: null }, usage: { output_tokens: 3 } }),
            part("message_stop", { type: "message_stop" }),
          ]),
    ];
  }
  if (wire === "chat") {
    const base = { id: `chatcmpl_matrix_${provider}`, object: "chat.completion.chunk", created: 1_800_000_000, model: body.model };
    return [
      part(null, { ...base, choices: [{ index: 0, delta: { role: "assistant", content: "matrix " }, finish_reason: null }] }),
      ...(truncate
        ? []
        : [
            part(null, { ...base, choices: [{ index: 0, delta: { content: "answer" }, finish_reason: null }] }),
            part(null, {
              ...base,
              choices: [{ index: 0, delta: {}, finish_reason: "stop" }],
              usage: { prompt_tokens: 7, completion_tokens: 3, total_tokens: 10 },
            }),
            new TextEncoder().encode("data: [DONE]\n\n"),
          ]),
    ];
  }
  const full = response(provider, body);
  return [
    part(null, { type: "response.created", sequence_number: 0, response: { ...full, status: "in_progress", output: [] } }),
    part(null, { type: "response.output_text.delta", sequence_number: 1, output_index: 0, content_index: 0, item_id: full.output[0].id, delta: "matrix " }),
    ...(truncate
      ? []
      : [
          part(null, {
            type: "response.output_text.delta",
            sequence_number: 2,
            output_index: 0,
            content_index: 0,
            item_id: full.output[0].id,
            delta: "answer",
          }),
          part(null, { type: "response.completed", sequence_number: 3, response: full }),
        ]),
  ];
}

function streamed(provider, body, wire, marker, context, recordEvent) {
  const chunks = streamParts(provider, body, wire, marker.endsWith(":truncated"), marker);
  let closed = false;
  let timer;
  let finish;
  const lifetime = new Promise((resolve) => {
    finish = resolve;
  });
  const stream = new ReadableStream({
    start(controller) {
      const initial = wire === "anthropic" ? 3 : wire === "responses" ? 2 : 1;
      for (const chunk of chunks.slice(0, initial)) controller.enqueue(chunk);
      timer = setTimeout(
        () => {
          if (closed) return;
          for (const chunk of chunks.slice(initial)) controller.enqueue(chunk);
          if (marker === "fixture:matrix:codex:terminal-hang") return;
          closed = true;
          controller.close();
          finish();
        },
        marker.endsWith(":abort") ? 60_000 : 75
      );
    },
    cancel() {
      if (closed) return;
      closed = true;
      clearTimeout(timer);
      context.waitUntil(recordEvent({ kind: "matrix_cancelled", provider, marker }));
      finish();
    },
  });
  context.waitUntil(lifetime);
  return new Response(stream, { headers: { "content-type": "text/event-stream", "x-request-id": requestId(provider, marker) } });
}

/** Lithos really speaks Chat: its role-only chunk translates to response.created. */
function precommitStream(request, body, marker, context, recordEvent) {
  const base = { id: "chatcmpl_precommit_lithos", object: "chat.completion.chunk", created: 1_800_000_000, model: body.model };
  const chunk = (delta) => part(null, { ...base, choices: [{ index: 0, delta, finish_reason: null }] });
  let closed = false;
  let timer;
  let finish;
  const lifetime = new Promise((resolve) => (finish = resolve));
  const cancelled = () => {
    if (closed) return;
    closed = true;
    clearTimeout(timer);
    context.waitUntil(recordEvent({ kind: "matrix_cancelled", provider: "lithos", marker }));
    finish();
  };
  request.signal.addEventListener("abort", cancelled, { once: true });
  const stream = new ReadableStream({
    start(controller) {
      controller.enqueue(chunk({ role: "assistant" }));
      if (marker.endsWith(":after-output")) controller.enqueue(chunk({ content: "committed Lithos output" }));
      context.waitUntil(recordEvent({ kind: "matrix_precommit_ready", provider: "lithos", marker }));
      timer = setTimeout(
        () => {
          if (closed) return;
          closed = true;
          if (marker.endsWith(":failed")) controller.enqueue(part(null, { error: { type: "server_error", message: "Controlled presemantic failure" } }));
          if (marker.endsWith(":malformed")) controller.enqueue(new TextEncoder().encode("data: {invalid}\n\n"));
          controller.close();
          finish();
        },
        marker.endsWith(":abort") ? 60_000 : 75
      );
    },
    cancel: cancelled,
  });
  context.waitUntil(lifetime.finally(() => request.signal.removeEventListener("abort", cancelled)));
  return new Response(stream, { headers: { "content-type": "text/event-stream", "x-request-id": "precommit-lithos-id" } });
}

async function metadata(request, provider, url, recordEvent) {
  if (provider === "claude" && url.pathname === "/v1/organizations/workspaces") {
    if (!credentialMatches(request, provider, false)) return new Response(null, { status: 401 });
    await recordEvent({ kind: "matrix_workspaces", provider });
    return Response.json({ data: [{ id: "wrkspc_fixture_matrix", name: "Default", archived_at: null }] });
  }
  if (provider !== "openlux") return null;
  if (url.pathname === "/api/status") return Response.json({ success: true, data: { setup: true, quota_per_unit: 500000 } });
  if (url.pathname === "/api/ratio_config") {
    return Response.json({
      success: true,
      data: { model_ratio: { [MATRIX_MODELS.codex]: 1, [MATRIX_MODELS.unavailable]: 1, [MATRIX_MODELS.flash]: 1 }, model_price: {}, completion_ratio: {} },
    });
  }
  if (url.pathname !== "/api/log/token") return null;
  if (url.searchParams.get("key") !== MATRIX_KEYS.openlux_spend) return new Response(null, { status: 401 });
  await recordEvent({ kind: "matrix_billing_lookup", provider, credential_matched: true, page: url.searchParams.get("page") });
  return Response.json({ success: true, data: { items: [...billingLogs.values()], total: billingLogs.size } });
}

function quota() {
  return Response.json(
    { error: { type: "usage_limit_reached", code: "usage_limit_reached", resets_at: Math.floor(Date.now() / 1000) + 3600 } },
    {
      status: 429,
      headers: {
        "x-codex-primary-used-percent": "100",
        "x-codex-primary-window-minutes": "300",
        "x-codex-primary-reset-at": String(Math.floor(Date.now() / 1000) + 3600),
      },
    }
  );
}

async function digest(value) {
  const bytes = await crypto.subtle.digest("SHA-256", new TextEncoder().encode(value));
  return [...new Uint8Array(bytes)].map((byte) => byte.toString(16).padStart(2, "0")).join("");
}

/** Fixed M31 decision transport: answers are keyed by actual incoming questions, never a canned summary. */
async function compactionDecisions(request, body, recordEvent) {
  if (!isObject(body.state) || !isObject(body.questions)) return null;
  const marker = JSON.stringify(body.state).match(/fixture:matrix:compaction:[a-z0-9_-]+/u)?.[0];
  if (!marker) return null;
  if (!credentialMatches(request, "openrouter", false)) return new Response(null, { status: 401 });
  const names = Object.keys(body.questions);
  if (
    !names.every(
      (name) => /^(call|result)_t\d+$/u.test(name) && body.questions[name]?.type === "noul" && typeof body.questions[name]?.instructions === "string"
    )
  ) {
    return Response.json({ error: { type: "invalid_request_error", message: "Unexpected controlled compaction question shape" } }, { status: 400 });
  }
  await recordEvent({ kind: "matrix_compaction_jev", marker, model: body.model, questions: names, state_fields: Object.keys(body.state) });
  if (marker.endsWith(":failure")) return Response.json({ error: { type: "server_error", message: "Controlled Jev decision failure" } }, { status: 503 });
  const answered = marker.endsWith(":partial") ? names.slice(1) : names;
  return Response.json({ answers: Object.fromEntries(answered.map((name) => [name, { noul: 0.1 }])) });
}

/** Observe actual synthetic wire bytes, never compute the application's replay or reduction. */
async function contextObservation(body, marker) {
  if (marker.startsWith("fixture:matrix:compaction:")) {
    const messages = Array.isArray(body.messages) ? body.messages : [];
    return {
      compaction_messages: await Promise.all(
        messages.map(async (message) => {
          const text = typeof message.content === "string" ? message.content : JSON.stringify(message.content);
          return {
            role: message.role,
            tool_call_id: message.tool_call_id ?? null,
            sha256: await digest(text),
            bytes: new TextEncoder().encode(text).byteLength,
          };
        })
      ),
    };
  }
  if (marker.startsWith("fixture:matrix:replay:")) {
    const captured = { context_input: body.input ?? null, context_messages: body.messages ?? null };
    if (JSON.stringify(captured).length > 32_768) throw new Error("Controlled replay capture exceeded its fixed fixture bound");
    return captured;
  }
  if (!marker.startsWith("fixture:matrix:unicode:")) return {};
  const tool = body.messages?.find((message) => message.role === "tool" && message.tool_call_id === "call_unicode");
  if (!tool || typeof tool.content !== "string") return { unicode: null };
  const boundary = tool.content.indexOf("\n[gateway:");
  const prefix = boundary < 0 ? tool.content : tool.content.slice(0, boundary);
  return {
    unicode: {
      tool_call_id: tool.tool_call_id,
      bytes: new TextEncoder().encode(tool.content).byteLength,
      sha256: await digest(tool.content),
      prefix_sha256: await digest(prefix),
      marker: boundary < 0 ? null : tool.content.slice(boundary),
      well_formed: tool.content.isWellFormed(),
      replacement_character: tool.content.includes("\ufffd"),
    },
  };
}

/** Fixed destination upstream extension, not a standalone application. */
export async function handleProviderMatrix(request, _env, context, recordEvent) {
  const url = new URL(request.url);
  const destination = destinations.get(url.hostname);
  if (!destination || !destination.paths.includes(url.pathname)) return null;
  const { provider } = destination;
  if (request.method === "GET") {
    const handled = await metadata(request, provider, url, recordEvent);
    if (handled) return handled;
  }
  if (request.method === "GET" && url.pathname.endsWith("/models")) {
    if (provider === "codex" && codexAccount(request) === null) return null;
    if (!credentialMatches(request, provider, true)) return Response.json({ error: { type: "authentication_error" } }, { status: 401 });
    await recordEvent({ kind: "matrix_catalog", provider, client_version: url.searchParams.get("client_version"), credential_matched: true });
    return Response.json(models(provider));
  }
  if (request.method !== "POST") return null;
  const body = await request.clone().json();
  if (!isObject(body)) return null;
  if (provider === "openrouter" && url.pathname === "/api/alpha/decisions") {
    if (!credentialMatches(request, provider, false)) return new Response(null, { status: 401 });
    if (![MATRIX_MODELS.decision, "~typesafe/jev-latest"].includes(body.model)) return new Response(null, { status: 400 });
    await recordEvent({ kind: "matrix_dispatch", provider, marker: body.state, model: body.model, wire: "decisions", credential_matched: true });
    return Response.json({ answers: { ready: 0.9 }, usage: { input_tokens: 7, output_tokens: 3, cost: 0.01 }, trace: body.trace });
  }
  if (provider === "openrouter" && url.pathname === "/api/v1/systemone") return compactionDecisions(request, body, recordEvent);
  const marker = markerIn(body);
  if (!marker) return null;
  if (!credentialMatches(request, provider, false)) return Response.json({ error: { type: "authentication_error" } }, { status: 401 });
  const wire = url.pathname.endsWith("/messages") ? "anthropic" : url.pathname.endsWith("/chat/completions") ? "chat" : "responses";
  await recordEvent({
    kind: "matrix_dispatch",
    provider,
    marker,
    model: body.model,
    stream: body.stream === true,
    wire,
    credential_matched: true,
    effort: isObject(body.reasoning) ? (body.reasoning.effort ?? null) : (body.reasoning_effort ?? null),
    mask_secrets: request.headers.get("x-si-mask-secrets"),
    has_account: request.headers.has("chatgpt-account-id"),
    account_label: codexAccount(request)?.endsWith("-a") ? "a" : codexAccount(request)?.endsWith("-b") ? "b" : null,
    originator: request.headers.get("originator"),
    anthropic_version: request.headers.get("anthropic-version"),
    workspace: request.headers.get("anthropic-workspace-id"),
    thinking: isObject(body.thinking) ? body.thinking.type : null,
    ...(await contextObservation(body, marker)),
  });
  // Owner v6 sibling wire cases. Original markers/conditions remain untouched.
  if (marker.startsWith("fixture:matrix:early:")) {
    const mode = marker.split(":").at(-1);
    if (provider === "lithos" && (mode === "jsonhead" || body.stream !== true)) {
      return Response.json(completion(provider, body, marker), { headers: { "x-request-id": "early-lithos-json" } });
    }
    if (provider === "lithos" && mode !== "no200") {
      const value = {
        id: "chatcmpl_early_head",
        object: "chat.completion.chunk",
        created: 1800000000,
        model: body.model,
        choices: [{ index: 0, delta: { role: "assistant" }, finish_reason: null }],
      };
      return new Response(
        new ReadableStream({
          start(controller) {
            controller.enqueue(part(null, value));
            controller.close();
          },
        }),
        { headers: { "content-type": "text/event-stream", "x-request-id": "early-lithos-head" } }
      );
    }
    if ((provider === "lithos" && mode === "no200") || (provider === "openrouter" && ["mixed400", "mixed503", "no200"].includes(mode))) {
      const status = provider === "lithos" || mode === "mixed503" ? 503 : 400;
      const headers = new Headers({ "retry-after": mode === "mixed503" ? "r".repeat(513) : "17", "x-request-id": "early-" + provider });
      if (mode === "mixed503") {
        headers.set("set-cookie", "fixture-redaction-canary");
        headers.set("authorization", "fixture-redaction-canary");
      }
      return Response.json(
        { error: { code: "matrix_head_refusal", message: mode, type: status >= 500 ? "server_error" : "invalid_request_error" } },
        { status, headers }
      );
    }
  }
  if (provider === "claude" && marker.endsWith(":workspace") && !request.headers.has("anthropic-workspace-id")) {
    return Response.json({ error: { type: "invalid_request_error", message: "Request is not scoped to a workspace" } }, { status: 400 });
  }
  if (provider === "lithos" && marker === "fixture:matrix:compaction:failure") {
    return Response.json({ error: { type: "rate_limit_error", message: "Controlled compaction fallback rung" } }, { status: 429 });
  }
  if (provider === "lithos" && marker.startsWith("fixture:matrix:lithos:stream-refusal")) {
    return Response.json(
      { error: { type: "rate_limit_error", code: "tokens_exhausted", message: "wait" } },
      { status: 429, headers: { "x-should-retry": "false", "retry-after-ms": "5000" } }
    );
  }
  if (provider === "openrouter" && marker.startsWith("fixture:matrix:precommit-paid:")) {
    return Response.json({ error: { type: "rate_limit_error", message: "Controlled direct capacity refusal" } }, { status: 429 });
  }
  if (provider === "surplus" && marker === "fixture:matrix:precommit-paid:capacity") {
    return Response.json({ error: { type: "insufficient_balance", message: "Controlled paid capacity refusal" } }, { status: 402 });
  }
  if (marker.endsWith(":transient")) return Response.json({ error: { type: "server_error", message: "Controlled matrix transient failure" } }, { status: 503 });
  if (marker.endsWith(":unavailable"))
    return Response.json({ detail: `The '${body.model}' model is not supported when using Codex with a ChatGPT account.` }, { status: 400 });
  const paidFallback = /:(surplus|openlux|unknown)$/u.test(marker);
  if (provider === "codex" && (paidFallback || (marker.endsWith(":mixed") && codexAccount(request)?.endsWith("-a")))) return quota();
  if (marker.endsWith(":quota")) return quota();
  if (provider === "surplus" && /:(openlux|unknown)$/u.test(marker)) {
    return Response.json({ error: { type: "insufficient_balance", message: "Controlled paid capacity refusal" } }, { status: 402 });
  }
  if (provider === "openlux" && !marker.endsWith(":unknown")) {
    const id = requestId(provider, marker);
    billingLogs.set(id, {
      request_id: id,
      model_name: body.model,
      quota: 20,
      prompt_tokens: 7,
      completion_tokens: 3,
      created_at: Math.floor(Date.now() / 1000),
    });
  }
  if (provider === "lithos" && (marker.startsWith("fixture:matrix:precommit:") || marker.startsWith("fixture:matrix:precommit-paid:"))) {
    return precommitStream(request, body, marker, context, recordEvent);
  }
  if (body.stream === true || provider === "codex") return streamed(provider, body, wire, marker, context, recordEvent);
  return Response.json(wire === "chat" ? completion(provider, body, marker) : wire === "anthropic" ? anthropic(body, marker) : response(provider, body), {
    headers: { "x-request-id": requestId(provider, marker) },
  });
}
