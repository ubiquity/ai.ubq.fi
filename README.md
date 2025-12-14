# ai.ubq.fi

OpenAI API-compatible gateway for the ubq.fi ecosystem (Deno Deploy).

## How auth works

- Clients authenticate to `ai.ubq.fi` with a **UBQ gateway token**: `Authorization: Bearer <token>`.
  - Accepted tokens come from `UBQ_AI_AUTH_TOKENS` and/or API keys stored in Deno KV (created via `/admin/api-keys`).
- The gateway **does not use or forward your client token upstream**.
  - For upstream requests, it uses **Codex CLI ChatGPT auth** from `CODEX_AUTH_JSON_B64` (base64 of
    `~/.codex/auth.json`).
  - Usage is billed to that Codex/ChatGPT account (not to client-provided OpenAI API keys).
  - The OAuth `client_id` used for refresh-token rotation is **public** (not a secret); the secrets are the tokens in
    `CODEX_AUTH_JSON_B64` and your client/admin tokens.

## Quickstart (curl)

Set a gateway token:

```bash
export UBQ_AI_TOKEN="..."
```

Create one (admin):

```bash
curl -sS https://ai.ubq.fi/admin/api-keys \
  -H "Authorization: Bearer $UBQ_AI_ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{"name":"example key"}' \
  | jq -r .token
```

Health:

```bash
curl -sS https://ai.ubq.fi/health
```

List models:

```bash
curl -sS https://ai.ubq.fi/v1/models \
  -H "Authorization: Bearer $UBQ_AI_TOKEN"
```

Chat completion (OpenAI-compatible):

```bash
curl -sS https://ai.ubq.fi/v1/chat/completions \
  -H "Authorization: Bearer $UBQ_AI_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{
    "model": "gpt-5.1-codex-mini",
    "messages": [{"role":"user","content":"Tell me a short joke."}],
    "stream": false
  }'
```

Just the assistant message text:

```bash
curl -sS https://ai.ubq.fi/v1/chat/completions \
  -H "Authorization: Bearer $UBQ_AI_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{"model":"gpt-5.2-chat-latest","messages":[{"role":"user","content":"Tell me a short joke."}],"stream":false}' \
  | jq -r '.choices[0].message.content'
```

Notes:

- `system` messages are not supported by the Codex upstream; the gateway converts them to `developer`.
- The Codex upstream requires `stream: true`; when you set `"stream": false`, the gateway buffers the upstream stream
  and returns a normal JSON response.

Streaming:

```bash
curl -N https://ai.ubq.fi/v1/chat/completions \
  -H "Authorization: Bearer $UBQ_AI_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{
    "model": "gpt-5.1-codex-mini",
    "stream": true,
    "messages": [{"role":"user","content":"Say hello in 5 different ways."}]
  }'
```

## Runtime env

- `CODEX_AUTH_JSON_B64` (required): base64 of `~/.codex/auth.json` from a machine that ran `codex login`.
- `UBQ_AI_AUTH_TOKENS` (optional): Comma- or newline-separated client tokens accepted via `Authorization: Bearer ...`.
  The gateway can also accept API keys stored in Deno KV (created via `/admin/api-keys`).
- `UBQ_AI_ADMIN_TOKENS` (optional, recommended): Tokens accepted for admin endpoints. If unset on Deno Deploy, the admin
  endpoints accept a Deno Deploy token (`DENO_DEPLOY_TOKEN`, `ddw_...`) after verification against the Deno API.
- `CODEX_BASE_URL` (optional): Defaults to `https://chatgpt.com/backend-api/codex`.
- `CODEX_INSTRUCTIONS_B64` (optional): base64 override for the upstream `instructions` string (defaults to
  `codex_instructions.md`).
- `CORS_ALLOW_ORIGIN` (optional): Defaults to `*`.

## Admin: upload/validate Codex auth.json

This validates your posted `auth.json` against the upstream Codex endpoint and, if valid, stores the tokens in Deno KV
(becoming the active upstream auth for subsequent requests).

Treat `auth.json` as a secret (it contains refresh tokens).

```bash
export UBQ_AI_ADMIN_TOKEN="..."
curl -sS https://ai.ubq.fi/admin/codex/auth \
  -H "Authorization: Bearer $UBQ_AI_ADMIN_TOKEN" \
  -H "Content-Type: application/json" \
  --data-binary @~/.codex/auth.json \
  | jq
```

Or use the repo helper CLI:

```bash
cd lib/ai.ubq.fi
export UBQ_AI_ADMIN_TOKEN="..."
deno task upload:auth -- --url https://ai.ubq.fi
```

The helper CLI also accepts `DENO_DEPLOY_TOKEN` (if `UBQ_AI_ADMIN_TOKEN` is unset).

## Admin: create/manage UBQ API keys

API keys are stored in Deno KV (hashed) and are only returned once on creation.

Create (token only):

```bash
cd lib/ai.ubq.fi
export UBQ_AI_ADMIN_TOKEN="..."
deno task keys:create -- --name "example key" --token-only
```

List (admin):

```bash
deno task keys:list
```

Revoke (admin):

```bash
deno task keys:revoke -- --id "<id>"
```

## Supported routes

- `GET /` and `GET /health`
- `POST /admin/codex/auth` (admin only)
- `POST /admin/api-keys` (admin only)
- `GET /admin/api-keys` (admin only)
- `POST /admin/api-keys/revoke` (admin only)
- `GET /v1/models`
- `POST /v1/chat/completions` (streaming and non-streaming)
- `POST /v1/responses` (streaming and non-streaming; non-streaming buffers upstream SSE)

## Local dev

```bash
export UBQ_AI_AUTH_TOKENS="dev-token"
export CODEX_AUTH_JSON_B64="$(base64 < ~/.codex/auth.json | tr -d '\n')"
deno task dev
```
