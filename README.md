# ai.ubq.fi

OpenAI API-compatible gateway for the ubq.fi ecosystem (Deno Deploy).

## Runtime env

- `OPENAI_API_KEY` (required): Upstream OpenAI API key used by the gateway.
- `UBQ_AI_AUTH_TOKENS` (required in production): Comma- or newline-separated API tokens accepted from clients via
  `Authorization: Bearer ...`.
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
