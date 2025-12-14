# ai.ubq.fi

OpenAI API-compatible gateway for the ubq.fi ecosystem (Deno Deploy).

## How auth works

- Clients authenticate to `ai.ubq.fi` with a **UBQ gateway token**: `Authorization: Bearer <token>`.
  - The allowed tokens come from `UBQ_AI_AUTH_TOKENS` (comma- or newline-separated).
- The gateway **does not use or forward your client token to OpenAI**.
  - For upstream requests, it always sets `Authorization: Bearer $OPENAI_API_KEY`.
  - API usage is billed to whatever `OPENAI_API_KEY` is configured on the server.

## Quickstart (curl)

Set a gateway token (must match one of the server’s `UBQ_AI_AUTH_TOKENS`):

```bash
export UBQ_AI_TOKEN="..."
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
    "model": "gpt-5.2-chat-latest",
    "messages": [{"role":"user","content":"Tell me a short joke."}],
    "max_completion_tokens": 120
  }'
```

Notes:

- Some models (including `gpt-5.2-chat-latest`) require `max_completion_tokens` instead of `max_tokens`.
- Some models only support the default `temperature` value; omit `temperature` if you get `unsupported_value`.

Streaming:

```bash
curl -N https://ai.ubq.fi/v1/chat/completions \
  -H "Authorization: Bearer $UBQ_AI_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{
    "model": "gpt-5.2-chat-latest",
    "stream": true,
    "messages": [{"role":"user","content":"Say hello in 5 different ways."}],
    "max_completion_tokens": 120
  }'
```

## Runtime env

- `OPENAI_API_KEY` (required): Upstream OpenAI API key used by the gateway.
- `UBQ_AI_AUTH_TOKENS` (required in production): Comma- or newline-separated tokens accepted from clients via
  `Authorization: Bearer ...` (gateway tokens, not OpenAI API keys).
- `OPENAI_BASE_URL` (optional): Defaults to `https://api.openai.com`.
- `CORS_ALLOW_ORIGIN` (optional): Defaults to `*`.

## Supported routes

- `GET /` and `GET /health`
- Proxies all OpenAI endpoints under `/v1/*` (e.g., `POST /v1/responses`, `POST /v1/chat/completions`), including
  streaming responses.

## Local dev

```bash
export OPENAI_API_KEY="..."
export UBQ_AI_AUTH_TOKENS="dev-token"
deno task dev
```
