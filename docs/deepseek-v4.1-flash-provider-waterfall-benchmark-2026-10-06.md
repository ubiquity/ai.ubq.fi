# DeepSeek V4.1 Flash provider waterfall benchmark — 2026-10-06

Status: draft pending the w2/w3 availability windows; all w1 numbers in this file are measured from the frozen corpus
through the isolated benchmark gateway (`benchmarks/provider-waterfall/`).

## Decision

Recommended provider-level waterfall for DeepSeek V4.1 Flash subagents:

1. **OpenRouter (`deepseek/deepseek-v4.1-flash`)** — primary: it won every weighted dimension without a failure: lowest
   measured effective cost (tied with Lithos at $0.0064 vs $0.0065 per request), fastest TTFT (643 ms median), best
   prompt-cache accounting (74% cache-hit tokens), and 30/30 first-attempt success.
2. **LithosAI (`deepseek-ai/DeepSeek-V4.1-Flash`)** — secondary: near-identical cost and 30/30 reliability on fully
   independent infrastructure (own deployment at `api.lithosai.cloud`), so it is the natural recovery tier if the
   OpenRouter marketplace or its upstream (today: Together) degrades.
3. **DeepSeek direct (`deepseek-flash`)** — tertiary: the authoritative source for the model identity, 30/30 reliability
   and the best cache ratio among the direct routes, but 1.5-1.6x the effective cost of the top two (peak-window
   pricing) and slower TTFT (1183 ms median). As the canonical upstream it shares no serving path with either earlier
   tier.

Excluded entirely: **Surplus Intelligence** could not serve a single request during the benchmark (HTTP 402
"Insufficient USDC balance: need ~$1.0000, have $0.9895") and **OpenLux** cannot serve the Responses wire that subagent
traffic uses (its model record advertises `/v1/chat/completions` only). Both exclusions are capacity/compatibility
facts, not score placements; Surplus should be re-benchmarked if it is funded, and OpenLux can only become a fallback if
it starts advertising `openai-response` for this model.

## Scope

Per the operator's restriction this pass covers objective sections 1 (frozen corpus), 3 (speed), 4 (cost), 5
(reliability), 10 (ranking), 11 (waterfall adjustment) and 14 (decision). Cache protocol, the three-window temporal
matrix, the concurrency ladder, the deterministic correctness suite and the hard qualification-gate section are out of
scope; a light concurrency probe and spaced availability batches still ran because section 14 and section 5 require
them.

## Method

- **Corpus**: 30 frozen entries rebuilt from real recorded DeepSeek subagent sessions (32k-line sessions under
  `~/.codex/sessions`), preserving recorded developer/user messages, tool outputs, reasoning effort and boundary shape;
  secrets sanitized (3 substitutions: 2 Fernet blobs, 1 `sk-` key; zero residual credential matches). Corpus sha256
  `6aa1248dac25135f6fe2dfe8444f33f8f357d369940d296cd692b68ba1cdb288`; class counts 6/9/9/6 (small/medium/large/xlarge).
  The recorded pool had no request below 18.7k input tokens, so `small` entries are the closest recorded requests.
- **Tools**: the frozen Codex 0.160.1 tool array captured from a real loopback Codex request
  (`fixtures/codex-tools-0.160.1.json`, 23 entries, sha256 `e48b8f1e...`), used verbatim for every entry and provider.
- **Routing**: every request went through an isolated instance of the production release (`3691fe4a...`) on
  loopback:7998 with its own KV; paid tiers were pinned per batch by provider selection (`["surplus"]`, `["openlux"]`,
  `["lithos"]`, `["deepseek"]`, `["openrouter"]`). The production Mac gateway on 7999 was never modified.
- **Wire**: Responses (`/v1/responses`) for Surplus, Lithos, DeepSeek and OpenRouter; OpenLux only advertises chat for
  this model, so its runs use `/v1/chat/completions` with the same semantic content and deterministic projection. Every
  row records wire and model id.
- **Measurements**: client-side stage timestamps (headers, first byte, first event, first output delta, terminal event,
  completion), provider-reported usage, HTTP status, `x-uos-upstream`, response ids. Success requires the complete
  expected transaction; failures are classified in the section-5 taxonomy. Retry policy: one retry after 2 s for
  transport/timeout/429/5xx; first-attempt success and retry recovery are reported separately and failures are never
  hidden.
- **Costs**: versioned rate cards in `cost.ts` with sources and retrieval dates (`cost-evidence.md`), applied to the
  measured input/cached/output token distribution; observed billing is used where a provider exposes it.

## Results (window w1, US evening / Asian morning, 2026-10-06 01:36-01:59 UTC)

| Rank | Provider             | Wire      | Effective cost/req | Corpus cost (30 req) | TTFT P50/P95  | TPS P50    | E2E P50/P95   | Success (1st attempt) | Cache hit | Cost/Speed/Reliability | Overall  |
| ---- | -------------------- | --------- | ------------------ | -------------------- | ------------- | ---------- | ------------- | --------------------- | --------- | ---------------------- | -------- |
| 1    | OpenRouter           | responses | $0.0064            | $0.1924              | 643/1181 ms   | 314 tok/s  | 2019/57304 ms | 30/30                 | 74.3%     | 100.0/50.6/100.0       | 85.2     |
| 2    | LithosAI             | responses | $0.0065            | $0.1946              | 3801/6393 ms  | 289 tok/s  | 5916/56144 ms | 30/30                 | 30.6%     | 99.1/11.3/100.0        | 72.9     |
| 3    | DeepSeek direct      | responses | $0.0101            | $0.3042              | 1183/2089 ms  | 255 tok/s  | 3029/42465 ms | 30/30                 | 49.0%     | 56.0/49.4/100.0        | 62.8     |
| 4    | OpenLux              | chat      | $0.0149            | $0.4465              | 6708/18546 ms | 2728 tok/s | 6909/18976 ms | 29/30                 | 3.6%      | 0.0/50.0/98.0          | 34.6     |
| —    | Surplus Intelligence | responses | n/a                | $0                   | —             | —          | —             | 0/1 (402)             | —         | —                      | excluded |

Notes: the corpus-cost column is what these 30 real-workload requests actually cost on each provider, so it is a
concrete budget figure (heavy long-context entries dominate it). OpenLux's single first-attempt failure was a 504
pre-header gateway timeout on the 943-item `xlarge-04` request, recovered by the retry (100% retry recovery). OpenLux's
and, in part, OpenRouter's TPS medians are distorted by burst delivery (tokens arriving in a late burst); both providers
also show the highest TTFT, so their speed score already reflects the practical latency. OpenRouter keeps the lowest
median end-to-end latency of the four.

### Reliability detail

- Failure taxonomy across w1: one `http_5xx` (OpenLux 504) and one deliberate Surplus `capacity_failure` probe; zero of
  the other fifteen taxonomy classes appeared.
- Before the w1 window, Surplus refused every request (five attempts across pilots and the probe) with 402
  `insufficient_balance`; the recorded message is exact evidence.
- OpenRouter's p95/p99 tail is dominated by its own long generations (57 s p95) rather than transport faults; no
  interrupted streams, no malformed SSE, no missing terminal events occurred on any provider.

### Cost evidence and billing surprises

- **Surplus**: 402 balance $0.9895 vs ~$1.00 required; no measurable spend, so its price card could not be validated
  against observed billing.
- **OpenLux**: billed through its quota system; observed billing surface is the token-log/ledger reconciliation path.
  Its chat-wire requests carried no cache hits (3.6%) and the highest per-request cost.
- **OpenRouter material surprise**: the public generation endpoint reports `tokens_prompt: 0`, `tokens_completion: 0`,
  `total_cost: 0` for all 34 benchmark generations (all routed to upstream provider "Together"), so observed
  per-generation billing is unavailable for this traffic; expected cost uses the OpenRouter catalogue card. This also
  proves the OpenRouter route is not DeepSeek-direct capacity.
- **DeepSeek direct**: rate card validated against the official pricing page (peak $0.30/$1.20, off-peak half, peak
  01:00-04:00 and 06:00-10:00 UTC Mon-Fri); the w1 window ran at peak rates.
- **LithosAI**: API publishes no price metadata; the bounded source is the vendor pricing page (Base tier $0.15/$0.60
  per 1M, early access). No observed billing surface exists on this route.

### Concurrency probe (light, section-14 evidence)

Four corpus entries run at concurrency 4 per provider (all 16 requests succeeded): average end-to-end ratio vs the same
entries at concurrency 1 — OpenRouter ×2.23, DeepSeek ×1.25, Lithos ×1.23, OpenLux ×0.95. The ordering does not change
under this light load (OpenRouter keeps the lowest latencies); OpenRouter amplifies most under concurrency.

### Shared upstream / independence evidence

| Provider        | Observed serving path                                                                 | Independence note                                                                                                                                                 |
| --------------- | ------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| OpenRouter      | `deepseek/deepseek-v4.1-flash-20260910` via upstream **Together** (34/34 generations) | Marketplace route; not DeepSeek-direct capacity. If Together degrades the whole route degrades, so it must not be assumed to be independent of other aggregators. |
| LithosAI        | `api.lithosai.cloud` own deployment                                                   | Fully independent serving stack and billing.                                                                                                                      |
| DeepSeek direct | `api.deepseek.com` (authoritative model source)                                       | Canonical upstream; independent of both aggregators.                                                                                                              |
| OpenLux         | OpenLux quota system (upstream identity not exposed)                                  | Excluded for wire incompatibility.                                                                                                                                |
| Surplus         | Surplus marketplace (no successful request to observe)                                | Unknown serving path; excluded until funded.                                                                                                                      |

## Evidence index

- Raw attempt rows: `benchmarks/provider-waterfall/results/*.jsonl` (+ `.meta.json` per batch with gateway release,
  selection readback and corpus hash).
- Aggregate metrics: `results/metrics.json`; normalized scores: `results/scores.json` and `results/scores.md`.
- OpenRouter generation reconciliation: `results/openrouter-observed-costs.json`.
- Rate cards and sources: `cost.ts`, `cost-evidence.md`, `cost/price-snapshot-2026-10-06.json`.
- Gateway-side batch logs: `results/logs/`.
- Corpus provenance: `corpus/corpus-v1.manifest.json`, `corpus-build-report.md`.

## Availability windows

- **w1** (2026-10-06 01:36-01:59 UTC, US evening/Asian morning): full 30-entry corpus, reported above.
- **w2** (planned 2026-10-06 ~03:15 UTC, Asian daytime): six-entry availability subset across providers.
- **w3** (planned 2026-10-06 ~14:30 UTC, US morning/off-peak pricing): same six-entry subset.

Windows are labeled `w2-asia` / `w3-day` in the raw rows; the final metrics recompute after both windows land.
