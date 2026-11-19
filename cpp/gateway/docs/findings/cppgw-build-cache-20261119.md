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
- [x] Measure cold and warm sanitizer builds.
- [x] Compare header invalidation with Ninja's dependency records and restore the header.
- [x] Document the cache.
- [x] Run the final change-scoped checks.

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
| ASan/UBSan cold | 1471.818 | 1426.423 | 0 | 509 | 0% |
| ASan/UBSan warm | 48.322 | 4.606 | 509 | 0 | 100% |

All five runs passed 113 tests and 1,314 cases. Each built 366
production objects. The object phase includes vendor archives, and the cache
counts also include the SQLite probe compilation. All 509 compiler calls were
cacheable in each run. The receipts reported the same input SHA-256
`aa3911ae438220586dad38f37f3c61ddf59397c565e92e01ffaf6411d9b6ceea`
before and after execution.

All six vendor archive digests matched across the release builds. Running
`ninja -f "$build/build.ninja" -n` from the checkout after the warm run
reported `ninja: no work to do.`

## Header invalidation

Appended `static_assert(true, "cppgw compiler cache invalidation probe");` to
`cpp/gateway/include/uos/rt/test.hpp` in the second worktree. A fresh release
shard build reused the existing release cache. The baseline `ninja -t deps`
records listed 113 objects that included the header, and the per-source
`ccache.log` miss set matched those 113 paths exactly. The other 396 calls
hit. All 113 tests and 1,314 cases passed.

The edited input SHA-256 was
`7b8791bfd2654c3cec26ef165b00dbd9e62ba3ba32a8a732e836d97354a16455`
at the start and end of that run. `git restore` restored the header and
`git diff --exit-code -- cpp/gateway/include/uos/rt/test.hpp` passed.

## Implementation checks

- A relocated `-MD` probe restored its object and dependency file, rewrote the
  target to the second output directory and retained its ASan and libFuzzer
  symbols. A separate coverage probe restored the same object and `.gcno`
  bytes, and changing compiler bytes caused a miss.
- The Bookworm ARM64 Docker probe verified an explicit empty launcher in the
  derived build stage. Its packaged ccache 4.7.5 recorded one miss followed by
  one hit using the final per-run log parser. [Docker ARG scope](https://docs.docker.com/reference/dockerfile/#scope)

## Committed-diff review

Base `f92b14a97818ff81bf95cbfb2642e8ff13b4ab13`, head
`91cfa29834097c81147846e72802f378765e42ab`, model
`ubiquity/deepseek-flash`, exit 0. The original output is
`/var/tmp/cppgw-cache-review.Hw3tV9/review.log`.

```sh
codex review --base refs/remotes/origin/agent-jobs/cppgw-cpp-only-20261119-44610457899-1 \
  -c 'shell_environment_policy.inherit="core"'
```

> [P2] Do not require ccache's 4.10-only --print-log-stats option
>
> `stats()` invokes `ccache --print-log-stats`, which was added in ccache 4.10. `cpp/gateway/docs/CI.md` documents Ubuntu 24.04 as a supported prerequisite and tells users to install `ccache`; that release ships ccache 4.9.1, so the command fails with an unknown option. Because `cache_stats` is computed at `driver.py:523` before `receipt.json` is written, the top-level handler turns an otherwise successful compile/test run into exit 2 with no receipt. Return `None` (or fall back to `--show-log-stats`) when the option/version is unsupported. This only affects hosts whose ccache predates 4.10 (Ubuntu 24.04, Debian bookworm); the 26.04 CI runner is unaffected.

Commit `86a4ea4c` reads the per-run log directly. The parser reproduced the
earlier release receipt counts and passed the Bookworm probe.

> [P3] Forward UOS_CI_COMPILER_LAUNCHER into the portable cpp stage
>
> `build.sh` forwards `UOS_CI_COMPILER_LAUNCHER` as a build arg, but the Dockerfile declares the `ARG` only in the `toolchain` stage, and build args do not cross `FROM toolchain AS cpp`. The `cpp` stage's `compiler_cache.launcher()` therefore always auto-detects the image's `/usr/bin/ccache`; a non-release portable build with `UOS_CI_COMPILER_LAUNCHER=''` still builds the gateway through ccache (contrary to the documented disable), and a custom launcher applies only to dependency builds. Re-declare/resolve the argument in the `cpp` stage while keeping the ccache default when it is unset.

BuildKit passed the explicit empty string from `ARG UOS_CI_COMPILER_LAUNCHER`
in `toolchain` to `FROM toolchain AS cpp` and the helper returned an empty
launcher list.

## Final verification

The warm release check used commit
`7d8ff2530568d9c494949a1e186c986e518dce6b` and tree
`eec6169a5fd485c6a5f35afde8445ffe8507d12a`. The two-worker sanitizer
check and fuzz trials used commit `d51217dd5c250e742ecaea7b4bbb89602dc1f6e4`
and tree `d7dbe89652dec660844880bab620fe9d0203be54`. The difference
between these commits is this findings report. Both final suite checks hit
509 of 509 compiler calls and passed 113 tests and 1,314 cases.

Three additional four-worker sanitizer checks hit 509 of 509 compiler calls
and produced byte-identical copies of the 508 objects from the measured warm
run. They each had one different fixture failure. The concurrent check
exceeded the two-second Codex supervisor deadline assertion. The two solo
checks reported a missing Gemini fixture `child-pid` file and a Codex
follow-ups fixture stack overflow, respectively. The final solo rerun used
`UOS_CI_JOBS=2` with the same build configuration and passed. The timing
table records the original four-worker cold and warm runs.

The `base64` fuzz target ran twice under the workflow's network namespace
with `--seconds 5 --jobs 4`. Both runs passed. The cold build took 84.122 s
with 53 misses, and the warm build took 1.222 s with 53 hits. The GCC debug,
Clang debug and Clang sanitizer driver controls each passed all seven checks.

```sh
python3 -B cpp/gateway/tools/links.py --check
actionlint -config-file cpp/gateway/tools/ci-actionlint.yaml \
  .github/workflows/cpp-gateway-ci.yml .github/workflows/cpp-gateway-release.yml
docker buildx build --check --platform linux/arm64 --target package --no-cache-filter cpp \
  --build-arg UOS_CI_COMPILER_LAUNCHER= --build-arg FRESH_COMPILE=1 \
  -f cpp/gateway/packaging/portable/Dockerfile .
git diff --check f92b14a97818ff81bf95cbfb2642e8ff13b4ab13 HEAD
```

The registration gate checked 449 tests. Both workflows passed actionlint
1.7.12. Package verification covered Dockerfile validation with the release
arguments, the Bookworm launcher and stats probe, and macOS script syntax
on Linux.
