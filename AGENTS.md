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
- Production runs on `codex@vps.pavlovcik.com` in `/home/codex/repos/ubiquity/ai.ubq.fi`. Deno Deploy hosting is
  retired; do not deploy this service there.
- Keep the service configuration in `ops/` and link its systemd files from `/etc/systemd/system/`. Keep secrets in the
  repository-root `.env`, KV in `.data/kv.sqlite3`, and immutable releases in `.data/releases/<full-git-sha>`.
- Serialize VPS deployment writers with `.data/deploy.lock`. Run `deno task deploy:vps` from the clean VPS checkout
  after required CI passes. The launcher resolves `.data/current` once so live code and assets stay on the selected
  release.
- A deployment is accepted only when a direct Mac-to-VPS authenticated inference request succeeds and `/health` returns
  the full Git SHA and `vps-<full-git-sha>` in both its body and identity headers. Verify the public
  `https://ai.ubq.fi/health` route after a DNS or proxy change.
- Cloudflare must retain the proxied `ai.ubq.fi` A record and the hostname-specific Worker exclusion route; the wildcard
  Worker otherwise sends requests to retired Deno hosting.

## Sentinel Retirement

- Sentinel automation has moved to the separate `ubiquity/sentinel` repository. Do not restore embedded Sentinel
  workflows, schedules, incident dispatch, or deployment credentials in this repository.
- The owner approved authenticated failure capture/export, bounded upstream replay data, and exact-build receipts on
  2026-09-07 for the standalone Sentinel integration. Keep scheduling and agents standalone; live activation and
  promotion ownership transfer remain separate from this integration.

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
- Treat malformed, incomplete, or identity-mismatched review data as fail-closed: preserve the exact evidence, ingest
  nothing, and surface the failure. Never drop findings, salvage a partial parse, or mark a failed review complete.

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

## Lint, Format and Type Gates

- Run `sh scripts/verify.sh` (or `deno task verify`) before declaring any change ready. It runs every gate and reports
  all failures rather than stopping at the first: `deno types`, Prettier, ESLint, knip, `deno fmt --check`, `deno lint`,
  `deno check` and the full test suite.
- Prettier owns `*.ts` and `*.mjs` (`printWidth: 160`); `deno fmt` owns JSON, Markdown, CSS, HTML and `static/*.js`.
  Never let both format a file; `deno.json`'s `fmt.exclude` and `.prettierignore` enforce the split.
- Run `deno task build` and `deno task test` after any `--fix` run. This lint project's type environment is close to,
  but not identical with, Deno's checker, so a type-aware autofix can disagree with `deno check`; Deno is the authority.
- `deno check` and the test suite are the only gates that decide whether a lint fix is correct. Never add
  `eslint-disable`, `@ts-ignore` or `as any` to silence a finding: if a finding is deliberate-by-design, narrow it in
  `tools/lint/eslint.config.mjs` with the measurement recorded in a comment, the way the existing DIVERGENCE entries do.
- Do not add a `package.json` dependency to the repository root. Deno resolves its JSR/npm graph from the global cache
  only while the root manifest declares no dependencies; adding one breaks `deno task build` on a fresh checkout. Put
  dev tooling in `tools/lint/`. See `tools/lint/README.md` for the measurements.
- Keep `.deno-types.d.ts` generated rather than committed: `deno task types` writes it, and without it the type-aware
  rules silently lose findings instead of failing.
