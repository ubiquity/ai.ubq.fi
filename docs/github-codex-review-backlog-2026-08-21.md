# GitHub Codex review backlog — 2026-08-21

## Purpose

Track the non-P1 review work for the agent-readiness pull request. This is a planning document. It does not authorize a
deployment, a Cloudflare change, an API behavior change, a review reply, or thread resolution.

## Snapshot

- Pull request: [#101](https://github.com/ubiquity/ai.ubq.fi/pull/101), feat/agent-readiness into development.
- Reviewed commit: 5152bb35a9d1849301955bfb3bf5f80122202fc6.
- Snapshot time: 2026-08-21 23:38 UTC.
- Open non-P1 findings: three P2 findings. There are no P3 or P4 review threads at this snapshot.
- P1 is intentionally outside this document. Do not mix a P1 schema fix with any item below.
- The checkout contains unrelated, user-owned changes. Preserve them. Do not stage them as part of this backlog work.

## Shared constraints

- Keep the public OpenAI-compatible contract truthful. Do not add gateway-only request fields or change a response only
  to make documentation easier.
- Make the smallest change that fixes the reviewed claim. A documentation fix is preferred over changing all runtime
  paths to meet an incorrect promise.
- Recheck each review thread against the current patch before implementation. A P1 merge can move the referenced lines.
- Use the current IETF HTTPAPI RateLimit document at
  <https://datatracker.ietf.org/doc/draft-ietf-httpapi-ratelimit-headers/> as the primary source. At this snapshot it is
  an Internet-Draft, not RFC 9449. Do not claim RFC conformance until the field grammar and document status are verified
  again.
- Do not use live provider capacity, production deployment, or Cloudflare WAF changes for this work. All checks must be
  local and deterministic.

## P2 work

### P2-01 — Correct the RateLimit specification attribution

- Review:
  [Reference the correct rate-limit specification](https://github.com/ubiquity/ai.ubq.fi/pull/101#discussion_r3834306100)
- Status: ready after one standards check.
- Affected paths: static/openapi.json, static/docs/llms-agents.md, src/api_key_policy.ts, tests/static-assets.test.ts,
  and tests/api-key-policy-dispatch-reuse.test.ts.

Evidence to confirm before editing:

1. The current IETF HTTPAPI RateLimit document names the exact RateLimit and RateLimit-Policy fields that this gateway
   emits.
2. The emitted structured-field grammar is still valid for that document.
3. RFC 9449 remains unrelated to these HTTP fields.

Implementation outline:

1. Remove the incorrect RFC 9449 attribution from the OpenAPI descriptions, agent guide, and source comment.
2. Refer to the HTTPAPI RateLimit document by name and canonical URL, or use a neutral description if its status or
   grammar is not a conformance claim.
3. Keep the legacy RateLimit-Limit, RateLimit-Remaining, and RateLimit-Reset fields explicitly marked as compatibility
   fields.
4. Do not change emitted header values in this item unless the standards check proves a current value is invalid. If it
   does, split that wire change into a separate reviewed task with an interoperability decision.

Tests and acceptance criteria:

- Add a static-asset assertion that the published OpenAPI and agent guide no longer cite RFC 9449 and identify the
  correct HTTPAPI document accurately.
- Keep the existing bounded API-key test that proves the gateway emits RateLimit, RateLimit-Policy, compatibility
  fields, and Retry-After for its own authoritative window.
- Validate GET /openapi.json as JSON and read the served agent guide after the change.

Risk and decision:

- The IETF document is still versioned as a draft. The safe default is precise, non-RFC wording. A claim of full
  protocol conformance needs a separate standards review and owner approval.

### P2-02 — Document bodyless 304 catalog responses

- Review:
  [Exclude bodyless 304 responses from JSON parsing guidance](https://github.com/ubiquity/ai.ubq.fi/pull/101#discussion_r3834306106)
- Status: ready.
- Affected paths: static/developers.html, static/docs/llms-agents.md if its general response guidance needs the same
  clarification, tests/static-assets.test.ts, and tests/codex-catalog.test.ts.

Evidence to preserve:

- src/codex_catalog.ts returns a null-body 304 response when a matching If-None-Match value is supplied to the versioned
  Codex catalog.
- The existing catalog test proves that a matching request returns 304 and an empty body.

Implementation outline:

1. Replace “parse every non-2xx response as JSON” with guidance that applies to JSON error responses.
2. State that a matching conditional request to GET /v1/models?client_version=X.Y.Z can return 304 Not Modified with no
   body. Clients must use their cached representation instead of parsing a body.
3. Keep the documented error envelope for actual JSON error responses.

Tests and acceptance criteria:

- Add a static-asset assertion for the 304/no-body rule in the developer portal.
- Retain or strengthen the catalog test to assert both status 304 and an empty body.
- Verify the rendered /developers page and the versioned catalog route in a local served runtime. The route must still
  return an empty 304 response for a matching ETag.

Risk and decision:

- This is a documentation-only correction. It must not remove ETag support or change the versioned Codex-native catalog
  contract.

### P2-03 — Make Retry-After guidance conditional

- Review:
  [Do not promise Retry-After on every 429](https://github.com/ubiquity/ai.ubq.fi/pull/101#discussion_r3834306108)
- Status: ready.
- Affected paths: static/developers.html, static/openapi.json, matching prose in static/docs/llms-agents.md, targeted
  response tests, and tests/static-assets.test.ts.

Evidence to preserve:

- A bounded API-key window has an authoritative deadline and emits Retry-After with its RateLimit fields.
- Other legitimate 429 paths do not have an authoritative retry deadline. The paid-model roster rejection and temporary
  Codex quota responses are examples named in the review thread.
- The developer portal says every 429 includes Retry-After, while the full agent guide already says “when a 429 includes
  Retry-After.”

Implementation outline:

1. Change developer-portal and OpenAPI wording so Retry-After is honored when present, not promised on every 429.
2. Describe the RateLimit fields as present only for the gateway's authoritative API-key window.
3. Keep RateLimited response headers documented as possible response metadata. Do not make a header required in the
   schema for a path that can return a 429 without it.
4. Do not synthesize a retry deadline where the gateway lacks one. If product policy changes to require Retry-After on
   all 429s, create a separate runtime task and test every 429 producer first.

Tests and acceptance criteria:

- Add a deterministic test for a 429 without a retry deadline and assert that its JSON error remains valid while
  Retry-After is absent.
- Keep the API-key-window test that asserts a present, positive Retry-After value.
- Add static-asset assertions that the developer portal and OpenAPI use conditional wording.
- Verify /developers, /openapi.json, and /docs/llms-agents.md in a local served runtime. Inspect actual response headers
  for the two controlled 429 cases.

Risk and decision:

- The default is a truthful documentation correction. Requiring a universal retry header changes the API contract and
  needs explicit product approval.

## P3 and P4 intake

There are no current P3 or P4 findings. Do not create speculative fixes. When a new review thread appears, record it
with this template before implementation:

| Field               | Required content                                                                     |
| ------------------- | ------------------------------------------------------------------------------------ |
| ID and priority     | Review priority, short title, and date found                                         |
| Review evidence     | Direct PR-thread URL, reviewed SHA, current/outdated state, and code context         |
| Expected contract   | Official source, repository rule, or existing behavior that proves the claim         |
| Impact              | User-visible, API, security, compatibility, and operational impact                   |
| Scope               | Exact source, static asset, schema, and test files likely to change                  |
| Proposed work       | Smallest safe behavior or documentation change; note alternatives rejected           |
| Validation          | Deterministic tests plus the public endpoint or machine-readable file to inspect     |
| Decision or blocker | Product choice, credential, external service, or missing evidence that prevents work |
| Completion          | Acceptance criteria, PR reply text, and whether thread resolution is authorized      |

Triage rules:

1. Confirm the thread is still current and reproduce the claim locally.
2. Treat the review severity as a priority signal, not proof. Link an authoritative contract or a deterministic failing
   test.
3. Put P3 work behind the three P2 items unless it affects security, data loss, or a released API incompatibility. Put
   P4 work behind P3 unless the owner explicitly reprioritizes it.
4. Do not reply to or resolve a thread until its fix is committed, pushed, and verified, and the user has authorized
   that remote review action.

## Execution order and validation

Use one writer for the P2 implementation because the OpenAPI schema and static-asset test are shared surfaces.

1. Re-fetch PR #101 threads and confirm the reviewed commit and dirty state.
2. Complete P1 separately; do not combine it with this backlog.
3. Complete P2-01, then P2-02, then P2-03.
4. For each item, run its focused test first, then run the shared static-asset, catalog, and API-key policy checks.
5. Before a commit, check formatting, lint only changed code, build, validate openapi.json, and inspect each changed
   public static route in a local served runtime.

Suggested final commands, adjusted to the files actually changed:

```sh
deno fmt --check static/openapi.json static/developers.html static/docs/llms-agents.md \
  src/api_key_policy.ts tests/static-assets.test.ts tests/codex-catalog.test.ts \
  tests/api-key-policy-dispatch-reuse.test.ts
deno test --env --allow-net=127.0.0.1 \
  --allow-env=CEREBRAS_API_KEY,VOYAGEAI_API_KEY,METERED_API_KEY,SURPLUS_API_KEY,GIT_REVISION,GITHUB_SHA,DENO_DEPLOYMENT_ID,DENO_DEPLOY_BUILD_ID \
  tests/static-assets.test.ts tests/codex-catalog.test.ts tests/api-key-policy-dispatch-reuse.test.ts
deno task build
git diff --check
```

Do not run the recursive root test command that enters nested worktrees. Do not deploy or make live inference requests
for this backlog unless a later task explicitly authorizes them.
