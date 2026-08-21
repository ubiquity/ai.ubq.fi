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
