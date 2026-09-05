# Provider Sentinel redesign

Status: proposed design, saved September 4, 2026 New York time. No implementation, workflow dispatch, production
inference test, or deployment was performed for this design.

## Objective and success

Make the provider recover from software defects without routine human intervention. A small deterministic bootstrap
protects availability. An upgradeable Sentinel repairs production failures, then completed Codex review findings, then
eligible GitHub issues. Codex review runs independently and never gates delivery.

Success is a real autonomous cycle: a captured defect produces a reproducible failure, a focused candidate passes
independent acceptance, the PR merges, the exact merged revision serves production correctly, and delayed review
findings become subsequent work. A separate drill must prove that bootstrap restores the recorded previous revision when
both production and the coding workflow fail.

The system must distinguish repairable defects from client errors, account authorization problems, exhausted capacity,
and upstream outages. Code cannot guarantee successful inference when every permitted upstream is unavailable. These
incidents remain visible and retryable; they cannot be relabeled as successful repairs.

## Current evidence

Inspection base: repository `/Users/nv/repos/ubiquity/ai.ubq.fi`, clean root checkout on `development`, local and remote
SHA `2ab2b39b61e6ae05584349e5a92d9275db1218c2`. GitHub evidence was collected around September 5, 2026 00:45 UTC, still
September 4 in New York. This is an inspection snapshot, not a current production health attestation.

- [Run 33931495629](https://github.com/ubiquity/ai.ubq.fi/actions/runs/33931495629): preparation and convergence fail
  with `Sentinel review backlog row has an invalid fingerprint or SHA`; repair is skipped.
- [Run 33933366948](https://github.com/ubiquity/ai.ubq.fi/actions/runs/33933366948): triage fails with
  `CodexInvocationError: Codex invocation stopped (runtime_failure).`; convergence then tries to read an absent matrix
  plan. The logs do not establish the underlying model runtime cause.
- [Bootstrap run 33931923259](https://github.com/ubiquity/ai.ubq.fi/actions/runs/33931923259): successful workflow
  result, but no recovery action despite eight observations of stuck progress.
- The latest 200 inspected Sentinel runs contained 164 failures and 33 successes. These counts describe the inspected
  window only. A green run that publishes a branch is not evidence of a deployed repair.
- GitHub APIs reported `development` as unprotected, no repository or inherited rulesets, and no effective branch rules.
  Required deterministic checks therefore need actual enforced delivery authority, not assumptions about branch
  protection.

Source-confirmed mismatches at the inspection SHA:

| Intended behavior                                | Current behavior and evidence                                                                                                                     |
| ------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------- |
| Failed payloads first on every wake-up           | `scripts/sentinel/main.ts:273` makes hourly captures archive-only.                                                                                |
| Required replays must pass                       | `main.ts:1259` accepts model dispositions for still-failing, regressed, and unavailable results.                                                  |
| Success means a valid API result                 | `src/sentinel_replay_capture.ts:303` can classify malformed or non-JSON HTTP 200 as completed.                                                    |
| Review latency is independent                    | `.github/workflows/provider-sentinel.yml:1552` runs the review worker before main repair; one review can occupy 40 minutes.                       |
| Review merged changes                            | `scripts/sentinel/rolling-review.ts:630` treats an already merged ancestor head as unreachable instead of retaining the original comparison base. |
| Main Sentinel can improve itself                 | `scripts/sentinel/policy.ts:41` protects all Sentinel source, workflows, and Sentinel tests.                                                      |
| Bootstrap survives broken main code              | Preparation uses current development before convergence selects `activation.active_sha`; bootstrap imports modules outside its digest boundary.   |
| Bootstrap can restore production                 | Bootstrap lacks Deno promotion authority. `bootstrap/main.ts:255` and `rollback-controller.ts:329` require healthy production before recovery.    |
| Bootstrap decides availability deterministically | Main runs a 30-minute passive monitor and then invokes a model for keep/rollback at `main.ts:5499`.                                               |

Prior work exists in `docs/provider-sentinel-self-healing-plan.md` and its recorded worktrees, plus open Sentinel
recovery PRs. Preserve those artifacts and their ownership. The proposal replaces the orchestration design if accepted;
it does not authorize merging, discarding, or cleaning old candidates. Inventory them before implementation and reuse
proved components.

## Canonical implementation identity

This is a single-writer implementation plan. The sequence below is dependent work, not a parallel module assignment.
Planning creates no implementation branch or worktree. The following identity was derived after saving this file at its
final path.

- Canonical plan path and goal identifier:
  `/Users/nv/repos/ubiquity/ai.ubq.fi/docs/provider-sentinel-redesign-2026-09-04.md`.
- Goal slug: `provider-sentinel-redesign-2026-09-04`; hash suffix: `gac146ab309`.
- Repository root: `/Users/nv/repos/ubiquity/ai.ubq.fi`.
- Canonical worktree name: `provider-sentinel-redesign-2026-09-04-gac146ab309`.
- Canonical worktree path:
  `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/provider-sentinel-redesign-2026-09-04-gac146ab309`.
- Canonical branch: `codex/provider-sentinel-redesign-2026-09-04-gac146ab309`.
- Base ref: `origin/development`; inspected base SHA: `2ab2b39b61e6ae05584349e5a92d9275db1218c2`.
- Lane state: planned; a future implementer owns and creates this exact lane after reconciliation. No module lanes are
  assigned.

Before implementation, reconcile this base against current development, existing goal work, active writers, and open
PRs. Record any base update explicitly; keep the recorded plan path and lane identity. Read `AGENTS.md`, this document,
and applicable project workflow, Git coordination, Deno, and review instructions in full.

## Architecture

There are two authority domains. The separate reviewer is a background worker, not another release decision-maker.

| Component         | Responsibility                                                                                                               | Authority                                                                                        |
| ----------------- | ---------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------ |
| Bootstrap         | Observe health, resume work, enforce acceptance, activate Sentinel versions, serialize promotions, restore previous versions | Fixed owner-controlled code and policy; sole production promotion authority                      |
| Evolving Sentinel | Diagnose incidents, implement repairs, improve its own allowed code, prepare candidate branches and PRs                      | Candidate workspace only; no bootstrap, release-state, credential, or acceptance-policy writes   |
| Review worker     | Run native Codex review on frozen revisions and publish exact evidence                                                       | Read-only code access; write review results through a controlled interface; no release authority |

Use one active coding candidate and one production release transaction. Remove repair matrix fan-out and convergence
from the replacement. Read-only diagnosis can run concurrently when useful. No model is required to select a queue,
determine a test result, promote a revision, or roll back.

Bootstrap must be an independently pinned package with its whole import graph, runtime, and dependencies fixed. It must
not import the provider, evolving Sentinel, current development configuration, or generated Markdown. The workflow that
loads it must also be protected from candidate edits. A tiny protected workflow can load the pinned package in the
current repository; a separate control repository is an optional stronger isolation choice, not a prerequisite for this
prototype.

Prompt instructions alone do not enforce this boundary. Use isolated jobs, restricted credentials, trusted diff checks,
and an independently controlled merge/promotion executor. The coding process receives neither repository write nor Deno
admin credentials. The executor verifies the actual Git diff and required checks. Repository rules must also prevent the
automation identity from bypassing this path.

Keep the existing direct Codex connection independent of `ai.ubq.fi`. Preserve `gpt-5.6-luna` with maximum reasoning for
CI/runtime implementation. Local implementation uses the configured DSH model as required by `AGENTS.md`; it must not
change the runtime Luna invariant.

## Durable state

Reuse existing Git state branches with compare-and-swap commits and encrypted capture/artifact storage. Avoid a new
database for the first slice. Store operational state as validated JSON. Render Markdown as a view; never parse a
manually edited Markdown table to decide whether incident repair can run.

Keep three small record types:

1. Work item: source/fingerprint, immutable evidence references, occurrence count and first/last seen, priority, state,
   base/candidate SHA, branch/PR, attempt count, lease generation, next eligible time, exact last failure, and
   acceptance references.
2. Release transaction: transaction and fencing generation, previous production SHA/revision, candidate SHA/revision,
   pre-promotion evidence, observed routing identity, promotion intent/result, acceptance deadline, decision, rollback
   evidence, and config compatibility identity without secret values.
3. Review task: PR number, exact base SHA, exact head SHA, reviewer/run identity, state, original result digest,
   findings, and retry information.

Track Sentinel separately: stable controller SHA, active controller SHA, candidate controller SHA, generation,
startup/functional acceptance, and previous controller. A provider `/health` SHA does not identify a tested Sentinel
controller release.

Useful work-item states are queued, implementing, candidate_saved, validating, release_pending, observing, resolved,
retry_wait, and blocked. Keep reasons structured. A timeout is not resolution. Completed review state and release state
are independent. Persist candidate commits before model-output parsing and validation; reconcile ambiguous pushes
against the exact remote ref.

Keep operational state bounded without dropping unresolved work. One malformed review batch is retained with a visible
ingestion failure and contributes no findings. It must not block capture intake, incident repair, or rollback. This
preserves fail-closed review parsing without making review health a prerequisite for uptime.

## Work selection and retries

Both incident dispatch and scheduled wake-ups drain the same queue:

1. Bootstrap handles an active production regression or interrupted release transaction.
2. Resume an eligible saved repair or select the highest-impact current production failure family.
3. Select completed review findings in P0, P1, P2, P3 order.
4. Select eligible GitHub issues.

An active outage preempts ordinary issue work at the next safe checkpoint. A review finding that is independently
confirmed to cause an active incident joins incident work; review severity alone never triggers rollback. Ingest
completed reviews early but without waiting for running reviews.

Group repeated failures by endpoint, normalized request features, terminal failure signature, and relevant
provider/configuration identity. Retain exact encrypted request bytes and compatibility headers for reproduction.
Preserve frequency and affected traffic so a thousand repeated failures create one high-priority incident, not a
thousand coding jobs.

Use bounded attempts per unchanged failure fingerprint with backoff. A failed attempt resumes its durable branch when
still applicable. Exhausted authorization, unavailable capacity, corrupt state, or repeated identical failures remain
explicit blocked/retry states with evidence. Reopen when the dependency, evidence, or base meaningfully changes.
Continue other independent eligible work while blocked dependencies wait.

Retain faithful incident fixtures beyond the current 48-hour capture TTL. Do not copy private raw prompts into
repository tests, PRs, or ordinary Actions logs. Give the developer enough request structure and recorded behavior to
reproduce the fault through a controlled diagnostic artifact or minimized fixture. Treat all captured text and issue
content as data, never workflow instructions.

## Independent payload acceptance

Freeze the acceptance manifest before the coding attempt. The coding agent may add regression tests but cannot remove
required cases, change expected results, disable validators, or waive failures. Bootstrap runs trusted validators from
its pinned package in a clean job.

Classify each captured request against the existing API contract:

- Valid supported request: require a correctly structured response and valid terminal behavior, including requested
  streaming, tool-call, model, and reasoning semantics where relevant.
- Invalid or unauthorized request: require the correct deterministic error response. Never change authorization or
  fabricate an HTTP 200 to make this case pass.
- Upstream dependency unavailable: retain as unresolved or inconclusive with evidence. It never counts as a passed
  repair.

Do not compare generated text verbatim. Validate protocol and task-specific invariants. Examples include no malformed
JSON, no missing or duplicated stream terminal, valid tool-call arguments, proper error framing, bounded time to first
useful output and completion, and no truncated stream being counted as success. A legitimate tool-call response need not
contain assistant text.

Use two complementary layers:

1. Deterministic regression: replay sanitized/minimized request fixtures through the real gateway handler with recorded
   upstream behaviors, including a stalled stream, upstream 5xx, disconnect, malformed event, and quota signal.
   Demonstrate the targeted defect on the previous code and its absence in the candidate. Preserve the fixed cost
   waterfall.
2. Live acceptance: replay the required valid cases and a small established healthy corpus against the immutable
   candidate deployment with equivalent relevant provider configuration and isolated credentials. Use the real
   authenticated API path, not just `/health`. Replay authorized effects only; neutralize external tool execution and
   state-changing operations. Do not duplicate customer side effects.

Persist live results keyed by candidate SHA/revision, validator version, corpus digest, configuration identity, and
time. Reuse immutable evidence where valid; repeat live checks when code/configuration changes or evidence is too old
for the release decision. Bound requests and provider spend under the existing policy.

A focused fix must pass every required case for its target failure family and introduce no regression in the healthy
corpus. Keep unrelated failing families open and work toward all supported captured cases passing. Do not require one
giant patch to cure all unrelated incidents before any improvement can ship. Record baseline failures explicitly; the
model cannot designate new exceptions after seeing candidate results.

For concurrency failures, preserve the recorded overlap/load pattern in a bounded test. Serial payload replay cannot
prove a race is fixed. Real traffic failure and latency observations remain necessary after promotion.

## Delivery and rollback

Use this serial transaction:

1. Save the focused candidate and open/reuse its PR. Record the immutable review base/head and enqueue Codex review
   immediately.
2. Run independent payload acceptance, the established healthy corpus, required deterministic CI, and the canonical
   `deno task sentinel:test-local` for Sentinel changes. Merge only through the enforced delivery path using an
   ancestry-preserving merge commit.
3. Build the exact merged SHA without moving stable traffic. Verify the immutable revision's body and headers against
   the full Git SHA and revision ID. If the base changed or the merged tree differs from the tested candidate,
   invalidate affected evidence and rerun acceptance.
4. Under the short shared deployment lock, record the previous proven healthy revision before promotion. When production
   is healthy, refresh its attestation. When production is already unavailable, retain the previously attested target
   from durable state, verify its immutable identity and configuration compatibility, and recover that transaction
   before starting a new ordinary release. Do not require a fresh healthy response from the broken app. Reconcile all
   existing deployment writers so automatic Deno builds cannot silently replace the pinned stable route. Persist
   promotion intent and an observation deadline before the API call.
5. Promote only the identified candidate revision with the existing Deno API and require HTTP 204. Verify the exact
   candidate on the managed production route, probe the custom domain, and run authenticated inference acceptance.
   Identified Cloudflare challenges follow the existing warning policy; any HTTP 200 identity mismatch fails acceptance.
6. Bootstrap observes production independently. A kept release updates the healthy record only after the defined
   checks/window pass. Candidate-attributable acceptance failure triggers restoration of exactly the recorded previous
   revision. Verify restored identity and inference before reporting recovery.

Codex review completion and findings of any severity never gate this transaction. Existing deterministic checks and
production acceptance still do. Preserve the current observation window initially; moving it out of the developer runner
removes idle coding time without silently weakening acceptance. The next candidate may be prepared during observation,
but production promotions remain serialized.

Initial deterministic failure rules should use the current 30-second observation cadence. Restore a transaction-owned
candidate after three consecutive liveness or authenticated-inference failures when the recorded prior revision passes
equivalent control probes, or after a frozen gateway invariant fails twice on the candidate and passes on the prior
revision. A repeated managed-route identity mismatch is a release failure: first reconcile control-plane ownership, then
restore only if this transaction still owns the route. Do not overwrite a later legitimate release. Equivalent controls
require matching relevant credentials/configuration; an immutable preview URL does not imply production-equivalent
context. If both revisions fail from the same upstream dependency, record dependency failure and stop ordinary
promotions instead of cycling revisions. These initial thresholds are proposed fixed policy for the prototype, to be
validated in preview drills before activation.

Rollback must work when the current app times out, returns non-JSON, or returns HTTP 500. Use the durable transaction
plus the authoritative Deno control-plane routing identity to prove ownership of the failing candidate. Do not require
the broken app to return healthy identity data before restoration. A stale observer cannot roll back a later release. If
routing identity or exclusive ownership cannot be established, retain the intent and report unresolved recovery; never
guess a revision from time or list order.

Arm recovery before promotion. If the deployment worker dies after moving traffic, bootstrap reconciles the Deno route
and the unfinished transaction. Loss of an observer is an uncertainty signal, not evidence by itself that the candidate
caused an outage; run independent acceptance and restore on the defined candidate failure conditions. A missing
transaction completion must never become an implicit healthy attestation.

All deployment operations, including bootstrap rollback and human/CI releases, use the same short promotion authority
and generation checks. Keep model calls and long tests outside that lock. After runner loss, verify or cancel the old
executor before reclaiming promotion ownership; an expired lease alone does not stop a stale process.

Revision rollback changes serving code; do not assume it restores KV contents, provider credentials, or external
effects. Keep destructive migrations, production secret changes, and incompatible persistent-data changes outside the
automatic repair lane. After rollback, leave Git history intact and prepare a focused revert or forward fix through the
normal PR flow. Do not force-push development or republish the rejected candidate automatically.

## Sentinel self-upgrades

Allow edits to the evolving planner/worker and their tests through a narrow independent policy. Continue to protect
bootstrap, release authority, fixed implementation model, credential handling, acceptance rules, and existing immutable
fixtures. Avoid a blanket protection on all Sentinel source.

The current stable Sentinel prepares an upgrade as a normal PR. Bootstrap checks its candidate at an exact SHA in an
isolated runner: startup, policy enforcement, queue selection, a real fixture repair that creates a durable candidate,
checkpoint recovery, and review intake. Run the candidate in observation mode against the queue without production
writes. Bootstrap activates it only after those fixed checks pass and stores the previous controller SHA.

If startup or required functional progress fails under available dependencies, bootstrap returns to the prior controller
SHA and resumes the saved work. Do not count a green no-op or elapsed healthy provider time as successful controller
validation. Restore the provider revision separately only if the provider also failed. Ordinary application changes must
not implicitly upgrade Sentinel.

Load the selected controller SHA before any evolving imports, preparation, or matrix-equivalent logic. The protected
entrypoint must still start when a proposed workflow, Deno configuration, or main Sentinel file is syntactically broken.

## Independent asynchronous review

Run native Codex review in a separate workflow with its own concurrency group and frozen base/head. Do not derive a new
comparison base from current development after merge. Git ancestry after merge is expected and does not make the
original diff unreachable.

Persist pending review before dispatch. A failed dispatch or unavailable reviewer stays retryable and does not block
delivery. The reviewer never holds coding or deployment locks. Validate the whole result and exact target identity
before ingesting; preserve original finding text and fingerprints. Malformed evidence remains an ingestion failure
without partial salvage.

Feed completed findings into the next eligible repair selection. A later PR must prove the finding is fixed on current
code; merging the reviewed PR is not proof of remediation. Retain `docs/sentinel-review-backlog.md` as the official
human-readable backlog view, generated from the validated operational records.

## Timing and uptime limits

GitHub Actions can perform all coding, tests, PR delivery, and release operations. Use incident dispatch for prompt
wake-ups and a schedule as a reconciliation fallback. GitHub documents a five-minute minimum schedule interval and
possible delayed or dropped scheduled jobs; an Actions-only design therefore cannot promise recovery within seconds.
[GitHub scheduling documentation](https://docs.github.com/en/actions/reference/workflows-and-actions/events-that-trigger-workflows#schedule)

For tighter recovery timing, the recommended later extension is a tiny independent deterministic watchdog on an already
managed independent host or separate deployment. It must be able to perform the same serialized rollback without waiting
for a new Actions runner; merely dispatching another job retains the scheduling dependency. Hosting, scoped credential
access, and any new configuration require an explicit implementation decision. They are not provisioned by this plan. A
separate Deno app isolates application bugs but not a Deno-wide outage.

Prevent immediate request storms with existing deterministic provider controls: bounded concurrency, request deadlines,
backoff with jitter, and circuit breakers keyed to the actual provider/account failure. Change these only for a
demonstrated incident. Preserve the rule that a timeout or upstream 5xx cannot authorize escalation to the next paid
tier. Sentinel repairs the recurring cause while these controls limit damage.

## Implementation sequence and acceptance

Use one writer and small PRs. Prove each behavior through its runtime before broad cleanup.

1. Restore the safety boundary: pinned bootstrap, independent provider/controller records, protected promotion executor,
   and rollback that survives unavailable app health. Demonstrate with two immutable preview revisions, a failed
   candidate, and a terminated release worker. Reuse exact revision attestation and promotion code.
2. Deliver one incident end to end: one JSON queue, deterministic priority, faithful capture fixture, strict trusted
   validator, one Luna candidate, one PR, exact merged deployment, and independent observation. Include independent
   review dispatch in this first deployed slice; no deployed repair may wait for the old sequential review step. Before
   enabling the replacement, reconcile active release transactions and import unresolved incident, candidate, review,
   and issue identities with their original evidence. Keep the old writer disabled when enabling the new writer; do not
   run competing release controllers.
3. Complete and verify asynchronous review result intake. Prove a review can finish after merge and enter the backlog
   while urgent repair proceeds. Prove malformed review data contributes no findings and does not stop incident work.
4. Enable controller self-upgrades under bootstrap acceptance, then enable ordinary issue fallback. Demonstrate both a
   successful controller upgrade and an automatic return from a broken one.
5. Verify every imported unresolved identity has a preserved disposition and explicit deduplication evidence. Remove the
   superseded matrix and orchestration entrypoints after the replacement's acceptance. Keep historical evidence and
   unfinished user-owned work.

Required evidence matrix:

| Scenario                                   | Required observable result                                                                                 |
| ------------------------------------------ | ---------------------------------------------------------------------------------------------------------- |
| One captured valid-request defect          | Previous handler fails the fixed case; candidate and deployed merged revision pass it and healthy corpus   |
| Duplicate incident storm                   | One work item with increased frequency; bounded dispatch and one active coding attempt                     |
| Invalid request                            | Correct rejection remains; no fake success or disabled auth                                                |
| Transient/total upstream outage            | Bounded retries and explicit unresolved dependency; no false pass or forbidden paid-tier escalation        |
| Corrupt review batch                       | Exact evidence retained, no partial ingestion, incident repair proceeds                                    |
| Slow review finishing after merge          | Original base/head reviewed; result ingested later; release timing unaffected                              |
| Worker timeout after edits                 | Durable saved candidate and bounded continuation, not lost runner-only work                                |
| `/health` responds 200 but inference fails | Candidate acceptance fails on real API behavior                                                            |
| Production app cannot answer               | Bootstrap restores the exact attested prior revision using control-plane identity                          |
| Runner dies after promotion                | Independent bootstrap observes/reconciles transaction and applies the defined acceptance/rollback decision |
| Stale observer races newer promotion       | Generation and exact routing checks prevent rollback of the later release                                  |
| Broken Sentinel entrypoint                 | Pinned bootstrap starts and selects the previous controller before loading broken code                     |
| No active incident or review work          | An eligible issue is implemented and delivered through the same acceptance path                            |

Run `deno task sentinel:test-local` as the single canonical local Sentinel harness, extending it with focused
deterministic cases for the new contracts. CI invokes that same command. Live replay is a separate explicitly evidenced
stage; a hermetic harness cannot establish deployed inference or rollback. Do not deliberately take down production to
prove recovery. Use preview drills first and perform any production fault exercise only with specific approval.

For repository implementation, follow the required commit, push, PR, asynchronous review, deterministic CI, merge,
deployment identity, ancestry, and clean-development handoff rules. This planning document does not authorize executing
the redesign or changing repository protection settings in this turn. Report source, local tests, preview proof,
production proof, controller activation, and unresolved cases as distinct outcomes.

## Sources and design limits

- Repository code and Actions evidence listed above are the primary basis for this proposal.
- [Deno timeline documentation](https://docs.deno.com/deploy/reference/timelines/) describes selecting and locking an
  existing revision for rollback without rebuilding. Exact promotion identity requirements come from this repository's
  current contract and implementation.
- [Official OpenAI GitHub Action documentation](https://learn.chatgpt.com/docs/github-action) establishes independent CI
  execution and output capture; it does not establish this repository's native review implementation or model/account
  availability. Preserve the current native review and owner-selected model contracts.
- A narrow deterministic test suite plus live observation reduces release risk but cannot prove correctness for every
  future payload or upstream behavior. The regression corpus must grow from real failures, and operational success must
  measure resolved incidents and recovery time rather than green workflow counts.
