# LLMs and Agents

UbiquityOS AI Gateway provides OpenAI-compatible endpoints for chat and responses, plus an agent message bus for
kernel-driven workflows. This page focuses on LLM usage and the agent bus.

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

## Authentication

Send a bearer token in `Authorization`:

```
Authorization: Bearer <token>
```

Accepted tokens:

- Gateway tokens configured via `UOS_AI_TOKEN` on the server.
- API keys stored in Deno KV (created via `/admin/api-keys`).
- Admin tokens (Deno Deploy token or allowlisted admin token) are also accepted for client routes.
- GitHub tokens are accepted only when paired with kernel attestation headers.

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
    "model": "gpt-5.2-chat-latest",
    "messages": [
      {"role": "system", "content": "You are a helpful assistant."},
      {"role": "user", "content": "Tell me a short joke."}
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

- `GET /v1/models`
- `POST /v1/chat/completions`
- `POST /v1/responses`
- `POST /v1/embeddings`
- `POST /v1/embeddings/jobs`
- `GET /v1/embeddings/jobs/{id}`
- `GET /v1/auth`
- `GET /v1/agent-bus`
- `POST /v1/agent-bus`
- `GET /health`
- `GET /health/auth`
- `GET /health/upstream`

## Models

`GET /v1/models` returns a normalized OpenAI-style model list (`object: "list"`, `data: [...]`). The gateway may serve
the upstream list or a cached snapshot when upstream is unavailable.

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
    "model": "gpt-5.2-chat-latest",
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
- `reasoning` (object or `null`).
- `stream` (defaults to `false`).

`reasoning` object fields:

- `effort` (string or `null`).
- `summary` (string or `null`).
- `generate_summary` (string or `null`).

Example:

```bash
curl -sS https://ai.ubq.fi/v1/responses \
  -H "Authorization: Bearer $UOS_AI_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{
    "model": "gpt-5.2-chat-latest",
    "reasoning": { "effort": "high" },
    "instructions": "You are a helpful assistant.",
    "input": "Summarize this in 1 sentence: ..."
  }'
```

When `stream` is `false`, the gateway buffers the upstream stream and returns the final `response` object. When `stream`
is `true`, the upstream SSE stream is passed through.

## Embeddings

`POST /v1/embeddings`

OpenAI-compatible embeddings endpoint backed by Voyage (cached in Deno KV, FIFO-bounded).

Request:

- `model` (string, required): accepts `text-embedding-3-small`, `text-embedding-3-large`, or `voyage-*`.
- `input` (string or string[], required).
- `encoding_format` (optional): `float` (default) or `base64`.

The gateway returns an OpenAI-style response with `object: "list"` and one `data[]` entry per input string.

Notes:

- Batching is strongly recommended (send `input` as an array).
- When rate limited, the gateway responds `429` with `Retry-After`.

## Embeddings Jobs (Async)

`POST /v1/embeddings/jobs` creates an async job. The gateway either completes it immediately (`200`) or queues it
(`202`) when Voyage is rate limited.

`GET /v1/embeddings/jobs/{id}` polls the job until `status="succeeded"` or `status="failed"`.

Notes:

- Jobs currently support `encoding_format="float"` only.
- When queued, the gateway responds with `Retry-After` and `retry_after_seconds`.
- Jobs are scoped to the authenticated client identity; poll using the same `Authorization` token and auth headers used
  to create the job (GitHub tokens must include the same kernel attestation headers + repo context).
- Inputs are stored encrypted in Deno KV for up to 24h to allow deferred processing, and deleted once the job completes.

## Reasoning defaults

- Reasoning models (model IDs starting with `gpt-5` or `o`) default to the configured reasoning effort.
- Non-reasoning models default to `none`.
- Use `reasoning_effort: null` (chat completions) or `reasoning: null` (responses) to disable reasoning.
- Allowed effort values: `none`, `minimal`, `low`, `medium`, `high`, `xhigh`.

Defaults can be managed via `/admin/defaults` (admin auth required). The built-in defaults are `model = gpt-5.2-codex`
and `reasoning_effort = medium`.

## Model normalization

The gateway normalizes some chat-latest aliases before sending upstream:

- `gpt-5.3-chat-latest` -> `gpt-5.3`
- `gpt-5.2-chat-latest` -> `gpt-5.2`
- `gpt-5.1-chat-latest` -> `gpt-5.1`
- `gpt-5-chat-latest` -> `gpt-5.2`

## Ignored parameters and warnings

Requests accept a broad set of OpenAI-compatible keys, but unknown top-level keys are rejected with
`invalid_request_error`. Only a subset are forwarded upstream. Ignored keys are reported in the `x-uos-warning` response
header (comma-separated).

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

`store` is always set to `false` by the gateway. The following are always ignored and will produce warnings:

- `temperature` -> `temperature_ignored`
- `max_tokens`, `max_completion_tokens`, `max_output_tokens` -> `max_output_tokens_ignored`

Any other accepted-but-unused key will emit a `<key>_ignored` warning.

## Agent Bus (LLM agents)

`/v1/agent-bus` stores and retrieves agent messages in Deno KV. It requires GitHub token auth with kernel attestation.

Required headers:

- `Authorization: Bearer <github_token>`
- `X-GitHub-Owner: <owner>`
- `X-GitHub-Repo: <repo>`
- `X-Ubiquity-Kernel-Token: <jwt>`

If the kernel attestation includes `installation_id`, also send:

- `X-GitHub-Installation-Id: <id>`

### POST /v1/agent-bus

Body:

- `agent_id` (string, required, max 120 chars)
- `body` (string, required, max 8000 chars)
- `channel` (string, optional, max 120 chars)
- `kind` (string, optional, max 120 chars)
- `metadata` (object, optional, JSON-encoded size <= 8000 chars)

The server assigns `id`, `created_at_ms`, `owner`, `repo`, and `state_id` from the kernel attestation.

### GET /v1/agent-bus

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

`GET /v1/auth` returns the auth mode, the method used, and a token fingerprint (never the raw token). Use it to confirm
which auth path was selected.

If the auth method is a KV API key, the response includes key metadata and usage counters.

## Health

- `GET /health` verifies service configuration (Codex auth, token/KV availability).
- `GET /health/auth` verifies Codex auth and refresh status.
- `GET /health/upstream` verifies upstream connectivity.

## Errors

Errors follow an OpenAI-style envelope:

```json
{
  "error": {
    "message": "...",
    "type": "invalid_request_error",
    "code": "..."
  }
}
```

Common status codes:

- `401` invalid or missing auth
- `403` forbidden (e.g., GitHub token required for agent bus)
- `429` rate limit exceeded for KV API keys
- `5xx` upstream or server errors
