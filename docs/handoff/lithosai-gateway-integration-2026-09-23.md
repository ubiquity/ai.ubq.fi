# LithosAI gateway integration handoff — 2026-09-23

## Status

Advisory handoff. Findings and verified probes only. Nothing in this repository was changed, deployed, or pushed, and
this document authorizes no source change.

Scope: everything needed to add LithosAI as an upstream provider in this gateway — endpoints, auth, the eight-model
catalog, wire-protocol deltas, rate-limit and billing semantics, and the translation hazards observed against a live
account. Every claim below is either reproduced from a probe on 2026-09-23 or attributed to a LithosAI document with its
URL. Anything unverified is marked as such.

Testing spend: exercise the DeepSeek V4.1 Flash tiers only. The Kimi K3 tiers list roughly 8x the input and 23x the
output rate of the DeepSeek base tier, and a Codex sub-agent can burn millions of input tokens per task, so Kimi ids
must not be used for test calls.

Context: a local Codex profile for LithosAI already exists on this Mac and works through a LiteLLM
Responses-to-Chat-Completions bridge (`~/.codex/lithosai.config.toml`, launchd agent `com.nv.lithosai-codex-bridge`).
That bridge is a stopgap; the durable path is this gateway routing LithosAI natively, after which the profile can point
at `https://ai.ubq.fi/v1` and the bridge can be retired.

## Objective

Add LithosAI as a first-class chat-only upstream provider in this gateway — catalog, routing, Responses translation,
metering, health, and error mapping — so Codex and other clients can select its eight models without any local proxy.

## Vendor facts (verified)

| Fact        | Value                                                                                                   |
| ----------- | ------------------------------------------------------------------------------------------------------- |
| Vendor      | LithosAI Inc. (`https://www.lithosai.com`), inference API for open-weight models on standard GPUs       |
| Base URL    | `https://api.lithosai.cloud/v1`                                                                         |
| Auth        | `Authorization: Bearer $LITHOSAI_API_KEY`; key prefix `lith_sk_`; keys shown once at creation           |
| Key scope   | Org-scoped, not user-scoped; every key draws the same rate limits and credit balance (attribution only) |
| Local key   | `LITHOSAI_API_KEY` is already exported from `~/.bash/.keys` on this Mac                                 |
| Console     | `https://console.lithosai.cloud` (keys, models, limits, billing, analytics)                             |
| Docs index  | `https://docs.lithosai.com/llms.txt`                                                                    |
| OpenAPI     | `https://docs.lithosai.com/openapi.yaml` (16991 bytes, 2026-09-23)                                      |
| Codex guide | `https://docs.lithosai.com/agent-integrations/codex.md`                                                 |

Endpoints that exist (from the OpenAPI, all under `/v1`):

| Endpoint                  | Method | Notes                                                                       |
| ------------------------- | ------ | --------------------------------------------------------------------------- |
| `/models`                 | GET    | Lists the models the organization can call                                  |
| `/models/{author}/{slug}` | GET    | Single model; payload is only `id`, `object`, `created`, `owned_by`         |
| `/chat/completions`       | POST   | OpenAI Chat Completions; `stream: true` yields SSE ending in `data: [DONE]` |

Endpoints that do not exist: there is no `/v1/responses`. Verified: `POST https://api.lithosai.cloud/v1/responses`
returned `404` with an empty body, so any Responses-speaking client needs a translation layer on our side. In this
gateway that layer is provider-specific rather than automatic: `src/deepseek_responses.ts` is the existing template
(Responses request to Chat Completions body, Chat completion back to a Responses object, Chat SSE into the Responses
event sequence, failing closed with `invalid_request_error` for anything outside the known subset), and narrow chat-only
routes are otherwise refused on `/v1/responses` with `unsupported_model`, as Cerebras is today.

## Model catalog (all eight, verified 2026-09-23)

`GET https://api.lithosai.cloud/v1/models` with the organization key returned exactly these ids, four DeepSeek and four
Moonshot AI:

| Model id                                     | Owner       | Params       | Advertised speed | Context   |
| -------------------------------------------- | ----------- | ------------ | ---------------- | --------- |
| `deepseek-ai/DeepSeek-V4.1-Flash`            | DeepSeek    | 552B (A16B)  | 250+ tok/s       | 1,048,576 |
| `deepseek-ai/DeepSeek-V4.1-Flash-fast`       | DeepSeek    | 552B (A16B)  | 350+ tok/s       | 1,048,576 |
| `deepseek-ai/DeepSeek-V4.1-Flash-ultra`      | DeepSeek    | 552B (A16B)  | 450+ tok/s       | 1,048,576 |
| `deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat` | DeepSeek    | 552B (A16B)  | 450+ tok/s       | 1,048,576 |
| `moonshotai/Kimi-K3`                         | Moonshot AI | 2.8T (A104B) | 80+ tok/s        | 1,048,576 |
| `moonshotai/Kimi-K3-fast`                    | Moonshot AI | 2.8T (A104B) | 150+ tok/s       | 1,048,576 |
| `moonshotai/Kimi-K3-ultra`                   | Moonshot AI | 2.8T (A104B) | 250+ tok/s       | 1,048,576 |
| `moonshotai/Kimi-K3-ultra-chat`              | Moonshot AI | 2.8T (A104B) | 250+ tok/s       | 1,048,576 |

Capability flags as published on the console models page: all eight advertise `chat`, `vision`, `tools`, `json_mode` and
`reasoning`; the four Kimi K3 entries additionally advertise `structured_outputs`. DeepSeek entries are described as
general-purpose; Kimi K3 entries as built for long-horizon coding (large repositories, tool use, debugging). The `-chat`
variants are tuned for high-interactivity chat rather than agent loops.

Listed rates per million tokens, as shown on `https://console.lithosai.cloud/models` on 2026-09-23. The page lists two
figures per rate; confirm which is currently active before this feeds cost accounting:

| Model                                        | Input         | Cached input    | Output        |
| -------------------------------------------- | ------------- | --------------- | ------------- |
| `deepseek-ai/DeepSeek-V4.1-Flash`            | $0.30 / $0.15 | $0.006 / $0.003 | $1.20 / $0.60 |
| `deepseek-ai/DeepSeek-V4.1-Flash-fast`       | $0.50 / $0.25 | $0.01 / $0.005  | $2.00 / $1.00 |
| `deepseek-ai/DeepSeek-V4.1-Flash-ultra`      | $0.70 / $0.35 | $0.014 / $0.007 | $2.80 / $1.40 |
| `deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat` | $0.70 / $0.35 | $0.014 / $0.007 | $2.80 / $1.40 |
| `moonshotai/Kimi-K3`                         | $2.40         | $0.24           | $12.00        |
| `moonshotai/Kimi-K3-fast`                    | $4.00         | $0.40           | $20.00        |
| `moonshotai/Kimi-K3-ultra`                   | $5.60         | $0.56           | $28.00        |
| `moonshotai/Kimi-K3-ultra-chat`              | $5.60         | $0.56           | $28.00        |

The API's `/v1/models` payload carries no context, capability, or price metadata — it is
`id`/`object`/`created`/`owned_by` only. Anything richer must be maintained in our catalog or scraped from the console.

## Wire-protocol deltas the adapter must own

1. Responses to Chat Completions. Required for every client that speaks the Responses API; reuse the existing
   translation used for the chat-only DeepSeek and Cerebras paths rather than writing a second one.

2. Streaming usage is unconditional and shaped unlike OpenAI's. Per the vendor OpenAPI and its `/chat/completions`
   description: most chunks carry a `usage` object, the opening role delta and the chunk carrying `finish_reason` omit
   it, and the final chunk before `[DONE]` reports totals with an empty `choices` array. Do not gate accounting on the
   client sending `stream_options.include_usage`.

3. `reasoning_content` is separate from `content` on assistant messages, and
   `completion_tokens_details.reasoning_tokens` counts reasoning tokens. Verified live: a Kimi K3 request with
   `max_tokens: 32` returned `content: ""`, `reasoning_content` filled, `finish_reason: "length"`, and
   `reasoning_tokens: 31`. Two consequences: budget small `max_tokens` requests accordingly, and map `reasoning_content`
   into Responses reasoning items if we want Codex to show thinking.

4. `reasoning_effort` is accepted and honoured; the vendor's drop-it advice is over-conservative for this path. Probed
   2026-09-23 against `deepseek-ai/DeepSeek-V4.1-Flash` with `none`, `minimal`, `low`, `medium`, `high`, `xhigh` and
   `max`: all seven returned 200, and the value moves the reasoning budget monotonically on a fixed multi-step prompt
   (`none` 0, `low` 236, `medium` 297, `high` 328, `xhigh` 347, `max` 354 reasoning tokens, `finish_reason: stop`
   throughout). The level is guidance, not a fixed quota: the same `max` yields 19-127 reasoning tokens on a trivial
   prompt and 354-787 on a real one. Through the local LiteLLM bridge with the vendor's `additional_drop_params` rule
   removed, the client-visible ladder survives (`model_reasoning_effort=none` -> 0 reasoning tokens, `=max` -> 787 on
   the multi-step prompt, 0 error items). The gateway adapter should therefore pass the effort through with Codex's
   level names mapped 1:1 rather than dropping it; keep the clamping question open only for `summary`-style values,
   which were not part of this probe.

5. Per-model sampling constraints. The OpenAPI records that Kimi K3 requires `n: 1`, `presence_penalty: 0.0` and
   `frequency_penalty: 0.0`; `top_k` is accepted as a non-standard parameter. The declared bounds in the schema are
   OpenAI-wire bounds, not per-model ones.

6. Error semantics worth mapping distinctly: `400` for `invalid_json`, `model_required`, `request_too_large`, plus
   engine-shaped per-parameter 400s passed through in a different envelope; `401` unauthorized; `402 insufficient_quota`
   when the organization is out of credit, carrying `x-should-retry: false`; `404` unknown model; `429` either
   `rate_limit_exceeded` with `error.type` naming which budget refused (`requests`, `input_tokens`, `output_tokens`) or
   `provider_overloaded` when the model is at capacity. Retry guidance: exponential backoff with jitter, prefer
   `retry-after-ms` then `retry-after`, never retry `400`, `401`, `402`, `404`, or anything with
   `x-should-retry: false`.

7. Rate limits are three per-minute budgets per model — requests, input tokens, output tokens — refilling continuously,
   with capacity set separately from the per-minute rate. So `x-ratelimit-remaining-*` is a balance, not
   limit-minus-spend. The headers accompany every admitted request and every refusal. Limits are per organization and
   shared across keys; current values live on the console Limits page, which is login-gated.

8. Billing is prepaid and per token, with three rates per model: input, cached input (a subset of input, not an
   addition), and output. A zero balance produces `402 insufficient_quota`. Auto-reload exists (threshold, target,
   monthly cap) and disarms itself after a failed charge.

## Suggested integration shape in this repository

- Provider client module in the shape of `src/cerebras.ts` (chat-only upstream: base URL, key env-var name, request
  mapping, SSE passthrough) plus a Responses adapter in the shape of `src/deepseek_responses.ts`, which is the module
  that actually makes Codex work against a Chat Completions upstream. `src/deepseek.ts` supplies the finish-disposition
  and reasoning-token helpers that fit the adapter's seams; `src/metered.ts` shows the same adapter duty done against a
  metered upstream that also exposes `/v1/responses` directly. The DeepSeek official route is the precedent for scoping:
  it is deliberately limited to `/v1/chat/completions` (see the comment in `src/openai.ts` above the `gpt-oss-120b`
  `unsupported_model` refusal) precisely because there is no Responses adapter for that base URL.
- Catalog entries for the eight ids with `context_window` and `max_context_window` both 1,048,576,
  `effective_context_window_percent` 95, capability flags for vision, tools, json_mode and reasoning, and
  `supported_endpoints` limited to `/v1/chat/completions` with `/v1/responses` served by our translation.
- Consider whether the three speed tiers per model are better expressed as speed tiers than as separate catalog entries;
  the `-fast` and `-ultra` suffixes are the same weights at higher per-token rates, so they are commercial tiers rather
  than distinct models.
- Metering and health: record provider health on the existing helper pattern, treat `402 insufficient_quota` as a
  distinct terminal state (out of credit, not a transient failure), and map the three `429` budget types into the
  admission/backoff policy rather than collapsing them.
- Secrets: the Mac launcher reads repository-root `.env`; the VPS needs its own copy of `LITHOSAI_API_KEY`. Adding a
  product environment variable or secret is an approval-gated change under the global instructions, so raise it before
  landing.
- Once native routing exists, point the local Codex profile at the gateway
  (`[model_providers.lithosai] base_url = "https://ai.ubq.fi/v1"`) and retire the LiteLLM bridge and its launchd agent.

## Local artifacts produced on 2026-09-23 (outside this repository)

These exist only to make the Codex client work today; they are not gateway code.

| Artifact                                                    | SHA-256                                                            | Note                                                                                                                                                                                                        |
| ----------------------------------------------------------- | ------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `~/.codex/lithosai.config.toml`                             | `45d919d67470afaae6d70328595af4293ed0346bcd6a567596fb29e607cd8f9d` | Codex profile: provider, default model, catalog pointer. Volatile by design — the user edits it; re-hash before relying (default model was `deepseek-ai/DeepSeek-V4.1-Flash-ultra` at 2026-09-23 10:40 UTC) |
| `~/.codex/model-catalogs/lithosai.json`                     | `45c60c5250cd6696e77e85a0e844b8c965083e94a1c1069904e29e44e0a35520` | All eight models in Codex catalog schema                                                                                                                                                                    |
| `~/.codex/lithosai/litellm.yaml`                            | `1d5fe6aa40f4a6e8fdcc01f597fe46f8e921eed74be22749fc1cce4fefcb294d` | LiteLLM bridge, one entry per model, `use_chat_completions_api: true`, `reasoning_effort` passes through                                                                                                    |
| `~/.codex/lithosai/start-bridge.sh`                         | `9582e48d43790efdc771bb9d342b26961b55dfa1745c900a51632ae7f6619d67` | Launch script; sources `~/.bash/.keys`                                                                                                                                                                      |
| `~/Library/LaunchAgents/com.nv.lithosai-codex-bridge.plist` | `87752960906bdc796768e56e97d8af3cf845ef479d1fac284023c33dc8086de9` | Keeps the bridge on `127.0.0.1:4000`                                                                                                                                                                        |

Bridge runtime is LiteLLM 1.102.1, installed isolated with `uv tool install 'litellm[proxy]'` at `~/.local/bin/litellm`.
The upstream key never enters these files; the client authenticates to the local proxy with a 0600 token at
`~/.codex/lithosai/proxy.key`.

Removal: `launchctl bootout gui/$(id -u)/com.nv.lithosai-codex-bridge`, then delete the plist, the profile file, and
`~/.codex/lithosai/`. `uv tool uninstall litellm` removes the bridge runtime.

## Reproduction commands

```bash
# Catalog
curl -sS https://api.lithosai.cloud/v1/models -H "Authorization: Bearer $LITHOSAI_API_KEY"

# Chat Completions (note reasoning_content and how much of a small max_tokens it consumes)
curl -sS https://api.lithosai.cloud/v1/chat/completions \
  -H "Authorization: Bearer $LITHOSAI_API_KEY" -H "Content-Type: application/json" \
  -d '{"model":"moonshotai/Kimi-K3","messages":[{"role":"user","content":"Reply with the single word: ping"}],"max_tokens":32}'

# Proves there is no Responses endpoint
curl -sS -o /dev/null -w '%{http_code}\n' https://api.lithosai.cloud/v1/responses \
  -H "Authorization: Bearer $LITHOSAI_API_KEY" -H "Content-Type: application/json" -d '{"model":"moonshotai/Kimi-K3","input":"ping"}'   # 404

# End-to-end through the local Codex profile (bridge must be running)
codex exec -p lithosai --json --skip-git-repo-check -o /tmp/lithos-last.txt \
  "Reply with exactly: lithosai-bridge-ok"
```

Observed 2026-09-23: the `/v1/models` list returned eight ids; the Chat Completions call returned `200` with
`reasoning_content` populated and `reasoning_tokens: 31`; the `/v1/responses` probe returned `404`; the Codex run
returned `lithosai-bridge-ok` in 3.9s with `rc=0`, and its session file recorded `model_provider: "lithosai"`,
`model: "moonshotai/Kimi-K3-ultra"` and `model_context_window: 996147` (1,048,576 times 0.95, which also proves the
custom catalog was in force rather than the unknown-slug fallback). A second, non-default catalog slug
(`deepseek-ai/DeepSeek-V4.1-Flash-ultra-chat`) and a shell tool call through the bridge were also verified. Session
evidence: `~/.codex/sessions/2026/09/23/rollout-2026-09-23T06-34-39-01a0cdd4-f1d7-79d1-a083-883273255b05.jsonl`; bridge
logs: `~/.codex/lithosai/bridge.stdout.log`.

## Open questions for the integrator

- The account's actual rate-limit capacities are behind the console Limits page; read them before choosing concurrency
  defaults.
- The console lists two figures per input/output rate for the DeepSeek tiers. Confirm whether that is a list/discount
  pair or a cached/uncached pair before wiring cost math.
- Only the seven `reasoning_effort` level names were probed; `summary`-style values were not, so the adapter should
  reject or clamp anything outside the level enum.
- Whether LithosAI intends native Responses support is unknown; their guide frames the proxy as a compatibility path, so
  do not assume it is coming.
- Only two of the eight ids were exercised end to end through Codex; the other six are verified to exist and to be
  registered by the bridge, but their translation behavior is untested.
