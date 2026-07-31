# Codex banked-reset resume — 2026-07-28

## Resume coordinates

- Canonical worktree: `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/feat-banked-resets`
- Branch: `feat/banked-resets`
- Primary Codex thread: `019fa7ad-7ad3-7c02-bad0-d0f8e881af7e`
- Superseded activation commit: `0767a64aaf128f4f5324d84b26fd5dc8cc9be4f3`
- Base integrated before activation: `ebe42945eac994decabef6451e66453400ae5f43` (`origin/development` at rebase time)
- Pinned provider-source submodule: `lib/codex` at `8e271dc02b23d42827875019924be0f5005642b0`

## Resumable local Codex review

- Full-branch review session: `019fa7f1-a319-7002-9886-d5e295191c3e`
- Earlier review session: `019fa7f0-8ebe-7900-8fbb-6252a64ce2ee`

The completed review found and this branch fixed two P1 issues: the global daily budget is now consumed only when a
transaction crosses the durable `submitted` boundary, and an active post-reset recovery-probe lease blocks ordinary
routing until its exact probe finishes. To inspect the recorded review again:

```sh
codex exec resume 019fa7f1-a319-7002-9886-d5e295191c3e \
  "Return the completed actionable findings only; do not modify files."
```

Use the primary thread ID above to locate this implementation conversation when reviewing the shadow observation or
later live-decision evidence.

## Current continuation for the expiring-credit canary

Production was rechecked on 2026-07-30 at `2b437eb11744e59c9e54dfeb4e1eac51991a24a7` / deployment `k72eejpydv8g`. It
remained in `shadow` with both caps set to `1`, the secret allowlist present, one exhausted account, one healthy
account, and an empty shadow-decision ledger. The old full-pool trigger therefore did **not** fire and no credit was
consumed.

The continuation broadens selection to a complete blocked cohort: every current auth slot must be either a stable
blocked account or a healthy non-probing sibling, but only blocked accounts reach inventory or redemption. Shadow still
makes zero consume calls. A repeated same-fence shadow request reuses its durable decision without a second inventory
GET.

The pinned upstream source defines exact terminal `reset` and `already_redeemed` consume codes. The continuation accepts
only those parsed codes as authoritative for a single at-most-one submission. Non-2xx, timeout, malformed/unknown JSON,
or response loss remains durable `unknown` and is never submitted again. This is not independently reconcilable
exactly-once behavior.

Terminal-only live mode additionally requires the global cap to be exactly `1`. That cap resets each UTC day, so the
operator must restore mode to `shadow` immediately after the one controlled canary.

## Historical validation evidence before the current continuation

- `deno task build` — passed
- `deno fmt --check serve.ts src tests scripts docs` — passed
- `deno lint serve.ts src tests scripts` — passed
- `deno task test` — 550 passed, 0 failed; stress suite 1 passed, 0 failed
- `git diff --check` — passed

The unscoped `deno fmt --check` intentionally scans `lib/codex` and reports upstream-owned formatting differences; it is
not a gateway formatting failure. Gateway tasks exclude the pinned submodule from tests.

## Dangling-work audit

`feat/banked-resets-release` at `65b24572a05bc0eae4193e9439088563b759b1b7` was compared with this canonical branch. It
is not integrated: it adds a status-only adapter that treats every `2xx` as a completed reset without parsing the
documented response code or proving reconciliation. The pinned upstream source disproves that assumption, so this branch
is rejected and retained only as an audit artifact; no other banked-reset worktree or branch contains work to merge.

## Next verification for the one-reset window

1. Freeze and deploy one exact candidate SHA while effective mode remains `shadow`.
2. Require both health domains to report that SHA and the same routed deployment ID.
3. Send one controlled normal request, then require a current `decision_reason: "selected"` record from the redacted
   shadow endpoint. There must be no submission event or consume call.
4. Change only the existing mode to `live`, re-attest configuration, and send exactly one controlled normal request.
5. Observe at most one consume call and one same-account recovery probe. A definitive probe `401`, `403`, or `429` may
   fall through once to the healthy sibling; a transport ambiguity must never replay.
6. Immediately restore mode to `shadow`, then audit the terminal `redeem_outcome` or durable `unknown` state and
   preserve every KV record.

The exact acceptance criteria, rollback commands, ambiguity boundary, and temporary-directory rule for all `deno deploy`
commands are in `codex-banked-reset-operations.md`.
