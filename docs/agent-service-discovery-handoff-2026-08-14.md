# Agent Service Discovery Handoff

## Objective

Make `ai.ubq.fi` understandable and usable by an autonomous AI agent after the user gives the agent one short prompt and
a `UOS_AI_TOKEN` through a secure environment variable.

## User outcome

An agent can start at `https://ai.ubq.fi/llms.txt`, discover the authoritative API contract, configure an
OpenAI-compatible client, list available models, and send a response or chat-completion request without repository
access or prior knowledge of the service.

## Current state

- Repository: `/home/codex/repos/ubiquity/ai.ubq.fi`
- Branch at planning time: `development`
- HEAD at planning time: `7c3ba47effc172152ee2da390f4260c12dad4782`
- The checkout contains unrelated user-owned changes. Preserve them.
- Detailed instructions exist at `static/docs/llms-agents.md` and are served at `/docs/llms-agents.md`.
- Live checks on 2026-08-14 returned `404` for `/llms.txt`, `/llms-full.txt`, `/.well-known/ai`, and `/openapi.json`.
- This is a direct, single-writer prototype. It does not require a new worktree, branch, module lane, or deployment.

## Discovery contract

### `GET /llms.txt`

Serve a concise, model-readable entry point. It must state:

- service purpose and canonical base URL;
- bearer authentication through `UOS_AI_TOKEN`;
- the supported model-discovery and inference endpoints;
- links to `/openapi.json` and `/llms-full.txt`;
- instructions to query `/v1/models` instead of assuming a model;
- instructions not to print, log, persist, or commit credentials.

### `GET /llms-full.txt`

Serve the existing detailed agent guide from `static/docs/llms-agents.md`. Keep `/docs/llms-agents.md` as an alias so
there is one detailed source of truth.

### `GET /openapi.json`

Serve an OpenAPI 3.1 document for the public OpenAI-compatible surface:

- `GET /v1/models`
- `POST /v1/chat/completions`
- `POST /v1/responses`

Document bearer authentication, server URL, representative request fields, streaming media types, and a reusable
OpenAI-style error envelope. Keep schemas permissive where the official OpenAI contract can evolve; do not invent
gateway-only aliases or alternate wire formats.

### `GET /`

For the JSON representation, include links to the three discovery documents. Preserve the existing HTML response.

## Non-goals

- Do not add MCP. The inference API is already usable through the OpenAI-compatible HTTP contract.
- Do not implement `/.well-known/ai` while the discovery draft is still experimental.
- Do not create credentials, expose server-held credentials, or add an environment variable.
- Do not deploy or alter live configuration as part of this prototype.
- Do not attempt to describe private admin endpoints in the public OpenAPI document.

## Implementation order

1. Add the concise `static/llms.txt` document.
2. Add `static/openapi.json` and validate it as JSON.
3. Register discovery routes in `src/static.ts` and add root JSON links.
4. Update the repository README entry point.
5. Add focused static-asset and contract tests.
6. Run focused tests, formatting checks, lint, type checking, and a local served-route smoke check.

## Acceptance criteria

- All three discovery routes return `200` locally with the intended content type.
- `/llms.txt` gives an agent enough information to configure and call the service.
- `/llms-full.txt` and `/docs/llms-agents.md` return the same detailed source file.
- `/openapi.json` parses as JSON and declares OpenAPI 3.1, bearer auth, and all three public inference paths.
- The non-HTML root response links to the discovery documents.
- Focused tests pass without changing unrelated dirty files.

## Suggested agent prompt

> Read `https://ai.ubq.fi/llms.txt` and follow its linked API specification. Configure ai.ubq.fi as an OpenAI-compatible
> provider using the `UOS_AI_TOKEN` available in your environment. Select a supported model from `/v1/models`, use the
> service for this task, and never print or persist the token.

## Completion report

Report changed files, focused validation, local route evidence, remaining rough edges, Git state, and whether production
deployment was performed.
