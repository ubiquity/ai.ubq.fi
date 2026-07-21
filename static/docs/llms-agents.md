# LLMs and Agents

UbiquityOS AI Gateway provides OpenAI-compatible endpoints for chat and responses, plus agent message storage for
kernel-driven workflows. This page focuses on LLM usage and agent messages.

## Base URL

All requests are served from:

```
https://ai.ubq.fi
```

The gateway is also available via the Deno Deploy default domain:

```
https://ai-ubq-fi.deno.dev
```

OpenAI client base URL (example):

```
https://ai.ubq.fi/v1
```

Use this `/v1` URL as the `baseURL` or `base_url` in OpenAI-compatible SDKs. Do not append another `/v1` in request
paths when the SDK already joins paths against the configured base URL.

## Authentication

Send a bearer token in `Authorization`:

```
Authorization: Bearer <UOS_AI_TOKEN>
```

Accepted tokens:

- Gateway tokens configured via `UOS_AI_TOKEN` on the server.
- API keys stored in Deno KV (created via `/admin/api-keys`).
- Admin tokens (Deno Deploy token or allowlisted admin token) are also accepted for client routes, but application
  integrations should not label client credentials as `DENO_DEPLOY_TOKEN`.
- GitHub tokens are accepted only when paired with kernel attestation headers.

For app integrations, name the client credential `UOS_AI_TOKEN` or another gateway/API-key-specific name. Reserve
`DENO_DEPLOY_TOKEN` for Deno Deploy admin operations.

The gateway never forwards your client token upstream. It uses Codex CLI auth configured on the server.

### GitHub token headers (kernel auth)

When using a GitHub token for any `/v1/*` route, include:

- `X-GitHub-Owner: <owner>`
- `X-GitHub-Repo: <repo>`
- `X-Ubiquity-Kernel-Token: <jwt>`

If the kernel attestation includes `installation_id`, also send:

- `X-GitHub-Installation-Id: <id>`

## Quick start (curl)

Set a gateway token:

```bash
export UOS_AI_TOKEN="..."
```

List models:

```bash
curl -sS https://ai.ubq.fi/v1/models \
  -H "Authorization: Bearer $UOS_AI_TOKEN"
```

Send a chat completion:

```bash
curl -sS https://ai.ubq.fi/v1/chat/completions \
  -H "Authorization: Bearer $UOS_AI_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{
    "model": "gpt-5.5",
    "reasoning_effort": "low",
    "stream": false,
    "messages": [
      {"role": "system", "content": "You are helpful."},
      {"role": "user", "content": "Say hi."}
    ]
  }'
```

Request embeddings:

```bash
curl -sS https://ai.ubq.fi/v1/embeddings \
  -H "Authorization: Bearer $UOS_AI_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{"model":"text-embedding-3-small","input":["hello","world"]}'
```

## Endpoints

OpenAI-compatible endpoints:

- `GET /v1/models`
- `POST /v1/chat/completions`
- `POST /v1/responses`
- `POST /v1/embeddings`

UOS gateway endpoints:

- `GET /uos/auth`
- `GET /uos/models/capabilities`
- `POST /uos/embeddings`
- `POST /uos/embedding-jobs`
- `GET /uos/embedding-jobs/{id}`
- `GET /uos/agent-messages`
- `POST /uos/agent-messages`

Health endpoints:

- `GET /health`
- `GET /health/auth`
- `GET /health/upstream`

## Models

`GET /v1/models` returns a strict OpenAI-style model list (`object: "list"`, `data: [...]`) from the stored live Codex
model catalog. Each model object contains only the OpenAI model fields: `id`, `object`, `created`, and `owned_by`. When
no snapshot has been initialized, it returns an empty list.

Use `/v1/models` as the source of truth instead of assuming OpenAI public API aliases are supported. The gateway is
backed by Codex with a ChatGPT account, so some OpenAI API model aliases may not be available through this gateway.
Hidden Codex catalog entries such as internal review models are filtered during snapshot upload and are not exposed.

Use `GET /uos/models/capabilities` for gateway-specific model metadata such as `supported_reasoning_levels`,
`default_reasoning_effort`, `context_window_tokens`, `max_context_window_tokens`, `auto_compact_token_limit_tokens`,
`supported_endpoints`, and `upstream_provider`. This metadata is intentionally not included in `/v1/models` so
OpenAI-compatible SDKs receive an OpenAI-shaped response.

Observed integration behavior:

- `gpt-5.5` works for `/v1/chat/completions`.
- `gpt-5.5` accepts `reasoning_effort: "low"`.
- `gpt-5-chat-latest` and `gpt-5.1-chat-latest` are rejected when they are absent from `/v1/models`:

```json
{
  "error": {
    "message": "The model 'gpt-5-chat-latest' does not exist or is not available through this gateway. Use /v1/models for supported models.",
    "type": "invalid_request_error",
    "param": "model",
    "code": "model_not_found"
  }
}
```

The gateway does not reject non-empty reasoning tier strings before contacting Codex. Codex upstream remains
authoritative and any upstream rejection is returned in the normal OpenAI-style error envelope.

## Chat Completions

`POST /v1/chat/completions`

Required:

- `messages` must be a non-empty array.

Optional (defaulted by the gateway):

- `model` (defaults to the configured model).
- `stream` (defaults to `false`).
- `reasoning_effort` (defaults to the configured reasoning effort).

Supported message content:

- String content: `"content": "..."`
- Mixed content array with text and images:

```json
{
  "role": "user",
  "content": [
    { "type": "text", "text": "Describe this image." },
    { "type": "image_url", "image_url": { "url": "https://..." } }
  ]
}
```

System and developer messages are combined into a single instruction block upstream. User and assistant messages are
passed through as conversation input.

Role mapping notes:

- `system` and `developer` are merged into upstream instructions.
- `tool` is treated as a developer message.
- Image parts are only accepted on user messages; assistant image parts are ignored.

### Streaming

Set `"stream": true` to receive server-sent events (`text/event-stream`) in OpenAI `chat.completion.chunk` format with a
final `data: [DONE]` sentinel.

```bash
curl -N https://ai.ubq.fi/v1/chat/completions \
  -H "Authorization: Bearer $UOS_AI_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{
    "stream": true,
    "messages": [
      {"role": "user", "content": "Say hello in 5 different ways."}
    ]
  }'
```

If `stream` is omitted or `false`, the gateway buffers the upstream stream and returns a standard chat completion JSON
response.

## Responses

`POST /v1/responses`

Input formats:

- String input: `"input": "..."`
- Array of message items:

```json
[
  { "type": "message", "role": "user", "content": [{ "type": "input_text", "text": "Hello" }] }
]
```

- Array of content items (coalesced into user messages):

```json
[
  { "type": "input_text", "text": "Summarize this" },
  { "type": "input_image", "image_url": "https://..." }
]
```

If `input` is omitted, the gateway sends an empty input array upstream.

Optional fields:

- `model` (defaults to the configured model).
- `instructions` (string).
- `reasoning` (object).
- `stream` (defaults to `false`).

`reasoning` object fields:

- `effort` (string).
- `summary` (string).
- `generate_summary` (string).

Example:

```bash
curl -sS https://ai.ubq.fi/v1/responses \
  -H "Authorization: Bearer $UOS_AI_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{
    "reasoning": { "effort": "high" },
    "instructions": "You are a helpful assistant.",
    "input": "Summarize this in 1 sentence: ..."
  }'
```

When `stream` is `false`, the gateway buffers the upstream stream and returns the final `response` object. When `stream`
is `true`, the upstream SSE stream is passed through.

## Embeddings

`POST /v1/embeddings`

OpenAI-compatible embeddings endpoint backed by Voyage (cached in Deno KV). The cache is quota-driven: it keeps writing
until KV is full, then evicts the oldest entries (FIFO) and retries.

Request:

- `model` (string, required): accepts `text-embedding-3-small`, `text-embedding-3-large`, or `voyage-4-large`.
- `input` (string or string[], required).
- `encoding_format` (optional): `float` (default) or `base64`.
- `dimensions` (optional): `256`, `512`, `1024` (default), or `2048`.

The gateway returns an OpenAI-style response with `object: "list"` and one `data[]` entry per input string.

Notes:

- Batching is strongly recommended (send `input` as an array).
- When rate limited (by Voyage or the gateway's own KV throttling), the gateway responds `429` with `Retry-After`.

`POST /uos/embeddings` exposes the retrieval-specific Voyage profile without adding provider-only fields to the
OpenAI-compatible endpoint. It requires `model="voyage-4-large"` and `input_type="query"|"document"`; `dimensions`
defaults to `1024`, `truncation` defaults to `true`, and `encoding_format` is fixed to `float`. Callers that need an
over-length input to fail instead of being shortened must send `truncation=false` explicitly.

## Embedding Jobs (Async)

`POST /uos/embedding-jobs` creates an async job. The gateway either completes it immediately (`200`) or queues it
(`202`) when Voyage is rate limited.

`GET /uos/embedding-jobs/{id}` polls the job until `status="succeeded"` or `status="failed"`.

Notes:

- Jobs use the same Voyage profile fields and validation as `/uos/embeddings` and support `encoding_format="float"`
  only.
- When queued, the gateway responds with `Retry-After` and `retry_after_seconds`.
- Jobs are scoped to the authenticated client identity; poll using credentials that resolve to the same identity scope
  used to create the job (same API key, or for GitHub/kernel auth the same `{owner, repo}` attestation context).
- Inputs are stored encrypted in Deno KV for up to 24h to allow deferred processing, and deleted once the job completes.

## Reasoning defaults

- Models with `supported_reasoning_levels` in the stored Codex catalog default to the configured reasoning effort.
- Non-reasoning models default to `none`.
- Reasoning tier strings come from each model's uploaded Codex CLI metadata; inspect `/uos/models/capabilities` instead
  of relying on a hard-coded tier list.
- Use `reasoning_effort: "none"` (chat completions) or `reasoning: { "effort": "none" }` (responses) to disable
  reasoning. `none` is always available even when the uploaded catalog omits it, and the gateway preserves it verbatim
  at the Codex upstream boundary.
- Other non-empty tier strings pass through to Codex upstream without gateway validation unless the stored catalog
  provides a wire translation. If upstream rejects one, inspect `/uos/models/capabilities` and retry with a tier
  advertised for that model.
- The stored catalog includes a metadata-derived `reasoning_effort_wire_map`; request handling applies that map
  generically and otherwise passes the selected effort through unchanged. For the current Codex catalog this maps the
  `ultra` orchestration preset to upstream effort `max`. Automatic multi-agent delegation is performed by Codex clients
  and is not provided by this stateless API gateway.
- In the browser chat playground, `Default` omits `reasoning_effort`; `None` always sends `reasoning_effort: "none"`.

Defaults can be managed via `/admin/defaults` (admin auth required). When no model is explicitly configured, the gateway
uses the first model in the current Codex model snapshot. If neither a configured default nor a snapshot is available,
no-model requests fail with `503` instead of fetching a live fallback catalog.

`GET /uos/models/capabilities` is the endpoint to inspect reasoning support and token-window limits programmatically.
`/v1/models` remains OpenAI-compatible and does not include gateway metadata.

## Ignored parameters and warnings

Requests accept a broad set of OpenAI-compatible keys, but unknown top-level keys are rejected with
`invalid_request_error`. Only a subset are forwarded upstream. Ignored keys are reported in the `x-uos-warning` response
header (comma-separated).

For the most portable request, start with only `model`, `messages`, `stream`, and `reasoning_effort`. Add optional
OpenAI parameters only after checking this section or validating the target endpoint.

Keys forwarded for chat completions:

- `tools`
- `tool_choice`
- `parallel_tool_calls`
- `prompt_cache_key`

Keys forwarded for responses:

- `tools`
- `tool_choice`
- `parallel_tool_calls`
- `prompt_cache_key`
- `text`
- `include`

The Codex CLI compatibility extension `client_metadata` is accepted as a string map and stripped before the gateway
builds the upstream request. It remains separate from the official OpenAI request schema. The gateway generates its own
upstream request metadata instead of forwarding client-supplied session identifiers.

`store` is always set to `false` by the gateway. The following are always ignored and will produce warnings:

- `temperature` -> `temperature_ignored`
- `max_tokens`, `max_completion_tokens`, `max_output_tokens` -> `max_output_tokens_ignored`
- `moderation` -> `moderation_ignored`
- `prompt_cache_options` -> `prompt_cache_options_ignored`

Any other accepted-but-unused key will emit a `<key>_ignored` warning.

## Agent Messages (LLM agents)

`/uos/agent-messages` stores and retrieves agent messages in Deno KV. It requires GitHub token auth with kernel
attestation.

Required headers:

- `Authorization: Bearer <github_token>`
- `X-GitHub-Owner: <owner>`
- `X-GitHub-Repo: <repo>`
- `X-Ubiquity-Kernel-Token: <jwt>`

If the kernel attestation includes `installation_id`, also send:

- `X-GitHub-Installation-Id: <id>`

### POST /uos/agent-messages

Body:

- `agent_id` (string, required, max 120 chars)
- `body` (string, required, max 8000 chars)
- `channel` (string, optional, max 120 chars)
- `kind` (string, optional, max 120 chars)
- `metadata` (object, optional, JSON-encoded size <= 8000 chars)

The server assigns `id`, `created_at_ms`, `owner`, `repo`, and `state_id` from the kernel attestation.

### GET /uos/agent-messages

Query parameters:

- `since` (ms, optional)
- `limit` (optional, default 50, max 200)
- `cursor` (optional)
- `channel` (optional)
- `agent_id` (optional)

Response includes:

- `messages`: list of agent messages
- `next_since`: ms timestamp of last message in the page
- `next_cursor`: cursor for pagination
- `has_more`: whether another page is available

## Auth introspection

`GET /uos/auth` returns the auth mode, the method used, and a token fingerprint (never the raw token). Use it to confirm
which auth path was selected.

If the auth method is a KV API key, the response includes key metadata and usage counters.

This is the best first check when an integration fails. It confirms whether the presented token resolved as a gateway
token, a KV API key, an admin token, a passkey session, or a GitHub/kernel token.

## Health

- `GET /health` is the readiness check: it validates configured Codex auth metadata and performs an upstream probe.
- `GET /health/auth` returns Codex auth metadata without refreshing or contacting upstream.
- `GET /health/upstream` runs the same upstream probe semantics as `/health` without readiness metadata.

Use `/health/auth` for passive auth state inspection. If it reports an expired access token or
`codex_auth_refresh_failed`, client authentication may still be valid; the server-side Codex auth needs repair.

## Errors

Errors from `/v1/*` OpenAI-compatible endpoints follow an OpenAI-style envelope, including upstream Codex `detail`
responses normalized by the gateway:

```json
{
  "error": {
    "message": "...",
    "type": "invalid_request_error",
    "code": "..."
  }
}
```

UOS, admin, or health/debug routes should still be parsed defensively by integrations that call them directly:

```ts
const message = body?.error?.message ??
  body?.detail ??
  response.statusText;
```

Common status codes:

- `401` invalid or missing auth
- `403` forbidden (e.g., GitHub token required for agent messages)
- `429` rate limit exceeded for KV API keys
- `5xx` upstream or server errors

If chat returns `503` with `refresh_token_reused`, the client token may be fine. Repair the gateway's upstream Codex
auth and retry.

Useful response headers for debugging:

- `x-deno-trace-id`
- `x-uos-request-id`
- `x-ubq-upstream`
- `x-uos-router-revision`
