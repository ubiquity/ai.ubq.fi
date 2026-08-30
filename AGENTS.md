# Project Guidance

- Keep OpenAI-compatible endpoints and request bodies aligned with the official OpenAI API schema. Do not add
  gateway-only aliases, sentinel values, or alternate wire formats.
- Keep `GET /v1/models` without query parameters strictly OpenAI-compatible. Treat `GET /v1/models?client_version=X.Y.Z`
  as a separate Codex-native compatibility contract that returns the rich upstream `{ "models": [...] }` catalog for
  that exact client version; never describe the versioned response as an official OpenAI schema.
- Treat Codex CLI compatibility as a first-class gateway contract for `/v1/responses`. Accept fields emitted by
  supported Codex CLI versions through explicit compatibility extensions that remain separate from the official OpenAI
  schema allowlists and drift checks; do not present those extensions as official OpenAI fields.
- Treat the uploaded Codex CLI model catalog as the source of truth for reasoning tier strings other than `none`.
  Preserve every non-empty advertised tier and do not enforce a hard-coded tier allowlist or tier membership check.
- Treat `none` as the sole gateway-known reasoning special case and expose it even when the uploaded catalog omits it.
  Normalize null efforts in upstream model metadata to `none`, preserve `none` verbatim at the Codex upstream request
  boundary, and never translate an explicit no-reasoning request to an omitted field or `null`.
- Mirror Codex CLI wire translation for advanced presets: send `ultra` upstream as `max`. Treat Codex's automatic
  multi-agent delegation for `ultra` as client-side orchestration, not as a distinct upstream reasoning effort.
- Use this fixed inference waterfall, in cost order: eligible Codex subscription capacity first, Surplus Intelligence
  second, and OpenLux last. Advance to the next paid tier only after an authoritative quota or capacity signal; do not
  treat a transient timeout, stalled stream, network or read error, or upstream 5xx as quota exhaustion.
- Treat the Provider Sentinel implementation-agent model as an owner-controlled invariant. Only `gpt-5.6-luna` is
  allowed. Agents must not change or substitute this model, or weaken the policy, instructions, or tests that enforce
  it. A failed, timed-out, or exhausted Luna attempt must remain a failure, retry, or blocked outcome; it never
  authorizes a model change.
- Treat `deno deploy --prod` as a build and production-timeline operation, not as proof that the stable route moved. A
  dashboard promotion creates a persistent production pin that a later deployment does not replace automatically.
- For every stable Deno deployment, capture the pre-deploy revision IDs, identify exactly one new succeeded revision,
  and verify that revision's immutable `/health` response against both the full Git SHA and revision ID. Then call
  `POST https://api.deno.com/v2/revisions/<revision-id>/promote` with the existing Deno organization token and require
  HTTP 204. Never select or promote a revision from list order, creation time, or a dashboard "latest" label.
- Serialize deployment writers. Do not use dashboard promotion during a CI release. Production promotion is complete
  only after `https://ai-ubq-fi.ubiquity-dao.deno.net/health` reports the promoted full Git SHA and revision ID in its
  body and response headers. Probe `https://ai.ubq.fi/health` too, and fail on any HTTP 200 identity mismatch.
- Cloudflare Free Bot Fight Mode can challenge GitHub-hosted `curl` requests to the public custom-domain health route
  with HTTP 403 and cannot be bypassed by a path-specific WAF skip rule. Record an identified Cloudflare 403 as a CI
  warning after the exact managed Deno route passes; do not misclassify it as an old Deno revision, weaken bot
  protection globally, report dashboard work as necessary, or wait through repeated identical probes.

## Rolling Asynchronous Codex Review Workflow

- Ship a bounded pull request for each unit of work, request a Codex review on it without waiting for the review to
  finish, merge the pull request only after the required deterministic CI and branch protections pass, test the merged
  result, continue with the next pull request, and collect completed review findings from earlier open or merged pull
  requests only after the fact.
- Codex review latency is never a merge gate. A review that has not completed, an unreviewed open pull request, or a
  review run that could not start must not block delivery, merging, testing, or the next unit of work.
- P0 and P1 Codex findings never block the reviewed pull request merge. No Codex review finding of any severity (P0, P1,
  P2, or P3) gates merge, delivery, testing, or the next unit of work: deterministic CI and branch protections are the
  only merge gates. Every severity enters the asynchronous official review backlog after the fact, with P0 then P1
  priority (P0 before P1 before P2 before P3) for selection as normal future work. Every claim and remediation carries
  exact evidence (exact reviewed head and base SHA, review identity, and the original finding text), and the existing
  production preview, health-identity, monitoring, and acceptance safeguards remain mandatory for every deployed
  candidate.
- Collect completed Codex review findings from earlier eligible open and merged Sentinel pull requests into the official
  review backlog (`docs/sentinel-review-backlog.md`), deduplicating by finding fingerprint. Select backlog entries as
  normal future work and implement them in a follow-up pull request that requests a new Codex review just like every
  other pull request. Never assume a reviewed finding was fixed simply because its reviewed pull request merged;
  verification is the follow-up work item itself.
- Treat malformed, incomplete, or identity-mismatched review data as fail-closed: preserve the exact evidence, ingest
  nothing, and surface the failure. Never drop findings, salvage a partial parse, or mark a failed review complete.
- Automatic rollback exists and is objective: any failed post-merge/post-promotion production acceptance caused by the
  newly delivered candidate is rolled back by the automatic rollback controller, which restores exactly the immutable
  prior Deno revision captured in the pre-deploy healthy attestation — promoted through the existing authenticated Deno
  API path, serialized with deployment writers, verified on the managed health endpoint (exact previous full Git SHA and
  revision ID in body and headers), probed on the custom domain with the existing Cloudflare-403 warning policy,
  evidenced in machine-readable rollback evidence, and failed closed whenever identity or promotion cannot be proven.
  Codex review findings alone never trigger a rollback, and no revision is ever chosen by list order, time, or a
  "latest" label: a review finding observed after a pull request merged is remedied by the backlog follow-up pull
  request and its own acceptance flow, not by reverting an already-published deployment.

## Repository Completion and Checkout Handoff

- Do not leave completed or accepted work only in a worktree, local branch, or unpushed commit. Commit and push every
  completed task branch, integrate it into `development` through the normal pull request and review workflow using an
  ancestry-preserving merge commit, and push the resulting `development` state.
- Immediately before declaring a task complete, fetch `origin/development`, prove every accepted task-created tip is an
  ancestor of the refreshed `origin/development`, and prove local `development` matches `origin/development`.
- Leave the repository-root checkout clean, on `development`, and fast-forwarded to `origin/development`. If another
  writer actively owns that checkout, preserve its state and coordinate until its completed work is integrated; never
  switch or overwrite an active or dirty checkout to satisfy this handoff rule.
- Preserve unfinished work in its existing owned branch or worktree and report its owner and next action. Do not merge
  unknown or incomplete work, discard dirty state, or describe accepted work as complete while it remains outside the
  `development` Git graph.
