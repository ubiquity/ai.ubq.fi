# Provider Decision Journal

This append-only journal records material decisions about provider routing, reliability, capacity, fallback, and
operations for `ai.ubq.fi`. Add a dated entry when a decision changes observable provider behavior. Do not rewrite old
entries when a decision changes; add a new entry that supersedes the earlier one.

Each entry must distinguish the decision from its implementation, validation, deployment, and live acceptance state.

## 2026-08-25 — Remove Codex admission control

### Status

- Decision: accepted by the service owner.
- Implementation: complete on local `development`.
- Validation: complete for the local build, lint, focused provider suites, and standard repository test task.
- Commit: none yet.
- Push: not performed.
- Deployment: not performed or authorized.
- Live acceptance: not performed.

### Trigger

Several independent `plz` requests using `gpt-oss-120b`, `gpt-5.3-codex-spark`, and `gpt-5.6-sol` failed with the
client-visible message `API error: "Unknown error"`.

Production telemetry for request `a8fc2006-dbd2-42a7-8c31-c686071e2373` on Git SHA
`0d795e28e42be63bbd7f0d4ce44d8ea0f6ab9d4a` and Deno revision `hbw1zer2k6pz` recorded:

```json
{
  "event": "codex_attempt",
  "attempt": 1,
  "slot": 1,
  "phase": "initial",
  "status": null,
  "status_class": "admission_caller_busy"
}
```

This proves the gateway rejected the request before upstream model dispatch. The failure was not an upstream HTTP
response, model error, or quota response.

### Decision

Remove the Codex admission-control system from the inference path as a hard cutover. Do not replace it with another
gateway concurrency limit, caller-lane lease, queue, or compatibility fallback.

Let the serverless gateway scale normally and let each upstream provider enforce its own capacity and quota limits.
Preserve existing routing behavior for authoritative upstream auth, quota, timeout, and transport outcomes.

### Rationale

The admission system did not provide a reliable capacity signal. It derived a caller lane from thread, session, prompt
cache, or authenticated-principal identity. When clients omitted the more specific metadata, unrelated requests using
the same credential could collapse into one lane.

The `caller_busy` result then stopped routing immediately. It could reject a request while gateway compute, other
account slots, sibling accounts, and other models still had capacity. Rapid retries repeated the same rejection, and the
client hid the structured `codex_admission_busy` response as `Unknown error`.

This created a gateway-originated outage mode without proving that an upstream provider was unavailable. Upstream
capacity remains finite, but speculative gateway serialization is not an accurate substitute for authoritative provider
responses.

### Removal scope

- Remove caller-lane identity derivation and distributed admission leases.
- Remove account admission slots, lease renewal, release retries, and lease-expiry stream cancellation.
- Remove the synthetic `codex_admission_busy` response and routing classification.
- Remove admission-specific image fan-out batching and release waits.
- Remove admission-only tests and update affected routing, streaming, usage, and KV-budget tests.
- Keep API-key quota reservation, account quota routing, timeout circuits, authentication handling, and paid-provider
  policy unchanged unless removal requires a direct mechanical adjustment.

### Acceptance

- Concurrent requests that previously shared an admission caller lane can reach normal provider dispatch independently.
- No runtime path reads or writes the `uos_ai/codex_admission/v1` KV namespace.
- No runtime response uses `codex_admission_busy`.
- Focused Codex routing, OpenAI compatibility, streaming, image fan-out, and KV-budget tests pass.
- Deployment and live verification require separate explicit authorization.

### Local implementation result

- Deleted `src/codex_admission.ts` and its dedicated test suite.
- Removed caller-lane derivation, account-slot leases, renewals, release retries, synthetic busy responses, and
  admission-specific stream signals.
- Removed the four-child image admission batch; all requested image children may dispatch concurrently subject to
  existing API-key quota policy and upstream behavior.
- Added a regression that dispatches eight concurrent Codex requests and requires eight upstream calls with HTTP 200
  responses.
- `deno task build` passed.
- Focused provider and quota validation passed with 111 tests.
- `deno task test` passed with 1,085 tests, 300 test steps, 12 ignored tests, and no failures; the measurement task also
  passed.
- No commit, push, deployment, or live production acceptance was performed.

## 2026-08-25 — Publish Codex admission-control removal for review

### Status

- Implementation commit: `6a5724047c033a0b33b241b0c42860d4efafdd9b`.
- Branch: `fix/remove-codex-admission-control`.
- Push: complete.
- Pull request: [#132](https://github.com/ubiquity/ai.ubq.fi/pull/132), targeting `development`.
- Local Codex review: complete with no actionable defects.
- Pull-request checks and hosted review: pending at the time of this entry.
- Deployment: not performed or authorized.
- Live acceptance: not performed.

### Decision state

The publication state above supersedes only the earlier implementation-status snapshot. The decision and removal scope
remain unchanged.

## 2026-09-18 — Mac local no-auth on a LAN-facing listener, VPS stays authenticated

### Status

- Decision: accepted by the service owner ("mac is no auth and vps has auth i guess cause vps is public").
- Implementation: complete on local `codex/mac-local-auth-20260918`.
- Validation: pending the registered focused suites and repository `verify` gate; no live acceptance yet.
- Commit: none yet.
- Push: not performed.
- Deployment: not performed or authorized. The running Mac daemon keeps its current build until an accepted deploy.
- Live acceptance: not performed; one bounded real Mac request remains a separate user acceptance step.

### Decision

The Mac companion keeps its LAN-facing `0.0.0.0:7999` listener, and authentication is decided per request by the actual
TCP peer:

- An actual numeric loopback peer (`127.0.0.0/8`, `::1`) is passwordless. It authenticates as the existing
  boot-provisioned local development principal (`src/local_development_key.ts`), so local Codex inference and the admin
  dashboard work without a credential.
- Every other peer — LAN clients, and any request that only presents a loopback URL through a forward, tunnel, or forged
  `Host` header — stays authenticated with the existing gateway credentials, for both the client and admin surfaces.
- The public VPS gateway is unchanged: it stays authenticated because it is public (Caddy to `127.0.0.1:7999`), and
  `scripts/serve-vps.ts` never enables a bypass.

### Implementation notes

- `src/local_admin_auth.ts` keeps `shouldDisableAdminAuthForListener` rejecting a non-loopback listener for the generic
  `--disable-admin-auth` path. The Mac entry point uses the narrowly named internal
  `configureMacLocalAdminAuthBypassForListener`, which accepts only the wildcard TCP listener and still requires the
  existing numeric-loopback-peer, loopback-URL, and same-origin checks in `isAdminAuthDisabledForRequest`.
- The bypass state is process-wide, but the peer is bound to the request object
  (`configureAdminAuthPeerForRequest(peer, request)`), because routing awaits before it authenticates: a single
  process-wide peer slot could otherwise be overwritten by a concurrent request, letting a LAN request inherit a
  loopback peer.
- No new CLI flag, environment variable, secret, route, or public response field was added.

## 2026-09-14 — Serial subscription routing: exhaust one Codex subscription before advancing

### Status

- Decision: accepted by the service owner; implementation merged and deployed.
- Implementation: complete on `codex/serial-subscription-routing`, merged as PR
  [#307](https://github.com/ubiquity/ai.ubq.fi/pull/307), merge commit `8bf9daad53a22abf8db4488ef1db2ac2d25c9d34`
  (ancestor of `development`).
- Validation: repository gates passed on the merged SHA; the durable active-subscription behavior is covered by the
  focused suites.
- Deployment: complete 2026-09-15 as `8bf9daad53a22abf8db4488ef1db2ac2d25c9d34`.
- Live acceptance: accepted; three authenticated VPS-origin inference requests succeeded.
- Open follow-ups: issues [#308](https://github.com/ubiquity/ai.ubq.fi/issues/308) and
  [#309](https://github.com/ubiquity/ai.ubq.fi/issues/309) (concurrent-admission edge cases).

### Decision

Route ordinary Codex inference through one durable global active subscription shared by every principal, key, and model.
Bootstrap the first eligible configured account and keep it despite headroom, idle time, restarts, old per-key
affinities, reorder, or a successful same-identity credential refresh. Move it only for authoritative model-applicable
quota or capacity exhaustion, a classified current-credential invalidity, or removal/replacement of the account in the
auth pool.

Each subscription is a separate prompt-cache and account-health identity, so spreading ordinary requests across
subscriptions keeps neither cache warm and obscures which subscription is degrading. The provider fallback chain Codex
-> Surplus -> OpenLux is intentional and ordered by cost; advance it only after authoritative exhaustion.

### Reversal risk

Restoring a retired per-key affinity override, load-balancing across subscriptions while the active one still has
capacity, or reordering the provider chain all reintroduce the cache and account-health problem this decision removed.
Do not make the order dynamic without a new dated entry.
