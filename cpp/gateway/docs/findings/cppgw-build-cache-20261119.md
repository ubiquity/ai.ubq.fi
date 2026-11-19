# Native gateway compiler cache

Base commit `f92b14a9` on `agent-jobs/cppgw-cpp-only-20261119-44610457899-1`.

## Checklist

- [x] Inspect the build graph and prepare the Ubuntu ARM64 toolchain and pinned SQLite.
- [x] Wire the portable Linux driver to the launcher and verify its fixture boundary.
- [x] Wire the fuzz CI builder to the launcher.
- [x] Wire the macOS packager and its build driver to the launcher.
- [x] Wire the portable Linux packager and its dependency builds to the launcher.
- [x] Set up the CI caches and keep the published Linux build fresh.
- [x] Measure cold, warm and cross-checkout release builds.
- [ ] Measure cold and warm sanitizer builds.
- [ ] Compare header invalidation with Ninja's dependency records and restore the header.
- [ ] Document the cache and run the change-scoped checks.

The driver already deduplicates its production inventory, vendor compilations
and selected test objects in one Ninja graph per configuration. The CI jobs can
share that compiler output without changing their shard or target matrices.

Measurement worktrees are frozen at `5e9aeeb8bb983d4c836fc5c99e3d78cd7c6f66d7`
(tree `6d9c89bf95cd9ef5981e308ea7462d86c5ddc36e`). On this runner, the
measurement script, per-run receipts and progress log are under
`/var/tmp/cppgw-build-cache-20261119.fzf7SD`. The script started at 22:33 UTC.

## ARM64 measurements

Ubuntu 26.04.1 on four ARM64 CPUs with 15 GiB RAM, Clang 21.1.8, ccache
4.12.3, Ninja 1.13.2, mold 2.40.4, Boost 1.90, ICU 78.2 and SQLite 3.54.0.
SQLite was built with the workflow's pinned source, checksum and configure
flags.

```sh
time env -i PATH=/usr/local/bin:/usr/bin:/bin LANG=C.UTF-8 \
  HOME="$run/home" TMPDIR="$run/tmp" CC=clang CXX=clang++ \
  UOS_CI_JOBS=4 UOS_CI_SQLITE_PREFIX=/usr/local UOS_CI_BUILD_DIR="$run/build" \
  CCACHE_DIR="$bench/cache-$config" CCACHE_MAXSIZE=1900MB \
  bash cpp/gateway/tools/ci-build.sh --config "$config" --shard 1/4
```

Each row used a new external build directory, home and temp directory. Each
configuration started with an empty cache. Before warm runs, `git clean -fdx`
ran in the disposable worktrees. The cross-checkout run used a second worktree
at a different depth with the same release cache. The driver ran tests in
private Linux namespaces with loopback mocks.

| Run | Wall (s) | Object phase (s) | Cache hits | Cache misses | Hit rate |
| --- | ---: | ---: | ---: | ---: | ---: |
| Release cold | 877.091 | 856.232 | 0 | 509 | 0% |
| Release warm | 21.731 | 1.291 | 509 | 0 | 100% |
| Release warm, second checkout | 21.805 | 1.385 | 509 | 0 | 100% |

All three release runs passed 113 tests and 1,314 cases. Each built 366
production objects. The object phase includes vendor archives, and the cache
counts also include the SQLite probe compilation. All 509 compiler calls were
cacheable. The three receipts reported the same input SHA-256
`aa3911ae438220586dad38f37f3c61ddf59397c565e92e01ffaf6411d9b6ceea`
before and after execution.
