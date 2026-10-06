# DeepSeek V4.1 Flash provider waterfall benchmark — design

Objective: determine the production provider-level fallback order for DeepSeek V4.1 Flash subagents by measuring every
candidate provider through this gateway under one frozen corpus, then ranking cost, speed and reliability.

Scope: the user restricted this pass to objective sections 1 (frozen corpus), 3 (speed), 4 (cost), 5 (reliability), 10
(ranking), 11 (waterfall adjustment) and 14 (decision). Cache protocol (2), the three-window temporal matrix (6), the
concurrency ladder (7), the deterministic correctness suite (8) and the hard qualification-gate section (9) are out of
scope for this pass; concurrency evidence appears only as a single light probe recorded for the section-14 report, and
no provider is formally excluded by section-9 gates except where it cannot serve the workload at all.

## Lane identity

Goal handoff P: `/Users/nv/.codex/attachments/aa1725e4-d2c9-4544-a4bb-507d0738e0ea/pasted-text-1.txt`.

goal_slug `pasted-text-1`, gid10 `94f770bf51`, goal lane `pasted-text-1-g94f770bf51`, branch
`codex/pasted-text-1-g94f770bf51`, base `cdd1be36a2189a3fd366d783a0327606d02dab67`.

Module lanes are recorded in the module contracts below; module workers return uncommitted files in their own lane and
the primary integrates reviewed content into the goal lane. Only the primary commits.

## Candidates and addressing

The gateway routes by client model id; Surplus and OpenLux share the paid catalogue id `deepseek-v4.1-flash`, so each
paid tier is pinned by narrowing the gateway provider selection to exactly that provider while its batch runs. All runs
execute against a dedicated loopback gateway instance (code release `.data/releases/<current>`, port 7998, isolated KV
`.data/benchmark/kv.sqlite3`) so no user traffic is affected. The dedicated instance shares the production `.env`
upstream credentials; this is deliberate because the question is provider behavior, not credential isolation.

| Provider             | Model id                          | Wire      | Pinned selection | Expected `x-uos-upstream` |
| -------------------- | --------------------------------- | --------- | ---------------- | ------------------------- |
| Surplus Intelligence | `deepseek-v4.1-flash`             | responses | `["surplus"]`    | `surplus`                 |
| OpenLux              | `deepseek-v4.1-flash`             | chat      | `["openlux"]`    | `metered`                 |
| LithosAI             | `deepseek-ai/DeepSeek-V4.1-Flash` | responses | `["lithos"]`     | `lithos`                  |
| DeepSeek direct      | `deepseek-flash`                  | responses | `["deepseek"]`   | `deepseek`                |
| OpenRouter           | `deepseek/deepseek-v4.1-flash`    | responses | `["openrouter"]` | `openrouter`              |

OpenLux currently advertises only `/v1/chat/completions` for `deepseek-v4.1-flash`, so it cannot serve the Responses
wire that subagent traffic uses; its runs use the chat wire with the same semantic content and the wire is recorded on
every row. A single forced Responses attempt against OpenLux is recorded as compatibility evidence.

## Frozen corpus

Source: real recorded Codex/DSH subagent sessions under `/Users/nv/.codex/sessions` whose turn_context model is a
DeepSeek V4.1 Flash id (`deepseek-ai/DeepSeek-V4.1-Flash*`, `deepseek-flash`, `deepseek-v4-flash*`). Each entry is one
request reconstructed from the session's recorded `response_item` sequence up to a natural generation boundary (a user
message or a tool result), dropping `reasoning` items, `web_search_call` items and any provider-specific encrypted
payload fields that cannot be replayed. Secrets are removed by the sanitizer described below. The tools array is the
frozen capture `fixtures/codex-tools-0.160.1.json` (sha256
`e48b8f1e1533e676ba43f1f15660a8f5e35d2a775c607b94cb4d46a2116e29ab`), captured from codex-cli 0.160.1 against the
loopback gateway on 2026-10-06; the same array is used for every entry and every provider.

Classes by recorded input tokens (tools included): small 2k–10k (20%), medium 10k–40k (30%), large 40k–160k (30%),
xlarge 160k–600k (20%). Target 30 entries: 6 small, 9 medium, 9 large, 6 xlarge. Every entry preserves the recorded
system/developer instructions, user messages, prior assistant/tool messages, prompt size class, max output tokens,
reasoning effort and tool configuration; prompts are never edited to accommodate a provider.

Corpus artifacts: `corpus/corpus-v1.jsonl` (one `CorpusEntry` per line) and `corpus/corpus-v1.manifest.json` (counts by
class, content hash, source session ids, sanitizer revision, tools fixture hash). The content hash is the sha256 of the
JSONL bytes; the runner verifies it before every batch.

Sanitization: strip values matching credential shapes (`sk-…`, `lith_sk_…`, `gAAAAA…` Fernet blobs, `Bearer …`,
`xoxb-…`, `ghp_…`, AWS keys, `UOS_AI_TOKEN`-style assignments) and `Authorization:` header values; redact the user's
email addresses and absolute home paths only when they appear inside credential-adjacent strings; everything else is
preserved verbatim, including tool outputs, because prompt size and content classes are part of the workload. The
sanitizer must be deterministic and idempotent; the manifest records how many substitutions each rule made.

## Wire projection

Responses entries are sent verbatim (`input`, `tools`, `tool_choice: "auto"`, `parallel_tool_calls: true`,
`reasoning.effort`, `max_output_tokens`, `store: false`, `stream: true`) with only the `model` field swapped per
provider. Chat-wire entries project the same content: every message keeps its text and order, `developer` maps to
`system`, tool calls and outputs map to `assistant.tool_calls` / `tool` messages, and `tools` map to chat function
specs. The projection is deterministic and implemented once in `client.ts`; a projection audit asserts the projected
chat request carries the same ordered text content as the Responses request.

## Measurements

Per attempt the runner records: request start, HTTP response headers, first byte, first SSE/NDJSON event, first output
delta, first completed output item, stream terminal event, completion, HTTP status, `x-uos-upstream`, provider request
id, wire, model, usage (input, cached input, cache write, output, reasoning, total), output characters, tool-call count,
and the terminal outcome with failure classification.

TTFT = first output delta − request start. Generation throughput = output tokens ÷ (completion − first output delta).
End-to-end = completion − request start. The report lists median/P90/P95/P99 for TTFT and throughput and median/P95/P99
for end-to-end latency.

Failure taxonomy (section 5): connection_failure, dns_network_failure, timeout, http_429, http_4xx, http_5xx,
malformed_sse, interrupted_stream, missing_completion, malformed_json, malformed_tool_call, context_length_rejection,
capacity_failure, empty_response, truncated_response, usage_accounting_failure. A request counts successful only when
the complete expected transaction finishes correctly (terminal event observed, no error frame, usage present).

Retry policy: production intent is one retry after a 2-second backoff for transport failures, timeouts, 429 and 5xx; 4xx
is not retried. The runner records the original attempt, then the retry, so first-attempt success rate and retry
recovery rate are separate columns and failures are never hidden.

Availability: the corpus is split into at least two spaced batches per provider (same entries, same order) so a single
burst cannot stand in for availability.

## Cost

Expected cost is computed per request from the provider's published rate card and separated into input, cached input and
output components; observed billed cost is recorded where the provider or the gateway ledger exposes it (Surplus
catalogue metadata and paid-fallback rollups, OpenLux ratio/token logs and rollups, DeepSeek official peak/off-peak rate
card converted to the request's UTC instant, OpenRouter generation endpoint cost, Lithos only if a bounded published
rate is found). Rate cards and sources are versioned in `cost.ts` with retrieval timestamps; discrepancies between
expected and observed cost are flagged per provider.

Effective cost per request and per 1M output tokens use the frozen corpus token/cache distribution; the corpus-weighted
effective cost is the primary cost metric. Dynamic-pricing marketplaces use observed billing as authoritative.

## Scoring

Among candidates that can serve the workload, each dimension is normalized to 0–100 (min–max across candidates; lower
cost, lower latency and higher reliability score higher; identical values tie at 100). Overall = 0.50 × cost + 0.30 ×
speed + 0.20 × reliability, where speed = 0.50 × median E2E + 0.25 × P95 E2E + 0.25 × median throughput, and reliability
= first-attempt success rate with P95/P99 tail failures and the light concurrency probe folded in as evidence rather
than a separate weighted term.

## Waterfall decision

Position 1 is the highest-scoring candidate. Later positions maximize failure independence from earlier positions, using
observable evidence: identical upstream error strings or request-id shapes, shared marketplace resale where visible, and
the compatibility finding for OpenLux. The report states the reason for every adjacent transition, lists exclusions,
billing surprises, variance and shared-failure-domain evidence, and states whether the primary changes under the light
concurrency probe.

## Module contracts

m1-corpus (lane `pasted-text-1-m1-corpus-a<aid10>`): owns `corpus-build.ts`, `corpus/`; produces `corpus-v1.jsonl`,
`corpus-v1.manifest.json`, `corpus-build-report.md`; must run the sanitizer self-check and a parse/verify pass; returns
uncommitted files.

m2-cost (lane `pasted-text-1-m2-cost-a<aid10>`): owns `cost.ts`, `cost-probe.ts`, `cost/`; produces the rate-card module
with exports `expectedCost(provider, usage, atMs)` and `observedCostSource(provider)`, a live probe that records the
current Surplus/OpenLux/DeepSeek/OpenRouter/Lithos price evidence with retrieval timestamps, and `cost-evidence.md`;
must not spend on paid inference beyond bounded metadata fetches (no completion requests); returns uncommitted files.

Primary owns everything else: `instance.ts`, `client.ts`, `runner.ts`, `metrics.ts`, `score.ts`, `providers.ts`,
`types.ts`, `README.md`, results and the report.

## Integration addendum (2026-10-06)

- The tools fixture hash in this document is `e48b8f1e1533e676ba43f1f15660a8f5e35d2a775c607b94cb4d46a2116e29ab`: the
  repository's deno-fmt pre-commit hook normalized the raw capture at commit `ccbcc364`, so the committed bytes differ
  from the pre-commit copy while the JSON semantics are unchanged.
- Frozen corpus: `corpus/corpus-v1.jsonl`, 30 entries, sha256
  `6aa1248dac25135f6fe2dfe8444f33f8f357d369940d296cd692b68ba1cdb288`, class counts 6/9/9/6.
- The recorded candidate pool contains no request below 18.7k input tokens, so the six small entries are the closest
  recorded requests (18.7k-18.8k); the corpus spans 18.7k-598k recorded tokens. The frozen tools array is added to every
  entry, so actual sizes differ from the recorded value by the toolset delta.
