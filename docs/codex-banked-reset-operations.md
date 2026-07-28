# Codex banked-reset operations

## Status and operating boundary

The pinned `lib/codex` submodule is the provider contract reference. Live mode uses an account-bound adapter only after
the normal flag, allowlist, cap, KV fence, and healthy-fallback gates pass. Tests inject its transport; no test or this
implementation run has called a real reset endpoint.

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

Settings are read for each gateway request so a new claim observes a changed kill switch without deleting durable
records.

| Variable                                        | Safe default | Requirement                                                                |
| ----------------------------------------------- | ------------ | -------------------------------------------------------------------------- |
| `CODEX_BANKED_RESET_ENABLED`                    | `false`      | Must be exactly `true` after trimming and case normalization.              |
| `CODEX_BANKED_RESET_MODE`                       | `disabled`   | Only `disabled`, `shadow`, and `live` are accepted.                        |
| `CODEX_BANKED_RESET_ACCOUNT_ALLOWLIST`          | empty        | A nonempty list of exact account IDs or stable account hashes is required. |
| `CODEX_BANKED_RESET_MAX_GLOBAL_PER_DAY`         | `0`          | Must be a positive integer before a new claim is allowed.                  |
| `CODEX_BANKED_RESET_MAX_PER_ACCOUNT_PER_WINDOW` | `1`          | Must be exactly `1`; all other values fail closed.                         |

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

## Provider contract and ambiguity

The upstream inventory body is `{ credits, available_count }`; `available_count` is authoritative and the optional
available `codex_rate_limits` credit ID is sent when present. Omission asks upstream to choose the next eligible credit.
The response body does not contain a receipt ID, so none is retained.

Upstream documents a caller-supplied `redeem_request_id`, same-key idempotent replay, and `already_redeemed`. It does
not document a retention period or standalone lookup endpoint. Consequently, this gateway treats a parsed terminal
result as final, but keeps response-loss records as `unknown` and never performs an automatic replay. Operators must
preserve those records and investigate before any manually authorized same-key reconciliation.

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
