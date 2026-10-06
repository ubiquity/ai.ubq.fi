# DeepSeek V4.1 Flash provider waterfall benchmark — 2026-10-06

Final for the operator-restricted scope (objective sections 1, 3, 4, 5, 10, 11, 14). All numbers are measured from the
frozen corpus through an isolated instance of the production gateway (`benchmarks/provider-waterfall/`); no time-of-day
windows are part of the design.

## Decision

Recommended provider-level waterfall for DeepSeek V4.1 Flash subagents:

**OpenRouter → LithosAI → DeepSeek direct → Surplus Intelligence**

This order is codified as the gateway-owned `ubiquity/deepseek-v4.1-flash` synthetic model — automatic provider fallback
with per-hop evidence — so agents address one model id and the gateway walks the measured order on their behalf; see
`docs/deepseek-waterfall-model-plan.md` for the implementation and its live hop evidence.

- **OpenRouter (`deepseek/deepseek-v4.1-flash`), primary.** Highest overall score (88.3): lowest measured effective cost
  per request ($0.0064), fastest TTFT (643 ms median), best prompt-cache accounting (74.3% cache-hit tokens) and 30/30
  first-attempt success. Observed serving path: `deepseek/deepseek-v4.1-flash-20260910` routed by OpenRouter to upstream
  provider **Together** in all 34 generations, so it is not DeepSeek-direct capacity.
- **LithosAI (`deepseek-ai/DeepSeek-V4.1-Flash`), secondary.** Near-identical economics ($0.0065/request, score 75.9) on
  a fully independent serving stack (`api.lithosai.cloud`) with 30/30 first-attempt success. If the OpenRouter
  marketplace or its Together upstream degrades, Lithos shares no observed serving path with it.
- **DeepSeek direct (`deepseek-flash`), tertiary.** The authoritative model source (`api.deepseek.com`, documented as
  the serving name for DeepSeek-V4.1-Flash), 30/30 success, 49.0% cache hits and 3029 ms median end-to-end, but 1.6x the
  effective cost of the top two at peak pricing ($0.0101). As the canonical upstream it is the strongest independence
  tier behind the two aggregators.
- **Surplus Intelligence (`deepseek-v4.1-flash`), final tier.** After the operator funded the account mid-benchmark it
  served 30/30 first-attempt successes at $0.0094/request (score 54.8) with the highest median generation throughput
  (365.6 tok/s) but the worst tail latency (p95 84 s, p99 102 s; for example `xlarge-01` took 101 s for 1,865 output
  tokens). Usable as a last-resort tier; its upstream serving path is not exposed by the marketplace, so its failure
  independence is unknown.

**Excluded: OpenLux.** The `deepseek-v4.1-flash` record from OpenLux advertises only `/v1/chat/completions`, so the
gateway cannot select it for the `/v1/responses` wire that subagent traffic uses. Its unadvertised `/v1/responses` route
answered HTTP 200 in a direct probe (two correct trivial completions) but also returned one completion unrelated to its
prompt, so it is excluded pending provider support rather than wired in on an undocumented route. Its chat-wire
measurement (75.9% slower TTFT than OpenRouter, $0.0149/request, one recovered 504) is recorded alongside.

## Scope and method

- Scope: corpus (1), speed (3), cost (4), reliability (5), ranking (10), waterfall adjustment (11), decision (14). Cache
  protocol, multi-window temporal matrices, the concurrency ladder, the deterministic correctness suite and the hard
  qualification gates were excluded by the operator; a light 4-way concurrency probe is included because section 14 asks
  whether the primary changes under concurrency, and Surplus funding was completed mid-run so its full corpus was
  measured.
- Corpus: 30 frozen entries rebuilt from real recorded DeepSeek subagent sessions (developer/user messages, tool
  outputs, reasoning effort and generation boundaries preserved), secrets sanitized (2 Fernet blobs and 1 `sk-` key
  redacted; zero residual credential patterns), sha256
  `6aa1248dac25135f6fe2dfe8444f33f8f357d369940d296cd692b68ba1cdb288`, class counts 6/9/9/6. The recorded pool had no
  request below 18.7k input tokens, so `small` entries are the closest recorded requests.
- Tools: the frozen codex-cli 0.160.1 tool array captured from a real loopback request
  (`fixtures/codex-tools-0.160.1.json`, sha256 `e48b8f1e...`), used verbatim for every entry and provider.
- Routing: every request went through an isolated instance of the production release (`3691fe4a...`) on loopback:7998
  with its own KV; the paid tiers were pinned per batch by provider selection (`["surplus"]`, `["openlux"]`,
  `["lithos"]`, `["deepseek"]`, `["openrouter"]`). The production Mac gateway on 7999 was never modified.
- Wires: Responses for Surplus, Lithos, DeepSeek and OpenRouter; OpenLux only advertises chat for this model so its runs
  used `/v1/chat/completions` with the same semantic content. Every row records the wire and model id.
- Measurements: client-side stage timestamps (headers, first byte, first event, first output, terminal event,
  completion), provider-reported usage, HTTP status, `x-uos-upstream`, response ids. Success requires the complete
  expected transaction; failures are classified in the section-5 taxonomy. Retry policy: one retry after 2 s for
  transport/timeout/429/5xx; first-attempt success and retry recovery are reported separately and failures are never
  hidden.

## Results — 30 requests per provider plus concurrency probe

| Rank | Provider             | Wire      | Effective cost/req | Corpus cost (30 req) | TTFT P50/P95  | TPS P50     | E2E P50/P95   | First-attempt success | Cache hit | Cost/Speed/Reliability | Overall               |
| ---- | -------------------- | --------- | ------------------ | -------------------- | ------------- | ----------- | ------------- | --------------------- | --------- | ---------------------- | --------------------- |
| 1    | OpenRouter           | responses | $0.0064            | $0.1924              | 643/1181 ms   | 314.3 tok/s | 2019/57304 ms | 30/30                 | 74.3%     | 100.0/60.9/100.0       | **88.3**              |
| 2    | LithosAI             | responses | $0.0065            | $0.1946              | 3801/6393 ms  | 289.2 tok/s | 5916/56144 ms | 30/30                 | 30.6%     | 99.1/21.2/100.0        | 75.9                  |
| 3    | DeepSeek direct      | responses | $0.0101            | $0.3042              | 1183/2089 ms  | 254.8 tok/s | 3029/42465 ms | 30/30                 | 49.0%     | 56.0/55.7/100.0        | 64.7                  |
| 4    | Surplus Intelligence | responses | $0.0094            | $0.2806              | 1995/7176 ms  | 365.6 tok/s | 6313/84157 ms | 30/30                 | 37.7%     | 65.3/7.2/100.0         | 54.8                  |
| 5    | OpenLux              | chat      | $0.0149            | $0.4465              | 6708/18546 ms | 2728 tok/s  | 6909/18976 ms | 29/30                 | 3.6%      | 0.0/50.0/98.0          | 34.6 (excluded: wire) |

Latency detail: median/P90/P95/P99 per provider is in `results/metrics.json`. OpenLux's single first-attempt failure was
a 504 pre-header gateway timeout on the 943-item `xlarge-04` request, recovered by the retry (100% retry recovery).
OpenLux's and part of OpenRouter's TPS medians are distorted by burst delivery (tokens arriving in a late burst) — both
also have the highest TTFTs, which the speed score reflects; OpenRouter keeps the lowest median end-to-end latency.

### Reliability

- Failures observed across the whole pass: one `http_5xx` (OpenLux xlarge-04, retry recovered) and the pre-funding
  Surplus `capacity_failure` probes (402 `insufficient_balance`; exact message: "Insufficient USDC balance: need
  ~$1.0000, have $0.9895").
- No connection, DNS, timeout, 429, malformed SSE, interrupted stream, missing completion, malformed JSON, malformed
  tool call, context-length, empty/truncated response or usage-accounting failures occurred on any provider.
- Retry recovery rate: 1/1 (OpenLux). First-attempt success: 30/30 for four providers, 29/30 for OpenLux.
- P99 end-to-end: OpenRouter 103 s, DeepSeek 111 s, Surplus 102 s, OpenLux 87 s, Lithos 72 s; the tails are dominated by
  long generations and long-context turns rather than transport faults.

### Cost and billing evidence

- **Surplus observed vs expected match exactly**: the gateway's settled paid ledger reports 291,478 micro-USD
  (`spend_microcredits`) for 35 settled requests (30 corpus + 4 concurrency probe + 1 funded probe); the rate-card
  computation over the same rows is 291,473 micro-USD — a 5 micro-USD rounding difference. Surplus observed billing is
  authoritative and confirms the catalogue card (prompt 3.0e-7, completion 1.2e-6, cache read 6.0e-9, cache write 3.0e-7
  USD/token).
- **OpenLux discrepancy**: the settled ledger reports 358,864 micro-USD for 56 requests (~6,408/request) while the
  catalogue-ratio expectation over the recorded rows is ~14,100/request. The quota-to-USD conversion for the marketplace
  cannot be fully reconciled from the public surfaces; the ledger figure is authoritative and the expected number may
  overstate OpenLux cost by roughly 2x.
- **OpenRouter material surprise**: the public generation endpoint reports `tokens_prompt: 0`, `tokens_completion: 0`,
  `total_cost: 0` for all 34 benchmark generations even after settling, so observed per-generation billing is
  unavailable and expected cost uses the OpenRouter catalogue card; the same endpoint proves the upstream provider
  identity (Together).
- **DeepSeek direct**: rate card validated against the official pricing page (peak $0.30 cache-miss input / $1.20 output
  per 1M, off-peak half; peak 01:00-04:00 and 06:00-10:00 UTC Mon-Fri). The full-corpus run fell in a peak window.
- **LithosAI**: the API publishes no price metadata or token logs; the bounded source is the vendor pricing page (Base
  tier $0.15/$0.003/$0.60 per 1M, early access). Expected-only, flagged as bounded rather than observed.

### Concurrency probe (light, section-14 evidence)

Four corpus entries at concurrency 4 (all 20 requests succeeded): mean per-entry end-to-end ratio vs the same entries at
concurrency 1 — OpenRouter ×2.23 (max ×2.84), Surplus ×2.38 (max ×5.87, the widest single-entry amplification), DeepSeek
×1.25, Lithos ×1.23, OpenLux ×0.95. The ordering does not change under this light load; OpenRouter keeps the lowest
latencies, and Surplus shows the sharpest individual latency amplification.

### Shared upstream / independence evidence

| Provider        | Observed serving path                                                                     | Independence note                                                                                      |
| --------------- | ----------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ |
| OpenRouter      | `deepseek/deepseek-v4.1-flash-20260910` via upstream **Together** (34/34 generations)     | Marketplace route; not DeepSeek-direct capacity. If Together degrades, this route degrades as a whole. |
| LithosAI        | `api.lithosai.cloud` own deployment                                                       | Fully independent serving stack and billing.                                                           |
| DeepSeek direct | `api.deepseek.com` (authoritative model source)                                           | Canonical upstream; independent of every aggregator.                                                   |
| Surplus         | Surplus marketplace; upstream identity not exposed (request ids are Surplus-issued ULIDs) | Unknown failure domain; observed billing settles through the gateway ledger.                           |
| OpenLux         | OpenLux quota system; upstream identity not exposed                                       | Excluded for wire incompatibility.                                                                     |

## Required machine-readable output

`benchmarks/provider-waterfall/results/provider-rows.json` carries one row per provider with the objective's fields
(samples, success and retry rates, TTFT/TPS/E2E median-P95-P99, input/cached/output tokens, cache-hit ratio, actual and
effective cost, normalized scores, exclusion notes), assembled from `results/metrics.json`, `results/scores.json`,
`results/openrouter-observed-costs.json` and the concurrency probe rows, and `results/scores.md` carries the concise
comparison table.

## Reproducibility

Raw attempt rows (`results/*.jsonl` with per-batch `.meta.json` recording gateway release, selection readback and corpus
hash), batch logs (`results/logs/`), rate cards (`cost.ts`, `cost-evidence.md`, `cost/price-snapshot-2026-10-06.json`),
corpus provenance (`corpus/corpus-v1.manifest.json`, `corpus-build-report.md`) and the methodology authority
(`DESIGN.md` plus addendum, `README.md` runbook) all live under `benchmarks/provider-waterfall/`.
