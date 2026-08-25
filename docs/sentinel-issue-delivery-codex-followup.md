# Sentinel issue-delivery Codex follow-up

This file is a temporary implementation handoff for the follow-up to merged PR #129. Delete this file before the follow-up PR is merged.

Implement and validate every item below against current `development`.

## Required fixes

1. Wire issue PR delivery into the actual Provider Sentinel execution path. Before a selected GitHub-issue candidate is pushed to `development`, create/update its evidenced PR and make sure the direct development push cannot bypass that gate. The current trusted-git configuration disables hooks, so relying on a git hook alone is not sufficient.
2. Wire terminal reconciliation into the Provider Sentinel workflow so successful production `keep` outcomes post supporting evidence and close the unchanged issue. Failed, manual-required, and rolled-back attempts remain open.
3. Grant the narrow workflow permissions required by the trusted delivery/reconciliation code: `pull-requests: write` and `issues: write`. Do not expose these credentials to the isolated LLM subprocess; keep mutations in deterministic trusted code.
4. Make rollback retries work. A rolled-back delivery may retry the same immutable issue fingerprint with a new candidate branch/SHA. The old merged PR must not permanently block the retry. Preserve the invariant of exactly one PR per concrete delivery attempt and deterministic deduplication within an attempt.
5. Make successful completion reconciliation idempotent. Re-running after a completed close must preserve the successful evidence and must not replace it with contradictory failure text.
6. Preserve completion evidence across transient failures. Validate the unchanged issue snapshot before mutation, persist/upsert completion evidence before the irreversible close, and make a retry capable of completing the close without corrupting evidence.
7. Run `deno fmt` on all files touched by PR #129 and this follow-up. `deno fmt --check` must pass.
8. Add deterministic regression tests for workflow/delivery invocation, permissions assumptions where testable, rollback retry PR selection, completion idempotency, and evidence-before-close retry behavior.
9. Run the focused tests plus repository formatting, lint/type/build checks required by the existing Sentinel validation contract.
10. Delete this temporary handoff file before finalizing.

## Review findings to resolve

Codex on PR #129:
- P1: delivery entrypoints were never invoked; Sentinel still pushes directly to development.
- P1: rolled-back issue retries are blocked by the prior merged PR sharing the immutable fingerprint.
- P1: Provider Sentinel lacks `pull-requests: write` and `issues: write`.
- P2: close can succeed before issue evidence is durably posted, making retry inconsistent.

CodeRabbit additionally found:
- `deno fmt --check` fails on four files.
- reconciliation is not idempotent after a successful close.

## Completion bar

Do not weaken protected-path, immutable-snapshot, production-monitoring, rollback, or evidence constraints. Do not merge. Leave the follow-up PR ready for a fresh `@codex review` after all tests pass.
