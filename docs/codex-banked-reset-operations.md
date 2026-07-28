# Codex banked-reset operations

## Status and operating boundary

Live redemption is deliberately unavailable in this revision. The only shipped production provider is the non-networking unavailable provider, so setting the feature flag to `live` cannot make a provider request or consume a reset.

This is intentional. The exact live-mode blocker is both of the following:

1. There is no reviewed provider contract proving caller-supplied idempotency, retention of that key, lookup by idempotency key after an ambiguous result, and an independent observation that quota was restored.
2. There is no production adapter implementing such a contract.

HTTP success alone is never verification. Do not infer endpoint paths, request bodies, inventory semantics, receipts, or reset types from an inference base URL. A future adapter must be reviewed independently and remain unreachable from automated tests.

## Candidate eligibility and routing

The feature is downstream of normal Codex account routing and never replaces ordinary account failover.

1. An account must return a completely parsed upstream `429` with error type exactly `usage_limit_reached`.
2. The response must carry a canonical absolute `Retry-After` HTTP-date. A relative delay remains valid for ordinary routing but can never name a durable reset window.
3. A changed stable deadline or a relative delay makes the circuit generation-ambiguous; it may not mint a new reset identity.
4. Every healthy configured account is tried first. A healthy fallback emits `codex_reset_skipped_healthy_fallback` and prevents redemption.
5. A strong KV read must prove the exact quota fence is still current before a new transaction. KV failure, stale state, malformed data, or ambiguity fails closed.

The ordinary bounded `429` retry and durable reset transaction are separate. A successful ordinary retry serves the request and must not fall through into redemption. A post-reset retry occurs once only; a second `429` is returned as an ordinary quota failure and is never fed back into reset selection.

## Configuration and rollback

Settings are read for each gateway request so a new claim observes a changed kill switch without deleting durable records.

| Variable | Safe default | Requirement |
| --- | --- | --- |
| `CODEX_BANKED_RESET_ENABLED` | `false` | Must be exactly `true` after trimming and case normalization. |
| `CODEX_BANKED_RESET_MODE` | `disabled` | Only `disabled`, `shadow`, and `live` are accepted. |
| `CODEX_BANKED_RESET_ACCOUNT_ALLOWLIST` | empty | A nonempty list of exact account IDs or stable account hashes is required. |
| `CODEX_BANKED_RESET_MAX_GLOBAL_PER_DAY` | `0` | Must be a positive integer before a new claim is allowed. |
| `CODEX_BANKED_RESET_MAX_PER_ACCOUNT_PER_WINDOW` | `1` | Must be exactly `1`; all other values fail closed. |

`shadow` records candidate decisions but makes no provider calls, including inventory reads. `disabled` and a false flag prevent new claims and submissions. Existing `submitted` or `unknown` records remain recovery-only: a future reviewed provider may lookup and independently verify them with the same key, but it may never inventory-read or submit a replacement reset.

For an unambiguous rollback, set both values below. The implementation checks the policy before claim, before the side-effect boundary, after that transition, and immediately after the final owner/fence/lease renewal.

```text
CODEX_BANKED_RESET_ENABLED=false
CODEX_BANKED_RESET_MODE=disabled
```

## Provider contract required before activation

A future adapter must document endpoint/method/auth/account scope, inventory and reset-type schemas, redemption result schemas, caller-supplied idempotency-key retention, lookup by that key after timeout or response loss, the exact meaning of each terminal result, an independent proof of restored quota, and whether the reset is account-, subscription-, organization-, or workspace-scoped.

The runtime contract gate requires positive idempotency retention, lookup by idempotency key, independent verification, and at least one reviewed nonempty reset type. Missing or malformed capabilities fail closed before inventory, lookup, verification, or redemption. Receipt IDs are retained only when an adapter explicitly declares them non-secret and safe.

## Durable transaction and routing repair

The ledger is separate from routing state:

```text
["uos_ai", "codex_reset_redemption", "v1", account_id_hash, quota_generation]
```

`claimed` has an owner lease; `submitted` means the provider may have received the request; `unknown` covers any possibly-spent ambiguity and can only reconcile using the same deterministic key; `verified` requires independent provider verification; `rejected` is terminal for definitive no-inventory, unsupported type, or provider rejection.

The deterministic identity derives from account hash and canonical observed deadline, never a request ID. It is stable across credential refresh; only account, credential, and idempotency hashes are persisted or emitted.

Every claim and submission fences routing slot, credential version, account hash, quota deadline, routing generation, and auth-pool slot. A stale owner, changed credential, changed deadline, CAS failure, or unavailable KV cannot submit, finalize, or clear a newer circuit.

Verified routing repair atomically clears only the exact quota block while retaining its observed deadline as an ambiguity tombstone and reserving a fenced recovery-probe lease for the one post-reset retry. A delayed old `429` cannot mint another identity while that lease exists. Only a successful recovery probe, including that post-reset retry, clears ambiguity. The retry force-reads the auth-pool slot immediately before transport; a changed credential or slot preserves the normal quota result.

## Observability, validation, and future canary

Events include `codex_reset_eligible`, `codex_reset_skipped_healthy_fallback`, `codex_reset_claimed`, `codex_reset_submit_started`, `codex_reset_submitted`, `codex_reset_unknown`, `codex_reset_rejected`, `codex_reset_verified`, `codex_reset_inference_retry`, `codex_reset_inference_retry_result`, `codex_reset_duplicate_prevented`, and `codex_reset_shadow_candidate`.

Metrics cover eligible and shadow candidates, submissions, verified and unknown outcomes, duplicate prevention, verification latency, estimated spend, and post-reset retry outcome. Raw tokens, auth JSON, provider credentials, and raw idempotency keys must never appear in logs or KV.

All current tests use injected fake providers, fake clocks, versionstamped fake KV, and mocked inference transports. No test has a provider URL, credential, or live redemption implementation. They cover strict trigger parsing, policy gates, leases, CAS/credential fencing, ambiguity recovery, timeouts, crashes, generated state-machine sequences, simultaneous requests, and Responses/Chat delivery in buffered and streamed modes.

Do not run a real canary until a reviewed adapter satisfies the contract above, shadow mode has observed real qualifying events without false positives, exactly one account is allowlisted with a global limit of one, rollback has been rehearsed without a provider call, and an operator can audit the ledger, verification, and one retry. Until then, live redemption remains unavailable by design.
