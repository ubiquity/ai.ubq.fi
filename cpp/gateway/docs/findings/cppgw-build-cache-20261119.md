# Native gateway compiler cache

Base commit `f92b14a9` on `agent-jobs/cppgw-cpp-only-20261119-44610457899-1`.

## Checklist

- [x] Inspect the build graph and prepare the Ubuntu ARM64 toolchain and pinned SQLite.
- [x] Wire the portable Linux driver to the launcher and verify its fixture boundary.
- [x] Wire the fuzz CI builder to the launcher.
- [x] Wire the macOS packager and its build driver to the launcher.
- [x] Wire the portable Linux packager and its dependency builds to the launcher.
- [ ] Set up the CI caches and keep the published Linux build fresh.
- [ ] Measure cold, warm and cross-checkout builds, plus header invalidation.
- [ ] Document the cache and run the change-scoped checks.

The driver already deduplicates its production inventory, vendor compilations
and selected test objects in one Ninja graph per configuration. The CI jobs can
share that compiler output without changing their shard or target matrices.

Measurement worktrees are frozen at `5e9aeeb8bb983d4c836fc5c99e3d78cd7c6f66d7`
(tree `6d9c89bf95cd9ef5981e308ea7462d86c5ddc36e`). On this runner, the
measurement script, per-run receipts and progress log are under
`/var/tmp/cppgw-build-cache-20261119.fzf7SD`. The script started at 22:33 UTC.
