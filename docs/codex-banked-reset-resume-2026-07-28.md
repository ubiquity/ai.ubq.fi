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

## Shipped behavior after production promotion

Defaults are global shadow telemetry: `CODEX_BANKED_RESET_ENABLED=true`, `CODEX_BANKED_RESET_MODE=shadow`, an empty
allowlist, and a zero global cap. Shadow cannot contact the provider. Live configuration still requires a non-empty
allowlist and cannot reach the provider because the pinned upstream source lacks documented idempotency retention,
lookup, and independent verification; see `codex-banked-reset-operations.md` for the reconciliation boundary.

The reset candidate is reached only after normal Codex account failover and the ordinary bounded retry are exhausted,
and only for a completely parsed `429` whose type is exactly `usage_limit_reached` with a stable absolute `Retry-After`
date. It may be observed in shadow mode, but cannot send provider inventory or consume calls. The pinned upstream source
does document that a `2xx` must still be parsed for JSON `code`; that adapter remains offline behind the contract gate.

## Validation evidence before the final fail-closed correction

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

## Next verification after limits naturally exhaust

1. Confirm the served production health response reports the promoted Git SHA and deployment ID.
2. With the shipped global shadow default, wait for a natural qualifying Codex `429`; do not call the reset endpoint as
   a smoke test.
3. Inspect redacted `codex_reset_eligible` and `codex_reset_shadow_candidate` events. There must be no `submit_started`,
   inventory, or consume request.
4. Do not authorize live redemption until the provider supplies the missing retention, lookup, and independent
   verification guarantees; preserve every `submitted` or `unknown` record and never replay a provider call.

The exact shadow configuration, event-by-event acceptance criteria, rollback, and live-decision prerequisites are in
`codex-banked-reset-operations.md` under “Two-phase rollout: shadow first, live only after contract closure.”

At the time this file was written, no real provider reset endpoint had been called by this implementation or its test
suite. The production workflow must provide immutable deployment attestation before the promotion is considered live.
