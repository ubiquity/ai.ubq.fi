# ai.ubq.fi

OpenAI API-compatible gateway for the ubq.fi ecosystem (Deno Deploy).

LLM and app integration notes live in [`static/docs/llms-agents.md`](static/docs/llms-agents.md) and are served at
`https://ai.ubq.fi/docs/llms-agents.md`.

## How auth works

- Clients authenticate to `ai.ubq.fi` with a **UOS gateway token**: `Authorization: Bearer <token>`.
  - Accepted tokens come from `UOS_AI_TOKEN` and/or API keys stored in Deno KV (created via `/admin/api-keys`).
  - Admin tokens (including Deno Deploy tokens) also grant access to client routes (`/v1/*`).
- The gateway **does not use or forward your client token upstream**.
  - For upstream requests, it uses **Codex CLI ChatGPT auth** from `CODEX_AUTH_JSON_B64` (base64 of
    `~/.codex/auth.json`).
  - Upstream usage/limits are tied to that OpenAI account + plan; client-provided OpenAI API keys are ignored.
  - The OAuth `client_id` used for refresh-token rotation is **public** (not a secret); the secrets are the tokens in
    `CODEX_AUTH_JSON_B64` and your client/admin tokens.
- Browser passkey sign-in is supported only on `https://ai.ubq.fi` and loopback development origins. Preview deployments
  use same-origin API calls and should use gateway/admin token auth instead of passkeys.

## Quickstart (curl)

Set a gateway token:

```bash
export UOS_AI_TOKEN="..."
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
curl -sS https://ai.ubq.fi/health/upstream
curl -sS https://ai.ubq.fi/health/auth
```

List models:

```bash
curl -sS https://ai.ubq.fi/v1/models \
  -H "Authorization: Bearer $UOS_AI_TOKEN"
```

Inspect gateway-specific model capabilities:

```bash
curl -sS https://ai.ubq.fi/uos/models/capabilities \
  -H "Authorization: Bearer $UOS_AI_TOKEN"
```

Whoami (debug which auth method was used; never returns raw secrets):

```bash
curl -sS https://ai.ubq.fi/uos/auth \
  -H "Authorization: Bearer $UOS_AI_TOKEN" \
  | jq
```

Chat completion (OpenAI-compatible):

```bash
curl -sS https://ai.ubq.fi/v1/chat/completions \
  -H "Authorization: Bearer $UOS_AI_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{
    "reasoning_effort": "high",
    "messages": [{"role":"system","content":"You are a helpful assistant."},{"role":"user","content":"Tell me a short joke."}],
    "stream": false
  }'
```

Just the assistant message text:

```bash
curl -sS https://ai.ubq.fi/v1/chat/completions \
  -H "Authorization: Bearer $UOS_AI_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{"messages":[{"role":"system","content":"You are a helpful assistant."},{"role":"user","content":"Tell me a short joke."}],"stream":false}' \
  | jq -r '.choices[0].message.content'
```

Notes:

- System/developer messages are optional. When present, the gateway combines them into upstream instructions.
- The Codex upstream requires `stream: true`; when you set `"stream": false`, the gateway buffers the upstream stream
  and returns a normal JSON response.
- Chat completions and responses allow omitting `model`; the gateway falls back to its configured default.
- Use `reasoning_effort` for chat completions or `reasoning` for responses to control reasoning level.

Streaming:

```bash
curl -N https://ai.ubq.fi/v1/chat/completions \
  -H "Authorization: Bearer $UOS_AI_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{
    "stream": true,
    "messages": [{"role":"system","content":"You are a helpful assistant."},{"role":"user","content":"Say hello in 5 different ways."}]
  }'
```

Responses (OpenAI-compatible):

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

Embeddings (OpenAI-compatible):

```bash
curl -sS https://ai.ubq.fi/v1/embeddings \
  -H "Authorization: Bearer $UOS_AI_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{"model":"text-embedding-3-small","input":"hello"}'
```

Notes:

- `input` can be a string or an array of strings (batching is strongly recommended).
- Backed by Voyage (`voyage-4-large`) and cached in Deno KV. The cache is quota-driven: it keeps writing until KV is
  full, then evicts the oldest entries (FIFO) and retries.
- Embedding dimensionality may differ from OpenAI because the vectors come from Voyage.
- When rate limited (by Voyage or the gateway's own KV throttling), the gateway returns `429` with `Retry-After`;
  clients should retry (or use the async jobs API below).

Embeddings jobs (async, gateway-specific):

Use this when you might exceed Voyage's free-tier rate limits. The gateway will either return the embeddings immediately
or queue the request and let you poll for completion.

Create:

```bash
curl -sS https://ai.ubq.fi/uos/embedding-jobs \
  -H "Authorization: Bearer $UOS_AI_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{"model":"text-embedding-3-small","input":["hello","world"]}' \
  | jq
```

Poll:

```bash
job_id="$(curl -sS https://ai.ubq.fi/uos/embedding-jobs \
  -H "Authorization: Bearer $UOS_AI_TOKEN" \
  -H "Content-Type: application/json" \
  --data '{"model":"text-embedding-3-small","input":"hello"}' \
  | jq -r .id)"

curl -sS "https://ai.ubq.fi/uos/embedding-jobs/${job_id}" \
  -H "Authorization: Bearer $UOS_AI_TOKEN" \
  | jq
```

Notes:

- Jobs currently support `encoding_format="float"` only.
- When queued, the gateway responds `202` with `Retry-After` and `retry_after_seconds`; poll until `status="succeeded"`.
- Jobs are scoped to the authenticated client identity; poll using credentials that resolve to the same identity scope
  used to create the job (same API key, or for GitHub/kernel auth the same `{owner, repo}` attestation context).
- Inputs are stored encrypted in Deno KV for up to 24h to allow deferred processing, and deleted once the job completes.

## CLI (ubq-ai)

Run from this repo:

```bash
cd lib/ai.ubq.fi
export UOS_AI_TOKEN="..."
deno task ubq-ai chat --system "You are a helpful assistant." "Tell me a short joke."
```

Client commands also accept an admin token (`DENO_DEPLOY_TOKEN`) when `UOS_AI_TOKEN` is unset.

Install on your machine:

```bash
cd lib/ai.ubq.fi
deno install -g --allow-env --allow-net --allow-read -n ubq-ai scripts/ubq-ai.ts
```

Examples:

```bash
export UOS_AI_TOKEN="..."
ubq-ai whoami | jq
ubq-ai models | jq
ubq-ai chat --system "You are a helpful assistant." "Tell me a short joke."
ubq-ai chat --system "You are a helpful assistant." --reasoning-effort high "Solve: 24*7."
ubq-ai chat --system "You are a helpful assistant." --stream "Say hello in 5 different ways."
ubq-ai responses --instructions "You are a helpful assistant." "Summarize this in 1 sentence: ..."
ubq-ai responses --instructions "You are a helpful assistant." --reasoning-effort high "Write a short proof sketch for the pigeonhole principle."
```

Debug (prints useful env/token fingerprints to stderr, never raw secrets):

```bash
ubq-ai -v models
```

Health probe (cron-friendly):

```bash
deno task health:check --url https://ai.ubq.fi
# auth metadata only (does not refresh auth or consume chat tokens):
deno task health:check --url https://ai.ubq.fi --auth
# or: deno run --allow-net scripts/health-check.ts --url https://ai.ubq.fi --json --auth
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
- `UOS_AI_TOKEN` (optional): Comma- or newline-separated client tokens accepted via `Authorization: Bearer ...`. The
  gateway can also accept API keys stored in Deno KV (created via `/admin/api-keys`).
- `DENO_DEPLOY_TOKEN` (optional, recommended): Tokens accepted for admin endpoints.
- `CODEX_BASE_URL` (optional): Defaults to `https://chatgpt.com/backend-api/codex`.
- `VOYAGEAI_API_KEY` (optional): Voyage API key used for embeddings. If unset, the gateway will look for a key stored in
  Deno KV at `["uos_ai","voyage_api_key"]`.
- `CORS_ALLOW_ORIGIN` (optional): Defaults to `*`.
- `UOS_API_KEY_DEFAULT_USAGE_LIMIT` (optional): Default usage limit for new API keys in requests/week. Defaults to `50`.
- `UOS_API_KEY_DEFAULT_EXPIRY_DAYS` (optional): Default expiration for new API keys in days. Defaults to `90`.

## Admin: upload/validate Codex auth.json

This validates your posted `auth.json` against the upstream Codex endpoint and, if valid, stores the tokens in Deno KV
(becoming the active upstream auth for subsequent requests). The helper extracts the Codex model catalog from your local
Codex CLI binary and uploads that snapshot as the single source of truth for `/v1/models` and the default model picker.

Treat `auth.json` as a secret (it contains refresh tokens). Use the repo helper CLI:

```bash
cd lib/ai.ubq.fi
export DENO_DEPLOY_TOKEN="..."
deno task upload:auth --url https://ai.ubq.fi
```

The helper CLI uses `DENO_DEPLOY_TOKEN` and requires a `codex` binary with embedded model metadata. Use `--codex-bin` to
point at a specific binary if it is not on your PATH.

## Admin: create/manage UBQ API keys

API keys are stored in Deno KV (hashed) and are only returned once on creation. Keys are prefixed with `u_` for easy
identification.

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

## Admin: kernel auth usage limits (GitHub token)

Kernel-attested GitHub token auth is tracked per `owner/repo` and can also be limited per org (`owner`). The default
limit is unlimited until an admin sets a per-repo or per-org limit. Limits reset weekly by default, unless `window_ms`
is provided.

Get repo usage/limit (admin):

```bash
export DENO_DEPLOY_TOKEN="..."
curl -sS "https://ai.ubq.fi/admin/kernel-usage?owner=acme&repo=demo" \
  -H "Authorization: Bearer $DENO_DEPLOY_TOKEN" \
  | jq
```

Get org usage/limit (admin):

```bash
export DENO_DEPLOY_TOKEN="..."
curl -sS "https://ai.ubq.fi/admin/kernel-usage?owner=acme&scope=org" \
  -H "Authorization: Bearer $DENO_DEPLOY_TOKEN" \
  | jq
```

Set repo limit (admin):

```bash
export DENO_DEPLOY_TOKEN="..."
curl -sS https://ai.ubq.fi/admin/kernel-usage \
  -H "Authorization: Bearer $DENO_DEPLOY_TOKEN" \
  -H "Content-Type: application/json" \
  --data-binary '{"owner":"acme","repo":"demo","usage_limit_requests":500,"reset_usage":true}' \
  | jq
```

Set org limit (admin) with 1 request per minute:

```bash
export DENO_DEPLOY_TOKEN="..."
curl -sS https://ai.ubq.fi/admin/kernel-usage \
  -H "Authorization: Bearer $DENO_DEPLOY_TOKEN" \
  -H "Content-Type: application/json" \
  --data-binary '{"owner":"acme","scope":"org","usage_limit_requests":1,"window_ms":60000,"reset_usage":true}' \
  | jq
```

CLI:

```bash
deno task ubq-ai admin kernel-usage get --owner acme --repo demo
deno task ubq-ai admin kernel-usage get --owner acme --scope org
deno task ubq-ai admin kernel-usage set --owner acme --repo demo --usage-limit 500 --reset-usage
deno task ubq-ai admin kernel-usage set --owner acme --scope org --usage-limit 1 --window-ms 60000 --reset-usage
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

- `GET /`, `GET /docs`, `GET /chat`, `GET /admin`, and static assets
- `GET /health`
- `GET /health/auth` (Codex auth metadata; no upstream refresh and no chat tokens used)
- `GET /health/upstream` (Codex chat probe)
- `POST /api/auth/register/start`, `POST /api/auth/register/finish`
- `POST /api/auth/login/start`, `POST /api/auth/login/finish`
- `GET /api/auth/session`, `POST /api/auth/logout`
- Browser passkey flows are intended for `https://ai.ubq.fi` and loopback development origins only; preview deployment
  URLs should use token auth.
- `GET /admin/passkey-users`, `PATCH /admin/passkey-users` (super-admin only)
- `POST /admin/codex/auth` (admin only)
- `GET /admin/codex/models`, `POST /admin/codex/models` (admin only)
- `POST /admin/codex/prompts/purge` (admin only)
- `GET /admin/defaults`, `POST /admin/defaults` (admin only)
- `POST /admin/kv-migration/import`, `GET /admin/kv-migration/validate` (super-admin only)
- `GET /admin/api-keys`, `POST /admin/api-keys`, `PATCH /admin/api-keys`, `DELETE /admin/api-keys` (admin only)
- `POST /admin/api-keys/revoke`, `POST /admin/api-keys/unrevoke` (admin only)
- `GET /admin/kernel-usage`, `POST /admin/kernel-usage`, `DELETE /admin/kernel-usage` (admin only)
- `GET /admin/kernel-policy-queue` (admin only)
- `GET /admin/kernel-pubkeys`, `POST /admin/kernel-pubkeys`, `DELETE /admin/kernel-pubkeys` (admin only)
- `GET /uos/auth`
- `GET /uos/models/capabilities`
- `POST /uos/embedding-jobs`, `GET /uos/embedding-jobs/:id`
- `GET /uos/agent-messages`, `POST /uos/agent-messages`
- `GET /v1/models`
- `POST /v1/embeddings`
- `POST /v1/chat/completions` (streaming and non-streaming)
- `POST /v1/responses` (streaming and non-streaming; non-streaming buffers upstream SSE)

## Local dev

```bash
export UOS_AI_TOKEN="dev-token"
export CODEX_AUTH_JSON_B64="$(base64 < ~/.codex/auth.json | tr -d '\n')"
deno task dev
```

## Deno KV migration

Use the KV migration helper to inspect a Deno 1/Classic KV database, export it, map it into a local Deno 2 KV, and
validate it before any production import.

```bash
export DENO_KV_ACCESS_TOKEN="..."
SOURCE_KV="https://api.deno.com/databases/<database-id>/connect"

deno task kv:probe --source "$SOURCE_KV"
deno task kv:export --source "$SOURCE_KV" --out .kv-migration/deno1.ndjson
deno task kv:analyze --in .kv-migration/deno1.ndjson --profile local
deno task kv:import-local --in .kv-migration/deno1.ndjson --db .kv-migration/deno1.sqlite3 --profile local --overwrite
deno task kv:validate --db .kv-migration/deno1.sqlite3 --strict
```

For production, run the remote import as a dry run first. Remote imports do not write until `--write` is passed.

```bash
DEST_KV="https://api.deno.com/databases/<deno-2-database-id>/connect"
deno task kv:analyze --in .kv-migration/deno1.ndjson --profile prod
deno task kv:import-remote --in .kv-migration/deno1.ndjson --dest "$DEST_KV" --profile prod --overwrite
deno task kv:import-remote --in .kv-migration/deno1.ndjson --dest "$DEST_KV" --profile prod --overwrite --write
deno task kv:validate --target "$DEST_KV" --strict
```

If the new Deno Deploy database is only reachable from the app runtime, use the super-admin HTTP importer. It writes
through the deployed app's own `Deno.openKv()` connection and is also dry-run by default:

```bash
BASE_URL="https://ai.ubq.fi"
deno task kv:import-http --in .kv-migration/deno1.ndjson --base-url "$BASE_URL" --profile prod --overwrite
deno task kv:import-http --in .kv-migration/deno1.ndjson --base-url "$BASE_URL" --profile prod --overwrite --write
deno task kv:validate-http --base-url "$BASE_URL" --strict
```

The `local` profile imports durable settings plus Codex auth/model records and legacy rows for replay. The `prod`
profile imports modern durable settings only: it skips `codex_auth` and `codex_models` because the deploy workflow
refreshes those from the Codex CLI snapshot, and it skips legacy `["key", ...]` rows by default. Runtime-only state such
as passkey sessions, WebAuthn challenges, embedding jobs, and rate windows is skipped.
