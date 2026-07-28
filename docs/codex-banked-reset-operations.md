# Codex banked-reset operations

## Status and operating boundary

The pinned `lib/codex` submodule is the provider contract reference. Its source establishes the request and response
schema plus same-key replay, but does not document idempotency retention, lookup by request ID, or independent proof
that quota was restored. The gateway therefore ships with live redemption disabled and will not call the account-bound
adapter, even if live configuration is supplied, until a reviewed provider contract proves those three guarantees. Tests
inject their transport; no test or this implementation run has called a real reset endpoint.

For a configured Codex base, the adapter uses `GET .../rate-limit-reset-credits` and
`POST .../rate-limit-reset-credits/consume`: `/api/codex/...` for the Codex layout and `/backend-api/wham/...` for the
ChatGPT layout. It sends Bearer auth, `ChatGPT-Account-ID`, and the Codex user agent. The consume JSON is
`{ "redeem_request_id": "<durable key>", "credit_id": "<optional opaque id>" }`.

Every returned 2xx is a successful HTTP transport response, but a reset is complete only when its documented JSON `code`
is `reset` or `already_redeemed`. `nothing_to_reset` and `no_credit` are definitive rejections. A non-2xx, empty,
malformed, or unrecognized 2xx response remains `unknown` and is never automatically re-submitted.

## Candidate eligibility and routing

The feature is downstream of normal Codex account routing and never replaces ordinary account failover.

1. An account must return a completely parsed upstream `429` with error type exactly `usage_limit_reached`.
2. The response must carry a canonical absolute `Retry-After` HTTP-date. A relative delay remains valid for ordinary
   routing but can never name a durable reset window.
3. A changed stable deadline or a relative delay makes the circuit generation-ambiguous; it may not mint a new reset
   identity.
4. Every healthy configured account is tried first. A healthy fallback emits `codex_reset_skipped_healthy_fallback` and
   prevents redemption.
5. A strong KV read must prove the exact quota fence is still current before a new transaction. KV failure, stale state,
   malformed data, or ambiguity fails closed.

The ordinary bounded `429` retry and durable reset transaction are separate. A successful ordinary retry serves the
request and must not fall through into redemption. A post-reset retry occurs once only; a second `429` is returned as an
ordinary quota failure and is never fed back into reset selection.

## Configuration and rollback

Settings are read for each gateway request. The shipped default is global `shadow` telemetry: it observes eligible
exhaustion across the current account pool but never contacts the provider. A live claim requires explicit `live` mode,
a non-empty account allowlist, a positive global cap, and per-window cap exactly one. Those settings alone do not
override the provider-contract gate described above.

| Variable                                        | Safe default | Requirement                                                           |
| ----------------------------------------------- | ------------ | --------------------------------------------------------------------- |
| `CODEX_BANKED_RESET_ENABLED`                    | `true`       | Set exactly `false` to stop shadow and live candidates immediately.   |
| `CODEX_BANKED_RESET_MODE`                       | `shadow`     | Observes all current accounts; `live` remains contract-gated.         |
| `CODEX_BANKED_RESET_ACCOUNT_ALLOWLIST`          | empty        | Required for live only; exact account IDs or stable hashes only.      |
| `CODEX_BANKED_RESET_MAX_GLOBAL_PER_DAY`         | `0`          | Must be positive for a live claim; cannot override the provider gate. |
| `CODEX_BANKED_RESET_MAX_PER_ACCOUNT_PER_WINDOW` | `1`          | Must be exactly `1`; all other values fail closed.                    |

`shadow` records candidate decisions but makes no provider calls, including inventory reads. `disabled` and a false flag
prevent new claims and submissions. Existing `submitted` or `unknown` records remain recovery-only: a future reviewed
provider may lookup and independently verify them with the same key, but it may never inventory-read or submit a
replacement reset.

The global cap applies to provider submissions, not just claims. A claim that survives a UTC-day boundary is rejected
before a provider call; it cannot consume a new day's capacity. A transaction that already crossed the durable
`submitted` boundary is instead retained for lookup-only reconciliation, never retried as a fresh spend.

For an unambiguous rollback, set both values below. The implementation checks the policy before claim, before the
side-effect boundary, after that transition, and immediately after the final owner/fence/lease renewal.

```text
CODEX_BANKED_RESET_ENABLED=false
CODEX_BANKED_RESET_MODE=disabled
```

## Two-phase rollout: shadow first, live only after contract closure

### Phase 1 — shadow observation

Shadow mode is the only approved next step. It exercises the qualifying-429, healthy-fallback, allowlist, routing-fence,
and redacted-event paths, but it makes **zero** inventory, consume, lookup, or verification calls to the reset provider.
It cannot spend a banked reset.

1. The shipped default observes every current Codex account. No allowlist is needed for shadow because it never reaches
   the provider. Do not add credentials, auth JSON, or arbitrary identifiers to the allowlist.
2. To make the global-shadow choice explicit in deployment configuration, set the existing values below. The empty
   allowlist is intentional in this phase.

   ```text
   CODEX_BANKED_RESET_ENABLED=true
   CODEX_BANKED_RESET_MODE=shadow
   CODEX_BANKED_RESET_ACCOUNT_ALLOWLIST=
   CODEX_BANKED_RESET_MAX_GLOBAL_PER_DAY=0
   CODEX_BANKED_RESET_MAX_PER_ACCOUNT_PER_WINDOW=1
   ```

3. Deploy the configuration, then verify the served `/health` response reports the intended immutable Git SHA and
   deployment ID before treating shadow as enabled.
4. Wait for a natural Codex exhaustion. Do not manufacture a 429 and do not call either reset-credit endpoint.
5. For each candidate, verify the event sequence is limited to `codex_reset_eligible` and
   `codex_reset_shadow_candidate`, with any `codex_reset_skipped_healthy_fallback` explained by a successful sibling
   account. There must be no `codex_reset_claimed`, `codex_reset_submit_started`, `codex_reset_submitted`,
   `codex_reset_verified`, inventory request, consume request, or ledger spend record.
6. Keep shadow enabled for the agreed observation period (for example, several days) and audit false positives, stable
   deadlines, healthy fallback behavior, and log redaction. Roll back immediately with the two disabled values above if
   the telemetry is wrong.

### Phase 2 — live redemption decision

Do **not** enable live mode merely because the shadow observation period elapsed. Shadow proves gateway selection and
observability, not provider reconciliation. Before any live implementation or deployment, an operator must attach a
reviewed upstream contract that proves all of the following for the exact production endpoint and account scope:

1. Caller-provided `redeem_request_id` idempotency with a documented retention period long enough to cover recovery.
2. Lookup by that idempotency key after a timeout or lost response.
3. Independent proof that the targeted quota window was reset, not just a successful HTTP status or response JSON.
4. The documented inventory, consume, account/workspace binding, terminal response codes, and receipt-handling schema.

Only then may a separate reviewed code change advertise `retentionMs > 0`, `lookup.byIdempotencyKey=true`, and
`verification.independentlyVerifiable=true` for that proven provider. That change needs fresh hermetic contract
fixtures, full validation, a one-account live allowlist, global cap exactly one, an operator watching the ledger, and a
new explicit authorization. Configuration alone cannot bypass the current provider-contract gate.

If that contract is still incomplete after the shadow period, keep `shadow` enabled or revert to `disabled`; do not make
a status-only or 2xx-only exception.

## Provider contract and ambiguity

The upstream inventory body is `{ credits, available_count }`; `available_count` is authoritative and the optional
available `codex_rate_limits` credit ID is sent when present. Omission asks upstream to choose the next eligible credit.
The response body does not contain a receipt ID, so none is retained.

Upstream documents a caller-supplied `redeem_request_id`, same-key idempotent replay, and `already_redeemed`. It does
not document a retention period, standalone lookup endpoint, or independent quota-restoration verification. Therefore a
parsed terminal result is not sufficient for automatic production redemption: response-loss records remain `unknown`,
and live mode remains structurally disabled. Operators must preserve those records and investigate before any manually
authorized same-key reconciliation.

## Durable transaction and routing repair

The ledger is separate from routing state:

```text
["uos_ai", "codex_reset_redemption", "v1", account_id_hash, quota_generation]
```

`claimed` has an owner lease; `submitted` means the provider may have received the request; `unknown` covers any
possibly-spent ambiguity and is never automatically re-submitted; `verified` follows a documented `reset` or
`already_redeemed` result; `rejected` is terminal for definitive no-inventory, unsupported type, `nothing_to_reset`,
`no_credit`, or provider rejection.

The deterministic identity derives from account hash and canonical observed deadline, never a request ID. It is stable
across credential refresh; only account, credential, and idempotency hashes are persisted or emitted.

Every claim and submission fences routing slot, credential version, account hash, quota deadline, routing generation,
and auth-pool slot. A stale owner, changed credential, changed deadline, CAS failure, or unavailable KV cannot submit,
finalize, or clear a newer circuit.

Verified routing repair atomically clears only the exact quota block while retaining its observed deadline as an
ambiguity tombstone and reserving a fenced recovery-probe lease for the one post-reset retry. A delayed old `429` cannot
mint another identity while that lease exists. Only a successful recovery probe, including that post-reset retry, clears
ambiguity. The retry force-reads the auth-pool slot immediately before transport; a changed credential or slot preserves
the normal quota result.

## Reconciliation and alerts

For a `submitted` or `unknown` ledger record, first set the rollback values above. Locate the record by its logged
account hash and quota generation, preserve the record and its deterministic key, and inspect its owner/fence,
timestamps, and stable error code. Do not delete it, edit it, inventory-read, or submit a replacement. The only
upstream-supported reconciliation is a manually authorized repeat with the same `redeem_request_id`; accept only its
documented `already_redeemed` or `reset` result, then perform the normal one-time inference retry. Otherwise retain
`unknown` and leave the normal quota failure in place.

Alert immediately on `codex_reset_unknown`, a `codex_reset_submit_started` event that lacks a terminal state after its
lease, any `codex_reset_duplicate_prevented` event, a verification failure, or a failed post-reset inference retry. The
response is to disable new claims, preserve the ledger, and perform the reconciliation procedure above. Track candidate,
submission, verified, unknown, duplicate-prevention, verification-latency, estimated-spend, and post-retry-success
metrics; do not auto-remediate an external spend.

## Observability, validation, and future canary

Events include `codex_reset_eligible`, `codex_reset_skipped_healthy_fallback`, `codex_reset_claimed`,
`codex_reset_submit_started`, `codex_reset_submitted`, `codex_reset_unknown`, `codex_reset_rejected`,
`codex_reset_verified`, `codex_reset_inference_retry`, `codex_reset_inference_retry_result`,
`codex_reset_duplicate_prevented`, and `codex_reset_shadow_candidate`.

Metrics cover eligible and shadow candidates, submissions, verified and unknown outcomes, duplicate prevention,
verification latency, estimated spend, and post-reset retry outcome. Raw tokens, auth JSON, provider credentials, and
raw idempotency keys must never appear in logs or KV.

All current tests use injected fake providers or mocked adapter transports, fake clocks, versionstamped fake KV, and
mocked inference transports. They cover strict trigger parsing, policy gates, leases, CAS/credential fencing, ambiguity
recovery, timeouts, crashes, generated state-machine sequences, simultaneous requests, and full
qualifying-429-to-verified-retry delivery through Responses and Chat in buffered and streamed modes.

Do not run a real canary without separate authorization. Before one, shadow mode must observe real qualifying events
without false positives, exactly one account must be allowlisted with a global limit of one, rollback must be rehearsed
without a provider call, and an operator must be able to audit the ledger and one retry.
