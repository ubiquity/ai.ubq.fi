# Codex banked-reset resume — 2026-07-28

## Resume coordinates

- Canonical worktree: `/Users/nv/repos/ubiquity/ai.ubq.fi/.codex-worktrees/feat-banked-resets`
- Branch: `feat/banked-resets`
- Activation commit: `0767a64aaf128f4f5324d84b26fd5dc8cc9be4f3`
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

## Live behavior after production promotion

No new environment variable is required. Defaults are live mode, the current Codex account pool, one provider submission
per UTC day globally, and one submission per account/window. Existing `CODEX_BANKED_RESET_*` variables are optional
restrictions or immediate kill switches; see `codex-banked-reset-operations.md` for their exact effects.

The reset path is reached only after normal Codex account failover and the ordinary bounded retry are exhausted, and
only for a completely parsed `429` whose type is exactly `usage_limit_reached` with a stable absolute `Retry-After`
date. It then reads inventory and sends the upstream Codex contract discovered in the pinned submodule. A `2xx` is an
HTTP success, while the documented JSON `code` selects the terminal result: `reset` and `already_redeemed` recover;
`nothing_to_reset` and `no_credit` reject; malformed, empty, unrecognized, or non-2xx replies remain `unknown` and are
not replayed automatically.

## Validation tied to the activation commit

- `deno task build` — passed
- `deno fmt --check serve.ts src tests scripts docs` — passed
- `deno lint serve.ts src tests scripts` — passed
- `deno task test` — 550 passed, 0 failed; stress suite 1 passed, 0 failed
- `git diff --check` — passed

The unscoped `deno fmt --check` intentionally scans `lib/codex` and reports upstream-owned formatting differences; it is
not a gateway formatting failure. Gateway tasks exclude the pinned submodule from tests.

## Next verification after limits naturally exhaust

1. Confirm the served production health response reports the promoted Git SHA and deployment ID.
2. Wait for a natural qualifying Codex `429`; do not call the reset endpoint as a smoke test.
3. Inspect the redacted `codex_reset_*` events. A normal successful sequence is `eligible`, `claimed`, `submit_started`,
   `submitted`, `verified`, followed by one `inference_retry` and its result.
4. If the result is `unknown`, or the retry does not emit a validated `response.completed`, set the existing rollback
   values in the operations document, preserve the KV ledger record, and do not replay the provider call.

At the time this file was written, no real provider reset endpoint had been called by this implementation or its test
suite. The production workflow must provide immutable deployment attestation before the promotion is considered live.
