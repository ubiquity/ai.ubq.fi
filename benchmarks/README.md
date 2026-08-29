# Benchmark foundation (m02)

Deterministic, hermetic task benchmark for the Cerebras GPT-OSS Harmony agent foundation. Owned by worker
`m02-benchmark-foundation-16ed155973`; the shared result contract in [`schemas.ts`](./schemas.ts) is the agreement point
for the adapter module (`m03`), the canonical tool layer (`m04`) and the reliability layer (`m05`).

Everything here is local and deterministic by default. The bundled `reference` adapter replays recorded trails, so **no
external inference runs unless an adapter is registered that deliberately calls a model, and the runner refuses to run
external-inference adapters until an approved live gate exists (m03/m05)**.

## Commands

From the repository root:

```sh
deno task benchmark:run -- --configs=reference                 # hermetic default: all 25 tasks
deno task benchmark:run -- --configs=reference --tasks=nav-*   # selection by id/glob/category
deno task benchmark:run -- --configs=reference --tasks=category:long --limit=2
deno task benchmark:summary                                    # aggregate benchmark-runs (no inference)
deno task benchmark:summary -- --runs=some-other-root --json
deno task benchmark:fixture-revision                           # prove declared fixture revisions
deno task benchmark:test                                       # focused hermetic test suite
```

Run artifacts (`benchmark-runs/`, git-ignored):

```
benchmark-runs/
  runs/<run_id>/trajectory.jsonl   # one validated event per line
  runs/<run_id>/result.jsonl       # one BenchmarkResult record
  summary.json                     # written by the summarize command
  tmp/                             # disposable fixture workspaces (removed after each run)
```

The runner accepts `--` task separators exactly as `deno task` forwards them; plain
`deno run --allow-read --allow-write=benchmark-runs --allow-run=sh,git
benchmarks/runner.ts --configs=reference` works
identically. No environment variables are read and no secrets are involved.

## Layout

```
benchmarks/
  schemas.ts          # shared result contract: manifest / trajectory / result / summary
  manifest.ts         # manifest loading + selection (id, glob, category:<name>)
  fixture.ts          # disposable workspaces, fixture revisions, write-scope enforcement
  adapter.ts          # adapter contract, tool layer, reference (scripted) adapter
  oracle.ts           # deterministic verification + file/git oracles
  metrics.ts          # metric derivation + aggregation + formatting
  runner.ts           # CLI: run selected configs
  summarize.ts        # CLI: summarize existing JSONL without any adapter execution
  fixture-revision.ts # CLI: verify declared fixture revisions
  tasks/*.json        # 25 deterministic task manifests (5 per category)
  fixtures/<id>/…     # disposable fixture snapshots (snapshots; regenerable)
  tools/gen-fixtures.ts # regenerates fixtures and prints content revisions
  tests/*.test.ts     # focused hermetic tests (31)
```

## Shared result contract

`BENCHMARK_SCHEMA_VERSION = "1.0"`. Adapters A/B/C/D must write this exact shape; m05 builds reliability detection on
top of it.

### Task manifest (`tasks/*.json`)

Each task declares: `id`, `category` (`navigation | coding | sequential |
failure | long`), `title`, `description`,
`fixture` (snapshot dir), `fixture_revision` (`sha256:<hex>`, validated against the snapshot on every run),
`timeout_ms`, `max_tool_calls`, `min_tool_calls`, `min_model_calls`, `allowed_write_scope` (globs, `!` negates),
optional `git` (`init`/`history`), `verify.command`, `oracle.file_checks` / `oracle.git_checks`, and an optional
`scripted_trail`.

Success requires all of:

1. no run-level failure class (see below),
2. `verification.command` exits 0 within its timeout,
3. every declared oracle check passes (file content and/or git state),
4. recorded tool calls ≥ `min_tool_calls` and model calls ≥ `min_model_calls`.

The corpus contains 25 tasks: 5 navigation, 5 coding, 5 sequential-tool, 5 injected-failure, and 5 long-horizon (four
tasks exceed 20 tool calls, one exceeds 10).

### Trajectory events (`trajectory.jsonl`, one JSON object per line)

```ts
{ type: "run", at, run_id, task_id, category, config_id, adapter_id, fixture, fixture_revision }
{ type: "model_request", at, id, model, message_count, input_tokens, output_tokens, tool_count }
{ type: "model_response", at, request_id, content?, tool_calls?[], finish_reason? }
{ type: "tool_call", at, id, tool, arguments, valid, invalid_reason?, is_wrong_tool?, is_repeated? }
{ type: "tool_result", at, id, ok, output?, error?, error_code?, duration_ms? }
{ type: "verify", at, command, passed, exit_code?, timed_out, output? }
```

Contract rules for adapters:

- record `run` as the first event (the runner does this),
- for every **attempted** tool invocation record a `tool_call` event followed by exactly one `tool_result` with the same
  `id`, including invalid calls (`valid: false`) and wrong-tool calls (`is_wrong_tool: true`),
- token fields on `model_request` are authoritative for `input_tokens` / `output_tokens` / `context_size`,
- never decide success yourself: the runner always runs the declared verification and oracle, so failures keep their
  full trajectories.

### Result record (`result.jsonl`)

`schema_version`, `run_id`, `task_id`, `config_id`, `adapter_id`, `fixture`, `fixture_revision`, `started_at`,
`ended_at`, `wall_time_ms`, `success`, `failure_class`
(`timeout | adapter_error | tool_call_limit |
min_calls_not_met | verification_failed | fixture_revision_mismatch`),
`failure_detail`, `metrics`, `verification`, `oracle`, `required_calls`, `trajectory`, `created_at`.

`metrics` is derived from the event stream and contains: `model_calls`, `tool_calls`, `invalid_tool_calls`,
`wrong_tool_calls`, `repeated_calls`, `tool_errors`, `recovery_attempts`, `input_tokens`, `output_tokens`,
`context_size` (largest request input+output).

Derivation rules (deterministic, see `metrics.ts`):

- `invalid_tool_calls`: `tool_call.valid === false` calls, plus `tool_result.error_code === "invalid_args"` results,
- `repeated_calls`: explicitly flagged calls plus consecutive identical (tool, canonical arguments) calls whose
  predecessor **succeeded** (retries after failures count as recovery, not duplication),
- `recovery_attempts`: distinct failed call ids later followed by a successful call of the same tool.

## Adapter contract (m03/m04/m05)

```ts
interface BenchmarkAdapter {
  configId: string; // stable result id: A, B, C, D, ...
  name: string;
  description: string;
  requiresExternalInference: boolean; // refused by the runner when true
  run(ctx: AdapterRunContext): Promise<void>;
}
```

`AdapterRunContext` provides `runId`, `task`, `workspace`, `record(event)`, `checkToolLimit()`, `signal` (whole-run
timeout), `time()`. Register adapters in `defaultAdapters()` (`adapter.ts`); the runner refuses
`requiresExternalInference: true` adapters with a clear error until an approved live-inference gate exists. `m05`
decides failure-classes and reliability semantics; the runner only classifies terminal causes.

## Fixtures, revisions and write scope

- Writes never leave the repository: the runner copies `benchmarks/fixtures/<id>` into `benchmark-runs/tmp/<run_id>` (a
  disposable directory; removed after the run) and the adapter may write only there.
- `allowed_write_scope` globs are enforced by the workspace tool layer (`write_scope` error code); paths are resolved
  inside the workspace root. `shell.exec` runs unsandboxed inside the disposable workspace by design.
- `fixture_revision` is a SHA-256 over sorted relative paths + contents (prefix `fixture-v1`).
  `benchmark:fixture-revision` proves all declared revisions match the snapshots; the runner fails a run on mismatch
  before the adapter executes.
- `gen-fixtures.ts` regenerates only `benchmarks/fixtures/` and prints the current revision for every task; never run it
  against other directories.

## Deterministic oracles and verification

`verify.command` runs via `sh -c` inside the workspace with its declared timeout (default 20s). Oracle checks:

- `file_checks`: `exists | equals | contains | regex` (with `invert`),
- `git_checks`: `commit_count | head_message | worktree_clean |
  file_committed` — fail closed when the task declares
  no git repository.

Git fixtures (`nav-005`, `seq-004`) initialize a repository rooted inside the disposable workspace, so git never touches
the surrounding checkout. History fixtures (`nav-005`) commit full-tree snapshots in order as the repository history;
the verification script ships inside the final snapshot.

## Hermetic testing

`benchmark:test` runs 31 focused tests: schema validation, manifest corpus revision proof, oracle pass/fail/timeout,
runner classification (timeout/tool-limit/min-calls/verification/revision mismatch), external adapter refusal, metric
derivation, aggregation, and a full 25-task reference matrix asserting every task succeeds and the injected-failure
metric profile (e.g. `fail-001` has exactly 1 tool error + 1 recovery, `long-003` 2 + 2).
