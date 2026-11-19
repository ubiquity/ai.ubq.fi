# Native gateway compiler cache

Base commit `f92b14a9` on `agent-jobs/cppgw-cpp-only-20261119-44610457899-1`.

## Checklist

- [x] Inspect the build graph and prepare the Ubuntu ARM64 toolchain and pinned SQLite.
- [ ] Wire the portable driver, fuzz builder and package builds to the launcher.
- [ ] Set up the CI caches and keep the published Linux build fresh.
- [ ] Measure cold, warm and cross-checkout builds, plus header invalidation.
- [ ] Document the cache and run the change-scoped checks.
