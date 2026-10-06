# DeepSeek V4.1 Flash provider waterfall benchmark

Measures the candidate provider-level fallback order for DeepSeek V4.1 Flash subagents through this gateway under one
frozen corpus: cost, speed and reliability, then a weighted ranking and a waterfall recommendation with failure-domain
reasoning.

Scope for this pass (user restriction): objective sections 1 (frozen corpus), 3 (speed), 4 (cost), 5 (reliability), 10
(ranking), 11 (waterfall adjustment) and 14 (decision). Cache protocol, the three-window temporal matrix, the
concurrency ladder, the deterministic correctness suite and the hard qualification-gate section are out of scope; a
light concurrency probe and spaced availability batches still run because section 14 and section 5 ask for them.
`DESIGN.md` is the authority; this file is the operational index.

## Candidates

| Provider             | Model id                          | Wire      | Pinned selection |
| -------------------- | --------------------------------- | --------- | ---------------- |
| Surplus Intelligence | `deepseek-v4.1-flash`             | responses | `["surplus"]`    |
| OpenLux              | `deepseek-v4.1-flash`             | chat      | `["openlux"]`    |
| LithosAI             | `deepseek-ai/DeepSeek-V4.1-Flash` | responses | `["lithos"]`     |
| DeepSeek direct      | `deepseek-flash`                  | responses | `["deepseek"]`   |
| OpenRouter           | `deepseek/deepseek-v4.1-flash`    | responses | `["openrouter"]` |

## Runbook

1. Start the isolated benchmark gateway instance (loopback 7998, isolated KV `.data/benchmark/kv.sqlite3`, same `.env`
   credentials as production; it never touches the Mac gateway on 7999):

```sh
deno run --unstable-kv --env-file=$REPO/.env --allow-env --allow-net --allow-sys=hostname \
  --allow-read=$REPO,$REPO/.codex,$TMPDIR --allow-write=$REPO/.data \
  benchmarks/provider-waterfall/instance.ts --disable-admin-auth
```

Before the first use, seed the isolated KV once so the paid pricing and local principal exist:
`sqlite3 $REPO/.data/kv.sqlite3 "VACUUM INTO '$REPO/.data/benchmark/kv.sqlite3'"`.

2. Verify the frozen corpus (never reads sessions):

```sh
deno run --allow-read=benchmarks/provider-waterfall benchmarks/provider-waterfall/corpus-build.ts verify
```

3. Run one provider batch (pins the selection for the batch, records one JSONL row per attempt, retries
   transport/429/5xx failures once):

```sh
deno run --allow-env --allow-net --allow-read=$PWD --allow-write=$PWD/benchmarks/provider-waterfall/results \
  benchmarks/provider-waterfall/runner.ts --provider=lithos --period=w1-eve --batch=1 --limit=15
```

`--offset=N`, `--limit=N`, `--reverse=true` slice the frozen order; `--pilot` runs a one-request smoke per provider.

4. Derive metrics and scores:

```sh
deno run --allow-read=benchmarks/provider-waterfall/results --allow-write=benchmarks/provider-waterfall/results benchmarks/provider-waterfall/metrics.ts
deno run --allow-read=$PWD --allow-write=benchmarks/provider-waterfall/results benchmarks/provider-waterfall/score.ts
```

`metrics.json` carries per-provider latency percentiles, success/retry rates, usage and failure taxonomy; `scores.json`
and `scores.md` carry the normalized cost/speed/reliability scores and the comparison table. Periods prefixed `probe`
are excluded from scoring and serve as availability/concurrency evidence.

5. Reproduce the rate card:

```sh
deno run --env-file=$REPO/.env --allow-env=SURPLUS_API_KEY,METERED_API_KEY,OPENROUTER_API_KEY \
  --allow-net=api.surplusintelligence.ai,api.openlux.ai,openrouter.ai \
  --allow-read=$PWD --allow-write=benchmarks/provider-waterfall/cost benchmarks/provider-waterfall/cost-probe.ts
```

## Artifacts

- `corpus/corpus-v1.jsonl` + manifest — frozen corpus (sha256 `6aa1248d...`), built from real recorded DeepSeek subagent
  sessions; `corpus-build-report.md` records selection, sanitizer counts and limits.
- `fixtures/codex-tools-0.160.1.json` — frozen Codex tool array used verbatim for every entry.
- `results/` — raw attempt rows (`<provider>-<period>-b<batch>.jsonl`), run metadata, batch logs, `metrics.json`,
  `scores.json`, `scores.md`.
- `cost.ts`, `cost-evidence.md`, `cost/price-snapshot-*.json` — versioned rate cards, observed-billing sources and raw
  evidence.
- `DESIGN.md` — methodology, module contracts and integration addendum.
- Report: `../../../docs/deepseek-v4.1-flash-provider-waterfall-benchmark-2026-10-06.md` (final decision, comparison
  table, exclusions).

## Known limitations

- The six `small` corpus entries are the closest recorded requests (18.7k-18.8k tokens); no sub-agent request below that
  exists in the harvested sessions.
- OpenLux can only serve the chat wire for this model, so its rows are chat-wire; every row records the wire and model
  id.
- Surplus refused every request before the operator funded the account mid-pass (402 insufficient USDC balance); after
  funding it completed the full corpus with 30/30 success, so its rows are measured results and the pre-funding failures
  are retained as probe evidence.
- The frozen tools array is larger than some recorded sessions' own toolsets; sizes are comparable across providers, not
  byte-identical to each session's original request.
