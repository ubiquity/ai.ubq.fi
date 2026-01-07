# ai.ubq.fi

OpenAI API-compatible gateway for the ubq.fi ecosystem (Deno Deploy).

## How auth works

- Clients authenticate to `ai.ubq.fi` with a **UBQ gateway token**: `Authorization: Bearer <token>`.
  - Accepted tokens come from `UBIQUITY_AI_USER_TOKEN` and/or API keys stored in Deno KV (created via
    `/admin/api-keys`).
  - Admin tokens (including Deno Deploy tokens) also grant access to client routes (`/v1/*`).
- The gateway **does not use or forward your client token upstream**.
  - For upstream requests, it uses **Codex CLI ChatGPT auth** from `CODEX_AUTH_JSON_B64` (base64 of
    `~/.codex/auth.json`).
  - Upstream usage/limits are tied to that OpenAI account + plan; client-provided OpenAI API keys are ignored.
  - The OAuth `client_id` used for refresh-token rotation is **public** (not a secret); the secrets are the tokens in
    `CODEX_AUTH_JSON_B64` and your client/admin tokens.

## Quickstart (curl)

Set a gateway token:

```bash
export UBIQUITY_AI_USER_TOKEN="..."
```

Create one (admin):

```bash
curl -sS https://ai.ubq.fi/admin/api-keys \
  -H "Authorization: Bearer $DENO_DEPLOY_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{"name":"example key","expires_at_ms":-1}' \
  | jq -r .token
```

Health:

```bash
curl -sS https://ai.ubq.fi/health
```

List models:

```bash
curl -sS https://ai.ubq.fi/v1/models \
  -H "Authorization: Bearer $UBIQUITY_AI_USER_TOKEN"
```

Whoami (debug which auth method was used; never returns raw secrets):

```bash
curl -sS https://ai.ubq.fi/v1/auth \
  -H "Authorization: Bearer $UBIQUITY_AI_USER_TOKEN" \
  | jq
```

Chat completion (OpenAI-compatible):

```bash
curl -sS https://ai.ubq.fi/v1/chat/completions \
  -H "Authorization: Bearer $UBIQUITY_AI_USER_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{
    "model": "gpt-5.1-codex-mini",
    "reasoning_effort": "high",
    "messages": [{"role":"user","content":"Tell me a short joke."}],
    "stream": false
  }'
```

Just the assistant message text:

```bash
curl -sS https://ai.ubq.fi/v1/chat/completions \
  -H "Authorization: Bearer $UBIQUITY_AI_USER_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{"model":"gpt-5-chat-latest","messages":[{"role":"user","content":"Tell me a short joke."}],"stream":false}' \
  | jq -r '.choices[0].message.content'
```

Notes:

- `system` messages are not supported by the Codex upstream; the gateway converts them to `developer`.
- The Codex upstream requires `stream: true`; when you set `"stream": false`, the gateway buffers the upstream stream
  and returns a normal JSON response.
- Defaults: if `model` is omitted/blank, the gateway uses `gpt-5-chat-latest`; if reasoning is omitted, the gateway uses
  `xhigh` reasoning effort (for `gpt-5*` and `o*` models).

Streaming:

```bash
curl -N https://ai.ubq.fi/v1/chat/completions \
  -H "Authorization: Bearer $UBIQUITY_AI_USER_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{
    "model": "gpt-5.1-codex-mini",
    "stream": true,
    "messages": [{"role":"user","content":"Say hello in 5 different ways."}]
  }'
```

Responses (OpenAI-compatible):

```bash
curl -sS https://ai.ubq.fi/v1/responses \
  -H "Authorization: Bearer $UBIQUITY_AI_USER_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{
    "model": "gpt-5-chat-latest",
    "reasoning": { "effort": "high" },
    "input": "Summarize this in 1 sentence: ..."
  }'
```

## CLI (ubq-ai)

Run from this repo:

```bash
cd lib/ai.ubq.fi
export UBIQUITY_AI_USER_TOKEN="..."
deno task ubq-ai chat "Tell me a short joke."
```

Client commands also accept an admin token (`DENO_DEPLOY_TOKEN`) when
`UBIQUITY_AI_USER_TOKEN` is unset.

Install on your machine:

```bash
cd lib/ai.ubq.fi
deno install -g --allow-env --allow-net --allow-read -n ubq-ai scripts/ubq-ai.ts
```

Examples:

```bash
export UBIQUITY_AI_USER_TOKEN="..."
ubq-ai whoami | jq
ubq-ai models | jq
ubq-ai chat "Tell me a short joke."
ubq-ai chat --reasoning-effort high "Solve: 24*7."
ubq-ai chat --stream "Say hello in 5 different ways."
ubq-ai responses "Summarize this in 1 sentence: ..."
ubq-ai responses --reasoning-effort high "Write a short proof sketch for the pigeonhole principle."
```

Debug (prints useful env/token fingerprints to stderr, never raw secrets):

```bash
ubq-ai -v models
```

Admin examples (uses `DENO_DEPLOY_TOKEN`):

```bash
export DENO_DEPLOY_TOKEN="..."
ubq-ai admin upload-auth --auth-json ~/.codex/auth.json | jq
ubq-ai admin keys create "example key"
ubq-ai admin keys create "tmp key" --expires week
ubq-ai admin keys list | jq
```

## Runtime env

- `CODEX_AUTH_JSON_B64` (required): base64 of `~/.codex/auth.json` from a machine that ran `codex login`.
- `UBIQUITY_AI_USER_TOKEN` (optional): Comma- or newline-separated client tokens accepted via
  `Authorization: Bearer ...`. The gateway can also accept API keys stored in Deno KV (created via `/admin/api-keys`).
- `DENO_DEPLOY_TOKEN` (optional, recommended): Tokens accepted for admin endpoints.
- `CODEX_BASE_URL` (optional): Defaults to `https://chatgpt.com/backend-api/codex`.
- `CODEX_INSTRUCTIONS_B64` (optional): base64 override for the upstream `instructions` string (defaults to
  `codex_instructions.md`).
- `CORS_ALLOW_ORIGIN` (optional): Defaults to `*`.
- `UOS_API_KEY_DEFAULT_USAGE_LIMIT` (optional): Default usage limit for new API keys in requests/week. Defaults to `50`.
- `UOS_API_KEY_DEFAULT_EXPIRY_DAYS` (optional): Default expiration for new API keys in days. Defaults to `90`.

## Admin: upload/validate Codex auth.json

This validates your posted `auth.json` against the upstream Codex endpoint and, if valid, stores the tokens in Deno KV
(becoming the active upstream auth for subsequent requests).

Treat `auth.json` as a secret (it contains refresh tokens).

```bash
export DENO_DEPLOY_TOKEN="..."
curl -sS https://ai.ubq.fi/admin/codex/auth \
  -H "Authorization: Bearer $DENO_DEPLOY_TOKEN" \
  -H "Content-Type: application/json" \
  --data-binary @~/.codex/auth.json \
  | jq
```

Or use the repo helper CLI:

```bash
cd lib/ai.ubq.fi
export DENO_DEPLOY_TOKEN="..."
deno task upload:auth --url https://ai.ubq.fi
```

The helper CLI uses `DENO_DEPLOY_TOKEN`.

## Admin: create/manage UBQ API keys

API keys are stored in Deno KV (hashed) and are only returned once on creation. Keys are prefixed with `u_` for easy identification.

**Default Limits:**

- **Expiration**: 90 days (can be overridden with `--expires` or `--expires-at-ms`)
- **Usage Limit**: 50 requests/week (can be overridden with `--usage-limit`)
- **Reset Period**: Weekly (7 days, automatic)

Expiration:

- `expires_at_ms` is a Unix epoch millisecond timestamp; `-1` means "does not expire".
- Expired keys are rejected like revoked keys.

Usage Limits:

- `usage_limit_requests` sets maximum requests per week; `-1` means unlimited.
- `usage_requests` tracks current usage; resets automatically every 7 days.
- `usage_reset_at_ms` is the next reset timestamp.
- Rate limit errors (429) include reset time in the message.

Create (token only):

```bash
cd lib/ai.ubq.fi
export DENO_DEPLOY_TOKEN="..."
deno task ubq-ai admin keys create "example key"
```

Create (expires in a week):

```bash
deno task ubq-ai admin keys create "tmp key" --expires week
```

Create (with custom usage limit):

```bash
deno task ubq-ai admin keys create "high-volume key" --usage-limit 1000
```

Create (unlimited usage):

```bash
deno task ubq-ai admin keys create "unlimited key" --usage-limit unlimited
```

List (admin):

```bash
deno task ubq-ai admin keys list
```

Revoke (admin):

```bash
deno task ubq-ai admin keys revoke --id "<id>"
```

## Supported routes

- `GET /` and `GET /health`
- `POST /admin/codex/auth` (admin only)
- `POST /admin/api-keys` (admin only)
- `GET /admin/api-keys` (admin only)
- `POST /admin/api-keys/revoke` (admin only)
- `GET /v1/auth`
- `GET /v1/models`
- `POST /v1/chat/completions` (streaming and non-streaming)
- `POST /v1/responses` (streaming and non-streaming; non-streaming buffers upstream SSE)

## Local dev

```bash
export UBIQUITY_AI_USER_TOKEN="dev-token"
export CODEX_AUTH_JSON_B64="$(base64 < ~/.codex/auth.json | tr -d '\n')"
deno task dev
```
