# Native gateway final qualification

## Starting point

- Integration base `13502438fb2f1a986e61af3456d84611a12eed9e`, tree
  `cb6e0ccf3859245492c5bdff8107157a0dc93350`, confirmed with
  `git log -1 --format='%H %T'`. The runner checked it out as `agent-job`.
- `git fetch origin agent-jobs/cppgw-residuals-1-20261119-44477130790-1` succeeded.
  `git log -6 --oneline FETCH_HEAD` showed the residual branch headed by `a5b0e14a`.
- `uname -sm`, `cat /etc/os-release` and `nproc` showed Linux aarch64,
  Ubuntu 24.04.5 LTS and four CPUs. `sudo -n unshare --net -- true` passed.
- The checkout had no Git author configured. New commits use the existing
  branch's `Codex <codex@openai.com>` identity in local Git configuration.
- Scratch files and dependency builds go outside the checkout. Tests use the
  repository's loopback isolation and credential-free environments.

## Progress

Merged the fetched residual branch with `git merge --no-ff --no-commit FETCH_HEAD`
and an ordinary two-parent merge commit. It brings in `b58afd3d1` (request bounds),
`f90d511bf` (SSE cancellation), and `d35a8a8b` (allowlists). The allowlist
revert, documentation, and full qualification are pending.
