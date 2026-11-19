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
and ordinary two-parent merge commit `984f15ee`. It brings in `b58afd3d1` (request bounds),
`f90d511bf` (SSE cancellation), and `d35a8a8b` (allowlists). Applied `git revert --no-commit d35a8a8bbc9a986bded5bba2d9c0f0f1a11c4ea1`.
The only conflict was in the earlier findings file, which retains its historical
results with a note pointing here. The code retains the model length checks in
both inference entry points. The two conformance cases again have the same HTTP
400 and zero-dispatch assertions on both sides. The allowlist tests were removed
by the revert. Documentation and full qualification are pending.

The runtime documentation now describes the portable and Arch build profiles,
effective bounds, the explicit systemd environment, and the build and service
environment variables. The cutover checklist uses the existing shadow unit and
deployment lock. It labels every live-data and Arch host step as unverified.
The storage runbooks now reflect the sparse and cyclic graph codec already in
the branch. Historic synthetic storage receipts remain scoped to their inputs.
