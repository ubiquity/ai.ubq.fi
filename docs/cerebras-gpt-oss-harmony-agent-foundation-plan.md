# Cerebras GPT-OSS Harmony Agent Foundation and Bootstrap Progress Plan

## Status and authority

- State: planned
- Role: planning facilitator; the executing primary agent becomes orchestrator after the goal is resumed.
- Repository: `/Users/nv/repos/ubiquity/ai.ubq.fi`
- Base ref: `origin/development`
- Planned base SHA: `dd38c3f873978d1d3858e56dbcac864c41d7293b`
- Canonical worktree:
  `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/cerebras-gpt-oss-harmony-agent-foundation-53c80930e6`
- Canonical branch: `codex/cerebras-gpt-oss-harmony-agent-foundation-53c80930e6`
- Plan path: `/Users/nv/repos/ubiquity/ai.ubq.fi/docs/cerebras-gpt-oss-harmony-agent-foundation-plan.md`
- Plan identity: `53c80930e6`

## Objective

Build an evidence-first Cerebras `gpt-oss-120b` agent foundation that follows OpenAI Harmony semantics, measures
reliability against reproducible baselines, and supplies Provider Sentinel with a bounded progress decision. Obvious
progress and obvious boot loops are decided deterministically. Only ambiguous histories reach one weak-model call. The
model sees no tools and returns one strict boolean field. A model failure is `unknown` and cannot authorize promotion.

This work does not change the Provider Sentinel implementation-agent invariant. Implementation remains Luna-only.
GPT-OSS is a progress classifier and experimental worker harness, never an implementation-model substitute.

## Acceptance surfaces

1. A checked-in protocol probe command emits JSONL results for Cerebras reasoning, tool turns, strictness, structured
   output, and Harmony replay cases.
2. A checked-in benchmark command runs the same task manifest through the existing gateway path, codex-infinity
   baseline, canonical Harmony harness, and a configured strong control adapter. Raw trajectories and summarized metrics
   are reproducible.
3. The canonical harness exposes only `filesystem.read/find/search`, `browser.search/open/find`, `shell.exec`,
   `editor.apply_patch`, and `task.update_plan`, with schema validation and a stable model-facing contract.
4. The reliability layer detects invalid calls, duplicate actions, failed commands, ineffective edits, missing
   verification, stalled state, and false completion. It preserves full failed trajectories.
5. Bootstrap progress evaluation uses run identity, generation, phase or milestone, failure fingerprint, Git SHA, ledger
   version, retry state, and verification evidence. It returns `progress`, `stuck`, or `ambiguous` before any model
   call.
6. The ambiguous classifier sends one bounded, data-only Harmony request to exact model `gpt-oss-120b`, with no tools,
   and requires the assistant's complete trimmed final content to be the literal `true` or `false`. The harness parses
   it with the anchored case-insensitive expression `/^(true|false)$/i`; transport, refusal, any surrounding prose,
   timeout, or model mismatch becomes `unknown`.
7. Bootstrap promotion remains gated by exact immutable revision and full Git SHA health proof. A classifier response
   cannot override an authoritative failure, missing verification, or identity mismatch.
8. Focused tests pass, benchmark artifacts contain the required metrics and classifications, and the final report gives
   a data-supported viability conclusion without forcing a favorable result.
9. Every accepted task branch is merged with visible ancestry into `development`; refreshed local and remote
   `development` match; the repository-root checkout is clean unless an identified owner still holds it.

## Required sources and protocol decisions

Use primary sources and record the exact revision or retrieval date in the research report:

- OpenAI `gpt-oss`: https://github.com/openai/gpt-oss
- OpenAI Harmony format and library: https://github.com/openai/harmony
- codex-infinity: https://github.com/lee101/codex-infinity
- Cerebras Academic Research Agent: https://inference-docs.cerebras.ai/cookbook/agents/academic-research-agent
- Browser-Use Cerebras example and underlying adapter:
  https://github.com/browser-use/browser-use/blob/main/examples/models/cerebras_example.py

Protocol defaults to prove, not assume:

- Preserve analysis across an unfinished tool turn; after a completed final answer, replay normalized final state
  without old private analysis.
- Keep tool-call and tool-result serialization aligned with Harmony roles and channels.
- Normalize all exposed tool definitions to one Cerebras-compatible strictness value.
- Do not combine tools and structured `response_format` unless a protocol probe proves support.
- For the bootstrap classifier, use no tools, one independent request, no structured-output wrapper, and a small output
  limit. Trim the final content and accept only a full-string `true` or `false` match.
- Start classifier experiments with low reasoning and compare medium. Do not send explicit `none`, which the current
  gateway rejects for GPT-OSS.

## Architecture and authority

The decision pipeline is:

1. Build a canonical observation from recent runs and recovery records.
2. Apply deterministic rules.
   - Return `progress` for new durable state, a later verified milestone, a new accepted Git identity, a monotonic
     ledger advance tied to useful work, or a materially different corrective action followed by new evidence.
   - Return `stuck` for a bounded sequence with unchanged source identity, generation, phase, fingerprint, Git SHA,
     ledger version, retry state, and verification outcome, or for a cycle that returns to the same canonical state
     without durable advancement.
   - Return `ambiguous` only when fields change but deterministic evidence cannot establish meaningful advancement.
3. For `ambiguous`, call the GPT-OSS classifier once. Give it only the bounded canonical observation and the decision
   definition. It outputs only `true` or `false`.
4. Convert any classifier failure to `unknown`. `unknown` is fail-closed for promotion.
5. Promotion requires the existing exact-SHA and revision gates plus a positive progress decision. The classifier can
   resolve ambiguity; it cannot override deterministic vetoes.

Use the existing Cerebras transport, error normalization, and model constant where practical, but isolate Harmony state,
benchmark logic, tool execution, and bootstrap classification from `src/openai.ts`. Do not scatter Cerebras exceptions
across unrelated gateway code.

## Benchmark design

Create approximately 25 deterministic tasks across repository navigation, coding, sequential tool use, injected tool
failures, and long trajectories. Include tasks requiring more than 10 and more than 20 tool calls. Each task declares
fixture revision, allowed write scope, verification command, timeout, and success oracle. Use disposable fixture
worktrees or temporary repositories, never the active canonical checkout.

Run four configurations against the same manifests:

- A: current gateway GPT-OSS Chat Completions behavior.
- B: unmodified or minimally wrapped codex-infinity Cerebras support.
- C: canonical Harmony harness and compact tool surface.
- D: the same task contract with a configured strong control model. Keep the control adapter generic until the
  repository has an approved available model; do not add a secret, environment variable, CLI flag, or paid provider
  without owner approval.

Record task success, wall time, model calls, tool calls, invalid and wrong tool calls, repeated calls, tool errors,
recovery, input and output tokens, context size, verification, and failure class. Preserve full trajectories for every
failure. Compare full-transcript replay with structured state at short, medium, and large contexts. Compare broad and
canonical tool surfaces. Persist rate-limited results and never rerun them merely for formatting.

Live provider runs must be bounded and staged: protocol probes first, then a small smoke subset, then the full matrix
only after costs and expected calls are reported. Reuse the existing `CEREBRAS_API_KEY`; do not introduce a new secret
or interface.

## Modules and execution graph

The orchestrator owns this plan, shared schemas, dependency decisions, integration, benchmark release runs, bootstrap
authority changes, and final end-to-end acceptance. Workers are not alone in the repository; each must preserve
unrelated edits and must not revert another lane.

### m01-protocol-probes — `511a869bee`

- Depends on: none.
- Owns: new isolated Harmony/Cerebras adapter and protocol probe files; focused adapter tests.
- Delivers: minimal reproductions for reasoning return/replay, tool call/result shape, mixed strictness, structured
  output with tools, supported reasoning efforts, parallel tools, consecutive tool turns, and native Harmony versus
  generic function calling.
- Must not change bootstrap control authority.

### m02-benchmark-foundation — `16ed155973`

- Depends on: stable result schema agreed with the orchestrator.
- Owns: benchmark manifests, fixtures, JSONL trajectory/result schema, runner, metric aggregation, and deterministic
  task oracles.
- Delivers: one local command that runs selected configurations and one command that summarizes existing JSONL without
  external inference.

### m03-baseline-adapters — `de0bd1341f`

- Depends on: m02 runner contract.
- Owns: A, B, and D adapters and external-project setup notes. External repositories remain pinned test dependencies or
  documented checkouts; do not copy code without recording its license and provenance.
- Delivers: current gateway baseline, codex-infinity baseline, Browser-Use trace notes, and generic strong-control
  adapter.

### m04-canonical-tools — `c92e83ab60`

- Depends on: m01 protocol contract and m02 runner contract.
- Owns: canonical model-facing tool schemas and infrastructure routing for read/find/search, browser search/open/find,
  shell exec, apply_patch, and update_plan.
- Delivers: stable schemas, tool-result envelopes, path/write boundaries, and injected fake backends for deterministic
  tests.

### m05-reliability-context — `d0ece8c381`

- Depends on: m01, m02, and m04.
- Owns: schema validation, concise machine-readable tool errors, duplicate and semantic-loop detection, retry policy,
  verification requirements, structured state, transcript pruning experiments, and failure classification.
- Delivers: canonical configuration C and evidence for broad-versus-compact tools and full-versus-structured context.

### m06-bootstrap-progress — `cdac7f75fe`

- Depends on: proven m01 classifier contract and a passing bounded C smoke benchmark.
- Owns: `scripts/sentinel/bootstrap/**`, its focused tests, state schema changes, and required workflow digest-pin
  changes. It is the sole writer to this protected trust domain.
- Delivers: canonical run observations, pure deterministic verdicts, bounded ambiguous classifier, persisted evidence,
  and integration with promotion policy under the authority rules above.
- Must preserve existing rollback classifications, threshold semantics, CAS state, protected-path enforcement,
  deployment identity proof, and Luna-only implementation policy.

### m07-results-recommendation — `17811d6a47`

- Depends on: accepted benchmark outputs from A/B/C/D and m06 acceptance.
- Owns: research report, comparison report, charts or tables generated from persisted JSONL, and final recommendation.
- Delivers one supported conclusion: primary agent; bounded worker with stronger orchestrator; narrow inference only; or
  insufficient reliability.

Execution order: m01 and m02 may run concurrently after the orchestrator freezes their shared result schema. m03 follows
the m02 contract and may overlap late m01 work. m04 follows m01 and m02. m05 follows m04. m06 begins only after the
classifier protocol and smoke evidence pass. m07 follows accepted benchmark and bootstrap results. Only the orchestrator
integrates branches and runs the full external matrix.

## Verification and failure handling

- Unit tests cover parsers, schema projection, Harmony serialization, duplicate detection, deterministic progress rules,
  ambiguous classification, and fail-closed errors.
- Integration tests use fake transports and fake tools for repeatable multi-turn sequences, invalid arguments, failed
  commands, recovery, edits, and verification.
- Live probes capture sanitized request/response metadata and never print secrets or private reasoning.
- Benchmark tasks cannot claim success from model text. The declared oracle must pass.
- Classifier prompt injection is limited by treating run content as data and by exposing zero tools. Its trimmed final
  output must match `/^(true|false)$/i` in full and has no direct control-plane capability.
- If Cerebras cannot preserve required Harmony semantics, record the exact boundary and retain only the configurations
  that pass protocol probes.
- If the broad benchmark would consume material quota, pause after reporting the smoke result and estimated matrix size.
- Do not terminate or interfere with existing Codex, DSH, tmux, browser, deployment, or workflow processes.

## Git and delivery

Before execution, fetch `origin/development`, create the canonical worktree and branch above from the refreshed base,
and record any base drift in this plan. Derive each module branch from the canonical branch using
`codex/cerebras-gpt-oss-harmony-agent-foundation-53c80930e6-<module>-<aid10>`. Each accepted worker tip must enter the
canonical branch with an ancestry-preserving merge commit. Run focused tests after every integration and the complete
relevant suite before delivery.

Push the canonical branch, open or reuse one pull request into `development`, run the bounded Codex review loop, wait
for required CI and deployment checks, and merge. Fetch `origin/development`; prove every accepted worker tip is an
ancestor; prove local `development` equals `origin/development`; then leave the repository-root checkout clean on
`development`. Production promotion follows the project’s exact revision selection, immutable health, HTTP 204
promotion, and dual-host identity checks. Do not infer promotion from a successful deployment.

## Orchestrator handoff

Resume the Cerebras GPT-OSS Harmony agent foundation from plan
`/Users/nv/repos/ubiquity/ai.ubq.fi/docs/cerebras-gpt-oss-harmony-agent-foundation-plan.md` with canonical worktree
`/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/cerebras-gpt-oss-harmony-agent-foundation-53c80930e6` on branch
`codex/cerebras-gpt-oss-harmony-agent-foundation-53c80930e6`, refresh `origin/development`, preserve all unrelated
state, execute modules m01 through m07 by their dependency graph, and do not integrate bootstrap promotion authority
until the deterministic progress rules and zero-tool literal-boolean Harmony classifier pass their protocol and smoke
acceptance gates.
