# Production change control

Decision record: 2026-09-21. The repository owner selected the following policy for any GitHub production environment
and for the `development` branch. GitHub environment and branch rules are repository settings, not files in this
checkout; the owner must apply and verify them in GitHub Settings.

## Selected policy

### `production` environment

- Required reviewer: `0x4007` (one approval).
- Wait timer: 5 minutes.
- Deployment branches: selected branches/tags only, with `development` as the only allowed branch.
- Prevent self-review: enabled.
- Administrator bypass: disabled.

The reviewer gate must be attached to the job that would first receive production secrets. No secret, deployment, or
promotion step may be in a job that runs before that environment job succeeds.

### `development` branch

- Pull request required; one approving review is required and stale approvals are dismissed.
- Require approval of the latest push, conversation resolution, and the `Gateway CI / validate` and
  `Gateway CI / verify-artifact` checks, with the branch up to date before merging.
- Enforce the rules for administrators; deny force-pushes and deletion.
- Keep merge commits allowed. The repository's ancestry-preserving merge policy must not be replaced with a linear-only
  rule.

These settings provide the only entry to the selected `development` deployment branch. A direct push must not be an
alternative production path.

## Current repository topology

The former Deno Deploy production workflow and embedded Sentinel promotion workflow were retired. The only workflow in
this checkout, `.github/workflows/deno-deploy.yml`, is validation-only: it has read-only contents permission, runs on
pull requests, `development` pushes, or manual dispatch, and has no `production` environment, secrets, deployment, or
promotion step. Do not restore an embedded Sentinel workflow here; Sentinel revision control is standalone.

Production is deployed by the owner from the canonical VPS checkout after the required CI checks pass, as documented in
[`ops/README.md`](../ops/README.md). `ops/deploy.ts` archives the exact `HEAD` SHA, writes the release attestation, and
refuses a dirty checkout, a non-`development` branch, or a `HEAD` that differs from `origin/development`; the service
health check then requires the same full SHA and `vps-<SHA>` deployment identity. The immutable artifact SHA and
revision attestation paths are therefore preserved without putting production secrets in GitHub Actions.

## Verification evidence

Verified against `33dfec5405f27e67acfa0ca9caed615ae50e43e1` on 2026-09-21:

- The workflow inventory contains only `.github/workflows/deno-deploy.yml`.
- A controlled workflow-policy check found no `environment:` declaration, `secrets.*` reference, Deno deployment API, or
  revision-promotion call in that workflow. Consequently, a workflow run cannot expose production secrets or make a
  deployment/promotion available before approval; there is no embedded production job to bypass approval.
- This is a controlled absence test, not a live environment-approval run: this repository has no production job on which
  an approval wait could be exercised. The standalone production controller owner must perform that live test in its own
  repository after applying the selected settings.
- The static retirement assertion rejects `sentinel-revision-control` and `scripts/sentinel` in the gateway workflow.
  The `deno task sentinel:test-local` run was attempted but could not start because the sandbox could not resolve
  `registry.npmjs.org` while caching an existing dependency; it was not treated as a pass.
- The owner still must record the GitHub Settings run proving the selected environment and branch rules are active. A
  local checkout cannot prove remote settings, so this record does not claim that remote protection has already been
  applied.
