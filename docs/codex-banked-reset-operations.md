# Codex banked-reset operations

## Status and operating boundary

**A status-only production adapter is shipped, but it is disabled by default.** Normal traffic cannot call it unless all
existing live controls are set: the feature flag, `live` mode, a nonempty account allowlist, a positive global cap, and
a per-account-window cap of exactly one. This change did not make a real provider request or consume a reset.

The adapter uses the selected account's existing Codex bearer token and account header internally. It maps the normal
`https://chatgpt.com/backend-api/codex` base to these routes:

```text
GET  /backend-api/wham/rate-limit-reset-credits
POST /backend-api/wham/rate-limit-reset-credits/consume
```

It never parses a response body. Under the explicit rollout policy, any inventory `2xx` means one available
`banked_reset`, and any consume `2xx` is terminal enough to repair routing and issue the one inference retry. The POST
body is only `{ "redeem_request_id": "<deterministic key>" }`. A non-2xx inventory response stops before consume and is
rejected. A non-2xx or transport failure after a consume attempt is durable `unknown` and is never resubmitted. The
local `status-NNN` marker is not a provider receipt and is never persisted or logged.

This is a deliberately status-only policy, not independent proof that the provider restored quota. Do not describe a
successful `2xx` as independently verified. Unit and integration tests inject `fetch`; they must never reach the real
provider. The default disabled configuration remains the operational boundary until an operator deliberately enables a
separate one-account rollout.

## What can become a candidate

The feature is downstream of normal Codex account routing. It never replaces ordinary account failover.

1. An account must return a completely parsed upstream `429` with OpenAI error type exactly `usage_limit_reached`.
2. That response must carry a canonical absolute `Retry-After` HTTP-date. It is only a provisional routing observation,
   not provider proof of a quota-window generation. A relative delta `Retry-After` remains valid for ordinary routing
   but can never create a banked-reset candidate. Once a stable observation exists, a later changed date or relative
   delay makes the circuit generation-ambiguous, even after expiry or an administrative recheck: no new claim can use
   either value, while any pre-existing record remains lookup/verification-only. Verified reconciliation releases normal
   routing but retains its stable observation as an ambiguous tombstone and reserves a fenced recovery-probe lease for
   the one post-reset retry, so a delayed in-flight `429` cannot mint a second identity. Only a successful recovery
   probe, including that fenced post-reset retry, clears the ambiguity. A malformed, missing, expired, ambiguous, or
   non-qualifying response likewise cannot create a candidate.
3. Routing must first try every healthy configured account. A successful fallback emits
   `codex_reset_skipped_healthy_fallback`; no reset is attempted.
4. Immediately before a reset transaction, a strong KV read must prove the exact quota-block routing fence still exists.
   KV failure or a stale fence fails closed.

Only after those checks can a configured live provider create a **new** transaction. The ordinary bounded 429 retry and
this transaction are separate. If a later account in the same failover pass returns an ordinary, malformed, or otherwise
non-qualifying `429`, the earlier candidate is cleared: that request has not proved that every failure was a stable
usage-limit exhaustion. A later `401` or `403` cannot serve the request but does not erase a separately verified
usage-limit candidate from another account.

`verified` is durable provider-verification state, not proof that account routing has already been repaired. It may be
written before the routing circuit is reconciled. When normal gateway selection finds every account quota-blocked, the
gateway runs a recovery-only path for each exactly fenced blocked account: it can look up and independently verify an
existing matching `submitted` or `unknown` transaction, or reuse an already `verified` record. It creates no claim, does
not read inventory, and never calls `redeem` on that path.

The gateway issues one post-reset inference retry only after it has both a `verified` record and atomically reconciled
the exact routing fence: same account slot, credential version, quota deadline, and routing generation. That
reconciliation installs the fenced recovery-probe lease used by the retry. A missing, malformed, stale, rejected, or
still-leased record leaves the ordinary all-blocked `429` intact. A retry that returns a second `429` is never fed back
into reset selection. Immediately after any dispatch reservation and before the retry's transport starts, the gateway
force-reads the auth-pool slot one final time; a changed account or credential preserves the original quota result
instead of sending stale credentials. No response headers or SSE bytes are committed before this decision completes.

## Configuration, shadowing, and the kill switch

The banked-reset settings are read from `Deno.env` for each gateway request, rather than once at module load. There is
no in-process banked-reset settings cache. This makes an updated environment value visible as soon as the hosting
platform makes it visible to an isolate, without deleting a durable transaction record.

| Variable                                        | Safe default | Live requirement and behavior                                                                                                        |
| ----------------------------------------------- | ------------ | ------------------------------------------------------------------------------------------------------------------------------------ |
| `CODEX_BANKED_RESET_ENABLED`                    | `false`      | Must be exactly `true` (case-insensitive after trimming). Any other value disables the feature.                                      |
| `CODEX_BANKED_RESET_MODE`                       | `disabled`   | Only `disabled`, `shadow`, and `live` are recognized; unknown values are `disabled`.                                                 |
| `CODEX_BANKED_RESET_ACCOUNT_ALLOWLIST`          | empty        | A comma- or newline-separated set of account IDs or stable account-ID hashes. It must be nonempty and contain the candidate account. |
| `CODEX_BANKED_RESET_MAX_GLOBAL_PER_DAY`         | `0`          | Must be a positive safe integer in live mode. `0`, malformed, negative, decimal, or overflowing input disables live claims.          |
| `CODEX_BANKED_RESET_MAX_PER_ACCOUNT_PER_WINDOW` | `0`          | Must be exactly `1` in live mode. A larger value is not an expansion control; it is invalid and fails closed.                        |

For a fresh candidate, `shadow` mode still requires the feature flag and allowlist, then emits an
eligible/shadow-candidate observation. It makes **zero provider calls**, including inventory reads, writes no redemption
transaction, and does not consume the global cap. It is the safe observation mode before deliberately enabling a
one-account live rollout.

That fresh-candidate guarantee does not discard an expensive prior transaction. If normal all-blocked traffic finds an
existing `submitted` or `unknown` record, conventional providers may use recovery-only lookup/verification in `shadow`
or `disabled` mode. The status-only adapter makes no recovery network call and never resubmits. Recovery never creates a
claim, reads inventory, or calls `redeem`.

The code-level kill switch is to set either `CODEX_BANKED_RESET_ENABLED=false` or `CODEX_BANKED_RESET_MODE=disabled`;
use both for an unambiguous rollback. Configuration is read before a new claim, before the durable
`claimed -> submitted` side-effect boundary, after that transition before the final owner/fence/lease renewal, and once
more synchronously after that renewal immediately before provider invocation. A disable observed at any submission check
prevents the call; the conservative `claimed` or `submitted` record remains for lookup-only recovery. The switch never
erases `submitted`, `unknown`, `verified`, or `rejected` records.

This implementation does **not** yet prove a literal distributed, immediate runtime kill switch. The existing durable
`RuntimeConfigV2` has no banked-reset fields and has a five-minute cache; hosted deployment environment updates may also
require a configuration revision to start serving. In addition, an environment change can race after the pre-claim
policy read and leave a new `claimed` record, although the subsequent submission checks still prevent a provider call.
Before live mode is enabled, operators must either prove the platform's update semantics and that narrow race is
acceptable, or land a separately reviewed control-plane fence that atomically blocks new claims.

Disabled mode deliberately still permits recovery-only lookup/verification for an existing `submitted` or `unknown`
record when a conventional provider supports it, and exact-fence routing reconciliation for an already `verified`
record. The status-only adapter leaves an ambiguous record `unknown`; it makes no blind second redemption.

## Provider boundary and status-only policy

The provider interface is deliberately small and carries only a safe account context:

- `readInventory(context)` reports available count, observed time, and reset type. Its timestamp must be no more than 30
  seconds old and must not be in the future at the gateway clock; inventory is not a reusable cache.
- `redeem(context, idempotencyKey)` submits one logical redemption.
- `lookup(context, idempotencyKey, providerReceiptId)` reconciles a previously submitted or ambiguous transaction.
- `verifyApplied(context)` independently proves that quota was restored.

The account context excludes access tokens, refresh tokens, raw auth JSON, and provider credentials. A conventional
provider can use the full reconciliation path when its declared contract has all of these capabilities:

- caller-supplied deterministic idempotency keys with a positive documented retention period;
- lookup by idempotency key (receipt lookup may be additive, not a substitute);
- independent verification that the reset was applied; and

- one or more reviewed, nonempty supported reset-type identifiers.

The built-in status-only provider is the approved alternative for this rollout: it accepts the caller's deterministic
key, has the reviewed `banked_reset` type, and explicitly declares that a consume `2xx` is final. It intentionally does
not claim retention, lookup, or independent verification. A failed inventory response is rejected before a consume
attempt; any lost/non-2xx consume result stays `unknown` and no retry is sent.

New submission also requires the policy controls in the preceding table, a healthy-fallback-free candidate, KV
availability, and current routing/auth fences. Existing `submitted`/`unknown` recovery needs a conventional provider's
lookup/verification capabilities, but it does not require live mode or an allowlist because it creates no external
mutation. Any missing capability, malformed state, stale fence, configuration, inventory validation, or CAS operation
fails closed. The only built-in HTTP calls are the documented status-only routes above.

## Durable transaction and reconciliation

Redemption is recorded separately from routing because routing describes availability while a redemption record
describes an expensive external mutation. The durable key is:

```text
["uos_ai", "codex_reset_redemption", "v1", account_id_hash, quota_generation]
```

The record contains the hashed account ID, an opaque hashed credential version, quota and routing generations, hashed
deterministic idempotency key, owner token/fence, lease, a receipt only when the provider explicitly approves durable
retention and logging, timestamps, and a stable last error code. It does not contain raw account IDs, access tokens,
refresh tokens, auth JSON, raw credential versions, or raw idempotency keys.

| State       | Meaning                                                                                                                                                                                                                    | Safe operator/recovery action                                                                                                                                                                                                         |
| ----------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `claimed`   | One owner atomically created the logical transaction and holds a 30-second lease. No provider call may happen before it is changed to `submitted`.                                                                         | In live mode only, an expired lease may be taken over using the same deterministic key after fresh routing/auth fence checks. Disabled/shadow recovery leaves it pending; it never resumes a submission or mints a new logical reset. |
| `submitted` | The side-effect boundary was durably crossed before invoking the provider. Immediately before invocation, the owner fence/lease and routing/auth fences are renewed atomically. A receipt is persisted only when approved. | Treat as potentially spent. Before lease expiry it remains in progress. A conventional provider may recover it by lookup/verification; the status-only adapter never resubmits it.                                                    |
| `unknown`   | Transport failure, malformed result, lost response, lookup ambiguity (including a negative lookup while an earlier call may still be in flight), or failed verification may have followed a provider commit.               | Treat as potentially spent. Before lease expiry it remains in progress. A conventional provider may use lookup/verification only; the status-only adapter leaves it unknown and never inventory-reads or submits a replacement.       |
| `verified`  | A conventional provider independently verified the reset, or the configured status-only provider received a consume `2xx`. The routing circuit can still be quota-blocked.                                                 | Terminal redemption state, but not an inference permit by itself. Reconcile the exact routing fence first; only then may the gateway issue one retry. Do not create another redemption for this generation.                           |
| `rejected`  | Inventory, reset type, provider result, or another definitive condition was rejected.                                                                                                                                      | Terminal audit state. Preserve it for investigation; do not loop or create a replacement key.                                                                                                                                         |

Lease takeover changes owner/fence only. It is never permission to generate a new idempotency key. A crash, timeout, or
lost redemption response is always handled as `unknown`. Conventional providers may then use lookup/verification; the
status-only adapter does nothing further. A recovery process must not interpret a timeout as permission for a blind
second redemption.

### State-specific parse, fence, authentication, and lease checks

- **Parse and context:** every durable record must pass the versioned parser, required-field checks, timestamp ordering,
  and state-specific invariants (for example, `claimed` has no submission timestamp; `verified` has both submission and
  verification timestamps; `unknown` carries a stable error code). The account hash, opaque credential-version hash,
  quota generation, and idempotency-key hash must exactly match the current context. Malformed or mismatched records are
  not repaired opportunistically; they fail closed.
- **New claim and submission:** creation of a new `claimed` record strongly reads and atomically checks two fences: the
  exact quota-block routing slot (credential version, generation, `header_retry_after` source, a stable absolute
  observed deadline, equal observed and blocked deadline, and no deadline-revision ambiguity) and the auth-pool slot
  (same account ID and credentials). The gateway repeats those checks at the `claimed -> submitted` boundary, along with
  owner token, ownership fence, routing generation, and an unexpired lease. A fresh, non-future inventory observation
  and an unexpired quota deadline are required after inventory and again before that boundary. Immediately before
  provider invocation, it atomically renews the `submitted` owner fence and lease while repeating the routing/auth
  checks and rechecking the quota deadline; no asynchronous work follows that renewal before `redeem` begins.
- **Lease and ownership:** a non-expired `claimed`, `submitted`, or `unknown` record is in progress. A takeover after
  expiry increments the owner fence. The final pre-redeem renewal also increments that fence. A stale owner cannot
  update, submit, verify, or finalize the record. An expired `claimed` record needs live submission policy and fresh
  routing/auth checks; an expired `submitted` or `unknown` record is recovery-only.
- **Recovery ambiguity:** a conventional provider's `lookup` result that says `rejected` is not by itself a terminal
  state because an earlier provider invocation can still be in flight after its owner lease expires. Recovery performs
  independent verification and otherwise records `unknown`; only a direct provider invocation that returns `rejected`
  may durably record that terminal state. The status-only adapter has no recovery request and leaves the record unknown.
- **Verified routing repair:** conventional verification or the explicit status-only `2xx` policy updates the redemption
  record first. The gateway separately strongly checks and atomically clears the exact routing fence while also checking
  the current auth-pool slot, then force-reads that slot again immediately before retrying inference. A stale
  credential, changed deadline, changed generation, auth-pool replacement before submission or routing repair, failed
  CAS, or unavailable KV prevents the retry rather than clearing a newer circuit.

### Generation and credential fencing

Until a reviewed provider contract exposes a true quota-window generation, the gateway derives a provisional identity
from the account-ID hash and a canonical observed deadline. It intentionally does not use a request ID or credential
version, so credential refresh cannot manufacture a second logical redemption for the same observation. Crucially, any
later changed stable deadline or relative delay before a successful recovery probe marks the circuit
generation-ambiguous and blocks all new redemption claims for it; it never creates a second key. An opaque account-scope
hash keeps that guard attached to the same account through pool reordering. A future live adapter must either replace
this with a provider-proven generation or preserve this fail-closed behavior. The deterministic idempotency key is
derived from that single permitted identity; only its hash is persisted or emitted.

Older routing records may lack the opaque account-scope hash. If their credential version proves the same account, the
gateway attaches the hash and preserves the observation as lookup-only. If it cannot prove that association, ordinary
routing starts neutrally for the replacement account but a global legacy fence denies every new banked-reset claim; an
old identity is never transferred by slot number to another account.

Credential version and routing generation are separate fences. The opaque credential-version hash is made before it can
reach the durable record, provider context, or telemetry. A redemption record can mutate only when its context matches.
Provider verification may mark that record `verified` before routing is repaired. Routing is cleared only in the next
fenced step, when the live slot still has the same credential version, quota reset deadline, and routing generation. The
clear increments routing generation. Therefore an old worker or a reset from a rotated credential cannot admit a newly
installed credential or erase a newer quota block.

If credentials rotate while a record is `submitted` or `unknown`, do not delete the record and do not make a new claim.
Preserve the record and determine, under the reviewed provider contract, whether the reset is account- or
subscription-scoped before any manually supervised reconciliation.

### Offline test safety evidence

The banked-reset suite uses a deterministic clock/token source, an in-memory KV fake with versionstamps, injected CAS/KV
failures and transition barriers, and a scripted provider fake that records every method, account/window, idempotency
key, receipt, commit count, and whether a simulated timeout happened before or after its commit. Sanitized boundary
fixtures cover inventory, terminal/ambiguous redemption outcomes, lookup results, malformed successes, and schema drift.
Seeded generated event sequences exercise request, claim, submission, crash, lease expiry, credential rotation, lookup,
verification, retry, and KV-failure invariants. Public Responses and Chat tests cover buffered and streamed verified and
unknown outcomes. None of these fixtures contains a provider URL, credential, or network implementation.

## UTC global-cap semantics

The global cap lives at:

```text
["uos_ai", "codex_reset_redemption", "global_day", "v1", YYYY-MM-DD]
```

`YYYY-MM-DD` is calculated from `Date#toISOString()`, so it is a UTC calendar day, not the operator's local day. A new
redemption record and the daily counter are atomically created/updated in one KV transaction. The counter is incremented
when a **new logical transaction is claimed**, not when a provider returns success or when independent verification
succeeds. This is intentionally conservative:

- a claim that later becomes rejected or unknown still consumes that day's safety budget;
- reconciliation, duplicate prevention, and lease takeover do not increment the counter;
- changing accounts does not bypass a global cap; and
- a cap of one permits at most one new logical claim across all allowlisted accounts for that UTC day.

Do not manually delete or edit a daily record to create extra capacity. The next UTC day uses a different key. A failed
strong read, malformed record, CAS conflict exhaustion, or unavailable KV prevents the claim instead of relaxing the
cap.

## Logs, metrics, and alerts

The default telemetry sink writes structured `codex_banked_reset` and `codex_banked_reset_metric` console records. It is
not an alerting system; operations must route these records to the deployment's approved log and metrics backend before
any future live rollout.

### Safe fields

Permitted correlation fields are request ID, hashed account ID, hashed idempotency key, quota generation, hashed
credential version, routing generation, owner/fence number, state transition, stable error code, latency, and a receipt
ID only when the reviewed provider contract marks receipt IDs safe to persist and log. The implementation's credential
version is itself a digest of the account and credentials; do not substitute raw credential material for it.

Never log or expose through an admin endpoint:

- access or refresh tokens;
- raw auth JSON or provider credentials;
- raw account IDs unless a separately reviewed operator-only system requires them;
- raw deterministic idempotency keys; or
- owner tokens and receipts that have not been contract-approved as safe.

### Events and emitted metrics

Events include:

```text
codex_reset_eligible
codex_reset_skipped_healthy_fallback
codex_reset_shadow_candidate
codex_reset_claimed
codex_reset_submit_started
codex_reset_submitted
codex_reset_unknown
codex_reset_rejected
codex_reset_verified
codex_reset_inference_retry
codex_reset_inference_retry_result
codex_reset_duplicate_prevented
```

The current implementation emits counters for eligible events, shadow candidates, submission attempts, verified resets,
unknown outcomes, duplicate prevention, verification latency, estimated verified-reset spend, and
`codex_reset_post_retry_total`. The post-retry metric is emitted once for every permitted post-reset inference attempt,
whether it returns a response or throws; use its status field (or `null` for a non-status exception) with the
corresponding inference-retry event to distinguish success from failure. Actual provider spend and inventory decrease
cannot be measured from this status-only policy.

Configure these alert conditions before live mode is ever considered:

1. **Immediate investigation:** `codex_reset_submit_started` without a timely terminal `verified` or `rejected` event,
   any `codex_reset_unknown`, and any stale `submitted`/`unknown` record after its lease. These are possible expensive
   mutations and must not be retried blindly.
2. **Safety/configuration:** a `provider_contract_unproven` rejection while someone believes live mode is enabled, any
   operator-visible global-cap/allowlist policy failure, or any submission event while the mode should be disabled or
   shadow.
3. **Canary quality:** a `codex_reset_post_retry_total` observation with a failed/non-2xx or null-status retry,
   duplicate-prevention spikes, a verified record that cannot reconcile its exact routing fence, or a shadow candidate
   whose underlying response is later found not to be a fully parsed `usage_limit_reached`.
4. **Budget:** observed claims, submission attempts, verified records, and unknown records should reconcile to the daily
   KV count. Any discrepancy is a stop-and-audit condition.

## Rollback and future canary gate

### Rollback procedure

1. Set `CODEX_BANKED_RESET_ENABLED=false` and `CODEX_BANKED_RESET_MODE=disabled` in the deployment configuration. Verify
   the new revision/configuration is serving before reporting rollback complete. Do not describe this as an immediate
   live kill switch until the activation gate above has been closed and rehearsed.
2. Preserve every redemption and global-day KV record. Do not clear routing circuits manually and do not regenerate
   keys.
3. If any record is `submitted` or `unknown`, treat the reset as possibly consumed. Keep live submission disabled. A
   conventional provider may use same-key `lookup`, then independent `verifyApplied`, then exact-fence routing
   reconciliation. The status-only adapter makes no recovery network request and never resubmits. A `claimed` record
   remains pending while disabled; it cannot resume submission.
4. An already `verified` record may likewise repair only its exact current routing fence and allow one inference retry.
   Do not clear a circuit manually if the fence check fails.
5. Audit structured events, durable record states, receipt safety, routing fences, and post-reset inference result
   before proposing re-enablement.

No deployment command is prescribed here because configuration deployment is environment-specific. The exact
immutable-revision rollback command must be written, peer-reviewed, and rehearsed without a provider call before live
mode is enabled.

### Canary preconditions

There is **no authorized live canary today**. Do not try to exercise a real banked reset during implementation,
automated tests, shadow observation, or configuration validation. A future one-reset canary requires all of the
following first:

- the status-only `2xx` policy, exact routes, authentication boundary, and sanitized fixtures have been reviewed;
- unit and integration tests prove that no provider network request is made outside a mocked live-policy path;
- unit, integration, race, streaming, failure-matrix, and property tests pass repeatedly using only fakes;
- shadow mode has observed real qualifying candidates and operators have audited them for false positives;
- exactly one target account is explicitly allowlisted;
- the global limit is exactly `1`, per-account-per-window is exactly `1`, and all other live policy controls have been
  independently checked;
- the target's inventory has been read through the approved status-only endpoint and the operator accepts its `2xx`
  policy;
- logs, metrics, durable KV inspection, operator coverage, and a prewritten rollback are active; and
- the canary request is buffered/non-streaming and deterministic, with an operator watching the record state and
  response.

The future canary evidence must show one qualifying exhaustion, one durable claim, one stable idempotency-key hash, one
consume `2xx`, one inference retry on the redeemed account, and no second submission. Immediately restore disabled mode
after the single canary and audit all records before discussing expansion.
