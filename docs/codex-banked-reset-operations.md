# Codex banked-reset operations

## Status and safety boundary

The pinned `lib/codex` submodule is the provider-contract reference. It defines the account-bound inventory and consume
routes, a caller-supplied `redeem_request_id`, exact consume response codes, and `already_redeemed` as same-attempt
success. The production adapter uses:

- `GET .../rate-limit-reset-credits`
- `POST .../rate-limit-reset-credits/consume`
- `{ "redeem_request_id": "<durable key>", "credit_id": "<exact opaque id>" }`

The adapter sends Bearer auth, `ChatGPT-Account-ID`, and the Codex user agent. It never asks upstream to select a credit
implicitly.

This rollout is deliberately **at-most-one**, not generally reconcilable exactly-once:

- A parsed 2xx JSON `code` of `reset` or `already_redeemed` is an authoritative terminal success for that one
  submission.
- `nothing_to_reset` and `no_credit` are terminal rejections.
- A non-2xx, timeout, connection loss, empty or malformed JSON, or unknown code is durable `unknown`.
- An ambiguous submission is never sent again and the terminal-only provider performs no speculative lookup or
  verification call.
- Upstream commit plus response loss can consume the credit while leaving the gateway unable to prove it. Operators must
  treat `unknown` as possibly spent.

The terminal-only path is structurally enabled only when `CODEX_BANKED_RESET_MAX_GLOBAL_PER_DAY` is exactly `1`. The cap
is one provider submission per UTC day, not one for the lifetime of the deployment. Return mode to `shadow` immediately
after the canary.

All automated tests use fake providers or mocked transports. They do not call the real reset-credit endpoint.

## Candidate and routing eligibility

Banked-reset selection is downstream of the durable Codex routing state:

1. A slot must have returned a completely parsed upstream `429` whose error type is exactly `usage_limit_reached`.
2. The response must contain a canonical absolute `Retry-After` HTTP-date in the future. Relative delays remain valid
   for ordinary routing but cannot identify a banked-reset window.
3. A revised deadline, ambiguous generation, active recovery-probe lease, invalid credential, unmapped slot, stale
   fence, or unavailable KV prevents the cohort from being complete.
4. A fresh strong routing read must account for every current auth-pool slot as either:
   - a stable blocked account; or
   - a healthy, non-probing sibling.
5. Only stable blocked accounts reach inventory or redemption. Healthy siblings prove cohort completeness but are not
   inventory-read or reset.

The request that first discovers a quota block still performs ordinary failover and may be served by a healthy sibling
without any reset-provider call. On a later request, the persisted stable block is visible at initial routing; the
blocked cohort is evaluated before ordinary healthy routing so an expiring credit can be tested.

Inventory reads have a fixed five-second deadline. Inventory failure or timeout skips reset work and leaves the healthy
sibling available.

The ordinary bounded `429` retry remains separate. A successful ordinary retry serves the request and never falls
through to a reset.

After a verified reset, the original inference request is the one recovery probe against the reset account:

- `2xx` returns directly and retains the normal completion probe until the response is explicitly completed.
- Definitive `401`, `403`, or `429` may fall through once to a freshly revalidated sibling that was healthy before
  preflight.
- Any transport error or timeout may have dispatched upstream work, so it is rethrown and never replayed on a sibling.
- Any other HTTP response is returned directly.

## Configuration

Settings are re-read on each gateway request and immediately before the consume boundary.

| Variable                                        | Safe default | Canary requirement                                                       |
| ----------------------------------------------- | ------------ | ------------------------------------------------------------------------ |
| `CODEX_BANKED_RESET_ENABLED`                    | `true`       | `true` during shadow/live; `false` for fail-closed rollback.             |
| `CODEX_BANKED_RESET_MODE`                       | `shadow`     | `shadow`, then briefly `live`, then immediately back to `shadow`.        |
| `CODEX_BANKED_RESET_ACCOUNT_ALLOWLIST`          | empty        | Stable hashes for every approved current account; never raw credentials. |
| `CODEX_BANKED_RESET_MAX_GLOBAL_PER_DAY`         | `0`          | Exactly `1`; terminal-only live rejects every other value.               |
| `CODEX_BANKED_RESET_MAX_PER_ACCOUNT_PER_WINDOW` | `1`          | Exactly `1`; every other value fails closed.                             |

Shadow mode may GET inventory for stable blocked accounts and writes one redacted, deduplicated decision for that
blocked episode. It makes zero consume calls. A repeated selected decision returns `already_would_spend_once` without a
second inventory GET after current strong fences pass. A repeated non-selection preserves its original reason; it is
never mislabeled as a spend candidate.

The global daily record is charged atomically when a transaction crosses the durable `submitted` boundary. A claim that
crosses a UTC-day boundary is rejected before provider submission and cannot borrow the next day's capacity.

Never clear routing KV, shadow decisions, the redemption ledger, or daily-cap records during rollout or rollback.

## Shadow proof

1. Deploy the exact candidate while production remains in `shadow`.
2. Verify both `/health` endpoints serve the same candidate SHA and routed deployment ID:
   - `https://ai-ubq-fi.ubiquity-dao.deno.net/health`
   - `https://ai.ubq.fi/health`
3. Verify the effective configuration remains enabled, in `shadow`, capped at exactly one, and has the secret allowlist
   present.
4. Confirm `/health/providers` still shows the intended stable exhausted account and a healthy non-probing sibling.
5. Send one controlled normal inference request. Do not call the reset-credit endpoint directly.
6. Read `GET /admin/providers/codex/banked-resets/shadow-decisions` and require a current record with:
   - `decision_reason: "selected"`
   - a non-null selected account hash and credit hash
   - an expiry still in the future
   - the expected blocked-slot routing generation and quota deadline
7. Require zero `codex_reset_claimed`, `codex_reset_submit_started`, `codex_reset_submitted`, or `codex_reset_verified`
   events during the shadow request.
8. A second shadow evaluation for the same current episode must not issue another inventory GET.

An empty ledger, an expired decision, any non-`selected` reason, changed fences, or unavailable inventory is a failed
shadow gate. Stay in `shadow`.

## One-reset live canary

Freeze one release operator, one exact served SHA, and one production configuration writer. Do not run another deploy,
workflow retry, console promotion, or Deno configuration change concurrently.

Only after the shadow proof above:

1. Change only the existing mode:

   ```sh
   deno deploy env update-value CODEX_BANKED_RESET_MODE live \
     --org ubiquity-dao \
     --app ai-ubq-fi \
     --token "$DENO_DEPLOY_TOKEN_UBIQUITY_DAO"
   ```

2. The environment update restarts isolates without creating a new build. Re-read the effective configuration without
   printing the secret allowlist, require mode `live`, both caps `1`, and the allowlist present, then re-attest both
   health domains at the unchanged candidate SHA and deployment ID.
3. Send exactly one controlled normal inference request.
4. Expect at most:
   - one fresh account-bound inventory GET for the blocked account;
   - one consume POST for the exact shadow-selected credit;
   - one terminal `reset` or `already_redeemed` result;
   - one post-reset inference probe on the reset account.
5. Immediately return mode to `shadow`, regardless of whether the outcome was terminal or ambiguous:

   ```sh
   deno deploy env update-value CODEX_BANKED_RESET_MODE shadow \
     --org ubiquity-dao \
     --app ai-ubq-fi \
     --token "$DENO_DEPLOY_TOKEN_UBIQUITY_DAO"
   ```

6. Verify the effective mode is again `shadow` and both health domains still report the unchanged candidate SHA and
   deployment ID after the isolate restart.
7. Inspect bounded logs for the attested deployment and require:
   - exactly one `codex_reset_claimed`;
   - exactly one `codex_reset_submit_started`;
   - either one terminal `codex_reset_verified` with `redeem_outcome` equal to `reset` or `already_redeemed`, or one
     durable `codex_reset_unknown`;
   - at most one `codex_reset_inference_retry` and matching result;
   - no second submission attempt.

If the live request returns through the healthy sibling after a definitive `401`, `403`, or `429` probe, the reset
submission may still have succeeded; judge the consume result from the durable transaction event, not only the client
response.

## Durable state and ambiguity

The redemption ledger key is separate from routing state:

```text
["uos_ai", "codex_reset_redemption", "v1", account_id_hash, quota_generation]
```

`claimed` has an owner lease. `submitted` means the provider may have received the request. `unknown` means it may have
committed but did not return a recognized terminal result. `verified` follows exact `reset` or `already_redeemed`, or a
future independently verified contract. `rejected` is terminal for a definitive policy, inventory, or provider
rejection.

The deterministic identity derives from account hash and canonical observed deadline, never from the inference request
ID. Claims and submissions fence routing slot, credential version, account hash, quota deadline, routing generation, and
auth-pool slot.

Verified routing repair clears only the exact quota block, retains its ambiguity tombstone, and reserves the single
post-reset probe. Only a successfully completed recovery response clears that ambiguity.

For a terminal-only `submitted` or `unknown` record:

1. Return mode to `shadow`; use the full rollback below if any unexpected activity continues.
2. Preserve the ledger, routing state, decision, daily cap, logs, and exact deployment identity.
3. Do not repeat the consume request, even with the same `redeem_request_id`.
4. Treat the credit as possibly spent and investigate manually.

## Fail-closed rollback

If logs show more than one attempted submission, a fence mismatch, unexpected provider traffic, or any ongoing live
activity, set both values:

```sh
deno deploy env update-value CODEX_BANKED_RESET_ENABLED false \
  --org ubiquity-dao \
  --app ai-ubq-fi \
  --token "$DENO_DEPLOY_TOKEN_UBIQUITY_DAO"

deno deploy env update-value CODEX_BANKED_RESET_MODE disabled \
  --org ubiquity-dao \
  --app ai-ubq-fi \
  --token "$DENO_DEPLOY_TOKEN_UBIQUITY_DAO"
```

Do not delete KV records during rollback.

Run every `deno deploy` command from a temporary directory. The current CLI can otherwise modify repository `deno.json`
even for administrative commands.

## Validation and observability

Events include `codex_reset_eligible`, `codex_reset_skipped_healthy_fallback`, `codex_reset_claimed`,
`codex_reset_submit_started`, `codex_reset_submitted`, `codex_reset_unknown`, `codex_reset_rejected`,
`codex_reset_verified`, `codex_reset_inference_retry`, `codex_reset_inference_retry_result`,
`codex_reset_duplicate_prevented`, and `codex_reset_shadow_candidate`.

The terminal verified event includes the safe `redeem_outcome` enum. Logs and KV must never contain raw tokens, auth
JSON, credentials, raw account IDs, raw credit IDs, or raw idempotency keys.

Before release, run:

```sh
deno fmt --check serve.ts src tests scripts docs
deno lint serve.ts src tests scripts
deno task build
deno task test
git diff --check
```

The gateway tests cover partial-cohort shadow selection, sequential deduplication, exact shadow-to-live credit matching,
one terminal consume, concurrency and the daily cap, definitive probe fallback, transport no-replay, inventory timeout,
fence and credit drift, and full-pool recovery.
