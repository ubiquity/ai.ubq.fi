# ai.ubq.fi

OpenAI API-compatible gateway for the ubq.fi ecosystem (Deno Deploy).

## How auth works

- Clients authenticate to `ai.ubq.fi` with a **UBQ gateway token**: `Authorization: Bearer <token>`.
  - The allowed tokens come from `UBQ_AI_AUTH_TOKENS` (comma- or newline-separated).
- The gateway **does not use or forward your client token upstream**.
  - For upstream requests, it uses **Codex CLI ChatGPT auth** from `CODEX_AUTH_JSON_B64` (base64 of
    `~/.codex/auth.json`).
  - Usage is billed to that Codex/ChatGPT account (not to client-provided OpenAI API keys).
  - The OAuth `client_id` used for refresh-token rotation is **public** (not a secret); the secrets are the tokens in
    `CODEX_AUTH_JSON_B64` and your `UBQ_AI_AUTH_TOKENS`.

## Quickstart (curl)

Set a gateway token (must match one of the server’s `UBQ_AI_AUTH_TOKENS`):

```bash
export UBQ_AI_TOKEN="..."
```

Generate one (example):

```bash
openssl rand -hex 32
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
- `UBQ_AI_AUTH_TOKENS` (required in production): Comma- or newline-separated tokens accepted from clients via
  `Authorization: Bearer ...` (gateway tokens, not OpenAI API keys).
- `CODEX_BASE_URL` (optional): Defaults to `https://chatgpt.com/backend-api/codex`.
- `CODEX_INSTRUCTIONS_B64` (optional): base64 override for the upstream `instructions` string (defaults to
  `codex_instructions.md`).
- `CORS_ALLOW_ORIGIN` (optional): Defaults to `*`.

## Supported routes

- `GET /` and `GET /health`
- `GET /v1/models`
- `POST /v1/chat/completions` (streaming and non-streaming)
- `POST /v1/responses` (streaming and non-streaming; non-streaming buffers upstream SSE)

## Local dev

```bash
export UBQ_AI_AUTH_TOKENS="dev-token"
export CODEX_AUTH_JSON_B64="$(base64 < ~/.codex/auth.json | tr -d '\n')"
deno task dev
```
