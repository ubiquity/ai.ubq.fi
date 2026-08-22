# Provider Sentinel

Provider Sentinel is a single-runner Deno orchestration workflow for provider incident triage, repair, native review,
preview replay, explicit Deno revision promotion, and post-promotion monitoring. Repository content, Deno logs, and
captured request bodies are untrusted inputs. Agent prompts cannot change the fixed model, review, credential,
deployment, or promotion policy in `scripts/sentinel/`.

## Modes and schedule

The `Provider Sentinel` Actions workflow has three modes:

- `preview`: a manual `workflow_dispatch` run on the trusted `development` ref. It is supervised and may deploy only the
  exact candidate SHA to `p-ai-ubq-fi`. It does not push `development` or promote production. A manual run selected on
  any other ref is skipped.
- `daily`: the 06:00 UTC schedule inspects the previous 24 hours.
- `incident`: an accepted gateway or provider failure writes a durable Deno KV incident before dispatching this workflow
  through the dedicated Ubiquity Sentinel GitHub App. One repair is active at a time; failures during it coalesce into
  one pending successor. A production-only Deno cron retries pending delivery and creates no runs while the outbox is
  empty. Each batch includes at least the preceding 20 minutes and expands back to its first failure.

The orchestrator also has an observation mode for a supervised soak period. `--mode observe` inspects the previous 125
minutes, which supports a two-hour schedule with five minutes of overlap. It captures complete production logs, runs
only the read-only triage agent, writes `triage.json`, `observation.json`, and `cycle.json`, and then returns
unconditionally. It never exports or decrypts replay captures, creates a candidate worktree, edits code, runs replay
inference, pushes Git, dispatches a deployment, promotes a revision, or rolls back production. Observation mode needs
only the Deno log token and the two Codex auth slots; it does not receive preview credentials or the replay key.

On GitHub Actions, daily and preview intervals are anchored to the workflow run's immutable `created_at` value. An
incident also receives its durable first-failure timestamp and expands the interval back to that point, bounded by the
48-hour replay retention window.

After anchoring the interval, the orchestrator computes one event deduplication key. Incident attempts use the durable
`<incident-id>-a<attempt>` signal. Ambiguous delivery retries reuse an attempt; a confirmed failed workflow advances it.
Daily and preview cycles exit before raw-log capture when a 90-day evidence artifact already has that key. Incident
retries never trust artifact existence as success because failed runs also preserve encrypted evidence; every delivered
incident run must complete and write its nonce-bound acknowledgement.

The workflow uses one repository-wide concurrency group and does not cancel an active run. Its `queue: max` policy
retains pending incident delivery instead of replacing a signal while a repair or deployment is active. Scheduled and
App-authenticated incident runs are skipped unless `SENTINEL_AUTONOMY_ENABLED` is exactly `true`. Incident dispatch also
requires the fixed GitHub App actor, trusted `development` ref, opaque incident ID, attempt, and first-failure
timestamp. Manual preview runs remain eligible while that gate is disabled.

The workflow supports public, private, and internal repository visibility. It fails before checkout or raw-log capture
unless `SENTINEL_ARTIFACT_KEY` is present and decodes to exactly 32 bytes. After the cycle, it scans every prospective
artifact path for credentials, encrypts raw logs and durable reports with AES-256-GCM, reads the persisted ciphertext
back, decrypts it, verifies every original byte, and only then removes the plaintext directories. The upload step
accepts only the single verified ciphertext envelope. A scan or encryption failure uploads no raw logs or reports.

## Required repository configuration

Add these Actions secrets:

- `SENTINEL_CODEX_AUTH_SLOT_1_B64`: base64 encoding of one complete Codex CLI `auth.json` document.
- `SENTINEL_CODEX_AUTH_SLOT_2_B64`: base64 encoding of a second complete Codex CLI `auth.json` document.
- `SENTINEL_ARTIFACT_KEY`: one cryptographically random 32-byte value encoded as standard base64. It encrypts the full
  raw-log and report artifact before upload and remains separate from the replay key.
- `SENTINEL_REPLAY_KEY`: one cryptographically random 32-byte value encoded as base64url. The deployment workflow
  validates its decoded size without printing it, then synchronizes it to preview and production with
  `deno deploy env load --replace`.
- `SENTINEL_GITHUB_APP_PRIVATE_KEY`: the private key for App ID `4682172`, installed only on this repository with
  Actions write and Metadata read. The deployment workflow sends it through one Deno v2 app patch as a secret limited to
  the production context. It never enters the general deploy environment file or a preview timeline.

The workflow also requires existing secrets `DENO_DEPLOY_TOKEN` and `PREVIEW_UOS_AI_USER_TOKEN`. The deployment workflow
installs the preview credential only on `p-ai-ubq-fi`, and replay uses that same credential to replace authorization.
Production secret synchronization selects only `UBIQUITY_AI_USER_TOKEN`; an absent production token fails the deployment
instead of falling through to the preview token. `GITHUB_TOKEN` comes from the workflow and has `actions: write` and
`contents: write` permissions. Checkout does not persist it in Git configuration; the trusted orchestrator passes a
process-local Git HTTP authorization header only to explicit fetch and push commands without writing the token into the
checkout. The selected real Codex auth remains only in the parent orchestrator and its loopback relay. Agent
`CODEX_HOME` directories contain a synthetic non-secret auth document. The outer Bubblewrap sandbox hides host runner
files and PIDs and mounts only system files, the repository, the selected checkout, and the synthetic home. Neither real
nor synthetic auth is placed under `.sentinel/`, logged, prompted, or uploaded. Workflow secrets are scoped to the
orchestration and final artifact-scan steps, not dependency installation or artifact upload actions.

Complete raw Deno log captures are given to triage without field sanitization, filtering, or summarization. Raw logs and
durable reports are then compressed and encrypted into one authenticated ciphertext envelope before Actions receives
them. The repository or organization must permit 90-day artifact retention. Runs with new captures upload a separate
encrypted replay bundle. Both artifacts use 90-day retention. Codex auth documents and deployment credentials are not
artifacts.

Each encrypted replay bundle uses a non-sensitive, digest-addressed `sentinel-replay-bundle-v1-*` name and includes its
index manifest. The keyed case-group Bloom filter in the artifact name lets future runs locate and download only bundles
that may match the current incident. The index contains no request bodies, decrypted capture fields, authorization
values, cookies, or credentials. Matching retained bundles are subject to fail-closed aggregate artifact-count,
compressed-byte, and extracted-byte limits. Duplicate retained captures are collapsed by capture fingerprint before
decryption and replay. Time-window exports enforce both interval boundaries. Incident exports also follow opaque KV
references to an earlier encrypted manifest when the same failed input was deduplicated during the active batch.

Generate a replay key locally without writing the plaintext to shell history:

```sh
openssl rand 32 | openssl base64 -A | tr '+/' '-_' | tr -d '='
```

The Sentinel and deployment workflows pin third-party actions to full commit SHAs and pin Deno to `2.9.5`. Review and
update these pins deliberately; do not replace them with moving major-version tags before autonomous operation. The
Sentinel primes one run-scoped `DENO_DIR`, then starts the orchestrator with locked, frozen, cached-only dependency
resolution.

## Deployment identity and workflow dispatch

The default workflow token can push a candidate to `development`, but that token's push does not create another push
workflow run. Provider Sentinel therefore performs an explicit workaround: after proving that `origin/development` has
not advanced and pushing the accepted SHA, it dispatches `.github/workflows/deno-deploy.yml` at the exact `development`
ref. It accepts only the deployment run whose recorded head SHA equals the candidate SHA. Preview runs use the same
explicit dispatch with `deploy_preview=true` at the temporary Sentinel ref. Every Sentinel dispatch also sets
`sentinel_build_only=true`. For that path, the pinned reusable workflow is forced into its mode without `--prod` while
its preview target is set to the requested application. The run requires exactly one post-baseline revision, verifies
its immutable body and header identity, and uploads `sentinel-deployment-<run-id>` with the exact app, SHA, and
revision. The orchestrator follows the run ID returned by GitHub's dispatch API and accepts only that run-scoped
artifact. Normal push and manual deployment runs retain the workflow-owned verification and promotion behavior.

Deployment success alone is not production identity. The orchestrator resolves the new routed revision whose revision
URL `/health` reports the exact candidate Git SHA, promotes that revision with Deno's revision API, requires HTTP 204,
and verifies the public Deno host and `ai.ubq.fi` report both the candidate SHA and promoted revision. It records the
previous healthy SHA and revision before deployment so rollback has an exact target. Health attestation requires HTTP
200. An identity-bearing error response is not treated as healthy.

Every preview, production, rollback, and revert promotion is dispatched to
`.github/workflows/sentinel-revision-control.yml`. That short workflow shares the `ai-ubq-fi-deploy` concurrency queue
with all deployment writers. It rechecks the live `development` SHA, the expected stable identity, application
membership, routed status, and immutable health immediately before promotion. Production runs retain the existing
`production` environment gate. If a promotion attempt has an ambiguous result or post-promotion verification fails, the
workflow observes the managed route while it still holds the lock. It restores and verifies the saved revision when the
target is live, records a verified no-change result when the previous revision remained live, and records an unknown
outcome without overwriting an unrelated identity.

Preview replay uses the resolved revision's immutable hostname, not the shared `p-ai-ubq-fi` stable hostname. Production
`keep` is finalized only after one last managed-host identity check and custom-host probe. An exact custom-host HTTP 200
identity mismatch is fatal. An identified Cloudflare Bot Fight Mode challenge is recorded once as a warning after the
managed route passes, without repeated identical probes. Before rollback can promote the recorded old revision, the
orchestrator re-fetches `origin/development`, checks production identity, and stops if the managed identity belongs to
an unknown deployment. Durable preview and production decision files use the declared deployment-identity shape with
app, Git SHA, revision, health URL, and observation time for both candidate and previous revisions.

The supervised preview always proves the rollback leg after monitoring. It restores the candidate only after a `keep`
decision. A `rollback` decision leaves the prior preview revision live and records the candidate as rejected.

Replay transport has a hard inference-only endpoint allowlist. Failed stateful embedding-job requests may remain in the
encrypted incident evidence, but Sentinel never replays `/uos/embedding-jobs` or administrative endpoints and never
executes tool calls returned by a model.

The cycle will not start a production push after its first 90 minutes. This preserves a large part of the hosted
runner's six-hour limit for the fixed 30-minute observation window, the separate monitoring agent, and a deployment plus
Git revert if any production-stage operation fails.

## Supervised preview acceptance

Keep `SENTINEL_AUTONOMY_ENABLED` unset or `false` until the ciphertext artifact policy and immutable dependency pins
satisfy the gates above and one manual preview cycle on `development` demonstrates all of these results:

1. Both auth slots are validated without disclosure, quota selection is correct, and account changes between stages are
   handled.
2. Complete raw logs and durable reports are exported only as authenticated ciphertext, and new encrypted KV captures
   preserve exact request bytes and allowed compatibility headers while replay replaces authorization.
3. Triage reports every evidence-backed finding, implementation records every disposition, and native `codex review`
   blocks P0/P1 while deduplicating P2/P3 into `docs/sentinel-review-backlog.md`.
4. Formatting, lint, build, affected tests, served HTTP/SSE checks, secret scanning, and replay validation pass for the
   exact candidate SHA.
5. The exact SHA reaches `p-ai-ubq-fi`; a simulated keep and rollback both restore the expected revision identity.

After the supervised run has a complete durable report, set the repository variable `SENTINEL_AUTONOMY_ENABLED=true`.
Remove or change that variable to stop new autonomous daily and incident cycles.
