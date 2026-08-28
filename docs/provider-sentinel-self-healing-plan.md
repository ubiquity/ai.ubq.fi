# Provider Sentinel Self-Healing Plan

## Objective

Make Provider Sentinel recover automatically from runner loss, model timeout, invalid model output, review exhaustion,
Git transport ambiguity, and interrupted state publication without losing candidate work or automatically merging
uncertain work.

Success means every selected issue and every workspace mutation reaches one durable terminal disposition: merged,
rejected with an exact reason and evidence, or blocked with an owner and next action. No run may leave work only in a
runner workspace or only in an encrypted artifact.

## Current State and Evidence

- Repository: `/Users/nv/repos/ubiquity/ai.ubq.fi`.
- Planning base: `origin/development` at `173ece0f6d50f28296d7c751215f51022896722e`, the merge commit for PR #174.
- The repository-root checkout was clean except for a pre-existing untracked `handoffs/` directory and matched
  `origin/development` when this plan was written.
- Production served base SHA `173ece0f6d50f28296d7c751215f51022896722e` from Deno revision `rt0yvwm62vd7` on both health
  routes at 2026-08-28 18:27 UTC.
- Run `33197180235` failed after candidate implementation because the reported `changed_files` did not match the Git
  diff. It produced encrypted artifact `9697049137`, but no matching remote candidate branch was visible.
- Run `33197618818` started after the former run completed and was still active at 2026-08-28 18:27 UTC.
- Issue #136 candidate `sentinel/candidate-33188346422-1` is durable at `7a1a853bedbcec6e103f62ab82ace2c60bb9ae7a` with
  `retry_pending` disposition.
- Older run `33190526163` has encrypted evidence artifact `9695880683`, expiring 2026-11-26. No recovery branch,
  recovery PR, or exact rejection was visible.

## Canonical Goal Identity

- Canonical plan path: `/Users/nv/repos/ubiquity/ai.ubq.fi/docs/provider-sentinel-self-healing-plan.md`.
- Canonical goal identifier: `/Users/nv/repos/ubiquity/ai.ubq.fi/docs/provider-sentinel-self-healing-plan.md`.
- Goal slug: `provider-sentinel-self-healing-plan`.
- Hash suffix: `gddab649c6f`.
- Canonical worktree name: `provider-sentinel-self-healing-plan-gddab649c6f`.
- Repository root: `/Users/nv/repos/ubiquity/ai.ubq.fi`.
- Canonical worktree path:
  `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/provider-sentinel-self-healing-plan-gddab649c6f`.
- Canonical branch: `codex/provider-sentinel-self-healing-plan-gddab649c6f`.
- Base ref and SHA: `origin/development` at `173ece0f6d50f28296d7c751215f51022896722e`.
- Lane state: `planned`; the future orchestrator creates and owns it after reconciling current state.
- Persistent goal sentence: Goal: Use canonical worktree name `provider-sentinel-self-healing-plan-gddab649c6f` at
  `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/provider-sentinel-self-healing-plan-gddab649c6f` on branch
  `codex/provider-sentinel-self-healing-plan-gddab649c6f`; read `AGENTS.md` and
  `/Users/nv/repos/ubiquity/ai.ubq.fi/docs/provider-sentinel-self-healing-plan.md` in full, then orchestrate and
  implement the plan end to end, delegate each write module only to its recorded isolated worktree, keep integration and
  final validation in the canonical worktree, and never switch the canonical branch or worktree.

## Safety Boundaries

- Preserve the owner-controlled Sentinel implementation model: only `gpt-5.6-luna` with maximum reasoning is allowed.
  Failure never authorizes substitution.
- Recovery may create or update a quarantined candidate branch and a draft review PR. It must never merge, deploy, close
  an issue, or delete a branch based only on recovered data.
- Keep the existing fixed inference waterfall unchanged.
- Do not introduce a new secret, environment variable, CLI argument, or workflow input. Reuse the existing artifact and
  replay keys and workflow contracts.
- Keep artifacts ciphertext-only. Never place decrypted candidate contents in logs, step summaries, caches, or ordinary
  workflow artifacts.
- Use leases and compare-and-swap state transitions. A stale run must not overwrite a newer disposition.
- Bound automatic retries by failure fingerprint and attempt count. Repeated identical failures move to
  `manual_required`; they do not loop hourly forever.
- Do not weaken branch protection, review requirements, or deployment attestation.

## Architecture

### 0. Two trust domains

Split Sentinel into two independent systems with separate authority:

- **Bootstrap Sentinel** is small, owner-controlled, version-pinned, and forbidden from modifying itself. It performs
  health evaluation, active-version selection, rollback, recovery dispatch, and repair-constraint publication. Its code
  changes only through the normal human-reviewed repository workflow.
- **Evolving Sentinel** performs provider diagnosis, issue selection, implementation, review, and candidate delivery. It
  may propose changes to its own implementation, but it cannot change the Bootstrap Sentinel, its stable-version
  registry, rollback policy, Luna-only invariant, or acceptance gates.

The bootstrap runs from a pinned workflow and imports only a narrow, hash-verified bootstrap module. The evolving
implementation runs from an immutable Git SHA selected by the bootstrap, not implicitly from the current `development`
checkout.

Maintain a signed or repository-protected `SentinelReleaseRecordV1` with `stable_sha`, `candidate_sha`, acceptance
evidence, activation time, rollback reason, and monotonic generation. Updating `stable_sha` requires the normal
protected PR and acceptance workflow. The evolving Sentinel cannot write this record directly.

The bootstrap considers an active version unhealthy only from authoritative signals: invalid startup/policy checks,
repeated workflow failures with the same fingerprint, failure to create a durable checkpoint, corrupted state
transitions, or a canary acceptance failure. A single timeout, network error, provider 5xx, or Luna capacity failure
does not trigger rollback.

Rollback is a control-plane pointer change from the unhealthy active SHA to the last proven `stable_sha`. It never
rewrites Git history, force-pushes, reverts unrelated `development` changes, deletes branches, or merges candidate work.
The bootstrap then:

1. fences new evolving runs on the unhealthy generation;
2. preserves its branch, recovery record, logs, and encrypted artifacts;
3. activates the known-good immutable SHA for subsequent Sentinel runs;
4. publishes a `SentinelFailureConstraintV1` containing the stable failure fingerprint, violated invariant, minimal
   evidence references, and a deterministic regression-test requirement;
5. creates or updates one quarantined repair branch and draft PR based on current `development`, with the failed
   evolution and constraint as inputs and auto-merge disabled;
6. runs the repaired version as a canary; promotion to stable still requires protected review, CI, and live acceptance.

Constraints are data, not free-form prompt accumulation. They are deduplicated by fingerprint, bounded in size, tied to
an executable regression test, and retired only through a reviewed change that proves the invariant is enforced
elsewhere. This prevents the instruction set from growing without bound or being poisoned by one ambiguous failure.

### 1. Durable recovery record

Define one versioned `SentinelRecoveryRecordV1` for every selected work item. Its identity is
`(repository, source kind, source id, source revision, candidate generation)`. It records:

- run ID, attempt, lease token, base SHA, candidate generation, and timestamps;
- phase and monotonic state version;
- candidate branch and SHA when available;
- canonical Git-derived changed files and tree hash;
- failure class and stable failure fingerprint;
- artifact IDs and digests, never keys;
- disposition and exact reason;
- predecessor record when a recovery supersedes an incomplete run.

The allowed states are `claimed`, `implementation_running`, `workspace_dirty`, `checkpoint_publishing`,
`checkpoint_durable`, `validation_failed`, `review_pending`, `retry_wait`, `recovery_pending`, `manual_required`,
`rejected`, and `delivered`. Terminal states are `manual_required`, `rejected`, and `delivered`. Every transition
validates its predecessor and lease.

The canonical durable index remains the existing issue ledger/state mechanism. The candidate commit and recovery record
are published together by one atomic Git push where possible. When push outcome is ambiguous, the record remains
non-terminal and reconciliation proves the remote ref before advancing.

### 2. Checkpoint-before-validation supervisor

Treat model reports as untrusted metadata. After Luna returns or a timeout/cancellation leaves a dirty tree:

1. Compute the candidate diff from Git.
2. If the tree changed, create a checkpoint commit and publish it to a deterministic quarantined branch before
   validating `changed_files`, review output, or delivery metadata.
3. Derive canonical changed files from Git. A report mismatch becomes `validation_failed/report_diff_mismatch`; it does
   not discard the checkpoint.
4. If no tree changed, record `rejected/no_candidate_diff` with the model report digest.
5. Use a top-level finalizer to run this checkpoint path after every caught failure and normal exit. The workflow-level
   reconciler handles hard runner loss where the finalizer cannot run.

Candidate branch names include the source identity and generation, not only a run ID, so retries converge on the same
work item without colliding with unrelated runs.

### 3. Recovery controller

Run reconciliation before normal selection and in a scheduled recovery job. It scans non-terminal ledger records, recent
failed/cancelled/timed-out workflow runs, relevant remote candidate refs, and encrypted artifact metadata.

For each incomplete record, it acquires a fenced lease and chooses exactly one action:

- remote branch exists and matches the record: resume validation/review from that SHA;
- branch publication was ambiguous: prove the remote ref and advance or retry the atomic push;
- encrypted candidate exists but branch does not: decrypt only inside GitHub Actions, reconstruct the exact base and
  patch in an isolated workspace, create a checkpoint commit, push a quarantined branch, and create or update a draft PR
  with auto-merge disabled;
- artifact contains no candidate diff: record `rejected/no_candidate_diff`;
- base, digest, or patch cannot be proved: record `manual_required` with exact evidence and next action;
- no recoverable checkpoint or artifact exists: record `rejected/no_recoverable_candidate` only after proving the
  absence across the bounded evidence sources.

Recovery is idempotent. Re-running it must find or reproduce the same branch SHA and PR, not create duplicates.

### 4. Retry policy and circuit breaker

Classify failures into stable categories: capacity/quota, transient transport, runner interruption, invalid
implementation report, validation failure, review exhaustion, Git publication ambiguity, stale source, and unrecoverable
evidence.

- Capacity and transient transport use bounded exponential backoff with jitter and the existing retry checkpoint.
- Runner interruption resumes from the durable checkpoint.
- Invalid reports preserve work, use Git-derived facts, and enter review only after local validation succeeds.
- Validation failures permit one repair attempt on the same candidate generation. A repeated fingerprint becomes
  `manual_required`.
- Review exhaustion retains the branch and becomes `manual_required` or `retry_wait` according to the existing severity
  policy.
- Three identical failure fingerprints within the bounded history open a per-work-item circuit breaker. Hourly selection
  skips it until its recorded retry time or a source revision change.

### 5. Operator-visible reconciliation

Each run summary reports counts and links for recovered, pending, rejected, and manually blocked records. It must show
the source, candidate SHA, branch, disposition, failure fingerprint, artifact expiry, and next automatic action time. Do
not expose decrypted content.

A single recovery report must prove that the two known artifacts from runs `33190526163` and `33197180235` reached
durable dispositions before this project can be called stable.

## Shared Contracts and Integration Rules

- The orchestrator owns the recovery schema, state transitions, workflow wiring, and final end-to-end fixtures because
  they are shared surfaces.
- Git is authoritative for candidate content, tree identity, and changed files. Model JSON cannot override Git facts.
- The ledger is authoritative for work-item ownership and disposition. Workflow run status alone is not a disposition.
- Artifacts are authoritative only for encrypted forensic/recovery input and their digest/retention metadata.
- Draft PR existence is review visibility, not delivery. Only the existing protected merge workflow can produce
  `delivered`.
- Hard cut over all failure paths to the new recovery record. Do not maintain a parallel legacy recovery path.

## Implementation Modules

The orchestrator first lands the shared `SentinelRecoveryRecordV1` contract and transition validator on the canonical
branch. Every dependent module starts from that exact integrated SHA.

### m01-bootstrap

- Purpose: implement the immutable bootstrap trust boundary, stable-version registry, health decision, fenced activation
  pointer, and safe rollback controller.
- Owned surfaces: new narrowly scoped bootstrap modules under `scripts/sentinel/bootstrap/`, their tests, and the
  protected bootstrap workflow entry point. The orchestrator owns the release-record schema and final workflow
  integration.
- Dependency: integrated shared recovery and release-record contracts.
- Lane: `provider-sentinel-self-healing-plan-m01-bootstrap-a4f51eab434`; branch
  `codex/provider-sentinel-self-healing-plan-m01-bootstrap-a4f51eab434`; worktree
  `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/provider-sentinel-self-healing-plan-m01-bootstrap-a4f51eab434`;
  expected base is the exact canonical SHA after the shared contracts land.
- Acceptance: authoritative repeated failure fences the unhealthy generation and selects the previous stable SHA;
  transient provider or Luna failures do not roll back; no code path rewrites history, merges, deploys, or edits
  bootstrap policy.
- Prohibited: self-modification, force push, automatic merge, stable-record mutation outside protected promotion,
  provider/model substitution.

### m02-checkpoint

- Purpose: checkpoint every dirty candidate before report validation and convert report mismatches into durable
  validation failures.
- Owned surfaces: candidate creation/finalization paths in `scripts/sentinel/main.ts`, `scripts/sentinel/issues.ts`, and
  focused checkpoint tests in `tests/sentinel-orchestrator.test.ts`.
- Dependency: integrated shared recovery contract.
- Lane: `provider-sentinel-self-healing-plan-m02-checkpoint-a81f10b065c`; branch
  `codex/provider-sentinel-self-healing-plan-m02-checkpoint-a81f10b065c`; worktree
  `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/provider-sentinel-self-healing-plan-m02-checkpoint-a81f10b065c`;
  expected base is the integrated shared-contract SHA.
- Acceptance: a simulated `changed_files` mismatch leaves a remotely publishable checkpoint record and never reports an
  untracked dirty candidate.
- Prohibited: model substitution, review-policy changes, workflow edits.

### m03-reconciler

- Purpose: reconcile incomplete records, ambiguous Git pushes, remote refs, and retry checkpoints idempotently.
- Owned surfaces: `scripts/sentinel/issue-delivery-reconcile.ts`, `scripts/sentinel/issue-delivery.ts`, and
  `tests/sentinel-issue-delivery.test.ts`.
- Dependency: integrated shared recovery contract.
- Lane: `provider-sentinel-self-healing-plan-m03-reconciler-ae75f6fab8b`; branch
  `codex/provider-sentinel-self-healing-plan-m03-reconciler-ae75f6fab8b`; worktree
  `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/provider-sentinel-self-healing-plan-m03-reconciler-ae75f6fab8b`;
  expected base is the integrated shared-contract SHA.
- Acceptance: repeated reconciliation produces one disposition, one branch identity, and no duplicate delivery.
- Prohibited: artifact decryption and workflow changes.

### m04-artifact-recovery

- Purpose: recover encrypted candidate evidence inside Actions into a quarantined branch or exact rejection.
- Owned surfaces: artifact crypto/recovery scripts under `scripts/sentinel/`, focused artifact integration tests, and a
  narrowly scoped reusable workflow job owned by this module after the orchestrator freezes the workflow contract.
- Dependency: integrated shared recovery contract; coordinate the single `.github/workflows/provider-sentinel.yml` edit
  with the orchestrator.
- Lane: `provider-sentinel-self-healing-plan-m04-artifact-recovery-a91342031ff`; branch
  `codex/provider-sentinel-self-healing-plan-m04-artifact-recovery-a91342031ff`; worktree
  `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/provider-sentinel-self-healing-plan-m04-artifact-recovery-a91342031ff`;
  expected base is the integrated shared-contract SHA.
- Acceptance: an encrypted fixture reconstructs a deterministic commit and draft-PR request with auto-merge disabled;
  corrupt/wrong-base evidence produces `manual_required` without plaintext leakage.
- Prohibited: artifact-key changes, new secrets, automatic merge, production deployment.

### m05-retry-policy

- Purpose: add failure fingerprints, bounded backoff, and the per-work-item circuit breaker.
- Owned surfaces: retry classification helpers in `scripts/sentinel/` and focused tests in
  `tests/sentinel-orchestrator.test.ts`; coordinate overlapping test edits after m02 integration.
- Dependency: m02 and m03 integrated.
- Lane: `provider-sentinel-self-healing-plan-m05-retry-policy-a6a8ded8cb6`; branch
  `codex/provider-sentinel-self-healing-plan-m05-retry-policy-a6a8ded8cb6`; worktree
  `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/provider-sentinel-self-healing-plan-m05-retry-policy-a6a8ded8cb6`;
  expected base is the canonical SHA after m02 and m03 integration.
- Acceptance: repeated identical failures stop automatic attempts at the defined bound while a changed source revision
  permits a fresh generation.
- Prohibited: provider waterfall or Luna policy changes.

### m06-observability

- Purpose: expose complete recovery dispositions and next actions in workflow summaries without sensitive content.
- Owned surfaces: summary/report generation in `.github/workflows/provider-sentinel.yml` and its focused assertions.
  This module runs after m04 so the workflow has one writer.
- Dependency: m01 through m05 integrated.
- Lane: `provider-sentinel-self-healing-plan-m06-observability-abe730fef12`; branch
  `codex/provider-sentinel-self-healing-plan-m06-observability-abe730fef12`; worktree
  `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/provider-sentinel-self-healing-plan-m06-observability-abe730fef12`;
  expected base is the canonical SHA after m01 through m05 integration.
- Acceptance: fixtures for recovered, rejected, retrying, and manual records render source, SHA, branch, fingerprint,
  expiry, and next action.
- Prohibited: new dashboard, environment variables, or external notification channels.

## Implementation Order

1. Reconcile current `development`, active runs, known artifacts, branches, PRs, and issue ledger. Do not overwrite
   newer work.
2. Create the canonical lane from the recorded base only if its planned path and branch are free.
3. Orchestrator implements and integrates the shared recovery schema and transition tests.
4. Run m01 through m04 concurrently only where their owned files remain disjoint after the shared contract is
   integrated. The orchestrator keeps the stable-version registry and workflow shell under one writer and serializes
   shared test-file conflicts.
5. Integrate and validate m01-m04. Then run m05.
6. Integrate m05. Run m06 as the sole workflow writer.
7. Freeze to one writer for combined tests, local recovery simulation, review, PR delivery, deployment, and live
   recovery acceptance.
8. Recover or exactly reject artifacts `9695880683` and `9697049137` through the new controlled workflow. This external
   mutation requires the normal repository delivery workflow and must not merge recovered candidates automatically.

## Validation Matrix

| Layer               | Required evidence                                                                                                                                                                                                                                           |
| ------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Static              | Format, lint, type check, and policy checks pass on the exact integrated SHA.                                                                                                                                                                               |
| Focused             | State-machine transition, checkpoint, reconciliation, artifact crypto, retry fingerprint, circuit-breaker, and summary tests pass.                                                                                                                          |
| Bootstrap isolation | Tests prove the evolving code cannot import, edit, select, or bypass bootstrap policy and cannot write the stable release record.                                                                                                                           |
| Rollback            | An unhealthy synthetic generation moves only the active runtime pointer to the previous stable SHA, fences stale leases, preserves failed work, and emits one deduplicated failure constraint plus draft repair request.                                    |
| Fault injection     | Kill-before-push, kill-after-ambiguous-push, invalid `changed_files`, timeout with dirty tree, corrupt artifact, stale base, duplicate reconciler, and lease-steal fixtures each reach the expected durable state. No real process is killed for this test. |
| Local end to end    | A synthetic issue proceeds from claim through dirty candidate, checkpoint, injected failure, recovery, draft review, and exact disposition with one branch identity.                                                                                        |
| GitHub Actions      | One controlled failed run proves automatic checkpoint/reconciliation; one controlled artifact-recovery run proves draft-only recovery and ciphertext-only handling. Persist these rate-limited results.                                                     |
| Git graph           | Every accepted implementation tip is an ancestor of the canonical tip and, after merge, refreshed `origin/development`.                                                                                                                                     |
| Deployment          | The merged full SHA and exact revision ID pass immutable and both stable health routes under the repository deployment rules.                                                                                                                               |
| Live backlog        | At least one normal backlog item reaches a durable disposition after deployment. Both known encrypted candidates have durable recovered or exact rejected dispositions.                                                                                     |

## Definition of Done

- No non-terminal record is older than its recovery service-level window without `manual_required` owner and next
  action.
- Every dirty candidate is discoverable through a remote Git ref before report validation can terminate the run.
- Reconciliation is idempotent under duplicate and overlapping workflow execution.
- Automatic retries are bounded and source-revision aware.
- Recovered work cannot merge automatically.
- Bootstrap Sentinel cannot self-modify, and Evolving Sentinel cannot change bootstrap authority or promote itself to
  stable.
- Rollback selects an immutable proven Sentinel SHA without reverting repository history or unrelated application code.
- The known candidates from runs `33190526163`, `33197180235`, and branch `sentinel/candidate-33188346422-1` each have a
  durable, evidenced disposition.
- The focused and full repository checks, bounded Codex review loop, required CI, ancestry-preserving merge, deployment
  attestation, and live acceptance pass.
- Repository root ends clean on `development`, equal to refreshed `origin/development`; every task-created ref is
  classified as integrated, rejected with reason, or blocked with owner and next action.

## Final Report

Report separately: implemented SHA, focused and full checks, PR/review/CI state, merged SHA, deployment revision and
dual-host identity, each recovered candidate disposition, retry/circuit-breaker evidence, Git ancestry proof, remaining
rough edges, and final repository status.
