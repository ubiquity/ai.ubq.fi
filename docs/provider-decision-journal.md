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
